# RG Cube USB-C dock plan: power in, HDMI and USB at the same time

Date: 2026-11-24 (revised after a technical review and a safety review; first version 2026-11-23)
Status: implemented for test 40 as section 12 lists, which ships the c2c dtbo without a device test (the
owner's decision); none of it has run on the device. One device-tree fix (v3) runs on the test Cube as a hand edit.
Evidence: `out/.dock/FINDINGS.md` (live testing, Thimsy 1.0a test 37), the logs and images in `out/.dock/`,
six research write-ups with scratch files in `out/.dock/work/<topic>/` (roleswap-driver, dp-driver,
charging-15w, build-integration, dock-matrix, t820-portability), and the two reviews' scratch files in
`out/.dock/work/plan-review/` and `out/.dock/work/plan-safety/`.

## Short answer: does a Cube driver patch work on other T820 devices?

- **The patched `.ko` files: no.** A patched module loads only on the exact same kernel build (vermagic
  `5.4.233 SMP preempt mod_unload modversions aarch64`, `__versions` symbol CRCs, Clang CFI), and fixed
  offsets move between builds. The Cube's kernel was built on 28 Feb 2025 (`uname.txt`). Other devices'
  build dates are not checked locally (the "RG556 Feb 2024" figure from the research is unverified). A device
  that ships byte-identical modules would be the only exception; nobody has checked yet (D8/Q7).
- **The patch technique: only after re-locating every site by anchors** (symbol, instruction pattern,
  relocations), never by copying offsets.
- **The device-tree route (`use_pdhub_c2c`) is the portable one, but it is not proven yet, even on the Cube.**
  The switches are in Unisoc's shared 5.4 drivers, so a device whose modules contain the same code should
  take the same kind of DT edit. That edit must be **written per device, never copied**: in c2c mode VBUS
  has to come from a `vbus-supply` on the PD node, and it must point to that board's own charger/boost
  regulator. Each device also needs its own offline check and hardware tests (DP wiring, charger IC and OTG
  rating, redriver).
- Anbernic's own support page says the RG556 can't output video while charging and that this is "not a
  hardware defect". That matches the Cube's symptom, but doesn't prove the same cause.

Section 7 has the details.

## Conventions

- **CONFIRMED**: seen in the code, the binaries or the logs. **INFERRED**: reasoned from evidence, not tested.
  **UNKNOWN**: not determined.
- Addresses like `.text+0x594` are offsets into the module's `.text` section. "File" gives the offset in the
  `.ko` file. `.text` starts at file offset 0x1000 in `sc27xx_typec.ko`, 0x2000 in `sc27xx_pd.ko`,
  `dwc3-sprd.ko`, `musb_sprd.ko`, 0x3000 in `sprd_tcpm.ko`, 0x30000 in `sprd-drm.ko`.
- Module hashes (sha256, from `out/.dock/modules/`, the Cube's vendor_dlkm):
  - `sc27xx_typec.ko` 03186a0f...
  - `sc27xx_pd.ko` beae150f...
  - `dwc3-sprd.ko` 1e0cb758...
  - `musb_sprd.ko` 53301ee5...
  - `sprd_tcpm.ko` b3ec27ea...
  - `sprd_typec_displayport.ko` 9fcf78fe...
  - `sprd-drm.ko` 0659ff53... on the device (Thimsy's patched copy), 4d60c3c0... original (`scripts/sprd_drm.py` `MODULE_SHA256`)
- "dtbo entry 2" is the Cube's board overlay: `UMS9620 2H10`, id 0x2100, `androidboot.dtbo_idx=2`.
- "Base DT": the device's `dtb_a.img` backup is all zeros (CONFIRMED). The base DT the overlay is applied to
  is the dtb section of vendor_boot (`work/build-integration/vb/dtb`, Unisoc dt_table magic 0xd7b7ab1e).
- State names used in the dock matrix: **(a)** original DT, **(b)** DT fix v3 only (what the test Cube runs now),
  **(c)** this plan done.

---

## 1. Goal and scope

**Goal:** a powered USB-C dock gives the RG Cube three things at once:
- power in (the Cube charges);
- picture on the dock's HDMI/DP output;
- working USB (USB3 and USB2) on the dock's ports.

Today the Cube gets only one of these at a time, depending on plug order. Nothing that works today
(HDMI dongles and bus-powered hubs with the Cube as source, OTG adapters, plain charging) may break.

**In scope:**
- The DT fix (data role, PDOs, c2c mode with its VBUS supply) shipped through the build and the installer.
- The role-swap bug: after a dock makes the Cube USB host, the USB controller must follow.
- The DP gaps that stop or break the picture with docks.
- Charging: how close to "15 W" the Cube can get, safely.
- Nano / NanoTypec changes so userspace helps instead of fighting the dock.
- A dock and adapter matrix with expected behaviour.
- A portability design for the other Anbernic T820 devices (RG556, RG406H, RG406V, RG476H, RG Slide).

**Out of scope:** 4-lane DP (the Cube's DP controller is capped at 2 lanes), MST (sprd-drm is SST only,
INFERRED), Fast Role Swap, VCONN sourcing. The Wi-Fi drops get their own short section (11) as a separate
investigation.

**15 W in short:** not reachable. The AW32257 charger takes about 10-11 W at most (INFERRED from the chip
limits). Anbernic's rating of the Cube at 5 V 2 A is reported by the research but not verified here. The
target is 2.0 A charge current (about 9-10 W in) from sources whose contract allows it, and only with a
design that fails safe. See 4d.

---

## 2. What testing established

All CONFIRMED on the device unless marked. Details in `FINDINGS.md`.

**Test hub:** VIA Labs USB-C hub (PD id 2109:0102, HUB/AMA), DP caps 0x00000c05 (UFP_D, pins C and D),
USB2 hub 2109:2817, USB3 hub 2109:0817, billboard 2109:8817, HDMI, PD pass-through. With its charger it
offers 5V 1A / 9V 1.66A / 12V 1.25A. **Reference:** an HDMI-only dongle (057e:2003, pin C) works with the
Cube as source.

**Failures with the original DT** (connector in dtbo entry 2, fragment@14):
- Hub without charger: Cube is source+host at 5 V 400 mA. DP enters, Status 0x1a, HPD never rises (hub
  probably underpowered). USB works.
- Charger added afterwards: nothing. No PR_Swap from the hub (Cube's source PDO has no DRP bit).
  Sysfs swaps are refused: `[sprd_tcpm_pr_set] can't power role swap`, `can't data role swap`.
- Port forced sink-only: Cube charges at about 5 V 1 A as UFP. The hub sends DR_Swap every 1.6 s; the Cube
  refuses (no `data-role` in DT, so its type-c caps are DFP only).

**The DT fix** (written by hand to dtbo_a; slot B and the backups untouched):
- `data-role = "dual"`; sink PDO 0x2601912c (5 V 3 A plus DRP|DRD).
- Source PDO variants:

| Variant | source-pdos | Meaning | Result |
|---|---|---|---|
| original | 0x04019028 | 5 V 400 mA, USB comm | hub never raises HPD |
| v1 | 0x26019028 | DRP+DRD, 400 mA | no log kept |
| v2 | 0x2601905a | DRP+DRD, 900 mA | hub never answers Source_Capabilities, endless hard resets |
| **v3** | **0x0401905a** | USB comm, 900 mA | hub negotiates 900 mA; HDMI-only dongle works. **Runs now** |

- Result with v3, hub with charger plugged first: Cube is sink, accepts DR_Swap (`DR_SWAP_ACCEPT ->
  DR_SWAP_CHANGE_DR`, `set roles [sink:host]`), charges at about 5 W (5 V 1 A).

**The role-swap bug:** after the swap to host, sc27xx_typec logs `sc27xx_typec_set_pd_dr_swap_flag entered!`,
`sc27xx_typec_set_pd_swap_event pd_swap_flag:4!`, `typec has disconnected!`. The typec extcon stays USB=1
USB-HOST=0 and dwc3 stays in device mode. Hub USB is dead; DP link training fails with -71 after
`PHY BUSY timed out` (pin D and pin C alike).

**The debugfs workaround** (`echo host > /sys/kernel/debug/25100000.dwc3/mode` after the swap):
- USB3 hub enumerates. DP: Attention 0x9a, `Link training succeeded rate=2 lanes=2`, 1080p picture while
  charging. Pin D works; the lane layout was not the problem.
- **USB2 timing:** switched within about 1 s of the swap, USB2 fails (`device descriptor read/64 error -71`,
  `unable to enumerate`). A second device->host toggle a few seconds later brings up the USB2 hub, the
  billboard and the devices behind it, and DP retrains.
- **The debugfs panic:** a script reading the debugfs `mode` file every 0.1 s crashed the kernel:
  `SError ... pc : dwc3_mode_show+0x48 [dwc3_sprd_core] ... Comm: cat`, right after
  `dwc3_sprd_runtime_suspend` (`panic-dmesg.txt`). A register read on a runtime-suspended controller.
  **Userspace must never touch that file.**

**Other DP gaps seen:** HPD set in the Status Update reply is ignored; writing sysfs `pin_assignment` or
`configuration` re-sends Configure but the hub doesn't re-send HPD. (With the VIA hub the Status reply
carried 0x1a, HPD low, at 06:28:08.132 and the Attention 0x9a came at 06:28:09.949, `pdlog-1.txt`.)

**Never observed:** a recorded session with the Cube as source in which VBUS was driven through the PD
driver. Every working source case so far got VBUS from dwc3-sprd's regulator call (see R12).

---

## 3. Root causes

| # | Cause | Where | Status |
|---|---|---|---|
| R1 | Connector DT has no `data-role`, so the port is DFP-only and refuses DR_Swap. Source PDO 400 mA is too little for bus-powered hubs | dtbo entry 2, `/fragment@14/__overlay__/connector` (target `pmic_pd` = `/soc/aon/spi@400000/pmic@0/pd@e00`). Same values as Unisoc's reference `ums9620-2h10-overlay.dts` and entries 0, 3, 4. **Entry 1 (UMS9620 1H10) differs:** sink-pdos `<0x040190c8 0xc0dc213c>` (5 V 2 A) | CONFIRMED |
| R2 | **The data-role swap is dropped** because Unisoc's "PD hub" mode `use_pdhub_c2c` is off. Only that mode sets the typec "partner connected" flag (byte +49). `sc27xx_typec_set_pd_swap_event` (`.text+0x544`) checks it at `.text+0x594` (`ldrb w8,[x22,#49]; cbz w8,.text+0x670`, file 0x1594) and logs `typec has disconnected!`. +49 is set to 1 only at `.text+0x13dc`, on the c2c branch of the attach path (`cbnz` on byte +448 at `.text+0x11e0`); it is cleared at `.text+0xe7c` (disconnect) | `sc27xx_typec.ko` | CONFIRMED |
| R3 | The same flag gates sysfs PR/DR swaps in tcpm and the PMIC swap and try-role programming | `sprd_tcpm_pr_set` (`.text+0x4648`) and `sprd_tcpm_dr_set` (`.text+0x4384`) read port+0x1068 (`.text+0x4668` / `0x43a8`; copied from `sc27xx_pd` byte +891 at `.text+0xb18-0xb28`). `sc27xx_pd_set_swap` (`.text+0x1c64`), `sc27xx_pd_set_typec_roles` (`.text+0x1cec`) log `not use_pdhub_c2c ...` | CONFIRMED |
| R4 | None of the four c2c properties exists in the live DT or in any of the 5 dtbo entries. Kernel log: `use_pdhub_c2c = 0` | `live.dtb`, `dtbo_a.img` | CONFIRMED |
| R5 | USB2 failure after a fast debugfs switch: debugfs skips the vendor steps (switch D+/D- to the controller, wait for runtime suspend) and overlaps BC1.2 charger detection, which takes over D+/D- after the PD contract (`sprd_bc1p2_charger_detect` at 06:23:58.801 and 06:24:00.357, swap at 06:23:58.322) | `klog-1.txt` | INFERRED |
| R6 | The debugfs panic: `dwc3_mode_show+0x48` reads a register while the controller is runtime-suspended | `dwc3_sprd_core` (vendor_boot) | CONFIRMED |
| R7 | HPD in the DP Status Update reply is never forwarded. Only the ATTENTION branch of the rx handler calls `tcpc->dp_altmode_notify` (`.text+0xc618-0xc678`). ACKs leave through `.text+0xc9c8` (`b.lt .text+0xc5d0`) with no notify | `sprd_tcpm.ko` `sprd_tcpm_pd_rx_handler` | CONFIRMED |
| R8 | Any change on the sc27xx-typec extcon sends `hpd_status = 3` and tears DP down; nothing brings it back except a new Attention. sc27xx_pd registers `devm_extcon_register_notifier_all` on the typec extcon, so **every** cable change counts, including the USB=0 / USB_HOST=1 pair that c2c's own DR-swap work produces | `sc27xx_pd_extcon_event` (`.text+0x1358`) | CONFIRMED |
| R9 | With DP off, only hpd 1 powers DP up. A first Attention that also carries IRQ_HPD (0x18a, hpd 2) is ignored | `sprd-drm.ko` `sprd_dp_pd_event` (`.text+0x8964`), compare at `.text+0x89b4` | CONFIRMED |
| R10 | Pin selection: a partner offering C+E or D+F gets a Configure with two pin bits (invalid). A hub offering C+D without "multi-function preferred" gets pin C and loses USB3 for nothing (the Cube does 2 lanes either way) | `sprd_typec_displayport.ko` `sprd_dp_altmode_status_update` `.text+0xcb0-0xce4` | CONFIRMED (code); effect on real docks INFERRED |
| R11 | Charging: the AW32257 buck charger has no power path, no input-current-limit register, 4-6 V input (OVP 6.4 V), 2.48 A max. Charge current comes only from the BC1.2 type: DCP gets 2.0 A, UNKNOWN (the test hub, likely pure-PD chargers) gets 1.05 A (chip: 992 mA). The "15 W" values in the DT are Unisoc reference-board leftovers (bq2560x + bq2597x charge pump, still present in entry 0) | `aw32257_charger.ko`, `sprd-charger-manager.ko`, dtbo entry 2 battery node `/fragment/__overlay__/battery` (the fragment literally named `fragment`, `target-path = "/"`), AW32257 datasheet V1.5 (`work/charging-15w/ds/aw32257.txt`) | CONFIRMED |
| R12 | **The PD driver has no real VBUS supply.** `pd@e00` has no `vbus-supply`; `sc27xx_pd` probe gets "vbus" with `devm_regulator_get` (`.text+0x998`) and receives the dummy regulator (klog: `sc27xx-pd supply vbus not found, using dummy regulator`; panic-dmesg: `regulator-dummy: Underflow ... cannot disable vbus regulator`). So tcpm's `set_vbus` (`sc27xx_pd_set_vbus`, `.text+0x2004`) switches nothing today. The only real VBUS source is the AW32257 boost `vddvbus` (`/soc/ap-ahb/i2c@22a0000/charger@6a/otg-vbus`, live phandle 0x223), reached only through the `vbus-supply` of the `usb` and `usb3` nodes, by `dwc3_sprd_otg_start_host` (`.text+0x29a0`) and musb `sprd_musb_work`. **c2c mode skips exactly those two calls** (dwc3 `.text+0x2a28`/`0x2ab4`, musb `.text+0x1d78`/`0x1f3c`) | `sc27xx_pd.ko`, `dwc3-sprd.ko`, `musb_sprd.ko`, `live.dtb`, klog | CONFIRMED (code, DT, logs); effect with c2c on INFERRED |

**Why c2c mode is the intended path, not a hack (CONFIRMED in the binaries):**
- With the flag on, swap flag 4 queues `sc27xx_typec_extcon_drswap_work` (`.text+0x1704-0x1804`, delayed work
  at +0x138, queued with a 2-jiffy delay at `.text+0x60c`): sets `dr_swap_executing`, extcon USB=0, then
  USB_HOST=1 (only while `.bss+4` "attached" is set; the c2c attach path sets it at `.text+0x1270`/`0x1364`),
  clears executing.
- `dwc3-sprd` registers its id notifier on the typec extcon's USB_HOST cable (extcon phandle index 1, probe
  `.text+0xd28`); its vbus notifier is on index 0 (`extcon_gpio`) for USB. `dwc3_sprd_id_notifier`
  (`.text+0x23a4`) queues `dwc3_sprd_hotplug_sm_work` (`.text+0x199c`).
- The state machine goes PERIPHERAL -> IDLE, waits for the dwc3 core to be runtime-suspended
  (`waiting dwc3 suspended`, requeued every 25 jiffies), resumes it, switches D+/D- to the controller
  (`call_sprd_usbphy_event_notifiers(0,1,NULL)` at `.text+0x1bdc`, c2c only), then starts host.
- In c2c mode `dwc3_sprd_otg_start_host` (the c2c test at `.text+0x2ab4`) skips the `vbus` regulator (OTG
  boost). Without c2c it would turn the boost on while the Cube is a sink fed by the hub. So c2c is the
  mode made for "host while sink", **but only together with a VBUS supply on the PD node** (R12): in c2c
  mode the PD driver is the only thing left that can power VBUS when the Cube is the source.
- Production use: six Nubia UMS9620 device overlays in a public 5.4.254 tree set `use_pdhub_c2c` on `usb`,
  `usb3` and `pmic_typec` (local copies: `work/t820-portability/src/nubia/ov-{caltrop,camellia,durian,
  harrow,papaya,pine}.dts`). None of them sets `use-pdhub-c2c` or `vbus-supply` on `pmic_pd`; their base
  dtsi is UNKNOWN. Unisoc's reference `ums9620-2h10-overlay.dts` (Nubia and ZTE copies) does **not** set c2c.
  The c2c overlays also rewire extcons (M1 in 4a). A web search found no other public `use_pdhub_c2c` hit.

This also explains R5: the vendor path does exactly the two steps the debugfs write skipped. Whether
BC1.2 detection still overlaps host start in c2c mode is UNKNOWN; test it.

---

## 4. The fix design

Order of preference: **device tree first** (portable, no binary patch), binary patches only where the DT
can't reach.

### 4a. Device tree

All edits are to **dtbo entry 2 only** (see decision D1). Values are big-endian cells as in the dts.

**Connector** (`/fragment@14/__overlay__/connector`, compatible `usb-c-connector`):

| Property | Original | New | Why |
|---|---|---|---|
| `data-role` | absent | `"dual"` | lets the Cube accept DR_Swap (R1). Live-tested |
| `sink-pdos` | `<0x0401912c 0xc0dc213c>` | `<0x2601912c>` (D2, recommended) or `<0x2601912c 0xc0dc213c>` | DRP+DRD bits on 5 V 3 A, so a dock sees a dual-role partner. Live-tested. Dropping the PPS APDO (3.3-11 V) is recommended: the selector ignored it with the hub (5 V requested against 9 V/12 V offers), but c2c changes the PD paths and the APDO is useless with a 4-6 V charger |
| `source-pdos` | `<0x04019028>` | `<0x0401905a>` (v3) | 900 mA powers the VIA hub; no DRP/DRD bits. Live-tested with the hub and the HDMI dongle |
| `op-sink-microwatt` | 10 W | unchanged | the mismatch flag it causes is harmless |

**Why v3 and not v1/v2:**
- 400 mA (original, v1) left the hub without enough power: HPD never rose.
- v2 (DRP|DRD at 900 mA) broke negotiation with the hub (hard-reset loop). Which bit caused it is UNKNOWN.
- DRD in a source PDO gains nothing for docks: the Cube is already DFP when it sources.
- DRP only matters for "hub first, charger later" (matrix row 4), and the PR_Swap it invites probably
  fails today (R3). Retest DRP only after phase 1 works (isolation run in phase 6).
- **Never above 1000 mA:** the AW32257's OTG boost is rated 5.05 V 1 A (datasheet, CONFIRMED).

**c2c mode** (the fix for R2-R4) **plus the VBUS supply** (the fix for R12). Four boolean properties in two
spellings, and one phandle:

| Node (live path) | Overlay location in entry 2 | Property | Stage |
|---|---|---|---|
| `/soc/aon/spi@400000/pmic@0/typec@380` | **no fragment today.** Add `fragment@N` with `target = <0xffffffff>` and `__overlay__ { use_pdhub_c2c; }`, and append `/fragment@N:target:0` to `__fixups__/pmic_typec` | `use_pdhub_c2c` | core |
| `/soc/ipa-apb/usb3@25100000` (dwc3-sprd glue, `extcon = <0x116 0x64>`) | `/fragment@8/__overlay__` (target `usb3`) | `use_pdhub_c2c` | core |
| `/soc/aon/usb@a00000` (musb) | `/fragment@6/__overlay__` (target `usb`) | `use_pdhub_c2c` | core |
| `/soc/aon/spi@400000/pmic@0/pd@e00` | `/fragment@14/__overlay__` (target `pmic_pd`) | `vbus-supply = <0x07>` | core |
| `/soc/aon/spi@400000/pmic@0/pd@e00` | `/fragment@14/__overlay__` | `use-pdhub-c2c` (hyphens) | second step |

**How `vbus-supply` is encoded** (CONFIRMED in entry 2): `vddvbus` is a node **inside entry 2 itself**
(`__symbols__/vddvbus = "/fragment@4/__overlay__/charger@6a/otg-vbus"`, local phandle 0x07), already used the
same way by `fragment@6` and `fragment@8`. So the edit is `vbus-supply = <0x07>` in `/fragment@14/__overlay__`
plus `__local_fixups__/fragment@14/__overlay__ { vbus-supply = <0x00>; }`. **Not** a `__fixups__/vddvbus`
entry: the base DT in vendor_boot has no `vddvbus` symbol (the node only exists once entry 2 is applied), so
an unresolvable fixup could make the overlay fail to apply. The patcher must find the phandle from
`__symbols__/vddvbus`, not hard-code 0x07.

**The core set is all or nothing (hardware rule).**
- If `typec@380` has c2c but `usb3` doesn't, the DR-swap work raises USB_HOST=1 and non-c2c dwc3 turns the
  AW32257 boost on while the hub is driving VBUS: back-driving the hub (INFERRED).
- If `vbus-supply` is added while dwc3/musb are not c2c, two consumers toggle the same regulator.
  `sc27xx_pd_set_vbus` decides by `regulator_is_enabled()` (hardware state, `.text+0x20ac`), so it can
  disable a boost that dwc3 enabled: unbalanced disables (INFERRED).
- If c2c is on without `vbus-supply`, nothing powers VBUS when the Cube is the source (R12): HDMI dongles
  (matrix row 1, works today), bus-powered hubs (row 3), OTG adapters and the "charger pulled, Cube
  re-attaches as source" case (row 5) lose power, and tcpm may loop on source-on timeouts (INFERRED).
- `patch_dtbo.py` refuses any partial core set. The P-C2C module fallback (4b) is likewise one atomic step.

**Staging** (phase 1, decision D10):
- **1b, core:** `typec@380`, `usb3`, `usb` c2c plus `vbus-supply` on `pd@e00`. This matches Unisoc's own
  production c2c overlays (Nubia), plus the supply. The dock-initiated DR_Swap already works without the pd
  flag (live-tested with v3), and the drswap work only needs the typec flag.
- **1c, add `use-pdhub-c2c` on `pd@e00`:** opens sysfs PR/DR swaps (R3; needed for F4 and Nano-initiated
  swaps), programs Try.SRC/Try.SNK and swap registers in the PMIC, sets `can_power_data_role_swap`, and (in
  the public source, `nubia/sc27xx_pd.c:1460`) switches the PD clock from `D13M_A6P5M` to `D6M_A6M`. Only
  when 1b is clean.
- **1d, variant:** Unisoc's c2c extcon rewiring, only if swaps or BC1.2 misbehave in 1b/1c. In the Nubia
  sources, non-c2c `ov-camelia.dts` and c2c `ov-camellia.dts` differ in their USB nodes only by: `hsphy`
  and `ssphy` extcon `extcon_gpio` -> `pmic_typec`; `usb`/`usb3` extcon `<extcon_gpio, pmic_typec>` ->
  `<pmic_typec, pmic_typec>`; `pmic_fgu` gains `use-typec-extcon` and `extcon = <pmic_typec>`; plus the three
  c2c flags. The Cube keeps `extcon_gpio` as dwc3's VBUS extcon, so dwc3's c2c filters (`dr_swap_executing`,
  `chg_state = DETECTED` to skip BC1.2 on a swap) key off a cable dwc3 never sees, and R5 may persist
  (INFERRED).

Readers (CONFIRMED): `sc27xx_typec` probe `of_find_property` `.text+0x7c4`, `cset w9` `.text+0x7d0` (stores
byte +448); `dwc3-sprd` probe `.text+0xc58`/`0xc64` (+262); `musb_sprd` probe `.text+0x758`/`0x76c` (+441);
`sc27xx_pd` probe `device_property_present("use-pdhub-c2c")` `.text+0x510`, `and w8,w0,#1` `.text+0x518`
(+891, also logs `use_pdhub_c2c = %d`).

**Side effects of c2c mode** (CONFIRMED in code; effect on the device UNKNOWN until phase 1):
- sc27xx_typec stops registering its own typec port (`.text+0x7f0`). Only tcpm's port remains, so the
  port number under `/sys/class/typec` can change. NanoTypec's `pdPort()` picks the port by PD revision
  (CONFIRMED in its tests), so it copes. The README's hard-coded `port1` was the one thing that didn't; test 40
  fixed it (4e.7: README External monitors now describes the port found by what it can do).
- sc27xx_pd (stage 1c) starts PR-swap and Try.SRC/Try.SNK register writes, takes a different plug-in path in
  `sc27xx_pd_check_vbus_cc_status` (`.text+0x2f10`), and switches the PD clock (public source; INFERRED for the
  Cube's binary). **Try.SNK becomes active**: DRP partners (phones, other handhelds, a laptop's DRP port, PD
  power banks) may settle in different roles than today.
- Sysfs PR/DR swaps start working (R3, stage 1c).
- **VBUS when the Cube is source:** dwc3 and musb no longer touch the boost; tcpm's `set_vbus` drives it
  through `vbus-supply` (R12). With the supply, tcpm calls `set_vbus` on any Rd attach, so non-PD OTG
  adapters should keep working (INFERRED). Without it they lose VBUS (likely).
- **Probe order:** `sc27xx_pd` uses `devm_regulator_get` (not optional), so with the supply it waits
  (deferred probe) until `aw32257_charger` has registered `vddvbus`. Both are in `modules.load` (lines 69 and
  115) and in recovery's `modules.load.recovery` (lines 119 and 155). No dependency cycle was found
  (charger@6a uses `extcon_gpio` and `hsphy`, not the PD node). INFERRED safe; check boot and recovery logs.
- **DR swap and DP (R8):** the drswap work's USB=0 and USB_HOST=1 changes each send `hpd 3`. In the normal
  flow this runs about 2 jiffies after the swap, before DP mode entry (Configure ACK 19 ms after the swap),
  so it is harmless. Any DR swap **after** DP is up (Nano F4, sysfs `data_role`) turns DP off (INFERRED).
- **charger-manager also watches the typec extcon** (DT: charger-manager `extcon = <pmic_typec pmic_pd>`).
  `sprd_vchg_typec_extcon_work` (`sprd-charger-manager.ko` `.text+0x10420`) reads cable USB(1); when it is 0
  it clears state (+452, +450) and queues work. In c2c the Cube sits at USB=0, USB_HOST=1 while sinking.
  Whether charging or the PD charge vote is affected is UNKNOWN. Phase 1 logs it.

**Not in the DT:** no `usb-role-switch` link from the connector to dwc3. `sprd_tcpm_set_roles`
(`.text+0xb10c`; the role-switch load is at `.text+0xb1c0`) skips its role switch when the pointer is NULL,
and dwc3-sprd already owns the role. Do not add `sprd,support-fchg`, `sprd,support-pd-pps`,
`sprd,support-sfcp`, `cm-charge-pumps`, or 9/12 V sink PDOs (4d).

**Optional DT cleanups:** the charging change in 4d (unknown-jeita table), only under D5/D11.

### 4b. Driver patches

Each patch: a table entry in the style of `scripts/sprd_drm.py` (`PATCHES`, `patch_module()`), checking the
module sha256 and every original word, refusing anything else. A generic patcher is in section 5.

**P-C2C: not a complete fallback.** Forces the flag in probe, one word per module. It **cannot** add the
VBUS supply (R12), so on its own it breaks the Cube-as-source cases. It is useful only if the bootloader
accepts the `vbus-supply` edit but some c2c property can't be read, and it must patch the core modules as
one atomic step (typec, dwc3, musb; sc27xx_pd only for stage 1c). Words and offsets CONFIRMED by reading the
files.

| Module | `.text` | File | Original | New |
|---|---|---|---|---|
| `sc27xx_typec.ko` | 0x7d0 | 0x17d0 | 0x1a9f07e9 `cset w9, ne` | 0x52800029 `mov w9, #1` |
| `dwc3-sprd.ko` | 0xc64 | 0x2c64 | 0x1a9f07e8 `cset w8, ne` | 0x52800028 `mov w8, #1` |
| `musb_sprd.ko` | 0x76c | 0x276c | 0x1a9f07ea `cset w10, ne` | 0x5280002a `mov w10, #1` |
| `sc27xx_pd.ko` (1c only) | 0x518 | 0x2518 | 0x12000008 `and w8, w0, #1` | 0x52800028 `mov w8, #1` |

- The musb site: `of_find_property(np, "use_pdhub_c2c")` at `.text+0x758` (string `.rodata+0x1e5`), `cset w10`
  at 0x76c, `strb w10,[x19,#441]` at 0x794. The `cset` at 0x740 belongs to `wakeup-source`; don't touch it.
- No relocation targets these words (checked in the disassembly).
- All four load from vendor_dlkm (`modules.load` lines 55, 56, 68, 69). Recovery loads copies from vendor_boot
  (`modules.load.recovery`), byte-identical. musb_sprd is not in recovery's list.

**DP-B: first HPD that also carries IRQ_HPD** (R9). Recommended. `sprd-drm.ko`, append to
`sprd_drm.py` `PATCHES`:
- `(0x89b4, [0x7100051f, 0x54000b21], [0x4a480508, 0x36000b28])`: `eor w8,w8,w8,lsr #1; tbz w8,#0,.text+0x8b1c`.
  hpd 1 or 2 powers DP up from off; 0 and 3 are ignored. w8 is dead afterwards (reloaded at 0x89c8; 0x8b1c
  reloads w2).
- Doesn't overlap sprd_drm.py's sites (0x8cdc-0x8e50, 0x90f4). Bytes are the same in the original and the
  patched module. Built and checked statically only.

**DP-D: HPD high while DP is "on" retrains** (prerequisite for DP-A, see below). `sprd-drm.ko`:
- `.text+0x8a58` (file 0x38a58): 0x35000628 (`cbnz w8,.text+0x8b1c`, ignore) -> 0x35000468
  (`cbnz w8,.text+0x8ae4`, the hpd 2 / IRQ_HPD path, which calls the same op `[ops+40]` with 2).
- Why: `sprd_dp_pd_event` sets the "on" byte `[x20,#3001]` at `.text+0x8a40` after an hpd 1 **whatever the
  training result**, and clears it only for hpd 0 or 3 (`.text+0x8adc`). A failed training therefore leaves
  DP "on", and every later hpd 1 is dropped at 0x8a58: the screen stays black until an unplug.
- Whether the IRQ_HPD op retrains a link that never trained is UNKNOWN. Checked statically only.

**DP-A: HPD from the Status Update / Configure ACK** (R7). Only after phase 1, and **never alone**. The VIA
hub sends an Attention 2-3 s after Configure anyway (CONFIRMED: `hpd_status = 1` at 06:24:00.154, swap at
06:23:58.32; its Status reply carried HPD low). It matters for docks that report HPD only in the Status reply,
and it gives Nano a retry lever (4e). `sprd_tcpm.ko`:
1. `.text+0xc9c8`: 0x54ffe04b (`b.lt .text+0xc5d0`) -> 0x54fc1b0b (`b.lt .text+0x4d28`).
2. Stub `sprd_tcpm_debug_show` (debugfs only) at `.text+0x4ac4`: `mov w0,wzr; ret` (0x2a1f03e0, 0xd65f03c0).
3. Hook in relocation-free gaps of that function: `.text+0x4d28` (13 words), `0x4df8` (10), `0x4e8c` (9).
   It requires an ACK of DP Configure (`(p0 & 0xff) == 0x51`, `p0>>16 == 0xff01`), finds the 0xff01 partner
   altmode (`port+0xa58`, svid at `alt+936`), reads `dp->data.status` and `conf` (`[alt+152]`, +0, +4), and if
   HPD is set calls `tcpc->dp_altmode_notify(tcpc, status & ~0x100)` (`[port+136]`, then `+144`), then
   branches to `.text+0xc5d0`.
- **The main-case hazard (safety review):** the hook fires about 20 ms after DR_Swap. Host mode starts
  asynchronously (the hotplug state machine waits for runtime suspend and requeues every 25 jiffies), so the
  training it triggers likely runs before the combo PHY is in host configuration and fails with -71
  (INFERRED). Because of the stuck "on" byte (DP-D above), the dock's later Attention is then ignored, where
  without DP-A it would have trained the link about 2 s later. The Nano retry (4e.5) has the same flaw.
- **Required before shipping DP-A, one of:** (a) the hook sends an HPD-low notify (`status & ~0x180`; CONFIRMED
  that `sc27xx_pd_dp_altmode_notify` maps no-HPD to hpd 0 at `.text+0x25e8-0x25fc`, and hpd 0 while "on" takes
  the off path at `.text+0x8a5c`: DP off, "on" cleared) before the HPD-high one, so every hook call is a full power-up and training;
  needs re-fitting into the gaps (UNKNOWN whether it fits); or (b) ship DP-D with it. (a) is preferred,
  because it doesn't depend on what IRQ_HPD does with an untrained link. Phase-3 gate test: "monitor
  connected before docking, c2c".
- **CFI:** the hook adds no new indirect-call target, but it calls `blr x14` **without** the
  `__cfi_slowpath` check the ATTENTION path does. Acceptable (same target, same function pointer), but it
  removes a check rather than keeping it.
- The hook reads `dp->data` without `dp->lock` (benign: two words written by the same rx path).
- It fires for **every** Configure ACK whose saved status has HPD, including today's working source-mode
  dongles. That moves their training earlier than today; phase 3 must regression-test row 1.
- w24 holds p0 on the Configure-ACK path (0xc160 -> 0xc478 -> 0xc9a0 -> 0xc9c4); it is clobbered on other paths
  (0xc6f4, 0xc904/0xc92c), which the svid check makes safe in practice. `rx_done` (0xc5d0) uses only x19/x20.
- Words: `out/.dock/work/dp-driver/patch/tcpm_hook.S`, `apply.py`. Built and checked statically only.
  The final patcher must also check the original words inside the cave, not only the entry points.

**DP-C: pin selection** (R10). Optional. `sprd_typec_displayport.ko`:
- `.text+0xcc0`: 0x121c0108 -> 0x52800208 (`mov w8,#0x10`): always prefer multi-function pins (D).
- `.text+0xcd8..0xce4`: `[0x72001d1f, 0x54000120, 0x12001d08, 0x33181d0a]` -> `[0x4b0803e9, 0x6a090108,
  0x54000100, 0x33181d0a]`: configure the single lowest offered pin.
- Nit to fix before shipping: "lowest offered pin" picks A or B over C-F if a legacy adapter offers them.
  Mask with 0x3c first when any of C, D, E, F is present (needs re-fitting; UNKNOWN whether it fits).

**Not patched:** `sprd_tcpm` swap gates (c2c opens them), `dwc3_sprd_core` debugfs (just never read it),
the PHY BUSY waits (non-fatal, about 0.5 s each; shorten only after measuring), the PTN38003A calls (they
return -ENODEV and the result is ignored).

### 4c. DisplayPort gaps

| Gap | Effect | Plan |
|---|---|---|
| Host mode must be up before training. DP probably needs `sprd_ssphy_init` (USB3/DP combo PHY mux, host side; INFERRED). Configure ACK comes about 20 ms after DR_Swap; the dwc3 state machine is asynchronous | if HPD arrives first, training fails, DP stays "on", and later hpd 1 is ignored (DP-D) | Phase 1 measures it. With the VIA hub the Attention comes about 2 s later, probably in time (INFERRED). DP-A must not make this worse (see 4b) |
| R7 HPD in Status reply | docks that see the monitor at mode entry stay dark | DP-A with (a) or DP-D |
| R8 hpd=3 teardown on any extcon change | a role re-toggle, a DR swap after DP is up (F4, sysfs), a PR swap or pulling the dock's charger turns DP off for good | short term: Nano re-sends Configure (needs DP-A). Long term: a source-level change sending hpd 3 only on a real detach; as a binary patch it needs more work (UNKNOWN effort). Keep F4 away from ports where DP is up |
| R9 first Attention 0x18a | some dongles never light up | DP-B |
| R10 pin selection | USB3 lost on C+D hubs without the MF bit; invalid Configure on C+E | DP-C, optional |
| Repeated HPD-high while on does nothing (`.text+0x8a58`) | no recovery from a failed or dropped link | DP-D, required if DP-A ships without variant (a) |
| No VCONN supply (`sc27xx-pd: vconn NULL!!!`) | VCONN-powered adapters and active cables fail when the Cube is source | hardware; document |
| 2 lanes, HBR2 measured (`rate=2 lanes=2`) | about 1080p60 / 1440p60 / 4K30 | hardware or fixed cap in sprd-drm (`dptx_init` `strh 0x203`); document |
| Flipped plug (CC2) never tested | USB3+DP mux orientation without the redriver unverified | test in phase 6 |

### 4d. Charging toward 15 W

**Facts** (CONFIRMED unless marked):
- AW32257 (datasheet V1.5, `work/charging-15w/ds/aw32257.txt`): 4-6 V input, OVP 6.4 V, charge current
  496-2480 mA (REG04), no input-current limit (REG01 bits 7:6 NA), input-voltage DPM only (driver sets VSP
  4.25 V), REG06 safety limit (default 1116 mA), OTG boost 5.05 V 1 A. No power path: the "charge current"
  also feeds the system.
- Anbernic's driver caps the request at 2.3 A (`aw32257_set_charge_current` `.text+0x2c30`), giving chip code
  13 (2232 mA) at most (the threshold table at `.text+0x2cc8` is consistent with it; the exact code is
  INFERRED).
- charger-manager picks current from the BC1.2 type only (jeita tables in the battery node,
  `/fragment/__overlay__/battery` in entry 2): DCP 2.0 A (0x1e8480), UNKNOWN 1.05 A (0x100590), CDP 1.5 A
  (0x16e360), SDP 0.5 A (0x7a120) in the 15-52 °C band.
- sprd_tcpm already requests 5 V 3 A from a 5 V 3 A source. The test hub gets 5 V 1 A because that is its 5 V
  offer.
- `constant_charge_current` written from userspace is a MIN vote and returns `-ENODEV` while no external
  power is online (public Moto source `work/charging-15w/src/moto_charger-manager.c:5015-5024`; that the
  Cube's binary behaves the same is INFERRED).
- Maximum input about 10-11 W (INFERRED from chip limits). 15 W is not reachable.

**Hard rules:** no fchg/PPS/SFCP flags, no charge pump, no 9 V or 12 V sink PDO. `sprd,support-fchg` would
make `sprd_fchg_extcon` ask for 9 V, above the AW32257's 6.4 V OVP. A unit test checks the patched DT has no
such property and no sink PDO or APDO whose maximum voltage exceeds 5 V.

**Step 1, measure first** (phase 5): a DCP charger, screen off. Expect `chgr type 2`,
`aw32257_set_charge_current uA:2000000`, ibat about 1.8-1.9 A. Read the AW32257 register dump (expected at
`/sys/class/power_supply/aw32257/registers`, INFERRED path). If current stops near 1.1 A, the REG06 safety
limit is active, and probe would need to write REG06 (UNKNOWN whether Anbernic's undocumented
register-0x38 write already handles it).

**Step 2, UNKNOWN-type sources up to 2.0 A, bounded by the contract.** Default: **do not ship** (stay at
1.05 A for UNKNOWN) unless D5 and D11 say otherwise. If adopted, both parts together, never part A alone:
- **A (DT):** in `unknown-jeita-temp-table`, change the 15-52 °C current from 0x100590 (1.05 A) to 0x1e8480
  (2.0 A). Full table: `<0x3e8 0x406 0x00 0x423920 0x47e 0x49c 0xaae60 0x423920 0x5f0 0x5d2 0x1e8480 0x423920
  0x640 0x622 0xaae60 0x3e8fa0>`.
- **B (Nano, minimum vote), UNKNOWN-type sources only:**
  - Act only when the BC1.2 type is UNKNOWN (read from the charger's power_supply `usb_type`; path
    INFERRED). For DCP, CDP and SDP write a value that doesn't bind (at or above the DT maximum, e.g.
    2000000), so ordinary chargers keep today's current. No blanket boot vote.
  - On every attach and Type-C/PD change while power is online (writes fail with `-ENODEV` otherwise),
    compute I_contract: PD `current_max` at 5 V from `/sys/class/power_supply/sprd-tcpm-source-psy-sc27xx-pd/`,
    else Rp from `power_operation_mode` (3.0 / 1.5 A), else 0.5 A.
  - Budget = min(2000000, 0.85 x 5.0 V x I_contract / 4.4 V) µA. 1 A -> about 0.96 A; 3 A -> 2.0 A.
- Why B is needed: the AW32257 can't limit input current, so a 2 A table would pull about 1.9 A from a 1 A
  contract (INFERRED).
- **Part A fails unsafe whenever Nano isn't running:** off-mode charging (Q5), recovery (same dtbo), early
  boot, a Nano crash. In that state a 1 A source sees about 1.9 A drawn. The VSP 4.25 V input-voltage limit
  means the likely result is the source sagging, PD hard resets or a hub brownout, not damage to the Cube
  (INFERRED). Q5 blocks shipping part A; D11 decides whether these windows are acceptable or whether a
  kernel-side limit is needed first.

**Step 3, optional:** 2.2 A (0x2191c0, chip 2108 mA) for DCP/UNKNOWN, only after measuring temperatures.

**Expected result:** a 5 V 3 A charger or dock gets about 9-10 W in (with step 2); the test hub stays at
about 5 W (its 5 V offer); DCP bricks stay at today's 2.0 A.

### 4e. Nano / NanoTypec

In `patches/thimsy/0001-thimsy.patch` (NanoTypec.cpp/.h, NanoMenu `typecTick()`):
1. **Read the data role.** Add `std::string dataRole` to `View`, filled from `<port>/data_role`.
2. **Leave docks alone.** In `step()`'s empty-state branch, before the flip test:
   `if (v.partner && v.powerRole == "sink" && (v.pd == "yes" || v.dataRole == "host")) -> tried, no write`.
   Today's flip condition already needs `pd == "no"`, so the VIA hub isn't flipped (CONFIRMED); this makes it
   explicit and covers a host-while-sink without PD.
3. **`flip:` branch:** if the partner comes back as sink+host, write `dual`, go to `tried`, log
   "a dock powers the Cube".
4. **Never touch `/sys/kernel/debug/*dwc3/mode`.** Add a unit test that greps for it.
5. **DP retry** (needs DP-A with variant (a), or DP-A plus DP-D): when the port is sink+host, the partner has
   an active 0xff01 altmode, and `/sys/class/drm/*DP*/status` is still `disconnected` 3 s after the role
   changed or after an `hpd 3` teardown with the partner still present, write the current value back to
   `.../displayport/configuration` once. With DP-A this re-asserts HPD from the saved status. At most 2
   tries per attach.
6. **Charge budget writer** (4d step 2 B), only if D5/D11 adopt it; UNKNOWN-type sources only.
7. **Port lookup:** keep scanning `/sys/class/typec`; c2c mode removes sc27xx_typec's port, so don't assume
   `port1` anywhere (README, tests). Done in test 40 (§12), ahead of the DT: NanoTypec finds the port by
   what it can do, and the README no longer names `port1`.
8. **Docs:** README "External Display": plug the dock's charger in **before** the Cube. Charger-later
   doesn't make the Cube charge (no DRP in the source PDO).
9. **Not now:** F4 from the dock research (Nano asks for host by DR_Swap when a PD partner leaves the Cube as
   device for 3 s with no USB gadget configured). Useful for docks that wait for the device to ask; needs
   stage 1c (sysfs dr_set). Must not fire once DP is up (R8). Decision D6.

`nano-typec-guard.sh`: unchanged (it only writes `dual`).

---

## 5. Shipping it

### Build (repo thimsy-os-nano)
- **`scripts/avb_footer.py`** (new): move `footer_info()` out of `scripts/patch_vendor_boot.py`; also return
  "Image size" and "Original image size". Add `add_footer(...)` (the `avbtool add_hash_footer` call
  vendor_boot already uses).
- **`scripts/patch_dtbo.py`** (new, from the prototype `out/.dock/work/build-integration/dtbo_usbc.py`):
  1. Check input sha256 `ca5a55ff...` (PAC `DTBO` = `DTBO_B` = device `dtbo_a.img`), as a per-device table.
  2. Parse the dt_table (5 entries) and the FDT of the `dtbo_idx` entry (2).
  3. Find nodes by meaning: `__fixups__/pmic_pd` -> fragment -> `connector` with `usb-c-connector`;
     `__fixups__/usb`, `usb3` -> fragments; `pmic_typec` -> new fragment; `__symbols__/vddvbus` -> the local
     phandle for `vbus-supply` (plus its `__local_fixups__` entry); the battery node via `target-path = "/"`
     -> `__overlay__/battery` (the fragment named `fragment`, not `fragment@0`). Never by fragment number alone.
  4. Each edit is (property, expected old value, new value), **per entry** if D1 picks more than entry 2
     (entry 1's sink-pdos differ). Old value wrong: refuse. Already new: no-op. Refuse a partial c2c core set.
  5. Rebuild the FDT and the dt_table; other entries byte-identical. The dt_table header is **48 bytes
     short**: `total_size` = 538488, but the entries end at 538536, which equals the AVB "Original image
     size". Write `total_size = end_of_entries - 48`, exactly as the original does (the prototype already
     does this).
  6. Self-check: re-sign the unpatched raw and require it byte-identical to the input (CONFIRMED to hold for
     the Cube); then sign the patched raw with the input's salt, prop
     `com.android.build.dtbo.fingerprint`, SHA256_RSA4096, partition size 8388608, `AVB_KEY`.
  7. Offline merge check: `fdtoverlay` the patched entry onto the base DT from vendor_boot's dtb section,
     decompile, diff against `live.dtb`: only the intended properties may differ (plus the pre-existing v3
     differences of the test Cube).
- **`scripts/build.sh`**, after line 266 (`rm -f "$vendor_boot" ...`): extract `DTBO`, run `patch_dtbo.py`,
  `pactool.py replace` into `DTBO` and `DTBO_B`. Output stays 8 MiB, so `replace` accepts it. Gate behind a
  `DOCK_FIX` switch so the DT never ships without what phase 1 proves is needed.
- **Module patches** (only if used): a generic `scripts/kmod.py` (ELF64 ET_REL; site = symbol + offset or
  unique pattern; refuse on wrong words, relocations in the site, ambiguity) and `scripts/vendor_modules.py`
  (registry: module -> patch, known sha256, `where={vendor_dlkm, vendor_boot}`). Port `sprd_drm.py` onto it.
  `patch_vendor_dlkm.py` builds its `rebuild()` map from the registry (today
  `{MODULE: patch_module, MODULE_LIST: ...}`); `patch_vendor_boot.py` splices recovery copies the same way.
- **Tests** in `tests/unit/`: `test_dtbo.py` (FDT fixture, values, refusals including a partial c2c set and
  entry 1's different sink-pdos, the 48-byte header rule, `vbus-supply` with its local fixup, no >5 V PDO or
  APDO, footer via the avbtool shim), `test_kmod.py`, the registry run through `test_vendor_patches.py`,
  NanoTypec cases.

### Installer (repo thimsy-os, branch universal-installer)
- `installer/catalog/devices/anbernic-rg-cube.yaml`:
  - `flash.update`: add `DTBO, DTBO_B`.
  - `update_check`: remove `DTBO, DTBO_B` (`spd_plan` bails on an entry that is both written and checked,
    `flash.rs:271`).
  - `writes_update`: add `dtbo_a, dtbo_b`. `writes_full` already has them.
- `flash.rs`: no code change (`spd_plan()`, `check_base()` are generic).
- Packages: no Op change. `dev pack` (`pack.rs`) diffs every changed PAC entry generically, so a patched DTBO
  automatically produces `PacDelta` ops for `DTBO` and `DTBO_B`; since their contents are identical, both
  share one bsdiff artifact (`by_hash`). Add a package test that expects the two new `PacDelta` ops.
- `package.rs`: bump `MIN_INSTALLER` (line 32, now `0.1.0-test18`). An old installer reads back DTBO,
  finds a mismatch and asks for a Full install.
- `installer/tests/e2e.sh`: 7 partitions (today 5), new write list, use `dtb_b` as the read-back mismatch
  partition (today `dtbo_b`). Update `docs/engine-protocol.md` and `protocol-samples/resolve-rg-cube-heavy.jsonl`.
- Catalog check in `catalog.rs`: reject `update` and `update_check` overlap.
- Note: the test Cube, with its hand-edited dtbo, fails today's Update check (DTBO differs from the PAC), so
  today any installer run on it means a Full install, which erases userdata and metadata (`erase_full`).
  Once DTBO is in `update`, it is simply overwritten.

### Signing
- dtbo is chained in vbmeta (key sha1 3b0792d6..., rollback location 6), but the shipped dtbo is already signed
  with the AOSP test key (2597c218..., SHA256_RSA4096). The Cube already boots orange (unlocked). Re-signing
  with the test key changes nothing. vbmeta is not touched. CONFIRMED.
- Thimsy requires an unlocked bootloader already. A locked device would not boot any of this.

### Update and Full paths, rollback
- **Update:** writes logo, fbootlogo, vendor_boot_a/b, dtbo_a/b, super. Keeps data. It writes **both** dtbo
  slots, so after an Update slot B is no longer an escape.
- **Full install:** writes everything in `writes_full` through Unisoc download mode (BROM/FDL) and erases
  userdata and metadata. This is the universal recovery path: the BROM is in ROM, so a bad dtbo, vendor_boot or
  super can always be overwritten, at the cost of the user's data.
- **Rollback:** keep the previous Thimsy package downloadable and flash it (Update), or a Full install of the
  previous Thimsy or stock GammaOS PAC. Keep `out/.dock/dtbo_a.img`, `vbmeta_a.img` as the original backups
  (`dtb_a.img` is all zeros and is not a usable backup).
- **What is not a fallback** (safety review): recovery and fastbootd use the same dtbo and byte-identical
  `sc27xx_typec`/`sc27xx_pd`/`dwc3-sprd`/`sprd_tcpm` from vendor_boot, so a USB-C breakage shows there too.
  Slot B is not automatic: switching needs fastboot `--set-active` (Q6) or download mode, and whether the
  `*_b` logical partitions exist in `super` is UNKNOWN (check with `lpdump`).

---

## 6. Other docks: matrix and expected behaviour

From `out/.dock/work/dock-matrix/`. "Seen" = observed; everything else INFERRED. Column (c) assumes the core
c2c set **with** `vbus-supply`; without the supply, rows 1, 3, 5 (charger pulled) and OTG adapters lose VBUS.

| # | Kind | (a) original | (b) DT v3 now | (c) after this plan |
|---|---|---|---|---|
| 1 | Bus-powered HDMI/DP dongle (pin C) | works | works (seen) | works only with `vbus-supply` (VBUS now from tcpm); DP-A also covers dongles that report HPD only in Status; DP-B covers first Attention 0x18a. Regression-test after DP-A (earlier training) |
| 2 | USB-C to DP / VGA cable | expected to work | same | same; pin-E-only cables untested |
| 3 | Bus-powered multiport hub, no power-in | HDMI dark at 400 mA (seen) | display + light USB devices (hub negotiates 900 mA, seen) | same, only with `vbus-supply`. HDDs or phone charging from the hub will brown out (1 A OTG ceiling) |
| 4 | PD hub, Cube first, charger later | keeps sourcing (seen) | same | same: needs DRP in the source PDO plus a working PR_Swap. Advise "charger first" |
| 5 | **PD hub, charger first (main case)** | charges only, DR_Swap refused (seen) | charges, DR_Swap accepted, USB dead, DP -71 (seen) | **charge + DP 1080p + USB3 + USB2** expected (the debugfs workaround showed the hardware can do it; the c2c path itself has never run on the device). Pulling the charger: Cube detaches and re-attaches as source (needs `vbus-supply`); a few seconds blank, DP back only if the hub re-sends Attention or Nano retries |
| 6 | Switch-style TV dock with brick (third-party) | charge only | charge, no display | expected to work if the dock sends DR_Swap. Official Nintendo dock: uncertain (proprietary handshake) |
| 7 | Steam Deck / handheld docks | charge only | charge, no display | expected to work |
| 8 | Laptop USB-C dock (Dell/Lenovo/HP/Anker) | charge only | no display | one output expected; MST docks mirror or show the first output; pin C without MF loses USB3 unless DP-C |
| 9 | USB-C monitor with PD | charge only | no display | works if the monitor sends DR_Swap; otherwise needs F4 (D6) |
| 10 | Portable monitor | no power-in: too little power in every state. With power-in: as row 9 | | |
| 11 | Thunderbolt 4 / USB4 dock | charge only | no display | expected to fall back to DP alt mode; TB3 Alpine Ridge docks: charge only |
| 12 | Plain charger (5 V, PD, PPS) | charges | charges | charges; DCP stays at 2.0 A; 3 A contracts reach 2.0 A only with 4d step 2. Never above 5 V |
| 13 | PC / laptop port | adb/MTP | adb/MTP | adb/MTP; F4 must not fire here |
| 14 | DisplayLink dock | no | no | with power: via USB host and the DisplayLink app (INFERRED) |
| 15 | DRP partner: phone, other handheld, laptop DRP port, PD power bank | today's roles (not recorded) | same | Try.SNK is programmed in stage 1c: roles may change. Regression-test |
| 16 | USB-A OTG adapter with a flash drive (non-PD) | works | works | works only with `vbus-supply` (Q1) |

Not covered by any state: VCONN-powered adapters and active cables with the Cube as source; Fast Role
Swap. Both orientations of the plug must be tested; every recorded session so far used CC1.

---

## 7. Other T820 devices: portability design

**What carries over:**
- The DT gap (R1) and the c2c gap (R4) come from Unisoc's reference `ums9620-2h10` overlay, which entries 0,
  2, 3 and 4 of the Cube's dtbo share (entry 1 differs in its 5 V sink current). CONFIRMED for the Cube. For
  the RG556, RG406H, RG406V, RG476H and RG Slide this is INFERRED: same platform (`ums9620_2h10`), same
  flashing route, each with its own Anbernic-customised dtbo (upstream ships separate
  `RG_556_STOCK_DTBO.zip` / `RG_CUBE_STOCK_DTBO.zip` and warns not to mix them).
- The driver defects (R2, R7-R10, R12) are in shared Unisoc code. They exist on any T820 device with these
  5.4 drivers and DP alt mode wired (INFERRED until each device's modules are checked).

**What doesn't carry over:**
- Binary patches with fixed offsets: different builds shift code and struct offsets (vermagic and
  `__versions` CRCs pin each `.ko` to its build). Patched `.ko` files from the Cube must never be copied to
  another device.
- The DT edit itself: in particular `vbus-supply`, which must point to **that board's** boost regulator, and
  the fragment layout. It is generated per device from the table below, never copied.
- The Cube's charging values (AW32257). Other boards may use a different charger.
- The source PDO limit (tied to the AW32257's 1 A OTG).
- The 2-lane DP result and pin-D preference: a board with a working PTN38003A may do 4 lanes on pin C.
- Proof: the c2c fix has not run yet even on the Cube. Nothing about other devices is better than INFERRED.

**Design:**
1. **Per-device tables, never per-device code.** In the device pack/catalog (`installer/catalog/devices/<id>.yaml`
   or `packaging/devices/<id>/`): dtbo input sha256, `dtbo_idx`, entry id, the DT edit triples, **the VBUS
   regulator node** (symbol and whether it is local to the overlay or in the base DT), the extcon wiring of
   `usb`/`usb3`, the charger IC and its OTG rating, the source-PDO current cap, whether DP is wired, whether a
   PTN38003A is fitted.
2. **DT first.** The c2c properties need no binary offsets. The strings `use_pdhub_c2c` / `use-pdhub-c2c`
   being present in a device's modules is necessary but doesn't prove the same behaviour: the offline check
   (5.) compares the readers and the R2/R12 sites by anchors. The patcher locates nodes through `__fixups__`,
   `__symbols__` and `__local_fixups__`, so it is not tied to fragment numbers.
3. **Module patches by anchors.** Where a binary patch is still needed (DP-A, DP-B, DP-C, DP-D), sites are
   found by symbol plus instruction pattern and relocation anchors (calls, string references), with wildcards
   for registers and struct offsets. Exactly one match or refuse. Known builds stay as hash-keyed fixtures.
   Prototype: `out/.dock/work/t820-portability/proto/locate.py` (finds the Cube's `.text+0x594` site from
   anchors alone).
4. **Identity gate:** offline by PAC or partition hash; online by `ro.boot.hardware=ums9620_2h10`,
   `ro.product.vendor.model` (RGCUBE for the Cube), `ro.boot.dtbo_idx`. Unknown device: refuse.
5. **Offline check before any hardware:** extract each device's dtbo, vendor_boot (base DT) and vendor_dlkm
   from its PAC (`pactool extract` -> `lptool` -> `fsck.erofs --extract`), hash the six modules, compare
   build-ids, run the DT and module locators in dry-run mode, and run the `fdtoverlay` merge check against
   that device's base DT. Downloading the PACs (2.6-3 GB each) or the stock DTBO zips needs the owner's
   go-ahead (D8).
6. **Per-device hardware tests** (matrix rows 1, 5, 13, 16, both orientations) before enabling the dock fix
   for that device in the catalog. The RG Slide's DP wiring is unconfirmed; the RG476H's sources conflict.

---

## 8. Risks

| Risk | Likelihood | Effect | Mitigation / recovery |
|---|---|---|---|
| **No VBUS as source in c2c mode** | likely without `vbus-supply` on `pd@e00` (R12) | HDMI dongles, bus-powered hubs, OTG adapters dead; tcpm source-on loops | `vbus-supply` is part of the core set; phase 1 tests rows 1, 3, 16 first |
| **Partial c2c set** | low (patcher refuses) | boost back-drives a powered hub, or unbalanced regulator disables | all-or-nothing rule (4a); boot check reads all properties under `/proc/device-tree` |
| A malformed dtbo | low (patcher self-checks, offline merge check) | bootloader can't apply the overlay: no boot or a broken board config (touch, keys, fan) | phase 1a: data backup, patcher-built image with a proper footer, `fdtoverlay` diff. Recover: adb (Wi-Fi) + `dd` of the backup if Android boots; else download mode, which means a Full install and data loss (see 5) |
| c2c mode breaks attach or charging | medium (Anbernic never validated it) | no charging, no adb over USB, odd Try.SRC/Try.SNK behaviour | staged 1b/1c/1d; regression list in phase 1. Revert: flash the original dtbo. Recovery is affected the same way |
| charger-manager reacts to USB=0 while sinking in c2c | UNKNOWN | charging stops or charge vote drops after a DR swap | phase 1 logs ibat and charger-manager after the swap |
| Kernel panic in the c2c path at boot with a cable attached | low | boot loop while plugged | unplug and boot; then revert the dtbo |
| `sc27xx_pd` probe waits on `vddvbus` | low (aw32257 loads in normal and recovery lists) | no PD until the charger driver is up; forever if it fails | check boot and recovery dmesg for deferred probe |
| Binary patch on the wrong build | low (sha/anchor checks refuse) | module fails to load or misbehaves | refuse on any mismatch; never ship without a hash or unique-anchor match |
| Patched sc27xx_* module fails to load | low | no USB-C at all (charging also depends on it) | device still boots; Wi-Fi adb; installer Update or Full with the previous package |
| **DP-A black screen in the main case** | medium if shipped alone | early training fails before host is up, DP stuck "on", later Attention ignored | ship only with variant (a) or DP-D; gate test "monitor connected before docking, c2c" |
| DP-A hook bug | medium until tested | a PD-message path crash (port lock held) | ship DP-A in its own phase; test with dongle and hub; easy revert |
| DR swap after DP is up (F4, sysfs) | certain if it happens (R8) | DP off | F4 not while DP is up; Nano retry with DP-A |
| Charge-budget writer slows ordinary chargers | certain if B applies to all types | DCP drops from 2.0 A to about 0.5-1.0 A | B acts on UNKNOWN-type sources only |
| Charging overdraw on a 1 A contract | real if 4d part A ships; whenever Nano isn't running even with B | PD hard resets, source sag, hub brownout | default: don't ship step 2; Q5 blocker; D11; measure with a USB-C meter |
| Charger IC overvoltage | only if fchg/PPS/9 V is enabled | AW32257 OVP or damage | hard rule in 4d; a unit test that the patched DT has no such property and no >5 V PDO/APDO |
| Debugfs reads | any userspace read of the dwc3 `mode` file | SError panic | never read it; test grep |
| Old installer + new package | certain if not bumped | Update refused, Full install asked | `MIN_INSTALLER` bump |
| Update removes the slot-B escape | certain (Update writes both slots) | no untouched slot after an Update | keep the previous package downloadable as the rollback |
| Behaviour change for monitors via Nano | low | Nano's flip-to-source logic changes | NanoTypec unit tests; rows 1, 3 regression |

---

## 9. Phases

Each phase ends with on-device tests on the Cube. Keep `adb` over USB available where possible; Wi-Fi adb
drops (section 11).

**Phase 0: investigation (done).** Root causes, the v3 DT live, research write-ups, scratch prototypes
(`dtbo_usbc.py`, DP-A/DP-B words, anchor locator), technical and safety reviews.

**Phase 1a: preparation, before any flashing (owner, D9).**
- Back up the user's data. A failed dtbo can end in a Full install, which erases userdata.
- Confirm download-mode entry works. Find out whether any tool can write `dtbo_a` alone in download mode
  using `out/.dock/dtbo_a.img` (Q12); the Thimsy installer only does Update and Full.
- Check `lpdump` for `*_b` partitions and fastboot `--set-active` (Q6, Q11).
- Build the test images with `patch_dtbo.py` plus `avbtool add_hash_footer` (not a raw hand edit and `dd`: the
  edit grows the image while the old footer records 538536 bytes, and a bootloader may load it truncated), so
  the hand-test image is the same artifact phase 2 ships.
- Check each image offline: `fdtoverlay` onto the base DT from vendor_boot, decompile, diff against
  `live.dtb`; only the intended properties may differ.
- Capture one Cube-as-source session on today's DT (HDMI dongle) and keep its dmesg and PD log: it should show
  `vbus := On` on the dummy regulator, confirming R12 on the device.
- First fallback after this: adb root over Wi-Fi and `dd` of the backup.

**Phase 1: c2c on the device (owner), staged.** Flash the phase-1a images to slot A only.
- **1b: core set** (v3 connector, typec/usb3/usb c2c, `vbus-supply`).
  - Boot check: all properties present under `/proc/device-tree` (`soc/aon/spi@400000/pmic@0/typec@380/use_pdhub_c2c`,
    `soc/ipa-apb/usb3@25100000/use_pdhub_c2c`, `soc/aon/usb@a00000/use_pdhub_c2c`,
    `soc/aon/spi@400000/pmic@0/pd@e00/vbus-supply`); dmesg has **no** `sc27xx-pd supply vbus not found`;
    `/sys/class/typec` has only tcpm's port; Nano still finds it. (The `use_pdhub_c2c = %d` dmesg line comes
    from sc27xx_pd only and reports the pd flag, 0 in this stage.)
  - **VBUS first:** HDMI-only dongle as source (row 1), hub without charger (row 3), OTG adapter with a flash
    drive (row 16). Expect `set vbus on` / `vbus := On` and the AW32257 boost on. Any failure stops phase 1.
  - Hub with charger, charger first: expect `sc27xx_typec_extcon_drswap_work pd_swap_evt:4`, then dwc3-sprd
    `!id`, `turn on host`, no `Failed to enable vbus`. USB2 hub, billboard, USB3 hub and devices enumerate on
    the first try. DP trains while charging. Log ibat and charger-manager messages for a minute after the swap.
  - Pull the dock's charger while docked: the Cube re-attaches as source and powers the hub.
  - Swap back (unplug): `switch dp/dm to other` on disconnect.
- **1c: add `use-pdhub-c2c` on `pd@e00`.** dmesg `use_pdhub_c2c = 1`. Sysfs `data_role`/`power_role` swaps
  work; repeat 1b's list.
- **1d (only if needed): extcon rewiring variant** (4a).
- Regressions in every stage: plain charger (DCP and PD); PC cable (adb/MTP); HDMI-only dongle as source;
  hub without charger; OTG adapter; USB-C audio adapter; DRP partners (phone, another handheld, a laptop's
  DRP port, a PD power bank); suspend/resume docked; boot docked; off-mode charging; recovery adb.
- Do not read the dwc3 debugfs file; use `lsusb` and dmesg.
- Decision after phase 1: which stage ships (default 1b, or 1c if F4/Nano swaps are wanted).

**Phase 2: ship the DT.** `avb_footer.py`, `patch_dtbo.py`, build.sh step, installer catalog/e2e changes,
package test for the two `PacDelta` ops, `MIN_INSTALLER` (the README `port1` fix is already done in test 40,
4e.7). Test: a built PAC's
DTBO equals the phase-1 image byte for byte; installer Update on the Cube from a clean Thimsy install; then
a Full install. Repeat phase 1's list. Keep the previous package downloadable as the rollback.

**Phase 3: DP patches.** DP-B first (small), then DP-A with variant (a) (or DP-A plus DP-D), then DP-C if
wanted. Generic `kmod.py` registry. Tests: **gate: monitor connected before docking, c2c**; matrix rows 1
and 5; monitor connected last (Attention path); first-Attention 0x18a dongle; a DR swap after DP is up (R8);
`debugfs sprd_tcpm` reads empty after DP-A.

**Phase 4: Nano.** 4e items 1-5, 8 (7 is done in phase 2). Tests: NanoTypec unit cases; on device rows 1, 3,
5, 13; pull the dock's charger while docked (DP comes back via the retry, with DP-A).

**Phase 5: charging.** Measure (4d step 1). Step 2 only if D5/D11 adopt it and Q5 is answered. Tests with a
USB-C meter: test hub (≤ about 5 W), 5 V 3 A DCP (stays about 2.0 A), 5 V 3 A PD-only charger (about 9-10 W
with step 2), 1.5 A Rp source, 1 A PD source with Nano stopped (the unguarded case), laptop SDP (0.5 A).
VBUS never above 5.x V; no hard resets. Temperatures under load.

**Phase 6: dock matrix.** Rows 1-16 in both plug orientations, plug orders (i)-(viii) from the dock research.
Source PDO DRP isolation run, one boot each, PD log for the first 3 s: 0x0401905a, 0x2401905a (DRP),
0x0601905a (DRD), 0x2601905a (v2), 0x26019028 (v1).

**Phase 7: other T820 devices.** Offline extraction and dry runs (7.5), then per-device tables (including the
VBUS regulator node), then hardware tests per device with a volunteer.

---

## 10. Open questions and decisions for the owner

**Decisions:**
- **D1.** Patch only dtbo entry 2 (recommended; the others are reference boards the Cube never boots), or all
  5 entries as the build-integration prototype does? (All entries need per-entry expected values: entry 1's
  sink-pdos differ.)
- **D2.** Drop the PPS APDO from sink-pdos (recommended) or leave it?
- **D3.** DT route for c2c (recommended) vs the module patches? (The module patches can't supply VBUS, so the
  DT edit is needed either way.)
- **D4.** Ship DP-A (code cave in sprd_tcpm, stubs its debugfs dump), and if so with variant (a) or with DP-D?
- **D5.** Charging 4d step 2: adopt the Nano charge-budget writer (UNKNOWN-type sources only), or stay at
  1.05 A for UNKNOWN sources (recommended until Q5 and D11 are settled)?
- **D6.** Nano asks for host role (F4) for docks and monitors that never send DR_Swap?
- **D7.** Source PDO: keep v3 (900 mA) or lower to save battery with monitors that draw from the Cube?
- **D8.** Approve downloading other devices' firmware (stock DTBO zips about 743 KB; PACs 2.6-3 GB each) for
  the offline portability check?
- **D9.** Accept phase 1's data-loss risk (a failed dtbo can end in a Full install) and back up first?
- **D10.** Ship `vbus-supply` on `pd@e00` as part of the core c2c set (recommended; required for the
  Cube-as-source cases), which stage ships (1b or 1c), and whether to try the extcon rewiring variant (1d).
- **D11.** If step 2 is adopted: require a fail-safe limit (kernel- or DT-side) before shipping part A, or
  accept the windows where Nano isn't running (off-mode charging, recovery, early boot, a Nano crash)?

**Open questions:**
- **Q1.** Does c2c mode with `vbus-supply` power non-PD OTG adapters and bus-powered dongles? Without the
  supply: very likely not (R12). (phase 1b, first test)
- **Q2.** Does host mode come up before the first HPD in c2c mode, and does BC1.2 still overlap host start?
- **Q3.** What exactly failed with v2 (no GoodCRC, PR_Swap, Get_*)? No PD log was kept.
- **Q4.** Is the AW32257 REG06 safety limit (1116 mA default) active? What does register 0x38 do?
- **Q5.** Does charger mode (off-mode charging) run Thimsy's init hooks? Blocks 4d part A.
- **Q6.** Can fastboot switch slots on this bootloader (`--set-active`)? It sets how cheap a slot-B rollback is.
- **Q7.** Do the other T820 devices ship byte-identical modules, the same dtbo connector, the same charger IC
  and VBUS regulator, a fitted PTN38003A, DP wiring? (needs D8)
- **Q8.** Flipped plug (CC2): does the USB3+DP mux work without the redriver?
- **Q9.** No public Unisoc 5.4 source matching the Cube's build was found. The closest are the Nubia/ZTE
  5.4.254 trees; six Nubia device overlays set c2c (on typec, usb, usb3 only), Unisoc's reference overlay does
  not. What their base dtsi puts on `pmic_pd` (`vbus-supply`?) is UNKNOWN.
- **Q10.** Does charger-manager's typec-extcon handler (USB=0 while sinking in c2c) disturb charging?
- **Q11.** Do `*_b` logical partitions exist in `super` (`lpdump`)? Is slot A marked successful?
- **Q12.** Can any available tool write a single partition (`dtbo_a`) in download mode without a Full install?
- **Q13.** What does the IRQ_HPD op (`[ops+40]` with 2) do on a link that never trained? (decides DP-D's value)
- **Not verifiable here** (left as reported or INFERRED): the RG556 kernel build date, Anbernic's "5 V 2 A"
  rating, `sprd_ssphy_init` being required for DP, the exact AW32257 chip code for 2.3 A.

---

## 11. Separate investigation: Wi-Fi drops

Not part of this fix. Seen: adb over Wi-Fi dropped while the hub was attached or after dwc3 mode switches;
the Cube itself lost Wi-Fi at least twice (fixed by toggling Wi-Fi). Wi-Fi is Marlin3 over SDIO, 5 GHz
(5785 MHz).

**What the log shows** (`klog-1.txt`, CONFIRMED):
- Two Wi-Fi firmware asserts: `fw assert:[CMD] CMD_REGISTER_FRAME, [REASON] CMD_RSP_TIMEOUT_ERROR` at
  06:24:03.457 and `CMD_LLSTAT ... CMD_RSP_TIMEOUT_ERROR` at 06:28:13.317. Each is followed by
  `wcn_reset_process`, then `wlan0: iface_tx_timeout` (34 in total) and, the second time, a burst of
  `WCN SDIO: sdiohal_card_lock failed`.
- **Both fall inside a failed DP link-training window:** HPD at 06:24:00.154, 14 `PHY BUSY timed out`
  (about 0.5 s apart), `Link training failed -71` at 06:24:07.408; and HPD at 06:28:09.949, `-71` at
  06:28:17.233. The SDIO errors start in the same millisecond as the second `-71`.

**Hypotheses** (all INFERRED; none tested):
1. **DP training busy-waits starve the Wi-Fi host side.** Each PHY BUSY wait polls 100001 times with
   `udelay(1)` in sprd-drm's IRQ thread; a failed training runs 14 of them in about 7 s. If preemption or a
   shared lock is held, the SDIO/WCN command path misses its timeout. Test: does Wi-Fi survive a successful
   training (3 waits) and the c2c fix (training succeeds)? Disassemble whether the waits run under a spinlock.
2. **Combo PHY / clock / power-domain interaction.** Re-initialising the USB3/DP PHY in device configuration
   may disturb a shared clock or supply that the WCN chip uses. Test: Wi-Fi throughput while toggling DP
   without a hub.
3. **Power droop.** VBUS and system-rail changes during PD swaps or while the Cube sources 900 mA. Test:
   with the dock on its charger vs Cube as source.
4. **RF interference from the hub or USB3/DP signalling.** Known for USB3 near 2.4 GHz; less likely at
   5.8 GHz. Test: same hub with a shielded cable or at distance.
5. **adb-only drops: adbd restarts when the USB gadget config changes** (dwc3 role changes rewrite
   `sys.usb.config`), which kills adb over TCP too even when Wi-Fi is up. Test: `ping` the Cube during a
   swap while adb is connected.

If phase 1 makes DP training succeed first time, re-check whether the asserts disappear. That cheaply
separates hypothesis 1 from the others.

---

## 12. First implementation (test 40): what ships and the decisions taken

Status of this section: what test 40 implements (revised 2026-11-24 after a technical and a safety review
of this section and `display_modes_plan.md`). None of it has run on the device.

The owner asked (2026-11-24) to implement the plan, review it three times, test in Docker and publish a
draft test release. Decisions taken for this first implementation, with the plan's recommendations:

| Decision | Taken |
|---|---|
| D1 | dtbo entry 2 only |
| D2 | drop the PPS APDO from sink-pdos |
| D3 | device-tree route for c2c; no P-C2C module patches |
| D4 | DP-A **not** in this release (it needs phase 1 on the device first); DP-B ships; DP-C, DP-D no |
| D5 / D11 | no charging step 2: UNKNOWN-type sources stay at 1.05 A; phase 5 measures first |
| D6 | no F4 |
| D7 | source PDO v3 (5 V 900 mA, original flags) |
| D8 | no downloads of other devices' firmware; the per-device table holds only the Cube |
| D9 | the owner backs up data before installing; recovery is a Full install |
| D10 | core set (stage 1b) with `vbus-supply`; no `use-pdhub-c2c` on pd@e00 (stage 1c), no 1d |

Display modes (`display_modes_plan.md` section 7), taken:
1 kernel route; 2 bandwidth check with 1000; 3 Auto stays 1080p60 (stage 1); 4 3840x2160@30 offered with
confirm-and-revert, DCI 4096-wide modes never offered (rejected in the kernel, display plan Step 1C, and
filtered in system_server); 5 1440p60 offered (hidden if it fails on the device); 6 per monitor; 7 modes
below 50 Hz hidden in Dual Screen; 8 15 s, cursor on Revert; 9 physical and render range pinned to the
mode's rate (as today); 10 no "render at 1080p, output 4K". The 4K60 4:2:0 video path (section 8) is
planned, not implemented.

Decision 4 relies on confirm-and-revert, which at 3840 wide works only if the Keep/Revert dialog reaches the
monitor: `AddFlushData` drops every layer that is not 3840 wide. So 3840x2160 is listed only in a build that
also forces client composition for a monitor at 3840 wide (display plan Step 5); a build without it hides
3840x2160.

What ships where:
- **Vendor side, both bases** (dtbo, vendor_dlkm, vendor_boot):
  - The dtbo change (`scripts/patch_dtbo.py`) behind `DOCK_FIX`, **default 1** (owner's decision,
    2026-11-24): test 40 ships the c2c dtbo **without the phase-1 hand test**, so Docker (1.2 base) builds
    carry it too. `DOCK_FIX=0` keeps the stock dtbo. `patch_dtbo.py` holds a per-device `tested_sha256`,
    the sha256 of the approved image; for the Cube it is `out/.dock/dtbo-c2c.img`,
    `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536` (`patch` of the stock
    `ca5a55ff…` with AOSP's test key), approved by the owner for test 40 without a device test.
    `build.sh` refuses `DOCK_FIX=1` unless the patched image matches it byte for byte (phase 2's "byte for
    byte" check as a build gate) and unless `fdtoverlay` is there for the merge check. The owner accepts the
    risk: a bad overlay means no boot or a broken board config, Update writes both dtbo slots, so the way
    back is a Full install of an earlier Thimsy or stock PAC, which erases user data; the owner debugs
    against build 40. The README tells users to back up, be ready for a Full install, plug a dock's charger
    in first, and what to report.
  - In `sprd-drm.ko`: the link-bandwidth check and the DCI reject (display plan 1A, 1C), and DP-B as its own
    named list in `sprd_drm.py`, so it can be dropped without touching 1A.
  - vendor_boot (recovery) never gets 1B.
- **1.4.4 base only** (needs Thimsy's framework hold):
  - Every valid mode user-defined (1B), in vendor_dlkm only: `patch_module(ko, all_modes=False)` swaps new
    word [2] of the 0x8d8c tuple for the `nop` when `all_modes` is true. `build.sh` exports
    `THIMSY_DP_ALL_MODES=1` only with `SYSTEM_IMG` (0 otherwise), and `sprd_drm.py` takes its default from
    it; `patch_vendor_boot.py` never applies it. `test_vendor_patches.py` tests both variants.
  - The framework's per-monitor mode with trial and revert, keyed on a hash of `DeviceProductInfo`; Auto
    always pins a concrete mode; the monitor's mode list filtered in system_server; `try` and `keep`
    checked in system_server; the applying marker; stored modes below 50 Hz ignored in Dual Screen
    (display plan Step 2 and Step 3's framework part).
  - **Not implemented (TODO):** Step 3's SurfaceFlinger part, restarting a monitor's vsync check
    (`gammaCheckMonitorVsync`) when its mode changes.
  - `cmd display gammaos-monitor-mode`, Nano's Output Resolution and Refresh Rate rows (both disabled over a
    running game).
  - The NanoTypec changes (4e items 1-4, 7). The README advice to plug the dock's charger in first (4e.8)
    applies to both bases, as both carry the dtbo fix.
  - On the 1.2 base, 1B is left out, so the stock framework sees no more modes than today (up to 3).
    Without Thimsy's hold, the HWC's per-width config groups would let SurfaceFlinger switch between
    24/30/50/60 Hz modes, each a full modeset.
- **Installer** (repo thimsy-os, branch universal-installer, checked out as `thimsy-os-installer`; section 5):
  - `anbernic-rg-cube.yaml`: add `DTBO, DTBO_B` to `update` **and** remove them from `update_check`
    (`spd_plan` bails on an entry that is both written and checked, `flash.rs:271`); add `dtbo_a, dtbo_b` to
    `writes_update`.
  - A catalog check in `catalog.rs` that rejects `update` and `update_check` overlap.
  - `installer/tests/e2e.sh` with 7 partitions and `dtb_b` as the read-back mismatch partition; the package
    test that a changed DTBO gives two `PacDelta` ops (a real PAC has them only with `DOCK_FIX=1`).
  - `MIN_INSTALLER` bump.
  - Update then writes the build's dtbo to both slots: the c2c one by default, the stock one with
    `DOCK_FIX=0`. On the owner's test Cube that replaces the hand-edited v3 dtbo (today the same Update is
    refused there and a Full install is asked for).

Phase 1 (the hand test of the c2c dtbo on the owner's Cube) was skipped for test 40 by the owner's decision;
this overrides sections 5 and 9 where they put phase 1 before any build with `DOCK_FIX=1`. Test 40 itself is
the first run of the c2c dtbo on a device, and its results go in section 2. The README asks testers to
report: whether the Cube boots and charges from a plain charger; a dock with its charger plugged in first
(power, HDMI, USB3 and USB2); an HDMI dongle or hub without a charger; a self-powered dual-role USB-C monitor
without PD, the first plug-in after a boot and a later one separately; and for a failure,
`dmesg | grep -iE 'tcpm|typec|pd'`, `ls /sys/class/typec` and
`cat /sys/class/typec/*/data_role /sys/class/typec/*/power_role` (`getprop sys.gammaos.nano.typec_port` only
on a 1.4.4 build). Test 40 is a 1.2-base build with no Nano, so it has no NanoTypec flip (which writes
`source` to `port_type` on a later plug-in) and no `typec_port` property: a later plug-in that stays dark is
the 1.2 base's behaviour, and the NanoTypec flip case belongs to a later 1.4.4 build. The self-powered
monitor case works today on 1.4.4 with VBUS from
dwc3's OTG boost; with c2c and `vbus-supply` the VBUS comes from tcpm `set_vbus`, never observed (matrix row
9 should read "works today via the NanoTypec flip; with c2c, VBUS comes from tcpm").

Before this release leaves draft, on the device, because DP-B changes today's behaviour (a failed early
training leaves DP "on" and later Attentions are ignored, 4b DP-D): matrix rows 1 and 3, row 5, and a
first-Attention 0x18a dongle.
