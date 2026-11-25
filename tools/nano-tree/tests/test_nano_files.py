"""nano/ holds exactly the A.2 set (docs/linux/IMPLEMENTATION.md A.2 and A.6): the files the
patch's Nano block adds or modifies, the base's untouched files outside the asset folders (checked
when the base cache work/nano-base/ exists) and fork-only files; nothing the block touches is
ignored; the scraper credentials stay ignored; the index's executable bits are the patch's."""
from __future__ import annotations

import os
import subprocess
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.compose import files_problems, tracked_entries  # noqa: E402
from nanotree.config import Layout, Upstream  # noqa: E402
from nanotree.gitcache import Cache, Fork  # noqa: E402
from nanotree.patchfile import Patch  # noqa: E402

NANO = os.path.join(ROOT, "nano")
CREDENTIALS = ("screenscraper_dev.local.txt", "NanoScraperDevCreds.gen.h")


def git(*args, input=None):
    return subprocess.run(["git"] + list(args), cwd=ROOT, input=input, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)


@unittest.skipUnless(os.path.isdir(NANO), "no nano/ in this checkout")
class NanoFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = Layout(ROOT)
        cls.cfg = Upstream.load(ROOT)
        cls.fork = Fork(ROOT)
        with open(cls.layout.patch, "rb") as f:
            cls.block = Patch.parse(f.read()).block_sections(cls.cfg.prefix)
        cls.tracked = tracked_entries(cls.fork)
        cache = Cache(cls.layout.cache)
        cls.base_tree = None
        if cache.exists() and cache.has_commit(cls.cfg.base):
            cls.base_tree = cache.ls_tree("%s:%s" % (cls.cfg.base, cls.cfg.path))

    def rel(self, section):
        return section.path[len(self.cfg.prefix):]

    def test_set_is_the_a2_rule(self):
        problems = files_problems(self.cfg, self.tracked, self.block, self.base_tree)
        self.assertEqual(problems, [], "\n".join(problems[:40]))

    def test_base_part_checked(self):
        if self.base_tree is None:
            self.skipTest("no base cache work/nano-base/ (nano_tree.py fetch): base files not checked")
        assets = [r for r in self.tracked if self.cfg.is_asset(r)]
        in_block = {self.rel(s) for s in self.block}
        self.assertTrue(set(assets) <= in_block, "assets in nano/ without a section")

    def test_block_paths_not_ignored(self):
        paths = ["nano/" + self.rel(s) for s in self.block if not s.deleted]
        proc = git("check-ignore", "--no-index", "-z", "--stdin",
                   input=b"\0".join(p.encode("utf-8", "surrogateescape") for p in paths) + b"\0")
        self.assertIn(proc.returncode, (0, 1), proc.stderr)
        ignored = [p for p in proc.stdout.decode("utf-8", "replace").split("\0") if p]
        self.assertEqual(ignored, [], "paths of the patch that git would ignore under nano/")

    def test_archive_fixtures_tracked(self):
        sevens = sorted(r for r in self.tracked if r.endswith(".7z"))
        self.assertEqual(len(sevens), 6, sevens)

    def test_credentials_ignored(self):
        for name in CREDENTIALS:
            proc = git("check-ignore", "--no-index", "-q", "nano/" + name)
            self.assertEqual(proc.returncode, 0, "nano/%s is not ignored" % name)
            self.assertNotIn(name, self.tracked)

    def test_modes_are_the_patchs(self):
        """Every path with a section has the section's mode in the index; with the cache, every
        other path has the base's mode."""
        wrong = []
        for s in self.block:
            if s.deleted:
                continue
            rel = self.rel(s)
            want = s.new_mode()
            have = self.tracked.get(rel, (None,))[0]
            if want and have != want:
                wrong.append("%s: index %s, patch %s" % (rel, have, want))
        if self.base_tree is not None:
            in_block = {self.rel(s) for s in self.block}
            for rel, (mode, _sha) in self.tracked.items():
                if rel not in in_block and rel in self.base_tree and self.base_tree[rel][0] != mode:
                    wrong.append("%s: index %s, base %s" % (rel, mode, self.base_tree[rel][0]))
        self.assertEqual(wrong, [])
        executables = sorted(r for r, (m, _s) in self.tracked.items() if m == "100755")
        self.assertIn("nano-typec-guard.sh", executables)


if __name__ == "__main__":
    unittest.main()
