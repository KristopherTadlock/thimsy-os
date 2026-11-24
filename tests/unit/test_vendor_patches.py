"""The pure parts of sprd_drm.py, patch_vendor_dlkm.py and patch_vendor_boot.py."""
import contextlib
import hashlib
import io
import os
import struct
import sys
from unittest import mock

from tests import util
from tests.fixtures import cpio

sprd_drm = util.load_script("sprd_drm")
dlkm = util.load_script("patch_vendor_dlkm")
vboot = util.load_script("patch_vendor_boot")

AVB_INFO = """Footer version:           1.0
Image size:               104857600 bytes
Original image size:      2523136 bytes
VBMeta offset:            2523136
VBMeta size:              2112 bytes
--
Minimum libavb version:   1.0
Header Block:             256 bytes
Authentication Block:     576 bytes
Auxiliary Block:          1280 bytes
Public key (sha1):        2597c218aae470a130f61162feaae70afd97f011
Algorithm:                SHA256_RSA4096
Rollback Index:           7
Flags:                    2
Rollback Index Location:  1
Release String:           'avbtool 1.2.0'
Descriptors:
    Hash descriptor:
      Image Size:            2523136 bytes
      Hash Algorithm:        sha256
      Partition Name:        vendor_boot
      Salt:                  0f7a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8
      Digest:                d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0
      Flags:                 0
    Prop: com.android.build.vendor_boot.fingerprint -> 'Anbernic/RG_Cube/cube:14/UP1A/1:user/release-keys'
    Prop: com.android.build.vendor_boot.os_version -> '14'
"""


def fake_module():
    """A module image with the original words at every patch site, and its sha256."""
    sites = sprd_drm.PATCHES + sprd_drm.HPD_PATCHES
    end = max(addr + 4 * len(old) for addr, old, _ in sites)
    ko = bytearray(b"\x5a" * (sprd_drm.TEXT_OFFSET + end + 64))
    for addr, old, _ in sites:
        struct.pack_into("<%dI" % len(old), ko, sprd_drm.TEXT_OFFSET + addr, *old)
    return bytes(ko), hashlib.sha256(ko).hexdigest()


def patched(all_modes, env=None):
    """fake_module() and patch_module(it, all_modes), with THIMSY_DP_ALL_MODES only as ENV sets it."""
    ko, sha = fake_module()
    with mock.patch.object(sprd_drm, "MODULE_SHA256", sha), mock.patch.dict(os.environ):
        os.environ.pop("THIMSY_DP_ALL_MODES", None)
        os.environ.update(env or {})
        return ko, sprd_drm.patch_module(ko, all_modes)


def word(ko, addr):
    return struct.unpack_from("<I", ko, sprd_drm.TEXT_OFFSET + addr)[0]


def branch_target(addr, w):
    """The target of the B, B.cond, CBZ, CBNZ, TBZ or TBNZ word W at ADDR, else None."""
    def signed(v, bits):
        return v - (1 << bits) if v & (1 << (bits - 1)) else v
    if w & 0xfc000000 == 0x14000000:
        return addr + 4 * signed(w & 0x3ffffff, 26)
    if w & 0xff000010 == 0x54000000 or w & 0x7e000000 == 0x34000000:
        return addr + 4 * signed((w >> 5) & 0x7ffff, 19)
    if w & 0x7e000000 == 0x36000000:
        return addr + 4 * signed((w >> 5) & 0x3fff, 14)
    return None


TBZ_NOT_PREFERRED = 0x36180608   # tbz w8, #3, 0x8e54, at 0x8d94


class SprdDrm(util.TestCase):
    def test_patch_table(self):
        for all_modes in (False, True):
            table = sprd_drm.patches(all_modes)
            self.assertEqual(len(table), len(sprd_drm.PATCHES) + len(sprd_drm.HPD_PATCHES))
            sites = [(addr, addr + 4 * len(old)) for addr, old, _ in table]
            self.assertEqual(sites, sorted(sites))
            for (_, end), (start, _) in zip(sites, sites[1:]):
                self.assertLessEqual(end, start, "patch sites overlap")
            for addr, old, new in table:
                self.assertEqual(len(old), len(new), hex(addr))
                self.assertEqual(addr % 4, 0)
                self.assertNotEqual(old, new, hex(addr))
        # patches(True) leaves the table itself as it is.
        self.assertEqual(dict((a, n) for a, _, n in sprd_drm.PATCHES)[0x8d8c][2], TBZ_NOT_PREFERRED)

    def test_branch_targets(self):
        """Every branch in mode_valid's new code and in the HPD patch goes where it should."""
        want = {
            0x89b8: 0x8b1c,                     # hpd 0 or 3 while off: ignored
            0x8ce4: 0x8dd8,                     # clock too high: MODE_CLOCK_HIGH
            0x8ce8: 0x8db8,                     # the link check
            0x8d8c: 0x8e48,                     # CEA 640x480: user-defined
            0x8d94: 0x8e54,                     # stub, not preferred: return (without all_modes)
            0x8da0: 0x8e50,                     # stub: store the type
            0x8dbc: 0x8e24, 0x8dcc: 0x8e24,     # no dptx, never trained: the width check
            0x8dd4: 0x8df0, 0x8e08: 0x8e14, 0x8e30: 0x8e3c,
            0x8e20: 0x8dd8, 0x8e2c: 0x8dd8,     # link too slow, too wide: MODE_CLOCK_HIGH
            0x8e40: 0x8cec,                     # back to the CEA code checks
        }
        got = {}
        for addr, _, new in sprd_drm.patches(False):
            if addr in (0x89b4, 0x8cdc, 0x8d8c, 0x8db8, 0x8df0, 0x8e14, 0x8e3c):
                for i, w in enumerate(new):
                    target = branch_target(addr + 4 * i, w)
                    if target is not None:
                        got[addr + 4 * i] = target
        self.assertEqual(got, want)

    def test_applies_every_patch(self):
        for all_modes in (False, True):
            ko, out = patched(all_modes)
            self.assertEqual(len(out), len(ko))
            for addr, _, new in sprd_drm.patches(all_modes):
                off = sprd_drm.TEXT_OFFSET + addr
                self.assertEqual(list(struct.unpack_from("<%dI" % len(new), out, off)), new)
            changed = {i for i in range(len(ko)) if ko[i] != out[i]}
            allowed = {sprd_drm.TEXT_OFFSET + addr + i for addr, old, _ in sprd_drm.patches(all_modes)
                       for i in range(4 * len(old))}
            self.assertLessEqual(changed, allowed)

    def test_new_words(self):
        for all_modes in (False, True):
            _, out = patched(all_modes)
            # eor w8, w8, w8, lsr #1; tbz w8, #0, 0x8b1c
            self.assertEqual([word(out, 0x89b4), word(out, 0x89b8)], [0x4a480508, 0x36000b28])
            self.assertEqual(word(out, 0x8ce8), 0x14000034)          # b 0x8db8
            self.assertEqual(word(out, 0x8db8), 0xf943ca8b)          # ldr x11, [x20, #0x790]
            self.assertEqual(word(out, 0x8e18), 0x0b0a054f)          # add w15, w10, w10, lsl #1
            self.assertEqual(word(out, 0x8e28), 0x713c01ff)          # cmp w15, #0xf00
            self.assertEqual(word(out, 0x8e40), 0x17ffffab)          # b 0x8cec
            self.assertEqual(word(out, 0x8d94), sprd_drm.NOP if all_modes else TBZ_NOT_PREFERRED)
        self.assertEqual(sprd_drm.NOP, 0xd503201f)

    def test_variants_differ_only_at_the_all_modes_word(self):
        _, plain = patched(False)
        _, every = patched(True)
        off = sprd_drm.TEXT_OFFSET + 0x8d94
        differ = {i for i in range(len(plain)) if plain[i] != every[i]}
        self.assertTrue(differ)
        self.assertLessEqual(differ, set(range(off, off + 4)))

    def test_all_modes_from_the_environment(self):
        for value, nop in ((None, False), ("1", True), ("0", False), ("true", False), ("", False)):
            env = {} if value is None else {"THIMSY_DP_ALL_MODES": value}
            _, out = patched(None, env)
            self.assertEqual(word(out, 0x8d94), sprd_drm.NOP if nop else TBZ_NOT_PREFERRED, value)
            # An explicit all_modes wins over the environment.
            _, out = patched(False, env)
            self.assertEqual(word(out, 0x8d94), TBZ_NOT_PREFERRED, value)

    def test_wrong_module(self):
        with self.assertRaises(SystemExit) as e:
            sprd_drm.patch_module(b"not the module")
        self.assertIn("not the 1.2 module", str(e.exception))
        # One changed bit in an original word: the 1.2 patches', the link check's, the HPD patch's,
        # and the word all_modes replaces.
        for addr, byte in ((sprd_drm.PATCHES[2][0], 0), (0x89b4, 4), (0x8db8, 4 * 7), (0x8e3c, 4),
                           (0x8d8c, 4 * 2)):
            for all_modes in (False, True):
                ko, _ = fake_module()
                ko = bytearray(ko)
                ko[sprd_drm.TEXT_OFFSET + addr + byte] ^= 1
                with mock.patch.object(sprd_drm, "MODULE_SHA256", hashlib.sha256(ko).hexdigest()):
                    with self.assertRaises(SystemExit) as e:
                        sprd_drm.patch_module(bytes(ko), all_modes)
                self.assertIn("unexpected code at 0x%x" % addr, str(e.exception))

    def test_all_modes_needs_its_word(self):
        """A table whose 0x8d8c patch lacks the tbz refuses all_modes rather than replace another word."""
        table = [(addr, old, list(new)) for addr, old, new in sprd_drm.PATCHES]
        dict((addr, new) for addr, _, new in table)[0x8d8c][2] = 0x12345678
        with mock.patch.object(sprd_drm, "PATCHES", table):
            self.assertEqual(len(sprd_drm.patches(False)), len(table) + len(sprd_drm.HPD_PATCHES))
            with self.assertRaises(SystemExit) as e:
                sprd_drm.patches(True)
        self.assertIn("no all_modes word in the 0x8d8c patch", str(e.exception))

    def test_real_module(self):
        """Both variants of the real module, when the build's copy of it is at hand."""
        path = os.path.join(util.ROOT, "out", ".dock", "work", "dpmodes-kernel", "sprd-drm.orig.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        ko = util.slurp(path)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_drm.MODULE_SHA256)
        plain, every = sprd_drm.patch_module(ko, False), sprd_drm.patch_module(ko, True)
        self.assertEqual(hashlib.sha256(plain).hexdigest(),
                         "4c2b7eb7cf021941a9cc23d67abd6e86dc63c52a1ba82deb08ee4154b1bff14c")
        self.assertEqual(hashlib.sha256(every).hexdigest(),
                         "489154a1964a890dc682ed8bd5c87cf618e55d005188d1b32390277e2df3a92c")


MODULES = ["sprd-drm.ko", "wcn_bsp.ko", "mali_kbase.ko", "sprd_wlan_combo.ko", "focaltech_ts.ko",
           "sprdbt_tty.ko", "sprd_fm.ko", "gnss_common_ctl_all.ko", "gnss_dbg.ko", "gt9xx_ts.ko",
           "gnss_pmnotify_ctl.ko", "pcie-sprd-misc.ko", "audio.ko", "pcie-sprd.ko"]


class VendorDlkm(util.TestCase):
    def test_reorder(self):
        out = dlkm.reorder_modules(("\n".join(MODULES) + "\n").encode(), False).decode().split("\n")
        self.assertEqual(out[-1], "")
        self.assertEqual(out[:-1], ["sprd-drm.ko", "mali_kbase.ko", "gt9xx_ts.ko", "focaltech_ts.ko", "audio.ko",
                                    "wcn_bsp.ko", "sprd_wlan_combo.ko", "sprdbt_tty.ko", "sprd_fm.ko",
                                    "gnss_common_ctl_all.ko", "gnss_dbg.ko", "gnss_pmnotify_ctl.ko",
                                    "pcie-sprd-misc.ko", "pcie-sprd.ko"])
        leds = dlkm.reorder_modules("\n".join(MODULES).encode(), True).decode().split()
        self.assertEqual(leds[-1], "cust-leds.ko")
        self.assertEqual(sorted(leds), sorted(MODULES + ["cust-leds.ko"]))

    def test_errors(self):
        cases = [
            ([m for m in MODULES if m != "gnss_dbg.ko"], "modules.load lacks gnss_dbg.ko"),
            (MODULES + ["cust-leds.ko"], "already loads cust-leds.ko"),
            ([m for m in MODULES if m != "gt9xx_ts.ko"], "lacks the touch drivers"),
        ]
        for mods, message in cases:
            with self.assertRaises(SystemExit) as e:
                dlkm.reorder_modules("\n".join(mods).encode(), False)
            self.assertIn(message, str(e.exception))


class VendorBootCpio(util.TestCase):
    FILES = [("lib", b""), ("lib/modules/sprd-drm.ko", b"KO" * 33),
             ("lib/modules/modules.load", b"a.ko\ncust-leds.ko\nb.ko\n"), ("init", b"x" * 7)]

    def test_find_and_replace(self):
        archive = cpio.build(self.FILES)
        off, size = vboot.find_cpio_entry(archive, "lib/modules/sprd-drm.ko")
        self.assertEqual(archive[off:off + size], b"KO" * 33)
        new = vboot.replace_cpio_entry(archive, vboot.FIRST_STAGE, b"a.ko\nb.ko\n")
        self.assertEqual(cpio.members(new), [self.FILES[0], self.FILES[1],
                                             ("lib/modules/modules.load", b"a.ko\nb.ko\n"), self.FILES[3]])
        # Every size works, padding included.
        for n in range(1, 9):
            grown = vboot.replace_cpio_entry(archive, "init", b"y" * n)
            self.assertEqual(cpio.members(grown)[-1], ("init", b"y" * n))
            self.assertEqual(len(grown) % 4, 0)

    def test_missing_entry(self):
        archive = cpio.build(self.FILES)
        for func in (vboot.find_cpio_entry, lambda c, n: vboot.replace_cpio_entry(c, n, b"")):
            with self.assertRaises(SystemExit) as e:
                func(archive, "lib/modules/nope.ko")
            self.assertIn("no lib/modules/nope.ko", str(e.exception))

    def test_rejects_crc_format(self):
        # A 070702 entry's checksum would no longer match its patched data.
        archive = cpio.build(self.FILES, magic=b"070702")
        for func in (vboot.find_cpio_entry, lambda c, n: vboot.replace_cpio_entry(c, n, b"")):
            with self.assertRaises(SystemExit) as e:
                func(archive, vboot.MODULE)
            self.assertIn("not a newc (070701) cpio header at 0", str(e.exception))

    def test_drop_deferred(self):
        self.assertEqual(vboot.drop_deferred(b"a.ko\ncust-leds.ko\nb.ko\n"), b"a.ko\nb.ko\n")
        with self.assertRaises(SystemExit):
            vboot.drop_deferred(b"a.ko\nb.ko\n")


class VendorBootFooter(util.TestCase):
    def setUp(self):
        super().setUp()
        self.info = self.write("info.txt", AVB_INFO)
        self.log = self.path("shim.log")
        self.bin = util.shim_dir(self.tmp, "avbtool")
        self.env = {"PATH": self.bin + os.pathsep + os.environ["PATH"], "FAKE_AVBTOOL_INFO": self.info,
                    "SHIM_LOG": self.log}

    def test_footer_info(self):
        with mock.patch.dict(os.environ, self.env):
            salt, props, opts = vboot.footer_info("vendor_boot.img")
        self.assertEqual(salt, "0f7a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8")
        self.assertEqual(props, [
            ("com.android.build.vendor_boot.fingerprint", "Anbernic/RG_Cube/cube:14/UP1A/1:user/release-keys"),
            ("com.android.build.vendor_boot.os_version", "14")])
        # The image's own flags, not the hash descriptor's.
        self.assertEqual(opts, {"algorithm": "SHA256_RSA4096", "rollback_index": "7", "flags": "2",
                                "rollback_index_location": "1"})

    def test_footer_info_needs_every_field(self):
        for line in ("Salt:", "Rollback Index Location:", "Algorithm:"):
            text = "\n".join(i for i in AVB_INFO.splitlines() if not i.strip().startswith(line))
            with open(self.info, "w") as f:
                f.write(text)
            with mock.patch.dict(os.environ, self.env), self.assertRaises(SystemExit):
                vboot.footer_info("vendor_boot.img")

    def test_resign_carries_the_footer_over(self):
        """main() with shims for the AOSP tools: the image is re-signed like the original."""
        ramdisk = cpio.build(VendorBootCpio.FILES)
        src = self.write("vendor_boot.img", ramdisk)
        shims = util.shim_dir(self.path("more"), extra={
            # The fake image format: the image is its (uncompressed) ramdisk.
            "unpack_bootimg": """#!/bin/sh
while [ $# -gt 0 ]; do case "$1" in --boot_img) img=$2; shift;; --out) out=$2; shift;; --format) fmt=$2; shift;; esac; shift; done
mkdir -p "$out" && cp "$img" "$out/vendor_ramdisk00"
[ "$fmt" = mkbootimg ] && echo "--header_version 4 --vendor_ramdisk_fragment $out/vendor_ramdisk00"
exit 0
""",
            "lz4": """#!/bin/sh
if [ "$1" = -dc ]; then exec cat "$2"; fi
for a; do :; done; out=$a
while [ $# -gt 2 ]; do shift; done; cp "$1" "$out"
""",
            "mkbootimg": """#!/bin/sh
echo "mkbootimg $*" >>"$SHIM_LOG"
while [ $# -gt 0 ]; do case "$1" in --vendor_ramdisk_fragment) rd=$2; shift;; --vendor_boot) out=$2; shift;; esac; shift; done
cp "$rd" "$out"
"""})
        key = self.write("key.pem", "fake")
        dst = self.path("out.img")
        # The build may export THIMSY_DP_ALL_MODES=1 for vendor_dlkm; recovery never gets all_modes.
        env = dict(self.env, PATH=shims + os.pathsep + self.env["PATH"], MKBOOTIMG=os.path.join(shims, "mkbootimg"),
                   AVB_KEY=key, THIMSY_DP_ALL_MODES="1")
        calls = []

        def patch_module(ko, all_modes=None):
            calls.append(all_modes)
            return ko.lower()
        with mock.patch.dict(os.environ, env), mock.patch.object(sys, "argv", ["patch_vendor_boot.py", src, dst]), \
                mock.patch.object(vboot, "patch_module", patch_module), \
                contextlib.redirect_stdout(io.StringIO()):
            vboot.main()
        self.assertEqual(calls, [False])
        self.assertEqual(cpio.members(util.slurp(dst)), [
            VendorBootCpio.FILES[0], ("lib/modules/sprd-drm.ko", b"ko" * 33),
            ("lib/modules/modules.load", b"a.ko\nb.ko\n"), VendorBootCpio.FILES[3]])
        footer = [c for c in util.slurp(self.log, text=True).splitlines() if c.startswith("avbtool add_hash_footer")]
        self.assertEqual(len(footer), 1)
        args = footer[0].split()
        pairs = dict(zip(args[2::2], args[3::2]))
        self.assertEqual(pairs["--image"], dst)
        self.assertEqual(pairs["--partition_name"], "vendor_boot")
        self.assertEqual(pairs["--partition_size"], "104857600")
        self.assertEqual(pairs["--algorithm"], "SHA256_RSA4096")
        self.assertEqual(pairs["--rollback_index"], "7")
        self.assertEqual(pairs["--rollback_index_location"], "1")
        self.assertEqual(pairs["--flags"], "2")
        self.assertEqual(pairs["--salt"], "0f7a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8")
        self.assertEqual(args.count("--prop"), 2)
