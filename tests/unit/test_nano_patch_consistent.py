"""patches/thimsy/0001-thimsy.patch's Nano block agrees with nano/, without the base cache
(docs/linux/IMPLEMENTATION.md A.6): every section's post-image id is the blob of nano/'s file and
its mode the index's; every file the block adds or modifies is in nano/, and every nano/ file
outside the fork-only globs has a section (or, checked only with the cache, equals the base);
tools/nano-tree/block.sha256 is the block's hash; and the patch outside the Nano block is what
nano-144 has at the merge base. Skips where there is no nano/ (nano-144 before the merge back).
"""
from __future__ import annotations

import os
import subprocess
import sys
import unittest

from tests import util

NANO_FILES = os.path.join(util.ROOT, "nano")
TOOL_DIR = os.path.join(util.ROOT, "tools", "nano-tree")
PATCH = os.path.join(util.ROOT, "patches", "thimsy", "0001-thimsy.patch")
HAVE = os.path.isdir(NANO_FILES) and os.path.isfile(os.path.join(TOOL_DIR, "upstream.json"))
if HAVE and TOOL_DIR not in sys.path:
    sys.path.insert(0, TOOL_DIR)


def git(*args, input=None):
    proc = subprocess.run(["git"] + list(args), cwd=util.ROOT, input=input, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    return proc.returncode, proc.stdout


@unittest.skipUnless(HAVE, "no nano/ in this checkout (nano-144 keeps Nano only in the patch)")
class NanoPatchConsistent(util.TestCase):
    @classmethod
    def setUpClass(cls):
        from nanotree.compose import tracked_entries
        from nanotree.config import Layout, Upstream
        from nanotree.gitcache import Cache, Fork
        from nanotree.patchfile import Patch
        cls.Patch = Patch
        cls.cfg = Upstream.load(util.ROOT)
        cls.layout = Layout(util.ROOT)
        with open(PATCH, "rb") as f:
            cls.data = f.read()
        cls.patch = Patch.parse(cls.data)
        cls.before, cls.block, cls.after = cls.patch.split_block(cls.cfg.prefix)
        cls.sections = cls.patch.block_sections(cls.cfg.prefix)
        cls.tracked = tracked_entries(Fork(util.ROOT))
        cache = Cache(cls.layout.cache)
        cls.base_tree = None
        if cache.exists() and cache.has_commit(cls.cfg.base):
            cls.base_tree = cache.ls_tree("%s:%s" % (cls.cfg.base, cls.cfg.path))

    def rel(self, s):
        return s.path[len(self.cfg.prefix):]

    def test_post_images_are_nano_files(self):
        live = [s for s in self.sections if not s.deleted]
        missing = [self.rel(s) for s in live if not os.path.isfile(os.path.join(NANO_FILES, self.rel(s)))]
        self.assertEqual(missing, [], "files the patch adds or modifies but nano/ lacks")
        paths = "\n".join(os.path.join(NANO_FILES, self.rel(s)).replace("\\", "/") for s in live) + "\n"
        rc, out = git("hash-object", "--no-filters", "--stdin-paths", input=paths.encode("utf-8"))
        self.assertEqual(rc, 0)
        wrong = []
        for s, sha in zip(live, out.decode().split()):
            if not sha.startswith(s.post_id()):
                wrong.append("%s: patch %s, nano/ %s" % (self.rel(s), s.post_id(), sha[:12]))
        self.assertEqual(wrong, [], "nano/ differs from the patch; run tools/nano-tree/nano_tree.py regen")

    def test_deleted_files_are_gone(self):
        present = [self.rel(s) for s in self.sections
                   if s.deleted and os.path.exists(os.path.join(NANO_FILES, self.rel(s)))]
        self.assertEqual(present, [])

    def test_modes_are_the_index_modes(self):
        wrong = []
        for s in self.sections:
            want = s.new_mode()
            have = self.tracked.get(self.rel(s), (None,))[0]
            if want and have != want:
                wrong.append("%s: patch %s, index %s" % (self.rel(s), want, have))
        self.assertEqual(wrong, [])

    def test_every_nano_file_has_a_section(self):
        """Fetched assets are never in nano/ unchanged; other files without a section must equal
        the base (checked when the base cache exists)."""
        in_block = {self.rel(s) for s in self.sections}
        stray = []
        for rel, (mode, sha) in sorted(self.tracked.items()):
            if rel in in_block or self.cfg.is_fork_only(rel):
                continue
            if self.cfg.is_asset(rel):
                stray.append("%s: an asset without a section (it is fetched from the base)" % rel)
            elif self.base_tree is not None and self.base_tree.get(rel) != (mode, sha):
                stray.append("%s: differs from the base but has no section" % rel)
        self.assertEqual(stray, [])

    def test_block_hash_recorded(self):
        from nanotree.patchfile import sha256
        self.assertEqual(self.layout.read_block_sha(), sha256(self.block),
                         "tools/nano-tree/block.sha256 is not the Nano block's hash: the block was "
                         "changed outside nano_tree.py regen (import it with nano_tree.py import-patch)")

    def test_block_contiguous_and_sorted(self):
        self.assertEqual(self.patch.unsorted(), [])
        self.assertTrue(self.sections)

    def test_outside_block_unchanged(self):
        """The fork never edits the framework parts of the patch: before and after the Nano block,
        the patch is what nano-144 had at the merge base."""
        rc, out = git("rev-parse", "--verify", "-q", "nano-144^{commit}")
        if rc != 0:
            self.skipTest("no local nano-144 branch")
        rc, mb = git("merge-base", "HEAD", "nano-144")
        self.assertEqual(rc, 0)
        rc, old = git("cat-file", "blob", "%s:patches/thimsy/0001-thimsy.patch" % mb.decode().strip())
        self.assertEqual(rc, 0)
        before, _block, after = self.Patch.parse(old).split_block(self.cfg.prefix)
        self.assertTrue(before == self.before, "the patch before the Nano block differs from nano-144's")
        self.assertTrue(after == self.after, "the patch after the Nano block differs from nano-144's")


if __name__ == "__main__":
    unittest.main()
