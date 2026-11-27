# DisplayPort modes on the RG Cube: what can it drive?

Status: research only, 2026-11-25. No code, test or partition was changed, and nothing was run on the Cube except read-only `ls` and `zcat /proc/config.gz`. The study is of the test 41 module `out/dp-evidence/sprd-drm-test41.ko`, the clock modules `clk-sprd.ko` and `ums9620-clk.ko` and the power-domain module `sprd-vpu-pw-domain.ko` (vendor_boot copies in `out/.dock/work/build-integration/vb/rd/lib/modules/`), the live DTS (`out/.dock/work/dp-driver/live.dts`), and the 2026-11-25 device logs in `out/dp-evidence/`. Scratch work is in `out/dp-evidence/research-modes/`:
- `modes.py` produces the numbers in section 4 and the EDID files.
- `table.md` is the raw table.
- `edids/` holds the experiment EDIDs.
- There are also disassemblies and symbol dumps of the clock modules.

**[C]** means confirmed from disassembly, the DTS or device logs. **[I]** means inferred, to be checked on the device.

## 1. Short answers

| Family | Works with the current driver? | Binary patch to sprd-drm.ko? | Impossible in hardware? | Confidence |
|---|---|---|---|---|
| **1440p60** | No. Two separate blockers: a 2560-wide mode gives no signal even at the proven 2673 MHz PLL rate, and the monitor's own 241.5 MHz timing needs PLL rate 2173.5 MHz, which has never worked. | Probably. Route bypass mode to 2560-wide modes (about 5 words in `dpu_modeset`) and drive a 297 MHz timing, which keeps the PLL at 2673 MHz. Also needs one-layer composition (SurfaceFlinger) and a monitor that accepts the timing. | Not at 60 Hz: the link has room (67-82%) and bypass is built for 594 Mpix/s. The width limit in normal (non-bypass) mode looks like hardware. | Medium that it is reachable this way; high that it fails today. |
| **1440p 50/75** | No (same blockers). | 50 Hz: same as 60. 75 Hz: 2640x1500 at 297 MHz in bypass keeps the PLL at 2673 MHz. | No. | Medium-low. |
| **1440p120** | No. | Bypass, plus a 540 MHz cap, PLL retune and an HBR3 sink. | Yes on any HBR2 link (138%). Possible only with an HBR3 sink (92%). | Low. |
| **1440p144** | No. | No. | Yes: 108% of 2-lane HBR3, RGB 8 bpc. | High. |
| **4K30/25/24 RGB** | Unknown. The vendor's bypass path at the proven 2673 MHz has run at connect three times without a panic, but never long enough to show a picture. Thimsy fix A (no width of 3840 or more) removes it from the next build. | Revert fix A, then one-layer composition. A possible DPTX dual-pixel setting (section 3.4). | Probably not: 82.5% of the link at the proven PLL rate. | Medium-low. |
| **Below 1080p** | Yes for clocks that divide 297 MHz, when set at connect: 720p50/60 (74.25), 480p/576p (27.0), 1080p50 (148.5). With fix D this also holds after a mode change. No for standard DMT clocks (25.175, 40, 65, 85.5, 108, 119 MHz), because they need a PLL rate other than 2673 MHz. | Yes, two options. A "snap to 297" retiming patch (about 25-40 words in a code cave) moves every such mode onto 297/k. The other option is to make the PLL retune work (unknown size, section 2.4). | No. | High for the 297/k modes; medium for snapped timings (monitor acceptance). |
| **1920x1200** | No at its CVT-RB 154 MHz clock (PLL 1386 MHz). Yes predicted at 148.5 MHz timings. | Snap (as above) or retune. | No. | Medium. |
| **2560x1080** | No (width). | Bypass for wide modes. At CEA VIC 90 (198 MHz) bypass gives p = 99 MHz, which divides 297, so the PLL stays at **2673 MHz** with no snapping needed. | No. | Medium. |

What the hardware can do, in one paragraph:
- **Link.** 2-lane HBR2 carries up to 360 MHz RGB; 2-lane HBR3 up to 540 MHz.
- **Normal mode.** The DPU1 runs at most 297 MHz and, it seems, no wider than something between 1920 and 2560 pixels.
- **Bypass mode.** One layer only, 2 pixels per clock, up to 594 MHz.
- **PIXELPLL.** A fractional-N PLL that should reach any rate from about 1.3 to 3.2 GHz. Only 2673 MHz has ever given a picture.
- **The practical rule today:** pixel clock = 297/k MHz (594/k MHz in bypass) and width ≤ 1920.

## 2. How the DP pixel clock is really made

### 2.1 The clock tree [C]

```
ext-26m -> pixelpll (g5r PLL, ums9620-clk .data 0x1338, reg anlg_phy_g5r 0x64320000+0xa8..0xb0)
             |  (hardware sees VCO/9 on this input; see 2.2)
             +--> ipa_dpi mux [tgpll_128m, tgpll_192m, pixelpll, tgpll_307m2] + 4-bit divider (1..16)
             |       = DT "clk_dpu_dpi" <0x47 6>  -> DPU1 DPI clock -> DPTX video
             +--> pixelpll_668m25 (VCO/4) = DT "clk_src_680m" of a VSP (video codec) node  <0x30 0x0d>
             +--> pixelpll_297m   (VCO/9), pixelpll_41m76 (VCO/64)
tgpll_512m -> ipa_dpu = DT "clk_dpu_core" <0x47 5>   (DPU1 core: always 512 MHz)
PMU 0xa5c bit 2 = "pixelpll_gate" (clock driver) AND vpu_pd_top's pmu-apb-pixelpll-syscon (power domain)
```

- **DPU1 is `dpu_lite_r3p0`.** Its core ops table `dpu_lite_r3p0_core_ops` (.rodata 0xb3a8) points through CFI stubs to `dpu_init` 0xaa3c, `dpu_isr` 0xac24, `dpu_dpi_init` 0xad48, `dpu_flip` 0xae54 and `dpu_capability` 0xb0e0 (4 layers). Its clock ops `qogirn6pro_dpu1_clk_ops` (0xb460) point to `dpu_clk_parse_dt` 0xbc24, `dpu_clk_init` 0xbe94, `dpu_clk_enable` 0xc0c4 and `dpu_clk_disable` 0xc1b0.
- **`dpu_clk_parse_dt` stores** 128m, 192m, 256m, 307m2, 384m, 512m, core, dpi and pixelpll at .bss +0x08…+0x48.
- **`dpu_clk_init` 0xbe94, step by step:**
  1. Choose the DPI source from the **full** pixel clock. In order: 128 MHz if it divides, then 192 MHz, then 297 MHz, then 307.2 MHz; otherwise print "use default DPI_CLK_SRC" and take 297.
  2. Print `DPU_CORE_CLK = 512000000, DPI_CLK_SRC = <choice>`.
  3. `clk_set_parent(core, 512m)`.
  4. Parent the DPI clock on that choice. **297 means the PIXELPLL input** (0xbfa8-0xbfe0).
  5. Always `clk_set_rate(pixelpll, 9*floor(297e6/p)*p)`, then `clk_set_rate(dpi, 9*p)`, with `p = pclk >> bypass` (0xbff4-0xc020).

  Failures print "dpu update pixelpll clk rate failed" or "dpu update dpi clk rate failed".
- **`dpu_clk_disable`** resets the parents (dpi → pixelpll, core → 192m) and sets pixelpll back to **2673 MHz** and the DPI clock to 2673 (0xc218-0xc240).
- **Corrections to `display_modes_plan.md` §2** (read-only for this study):
  - The DPU1 core clock is always the 512 MHz source; 297 MHz is the DPI-source label.
  - "DPI_CLK_SRC = 297000000" is the **mux choice "PIXELPLL"**, not a fixed 297 MHz clock.
  - Every DP mode seen so far, 1080p60 included, runs from PIXELPLL. `README-root-cause.txt` item 2's "any DP rate that would need PIXELPLL" therefore covers all of them.
- **The PIXELPLL clock** (`sprd_pll_ops`, flags 0, parent ext-26m): 3 registers.
  - Bit fields as (shift, width): lock_done (17,1); div_s (0,1); mod_en (1,1); sdm_en (2,1); ibias (3,3); n (6,11); **nint (55,7)**; **kint (32,23)**; postdiv (67,1).
  - Ibias table bands: ≤2.0 GHz, ≤2.8 GHz, ≤3.2 GHz. fvco 1.6 GHz with fflag 1 (postdiv /2 below 1.6 GHz). k1 = k2 = 1000. This is the same layout as mainline `drivers/clk/sprd/pll.c`.
  - **2673 MHz is itself fractional:** 26 × 102.8077.
  - `sprd_pll_round_rate` returns the request unchanged (0x19f0). `sprd_pll_set_rate` (0x19f8) is a regmap read-modify-write followed by `udelay(240)`, with **no lock check**. So `clk_set_rate` "succeeds" for any rate, whether or not the PLL locks.
- **`ipa_dpi`** (`sprd_comp_ops`, flags 0, so **no CLK_SET_RATE_PARENT**): a 2-bit mux at reg 0x88 and a 4-bit divider. The driver's own rate calls decide both PLL and divider.

### 2.2 The ×9 [C from behaviour]

The driver asks the clock framework for a DPI clock of 9×p, for example 1336.5 MHz for 1080p60, and that mode works. So the DPI input of the mux is really VCO/9; the clock framework's numbers are 9× the hardware's.

The real pixel clock is `VCO / 9 / k` with `k = floor(297/p)`:
- When p divides 297 MHz, the VCO stays at 2673 MHz.
- For every other p, the VCO must move to `9·k·p`, which is in (1336.5, 2673] MHz.

### 2.3 What has been seen

| PIXELPLL (VCO) | Modes | Result |
|---|---|---|
| 2673 MHz (VCO, no postdiv) | 1080p60 CEA, 1080p120 CEA, 4K30 bypass | picture (4K30 not confirmed) |
| 2493 MHz | 1080p60 CVT-RB 138.5 | no signal |
| 2492.3 MHz | 640x480@60 25.175 | no signal |
| 2173.5 MHz | 1440p60 CVT-RB 241.5 | no signal |

No run printed "pixelpll clk rate failed" (expected: `set_rate` cannot fail) or "dpu1 underflow". That warning is a plain `printk` in `sprd_dpu_isr.197` (0x9c8c) on status bit 2, and `dpu_dpi_init` enables that interrupt (0x3f5 at reg 0x1e0). No run logged Thimsy's "no composer vsync from monitor" either. So DPU1 kept running and producing vsync, but the monitor saw no valid stream **[I]**.

### 2.4 Why rates other than 2673 MHz fail: open, ranked [I]

1. **The PLL does not settle on the new rate when it is reprogrammed while running.** It might need to be powered or gated around the write, or the fractional (SDM) path might need a reset. There is no lock check. Every failing VCO was also below 2.5 GHz, so a narrower real VCO range is possible, even though the driver's ibias table claims 2.0-2.8 GHz.
2. **The DPTX stream clock no longer matches what the controller expects** (Mvid/Nvid or video FIFO), because the actual DPI rate is not what the timing says.

Experiment E3 (section 7) separates these by reading the PLL registers back.

**Side effects of retuning PIXELPLL:**
- The video codec's 680 MHz option is PIXELPLL/4, so it would run slower (never faster: the VCO never exceeds 2673 MHz).
- PMU 0xa5c bit 2 has two owners: the clock driver's `pixelpll_gate` and `vpu_pd_top`. DPU1 (IPA power domain 0x44) holds neither. "PMU_APB_PIXELPLL pw off" therefore happens whenever the panel/VSP domain powers down.

  Whether that bit is a real power switch or only a vote is unknown. If it is a hard switch, no DP mode can survive Docked mode, 1080p60 included. No log so far shows a DP picture surviving "pw off", so this is open; experiment E7 tests it.
- **The 128/192/307.2 MHz paths are probably broken for DP [I].** The DPI clock is still asked for 9×p from a 128-307 MHz parent. They are reached only when p divides 128, 192 or 307.2 MHz (24, 32, 48, 64, 96, 38.4, 76.8, 153.6 MHz…). No common monitor mode uses those clocks. A future mode_valid should still reject them.

## 3. Line width, bypass and earlier 4K attempts

### 3.1 What software programs [C]

- `dpu_init`: reg 0x20 = `hactive | vactive<<16` (16-bit fields). No blend-size register is written.
- `dpu_dpi_init`: reg 0x1f4 = `hsync | hbp<<8 | hfp<<20` and reg 0x1f8 = the same for vertical. The fields are therefore **sync ≤ 255 and porches ≤ 4095**, the same layout as the upstream Unisoc `sprd_dpu.c`.
- It warns when `vsync+vbp < 32` ("underflow risk"). CEA 720p (25), CVT-RB 1080p (28) and CEA 2560x1080 (16) all trip that warning.
- `dpu_layer` 0xb618 has no width check, and `dpu_capability` reports no size.

Nothing in software limits the width. A 2560-wide mode is programmed exactly like a 1920-wide one.

### 3.2 Evidence for a hardware width limit in normal mode [C for the facts, I for the conclusion]

- At the same proven clock (297 MHz pclk, PIXELPLL 2673, normal mode, HBR2x2, tu=52 vs tu=52), 1920x1080@120 shows a picture.
- 2560x1440@60 with totals 3300x1500 and 2720x1820 does not (`dmesg-1440at297.txt`, `dmesg-1440tall.txt`, clean connects).
- The vendor puts **every** 3840x2160 mode into bypass (`dpu_modeset` 0xb37c: `hactive==0xf00 && vactive==0x870`), 4K30 at 297 MHz included. That clock is within the normal-mode DPI cap, so normal mode cannot be width-limited by clock there: a width limit somewhere in (1920, 3840] must exist.
- The failures look like "no signal", not garbage, and give no underflow interrupt. That fits a DPU1 that outputs lines shorter than the DPTX's programmed `hactive`: the DPTX video FIFO desynchronises and sends no valid stream.

The remaining doubt is that the monitor rejected both custom 297 MHz timings. Experiments E2 and E4 settle it.

### 3.3 What bypass does and needs [C]

- **Trigger.** `dpu_modeset` sets ctx+428 = 1 for exactly 3840x2160.
- **Effects.** `dpu_dpi_init` sets reg 0x10 bit 0. `dpu_clk_init` uses `p = pclk/2`, so the DPI clock is half the pixel clock: 2 pixels per clock, a 594 MHz pixel-clock ceiling.
- **Layers.** `dpu_flip` (0xaee4) wants exactly one layer. With more it prints "bypass mode layer count error" and **carries on** anyway, then loads a 9-value table to regs 0x424/0x428 (probably the YUV→RGB matrix).
- **Output format.** `sprd_dp_encoder_mode_set` (0x90ec) chooses the format from the **bypass flag, not the width**: NV12 or P010 framebuffer → YCbCr 4:2:0, anything else → RGB 8 bpc. A wider bypass trigger therefore inherits RGB output for free.
- **Composer.** The vendor HWC drops non-3840-wide layers only when the active mode is 3840 wide (`SprdDrm::AddFlushData`, plan §1). At 2560 in bypass it would pass several layers, so SurfaceFlinger must compose everything into one client target. For tests that is `service call SurfaceFlinger 1008 i32 1` ("Disable HW overlays", not persistent). For shipping it is a Thimsy SurfaceFlinger rule for monitors whose mode is in bypass.

### 3.4 A bypass risk not in the plan [C fact, I effect]

The DPTX "multipixel" byte (dptx+3461, VSAMPLE_CTRL bits 22:21 in `dptx_video_core_config` 0x36b8c) is **never written** anywhere in `.text`. It stays 0, so the DPTX is always in single-pixel input mode.

In bypass the DPU1 sends 2 pixels per DPI clock at pclk/2. If the DPTX samples one pixel per clock, then RGB bypass (and the vendor's own 4K60 NV12 path) cannot work unless the SoC widens the bus on its own. This is the first thing E5 (4K30) tells us. If 4K30 shows a picture, bypass works and section 5's bypass routes are open. If 4K30 shows "no signal", setting multipixel = 1 (dual) in bypass is about 4 words.

### 3.5 What made earlier 4K attempts panic or show nothing [C]

- **The panic** was the composer's hotplug commit with no framebuffer reaching the bypass format check. Thimsy's 0x90f4 patch fixed it.
- **"Nothing" in test 41.** The composer started the H27S17 in 4K30 bypass at every plug-in (`dmesg-run2-ring.txt` 79.99 s, 116.3 s and 141.3 s). The framework switched to 1920x1080 within 2.3-3 s each time (82.94 s, 118.6 s), before a monitor normally syncs. That switch kept the 4K bypass divider and bypass bit (no `dpu_clk_init`, no `dpu_dpi_init`).

So RGB 4K30 bypass has **never had a fair test**; there is no evidence that it fails.

## 4. Mode table

Columns:
- **Gbit/s**: RGB 24 bpp.
- **% HBR2**: share of the 2-lane HBR2 payload (8.64 Gbit/s), the H27S17's link.
- **PIXELPLL**: the VCO rate the driver programs, normal / bypass (2673 MHz is the only rate ever proven).
- **Width path**: normal ≤ 1920 is proven; wider needs bypass.

Experiment IDs refer to section 7.

| Mode | Pclk MHz | Gbit/s (% HBR2) | PIXELPLL MHz normal / bypass | Width path | Works now? | Needs | Evidence | Conf. | Safe test |
|---|---|---|---|---|---|---|---|---|---|
| 2560x1440@60 CVT-RB (H27S17 native, 2720x1481) | 241.5 | 5.80 (67%) | 2173.5 / 2173.5 | bypass | No | Bypass for wide + working retune (or snap) + one layer | `dmesg-mirror.txt`, `dmesg-1440only.txt`: no signal | High (fails); Med (fixable) | y4 |
| 2560x1440@60 snapped (3300x1500) | 297 | 7.13 (82%) | 2673 / 2673 | bypass | No (normal: no signal) | Bypass for wide + one layer + snap or EDID | `dmesg-1440at297.txt`, `-tall.txt` | Med | y1 |
| 2560x1440@50 CVT-RB (2720x1474) / snapped (3300x1800) | 200.25 / 297 | 4.81 / 7.13 | 1802 / 2673 | bypass | No | As above | none | Med-low | y1 variant |
| 2560x1440@75 snapped (2640x1500); CVT-RB is 304.25 | 297 | 7.13 (82%) | 2673 / 2673 | bypass | No | Bypass + monitor accepting hblank 80; CVT-RB also needs cap > 299 and retune (1369) | none | Med-low | y3 |
| 2560x1440@120 (H27S17 DTD 2720x1525) | 497.75 | 11.95 (138%; 92% of HBR3x2) | — / 2240 | bypass | No | HBR3 sink, cap 540, retune | link trained HBR2 | Low | none on H27S17 |
| 2560x1440@144 | 583.6 | 14.0 (162%; 108% HBR3) | — | — | **Impossible** (RGB 8 bpc, 2 lanes) | — | arithmetic | High | — |
| 3840x2160@30 CEA 95 (4400x2250) | 297 | 7.13 (82%) | — / 2673 | bypass (vendor) | Unknown; listed and run at connect by test 41 / 1440only, never seen | One layer; fix A reverted; maybe DPTX dual-pixel | `dmesg-run2-ring.txt` | Med-low | x1 |
| 3840x2160@25/24 CEA 94/93 (5280/5500x2250) | 297 | 7.13 | — / 2673 | bypass | As 4K30 (hfp 1056/1276 fits 12 bits) | As 4K30; the sink must take 24/25 Hz (H27S17 lists VIC 93) | none | Med-low | x1 with 93/94 timing |
| 3840x2160@30 CVT-RB | 262.75 | 6.31 | — / 2364.75 | bypass | No | Retune | none | Low | — |
| 1280x720@60 / @50 CEA 4/19 | 74.25 | 1.78 (21%) | 2673 (k=4) | normal | Yes at connect (predicted); after a switch only with fix D | — (vs+vbp 25 warns) | never run | Med-high | x2 |
| 720x480@59.94 / 720x576@50 CEA | 27.0 | 0.65 | 2673 (k=11) | normal | Yes at connect (predicted) | — | never run | Med-high | — |
| 640x480@60 DMT/CEA 1 | 25.175 | 0.60 | 2492.3 (k=11) | normal | No | Retune or snap to 29.7 (900x550) or 27.0 (858x525) | `dmesg-try640.txt` | High (fails) | x11 |
| 800x600@60 DMT | 40.0 | 0.96 | 2520 | normal | No (predicted) | Retune or snap 42.43 (297/7, 1056x670) | — | Med | — |
| 1024x768@60 DMT | 65.0 | 1.56 | 2340 | normal | No (predicted) | Retune or snap 59.4 (297/5, 1250x792) | — | Med | — |
| 1280x1024@60 DMT | 108.0 | 2.59 (30%) | 1944 | normal | No (predicted) | Retune or snap 99 (1500x1100) | — | Med | x8 |
| 1366x768@60 DMT | 85.5 | 2.05 | 2308.5 | normal | No (predicted) | Snap 74.25 (1500x825); odd width 1366 untested anywhere | — | Med-low | — |
| 1440x900@60 CVT-RB | 88.75 | 2.13 | 2396.25 | normal | No (predicted) | Snap 99 (1650x1000) | — | Med | — |
| 1600x900@60 DMT-RB | 108.0 | 2.59 | 1944 | normal | No (predicted) | Snap 99 (1760x937, 60.03 Hz) | — | Med | — |
| 1680x1050@60 CVT-RB | 119.0 | 2.86 | 2142 | normal | No (predicted) | Snap 148.5 (2200x1125) | — | Med | x9 |
| 1920x1080@60 CEA 16 | 148.5 | 3.56 (41%) | 2673 | normal | **Yes** | — | `dmesg-edid.txt` | High | control |
| 1920x1080@60 CVT-RB (H27S17's own) | 138.5 | 3.32 | 2493 | normal | No | Retune or snap to CEA 16 | `dmesg-mirror.txt` | High (fails) | x4 |
| 1920x1080@120 CEA 63 | 297 | 7.13 (82%) | 2673 | normal | **Yes** | — | `e120.log`, `dmesg-1080p120.txt` | High | — |
| 1920x1080@120 CVT-RB | 285.5 | 6.85 | 2569.5 | normal | No (predicted) | Retune | — | Med | — |
| 1920x1080@144 CVT-RB | 346.5 | 8.32 (96%) | — / 1559 | needs bypass for clock | No | Bypass for > 297 MHz + retune; 96% of link | — | Low | — |
| 1920x1200@60 CVT-RB | 154.0 | 3.70 | 1386 (postdiv) | normal | No (predicted) | Retune, or snap 148.5 (2000x1235 60.12 Hz or 2200x1350 50 Hz) | — | Med | x10 |
| 2560x1080@60 CEA 90 (3000x1100) | 198.0 | 4.75 (55%) | 1782 / **2673** (k=3) | bypass | No | Bypass for wide + one layer; no snap needed | width evidence above | Med | x7, then y2 |
| 2560x1080@50 CEA 89 | 185.625 | 4.46 | 1670.6 / 2505.9 | bypass | No | Bypass + retune | — | Low | — |
| 2560x1080@60 snapped (3300x1500) | 297 | 7.13 | 2673 / 2673 | bypass | No | Bypass + one layer | — | Med | x7 |

The full numeric table, with HBR3 and 297/p exactness for every row, is `out/dp-evidence/research-modes/table.md` (`python -I modes.py`).

## 5. Per family

### 5.1 1440p (2560x1440)

**Current driver.** No. Normal mode at 2560 width gave no signal even at 2673 MHz. The native 241.5 MHz timing additionally needs PLL rate 2173.5 MHz.

**Binary patch.** Probably yes, through bypass:
- **P2:** in `dpu_modeset` (0xb37c-0xb390), replace `cmp hactive,#3840; b.ne; ldr vactive; cmp #2160; b.ne` with `cmp hactive,#1920; b.ls normal` plus nops. That is 5 words in place, with no relocations in that range other than the printk `adrp`/`add`s at 0xb394/0xb398/0xb3a4/0xb3ac, which stay.
- **mode_valid:** 2560-wide modes are already allowed (fix A rejects only 3840 or more). 1440p75 at its CVT-RB clock would also need the 299 MHz cap relaxed when width > 1920, a few words in the existing cap tuple.
- **Timing:** feed it a 297 MHz timing. A monitor-specific EDID would work, but the general fix is **P1 "snap to 297"**: in `sprd_dp_connector_mode_valid`, which takes a non-const mode in 5.4, rewrite a mode whose clock (or clock/2 in bypass) does not divide 297 MHz.
  - Pick `clock' = 297000/k` kHz (594000/k in bypass) as the nearest value at or above the clock that still leaves `htotal' = clock'·1000/(vtotal·vrefresh)` ≥ `hsync_end + 16` and the back porch ≤ 4095.
  - Then set `clock` and `htotal`. The extra blanking goes into the back porch, and the refresh rate moves by under 0.1%.
  - About 25-40 instructions in a code cave (the dead `dpu_init.1506` area that fix C uses may be full: choose another dead per-SoC function).
- **One layer:** the one-layer rule in SurfaceFlinger (framework, not the driver).
- **Possibly** the DPTX dual-pixel setting (3.4).

**Hardware.** 60 Hz fits everywhere (67-82% of HBR2). 120 Hz needs an HBR3 sink. 144 Hz is impossible at RGB 8 bpc on two lanes. Four lanes would need DP pin assignment C/E, which the driver hard-codes off and which would take USB3 from the dock.

**Test.** E2 (does the monitor accept a non-listed timing) and E4 (width sweep) with the current build. Then y1/y3/y4 with a P2 build and "Disable HW overlays".

**Confidence.** High that today's failure is real. Medium that bypass + 297 MHz gives 1440p50/60/75. Low for 120.

### 5.2 4K30 / 25 / 24 (3840x2160 RGB through bypass)

**Current driver.** Unknown. Test 41 and `sprd-drm-1440only.ko` list and run it, at 2673 MHz, three times without a panic, but it was never held long enough to see. The next build's fix A rejects it.

**Binary patch.** Revert fix A, or limit it to 4096-wide. If E5 shows "no signal", add DPTX dual-pixel in bypass (`dptx+3461 = 1` before `dptx_video_core_config`, about 4 words). If it shows a picture with missing or garbled overlays, it is the HWC layer drop (fix with one-layer composition, no driver change).

**Hardware.** Probably possible: 82.5% of HBR2, the proven PLL rate, the vendor's own 4K path. 4K60 RGB is impossible (165%); 4K60 NV12 is plan §8.

**Test.** E5 (x1) now, with the installed build, in Mirror mode.

**Confidence.** Medium-low for an RGB picture without DPTX changes; high that it does not panic at connect.

### 5.3 Below 1080p

**Current driver.**
- Modes whose clock divides 297 MHz work when set at connect: 720p50/60 (k=4), 480p/576p 27 MHz (k=11), 1080p50 (k=2), 1080p100 (k=1).
- After a mode switch they work only once fix D (clock re-init on mode change) is in.
- Standard DMT clocks need other PLL rates and fail today. 640x480@60 failed on the device.

**Binary patch.** P1 snap covers all of them, with the snapped timings listed in section 4. Simplest interim alternative: reject in mode_valid every mode whose clock does not divide 297 MHz, as `README-root-cause.txt` suggests (about 5 words). Either way, also reject clocks that divide 128/192/307.2 MHz.

**Hardware.** No limit: the PLL's 4-bit divider allows p ≥ 18.6 MHz, and the link has ample room.

**Test.** E1 (720p), E6 (snapped 640x480, 1280x1024, 1680x1050).

**Confidence.** High for 720p/480p/576p. Medium for snapped DMT sizes (depends on E2).

### 5.4 Other sizes

**1920x1200.**
- Its CVT-RB 154 MHz needs PLL 1386 MHz, the postdiv path, which has never been tried.
- A 148.5 MHz timing works at 2673 MHz: 2200x1350 at 50 Hz (comfortable) or 2000x1235 at 60.12 Hz (tight blanking).
- Width 1920, so normal mode.
- The framework already groups 1920x1080 and 1920x1200 by width (plan §1). That is fine once modes are picked explicitly.

**2560x1080.**
- Width forces bypass. CEA VIC 90 then lands on 2673 MHz by itself (p = 99 = 297/3).
- Its vsync+vbp (16) trips the "underflow risk" warning; watch for "dpu1 underflow".

**Test.** E6 (x10), E4 (x7), then y2.

**Confidence.** Medium.

## 6. Recommended order of work

1. **Run E0-E3 first; they need no new build.** They tell whether the PLL retunes at all (register readback) and whether the monitor accepts non-listed timings. Everything else depends on these two answers.
2. **Ship the safe set** with fixes C/D in place: only modes whose pixel clock divides 297 MHz and with width ≤ 1920 (CEA 720p/1080p 50/60/100/120, 480p/576p). Add the mode_valid reject for other clocks, so Android never offers a mode that gives no signal.
3. **If E2 passes, add P1 "snap to 297".** This brings back the monitor's own 1080p (H27S17's CVT-RB) and 1280x1024, 1600x900, 1680x1050, 1920x1200, 640x480, all at the proven 2673 MHz.
4. **If E3 shows the PLL retuning and locking,** keep `dpu_clk_init`'s exact clocks instead of snapping where they work. If it does not, drop retune entirely; snapping covers every listed family except 1440p120.
5. **Run E5 (4K30) and E4 (width sweep).** If bypass works, build P2 (bypass for widths over 1920, about 5 words) together with SurfaceFlinger's one-layer rule. Test y1-y3 (1440p60/75 snapped, 2560x1080 CEA 90), then y4 (native 241.5, only if step 4 succeeded).
6. **4K30 in Output Resolution** only after E5 shows a picture and the one-layer rule is in. This means relaxing fix A to "wider than 3840".
7. **Docked mode** (E7) before any non-2673 rate ships, because of the PIXELPLL power bit.
8. **Not worth pursuing:** 1440p144 or 4K60 RGB (link-impossible), and 1440p120 unless someone has an HBR3 monitor.

Steps 2-3 are kernel-module patches in `scripts/sprd_drm.py`, which another workflow is editing now. This study proposes and does not implement them.

## 7. Device experiments (ready to run; each needs the owner's go-ahead)

### Rules for every experiment

- **Mode and overrides.** Mirror mode (`persist.gammaos.display.mode=mirror`), panel on. EDID overrides only. **No partition is written.**
- **Driver.** The one installed now (test 41, or `sprd-drm-1440only.ko` from `README-1440only.txt`). Both lack fix A, so 3840x2160 is accepted. With only one detailed timing in the EDID, that timing is the preferred mode, so the composer sets it **at connect** and `dpu_clk_init` runs with its clock.
- **Auto switches.** Auto holds the test mode when it is in 50-61 Hz. For x1 (30 Hz) Auto may switch to 640x480. Right after the plug-in, run `cmd display gammaos-monitor-mode try 3840 2160 30` and watch during the 15 s trial: the clock set at connect is still in place.
- **Never touch** `/sys/kernel/debug/*dwc3*/mode`.
- **If the Cube hangs or panics:** unplug **every** cable first, then hold power. pstore keeps the crash; read `/sys/fs/pstore` after the reboot. Unplugging the monitor can itself panic (the unfixed `dptx_irq` bug), so do the experiments in one session and unplug only at the end.
- **Files.** All EDIDs are in `out/dp-evidence/research-modes/edids/`. Each is the H27S17 base block with one detailed timing, no established or standard timings, and no extension.

### Common steps (PowerShell, from `out\dp-evidence\research-modes` in the repository)

Setup, once per boot:

```powershell
$adb = "adb"    # or the full path to platform-tools\adb.exe
```

```powershell
& $adb shell "mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; ls /sys/kernel/debug/regmap | grep -i 6432"
```

For each experiment, with the monitor unplugged:

```powershell
& $adb push edids\x2-720p60-cea.bin /data/local/tmp/t.bin
```

```powershell
& $adb shell "cat /data/local/tmp/t.bin > /sys/kernel/debug/dri/0/DP-1/edid_override; dmesg -C; logcat -c"
```

Plug the monitor in and wait 20 s. Then collect:

```powershell
& $adb shell "dmesg | grep -iE 'pixelclock|DPI_CLK|pixelpll|bypass_mode|normal mode|underflow|layer count|tu =|Link training|HOT_|set mode'"
```

```powershell
& $adb shell "cmd display gammaos-monitor-mode list; cat /sys/kernel/debug/clk/pixelpll/clk_rate /sys/kernel/debug/clk/ipa-dpi/clk_rate"
```

```powershell
& $adb shell "grep -iE '^0*(a8|ac|b0):' /sys/kernel/debug/regmap/*64320000*/registers"
```

Write down what the monitor shows (picture / black / "no signal") and its OSD resolution and refresh rate. Clear the override with `echo reset > .../edid_override` only when the next experiment needs a different EDID; the next `cat` replaces it anyway.

### Reading the PIXELPLL registers

The registers are at anlg_phy_g5r 0x64320000 + 0xa8/0xac/0xb0. Decoded from the clock driver's bit fields:
- Word 0xa8: bit 0 = div_s (fractional enable), bit 17 = lock_done.
- Word 0xac: bits 0-22 = kint, bits 23-29 = nint.
- Word 0xb0: bit 3 = postdiv.
- Rate ≈ 26 MHz × (nint + kint/2^23), halved with postdiv.

Expected 0xac values (approximate):

| PIXELPLL | 0xac ≈ |
|---|---|
| 2673 MHz | 0x33676276 |
| 2592 MHz | 0x31d89d8a |
| 2493 MHz | 0x2ff13b14 |
| 2173.5 MHz | 0x29cc4ec5 |
| 1458 MHz (VCO 2916, postdiv) | 0x3813b13b |

E0 calibrates this decode.

### Experiments

| # | EDID file | Timing (pclk; H active/fp/sync/bp; V active/fp/sync/bp; total; rate; sync polarity) | PIXELPLL / k | What it answers | Pass looks like |
|---|---|---|---|---|---|
| E0 | `../edid-1080p60cea.bin` (existing) | 148.5; 1920/88/44/148; 1080/4/5/36; 2200x1125; 60.000; +/+ | 2673 / 2 | Control; calibrates the register decode | Picture; 0xac ≈ 0x33676276, lock bit set |
| E1 | `x2-720p60-cea.bin` | 74.25; 1280/110/40/220; 720/5/5/20; 1650x750; 60.000; +/+ | 2673 / 4 | Divider k=4 path; whether the vs+vbp<32 warning matters | Picture at 720p |
| E2 | `x12-1080p60-oddtotal.bin` | 148.5; 1920/48/32/80; 1080/3/5/102; 2080x1190; 59.995; +/- | 2673 / 2 | **Does the H27S17 accept a timing it never listed?** (gates every snapped timing and y1) | Picture |
| E3a | `x3-1080p60-144m.bin` | 144.0; 1920/48/32/80; 1080/3/5/66; 2080x1154; 59.992; +/- | **2592** / 2 | Does a retune work at all (VCO above 2.5 GHz)? | Picture; 0xac ≈ 0x31d89d8a, lock set |
| E3b | `x4-1080p60-cvtrb.bin` | 138.5; 1920/48/32/80; 1080/3/5/23; 2080x1111; 59.934; +/- | **2493** / 2 | Repeat of the known failure, now with register readback | Today: no signal. Do the registers show 2493 and lock? |
| E3c | `x13-1080p60-162m.bin` | 162.0; 1920/88/44/148; 1080/4/5/136; 2200x1225; 60.111; +/+ | **1458** (postdiv, VCO 2916) / 1 | Retune through the postdiv path | Picture or not; registers |
| E4a | `x5-2048x1080p60.bin` | 148.5; 2048/48/32/72; 1080/4/5/36; 2200x1125; 60.000; +/+ | 2673 / 2 | Normal-mode width limit (2048) | Picture = limit is above 2048 |
| E4b | `x6-2304x1080p50.bin` | 148.5; 2304/88/64/184; 1080/4/5/36; 2640x1125; 50.000; +/+ | 2673 / 2 | Width 2304 | |
| E4c | `x7-2560x1080p60-297.bin` | 297; 2560/88/88/564; 1080/4/5/411; 3300x1500; 60.000; +/+ | 2673 / 1 | Width 2560 with 1080 lines (vs the two failed 1440-line tries) | Expected: no signal |
| E5 | `x1-4k30-cea.bin` | 297; 3840/176/88/296; 2160/8/10/72; 4400x2250; 30.000; +/+ | 2673 / 2 (bypass p=148.5) | **Does RGB bypass work at all?** Then run `try 3840 2160 30` and hold 15 s | Picture (UI possibly missing overlays); `bypass_mode`; no panic |
| E6a | `x8-1280x1024p60-99m.bin` | 99.0; 1280/72/48/100; 1024/3/7/66; 1500x1100; 60.000; +/+ | 2673 / 3 | Snapped DMT size | Picture |
| E6b | `x9-1680x1050p60.bin` | 148.5; 1680/120/176/224; 1050/3/6/66; 2200x1125; 60.000; +/+ | 2673 / 2 | Snapped 16:10 | Picture |
| E6c | `x10-1920x1200p50.bin` | 148.5; 1920/88/44/148; 1200/3/6/141; 2200x1350; 50.000; +/+ | 2673 / 2 | 1920x1200 at the proven rate | Picture |
| E6d | `x11-640x480p60-29m7.bin` | 29.7; 640/72/64/124; 480/10/2/58; 900x550; 60.000; -/- | 2673 / 10 | Snapped 640x480 | Picture |
| E7 | (any passing EDID, e.g. x2) in **Docked** mode | — | — | Does "PMU_APB_PIXELPLL pw off" (panel off) kill a running DP picture? | Picture stays after `pw off` in dmesg |
| y1 | `y1-2560x1440p60-297.bin` (= `edid-1440p60at297.bin` timing) | 297; 2560/48/32/660; 1440/3/5/52; 3300x1500; 60.000; +/- | 2673 / 2 (bypass) | 1440p60 through bypass. **Needs a P2 build** + `service call SurfaceFlinger 1008 i32 1` | Picture |
| y2 | `y2-2560x1080p60-cea90.bin` | 198; 2560/248/44/148; 1080/4/5/11; 3000x1100; 60.000; +/+ | 2673 / 3 (bypass p=99) | 2560x1080 CEA 90. Needs P2 | Picture |
| y3 | `y3-2560x1440p75-297.bin` | 297; 2560/8/32/40; 1440/3/5/52; 2640x1500; 75.000; +/- | 2673 / 2 (bypass) | 1440p75. Needs P2 | Picture |
| y4 | `y4-2560x1440p60-cvtrb.bin` | 241.5; 2560/48/32/80; 1440/3/5/33; 2720x1481; 59.951; +/- | 2173.5 / 2 (bypass) | Native 1440p60. Needs P2 and a working retune (E3) | Picture |

### How to read the results

- **E0 fails:** the setup is wrong; stop.
- **E1 fails:** the divider path is broken for k ≠ 1, 2, which is unexpected; stop and look at dmesg.
- **E2 fails:** snapping is not viable for this monitor. Rely on retune, and test E2 again on another monitor.
- **E3a passes but E3b fails:** the VCO has a lower limit around 2.5 GHz. Limit retune to above 2.5 GHz and snap the rest.
- **E3a fails, with registers showing the new rate and lock:** the problem is DPTX-side (Mvid/FIFO). Snap everything.
- **E3a fails, with registers showing no lock:** the PLL needs a reprogramming sequence. That is a P3 patch; until then, snap.
- **E4:** the largest passing width is the normal-mode limit. Make the P2 bypass threshold that width.
- **E5 gives "no signal":** try DPTX dual-pixel before any bypass work.
- **E5 gives a picture:** bypass works, and y1-y3 are worth building for.

## 8. Sources

- **Disassembly and data** of `sprd-drm-test41.ko`, `ums9620-clk.ko`, `clk-sprd.ko` and `sprd-vpu-pw-domain.ko`, with the addresses given above. The DTS is `out/.dock/work/dp-driver/live.dts` (dpu1@31880000, dptx@31890000, g5r-pll, clock-controller@25010000 `sprd,ums9620-ipa-clk`, power-domain@0 `vpu_pd_top`). The device logs are in `out/dp-evidence/`.
- **Upstream Unisoc DRM driver** (`dpu_r2p0` only; no `dpu_lite_r3p0` or DP code is public). Its DPI timing register layout matches what is decoded here: [android common kernel drivers/gpu/drm/sprd](https://android.googlesource.com/kernel/common/+/android14-6.1/drivers/gpu/drm/sprd/sprd_dsi.c), [sprd_dpu.h](https://git.zx2c4.com/linux-dev/tree/drivers/gpu/drm/sprd/sprd_dpu.h), and the [patch series](https://lists.freedesktop.org/archives/dri-devel/2021-January/293070.html).
- **Unisoc's T820 page** claims multi-screen output "up to 4K HDR10+", with no DP mode table: [unisoc.com T820](https://www.unisoc.com/en_us/home/T5GSJ-T820). No public source gives the DPU1 line width or the PIXELPLL range; both remain to be tested on the device (E3, E4).
