"""check_charger_binary.py: the off-mode charger looks for the battery until it finds one."""
import os

from tests import util

ccb = util.load_script("check_charger_binary")

GOOD = b"\x7fELF\x02\x01\x01" + b"\0" * 64 + b"<4>healthd: battery appeared after start\n\0" + b"\0" * 64
BAD = GOOD.replace(b"battery appeared after start", b"BatteryStatusPath not found\0\0\0\0")


class Problem(util.TestCase):
    def test_good(self):
        self.assertIsNone(ccb.problem(GOOD))

    def test_charger_without_the_rescan(self):
        """The charger shipped up to test 27."""
        self.assertIn("lacks \"battery appeared after start\"", ccb.problem(BAD))

    def test_not_elf(self):
        self.assertIn("is not an ELF file", ccb.problem(b"#!/system/bin/sh\nbattery appeared after start\n"))


class Image(util.TestCase):
    def setUp(self):
        super().setUp()
        self.bin = util.need(self, "mke2fs", "debugfs")
        self.env = {"PATH": os.pathsep.join(self.bin + [os.environ["PATH"]])}

    def image(self, charger, shared_blocks=False):
        root = self.path("root")
        os.makedirs(os.path.join(root, "system/etc"), exist_ok=True)
        if charger is not None:
            self.write("root/system/bin/charger", charger, mode=0o755)
        img = self.path("system.img")
        self.assertOk(util.run(["mke2fs", "-q", "-F", "-t", "ext4", "-O", "^metadata_csum", "-d", root, img, "4M"],
                               env=self.env))
        if shared_blocks:
            # As in test_check_charger_rc: the source build's read-only ext4 with shared blocks.
            with open(img, "r+b") as f:
                f.seek(1024 + 0x64)
                ro_compat = int.from_bytes(f.read(4), "little") | 0x4000
                f.seek(1024 + 0x64)
                f.write(ro_compat.to_bytes(4, "little"))
        return img

    def check(self, *args):
        return util.run_script("check_charger_binary", *args, env=self.env)

    def test_good(self):
        proc = self.assertOk(self.check(self.image(GOOD)))
        self.assertIn("looks for the battery until it finds one", proc.stdout)

    def test_shared_blocks(self):
        self.assertOk(self.check(self.image(GOOD, shared_blocks=True)))

    def test_bad(self):
        self.assertFails(self.check(self.image(BAD)), "lacks \"battery appeared after start\"")

    def test_no_charger(self):
        self.assertFails(self.check(self.image(None)), "has no /system/bin/charger")

    def test_not_ext4(self):
        img = self.write("system.img", b"\0" * 65536)
        self.assertFails(self.check(img), "debugfs could not read /system/bin/charger", "Bad magic number")

    def test_usage(self):
        self.assertFails(self.check(), "check_charger_binary.py SYSTEM_IMG")


class Build(util.TestCase):
    def test_build_checks_the_system_image_first(self):
        """build.sh runs the check on SYSTEM_IMG as given, before sudo and the long steps."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        call = 'python3 -I "$ROOT/scripts/check_charger_binary.py" "$SYSTEM_IMG"\n'
        self.assertEqual(text.count(call), 1)
        at = text.index(call)
        self.assertLess(at, text.index("\nsudo -v\n"))
        self.assertLess(at, text.index('cp "$SYSTEM_IMG" "$system"'))
        self.assertGreater(at, text.index('SYSTEM_IMG="$(realpath "$SYSTEM_IMG")"'))


class AospTree(util.TestCase):
    def test_battery_monitor_has_the_marker(self):
        """The BatteryMonitor the charger is built from logs the string this script looks for."""
        path = os.path.join(util.AOSP, "system/core/healthd/BatteryMonitor.cpp")
        if not os.path.isfile(path):
            self.skipTest("no AOSP tree at %s" % util.AOSP)
        self.assertIn(ccb.MARKER.decode(), util.slurp(path, text=True))
