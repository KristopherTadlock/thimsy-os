# Test 42 plan: display modes (1080p60 default, 1080p120, 1440p60) and the USB-C dock

Date: 2026-11-25. Status: built and run in test 42 (dp_dock_findings.md §12): the fixed mode table and
fix D (MOD-Q, MOD-M; B5, B7, B9), the dock dtbo with fix A (DK1), 4K30 (MOD-K, K1), 4K60 4:2:0 (MOD-Y1,
T3) and fix E (MOD-Y1-E, irqfix run 1) ran on the Cube. The plan below is kept as written. One part was
implemented differently: there is no "Display Modes" item (sections 4.3 and 7); Output Resolution stays,
as one list of resolution and rate entries, and the Refresh Rate setting is gone (README, External
monitors).

The owner's goal for test 42: "1440p60 working, the usb type c dongle working in all functions, along with
1080p120hz working. Allow the user to toggle display modes in the display settings under a new menu item
'display modes' with 1080p60hz being the default."

Sources this plan is built from (read them for the details behind each step):
- `dp_dock_findings.md`: the test 41 device results (M-xx, P-xx, R-xx IDs used below).
- `dp_modes_feasibility.md`: the clock tree, bypass, the mode table and experiments E0-E7, x1-x13, y1-y4.
- `display_modes_plan.md`: the framework and Nano design so far, and section 9 (driver fixes A-D, now in
  `scripts/sprd_drm.py`, uncommitted, never run on a Cube).
- `dock_multifunction_plan.md`: the dock root causes and fixes (dtbo fix A, IRQ fix C).
- Three research tracks, with scratch files in `out/dp-evidence/test42-research/`:
  - `1440p-bypass/`: P2, FLIP and MP words (`p2_words.py`, `p2.s`), and the snapped 1440p EDIDs (`edids/`).
  - `modes/`: the fixed mode table in the kernel (`words.txt`, `gm.s`, `mv.s`, `table.s`, `build.py`, a
    built but never-run `sprd-drm.test42-modes.ko`).
  - The Display Modes menu design (reported in text only; summarised in section 4).

Tags: **[C]** confirmed in a log, binary or file; **[I]** inferred; **[O]** open.

## 0. The plan in short

**Facts it rests on:**
- The DP pixel clock comes from PIXELPLL = 9 · floor(297/p) · p MHz, with p = pclk >> bypass. Only 2673 MHz
  (p divides 297) has ever given a picture [C].
- Normal mode seems limited to 1920 pixels wide: 2560x1440 failed at 241.5 MHz and at 297 MHz [C fact, I cause].
- Proven pictures [C]: CEA 1080p60 at 148.5 MHz (forced by an EDID override on the H27S17, which doesn't
  list it) and CEA 1080p120 at 297 MHz (that one unlogged, M-28 [O]).

**What test 42 does:**
1. **The kernel offers a fixed, safe set of modes, whatever the EDID says.** It adds them itself:
   - CEA 720p60 (74.25 MHz);
   - CEA 1080p60 (148.5 MHz), the only PREFERRED mode, so it is the default;
   - CEA 1080p120 (297 MHz);
   - 2560x1440@60 at 297 MHz through DPU1 bypass.

   Each mode is added only when the monitor lists something like it. mode_valid drops every other mode.
   This replaces fix C, which would start the H27S17 at its 138.5 MHz 1080p60, a rate that has never
   worked. Every mode on the list runs PIXELPLL at 2673 MHz.
2. **1440p goes through bypass.** P2 routes widths over 1920 to bypass, and FLIP removes a stray write.
   MP (DPTX dual pixel) is added only if an experiment says RGB bypass needs it. SurfaceFlinger composes a
   monitor in bypass into one layer.
3. **The dock fix:**
   - dtbo fix A: usb3's VBUS extcon moves to `pmic_typec`, so the DR_Swap starts the USB host and with it
     the combo PHY that DP needs.
   - Fix C: the `dw_dptx` IRQ moves off CPU0, so a slow training can't starve Wi-Fi.
4. **A "Display Modes" item** (as built instead: Output Resolution as one list, no Refresh Rate; see the
   status above) in Settings > Display Settings offers three choices: 1080p 60 Hz (Default),
   1080p 120 Hz and 1440p 60 Hz. A choice runs as the existing 15 s trial with Keep/Revert, and is saved
   once for every monitor.
5. **Experiments first.** The experiments in section 1 decide whether 1440p ships (and with which timing
   and whether MP is needed), and whether Docked mode can show a picture at all. If 1440p bypass fails,
   test 42 ships everything else and hides 1440p (section 7).

---

## 1. Device experiments that must run first

### 1.1 Rules for every experiment

- **Go-ahead.** Each experiment runs only with the owner's go-ahead in that session.
  - Phase A writes no partition.
  - Every Phase B partition write (vendor_dlkm_a, dtbo_a) needs its own explicit approval.
- **Never touch dwc3 debugfs.** Never read or write `/sys/kernel/debug/*dwc3*` (P-01 panicked the kernel).
  Mount debugfs only for `dri/0/DP-1/edid_override`, `dri/0/state`, `clk` and `regmap`.
- **Logging:** printk level 8 (`echo 8 > /proc/sys/kernel/printk` as root) and `dmesg -C; logcat -c`
  before each plug-in.
- **After any reboot,** copy `/sys/fs/pstore/*` off the Cube before the next test. Each panic overwrites the
  single record.
- **If the Cube hangs:** unplug every cable first, then hold power.
- **Unplug risk.** The installed driver has no IRQ guard, so an unplug at the Cube end can panic (PANIC A,
  intermittent, P-08).
  - To re-apply an EDID override, re-plug at the monitor end: pull the HDMI cable from the monitor and
    leave the USB-C adapter in the Cube. That drops HPD without the USB 3 PHY shutdown that raised PANIC A's
    Type-C interrupt [I].
  - Unplug the adapter only at the end of a session.
- **Dock tests** end with Power off from the power menu, then unplug.
- **Files:**
  - EDIDs in `out/dp-evidence/` (`edid-1080p60cea.bin`, `edid-1080p120cea.bin`),
    `out/dp-evidence/research-modes/edids/` (x1-x13, y1-y4) and
    `out/dp-evidence/test42-research/1440p-bypass/edids/` (y1b, y1c, y5).
  - Results go to `out/dp-evidence/test42/<experiment>/` (dmesg, logcat, `list` output, the monitor's OSD
    reading, the tester's picture/no-signal note).

**Common steps**, with `$adb = "adb"` (adb from PATH, or the full path to platform-tools\adb.exe; as in feasibility §7):
1. Once per boot: `& $adb shell "mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null"`.
2. `& $adb push <edid> /data/local/tmp/t.bin`.
3. `& $adb shell "cat /data/local/tmp/t.bin > /sys/kernel/debug/dri/0/DP-1/edid_override; dmesg -C; logcat -c"`.
4. Re-plug at the monitor end and wait 20 s.
5. Collect:
   - `dmesg | grep -iE 'pixelclock|DPI_CLK|pixelpll|bypass_mode|normal mode|underflow|layer count|tu =|Link training|HOT_|set mode|atomic_(en|dis)able'`
   - `cmd display gammaos-monitor-mode list`
6. Note what the monitor shows and its OSD resolution and rate.

### 1.2 Phase A: on the installed build, no partition writes

The Cube today [I, per T-02]:
- test 41 (1.4.4 system);
- `vendor_dlkm_a` holding the 1440-only driver (no fix A, B, C or D; preferred and user-defined modes only);
- Magisk, verity off, SELinux permissive;
- `persist.gammaos.display.mode=mirror`.

Without fix D, a mode change keeps DPU1's connect-time clock (R-02). So each experiment sets its mode at
connect, through an EDID override with one timing.

| # | Mode / setup | EDID | Steps beyond the common ones | Pass | What it decides |
|---|---|---|---|---|---|
| A0 | Control, Mirror | `edid-1080p60cea.bin` | none | Picture; `pixelclock is 148500000`; no HOT_UNPLUG for 5 min | Setup is sound. Stop if it fails |
| **A1** | **Docked mode** at 148.5 MHz (E7) | `edid-1080p60cea.bin` | Choose Docked in Settings > Display Settings > External Display first | The picture stays for 5 min after `PMU_APB_PIXELPLL pw off` appears in dmesg (the panel off) | Whether DP can show anything in Docked mode, the default. No DP picture in Docked mode has ever been logged (R-04 rules PIXELPLL out only for failures that happened anyway). **A failure blocks test 42**: Docked then needs its own fix (keep PIXELPLL powered while DPU1 runs), researched first |
| **A2** | CEA 1080p120, Mirror (puts M-28 on record) | `edid-1080p120cea.bin` | At connect, Auto switches to 640x480 within about 50 ms without a clock change (M-26). Within 5 s run `cmd display gammaos-monitor-mode try 1920 1080 120`, then `keep`. Hold 5 min | Picture; OSD 1920x1080 120 Hz; no HOT_UNPLUG; no `no composer vsync from monitor` in logcat | 1080p120 (table record R2) ships. Also the baseline that 1920 wide works at 297 MHz in normal mode |
| A3 | CEA 720p60, Mirror (E1) | `research-modes/edids/x2-720p60-cea.bin` | none (Auto holds it: it is the only mode at 50-61 Hz) | Picture; `pixelclock is 74250000`; ignore the "(vsync + vbp) < 32" warning unless `dpu1 underflow` follows | Table record R0 (720p60, the fallback for monitors smaller than 1080p) stays. If it fails, drop R0 |
| **A4** | RGB bypass at 3840x2160@30 (E5; the bypass track's A1) | `research-modes/edids/x1-4k30-cea.bin` | 1. `su -c resetprop ro.gammaos.display.uhd true`, then `stop; start`, so the framework lists 3840 (M-04). 2. Plug in; Auto goes to 640x480 within about 3 s. 3. `cmd display gammaos-monitor-mode try 3840 2160 30`, then `keep` (DPU1 still holds the connect-time 4K setup). 4. Hold 60 s. Collect `dmesg \| grep -iE 'bypass\|pixelclock\|layer count\|underflow\|HOT_'` | OSD 3840x2160 30 Hz with a picture; `bypass_mode`; `pixelclock is 297000000`; no `layer count error`, no `dpu1 underflow`, no HOT_UNPLUG | **Decides MP.** Picture: P2 without MP. "No signal", an HPD drop, or a half-width, doubled or garbled picture: build P2 with MP. If the uhd property is cached or the switch path gets in the way, skip A4 and test two Phase B modules in turn (without and with MP) |
| A5 | Normal-mode width sweep (E4a, E4b, E4c), Mirror | `x5-2048x1080p60.bin`, `x6-2304x1080p50.bin`, `x7-2560x1080p60-297.bin` | none | Record picture / no signal for each | The normal-mode width limit. P2's threshold stays 1920 for test 42 whatever the result. Not blocking; it tells whether the 2560 failures were width (expected: x7 fails) |
| A6 | Non-listed odd timing (E2), Mirror | `x12-1080p60-oddtotal.bin` | none | Picture | Whether the H27S17 takes a timing it doesn't list, which predicts B2. Optional |
| A7 | Read-only | none | `cat /sys/kernel/debug/dri/0/state` | DPU1's CRTC has 4 planes | Confirms the FLIP finding (bypass flip writes through planes[7] of a 4-plane array). FLIP ships either way |
| **A8** | Dock: Wi-Fi isolation (dock plan test 1) | none | Start the on-device logger (`dock_multifunction_plan.md` 6.1). Look up the IRQ by name: `awk -F: '/dw_dptx/ {gsub(/ /,"",$1); print $1}' /proc/interrupts`. Write `3` to `/proc/irq/<n>/smp_affinity_list`, and check that `Cpus_allowed_list` of `irq/<n>-dw_dptx` is 3. Run the VIA dock with its charger first and the TV on HDMI (DP still fails without dtbo fix A). Then Power off, unplug, and pull the logs | No `fw assert`, no `wcn_reset_process`; `ping` to the router keeps answering through the ~7 s training | Fix C ships, and fix D.1 (busy-wait bound) is not needed for Wi-Fi. A fail means the starved path is something else; look at the logs before test 42 |

Optional, not needed for test 42: E3a-c (PIXELPLL retune, with register readback). Every test 42 mode runs at
2673 MHz, so no retune is needed.

### 1.3 Phase B: test modules in vendor_dlkm_a, and the dock dtbo in dtbo_a

**Writing an image.** Each write needs the owner's approval for that image:
- Write it the way the 1440-only image was written (dd as root to the `vendor_dlkm_a` block device),
  after recording its sha256.
- Restore with `out/dp-evidence/vendor_dlkm_a-test41.img` (sha256 `ec24e7bb…`) or by installing test 42.
- Section 5.1 says how the images are made.

**The modules:**
- **MOD-P (probe module):**
  - contents: the all_modes test 41 patches + fixes A, B, C and D + P2 + FLIP, + MP only if A4 asked for it;
  - not in it: the mode table;
  - EDID overrides still pass whole, so this module tries timings.
- **MOD-M (the test 42 driver):**
  - contents: all_modes + A, B and D + the mode table + P2 + FLIP (+ MP);
  - the table has 4 records (NREC 4), and record R3 takes the timing that wins B2/B3;
  - fix C goes.
- **MOD-M3 (fallback):** MOD-M without P2 and FLIP, with 3 records (NREC 3). Built only if B2/B3 fail
  (section 7).

**SurfaceFlinger:** until the test 42 system build, one layer comes from `adb shell service call SurfaceFlinger
1008 i32 1` ("Disable HW overlays"). Set it again after every boot, before the plug-in.

| # | Module, mode | EDID / setup | Steps | Pass | Decides |
|---|---|---|---|---|---|
| B1 | MOD-P, Mirror | `edid-1080p60cea.bin` | as A0 | `normal mode`, picture, 148.5 MHz | P2 leaves 1920 and narrower untouched |
| **B2** | MOD-P, Mirror, SF 1008 | `test42-research/1440p-bypass/edids/y1c-2560x1440p60-297-snap3346.bin` (2560/2608/2640/3346, 1440/1443/1448/1481, +h −v, 59.934 Hz, 88.76 kHz: the monitor's own CVT-RB 1440p60 with the extra blank in hbp) | Plug in. Auto holds 2560x1440@60 (the largest mode at 50-61 Hz). Hold 5 min | OSD 2560x1440 60 Hz with a picture; `bypass_mode`; `pixelclock is 297000000`; `tu = 52, tu_frac = 8`; no `layer count error`, no `dpu1 underflow`, no HOT_UNPLUG; no oops | **1440p ships, with this timing** |
| B3 | as B2 | `y1b-2560x1440p60-297-snap.bin` (htotal 3342, 60.006 Hz), then `research-modes/edids/y1-2560x1440p60-297.bin` (3300x1500, 90.0 kHz) | Only if B2 fails | as B2 | Whether a second-choice timing works. If all three fail: with A4 passing, the monitor or the 2560 bypass is the problem; with A4 failing and MP in, bypass is. Either way 1440p is hidden (section 7). If a picture shows sparkles or `dpu1 underflow`, the DPTX threshold needs its dual-pixel value: a new patch, words not yet found |
| B4 | MOD-P, **Docked**, SF 1008 | the B2/B3 winner | Docked with the panel off, then once without SF 1008 to see the multi-layer failure (informative only) | as B2, in Docked | 1440p in the default mode (PIXELPLL power, A1's question at 297 MHz bypass) |
| **B5** | MOD-P, Mirror then Docked, SF 1008 | `y5-1080p60cea+1440p60snap.bin` (CEA 1080p60 preferred + snapped 1440p) | `try 2560 1440 60` / `revert`, 5 times in each mode | Each switch logs `sprd_dpu_atomic_disable`, `sprd_dpu_atomic_enable`, the new `pixelclock`, and `normal mode` / `bypass_mode`; a picture within 2 s; no oops | Fix D works into and out of bypass. Docked also covers the remaining PANIC B path (`dpu_clk_disable` → `clk_set_parent`), which fix D now runs on every DP mode change |
| B6 | MOD-P, Mirror, at 1440p | B2 winner | Unplug and re-plug 5 times at the monitor end, then 5 times at the adapter | pstore clean, no `SError` / `Internal error`, a picture after each re-plug | The IRQ guard B holds in bypass |
| **B7** | **MOD-M**, Docked, then Dual Screen, then Mirror | the H27S17's own EDID (reboot or `echo reset > …/edid_override`) | Connect. `list`. `try 1920 1080 120` + `keep`; `try 1280 720 60`; `try 2560 1440 60` (SF 1008 set); `auto` | See below | MOD-M is the test 42 driver |
| B8 | MOD-M, Docked | the 4K TV through the HDMI-only dongle (it lists VIC 16) | Connect; `list` | 1080p60 at 148.5 MHz is type 0x68 and the only preferred mode; the list has 1080p60 and 720p60, plus 1080p120 only if the TV lists 1080p at 119 Hz or more, plus nothing 2560 wide | The table on a TV |
| B9 | MOD-M | any monitor | 20 unplugs (`display_modes_plan.md` §9 device test 3): at the monitor end, at the adapter, with the Cube's screen asleep, and with the monitor's output off | No SError or panic; no `nobody cared` / `Disabling IRQ`; the DP line in `/proc/interrupts` grows by a few at most; a picture again after each re-plug | Unplug without panic |
| **DK1** | dtbo with dock fix A in `dtbo_a` (dock plan test 2), MOD-M in vendor_dlkm_a, IRQ fix C at runtime | the VIA dock | The dock plan's 6.3 matrix, rows 1-10 (plain charger, PC cable, DP-only adapter, HDMI dongle, **dock charger first + TV**, flipped plug, dock without charger, charger pulled, reboot docked, recovery with a PC cable). Back up both dtbo slots first; write only `dtbo_a` | Row 5: charging; `vbus:0 event received` after the swap, then `host:1` and `dwc3_sprd_otg_start_host`; USB3 hub 2109:0817 and USB2 hub 2109:2817 the first time; `Link training succeeded` with at most 3 PHY waits; a picture; Wi-Fi up; a USB stick mounts. Rows 1, 2, 9 and 10 unchanged from today | Dtbo fix A ships, and its `tested_sha256` is taken from this exact image. USB2 `-71` after the swap: try A+. Rows 1, 2 or 9 broken: go back to the test 40 dtbo and evaluate dwc3 fix B |

**B7 pass criteria** (from the modes track):
- **At connect:**
  - exactly four `sprd_dpu_mode_valid() mode:` lines (three with NREC 3), and type 0x68 only on
    1920x1080 148500 2200x1125;
  - one `sprd_dpu_atomic_enable` with `pixelclock is 148500000`, no later `set mode`, and a picture;
  - `list` shows 1920x1080@60, 1920x1080@120, 1280x720@60 and 2560x1440@60, and nothing at 138.5, 241.5,
    148.352, 296.703 or 285.5 MHz;
  - no oops in get_modes. This proves the calls through the borrowed `bl` words.
- **Each switch:** atomic_disable and atomic_enable with the new pixel clock, and a picture within about 2 s.
- **Docked:** also check the density in `dumpsys display`, since the synthesized modes have width_mm 0.

**Order:**
1. A0 → A1 → A2 → A3 → A4 → A7, then A5, A6 and A8 when time allows.
2. Build MOD-P (+MP per A4): B1 → B2 (→ B3) → B4 → B5 → B6.
3. Write R3's timing into the table and build MOD-M: B7 → B8 → B9.
4. Build the dock dtbo: DK1.

Phase A answers the go/no-go questions before any image is made.

---

## 2. Driver changes (`scripts/sprd_drm.py`)

All new code is in the all_modes (1.4.4, `SYSTEM_IMG`) variant only, unless marked "both". Recovery
(`patch_vendor_boot.py`) and the 1.2 base keep what they have today: PATCHES, HPD, IRQ guard B and fix A.

### 2.1 Kept as they are now

- `PATCHES` (both):
  - mode_valid's 299 MHz cap;
  - the link-bandwidth check;
  - fix A (`b.hs` at 0x8e2c: nothing 3840 or wider);
  - the 0x90f4 no-framebuffer-in-bypass patch, unless MP replaces it (2.4).
- `HPD_PATCHES` (both).
- `IRQ_GUARD_PATCHES`, fix B (both).
- `MODE_CHANGE_PATCHES`, fix D (all_modes). It is needed for every switch away from the connect mode,
  1440p included: a mode change must re-run `dpu_dpi_init` (reg 0x10 bypass bit), `dpu_init` (size) and
  `dpu_clk_init`.

### 2.2 Removed

- `SAFE_START_PATCHES` (fix C) and the `SAFE_START` word (0x8d8c index 1 → `b 0x202b4`). Fix C picks the
  H27S17's 138.5 MHz 1080p60 (PIXELPLL 2493 MHz, never worked), and its cave at 0x202b4 now holds the
  get_modes code.
- The `ALL_MODES` nop at 0x8d94. With the new dispatch the stub at 0x8d90 is unreachable, so the
  `ALL_MODES_WORDS` list ends up empty. Keep `patches()` handling an empty list.

### 2.3 New: the fixed mode table (all_modes; modes track)

The old and new words for every tuple are in `out/dp-evidence/test42-research/modes/words.txt`.
`build.py` there applies them to stock.ko and checks each original word. The assembler sources are `gm.s`,
`mv.s` and `table.s`.

**The table** is in `dpu_init.1618` at 0x227a0: 80 words, one 80-byte record per mode. Each record holds:
- name[16];
- the 12 timing words that map onto `drm_display_mode` +56..+103;
- vrefresh, type, gate (hd | vd<<16; 0 means always) and gate rate.

| Record | Mode | Clock (kHz) | H (active/ss/se/total) | V (active/ss/se/total) | Flags | Type | Created when the EDID lists |
|---|---|---|---|---|---|---|---|
| R0 | CEA 4 720p60 | 74250 | 1280/1390/1430/1650 | 720/725/730/750 | 0x5 | 0x60 | always |
| R1 | CEA 16 1080p60 | 148500 | 1920/2008/2052/2200 | 1080/1084/1089/1125 | 0x5 | **0x68 (the only PREFERRED)** | any mode at least 1920x1080 at 24 Hz or more, or no EDID |
| R2 | CEA 63 1080p120 | 297000 | as R1 | as R1 | 0x5 | 0x60 | exactly 1920x1080 at 119 Hz or more |
| R3 | 2560x1440@60 (bypass) | 297000 | the B2/B3 winner, below | | 0x9 | 0x60 | exactly 2560x1440 at 59 Hz or more |

**R3's timing:**
- `words.txt` has 3300x1500: `0xce4` (htotal), V 1443/1448/1500 (`0x5a3`, `0x5a8`, `0x5dc`).
- If y1c wins B2, change two words: htotal `0xce4` → `0xd12` (3346) and vtotal `0x5dc` → `0x5c9` (1481).
- If y1b wins: htotal `0xd0e` (3342) and vtotal `0x5c9`.
- vrefresh stays 60 in all three.

**NREC** is the record count, one immediate in each of two words:
- gm at 0x202d0: `0x52800098` (4) or `0x52800078` (3);
- mv at 0x24980: `0x5280008c` (4) or `0x5280006c` (3).

NREC 4 only together with P2 and FLIP (2.4).

**The get_modes cave** in `dpu_init.1506` at 0x202b4 (88 of 91 free words, the space fix C used):
- **Hooks:**
  - 0x8c7c `mov w19,w0` (`0x2a0003f3`) → `b 0x202bc` (`0x14005d90`);
  - 0x8ca0 `mov w19,wzr` (`0x2a1f03f3`), the no-EDID path, → `b 0x202b4` (`0x14005d85`). That stub sets
    x20 = 0 so that `kfree(NULL)` is harmless, then adds R0 and R1 without gates.
- **What it does:**
  - creates each record whose gate opens (`drm_mode_create`, filled in from the table, `drm_mode_probed_add`);
  - sets `picture_aspect_ratio` (+188) to 0 on every probed mode, so the EDID's CEA twins merge with the
    table's modes;
  - computes the gates from the EDID modes' rounded refresh.
- **No new relocation:** the cave calls through the already-relocated `bl` words at 0x10dec
  (drm_mode_create) and 0x10ce8 (drm_mode_probed_add) in the live `sprd_panel_get_modes`.

**The mode_valid cave** in `dpu_init.1724` at 0x2497c (25 words):
- **Hooks:**
  - 0x8cdc tuple word 3: `0x14000034` (`b 0x8db8`) → `0x14006f25` (`b 0x2497c`), entered after the
    299 MHz cap;
  - 0x8e3c tuple word 0: `0xb9403668` → `0x14000006` (`b 0x8e54`, MODE_OK instead of the old CEA
    dispatch). Both are new `ALL_MODES_WORDS` entries.
- **What it does:**
  - compares mode+56..+103 with each record;
  - on a match, stores the record's vrefresh (+180) and type (+52), then continues to the existing link
    check (0x8db8) and the 3840 check;
  - no match: MODE_CLOCK_HIGH (0x8dd8).

**Result on the H27S17:** exactly 1080p60 148.5 (0x68), 1080p120 297, 720p60 74.25, and 1440p 297 with
NREC 4. Its 138.5, 241.5, 59.94 Hz and 4K modes are dropped. A dock that trains HBR loses 120 Hz and 1440p
through the existing link check.

**What this means for testing:** on a test 42 build an EDID override only opens or closes the gates. It can
no longer bring in its own timing, so timing experiments use MOD-P or `vendor_dlkm_a-test41.img`.

### 2.4 New: bypass for 1440p (all_modes, with NREC 4; 1440p-bypass track)

The tuples are in `out/dp-evidence/test42-research/1440p-bypass/p2_words.py` (assembler source `p2.s`).
Their original words were checked against stock.ko.
- **P2 (required):** dpu_modeset.
  ```
  (0xb380, [0x713c011f, 0x54000101, 0xb9403a88, 0x7121c11f, 0x540000a1],
           [0x711e011f, 0x54000109, 0xd503201f, 0xd503201f, 0xd503201f])
  ```
  - `cmp w8,#0x780; b.ls 0xb3a4; nop×3`: anything wider than 1920 goes to bypass (ctx+428 = 1).
  - The printk adrp/add words at 0xb394, 0xb398, 0xb3a4 and 0xb3ac stay.
  - The other four users of the bypass flag need no change, for these reasons:
    - **dpu_dpi_init:** reg 0x10.
    - **dpu_clk_init:** p = 148.5 gives PIXELPLL 2673.
    - **encoder_mode_set:** RGB 8 bpc with no framebuffer or an RGB one.
    - **dpu_flip.**
  - The DPTX side takes the full 297 MHz mode (tu 52/8, 82.5% of HBR2 x2).
- **FLIP (with P2):** dpu_flip's bypass branch reads planes[7].state of DPU1's 4-plane array and writes a
  byte through it on every bypass flip.
  ```
  (0xaef8, [0xf95522a8, 0x3902611f], [0xd503201f, 0xd503201f])
  ```
- **MP (only if A4 or B2 need it):** the DPTX pixel mode (dptx+3461) is set to the bypass flag, so the DPTX
  takes 2 pixels per clock in bypass.
  - It **replaces** the 0x90f4 PATCHES tuple in the all_modes variant, and starts at 0x90f0. Both variants
    of the tuple are in `p2_words.py`.
  - It is a tuple swap like `ALL_MODES_WORDS`, not an extra tuple, since the two overlap.
  - encoder_mode_set runs on every modeset, so going back to 1080p writes 0 again.
- **Do not raise the 299 MHz cap.** A 2560-wide mode at 297.001-299.008 MHz would get PIXELPLL 1336.5 MHz.
- **One switch for the coupling.** Add `WIDE = True/False` to `sprd_drm.py`. When true it adds P2 and FLIP to
  `ALL_MODES_PATCHES` and sets NREC 4, and `WIDE_MP` picks MP.
  - `build.sh` writes `ro.gammaos.display.bypass_width=1920` (section 4) only when `SYSTEM_IMG` is set and
    `WIDE` is true.
  - A unit test checks that the property and the words agree.
- **Not in test 42** (words not yet found, or not needed):
  - P1 "snap to 297": not needed, since the table supplies the timings.
  - Relaxing fix A for 4K30.
  - The DPTX dual-pixel threshold (dptx+3510), only if B2 shows sparkles or underflow.
  - A fix for PANIC B (`dpu_clk_disable` after the VPU domain is off, P-06).
  - Dock D.1 (bound the PHY busy-wait at 0x32ec8) and D.2 (retrain on a later HPD-high).

### 2.5 Docstring, README and tests

- **Docstring:**
  - Fix A: 3840 and wider is still rejected, but bypass now also serves widths over 1920.
  - Fix C goes away.
  - New paragraphs for the table, P2, FLIP and MP.
- **`tests/unit/test_vendor_patches.py`:**
  - Both variants, and both NREC values (WIDE on and off).
  - The original-word and new-word checks for every new tuple, and branch targets:
    - 0x8c7c→0x202bc, 0x8ca0→0x202b4, 0x8ce8→0x2497c;
    - 0x20410→0x8c80, 0x249cc→0x8db8, 0x249dc→0x8dd8, 0x8e3c→0x8e54;
    - P2 `b.ls`→0xb3a4, plus the table's `adr`.
  - The words at 0x10dec and 0x10ce8 are `0x94000000` and carry CALL26 relocations to `drm_mode_create`
    and `drm_mode_probed_add`.
  - No relocation or `.altinstructions` entry falls inside 0x202b4-0x20414, 0x227a0-0x228e0 or
    0x2497c-0x249e0.
  - Only the dead CFI slots 0x3f2b0, 0x3f2b4 and 0x3f2b8 point at those function starts.
  - Fix C is gone, and `SAFE_START` is no longer applied.
  - The non-all_modes variant is unchanged (recovery and the 1.2 base).
  - `WIDE` and the build property agree.
  - Existing tests that name fix C or `ALL_MODES_WORDS` are updated.
- **README "External monitors":** rewrite the driver bullets: the fixed set, the preferred 1080p60, bypass
  for 1440p, and fix C gone.

---

## 3. The dock fix (`dock_multifunction_plan.md`)

**The cause** (dock plan section 2):
1. dwc3 takes its VBUS state from `extcon_gpio`, which stays 1 while the dock charges the Cube.
2. So after the DR_Swap, `dwc3_sprd_evt_prepare_work` stops at "vbus charger detect not finished", and the
   USB host never starts.
3. The USB side powers the USB3/DP combo PHY, so DP training then fails with -71.
4. The failed training busy-waits about 7 s on CPU0 at FIFO 50. That starves the SDIO host's IRQ thread
   (`irq/24-mmc2`, same CPU and priority), and the Wi-Fi chip resets and never recovers.
5. The PTN38003A messages are noise.

**The fixes in test 42:**
1. **Dtbo fix A** (`scripts/patch_dtbo.py`, dtbo entry 2 only, part of the existing all-or-nothing core
   set). It changes two fixup strings and nothing else:
   - remove `/fragment@8/__overlay__:extcon:0` from `/__fixups__/extcon_gpio`;
   - append the same string to `/__fixups__/pmic_typec`.

   Find the entries by string, as the patcher does today, never by fragment number.
   - **Expected merge:** `usb3@25100000 { extcon = <0x64 0x64>; }`, with nothing else different. The
     patcher's `fdtoverlay` check against vendor_boot's dtb shows it.
   - **Check:** extend the patcher's `check` to require it.
   - **Tests:** update `tests/unit/test_dtbo.py`.
   - **Approval:** set the RG Cube's `tested_sha256` only after DK1 passes with that exact image (README:
     the build stops without it).
   - **Fallbacks:** A+ (also move `usb`, `hsphy` and `ssphy` to `pmic_typec`) if USB2 still fails, and the
     dwc3-sprd one-word patch (`.text+0x2490`, `0x3901667f` → `0x7900b27f`) if A breaks the charger, PC or
     boot rows. Neither is in test 42 unless DK1 calls for it.
2. **Fix C: the `dw_dptx` IRQ off CPU0.** A boot action in `frameworks/base/cmds/gammaos-nano/gammaos-nano.rc`
   (the Thimsy patch) starts a oneshot service, like `nano-typec-guard`: `user root`, `seclabel u:r:su:s0`,
   `disabled`, `oneshot`.
   - **The script** (a new `/system/bin/thimsy-dp-irq.sh` beside the other Nano scripts):
     `irq=$(awk -F: '/dw_dptx/ {gsub(/ /,"",$1); print $1}' /proc/interrupts); [ -n "$irq" ] && echo 3 > /proc/irq/$irq/smp_affinity_list`.
   - **When:** on `on boot`, and again on `property:sys.boot_completed=1`, since sprd-drm loads in the
     background and may not have its IRQ yet at `on boot`. A dock attached at boot may train once on CPU0
     before that. Accept this, and note it in the README.
   - **SELinux:** writing `/proc/irq` from that domain is [I]; the Cube boots permissive today (T-01).
     Check for denials in the test 42 logs.
   - **Scope:** Cube-only in effect (it finds nothing on a device without `dw_dptx`).
3. **Not in test 42:**
   - D.1, bounding `dptx_phy_wait_busy`'s loop count. Every training hits 3 full waits, and the PHY may rely
     on them. With C in place, Wi-Fi no longer depends on it.
   - D.2, retrain on a later HPD-high. Words not yet found. It is needed only for a dock whose HPD comes
     before the host is up; with A this hub raises HPD about 1.9 s after the swap [I].
4. **Until DK1 and B9 pass:** every dock test still ends with Power off before unplugging.

---

## 4. SurfaceFlinger, framework and Nano changes (all in `patches/thimsy/0001-thimsy.patch`)

Every change below goes into the Thimsy patch. `scripts/aosp/sync_patch.py` applies it to the tree at
compile time.

### 4.0 One build property

**`ro.gammaos.display.bypass_width=1920`.** The build sets it only when the driver has P2 (section 2.4).
This plan uses this one name in place of the two the research tracks proposed (`ro.gammaos.display.wide`
and `bypass_width`). Unset means:
- nothing wider than 1920 is listed, except 3840 under the existing `ro.gammaos.display.uhd`, which stays
  unset;
- SurfaceFlinger adds no one-layer rule;
- Nano hides the 1440p option.

It goes into the system `build.prop` next to Nano's properties (`mods/50-nano.sh`), and into the device
profile once those exist (multi-device plan).

### 4.1 SurfaceFlinger

1. **One layer for a monitor in bypass.** This builds on the existing `thimsyMirror` flag (1440p-bypass track).
   - **CompositionEngine:**
     - `include/compositionengine/Output.h`: `virtual void setThimsySingleLayer(bool) = 0;`.
     - `impl/Output.h`: the override.
     - `mock/Output.h`: `MOCK_METHOD(void, setThimsySingleLayer, (bool));`.
     - `impl/OutputCompositionState.h`: `bool thimsySingleLayer{false};`.
     - `src/OutputCompositionState.cpp`: its `dumpVal` line.
   - **`src/Output.cpp`:**
     - The setter is written like `setThimsyMirror`: return if unchanged, then `editState()`, then
       `dirtyEntireOutput()`.
     - In `Output::updateCompositionState`:
       ```cpp
       const bool thimsyClientOnly = getState().thimsyMirror || getState().thimsySingleLayer;
       bool forceClientComposition = mLayerRequestingBackgroundBlur != nullptr || thimsyClientOnly;
       ```
     - The blur reset becomes `if (mLayerRequestingBackgroundBlur == layer && !thimsyClientOnly)`.
   - **`SurfaceFlinger.cpp`**, in `composite()`, right after `display->setThimsyMirror(mirror);`:
     `display->setThimsySingleLayer(device && gammaNeedsSingleLayer(*device));`.
   - **The helper `gammaNeedsSingleLayer`,** next to `gammaIsMirror`, declared in `SurfaceFlinger.h` with
     `REQUIRES(mStateLock)`:
     - true for a monitor (`gammaIsMonitor`) whose active mode is wider than `bypass_width`;
     - also true when its desired mode is, which covers the frame in which the composer switches.
     - **Check the A14 API names against the tree:** `getDesiredActiveMode()` and `ActiveModeInfo::modeOpt`
       are [I]; the WSL tree could not be read during research.
   - **Why it works:** forced client composition wins over the layer planner, so the vendor HWC gets one
     client target. `AddFlushData` drops layers only at 3840, so at 2560 it would otherwise pass up to 4
     layers [C].
2. **Restart the vsync check on a monitor mode change** (TODO.md, `display_modes_plan.md` Step 3, not done
   in test 40/41).
   - **The change:** when a monitor's active mode changes, set `mGammaVsyncEnabledAt` to now for it and
     clear `mGammaVsyncBroken`, so the 3 s no-vsync window doesn't mark a 120 Hz or 1440p switch (fix D:
     a full DPU1 disable and enable) as broken.
   - **Tests:** add a test where SurfaceFlinger's Thimsy tests live, or a device check (6.4).

### 4.2 Framework (`frameworks/base/services/core/java/com/android/server/display/`)

Merged from the UI and modes tracks. Where they disagree, this plan picks:
- one property (4.0);
- the flap guard instead of changing the applying marker (reason under DisplayManagerService below).

**`GammaosScreenArrangement.java`:**
- `BYPASS_WIDTH_PROP`, and `static final class Caps {boolean uhd; int bypassWidth;}`.
- **`listed(w, h, caps)`** becomes an allow-list: 1920x1080@60, 1920x1080@120 and 1280x720@60, plus
  2560x1440@60 only when `bypassWidth > 0`. The 3840 rule under `uhd` is unchanged; nothing below
  640x480.
  - The kernel already offers only these; this is defence in depth.
  - Thread `caps` through `offered`, `autoMode`, `monitorMode`, `tryRefusal` and `listOrder`.
- **`tryRefusal`:** new texts "not one of the display modes" and "wider than 1920 needs a build that sends
  it through bypass (ro.gammaos.display.bypass_width)".
- **`MonitorModes`:**
  - one global `choice` (`CHOICE_KEY = "choice"`), with lines `choice WxH@HZ` and `applying choice` in
    `/data/system/gammaos_monitor_modes.txt`;
  - the old per-monitor `mode <hex>` and `applying <hex>` lines are ignored and dropped at the next save.
  - **Why global:** the Cube's composer gives no `DeviceProductInfo` (deviceProductInfo null, key none, M-28),
    so per-monitor memory never persists there.
- **New pure class `Flaps`.** If a monitor held at a non-default choice is removed within 15 s of that mode
  first being active, and added again within 5 s, twice in a row, Auto is held for that uniqueId until
  reboot or a new choice. The saved choice stays.
- **`monitorMode` precedence:**
  1. a running trial;
  2. the global choice, if this monitor lists and offers it;
  3. Auto: 1920x1080 within 1 Hz of 60, which is table record R1, the owner's default.

**`LocalDisplayAdapter.java`:**
- `gammaosCaps()`.
- `updateDisplayModesLocked` filters with `listed(…, caps)`.
- `gammaosChoice`, `gammaosTryRefusal` and `gammaosListed` take `caps`.
- The hold stays pinned to `[r,r]`.

**`DisplayManagerService.java`:**
- `gammaosStoredModeLocked` returns the choice, or null under the flap guard.
- `gammaosMonitorConnectedLocked`:
  - an `applying choice` found at connect drops the choice, and sets `sys.gammaos.display.monitor_choice_dropped=<mode>`;
  - otherwise it writes `applying choice` and then applies the choice;
  - the marker is cleared after 20 s active or at an unplug, as today.
  - **Why not change the marker instead:** the modes track proposed keeping the marker on an early unplug.
    That would forget a choice whenever the user unplugs within 20 s. The flap guard handles the
    black-screen loop without that cost.
- `gammaosKeepMonitorMode` stores the choice.
- `gammaosAutoMonitorMode` and `gammaosForgetMonitorModes` work with no monitor (they clear the choice).
- New `gammaosPublishMonitorChoiceLocked`: publishes `sys.gammaos.display.monitor_choice` ("" for the
  default, else `WxH@HZ`) at boot and after keep, auto, forget and a dropped marker. The existing
  `sys.gammaos.` property context covers it, so no SELinux change is needed.
- `gammaosListMonitorModes`:
  - adds `choice WxH@HZ|none`;
  - prints `monitor none` with no monitor;
  - keeps the `key` and `stored` lines.
- `gammaosDisplayDeviceRemovedLocked` feeds `Flaps`.
- Remove `mGammaosSessionModes`.

**`DisplayManagerShellCommand.java`:** help text only. The verbs are unchanged:
`list|try W H HZ|keep|revert|auto|forget`.

### 4.3 Nano: the Display Modes item

**Placement.** The owner's words are "in the display settings under a new menu item 'display modes'".

**Recommended:** a "Display Modes" row directly in Settings > Display Settings, after External Display:
- a `kDisplayCh` entry in `NanoMenuPS3Data.h`;
- a branch in `ps3XmbSelect`'s `PS3_DATA_SUBMENU` next to `it.label == "External Display"` that calls
  `monitorModeOpen`;
- one line in `buildQuickDisplaySubmenu`, so the Quick Menu > Display copy gets it.

In the legacy settings tree (`NanoMenuSettingsTree.cpp`), add `b.action("monitor_display_modes","Display Modes")`,
and a `handleSettingsTreeSelect` case, only once the side panel has been checked in that theme.

**Alternative (the UI track's pick):** the row inside External Display, after the Docked, Dual Screen and
Mirror rows. That puts everything about the monitor in one place and needs no `kDisplayCh` change. **Owner
decision D1.**

**Either way:**
- The row replaces Output Resolution, Refresh Rate and Forget Monitor Resolutions.
- It shows even with no monitor connected. That is the black-screen recovery path: unplug, then choose
  Display Modes > 1080p 60 Hz (Default) on the Cube's screen.

**The row:**
- **Label** "Display Modes", icon 16.
- **Description:** "Sets the resolution and refresh rate a monitor runs at. 1080p 60 Hz is the default. A new
  mode shows for 15 seconds and is kept only if you choose Keep. The choice is used for every monitor that
  offers it."
- **Value:** the held preset, a non-preset mode set through cmd (modeWords), "Auto (WxH)" when the monitor
  lacks 1080p60, or the saved choice with no monitor.
- **Over a game:** dimmed with "Close the game to change the resolution", as today.
- **`monitorModeSig()`** adds `monitor_choice`, so the 500 ms tick rebuilds the row.

**The chooser:**
- A side panel (`mPs3DlgKind=1`) titled "Display Modes", on theme key 68; key 69 is retired.
- **Options, in order:**
  1. "1080p 60 Hz (Default)" = 1920x1080@60;
  2. "1080p 120 Hz" = 1920x1080@120;
  3. "1440p 60 Hz" = 2560x1440@60, listed only when `ro.gammaos.display.bypass_width` is set.
- **Cursor:** starts on the held mode, or on the saved choice with no monitor, else on Default.
- **No "Auto" entry:** Default is Auto. 720p60 is not shown (not asked for), but stays reachable with
  `cmd … try 1280 720 60`. Adding it later is one table line and one string.
- **Unavailable presets** (the monitor doesn't list them, or there is no monitor) are drawn at 0.45 alpha.
  - Mechanism: `mPs3DlgSwatch` value -3, multiplied in `renderPs3Dialog`'s side-panel item loop.
  - Choosing one shows "This monitor doesn't offer %s", or "Connect a monitor to try %s", and does nothing.

**Applying a choice** (off the UI thread, through `monitorModeCall`):
- **The held mode:** nothing happens.
- **Default:** `cmd display gammaos-monitor-mode auto` at once, no trial, banner "Setting changed". It
  works with no monitor.
- **Another preset:** `try W H HZ`, then the existing Keep This Resolution? dialog (key 70, cursor on
  Revert, guard 750 ms, closes 1 s before the system's 15 s revert), with the preset's label in the text.
  - Keep runs `keep` and shows "Setting changed".
  - B, closing and the timeout run `revert`.
  - A failure shows "The monitor could not switch to %s".
- **New guard in `monitorModeTick`:** if the trial is lost (`sys.gammaos.display.monitor_mode`'s reason is no
  longer `trial`, as after an HPD drop), close the dialog and show the failure message. Also show "The
  monitor went back to 1080p 60 Hz" once when `monitor_choice_dropped` is set, then clear it.

**Files:**
- `NanoMonitorMode.h/.cpp` (pure):
  - `Preset`, `presets(bool wide)`, `PresetRow`, `presetRows(list, choice, wide, dual)`,
    `parseChoiceProp`, `tryArgs`;
  - `parseList` reads `choice` and `monitor none`.
- `NanoMenuMonitorMode.cpp`: `monitorModeRows`, `monitorModeOpen`, `monitorModeResult`,
  `monitorModeChooser`, `monitorModeChosen`, `monitorModeTick`, `monitorModeSig`. Constants `kRowModes=0`,
  `kKeyModes=68`; drop `kRowRate`, `kRowForget` and `kKeyRate`.
- `NanoMenu.h`: `MMC_LIST_SIZES`/`MMC_LIST_RATES` become `MMC_LIST_MODES`, and `mMonModeChoices` becomes
  a `vector<PresetRow>`.
- `NanoMenuPS3Menu.cpp`:
  - `buildExternalDisplaySubmenu` loses the three old rows;
  - `applyThemeSetting` keeps `case 68`, and 69 goes;
  - `renderPs3Dialog` dims swatch -3.
- `docs/THEME_SYSTEM.md`: key 68 is the Display Modes chooser, and 69 is free.

**Strings.** New keys in all 21 `ps3xmb/i18n` files (`i18n_check.py` fails on a missing key or a `%s`
mismatch). The preset labels are whole keys, not formats:
- "Display Modes"
- the row description above
- "1080p 60 Hz (Default)"
- "1080p 60 Hz"
- "1080p 120 Hz"
- "1440p 60 Hz"
- "This monitor doesn't offer %s"
- "Connect a monitor to try %s"
- "The monitor went back to 1080p 60 Hz"

Reused keys: Keep This Resolution?, Keep, Revert, the countdown body, "The monitor could not switch to %s",
"Close the game to change the resolution", "Setting changed".

### 4.4 Unit tests (host)

**`test/unit/NanoMonitorMode_test.cpp`** (run by `tests/unit/test_nano_units.py` as `monitor_mode`):
- `presets(false)` has 2 rows and `presets(true)` has 3; the labels are exact; `tryArgs` gives
  "try 1920 1080 120".
- `presetRows` with the H27S17's test 42 list: all available, and "current" follows the held mode.
- A 1080p60-only TV: 120 and 1440 dimmed.
- No monitor: only Default enabled.
- Held 1920x1080@59.94 marks 1080p60 current; a held non-preset marks nothing current.
- Dual Screen: all presets still available.
- `parseChoiceProp`; `parseList` with `monitor none` and `choice`.
- Update the `kList` fixture.

**`test/java/GammaosScreenArrangementCheck.java`** (run by `tests/unit/test_screen_arrangement.py`):
- The allow-list:
  - 2560 wide is false without `bypassWidth` and true with it;
  - 1920x1080@60 and @120 and 1280x720@60 are true;
  - 1920x1200 is false;
  - 3840 needs `uhd`; 4096 never.
- `autoMode` on the H27S17 list picks 1920x1080@60.
- `monitorMode` with the global choice 1920x1080@120: "user" when the monitor lists it, "auto" when it
  doesn't.
- `tryRefusal` texts.
- `MonitorModes`: parse and format of `choice` and `applying choice`; legacy lines dropped; round trip;
  `dropIfApplying(CHOICE_KEY)`.
- `Flaps`: two quick cycles trip it, slow ones don't, and a new choice resets it.
- Update the existing `listed` tests for `caps`.

**Docs:**
- README "External monitors": replace the Output Resolution, Refresh Rate and Forget paragraphs with Display
  Modes, rewrite the "Only TV modes at 148.5 or 297 MHz…" bullet, and add the dock behaviour.
- TODO.md: the test 42 items.
- `display_modes_plan.md` §9: fix C superseded.

---

## 5. Build and install

### 5.1 Driver test images for Phase B (no system build)

1. **Make the module.** Implement 2.3 and 2.4 in `scripts/sprd_drm.py` with the variant switches (`WIDE`,
   `WIDE_MP`, NREC; a temporary switch for MOD-P, which keeps fix C and has no table). `patch_module()`
   then gives each module from the stock 1.2 `sprd-drm.ko` (sha256 `4d60c3c0…`). `build.py` in
   `test42-research/modes/` shows the same approach.
2. **Make the image.**
   - `scripts/patch_vendor_dlkm.py IN OUT`, run in the Docker build container (it has erofs-utils 1.7+),
     with `THIMSY_DP_ALL_MODES=1`.
   - IN is the stock 1.2 `vendor_dlkm` (`pactool.py` / `lptool.py` from the release PAC, as `build.sh`
     does). Not the test 41 image: its module is already patched and fails the sha check.
   - Name the images `out/dp-evidence/test42/vendor_dlkm_a-MOD-P.img` and so on, and record each sha256.
3. **Run the unit tests first:** `tests/unit/test_vendor_patches.py` and the disassembly check of each new
   word (rv-bt-binutils container).
4. **Install** (owner approval per write): push the image, `dd` it to the `vendor_dlkm_a` block device as
   root, read back and compare the sha256, then reboot. Restore with `vendor_dlkm_a-test41.img`.
5. **The dock dtbo for DK1:**
   - `scripts/patch_dtbo.py patch` with fix A, then its `check` with vendor_boot (`fdtoverlay`).
   - Back up `dtbo_a` and `dtbo_b` to the PC.
   - With approval, write only `dtbo_a`.
   - Recovery: adb + `dd` of the backup, or a Full install.

### 5.2 The test 42 build (1.4.4 system needed)

1. **Code:** the driver (2), dtbo fix A with its new `tested_sha256` from DK1 (3), the IRQ action (3), and
   the SurfaceFlinger, framework and Nano changes in `patches/thimsy/0001-thimsy.patch` (4).
   - Set `WIDE` and `WIDE_MP` from the Phase B results.
   - The build property follows `WIDE`.
2. **Host tests:** `tests/unit` (vendor patches, dtbo, screen arrangement, Nano units, i18n) in Docker.
   `SKIP_TESTS` stays unset.
3. **The system image:**
   - `scripts/build_144.sh --system-only` compiles it in the WSL distro `thimsy-aosp` (`/aosp/gammaos`,
     incremental through `sync_patch.py`).
   - **Two blockers:**
     - Another AOSP compile is running in that distro now. Wait for it to finish; never start a second one.
     - During the research the distro failed to start (`Wsl/Service/CreateInstance/E_FAIL`). Fix that first
       (`wsl --shutdown`, then start it again), or compile on the owner's other machine (Linux:
       `AOSP_DIR=… scripts/build_144.sh --system-only`, then copy `out/aosp/` across).
   - `build_144.sh --pac-only` refuses a `system.img` built from another version of the patch.
4. **The .pac:** `THIMSY_RELEASE="1.0a test 42" scripts/build_144.sh --pac-only`. With `SYSTEM_IMG` this
   gives `THIMSY_DP_ALL_MODES=1`, the patched vendor_dlkm (MOD-M) and vendor_boot (A+B only), and the dtbo
   on both slots (`DOCK_FIX=1`).
5. **Release:** a draft prerelease with the installer, as for test 41 (project release workflow).
   - The release notes say which parts have run on a Cube (Phase B) and which haven't. The SurfaceFlinger
     rule, the Display Modes UI and the boot IRQ action run first in test 42.
6. **Install:**
   - Use an installer mode that writes dtbo and super (test 41's Full install wrote `dtbo_a/b`, S-07). That
     also replaces the hand-written vendor_dlkm_a and dtbo_a.
   - Keep root (Magisk), or have another way to read dmesg, pstore and `/proc/irq`, for section 6's logs.

---

## 6. Device test plan for test 42

Monitors and adapters:
- the H27S17, whose EDID has no CEA 1080p60 and lists 2560x1440 and 1080p120, through the USB-C to HDMI
  adapter;
- the 4K TV through the HDMI-only dongle, and through the VIA dock;
- an HBR-only adapter if one is at hand.

Log as in 1.1, plus `dumpsys SurfaceFlinger` and `dumpsys display`. After every reboot, save pstore.

| # | Case | Pass |
|---|---|---|
| T1 | **1080p60 default on a monitor without CEA 1080p60** (H27S17), plugged in at the home screen, in Docked mode | At connect, `pixelclock is 148500000` with type 0x68 on the 1920x1080 148500 mode; no later `set mode`; picture; OSD 1920x1080 60 Hz; no HOT_UNPLUG for 5 min; `list` shows exactly the presets plus 720p60 |
| T2 | **1080p60 default on a TV** (4K TV, dongle) | As T1. The TV's CEA 1080p60 and the table's merge into one entry; no 3840 or 4096 entries; nothing below 1080p except 720p60 |
| T3 | Display Modes UI, no monitor | The row is in Display Settings (or External Display, per D1). With no monitor only Default is enabled, and the others say "Connect a monitor to try …". Choosing Default with no monitor works and clears the choice |
| T4 | **1080p120** on the H27S17: choose it, Keep; in Docked, Dual Screen and Mirror | atomic_disable then atomic_enable (fix D), `pixelclock is 297000000`, picture within 2 s, OSD 120 Hz; no `no composer vsync from monitor` (the vsync window restart); Dual Screen: the panel keeps working; one RetroArch game and one drastic-nano game for 10 min (pacing, heat) |
| T5 | **1440p60** (if `WIDE`): choose it, Keep; in Docked, Mirror and Dual Screen | `bypass_mode`, `pixelclock is 297000000`, tu 52/8, OSD 2560x1440 60 Hz; `dumpsys SurfaceFlinger` shows every layer on the monitor's output as CLIENT and one layer reaching the composer; no `bypass mode layer count error` in dmesg; the Keep dialog and the Quick Menu over a game are visible on the monitor; a game for 10 min in Docked and in Dual Screen; no `dpu1 underflow` |
| T6 | Switching back and forth | 1080p60 → 120 → Keep → 60 (Default) → 1440 → time out → 120 → B, in Docked and in Dual Screen: each change gives a picture within about 2 s; no HOT_UNPLUG for 2 min after; no oops (`dpu_clk_disable` path) |
| T7 | Rejected or lost modes | 1. Unplug during a trial: the dialog closes with "could not switch", and the reconnect is at 1080p60. 2. A dock that trains HBR (if available): 120 and 1440 are not listed, and a saved 120 falls back to Default for that connection. 3. Flap guard: force a saved choice the monitor drops (for example `try`/`keep` 720p on a monitor that rejects it), replug twice, and the third connect holds 1080p60 with "The monitor went back to 1080p 60 Hz" |
| T8 | Boot and replug with a saved choice | Boot docked with 120 (and 1440) saved: the boot animation is at 1080p60, the switch comes about 3 s after connect, and `applying choice` is gone from `/data/system/gammaos_monitor_modes.txt` 20 s later. `stop; start` within 20 s of a connect: the next connect is at Default and `monitor_choice` is "". Three replugs bring the choice back. A monitor that doesn't list the choice runs at Default, and the choice stays saved |
| T9 | **Dock, all functions** (VIA dock, charger first, TV on HDMI, keyboard and USB stick on the dock) | Charging (+0.5 A or more); after the DR_Swap: `vbus:0 event received`, `host:1`, `turn on host`; USB3 hub 2109:0817 and USB2 hub 2109:2817 with no second toggle; the keyboard types; the stick mounts; `Link training succeeded rate=2 lanes=2` with at most 3 PHY waits; TV at 1080p60; Wi-Fi stays up (ping through the connect); the `dw_dptx` IRQ thread is on CPU 3 (`Cpus_allowed_list`) |
| T10 | Dock variants | The plug flipped; the dock without its charger, charger added later; the charger pulled while docked (no back-feed); boot with the dock attached; plain wall charger; PC cable (adb works); recovery with a PC cable (adb) |
| T11 | **Unplug without panic** | 20 unplugs per case: the adapter at the Cube, the monitor cable, the dock (while charging), with the Cube's screen on and asleep, at 1080p60 and at 1440p (bypass). No SError, `Internal error` or panic; pstore clean; no `nobody cared` / `Disabling IRQ`; the composer sees each unplug; replug gives a picture. After this passes, the "Power off before unplugging" rule can go from the README |
| T12 | Docked / Mirror / Dual Screen matrix | Each display mode at each of 1080p60, 1080p120 and 1440p60: a picture; Docked turns the panel off and back on at unplug; Mirror shows the panel's content; Dual Screen keeps the panel at its own mode, with the monitor pacing it |
| T13 | Recovery with a monitor | Recovery (A+B driver, no table) comes up as before on the H27S17 and the TV |
| T14 | Heat and power | `thermal_zone*/temp` and `battery/current_now` after 10 min in Nano and in a game, at 1080p60, 120 and 1440, against 1080p60 |

---

## 7. Risks and fallbacks

**If 1440p bypass fails** (A4 and B2/B3, or T5): test 42 ships without 1440p.
- `WIDE = False`: no P2 or FLIP, NREC 3 (MOD-M3), and no `ro.gammaos.display.bypass_width`.
- The kernel offers 1080p60 (default), 1080p120 and 720p60. The framework lists no mode wider than 1920.
  Display Modes shows two choices. A 1440p monitor runs at 1080p60, scaled by the monitor.
- The README says 1440p needs work still to come. The research path then is:
  - the DPTX dual-pixel threshold, if MP gave a picture with sparkles or underflow;
  - the normal-mode width limit (A5);
  - a working PIXELPLL retune (E3) for the native 241.5 MHz timing.

Everything else can ship in test 42 on its own: the table, 1080p120, the dock fix and the Display Modes item.

**Other risks:**

| Risk | Sign | Fallback |
|---|---|---|
| **Docked mode can't show DP** (A1 fails: PIXELPLL power off kills the picture) | No picture in Docked at 148.5 MHz | Blocks test 42's default mode. Research a fix (DPU1 holding PIXELPLL power, or the panel's domain kept on while DP runs) before building. Mirror and Dual Screen would still work. Don't change the default display mode without the owner |
| A monitor rejects the synthesized CEA 1080p60 (only the H27S17 has been seen to accept a 1080p60 it doesn't list, TEST 7) | Black screen at connect, HPD flapping | The user unplugs, and the panel returns in Docked mode. Dual Screen / Mirror, or `cmd … try 1280 720 60`. A follow-up: after 2 quick HPD drops at Auto, use 720p for that connection (framework). Note it in the README |
| Fix D's DPU1 disable on every mode change reaches the PANIC B path (`dpu_clk_disable` → `clk_set_parent` after the VPU domain is off, P-06), especially in Docked mode | Oops in `sprd_dpu_atomic_disable.202` during B5, T6 or T11 | B5 and T6 test it before shipping. If it fails, find the fault (which clock mux register, and which power domain) and guard `dpu_clk_disable`, a new patch whose words are not yet found. Until then, ship without mode changes (Default only, no Display Modes trials): the table's 1080p60 at connect needs no fix D |
| Fix D after an unplug (a stale composer mode switch on an unplugged DP, as in P-06) | Panic after an unplug during a trial or an applying choice | T11 covers it. The framework drops trials on unplug already; check that applying a choice can't run on a removed display |
| Exact-match mode_valid hides modes some users want (1080p50, 1920x1200, 1440p75, 2560x1080) | Not listed | By design for test 42. Each can be added later as a table record at a 297/k or 594/k MHz clock |
| The borrowed `bl` words or the code caves misbehave | Oops in `sprd_panel_get_modes` / `dpu_init.1506` / `dpu_init.1724` names in traces | B7 tests them. The unit tests pin the relocations. Fall back to `vendor_dlkm_a-test41.img` |
| Dtbo fix A breaks the charger, the PC gadget or boot (DK1 rows 1, 2, 9) | Charging or adb broken | Keep the test 40/41 dtbo (no `tested_sha256` change), and evaluate the dwc3-sprd fix B. The dock then still has no USB host or DP, but nothing else regresses |
| USB2 fails first time after the swap | `device descriptor read/64 error -71` | A+ (all PHYs on `pmic_typec`), tested as part of DK1 |
| IRQ fix C blocked by SELinux, or a boot-docked training on CPU0 before it runs | `avc: denied` on `/proc/irq`; a Wi-Fi assert at a docked boot | Add the denial's rule in the Thimsy sepolicy. Run the action earlier (after the vendor modules load). D.1 as a follow-up |
| SurfaceFlinger rule leaves a DEVICE layer (desired-mode timing, IME, video overlay) | `layer count error` in dmesg at 1440p, missing overlays | Widen the rule: check the pending mode on the composer side. If it can't be fixed in time, ship without 1440p (above) |
| 1080p120 through a DP-to-HDMI dock or converter that can't carry 297 MHz | No picture at 120 | The trial reverts. The Flaps guard covers a kept one |
| Emulators at 120 Hz run fast or judder | T4 | Documented. Render-range option ([0,120]) as a follow-up (`display_modes_plan.md` decision 9) |
| The WSL AOSP distro won't start, or the running compile collides | No system image | The owner's other machine. Only driver work (Phase B images) can proceed without it |

**Owner decisions:**
- **D1:** Display Modes directly in Display Settings (recommended, the owner's words) or inside External
  Display.
- **D2:** Show 720p 60 Hz in the menu (not asked for; kept in the kernel and `cmd` either way).
- **D3:** Ship 1440p if it works on the H27S17 alone, or wait for a second 1440p monitor.
- **D4:** The global choice, not per-monitor (forced by the missing DeviceProductInfo), and the flap-guard
  thresholds (15 s and 5 s, tuned in T7).
- **D5:** Add the table synthesis to recovery and the 1.2 base too (the modes track's option: a monitor
  without CEA 1080p60 would then get 1080p60 there as well). Proposed: not in test 42.
