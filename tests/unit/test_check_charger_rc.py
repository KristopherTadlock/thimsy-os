"""check_charger_rc.py: the off-mode charger starts only after the RG Cube's battery is registered."""
import os

from tests import util

ccr = util.load_script("check_charger_rc")

WAIT = """
on charger && property:ro.product.vendor.model=RGCUBE
    wait /sys/class/power_supply/battery 10
"""
START = """
on charger
    class_start charger
"""
OTHER = """
on early-init
    start ueventd

service charger /system/bin/charger
    class charger
"""


class Problem(util.TestCase):
    def test_wait_before_start(self):
        self.assertIsNone(ccr.problem(OTHER + WAIT + START))

    def test_no_wait(self):
        """The init.rc shipped up to test 26: the charger starts with nothing waited for."""
        self.assertIn("`class_start charger` runs before any `wait", ccr.problem(OTHER + START))

    def test_wait_after_start(self):
        self.assertIn("runs before any `wait", ccr.problem(START + WAIT))

    def test_start_charger_counts(self):
        self.assertIn("`start charger` runs before", ccr.problem("on charger\n    start charger\n" + WAIT))

    def test_wait_in_a_normal_boot_action_does_not_count(self):
        rc = WAIT.replace("RGCUBE", "RGCUBE && property:ro.bootmode=normal") + START
        self.assertIn("runs before any `wait", ccr.problem(rc))

    def test_wait_on_another_trigger_does_not_count(self):
        self.assertIn("runs before any `wait", ccr.problem(WAIT.replace("on charger", "on init") + START))

    def test_unconditional_wait(self):
        self.assertIsNone(ccr.problem("on charger\n    wait /sys/class/power_supply/battery 10\n" + START))

    def test_wait_in_the_same_action(self):
        self.assertIsNone(ccr.problem(
            "on charger\n    wait /sys/class/power_supply/battery 10\n    class_start charger\n"))

    def test_timeout(self):
        # nan, inf and 1_0 are Python floats; init's ParseDouble takes nan and inf and then waits an
        # undefined time, and rejects 1_0.
        for bad in (" 1", " 4.9", " soon", " 31", " nan", " inf", " 1_0", " 1e1", " -10"):
            rc = WAIT.replace("battery 10", "battery" + bad) + START
            self.assertIn("needs a timeout of 5 to 30 s", ccr.problem(rc), bad)
        for good in (" 5", " 7.5", " 30", ""):
            self.assertIsNone(ccr.problem(WAIT.replace("battery 10", "battery" + good) + START), good)

    def test_wait_init_rejects(self):
        """A wait with an argument too many: init drops the command when it parses the file."""
        rc = WAIT.replace("battery 10", "battery 10 s") + START
        self.assertIn("init rejects `wait /sys/class/power_supply/battery 10 s`", ccr.problem(rc))

    def test_header_init_rejects(self):
        """Headers init's ParseTriggers refuses: the whole action, the wait with it, is dropped."""
        for header in ("on charger property:ro.product.vendor.model=RGCUBE",
                       "on charger && && property:ro.product.vendor.model=RGCUBE",
                       "on charger && property:ro.product.vendor.model=RGCUBE"
                       " && property:ro.product.vendor.model=RGCUBE",
                       "on charger && property:ro.product.vendor.model",
                       "on charger && charger"):
            rc = WAIT.replace("on charger && property:ro.product.vendor.model=RGCUBE", header) + START
            self.assertIn("init rejects `%s`" % header, ccr.problem(rc), header)

    def test_wait_for_another_model_does_not_count(self):
        for cond in ("property:ro.product.vendor.model=RGCube", "property:ro.product.model=RGCUBE"):
            rc = WAIT.replace("property:ro.product.vendor.model=RGCUBE", cond) + START
            self.assertIn("runs before any `wait", ccr.problem(rc), cond)

    def test_wait_for_another_node_does_not_count(self):
        rc = WAIT.replace("power_supply/battery", "power_supply/usb") + START
        self.assertIn("runs before any `wait", ccr.problem(rc))

    def test_comments_and_continuations(self):
        rc = ("# on charger\n#    class_start charger\n"
              "on charger \\\n    && property:ro.product.vendor.model=RGCUBE\n"
              "    wait /sys/class/power_supply/battery 10 # the fuel gauge\n" + START)
        self.assertIsNone(ccr.problem(rc))

    def test_crlf_and_trailing_whitespace(self):
        rc = (OTHER + WAIT + START).replace("\n", " \t\r\n")
        self.assertIsNone(ccr.problem(rc))

    def test_no_charger_start(self):
        self.assertIn("no `on charger` action starts the charger", ccr.problem(OTHER + WAIT))

    def test_aosp_tree(self):
        """The init.rc the system image is built from (AOSP=<tree>, default ~/gammaos-src)."""
        path = os.path.join(util.AOSP, "system/core/rootdir/init.rc")
        if not os.path.isfile(path):
            self.skipTest("no AOSP tree at %s" % util.AOSP)
        self.assertIsNone(ccr.problem(util.slurp(path, text=True)))


class Image(util.TestCase):
    def setUp(self):
        super().setUp()
        self.bin = util.need(self, "mke2fs", "debugfs")
        self.env = {"PATH": os.pathsep.join(self.bin + [os.environ["PATH"]])}

    def image(self, init_rc, shared_blocks=False):
        root = self.path("root")
        os.makedirs(os.path.join(root, "system/bin"), exist_ok=True)
        if init_rc is not None:
            self.write("root/system/etc/init/hw/init.rc", init_rc)
        img = self.path("system.img")
        self.assertOk(util.run(["mke2fs", "-q", "-F", "-t", "ext4", "-O", "^metadata_csum", "-d", root, img, "4M"],
                               env=self.env))
        if shared_blocks:
            # The source build's system images (BOARD_EXT4_SHARE_DUP_BLOCKS) are read-only ext4 with
            # shared blocks, which mke2fs cannot make: set the flag in the superblock.
            with open(img, "r+b") as f:
                f.seek(1024 + 0x64)
                ro_compat = int.from_bytes(f.read(4), "little") | 0x4000
                f.seek(1024 + 0x64)
                f.write(ro_compat.to_bytes(4, "little"))
        return img

    def check(self, *args):
        return util.run_script("check_charger_rc", *args, env=self.env)

    def test_good(self):
        proc = self.assertOk(self.check(self.image(OTHER + WAIT + START)))
        self.assertIn("waits for the battery before starting the charger", proc.stdout)

    def test_shared_blocks(self):
        self.assertOk(self.check(self.image(OTHER + WAIT + START, shared_blocks=True)))

    def test_bad(self):
        self.assertFails(self.check(self.image(OTHER + START)), "runs before any `wait")

    def test_no_init_rc(self):
        self.assertFails(self.check(self.image(None)), "has no /system/etc/init/hw/init.rc")

    def test_not_ext4(self):
        """A sparse or EROFS image: debugfs exits 0 here too, and the reason is passed on."""
        img = self.write("system.img", b"\0" * 65536)
        self.assertFails(self.check(img), "debugfs could not read /system/etc/init/hw/init.rc", "Bad magic number")

    def test_usage(self):
        self.assertFails(self.check(), "check_charger_rc.py SYSTEM_IMG")


class Build(util.TestCase):
    def test_build_checks_the_system_image_first(self):
        """build.sh runs the check on SYSTEM_IMG as given, before sudo and the long steps."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        call = 'python3 -I "$ROOT/scripts/check_charger_rc.py" "$SYSTEM_IMG"\n'
        self.assertEqual(text.count(call), 1)
        at = text.index(call)
        self.assertLess(at, text.index("\nsudo -v\n"))
        self.assertLess(at, text.index('cp "$SYSTEM_IMG" "$system"'))
        self.assertGreater(at, text.index('SYSTEM_IMG="$(realpath "$SYSTEM_IMG")"'))
        self.assertIn("\nset -euo pipefail\n", text)
