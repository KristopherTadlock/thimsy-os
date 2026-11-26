"""The Android surface (docs/linux/IMPLEMENTATION.md H.1): the files under nano/ that Soong compiles
or installs, read from nano/Android.bp, and the check that every change to them since the last
merge from nano-144 was made by a commit carrying an `android-surface:` line.

The surface is computed generously, because a file missed here is a change that reaches the Cube's
image unreviewed, while a file wrongly included only asks for a line in a commit message:

- every file a module of nano/Android.bp names in any string property (srcs, src, data, init_rc,
  arch- and target-specific lists, filegroups, prebuilt_etc globs), globs expanded over nano/;
- nano/Android.bp itself;
- every file reached by #include from those, quoted or angle, resolved against the including
  file's folder, nano/ itself and every include folder a module of nano/Android.bp names;
- every header under a library module's export_include_dirs (drastic-nano's unmodified base files
  are not readable here, and they may include one);
- every nano/ file that the patch's other sections (drastic-nano, the framework, the device
  makefiles) #include or name by its path under the Nano folder.
"""
from __future__ import annotations

import posixpath
import re
import subprocess
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .androidbp import Blueprint, expand_globs
from .config import PATCH_REL, NanoTreeError
from .patchfile import Patch

C_FAMILY = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".S", ".def")
INCLUDE = re.compile(rb'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\r\n]+)[>"]', re.M)
# A commit message line that marks a reviewed change to the Android surface: the step id, then what
# and how it was reviewed, e.g. "android-surface: G9a mechanical move of ... (review: color-moved clean)".
SURFACE_LINE = re.compile(r"^android-surface:[ \t]+(G[0-9][0-9A-Za-z-]*)\b.*\breview\b", re.M | re.I)
ANY_SURFACE_LINE = re.compile(r"^android-surface:", re.M | re.I)


def _norm(path: str) -> Optional[str]:
    """PATH (relative, posix) normalised; None when it leaves the Nano folder."""
    if not path or path.startswith("/"):
        return None
    p = posixpath.normpath(path)
    if p == "." or p == ".." or p.startswith("../"):
        return None
    return p


def _strings(value: object, key: str = "") -> Iterable[Tuple[str, str]]:
    """(property name, string) for every string inside VALUE, nested maps and lists included."""
    if isinstance(value, str):
        yield key, value
    elif isinstance(value, list):
        for v in value:
            for kv in _strings(v, key):
                yield kv
    elif isinstance(value, dict):
        for k, v in value.items():
            if k.startswith("_"):
                continue
            for kv in _strings(v, k):
                yield kv


def _dirs_of(files: Iterable[str]) -> Set[str]:
    out = set()
    for f in files:
        d = posixpath.dirname(f)
        while d:
            out.add(d)
            d = posixpath.dirname(d)
    return out


def blueprint_surface(bp: Blueprint, files: Sequence[str], prefix: str) -> Tuple[Set[str], List[str]]:
    """(files the modules name, include folders) for a parsed nano/Android.bp. FILES are the paths
    under nano/; PREFIX is the Nano folder's AOSP path with a trailing slash."""
    fileset = set(files)
    dirs = _dirs_of(files)
    ordered = sorted(files)
    named: Set[str] = set()
    inc: List[str] = []
    root = prefix.rstrip("/")
    for m in bp.modules:
        for key, s in _strings(m):
            if key.endswith("include_dirs"):
                d = s.strip("/")
                if d == root:
                    d = ""
                elif d.startswith(prefix):
                    d = d[len(prefix):]
                d = _norm(d) if d else ""
                if d is not None and (d == "" or d in dirs) and d not in inc:
                    inc.append(d)
                if key == "export_include_dirs" and d:
                    # A library's exported headers are API for modules this file can't see
                    # (drastic-nano's own sources, read only as far as the patch shows them).
                    named.update(f for f in ordered if f.startswith(d + "/") and f.endswith(C_FAMILY))
                continue
            if s.startswith(":") or not s:
                continue
            if any(c in s for c in "*?["):
                named.update(expand_globs([s], ordered))
                continue
            p = _norm(s)
            if p is not None and p in fileset:
                named.add(p)
    return named, [""] + [d for d in inc if d]


def includes_of(data: bytes) -> List[Tuple[str, str]]:
    """(quote, path) of every #include in DATA ('"' or '<')."""
    out = []
    for m in INCLUDE.finditer(data):
        out.append((m.group(1).decode("ascii"), m.group(2).decode("utf-8", "replace").strip()))
    return out


def resolve_include(including: Optional[str], quote: str, name: str, inc_dirs: Sequence[str],
                    fileset: Set[str]) -> Optional[str]:
    """The nano/ path an #include names, or None (a system or out-of-tree header). Generous: an
    angle include is tried in the include folders too, and nano/ itself is always one of them."""
    cands = []
    if quote == '"' and including is not None:
        cands.append(posixpath.join(posixpath.dirname(including), name))
    for d in inc_dirs:
        cands.append(posixpath.join(d, name) if d else name)
    for c in cands:
        p = _norm(c)
        if p is not None and p in fileset:
            return p
    return None


def compute_surface(files: Sequence[str], read: Callable[[str], bytes], prefix: str,
                    outside: Sequence[bytes] = ()) -> Set[str]:
    """The Android surface: paths under nano/. FILES lists nano/'s files, READ returns one's bytes,
    OUTSIDE holds the patch's sections outside the Nano block (their text is scanned for includes
    of, and paths to, Nano's files)."""
    fileset = set(files)
    if "Android.bp" not in fileset:
        raise NanoTreeError("nano/Android.bp is missing")
    bp = Blueprint(read("Android.bp").decode("utf-8"))
    named, inc_dirs = blueprint_surface(bp, files, prefix)
    surface: Set[str] = {"Android.bp"} | named
    mention = re.compile(re.escape(prefix.encode("utf-8")) + rb"([A-Za-z0-9_.+/@-]+)")
    for sec in outside:
        for line in sec.split(b"\n"):
            if not line[:1] in (b"+", b" ") or line.startswith(b"+++ "):
                continue
            body = line[1:]
            for quote, name in includes_of(body):
                p = resolve_include(None, quote, name, inc_dirs, fileset)
                if p:
                    surface.add(p)
            for m in mention.finditer(body):
                p = _norm(m.group(1).decode("utf-8", "replace"))
                if p and p in fileset:
                    surface.add(p)
    todo = sorted(p for p in surface if p.endswith(C_FAMILY))
    seen: Set[str] = set(todo)
    while todo:
        path = todo.pop()
        for quote, name in includes_of(read(path)):
            p = resolve_include(path, quote, name, inc_dirs, fileset)
            if p is not None:
                surface.add(p)
                if p not in seen and p.endswith(C_FAMILY):
                    seen.add(p)
                    todo.append(p)
    return surface


def module_sources(bp: Blueprint, files: Sequence[str]) -> Dict[str, Set[str]]:
    """For each named module of a Blueprint, its srcs with filegroups and globs expanded."""
    ordered = sorted(files)
    out: Dict[str, Set[str]] = {}
    for m in bp.modules:
        if not m.name:
            continue
        srcs: Set[str] = set()
        for s in bp.srcs(m):
            if s.startswith(":"):
                continue
            if any(c in s for c in "*?["):
                srcs.update(expand_globs([s], ordered))
            else:
                srcs.add(s)
        out[m.name] = srcs
    return out


def block_map(patch: bytes, prefix: str) -> Dict[str, bytes]:
    """The Nano block of PATCH as {path under nano/: section bytes}."""
    return {s.path[len(prefix):]: s.data for s in Patch.parse(patch).block_sections(prefix)}


def outside_sections(patch: bytes, prefix: str) -> List[bytes]:
    return [s.data for s in Patch.parse(patch).sections if not s.path.startswith(prefix)]


class Commit:
    __slots__ = ("sha", "message", "changed")

    def __init__(self, sha: str, message: str, changed: Set[str]):
        self.sha = sha
        self.message = message
        self.changed = changed

    @property
    def annotated(self) -> bool:
        return bool(SURFACE_LINE.search(self.message))

    @property
    def subject(self) -> str:
        return self.message.split("\n", 1)[0]


def surface_diffs(head_block: Dict[str, bytes], base_block: Dict[str, bytes], surface: Set[str],
                  head_files: Set[str]) -> Set[str]:
    """The paths whose Nano-block section differs between HEAD and the merge base and that count as
    Android-surface changes: every path of either block (not only HEAD's surface, which is computed
    from nano/ at HEAD and so cannot see a path that is gone from it), counted when it is on HEAD's
    surface or is not a file of nano/ at HEAD. The second covers a deleted file, an asset whose
    section regen dropped because the file left nano/ (the image then ships the base copy, or
    nothing, even where an Android.bp glob still matches), and a section for a file that was never
    committed (android-safety-1)."""
    out = set()
    for p in set(head_block) | set(base_block):
        if head_block.get(p) != base_block.get(p) and (p in surface or p not in head_files):
            out.add(p)
    return out


def coverage(diffs: Set[str], commits: Sequence[Commit]) -> Tuple[List[str], List[Tuple[Commit, List[str]]]]:
    """(differing paths no annotated commit changed, [(unannotated commit, the differing paths it
    changed)]). A commit "changes" a path when the path's patch section differs from its parent's."""
    covered: Set[str] = set()
    unannotated: List[Tuple[Commit, List[str]]] = []
    for c in commits:
        hit = sorted(diffs & c.changed)
        if not hit:
            continue
        if c.annotated:
            covered.update(hit)
        else:
            unannotated.append((c, hit))
    return sorted(diffs - covered), unannotated


# -- git

class Repo:
    """Read-only git access to the fork, objects only (no work-tree state)."""

    def __init__(self, root: str):
        self.root = root
        self._blobs: Dict[str, Dict[str, bytes]] = {}
        self._cat: Optional[subprocess.Popen] = None

    def git(self, *args: str, check: bool = True) -> Tuple[int, bytes]:
        proc = subprocess.run(["git", "-c", "core.quotepath=off"] + list(args), cwd=self.root,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if check and proc.returncode != 0:
            raise NanoTreeError("git %s: %s" % (" ".join(args), proc.stderr.decode("utf-8", "replace").strip()))
        return proc.returncode, proc.stdout

    def rev(self, rev: str) -> Optional[str]:
        rc, out = self.git("rev-parse", "--verify", "-q", rev + "^{commit}", check=False)
        return out.decode().strip() if rc == 0 else None

    def merge_base(self, a: str, b: str) -> Optional[str]:
        rc, out = self.git("merge-base", a, b, check=False)
        return out.decode().strip() if rc == 0 and out.strip() else None

    def ls_tree(self, rev: str, folder: str) -> Dict[str, str]:
        """{path under FOLDER: blob id} at REV."""
        _, out = self.git("ls-tree", "-r", "-z", rev, "--", folder)
        res: Dict[str, str] = {}
        pre = folder.rstrip("/") + "/"
        for rec in out.split(b"\0"):
            if not rec:
                continue
            meta, _, path = rec.partition(b"\t")
            mode, typ, sha = meta.decode().split(" ")
            p = path.decode("utf-8", "surrogateescape")
            if typ == "blob" and p.startswith(pre):
                res[p[len(pre):]] = sha
        return res

    def cat(self, obj: str) -> Optional[bytes]:
        """One object's bytes through a long-lived `git cat-file --batch`; None when missing."""
        if self._cat is None:
            self._cat = subprocess.Popen(["git", "cat-file", "--batch"], cwd=self.root,
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        assert self._cat.stdin is not None and self._cat.stdout is not None
        self._cat.stdin.write(obj.encode("utf-8") + b"\n")
        self._cat.stdin.flush()
        header = self._cat.stdout.readline()
        if header.endswith(b" missing\n") or not header:
            return None
        size = int(header.split()[2])
        data = self._cat.stdout.read(size)
        self._cat.stdout.read(1)
        return data

    def close(self) -> None:
        if self._cat is not None:
            try:
                assert self._cat.stdin is not None and self._cat.stdout is not None
                self._cat.stdin.close()
                self._cat.wait(timeout=10)
                self._cat.stdout.close()
            except Exception:  # noqa: BLE001 - best effort
                self._cat.kill()
            self._cat = None

    def patch_blob(self, rev: str) -> Optional[str]:
        rc, out = self.git("rev-parse", "--verify", "-q", "%s:%s" % (rev, PATCH_REL), check=False)
        return out.decode().strip() if rc == 0 else None

    def block_at(self, rev: str, prefix: str) -> Dict[str, bytes]:
        """The patch's Nano block at REV, cached by blob id; {} when REV has no patch."""
        blob = self.patch_blob(rev)
        if blob is None:
            return {}
        if blob not in self._blobs:
            data = self.cat(blob)
            self._blobs[blob] = block_map(data, prefix) if data is not None else {}
        return self._blobs[blob]

    def surface_commits(self, base: str, head: str, prefix: str, paths: Set[str]) -> List[Commit]:
        """The non-merge commits in BASE..HEAD that changed the patch, oldest first, each with the
        paths of PATHS whose Nano-block section it changed."""
        _, out = self.git("log", "--no-merges", "--reverse", "--format=%H%x00%P%x00%B%x01",
                          "%s..%s" % (base, head), "--", PATCH_REL)
        commits = []
        for rec in out.decode("utf-8", "replace").split("\x01"):
            rec = rec.strip("\n")
            if not rec:
                continue
            sha, parents, msg = rec.split("\x00", 2)
            parent = parents.split()[0] if parents.split() else None
            after = self.block_at(sha, prefix)
            before = self.block_at(parent, prefix) if parent else {}
            changed = {p for p in paths if after.get(p) != before.get(p)}
            commits.append(Commit(sha, msg, changed))
        return commits
