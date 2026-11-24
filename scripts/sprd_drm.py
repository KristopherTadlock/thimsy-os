"""The RG Cube 1.2 vendor kernel's DisplayPort driver (sprd-drm.ko) and its patch.

Plugging in a monitor made the vendor composer set the largest mode the monitor
lists. For any 3840x2160 mode the driver goes into bypass and reads the plane's
framebuffer format, but the composer's hotplug commit has no framebuffer, so
the kernel panicked; and 4K at 60 Hz needs more than the Cube's two DP lanes.

sprd_dp_connector_mode_valid:
  - rejects modes with a pixel clock above 4K at 30 Hz (about 297 MHz, the
    most the display controller's pixel clock reaches);
  - rejects modes the trained link can't carry at RGB 8 bpc, from the rate and
    lane count of the last training (the driver would program an impossible
    transfer unit for them and ignore the error); a link that never trained is
    not checked;
  - rejects modes wider than 3840 (DCI 4096x2160), which the vendor composer
    and display controller don't handle;
  - keeps CEA 1080p60 the only preferred mode, as the vendor driver does, so
    the composer sets 1080p at 60 Hz when a monitor connects; the monitor's own
    preferred mode becomes user-defined instead of being dropped, since the
    vendor composer lists only preferred and user-defined modes. With
    all_modes, every mode that passes becomes user-defined, so the composer
    lists them all; that is for the 1.4.4 base only, whose framework holds a
    monitor at the mode the user chose. CEA 1080p60 stays the only preferred
    mode either way.
sprd_dp_encoder_mode_set:
  - treats a missing framebuffer in bypass as RGB instead of reading through it.
sprd_dp_pd_event (HPD_PATCHES):
  - while DisplayPort is off, powers it up for an HPD with IRQ_HPD set (hpd 2)
    as well as for a plain HPD (hpd 1); the vendor driver ignored a first
    Attention that had IRQ_HPD set, so an adapter that sends one never lit up.

The module is in vendor_dlkm (loaded at boot) and in vendor_boot (recovery).
all_modes defaults to THIMSY_DP_ALL_MODES=1 in the environment; recovery's copy
never gets it (patch_vendor_boot.py).
"""
import hashlib
import os
import struct
import sys

MODULE_SHA256 = "4d60c3c021bdd779cd55502b73fbe849eb2e773f7b9671dbd843444d8ce5f932"
TEXT_OFFSET = 0x30000   # .text in the module file
NOP = 0xd503201f

# (address in .text, original words, patched words)
PATCHES = [
    # mode_valid: clock > 0x49000 kHz -> MODE_CLOCK_HIGH; keep the type's preferred bit;
    # then the link and width checks at 0x8db8.
    (0x8cdc, [0xb9403668, 0x36180068, 0x121c7908, 0xb9003668],
             [0xb9403a6a, 0x7141255f, 0x540007a8, 0x14000034]),   # b 0x8db8
    # mode_valid: every mode but CEA 1080p60 (other CEA codes, non-CEA modes, a
    # 1080p at another clock, 640x480 at another clock) goes to the stub below,
    # which makes a preferred mode user-defined, so the composer still lists it
    # and CEA 1080p60 stays the only preferred mode.
    (0x8d04, [0x54000a81], [0x54000461]),   # b.ne stub
    (0x8d18, [0x540009e1], [0x540003c1]),   # b.ne stub
    (0x8d88, [0x54000661], [0x54000041]),   # b.ne stub
    # CEA 640x480 skips its log line (its space holds the stub) and stays
    # user-defined; then the stub. ALL_MODES swaps its tbz for a nop.
    (0x8d8c, [0xb940b663, 0x29479a65, 0x2948a667, 0x294a2e6a, 0x294b366c, 0xb940666e],
             [0x1400002f,               # b    0x8e48: type |= user-defined
              0xb9403668,               # ldr  w8, [x19, #0x34]   (type)
              0x36180608,               # tbz  w8, #3, return     (not preferred)
              0x121c7908,               # and  w8, w8, #~preferred
              0x321b0108,               # orr  w8, w8, #user-defined
              0x1400002c]),             # b    0x8e50: store type, return
    # mode_valid: drop modes the trained link can't carry at RGB 8 bpc, and modes
    # wider than 3840. The code runs in the dead log-line tails of CEA 640x480 and
    # CEA 4K60 (unreachable since 0x8d8c and 0x8dd8 are patched), in four pieces
    # around the relocations at 0x8da4-0x8db4, 0x8e0c, 0x8e10, 0x8e34, 0x8e38 and
    # 0x8e44, which stay as they are. x20 is the connector, x19 the mode, w10 the
    # clock in kHz, w0 the CEA code; x11-x15 are free.
    (0x8db8, [0x528c4ae4, 0xb90033ee, 0xb9002be8, 0xb90023ed, 0xb9001bec, 0xb90013eb,
              0xb9000bea, 0x1400001b],
             [0xf943ca8b,               # ldr  x11, [x20, #0x790]  (dptx)
              0xb400034b,               # cbz  x11, 0x8e24
              0x9140056b,               # add  x11, x11, #0x1000
              0x39455d6c,               # ldrb w12, [x11, #0x157]  (trained rate)
              0x3945616d,               # ldrb w13, [x11, #0x158]  (trained lanes)
              0x340002cd,               # cbz  w13, 0x8e24         (never trained: skip)
              0x5280144e,               # mov  w14, #162
              0x14000007]),             # b    0x8df0
    # mode_valid: the CEA 4K60 branch (now unreachable) becomes the MODE_CLOCK_HIGH return.
    (0x8dd8, [0xb9403a69, 0x52820a0a, 0x72a0012a, 0x6b0a013f, 0x54000361, 0x39431289],
             [0xa9447bfd, 0xa9454ff4, 0xf85f8e5e, 0x528001e0, 0x910183ff, 0xd65f03c0]),
    (0x8df0, [0x36180329, 0x294a2e6a, 0x294b366c, 0xb940666e, 0xb940b663, 0x29479a65,
              0x2948a667],
             [0x528021cf,               # mov  w15, #270
              0x1b0c7def,               # mul  w15, w15, w12
              0x7100019f,               # cmp  w12, #0
              0x1a8f01ce,               # csel w14, w14, w15, eq   (rate 0 is 162, else 270 * rate)
              0x1b0d7dce,               # mul  w14, w14, w13
              0x52807d0f,               # mov  w15, #1000
              0x14000003]),             # b    0x8e14
    (0x8e14, [0x52820a04, 0x91004262, 0xb90033ee, 0xb9002be8, 0xb90023ed, 0xb9001bec,
              0xb90013eb, 0xb9000bea],
             [0x1b0f7dce,               # mul  w14, w14, w15       (bytes per ms the link carries)
              0x0b0a054f,               # add  w15, w10, w10, lsl #1 (bytes per ms at RGB 8 bpc)
              0x6b0e01ff,               # cmp  w15, w14
              0x54fffdc8,               # b.hi 0x8dd8              (MODE_CLOCK_HIGH)
              0xb9403e6f,               # 0x8e24: ldr w15, [x19, #0x3c] (hdisplay)
              0x713c01ff,               # cmp  w15, #0xf00
              0x54fffd68,               # b.hi 0x8dd8              (wider than 3840: MODE_CLOCK_HIGH)
              0x14000003]),             # b    0x8e3c
    (0x8e3c, [0x72a00124, 0xb90003e9],
             [0xb9403668,               # ldr  w8, [x19, #0x34]    (type)
              0x17ffffab]),             # b    0x8cec
    # encoder_mode_set: NULL framebuffer in bypass -> RGB, 8 bpc.
    (0x90f4, [0xf94042c8, 0x528ac9c9, 0x72a64629, 0xf9426d08, 0xf9400908, 0xf9402508,
              0xb9400108, 0x6b09011f, 0x54000100, 0x52860a09, 0x72a60629, 0x6b09011f,
              0x540000c1, 0x52800028, 0x52800149, 0x14000005, 0x52800028, 0x14000002,
              0x2a1f03e8, 0x52800109],
             [0xf94042c8, 0xf9426d08, 0xf9400908, 0xb40001e8, 0xf9402508, 0xb940010a,
              0x52800028, 0x528ac9c9, 0x72a64629, 0x6b09015f, 0x54000120, 0x52860a09,
              0x72a60629, 0x6b09015f, 0x54000081, 0x52800149, 0x14000004, 0xd503201f,
              0x2a1f03e8, 0x52800109]),
]

# pd_event while DisplayPort is off (its "on" byte clear): the vendor code powers
# up for hpd 1 only (cmp w8, #1; b.ne out). Power up when exactly one of bits 0
# and 1 is set: hpd 1 and 2 power up, 0 and 3 are ignored as before. w8 is dead
# after it (0x89c8 and 0x8b1c load what they use).
HPD_PATCHES = [
    (0x89b4, [0x7100051f, 0x54000b21],
             [0x4a480508,               # eor  w8, w8, w8, lsr #1
              0x36000b28]),             # tbz  w8, #0, 0x8b1c
]

# all_modes: the stub's tbz (new word 2 of the 0x8d8c patch, at 0x8d94) becomes a
# nop, so every mode that passes is made user-defined, not only a preferred one.
# (address of the patch, index of the word, the word without and with all_modes)
ALL_MODES = (0x8d8c, 2, 0x36180608, NOP)


def patches(all_modes):
    """Every (address, original words, patched words), in address order."""
    out = []
    for addr, old, new in sorted(PATCHES + HPD_PATCHES):
        if all_modes and addr == ALL_MODES[0]:
            if new[ALL_MODES[1]] != ALL_MODES[2]:
                sys.exit("sprd_drm: no all_modes word in the 0x%x patch" % addr)
            new = list(new)
            new[ALL_MODES[1]] = ALL_MODES[3]
        out.append((addr, old, new))
    return out


def patch_module(ko, all_modes=None):
    if all_modes is None:
        all_modes = os.environ.get("THIMSY_DP_ALL_MODES") == "1"
    if hashlib.sha256(ko).hexdigest() != MODULE_SHA256:
        sys.exit("sprd-drm.ko is not the 1.2 module this patch is for")
    ko = bytearray(ko)
    for addr, old, new in patches(all_modes):
        off = TEXT_OFFSET + addr
        n = len(old)
        if list(struct.unpack_from("<%dI" % n, ko, off)) != old:
            sys.exit("unexpected code at 0x%x" % addr)
        struct.pack_into("<%dI" % n, ko, off, *new)
    return bytes(ko)
