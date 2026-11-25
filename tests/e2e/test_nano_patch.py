"""The patch's Nano block against the GammaOS base (docs/linux/IMPLEMENTATION.md A.6), with the base
cache work/nano-base/ (tools/nano-tree/nano_tree.py fetch; skips without it): regen --check passes
(the committed patch is exactly what nano/ produces), the block applies to the base and gives
compose's tree, and a synthetic nano-144 change imported into a scratch clone gives back exactly
the changed block. Skips where there is no nano/ (nano-144 before the merge back)."""
from __future__ import annotations

import os
import stat
import subprocess
import sys
import unittest

from tests import util

NANO_FILES = os.path.join(util.ROOT, "nano")
TOOL_DIR = os.path.join(util.ROOT, "tools", "nano-tree")
TOOL = os.path.join(TOOL_DIR, "nano_tree.py")
CACHE = os.path.join(util.ROOT, "work", "nano-base")
HAVE = os.path.isdir(NANO_FILES) and os.path.isfile(TOOL)
if HAVE and TOOL_DIR not in sys.path:
    sys.path.insert(0, TOOL_DIR)


def git(cwd, *args, input=None):
    proc = subprocess.run(["git"] + list(args), cwd=cwd, input=input, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise AssertionError("git %s: %s" % (" ".join(args), proc.stderr.decode("utf-8", "replace")))
    return proc.stdout


def make_writable(top):
    """Git writes its objects read-only, which Windows refuses to delete."""
    for dirpath, _dirs, names in os.walk(top):
        for n in names:
            try:
                os.chmod(os.path.join(dirpath, n), stat.S_IREAD | stat.S_IWRITE)
            except OSError:
                pass


@unittest.skipUnless(HAVE, "no nano/ in this checkout")
class NanoPatch(util.TestCase):
    def setUp(self):
        super().setUp()
        from nanotree.config import Layout, Upstream
        from nanotree.gitcache import Cache, Fork
        self.cfg = Upstream.load(util.ROOT)
        self.layout = Layout(util.ROOT)
        self.fork = Fork(util.ROOT)
        self.cache = Cache(CACHE)
        if not (self.cache.exists() and self.cache.has_commit(self.cfg.base)):
            self.skipTest("no base cache: run tools/nano-tree/nano_tree.py fetch (or fetch --from-cache DIR)")

    def test_regen_matches(self):
        proc = util.run([sys.executable, TOOL, "regen", "--check"], cwd=util.ROOT, timeout=600)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("regen --check: ok", proc.stdout)

    def test_patch_applies_to_base(self):
        from nanotree.compose import composed_entries, worktree_entries
        from nanotree.importer import tree_with_block
        from nanotree.patchfile import Patch
        with open(self.layout.patch, "rb") as f:
            block = Patch.parse(f.read()).split_block(self.cfg.prefix)[1]
        self.cache.alternates = [self.fork.objects_dir()]
        applied = tree_with_block(self.cfg, self.cache, self.cfg.base, block)
        base_tree = self.cache.ls_tree("%s:%s" % (self.cfg.base, self.cfg.path))
        composed = composed_entries(self.cfg, base_tree, worktree_entries(self.fork, self.cache, util.ROOT))
        composed = {r: e for r, e in composed.items() if not self.cfg.is_fork_only(r)}
        self.assertEqual(sorted(set(applied) ^ set(composed)), [])
        diff = sorted(r for r in applied if applied[r] != composed[r])
        self.assertEqual(diff, [])

    @util.slow
    def test_import_round_trip(self):
        """One hunk edited, one file added, one deleted on a synthetic nano-144 side: imported into
        a scratch clone's nano/, the regenerated block is exactly nano-144's."""
        from nanotree.config import Layout
        from nanotree.gitcache import Cache, Fork
        from nanotree.importer import import_blocks
        from nanotree.patchfile import Patch
        from nanotree.regen import regen, regen_bytes
        head = git(util.ROOT, "rev-parse", "HEAD").decode().strip()
        if git(util.ROOT, "status", "--porcelain", "--", "nano", "patches").strip():
            self.skipTest("nano/ or the patch have uncommitted changes (the scratch clone is of HEAD)")
        scratch = self.path("clone")
        self.addCleanup(make_writable, scratch)
        git(self.tmp, "clone", "-q", "--shared", "--no-checkout", util.ROOT.replace("\\", "/"), "clone")
        git(scratch, "config", "core.autocrlf", "false")
        git(scratch, "checkout", "-q", "--detach", head)
        proc = util.run([sys.executable, os.path.join(scratch, "tools", "nano-tree", "nano_tree.py"),
                         "fetch", "--from-cache", CACHE], cwd=scratch)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        layout, fork, cache = Layout(scratch), Fork(scratch), Cache(os.path.join(scratch, "work", "nano-base"))
        with open(layout.patch, "rb") as f:
            committed = f.read()
        old_block = Patch.parse(committed).split_block(self.cfg.prefix)[1]
        # The nano-144 side: made in the scratch clone's working tree, then undone.
        edited = os.path.join(scratch, "nano", "NanoTypec.cpp")
        with open(edited, "rb") as f:
            original = f.read()
        changed = original.replace(b"\n", b"\n// round trip\n", 1)
        with open(edited, "wb") as f:
            f.write(changed)
        added = os.path.join(scratch, "nano", "RoundTripAdded.cpp")
        with open(added, "wb") as f:
            f.write(b"int roundTrip = 1;\n")
        os.remove(os.path.join(scratch, "nano", "NanoMonitorMode.h"))
        _new_patch, new_block = regen_bytes(layout, self.cfg, fork, cache, committed)
        self.assertNotEqual(new_block, old_block)
        git(scratch, "checkout", "-q", "--", "nano")
        os.remove(added)
        plan, _touched = import_blocks(layout, self.cfg, fork, cache, old_block, new_block, log=lambda m: None)
        self.assertEqual(plan.conflicts, [])
        self.assertEqual(regen(layout, self.cfg, fork, cache, force=True, log=lambda m: None), 0)
        with open(layout.patch, "rb") as f:
            self.assertEqual(Patch.parse(f.read()).split_block(self.cfg.prefix)[1], new_block)
        with open(edited, "rb") as f:
            self.assertEqual(f.read(), changed)
        self.assertFalse(os.path.exists(os.path.join(scratch, "nano", "NanoMonitorMode.h")))


if __name__ == "__main__":
    unittest.main()
