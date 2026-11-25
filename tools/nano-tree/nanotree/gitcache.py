"""Running git: the fork's repository, and the base cache (work/nano-base/) with temporary indexes
and the pinned environment of IMPLEMENTATION.md A.3 (no system, user or global config, no system
attributes, no network for lazy fetches)."""
from __future__ import annotations

import atexit
import os
import shutil
import subprocess
import tempfile
from typing import Dict, Iterable, List, Optional, Tuple

from .config import NanoTreeError

ZERO = "0" * 40
_EMPTY_HOME: Optional[str] = None
# Variables that would point a git command at another repository, index or object store.
_REPO_VARS = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
              "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
              "GIT_PREFIX", "GIT_CEILING_DIRECTORIES", "GIT_DISCOVERY_ACROSS_FILESYSTEM",
              "GIT_CONFIG", "GIT_CONFIG_PARAMETERS", "GIT_CONFIG_COUNT", "GIT_EXTERNAL_DIFF",
              "GIT_DIFF_OPTS", "GIT_QUARANTINE_PATH", "GIT_REPLACE_REF_BASE", "GIT_SHALLOW_FILE",
              "GIT_GRAFT_FILE", "GIT_NO_REPLACE_OBJECTS")


def _empty_home() -> str:
    global _EMPTY_HOME
    if _EMPTY_HOME is None:
        _EMPTY_HOME = tempfile.mkdtemp(prefix="nano-tree-home-")
        atexit.register(shutil.rmtree, _EMPTY_HOME, True)
    return _EMPTY_HOME


def pinned_env(extra: Optional[Dict[str, str]] = None, network: bool = False) -> Dict[str, str]:
    env = {k: v for k, v in os.environ.items() if k not in _REPO_VARS and not k.startswith("GIT_CONFIG_KEY_")
           and not k.startswith("GIT_CONFIG_VALUE_")}
    home = _empty_home()
    env.update({
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_ATTR_NOSYSTEM": "1",
        "HOME": home,
        "XDG_CONFIG_HOME": home,
        "USERPROFILE": home,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_OPTIONAL_LOCKS": "0",
        "LC_ALL": "C",
        "LANG": "C",
        # A blob missing from the partial-clone cache must fail, never trigger a download.
        "GIT_ALLOW_PROTOCOL": "file:https:ssh:git" if network else "none",
        "GIT_AUTHOR_NAME": "nano-tree", "GIT_AUTHOR_EMAIL": "nano-tree@localhost",
        "GIT_COMMITTER_NAME": "nano-tree", "GIT_COMMITTER_EMAIL": "nano-tree@localhost",
    })
    env.pop("GIT_CONFIG_GLOBAL", None)
    if extra:
        env.update(extra)
    return env


def run_git(args: List[str], cwd: Optional[str] = None, env: Optional[Dict[str, str]] = None,
            input: Optional[bytes] = None, check: bool = True, ok_codes: Iterable[int] = (0,)
            ) -> Tuple[int, bytes, bytes]:
    proc = subprocess.run(["git"] + list(args), cwd=cwd, env=env, input=input,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and proc.returncode not in tuple(ok_codes):
        raise NanoTreeError("git %s failed (%d): %s" % (" ".join(args[:4]), proc.returncode,
                                                         proc.stderr.decode("utf-8", "replace").strip()))
    return proc.returncode, proc.stdout, proc.stderr


def git_version(env: Optional[Dict[str, str]] = None) -> Tuple[int, ...]:
    _, out, _ = run_git(["--version"], env=env)
    word = out.decode().split()[2]
    nums = []
    for part in word.split("."):
        if part.isdigit():
            nums.append(int(part))
        else:
            break
    return tuple(nums)


def posix(path: str) -> str:
    return path.replace("\\", "/")


class Fork:
    """The fork's own repository (the checkout or worktree that holds nano/). Commands run with the
    caller's environment, so GIT_DIR and GIT_WORK_TREE set by the caller are honoured."""

    def __init__(self, root: str):
        self.root = root

    def git(self, *args: str, input: Optional[bytes] = None, check: bool = True,
            env: Optional[Dict[str, str]] = None, ok_codes: Iterable[int] = (0,)) -> bytes:
        e = dict(os.environ)
        e.setdefault("GIT_OPTIONAL_LOCKS", "0")
        e["GIT_ALLOW_PROTOCOL"] = e.get("GIT_ALLOW_PROTOCOL", "file:https:ssh:git")
        if env:
            e.update(env)
        return run_git(list(args), cwd=self.root, env=e, input=input, check=check,
                       ok_codes=ok_codes)[1]

    def objects_dir(self) -> str:
        common = self.git("rev-parse", "--git-common-dir").decode().strip()
        if not os.path.isabs(common):
            common = os.path.join(self.root, common)
        objects = os.path.join(common, "objects")
        if not os.path.isdir(objects):
            raise NanoTreeError("no object directory at %s" % objects)
        return os.path.abspath(objects)

    def git_dir(self) -> str:
        d = self.git("rev-parse", "--git-dir").decode().strip()
        return d if os.path.isabs(d) else os.path.abspath(os.path.join(self.root, d))

    def show(self, rev: str, path: str) -> Optional[bytes]:
        code, out, _ = run_git(["cat-file", "blob", "%s:%s" % (rev, path)], cwd=self.root,
                               env=dict(os.environ, GIT_ALLOW_PROTOCOL="none"), check=False)
        return out if code == 0 else None

    def rev_parse(self, rev: str) -> Optional[str]:
        code, out, _ = run_git(["rev-parse", "--verify", "-q", rev + "^{commit}"], cwd=self.root,
                               env=dict(os.environ), check=False)
        return out.decode().strip() if code == 0 else None


class Cache:
    """The base cache: a git repository holding the GammaOS base commit and its Nano blobs.
    Every command runs in the pinned environment; the fork's objects can be added as alternates."""

    def __init__(self, path: str):
        self.path = os.path.abspath(path)
        self.git_dir = os.path.join(self.path, ".git")
        if not os.path.isdir(self.git_dir) and os.path.isfile(os.path.join(self.path, "HEAD")):
            self.git_dir = self.path  # a bare repository
        self.alternates: List[str] = []
        self._tmp: List[str] = []

    def exists(self) -> bool:
        return os.path.isfile(os.path.join(self.git_dir, "HEAD"))

    def env(self, index: Optional[str] = None, network: bool = False) -> Dict[str, str]:
        extra = {"GIT_DIR": self.git_dir}
        if index:
            extra["GIT_INDEX_FILE"] = index
        if self.alternates:
            extra["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = os.pathsep.join(self.alternates)
        return pinned_env(extra, network=network)

    def git(self, *args: str, index: Optional[str] = None, input: Optional[bytes] = None,
            check: bool = True, network: bool = False, ok_codes: Iterable[int] = (0,)) -> bytes:
        # core.compression=1 is git's built-in level (Z_BEST_SPEED) for binary patch sections, the
        # one the committed patch was made with; -1 (zlib's default) would change every binary section.
        base = ["-c", "core.quotepath=off", "-c", "core.autocrlf=false", "-c", "core.safecrlf=false",
                "-c", "core.compression=1", "-c", "core.fsmonitor=false"]
        return run_git(base + list(args), cwd=self.path, env=self.env(index, network), input=input,
                       check=check, ok_codes=ok_codes)[1]

    def git_rc(self, *args: str, index: Optional[str] = None, input: Optional[bytes] = None
               ) -> Tuple[int, bytes, bytes]:
        base = ["-c", "core.quotepath=off", "-c", "core.autocrlf=false"]
        return run_git(base + list(args), cwd=self.path, env=self.env(index), input=input, check=False)

    def temp_index(self) -> str:
        fd, path = tempfile.mkstemp(prefix="nano-index-", dir=self.git_dir)
        os.close(fd)
        os.unlink(path)  # git refuses an empty file as an index; it creates it
        self._tmp.append(path)
        return path

    def cleanup(self) -> None:
        for p in self._tmp:
            for q in (p, p + ".lock"):
                try:
                    os.unlink(q)
                except OSError:
                    pass
        self._tmp = []

    def has_object(self, sha: str) -> bool:
        code, _, _ = self.git_rc("cat-file", "-e", sha)
        return code == 0

    def has_commit(self, sha: str) -> bool:
        code, out, _ = self.git_rc("cat-file", "-t", sha)
        return code == 0 and out.strip() == b"commit"

    def ls_tree(self, treeish: str) -> Dict[str, Tuple[str, str]]:
        """{path: (mode, blob)} of every file under TREEISH (recursive), paths relative to it."""
        out = self.git("ls-tree", "-r", "-z", "--full-tree", treeish)
        result = {}
        for rec in out.split(b"\0"):
            if not rec:
                continue
            meta, _, path = rec.partition(b"\t")
            mode, typ, sha = meta.decode().split()
            if typ == "blob":
                result[path.decode("utf-8", "surrogateescape")] = (mode, sha)
        return result

    def missing_objects(self, shas: Iterable[str]) -> List[str]:
        shas = list(shas)
        if not shas:
            return []
        out = self.git("cat-file", "--batch-check", input=("\n".join(shas) + "\n").encode())
        return [line.split()[0].decode() for line in out.splitlines() if line.endswith(b" missing")]

    def cat_blobs(self, shas: List[str]) -> Dict[str, bytes]:
        """The contents of the blobs SHAS, read through one `cat-file --batch`."""
        result: Dict[str, bytes] = {}
        uniq = sorted(set(shas))
        if not uniq:
            return result
        out = self.git("cat-file", "--batch", input=("\n".join(uniq) + "\n").encode())
        pos = 0
        for sha in uniq:
            nl = out.index(b"\n", pos)
            header = out[pos:nl].split()
            if len(header) < 3 or header[1] == b"missing":
                raise NanoTreeError("blob %s is missing from the cache %s" % (sha, self.path))
            size = int(header[2])
            result[sha] = out[nl + 1:nl + 1 + size]
            pos = nl + 1 + size + 1
        return result

    def hash_files(self, paths: List[str]) -> List[str]:
        """Write the files PATHS into the cache's object store as they are (no filters)."""
        if not paths:
            return []
        out = self.git("hash-object", "-w", "--no-filters", "--stdin-paths",
                       input=("\n".join(posix(p) for p in paths) + "\n").encode("utf-8", "surrogateescape"))
        ids = out.decode().split()
        if len(ids) != len(paths):
            raise NanoTreeError("hash-object returned %d ids for %d files" % (len(ids), len(paths)))
        return ids

    def hash_bytes(self, data: bytes) -> str:
        return self.git("hash-object", "-w", "--no-filters", "--stdin", input=data).decode().strip()

    def index_info(self, index: str, entries: List[Tuple[str, str, str]]) -> None:
        """Set index entries (mode, sha, path); mode "0" removes the path."""
        if not entries:
            return
        recs = []
        for mode, sha, path in entries:
            recs.append(("%s %s\t%s" % (mode, sha, path)).encode("utf-8", "surrogateescape"))
        self.git("update-index", "-z", "--index-info", index=index, input=b"\0".join(recs) + b"\0")

    def index_tree(self, index: str, prefix: str) -> Dict[str, Tuple[str, str]]:
        """{path relative to PREFIX: (mode, blob)} of the index's entries under PREFIX."""
        out = self.git("ls-files", "-s", "-z", "--", prefix, index=index)
        result = {}
        for rec in out.split(b"\0"):
            if not rec:
                continue
            meta, _, path = rec.partition(b"\t")
            mode, sha, _stage = meta.decode().split()
            p = path.decode("utf-8", "surrogateescape")
            if p.startswith(prefix):
                result[p[len(prefix):]] = (mode, sha)
        return result

    def base_index(self, base: str, prefix: str) -> str:
        """A temporary index holding BASE's tree at PREFIX (a folder path with a trailing slash)."""
        index = self.temp_index()
        self.git("read-tree", "--prefix=" + prefix, "%s:%s" % (base, prefix.rstrip("/")), index=index)
        return index

    def apply_cached(self, index: str, patch: bytes, include: Optional[str] = None) -> Tuple[bool, str]:
        args = ["apply", "--cached", "--whitespace=nowarn", "--binary"]
        if include:
            args.append("--include=" + include)
        args.append("-")
        code, _, err = self.git_rc(*args, index=index, input=patch)
        return code == 0, err.decode("utf-8", "replace")


def init_cache(path: str) -> Cache:
    os.makedirs(path, exist_ok=True)
    cache = Cache(path)
    if not cache.exists():
        run_git(["init", "-q", path], env=pinned_env())
    cfg = [("core.autocrlf", "false"), ("core.filemode", "false"), ("core.longpaths", "true"),
           ("gc.auto", "0")]
    for k, v in cfg:
        cache.git("config", k, v)
    return cache
