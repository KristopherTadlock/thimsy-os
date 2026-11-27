#!/usr/bin/env python3
"""Patches the RG Cube 1.2 vendor_dlkm: its DisplayPort driver (sprd-drm.ko), its charger manager
(sprd-charger-manager.ko) and the order its modules load in.

  patch_vendor_dlkm.py [--load-leds] [--stock-charger] IN OUT

Applies sprd_drm.py's patch to the copy the kernel loads at boot (sprd_drm.patch_release: the result
must be, byte for byte, the module sprd_drm.RELEASE_SHA256 pins, MOD-Y1-E with THIMSY_DP_ALL_MODES=1
as tested on the RG Cube in test 42, or the build stops). vendor_dlkm is a compressed
EROFS image, so it is rebuilt (erofs_image.py). vendor_dlkm has no verity.

The charger manager gets sprd_cm.py's CM12 (PDHUB and PD10W): an SDP behind a USB PD hub or AMA
charges at the 1.05 A table instead of 0.5 A, and an explicit 5 V 3 A USB PD contract at the
2.0 A DCP table instead of 1.05 A. The stock module goes in (its sha256 is checked) and the result
must be, byte for byte, the module tested on the RG Cube in test 42 (sprd_cm.TESTED_SHA256), or
the build stops. PD10W reads sprd_tcpm's port struct at fixed offsets, so the image's
sprd_tcpm.ko must be the one they were taken from (sprd_cm.check_tcpm; it is not changed). The
owner shipped it as tested, with the known limits sprd_cm.py's docstring lists (decisions D11 and
D12). --stock-charger (build.sh CHARGE_FIX=0) keeps the stock charger manager and skips both.
Recovery loads its own copy from vendor_boot, which stays stock (patch_vendor_boot.py). Off-mode
charging loads this copy (modules.load.charger), after sprd_tcpm.ko and sprd_battery_info.ko, as
modules.load does; the patch changes neither list's order for them.

The vendor's insmod-sh loads lib/modules/modules.load one module at a time, in the background,
from early in boot. Its first module, the Wi-Fi/Bluetooth base (wcn_bsp.ko), pulls in the PCIe
driver, which waits a second for a link the RG Cube does not have (nothing is on its PCIe), so the
display and GPU drivers the screen needs, the controls, touch and audio all waited behind it. The
radios and PCIe now load last, and the Goodix touch driver before the FocalTech one (they share
pins; see TOUCH_FIRST). The LED driver (cust-leds.ko, a one-second probe), moved out of the first
stage (patch_vendor_boot.py), where nothing else could run meanwhile, is not in this list: init loads
it in a process of its own beside it (system/core/rootdir/init.rc in the Nano patch). With a system
image that lacks that (--load-leds), it loads after the radios instead.

Needs on PATH: fsck.erofs and mkfs.erofs (erofs-utils 1.7 or later).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sprd_cm  # noqa: E402
from erofs_image import rebuild  # noqa: E402
from sprd_drm import patch_release  # noqa: E402

MODULE = "lib/modules/sprd-drm.ko"
MODULE_LIST = "lib/modules/modules.load"
CHARGER = "lib/modules/sprd-charger-manager.ko"
TCPM = "lib/modules/sprd_tcpm.ko"
# The radios and what they depend on (modules.dep), loaded last.
LAST = ["wcn_bsp.ko", "sprd_wlan_combo.ko", "sprdbt_tty.ko", "sprd_fm.ko", "gnss_common_ctl_all.ko",
        "gnss_dbg.ko", "gnss_pmnotify_ctl.ko", "pcie-sprd-misc.ko", "pcie-sprd.ko"]
DEFERRED = "cust-leds.ko"   # from the first stage; init loads it beside this list (init.rc)
# Two touch drivers for the panels the RG Cube ships with, on the same I2C bus and the same reset and
# interrupt pins (the board's device tree). The FocalTech one first spent 1.3 s retrying a chip that
# is not there before it gave the pins up; with Goodix first, whichever panel is fitted takes the pins
# and the other driver fails at once.
TOUCH_FIRST, TOUCH_THEN = "gt9xx_ts.ko", "focaltech_ts.ko"


def reorder_modules(data, load_leds):
    mods = data.decode().split()
    if sorted(set(LAST) - set(mods)):
        sys.exit("vendor_dlkm: modules.load lacks %s" % sorted(set(LAST) - set(mods))[0])
    if DEFERRED in mods:
        sys.exit("vendor_dlkm: modules.load already loads %s" % DEFERRED)
    if TOUCH_FIRST not in mods or TOUCH_THEN not in mods:
        sys.exit("vendor_dlkm: modules.load lacks the touch drivers")
    order = [m for m in mods if m not in LAST and m != TOUCH_FIRST]
    order.insert(order.index(TOUCH_THEN), TOUCH_FIRST)
    order += [m for m in mods if m in LAST]
    if load_leds:
        order.append(DEFERRED)
    return ("\n".join(order) + "\n").encode()


def changes(load_leds, charger):
    """The files the rebuild changes: path in the image -> function of the old contents."""
    out = {MODULE: patch_release, MODULE_LIST: lambda d: reorder_modules(d, load_leds)}
    if charger:
        out[CHARGER] = sprd_cm.patch_tested
        out[TCPM] = sprd_cm.check_tcpm      # gives it back unchanged
    return out


def main():
    args = sys.argv[1:]
    load_leds = "--load-leds" in args
    charger = "--stock-charger" not in args
    args = [a for a in args if a not in ("--load-leds", "--stock-charger")]
    if len(args) != 2:
        sys.exit(__doc__)
    src, dst = args
    new = rebuild(src, "vendor_dlkm", changes(load_leds, charger))
    with open(dst, "wb") as f:
        f.write(new)
    done = [MODULE, MODULE_LIST] + ([CHARGER] if charger else [])
    print("patched %s -> %s%s" % (", ".join(done), dst, "" if charger else " (stock charger manager)"))


if __name__ == "__main__":
    main()
