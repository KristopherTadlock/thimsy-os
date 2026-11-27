"""The pure parts of sprd_drm.py, patch_vendor_dlkm.py and patch_vendor_boot.py."""
import contextlib
import hashlib
import io
import os
import struct
import sys
from unittest import mock

from tests import a64, util
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


def all_sites():
    """Every patch, the all_modes and wide ones included (MP's original words take in the 0x90f4 patch's)."""
    return (sprd_drm.PATCHES + sprd_drm.HPD_PATCHES + sprd_drm.IRQ_GUARD_PATCHES + sprd_drm.IRQ_MASK_PATCHES
            + sprd_drm.ALL_MODES_PATCHES + sprd_drm.WIDE_PATCHES + sprd_drm.WIDE_MP_PATCHES)


def words(table):
    """{address: word} of every word in TABLE's patched words."""
    return {addr + 4 * i: w for addr, _, new in table for i, w in enumerate(new)}


def fake_module():
    """A module image with the original words at every patch site, and its sha256."""
    sites = all_sites()
    end = max(addr + 4 * len(old) for addr, old, _ in sites)
    ko = bytearray(b"\x5a" * (sprd_drm.TEXT_OFFSET + end + 64))
    for addr, old, _ in sites:
        struct.pack_into("<%dI" % len(old), ko, sprd_drm.TEXT_OFFSET + addr, *old)
    return bytes(ko), hashlib.sha256(ko).hexdigest()


def patched(all_modes, env=None, wide=None, wide_mp=None):
    """fake_module() and patch_module(it, all_modes, wide, wide_mp), with THIMSY_DP_ALL_MODES only as ENV sets it."""
    ko, sha = fake_module()
    with mock.patch.object(sprd_drm, "MODULE_SHA256", sha), mock.patch.dict(os.environ):
        os.environ.pop("THIMSY_DP_ALL_MODES", None)
        os.environ.update(env or {})
        return ko, sprd_drm.patch_module(ko, all_modes, wide, wide_mp)


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
LDR_TYPE = 0xb9403668            # ldr w8, [x19, #52], at 0x8d90 and 0x8e3c
B_HS_WIDE = 0x54fffd62           # b.hs 0x8dd8 (3840 or more wide), at 0x8e2c
B_LINK = 0x14000034              # b 0x8db8 (the link check), at 0x8ce8
B_MODE_FILTER = 0x14006f25       # b 0x2497c (the mode table's filter), at 0x8ce8 with all_modes
B_MODE_OK = 0x14000006           # b 0x8e54 (MODE_OK), at 0x8e3c with all_modes
# Fix C's words, gone: b 0x202b4 at 0x8d90 and the nop at 0x8d94 (all_modes, until test 42 MOD-M).
B_SAFE_START = 0x14005d49
# mov w24, #3 and mov w12, #3: the mode table's record count without WIDE.
NREC3 = {0x202d0: 0x52800078, 0x24980: 0x5280006c}
# Every word the mode table or fix C sets: the table's patches, the two words all_modes swaps
# for it, and fix C's two.
MODES_SITES = set(words(sprd_drm.MODES_PATCHES)) | {0x8ce8, 0x8e3c, 0x8d90, 0x8d94}
# Every word fix E (the DPTX interrupt mask, dp_dock_findings.md T2-sleep) sets: encoder_disable's
# and encoder_enable's hooks and caves, pd_event off's cave (fix B's, rewritten, one word longer)
# and dptx_core_init's IEN store. Newer than every test 42 module up to MOD-Y1.
IRQ_MASK_SITES = ({0x9208, 0x9550, 0x331fc} | set(range(0x328c0, 0x328d4, 4))
                  | set(range(0x329f0, 0x32a10, 4)) | set(range(0x32968, 0x3299c, 4)))
# pd_event off's first two words (the dptx and its registers) are fix B's as well.
SAME_AS_B = {0x32968, 0x3296c}

# What each new word is, as GNU objdump prints it (aarch64-linux-gnu-objdump -d of
# the patched module, "<symbol>" and "// comment" parts left out, branch targets as
# 0x...; b.cs is b.hs). tests/a64.py decodes the words to the same text.
DISASSEMBLY = {
    "IRQ_GUARD_PATCHES": {
        0x87ec: "b 0x329b0",                      # bind: to the Type-C mask
        0x89f4: "b 0x3292c",                      # pd_event on: glb->enable, then the flag
        0x8a94: "b 0x32968",                      # pd_event off: mask, then glb->disable
        0x920c: "strb wzr, [x19, #2960]",         # encoder_disable: the flag first
        0x9210: "ldr x8, [x19, #2840]",
        0x9214: "ldr x8, [x8, #16]",
        0x9218: "cbz x8, 0x923c",
        0x9324: "b 0x32958",                      # encoder_enable: glb->enable, then the flag
        0x328f4: "ldr x9, [x1, #40]",             # the guard
        0x328f8: "cbz x9, 0x32924",
        0x328fc: "ldr x9, [x9, #152]",
        0x32900: "cbz x9, 0x32924",
        0x32904: "ldrb w10, [x9, #2960]",
        0x32908: "ldrb w11, [x9, #3001]",
        0x3290c: "orr w10, w10, w11",
        0x32910: "cbz w10, 0x32924",
        0x32914: "b 0x3291c",
        0x3291c: "str x30, [x18], #8",
        0x32920: "b 0x37b54",
        0x32924: "mov w0, wzr",
        0x32928: "ret",
        0x3292c: "blr x8",                        # pd_event on
        0x32930: "mov w9, #0x1",
        0x32934: "strb w9, [x20, #3001]",
        0x32938: "b 0x89f8",
        0x32958: "blr x19",                       # encoder_enable
        0x3295c: "mov w8, #0x1",
        0x32960: "strb w8, [x21, #2960]",
        0x32964: "b 0x9328",
        0x32968: "ldr x9, [x20, #2984]",          # pd_event off
        0x3296c: "ldr x10, [x9, #3432]",
        0x32970: "ldr w11, [x10, #3332]",
        0x32974: "and w11, w11, #0xffffff7f",
        0x32978: "dmb oshst",
        0x3297c: "str w11, [x10, #3332]",
        0x32980: "ldr w11, [x10, #3332]",
        0x32984: "dmb osh",
        0x32988: "ldr x8, [x20, #2840]",          # the flag stays set (0x8adc clears it)
        0x3298c: "ldr x8, [x8, #16]",
        0x32990: "cbz x8, 0x8ac4",
        0x32994: "b 0x8aa4",
        0x329b0: "ldr x9, [x19, #2984]",          # bind
        0x329b4: "ldr x10, [x9, #3432]",
        0x329b8: "ldr w11, [x10, #3332]",
        0x329bc: "and w11, w11, #0xffffff7f",
        0x329c0: "dmb oshst",
        0x329c4: "str w11, [x10, #3332]",
        0x329c8: "ldr w11, [x10, #3332]",
        0x329cc: "dmb osh",
        0x329d0: "blr x21",
        0x329d4: "b 0x87f0",
        0x32850: "ldr x9, [x19, #40]",            # the threaded handler's guard
        0x32854: "cbz x9, 0x32878",
        0x32858: "ldr x9, [x9, #152]",
        0x3285c: "cbz x9, 0x32878",
        0x32860: "ldrb w10, [x9, #2960]",
        0x32864: "ldrb w11, [x9, #3001]",
        0x32868: "orr w10, w10, w11",
        0x3286c: "cbz w10, 0x32878",
        0x32870: "ldr x8, [x19, #3432]",
        0x32874: "b 0x37530",
        0x32878: "str wzr, [x19, #3420]",         # not clocked: its unplug path
        0x3287c: "str wzr, [x19, #3544]",
        0x32880: "mov w22, wzr",
        0x32884: "mov w8, #0x1156",
        0x32888: "add x23, x19, x8",
        0x3288c: "mov w0, wzr",
        0x32890: "b 0x375e8",
        0x3752c: "b 0x32850",                     # dptx_threaded_irq: to its guard
        0x37b50: "b 0x328f4",                     # dptx_irq: to the guard
    },
    "IRQ_MASK_PATCHES": {
        0x9208: "b 0x328c0",                      # encoder_disable: IEN = 0 first
        0x328c0: "str w9, [x8]",                  # the vendor's 0x300 write
        0x328c4: "str wzr, [x8, #2564]",          # 0x300 + 0xa04: IEN
        0x328c8: "ldr w9, [x8, #2564]",
        0x328cc: "dmb osh",
        0x328d0: "b 0x920c",                      # fix B's flag clear, glb->disable
        0x9550: "b 0x329f0",                      # encoder_enable, at its end: IEN back
        0x329f0: "ldr x9, [x21, #2984]",
        0x329f4: "ldr x10, [x9, #3432]",
        0x329f8: "ldrb w9, [x21, #3001]",
        0x329fc: "mov w11, #0x15",
        0x32a00: "orr w11, w11, w9, lsl #7",
        0x32a04: "str w11, [x10, #3332]",
        0x32a08: "strb w8, [x21, #2960]",
        0x32a0c: "b 0x9554",
        0x32968: "ldr x9, [x20, #2984]",          # pd_event off (fix B's cave, rewritten)
        0x3296c: "ldr x10, [x9, #3432]",
        0x32970: "ldrb w9, [x20, #2960]",         # encoder on: IEN = 0x15, off: 0
        0x32974: "mov w11, #0x15",
        0x32978: "mul w11, w11, w9",
        0x3297c: "dmb oshst",
        0x32980: "str w11, [x10, #3332]",
        0x32984: "ldr w11, [x10, #3332]",
        0x32988: "dmb osh",
        0x3298c: "ldr x8, [x20, #2840]",          # the flag stays set (0x8adc clears it)
        0x32990: "ldr x8, [x8, #16]",
        0x32994: "cbz x8, 0x8ac4",
        0x32998: "b 0x8aa4",
        0x331fc: "mov w8, w10",                   # dptx_core_init: IEN = 0x95 exactly
    },
    "MODE_CHANGE_PATCHES": {
        0x2614: "b.eq 0x61a4",
        0x26ac: "b.eq 0x61d8",
        0x61a4: "ldr w9, [x19, #1816]",
        0x61a8: "cmp w9, #0x2",
        0x61ac: "b.ne 0x2644",
        0x61b0: "b 0x2618",
        0x61d8: "ldr w9, [x19, #1816]",
        0x61dc: "cmp w9, #0x2",
        0x61e0: "b.ne 0x2720",
        0x61e4: "b 0x26b0",
    },
    "GET_MODES_HOOKS": {
        0x8c7c: "b 0x202bc",                      # after drm_add_edid_modes
        0x8ca0: "b 0x202b4",                      # no EDID
    },
    "GET_MODES": {
        0x202b4: "mov x20, xzr",                  # no EDID: kfree(NULL)
        0x202b8: "mov w0, wzr",
        0x202bc: "sub sp, sp, #0x30",             # from the EDID path
        0x202c0: "stp x21, x22, [sp]",
        0x202c4: "stp x23, x24, [sp, #16]",
        0x202c8: "str x25, [sp, #32]",
        0x202cc: "mov w22, w0",
        0x202d0: "mov w24, #0x4",                 # the records (NREC)
        0x202d4: "cbnz x20, 0x202dc",
        0x202d8: "mov w24, #0x2",                 # no EDID: two
        0x202dc: "adr x21, 0x227a0",              # the table
        0x202e0: "mov x23, xzr",
        0x202e4: "ldr w9, [x21, #72]",            # each record: its gate
        0x202e8: "cbz w9, 0x20364",
        0x202ec: "cbz x20, 0x20364",
        0x202f0: "cbz x23, 0x203f0",
        0x202f4: "ldr w11, [x21, #76]",
        0x202f8: "and w15, w9, #0xffff",
        0x202fc: "lsr w16, w9, #16",
        0x20300: "ldr x12, [x23]",
        0x20304: "cmp x12, x23",                  # each mode on the list
        0x20308: "b.eq 0x203f0",
        0x2030c: "ldr w13, [x12, #60]",
        0x20310: "ldr w14, [x12, #80]",
        0x20314: "tbz w11, #31, 0x20324",
        0x20318: "cmp w13, w15",                  # exactly the size
        0x2031c: "ccmp w14, w16, #0x0, eq",
        0x20320: "b.ne 0x2035c",
        0x20324: "cmp w13, w15",                  # at least the size
        0x20328: "ccmp w14, w16, #0x0, cs",
        0x2032c: "b.cc 0x2035c",
        0x20330: "ldr w13, [x12, #72]",           # its refresh rate, rounded
        0x20334: "ldr w14, [x12, #92]",
        0x20338: "mul w13, w13, w14",
        0x2033c: "ldr w14, [x12, #56]",
        0x20340: "mov w17, #0x3e8",
        0x20344: "mul w14, w14, w17",
        0x20348: "add w14, w14, w13, lsr #1",
        0x2034c: "udiv w14, w14, w13",
        0x20350: "and w17, w11, #0xff",
        0x20354: "cmp w14, w17",
        0x20358: "b.cs 0x20364",
        0x2035c: "ldr x12, [x12]",
        0x20360: "b 0x20304",
        0x20364: "ldr x0, [x19]",                 # add the record
        0x20368: "adr x9, 0x10dec",
        0x2036c: "ldr w10, [x9]",
        0x20370: "sbfx x10, x10, #0, #26",
        0x20374: "add x9, x9, x10, lsl #2",
        0x20378: "blr x9",                        # drm_mode_create
        0x2037c: "cbz x0, 0x203f0",
        0x20380: "mov x25, x0",
        0x20384: "ldp x9, x10, [x21]",
        0x20388: "stp x9, x10, [x25, #16]",
        0x2038c: "ldp x9, x10, [x21, #16]",
        0x20390: "stp x9, x10, [x25, #56]",
        0x20394: "ldp x9, x10, [x21, #32]",
        0x20398: "stp x9, x10, [x25, #72]",
        0x2039c: "ldp x9, x10, [x21, #48]",
        0x203a0: "stp x9, x10, [x25, #88]",
        0x203a4: "ldp w9, w10, [x21, #64]",
        0x203a8: "str w9, [x25, #180]",
        0x203ac: "str w10, [x25, #52]",
        0x203b0: "mov x0, x19",
        0x203b4: "mov x1, x25",
        0x203b8: "adr x9, 0x10ce8",
        0x203bc: "ldr w10, [x9]",
        0x203c0: "sbfx x10, x10, #0, #26",
        0x203c4: "add x9, x9, x10, lsl #2",
        0x203c8: "blr x9",                        # drm_mode_probed_add
        0x203cc: "add w22, w22, #0x1",
        0x203d0: "cbnz x23, 0x203f0",
        0x203d4: "ldr x23, [x25]",                # the first add: the list head,
        0x203d8: "mov x12, x23",
        0x203dc: "ldr x12, [x12]",                # and every mode's aspect ratio cleared
        0x203e0: "cmp x12, x23",
        0x203e4: "b.eq 0x203f0",
        0x203e8: "str wzr, [x12, #188]",
        0x203ec: "b 0x203dc",
        0x203f0: "add x21, x21, #0x50",           # the next record
        0x203f4: "subs w24, w24, #0x1",
        0x203f8: "b.ne 0x202e4",
        0x203fc: "mov w19, w22",
        0x20400: "ldp x21, x22, [sp]",
        0x20404: "ldp x23, x24, [sp, #16]",
        0x20408: "ldr x25, [sp, #32]",
        0x2040c: "add sp, sp, #0x30",
        0x20410: "b 0x8c80",                      # kfree(edid), return
    },
    "MODE_FILTER": {
        0x2497c: "adr x11, 0x227a0",
        0x24980: "mov w12, #0x4",                 # NREC
        0x24984: "ldp x13, x14, [x19, #56]",      # each record: the mode's twelve words
        0x24988: "ldp x15, x16, [x11, #16]",
        0x2498c: "cmp x13, x15",
        0x24990: "ccmp x14, x16, #0x0, eq",
        0x24994: "b.ne 0x249d0",
        0x24998: "ldp x13, x14, [x19, #72]",
        0x2499c: "ldp x15, x16, [x11, #32]",
        0x249a0: "cmp x13, x15",
        0x249a4: "ccmp x14, x16, #0x0, eq",
        0x249a8: "b.ne 0x249d0",
        0x249ac: "ldp x13, x14, [x19, #88]",
        0x249b0: "ldp x15, x16, [x11, #48]",
        0x249b4: "cmp x13, x15",
        0x249b8: "ccmp x14, x16, #0x0, eq",
        0x249bc: "b.ne 0x249d0",
        0x249c0: "ldp w13, w14, [x11, #64]",      # a match: its vrefresh and type
        0x249c4: "str w13, [x19, #180]",
        0x249c8: "str w14, [x19, #52]",
        0x249cc: "b 0x8db8",                      # the link and width checks
        0x249d0: "add x11, x11, #0x50",
        0x249d4: "subs w12, w12, #0x1",
        0x249d8: "b.ne 0x24984",
        0x249dc: "b 0x8dd8",                      # none: MODE_CLOCK_HIGH
    },
    "WIDE_PATCHES": {
        0xb380: "cmp w8, #0x780",                 # P2: wider than 1920 is bypass
        0xb384: "b.ls 0xb3a4",
        0xb388: "nop",
        0xb38c: "nop",
        0xb390: "nop",
        0xaef0: "ldr x8, [x21, #1240]",           # FLIP: planes[0].state
        0xaef4: "mov w9, #0x7",
        0xaef8: "strb w9, [x8, #152]",            # its layer.index = 7
        0xaefc: "nop",
    },
    "WIDE_MP_PATCHES": {
        0x90f0: "strb w8, [x20, #3461]",          # pixel mode = bypass flag
        0x90f4: "cbz w8, 0x913c",
        0x90f8: "ldr x8, [x22, #128]",
        0x90fc: "ldr x8, [x8, #1240]",
        0x9100: "ldr x8, [x8, #16]",
        0x9104: "cbz x8, 0x913c",
        0x9108: "ldr x8, [x8, #72]",
        0x910c: "ldr w10, [x8]",
        0x9110: "mov w8, #0x1",
        0x9114: "mov w9, #0x564e",
        0x9118: "movk w9, #0x3231, lsl #16",
        0x911c: "cmp w10, w9",
        0x9120: "b.eq 0x9140",
        0x9124: "mov w9, #0x3050",
        0x9128: "movk w9, #0x3031, lsl #16",
        0x912c: "cmp w10, w9",
        0x9130: "b.ne 0x913c",
        0x9134: "mov w9, #0xa",
        0x9138: "b 0x9144",
        0x913c: "mov w8, wzr",
        0x9140: "mov w9, #0x8",
        0x372f8: "mov w8, #0xc",                  # TH40: small blanking, 12
        0x372fc: "b 0x37318",
        0x37300: "mov w8, #0x28",                 # dual pixel mode, 40
        0x37304: "b 0x37318",
        0x37308: "nop",
        0x3730c: "ldrb w9, [x19, #3461]",         # large blanking: the pixel mode
        0x37310: "mov w8, #0x10",                 # 16
        0x37314: "cbnz w9, 0x37300",
    },
}
# (all_modes, wide, wide_mp): every combination patches() tells apart.
VARIANTS = [(False, False, False), (False, True, True), (True, False, False), (True, True, False),
            (True, True, True)]
STRB_PIXEL_MODE = 0x39361688     # strb w8, [x20, #3461], at 0x90f0 with wide_mp
CBZ_BYPASS = 0x34000288          # cbz w8, 0x9140, the stock word at 0x90f0
# The single words: the width check, the stub's two words, the words all_modes swaps
# for the mode table, fix C's old words, and the record counts without WIDE.
DISASSEMBLY_WORDS = {
    (0x8e2c, B_HS_WIDE): "b.cs 0x8dd8",
    (0x8d90, LDR_TYPE): "ldr w8, [x19, #52]",
    (0x8d90, B_SAFE_START): "b 0x202b4",
    (0x8d94, TBZ_NOT_PREFERRED): "tbz w8, #3, 0x8e54",
    (0x8d94, sprd_drm.NOP): "nop",
    (0x8ce8, B_LINK): "b 0x8db8",
    (0x8ce8, B_MODE_FILTER): "b 0x2497c",
    (0x8e3c, B_MODE_OK): "b 0x8e54",
    (0x202d0, NREC3[0x202d0]): "mov w24, #0x3",
    (0x24980, NREC3[0x24980]): "mov w12, #0x3",
}
# sha256 of the real module patched without all_modes (recovery's, patch_vendor_boot.py), and,
# with uhd and y420 off (SprdDrm pins them off), with all_modes but neither wide nor wide_mp (the
# mode table's three records: MOD-M3), with wide but not wide_mp (P2, FLIP and four records), and
# with all three (MOD-M, out/dp-evidence/test42/modm/: the default until test 42 shipped 4K60
# 4:2:0; SprdDrmUhdY420 pins today's default, MOD-Y1-E).
# Those four with fix E (the defaults), and without it (irq_mask=False: the modules test 42 ran).
PLAIN_SHA256 = "c7e8c8295939b6a91904239186778af2e5ed266d385807bb1be1d9c633162ea2"
ALL_MODES_SHA256 = "f21cf6f62d48ccc0d3124010d1bbe9694fcf420e78be3ee36eb94de684ff5e28"
WIDE_NO_MP_SHA256 = "cb1c11e1e70991c99d14c43de4d3d57e977d4b1e65fca60b26c8dfb49bc44197"
WIDE_SHA256 = "ed94c442ba9830e9d01d8541f1657a2e04fbdd5ffd187f375c223382cac4346a"
PLAIN_NOMASK_SHA256 = "b86f53b442a585eb18d8ee4eb3a8e31b91da7d7a6c92154cd478b816d6fc8981"
ALL_MODES_NOMASK_SHA256 = "a8d78a649834097f43c1380ea13302e548309049f5a68f565d468903f73c714f"
WIDE_NO_MP_NOMASK_SHA256 = "0adccca5a553736b2ee2d53a922c1dd133ced70295ec6d7f2a2ff285443f97b6"
WIDE_NOMASK_SHA256 = "8648b53331ca5801bc04c175dc1f2b5b999052d9140fb5c8e0bb77622c81b8c3"
# Test 42's MOD-Q without its diagnostic background
# (out/dp-evidence/test42/modq/sprd-drm-MOD-Q-nobg.ko): the default before the mode
# table, with fix C. 2560x1440 at 60 Hz showed a correct, stable picture.
MOD_Q_NOBG_SHA256 = "b12699c6bf19c7d0646485d54a49305d212f78f751910132c145d2fbacaf07f2"
# The module test 42 shipped first (MOD-P, out/dp-evidence/test42/sprd-drm-MOD-P.ko): the
# default before FLIP stored layer 7 (it only nopped the planes[7] load and store), before
# TH40 and before the mode table. A locked 2560x1440 signal, a black picture.
MOD_P_SHA256 = "07c4bb84c27f05fb0ae75b5139a1c970dd6434246e0fb3718e02c5525f33fb47"
# The module 1.4.4 test 41 shipped: all_modes, before the IRQ guard, the mode-change
# and the safe-start patches, with b.hi at 0x8e2c.
TEST41_SHA256 = "489154a1964a890dc682ed8bd5c87cf618e55d005188d1b32390277e2df3a92c"


class SprdDrm(util.TestCase):
    """The patch without the 4K switches: UHD and Y420 are pinned off here, so "the default" in
    these tests is MOD-M's family. The shipped default (UHD and Y420 on) is SprdDrmUhdY420's."""

    def setUp(self):
        super().setUp()
        pin = mock.patch.multiple(sprd_drm, UHD=False, Y420=False)
        pin.start()
        self.addCleanup(pin.stop)

    def test_patch_table(self):
        for all_modes, wide, wide_mp in VARIANTS:
            table = sprd_drm.patches(all_modes, wide, wide_mp)
            # MP takes the 0x90f4 patch's place: wide_mp adds one entry, TH40.
            self.assertEqual(len(table), len(sprd_drm.PATCHES) + len(sprd_drm.HPD_PATCHES)
                             + len(sprd_drm.IRQ_GUARD_PATCHES) + len(sprd_drm.IRQ_MASK_PATCHES) - 1
                             + (len(sprd_drm.ALL_MODES_PATCHES) if all_modes else 0)
                             + (len(sprd_drm.WIDE_PATCHES) if all_modes and wide else 0)
                             + (len(sprd_drm.WIDE_MP_PATCHES) - 1 if all_modes and wide and wide_mp else 0))
            sites = [(addr, addr + 4 * len(old)) for addr, old, _ in table]
            self.assertEqual(sites, sorted(sites))
            for (_, end), (start, _) in zip(sites, sites[1:]):
                self.assertLessEqual(end, start, "patch sites overlap")
            for addr, old, new in table:
                self.assertEqual(len(old), len(new), hex(addr))
                self.assertEqual(addr % 4, 0)
                self.assertNotEqual(old, new, hex(addr))
        # patches(True) leaves the table itself as it is.
        self.assertEqual(dict((a, n) for a, _, n in sprd_drm.PATCHES)[0x8d8c][1:3], [LDR_TYPE, TBZ_NOT_PREFERRED])
        self.assertEqual(dict((a, n) for a, _, n in sprd_drm.PATCHES)[0x8cdc][3], B_LINK)
        self.assertEqual(dict((a, n) for a, _, n in sprd_drm.PATCHES)[0x8e3c][0], LDR_TYPE)
        self.assertEqual(sprd_drm.ALL_MODES_WORDS, [(0x8cdc, 3, B_LINK, B_MODE_FILTER),
                                                    (0x8e3c, 0, LDR_TYPE, B_MODE_OK)])
        self.assertEqual(sprd_drm.NREC_WORDS, [(0x202b4, 7, 0x52800098, NREC3[0x202d0]),
                                               (0x2497c, 1, 0x5280008c, NREC3[0x24980])])
        self.assertEqual(sprd_drm.MODES_PATCHES, sprd_drm.GET_MODES_HOOKS + [sprd_drm.GET_MODES, sprd_drm.MODE_TABLE,
                                                                             sprd_drm.MODE_FILTER])
        self.assertEqual(sprd_drm.ALL_MODES_PATCHES, sprd_drm.MODE_CHANGE_PATCHES + sprd_drm.MODES_PATCHES)
        # Fix C is gone.
        self.assertFalse(hasattr(sprd_drm, "SAFE_START_PATCHES") or hasattr(sprd_drm, "SAFE_START")
                         or hasattr(sprd_drm, "ALL_MODES"))
        self.assertEqual(sprd_drm.WIDE_PATCHES, [sprd_drm.P2, sprd_drm.FLIP])
        self.assertEqual(sprd_drm.WIDE_MP_PATCHES, [sprd_drm.MP, sprd_drm.TH40])

    def test_wide_table(self):
        """Which table wide and wide_mp give: P2, FLIP and the mode table's fourth record with
        all_modes and wide, MP instead of the 0x90f4 patch and TH40 with wide_mp too, nothing of
        them without all_modes."""
        sites = (0x90f0, 0x90f4, 0xaef0, 0xb380, 0x372f8)
        test41 = sprd_drm.patches(True, False, False)
        self.assertEqual([a for a, _, _ in test41 if a in sites], [0x90f4])
        # Without all_modes neither switch does anything; wide_mp needs wide.
        for wide in (None, False, True):
            for wide_mp in (None, False, True):
                self.assertEqual(sprd_drm.patches(False, wide, wide_mp), sprd_drm.patches(False, False, False))
        self.assertEqual(sprd_drm.patches(True, False, True), test41)
        # Without wide the mode table has three records, with it four (the 2560x1440 one).
        counts = {0x202d0: 0x52800098, 0x24980: 0x5280008c}         # mov w24, #4; mov w12, #4
        self.assertEqual({a: w for a, w in words(test41).items() if a in counts}, NREC3)
        four = [(a, o, [counts.get(a + 4 * i, w) for i, w in enumerate(n)]) for a, o, n in test41]
        # wide without wide_mp: that table and P2 and FLIP, the 0x90f4 patch kept.
        wide = sprd_drm.patches(True, True, False)
        self.assertEqual(wide, sorted(four + sprd_drm.WIDE_PATCHES))
        self.assertEqual({a: w for a, w in words(wide).items() if a in counts}, counts)
        self.assertEqual([a for a, _, _ in wide if a in sites], [0x90f4, 0xaef0, 0xb380])
        # wide_mp: MP at 0x90f0 instead of the 0x90f4 patch, and TH40.
        every = sprd_drm.patches(True, True, True)
        self.assertEqual(every, sorted([t for t in wide if t[0] != 0x90f4] + [sprd_drm.MP, sprd_drm.TH40]))
        self.assertEqual([a for a, _, _ in every if a in sites], [0x90f0, 0xaef0, 0xb380, 0x372f8])
        self.assertIn(sprd_drm.MP, every)
        self.assertIn(sprd_drm.TH40, every)
        # Both default on, and the defaults follow the switches.
        self.assertEqual((sprd_drm.WIDE, sprd_drm.WIDE_MP), (True, True))
        self.assertEqual(sprd_drm.patches(True), every)
        with mock.patch.object(sprd_drm, "WIDE_MP", False):
            self.assertEqual(sprd_drm.patches(True), wide)
        with mock.patch.object(sprd_drm, "WIDE", False):
            self.assertEqual(sprd_drm.patches(True), test41)
        # An explicit argument wins over a switch.
        with mock.patch.object(sprd_drm, "WIDE", False), mock.patch.object(sprd_drm, "WIDE_MP", False):
            self.assertEqual(sprd_drm.patches(True, True, True), every)

    def test_mp_words(self):
        """MP covers the stock cbz on the bypass flag (0x90f0) and the 0x90f4 patch's original
        words; after its strb and cbz it is the 0x90f4 patch's code one word up, without its nop."""
        addr, old, new = sprd_drm.MP
        patch = [t for t in sprd_drm.PATCHES if t[0] == 0x90f4][0]
        self.assertEqual(addr, 0x90f0)
        self.assertEqual(old, [CBZ_BYPASS] + patch[1])
        self.assertEqual(len(new), len(old))
        self.assertEqual(new[0], STRB_PIXEL_MODE)
        self.assertEqual(a64.disassemble(0x90f0, CBZ_BYPASS), "cbz w8, 0x9140")
        # The same instructions, branches to the same places, as the 0x90f4 patch's.
        before = [a64.disassemble(0x90f4 + 4 * i, w) for i, w in enumerate(patch[2])]
        after = [a64.disassemble(0x90f0 + 4 * i, w) for i, w in enumerate(new)]
        self.assertEqual(before.count("nop"), 1)
        self.assertEqual(after[2:], [t for t in before if t != "nop"])
        # The stock cbz went to 0x9140 with w8 0; MP's goes to 0x913c, which zeroes w8 first.
        self.assertEqual(after[1], "cbz w8, 0x913c")
        # Branch targets: not bypass and no framebuffer to RGB (0x913c), NV12 to 8 bpc (0x9140),
        # P010 past it (0x9144, where the stock code goes on).
        got = {0x90f0 + 4 * i: branch_target(0x90f0 + 4 * i, w) for i, w in enumerate(new)
               if branch_target(0x90f0 + 4 * i, w) is not None}
        self.assertEqual(got, {0x90f4: 0x913c, 0x9104: 0x913c, 0x9120: 0x9140, 0x9130: 0x913c, 0x9138: 0x9144})
        self.assertEqual(sorted(set(got.values())), [0x913c, 0x9140, 0x9144])
        # P2's one branch is to the normal-mode code, as the stock b.ne's were.
        self.assertEqual(branch_target(0xb384, sprd_drm.P2[2][1]), 0xb3a4)
        self.assertEqual({branch_target(0xb384, sprd_drm.P2[1][1]), branch_target(0xb390, sprd_drm.P2[1][4])},
                         {0xb3a4})
        self.assertEqual(sprd_drm.P2[2][2:], [sprd_drm.NOP] * 3)

    def test_flip_words(self):
        """FLIP covers the bypass branch's layer-count check and the planes[7] store, and stores 7
        in planes[0].state's layer index instead, at the same offset in the state."""
        addr, old, new = sprd_drm.FLIP
        self.assertEqual(addr, 0xaef0)
        self.assertEqual([a64.disassemble(addr + 4 * i, w) for i, w in enumerate(old)],
                         ["cmp w2, #0x1", "b.ne 0xb0c8", "ldr x8, [x21, #10816]", "strb wzr, [x8, #152]"])
        got = [a64.disassemble(addr + 4 * i, w) for i, w in enumerate(new)]
        self.assertEqual(got, ["ldr x8, [x21, #1240]", "mov w9, #0x7", "strb w9, [x8, #152]", "nop"])
        # The byte the stock store zeroed (layer.index), through planes[0]'s state: x21 + 1240
        # (0x4d8, as the loop at 0xb048 adds) instead of planes[7]'s, seven planes (0x558) on.
        self.assertEqual(got[2].split(", ", 1)[1], a64.disassemble(0xaefc, old[3]).split(", ", 1)[1])
        self.assertEqual(10816 - 1240, 7 * 0x558)
        # No branch left: the printk at 0xb0c8 is never reached.
        self.assertEqual([branch_target(addr + 4 * i, w) for i, w in enumerate(new)], [None] * 4)

    def test_th40_words(self):
        """TH40 keeps dptx_video_ts_calculate's entries (0x372f8 falling through, small blanking;
        0x3730c, large blanking) and its exit (0x37318, then the store at 0x37328): 12 on the
        small path, 40 on the large one in dual pixel mode, 16 otherwise."""
        addr, old, new = sprd_drm.TH40
        self.assertEqual(addr, 0x372f8)
        end = addr + 4 * len(old)
        self.assertEqual(end, 0x37318)
        # The stock code: pixel mode 2 (quad) gives 4 on the small path, else 12; the large path 16.
        self.assertEqual([a64.disassemble(addr + 4 * i, w) for i, w in enumerate(old)],
                         ["ldrb w8, [x19, #3461]", "cmp w8, #0x2", "b.ne 0x37314", "mov w8, #0x4",
                          "b 0x37318", "mov w8, #0x10", "b 0x37318", "mov w8, #0xc"])
        got = {addr + 4 * i: branch_target(addr + 4 * i, w) for i, w in enumerate(new)
               if branch_target(addr + 4 * i, w) is not None}
        self.assertEqual(got, {0x372fc: 0x37318, 0x37304: 0x37318, 0x37314: 0x37300})

        def run(pc, pixel_mode):
            """w8 at the exit, from PC, with PIXEL_MODE at dptx+3461 (tests/a64.py's text)."""
            regs = {}
            for _ in range(len(new) + 1):
                if pc == end:
                    return regs["w8"]
                self.assertTrue(addr <= pc < end, hex(pc))
                op, _, args = a64.disassemble(pc, new[(pc - addr) // 4]).partition(" ")
                args = args.split(", ")
                pc += 4
                if op == "mov":
                    regs[args[0]] = int(args[1][1:], 16)
                elif op == "ldrb" and args[1:] == ["[x19", "#3461]"]:
                    regs[args[0]] = pixel_mode
                elif op == "b":
                    pc = int(args[0], 16)
                elif op == "cbnz":
                    pc = int(args[1], 16) if regs[args[0]] else pc
                else:
                    self.assertEqual(op, "nop")
            self.fail("no exit")
        for pixel_mode in (0, 1, 2):
            self.assertEqual(run(0x372f8, pixel_mode), 12)
            self.assertEqual(run(0x3730c, pixel_mode), 40 if pixel_mode else 16)

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

    def test_new_branch_targets(self):
        """Every branch of the IRQ guard, the mode-change, the mode table, P2, MP and TH40 goes where it should."""
        guard_b = {
            0x87ec: 0x329b0, 0x329d4: 0x87f0,                   # bind: mask Type-C, glb->disable, back
            0x89f4: 0x3292c, 0x32938: 0x89f8,                   # pd_event on: glb->enable, flag, back
            0x8a94: 0x32968, 0x32990: 0x8ac4, 0x32994: 0x8aa4,  # pd_event off: to its glb->disable
            0x9218: 0x923c,                                     # encoder_disable: no glb->disable
            0x9324: 0x32958, 0x32964: 0x9328,                   # encoder_enable: glb->enable, flag, back
            0x37b50: 0x328f4, 0x32914: 0x3291c, 0x32920: 0x37b54,   # dptx_irq: guard, then its own code
            0x328f8: 0x32924, 0x32900: 0x32924, 0x32910: 0x32924,   # not clocked: IRQ_NONE
            0x3752c: 0x32850, 0x32874: 0x37530,                 # dptx_threaded_irq: guard, its own code
            0x32854: 0x32878, 0x3285c: 0x32878, 0x3286c: 0x32878,   # not clocked:
            0x32890: 0x375e8,                                   # its unplug path, no registers
        }
        # Fix E: pd_event off's cave is a word longer; encoder_disable and _enable's caves.
        guard = {a: t for a, t in guard_b.items() if a not in (0x32990, 0x32994)}
        guard.update({
            0x32994: 0x8ac4, 0x32998: 0x8aa4,                   # pd_event off: to its glb->disable
            0x9208: 0x328c0, 0x328d0: 0x920c,                   # encoder_disable: IEN = 0, back
            0x9550: 0x329f0, 0x32a0c: 0x9554,                   # encoder_enable: IEN back, back
        })
        mode_change = {
            0x2614: 0x61a4, 0x61ac: 0x2644, 0x61b0: 0x2618,     # enable: not DP skips, DP calls it
            0x26ac: 0x61d8, 0x61e0: 0x2720, 0x61e4: 0x26b0,     # disable: likewise
        }
        modes = {
            0x8c7c: 0x202bc, 0x8ca0: 0x202b4,                   # get_modes: EDID, no EDID
            0x202d4: 0x202dc,                                   # an EDID: every record
            0x202e8: 0x20364, 0x202ec: 0x20364,                 # no gate, no EDID: add it
            0x202f0: 0x203f0, 0x20308: 0x203f0,                 # no list yet, no mode opens it: skip
            0x20314: 0x20324,                                   # at least the size
            0x20320: 0x2035c, 0x2032c: 0x2035c,                 # not the size: the next mode
            0x20358: 0x20364,                                   # fast enough: add it
            0x20360: 0x20304,                                   # the next mode
            0x2037c: 0x203f0,                                   # drm_mode_create failed: skip
            0x203d0: 0x203f0, 0x203e4: 0x203f0, 0x203ec: 0x203dc,   # the aspect ratios, once
            0x203f8: 0x202e4,                                   # the next record
            0x20410: 0x8c80,                                    # kfree(edid), return
            0x8ce8: 0x2497c,                                    # mode_valid, after the cap: the filter
            0x24994: 0x249d0, 0x249a8: 0x249d0, 0x249bc: 0x249d0,   # no match: the next record
            0x249d8: 0x24984,
            0x249cc: 0x8db8,                                    # a match: the link and width checks
            0x249dc: 0x8dd8,                                    # none: MODE_CLOCK_HIGH
            0x8e3c: 0x8e54,                                     # after the width check: MODE_OK
        }
        wide = {
            0xb384: 0xb3a4,                                     # P2: 1920 or narrower, normal mode
        }
        mp = {
            0x90f4: 0x913c, 0x9104: 0x913c, 0x9130: 0x913c,     # not bypass, no framebuffer, not YUV: RGB
            0x9120: 0x9140, 0x9138: 0x9144,                     # NV12: 8 bpc; P010: 10 bpc
            0x372fc: 0x37318, 0x37304: 0x37318,                 # TH40: 12, 40: to the store
            0x37314: 0x37300,                                   # dual pixel mode: 40
        }
        every = {**guard, **mode_change, **modes}
        for variant, want in (((False, True, True), guard), ((True, False, False), every),
                              ((True, True, False), {**every, **wide}), ((True, True, True), {**every, **wide, **mp})):
            all_modes, with_wide, with_mp = variant
            groups = sprd_drm.IRQ_GUARD_PATCHES + sprd_drm.IRQ_MASK_PATCHES \
                + (sprd_drm.ALL_MODES_PATCHES if all_modes else []) \
                + (sprd_drm.WIDE_PATCHES if all_modes and with_wide else []) \
                + (sprd_drm.WIDE_MP_PATCHES if all_modes and with_wide and with_mp else [])
            sites = {addr for addr, _, _ in groups}
            swapped = {0x8ce8, 0x8e3c} if all_modes else set()
            got = {}
            for addr, _, new in sprd_drm.patches(*variant):
                if addr == sprd_drm.MODE_TABLE[0]:          # data, not code
                    continue
                for i, w in enumerate(new):
                    target = branch_target(addr + 4 * i, w)
                    if target is not None and (addr in sites or addr + 4 * i in swapped):
                        got[addr + 4 * i] = target
            self.assertEqual(got, want, variant)
        # Without fix E, fix B's branches as before (the test 42 modules).
        sites = {addr for addr, _, _ in sprd_drm.IRQ_GUARD_PATCHES}
        got = {addr + 4 * i: branch_target(addr + 4 * i, w) for addr, _, new in sprd_drm.patches(False, irq_mask=False)
               if addr in sites for i, w in enumerate(new) if branch_target(addr + 4 * i, w) is not None}
        self.assertEqual(got, guard_b)

    def test_mode_table_addresses(self):
        """The mode table's adr words: the table (from both pieces of code), and the two relocated
        bl words in sprd_panel_get_modes the calls go through."""
        def adr_targets(table):
            out = {}
            for addr, _, new in table:
                if addr == sprd_drm.MODE_TABLE[0]:          # data, not code
                    continue
                for i, w in enumerate(new):
                    text = a64.disassemble(addr + 4 * i, w)
                    if text.startswith("adr "):
                        out[addr + 4 * i] = int(text.split(", ")[1], 16)
            return out
        want = {0x202dc: 0x227a0, 0x20368: 0x10dec, 0x203b8: 0x10ce8, 0x2497c: 0x227a0}
        self.assertEqual(adr_targets(sprd_drm.MODES_PATCHES), want)
        # No other patch has an adr.
        self.assertEqual(adr_targets(sprd_drm.patches(True)), want)
        # The table is aligned for the ldp's, one 80-byte record per mode.
        self.assertEqual((sprd_drm.MODE_TABLE[0] % 16, len(sprd_drm.MODE_TABLE[2])), (0, 20 * len(sprd_drm.MODES)))
        # Each piece fits its function: dpu_init.1506 is 0x18c bytes, .1618 0x1a4, .1724 0x270.
        for (addr, old, _), size in ((sprd_drm.GET_MODES, 0x18c), (sprd_drm.MODE_TABLE, 0x1a4),
                                     (sprd_drm.MODE_FILTER, 0x270)):
            self.assertLessEqual(4 * len(old), size, hex(addr))
        # The calls: the sbfx keeps the bl's 26-bit offset, signed (bits 0-25 of x10).
        self.assertEqual(a64.disassemble(0x20370, sprd_drm.GET_MODES[2][(0x20370 - 0x202b4) // 4]),
                         "sbfx x10, x10, #0, #26")

    def test_disassembly(self):
        """The new words are the instructions their comments say (tests/a64.py's decoding)."""
        for name, want in DISASSEMBLY.items():
            table = getattr(sprd_drm, name)
            table = [table] if isinstance(table[0], int) else table
            got = {a: a64.disassemble(a, w) for a, w in words(table).items()}
            self.assertEqual(got, want, name)
        for (addr, w), want in DISASSEMBLY_WORDS.items():
            self.assertEqual(a64.disassemble(addr, w), want, hex(addr))
        plain, every = words(sprd_drm.patches(False)), words(sprd_drm.patches(True))
        three = words(sprd_drm.patches(True, False, False))
        self.assertEqual((plain[0x8e2c], every[0x8e2c]), (B_HS_WIDE, B_HS_WIDE))
        self.assertEqual((plain[0x8ce8], every[0x8ce8], three[0x8ce8]), (B_LINK, B_MODE_FILTER, B_MODE_FILTER))
        self.assertEqual((plain[0x8e3c], every[0x8e3c], three[0x8e3c]), (LDR_TYPE, B_MODE_OK, B_MODE_OK))
        self.assertEqual({a: every[a] for a in NREC3}, {0x202d0: 0x52800098, 0x24980: 0x5280008c})
        self.assertEqual({a: three[a] for a in NREC3}, NREC3)
        # The stub's words are no longer swapped (and no longer reached) with all_modes.
        self.assertEqual((plain[0x8d90], every[0x8d90]), (LDR_TYPE, LDR_TYPE))
        self.assertEqual((plain[0x8d94], every[0x8d94]), (TBZ_NOT_PREFERRED, TBZ_NOT_PREFERRED))
        self.assertEqual((0x90f0 in plain, every[0x90f0]), (False, STRB_PIXEL_MODE))
        self.assertEqual((0x372f8 in plain, every[0x372f8]), (False, sprd_drm.TH40[2][0]))

    def test_decoder(self):
        """tests/a64.py against objdump's text for words of the vendor module itself."""
        cases = {
            (0x2600, 0xf9433808): "ldr x8, [x0, #1648]",
            (0x2608, 0x39402908): "ldrb w8, [x8, #10]",
            (0x260c, 0x121f0508): "and w8, w8, #0x6",
            (0x2610, 0x7100091f): "cmp w8, #0x2",
            (0x2638, 0x54000122): "b.cs 0x265c",
            (0x267c, 0x17fffff0): "b 0x263c",
            (0x25fc, 0x910003fd): "mov x29, sp",
            (0x25f0, 0xf800865e): "str x30, [x18], #8",
            (0x3cfc, 0xb9000149): "str w9, [x10]",
            (0x89cc, 0xb4000168): "cbz x8, 0x89f8",
            (0x8a58, 0x35000628): "cbnz w8, 0x8b1c",
            (0x8988, 0x52800581): "mov w1, #0x2c",
            (0x1d28, 0x2b10015f): "cmn w10, w16",
            (0x2050, 0x4b0803e8): "neg w8, w8",
            (0x4cb4, 0x32000bea): "orr w10, wzr, #0x7",
            (0x36d4, 0xb21303e8): "mov x8, #0x200000002000",
            (0x8d94, 0x36180608): "tbz w8, #3, 0x8e54",
            (0x8e18, 0x0b0a054f): "add w15, w10, w10, lsl #1",
            # The classes P2, FLIP, MP and TH40 use: b.ls, strb of a register, ldr through x21 and x22.
            (0x79fc, 0x540000e9): "b.ls 0x7a18",
            (0x914c, 0x3936c689): "strb w9, [x20, #3505]",
            (0x35ae8, 0x39761668): "ldrb w8, [x19, #3461]",
            (0xaef8, 0xf95522a8): "ldr x8, [x21, #10816]",
            (0xaefc, 0x3902611f): "strb wzr, [x8, #152]",
            (0x90f4, 0xf94042c8): "ldr x8, [x22, #128]",
            (0xb380, 0x713c011f): "cmp w8, #0xf00",
            # The stock words FLIP and TH40 replace, and the code around them.
            (0xaef0, 0x7100045f): "cmp w2, #0x1",
            (0xaef4, 0x54000ea1): "b.ne 0xb0c8",
            (0xb0dc, 0x17ffff87): "b 0xaef8",
            (0xb048, 0x911362b5): "add x21, x21, #0x4d8",
            (0x372e8, 0x35000134): "cbnz w20, 0x3730c",
            (0x372f4, 0x540000c8): "b.hi 0x3730c",
            (0x37300, 0x540000a1): "b.ne 0x37314",
            (0x37304, 0x52800088): "mov w8, #0x4",
            (0x37314, 0x52800188): "mov w8, #0xc",
            (0x37328, 0x3936da68): "strb w8, [x19, #3510]",
            # The classes the mode table uses: ldp and stp (signed offset), adr, ccmp, and the
            # bitfield moves (lsr, sbfx and their kin).
            (0x8c20, 0xa9014ff4): "stp x20, x19, [sp, #16]",
            (0x8ca8, 0xa9414ff4): "ldp x20, x19, [sp, #16]",
            (0x8d20, 0x29479a65): "ldp w5, w6, [x19, #60]",
            (0x94c0, 0x10000009): "adr x9, 0x94c0",
            (0x5374, 0xfa401924): "ccmp x9, #0x0, #0x4, ne",
            (0xf93c, 0x93427d03): "sbfx x3, x8, #2, #30",
            (0x1a74, 0xd37ffdae): "lsr x14, x13, #63",
            (0x1a78, 0x936cfdad): "asr x13, x13, #44",
            (0x3c0c, 0xd37ef50e): "lsl x14, x8, #2",
            (0x3d20, 0x531c6ea9): "lsl w9, w21, #4",
            (0x41e4, 0x5308212a): "ubfx w10, w9, #8, #1",
            (0x4184, 0x531b0108): "ubfiz w8, w8, #5, #1",
            (0x7f64, 0x937e7c50): "sbfiz x16, x2, #2, #32",
            (0x21e8, 0x93407ed3): "sxtw x19, w22",
            (0x36d14, 0x93401c29): "sxtb x9, w1",
            # Forms it does not know: ldr x30, [x18, #-8]! (pre-index), stp and ldp with
            # writeback, and no instruction.
            (0x2654, 0xf85f8e5e): "? f85f8e5e",
            (0x8c1c, 0xa9be7bfd): "? a9be7bfd",
            (0x8cac, 0xa8c27bfd): "? a8c27bfd",
            (0x0, 0x00000000): "? 00000000",
        }
        for (addr, w), want in cases.items():
            self.assertEqual(a64.disassemble(addr, w), want, hex(w))

    def test_applies_every_patch(self):
        for all_modes, wide, wide_mp in VARIANTS:
            ko, out = patched(all_modes, wide=wide, wide_mp=wide_mp)
            self.assertEqual(len(out), len(ko))
            table = sprd_drm.patches(all_modes, wide, wide_mp)
            for addr, _, new in table:
                off = sprd_drm.TEXT_OFFSET + addr
                self.assertEqual(list(struct.unpack_from("<%dI" % len(new), out, off)), new)
            changed = {i for i in range(len(ko)) if ko[i] != out[i]}
            allowed = {sprd_drm.TEXT_OFFSET + addr + i for addr, old, _ in table for i in range(4 * len(old))}
            self.assertLessEqual(changed, allowed)

    def test_wide_words(self):
        """P2, FLIP, MP and TH40's words land only with all_modes and their switches; otherwise the
        stock words (or the 0x90f4 patch's) stay."""
        p2, flip = (sprd_drm.TEXT_OFFSET + sprd_drm.P2[0], 5), (sprd_drm.TEXT_OFFSET + sprd_drm.FLIP[0], 4)
        fix = [t for t in sprd_drm.PATCHES if t[0] == 0x90f4][0]
        for all_modes, wide, wide_mp in VARIANTS + [(True, False, True), (True, None, None)]:
            _, out = patched(all_modes, wide=wide, wide_mp=wide_mp)
            on = all_modes and wide is not False
            mp = on and wide_mp is not False
            for (off, n), (_, old, new) in ((p2, sprd_drm.P2), (flip, sprd_drm.FLIP)):
                self.assertEqual(list(struct.unpack_from("<%dI" % n, out, off)), new if on else old)
            self.assertEqual(list(struct.unpack_from("<8I", out, sprd_drm.TEXT_OFFSET + sprd_drm.TH40[0])),
                             sprd_drm.TH40[2] if mp else sprd_drm.TH40[1])
            self.assertEqual(word(out, 0x90f0), STRB_PIXEL_MODE if mp else CBZ_BYPASS)
            got = list(struct.unpack_from("<21I", out, sprd_drm.TEXT_OFFSET + 0x90f0))
            self.assertEqual(got, sprd_drm.MP[2] if mp else [CBZ_BYPASS] + fix[2])
            # The word after them is left alone.
            self.assertEqual(word(out, 0x9144), 0x5a5a5a5a)
        _, out = patched(True)
        # cmp w8, #0x780; b.ls 0xb3a4
        self.assertEqual([word(out, 0xb380), word(out, 0xb384)], [0x711e011f, 0x54000109])
        # ldr x8, [x21, #1240]; mov w9, #7; strb w9, [x8, #152]; nop
        self.assertEqual([word(out, a) for a in range(0xaef0, 0xaf00, 4)],
                         [0xf9426ea8, 0x528000e9, 0x39026109, sprd_drm.NOP])
        # mov w8, #12; b; mov w8, #40; b; nop; ldrb w9, [x19, #3461]; mov w8, #16; cbnz w9, 0x37300
        self.assertEqual([word(out, a) for a in range(0x372f8, 0x37318, 4)],
                         [0x52800188, 0x14000007, 0x52800508, 0x14000005, sprd_drm.NOP, 0x39761669,
                          0x52800208, 0x35ffff69])

    def test_new_words(self):
        for all_modes in (False, True):
            _, out = patched(all_modes)
            # eor w8, w8, w8, lsr #1; tbz w8, #0, 0x8b1c
            self.assertEqual([word(out, 0x89b4), word(out, 0x89b8)], [0x4a480508, 0x36000b28])
            # b 0x8db8 (the link check), or b 0x2497c (the mode table's filter, then it)
            self.assertEqual(word(out, 0x8ce8), B_MODE_FILTER if all_modes else B_LINK)
            self.assertEqual(word(out, 0x8db8), 0xf943ca8b)          # ldr x11, [x20, #0x790]
            self.assertEqual(word(out, 0x8e18), 0x0b0a054f)          # add w15, w10, w10, lsl #1
            self.assertEqual(word(out, 0x8e28), 0x713c01ff)          # cmp w15, #0xf00
            self.assertEqual(word(out, 0x8e2c), B_HS_WIDE)           # b.hs 0x8dd8 (3840 or more wide)
            # ldr w8, [x19, #52] then b 0x8cec (the CEA code checks), or b 0x8e54 (MODE_OK)
            self.assertEqual(word(out, 0x8e3c), B_MODE_OK if all_modes else LDR_TYPE)
            self.assertEqual(word(out, 0x8e40), 0x17ffffab)
            self.assertEqual(word(out, 0x8d90), LDR_TYPE)
            self.assertEqual(word(out, 0x8d94), TBZ_NOT_PREFERRED)
            # The IRQ guard, in both: recovery's copy gets it too.
            self.assertEqual(word(out, 0x37b50), 0x17ffeb69)         # b 0x328f4
            self.assertEqual(word(out, 0x328f4), 0xf9401429)         # ldr x9, [x1, #40]
            self.assertEqual(word(out, 0x920c), 0x392e427f)          # strb wzr, [x19, #2960]
            # Fix E, in both too.
            self.assertEqual(word(out, 0x9208), 0x1400a5ae)          # b 0x328c0 (IEN = 0)
            self.assertEqual(word(out, 0x328c4), 0xb90a051f)         # str wzr, [x8, #2564]
            self.assertEqual(word(out, 0x9550), 0x1400a528)          # b 0x329f0 (IEN back)
            self.assertEqual(word(out, 0x32a04), 0xb90d054b)         # str w11, [x10, #3332]
            self.assertEqual(word(out, 0x32978), 0x1b097d6b)         # mul w11, w11, w9
            self.assertEqual(word(out, 0x331fc), 0x2a0a03e8)         # mov w8, w10 (IEN = 0x95)
            # The mode-change patches and the mode table only with all_modes; otherwise the
            # original words stay.
            for addr, old, new in sprd_drm.ALL_MODES_PATCHES:
                self.assertEqual(word(out, addr), new[0] if all_modes else old[0], hex(addr))
            self.assertEqual(word(out, 0x8c7c), 0x14005d90 if all_modes else 0x2a0003f3)   # b 0x202bc
            self.assertEqual(word(out, 0x8ca0), 0x14005d85 if all_modes else 0x2a1f03f3)   # b 0x202b4
        self.assertEqual(sprd_drm.NOP, 0xd503201f)

    def test_unplug_flag_and_threaded_guard(self):
        """pd_event off doesn't clear its flag before glb->disable (the unplug's HPD interrupt
        comes while it runs and must be acked); the vendor's clear after it (0x8adc) is no patch
        site. The threaded handler's guard, without the flags, never loads the register base."""
        for all_modes in (False, True):
            _, out = patched(all_modes)
            table = sprd_drm.patches(all_modes)
            sites = {addr + 4 * i for addr, old, _ in table for i in range(len(old))}
            self.assertNotIn(0x8adc, sites)
            self.assertNotIn(0x375e8, sites)

            def run(pc, end):
                """The words from PC, following unconditional branches, up to the one at END."""
                seen = []
                while pc != end:
                    self.assertIn(pc, sites, hex(pc))
                    w = word(out, pc)
                    seen.append(w)
                    pc = branch_target(pc, w) if w & 0xfc000000 == 0x14000000 else pc + 4
                    self.assertLess(len(seen), 64)
                return seen
            # pd_event off, from after detect(0) to the vendor's glb->disable call (0x8aa4).
            off = run(0x8a94, 0x8aa4)
            self.assertNotIn(0x392ee69f, off)                    # strb wzr, [x20, #3001]
            self.assertNotIn(0x1218796b, off)                    # no IEN read-modify-write any more:
            self.assertIn(0x396e4289, off)                       # the encoder's flag picks
            self.assertIn(0x1b097d6b, off)                       # 0x15 or 0
            self.assertIn(0xb90d054b, off)                       # str w11, [x10, #3332]
            # encoder_disable: IEN = 0 before fix B's flag clear and glb->disable; encoder_enable:
            # the IEN store after timing_set and dpu1_run, the flag set long before (0x32960).
            self.assertEqual(run(0x9208, 0x920c)[2:4], [0xb90a051f, 0xb94a0509])
            self.assertEqual(word(out, 0x920c), 0x392e427f)
            self.assertEqual(run(0x9550, 0x9554)[6], 0xb90d054b)
            self.assertEqual(word(out, 0x32960), 0x392e42a8)
            # The threaded handler: hook, then the not-clocked path to 0x375e8.
            self.assertEqual(word(out, 0x3752c), 0x17ffecc9)
            path = run(0x32878, 0x375e8)
            self.assertFalse([w for w in path if w & 0xffffffe0 == 0xf946b660])   # ldr xN, [x19, #3432]
            self.assertEqual(path[-1], 0x14001356)

    def test_irq_mask_table(self):
        """Fix E is in every variant (recovery's too) unless irq_mask=False; its pd_event cave
        replaces fix B's (same original words and one more, 0x910003fd at 0x32998), and nothing
        else of fix B changes."""
        self.assertTrue(sprd_drm.IRQ_MASK)
        guard_off = [t for t in sprd_drm.IRQ_GUARD_PATCHES if t[0] == 0x32968][0]
        self.assertEqual(sprd_drm.IRQ_MASK_OFF[1], guard_off[1] + [0x910003fd])
        self.assertIn(sprd_drm.IRQ_MASK_OFF, sprd_drm.IRQ_MASK_PATCHES)
        self.assertEqual(sorted(a for a, _, _ in sprd_drm.IRQ_MASK_PATCHES),
                         [0x9208, 0x9550, 0x328c0, 0x32968, 0x329f0, 0x331fc])
        variants = [(False,), (True, False, False), (True, True, False), (True,),
                    (True, True, True, True), (True, True, True, True, True), (True, True, True, True, True, False)]
        for v in variants:
            without = sprd_drm.patches(*v, irq_mask=False)
            self.assertIn(guard_off, without)
            self.assertFalse({a for a, _, _ in without} & {0x9208, 0x9550, 0x328c0, 0x329f0, 0x331fc}, v)
            with_e = sorted([t for t in without if t != guard_off] + sprd_drm.IRQ_MASK_PATCHES)
            self.assertEqual(sprd_drm.patches(*v), with_e, v)
            self.assertEqual(sprd_drm.patches(*v, irq_mask=True), with_e, v)
            with mock.patch.object(sprd_drm, "IRQ_MASK", False):
                self.assertEqual(sprd_drm.patches(*v), without, v)
                self.assertEqual(sprd_drm.patches(*v, irq_mask=True), with_e, v)
        # patch_module passes it on.
        ko, sha = fake_module()
        with mock.patch.object(sprd_drm, "MODULE_SHA256", sha):
            without = sprd_drm.patch_module(ko, False, irq_mask=False)
            self.assertEqual([word(without, a) for a in (0x9208, 0x9550, 0x331fc, 0x32970)],
                             [0xb9000109, 0x392e42a8, 0x2a0a0108, 0xb94d054b])
            self.assertEqual(word(without, 0x32998), 0x910003fd)
            self.assertEqual(word(sprd_drm.patch_module(ko, False), 0x9208), 0x1400a5ae)

    def run_ien(self, out, pc, ends, regs, mem):
        """Runs OUT's words from PC (tests/a64.py's text) to one of ENDS on a model of the memory
        MEM {address: value}; returns the pc, and every load and store [(op, address, value)]."""
        trace = []

        def get(r):
            if r in ("wzr", "xzr"):
                return 0
            return regs[r[1:]] & (0xffffffff if r[0] == "w" else (1 << 64) - 1)

        def put(r, v):
            regs[r[1:]] = v & (0xffffffff if r[0] == "w" else (1 << 64) - 1)
        for _ in range(64):
            if pc in ends:
                return pc, trace
            op, _, rest = a64.disassemble(pc, word(out, pc)).partition(" ")
            args = rest.split(", ")
            pc += 4
            if op == "b":
                pc = int(args[0], 16)
            elif op == "cbz":
                pc = int(args[1], 16) if get(args[0]) == 0 else pc
            elif op == "mov":
                put(args[0], int(args[1][1:], 16) if args[1][0] == "#" else get(args[1]))
            elif op == "orr":
                self.assertTrue(len(args) == 3 or args[3].startswith("lsl #"), rest)
                put(args[0], get(args[1]) | get(args[2]) << (int(args[3][5:]) if len(args) == 4 else 0))
            elif op == "mul":
                put(args[0], get(args[1]) * get(args[2]))
            elif op == "dmb":
                pass
            elif op in ("ldr", "ldrb", "str", "strb"):
                mem_op = rest.split("[")[1].rstrip("]").split(", #")
                addr = get(mem_op[0]) + (int(mem_op[1]) if len(mem_op) > 1 else 0)
                if op.startswith("ldr"):
                    self.assertIn(addr, mem, hex(addr))
                    put(args[0], mem[addr] & (0xff if op == "ldrb" else (1 << 64) - 1))
                    trace.append(("ldr", addr, mem[addr]))
                else:
                    mem[addr] = get(args[0]) & (0xff if op == "strb" else (1 << 64) - 1)
                    trace.append(("str", addr, mem[addr]))
            else:
                self.fail("0x%x: %s" % (pc - 4, op))
        self.fail("no end")

    def test_irq_mask_values(self):
        """What fix E writes to IEN (the dptx registers + 0xd04), run on a model of struct sprd_dp:
        0 in encoder_disable, read back before fix B's flag clear; 0x15 | pd_event << 7 at
        encoder_enable's end, before the vendor's flag store; 0x15 or 0 (encoder on or off) in
        pd_event off, read back, then its glb->disable as before; 0x95 in dptx_core_init."""
        dp, dptx, base, glb, disable = 0x10000, 0x20000, 0x30000, 0x40000, 0xf00d0
        ien, enc, pd = base + 0xd04, dp + 2960, dp + 3001

        def memory(enc_on, pd_on, glb_disable=disable):
            return {dp + 2984: dptx, dptx + 3432: base, dp + 2840: glb, glb + 16: glb_disable,
                    enc: enc_on, pd: pd_on, ien: 0xffffffff}
        for all_modes in (False, True):
            _, out = patched(all_modes)
            # encoder_disable: x19 the sprd_dp, x8 base + 0x300, w9 the vendor's value for it.
            for pd_on in (0, 1):
                pc, trace = self.run_ien(out, 0x9208, {0x921c, 0x923c},
                                         {"19": dp, "8": base + 0x300, "9": 0x1234}, memory(1, pd_on))
                self.assertEqual(pc, 0x921c)
                self.assertEqual([t for t in trace if t[0] == "str"],
                                 [("str", base + 0x300, 0x1234), ("str", ien, 0), ("str", enc, 0)])
                self.assertEqual(trace[2], ("ldr", ien, 0))
            # encoder_enable's end: x21 the sprd_dp, w8 = 1 (0x954c), the flag set at 0x32960.
            for pd_on in (0, 1):
                pc, trace = self.run_ien(out, 0x9550, {0x9554}, {"21": dp, "8": 1}, memory(1, pd_on))
                self.assertEqual(trace, [("ldr", dp + 2984, dptx), ("ldr", dptx + 3432, base),
                                         ("ldr", pd, pd_on), ("str", ien, 0x15 | pd_on << 7), ("str", enc, 1)])
            # pd_event off: x20 the sprd_dp; its flag stays set (0x8adc clears it after glb->disable).
            for enc_on in (0, 1):
                mem = memory(enc_on, 1)
                pc, trace = self.run_ien(out, 0x8a94, {0x8aa4, 0x8ac4}, {"20": dp}, mem)
                self.assertEqual((pc, trace[-1]), (0x8aa4, ("ldr", glb + 16, disable)))
                self.assertEqual([t for t in trace if t[0] == "str"], [("str", ien, 0x15 if enc_on else 0)])
                self.assertEqual(trace[4], ("ldr", ien, 0x15 if enc_on else 0))
                self.assertEqual((mem[pd], mem[enc]), (1, enc_on))
            # No glb->disable: past the call, as the vendor's cbz did.
            pc, _ = self.run_ien(out, 0x8a94, {0x8aa4, 0x8ac4}, {"20": dp}, memory(0, 1, 0))
            self.assertEqual(pc, 0x8ac4)
            # dptx_core_init: IEN = w10 (0x95), no longer w8 (its reset value) | w10.
            self.assertEqual(a64.disassemble(0x331fc, word(out, 0x331fc)), "mov w8, w10")

    def test_variants_differ_only_in_the_all_modes_patches(self):
        def differ(a, b):
            return {(i - sprd_drm.TEXT_OFFSET) & ~3 for i in range(len(a)) if a[i] != b[i]}
        _, plain = patched(False)
        _, every = patched(True, wide=False, wide_mp=False)
        got = differ(plain, every)
        self.assertLessEqual(got, {0x8ce8, 0x8e3c} | set(words(sprd_drm.ALL_MODES_PATCHES)))
        self.assertLessEqual({0x8ce8, 0x8e3c} | {addr for addr, _, _ in sprd_drm.ALL_MODES_PATCHES}, got)
        # Without all_modes, wide and wide_mp change nothing.
        self.assertEqual(patched(False, wide=True, wide_mp=True)[1], plain)
        # wide adds P2 and FLIP and the mode table's fourth record (its two counts), and
        # nothing else.
        _, wide = patched(True, wide=True, wide_mp=False)
        self.assertEqual(differ(every, wide), set(words(sprd_drm.WIDE_PATCHES)) | set(NREC3))
        # wide_mp then changes only MP's and TH40's words: the strb at 0x90f0, the 0x90f4 code
        # moved up, and the threshold.
        _, mp = patched(True)
        got = differ(wide, mp)
        self.assertLessEqual(got, set(words(sprd_drm.WIDE_MP_PATCHES)))
        self.assertEqual(got, {a for a, w in words(sprd_drm.WIDE_MP_PATCHES).items() if word(wide, a) != w})
        self.assertIn(0x90f0, got)
        self.assertLessEqual(set(words([sprd_drm.TH40])), got)

    def test_all_modes_from_the_environment(self):
        for value, nop in ((None, False), ("1", True), ("0", False), ("true", False), ("", False)):
            env = {} if value is None else {"THIMSY_DP_ALL_MODES": value}
            _, out = patched(None, env)
            self.assertEqual(word(out, 0x8ce8), B_MODE_FILTER if nop else B_LINK, value)
            self.assertEqual(word(out, 0x8c7c), 0x14005d90 if nop else 0x2a0003f3, value)
            self.assertEqual(word(out, 0x2614), 0x5401dc80 if nop else 0x54000180, value)
            # The wide words follow all_modes, wherever it comes from.
            self.assertEqual(word(out, 0x90f0), STRB_PIXEL_MODE if nop else CBZ_BYPASS, value)
            self.assertEqual(word(out, 0xb380), 0x711e011f if nop else 0x713c011f, value)
            self.assertEqual(word(out, 0xaef0), 0xf9426ea8 if nop else 0x7100045f, value)
            self.assertEqual(word(out, 0x372f8), 0x52800188 if nop else 0x39761668, value)
            # An explicit all_modes wins over the environment.
            _, out = patched(False, env)
            self.assertEqual(word(out, 0x8ce8), B_LINK, value)
            self.assertEqual(word(out, 0x90f0), CBZ_BYPASS, value)

    def test_wrong_module(self):
        with self.assertRaises(SystemExit) as e:
            sprd_drm.patch_module(b"not the module")
        self.assertIn("not the 1.2 module", str(e.exception))
        # One changed bit in an original word: the 1.2 patches', the link check's, the HPD patch's,
        # the words all_modes replaces, the width check's and the IRQ guard's.
        for addr, byte in ((sprd_drm.PATCHES[2][0], 0), (0x89b4, 4), (0x8db8, 4 * 7), (0x8e3c, 4),
                           (0x8d8c, 4 * 2), (0x8d8c, 4), (0x8e14, 4 * 6), (0x37b50, 0), (0x328f4, 4 * 8),
                           (0x32968, 4 * 12), (0x920c, 4 * 3), (0x87ec, 3), (0x9208, 0), (0x328c0, 4 * 4),
                           (0x9550, 2), (0x329f0, 4 * 7), (0x331fc, 1),
                           (0x32850, 4 * 16), (0x3752c, 0)):
            for all_modes in (False, True):
                ko, _ = fake_module()
                ko = bytearray(ko)
                ko[sprd_drm.TEXT_OFFSET + addr + byte] ^= 1
                with mock.patch.object(sprd_drm, "MODULE_SHA256", hashlib.sha256(ko).hexdigest()):
                    with self.assertRaises(SystemExit) as e:
                        sprd_drm.patch_module(bytes(ko), all_modes)
                self.assertIn("unexpected code at 0x%x" % addr, str(e.exception))
        # P2's, FLIP's, TH40's and the stock cbz MP covers matter only with all_modes and their switch;
        # a changed word of the 0x90f4 patch is reported at MP's address when MP replaces it.
        # (address, byte, where the error is reported with (wide on, wide_mp on), None for no error)
        for addr, byte, reported in ((0xb380, 4 * 4, lambda on, mp: 0xb380 if on else None),
                                     (0xaef0, 4 * 3, lambda on, mp: 0xaef0 if on else None),
                                     (0x372f8, 4 * 7, lambda on, mp: 0x372f8 if mp else None),
                                     (0x90f0, 0, lambda on, mp: 0x90f0 if mp else None),
                                     (0x90f4, 4 * 19, lambda on, mp: 0x90f0 if mp else 0x90f4)):
            for all_modes, wide, wide_mp in VARIANTS:
                where = reported(all_modes and wide, all_modes and wide and wide_mp)
                ko, _ = fake_module()
                ko = bytearray(ko)
                ko[sprd_drm.TEXT_OFFSET + addr + byte] ^= 1
                with mock.patch.object(sprd_drm, "MODULE_SHA256", hashlib.sha256(ko).hexdigest()):
                    if where is None:
                        out = sprd_drm.patch_module(bytes(ko), all_modes, wide, wide_mp)
                        off = sprd_drm.TEXT_OFFSET + addr + byte
                        self.assertEqual(out[off], ko[off])
                        continue
                    with self.assertRaises(SystemExit) as e:
                        sprd_drm.patch_module(bytes(ko), all_modes, wide, wide_mp)
                self.assertIn("unexpected code at 0x%x" % where, str(e.exception),
                              (hex(addr), all_modes, wide, wide_mp))
        # The mode-change patches' and the mode table's original words matter only with all_modes.
        for addr, byte in ((0x2614, 0), (0x61d8, 4 * 3), (0x8c7c, 0), (0x8ca0, 3), (0x202b4, 4 * 87),
                           (0x227a0, 4 * 79 + 3), (0x2497c, 4 * 24)):
            ko, _ = fake_module()
            ko = bytearray(ko)
            ko[sprd_drm.TEXT_OFFSET + addr + byte] ^= 1
            with mock.patch.object(sprd_drm, "MODULE_SHA256", hashlib.sha256(ko).hexdigest()):
                out = sprd_drm.patch_module(bytes(ko), False)
                self.assertEqual(out[sprd_drm.TEXT_OFFSET + addr + byte], ko[sprd_drm.TEXT_OFFSET + addr + byte])
                with self.assertRaises(SystemExit) as e:
                    sprd_drm.patch_module(bytes(ko), True)
            self.assertIn("unexpected code at 0x%x" % addr, str(e.exception))

    def test_refuses_its_own_output(self):
        """A patched module is not the module the patch is for: patching twice stops."""
        for all_modes in (False, True):
            ko, out = patched(all_modes)
            with mock.patch.object(sprd_drm, "MODULE_SHA256", hashlib.sha256(ko).hexdigest()):
                with self.assertRaises(SystemExit) as e:
                    sprd_drm.patch_module(out, all_modes)
            self.assertIn("not the 1.2 module", str(e.exception))

    def test_all_modes_needs_its_words(self):
        """A table whose 0x8cdc patch lacks the b 0x8db8, or whose 0x8e3c patch lacks the ldr,
        refuses all_modes rather than replace another word; likewise the mode table's record
        counts without wide."""
        for site, index in ((0x8cdc, 3), (0x8e3c, 0)):
            table = [(addr, old, list(new)) for addr, old, new in sprd_drm.PATCHES]
            dict((addr, new) for addr, _, new in table)[site][index] = 0x12345678
            with mock.patch.object(sprd_drm, "PATCHES", table):
                self.assertEqual(len(sprd_drm.patches(False)),
                                 len(table) + len(sprd_drm.HPD_PATCHES) + len(sprd_drm.IRQ_GUARD_PATCHES)
                                 + len(sprd_drm.IRQ_MASK_PATCHES) - 1)
                with self.assertRaises(SystemExit) as e:
                    sprd_drm.patches(True)
            self.assertIn("no all_modes word %d in the 0x%x patch" % (index, site), str(e.exception))
        for site, index in ((0x202b4, 7), (0x2497c, 1)):
            table = [(addr, old, list(new)) for addr, old, new in sprd_drm.ALL_MODES_PATCHES]
            dict((addr, new) for addr, _, new in table)[site][index] = 0x12345678
            with mock.patch.object(sprd_drm, "ALL_MODES_PATCHES", table):
                self.assertEqual(words(sprd_drm.patches(True, True))[site + 4 * index], 0x12345678)
                with self.assertRaises(SystemExit) as e:
                    sprd_drm.patches(True, False)
            self.assertIn("no all_modes word %d in the 0x%x patch" % (index, site), str(e.exception))

    def test_real_module(self):
        """Every variant of the real module, when the build's copy of it is at hand."""
        path = os.path.join(util.ROOT, "out", ".dock", "work", "dpmodes-kernel", "sprd-drm.orig.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        ko = util.slurp(path)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_drm.MODULE_SHA256)
        want = {(False, False, False): PLAIN_SHA256, (False, True, True): PLAIN_SHA256,
                (True, False, False): ALL_MODES_SHA256, (True, False, True): ALL_MODES_SHA256,
                (True, True, False): WIDE_NO_MP_SHA256, (True, True, True): WIDE_SHA256,
                (False, None, None): PLAIN_SHA256, (True, None, None): WIDE_SHA256}
        got = {v: hashlib.sha256(sprd_drm.patch_module(ko, *v)).hexdigest() for v in want}
        self.assertEqual(got, want)
        nomask = {PLAIN_SHA256: PLAIN_NOMASK_SHA256, ALL_MODES_SHA256: ALL_MODES_NOMASK_SHA256,
                  WIDE_NO_MP_SHA256: WIDE_NO_MP_NOMASK_SHA256, WIDE_SHA256: WIDE_NOMASK_SHA256}
        got = {v: hashlib.sha256(sprd_drm.patch_module(ko, *v, irq_mask=False)).hexdigest() for v in want}
        self.assertEqual(got, {v: nomask[h] for v, h in want.items()})

    def test_stock_from_test41(self):
        """The stock module rebuilt from the one 1.4.4 test 41 shipped (its patches reversed with
        the original words) is the 1.2 module, patching it gives the pinned modules, and test 41's
        differs from the new all_modes one only in the new words."""
        path = os.path.join(util.ROOT, "out", "dp-evidence", "sprd-drm-test41.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        test41 = util.slurp(path)
        self.assertEqual(hashlib.sha256(test41).hexdigest(), TEST41_SHA256)
        stock = bytearray(test41)
        for addr, old, _ in sprd_drm.patches(True, False, False):
            struct.pack_into("<%dI" % len(old), stock, sprd_drm.TEXT_OFFSET + addr, *old)
        stock = bytes(stock)
        self.assertEqual(hashlib.sha256(stock).hexdigest(), sprd_drm.MODULE_SHA256)
        plain = sprd_drm.patch_module(stock, False, irq_mask=False)
        every = sprd_drm.patch_module(stock, True, False, False, irq_mask=False)
        self.assertEqual(hashlib.sha256(plain).hexdigest(), PLAIN_NOMASK_SHA256)
        self.assertEqual(hashlib.sha256(every).hexdigest(), ALL_MODES_NOMASK_SHA256)
        self.assertEqual(hashlib.sha256(sprd_drm.patch_module(stock, False)).hexdigest(), PLAIN_SHA256)
        new = words(sprd_drm.IRQ_GUARD_PATCHES + sprd_drm.ALL_MODES_PATCHES)
        # The words all_modes swaps for the mode table (and its counts without wide), the width
        # check, and the stub's tbz, which test 41's all_modes made a nop.
        new.update(NREC3)
        new.update({0x8ce8: B_MODE_FILTER, 0x8e3c: B_MODE_OK, 0x8e2c: B_HS_WIDE, 0x8d94: TBZ_NOT_PREFERRED})
        differ = {}
        for i in range(0, len(test41), 4):
            if test41[i:i + 4] != every[i:i + 4]:
                differ[i - sprd_drm.TEXT_OFFSET] = struct.unpack_from("<I", every, i)[0]
        # Every new word that is not already the stock word there (three are), and nothing else.
        self.assertEqual(differ, {a: w for a, w in new.items() if word(test41, a) != w})
        self.assertEqual(sorted(a for a, w in new.items() if word(stock, a) == w), [0x9214, 0x9218, 0x329c0])
        for ko in (plain, every):
            # pd_event off: glb->disable (blr at 0x8ac0), then the vendor's flag clear.
            self.assertEqual((word(ko, 0x8ac0), word(ko, 0x8adc)), (0xd63f0100, 0x392ee69f))
            # Where the threaded guard goes without the flags: the vendor unplug path from the
            # redriver on (mov w0, wzr; str wzr, [x19, #3548]; strb wzr, [x23]; strb wzr, [x23, #11];
            # b 0x3787c), then extcon and DRM (w2 from w22), then the unlock: no register base load.
            self.assertEqual([word(ko, a) for a in range(0x375ec, 0x37604, 4)],
                             [0x2a1f03e0, 0x94000000, 0xb90dde7f, 0x390002ff, 0x39002eff, 0x1400009f])
            self.assertEqual(word(ko, 0x37880), 0x530822c2)    # ubfx w2, w22, #8, #1
            self.assertEqual(word(ko, 0x3789c), 0x34000368)    # cbz w8, 0x37908 ([3548] clear)
            for a in list(range(0x375e8, 0x37604, 4)) + list(range(0x3787c, 0x378a0, 4)) + \
                    list(range(0x37908, 0x3795c, 4)):
                self.assertNotEqual(word(ko, a) & 0xffffffe0, 0xf946b660, hex(a))

    def test_stock_from_test42(self):
        """The stock module rebuilt from the one test 42 showed 2560x1440 with (MOD-Q without
        its diagnostic background: all_modes, wide and wide_mp, with fix C) is the 1.2 module.
        The default now differs from it only in fix C's and the mode table's words: P2's, FLIP's,
        MP's and TH40's are the same; and without wide it differs from the default only in
        those, and the mode table's counts."""
        path = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "modq", "sprd-drm-MOD-Q-nobg.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        test42 = util.slurp(path)
        self.assertEqual(hashlib.sha256(test42).hexdigest(), MOD_Q_NOBG_SHA256)
        stock = bytearray(test42)
        for addr, old, _ in sprd_drm.patches(True, True, True):
            struct.pack_into("<%dI" % len(old), stock, sprd_drm.TEXT_OFFSET + addr, *old)
        stock = bytes(stock)
        self.assertEqual(hashlib.sha256(stock).hexdigest(), sprd_drm.MODULE_SHA256)
        default = sprd_drm.patch_module(stock, True, irq_mask=False)
        self.assertEqual(hashlib.sha256(default).hexdigest(), WIDE_NOMASK_SHA256)
        differ = {i - sprd_drm.TEXT_OFFSET for i in range(0, len(test42), 4) if test42[i:i + 4] != default[i:i + 4]}
        self.assertLessEqual(differ, MODES_SITES)
        # Fix E on top changes its own words only.
        with_e = sprd_drm.patch_module(stock, True)
        self.assertEqual(hashlib.sha256(with_e).hexdigest(), WIDE_SHA256)
        self.assertEqual({i - sprd_drm.TEXT_OFFSET for i in range(0, len(test42), 4)
                          if with_e[i:i + 4] != default[i:i + 4]}, IRQ_MASK_SITES - SAME_AS_B)
        self.assertLessEqual({0x8c7c, 0x8ca0, 0x8ce8, 0x8e3c, 0x8d90, 0x8d94}, differ)
        # MOD-Q had fix C: the stub went to 0x202b4 first, and its tbz was a nop.
        self.assertEqual([word(test42, a) for a in (0x8d90, 0x8d94, 0x8ce8, 0x8e3c)],
                         [B_SAFE_START, sprd_drm.NOP, B_LINK, LDR_TYPE])
        every = sprd_drm.patch_module(stock, True, False, False, irq_mask=False)
        new = words(sprd_drm.WIDE_PATCHES + sprd_drm.WIDE_MP_PATCHES)
        differ = {}
        for i in range(0, len(default), 4):
            if default[i:i + 4] != every[i:i + 4]:
                differ[i - sprd_drm.TEXT_OFFSET] = struct.unpack_from("<I", default, i)[0]
        new.update({0x202d0: 0x52800098, 0x24980: 0x5280008c})
        self.assertEqual(differ, {a: w for a, w in new.items() if word(every, a) != w})
        # MP's words that are the 0x90f4 patch's already: its last two (mov w8, wzr; mov w9, #8).
        self.assertEqual(sorted(a for a, w in new.items() if word(every, a) == w), [0x913c, 0x9140])
        # The stock code around them: hactive in w8 before P2 (ldr w8, [x20, #40]), the bypass
        # flag in w8 before MP (ldrb w8, [x23, #1372]), the code MP's branches go back to.
        self.assertEqual(word(stock, 0xb37c), 0xb9402a88)
        self.assertEqual(word(stock, 0x90ec), 0x395572e8)
        self.assertEqual(word(stock, 0x9144), 0x912d82a1)    # add x1, x21, #0xb60
        # Around FLIP: the layer count in w2 (and w2, w20, #0xff), the planes loop's first state
        # at x21 + 0x4d8, the count printk going back to the planes[7] store (now FLIP's strb).
        self.assertEqual(word(stock, 0xaeec), 0x12001e82)
        self.assertEqual(word(stock, 0xb048), 0x911362b5)    # add x21, x21, #0x4d8
        self.assertEqual(word(stock, 0xb0dc), 0x17ffff87)    # b 0xaef8
        # Around TH40: its entries (cbnz w20 and b.hi to 0x3730c, falling through to 0x372f8)
        # and the store of the threshold after the exit.
        self.assertEqual([word(stock, a) for a in (0x372e8, 0x372f4)], [0x35000134, 0x540000c8])
        self.assertEqual(word(stock, 0x37328), 0x3936da68)   # strb w8, [x19, #3510]

    def test_mod_p(self):
        """MOD-P, the module test 42 first shipped (a locked 2560x1440 signal, no picture), is
        the default table with the old FLIP (the planes[7] load and store nopped, the layer-count
        check kept), without TH40 and with fix C instead of the mode table: it differs from the
        default only there."""
        path = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "sprd-drm-MOD-P.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        mod_p = util.slurp(path)
        self.assertEqual(hashlib.sha256(mod_p).hexdigest(), MOD_P_SHA256)
        stock = bytearray(mod_p)
        for addr, old, _ in sprd_drm.patches(True):
            struct.pack_into("<%dI" % len(old), stock, sprd_drm.TEXT_OFFSET + addr, *old)
        stock = bytes(stock)
        self.assertEqual(hashlib.sha256(stock).hexdigest(), sprd_drm.MODULE_SHA256)
        every = sprd_drm.patch_module(stock, True, irq_mask=False)
        differ = {i - sprd_drm.TEXT_OFFSET for i in range(0, len(mod_p), 4) if mod_p[i:i + 4] != every[i:i + 4]}
        new = words([sprd_drm.FLIP, sprd_drm.TH40])
        self.assertEqual(differ - MODES_SITES, {a for a, w in new.items() if word(mod_p, a) != w})
        self.assertEqual([word(mod_p, a) for a in (0x8d90, 0x8d94)], [B_SAFE_START, sprd_drm.NOP])
        # The one word they share: the nop at 0xaefc.
        self.assertEqual(sorted(set(new) - differ), [0xaefc])
        self.assertEqual([word(mod_p, a) for a in range(0xaef0, 0xaf00, 4)],
                         sprd_drm.FLIP[1][:2] + [sprd_drm.NOP, sprd_drm.NOP])
        self.assertEqual([word(mod_p, a) for a in range(0x372f8, 0x37318, 4)], sprd_drm.TH40[1])

    def test_irq_mask_in_the_real_module(self):
        """In the real module: the stock code around fix E's hooks is what its comments say, and
        no alternative or static-key (jump label) site is a fix B or E word, so nothing rewrites
        them at load or run time (the readl alternatives in the dead functions, 0x3284c, 0x328bc,
        0x32918, 0x32954, 0x329ac and 0x329ec, stay unpatched and unrun)."""
        path = os.path.join(util.ROOT, "out", ".dock", "work", "dpmodes-kernel", "sprd-drm.orig.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        ko = util.slurp(path)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_drm.MODULE_SHA256)
        text_of = {a: a64.disassemble(a, word(ko, a)) for a in (
            0x9200, 0x9204, 0x9208, 0x921c, 0x92e8, 0x950c, 0x9524, 0x954c, 0x9550, 0x9558,
            0x331d8, 0x331dc, 0x331e0, 0x331fc, 0x33200, 0x33204, 0x32998)}
        self.assertEqual(text_of, {
            0x9200: "and w9, w9, #0xffffffdf",         # encoder_disable: the stream's 0x300 bit off
            0x9204: "add x8, x8, #0x300",
            0x9208: "str w9, [x8]",
            0x921c: "? 90000009",                      # adrp x9 (not in a64.py)
            0x92e8: "sub x21, x19, #0x3a8",            # encoder_enable: x21 the struct sprd_dp
            0x950c: "b 0x9554",                        # "already enabled": past the flag store
            0x9524: "ldp x21, x20, [sp]",
            0x954c: "mov w8, #0x1",
            0x9550: "strb w8, [x21, #2960]",
            0x9558: "? f85f83a8",                      # ldur x8, [x29, #-8] (not in a64.py)
            0x331d8: "mov w10, #0x95",                 # dptx_core_init: IEN |= 0x95
            0x331dc: "add x8, x8, #0xd04",
            0x331e0: "ldr w8, [x8]",
            0x331fc: "orr w8, w8, w10",
            0x33200: "add x9, x9, #0xd04",
            0x33204: "str w8, [x9]",
            0x32998: "mov x29, sp",                    # dptx_soft_reset_all's, never run
        })
        # 0x921c and 0x9554 load x9 afresh (adrp), so w9 and x9 are dead where fix E's caves go back.
        self.assertEqual(word(ko, 0x921c) & 0x9f00001f, 0x90000009)
        self.assertEqual(word(ko, 0x9554) & 0x9f00001f, 0x90000009)
        shoff, = struct.unpack_from("<Q", ko, 0x28)
        shnum, shstrndx = struct.unpack_from("<HH", ko, 0x3c)
        raw = [struct.unpack_from("<IIQQQQIIQQ", ko, shoff + 64 * i) for i in range(shnum)]
        name = [ko[raw[shstrndx][4] + s[0]:ko.index(b"\0", raw[shstrndx][4] + s[0])].decode() for s in raw]
        text, symtab = name.index(".text"), name.index(".symtab")
        targets = {}
        for s in raw:
            if s[1] == 4 and name[s[7]] in ("__jump_table", ".altinstructions"):
                for j in range(s[5] // 24):
                    _, info, addend = struct.unpack_from("<QQq", ko, s[4] + 24 * j)
                    _, _, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", ko, raw[symtab][4] + 24 * (info >> 32))
                    if shndx == text:
                        targets.setdefault(name[s[7]], set()).add((value + addend) & ~3)
        self.assertEqual(sorted(a for a in targets[".altinstructions"] if 0x32814 <= a < 0x32a24),
                         [0x3284c, 0x328bc, 0x32918, 0x32954, 0x329ac, 0x329ec])
        patched_words = set()
        for v in ((False,), (True,), (True, True, True, True, True), (True, True, True, True, True, False)):
            for addr, old, _ in sprd_drm.patches(*v):
                patched_words |= {addr + 4 * i for i in range(len(old))}
        self.assertLessEqual(IRQ_MASK_SITES, patched_words)
        for section, sites in targets.items():
            self.assertFalse(sorted(hex(a) for a in patched_words & sites), section)

    def test_mode_table_records(self):
        """The table as data: four 80-byte records, the modes test42_plan.md 2.3 lists, each one's
        vrefresh DRM's rounded rate, each clock 297 MHz over a whole number."""
        table = sprd_drm.MODE_TABLE[2]
        self.assertEqual(len(table), 80)
        got = []
        for i in range(4):
            r = table[20 * i:20 * i + 20]
            name = struct.pack("<4I", *r[:4]).rstrip(b"\0").decode()
            clock, hd, hss, hse, ht, hskew, vd, vss, vse, vt, vscan, flags = r[4:16]
            vrefresh, type_, size, rate = r[16:20]
            self.assertEqual((hskew, vscan), (0, 0))
            self.assertEqual(round(clock * 1000 / (ht * vt)), vrefresh, name)
            self.assertEqual(297000 % clock, 0, name)
            got.append((name, clock, (hd, hss, hse, ht), (vd, vss, vse, vt), flags, vrefresh, type_,
                        (size & 0xffff, size >> 16, rate & 0xff, bool(rate >> 31)) if size else (size, rate)))
        self.assertEqual(got, [
            ("1280x720", 74250, (1280, 1390, 1430, 1650), (720, 725, 730, 750), 0x5, 60, 0x60, (0, 0)),
            ("1920x1080", 148500, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5, 60, 0x68,
             (1920, 1080, 24, False)),
            ("1920x1080", 297000, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5, 120, 0x60,
             (1920, 1080, 119, True)),
            # Test 42's y1c: +hsync -vsync (DRM flags 0x9), 59.934 Hz.
            ("2560x1440", 297000, (2560, 2608, 2640, 3346), (1440, 1443, 1448, 1481), 0x9, 60, 0x60,
             (2560, 1440, 59, True)),
        ])
        self.assertAlmostEqual(297000000 / (3346 * 1481), 59.934, places=3)
        # Only 1080p60 is preferred (type bit 3); all are driver (0x40) and user-defined (0x20).
        self.assertEqual([t for _, _, _, _, _, _, t, _ in got if t & 8], [0x68])
        # The words as test42-research/modes/words.txt has them, but for R3's htotal and vtotal
        # (its 3300x1500 became y1c's 3346x1481).
        self.assertEqual(table[60:80], [0x30363532, 0x34343178, 0x30, 0, 0x48828, 0xa00, 0xa30, 0xa50, 0xd12, 0,
                                        0x5a0, 0x5a3, 0x5a8, 0x5c9, 0, 9, 0x3c, 0x60, 0x05a00a00, 0x8000003b])
        self.assertEqual(table[:20], [0x30383231, 0x30323778, 0, 0, 0x1220a, 0x500, 0x56e, 0x596, 0x672, 0,
                                      0x2d0, 0x2d5, 0x2da, 0x2ee, 0, 5, 0x3c, 0x60, 0, 0])

    def test_mode_table_code(self):
        """GET_MODES and MODE_FILTER run on a model of the connector (tests/a64.py's text, as the
        TH40 test does): which records each EDID's modes let in, with four records and with three,
        the count get_modes returns, the aspect ratios cleared, the registers and stack given back;
        and which modes mode_valid keeps, with the table's vrefresh and type."""
        conn, head, dev, sp = 0x100000, 0x100800, 0xdead0000, 0x300000
        create, probed_add = 0x7000000, 0x7000100        # where the loader's bl words go
        bl = {0x10dec: create, 0x10ce8: probed_add}
        table = sprd_drm.MODE_TABLE[0]
        records = [sprd_drm.MODE_TABLE[2][20 * i:20 * i + 20] for i in range(4)]

        class Machine:
            """The patched module's code and table, a connector with its probed-mode list, and
            drm_mode_create and drm_mode_probed_add."""
            def __init__(m, out, create_fails=False):
                m.out, m.mem, m.modes, m.create_fails = out, {}, [], create_fails
                m.store(conn, 8, dev)
                m.store(head, 8, head)
                m.store(head + 8, 8, head)

            def load(m, a, n):
                if table <= a < table + 320:
                    return int.from_bytes(m.out[sprd_drm.TEXT_OFFSET + a:sprd_drm.TEXT_OFFSET + a + n], "little")
                if a in bl and n == 4:                      # bl, its offset filled in
                    return 0x94000000 | ((bl[a] - a) // 4 & 0x3ffffff)
                self.assertTrue(all(a + i in m.mem for i in range(n)), hex(a))
                return int.from_bytes(bytes(m.mem[a + i] for i in range(n)), "little")

            def store(m, a, n, v):
                self.assertFalse(table <= a < table + 320 or a in bl, hex(a))
                for i, b in enumerate((v & ((1 << 8 * n) - 1)).to_bytes(n, "little")):
                    m.mem[a + i] = b

            def new_mode(m):
                a = 0x200000 + 0x100 * len(m.modes)
                m.modes.append(a)
                for i in range(0xc8):
                    m.mem[a + i] = 0
                return a

            def add_tail(m, a):
                prev = m.load(head + 8, 8)
                m.store(a, 8, head)
                m.store(a + 8, 8, prev)
                m.store(prev, 8, a)
                m.store(head + 8, 8, a)

            def listed(m):
                out, a = [], m.load(head, 8)
                while a != head:
                    out.append(a)
                    a = m.load(a, 8)
                return out

            def timing(m, a):
                return [m.load(a + off, 4) for off in range(56, 104, 4)]

            def run(m, pc, regs, ends):
                """Steps from PC until one of ENDS; returns the pc and the registers."""
                flags = (0, 0, 0)                           # z, c, (n and v unused by the code)

                def get(r):
                    if r in ("wzr", "xzr"):
                        return 0
                    v = regs.get(r if r == "sp" else "x" + r[1:], 0)
                    return v & 0xffffffff if r[0] == "w" else v

                def put(r, v):
                    if r not in ("wzr", "xzr"):
                        regs[r if r == "sp" else "x" + r[1:]] = v & (0xffffffff if r[0] == "w" else (1 << 64) - 1)

                def compare(a, b, bits):
                    mask = (1 << bits) - 1
                    return (int((a - b) & mask == 0), int(a & mask >= b & mask), 0)

                def holds(c):
                    z, cy, _ = flags
                    return {"eq": z, "ne": not z, "cs": cy, "cc": not cy}[c]

                def imm(t):
                    return int(t[1:], 0)

                for _ in range(20000):
                    if pc in ends:
                        return pc, regs
                    if pc in (create, probed_add):
                        if pc == create:                    # drm_mode_create(dev): a zeroed mode
                            self.assertEqual(regs["x0"], dev)
                            regs["x0"] = 0 if m.create_fails else m.new_mode()
                        else:                               # drm_mode_probed_add(connector, mode)
                            self.assertEqual(regs["x0"], conn)
                            m.add_tail(regs["x1"])
                        for r in list(range(1, 19)):        # what a call may change (not x18)
                            if r != 18:
                                regs["x%d" % r] = 0x5a5a5a5a5a5a5a5a
                        pc = regs["x30"]
                        continue
                    self.assertTrue(0x202b4 <= pc < 0x20414 or 0x2497c <= pc < 0x249e0, hex(pc))
                    text = a64.disassemble(pc, word(m.out, pc))
                    op, _, rest = text.partition(" ")
                    operands, _, addr = rest.partition("[")
                    args = [t.strip() for t in operands.split(",") if t.strip()]
                    if addr:
                        base, _, off = addr.rstrip("]").partition(", #")
                        addr = get(base) + int(off or 0)
                    pc += 4
                    bits = 64 if args and args[0][0] in "xs" else 32
                    if op == "mov":
                        put(args[0], imm(args[1]) if args[1][0] == "#" else get(args[1]))
                    elif op in ("add", "sub", "subs"):
                        b = imm(args[2]) if args[2][0] == "#" else get(args[2])
                        if len(args) == 4:                  # a shifted register
                            kind, amount = args[3].split(" #")
                            b = b << int(amount) if kind == "lsl" else b >> int(amount)
                        if op == "subs":
                            flags = compare(get(args[1]), b, bits)
                        put(args[0], get(args[1]) + b if op == "add" else get(args[1]) - b)
                    elif op == "cmp":
                        flags = compare(get(args[0]), get(args[1]), bits)
                    elif op == "ccmp":
                        nzcv = imm(args[2])
                        flags = compare(get(args[0]), get(args[1]), bits) if holds(args[3]) else \
                            (nzcv >> 2 & 1, nzcv >> 1 & 1, 0)
                    elif op == "and":
                        put(args[0], get(args[1]) & imm(args[2]))
                    elif op == "lsr":
                        put(args[0], get(args[1]) >> imm(args[2]))
                    elif op == "sbfx":
                        lsb, width = imm(args[2]), imm(args[3])
                        v = get(args[1]) >> lsb & ((1 << width) - 1)
                        put(args[0], v - (1 << width) if v >> (width - 1) else v)
                    elif op == "mul":
                        put(args[0], get(args[1]) * get(args[2]))
                    elif op == "udiv":
                        put(args[0], get(args[1]) // get(args[2]) if get(args[2]) else 0)
                    elif op == "adr":
                        put(args[0], int(args[1], 16))
                    elif op in ("ldr", "ldp"):
                        n = 8 if args[0][0] == "x" else 4
                        for i, r in enumerate(args):
                            put(r, m.load(addr + n * i, n))
                    elif op in ("str", "stp"):
                        n = 8 if args[0][0] == "x" else 4
                        for i, r in enumerate(args):
                            m.store(addr + n * i, n, get(r))
                    elif op == "blr":
                        pc, regs["x30"] = get(args[0]), pc
                        self.assertIn(pc, (create, probed_add))
                    elif op == "b":
                        pc = int(args[0], 16)
                    elif op.startswith("b."):
                        pc = int(args[0], 16) if holds(op[2:]) else pc
                    elif op in ("cbz", "cbnz"):
                        pc = int(args[1], 16) if (get(args[0]) == 0) == (op == "cbz") else pc
                    elif op == "tbz":
                        pc = int(args[2], 16) if not get(args[0]) >> imm(args[1]) & 1 else pc
                    else:
                        self.fail("%s at 0x%x" % (text, pc - 4))
                self.fail("no end")

        # EDID modes: clock, h (active, sync start, sync end, total), v likewise, flags.
        cea720p60 = (74250, (1280, 1390, 1430, 1650), (720, 725, 730, 750), 0x5)
        cea1080p60 = (148500, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5)
        cea1080p120 = (297000, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5)
        cvt1080p60 = (138500, (1920, 1968, 2000, 2080), (1080, 1083, 1088, 1111), 0x9)   # the H27S17's own
        fhd100 = (226500, (1920, 1968, 2000, 2080), (1080, 1083, 1088, 1089), 0x9)
        fhd119 = (296703, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5)      # 119.88 Hz
        uw60 = (198000, (2560, 2608, 2640, 2720), (1080, 1083, 1093, 1111), 0x9)         # 2560x1080
        qhd60 = (241500, (2560, 2608, 2640, 2720), (1440, 1443, 1448, 1481), 0x9)        # 59.95 Hz
        qhd30 = (120750, (2560, 2608, 2640, 2720), (1440, 1443, 1448, 1481), 0x9)
        uwqhd60 = (319750, (3440, 3488, 3520, 3600), (1440, 1443, 1453, 1481), 0x9)     # 3440x1440
        uhd30 = (297000, (3840, 4016, 4104, 4400), (2160, 2168, 2178, 2250), 0x5)
        y1c = (297000, (2560, 2608, 2640, 3346), (1440, 1443, 1448, 1481), 0x9)          # R3's own timing
        cases = [
            # (the EDID's modes, None for no EDID; the records added with four records, with three)
            ("H27S17", [cea1080p60, cvt1080p60, qhd60, cea1080p120, uhd30], [0, 1, 2, 3], [0, 1, 2]),
            ("1080p60 only", [cvt1080p60], [0, 1], [0, 1]),
            ("720p only", [cea720p60], [0], [0]),
            ("2560x1080", [uw60], [0, 1], [0, 1]),
            ("3440x1440", [uwqhd60], [0, 1], [0, 1]),
            ("1440p at 30 Hz", [qhd30], [0, 1], [0, 1]),
            ("1080p at 100 Hz", [fhd100], [0, 1], [0, 1]),
            ("1080p at 119.88 Hz", [fhd119], [0, 1, 2], [0, 1, 2]),
            ("1440p60 alone", [qhd60], [0, 1, 3], [0, 1]),
            ("y1c by override", [y1c], [0, 1, 3], [0, 1]),
            ("an EDID without modes", [], [0], [0]),
            ("no EDID", None, [0, 1], [0, 1]),
        ]
        saved = {"x%d" % r: 0x1000 + r for r in range(21, 26)}
        for name, edid, four, three in cases:
            for wide, want in ((True, four), (False, three)):
                m = Machine(patched(True, wide=wide, wide_mp=False)[1])
                before = []
                for clock, h, v, flags in edid or []:
                    a = m.new_mode()
                    for off, value in zip(range(56, 104, 4), [clock, *h, 0, *v, 0, flags]):
                        m.store(a + off, 4, value)
                    m.store(a + 188, 4, 2)                  # HDMI_PICTURE_ASPECT_16_9, as a CEA mode has
                    m.add_tail(a)
                    before.append(a)
                # From the hook (after drm_add_edid_modes, w0 its count; or after "get edid
                # failed", x20 left over) to 0x8c80: kfree(x20), return w19.
                hook = 0x8c7c if edid is not None else 0x8ca0
                regs = {"x0": len(before), "x19": conn, "x20": 0xedd0000 if edid is not None else 0x77,
                        "sp": sp, "x30": 0x8c18, **saved}
                pc, regs = m.run(branch_target(hook, word(m.out, hook)), regs, {0x8c80})
                added = [[r[4:16] for r in records].index(m.timing(a)) for a in m.listed()[len(before):]]
                self.assertEqual(added, want, (name, wide))
                self.assertEqual(m.listed()[:len(before)], before, name)
                self.assertEqual(regs["x19"] & 0xffffffff, len(before) + len(want), (name, wide))
                self.assertEqual(regs["x20"], 0xedd0000 if edid is not None else 0, name)
                self.assertEqual(({r: regs[r] for r in saved}, regs["sp"]), (saved, sp), name)
                # Every mode's aspect ratio is none, so the EDID's CEA 1080p60 and 1080p120 merge
                # with the table's; each added mode is its record: name, timing, vrefresh, type.
                self.assertEqual({m.load(a + 188, 4) for a in m.listed()}, {0}, name)
                for a, i in zip(m.listed()[len(before):], want):
                    r = records[i]
                    self.assertEqual([m.load(a + 16 + 4 * j, 4) for j in range(8)], r[:4] + [0] * 4)
                    self.assertEqual(m.timing(a), r[4:16])
                    self.assertEqual((m.load(a + 180, 4), m.load(a + 52, 4)), (r[16], r[17]))

                # mode_valid, from 0x8ce8 (after the cap), with every mode on the list: only the
                # table's go on to the link check, with its vrefresh and type; the rest are
                # MODE_CLOCK_HIGH and left as they are.
                self.assertEqual(branch_target(0x8ce8, word(m.out, 0x8ce8)), 0x2497c)
                for a in m.listed():
                    m.store(a + 180, 4, 0x77)
                    m.store(a + 52, 4, 0x48)                # driver | preferred
                    pc, regs = m.run(0x2497c, {"x19": a, "x20": conn, "x10": m.load(a + 56, 4), **saved},
                                     {0x8db8, 0x8dd8})
                    match = [r for r in records[:4 if wide else 3] if r[4:16] == m.timing(a)]
                    self.assertEqual(pc, 0x8db8 if match else 0x8dd8, (name, wide, m.timing(a)))
                    self.assertEqual((m.load(a + 180, 4), m.load(a + 52, 4)),
                                     (match[0][16], match[0][17]) if match else (0x77, 0x48))
                    self.assertEqual((regs["x10"], regs["x19"], regs["x20"]), (m.load(a + 56, 4), a, conn))
                    self.assertEqual({r: regs[r] for r in saved}, saved)
        # drm_mode_create failing: nothing is added, the count stays, and it still returns.
        m = Machine(patched(True)[1], create_fails=True)
        a = m.new_mode()
        m.add_tail(a)
        pc, regs = m.run(0x202bc, {"x0": 1, "x19": conn, "x20": 0xedd0000, "sp": sp, "x30": 0x8c18}, {0x8c80})
        self.assertEqual((m.listed(), regs["x19"], regs["sp"]), ([a], 1, sp))

    def test_relocations(self):
        """In the real module: no relocation applies to a word any variant patches; the words at
        0x10dec and 0x10ce8 are bl's the loader points at drm_mode_create and drm_mode_probed_add;
        nothing refers to the mode table's three pieces but the CFI jump-table slots of the
        sharkl3, sharkl5 and sharkl5pro display controllers' core ops, and no branch outside
        them goes in."""
        path = os.path.join(util.ROOT, "out", ".dock", "work", "dpmodes-kernel", "sprd-drm.orig.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        ko = util.slurp(path)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_drm.MODULE_SHA256)
        shoff, = struct.unpack_from("<Q", ko, 0x28)
        shnum, shstrndx = struct.unpack_from("<HH", ko, 0x3c)
        raw = [struct.unpack_from("<IIQQQQIIQQ", ko, shoff + 64 * i) for i in range(shnum)]

        def cstr(off):
            return ko[off:ko.index(b"\0", off)].decode()
        # (name, type, file offset, size, link, info) of every section
        secs = [(cstr(raw[shstrndx][4] + s[0]), s[1], s[4], s[5], s[6], s[7]) for s in raw]
        text = [i for i, s in enumerate(secs) if s[0] == ".text"][0]
        rodata = [i for i, s in enumerate(secs) if s[0] == ".rodata"][0]
        self.assertEqual(secs[text][2], sprd_drm.TEXT_OFFSET)
        symtab = [s for s in secs if s[1] == 2][0]
        syms = [struct.unpack_from("<IBBHQQ", ko, symtab[2] + 24 * i) for i in range(symtab[3] // 24)]
        names = secs[symtab[4]][2]
        relocs = []     # (section it applies to, offset, type, symbol name, symbol's section, value + addend)
        for s in secs:
            if s[1] == 4:                                   # SHT_RELA
                for j in range(s[3] // 24):
                    off, info, addend = struct.unpack_from("<QQq", ko, s[2] + 24 * j)
                    sym = syms[info >> 32]
                    relocs.append((s[5], off, info & 0xffffffff, cstr(names + sym[0]), sym[3], sym[4] + addend))
        in_text = {off: (kind, name) for sec, off, kind, name, _, _ in relocs if sec == text}
        for variant in VARIANTS:
            for addr, old, _ in sprd_drm.patches(*variant):
                for i in range(len(old)):
                    self.assertNotIn(addr + 4 * i, in_text, (hex(addr + 4 * i), variant))
        call26 = 283                                        # R_AARCH64_CALL26
        self.assertEqual((word(ko, 0x10dec), in_text[0x10dec]), (0x94000000, (call26, "drm_mode_create")))
        self.assertEqual((word(ko, 0x10ce8), in_text[0x10ce8]), (0x94000000, (call26, "drm_mode_probed_add")))
        caves = [(addr, addr + 4 * len(old)) for addr, old, _ in (sprd_drm.GET_MODES, sprd_drm.MODE_TABLE,
                                                                     sprd_drm.MODE_FILTER)]
        self.assertEqual(caves, [(0x202b4, 0x20414), (0x227a0, 0x228e0), (0x2497c, 0x249e0)])
        refs = sorted((secs[sec][0], off, value) for sec, off, _, _, symsec, value in relocs
                      if symsec == text and any(a <= value < b for a, b in caves))
        self.assertEqual(refs, [(".text", 0x3f2b0, 0x202b4), (".text", 0x3f2b4, 0x227a0),
                                (".text", 0x3f2b8, 0x2497c)])
        # Each slot is the init of one core ops, which only sharkl3's, sharkl5's and sharkl5pro's
        # match data use; the Cube's device tree has none of them.
        users = sorted((value, secs[sec][0], off) for sec, off, _, _, symsec, value in relocs
                       if symsec == text and value in (0x3f2b0, 0x3f2b4, 0x3f2b8))
        self.assertEqual(users, [(0x3f2b0, ".rodata", 0xcea0), (0x3f2b4, ".rodata", 0xcf80),
                                 (0x3f2b8, ".rodata", 0xd060)])
        ops = sorted((name, off) for sec, off, _, name, _, _ in relocs
                     if name in ("dpu_lite_r2p0_core_ops", "dpu_r2p0_core_ops", "dpu_r4p0_core_ops"))
        self.assertEqual(ops, [("dpu_lite_r2p0_core_ops", 0xaa80), ("dpu_r2p0_core_ops", 0xaa68),
                               ("dpu_r4p0_core_ops", 0xaa98)])
        matches = {}
        for sec, off, _, _, symsec, value in relocs:
            if sec == rodata and symsec == rodata and value in (0xaa68, 0xaa80, 0xaa98):
                # of_device_id: .compatible at 64, .data at 192
                matches.setdefault(value, []).append(cstr(secs[rodata][2] + off - 192 + 64))
        self.assertEqual(matches, {0xaa68: ["sprd,sharkl3-dpu"], 0xaa80: ["sprd,sharkl5-dpu"],
                                   0xaa98: ["sprd,sharkl5pro-dpu"]})
        # No branch, bl or adr outside the pieces goes into them (branches within .text carry no
        # relocation).
        for i in range(secs[text][3] // 4):
            addr = 4 * i
            if addr in in_text or any(a <= addr < b for a, b in caves):
                continue
            w = word(ko, addr)
            target = branch_target(addr, w)
            if w & 0x9f000000 == 0x10000000:                # adr
                target = int(a64.disassemble(addr, w).split(", ")[1], 16)
            elif w & 0xfc000000 == 0x94000000:              # bl
                target = int(a64.disassemble(addr, w).split(" ")[1], 16)
            self.assertFalse(target is not None and any(a <= target < b for a, b in caves), hex(addr))
        # The stock words around the hooks: drm_add_edid_modes, then mov w19, w0; kfree(edid) at
        # 0x8c80; drm_err, then mov w19, wzr; the return of w19 at 0x8ca4. get_modes's prologue
        # saves x30, x19 and x20 (x19 the connector from 0x8c28). mode_valid returns 0 at 0x8e54.
        self.assertEqual([word(ko, a) for a in (0x8c78, 0x8c7c, 0x8c80, 0x8c84, 0x8c9c, 0x8ca0, 0x8ca4)],
                         [0x94000000, 0x2a0003f3, 0xaa1403e0, 0x94000000, 0x94000000, 0x2a1f03f3, 0x2a1303e0])
        self.assertEqual([in_text[a][1] for a in (0x8c78, 0x8c84, 0x8c9c)], ["drm_add_edid_modes", "kfree", "drm_err"])
        self.assertEqual([word(ko, a) for a in (0x8c18, 0x8c1c, 0x8c20, 0x8c28)],
                         [0xf800865e, 0xa9be7bfd, 0xa9014ff4, 0xaa0003f3])
        self.assertEqual([a64.disassemble(a, word(ko, a)) for a in (0x8e54, 0x8e58, 0x8e60)],
                         ["ldp x29, x30, [sp, #64]", "ldp x20, x19, [sp, #80]", "mov w0, wzr"])


# sha256 of the real module with UHD (test 42 MOD-K, out/dp-evidence/test42/modk/: 3840x2160@30
# gave a picture on the Hisense TV, K1), and with Y420 too: with the guard (the reviewed guard fix,
# out/dp-evidence/test42/y420/guardfix/) and without it (MOD-Y1: 3840x2160@60 4:2:0 on the TV, Y1).
# Those with fix E (the defaults; Y420_NOGUARD is MOD-Y1-E, out/dp-evidence/test42/irqfix/), and
# without it (irq_mask=False: MOD-K, the guard fix and MOD-Y1 as test 42 ran them).
UHD_SHA256 = "383035264e5850cfa6dbfe7126f7ff7a4271103b44ed3a1df7ec8d5780edbad4"
Y420_SHA256 = "136739e9a77b3ef325914035649b5bb6d32521bcf01b098438f81ad3008774d6"
Y420_NOGUARD_SHA256 = "6eefac5870ad4b48062fb150a76ff479ee78bbdfb05a48455bfbd386713caada"
UHD_NOMASK_SHA256 = "cdbe8bd9e7c4448f5bc0b88ba91bdc3ad6845c7dbe17bce30af080b3ef1bf558"
Y420_NOMASK_SHA256 = "11e450794d0c05d001ca335aff96e7e5c5d1d78fb5b7d506637b8419e5ef7514"
Y420_NOGUARD_NOMASK_SHA256 = "9ae14445ded713e0fc4b785bea31c13a5d219caf978abbce93f578e40b3ec76b"
# Where each UHD and Y420 branch must go (sprd_drm.py's comments; SYNTHESIS.md section 4).
UHD_Y420_BRANCHES = {
    0x203f8: 0x1f278, 0x249d8: 0x1f2ac, 0x249cc: 0x1f2c0,                   # Y420_WORDS
    0x24a38: 0x202e4, 0x24a40: 0x202e4, 0x24a4c: 0x24984, 0x24a54: 0x24984,  # UHD's steps
    0x1f280: 0x24a30, 0x1f290: 0x203fc, 0x1f2a0: 0x203fc, 0x1f2a8: 0x202e4,
    0x1f2b4: 0x24a44, 0x1f2bc: 0x24984, 0x1f2c4: 0x8db8, 0x1f2cc: 0x1f2e8, 0x1f2dc: 0x1f2e8,
    0x1f2e4: 0x8db8, 0x1f2e8: 0x8dd8,
    0x9100: 0x913c, 0x910c: 0x9140,                                         # Y420_MP
    0x943c: 0x319f4, 0x31a08: 0x9528,
    0xaefc: 0x31a8c, 0x31a90: 0xaf00, 0x31a94: 0xaf00, 0x31ab0: 0xaf00, 0x31ab8: 0xaf00,  # guard
    0x8e2c: 0x8dd8,                                                         # b.hi
}
UHD_Y420_ADRS = {0x24a30: 0x228e0, 0x24a3c: 0x249e0, 0x24a44: 0x228e0, 0x24a50: 0x249e0,
                 0x1f278: 0x24a30, 0x1f2a4: 0x1f228, 0x1f2ac: 0x24a30, 0x1f2b8: 0x1f228}


class SprdDrmUhdY420(util.TestCase):
    """The 4K30 (UHD) and 4K60 YCbCr 4:2:0 (Y420) switches."""

    def stock(self):
        path = os.path.join(util.ROOT, "out", ".dock", "work", "dpmodes-kernel", "sprd-drm.orig.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        ko = util.slurp(path)
        self.assertEqual(hashlib.sha256(ko).hexdigest(), sprd_drm.MODULE_SHA256)
        return ko

    def test_defaults(self):
        """Test 42 ships 4K30 and 4K60 4:2:0: the all_modes default is MOD-Y1-E (UHD, Y420, no
        guard, fix E); recovery's (all_modes off) has none of them."""
        self.assertTrue(sprd_drm.UHD)
        self.assertTrue(sprd_drm.Y420)
        self.assertFalse(sprd_drm.Y420_GUARDED)
        self.assertTrue(sprd_drm.IRQ_MASK)
        self.assertEqual(sprd_drm.patches(True), sprd_drm.patches(True, True, True, True, True, False, True))
        self.assertNotEqual(sprd_drm.patches(True), sprd_drm.patches(True, uhd=True, y420=True, y420_guard=True))
        self.assertEqual(sprd_drm.patches(False), sprd_drm.patches(False, False, False, False, False, False, True))
        # build.sh's THIMSY_MONITOR_4K check: the default all_modes patch has Y420's mode.
        self.assertIn(sprd_drm.Y420_PATCH, sprd_drm.patches(True))
        self.assertNotIn(sprd_drm.Y420_GUARD, sprd_drm.patches(True))

    def test_release_pins(self):
        """The build pins its output (sprd_drm.patch_release, used by patch_vendor_dlkm.py and
        patch_vendor_boot.py): MOD-Y1-E with all_modes, the plain module without. The wiring is
        checked on a stand-in module, so a fresh clone without the stock one checks it too (the fake
        module can't hold UHD and Y420's overlapping sites, so they are off here)."""
        self.assertEqual(sprd_drm.RELEASE_SHA256, {True: Y420_NOGUARD_SHA256, False: PLAIN_SHA256})
        self.assertIn("out = {MODULE: patch_release, ",
                      util.slurp(os.path.join(util.ROOT, "scripts", "patch_vendor_dlkm.py"), text=True))
        self.assertIn("patch_release(cpio[off:off + size], all_modes=False)",
                      util.slurp(os.path.join(util.ROOT, "scripts", "patch_vendor_boot.py"), text=True))
        ko, sha = fake_module()
        with mock.patch.multiple(sprd_drm, MODULE_SHA256=sha, UHD=False, Y420=False):
            for all_modes in (True, False):
                out = sprd_drm.patch_module(ko, all_modes)
                pins = {all_modes: hashlib.sha256(out).hexdigest(), not all_modes: "0" * 64}
                with mock.patch.object(sprd_drm, "RELEASE_SHA256", pins):
                    self.assertEqual(sprd_drm.patch_release(ko, all_modes), out)
                    env = {"THIMSY_DP_ALL_MODES": "1" if all_modes else "0"}
                    with mock.patch.dict(os.environ, env):
                        self.assertEqual(sprd_drm.patch_release(ko), out)
                    with self.assertRaises(SystemExit) as e:
                        sprd_drm.patch_release(ko, not all_modes)
                    self.assertIn("no longer gives the release's module", str(e.exception.code))
                    # A table change (here fix E dropped) is refused.
                    with mock.patch.object(sprd_drm, "IRQ_MASK", False), self.assertRaises(SystemExit):
                        sprd_drm.patch_release(ko, all_modes)

    def test_shipped_module(self):
        """The build's copies: vendor_dlkm (patch_vendor_dlkm.py, patch_release with no arguments,
        THIMSY_DP_ALL_MODES=1 as build.sh sets it with SYSTEM_IMG) gets MOD-Y1-E, the module test
        42 passed on the RG Cube (out/dp-evidence/test42/irqfix/NOTES.md); recovery
        (patch_vendor_boot.py, all_modes=False) gets the plain module (no all_modes; HPD, the IRQ
        guard and fix E), which replaces test 41's recovery module and has not run on a Cube; without
        SYSTEM_IMG (THIMSY_DP_ALL_MODES=0) vendor_dlkm gets the plain one too."""
        ko = self.stock()
        with mock.patch.dict(os.environ, {"THIMSY_DP_ALL_MODES": "1"}):
            self.assertEqual(hashlib.sha256(sprd_drm.patch_module(ko)).hexdigest(), Y420_NOGUARD_SHA256)
            self.assertEqual(hashlib.sha256(sprd_drm.patch_module(ko, all_modes=False)).hexdigest(), PLAIN_SHA256)
            self.assertEqual(hashlib.sha256(sprd_drm.patch_release(ko)).hexdigest(), Y420_NOGUARD_SHA256)
            self.assertEqual(hashlib.sha256(sprd_drm.patch_release(ko, all_modes=False)).hexdigest(), PLAIN_SHA256)
        with mock.patch.dict(os.environ, {"THIMSY_DP_ALL_MODES": "0"}):
            self.assertEqual(hashlib.sha256(sprd_drm.patch_module(ko)).hexdigest(), PLAIN_SHA256)
            self.assertEqual(hashlib.sha256(sprd_drm.patch_release(ko)).hexdigest(), PLAIN_SHA256)

    def test_switches_need_their_base(self):
        """uhd needs all_modes, wide and wide_mp; y420 needs uhd."""
        for args in ((False, True, True), (True, False, True), (True, True, False)):
            self.assertEqual(sprd_drm.patches(*args, uhd=True, y420=True), sprd_drm.patches(*args))
        self.assertEqual(sprd_drm.patches(True, True, True, False, True), sprd_drm.patches(True, True, True, False))

    def test_real_module(self):
        ko = self.stock()
        got = {name: hashlib.sha256(sprd_drm.patch_module(ko, True, **kw)).hexdigest() for name, kw in (
            ("uhd", {"uhd": True, "y420": False}), ("y420", {"uhd": True, "y420": True, "y420_guard": True}),
            ("noguard", {"uhd": True, "y420": True, "y420_guard": False}), ("default", {}))}
        self.assertEqual(got, {"uhd": UHD_SHA256, "y420": Y420_SHA256, "noguard": Y420_NOGUARD_SHA256,
                               "default": Y420_NOGUARD_SHA256})
        got = {name: hashlib.sha256(sprd_drm.patch_module(ko, True, irq_mask=False, **kw)).hexdigest()
               for name, kw in (("uhd", {"uhd": True, "y420": False}),
                                ("y420", {"uhd": True, "y420": True, "y420_guard": True}),
                                ("noguard", {"uhd": True, "y420": True, "y420_guard": False}))}
        self.assertEqual(got, {"uhd": UHD_NOMASK_SHA256, "y420": Y420_NOMASK_SHA256,
                               "noguard": Y420_NOGUARD_NOMASK_SHA256})

    def test_mod_y1_and_fix_e(self):
        """MOD-Y1 (out/dp-evidence/test42/mody/, the module of vendor_dlkm_a-MOD-Y1-CM12 3d161cb9)
        is the irq_mask=False build; the irqfix test module (MOD-Y1-E) is the default all_modes
        one, the module test 42 ships, and differs from MOD-Y1 in fix E's words only."""
        ko = self.stock()
        path = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "mody", "sprd-drm-MOD-Y1.ko")
        if not os.path.isfile(path):
            self.skipTest("no %s" % path)
        y1 = util.slurp(path)
        self.assertEqual(sprd_drm.patch_module(ko, True, uhd=True, y420=True, y420_guard=False, irq_mask=False), y1)
        e = sprd_drm.patch_module(ko, True, uhd=True, y420=True, y420_guard=False)
        self.assertEqual(hashlib.sha256(e).hexdigest(), Y420_NOGUARD_SHA256)
        self.assertEqual(sprd_drm.patch_module(ko, True), e)
        self.assertEqual({i - sprd_drm.TEXT_OFFSET for i in range(0, len(y1), 4) if y1[i:i + 4] != e[i:i + 4]},
                         IRQ_MASK_SITES - SAME_AS_B)
        built = os.path.join(util.ROOT, "out", "dp-evidence", "test42", "irqfix", "sprd-drm-MOD-Y1-E.ko")
        if os.path.isfile(built):
            self.assertEqual(util.slurp(built), e)

    def test_swaps_chain(self):
        """Each Y420 swap replaces the word UHD put there, and each UHD swap a MOD-M word."""
        base = {(a, i): w for a, _, new in sprd_drm.patches(True, uhd=False) for i, w in enumerate(new)}
        uhd = {(a, i): w for a, _, new in sprd_drm.patches(True, uhd=True, y420=False) for i, w in enumerate(new)}
        for site, index, without, with_ in sprd_drm.UHD_WORDS:
            self.assertEqual((base[site, index], uhd[site, index]), (without, with_))
        for site, index, without, _ in sprd_drm.Y420_WORDS:
            self.assertEqual(uhd[site, index], without)

    def test_records(self):
        """The 4K30 and 4K60 records hold the CEA 95 and 97 timings, 80 bytes, 8-byte aligned."""
        for mode, (addr, _, new) in ((sprd_drm.UHD_MODE, sprd_drm.UHD_PATCH),
                                     (sprd_drm.Y420_MODE, sprd_drm.Y420_PATCH)):
            self.assertEqual(addr % 8, 0)
            rec = new[:20]
            self.assertEqual(rec, sprd_drm._record(*mode))
            self.assertEqual(rec[4:16], [mode[1]] + list(mode[2]) + [0] + list(mode[3]) + [0, 5])
        self.assertEqual(sprd_drm.UHD_MODE[1:3], (297000, (3840, 4016, 4104, 4400)))
        self.assertEqual(sprd_drm.Y420_MODE[1], 594000)

    def test_code_words(self):
        """Every new code word decodes; branches and adr's go where the comments say."""
        k = words(sprd_drm.patches(True, uhd=True, y420=True, y420_guard=True))
        m = words(sprd_drm.patches(True, uhd=False))
        records = set()
        for addr, _, _ in (sprd_drm.UHD_PATCH, sprd_drm.Y420_PATCH):
            records |= {addr + 4 * i for i in range(20)}
        seen = set()
        for addr, w in sorted(k.items()):
            if m.get(addr) == w or addr in records:
                continue
            text = a64.disassemble(addr, w)
            self.assertFalse(text.startswith("?"), (hex(addr), hex(w)))
            target = branch_target(addr, w)
            if w & 0x9f000000 == 0x10000000:
                target = int(text.split(", ")[1], 16)
                self.assertEqual(target, UHD_Y420_ADRS.get(addr), hex(addr))
                seen.add(addr)
            elif target is not None:
                self.assertEqual(target, UHD_Y420_BRANCHES.get(addr), hex(addr))
                seen.add(addr)
        self.assertEqual(seen, set(UHD_Y420_BRANCHES) | set(UHD_Y420_ADRS))

    def test_guard(self):
        """The guard leaves dpu_flip's HDR block stock, keys on ctx+429 that dpu_modeset stores with
        the bypass byte, and only exists with y420_guard."""
        ko = self.stock()
        guarded = sprd_drm.patch_module(ko, True, uhd=True, y420=True, y420_guard=True)
        plain = sprd_drm.patch_module(ko, True, uhd=True, y420=True, y420_guard=False)
        a, b = sprd_drm.TEXT_OFFSET + 0xaf00, sprd_drm.TEXT_OFFSET + 0xb03c
        self.assertEqual(guarded[a:b], ko[a:b])
        self.assertEqual(a64.disassemble(0xb3b0, word(guarded, 0xb3b0)), "strh w8, [x20, #428]")
        self.assertEqual(a64.disassemble(0xb39c, word(guarded, 0xb39c)), "csinc w8, w8, wzr, hi")
        self.assertEqual(a64.disassemble(0x31a8c, word(guarded, 0x31a8c)), "ldrb w9, [x19, #429]")
        self.assertEqual(word(plain, 0xb3b0), word(ko, 0xb3b0))
        self.assertEqual(word(plain, 0xaefc), sprd_drm.NOP)

    def test_no_relocations(self):
        """No relocation applies to a word UHD or Y420 patches."""
        ko = self.stock()
        shoff, = struct.unpack_from("<Q", ko, 0x28)
        shnum, shstrndx = struct.unpack_from("<HH", ko, 0x3c)
        raw = [struct.unpack_from("<IIQQQQIIQQ", ko, shoff + 64 * i) for i in range(shnum)]
        strtab = raw[shstrndx][4]
        name = [ko[strtab + s[0]:ko.index(b"\0", strtab + s[0])].decode() for s in raw]
        text = name.index(".text")
        hit = set()
        for s in raw:
            if s[1] == 4 and s[7] == text:
                for j in range(s[5] // 24):
                    hit.add(struct.unpack_from("<Q", ko, s[4] + 24 * j)[0] & ~3)
        for kw in ({"uhd": True, "y420": False}, {"uhd": True, "y420": True, "y420_guard": True}, {}):
            for addr, old, _ in sprd_drm.patches(True, **kw):
                for i in range(len(old)):
                    self.assertNotIn(addr + 4 * i, hit, (hex(addr + 4 * i), kw))


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

        def patch_release(ko, all_modes=None):
            calls.append(all_modes)
            return ko.lower()
        with mock.patch.dict(os.environ, env), mock.patch.object(sys, "argv", ["patch_vendor_boot.py", src, dst]), \
                mock.patch.object(vboot, "patch_release", patch_release), \
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
