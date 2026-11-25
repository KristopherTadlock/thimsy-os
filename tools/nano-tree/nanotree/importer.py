"""Bringing changes into nano/: an applied AOSP folder (import-tree), nano-144's patch changes
(import-patch and the merge driver), and a new GammaOS base (import-base). IMPLEMENTATION.md A.5."""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

from .compose import MARKER, Entries, composed_entries, files_problems, tracked_entries, worktree_entries
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
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
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
    ignored = set()
    if files:
        out = fork.git("check-ignore", "--no-index", "-z", "--stdin",
                       input=b"\0".join(("nano/" + r).encode("utf-8", "surrogateescape") for r in files) + b"\0",
                       ok_codes=(0, 1))
        ignored = {p.decode("utf-8", "surrogateescape")[len("nano/"):] for p in out.split(b"\0") if p}
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
        if old != data:
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            with open(dp, "wb") as f:
                f.write(data)
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
    for d in (O, T, M):
        for rel in [r for r in d if cfg.is_fork_only(r)]:
            del d[rel]
    plan = three_way(cache, O, T, M)
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


def _commit_for_patch_blob(fork: Fork, blob: str) -> Optional[str]:
    """A local commit (branch, remote branch, MERGE_HEAD or FETCH_HEAD) whose patch is BLOB."""
    out = fork.git("for-each-ref", "--format=%(objectname)", "refs/heads", "refs/remotes", check=False)
    revs = out.decode().split()
    for special in ("MERGE_HEAD", "FETCH_HEAD", "AUTO_MERGE"):
        r = fork.rev_parse(special)
        if r:
            revs.append(r)
    if not revs:
        return None
    query = "".join("%s:%s\n" % (r, PATCH_REL) for r in revs).encode()
    res = fork.git("cat-file", "--batch-check", input=query, check=False).decode().splitlines()
    for rev, line in zip(revs, res):
        if line.split()[:1] == [blob]:
            return rev
    return None


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
    b_blob = fork.git("hash-object", "--no-filters", "--stdin", input=b_data).decode().strip()
    theirs = _commit_for_patch_blob(fork, b_blob)
    if theirs and fork.show(theirs, "tools/nano-tree/upstream.json") is not None:
        log("nanopatch: both sides changed the generated Nano patch and the incoming side (%s) has "
            "nano/ too: git merges nano/ itself. After resolving nano/, run `nano_tree.py regen "
            "--force` and `git add %s`." % (theirs[:12], name))
        return 1
    if not cache.has_commit(cfg.base):
        log("nanopatch: the base cache is missing (run nano_tree.py fetch); the patch was left "
            "as nano/'s side")
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
    new_data, new_block = regen_bytes(layout, cfg, fork, cache, b_before + a_block + b_after,
                                      extra_blocks=[b_block])
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
