"""The Nano block of the patch, built from nano/ with pinned git plumbing (IMPLEMENTATION.md A.3)."""
from __future__ import annotations

from typing import List, Optional, Tuple

from .compose import Entries, worktree_entries
from .config import Layout, NanoTreeError, Upstream
from .gitcache import ZERO, Cache, Fork
from .patchfile import Patch, first_difference, sha256


def diff_args(cfg: Upstream) -> List[str]:
    return ["diff-index", "--cached", "-p", "--binary", "--abbrev=%d" % cfg.abbrev, "--no-renames",
            "--no-textconv", "--no-ext-diff", "--no-color", "--diff-algorithm=myers",
            "--src-prefix=a/", "--dst-prefix=b/", "--ignore-submodules=none"]


def stage_tree(cfg: Upstream, cache: Cache, base_tree: Entries, nano: Entries) -> str:
    """A temporary index holding the base with nano/ applied (fork-only paths left as the base has
    them). Returns the index path; the caller cleans up through cache.cleanup()."""
    prefix = cfg.prefix
    index = cache.base_index(cfg.base, prefix)
    deleted = set(cfg.deleted)
    entries = []
    for rel, (mode, sha) in base_tree.items():
        if cfg.is_fork_only(rel):
            continue
        if rel in nano:
            continue
        if not cfg.is_asset(rel) or rel in deleted:
            entries.append(("0", ZERO, prefix + rel))
    for rel, (mode, sha) in nano.items():
        if cfg.is_fork_only(rel):
            continue
        entries.append((mode, sha, prefix + rel))
    cache.index_info(index, entries)
    return index


def diff_block(cfg: Upstream, cache: Cache, index: str) -> bytes:
    # git before 2.31 ignores --abbrev for the patch's `index` lines and uses core.abbrev, so set
    # both. Binary sections are deflated at core.compression (set to 1, git's default, which
    # matches the committed patch) by Cache.git.
    args = ["-c", "core.abbrev=%d" % cfg.abbrev] + diff_args(cfg) + [cfg.base, "--", cfg.prefix]
    return cache.git(*args, index=index)


def _same_header(a, b) -> bool:
    keys = ("index", "new file mode", "deleted file mode", "old mode", "new mode")
    return all(a.header.get(k) == b.header.get(k) for k in keys)


def keep_committed_binaries(generated: bytes, committed_blocks: List[bytes]) -> bytes:
    """Binary sections are zlib output, which differs between zlib builds and settings: where a
    binary section's header (its `index` line and modes) equals a committed one's, keep the
    committed bytes."""
    gen = Patch.parse(generated)
    old: dict = {}
    for block in committed_blocks:
        for s in Patch.parse(block).sections:
            if s.binary:
                old.setdefault(s.path, []).append(s)
    changed = False
    for i, s in enumerate(gen.sections):
        if not s.binary:
            continue
        for o in old.get(s.path, ()):
            if _same_header(o, s):
                if o.data != s.data:
                    gen.sections[i] = o
                    changed = True
                break
    return gen.to_bytes() if changed else generated


def build_block(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, committed_block: bytes,
                nano: Optional[Entries] = None, extra_blocks: Optional[List[bytes]] = None) -> bytes:
    if not cache.has_commit(cfg.base):
        raise NanoTreeError("the cache %s lacks the base commit %s: run nano_tree.py fetch "
                            "(or fetch --from-cache DIR)" % (cache.path, cfg.base))
    cache.alternates = [fork.objects_dir()]
    try:
        base_tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
        if nano is None:
            nano = worktree_entries(fork, cache, layout.root)
        index = stage_tree(cfg, cache, base_tree, nano)
        generated = diff_block(cfg, cache, index)
    finally:
        cache.cleanup()
    return keep_committed_binaries(generated, [committed_block] + list(extra_blocks or []))


def read_patch(layout: Layout) -> bytes:
    with open(layout.patch, "rb") as f:
        return f.read()


def regen(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, check: bool = False,
          force: bool = False, log=print) -> int:
    data = read_patch(layout)
    patch = Patch.parse(data)
    before, block, after = patch.split_block(cfg.prefix)
    new_block = build_block(layout, cfg, fork, cache, block)
    new_data = before + new_block + after
    recorded = layout.read_block_sha()
    if check:
        status = 0
        diff = first_difference(data, new_data)
        if diff is not None:
            n, a, b = diff
            log("regen --check: the patch differs from what nano/ produces, first at line %d:" % n)
            log("  committed: %s" % a.decode("utf-8", "replace")[:200])
            log("  nano/:     %s" % b.decode("utf-8", "replace")[:200])
            status = 1
        if recorded != sha256(block):
            log("regen --check: tools/nano-tree/block.sha256 (%s) is not the committed block's hash "
                "(%s)" % (recorded, sha256(block)))
            status = 1
        if status == 0:
            log("regen --check: ok (%d bytes, block %s)" % (len(data), sha256(block)[:16]))
        return status
    if recorded is not None and recorded != sha256(block) and not force:
        raise NanoTreeError(
            "the patch's Nano block was changed outside regen (its hash is not block.sha256): a "
            "merge or an edit brought changes nano/ does not have. Import them first with "
            "`nano_tree.py import-patch` (see --help), or pass --force to overwrite them")
    if recorded is None and new_block != block and not force:
        raise NanoTreeError("no tools/nano-tree/block.sha256 and the block would change: pass --force")
    if new_data != data:
        with open(layout.patch, "wb") as f:
            f.write(new_data)
        log("regen: wrote %s (%d bytes)" % (layout.patch, len(new_data)))
    else:
        log("regen: the patch is already up to date")
    layout.write_block_sha(sha256(new_block))
    return 0


def regen_bytes(layout: Layout, cfg: Upstream, fork: Fork, cache: Cache, patch_data: bytes,
                nano: Optional[Entries] = None, extra_blocks: Optional[List[bytes]] = None
                ) -> Tuple[bytes, bytes]:
    """PATCH_DATA with its Nano block regenerated from nano/; returns (new patch, new block)."""
    before, block, after = Patch.parse(patch_data).split_block(cfg.prefix)
    new_block = build_block(layout, cfg, fork, cache, block, nano, extra_blocks)
    return before + new_block + after, new_block
