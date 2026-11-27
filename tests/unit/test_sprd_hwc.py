"""scripts/sprd_hwc.py: the vendor composer's SLEEP and CT4K patches."""
import hashlib
import os
import struct
import sys

from tests import a64, util

sys.path.insert(0, os.path.join(util.ROOT, "scripts"))
import sprd_hwc  # noqa: E402

LIB = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "bypass-research", "vendor", "hwcomposer.unisoc.so")
PATCHED_SHA256 = "86ea70726cba9da3d956640c9ec39cee3df0d9a8429539c9591fc80cb2736078"


class SprdHwc(util.TestCase):
    def lib(self):
        if not os.path.isfile(LIB):
            self.skipTest("no %s" % LIB)
        return util.slurp(LIB)

    def test_patched(self):
        lib = self.lib()
        out = sprd_hwc.patch_library(lib)
        self.assertEqual(hashlib.sha256(out).hexdigest(), PATCHED_SHA256)
        changed = [a for a in range(0, len(lib), 4) if lib[a:a + 4] != out[a:a + 4]]
        self.assertEqual(changed, [0x40d2c, 0x40d30, 0x5f670, 0x5f674, 0x5f6f0, 0x5f6f4, 0x5f6f8, 0x5f6fc])
        self.assertEqual(sprd_hwc.patch_library(lib, sleep=False, ct4k=False), lib)

    def test_words(self):
        """The new words decode and branch where the comments say."""
        text = {addr + 4 * i: a64.disassemble(addr + 4 * i, w)
                for addr, _, new in sprd_hwc.PATCHES for i, w in enumerate(new)}
        self.assertEqual(text, {
            0x40d2c: "mov x8, #0x1", 0x40d30: "b 0x40d08",
            0x5f670: "cbnz w8, 0x5f678", 0x5f674: "b 0x5f6f0",
            0x5f6f0: "? f878dac8",   # ldr x8, [x22, w24, sxtw #3] (GNU objdump; a64.py lacks it)
            0x5f6f4: "ldr w8, [x8, #1244]", 0x5f6f8: "cmp w8, #0xf00", 0x5f6fc: "b.eq 0x5f650"})

    def test_wrong_library(self):
        with self.assertRaises(SystemExit):
            sprd_hwc.patch_library(b"\0" * 4096)
