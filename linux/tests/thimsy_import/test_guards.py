"""E.7: a changed case block stops the import (hash guard); --accept-guards records it."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402

MK = "projects/ROCKNIX/packages/linux/package.mk"


def edit(src, rel, old, new):
    p = src / rel
    text = p.read_text(encoding="utf-8")
    assert old in text, old
    p.write_bytes(text.replace(old, new, 1).encode("utf-8"))


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = c.TempDir()
        root = self.tmp.__enter__()
        self.src = c.copy_fixture(root)
        self.out = root / "out"
        for fam in c.FAMILIES:
            code, _o, err = c.run(self.out, fam, self.src)
            self.assertEqual(code, 0, err)
        self.before = c.snapshot(self.out)

    def tearDown(self):
        self.tmp.__exit__(None, None, None)

    def test_changed_case_block_stops_the_import(self):
        edit(self.src, MK, '    PKG_PATCH_DIRS+=" 7.3"\n', '    PKG_PATCH_DIRS+=" 7.3 rk-extra"\n')
        code, _o, err = c.run(self.out, "RK3566", self.src)
        self.assertEqual(code, 3)
        self.assertIn("changed: %s:case:RK3566" % MK, err)
        self.assertEqual(c.snapshot(self.out), self.before, "a stopped import must write nothing")
        code, _o, err = c.run(self.out, "RK3566", self.src, "--accept-guards")
        self.assertEqual(code, 0, err)
        code, _o, err = c.run(self.out, "RK3566", self.src)
        self.assertEqual(code, 0, err)

    def test_other_family_branch_does_not_stop(self):
        edit(self.src, MK, 'PKG_GIT_CLONE_BRANCH="rk-6.1-rkr3"', 'PKG_GIT_CLONE_BRANCH="rk-6.1-rkr4"')
        code, out, err = c.run(self.out, "RK3566", self.src)
        self.assertEqual(code, 0, err)
        self.assertIn("wrote 0, removed 0", out)

    def test_firmware_block_and_unpack_order_are_guarded(self):
        edit(self.src, MK, "rtw88/rtw8821c_fw.bin ${PKG_BUILD}", "rtw88/rtw8821c_fw_v2.bin ${PKG_BUILD}")
        code, _o, err = c.run(self.out, "H700", self.src)
        self.assertEqual(code, 3)
        self.assertIn(":pre_make_target:elif#1", err)
        edit(self.src, "scripts/unpack", "      ${PATCH_DIRS_PKG} \\\n", "      ${PATCH_DIRS_PRJ} \\\n")
        code, _o, err = c.run(self.out, "RK3566", self.src)
        self.assertEqual(code, 3)
        self.assertIn("scripts/unpack:patch-order", err)

    def test_comment_only_change_does_not_stop(self):
        edit(self.src, MK, "    # Note that the patch dir 7.2", "    # NB: the patch dir 7.2")
        code, _o, err = c.run(self.out, "H700", self.src)
        self.assertEqual(code, 0, err)

    def test_unknown_patch_dir_shape_stops(self):
        edit(self.src, MK, '[[ "${DEVICE}" == RK* ]] && PKG_PATCH_DIRS+=" mainline-rockchip"',
             '[[ "${DEVICE}" == RK* || "${DEVICE}" == H7* ]] && PKG_PATCH_DIRS+=" mainline-rockchip"')
        code, _o, err = c.run(self.out, "RK3566", self.src, "--accept-guards")
        self.assertEqual(code, 2)
        self.assertIn("not supported", err)


if __name__ == "__main__":
    unittest.main()
