"""E.7: proprietary firmware is left out and reported (decision 24)."""

from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402
from thimsy_import import firmware  # noqa: E402


class FirmwareTest(unittest.TestCase):
    def test_proprietary_excluded_and_reported(self):
        with c.TempDir() as tmp:
            code, out, err = c.run(tmp, "H700")
            self.assertEqual(code, 0, err)
            doc = json.loads((tmp / "generated/h700/firmware.json").read_text(encoding="utf-8"))
            by = {(e["path"], e["builtin"]): e for e in doc["entries"]}
            panel = by[("panels/anbernic,rg40xx-panel.panel", True)]
            self.assertEqual(panel["decision"], "exclude")
            self.assertTrue(panel["fetch_candidate"])
            self.assertIsNone(panel["licence"])
            self.assertIn("firmware: excluded kernel-firmware panels/anbernic,rg40xx-panel.panel [built in]", out)
            rtw = by[("rtw88/rtw8821c_fw.bin", True)]
            self.assertEqual(rtw["decision"], "ship")
            self.assertIn("LICENCE.rtlwifi_firmware.txt", rtw["licence"])
            frag = (tmp / "generated/h700/linux-firmware.fragment").read_text(encoding="utf-8")
            self.assertIn('CONFIG_EXTRA_FIRMWARE="rtl_bt/rtl8821cs_config.bin rtl_bt/rtl8821cs_fw.bin '
                          'rtw88/rtw8821c_fw.bin regulatory.db regulatory.db.p7s"', frag)
            self.assertNotIn(".panel", frag)

    def test_extra_firmware_package_excluded(self):
        with c.TempDir() as tmp:
            code, out, err = c.run(tmp, "RK3566")
            self.assertEqual(code, 0, err)
            doc = json.loads((tmp / "generated/rk3566/firmware.json").read_text(encoding="utf-8"))
            ef = [e for e in doc["entries"] if e["package"] == "extra-firmware"]
            self.assertEqual(len(ef), 1)
            self.assertEqual(ef[0]["decision"], "exclude")
            self.assertIn("firmware: excluded extra-firmware", out)

    def test_without_whence_nothing_from_linux_firmware_ships(self):
        whence = None
        entries = firmware.collect("# RTW_88\nrtw88/rtw8821c_fw.bin\n", "x.dat", [], None, [], whence)
        self.assertEqual([e.decision for e in entries], ["exclude"])

    def test_whence_parser(self):
        text = (c.FIXTURE / "linux-firmware" / "WHENCE").read_text(encoding="utf-8")
        w = firmware.parse_whence(text)
        self.assertIn("rtl_bt/rtl8821cs_config.bin", w)
        self.assertEqual(w["rtl_bt/unknown_example.bin"], "", "listed, but with no usable licence")
        g = firmware.collect("rtl_bt/*\n", "x.dat", [], None, [], w)
        self.assertEqual(g[0].decision, "exclude", "a glob reaching an unlicensed file is not shipped")

    def test_unknown_builtin_line_stops(self):
        lines = ['elif [ "${DEVICE}" = "X" ]; then',
                 "  cp $(get_build_dir kernel-firmware)/.copied-firmware/qcom/a/* ${PKG_BUILD}/external-firmware/"]
        with self.assertRaises(Exception):
            firmware.parse_builtin_block(lines, "blk")


if __name__ == "__main__":
    unittest.main()
