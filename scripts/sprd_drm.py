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
  - rejects modes 3840 or more wide: 3840x2160 is the composer's 4K path and
    was the display controller's only bypass mode (a known panic risk; a
    monitor without CEA 1080p60 was started in it; with WIDE, bypass now also
    serves anything wider than 1920, see below), DCI 4096x2160 is not handled
    at all; with UHD (all_modes, the default) exactly 3840 passes, and the mode
    table keeps every other width out;
  - without all_modes (recovery and the 1.2 base), keeps CEA 1080p60 the only
    preferred mode, as the vendor driver does, so the composer sets 1080p at
    60 Hz when a monitor connects; the monitor's own preferred mode becomes
    user-defined instead of being dropped, since the vendor composer lists
    only preferred and user-defined modes. With all_modes the mode table
    below decides instead.
sprd_dp_connector_get_modes and _mode_valid (MODES_PATCHES, all_modes only):
  - the connector offers a fixed set of modes, whatever the EDID lists, each
    one only when the monitor lists something like it:
      CEA 720p60 (74.25 MHz), always;
      CEA 1080p60 (148.5 MHz), the only preferred mode, when the EDID lists a
        mode at least 1920x1080 at 24 Hz or more, or there is no EDID;
      CEA 1080p120 (297 MHz), when it lists exactly 1920x1080 at 119 Hz or more;
      2560x1440 at 59.93 Hz (297 MHz, through bypass; with WIDE only), when it
        lists exactly 2560x1440 at 59 Hz or more;
      CEA 3840x2160 at 30 Hz (297 MHz, through bypass; with UHD only), when it
        lists exactly 3840x2160 at 29 Hz or more;
      CEA 3840x2160 at 60 Hz as YCbCr 4:2:0 (594 MHz; with Y420 only), when it
        lists exactly 3840x2160 at 59 Hz or more and takes 4:2:0 for VIC 97.
    Every one of them runs the pixel PLL at 2673 MHz, the only rate that has
    given a picture. get_modes adds them after the EDID's modes (without an
    EDID, 720p60 and 1080p60 alone) and clears every mode's picture aspect
    ratio, so a CEA mode the EDID lists too merges with the table's. mode_valid
    keeps only a mode whose timings and flags are exactly one of the table's,
    gives it the table's refresh rate and type (driver and user-defined, and
    preferred for 1080p60 only), then runs the link and width checks; any
    other mode is dropped. So a monitor that lists 1080p or more starts in CEA
    1080p60, and the composer lists only modes that can work. This is for the 1.4.4 base only, whose
    framework holds a monitor at the mode the user chose. An EDID override
    can open or close a gate but no longer bring in a timing of its own.
  - it replaces the safe-start patch (fix C), which started a monitor without
    CEA 1080p60 in its own best 1080p60 at or below 148.5 MHz; on real
    monitors that picked modes the pixel PLL can't make (138.5 MHz, negative
    sync).
sprd_dp_encoder_mode_set:
  - treats a missing framebuffer in bypass as RGB instead of reading through it.
sprd_dp_pd_event (HPD_PATCHES):
  - while DisplayPort is off, powers it up for an HPD with IRQ_HPD set (hpd 2)
    as well as for a plain HPD (hpd 1); the vendor driver ignored a first
    Attention that had IRQ_HPD set, so an adapter that sends one never lit up.
dptx_irq (IRQ_GUARD_PATCHES):
  - returns IRQ_NONE without touching the controller while DisplayPort holds no
    clock (both of the driver's power flags clear); the vendor handler read
    and acked its registers regardless, and a Type-C interrupt raised by the
    USB 3 PHY's shutdown after an unplug made it write to a powered-down
    controller (SError, panic). The flags are now set right after the clocks go
    on; the encoder's is cleared before its clocks go off, pd_event's after (as
    the vendor driver does), since the unplug's own HPD interrupt arrives while
    pd_event's glb->disable runs and has to be acked. The Type-C interrupt is
    masked before DisplayPort's last clock goes off (the next plug unmasks it).
sprd_dp_encoder_disable / _enable, sprd_dp_pd_event, dptx_core_init
(IRQ_MASK_PATCHES, fix E, every variant, recovery too; irq_mask=False leaves it
out, for the test 42 modules up to MOD-Y1):
  - the encoder's disable writes the DPTX interrupt enable register (0xd04) 0
    while it still holds the controller's clock; its enable writes 0x15 (HPD,
    HDCP, SDP), with Type-C (0x80) only while pd_event holds DisplayPort, at
    its end, after the stream is up. A power-button sleep with the composer's
    sleep fix turns DisplayPort off by DPMS with the monitor still attached,
    and the line stormed until the kernel disabled it ("irq 76: nobody cared",
    dp_dock_findings.md, T2-sleep); with the encoder off no DPTX source can
    assert it now, and events latched meanwhile are taken at the enable.
    pd_event off writes 0x15 with the encoder on (an unplug while awake is
    reported as before), 0 with it off (no window after 0x8adc clears its
    flag); dptx_core_init writes exactly 0x95, no longer its reset value | 0x95
    (the handler acks nothing above bit 7).
dptx_threaded_irq (IRQ_GUARD_PATCHES):
  - while both flags are clear, reports the connector unplugged (as its own
    unplug path does) without touching the controller. The hard handler wakes
    it, but it runs only once pd_event's thread lets go of the CPU, after
    DisplayPort is off; it then read the powered-down controller (pstore-dock).
sprd_crtc_atomic_enable / _disable (MODE_CHANGE_PATCHES, all_modes only):
  - a mode change on the DisplayPort CRTC (active stays on) runs the second
    display controller's disable and enable, as an unplug and a plug do. The
    vendor helper skipped both for any CRTC (the panel's seamless resolution
    switch), so only the connect-time modeset ever set the pixel clock, size
    and porches: after the framework switched a monitor to another mode, it
    got the new timings without them, lost the signal and dropped HPD. The
    panel's CRTC keeps the vendor behaviour.
dpu_modeset and dpu_flip (WIDE_PATCHES, all_modes with WIDE):
  - a mode wider than 1920 puts the second display controller into bypass,
    where it sends two pixels per DPI clock from one layer; the vendor driver
    did that only for 3840x2160. 2560x1440 at 60 Hz (297 MHz) then needs a
    148.5 MHz DPI clock. In normal mode nothing wider than 1920 has shown a
    picture. WIDE also gives the mode table its 2560x1440 record.
  - dpu_flip puts the first plane on hardware layer 7 in bypass, the one layer
    bypass scans out, as the vendor 4.14 driver did; the 5.4 port wrote layer
    index 0 into a plane past the controller's four instead, and 2560x1440
    gave a locked signal but a black picture (test 42 MOD-P). Its layer-count
    check is gone: a second layer in bypass is not shown, so SurfaceFlinger
    must give the second display controller one layer.
sprd_dp_encoder_mode_set and dptx_video_ts_calculate (MP and TH40, all_modes
with WIDE and WIDE_MP):
  - sets the DisplayPort controller's pixel mode to the bypass flag, so it
    takes two pixels per clock in bypass and one otherwise. (Test 42 A4, 3840x2160
    at 30 Hz in bypass with single pixel mode, gave no picture, but A4 never
    presented a frame: the composer dropped it, so it says nothing about the pixel
    mode; dp_dock_findings.md, the correction to A4.)
  - the controller's initial transfer threshold is 40 in dual pixel mode
    (upstream dw-dp's value for it), 16 otherwise. With both dpu_flip's layer
    7 and this, 2560x1440 at 60 Hz showed a correct, stable picture (test 42
    MOD-Q).

The module is in vendor_dlkm (loaded at boot) and in vendor_boot (recovery).
all_modes defaults to THIMSY_DP_ALL_MODES=1 in the environment; recovery's copy
never gets it (patch_vendor_boot.py). With all_modes the defaults (WIDE, WIDE_MP,
UHD, Y420 and IRQ_MASK on, Y420_GUARDED off) give test 42's MOD-Y1-E (sha256
6eefac58...): 3840x2160 at 30 Hz (UHD) and at 60 Hz as YCbCr 4:2:0 (Y420) when
the monitor lists them, with fix E. Every other variant takes explicit arguments.
"""
import hashlib
import os
import struct
import sys

MODULE_SHA256 = "4d60c3c021bdd779cd55502b73fbe849eb2e773f7b9671dbd843444d8ce5f932"
# The modules the release build may write (patch_release), by all_modes. True: test 42's
# MOD-Y1-E, vendor_dlkm's with SYSTEM_IMG, the module on the RG Cube in test 42
# (out/dp-evidence/test42/irqfix/NOTES.md). False: the plain module (HPD, the IRQ guard and fix
# E), recovery's (vendor_boot) and vendor_dlkm's without SYSTEM_IMG. It replaces test 41's
# recovery module (4c2b7eb7..., before the IRQ guard and fix E) and has not run on a Cube yet.
RELEASE_SHA256 = {
    True: "6eefac5870ad4b48062fb150a76ff479ee78bbdfb05a48455bfbd386713caada",
    False: "c7e8c8295939b6a91904239186778af2e5ed266d385807bb1be1d9c633162ea2",
}
TEXT_OFFSET = 0x30000   # .text in the module file
NOP = 0xd503201f

# (address in .text, original words, patched words)
PATCHES = [
    # mode_valid: clock > 0x49000 kHz -> MODE_CLOCK_HIGH; keep the type's preferred bit;
    # then the link and width checks at 0x8db8 (with all_modes, the mode table's filter
    # first: ALL_MODES_WORDS).
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
    # user-defined; then the stub. With all_modes none of this is reached: after the
    # width check, 0x8e3c returns MODE_OK instead of going back to 0x8cec.
    (0x8d8c, [0xb940b663, 0x29479a65, 0x2948a667, 0x294a2e6a, 0x294b366c, 0xb940666e],
             [0x1400002f,               # b    0x8e48: type |= user-defined
              0xb9403668,               # ldr  w8, [x19, #0x34]   (type)
              0x36180608,               # tbz  w8, #3, return     (not preferred)
              0x121c7908,               # and  w8, w8, #~preferred
              0x321b0108,               # orr  w8, w8, #user-defined
              0x1400002c]),             # b    0x8e50: store type, return
    # mode_valid: drop modes the trained link can't carry at RGB 8 bpc, and modes
    # 3840 or more wide (b.hs at 0x8e2c; until 1.4.4 test 41 it was b.hi, wider
    # than 3840, so a monitor could start in 3840x2160 bypass). The code runs in
    # the dead log-line tails of CEA 640x480 and CEA 4K60 (unreachable since 0x8d8c and 0x8dd8 are patched), in four pieces
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
              0x54fffd62,               # b.hs 0x8dd8              (3840 wide or wider: MODE_CLOCK_HIGH)
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

# dptx_irq guard. The driver's two power flags in struct sprd_dp, +0xb90 (the
# encoder is enabled) and +0xbb9 (pd_event powered DisplayPort up), each stand for
# one reference on the controller's clocks (glb->enable / glb->disable). Here both
# are set right after glb->enable. The encoder's is cleared before its glb->disable;
# pd_event's stays where the vendor driver clears it, after its glb->disable
# (0x8adc): detect(0) makes the controller raise the unplug's HPD interrupt 3-10 ms
# later, while that glb->disable (about 14 ms) still has it clocked, and with the
# encoder already off (the screen asleep) a flag cleared before it would leave that
# level interrupt unacked: IRQ_NONE again and again until the kernel disables the
# line ("nobody cared", no hot-plug until a reboot). dptx_irq returns IRQ_NONE
# without touching the controller while both are clear. The Type-C interrupt (bit 7
# of the interrupt enable register, base + 0xd04) is masked while the controller is
# still clocked, before pd_event's and bind's glb->disable; the next plug's
# dptx_core_init enables it again (0x95). HPD stays unmasked: its unplug event is
# what tells DRM the monitor is gone. (Fix E, IRQ_MASK_PATCHES below, replaces pd_event's mask:
# IEN is 0x15 or 0 there, and 0 while the encoder is off.)
#
# dptx_threaded_irq, which the hard handler wakes for every HPD event, runs on the
# same CPU as pd_event's thread (both FIFO), so after an unplug with the encoder off
# it runs only once DisplayPort is off and read 0xd08 there (pstore-dock.txt: a
# synchronous external abort at dptx_threaded_irq+0x64). Right after its mutex_lock
# it now checks the same flags; with both clear it takes its own unplug path from
# 0x375e8 (the redriver off, the link state cleared, extcon and DRM told
# "disconnected") without the controller's registers, and skips the IRQ_HPD link
# check. sprd_dp_connector_detect reads only that link state (dptx + 0x1156).
#
# The new code lives in four dead functions (dptx_global_intr_en/_dis,
# dptx_soft_reset, dptx_soft_reset_all at 0x32814-0x32a24: nothing references them,
# they are not exported); the words with relocations (0x32824-0x3283c,
# 0x32894-0x328ac, 0x32948, 0x329dc) and readl alternatives (0x3284c, 0x328bc,
# 0x32918, 0x32954, 0x329ac, 0x329ec) there stay as they are and are never run.
# Only x8-x11 are used, and in the threaded handler w0, w22 and x23 (which it saves).
IRQ_GUARD_PATCHES = [
    # bind: mask Type-C before its glb->disable (blr x21 moves to 0x329d0).
    (0x87ec, [0xd63f02a0], [0x1400a871]),                   # b    0x329b0
    # pd_event on: set the flag right after glb->enable (blr x8 moves to 0x3292c).
    (0x89f4, [0xd63f0100], [0x1400a7ce]),                   # b    0x3292c
    # pd_event off, after dp_detect(0): mask Type-C, then the vendor's glb->disable;
    # the vendor's own clear of the flag follows it (0x8adc). 0x8a98-0x8aa0 are no
    # longer run.
    (0x8a94, [0xf9458e88], [0x1400a7b5]),                   # b    0x32968
    # encoder_disable: clear the flag before glb->disable (dp->glb, never NULL,
    # is not checked any more).
    (0x920c, [0xf9458e68, 0xb4000168, 0xf9400908, 0xb4000128],
             [0x392e427f,               # strb wzr, [x19, #2960]   (encoder enabled: no)
              0xf9458e68,               # ldr  x8, [x19, #2840]    (dp->glb)
              0xf9400908,               # ldr  x8, [x8, #16]       (glb->disable)
              0xb4000128]),             # cbz  x8, 0x923c
    # encoder_enable: set the flag right after glb->enable (blr x19 moves to 0x32958).
    (0x9324, [0xd63f0260], [0x1400a58d]),                   # b    0x32958
    # The guard, in dptx_soft_reset. x1 is the struct dptx (the irq's dev_id).
    (0x328f4, [0xf800865e, 0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf946b408, 0xaa0003f3,
               0x528003aa, 0x0a0a0034, 0x91081108],
              [0xf9401429,              # ldr  x9, [x1, #40]       (dptx->dev)
               0xb4000169,              # cbz  x9, 0x32924
               0xf9404d29,              # ldr  x9, [x9, #152]      (its drvdata: struct sprd_dp)
               0xb4000129,              # cbz  x9, 0x32924
               0x396e412a,              # ldrb w10, [x9, #2960]    (encoder enabled)
               0x396ee52b,              # ldrb w11, [x9, #3001]    (pd_event on)
               0x2a0b014a,              # orr  w10, w10, w11
               0x340000aa,              # cbz  w10, 0x32924        (not clocked)
               0x14000002]),            # b    0x3291c
    (0x3291c, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009],
              [0xf800865e,              # str  x30, [x18], #8      (dptx_irq's first word)
               0x1400148d,              # b    0x37b54             (the rest of dptx_irq)
               0x2a1f03e0,              # 0x32924: mov w0, wzr     (IRQ_NONE)
               0xd65f03c0]),            # ret
    # pd_event on.
    (0x3292c, [0xd50332bf, 0xf946b409, 0x5289f180, 0x72a00020],
              [0xd63f0100,              # blr  x8                  (glb->enable)
               0x52800029,              # mov  w9, #1
               0x392ee689,              # strb w9, [x20, #3001]    (pd_event on: yes)
               0x17ff5830]),            # b    0x89f8
    # encoder_enable.
    (0x32958, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009],
              [0xd63f0260,              # blr  x19                 (glb->enable)
               0x52800028,              # mov  w8, #1
               0x392e42a8,              # strb w8, [x21, #2960]    (encoder enabled: yes)
               0x17ff5a71]),            # b    0x9328
    # pd_event off. The flag stays set through glb->disable; 0x8adc clears it after.
    (0x32968, [0xd50332bf, 0xf946b669, 0x0a340108, 0x91081129, 0xb9000128, 0xa9414ff4,
               0xa8c27bfd, 0xf85f8e5e, 0xd65f03c0, 0xf800865e, 0xa9be7bfd, 0xf9000bf3],
              [0xf945d689,              # ldr  x9, [x20, #2984]    (dptx)
               0xf946b52a,              # ldr  x10, [x9, #3432]    (its registers)
               0xb94d054b,              # ldr  w11, [x10, #3332]   (0xd04: interrupt enable)
               0x1218796b,              # and  w11, w11, #0xffffff7f
               0xd50332bf,              # dmb  oshst
               0xb90d054b,              # str  w11, [x10, #3332]
               0xb94d054b,              # ldr  w11, [x10, #3332]   (read back: the write is done)
               0xd50333bf,              # dmb  osh
               0xf9458e88,              # ldr  x8, [x20, #2840]    (dp->glb)
               0xf9400908,              # ldr  x8, [x8, #16]       (glb->disable)
               0xb4eb09a8,              # cbz  x8, 0x8ac4
               0x17ff5844]),            # b    0x8aa4              (its CFI check, then blr)
    # bind.
    (0x329b0, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009, 0xd50332bf, 0xf946b409,
               0x5289f180, 0x72a00020, 0x2a0a0108, 0x91081129],
              [0xf945d669,              # ldr  x9, [x19, #2984]    (dptx)
               0xf946b52a,              # ldr  x10, [x9, #3432]
               0xb94d054b,              # ldr  w11, [x10, #3332]
               0x1218796b,              # and  w11, w11, #0xffffff7f
               0xd50332bf,              # dmb  oshst
               0xb90d054b,              # str  w11, [x10, #3332]
               0xb94d054b,              # ldr  w11, [x10, #3332]
               0xd50333bf,              # dmb  osh
               0xd63f02a0,              # blr  x21                 (glb->disable)
               0x17ff5787]),            # b    0x87f0
    # The threaded handler's guard, in dptx_global_intr_en/_dis (0x32850-0x32893,
    # between the readl alternative at 0x3284c and the relocations at 0x32894). x19 is
    # the struct dptx; dptx_threaded_irq saved x22 and x23 in its prologue.
    (0x32850, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009, 0xd50332bf, 0xf946b669,
               0x2a0a0108, 0x91341129, 0xb9000128, 0xf9400bf3, 0xa8c27bfd, 0xf85f8e5e,
               0xd65f03c0, 0xf800865e, 0xa9be7bfd, 0xf9000bf3, 0x910003fd],
              [0xf9401669,              # ldr  x9, [x19, #40]      (dptx->dev)
               0xb4000129,              # cbz  x9, 0x32878
               0xf9404d29,              # ldr  x9, [x9, #152]      (its drvdata: struct sprd_dp)
               0xb40000e9,              # cbz  x9, 0x32878
               0x396e412a,              # ldrb w10, [x9, #2960]    (encoder enabled)
               0x396ee52b,              # ldrb w11, [x9, #3001]    (pd_event on)
               0x2a0b014a,              # orr  w10, w10, w11
               0x3400006a,              # cbz  w10, 0x32878        (not clocked)
               0xf946b668,              # ldr  x8, [x19, #3432]    (0x3752c's word)
               0x1400132f,              # b    0x37530             (the handler as before)
               0xb90d5e7f,              # 0x32878: str wzr, [x19, #3420]
               0xb90dda7f,              # str  wzr, [x19, #3544]   (the HPD event: taken)
               0x2a1f03f6,              # mov  w22, wzr            (HPD state: low, for extcon)
               0x52822ac8,              # mov  w8, #0x1156
               0x8b080277,              # add  x23, x19, x8        (the link state, as at 0x3758c)
               0x2a1f03e0,              # mov  w0, wzr
               0x14001356]),            # b    0x375e8             (its unplug path, from the redriver on)
    # dptx_threaded_irq, right after its mutex_lock: to the guard.
    (0x3752c, [0xf946b668], [0x17ffecc9]),                  # b    0x32850
    # dptx_irq's first word (its shadow-stack push) moves to 0x3291c.
    (0x37b50, [0xf800865e], [0x17ffeb69]),                  # b    0x328f4
]

# Fix E: the DPTX interrupt enable register (IEN, base + 0xd04) while the encoder is off
# (dp_dock_findings.md, "Bug found after T2-sleep"; every variant, recovery too). The
# composer's sleep fix turns DisplayPort off by DPMS on a power-button sleep while the monitor
# stays connected (sprd_dp_encoder_disable, "dp suspend OK"); right after the wake the kernel
# logged "irq 76: nobody cared" and disabled dw_dptx (100001 interrupts). pd_event's reference
# keeps the controller clocked through such a sleep, so the guard (fix B) lets the vendor
# handler run, and its IRQ_NONE for ISTS[7:0] == 0 points at a source above bit 7 that IEN's
# reset value enables and nothing acks [I]: dptx_core_init's IEN |= 0x95 leaves the reset value's
# upper bits on. Now nothing in DPTX can assert the line while the encoder is off, and the
# encoder's enable takes what latched meanwhile:
#   - encoder_disable, right after its own video-stream write (0x9208, x8 = base + 0x300; the
#     encoder's flag is still set and its clock reference held): IEN = 0, read back, then fix
#     B's flag clear (0x920c) and glb->disable as before. w9 is dead after it (0x921c sets it).
#   - encoder_enable, at its end (0x9550, after timing_set and sprd_dpu1_run; fix B set the
#     flag at 0x32960, right after glb->enable, so IEN is written clocked): IEN = 0x15 (HPD,
#     HDCP, SDP) | pd_event's flag << 7 (Type-C only while pd_event holds DisplayPort), then
#     the vendor's flag store it displaces (w8 = 1). x21 is the struct sprd_dp on both paths
#     into 0x9528; the "already enabled" path (0x950c to 0x9554) skips it; x9-x11 are dead
#     there (0x9554 and 0x9558 load what they use).
#   - pd_event off (replaces fix B's 0x32968 cave, which masked Type-C only): IEN = 0x15 while
#     the encoder is on (the unplug's HPD event is acked while glb->disable still has the
#     controller clocked, as before), 0 while it is off, so after 0x8adc clears the flag nothing
#     can assert the line (a replug while asleep ran core_init's 0x95; the unplug that follows
#     latches, and the next encoder_enable or plug takes it). One word longer: 0x32998 is free.
#   - dptx_core_init (plug, bind): IEN = 0x95 exactly, not reset value | 0x95.
# The caves are in dptx_global_intr_dis (0x328c0-0x328d3) and dptx_soft_reset_all
# (0x329f0-0x32a0f): no relocation, alternative or jump-table entry lands on a patched word,
# and the readl alternatives right before them (0x328bc, 0x329ec) stay unpatched and unrun.
# Still free in 0x32814-0x32a24 after B and E: 0x32814-0x32820, 0x3282c, 0x32838,
# 0x32840-0x32848, 0x3289c, 0x328a8, 0x328b0-0x328b8, 0x328d4-0x328f0, 0x3293c-0x32944,
# 0x3294c-0x32950, 0x3299c-0x329a8, 0x329d8, 0x329e0-0x329e8 and 0x32a10-0x32a20.
IRQ_MASK = True   # fix E; False gives the modules test 42 ran up to MOD-Y1
_GUARD_OFF = [t for t in IRQ_GUARD_PATCHES if t[0] == 0x32968][0]
IRQ_MASK_OFF = (0x32968, _GUARD_OFF[1] + [0x910003fd],
                [0xf945d689,            # ldr  x9, [x20, #2984]    (dptx)
                 0xf946b52a,            # ldr  x10, [x9, #3432]    (its registers)
                 0x396e4289,            # ldrb w9, [x20, #2960]    (encoder enabled)
                 0x528002ab,            # mov  w11, #0x15          (HPD, HDCP, SDP)
                 0x1b097d6b,            # mul  w11, w11, w9        (0 with the encoder off)
                 0xd50332bf,            # dmb  oshst
                 0xb90d054b,            # str  w11, [x10, #3332]   (0xd04: IEN)
                 0xb94d054b,            # ldr  w11, [x10, #3332]   (read back: the write is done)
                 0xd50333bf,            # dmb  osh
                 0xf9458e88,            # ldr  x8, [x20, #2840]    (dp->glb)
                 0xf9400908,            # ldr  x8, [x8, #16]       (glb->disable)
                 0xb4eb0988,            # cbz  x8, 0x8ac4
                 0x17ff5843])           # b    0x8aa4              (its CFI check, then blr)
IRQ_MASK_PATCHES = [
    # encoder_disable: the vendor's 0x300 write moves to the cave.
    (0x9208, [0xb9000109], [0x1400a5ae]),                   # b    0x328c0
    (0x328c0, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009, 0xd50332bf],
              [0xb9000109,              # str  w9, [x8]            (the vendor's 0x300 write)
               0xb90a051f,              # str  wzr, [x8, #2564]    (0x300 + 0xa04 = 0xd04: IEN = 0)
               0xb94a0509,              # ldr  w9, [x8, #2564]     (read back: the write is done)
               0xd50333bf,              # dmb  osh
               0x17ff5a4f]),            # b    0x920c              (fix B's flag clear, glb->disable)
    # encoder_enable: the vendor's flag store moves to the cave.
    (0x9550, [0x392e42a8], [0x1400a528]),                   # b    0x329f0
    (0x329f0, [0xd50331bf, 0x2a0803e9, 0xca090129, 0xb5000009, 0xd50332bf, 0xf946b669,
               0x0a0a0108, 0x91081129],
              [0xf945d6a9,              # ldr  x9, [x21, #2984]    (dptx)
               0xf946b52a,              # ldr  x10, [x9, #3432]    (its registers)
               0x396ee6a9,              # ldrb w9, [x21, #3001]    (pd_event on)
               0x528002ab,              # mov  w11, #0x15
               0x2a091d6b,              # orr  w11, w11, w9, lsl #7   (Type-C while pd_event holds DP)
               0xb90d054b,              # str  w11, [x10, #3332]   (IEN)
               0x392e42a8,              # strb w8, [x21, #2960]    (the vendor's store, w8 = 1)
               0x17ff5ad2]),            # b    0x9554
    IRQ_MASK_OFF,
    # dptx_core_init, after its soft reset: IEN = 0x95.
    (0x331fc, [0x2a0a0108], [0x2a0a03e8]),                  # mov  w8, w10  (was orr w8, w8, w10)
]

# A mode change on the DisplayPort CRTC. sprd_crtc_atomic_enable/_disable (shared by
# the panel's CRTC, type 1, and DisplayPort's, type 2, at crtc + 1816) skip the
# display controller's own enable/disable when only the mode changed
# ((state flags & 6) == 2). For type 2 they now call them, so a mode change turns
# DisplayPort's display controller off and on with the new mode, dpu_clk_init
# included, as an unplug and a plug do. The checks live in sprd_dpu_resume
# (0x61a4-0x61e7, unreferenced and not exported); its words with relocations
# (0x61b8-0x61d4) stay and are never run. x19 is the CRTC; x9 and the flags are
# dead where the checks go back to.
MODE_CHANGE_PATCHES = [
    (0x2614, [0x54000180], [0x5401dc80]),   # b.eq 0x61a4 (was 0x2644, skip the enable)
    (0x26ac, [0x540003a0], [0x5401d960]),   # b.eq 0x61d8 (was 0x2720, skip the disable)
    (0x61a4, [0xf800865e, 0xa9be7bfd, 0xf9000bf3, 0x910003fd],
             [0xb9471a69,               # ldr  w9, [x19, #1816]    (CRTC type)
              0x7100093f,               # cmp  w9, #0x2            (DisplayPort)
              0x54fe24c1,               # b.ne 0x2644              (no: skip, as before)
              0x17fff11a]),             # b    0x2618              (yes: the controller's enable)
    (0x61d8, [0xf9400bf3, 0xa8c27bfd, 0xf85f8e5e, 0xd65f03c0],
             [0xb9471a69,               # ldr  w9, [x19, #1816]
              0x7100093f,               # cmp  w9, #0x2
              0x54fe2a01,               # b.ne 0x2720
              0x17fff133]),             # b    0x26b0              (the controller's disable)
]

# The fixed mode table (all_modes; test 42, test42_plan.md 2.3). It replaces the
# safe-start patch (fix C), whose space at 0x202b4 now holds GET_MODES, and the
# all_modes nop at 0x8d94: with the filter below, mode_valid's CEA code checks and
# its stub at 0x8d90 are no longer reached.
#
# The three pieces live in display controller inits only other SoCs use:
# dpu_init.1506 (sharkl5), .1618 (sharkl3) and .1724 (sharkl5pro). Each is reached
# only through a CFI jump-table slot (0x3f2b0, 0x3f2b4, 0x3f2b8) that its SoC's
# core ops point to; the Cube's device tree has only sprd,qogirn6pro-dpu and -dpu1.
# Nothing in them carries a relocation or an alternative.
#
# MODES, one 80-byte record each: name[16]; the twelve words of drm_display_mode
# +56..+103 (clock, hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay,
# vsync_start, vsync_end, vtotal, vscan, flags); vrefresh; type (0x60 driver and
# user-defined, 0x68 preferred too); the gate: the EDID mode that must be on the
# connector's list for get_modes to add the record (hdisplay | vdisplay << 16, 0
# for none: always), and its rate (bits 0-7 the least refresh rate in Hz, rounded
# as DRM's vrefresh; bit 31 the size must be exact, else at least it). Every clock
# is 297 MHz divided by a whole number, so the pixel PLL runs at 2673 MHz.
#
# (name, clock in kHz, horizontal active/sync start/sync end/total, vertical likewise,
#  flags, vrefresh, type, gate (width, height, least Hz, exact size) or None)
MODES = [
    ("1280x720", 74250, (1280, 1390, 1430, 1650), (720, 725, 730, 750), 0x5, 60, 0x60,
     None),                                                       # CEA 4
    ("1920x1080", 148500, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5, 60, 0x68,
     (1920, 1080, 24, False)),                                    # CEA 16, the only preferred mode
    ("1920x1080", 297000, (1920, 2008, 2052, 2200), (1080, 1084, 1089, 1125), 0x5, 120, 0x60,
     (1920, 1080, 119, True)),                                    # CEA 63
    # 2560x1440 at 59.934 Hz through bypass (+hsync, -vsync): test 42's y1c timing,
    # the one that gave a picture. Only with WIDE (NREC_WORDS).
    ("2560x1440", 297000, (2560, 2608, 2640, 3346), (1440, 1443, 1448, 1481), 0x9, 60, 0x60,
     (2560, 1440, 59, True)),
]


def _record(name, clock, h, v, flags, vrefresh, type_, gate):
    size, rate = 0, 0
    if gate:
        width, height, hz, exact = gate
        size, rate = width | height << 16, hz | (0x80000000 if exact else 0)
    return (list(struct.unpack("<4I", name.encode().ljust(16, b"\0")))
            + [clock, h[0], h[1], h[2], h[3], 0, v[0], v[1], v[2], v[3], 0, flags]
            + [vrefresh, type_, size, rate])


# The table, in dpu_init.1618.
MODE_TABLE = (0x227a0, [0xf800865e, 0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf9400009, 0x2a1f03e8,
                        0xaa0003f3, 0x9100b129, 0xb9000128, 0xb9404809, 0xb940380a, 0xf940000b,
                        0x2a094149, 0x9100816a, 0xb9000149, 0xf940000a, 0x9100914a, 0xb9000149,
                        0xf9400009, 0x5280060a, 0x91002129, 0xb900012a, 0xf9400009, 0x528cdb4a,
                        0x72a0088a, 0x91003129, 0xb900012a, 0xf9400009, 0x91004129, 0xb9000128,
                        0x3941e408, 0x52800069, 0xb905d809, 0x34000348, 0xf9400269, 0x2a1f03e8,
                        0x91010129, 0xb9000128, 0xf9400269, 0x9101c129, 0xb9000128, 0xf9400269,
                        0x91028129, 0xb9000128, 0xf9400269, 0x91034129, 0xb9000128, 0xf9400269,
                        0x91040129, 0xb9000128, 0xf9400269, 0x9104c129, 0xb9000128, 0xf9400269,
                        0x91058129, 0xb9000128, 0xf9400269, 0x91064129, 0xb9000128, 0xf9400269,
                        0x2a1f03e8, 0x529fffea, 0xaa1303e0, 0x91200129, 0xb9000128, 0xf9400269,
                        0x9120f129, 0xb9000128, 0xf9400269, 0x91210129, 0xb900012a, 0xf9400269,
                        0x91211129, 0xb9000128, 0xf9400268, 0x12bfffc9, 0x91212108, 0xb900010a,
                        0xf9400268, 0x91203108],
              [w for mode in MODES for w in _record(*mode)])

# get_modes: after drm_add_edid_modes (0x8c7c, w0 its count) or, without an EDID,
# after the "get edid failed" message (0x8ca0), to GET_MODES. x19 is the connector,
# x20 the EDID; the prologue saved both, and x30. The code saves the x21-x25 it uses.
GET_MODES_HOOKS = [
    (0x8c7c, [0x2a0003f3], [0x14005d90]),   # b 0x202bc (was mov w19, w0)
    (0x8ca0, [0x2a1f03f3], [0x14005d85]),   # b 0x202b4 (was mov w19, wzr)
]
# For each record whose gate opens: drm_mode_create, the record copied in,
# drm_mode_probed_add. No relocation is added: both are called through the bl words
# sprd_panel_get_modes has for them (0x10dec, 0x10ce8), whose offsets the module
# loader has filled in. After the first add, every mode on the connector's list gets
# picture_aspect_ratio (+188) 0, so DRM's merge of equal modes (drm_mode_equal
# compares it) takes a CEA mode the EDID lists and the table's as one. Then the
# count in w19, as 0x8c7c left it, and back to kfree(edid) and the return.
GET_MODES = (0x202b4, [0xf800865e, 0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf9400009, 0x2a1f03e8,
                       0xaa0003f3, 0x9100b129, 0xb9000128, 0xb9404809, 0xb940380a, 0xf940000b,
                       0x2a094149, 0x9100816a, 0xb9000149, 0xf940000a, 0x9100914a, 0xb9000149,
                       0xf9400009, 0x91002129, 0xb9000128, 0x39605c08, 0x39605809, 0x3960540a,
                       0xf940000b, 0x53144d08, 0x2a092108, 0x39605009, 0x2a0a1108, 0x52a0088a,
                       0x9100316b, 0x2a090108, 0x2a0a0108, 0xb9000168, 0xf9400008, 0x52880049,
                       0x72a00029, 0x91004108, 0xb9000109, 0x3941e408, 0x34000348, 0xf9400269,
                       0x2a1f03e8, 0x91010129, 0xb9000128, 0xf9400269, 0x9101c129, 0xb9000128,
                       0xf9400269, 0x91028129, 0xb9000128, 0xf9400269, 0x91034129, 0xb9000128,
                       0xf9400269, 0x91040129, 0xb9000128, 0xf9400269, 0x9104c129, 0xb9000128,
                       0xf9400269, 0x91058129, 0xb9000128, 0xf9400269, 0x91064129, 0xb9000128,
                       0xf9400268, 0x2a1f03f4, 0x529fffe9, 0x12bfffca, 0x91200108, 0xb9000114,
                       0xf9400268, 0xaa1303e0, 0x9120f108, 0xb9000114, 0xf9400268, 0x91210108,
                       0xb9000109, 0xf9400268, 0x91211108, 0xb9000114, 0xf9400268, 0x91212108,
                       0xb9000109, 0xf9400268, 0x91203108, 0xb900010a],
             [0xaa1f03f4,               # mov  x20, xzr            (no EDID: kfree(NULL) is harmless)
              0x2a1f03e0,               # mov  w0, wzr             (no modes yet)
              0xd100c3ff,               # 0x202bc: sub sp, sp, #0x30
              0xa9005bf5,               # stp  x21, x22, [sp]
              0xa90163f7,               # stp  x23, x24, [sp, #16]
              0xf90013f9,               # str  x25, [sp, #32]
              0x2a0003f6,               # mov  w22, w0             (the count so far)
              0x52800098,               # mov  w24, #4             (records; 3 without WIDE)
              0xb5000054,               # cbnz x20, 0x202dc
              0x52800058,               # mov  w24, #2             (no EDID: 720p60 and 1080p60, no gates)
              0x10012635,               # 0x202dc: adr x21, 0x227a0 (the table)
              0xaa1f03f7,               # mov  x23, xzr            (the list head: known after the first add)
              0xb9404aa9,               # 0x202e4: ldr w9, [x21, #72] (gate size)
              0x340003e9,               # cbz  w9, 0x20364         (no gate: add it)
              0xb40003d4,               # cbz  x20, 0x20364        (no EDID: add it)
              0xb4000817,               # cbz  x23, 0x203f0        (nothing added yet: skip)
              0xb9404eab,               # ldr  w11, [x21, #76]     (gate rate)
              0x12003d2f,               # and  w15, w9, #0xffff    (width)
              0x53107d30,               # lsr  w16, w9, #16        (height)
              0xf94002ec,               # ldr  x12, [x23]          (the first mode)
              0xeb17019f,               # 0x20304: cmp x12, x23
              0x54000740,               # b.eq 0x203f0             (back at the head: none opens it, skip)
              0xb9403d8d,               # ldr  w13, [x12, #60]     (hdisplay)
              0xb940518e,               # ldr  w14, [x12, #80]     (vdisplay)
              0x36f8008b,               # tbz  w11, #31, 0x20324   (at least the size)
              0x6b0f01bf,               # cmp  w13, w15
              0x7a5001c0,               # ccmp w14, w16, #0, eq
              0x540001e1,               # b.ne 0x2035c             (not exactly the size: next)
              0x6b0f01bf,               # 0x20324: cmp w13, w15
              0x7a5021c0,               # ccmp w14, w16, #0, hs
              0x54000183,               # b.lo 0x2035c             (smaller: next)
              0xb940498d,               # ldr  w13, [x12, #72]     (htotal)
              0xb9405d8e,               # ldr  w14, [x12, #92]     (vtotal)
              0x1b0e7dad,               # mul  w13, w13, w14
              0xb940398e,               # ldr  w14, [x12, #56]     (clock)
              0x52807d11,               # mov  w17, #1000
              0x1b117dce,               # mul  w14, w14, w17
              0x0b4d05ce,               # add  w14, w14, w13, lsr #1
              0x1acd09ce,               # udiv w14, w14, w13       (Hz, rounded; by 0 gives 0)
              0x12001d71,               # and  w17, w11, #0xff
              0x6b1101df,               # cmp  w14, w17
              0x54000062,               # b.hs 0x20364             (fast enough: add it)
              0xf940018c,               # 0x2035c: ldr x12, [x12]  (next)
              0x17ffffe9,               # b    0x20304
              0xf9400260,               # 0x20364: ldr x0, [x19]   (connector->dev)
              0x10f85429,               # adr  x9, 0x10dec         (sprd_panel_get_modes's bl drm_mode_create)
              0xb940012a,               # ldr  w10, [x9]
              0x9340654a,               # sbfx x10, x10, #0, #26   (its offset, as the loader filled it in)
              0x8b0a0929,               # add  x9, x9, x10, lsl #2
              0xd63f0120,               # blr  x9                  (drm_mode_create(dev): a zeroed mode)
              0xb40003a0,               # cbz  x0, 0x203f0
              0xaa0003f9,               # mov  x25, x0
              0xa9402aa9,               # ldp  x9, x10, [x21]      (name)
              0xa9012b29,               # stp  x9, x10, [x25, #16]
              0xa9412aa9,               # ldp  x9, x10, [x21, #16] (clock ... hsync_end)
              0xa903ab29,               # stp  x9, x10, [x25, #56]
              0xa9422aa9,               # ldp  x9, x10, [x21, #32] (htotal ... vsync_start)
              0xa904ab29,               # stp  x9, x10, [x25, #72]
              0xa9432aa9,               # ldp  x9, x10, [x21, #48] (vsync_end ... flags)
              0xa905ab29,               # stp  x9, x10, [x25, #88]
              0x29482aa9,               # ldp  w9, w10, [x21, #64] (vrefresh, type)
              0xb900b729,               # str  w9, [x25, #180]
              0xb900372a,               # str  w10, [x25, #52]
              0xaa1303e0,               # mov  x0, x19
              0xaa1903e1,               # mov  x1, x25
              0x10f84989,               # adr  x9, 0x10ce8         (its bl drm_mode_probed_add)
              0xb940012a,               # ldr  w10, [x9]
              0x9340654a,               # sbfx x10, x10, #0, #26
              0x8b0a0929,               # add  x9, x9, x10, lsl #2
              0xd63f0120,               # blr  x9                  (drm_mode_probed_add(connector, mode))
              0x110006d6,               # add  w22, w22, #1
              0xb5000117,               # cbnz x23, 0x203f0
              0xf9400337,               # ldr  x23, [x25]          (added at the tail: its next is the head)
              0xaa1703ec,               # mov  x12, x23
              0xf940018c,               # 0x203dc: ldr x12, [x12]
              0xeb17019f,               # cmp  x12, x23
              0x54000060,               # b.eq 0x203f0
              0xb900bd9f,               # str  wzr, [x12, #188]    (picture_aspect_ratio: none)
              0x17fffffc,               # b    0x203dc
              0x910142b5,               # 0x203f0: add x21, x21, #0x50 (the next record)
              0x71000718,               # subs w24, w24, #1
              0x54fff761,               # b.ne 0x202e4
              0x2a1603f3,               # mov  w19, w22            (the count, as 0x8c7c left it)
              0xa9405bf5,               # ldp  x21, x22, [sp]
              0xa94163f7,               # ldp  x23, x24, [sp, #16]
              0xf94013f9,               # ldr  x25, [sp, #32]
              0x9100c3ff,               # add  sp, sp, #0x30
              0x17ffa21c])              # b    0x8c80              (kfree(edid), return w19)

# mode_valid, after the 299 MHz cap: MODE_FILTER instead of the link check (0x8ce8,
# word 3 of the 0x8cdc patch), and after the width check MODE_OK instead of the CEA
# code checks (0x8e3c, word 0 of its patch; 0x8e54 returns 0). x19 is the mode, x20
# the connector, w10 the clock; x11-x16 are free (the link check sets the ones it
# uses). A mode whose twelve words +56..+103 are exactly a record's gets the
# record's vrefresh and type and goes on to the link and width checks; any other
# is MODE_CLOCK_HIGH (0x8dd8).
MODE_FILTER = (0x2497c, [0xf800865e, 0xa9be7bfd, 0xa9014ff4, 0x910003fd, 0xf9400009, 0x2a1f03e8,
                         0xf9406c14, 0xaa0003f3, 0x9100b129, 0xb9000128, 0xb9404809, 0xb940380a,
                         0xf940000b, 0x2a094149, 0x9100816a, 0xb9000149, 0xf940000a, 0x9100914a,
                         0xb9000149, 0xf9400009, 0x91002129, 0xb9000128, 0x39605c08, 0x39605809,
                         0x3960540a],
               [0x10fef12b,             # adr  x11, 0x227a0        (the table)
                0x5280008c,             # mov  w12, #4             (records; 3 without WIDE)
                0xa943ba6d,             # 0x24984: ldp x13, x14, [x19, #56] (clock ... hsync_end)
                0xa941416f,             # ldp  x15, x16, [x11, #16]
                0xeb0f01bf,             # cmp  x13, x15
                0xfa5001c0,             # ccmp x14, x16, #0, eq
                0x540001e1,             # b.ne 0x249d0
                0xa944ba6d,             # ldp  x13, x14, [x19, #72] (htotal ... vsync_start)
                0xa942416f,             # ldp  x15, x16, [x11, #32]
                0xeb0f01bf,             # cmp  x13, x15
                0xfa5001c0,             # ccmp x14, x16, #0, eq
                0x54000141,             # b.ne 0x249d0
                0xa945ba6d,             # ldp  x13, x14, [x19, #88] (vsync_end ... flags)
                0xa943416f,             # ldp  x15, x16, [x11, #48]
                0xeb0f01bf,             # cmp  x13, x15
                0xfa5001c0,             # ccmp x14, x16, #0, eq
                0x540000a1,             # b.ne 0x249d0
                0x2948396d,             # ldp  w13, w14, [x11, #64] (vrefresh, type)
                0xb900b66d,             # str  w13, [x19, #180]
                0xb900366e,             # str  w14, [x19, #52]
                0x17ff90fb,             # b    0x8db8              (the link and width checks)
                0x9101416b,             # 0x249d0: add x11, x11, #0x50 (the next record)
                0x7100058c,             # subs w12, w12, #1
                0x54fffd61,             # b.ne 0x24984
                0x17ff90ff])            # b    0x8dd8              (not in the table: MODE_CLOCK_HIGH)
MODES_PATCHES = GET_MODES_HOOKS + [GET_MODES, MODE_TABLE, MODE_FILTER]

# Words of other patches that all_modes changes:
# (address of the patch, index of the word, the word without and with all_modes)
ALL_MODES_WORDS = [
    (0x8cdc, 3, 0x14000034, 0x14006f25),    # b 0x8db8 -> b 0x2497c    (MODE_FILTER)
    (0x8e3c, 0, 0xb9403668, 0x14000006),    # ldr w8, [x19, #52] -> b 0x8e54 (MODE_OK)
]
# Without WIDE the table has three records, without 2560x1440:
# (address of the patch, index of the word, the word with WIDE and without)
NREC_WORDS = [
    (0x202b4, 7, 0x52800098, 0x52800078),   # mov w24, #4 -> mov w24, #3 (GET_MODES)
    (0x2497c, 1, 0x5280008c, 0x5280006c),   # mov w12, #4 -> mov w12, #3 (MODE_FILTER)
]
# Applied only with all_modes. A monitor's mode changes after the connect, and the
# mode table, are both only for the 1.4.4 base, whose framework holds a monitor at a
# mode of its own; the 1.2 base and recovery keep the mode the composer starts with.
ALL_MODES_PATCHES = MODE_CHANGE_PATCHES + MODES_PATCHES

# 1440p through DPU1's bypass (test 42; test42_plan.md 2.4). In normal mode the
# display controller drives one pixel per DPI clock and nothing wider than 1920
# has shown a picture; in bypass it drives two per clock with one layer, so a
# 2560-wide mode at 297 MHz needs a 148.5 MHz DPI clock (PIXELPLL 2673 MHz, the
# only setting that has worked). Only with all_modes, as the 1.2 base and
# recovery never leave 1080p.
#
# P2: dpu_modeset put only exactly 3840x2160 into bypass. Anything wider than
# 1920 goes there now (w8 is hactive, loaded at 0xb37c; b.ls to 0xb3a4 is normal
# mode, falling through to 0xb394 is "bypass_mode" and ctx+428 = 1).
P2 = (0xb380, [0x713c011f, 0x54000101, 0xb9403a88, 0x7121c11f, 0x540000a1],
              [0x711e011f,              # cmp  w8, #0x780
               0x54000109,              # b.ls 0xb3a4              (1920 or narrower: normal mode)
               NOP, NOP, NOP])
# FLIP: bypass scans out hardware layer 7 only. The vendor 4.14 driver's bypass
# branch in dpu_flip set planes[0]'s layer index to 7; the 5.4 port wrote index
# 0 into planes[7].state (planes + 0x2a40, past DPU1's four planes: an
# out-of-bounds read and a stray one-byte write), so the one layer stayed on
# hardware layer 0 and bypass showed black (test 42 MOD-P: a locked 2560x1440
# signal, no picture). The branch now stores 7 in planes[0].state's layer.index
# (x21 is the planes array, as at 0xb048; atomic_update rewrites the index from
# plane->index on every commit, so normal mode is as before). It covers the
# layer-count check (cmp w2, #1; b.ne to its printk at 0xb0c8): a second DPU1
# layer in bypass is silently not shown, so SurfaceFlinger must give DPU1 one
# layer. w9 is dead here (0xaf08 and the path from 0xb03c set x9 before reading it).
FLIP = (0xaef0, [0x7100045f, 0x54000ea1, 0xf95522a8, 0x3902611f],
                [0xf9426ea8,            # ldr  x8, [x21, #1240]    (planes[0].state)
                 0x528000e9,            # mov  w9, #7
                 0x39026109,            # strb w9, [x8, #152]      (layer.index = 7)
                 NOP])
WIDE_PATCHES = [P2, FLIP]
# MP: sprd_dp_encoder_mode_set sets the DPTX's pixel mode (dptx+3461; VSAMPLE_CTRL
# bits 22:21, 0 single, 1 dual) to the bypass flag, so the DPTX takes the two
# pixels per clock bypass sends (test 42 A4's no-picture result is no evidence for
# or against it: A4 never presented a frame, the composer dropped it;
# dp_dock_findings.md, the correction to A4). It replaces PATCHES' 0x90f4 entry (the same NULL-framebuffer fix,
# one word up, its nop gone) and starts at 0x90f0, the stock cbz on the flag.
# x20 is the dptx, w8 the bypass flag; every modeset writes it, so going back to
# a mode 1920 or narrower writes 0 again.
WIDE = True       # P2, FLIP and the table's 2560x1440, with all_modes
WIDE_MP = True    # MP and TH40, with WIDE


def _mp():
    old = [a for a in PATCHES if a[0] == 0x90f4][0][1]
    return (0x90f0, [0x34000288] + old,
            [0x39361688,                # strb w8, [x20, #3461]    (pixel mode = bypass flag)
             0x34000248,                # cbz  w8, 0x913c          (not bypass: RGB)
             0xf94042c8,                # ldr  x8, [x22, #128]
             0xf9426d08,                # ldr  x8, [x8, #1240]
             0xf9400908,                # ldr  x8, [x8, #16]
             0xb40001c8,                # cbz  x8, 0x913c          (no framebuffer: RGB)
             0xf9402508,                # ldr  x8, [x8, #72]
             0xb940010a,                # ldr  w10, [x8]           (fourcc)
             0x52800028,                # mov  w8, #1
             0x528ac9c9,                # mov  w9, #0x564e
             0x72a64629,                # movk w9, #0x3231, lsl #16 (NV12)
             0x6b09015f,                # cmp  w10, w9
             0x54000100,                # b.eq 0x9140
             0x52860a09,                # mov  w9, #0x3050
             0x72a60629,                # movk w9, #0x3031, lsl #16 (P010)
             0x6b09015f,                # cmp  w10, w9
             0x54000061,                # b.ne 0x913c
             0x52800149,                # mov  w9, #10
             0x14000003,                # b    0x9144
             0x2a1f03e8,                # 0x913c: mov w8, wzr      (RGB)
             0x52800109])               # 0x9140: mov w9, #8


MP = _mp()
# TH40: dptx_video_ts_calculate's init threshold (dptx+3510), which it picks after
# the transfer unit. Stock: 16 on the large-blanking path (hblank, dptx+3480,
# over 80, or w20 neither 0 nor 3 at 0x372e0), and on the small-blanking path 4
# when the pixel mode (dptx+3461) is 2, quad, else 12; the 32 at 0x372d8 stays as
# it is. Now: 40 when the pixel mode is dual (1), as
# upstream dw-dp uses for multi-pixel modes, 16 otherwise on the large-blanking
# path, and 12 on the small-blanking path (the quad 4 is gone: only MP's 0 or 1
# is ever written to dptx+3461). The entries stay where they were: 0x372f8 by
# falling through (small), 0x3730c from 0x372e8 and 0x372f4 (large). A dual pixel
# mode with hblank 80 or less would still take the small path and get 12;
# 2560x1440 at 297 MHz has an hblank of 786. Only with MP, which sets the pixel
# mode; without it dptx+3461 stays 0 and this is the stock 16 and 12. w9 is dead
# after 0x37318 (the printk call there).
TH40 = (0x372f8, [0x39761668, 0x7100091f, 0x540000a1, 0x52800088,
                  0x14000004, 0x52800208, 0x14000002, 0x52800188],
                 [0x52800188,           # mov  w8, #12             (small blanking)
                  0x14000007,           # b    0x37318
                  0x52800508,           # 0x37300: mov w8, #40     (dual pixel mode)
                  0x14000005,           # b    0x37318
                  NOP,
                  0x39761669,           # 0x3730c: ldrb w9, [x19, #3461] (large blanking: pixel mode)
                  0x52800208,           # mov  w8, #16
                  0x35ffff69])          # cbnz w9, 0x37300
WIDE_MP_PATCHES = [MP, TH40]

# UHD: CEA 3840x2160 at 30 Hz (VIC 95, 297 MHz, +h+v) through bypass, as a fifth
# record, gated on the EDID listing exactly 3840x2160 at 29 Hz or more (test 42
# B8: a Hisense TV whose HDMI 1.4 EDID prefers 4K30). Bypass sends two pixels per
# DPI clock, so this is the same 148.5 MHz DPI clock and dual-pixel DPTX as
# 2560x1440; it needs WIDE and WIDE_MP. The width check lets exactly 3840 through
# (b.hi, wider than 3840, as before test 41); the mode table keeps every other
# width out, DCI 4096x2160 included.
#
# The four records fill MODE_TABLE's cave up to 0x228e0, and from 0x228f0 on the
# rest of dpu_init.1618 carries relocations the module loader writes over. So the
# fifth record is at 0x249e0, after MODE_FILTER in dpu_init.1724 (whose first
# relocation is at 0x24a9c), and each loop's "next record" branch goes through a
# five-word step: past the 2560x1440 record (x == 0x228e0) it continues at 0x249e0.
# Both loops count down before the step, so with four records or fewer they end
# before reaching it. x9 is free in GET_MODES's loop (0x202e4 loads it), x13 in
# MODE_FILTER's (0x24984 loads it).
UHD = True        # the 3840x2160@30 record and width check, with WIDE and WIDE_MP
UHD_MODE = ("3840x2160", 297000, (3840, 4016, 4104, 4400), (2160, 2168, 2178, 2250), 0x5, 30,
            0x60, (3840, 2160, 29, True))                     # CEA 95
UHD_PATCH = (0x249e0,
             [0xf940000b, 0x53144d08, 0x2a092108, 0x39605009, 0x2a0a1108, 0x52a0088a,
              0x9100316b, 0x2a090108, 0x2a0a0108, 0xb9000168, 0xf9400008, 0x52880049,
              0x72a00029, 0x91004108, 0xb9000109, 0x3941e408, 0x34000348, 0xf9400269,
              0x2a1f03e8, 0x91010129, 0xb9000128, 0xf9400269, 0x9101c129, 0xb9000128,
              0xf9400269, 0x91028129, 0xb9000128, 0xf9400269, 0x91034129, 0xb9000128],
             _record(*UHD_MODE)
             + [0x10fef589,             # 0x24a30: adr x9, 0x228e0 (past the 2560x1440 record)
                0xeb0902bf,             # cmp  x21, x9
                0x54fdc561,             # b.ne 0x202e4             (GET_MODES: the next record)
                0x10fffd35,             # adr  x21, 0x249e0        (the 3840x2160 record)
                0x17ffee29,             # b    0x202e4
                0x10fef4ed,             # 0x24a44: adr x13, 0x228e0
                0xeb0d017f,             # cmp  x11, x13
                0x54fff9c1,             # b.ne 0x24984             (MODE_FILTER: the next record)
                0x10fffc8b,             # adr  x11, 0x249e0
                0x17ffffcc])            # b    0x24984
# (address of the patch, index of the word, the word without UHD and with it)
UHD_WORDS = [
    (0x8e14, 6, 0x54fffd62, 0x54fffd68),    # b.hs 0x8dd8 -> b.hi 0x8dd8 (exactly 3840 passes)
    (0x202b4, 7, 0x52800098, 0x528000b8),   # mov w24, #4 -> mov w24, #5 (GET_MODES)
    (0x202b4, 81, 0x54fff761, 0x540231c1),  # b.ne 0x202e4 -> b.ne 0x24a30 (the step)
    (0x2497c, 1, 0x5280008c, 0x528000ac),   # mov w12, #4 -> mov w12, #5 (MODE_FILTER)
    (0x2497c, 23, 0x54fffd61, 0x54000361),  # b.ne 0x24984 -> b.ne 0x24a44 (the step)
]

# Y420: CEA 3840x2160 at 60 Hz (VIC 97, 594 MHz, +h+v) as YCbCr 4:2:0 8 bpc, for one
# full-screen NV12 video layer only (display_modes_plan.md section 8; test 42 design in
# out/dp-evidence/test42/y420/SYNTHESIS.md). Bypass halves the DPI clock (297 MHz, pixel PLL
# 2673 MHz, as 1080p120); 4:2:0 halves the link load (12 bpp, 7.13 Gbit/s, 82.5% of 2-lane HBR2).
#
# Mode side: a sixth record, gated on the EDID listing exactly 3840x2160 at 59 Hz or more and on
# the sink taking 4:2:0 for VIC 97 (display_info.color_formats, connector+196, bit 3; bit 97 of
# hdmi.y420_vdb_modes | y420_cmdb_modes, connector+240/+256). GET_MODES sets
# connector->ycbcr_420_allowed (+127), without which drm_mode_validate_ycbcr420 drops a VIC 97
# the EDID lists only in a Y420 data block. (The offsets are kernel 5.4's struct drm_connector
# layout; the vendor code's own +196 read of color_formats pins every field before it.)
# mode_valid's clock cap goes up to 598 MHz (MODE_FILTER still admits only the table's timings),
# and a matched record above 299 MHz is checked for 4:2:0 again and counted at 12 bpp. Never
# preferred: CEA 1080p60's gate and link need are implied by 4K60's. The record and three stubs
# are in dpu_init.1455 (0x1f228-0x1f2eb), dpu_lite_r1p0's init, reached only through CFI slot
# 0x3f2ac, used only by dpu_lite_r1p0_core_ops (sharkle and pike2 match data).
#
# Output side: the colour format follows the mode, never the framebuffer. In bypass above
# 299008 kHz: YCbCr 4:2:0, 8 bpc, single pixel (the vendor design); other bypass modes RGB dual
# pixel (as MP); normal mode RGB single pixel. Replaces MP at 0x90f0. The VSC SDP says BT.709,
# CTA range, 8 bpc, and always takes sdp_list slot 0 (stock used a new one of 16 per enable); an
# RGB enable clears slot 0's SDP_VERTICAL_CTRL bit (in dptx_set_pixel_enc, which nothing calls);
# MSA MISC0 says 8 bpc. At 4K60, dpu_flip shows planes[0] only, and only if it is NV12, else no
# layer (Y420_GUARD), so an RGB frame is never fetched at 594 Mpix/s. It keys on a flag dpu_modeset
# stores next to the bypass byte (ctx+429, Y420_FLAG), not on ctx->vm.pixelclock: DPU1's crtc
# mode_valid (0xa278) rewrites ctx->vm whenever DRM's probe validates CEA 1080p60, so after any
# re-probe at 4K60 the clock read 148.5 MHz (Y420 review, test 42). It respects the caller's layer
# count (no layer is enabled for a count of 0), and it lives in dptx_set_pixel_enc, so the HDR block
# at 0xaf00-0xb03b stays stock: sprd_dp_connector_atomic_check does set ctx+468 when userspace
# sends HDR_OUTPUT_METADATA.
Y420 = True       # 3840x2160@60 YCbCr 4:2:0, with UHD (test 42 ships MOD-Y1-E)
Y420_MODE = ("3840x2160", 594000, (3840, 4016, 4104, 4400), (2160, 2168, 2178, 2250), 0x5, 60,
             0x60, (3840, 2160, 59, True))                    # CEA 97
Y420_PATCH = (0x1f228,
              [0xf9400009, 0x321c0108, 0x91059129, 0xb9000128, 0xb9404808, 0xb9403809, 0xf940000a,
               0x2a084128, 0x91002149, 0xb9000128, 0xf9400008, 0x52823e09, 0x72a00089, 0x91005108,
               0xb9000109, 0xf9400008, 0x52800029, 0x91054108, 0xb9000109, 0xf9400008, 0x52802009,
               0x91055108, 0xb9000109, 0xf9400008, 0x52802009, 0x72a01009, 0x91056108, 0xb9000109,
               0xf9400008, 0x91057108, 0xb9000109, 0x3941e408, 0x34000288, 0xf9400009, 0x2a1f03e8,
               0x9100c129, 0xb9000128, 0xf9400009, 0x91018129, 0xb9000128, 0xf9400009, 0x91024129,
               0xb9000128, 0xf9400009, 0x91030129, 0xb9000128, 0xf9400009, 0x9103c129, 0xb9000128],
              _record(*Y420_MODE)
              + [0x1002bdc9,            # 0x1f278: adr x9, 0x24a30 (past the 3840x2160@30 record)
                 0xeb0902bf,            # cmp  x21, x9
                 0x5402bd81,            # b.ne 0x24a30             (UHD's step, as before)
                 0x52800029,            # mov  w9, #1
                 0x3901fe69,            # strb w9, [x19, #127]     (connector->ycbcr_420_allowed)
                 0x39431269,            # ldrb w9, [x19, #196]     (display_info.color_formats)
                 0x36188b69,            # tbz  w9, #3, 0x203fc     (no YCbCr 4:2:0: done)
                 0xf9407e69,            # ldr  x9, [x19, #248]     (hdmi.y420_vdb_modes, VICs 64-127)
                 0xf940866a,            # ldr  x10, [x19, #264]    (hdmi.y420_cmdb_modes, VICs 64-127)
                 0xaa0a0129,            # orr  x9, x9, x10
                 0xb6088ae9,            # tbz  x9, #33, 0x203fc    (VIC 97 not 4:2:0: done)
                 0x10fffc35,            # adr  x21, 0x1f228        (the 3840x2160@60 record)
                 0x1400040f,            # b    0x202e4
                 0x1002bc2d,            # 0x1f2ac: adr x13, 0x24a30 (MODE_FILTER's step)
                 0xeb0d017f,            # cmp  x11, x13
                 0x5402bc81,            # b.ne 0x24a44             (UHD's step)
                 0x10fffb8b,            # adr  x11, 0x1f228
                 0x140015b2,            # b    0x24984
                 0x7141255f,            # 0x1f2c0: cmp w10, #0x49, lsl #12 (above 299 MHz: 4:2:0)
                 0x54f4d7a9,            # b.ls 0x8db8              (no: the link and width checks)
                 0x3943128d,            # ldrb w13, [x20, #196]
                 0x361800ed,            # tbz  w13, #3, 0x1f2e8
                 0xf9407e8d,            # ldr  x13, [x20, #248]
                 0xf940868e,            # ldr  x14, [x20, #264]
                 0xaa0e01ad,            # orr  x13, x13, x14
                 0xb608006d,            # tbz  x13, #33, 0x1f2e8
                 0x53017d4a,            # lsr  w10, w10, #1        (12 bpp: the check's 3 * clock is 3/2)
                 0x17ffa6b5,            # b    0x8db8
                 0x17ffa6bc])           # 0x1f2e8: b 0x8dd8        (MODE_CLOCK_HIGH)
Y420_MP = (0x90f0, MP[1],               # replaces MP (same original words)
           [0x39361688,                 # strb w8, [x20, #3461]    (pixel mode = bypass flag)
            0x34000248,                 # cbz  w8, 0x913c          (not bypass: RGB)
            0xb9403a6a,                 # ldr  w10, [x19, #56]     (mode->clock, kHz)
            0x7141255f,                 # cmp  w10, #0x49, lsl #12 (299008)
            0x540001e9,                 # b.ls 0x913c              (RGB, dual pixel)
            0x3936169f,                 # strb wzr, [x20, #3461]   (4:2:0: single pixel)
            0x52800028,                 # mov  w8, #1              (YCBCR420)
            0x1400000d]                 # b    0x9140              (8 bpc)
           + [NOP] * 11 +
           [0x2a1f03e8,                 # 0x913c: mov w8, wzr      (RGB)
            0x52800109])                # 0x9140: mov w9, #8
Y420_RGB_HOOK = (0x943c, [0x54000761], [0x54142dc1])   # b.ne 0x9528 -> b.ne 0x319f4
Y420_VSC_DB = (0x945c, [0x52a60039], [0x52a63039])     # mov w25, #0x30010000 -> #0x31810000
Y420_SLOT0 = (0x9510, [0x52802008, 0x910043e1, 0xaa1303e0, 0x780353e8],
              [0x913b8668,              # add  x8, x19, #0xee1     (sdp_list[0].payload[0])
               0x910043e1,              # add  x1, sp, #0x10
               0xaa1303e0,              # mov  x0, x19
               0xb900011f])             # str  wzr, [x8]           (bl dptx_enable_sdp follows)
Y420_RGB_CAVE = (0x319f4, [0x128002a0, 0x1400004d, 0x52822ae8, 0x8b080008, 0x3976c403, 0x39400501],
                 [0xf946b668,           # ldr  x8, [x19, #3432]    (dptx registers)
                  0xb9450109,           # ldr  w9, [x8, #0x500]    (SDP_VERTICAL_CTRL)
                  0x121d7929,           # and  w9, w9, #0xfffffffb (slot 0's VSC off)
                  0xd50332bf,           # dmb  oshst
                  0xb9050109,           # str  w9, [x8, #0x500]
                  0x17ff5ec8])          # b    0x9528
Y420_MISC0 = (0x36fb8, [0x2a1f03e8], [0x52a40008])     # mov w8, wzr -> #0x20000000 (MISC0 8 bpc)
# Y420_FLAG: dpu_modeset's bypass store (strb w8, [x20, #428]) becomes a strh of 0x0101 in
# bypass above 299008 kHz (ctx+428 = 1 as before, ctx+429 = 1: 4K60 4:2:0), 0x0001 in other
# bypass modes and 0 in normal mode. x19 is the mode (0xb368), callee-saved across the bl at 0xb378;
# P2's three spare words load and compare its clock (Y420_WORDS), the adrp/add at 0xb394/0xb398
# leave the flags alone, and nothing else touches ctx+429.
Y420_FLAG = [(0xb39c, [0x52800028], [0x1a9f8508]),    # mov w8, #1 -> csinc w8, w8, wzr, hi
             (0xb3b0, [0x3906b288], [0x79035a88])]    # strb w8, [x20, #428] -> strh
# Y420_GUARD: FLIP's spare word (0xaefc) branches here (Y420_GUARD_WORDS), into
# dptx_set_pixel_enc (never called; its .altinstructions entries at 0x31990, 0x31a38, 0x31a88 and
# 0x31adc lie outside 0x31a8c-0x31abb). x19 is the ctx, x21 the planes, w2 the layer count.
Y420_GUARD = (0x31a8c,
              [0xd50331bf, 0x2a0803ea, 0xca0a014a, 0xb500000a, 0x3976d28a, 0x3976d68b,
               0x3976da8c, 0xd50332bf, 0xf946b68d, 0x0a090108, 0x33001948, 0x2a0b4108],
              [0x3946b669,              # ldrb w9, [x19, #429]     (dpu_modeset's 4K60 flag)
               0x34eca389,              # cbz  w9, 0xaf00          (not 4K60: as before)
               0x34eca362,              # cbz  w2, 0xaf00          (count 0: no layer)
               0xf9426ea8,              # ldr  x8, [x21, #1240]    (planes[0].state)
               0xb940cd09,              # ldr  w9, [x8, #204]      (layer.format)
               0x528ac9ca,              # mov  w10, #0x564e
               0x72a6462a,              # movk w10, #0x3231, lsl #16 (NV12)
               0x6b0a013f,              # cmp  w9, w10
               0x52800034,              # mov  w20, #1             (planes[0] only)
               0x54eca280,              # b.eq 0xaf00
               0x2a1f03f4,              # mov  w20, wzr            (not NV12: no layer)
               0x17ff6512])             # b    0xaf00
# With the guard only, so y420_guard=False is the module test 42 ran as MOD-Y1.
# Y420_GUARDED is y420_guard's default: False, the module test 42 ships (MOD-Y1 with fix E,
# MOD-Y1-E, 6eefac58). MOD-Y1 showed 4K60 with the NV12 SurfaceFlinger (T3); MOD-Y1-E passed sleep
# and wake and unplug and replug while asleep at 1080p60 (out/dp-evidence/test42/irqfix/NOTES.md,
# run 1); 4K30, 4K60, 1440p and 1080p120 have not run again with fix E. True adds Y420_GUARD and
# Y420_FLAG, which never ran on a Cube with the NV12 SurfaceFlinger: do not ship it untested.
Y420_GUARDED = False
Y420_GUARD_PATCHES = Y420_FLAG + [Y420_GUARD]
Y420_GUARD_WORDS = [
    (0xaef0, 3, NOP, 0x14009ae4),           # FLIP's nop -> b 0x31a8c
    (0xb380, 2, NOP, 0xb9403a69),           # P2's nop -> ldr w9, [x19, #56] (mode->clock, kHz)
    (0xb380, 3, NOP, 0x7141253f),           # nop -> cmp w9, #0x49, lsl #12 (299008)
    (0xb380, 4, NOP, 0x52802028),           # nop -> mov w8, #0x101
]
Y420_PATCHES = [Y420_PATCH, Y420_MP, Y420_RGB_HOOK, Y420_VSC_DB, Y420_SLOT0, Y420_RGB_CAVE,
                Y420_MISC0]
# (address of the patch, index of the word, the word with UHD and with Y420); applied after
# UHD_WORDS, so each "without" word is UHD's "with" word.
Y420_WORDS = [
    (0x8cdc, 1, 0x7141255f, 0x7142495f),    # cmp w10, #0x49, lsl #12 -> #0x92 (598016 kHz cap)
    (0x202b4, 7, 0x528000b8, 0x528000d8),   # mov w24, #5 -> #6 (GET_MODES)
    (0x202b4, 81, 0x540231c1, 0x54ff7401),  # b.ne 0x24a30 -> b.ne 0x1f278
    (0x2497c, 1, 0x528000ac, 0x528000cc),   # mov w12, #5 -> #6 (MODE_FILTER)
    (0x2497c, 20, 0x17ff90fb, 0x17ffea3d),  # b 0x8db8 -> b 0x1f2c0 (12 bpp and the 4:2:0 recheck)
    (0x2497c, 23, 0x54000361, 0x54fd46a1),  # b.ne 0x24a44 -> b.ne 0x1f2ac
]


def patches(all_modes, wide=None, wide_mp=None, uhd=None, y420=None, y420_guard=None,
            irq_mask=None):
    """Every (address, original words, patched words), in address order.

    wide, wide_mp, uhd and y420 default to WIDE, WIDE_MP, UHD and Y420; wide needs all_modes,
    wide_mp needs wide, uhd needs wide_mp and y420 needs uhd. y420_guard defaults to
    Y420_GUARDED (False: no Y420_GUARD, the module test 42 tested and ships); True adds it, an
    untested variant. irq_mask defaults to IRQ_MASK (fix E, in every variant); False gives test
    42's modules up to MOD-Y1. So patches(True) is MOD-Y1-E, patches(False) recovery's."""
    y420_guard = Y420_GUARDED if y420_guard is None else y420_guard
    wide = WIDE if wide is None else wide
    wide_mp = WIDE_MP if wide_mp is None else wide_mp
    uhd = UHD if uhd is None else uhd
    uhd = bool(uhd and all_modes and wide and wide_mp)
    y420 = Y420 if y420 is None else y420
    y420 = bool(y420 and uhd)
    irq_mask = IRQ_MASK if irq_mask is None else irq_mask
    table = PATCHES + HPD_PATCHES + IRQ_GUARD_PATCHES
    if irq_mask:
        table = [t for t in table if t[0] != IRQ_MASK_OFF[0]] + IRQ_MASK_PATCHES
    if all_modes:
        table = table + ALL_MODES_PATCHES
        if wide:
            table = table + WIDE_PATCHES
            if wide_mp:
                table = [t for t in table if t[0] != 0x90f4] + WIDE_MP_PATCHES
                if uhd:
                    table = table + [UHD_PATCH]
                    if y420:
                        table = ([t for t in table if t[0] != 0x90f0] + Y420_PATCHES
                                 + (Y420_GUARD_PATCHES if y420_guard else []))
    swaps = []
    if all_modes:
        swaps = (ALL_MODES_WORDS + ([] if wide else NREC_WORDS) + (UHD_WORDS if uhd else [])
                 + (Y420_WORDS if y420 else []) + (Y420_GUARD_WORDS if y420 and y420_guard else []))
    out = []
    for addr, old, new in sorted(table):
        for site, index, without, with_ in swaps:
            if addr == site:
                if new[index] != without:
                    sys.exit("sprd_drm: no all_modes word %d in the 0x%x patch" % (index, addr))
                new = list(new)
                new[index] = with_
        out.append((addr, old, new))
    return out


def patch_module(ko, all_modes=None, wide=None, wide_mp=None, uhd=None, y420=None,
                 y420_guard=None, irq_mask=None):
    if all_modes is None:
        all_modes = os.environ.get("THIMSY_DP_ALL_MODES") == "1"
    if hashlib.sha256(ko).hexdigest() != MODULE_SHA256:
        sys.exit("sprd-drm.ko is not the 1.2 module this patch is for")
    ko = bytearray(ko)
    for addr, old, new in patches(all_modes, wide, wide_mp, uhd, y420, y420_guard, irq_mask):
        off = TEXT_OFFSET + addr
        n = len(old)
        if list(struct.unpack_from("<%dI" % n, ko, off)) != old:
            sys.exit("unexpected code at 0x%x" % addr)
        struct.pack_into("<%dI" % n, ko, off, *new)
    return bytes(ko)


def patch_release(ko, all_modes=None):
    """The release build's patch: patch_module with the defaults (all_modes from
    THIMSY_DP_ALL_MODES when None), the stock module in (MODULE_SHA256), and out exactly the
    module RELEASE_SHA256 pins for all_modes; else exit. A change to the tables, the defaults or
    the environment can't ship an unreviewed module."""
    if all_modes is None:
        all_modes = os.environ.get("THIMSY_DP_ALL_MODES") == "1"
    out = patch_module(ko, bool(all_modes))
    if hashlib.sha256(out).hexdigest() != RELEASE_SHA256[bool(all_modes)]:
        sys.exit("sprd-drm.ko: the patch (all_modes=%s) no longer gives the release's module %s..."
                 % (bool(all_modes), RELEASE_SHA256[bool(all_modes)][:8]))
    return out
