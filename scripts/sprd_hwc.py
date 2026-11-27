"""The RG Cube's stock vendor composer (vendor/lib64/hw/hwcomposer.unisoc.so) and its patch.

The stock composer is the GammaOS Next 1.2 PAC's vendor_a, which the 1.4.4 build keeps
(scripts/build.sh: the RG Cube's vendor stays the release's).

Test 42 (out/dp-evidence/test42/y420/nv12/composer/DESIGN.md, sleep-research/SYNTHESIS.md).

SLEEP (SprdHWComposer2::SET_POWER_MODE, 0x40c60):
  - the external display (type 0x10002) takes the same path as the primary and the
    virtual display, devices[1]->set_power_mode(mode) (SprdExternalDisplayDevice::
    set_power_mode, 0x57400, DPMS on the DP connector); the vendor code returned
    success for modes 0-3 and did nothing, so a docked sleep left the monitor lit
    (sleep-research/SYNTHESIS.md).
CT4K (SprdDrm::AddFlushData, 0x5f5b0):
  - at an external mode exactly 3840 wide the vendor code keeps only layers whose
    integer source-crop width (SprdHWLayer+1196) is 3840 ("4k bypass mode drop non-4k
    layer"). The client target SprdDisplayClient::SET_CLIENT_TARGET (0x4a1f0) builds
    never gets an integer crop (only the primary and virtual commits set one, 0x4ce8c,
    0x51fc0), so it was always dropped, PostDisplay found no layer and every present
    failed (test 42 A4). Now a layer whose integer crop is unset (0) is kept when its
    display frame (+1244: the buffer's width, since SurfaceFlinger sends no damage) is
    3840 wide. Every layer with a crop is judged as before. The debug-only log line of
    the drop is given up for the room.

The library's .text is at file offset == address (0x3f000), so addresses are file offsets.
"""
import hashlib
import struct
import sys

COMPOSER_SHA256 = "f3e18910fe5830a927a3a7957b617002e8c05335a33b2eab7c598862ec704a01"
TEXT = (0x3f000, 0x3f000 + 0x3bc28)   # .text [start, end)

# (address, original words, patched words)
SLEEP_PATCHES = [
    # 0x40d2c  cmp w5,#4 ; b.cc 0x40d88 (mov w0,wzr; ret)
    #      ->  mov x8,#1 ; b 0x40d08 (cmp w5,#3; b.hi 0x40d34 error; devices[x8]->set_power_mode)
    (0x40d2c, [0x710010bf, 0x540002c3], [0xd2800028, 0x17fffff6]),
]
CT4K_PATCHES = [
    # 0x5f670  ldr w8,[x23] ; cbnz w8,0x5f6f0        (debug flag -> log, then drop)
    #      ->  cbnz w8,0x5f678 ; b 0x5f6f0           (w8 = crop width != 3840: crop set -> drop;
    #                                                  crop 0 -> look at the display frame)
    (0x5f670, [0xb94002e8, 0x350003e8], [0x35000048, 0x1400001f]),
    # 0x5f6f0  mov w0,#4 ; mov x1,x25 ; mov x2,x26 ; bl __android_log_print
    #      ->  ldr x8,[x22,w24,sxtw #3]  (layer = list[i], as 0x5f65c)
    #          ldr w8,[x8,#1244]         (display frame width)
    #          cmp w8,#0xf00
    #          b.eq 0x5f650              (keep: i + 1)
    #  0x5f700 b 0x5f678 (unchanged: drop)
    (0x5f6f0, [0x52800080, 0xaa1903e1, 0xaa1a03e2, 0x94006d6d],
              [0xf878dac8, 0xb944dd08, 0x713c011f, 0x54fffaa0]),
]
PATCHES = SLEEP_PATCHES + CT4K_PATCHES


def patch_library(lib, sleep=True, ct4k=True):
    if hashlib.sha256(lib).hexdigest() != COMPOSER_SHA256:
        sys.exit("hwcomposer.unisoc.so is not the stock RG Cube composer this patch is for")
    lib = bytearray(lib)
    table = (SLEEP_PATCHES if sleep else []) + (CT4K_PATCHES if ct4k else [])
    for addr, old, new in sorted(table):
        n = len(old)
        if not (TEXT[0] <= addr and addr + 4 * n <= TEXT[1]) or len(new) != n:
            sys.exit("bad patch at 0x%x" % addr)
        if list(struct.unpack_from("<%dI" % n, lib, addr)) != old:
            sys.exit("unexpected code at 0x%x" % addr)
        struct.pack_into("<%dI" % n, lib, addr, *new)
    return bytes(lib)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: sprd_hwc.py IN OUT")
    with open(sys.argv[1], "rb") as f:
        out = patch_library(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print("hwcomposer.unisoc.so: SLEEP + CT4K, sha256 %s" % hashlib.sha256(out).hexdigest())
