"""Ratchets on nano/'s shared files (docs/linux/IMPLEMENTATION.md H.4). Fork-only checks: they live
outside tests/, so they never gate an Android release build. Run them with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests

`python3 tools/nano-tree/tests/test_nano_ratchets.py --update-props` rewrites props.txt after a
deliberate change of property keys (or after the merge driver imported new ones from nano-144).
"""
from __future__ import annotations

import os
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.config import Upstream  # noqa: E402
from nanotree.ratchets import (platform_conditionals, property_keys, read_list,  # noqa: E402
                               shared_sources, write_list)

NANO = os.path.join(ROOT, "nano")
PROPS = os.path.join(TOOL_DIR, "props.txt")
# __ANDROID__ and NANO_PLATFORM_* in shared files: none today, and none may be added in passes
# P0 to P3 (the seam is cut with shims, stubs and link-time substitution, not #ifdefs).
PLATFORM_CONDITIONAL_BUDGET = 0
PROPS_HEADER = """Property key literals in nano/'s shared C and C++ files (tools/nano-tree/nanotree/ratchets.py).
The Linux config store serves these names (IMPLEMENTATION.md B.3); test_nano_ratchets.py fails
when the set changes. After a deliberate change, or a merge that brought new keys from nano-144:
  python3 tools/nano-tree/tests/test_nano_ratchets.py --update-props"""


def _sources():
    cfg = Upstream.load(ROOT)
    return shared_sources(NANO, cfg)


def update_props() -> int:
    keys = property_keys(NANO, _sources())
    write_list(PROPS, PROPS_HEADER, list(keys))
    print("props.txt: %d keys" % len(keys))
    return 0


@unittest.skipUnless(os.path.isdir(NANO), "no nano/ in this checkout")
class Ratchets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = _sources()
        cls.keys = property_keys(NANO, cls.files)

    def test_property_keys_match_baseline(self):
        keys = self.keys
        baseline = set(read_list(PROPS))
        added = sorted(set(keys) - baseline)
        removed = sorted(baseline - set(keys))
        msg = []
        for k in added:
            msg.append("new key %s (in %s)" % (k, ", ".join(keys[k][:3])))
        for k in removed:
            msg.append("key gone: %s" % k)
        self.assertFalse(msg, "property keys differ from tools/nano-tree/props.txt; if deliberate, run "
                         "tools/nano-tree/tests/test_nano_ratchets.py --update-props:\n  " + "\n  ".join(msg[:40]))

    def test_no_new_platform_conditionals(self):
        hits = list(platform_conditionals(NANO, self.files))
        self.assertLessEqual(len(hits), PLATFORM_CONDITIONAL_BUDGET,
                             "__ANDROID__ / NANO_PLATFORM_* in shared files (budget %d):\n  %s" % (
                                 PLATFORM_CONDITIONAL_BUDGET,
                                 "\n  ".join("%s:%d %s" % h for h in hits[:40])))

    def test_sources_found(self):
        self.assertIn("NanoMenu.cpp", self.files)
        self.assertGreater(len(self.files), 200)


if __name__ == "__main__":
    if sys.argv[1:] == ["--update-props"]:
        sys.exit(update_props())
    unittest.main()
