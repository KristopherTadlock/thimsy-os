"""E.7: no quirk script text appears in any output (a grep for shell syntax)."""

from __future__ import annotations

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402

SHELL = re.compile(r"cat\s+<<|<<\s*EOF|^EOF$|#!/bin/sh|\becho\s|\bexport\s|\$\(|\$\{|`|"
                   r"/storage/\.config/profile\.d|\bthen$|^\s*fi$|;;|\bdone$", re.M)
KV = re.compile(r"^[A-Z_][A-Z0-9_]*=")


class NoShellTextTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = c.TempDir()
        cls.out = cls.tmp.__enter__()
        for fam in c.FAMILIES:
            code, _o, err = c.run(cls.out, fam)
            assert code == 0, err
        cls.files = c.tree_bytes(cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.__exit__(None, None, None)

    def test_thimsy_data_has_no_shell_syntax(self):
        """generated/ holds Thimsy's own rows: no shell syntax anywhere in it."""
        n = 0
        for rel, data in self.files.items():
            if not rel.startswith("generated/") or rel.endswith("defconfig.fragment"):
                continue
            n += 1
            m = SHELL.search(data.decode("utf-8"))
            self.assertIsNone(m, "%s: shell text %r" % (rel, m.group(0) if m else ""))
        self.assertGreater(n, 10)

    def test_defconfig_uses_only_buildroot_variables(self):
        for rel, data in self.files.items():
            if rel.endswith("defconfig.fragment"):
                text = data.decode("utf-8").replace("$(BR2_EXTERNAL_THIMSY_PATH)", "")
                self.assertIsNone(SHELL.search(text), rel)

    def test_no_quirk_line_in_any_output(self):
        """Every script line of every quirk file (not a KEY=value fact) is absent from every output."""
        qroot = c.FIXTURE / "projects/ROCKNIX/packages/hardware/quirks"
        lines = set()
        for p in qroot.rglob("*"):
            if not p.is_file() or p.name == "package.mk":
                continue
            for line in p.read_text(encoding="utf-8").splitlines():
                s = line.strip()
                if len(s) < 8 or s.startswith("#") or KV.match(s):
                    continue
                lines.add(s)
        self.assertIn("echo 1 > /sys/class/pwm/${DEVICE_PWM_MOTOR}/pwm0/enable", lines)
        blob = b"\n".join(self.files.values()).decode("utf-8")
        for s in sorted(lines):
            self.assertNotIn(s, blob)

    def test_shell_array_values_rejected(self):
        dev = self.files["generated/rk3566/devices/anbernic-rg-arc-d.json"].decode("utf-8")
        self.assertNotIn("CPU_FREQ", dev)
        self.assertIn('"DEVICE_PWM_MOTOR": "pwmchip1"', dev)


if __name__ == "__main__":
    unittest.main()
