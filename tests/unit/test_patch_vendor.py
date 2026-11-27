"""scripts/patch_vendor.py (the composer in vendor_a, build.sh COMPOSER_FIX) and scripts/ext4_image.py
(a file changed in place in an ext4 image), on small ext4 images made with mke2fs -d."""
import hashlib
import os
from unittest import mock

from tests import util

pv = util.load_script("patch_vendor")
ext4_image = pv.ext4_image       # the modules patch_vendor.py imports
sprd_hwc = pv.sprd_hwc

LIB = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "bypass-research", "vendor", "hwcomposer.unisoc.so")
T3_LIB = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "t3", "hwcomposer-ct4k.so")
LABEL = "u:object_r:vendor_file:s0"
BS = 4096


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Composer(util.TestCase):
    """The pins: the stock composer in, exactly a composer that ran on the RG Cube out."""

    def lib(self):
        if not os.path.isfile(LIB):
            self.skipTest("no %s" % LIB)
        return util.slurp(LIB)

    def test_pins(self):
        lib = self.lib()
        self.assertEqual(sha(lib), sprd_hwc.COMPOSER_SHA256)
        both = pv.patch_composer(lib)
        self.assertEqual(sha(both), pv.SLEEP_CT4K_SHA256)
        self.assertEqual(both, sprd_hwc.patch_library(lib, sleep=True, ct4k=True))
        ct4k = pv.patch_composer(lib, sleep=False)
        self.assertEqual(sha(ct4k), pv.CT4K_SHA256)
        self.assertEqual(ct4k, sprd_hwc.patch_library(lib, sleep=False, ct4k=True))
        # Two 4 KiB blocks change for SLEEP + CT4K (0x40000, 0x5f000), one for CT4K alone.
        self.assertEqual([a for a in range(0, len(lib), BS) if lib[a:a + BS] != both[a:a + BS]], [0x40000, 0x5f000])
        self.assertEqual([a for a in range(0, len(lib), BS) if lib[a:a + BS] != ct4k[a:a + BS]], [0x5f000])

    def test_t3_composer(self):
        """CT4K_SHA256 is the composer T3 bind-mounted (t3_prepare.sh's want_hwc)."""
        if not os.path.isfile(T3_LIB):
            self.skipTest("no %s" % T3_LIB)
        self.assertEqual(sha(util.slurp(T3_LIB)), pv.CT4K_SHA256)
        prepare = util.slurp(os.path.join(os.path.dirname(T3_LIB), "t3_prepare.sh"), text=True)
        self.assertIn("want_hwc=%s\n" % pv.CT4K_SHA256, prepare)

    def test_refuses(self):
        with self.assertRaises(SystemExit):
            pv.patch_composer(b"\0" * 4096)
        lib = self.lib()
        with self.assertRaises(SystemExit):
            pv.patch_composer(lib[:-1] + b"\1")
        # A patch table that no longer gives the tested composer stops the build.
        orig = sprd_hwc.patch_library
        with mock.patch.object(sprd_hwc, "patch_library", lambda lib, sleep, ct4k: orig(lib, sleep, False)),                 self.assertRaises(SystemExit) as e:
            pv.patch_composer(lib)
        self.assertIn("no longer gives the composer tested", str(e.exception))


class Ext4Case(util.TestCase):
    """A small vendor-like ext4 image: the composer, its 32-bit twin and other files."""

    def setUp(self):
        super().setUp()
        self.bin = util.need(self, "mke2fs", "debugfs", "dumpe2fs", "e2fsck")
        self.env = {"PATH": os.pathsep.join(self.bin + [os.environ["PATH"]])}
        env = mock.patch.dict(ext4_image.ENV, {"PATH": self.env["PATH"]})   # the tools ext4_image runs
        env.start()
        self.addCleanup(env.stop)

    def make(self, composer, extra=None, features=None):
        files = {"lib64/hw/hwcomposer.unisoc.so": composer, "lib/hw/hwcomposer.unisoc.so": os.urandom(40000),
                 "lib64/hw/gralloc.default.so": os.urandom(20072), "etc/init/composer.rc": b"service x\n"}
        files.update(extra or {})
        for rel, data in files.items():
            self.write("root/" + rel, data)
        image = self.path("vendor.img")
        cmd = ["mke2fs", "-q", "-F", "-t", "ext4", "-b", str(BS), "-I", "256", "-L", "vendor"]
        if features:
            cmd += ["-O", features]
        self.tool(cmd + ["-d", self.path("root"), image, "16M"])
        self.debugfs_w(image, "ea_set /lib64/hw/hwcomposer.unisoc.so security.selinux %s" % LABEL)
        self.tool(["e2fsck", "-fn", image])
        return image, files

    def tool(self, cmd):
        return self.assertOk(util.run(cmd, env=self.env))

    def debugfs_w(self, image, request):
        self.tool(["debugfs", "-w", "-R", request, image])

    def cat(self, image, path):
        return ext4_image.contents(image, path, "test")

    def stat_lines(self, image, path):
        """debugfs stat: the inode, its times, extents and extended attributes."""
        return ext4_image.inode(image, path, "test")[3].splitlines()


class Ext4Patch(Ext4Case):
    def flip(self, offsets):
        def change(data):
            data = bytearray(data)
            for o in offsets:
                data[o] ^= 0xff
            return bytes(data)
        return change

    def test_in_place(self):
        """Only the changed blocks of the file are written; its inode, label and every other file and
        byte stay as they were."""
        image, files = self.make(os.urandom(10 * BS + 123))
        path = "/lib64/hw/hwcomposer.unisoc.so"
        before = self.stat_lines(image, path)
        out = self.path("vendor.patched")
        written = ext4_image.patch_file(image, out, path, self.flip([5, BS * 3 + 7, 10 * BS + 100]), "vendor")
        self.assertEqual([lblk for lblk, _ in written], [0, 3, 10])
        new = self.cat(out, path)
        self.assertEqual(new, self.flip([5, BS * 3 + 7, 10 * BS + 100])(files["lib64/hw/hwcomposer.unisoc.so"]))
        self.assertEqual(self.stat_lines(out, path), before)
        self.assertIn('security.selinux (25) = "%s"' % LABEL, "\n".join(before))
        for rel, data in files.items():
            if rel != "lib64/hw/hwcomposer.unisoc.so":
                self.assertEqual(self.cat(out, "/" + rel), data, rel)
        self.assertEqual(os.path.getsize(out), os.path.getsize(image))
        self.assertEqual(ext4_image.differing_blocks(image, out, BS), sorted(p for _, p in written))
        self.tool(["e2fsck", "-fn", out])
        # The input image is not written.
        self.assertEqual(self.cat(image, path), files["lib64/hw/hwcomposer.unisoc.so"])

    def test_unchanged(self):
        image, _ = self.make(os.urandom(3 * BS))
        out = self.path("same.img")
        self.assertEqual(ext4_image.patch_file(image, out, "/lib64/hw/hwcomposer.unisoc.so", lambda d: d, "vendor"), [])
        self.assertEqual(util.slurp(out), util.slurp(image))

    def refused(self, image, needle, path="/lib64/hw/hwcomposer.unisoc.so", change=None):
        out = self.path("refused.img")
        with self.assertRaises(SystemExit) as e:
            ext4_image.patch_file(image, out, path, change or self.flip([1]), "vendor")
        self.assertIn(needle, str(e.exception))
        self.assertFalse(os.path.exists(out))

    def test_size_change(self):
        image, _ = self.make(os.urandom(3 * BS))
        self.refused(image, "only a change in place", change=lambda d: d + b"x")

    def test_shared_blocks(self):
        image, _ = self.make(os.urandom(3 * BS))
        self.debugfs_w(image, "feature shared_blocks")
        self.refused(image, "shares blocks between files")

    def test_not_a_file(self):
        image, _ = self.make(os.urandom(3 * BS))
        self.refused(image, "no /lib64/hw/missing.so", path="/lib64/hw/missing.so")
        self.refused(image, "not a regular file (directory)", path="/lib64/hw")

    def test_inode_flags(self):
        """fs-verity (0x100000) and other flags are refused."""
        image, _ = self.make(os.urandom(3 * BS))
        self.debugfs_w(image, "set_inode_field /lib64/hw/hwcomposer.unisoc.so flags 0x180000")
        self.refused(image, "inode flags 0x180000")

    def test_hole(self):
        """mke2fs -d leaves an all-zero block as a hole; a change there is refused."""
        lib = os.urandom(BS) + bytes(BS) + os.urandom(BS)
        image, _ = self.make(lib)
        if ext4_image.bmap(image, "/lib64/hw/hwcomposer.unisoc.so", 1, "test"):
            self.skipTest("this mke2fs wrote the zero block")
        self.refused(image, "block 1 of /lib64/hw/hwcomposer.unisoc.so is a hole", change=self.flip([BS + 1]))

    def test_dirty_image(self):
        image, _ = self.make(os.urandom(3 * BS))
        # A block of the composer marked free: e2fsck finds the block bitmap wrong.
        self.debugfs_w(image, "freeb %d" % ext4_image.bmap(image, "/lib64/hw/hwcomposer.unisoc.so", 0, "test"))
        self.refused(image, "e2fsck -fn fails on the original image")

    def test_bad_path(self):
        image, _ = self.make(os.urandom(3 * BS))
        self.refused(image, "bad path", path='/lib64/hw/a"b')
        self.refused(image, "bad path", path="lib64/hw/hwcomposer.unisoc.so")


class PatchVendor(Ext4Case):
    """The script build.sh runs, on an image holding the real stock composer."""

    def lib(self):
        if not os.path.isfile(LIB):
            self.skipTest("no %s" % LIB)
        return util.slurp(LIB)

    def run_it(self, *args):
        return util.run_script("patch_vendor", *args, env=self.env)

    def test_default_and_ct4k(self):
        image, files = self.make(self.lib())
        for args, want, blocks in (((), pv.SLEEP_CT4K_SHA256, 2), (("--ct4k-only",), pv.CT4K_SHA256, 1)):
            out = self.path("vendor%d.patched" % blocks)
            proc = self.assertOk(self.run_it(*args, image, out))
            self.assertIn("%d blocks written" % blocks, proc.stdout)
            self.assertIn(want, proc.stdout)
            self.assertEqual(sha(self.cat(out, pv.COMPOSER)), want)
            self.assertEqual(len(ext4_image.differing_blocks(image, out, BS)), blocks)
            self.assertEqual(self.cat(out, "/lib/hw/hwcomposer.unisoc.so"), files["lib/hw/hwcomposer.unisoc.so"])
            self.tool(["e2fsck", "-fn", out])

    def test_patched_twice(self):
        """An image whose composer is already patched (or not the stock one) is refused."""
        image, _ = self.make(self.lib())
        out = self.path("once.img")
        self.assertOk(self.run_it(image, out))
        self.assertFails(self.run_it(out, self.path("twice.img")), "not the stock RG Cube composer")
        self.assertFalse(os.path.exists(self.path("twice.img")))

    def test_usage(self):
        self.assertFails(self.run_it("only-one.img"), "patch_vendor.py [--ct4k-only] IN OUT")
        self.assertFails(self.run_it("--sleep", "a.img", "b.img"), "patch_vendor.py [--ct4k-only] IN OUT")


class BuildSwitch(util.TestCase):
    def test_build_sh(self):
        """COMPOSER_FIX defaults to 1 (SLEEP + CT4K) with SYSTEM_IMG and to 0 without it (the 1.2 base,
        where the composer fixes never ran), is checked before the long steps, takes 0, 1 or
        ct4k, and patches vendor_a before odm_sepolicy.py reads it."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        self.assertIn('FIX_DEFAULT=0\n[[ -z "${SYSTEM_IMG:-}" ]] || FIX_DEFAULT=1\n', text)
        self.assertIn('COMPOSER_FIX="${COMPOSER_FIX:-$FIX_DEFAULT}"\n', text)
        self.assertLess(text.index("FIX_DEFAULT=0"), text.index('COMPOSER_FIX="${COMPOSER_FIX:-$FIX_DEFAULT}"'))
        self.assertIn('  0|1|ct4k) ;;\n  *) echo "COMPOSER_FIX must be 0, 1 or ct4k" >&2; exit 1 ;;\n', text)
        self.assertLess(text.index('COMPOSER_FIX="${COMPOSER_FIX:-$FIX_DEFAULT}"'), text.index("\nsudo -v\n"))
        step = ('if [[ "$COMPOSER_FIX" != 0 ]]; then\n'
                '  composer=()\n'
                '  [[ "$COMPOSER_FIX" == 1 ]] || composer=(--ct4k-only)\n'
                '  python3 -I "$ROOT/scripts/patch_vendor.py" "${composer[@]}" "$vendor" "$vendor.patched"\n'
                '  python3 -I "$ROOT/scripts/lptool.py" replace "$super" vendor_a "$vendor.patched"\n'
                '  mv "$vendor.patched" "$vendor"\n'
                'fi\n')
        self.assertIn(step, text)
        self.assertLess(text.index('lptool.py" extract "$super" vendor_a "$vendor"'), text.index(step))
        self.assertLess(text.index(step), text.index('odm_sepolicy.py" "$odm" "$system" "$vendor"'))
        self.assertLess(text.index(step), text.index('pactool.py" replace "$pac.partial" super_full.img'))
        self.assertEqual(text.count("patch_vendor.py"), 2)    # the header and the step
        self.assertEqual(text.count("--ct4k-only"), 1)

    def test_build_144_passes_it_through(self):
        text = util.slurp(os.path.join(util.SCRIPTS, "build_144.sh"), text=True)
        self.assertRegex(text, r"\n  for v in [A-Z_ ]*\bCOMPOSER_FIX\b[A-Z_ ]*; do\n")

    def test_readme(self):
        text = util.slurp(os.path.join(util.ROOT, "README.md"), text=True)
        for needle in ("`COMPOSER_FIX=1`", "`COMPOSER_FIX=ct4k`", "`COMPOSER_FIX=0`", "`scripts/patch_vendor.py`",
                       "`scripts/ext4_image.py`"):
            self.assertIn(needle, text)
