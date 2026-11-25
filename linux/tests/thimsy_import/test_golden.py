"""E.7: the import equals the golden output, and a second run writes identical bytes."""

from __future__ import annotations

import os
import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402


class GoldenTest(unittest.TestCase):
    def test_golden_equality(self):
        with c.TempDir() as tmp:
            for fam in c.FAMILIES:
                code, out, err = c.run(tmp, fam)
                self.assertEqual(code, 0, err)
            got = c.tree_bytes(tmp)
            if os.environ.get("THIMSY_REGEN_GOLDEN"):
                shutil.rmtree(str(c.EXPECTED), ignore_errors=True)
                shutil.copytree(str(tmp), str(c.EXPECTED))
            want = c.tree_bytes(c.EXPECTED)
            self.assertEqual(sorted(got), sorted(want), "file sets differ")
            for rel in sorted(want):
                self.assertEqual(got[rel], want[rel], "%s differs from the golden file" % rel)

    def test_second_run_writes_nothing(self):
        with c.TempDir() as tmp:
            for fam in c.FAMILIES:
                self.assertEqual(c.run(tmp, fam)[0], 0)
            before = c.snapshot(tmp)
            for fam in c.FAMILIES:
                code, out, err = c.run(tmp, fam)
                self.assertEqual(code, 0, err)
                self.assertIn("wrote 0, removed 0", out)
            self.assertEqual(c.snapshot(tmp), before)

    def test_windows_checkout_imports_the_same(self):
        """CRLF line ends (core.autocrlf) on every text file give the same bytes."""
        with c.TempDir() as tmp:
            src = c.copy_fixture(tmp)
            for dirpath, _dirs, files in os.walk(str(src)):
                for f in files:
                    p = Path(dirpath) / f
                    data = p.read_bytes()
                    if b"\n" in data:
                        p.write_bytes(data.replace(b"\n", b"\r\n"))
            out = tmp / "out"
            for fam in c.FAMILIES:
                code, _o, err = c.run(out, fam, src)
                self.assertEqual(code, 0, err)
            self.assertEqual(c.tree_bytes(out), c.tree_bytes(c.EXPECTED))

    def test_stale_files_removed(self):
        with c.TempDir() as tmp:
            self.assertEqual(c.run(tmp, "RK3566")[0], 0)
            stale = tmp / "gpl" / "generated" / "rk3566" / "linux" / "patches" / "99-old" / "x.patch"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old\n")
            other = tmp / "board" / "rk3566" / "keep.txt"
            other.parent.mkdir(parents=True)
            other.write_bytes(b"not the importer's\n")
            code, out, _e = c.run(tmp, "RK3566")
            self.assertEqual(code, 0)
            self.assertFalse(stale.exists())
            self.assertFalse(stale.parent.exists())
            self.assertTrue(other.exists())


if __name__ == "__main__":
    unittest.main()
