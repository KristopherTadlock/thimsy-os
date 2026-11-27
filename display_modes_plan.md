# Higher external display modes (1440p60, 1080p120, 4K30, 4K60 4:2:0 video)

Status: proposed 2026-11-23; revised 2026-11-24 after a technical and a safety review. Section 4 is implemented for test 40 to the extent `usb_c_dock_plan.md` section 12 lists, except Step 3's SurfaceFlinger part (restarting the vsync check on a mode change), which is not implemented yet (TODO.md). Test 41 ran it on the device with a 1440p monitor: section 9 has what it found and the four driver fixes that followed (2026-11-25); test 42 ran B and D on the device, replaced C with a fixed mode table, and added E (section 9). Test 42 (2026-11-26) ships 4K: `scripts/sprd_drm.py`'s defaults build MOD-Y1-E (`6eefac58...`: UHD and Y420 on, no Y420 guard, fix E), which offers TV 4K at 30 Hz and 4K at 60 Hz as YCbCr 4:2:0 when the monitor lists them (section 8), and the build turns 4K on (`THIMSY_MONITOR_4K=1` with the default `COMPOSER_FIX=1`). Every claim is marked **[C]** (confirmed from the module disassembly, the vendor composer binary, the Thimsy patch or the device logs in `out/.dock`) or **[I]** (inferred, to be checked on the device).

Request: "support more display output resolutions, like 1080p120 or 4k30, or 1440p60". Today a monitor runs at 1080p60 by design (README "External Display": "For now a monitor always runs at 1080p at 60 Hz"). Three layers hold it there, and all three have to change. The hardware can drive all three requested modes over the Cube's link.

Working files: `out/.dock/work/dpmodes-kernel/` (`sprd-drm.orig.ko` rebuilt from the patched copy, `orig.dis`, `bwcheck.s`/`bwcheck.elf`) and `out/.dock/work/dpmodes-composer/` (`hwcomposer.unisoc.so` from vendor_a, `hwc.dis`). `out/.dock/modules/sprd-drm.ko` is the **patched** module pulled from the device (sha256 `0659ff53…`): all 7 `PATCHES` sites hold their new words, and putting the old words back gives exactly `MODULE_SHA256` `4d60c3c0…` **[C]**.

## 1. What holds a monitor at 1080p60 today

**Kernel, `sprd_dp_connector_mode_valid` (0x8cb8), as patched by `scripts/sprd_drm.py`.**
- Pixel clock > 0x49000 kHz (299.008 MHz) returns `MODE_CLOCK_HIGH` (patch at 0x8cdc, return at 0x8dd8) **[C]**.
- CEA 1080p60 (VIC 16 at 148500 kHz) is the only `PREFERRED` mode. CEA 640x480 and the monitor's own EDID-preferred mode become `USERDEF` (0x20); the stub at 0x8d90 does `tbz w8,#3,return` (at 0x8d94; 0x8d8c is CEA 640x480's `b 0x8e48`), so only a mode that was already preferred is made user-defined **[C]**.
- Every other mode that passes keeps type 0x40 (DRIVER only). In `dock-watch.log` (4K TV through a hub) 1080p60 is 0x48, 640x480@25.175 is 0x60, everything else 0x40 **[C]**.
- Note: `drm_match_cea_mode` matches on timing, so a monitor's DMT 1920x1080@60 (same 148.5 MHz timing) also counts as VIC 16 **[I]** (standard DRM behaviour, not checked in this kernel).

**Vendor composer, `/vendor/lib64/hw/hwcomposer.unisoc.so`** (loaded by `vendor.hwcomposer-2-4`).
- `DrmConnector::UpdateModes` keeps a DisplayPort mode only if its type has 0x8 or 0x20 (`tst w8,#0x28` near 0x6566c) **[C]**. So Android sees about three modes: 1080p60, 640x480 and the monitor's native mode (if ≤ 299 MHz). 1080p120, 1440p60 (unless native) and 4K30 never reach Android. The kept modes go into a `std::vector<DrmMode>` (`__push_back_slow_path` at 0x65794), so a longer list needs no fixed-size array **[C]**.
- At connect, `SprdDrm::ChosePreferredConfig` (0x5e4c0) sets the `PREFERRED` mode (`[conn+472]`); with none, the first kept mode **[C]**.
- For an external display, `SprdDrm::setActiveConfig` (0x5e600) does a full `MODE_ID` atomic modeset; the frame-rate-change CRTC-property shortcut is display 0 only **[I]** (read from disassembly, not run).
- `GetConfigAttributes` reports vsync from the kernel's integer vrefresh, so 59.94 reports as 60 **[C]**. `CONFIG_GROUP` is assigned by `h_display` only (1920x1080 and 1920x1200 share a group) **[C]**, harmless while Thimsy pins the mode **[I]**.
- At any 3840-wide active mode, `SprdDrm::AddFlushData` (0x5f5b0) drops every external-display layer that is not 3840 wide ("4k bypass mode drop non-4k layer") **[C]**. 4096 wide is not treated as bypass **[C]**.

**Android (Thimsy framework, `patches/thimsy/0001-thimsy.patch`).**
- `GammaosScreenArrangement.monitorMode(widths, heights, refreshRates)` returns the 1920x1080 mode within 1 Hz of 60, else -1 **[C]**.
- `LocalDisplayAdapter.gammaosMonitorModeLocked()` feeds it, and `setDesiredDisplayModeSpecsLocked` replaces whatever DisplayModeDirector asks for with `DesiredDisplayModeSpecs(held.getModeId(), false, [r,r], [r,r])` **[C]**. This runs for Mirror too (`gammaosConfigureMirrorLocked` passes `info.modeId` with any range, then the hold overrides it) **[C]**.
- `DisplayManagerService`: `configurePreferredDisplayModeLocked` returns early for `gammaosIsMonitor(device)`, and the global user-preferred mode is never applied to a monitor **[C]**. So `cmd display set-user-preferred-display-mode` changes nothing on a monitor.
- `DisplayWindowSettings` forbids a forced size or density on a monitor, and `monitorDensity` scales 213 dpi at 1080 lines by the mode's shorter side **[C]**.
- No UI: LineageOS 21 Settings has no external-display mode picker (that is Android 15) **[I]**, and Nano's External Display submenu (`buildExternalDisplaySubmenu`, `kExternalDisplayCh`) offers only Docked / Dual Screen / Mirror and Controller Screen **[C]**.

## 2. What the hardware can drive

- **DPU1 pixel clock: 297 MHz max outside bypass.** `dpu_clk_init` (0xbe94) sets `pixelpll = 9 * floor(297 MHz / p) * p` and `dpi = 9 * p`, with `p = pclk >> bypass`. Above 297 MHz the division gives 0 **[C]**. So the existing ~297 MHz cap is the real limit: do not raise it for 1080p144, 1440p75+ or 3440x1440.
- **Cap gap.** mode_valid's cap is 299.008 MHz (`cmp w10,#0x49,lsl #12`; 297000 kHz can't be encoded as one immediate), so a non-bypass mode at 297.001-299.008 MHz passes and gets pixelpll 0 **[C]**. Such timings are rare. Android can't filter them (the framework never sees pixel clocks); confirm-and-revert covers them.
- **Core clock.** `dpu_clk_init` (0xbea8-0xbf18) sets the core clock to 297 MHz whenever the pixel clock doesn't divide 128, 192, 297 or 307.2 MHz, after logging a warning; 1440p60 (241.5 MHz) and 1080p120 CVT-RB (285.5 MHz) take that path **[C]**.
- **pixelpll rates.** Every mode tested so far (1080p60, 720p60, 4K30 bypass) programs pixelpll to 2673 MHz **[C]**. 1080p120 CEA (297 MHz) and 4K30 CEA (bypass p = 148.5 MHz) also give 2673 MHz **[C]**. 1440p60 CVT-RB (241.5 MHz) gives 2173.5 MHz and 1080p120 CVT-RB (285.5 MHz) gives 2569.5 MHz, which have never been tried; whether pixelpll locks there is unknown **[I]**.
- **Link.** `max_lanes`=2, `max_rate`=HBR3 are hard-coded (`strh 0x0203` at dptx+3456 in `dptx_init` 0x38188 and `dptx_misc_reset` 0x374c8). Training takes min(sink, those) and is not chosen per mode; rate code 2 = 540 (10 Mbps units) = HBR2 **[C]**. The dock logs show `Link training succeeded rate=2 lanes=2` **[C]**. Do not raise lanes to 4: pin D shares the PHY with USB3 and the retrain path hard-codes 2 lanes **[C]**.
- **Format.** RGB 8 bpc always, except 3840x2160 bypass with an NV12 buffer (YCbCr 4:2:0, 8 bpc) or a P010 buffer (YCbCr 4:2:0, 10 bpc) (`sprd_dp_encoder_mode_set` 0x90f4-0x9140). `dptx_video_ts_calculate` counts 24 bits per pixel for RGB, 12 for NV12 and 15 for P010 (tables at `.rodata+0xc5c8` = {96,128,192,64} and `+0xc5d8` = {120,160,240,80}, in bits per pixel × 8). No DSC on DP, no 4:2:2/4:4:4 **[C]**.
- **No bandwidth check.** `dptx_video_ts_calculate` (0x37198) computes T = bpp×8 × clock / (link × lanes), with link = 270 × rate for rate 1-3 and 162 otherwise; T = 64000 is 100% of the link. It returns -EINVAL only when T ≥ 65000 (about 101.6%); between 100% and 101.6% it programs an impossible TU and returns 0. `sprd_dp_encoder_enable` (call at 0x9354) ignores the result, so an over-bandwidth mode gives a broken picture or none **[C]**.
- **No 60 Hz assumption.** `refresh_rate` (dptx+3512) is written, never read; timings are programmed from the `drm_display_mode` **[C]**.
- **Bypass** = any 3840x2160 mode at any refresh rate: one layer only (`dpu_flip` logs "bypass mode layer count error" but carries on), DPI clock pclk/2 **[C]**. Thimsy's 0x90f4 patch treats a missing framebuffer as RGB 8 bpc, which fixed the 4K hotplug panic **[C]**. RGB through bypass at 3840x2160@24/25/30 has never been shown on a screen **[I]**.
- **4096x2160@24/25/30** (297 MHz) passes the cap but is composed in normal mode at 4096 wide, a path the vendor never uses **[I]**. The test TV lists it (`DP-1=connected modes=1920x1080 1920x1080 4096x2160`) **[C]**.
- **Colour range.** CEA modes are signalled limited range, DMT/CVT modes full range (`dptx_timing_cfg` 0x363cc) **[C]**. So 1080p120 (CEA 63) and 4K30 (CEA 95) are limited, 1440p60 (CVT) is full.

## 3. Bandwidth on two lanes (RGB 8 bpc, 24 bpp)

Payload for 2 lanes after 8b/10b: RBR 2.59, HBR 4.32, HBR2 8.64, HBR3 12.96 Gbit/s. Computed with the driver's own formula with the correct 100% limit, which is stricter than the driver's own -EINVAL at about 101.6% (section 2) **[C]**.

| Mode | Pixel clock | Needs | 2-lane HBR2 | 2-lane HBR | Passes 297 MHz DPI cap |
|---|---|---|---|---|---|
| 1080p60 (CEA 16) | 148.5 MHz | 3.56 Gbit/s | 41% | 82.5% | yes |
| 1440p60 (CVT-RB) | 241.5 MHz | 5.80 | 67% | 134% (no) | yes (untested pixelpll rate) |
| 1080p120 (CEA 63) | 297 MHz | 7.13 | 82.5% | 165% (no) | yes |
| 1080p120 (CVT-RB) | 285.5 MHz | 6.85 | 79% | no | yes (untested pixelpll rate) |
| 2560x1080@60 (CEA 90) | 198 MHz | 4.75 | 55% | 110% (no) | yes |
| 4K30 (CEA 95, bypass) | 297 MHz | 7.13 | 82.5% | no | yes (DPI runs at 148.5) |
| 4096x2160@30 (CEA 100) | 297 MHz | 7.13 | 82.5% | no | yes (non-bypass, untested) |
| 4K60 (CEA 97) | 594 MHz | 14.26 | 165% (no) | no | no; only YUV420 video bypass |
| 1080p144 | ~325 MHz | 7.8 | 90% | no | no |
| 1440p75+ / 3440x1440@60 | 304-320 MHz | | | | no |

On 2-lane HBR2, everything up to the 297 MHz cap fits. On a dock or adapter that only trains HBR, only modes up to about 180 MHz fit: 1440p60, 1080p120 and 4K30 would give a black screen. Whether the user's dock trains HBR2 is unknown **[I]**.

## 4. Proposed changes, in order

**Step 1. Kernel: expose every mode that the DPU and the trained link can carry (`scripts/sprd_drm.py`).**
Instruction words assembled with `aarch64-linux-gnu-as` in the build container (`out/.dock/work/dpmodes-kernel/bwcheck.s`; the DCI words in the last two tuples were assembled and disassembled by the technical review and re-encoded by hand); branch targets re-checked by hand; old words checked against `orig.dis`; no relocation in `.text` 0x8cb8-0x8e64 other than 0x8cd8, 0x8d34/38/44/48/6c, 0x8da4/a8/b0/b4, 0x8e0c/10, 0x8e34/38 and 0x8e44, and no other section points into that range **[C]**; behaviour not run **[I]**. Registers x11-x15 are free here; w0 (the CEA VIC) and w10 (the clock) are preserved.

A. Link-bandwidth check (`clock_kHz * 3 > link10 * lanes * 1000` → `MODE_CLOCK_HIGH`), then the DCI reject (C):
1. In the 0x8cdc tuple, the last new word (0x8ce8) becomes `0x14000034` (`b 0x8db8`): new list `[0xb9403a6a, 0x7141255f, 0x540007a8, 0x14000034]`.
2. Four new tuples in the dead 640x480/4K60 printk tails, between the relocations at 0x8da4-0x8db4, 0x8e0c/0x8e10, 0x8e34/0x8e38 and 0x8e44 (which must not be overwritten). 0x8e3c/0x8e40 are dead once 0x8db8 is patched: their only branch source was `b 0x8e40` at 0x8dd4, and the 640x480 path (0x8d8c) branches to 0x8e48 **[C]**.

```python
    # mode_valid: drop modes the trained link can't carry at RGB 8 bpc.
    (0x8db8, [0x528c4ae4, 0xb90033ee, 0xb9002be8, 0xb90023ed, 0xb9001bec, 0xb90013eb,
              0xb9000bea, 0x1400001b],
             [0xf943ca8b,               # ldr  x11, [x20, #0x790]  (dptx)
              0xb400034b,               # cbz  x11, done
              0x9140056b,               # add  x11, x11, #0x1000
              0x39455d6c,               # ldrb w12, [x11, #0x157]  (trained rate)
              0x3945616d,               # ldrb w13, [x11, #0x158]  (trained lanes)
              0x340002cd,               # cbz  w13, done           (not trained: skip)
              0x5280144e,               # mov  w14, #162
              0x14000007]),             # b    0x8df0
    (0x8df0, [0x36180329, 0x294a2e6a, 0x294b366c, 0xb940666e, 0xb940b663, 0x29479a65,
              0x2948a667],
             [0x528021cf,               # mov  w15, #270
              0x1b0c7def,               # mul  w15, w15, w12
              0x7100019f,               # cmp  w12, #0
              0x1a8f01ce,               # csel w14, w14, w15, eq   (rate 0 = RBR)
              0x1b0d7dce,               # mul  w14, w14, w13
              0x52807d0f,               # mov  w15, #1000
              0x14000003]),             # b    0x8e14
    (0x8e14, [0x52820a04, 0x91004262, 0xb90033ee, 0xb9002be8, 0xb90023ed, 0xb9001bec,
              0xb90013eb, 0xb9000bea],
             [0x1b0f7dce,               # mul  w14, w14, w15       (bytes/ms the link carries)
              0x0b0a054f,               # add  w15, w10, w10, lsl #1 (clock * 3)
              0x6b0e01ff,               # cmp  w15, w14
              0x54fffdc8,               # b.hi 0x8dd8              (MODE_CLOCK_HIGH)
              0xb9403e6f,               # done: ldr w15, [x19, #60] (hdisplay)
              0x713c01ff,               # cmp  w15, #0xf00
              0x54fffd68,               # b.hi 0x8dd8              (wider than 3840: MODE_CLOCK_HIGH)
              0x14000003]),             # b    0x8e3c
    (0x8e3c, [0x72a00124, 0xb90003e9],
             [0xb9403668,               # ldr  w8, [x19, #0x34]    (type)
              0x17ffffab]),             # b    0x8cec
```

- dptx is at connector+0x790 (`get_modes` 0x8c40 and `detect` 0x9044 load it); rate/lanes at dptx+0x1157/0x1158 (`dptx_link_training_init` 0x351a8/0x351ac; `encoder_enable` passes the same bytes to `dptx_video_ts_calculate` at 0x9338-0x934c). Training starts from max_rate 3 and max_lanes 2 (dptx+3456/3457, 0x37830) **[C]**.
- The values are fresh whenever modes are probed **[C]**. dptx+0x1156 marks a trained link: `dptx_link_training_init` clears it (0x351b0), only a successful training sets it (0x35074), and the HPD-low path of `dptx_threaded_irq` clears it (0x375f8). `sprd_dp_connector_detect` returns disconnected while it is 0 (0x9044-0x905c). `dptx_threaded_irq` trains (0x3783c) before it calls `drm_helper_hpd_irq_event` (0x37894). So the connector is connected, and its modes probed, only after a successful training, with that training's rate and lanes. A later retrain from the IRQ_HPD link check (`dptx_link_check_status`, after the hotplug event) could lower the rate without a new probe **[I]**.
- Training never zeroes rate or lanes on failure: the -71 exit (0x34fc8) comes only after falling back to 1 lane at RBR, so after a failure rate is 0 and lanes 1 **[C]**. `cbz w13` therefore covers only "never trained since boot". It is harmless: after a failure the connector reads disconnected and no modes are probed (`klog-1.txt` shows no `mode_valid` calls after `Link training failed -71`), and even a forced probe would pass only modes up to 54 MHz (RBR × 1).
- The cbz targets stay at 0x8e24, so the DCI reject (C) also runs when the link check is skipped. hdisplay is a 32-bit int at mode+60 (the 5.4 layout, as the printk loads show) **[C]**.
- Optional headroom for the audio SDP: `mov w15,#970` = `0x5280794f` instead of 1000. With 1000, 1080p120 CEA is at 82.5%, so either works on HBR2.
- Side effect on an RBR-only link (2 lanes, modes up to 108 MHz): 1080p60 is rejected, so no mode is preferred. With B (1.4.4 base), the composer picks the first kept mode, the largest that fits. Without B (1.2 base), only modes that are user-defined anyway are kept: CEA 640x480, and the monitor's own preferred mode if it fits; the monitor comes up at 640x480 unless its preferred mode fits. Either is correct; today it would be a black screen.

B. Make every passing mode user-defined, so the composer lists it: in the 0x8d8c tuple, the third new word `0x36180608` (`tbz w8,#3,return`, at 0x8d94) becomes `0xd503201f` (`nop`). CEA 1080p60 stays the only preferred mode, so the composer still sets 1080p60 at connect (`ChosePreferredConfig`) and nothing changes for a user who never picks a mode **[I]**. DRM sorts preferred first, then by area, then by refresh rate, then higher clock first (`drm_mode_compare`) **[I]**, so 1080p60 stays first.
- B is for the 1.4.4 base only (it needs Thimsy's framework hold). Because its word lies inside the 0x8d8c tuple, it can't be a tuple of its own (that would overlap the 0x8d8c tuple, and the original word at 0x8d94 is 0x2948a667). `patch_module(ko, all_modes=False)` swaps new word [2] of the 0x8d8c tuple for the `nop` when `all_modes` is true. `build.sh` exports `THIMSY_DP_ALL_MODES=1` only with `SYSTEM_IMG` (0 otherwise), and `sprd_drm.py` takes `all_modes` from it for vendor_dlkm; `patch_vendor_boot.py` never applies B (recovery has no use for it). So the 1.2 base can't get B by mistake. `test_vendor_patches.py` tests both variants.

C. 4096-wide (DCI) modes are rejected in the kernel: any mode wider than 3840 returns `MODE_CLOCK_HIGH` (last two tuples above). Without it, a TV that lacks CEA 1080p60 would come up at DCI 4096x2160@30 with B (it sorts first by area, and the composer sets the first kept mode). Android filters them as well (Step 2).

Update the `sprd_drm.py` docstring: modes are limited by the DPI clock (297 MHz), by the trained link and to 3840 wide; with `all_modes`, every valid mode is user-defined; CEA 1080p60 stays the only preferred mode in both variants.

Why the kernel and not the composer: the same result is a one-word patch in `hwcomposer.unisoc.so` (file offset 0x65674, `0x540000e1` → `0x14000007`; `UpdateModes` compares the connector type with 0xa, DisplayPort, at 0x65670, and the second PT_LOAD maps file offset 0x3f000 at address 0x3f000) **[C]**, but that means rebuilding vendor_a (ext4, dm-verity/AVB). The kernel module is already patched in vendor_dlkm and vendor_boot.

**Step 2. Framework: the hold follows the user's choice.**
- `GammaosScreenArrangement.monitorMode(widths, heights, rates, trial, stored, nativeW, nativeH)` (pure, host-tested in `GammaosScreenArrangementCheck.java`). Precedence:
  1. a running trial;
  2. the stored mode for this monitor, if it is still listed (a different dock that trains fewer lanes no longer lists it: use Auto for this connection, keep the stored mode);
  3. Auto.
- Auto always pins a concrete mode; `gammaosMonitorModeLocked()` never returns null for a monitor. Today it returns null when `monitorMode` gives -1 (a monitor without 1080p60), and then DisplayModeDirector's votes apply, including an app's `preferredDisplayModeId`, which on Android 14 can change the resolution **[C]**. With B that would let any app switch the monitor to any listed mode, with no confirm and no revert.
- Auto, stage 1 (first build): 1920x1080 nearest 60 Hz exactly as today, else the largest listed mode at 50-61 Hz, else the composer's active mode at connect (if it is still listed after the filter below, else the first listed mode). Stage 2 (after device tests): the native size at 59-61 Hz, else the largest 16:9 mode at 59-61 Hz, else 1080p near 60. Auto never picks below 50 Hz or above 61 Hz while such a mode is listed.
- Filter the monitor's `mSupportedModes` in `LocalDisplayAdapter`, so no path (apps, DisplayModeDirector, `cmd`) can select a filtered mode: nothing wider than 3840 (DCI 4096; the kernel rejects it too, Step 1C), nothing below 640x480, no 720x400, and 3840x2160 only together with Step 5.
- In Dual Screen, a stored mode below 50 Hz is ignored and Auto is used; the stored value stays for Docked and Mirror (decisions 6 and 7: a 30 Hz monitor paces the panel's composition). Test it in `GammaosScreenArrangementCheck`.
- 59.94 and 60.00 both report 60 Hz (integer vrefresh) **[C]**, and the framework never sees pixel clocks. `LocalDisplayAdapter` merges modes with the same width, height and rates (`hasMatchingMode`) and keeps the first HWC config **[C]** (`out/.dock/work/dpmodes-composer/aosp/LocalDisplayAdapter.java`). DRM sorts equal sizes and integer rates by higher clock first, so 148.5 MHz 1080p60 (and the 60.00 variant of other sizes) is the one kept, provided the composer keeps the kernel's order **[I]**. Nothing to do.
- `LocalDisplayAdapter.setDesiredDisplayModeSpecsLocked`: keep the physical range pinned at [r,r] so frame-rate votes can't change mode. For 120 Hz, consider render range [0,r] (or [60,r]) so 60 fps games aren't paced at 120 **[I]**.

**Step 3. Framework: per-monitor memory and a trial with automatic revert.**
- `DisplayManagerService.configurePreferredDisplayModeLocked`: for a monitor, instead of returning early, read the monitor's stored mode into the hold. The panel's global `mUserPreferredMode` stays blocked for monitors.
- Key the stored mode on a hash of `DeviceProductInfo` (PnP id, product id, model year, name) from the start, in Thimsy's own store, not on uniqueId (`PersistentDataStore`'s keys): the Unisoc HWC's physical id may be port-based **[I]**, and then one stored mode would apply to every monitor. A monitor without product info is never persisted: Keep holds its mode until unplug only. Check what the HWC gives with two monitors: `dumpsys display | grep -E "uniqueId|deviceProductInfo"`.
- New `DisplayManagerShellCommand` verb `gammaos-monitor-mode list|try W H HZ|keep|revert|auto|forget`. Nano already runs privileged shell commands with `system()` (`am force-stop`, `appops`) **[C]**.
- `try`: the trial lives in system_server memory only, never persisted. system_server posts a 15 s revert on `mHandler`, so a Nano crash still reverts. If the active mode is not the requested one within 3 s, revert at once and report the failure. Unplug discards the trial. Because nothing is persisted before `keep`, a kernel panic or a forced reboot during a trial comes back at Auto.
- system_server enforces the rules, not only Nano's UI, since `cmd` is a separate entry point: `try` refuses a mode the Step 2 filters reject (wider than 3840, below 640x480, 3840x2160 without Step 5, below 50 Hz in Dual Screen), and `keep` persists only the trial that is running.
- `keep` persists it; `forget` clears every monitor's stored mode.
- **Applying marker.** After Keep, the stored mode is applied at every connect, including at boot with the monitor plugged in, and in Docked mode the panel is off. A stored mode that panics or hangs (a different adapter, a flaky pixelpll rate) would repeat at every docked boot. So system_server writes a per-monitor "applying" marker before it applies a stored mode and clears it once the mode has been active for 20 s with no trial running (timed from when it is first seen active, so a slow boot or a trial doesn't run the clock out early), or when the monitor is unplugged first (system_server saw it go, so nothing panicked or hung). A marker found at boot or at the next connect means Auto for that monitor and drops its stored mode. A stored mode that only shows black is undone by unplugging and choosing Forget Monitor Resolutions on the Cube's screen (README).
- **SurfaceFlinger.** `gammaCheckMonitorVsync` marks a monitor's vsync broken after 3 s without one, until unplug **[C]**. A full modeset (encoder disable/enable, a retrain with PHY BUSY waits of about 0.5 s each, a bypass switch) can take longer, and a revert would not bring hardware vsync back. When a monitor's active mode changes, restart its window (`mGammaVsyncEnabledAt` = now) and clear `mGammaVsyncBroken` for it. Add a test. **Not implemented in test 40** (TODO.md).
- Optional runtime property for Nano: `sys.gammaos.display.monitor_mode` = `WxH@Hz;auto|user|trial`.

**Step 4. Nano: Output Resolution and Refresh Rate with confirm-and-revert.**
- Settings > Display Settings > External Display (the Quick Menu > Display copy comes free, since `buildQuickDisplaySubmenu` reuses the item) **[C]**: rows Docked / Dual Screen / Mirror (unchanged), **Output Resolution**, **Refresh Rate**, Controller Screen, **Forget Monitor Resolutions**. The two mode rows appear only while a monitor is listed; `externalDisplayTick` already rebuilds the list every 500 ms **[C]**.
- Choosers are side panels (`mPs3DlgKind=1`), theme keys 68 and 69 (free) **[C]**. Output Resolution: "Auto (…)", then each size the monitor lists, largest first, the native one marked. Refresh Rate: "Auto (60 Hz)", then the rates at the current size. No live preview on Up/Down (a mode switch blanks the monitor for 1-3 s) **[I]**.
- Applying runs `cmd display gammaos-monitor-mode try W H HZ` off the UI thread, then shows "Keep This Resolution?" (confirm dialog, kind 0 / type 1, key 70): "The monitor now shows 2560 x 1440 at 60 Hz. It goes back to 1920 x 1080 at 60 Hz in %d s." Options Keep / Revert, cursor on Revert, input ignored for the first 0.5-1 s so the A press that applied it can't keep it. B, closing and the timeout all revert.
- In Dual Screen the dialog shows on the Cube's screen; in Docked it shows on the monitor (the timer covers a black picture).
- Over a running game, Output Resolution and Refresh Rate are disabled ("Close the game to change the resolution"): a size/density change is a configuration change that can restart an emulator **[I]**, and a rate change is a full modeset that blanks the monitor for 1-3 s **[I]**.
- New strings in every shipped translation (a Nano test checks every key) **[C]**.

**Step 5. 4K30 specifics.**
- At 3840x2160 the HWC drops every non-3840-wide layer **[C]** and the DPU wants exactly one layer **[C]**. Check on the device whether SurfaceFlinger already composes everything into one CLIENT target at 4K (`dumpsys SurfaceFlinger`, composition types). If any DEVICE layer survives (video overlay, IME), force client composition for every layer on that output while its mode is 3840 wide **[I]**.
- Without that, `AddFlushData` likely drops the Quick Menu over a game, the IME, toasts and possibly the Keep/Revert dialog itself, and two 3840-wide DEVICE layers reach the DPU's "bypass mode layer count error" path, whose hardware behaviour is unknown **[I]**. So 3840x2160 is listed only in a build that forces client composition at 3840 wide (Step 2 filter); a build without it hides 3840x2160.
- 4K quadruples the pixels Nano, apps and SurfaceFlinger draw. There is no "render at 1080p, output 4K" today (forced sizes are blocked on monitors **[C]**); leave it out of this work.

**Step 6. Docs.** README "External Display": replace "For now a monitor always runs at 1080p at 60 Hz…" with the new rows and limits; driver bullets (mode_valid now checks the link; all valid modes user-defined); `external_display_modes_plan.md` §R "Resolution"; TODO.md.

## 5. Risks

- **4K panic history.** The old panic was the composer's hotplug commit with no framebuffer in bypass; the 0x90f4 patch handles it **[C]**. RGB through bypass at 4K30 is still untested: expect a black or wrong picture at worst **[I]**, but another panic path can't be ruled out. A panic is not covered by the 15 s timer, only by the trial not being persisted, and after Keep by the applying marker (Step 3). Keep CEA 1080p60 preferred so no monitor comes up at 4K at connect.
- **Monitors without 1080p60.** With B, such a monitor comes up at the largest valid mode (first after DRM's sort), which may be 4K30 at hotplug (4096x2160 no longer, Step 1C), before the framework switches to Auto. Seen in test 41 (section 9, finding 1), and the switch then gave no picture (finding 2) **[C]**. Section 9's fixes A, C and D address it.
- **Misreported EDID and adapters.** TVs advertise DCI 4096x2160 (squashed on 16:9) **[C]**; USB-C/DP-to-HDMI adapters can pass through modes their HDMI side can't carry, and the kernel does not read the DPCD downstream-port TMDS limit **[I]**. Link-rate fallback adapters (HBR only) are covered by the check in Step 1A. Everything else is covered by confirm-and-revert and Forget.
- **Untested pixelpll rates.** 1440p60 and CVT-RB 1080p120 program pixelpll to rates never used **[I]**. Watch for `update pixelpll clk rate failed` / `use default DPI_CLK_SRC`. If 1440p60 fails, it should be hidden, not shipped broken.
- **Colour range.** CEA modes signal limited range; if the pixels are full range, blacks/whites clip differently between 1080p120/4K30 and 1440p60 **[I]**. Same as 1080p60 today.
- **Power and heat.** 1080p120 doubles the DPI clock and SurfaceFlinger's composition rate; 4K30 is 4x the pixels at half the rate; 1440p60 is 1.8x. Higher GPU load in Nano (XMB wave, blur), Mirror's second pass and RetroArch shaders, more battery draw and heat on a handheld that is often also powering a hub **[I]**. Measure before making anything Auto.
- **Pacing.** In Dual Screen the monitor is the vsync pacesetter **[C]**, so a 30 Hz monitor probably slows the built-in screen's composition to 30 Hz **[I]**. 120 Hz composes both screens at 120. After a 60 → 120 switch, check that Thimsy's `gammaCheckMonitorVsync` broken-vsync fallback does not trigger (Step 3 restarts its window on a mode change).
- **Emulators at 120 Hz.** RetroArch, PPSSPP, Dolphin, Azahar and drastic-nano may run double speed or judder when the display is 120 Hz **[I]**.

## 6. Testing on the device

Before the UI exists, test with `adb shell cmd display gammaos-monitor-mode try W H HZ` (Step 3), on a 1440p monitor, a 1080p120 monitor and a 4K TV, each through the usual hub and through a plain USB-C cable or adapter.

For each mode:
1. Before: `cat /sys/class/drm/card*-DP-1/modes`; `dumpsys display | grep -iE "mode|uniqueId|deviceProductInfo"` (is the mode listed? does it carry an id?).
2. `dmesg -C`, `logcat -c`, apply the mode, wait 15 s, then `dmesg | grep -iE "pixelpll|DPI_CLK|pixelclock|bypass|layer count|vsync \+ vbp|Link training|tu"` and `logcat -d | grep -iE "AddFlushData|setActiveConfig|hwc"`.
3. `dumpsys SurfaceFlinger | grep -iE "activeMode|refresh|Composition"`: the active mode and, at 4K30, that only one CLIENT target reaches the HWC.
4. Look at the monitor: its own info OSD should show the resolution and rate; check text sharpness, colour range (black and white levels against 1080p60) and audio.
5. Frame pacing: `dumpsys SurfaceFlinger --latency` on Nano's layer; one game in RetroArch and one in drastic-nano at 120 Hz.
6. Heat and power: `cat /sys/class/thermal/thermal_zone*/temp` and `/sys/class/power_supply/battery/current_now` after 10 minutes in Nano and in a game, against 1080p60.
7. Revert paths: let the 15 s timer run out; press B; unplug during a trial; reboot during a trial (must come back at Auto); `forget`.
8. Re-plug three times and plug into a different dock: the stored mode comes back, or Auto if the new link can't carry it (Step 1A check). Re-plug from an HBR2 hub to an HBR-only adapter: the list shrinks to what HBR carries (`Link training succeeded rate=1`).
9. After each mode switch and revert, the monitor keeps hardware vsync: no `GammaOS: no composer vsync from monitor` in logcat.
10. Boot docked with a kept mode: it comes back. Then restart the framework (`adb shell stop; adb shell start`) within 20 s of a connect: the applying marker must make the next connect use Auto and drop the stored mode. Unplug and re-plug within 20 s of a connect: the stored mode must come back. Boot docked with a kept mode and check `/data/system/gammaos_monitor_modes.txt` has no `applying` line 20 s after the monitor shows its mode.

Per-mode specifics:
- **1440p60:** pixelpll at 2173.5 MHz (CVT-RB); full range.
- **1080p120:** both CEA 63 (297 MHz, known pixelpll rate) and CVT-RB (285.5 MHz) if the monitor lists both; vsync keeps arriving (no `gammaCheckMonitorVsync` fallback); Dual Screen pacing.
- **4K30:** `bypass_mode` in dmesg; no `layer count error`; IME and Quick Menu overlays still visible (layers not dropped by `AddFlushData`); Nano at 3840x2160 with 427 dpi density.
- **2560x1080, 1920x1200** if available: non-16:9 sizes come out unscaled.

## 7. Decisions for the owner

Taken for test 40: `usb_c_dock_plan.md` section 12.

1. Expose modes in the kernel (Step 1B, one word in `sprd_drm.py`) rather than patching the vendor composer in vendor_a? Recommended: kernel.
2. Add the link-bandwidth check (Step 1A)? With 1000 or 970 (audio headroom)?
3. Auto: stay at 1080p60 in the first build (stage 1) and move to "native at about 60 Hz" only after device tests, or change the default now?
4. 4K30: show it in the first build (relying on confirm-and-revert), or hide 3840x2160 until RGB bypass has been seen working? Hide DCI 4096x2160 always, or allow it when it is the monitor's native mode?
5. 1440p60 at a non-2673 MHz pixelpll rate: ship if it works on one monitor, or wait for more?
6. Remember the mode per monitor only, or per monitor and per display mode (Docked / Dual Screen / Mirror)?
7. Allow modes below 50 Hz in Dual Screen (they likely slow the built-in screen), or hide them there?
8. Revert timeout (15 s proposed) and the dialog's starting cursor (Revert proposed).
9. 120 Hz render range: pin render at 120, or [0,120] so 60 fps content paces at 60?
10. A later "render at 1080p, output 4K" option (relaxing the monitor forced-size rule that caused R4): leave out, as proposed?

## 8. 4K60 over YCbCr 4:2:0 (video only)

**Status (2026-11-26): done for the whole UI, not only video.** Test 42's T3 showed Nano, the Quick Menu
and RetroArch at 4K60 with correct colours (`dp_dock_findings.md`, T3): SurfaceFlinger composes every layer
into one NV12 client target at a 3840x2160 monitor mode of 48 Hz or more (`ThimsyYuv420` in
CompositionEngine, `gammaYuv420Mode` in SurfaceFlinger.cpp; now in `patches/thimsy/0001-thimsy.patch`, the
sources of T3's binary byte for byte, `tests/unit/test_surfaceflinger_yuv420.py`), and the driver sends it
as 4:2:0 (`scripts/sprd_drm.py` Y420). The build turns it on with `ro.gammaos.display.uhd` and
`ro.gammaos.display.y420` (`mods/52-monitor-4k.sh`); the composer must pass a 3840-wide client target
(`scripts/sprd_hwc.py` CT4K). The release ships it: `sprd_drm.py`'s defaults now build MOD-Y1-E
(`6eefac58...`), T3's MOD-Y1 (`9ae14445...`, UHD and Y420 without the Y420 guard) plus fix E, so
`scripts/build.sh` sets `THIMSY_MONITOR_4K=1` with the default `COMPOSER_FIX=1` (SLEEP + CT4K,
`86ea7072...`). The guarded variant (`y420_guard=True`) never ran on a Cube with the NV12 SurfaceFlinger
and is not shipped; recovery's copy (all_modes off) is unchanged (`c7e8c829...`). The analysis below is the original plan, kept for its findings; steps 3 and 4
were replaced by the client-target approach.

Requested by the owner (2026-11-24). Full-colour 4K60 (594 MHz, 14.3 Gbit/s RGB 8 bpc) can't run on this
hardware: it needs 165% of the 2-lane HBR2 link, and the DPU's non-bypass pixel clock stops at 297 MHz
(section 2). The vendor driver does have one 4K path that gets around both limits:

- **Bypass** (any 3840x2160 mode, `dpu_modeset` 0xb37c): the DPU takes exactly one layer and runs its DPI
  clock at pclk/2 **[C]**. At 4K60, p = 594 MHz >> 1 = 297 MHz; `dpu_clk_init` sets pixelpll to
  9 × floor(297/297) × 297 = 2673 MHz and the DPI clock to 9 × p = 2673 MHz, the same two rates as 1080p120
  CEA and every mode tested so far; the core clock takes the 297 MHz fallback with its warning (section 2)
  **[C]**. Whether DPU1 sustains 594 Mpix/s (2 pixels per DPI clock) in bypass is **[I]**.
- **4:2:0 output:** with an NV12 buffer in bypass, the DP output is YCbCr 4:2:0 at 8 bpc, 12 bits per pixel
  **[C]**: 4K60 needs about 7.1 Gbit/s, 82.5% of 2-lane HBR2, the same as 4K30 RGB. P010 is 4:2:0 at 10 bpc,
  15 bits per pixel: 8.91 Gbit/s, 103% of the link; the driver computes T = 120 × 594000 / 1080 = 66000
  ≥ 65000, returns -EINVAL, and `encoder_enable` ignores it **[C]**. So the 4K60 path is NV12 only: 10-bit
  and HDR video are excluded, or the decoder must output 8-bit.

So 4K60 is reachable **only while a single NV12 video layer fills the 3840x2160 output** (a video player
in full screen with hardware overlay). Nano, games and any RGB layer at 4K60 are impossible: RGB through
bypass at 594 MHz would need 14.3 Gbit/s.

The vendor shipped this path. In the original module, mode_valid makes CEA VIC 97 (3840x2160@60, 594000 kHz)
user-defined when bit 3 of `ldrb [connector+196]` is set (0x8dd8-0x8e4c) **[C]**; that bit is very likely
`DRM_COLOR_FORMAT_YCRCB420` in `display_info.color_formats` (kernel 5.4 layout, not read from BTF) **[I]**.
Thimsy turned that branch into the `MODE_CLOCK_HIGH` return, and Step 1A's segments now occupy its tail.

What it would take (none of it is in the first implementation; each step needs the device):
1. **Kernel, mode_valid:** let 3840x2160 at 594 MHz pass only for a sink whose EDID advertises YCbCr 4:2:0
   for that mode (the CTA Y420 capability or Y420-only data blocks), and only with the bandwidth check (1A)
   computed at 12 bpp. Today the 297 MHz cap rejects it **[C]**. Three places change: the clock cap at
   0x8cdc, the `b.eq 0x8dd8` for VIC 97 at 0x8cfc (now the reject), and a new layout for the 1A segments or a
   cave elsewhere in sprd-drm.ko **[I]**.
   - mode_valid can't see the framebuffer format, so an RGB commit at 4K60 (T = 105600) is not stopped. It
     needs a format-aware `atomic_check` or a guarantee from the composer **[I]**.
   - The DRM core drops 4:2:0-only modes unless `connector->ycbcr_420_allowed` is set (kernel 5.4
     `drm_mode_validate_ycbcr420`) **[I]**.
2. **Kernel, encoder/dptx:** confirm the MSA/VSC SDP colorimetry is set to 4:2:0 for NV12 in bypass at
   60 Hz, not only at 30 Hz (it is the vendor's video path; whether it was used at 4K60 is unknown) **[I]**.
   DP 4:2:0 needs a VSC SDP that the sink supports, and a DP-to-HDMI converter must pass 4:2:0 through
   (DPCD downstream-port capabilities) **[I]**.
3. **Composer:** the vendor HWC must keep the video layer as a DEVICE layer in NV12 and send exactly one
   layer; any UI on top (subtitles, controls) forces client composition into RGB and breaks the mode. The
   mode is only usable when nothing else is visible **[I]**.
4. **Android:** a 4K60 mode visible to apps only during video playback, e.g. through the app's preferred
   display mode (`Window.preferredDisplayModeId`) with the framework hold (section 4, step 2) allowing a
   video app to request it, and returning to the user's mode when the video stops. Nano would not offer
   4K60 in Output Resolution.
5. **Test:** a 4K60-capable TV (HDMI 2.0, 4:2:0 at 60 Hz) through the HDMI-only dongle and through the hub;
   an 8-bit 4K60 H.265 file in a player that uses MediaCodec with a SurfaceView; `dmesg` for bypass and the DP
   colour format; the TV's info OSD should show 3840x2160 60 Hz YCbCr 4:2:0.

Decision for the owner: worth pursuing only if 4K60 video playback matters; it doesn't help games or the UI.
Order: after steps 1-4 of section 4 are working on the device.

## 9. Test 41 on the device: findings and the driver fixes

Test 41 (1.4.4 base, all_modes on) with a 1440p monitor (an H27S17) through a USB-C to DisplayPort adapter. Evidence in `out/dp-evidence/` (`dmesg-run2-ring.txt`, `dmesg-try1440.txt` at printk level 8, `pstore-reboot3.txt`, `pstore-crash4.txt`, `pstore-dock.txt`, `try1440.log`, the EDID-override runs `dmesg-mirror.txt`, `dmesg-1440only.txt`, `dmesg-1440at297.txt`, `dmesg-1080p120.txt` with `README-root-cause.txt`, `dumpsys-display-run2.txt`, `edid-H27S17-first256.hex`, `sprd-drm-test41.ko`); the analyses and scratch builds in `out/dp-evidence/work-*/`.

**Findings.**
1. **Start in 4K bypass.** The monitor has no CEA VIC 16: its 1080p60 is a CVT-RB DTD (138.5 MHz, 2080x1111) and its preferred mode is 2560x1440@60 CVT-RB (241.5 MHz). With all_modes no mode is `PREFERRED`, so the composer (`ChosePreferredConfig`, `setActiveConfig 0`) starts it at the first kept mode after DRM's sort, 3840x2160@30 (297 MHz, bypass) **[C]**. Section 5's "Monitors without 1080p60" risk, seen.
2. **A mode change gives no picture.** Only the connect-time modeset runs `sprd_dpu_atomic_enable` and with it `dpu_clk_init` **[C]**. Every later change (the framework's switch to 1920x1080@60 CVT-RB, a 2560x1440@60 trial) runs DP encoder disable, `dpu1 stop`, dp suspend, `mode_set_nofb`, encoder mode_set and enable, `dpu run`, and no DPU1 disable or enable: `sprd_crtc_atomic_enable/_disable` (0x25f0/0x2680, shared by both CRTCs) skip the controller's own enable/disable when only the mode changed (`(flags & 6) == 2`), the panel's seamless resolution switch **[C]**. The DP controller gets the new timing while DPU1 keeps the connect-time size, porches, bypass bit and pixel clock, so the monitor shows nothing, sleeps, drops HPD, and the composer reconnects it at 4K30, about every 35 s **[C]**. Link training is fine (HBR2 x2). PMU_APB_PIXELPLL going off and on in these windows belongs to the panel's power domain (vpu_pd_top), not DPU1's, and is not the cause **[I]**.
3. **Panic after an unplug.** `pstore-reboot3.txt`: after HOT_UNPLUG and both "dp suspend OK"s, the USB 3 PHY's shutdown raised `DPTX_ISTS_TYPE_C`; `dptx_irq` (0x37b50, vendor code) acked it at base + 0xd00 and read 0xc08 on the powered-down controller: SError, panic **[C]**. `dptx_irq` never checks the driver's power flags **[C]**. `pstore-crash4.txt` has the same trigger and survived it, then a separate abort in `sprd_dpu_atomic_disable` -> `dpu_clk_disable` -> `clk_set_parent` after DP's glb->disable gated the IPA clocks; that one is not addressed yet **[C]**. `pstore-dock.txt` (a dock unplugged with the encoder already off: one "dp suspend OK"): `dptx_irq` acked the unplug's HPD 3.7 ms after detect(0), while pd_event's glb->disable still had the controller clocked, and woke `dptx_threaded_irq`; that thread (FIFO, on CPU0 with pd_event's) ran only 140 ms later, after DP was off, and its first read of 0xd08 (`dptx_threaded_irq+0x64`) took a synchronous external abort **[C]**.
4. **2560x1440 shows nothing on the Cube, whatever the path.** With a full connect-time modeset (`sprd_dpu_atomic_enable`, `dpu_clk_init`, the panel on in Mirror) it gave no picture and HPD dropped 6-8 s later, at its own 241.5 MHz (`dmesg-mirror.txt`, `dmesg-1440only.txt`: "use default DPI_CLK_SRC", so pixelpll = 9 * floor(297 / p) * p, and no "pixelpll clk rate failed") and at an exact 297 MHz (`dmesg-1440at297.txt`, `e1440.log`) **[C]**. At 297 MHz, 1920x1080@120 CEA showed a picture **[C]**, so 2560 wide fails for a reason other than the clock (a DPU1 line-width limit outside bypass, or the monitor rejecting those timings) **[I]**. No rate outside 297 MHz's divisors (the "use default DPI_CLK_SRC" path) has shown a picture yet, and a failure there logs no error **[C]**. *Superseded by test 42 (2026-11-26): with bypass for anything wider than 1920, layer 7 in bypass (FLIP) and dual pixel mode with a transfer threshold of 40 (TH40), 2560x1440 at 60 Hz (297 MHz) showed a correct, stable picture (MOD-Q B2/B4-B6, MOD-M B7; dp_dock_findings.md 12.1-12.2).*

On the 1.2 base, monitors that list CEA 1080p60 came up at 1080p60 and nothing changed the mode, which is why problem 2 was never seen.

**Fixes in `scripts/sprd_drm.py`.** Every word was assembled with `aarch64-linux-gnu-as` at its address and disassembled in the patched module, and its original word is checked; no patched word carries a relocation, an alternative, a jump-table or a bug-table entry, and the code caves are unreferenced (`sprd_dpu_resume`, the `dptx_*` ones) or unreachable on the Cube (`dpu_init.1506`: a CFI jump-table slot, 0x3f2b0, that `dpu_lite_r2p0_core_ops` points to, used only by the `sharkl5_dpu` match data; the Cube's device tree has only `sprd,qogirn6pro-dpu` and `-dpu1`) **[C]**. None has run on the device **[I]**.
- **A. Nothing 3840 or more wide** (both variants, recovery too). The width check at 0x8e2c is `b.hs` instead of `b.hi`, so 3840x2160 (bypass, the composer's 4K path) is rejected along with DCI 4096. One word. It removes 3840-wide ultrawides as well, and Step 5 (4K30) and section 8 (4K60 video) need it reverted first. Done since test 42: with all_modes, UHD (the default) swaps it back to `b.hi`, so exactly 3840 passes and the mode table keeps every other width out; recovery keeps A.
- **B. IRQ guard** (`IRQ_GUARD_PATCHES`, both variants, recovery too). `dptx_irq` returns IRQ_NONE without touching the controller while both power flags are clear (sprd_dp + 0xb90, the encoder; + 0xbb9, pd_event). The flags are now set right after glb->enable (pd_event on, encoder enable); the encoder's is cleared before its glb->disable, pd_event's where the vendor clears it, after its glb->disable (0x8adc), because the unplug's own HPD interrupt arrives 3-10 ms after detect(0), while that glb->disable (about 14 ms) still has the controller clocked (`pstore-dock.txt`, `pstore-crash4.txt`): cleared before it, with the encoder already off, the level interrupt (`interrupts = <0 0x7b 4>`, requested by `dptx_init` with flags 0x80, `IRQF_SHARED`, not `IRQF_ONESHOT`) would go unacked and fire again at once until "nobody cared" disabled it **[C]**. The Type-C interrupt (0xd04 bit 7) is masked while the controller is still clocked, before pd_event's and bind's glb->disable; HPD stays unmasked, since its unplug event is what tells DRM; the next plug's `dptx_core_init` sets 0x95 again. `dptx_threaded_irq` checks the same flags right after its `mutex_lock` (0x3752c); with both clear it takes the register-free tail of its own unplug path (0x375e8: the redriver off, the link state cleared, extcon and DRM told "disconnected", the IRQ_HPD link check skipped), which is what `pstore-dock.txt` needed. The code is in the dead `dptx_global_intr_en/_dis` and `dptx_soft_reset(_all)`.
- **C. Safe start mode** (`SAFE_START_PATCHES`, all_modes only). *Superseded by test 42 (2026-11-26): the fixed mode table (`MODES_PATCHES`) replaced C; the driver no longer has `SAFE_START_PATCHES`, and a monitor that lists 1080p or more starts at CEA 1080p60 (148.5 MHz).* mode_valid's stub makes the best 1920x1080 at 60 Hz at or below 148.5 MHz the only preferred mode, unless another mode on the connector's list is as good. CEA 1080p60 always wins, so a monitor that lists it is unchanged; the H27S17 starts at its 138.5 MHz CVT-RB 1080p60, which is what Auto holds, so nothing changes the mode after the connect. That rate takes the "use default DPI_CLK_SRC" path (pixelpll 2493 MHz), which has never shown a picture (finding 4), so C replaces a start known to fail (4K bypass, or 2560 wide without A) with one not yet seen to work, and a black start repeats at every reconnect, since that mode is then the only preferred one **[I]**. A 297-divisible fallback (CEA 1080p50 or 720p60, 148.5 or 74.25 MHz, where the monitor lists one) is worth weighing before C ships. The code is in `dpu_init.1506` (the Sharkl5 controller's, unreachable on the Cube).
- **D. The clock on a mode change** (`MODE_CHANGE_PATCHES`, all_modes only). For the DisplayPort CRTC (type 2 at crtc + 1816) a pure mode change calls DPU1's atomic_disable and atomic_enable, so `dpu_clk_init`, `dpu_init` and `dpu_dpi_init` run for the new mode, as at an unplug and a plug; the panel's CRTC keeps the vendor skip. The code is in the unreferenced `sprd_dpu_resume`. D restores the clock setup a mode change lacked; it does not make 2560x1440 work on the Cube, which fails even with the full connect-time setup (finding 4), and between rates 297 MHz doesn't divide it depends on the unproven pixelpll path. *Superseded by test 42: 2560x1440 works with FLIP and TH40 (finding 4's note), and the mode table runs every mode at the one pixel PLL rate seen to work (2673 MHz).*
- **E. The DPTX interrupt mask while the encoder is off** (`IRQ_MASK_PATCHES`, both variants, recovery too; added after test 42's T2-sleep, `dp_dock_findings.md`). With the composer's sleep fix a power-button sleep turns DP off by DPMS with the monitor attached, and right after the wake the kernel disabled dw_dptx (`irq 76: nobody cared`, 100001 interrupts). The encoder's disable now writes the interrupt enable register (0xd04) 0 while it still holds the clock, right after its own video-stream write; its enable writes 0x15 (HPD, HDCP, SDP) with Type-C (0x80) only while pd_event holds DP, at its end, after the stream is up, so events latched while it was off are taken then. pd_event off writes 0x15 with the encoder on, 0 with it off (replacing B's Type-C-only mask). `dptx_core_init` writes exactly 0x95 instead of its reset value | 0x95: the handler acks only ISTS[7:0], and the storm's silent IRQ_NONE points at a source above bit 7 **[I]**. Caves in the dead `dptx_global_intr_dis` and `dptx_soft_reset_all`. Built as MOD-Y1-E (`out/dp-evidence/test42/irqfix/`) and run on the Cube with the SLEEP + CT4K composer (`irqfix/NOTES.md`, run 1): a sleep and wake with a monitor attached (once, then nine quick cycles and a longer sleep), an unplug while asleep (reported at the wake) and a replug while asleep all passed, with no storm (dw_dptx stayed in single digits, no unhandled interrupts) **[C]**. Not run with fix E yet: an unplug at the Cube's end while asleep, a deep-suspend sleep, 1080p120, 1440p and 4K60. It is the default the release ships.

D (and, until test 42 replaced it with the mode table, C) is only in the 1.4.4 (`SYSTEM_IMG`) vendor_dlkm copy, where the framework changes a monitor's mode; the 1.2 base and recovery keep the mode the composer starts with.

**Still open.** The crash4 DPU1 teardown abort. The pixelpll rates C and D bring in: 138.5 MHz gives 2493 MHz and 241.5 MHz gives 2173.5 MHz, never run (only 2673 MHz has); a failed `clk_set_rate` only logs "dpu update pixelpll clk rate failed". With the panel off (Docked) vpu_pd_top is off while D reprograms pixelpll, and every DPU1 clock setup so far had the panel on **[I]**. The threaded-handler panic after an unplug with the encoder off is confirmed (`pstore-dock.txt`) and is what its guard covers **[C]**. Races the guards leave: a threaded handler already past its check while pd_event's glb->disable turns DP off (it runs on the same CPU, and pd_event's thread doesn't sleep there, so only if it slept first, in an AUX transfer or the redriver's I2C); an interrupt latched in the last moments of glb->disable and taken after the clocks stop but before 0x8adc clears the flag; and an unplug event raised only after glb->disable, or a level source other than Type-C pending while DP is off, which would end in "nobody cared" (hotplug dead until a reboot, not a panic) **[I]**.

**Without a driver change**, a monitor already switched to a new mode can probably be brought back by turning the display off and on, which flips ACTIVE and so runs the full DPU1 enable: within about 10 s of the switch, `adb shell input keyevent KEYCODE_SLEEP; sleep 1; adb shell input keyevent KEYCODE_WAKEUP` (inferred from the composer's power-mode path, not tried) **[I]**. The start in 4K bypass and the unplug panics stay.

**Device test** (TODO.md, Needs the device), with the H27S17 and a 4K TV that lists VIC 16, at printk level 8:
1. Connect: no `"3840x2160"` or `"4096x` in the `sprd_dpu_mode_valid` lines and no `bypass_mode`; the H27S17's 1920x1080 138500 mode is type 0x48 and no other mode has 0x8; one `sprd_dpu_atomic_enable` with `pixelclock is 138500000`; a picture at 1920x1080 60 Hz on the monitor's OSD; no HOT_UNPLUG for 5 minutes. No picture, or a HOT_UNPLUG within about 10 s, fails C whether or not "pixelpll clk rate failed" appears (finding 4's failures logged no error). The TV: CEA 1080p60 (148500, 0x48) as before.
2. Mode changes, in Docked and in Dual Screen, between modes already seen to work at connect: an EDID override (debugfs `edid_override`, as in `README-root-cause.txt`) listing CEA 1080p60 (148.5 MHz), CEA 1080p120 (297 MHz) and CEA 720p60 (74.25 MHz), switched among with Output Resolution or `cmd display gammaos-monitor-mode try W H HZ` and `auto`. Not 2560x1440: it fails even at connect (finding 4), so it can't show whether D works. Then the H27S17's own 1080p60 (138.5 MHz) for the default-DPI_CLK_SRC path. For each change: between "dp suspend OK" and `sprd_dp_encoder_enable`, a `sprd_dpu_atomic_disable`, `set mode: WxH` with `normal mode`, `sprd_dpu_atomic_enable` and `dpu_clk_init ... pixelclock is <new>`; a picture within about 2 s; no HOT_UNPLUG loop. The panel's own off and on still show one DSI-side `sprd_dpu_atomic_*` per change.
3. Unplug 20 times (the monitor's cable, the adapter, a dock), each time waiting for the USB 3 suspend, with a picture on the monitor and again with the Cube's screen asleep or the monitor's output off (the encoder already off, `pstore-dock.txt`'s case): no SError or panic, no `DPTX_ISTS_TYPE_C` while DP is off, no "nobody cared" or "Disabling IRQ" in dmesg after any unplug; the DP line in `/proc/interrupts` grows by a few at most; the composer sees the monitor disconnected. Replug: HPD, link training and a picture.
4. USB devices and a dock plugged and unplugged with no monitor; the screen off and on with a monitor plugged. After any reboot, save `/sys/fs/pstore/*` before the next test (each panic overwrites the one console-ramoops record) and look for any "Internal error", "SError" or "Kernel panic". The new code reports under the names of the functions it borrows, so name these sites: `dptx_irq` and `dptx_threaded_irq` (vendor), `dptx_soft_reset` and `dptx_global_intr_en` (the guards), `sprd_dpu_resume` (D), `dpu_init.1506` (C), and the known remaining `dpu_clk_disable` / `sprd_dpu_atomic_disable.202` (crash4, which D now reaches at every DP mode change).
5. Recovery with a monitor: it comes up as before (A, B and E, with the hot-plug fix; recovery's module `c7e8c829...` has not run on a Cube).
