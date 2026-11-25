"""nano/ as git sees it (index plus working tree), the composed applied folder, and the A.2 file set."""
from __future__ import annotations

import json
import os
import shutil
import stat
import sys
from typing import Dict, List, Optional, Tuple

from .config import Layout, NanoTreeError, Upstream
from .gitcache import Cache, Fork
from .patchfile import Patch

Entries = Dict[str, Tuple[str, str]]  # path relative to nano/ -> (mode, blob id)
MARKER = ".nano-compose.json"


def _decode(b: bytes) -> str:
    return b.decode("utf-8", "surrogateescape")


def tracked_entries(fork: Fork, nano_dir: str = "nano") -> Entries:
    """{path relative to nano/: (mode, blob)} of the fork's index (stage 0, else stage 2)."""
    out = fork.git("ls-files", "-s", "-z", "--", nano_dir + "/")
    result: Entries = {}
    prefix = nano_dir + "/"
    for rec in out.split(b"\0"):
        if not rec:
            continue
        meta, _, path = rec.partition(b"\t")
        mode, sha, stage = meta.decode().split()
        p = _decode(path)
        if not p.startswith(prefix):
            continue
        rel = p[len(prefix):]
        if stage == "0" or (stage == "2" and rel not in result):
            result[rel] = (mode, sha)
    return result


def worktree_entries(fork: Fork, cache: Cache, root: str, nano_dir: str = "nano") -> Entries:
    """nano/ as it is in the working tree: the index's entries with their modes, files changed
    since staging re-hashed (into the cache's object store), deleted files left out, untracked
    files that are not ignored added with mode 100644. Modes always come from the index, never
    from the filesystem (Windows checkouts lose executable bits)."""
    entries = tracked_entries(fork, nano_dir)
    out = fork.git("status", "--porcelain=v1", "-z", "--no-renames", "--untracked-files=all",
                   "--", nano_dir + "/")
    prefix = nano_dir + "/"
    dirty: List[str] = []
    for rec in out.split(b"\0"):
        if len(rec) < 4:
            continue
        xy, path = rec[:2].decode(), _decode(rec[3:])
        if not path.startswith(prefix):
            continue
        rel = path[len(prefix):]
        x, y = xy[0], xy[1]
        if xy == "??":
            dirty.append(rel)
            entries.setdefault(rel, ("100644", ""))
        elif y == "D" or (x == "D" and y == " "):
            entries.pop(rel, None)
        elif y in "MTU" or x == "U" or xy in ("AA", "DU", "UD"):
            if os.path.isfile(os.path.join(root, nano_dir, *rel.split("/"))):
                dirty.append(rel)
                entries.setdefault(rel, ("100644", ""))
            else:
                entries.pop(rel, None)
    if dirty:
        ids = cache.hash_files([os.path.join(root, nano_dir, *r.split("/")) for r in dirty])
        for rel, sha in zip(dirty, ids):
            entries[rel] = (entries[rel][0], sha)
    return entries


def composed_entries(cfg: Upstream, base_tree: Entries, nano: Entries) -> Entries:
    """The applied folder: base files under the asset globs (unless deleted), overlaid by nano/."""
    result: Entries = {}
    deleted = set(cfg.deleted)
    for rel, ent in base_tree.items():
        if cfg.is_asset(rel) and rel not in deleted:
            result[rel] = ent
    result.update(nano)
    return result


def write_file(path: str, data: bytes, mode: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if os.name == "posix":
        st = os.stat(path).st_mode
        if mode == "100755":
            os.chmod(path, st | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        else:
            os.chmod(path, st & ~(stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))


def prepare_out(out: str) -> None:
    """Empty OUT for a new composition; refuse a non-empty directory not made by compose."""
    if os.path.isdir(out) and os.listdir(out):
        if not os.path.isfile(os.path.join(out, MARKER)):
            raise NanoTreeError("%s exists and was not written by compose; remove it first" % out)
        shutil.rmtree(out)
    os.makedirs(out, exist_ok=True)


def compose(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, out: str,
            with_siblings: bool = True, log=None) -> Entries:
    log = log or (lambda msg: print(msg, file=sys.stderr))
    if not cache.has_commit(cfg.base):
        raise NanoTreeError("the cache %s lacks the base commit %s: run nano_tree.py fetch"
                            % (cache.path, cfg.base))
    base_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
    nano = worktree_entries(fork, cache, layout.root)
    want = composed_entries(cfg, base_tree, nano)
    prepare_out(out)
    from_base = {rel: ent for rel, ent in want.items() if rel not in nano}
    blobs = cache.cat_blobs([sha for _m, sha in from_base.values()])
    for rel, (mode, sha) in sorted(from_base.items()):
        write_file(os.path.join(out, *rel.split("/")), blobs[sha], mode)
    for rel, (mode, _sha) in sorted(nano.items()):
        with open(os.path.join(layout.nano, *rel.split("/")), "rb") as f:
            write_file(os.path.join(out, *rel.split("/")), f.read(), mode)
    missing: List[str] = []
    if with_siblings and cfg.siblings:
        missing = export_siblings(layout, cfg, cache, os.path.join(out, "_siblings"))
        for m in missing:
            log("compose: sibling %s is not in the cache (run nano_tree.py fetch to get it)" % m)
    with open(os.path.join(out, MARKER), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"base": cfg.base, "files": len(want), "siblings_missing": missing}, f, indent=2)
        f.write("\n")
    return want


def export_siblings(layout: Layout, cfg: Upstream, cache: Cache, out: str) -> List[str]:
    """Write each sibling file (a path in the AOSP tree outside Nano's folder) as the base plus the
    patch's section leave it, under OUT/<path>. Returns the siblings that could not be made."""
    with open(layout.patch, "rb") as f:
        patch = Patch.parse(f.read())
    sections = {s.path: s for s in patch.sections}
    missing = []
    for path in cfg.siblings:
        code, out_ls, _ = cache.git_rc("ls-tree", "-z", cfg.base, "--", path)
        index = cache.temp_index()
        try:
            entry = None
            if code == 0 and out_ls.strip(b"\0"):
                meta, _, _p = out_ls.strip(b"\0").partition(b"\t")
                mode, _typ, sha = meta.decode().split()
                entry = (mode, sha)
            sec = sections.get(path)
            if entry is None and (sec is None or not sec.added):
                missing.append(path)
                continue
            if entry is not None:
                if not cache.has_object(entry[1]):
                    missing.append(path)
                    continue
                cache.index_info(index, [(entry[0], entry[1], path)])
            if sec is not None:
                ok, err = cache.apply_cached(index, sec.data)
                if not ok:
                    missing.append(path)
                    continue
            ent = cache.index_tree(index, "")
            if path not in ent:
                continue  # deleted by the patch
            mode, sha = ent[path]
            write_file(os.path.join(out, *path.split("/")), cache.cat_blobs([sha])[sha], mode)
        finally:
            cache.cleanup()
    return missing


def expected_tracked(cfg: Upstream, block_sections, base_tree: Optional[Entries]) -> Tuple[set, set]:
    """The A.2 rule as two sets of paths relative to nano/: (required, allowed). REQUIRED is every
    path the Nano block adds or modifies, plus (when BASE_TREE is known) every base file outside
    the asset globs that the block does not delete. ALLOWED adds fork-only paths, which are checked
    by pattern instead."""
    prefix = cfg.prefix
    required = set()
    deleted = set()
    for s in block_sections:
        rel = s.path[len(prefix):]
        if s.deleted:
            deleted.add(rel)
        else:
            required.add(rel)
    if base_tree is not None:
        for rel in base_tree:
            if not cfg.is_asset(rel) and rel not in deleted:
                required.add(rel)
    return required, deleted


def files_problems(cfg: Upstream, tracked: Entries, block_sections, base_tree: Optional[Entries]
                   ) -> List[str]:
    """Differences between nano/'s tracked files and the A.2 rule, as messages (empty: none)."""
    required, deleted = expected_tracked(cfg, block_sections, base_tree)
    problems = []
    for rel in sorted(required - set(tracked)):
        problems.append("missing from nano/: %s" % rel)
    prefix = cfg.prefix
    in_block = {s.path[len(prefix):] for s in block_sections}
    for rel in sorted(set(tracked) - required):
        if cfg.is_fork_only(rel):
            continue
        if rel in deleted:
            problems.append("in nano/ but deleted by the patch: %s" % rel)
        elif base_tree is not None and cfg.is_asset(rel) and rel not in in_block:
            problems.append("asset in nano/ without a patch section (it equals the base): %s" % rel)
        elif base_tree is None:
            continue
        elif rel not in in_block:
            problems.append("in nano/ but neither in the base nor in the patch: %s" % rel)
    return problems
