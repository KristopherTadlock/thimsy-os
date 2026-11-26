"""Absolute-path roots (docs/linux/IMPLEMENTATION.md B.5, H.4): every root an absolute path literal in
nano/'s shared runtime files names has a row in tools/nano-tree/path-roots.json, every link row lands
where linux/board/common/post-build.d/10-android-links.sh puts it, and the shared files that name raw
block devices are the known ones. Fork-only: never gates an Android build. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_path_roots.py

`python3 tools/nano-tree/tests/test_path_roots.py --list` prints every root with its row.
"""
from __future__ import annotations

import json
import os
import re
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.config import Upstream  # noqa: E402
from nanotree.pathroots import check_table, path_literals, root_of, runtime_files  # noqa: E402
from nanotree.ratchets import _read, lex, shared_sources  # noqa: E402

NANO = os.path.join(ROOT, "nano")
TABLE = os.path.join(TOOL_DIR, "path-roots.json")
LINKS_SCRIPT = os.path.join(ROOT, "linux", "board", "common", "post-build.d", "10-android-links.sh")
# Shared files that name a raw block device (B.5, "Raw block devices"): reviewed, one today.
BLOCK_DEVICE_FILES = ["NanoScreenCal.cpp"]
BLOCK_DEVICE = re.compile(r"(?<!/sys)/dev/block|mmcblk")   # not sysfs's /sys/dev/block


def _load():
    with open(TABLE, "r", encoding="utf-8") as f:
        return json.load(f)


def _files():
    return runtime_files(shared_sources(NANO, Upstream.load(ROOT)))


def _script_links():
    """{android path: target} from the script's `place LINK TARGET` lines."""
    with open(LINKS_SCRIPT, "r", encoding="utf-8") as f:
        text = f.read()
    return {"/" + m.group(1): m.group(2) for m in re.finditer(r"^place\s+(\S+)\s+(\S+)\s*$", text, re.M)}


@unittest.skipUnless(os.path.isdir(NANO) and os.path.isfile(TABLE), "no nano/ or path-roots.json here")
class PathRoots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = _load()
        cls.files = _files()
        cls.roots = path_literals(NANO, cls.files)

    def test_every_root_has_a_row(self):
        problems = check_table(self.table, self.roots)
        self.assertFalse(problems, "tools/nano-tree/path-roots.json:\n  " + "\n  ".join(problems))

    def test_links_match_the_image(self):
        links = _script_links()
        self.assertIn("/data", links)
        for r in self.table["rows"]:
            if r["disposition"] != "link":
                continue
            root = r["root"]
            best = max((p for p in links if root == p or root.startswith(p + "/")), key=len, default=None)
            self.assertIsNotNone(best, "%s is a link row, but 10-android-links.sh makes no link for it" % root)
            want = links[best] + root[len(best):]
            self.assertEqual(r["linux"], want, "%s: the row says %s, the image gives %s" % (root, r["linux"], want))

    def test_every_image_link_is_used(self):
        rows = {r["root"] for r in self.table["rows"]}
        for p in _script_links():
            self.assertTrue(p in rows or any(x.startswith(p + "/") for x in rows),
                            "10-android-links.sh links %s, but no shared file names it" % p)

    def test_block_device_files(self):
        hits = []
        for rel in shared_sources(NANO, Upstream.load(ROOT)):
            if rel.startswith(("test/", "compat/")):
                continue
            _code, strings = lex(_read(os.path.join(NANO, *rel.split("/"))))
            if any(BLOCK_DEVICE.search(s) for _l, s in strings):
                hits.append(rel)
        self.assertEqual(sorted(hits), sorted(BLOCK_DEVICE_FILES),
                         "shared files naming /dev/block or mmcblk changed; review the new ones against B.5 "
                         "(raw block devices) and update BLOCK_DEVICE_FILES")


class Unit(unittest.TestCase):
    def test_root_of(self):
        self.assertEqual(root_of("/data/system/nano.json"), "/data/system")
        self.assertEqual(root_of("/storage/emulated/0"), "/storage/emulated")
        self.assertEqual(root_of("/storage/1234-ABCD/roms"), "/storage/*")
        self.assertEqual(root_of("/storage/"), "/storage/*")
        self.assertEqual(root_of("/proc/%d/cmdline"), "/proc/*")
        self.assertEqual(root_of("/sdcard/ROMs"), "/sdcard")
        self.assertEqual(root_of("/metadata/gammaos"), "/metadata")

    def test_check_table(self):
        roots = {"/data/system": {"a.cpp"}, "/proc/self": {"b.cpp"}, "/proc": {"c.cpp"}, "/new": {"d.cpp"}}
        rows = [
            {"root": "/data/system", "disposition": "link", "linux": "/x", "note": "n"},
            {"root": "/proc/*", "disposition": "kernel", "note": "n"},
            {"root": "/gone", "disposition": "same", "note": "n"},
            {"root": "/bad", "disposition": "link", "note": "n"},
        ]
        problems = check_table({"rows": rows}, roots)
        self.assertTrue(any("no row for /new" in p for p in problems), problems)
        self.assertTrue(any("stale row /gone" in p for p in problems), problems)
        self.assertTrue(any("/bad: a link without" in p for p in problems), problems)
        self.assertFalse(any("/proc" in p for p in problems), problems)


def _list() -> int:
    rows = {r["root"]: r for r in _load()["rows"]}
    for root, files in sorted(path_literals(NANO, _files()).items()):
        r = rows.get(root) or rows.get("/%s/*" % root.split("/")[1])
        print("%-24s %-12s %-40s %s" % (root, r["disposition"] if r else "MISSING", (r or {}).get("linux", ""),
                                        ", ".join(sorted(files)[:2])))
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--list"]:
        sys.exit(_list())
    unittest.main()
