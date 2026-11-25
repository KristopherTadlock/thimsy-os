"""The ROCKNIX tree: paths, tag and commit, and how its files are read.

Files are read as text, never run. Two artefacts of a Windows checkout are undone so a
tree checked out on Windows imports to the same bytes as one checked out on Linux:

* CRLF line ends, when every line of a file ends in CRLF (core.autocrlf); a file with
  mixed ends is left alone, since a patch may carry CRs on purpose.
* Symbolic links checked out as small text files holding the link's target
  (core.symlinks=false); ROCKNIX's quirk directories use links between devices.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

PROJECT = "projects/ROCKNIX"

_SHA_RE = re.compile(r"^[0-9a-f]{40}$")


class ImportError_(Exception):
    """A problem that stops the import (bad input, unknown shape)."""


class GuardStop(ImportError_):
    """A hash guard changed: an imperative block needs a fresh hand-port."""


def normalise_newlines(data: bytes) -> bytes:
    """CRLF -> LF only when every line break in the file is CRLF."""
    if b"\r\n" not in data:
        return data
    lf = data.count(b"\n")
    crlf = data.count(b"\r\n")
    if lf == crlf:
        return data.replace(b"\r\n", b"\n")
    return data


class RocknixTree:
    def __init__(self, root: str, tag: str, commit: str | None = None):
        self.root = Path(root).resolve()
        if not (self.root / PROJECT).is_dir():
            raise ImportError_("%s has no %s directory: not a ROCKNIX tree" % (self.root, PROJECT))
        self.tag = tag
        found = read_git_head(self.root)
        if commit:
            commit = commit.strip().lower()
            if not _SHA_RE.match(commit):
                raise ImportError_("--commit must be a full 40-hex sha, got %r" % commit)
            if found and found != commit:
                raise ImportError_("--commit %s does not match the tree's HEAD %s" % (commit, found))
            self.commit = commit
        elif found:
            self.commit = found
        else:
            raise ImportError_("cannot read the ROCKNIX commit from %s/.git; pass --commit" % self.root)

    # -- paths -------------------------------------------------------------------------
    @property
    def project(self) -> Path:
        return self.root / PROJECT

    def path(self, rel: str) -> Path:
        return self.root / rel

    def rel(self, p: Path) -> str:
        return Path(os.path.relpath(str(p), str(self.root))).as_posix()

    def device_dir(self, family: str) -> Path:
        return self.project / "devices" / family

    def linux_pkg_dir(self) -> Path:
        """get_pkg_directory linux: the project's package overrides the distribution's."""
        for cand in (self.project / "packages" / "linux", self.root / "packages" / "linux"):
            if (cand / "package.mk").is_file():
                return cand
        raise ImportError_("no packages/linux/package.mk in the ROCKNIX tree")

    def exists(self, rel: str) -> bool:
        return self.resolve(self.path(rel)) is not None

    # -- reading -----------------------------------------------------------------------
    def resolve(self, p: Path, hops: int = 8) -> Path | None:
        """Follow real links and links checked out as text; None when missing or a dir."""
        for _ in range(hops):
            if os.path.islink(str(p)):
                p = Path(os.path.normpath(os.path.join(str(p.parent), os.readlink(str(p)))))
                continue
            if not p.is_file():
                return None
            target = _text_link_target(p)
            if target is None:
                return p
            p = target
        raise ImportError_("too many links at %s" % p)

    def read_bytes(self, p: Path) -> bytes:
        real = self.resolve(p)
        if real is None:
            raise ImportError_("missing file %s" % self.rel(p))
        return normalise_newlines(real.read_bytes())

    def read_text(self, p: Path) -> str:
        return self.read_bytes(p).decode("utf-8", errors="replace")

    def provenance(self, rel: str) -> str:
        return "ROCKNIX %s at %s (tag %s)" % (rel, self.commit, self.tag)


def _text_link_target(p: Path) -> Path | None:
    """A file that is a symbolic link checked out as text: short, one line, no newline,
    a relative path that names an existing file or directory next to it."""
    try:
        size = p.stat().st_size
    except OSError:
        return None
    if size == 0 or size > 255:
        return None
    data = p.read_bytes()
    if b"\n" in data or b"\0" in data or data.startswith(b"/") or data.startswith(b"#"):
        return None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if not ("/" in text or text.startswith("..")):
        return None
    target = Path(os.path.normpath(os.path.join(str(p.parent), text)))
    if target.exists():
        return target
    return None


def is_text_link_to_dir(p: Path) -> bool:
    t = _text_link_target(p) if p.is_file() else None
    return t is not None and t.is_dir()


def read_git_head(root: Path) -> str | None:
    """The commit HEAD names, read from .git without running git."""
    git = root / ".git"
    if git.is_file():  # a worktree or submodule: "gitdir: <path>"
        line = git.read_text(encoding="utf-8", errors="replace").strip()
        if line.startswith("gitdir:"):
            git = Path(os.path.normpath(os.path.join(str(root), line[7:].strip())))
    head = git / "HEAD"
    if not head.is_file():
        return None
    text = head.read_text(encoding="utf-8", errors="replace").strip()
    if _SHA_RE.match(text):
        return text
    if not text.startswith("ref:"):
        return None
    ref = text[4:].strip()
    common = git
    cfile = git / "commondir"
    if cfile.is_file():
        common = Path(os.path.normpath(os.path.join(str(git), cfile.read_text().strip())))
    for base in (git, common):
        rf = base / ref
        if rf.is_file():
            sha = rf.read_text(encoding="utf-8", errors="replace").strip()
            if _SHA_RE.match(sha):
                return sha
        packed = base / "packed-refs"
        if packed.is_file():
            for line in packed.read_text(encoding="utf-8", errors="replace").splitlines():
                parts = line.split(" ", 1)
                if len(parts) == 2 and parts[1].strip() == ref and _SHA_RE.match(parts[0]):
                    return parts[0]
    return None
