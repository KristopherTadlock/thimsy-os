"""Bringing changes into nano/: an applied AOSP folder (import-tree), nano-144's patch changes
(import-patch and the merge driver), and a new GammaOS base (import-base). IMPLEMENTATION.md A.5."""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import time
from typing import Dict, List, Optional, Tuple

from .compose import (MARKER, Entries, composed_entries, dirty_nano, files_problems,
                      tracked_entries, worktree_entries, write_file)
from .config import PATCH_REL, Layout, NanoTreeError, Upstream, base_from_fetch_script
from .gitcache import Cache, Fork, posix
from .patchfile import Patch, sha256

Entry = Tuple[str, str]  # (mode, blob)


def _err(msg: str) -> None:
    print(msg, file=sys.stderr)


def is_binary(data: bytes) -> bool:
    return b"\0" in data[:8000]


# ---------------------------------------------------------------------------------------------
# Trees in the cache


def tree_with_block(cfg: Upstream, cache: Cache, base: str, block: bytes) -> Entries:
    """{path relative to Nano's folder: (mode, blob)} of BASE's Nano folder with BLOCK applied."""
    index = cache.base_index(base, cfg.prefix)
    try:
        if block:
            ok, err = cache.apply_cached(index, block)
            if not ok:
                raise NanoTreeError("the Nano block does not apply to base %s: %s" % (base[:12], err.strip()[:500]))
        return cache.index_tree(index, cfg.prefix)
    finally:
        cache.cleanup()


def base_of_commit(fork: Fork, rev: str) -> Optional[str]:
    """The GammaOS base a commit records: upstream.json where it has one, else fetch_nano.sh's BASE=."""
    data = fork.show(rev, "tools/nano-tree/upstream.json")
    if data:
        return Upstream.from_text(data.decode("utf-8")).base
    data = fork.show(rev, "tools/android-launcher/fetch_nano.sh")
    if data:
        return base_from_fetch_script(data.decode("utf-8", "replace"))
    return None


# ---------------------------------------------------------------------------------------------
# The three-way merge per path


class Plan:
    def __init__(self):
        self.write: Dict[str, Tuple[str, bytes]] = {}   # rel -> (mode, data)
        self.delete: List[str] = []
        self.conflicts: List[Tuple[str, str]] = []      # (rel, reason)


def merge_file(cache: Cache, ours: bytes, base: bytes, theirs: bytes, labels=("nano/", "old", "new")
               ) -> Tuple[bytes, int]:
    tmp = tempfile.mkdtemp(prefix="nano-merge-")
    try:
        paths = []
        for name, data in (("ours", ours), ("base", base), ("theirs", theirs)):
            p = os.path.join(tmp, name)
            with open(p, "wb") as f:
                f.write(data)
            paths.append(posix(p))
        code, out, err = cache.git_rc("merge-file", "-p", "-L", labels[0], "-L", labels[1], "-L", labels[2],
                                      *paths)
        if code < 0 or code > 127:
            raise NanoTreeError("git merge-file failed: %s" % err.decode("utf-8", "replace"))
        return out, code
    finally:
        shutil.rmtree(tmp, True)


def three_way(cache: Cache, O: Entries, T: Entries, M: Entries, labels=("nano/", "old", "new")) -> Plan:
    """Changes from O to T, merged into M, per path (A.5 step 3)."""
    plan = Plan()
    todo = []
    for rel in sorted(set(O) | set(T)):
        o, t, m = O.get(rel), T.get(rel), M.get(rel)
        if o == t or m == t:
            continue
        if m == o:
            if t is None:
                plan.delete.append(rel)
            else:
                todo.append((rel, "take", o, t, m))
            continue
        if o is not None and t is not None and m is not None and o[1] == m[1] and o[0] != m[0]:
            todo.append((rel, "take-content", o, t, m))  # only our mode changed
            continue
        if o is not None and t is not None and m is not None and o[1] == t[1]:
            continue  # only their mode changed and ours changed too: keep ours
        if t is None:
            plan.conflicts.append((rel, "deleted on the incoming side, changed in nano/"))
            continue
        if m is None:
            plan.conflicts.append((rel, "changed on the incoming side, deleted in nano/"))
            continue
        todo.append((rel, "merge", o, t, m))
    shas = []
    for _rel, _kind, o, t, m in todo:
        shas += [e[1] for e in (o, t, m) if e is not None]
    blobs = cache.cat_blobs(shas)
    for rel, kind, o, t, m in todo:
        if kind == "take":
            plan.write[rel] = (t[0], blobs[t[1]])
            continue
        if kind == "take-content":
            plan.write[rel] = (m[0], blobs[t[1]])
            continue
        od = blobs[o[1]] if o else b""
        td, md = blobs[t[1]], blobs[m[1]]
        mode = t[0] if (o is None or o[0] == m[0]) else m[0]
        if is_binary(od) or is_binary(td) or is_binary(md):
            plan.conflicts.append((rel, "binary file changed on both sides (nano/'s version kept)"))
            continue
        merged, nconf = merge_file(cache, md, od, td, labels)
        plan.write[rel] = (mode, merged)
        if nconf:
            plan.conflicts.append((rel, "%d conflicting hunk(s), markers left in the file" % nconf))
    return plan


def apply_plan(layout: Layout, cfg: Upstream, plan: Plan, base_tree: Entries, nano: Entries,
               cache: Cache) -> Tuple[List[str], Dict[str, str]]:
    """Write PLAN into nano/ (the working tree only). Asset paths that end up equal to BASE_TREE's
    file are removed from nano/ instead (they are fetched), and asset deletions of base files go to
    upstream.json's "deleted". Returns (paths touched, {path: mode wanted})."""
    touched: List[str] = []
    modes: Dict[str, str] = {}
    deleted = set(cfg.deleted)
    for rel, (mode, data) in sorted(plan.write.items()):
        dest = os.path.join(layout.nano, *rel.split("/"))
        if cfg.is_asset(rel):
            deleted.discard(rel)
            b = base_tree.get(rel)
            if b is not None and b[0] == mode and cache.hash_bytes(data) == b[1]:
                if os.path.exists(dest):
                    os.remove(dest)
                    touched.append(rel)
                continue
        write_file(dest, data, mode)
        touched.append(rel)
        modes[rel] = mode
    for rel in plan.delete:
        dest = os.path.join(layout.nano, *rel.split("/"))
        if os.path.exists(dest):
            os.remove(dest)
            _prune_dirs(layout.nano, os.path.dirname(dest))
        touched.append(rel)
        if cfg.is_asset(rel) and rel in base_tree:
            deleted.add(rel)
    if deleted != set(cfg.deleted):
        cfg.deleted = sorted(deleted)
        cfg.save()
    return touched, modes


def _prune_dirs(top: str, d: str) -> None:
    top = os.path.abspath(top)
    d = os.path.abspath(d)
    while d.startswith(top) and d != top:
        try:
            os.rmdir(d)
        except OSError:
            return
        d = os.path.dirname(d)


def stage(fork: Fork, touched: List[str], modes: Dict[str, str]) -> None:
    """Stage TOUCHED (paths relative to nano/) and set the index modes MODES."""
    if touched:
        paths = ["nano/" + r for r in sorted(set(touched))]
        fork.git("update-index", "--add", "--remove", "-z", "--stdin",
                 input=b"\0".join(p.encode("utf-8", "surrogateescape") for p in paths) + b"\0")
    current = tracked_entries(fork)
    plus = [r for r, m in modes.items() if m == "100755" and current.get(r, ("",))[0] != "100755"]
    minus = [r for r, m in modes.items() if m == "100644" and current.get(r, ("",))[0] == "100755"]
    for flag, rels in (("--chmod=+x", plus), ("--chmod=-x", minus)):
        for i in range(0, len(rels), 100):
            fork.git("update-index", flag, "--", *["nano/" + r for r in rels[i:i + 100]])


def report_conflicts(plan: Plan) -> None:
    for rel, why in plan.conflicts:
        _err("CONFLICT nano/%s: %s" % (rel, why))


def check_files(layout: Layout, cfg: Upstream, fork: Fork, cache: Optional[Cache]) -> List[str]:
    """The A.2 set check (test_nano_files.py's) on the fork's index."""
    with open(layout.patch, "rb") as f:
        block = Patch.parse(f.read()).block_sections(cfg.prefix)
    base_tree = None
    if cache is not None and cache.has_commit(cfg.base):
        base_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
    return files_problems(cfg, tracked_entries(fork), block, base_tree)


# ---------------------------------------------------------------------------------------------
# import-tree


def import_tree(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, src: str, log=print) -> int:
    src = os.path.abspath(src)
    if not os.path.isdir(src):
        raise NanoTreeError("%s is not a directory" % src)
    if not cache.has_commit(cfg.base):
        raise NanoTreeError("the cache lacks the base commit %s: run nano_tree.py fetch" % cfg.base)
    cache.alternates = [fork.objects_dir()]
    base_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
    files: List[str] = []
    for dirpath, dirnames, filenames in os.walk(src):
        rel_dir = os.path.relpath(dirpath, src).replace(os.sep, "/")
        if rel_dir == ".":
            rel_dir = ""
            dirnames[:] = [d for d in dirnames if d not in (".git", "_siblings")]
        else:
            dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            rel = (rel_dir + "/" + name) if rel_dir else name
            if rel == MARKER:
                continue
            files.append(rel)
    files.sort()
    ignored = {p[len("nano/"):] for p in fork.ignored(["nano/" + r for r in files])}
    for r in sorted(ignored):
        log("import-tree: skipped (ignored): %s" % r)
    files = [r for r in files if r not in ignored]
    assets = [r for r in files if cfg.is_asset(r)]
    ids = dict(zip(assets, cache.hash_files([os.path.join(src, *r.split("/")) for r in assets])))
    keep = [r for r in files if not cfg.is_asset(r) or base_tree.get(r, ("", ""))[1] != ids[r]]
    tracked = tracked_entries(fork)
    with open(layout.patch, "rb") as f:
        patch_modes = {}
        for s in Patch.parse(f.read()).block_sections(cfg.prefix):
            m = s.new_mode()
            if m:
                patch_modes[s.path[len(cfg.prefix):]] = m
    modes: Dict[str, str] = {}
    touched: List[str] = []
    for rel in keep:
        sp = os.path.join(src, *rel.split("/"))
        dp = os.path.join(layout.nano, *rel.split("/"))
        with open(sp, "rb") as f:
            data = f.read()
        old = None
        if os.path.isfile(dp):
            with open(dp, "rb") as f:
                old = f.read()
        touched.append(rel)
        if os.name == "posix":
            modes[rel] = "100755" if os.stat(sp).st_mode & 0o111 else "100644"
        elif rel in tracked:
            modes[rel] = tracked[rel][0]
        elif rel in patch_modes:
            modes[rel] = patch_modes[rel]
        elif rel in base_tree:
            modes[rel] = base_tree[rel][0]
        else:
            modes[rel] = "100644"
        if old != data or os.name == "posix":
            write_file(dp, data, modes[rel])
    keep_set = set(keep)
    for rel in sorted(tracked):
        if rel in keep_set or cfg.is_fork_only(rel):
            continue
        dp = os.path.join(layout.nano, *rel.split("/"))
        if os.path.exists(dp):
            os.remove(dp)
            _prune_dirs(layout.nano, os.path.dirname(dp))
        touched.append(rel)
        log("import-tree: removed %s" % rel)
    stage(fork, touched, modes)
    log("import-tree: %d files from %s (%d assets that differ from the base)"
        % (len(keep), src, len([r for r in keep if cfg.is_asset(r)])))
    return 0


# ---------------------------------------------------------------------------------------------
# import-patch and the merge driver


def import_blocks(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, old_block: bytes,
                  new_block: bytes, stage_result: bool = True, log=print) -> Tuple[Plan, List[str]]:
    """Merge the change from OLD_BLOCK to NEW_BLOCK (both against cfg.base) into nano/."""
    cache.alternates = [fork.objects_dir()]
    base_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
    O = tree_with_block(cfg, cache, cfg.base, old_block)
    T = tree_with_block(cfg, cache, cfg.base, new_block)
    nano = worktree_entries(fork, cache, layout.root)
    M = composed_entries(cfg, base_tree, nano)
    # nano/ never takes a fork-only path from a patch and regen never writes one, so a change the
    # incoming side made to one would vanish without a word: a conflict instead (android-safety-2).
    fork_only_changes = sorted(r for r in set(O) | set(T) if cfg.is_fork_only(r) and O.get(r) != T.get(r))
    for d in (O, T, M):
        for rel in [r for r in d if cfg.is_fork_only(r)]:
            del d[rel]
    plan = three_way(cache, O, T, M)
    for rel in fork_only_changes:
        plan.conflicts.append((rel, "the incoming patch %s this path, but it is fork-only (upstream.json "
                                    "fork_only): nano/ never imports it and regen never writes it, so "
                                    "the change would be lost. Carry it over by hand (outside nano/, or "
                                    "narrow fork_only)" % ("adds" if rel not in O else "deletes" if rel not in T
                                                           else "changes")))
    touched, modes = apply_plan(layout, cfg, plan, base_tree, nano, cache)
    if stage_result:
        stage(fork, touched, modes)
    log("import: %d file(s) written, %d deleted, %d conflict(s)"
        % (len(plan.write), len(plan.delete), len(plan.conflicts)))
    return plan, touched


def import_patch(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, ref: Optional[str],
                 old: Optional[str], log=print) -> int:
    from .regen import regen
    if ref:
        new_data = fork.show(ref, PATCH_REL)
        if new_data is None:
            raise NanoTreeError("%s has no %s" % (ref, PATCH_REL))
    else:
        with open(layout.patch, "rb") as f:
            new_data = f.read()
    if old is None:
        if not ref:
            raise NanoTreeError("import-patch without REF needs --old REV (the commit whose patch "
                                "nano/ last matched)")
        old = fork.git("merge-base", "HEAD", ref).decode().strip()
    old_data = fork.show(old, PATCH_REL)
    if old_data is None:
        raise NanoTreeError("%s has no %s" % (old, PATCH_REL))
    old_base = base_of_commit(fork, old)
    new_base = base_of_commit(fork, ref) if ref else cfg.base
    for name, b in (("the old side (%s)" % old[:12], old_base), ("the new side", new_base)):
        if b and b != cfg.base:
            raise NanoTreeError("%s is based on GammaOS %s, nano/ on %s: run `nano_tree.py import-base "
                                "%s` first" % (name, b[:12], cfg.base[:12], b))
    old_block = Patch.parse(old_data).split_block(cfg.prefix)[1]
    new_block = Patch.parse(new_data).split_block(cfg.prefix)[1]
    if old_block == new_block:
        log("import-patch: the Nano block did not change between %s and %s" % (old[:12], ref or "the working patch"))
        return 0
    plan, _ = import_blocks(layout, cfg, fork, cache, old_block, new_block, log=log)
    if plan.conflicts:
        report_conflicts(plan)
        _err("import-patch: resolve the conflicts in nano/, `git add` them, then run nano_tree.py regen --force")
        return 1
    rc = regen(layout, cfg, fork, cache, force=True, log=log)
    problems = check_files(layout, cfg, fork, cache)
    for p in problems:
        _err("import-patch: %s" % p)
    return rc or (1 if problems else 0)


BLOCK_SHA_REL = "tools/nano-tree/block.sha256"
UPSTREAM_REL = "tools/nano-tree/upstream.json"


def _incoming_candidates(fork: Fork) -> List[str]:
    """Commits that may be the incoming side of the running merge, most likely first: the
    GITHEAD_<sha> variables `git merge` sets for its drivers, MERGE_HEAD and friends, then every
    local and remote branch."""
    revs: List[str] = []
    for key in sorted(os.environ):
        if key.upper().startswith("GITHEAD_") and len(key) == len("GITHEAD_") + 40:
            revs.append(key[len("GITHEAD_"):].lower())
    for special in ("MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "REBASE_HEAD", "FETCH_HEAD",
                    "AUTO_MERGE"):
        r = fork.rev_parse(special)
        if r:
            revs.append(r)
    out = fork.git("for-each-ref", "--format=%(objectname)", "refs/heads", "refs/remotes", check=False)
    revs += out.decode().split()
    seen = set()
    return [r for r in revs if not (r in seen or seen.add(r))]


def _blobs_at(fork: Fork, revs: List[str], path: str) -> List[Optional[str]]:
    """The blob id of PATH in each of REVS (None where it is missing)."""
    if not revs:
        return []
    query = "".join("%s:%s\n" % (r, path) for r in revs).encode()
    res = fork.git("cat-file", "--batch-check", input=query, check=False).decode().splitlines()
    result: List[Optional[str]] = []
    for line in res:
        parts = line.split()
        result.append(parts[0] if len(parts) >= 2 and parts[1] == "blob" else None)
    return result + [None] * (len(revs) - len(result))


def _commit_with_blob(fork: Fork, blob: str, path: str = PATCH_REL) -> Optional[str]:
    """A commit that may be the incoming side (see _incoming_candidates) whose PATH is BLOB."""
    revs = _incoming_candidates(fork)
    for rev, b in zip(revs, _blobs_at(fork, revs, path)):
        if b == blob:
            return rev
    return None


def _commit_for_patch_blob(fork: Fork, blob: str) -> Optional[str]:
    return _commit_with_blob(fork, blob, PATCH_REL)


def _hash(fork: Fork, data: bytes) -> str:
    return fork.git("hash-object", "--no-filters", "--stdin", input=data).decode().strip()


def _nano_entries_at(fork: Fork, rev: str) -> Entries:
    out = fork.git("ls-tree", "-r", "-z", rev, "--", "nano/")
    result: Entries = {}
    for rec in out.split(b"\0"):
        if not rec:
            continue
        meta, _, path = rec.partition(b"\t")
        mode, typ, sha = meta.decode().split()
        p = path.decode("utf-8", "surrogateescape")
        if typ == "blob" and p.startswith("nano/"):
            result[p[len("nano/"):]] = (mode, sha)
    return result


class ForkMerge(object):
    """A merge of two fork commits that both hold nano/ (a step branch into linux, say). The patch
    and block.sha256 are generated, so the merged ones are regenerated from the merge of the two
    nano/ trees, computed from git's objects rather than from the working tree (merge-ort runs
    the drivers before it writes the working tree, merge-recursive in path order)."""

    def __init__(self, ours: str, theirs: str, base: str):
        self.ours, self.theirs, self.base = ours, theirs, base


def identify_fork_merge(fork: Fork, path: str, o_data: bytes, a_data: bytes, b_data: bytes,
                        theirs: Optional[str] = None) -> Tuple[Optional[ForkMerge], str]:
    """The (ours, theirs, merge base) commits of the running merge, checked against the driver's
    three versions of PATH; (None, why) when they can't be told for certain."""
    ours = fork.rev_parse("HEAD")
    if not ours:
        return None, "no HEAD"
    if _blobs_at(fork, [ours], path)[0] != _hash(fork, a_data):
        return None, "this side's %s is not HEAD's (a rebase or cherry-pick?)" % path
    if theirs is None:
        theirs = _commit_with_blob(fork, _hash(fork, b_data), path)
    if not theirs:
        return None, "can't find the incoming commit"
    bases = fork.git("merge-base", "--all", ours, theirs, check=False, ok_codes=(0, 1)).decode().split()
    if len(bases) != 1:
        return None, "%d merge bases" % len(bases)
    if _blobs_at(fork, bases, path)[0] != _hash(fork, o_data):
        return None, "the merge base's %s is not the driver's ancestor version" % path
    return ForkMerge(ours, theirs, bases[0]), ""


MARK = b"@@@ nano_tree.py: the Nano block @@@\n"


def _merge_outside(cache: Cache, o: bytes, a: bytes, b: bytes) -> Optional[bytes]:
    if a == o or a == b:
        return b
    if b == o:
        return a
    merged, nconf = merge_file(cache, a, o, b, ("ours", "base", "theirs"))
    return None if nconf else merged


def compute_fork_merge(layout: Layout, fork: Fork, cache: Cache, fm: ForkMerge, log=_err
                       ) -> Optional[Tuple[bytes, str]]:
    """(merged patch, its block's sha256), or None (with the reasons logged) when nano/ or the
    patch's framework sections do not merge cleanly. Kept in the git directory for a day, since
    git runs the patch's driver and block.sha256's separately, in either order."""
    key = sha256(("%s %s %s" % (fm.ours, fm.theirs, fm.base)).encode())[:32]
    store = os.path.join(fork.git_dir(), "nanotree-merge")
    pfile, sfile = os.path.join(store, key + ".patch"), os.path.join(store, key + ".sha256")
    if os.path.isfile(pfile) and os.path.isfile(sfile):
        with open(pfile, "rb") as f:
            data = f.read()
        with open(sfile, "r", encoding="ascii") as f:
            return data, f.read().strip()
    texts = [fork.show(r, UPSTREAM_REL) for r in (fm.base, fm.ours, fm.theirs)]
    if any(t is None for t in texts):
        log("nanopatch: a side of the merge has no %s" % UPSTREAM_REL)
        return None
    t_o, t_a, t_b = texts
    if t_a == t_b or t_b == t_o:
        cfg_text = t_a
    elif t_a == t_o:
        cfg_text = t_b
    else:
        log("nanopatch: both sides changed %s; merge it by hand" % UPSTREAM_REL)
        return None
    if len({Upstream.from_text(t.decode("utf-8")).base for t in texts}) != 1:
        log("nanopatch: the sides are on different GammaOS bases; run `nano_tree.py import-base` "
            "on one side first")
        return None
    cfg = Upstream.from_text(cfg_text.decode("utf-8"))
    if not cache.has_commit(cfg.base):
        log("nanopatch: the base cache is missing (run nano_tree.py fetch)")
        return None
    patches = [fork.show(r, PATCH_REL) for r in (fm.base, fm.ours, fm.theirs)]
    if any(p is None for p in patches):
        log("nanopatch: a side of the merge has no %s" % PATCH_REL)
        return None
    parts = [Patch.parse(p).split_block(cfg.prefix) for p in patches]
    cache.alternates = [fork.objects_dir()]
    outside = _merge_outside(cache, *[p[0] + MARK + p[2] for p in parts])
    if outside is None or outside.count(MARK) != 1:
        log("nanopatch: both sides changed the same framework sections of the patch (outside the "
            "Nano block)")
        return None
    before, after = outside.split(MARK)
    O, M, T = (_nano_entries_at(fork, r) for r in (fm.base, fm.ours, fm.theirs))
    plan = three_way(cache, O, T, M, labels=("ours", "base", "theirs"))
    if plan.conflicts:
        for rel, why in plan.conflicts:
            log("nanopatch: nano/%s does not merge cleanly: %s" % (rel, why))
        return None
    merged = dict(M)
    for rel in plan.delete:
        merged.pop(rel, None)
    for rel, (mode, data) in plan.write.items():
        merged[rel] = (mode, cache.hash_bytes(data))
    from .regen import regen_bytes
    new_data, new_block = regen_bytes(layout, cfg, fork, cache, before + parts[1][1] + after,
                                      nano=merged, extra_blocks=[parts[2][1], parts[0][1]])
    digest = sha256(new_block)
    try:
        os.makedirs(store, exist_ok=True)
        _prune_store(store)
        with open(pfile + ".tmp", "wb") as f:
            f.write(new_data)
        with open(sfile, "w", encoding="ascii", newline="\n") as f:
            f.write(digest + "\n")
        os.replace(pfile + ".tmp", pfile)
    except OSError:
        pass
    return new_data, digest


_dirty_nano = dirty_nano


def _prune_store(store: str, age: float = 86400.0) -> None:
    now = time.time()
    for n in os.listdir(store):
        p = os.path.join(store, n)
        try:
            if now - os.path.getmtime(p) > age:
                os.remove(p)
        except OSError:
            pass


def merge_patch(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, o_path: str, a_path: str,
                b_path: str, name: str, log=None) -> int:
    """The `nanopatch` merge driver: %O %A %B %P. Writes the result to A_PATH. Exit 0 when the
    merge is complete, 1 when the user has to finish it (nano/ changed, or a conflict)."""
    log = log or _err
    with open(o_path, "rb") as f:
        o_data = f.read()
    with open(a_path, "rb") as f:
        a_data = f.read()
    with open(b_path, "rb") as f:
        b_data = f.read()
    theirs = _commit_for_patch_blob(fork, _hash(fork, b_data))
    if theirs and fork.show(theirs, UPSTREAM_REL) is not None:
        # Both sides are the fork: git merges nano/ itself, and the patch is regenerated from the
        # merge of the two nano/ trees.
        fm, why = identify_fork_merge(fork, PATCH_REL, o_data, a_data, b_data, theirs)
        result = compute_fork_merge(layout, fork, cache, fm, log) if fm else None
        if result is None:
            if not fm:
                log("nanopatch: can't regenerate the patch for this merge (%s)." % why)
            log("nanopatch: the patch was left as this side's. Once nano/ and %s are merged, run "
                "`nano_tree.py regen --force` and `git add %s %s`." % (UPSTREAM_REL, name, BLOCK_SHA_REL))
            return 1
        with open(a_path, "wb") as f:
            f.write(result[0])
        if b"merge-block" not in fork.git("config", "--get", "merge.nanoblock.driver", check=False,
                                          ok_codes=(0, 1)):
            log("nanopatch: the nanoblock driver is not installed (nano_tree.py install-driver), so "
                "%s may conflict; its merged value is %s" % (BLOCK_SHA_REL, result[1]))
        return 0
    if not cache.has_commit(cfg.base):
        log("nanopatch: the base cache is missing (run nano_tree.py fetch); the patch was left "
            "as nano/'s side")
        return 1
    # The block is regenerated from nano/ as it is on disk, so uncommitted or untracked files there
    # would go into the merge commit's patch (android-safety-4).
    dirty = _dirty_nano(fork, cfg)
    if dirty:
        log("nanopatch: nano/ has uncommitted changes or untracked files (%s%s); the patch was left as "
            "this side's. Commit, stash or remove them, abort the merge and merge again."
            % (", ".join(dirty[:5]), ", ..." if len(dirty) > 5 else ""))
        return 1
    o_before, o_block, o_after = Patch.parse(o_data).split_block(cfg.prefix)
    a_before, a_block, a_after = Patch.parse(a_data).split_block(cfg.prefix)
    b_before, b_block, b_after = Patch.parse(b_data).split_block(cfg.prefix)
    if (a_before, a_after) != (o_before, o_after):
        log("nanopatch: warning: this branch edited the patch outside the Nano block; the incoming "
            "side's framework sections are taken")
    if sha256(a_block) != (layout.read_block_sha() or sha256(a_block)):
        log("nanopatch: warning: this branch's Nano block does not match block.sha256")
    try:
        plan, touched = import_blocks(layout, cfg, fork, cache, o_block, b_block, stage_result=False,
                                      log=log)
    except NanoTreeError as e:
        log("nanopatch: %s. If the incoming side moved the GammaOS base, abort the merge and run "
            "`nano_tree.py import-base` first." % e)
        return 1
    from .regen import regen_bytes
    try:
        new_data, new_block = regen_bytes(layout, cfg, fork, cache, b_before + a_block + b_after,
                                          extra_blocks=[b_block])
    except NanoTreeError as e:
        report_conflicts(plan)
        log("nanopatch: %s; the patch was left as this side's." % e)
        return 1
    with open(a_path, "wb") as f:
        f.write(new_data)
    layout.write_block_sha(sha256(new_block))
    if plan.conflicts:
        report_conflicts(plan)
    if plan.conflicts or touched:
        log("nanopatch: nano/ was updated from the incoming patch (%d file(s)). Review it, then: "
            "`git add nano tools/nano-tree %s`, set any executable bit listed below with "
            "`git update-index --chmod=+x`, run `nano_tree.py regen --check`, and commit."
            % (len(touched), name))
        for rel, (mode, _d) in sorted(plan.write.items()):
            if mode == "100755":
                log("  executable: nano/%s" % rel)
        return 1
    return 0


def merge_block(layout: Layout, fork: Fork, cache: Cache, o_path: str, a_path: str, b_path: str,
                name: str, log=None) -> int:
    """The `nanoblock` merge driver for block.sha256 (%O %A %B %P). In a merge of two fork commits
    it writes the hash of the Nano block regenerated from the merged nano/ (what merge_patch
    writes into the patch). Otherwise, or when that merge is not clean, it keeps this side's value
    and exits 1."""
    log = log or _err
    with open(o_path, "rb") as f:
        o_data = f.read()
    with open(a_path, "rb") as f:
        a_data = f.read()
    with open(b_path, "rb") as f:
        b_data = f.read()
    if a_data == b_data:
        return 0
    fm, why = identify_fork_merge(fork, BLOCK_SHA_REL, o_data, a_data, b_data)
    result = compute_fork_merge(layout, fork, cache, fm, log) if fm else None
    if result is None:
        if not fm:
            log("nanoblock: can't compute the merged block's hash (%s)." % why)
        log("nanoblock: %s was left as this side's; once nano/ is merged, run `nano_tree.py regen "
            "--force` and `git add %s`." % (name, name))
        return 1
    with open(a_path, "w", encoding="ascii", newline="\n") as f:
        f.write(result[1] + "\n")
    return 0


# ---------------------------------------------------------------------------------------------
# import-base


def import_base(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, new: str, log=print) -> int:
    from .regen import regen
    if not cache.has_commit(new):
        raise NanoTreeError("the cache lacks %s: run `nano_tree.py fetch --base %s` first" % (new, new))
    cache.alternates = [fork.objects_dir()]
    old_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
    new_tree = cache.ls_tree("%s:%s" % (new, cfg.path))
    missing = cache.missing_objects(sorted({e[1] for e in new_tree.values()}))
    if missing:
        raise NanoTreeError("%d blob(s) of %s's Nano folder are missing from the cache: run "
                            "`nano_tree.py fetch --base %s`" % (len(missing), new[:12], new))
    nano = worktree_entries(fork, cache, layout.root)
    M = composed_entries(cfg, old_tree, nano)
    O = {r: e for r, e in old_tree.items() if not cfg.is_fork_only(r)}
    T = {r: e for r, e in new_tree.items() if not cfg.is_fork_only(r)}
    M = {r: e for r, e in M.items() if not cfg.is_fork_only(r)}
    plan = three_way(cache, O, T, M, labels=("nano/", "old base", "new base"))
    old_base = cfg.base
    cfg.base = new
    cfg.save()
    touched, modes = apply_plan(layout, cfg, plan, new_tree, nano, cache)
    # Overlay asset files that the new base now carries unchanged are no longer needed in nano/.
    for rel, ent in sorted(nano.items()):
        if cfg.is_asset(rel) and rel not in plan.write and new_tree.get(rel) == ent:
            os.remove(os.path.join(layout.nano, *rel.split("/")))
            touched.append(rel)
    stage(fork, touched, modes)
    log("import-base: %s -> %s: %d written, %d deleted, %d conflict(s)"
        % (old_base[:12], new[:12], len(plan.write), len(plan.delete), len(plan.conflicts)))
    if plan.conflicts:
        report_conflicts(plan)
        _err("import-base: resolve the conflicts, `git add` them, then run nano_tree.py regen --force")
        return 1
    log("import-base: the patch's sections outside the Nano block still target the old base")
    return regen(layout, cfg, fork, cache, force=True, log=log)
