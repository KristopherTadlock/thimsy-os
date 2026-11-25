"""E.7: a fragment that flips a Thimsy-critical option fails the conformance check;
--check reports differences without writing."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402


class ConformanceTest(unittest.TestCase):
    def test_good_fragment_passes(self):
        with c.TempDir() as tmp:
            for fam in c.FAMILIES:
                code, out, err = c.run(tmp, fam, None, "--fragment", str(c.DATA / "good.fragment"))
                self.assertEqual(code, 0, err)
                self.assertNotIn("conformance:", out)

    def test_flipped_option_fails_and_writes_nothing(self):
        with c.TempDir() as tmp:
            code, out, err = c.run(tmp, "RK3566", None, "--fragment", str(c.DATA / "flips-erofs.fragment"))
            self.assertEqual(code, 4)
            self.assertIn("conformance: CONFIG_EROFS_FS want y, base n, fragment n (flipped)", out)
            self.assertIn("fails the conformance check", err)
            self.assertEqual(c.tree_bytes(tmp), {})

    def test_uncovered_base_value_fails(self):
        with c.TempDir() as tmp:
            code, out, _e = c.run(tmp, "H700", None, "--fragment", str(c.DATA / "misses-erofs.fragment"))
            self.assertEqual(code, 4)
            self.assertIn("CONFIG_EROFS_FS want y, base n (not-covered)", out)

    def test_family_section_applies(self):
        """RK3566 needs Panfrost built in; ROCKNIX has it as a module."""
        with c.TempDir() as tmp:
            frag = tmp / "f.fragment"
            frag.write_text('CONFIG_INITRAMFS_SOURCE=""\nCONFIG_EROFS_FS=y\n', encoding="utf-8")
            code, out, _e = c.run(tmp / "o", "RK3566", None, "--fragment", str(frag))
            self.assertEqual(code, 4)
            self.assertIn("CONFIG_DRM_PANFROST want y, base m (not-covered)", out)
            code, _o, err = c.run(tmp / "o", "H700", None, "--fragment", str(frag))
            self.assertEqual(code, 0, err)


class CheckModeTest(unittest.TestCase):
    def test_check_writes_nothing(self):
        with c.TempDir() as tmp:
            code, out, _e = c.run(tmp, "RK3566", None, "--check")
            self.assertEqual(code, 1)
            self.assertIn("A generated/rk3566/catalog.json", out)
            self.assertEqual(c.tree_bytes(tmp), {})

    def test_check_after_import_is_clean_then_reports_edits(self):
        with c.TempDir() as tmp:
            self.assertEqual(c.run(tmp, "RK3566")[0], 0)
            code, out, _e = c.run(tmp, "RK3566", None, "--check")
            self.assertEqual(code, 0)
            self.assertIn("check: no differences", out)
            f = tmp / "generated/rk3566/catalog.json"
            f.write_bytes(f.read_bytes() + b" ")
            extra = tmp / "generated/rk3566/devices/old.json"
            extra.write_text("{}\n", encoding="utf-8")
            snap = c.snapshot(tmp)
            code, out, _e = c.run(tmp, "RK3566", None, "--check")
            self.assertEqual(code, 1)
            self.assertIn("M generated/rk3566/catalog.json", out)
            self.assertIn("D generated/rk3566/devices/old.json", out)
            self.assertEqual(c.snapshot(tmp), snap)


if __name__ == "__main__":
    unittest.main()
