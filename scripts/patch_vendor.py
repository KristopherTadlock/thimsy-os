#!/usr/bin/env python3
"""Patches the RG Cube 1.2 vendor partition (vendor_a): its composer, /vendor/lib64/hw/hwcomposer.unisoc.so.

  patch_vendor.py [--ct4k-only] IN OUT

The composer gets sprd_hwc.py's two patches (build.sh COMPOSER_FIX=1, the default):
  - SLEEP: Android's sleep turns an external monitor off (DPMS) and wakes it again; the vendor
    composer ignored the external display's power mode, so a docked sleep left the monitor lit;
  - CT4K: at a 3840-wide external mode the composer keeps SurfaceFlinger's client target, which it
    dropped (every present failed), so 4K60 4:2:0 output can show the composed screen.
--ct4k-only (COMPOSER_FIX=ct4k) applies CT4K alone, the composer test 42's T3 (4K60 4:2:0) ran.
The stock composer goes in (sprd_hwc.COMPOSER_SHA256 is checked) and the result must be, byte for
byte, the composer that ran on the RG Cube in test 42: SLEEP + CT4K in T2-sleep (SLEEP_CT4K_SHA256),
CT4K alone in T3 (CT4K_SHA256, out/dp-evidence/test42/t3/t3_prepare.sh); else the build stops.

The composer HAL service (/vendor/bin/hw/android.hardware.graphics.composer@2.4-service) loads it with
hw_get_module("hwcomposer") (ro.hardware.hwcomposer=unisoc). vendor is an ext4 image, and the patch
changes code words only (two 4 KiB blocks of the library for SLEEP + CT4K, one for CT4K), so the
library is rewritten in place (ext4_image.py): its size, inode, owner, mode, times and SELinux label
(u:object_r:vendor_file:s0) and every other byte of the image stay as they were. The 32-bit copy
(/vendor/lib/hw) is not used by the 64-bit service and stays stock.

vendor's dm-verity: vbmeta_vendor describes a hashtree for it, but the RG Cube's fstab (vendor_boot's
first_stage_ramdisk/fstab.ums9620_*) mounts every logical partition without avb=, so no partition in
super is verified at boot (system, vendor_dlkm and odm are already replaced the same way). The stock
vendor_a does not match that hashtree either (it describes a 681426944-byte image; vendor_a holds a
983040000-byte filesystem with no hashtree after it). The bootloader checks only the hash descriptors
(boot, vendor_boot, dtbo) and the vbmeta signatures, none of which this touches.

Needs on PATH: debugfs, dumpe2fs and e2fsck.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ext4_image  # noqa: E402
import sprd_hwc  # noqa: E402

COMPOSER = "/lib64/hw/hwcomposer.unisoc.so"
# SLEEP + CT4K of sprd_hwc.COMPOSER_SHA256: ran on the RG Cube in test 42 (dp_dock_findings.md T2-sleep).
SLEEP_CT4K_SHA256 = "86ea70726cba9da3d956640c9ec39cee3df0d9a8429539c9591fc80cb2736078"
# CT4K alone: the composer of test 42's T3, 4K60 4:2:0 with correct colours (t3/hwcomposer-ct4k.so).
CT4K_SHA256 = "dce325717c0c86e57a42073c932d116c5187f351dc3b273f83b8d402011f1265"


def patch_composer(lib, sleep=True):
    """The release build's composer: the stock one in, exactly a composer tested on the RG Cube out."""
    out = sprd_hwc.patch_library(lib, sleep=sleep, ct4k=True)   # exits unless lib is the stock composer
    want = SLEEP_CT4K_SHA256 if sleep else CT4K_SHA256
    if ext4_image.sha256(out) != want:
        sys.exit("hwcomposer.unisoc.so: the patch no longer gives the composer tested on the RG Cube")
    return out


def main():
    args = sys.argv[1:]
    sleep = "--ct4k-only" not in args
    args = [a for a in args if a != "--ct4k-only"]
    if len(args) != 2 or any(a.startswith("-") for a in args):
        sys.exit(__doc__)
    src, dst = args
    written = ext4_image.patch_file(src, dst, COMPOSER, lambda lib: patch_composer(lib, sleep), "vendor")
    print("vendor: %s %s (sha256 %s), %d blocks written -> %s" % (
        COMPOSER, "SLEEP + CT4K" if sleep else "CT4K only", SLEEP_CT4K_SHA256 if sleep else CT4K_SHA256,
        len(written), dst))


if __name__ == "__main__":
    main()
