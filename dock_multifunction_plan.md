# RG Cube dock plan, part 2: power, picture and USB together (test 41 findings)

Date: 2026-11-25. Status: research and plan, then run in test 42: the DK1 dtbo (fix A, `1b727faa…`)
was written to the Cube and ships in test 42 (dp_dock_findings.md §12.2); DK3 (other source currents:
no picture without the dock's charger at any offer) and DK1-TH2 (the owner's charge temperature steps:
the tables are live) were written to the Cube as tests only (dp_dock_findings.md §12.3). The plan below
is kept as written.
Follows `usb_c_dock_plan.md` (test 40's c2c dtbo fix) and uses its conventions: **CONFIRMED** (seen in code,
binaries or logs), **INFERRED** (reasoned from evidence, not tested), **UNKNOWN**. `.text+0x…` offsets are
into the module's `.text`; `dwc3-sprd.ko` `.text` starts at file offset 0x2000.

Evidence used:
- The test 41 dock run, `out/dp-evidence/dock-tv-run/` (`dmesg.txt`, `logcat.txt`, `state.txt`).
- The DP-only adapter run `out/dp-evidence/dmesg-run2-ring.txt`, and test 37's dock log `out/.dock/klog-1.txt`.
- Disassembly in `out/.dock/work/{roleswap-driver,dp-driver}/`. Excerpts used here are copied to
  `out/dp-evidence/dock-research/`.
- Unisoc's public `dwc3-sprd.c` and the Nubia UMS9620 overlays in `out/.dock/work/t820-portability/src/nubia/`.
  The source matches the Cube's binary function for function, down to the struct offsets.
- Read-only checks on the Cube, 2026-11-25: `out/dp-evidence/dock-research/device-readonly-2026-11-25.txt`.
  Module hashes on the device match `out/.dock/modules/`.

## Short answer

The hardware can do all three at once: in test 37 this dock gave charging, 1080p and USB2+USB3 together once
dwc3 was forced to host (2.0). Test 40 fixed the first blocker on the DR_Swap path, and test 41 stops at the
second. The redriver message is a red herring. Two bugs explain the failures:

1. **The USB host never starts after the DR_Swap.** Test 40 turned on Unisoc's c2c ("PD hub") mode but kept the
   non-c2c extcon wiring for the USB controller. dwc3-sprd still takes its VBUS state from `extcon_gpio`, which
   stays 1 while the dock charges the Cube. When the swap's host event arrives, dwc3's id notifier resets
   charger detection to "not detected". The event work then refuses to run: "vbus charger detect not finished".
   Nothing retries it, so the host is lost for good. Unisoc's own c2c boards wire the USB controller's VBUS to the
   **typec** extcon. That wiring makes the swap send USB=0 first, which clears the gate. (CONFIRMED in binary,
   source and log.)
2. **DisplayPort fails because of 1.** The USB3/DP combo PHY is powered and set up by the USB side
   (`sprd_ssphy_init`). With dwc3 idle, the DP PHY never leaves BUSY, and training ends with -71. The same dock
   trained fine (pin D, 2 lanes, 1080p while charging) in test 37 once host mode was forced. The DP-only adapter
   works because the Cube is host from the start. (CONFIRMED by the two runs; the PHY power link is INFERRED from
   the code.)
3. **Wi-Fi dies because the failed training starves CPU0.** DP training polls the PHY with
   `udelay(1)` × 100001 per wait, inside the `irq/76-dw_dptx` thread. That thread is SCHED_FIFO 50 and pinned
   to CPU0, the same as `irq/24-mmc2`, the SDIO host of the Wi-Fi chip. A failed training runs 14 waits (about
   7 s). The SDIO tx thread sat on `xmit_lock` for exactly that window, the firmware timed out a command and
   the WCN driver reset the chip. Its recovery then failed. (Thread and affinity facts CONFIRMED; that mmc2's
   IRQ thread is the starved path is INFERRED.)
4. **The PTN38003A redriver isn't on the board, or doesn't answer.** Its probe reads register 0 and gets -EIO.
   Every caller ignores the -ENODEV that follows. The same 3-messages-per-training pattern appears in the
   DP-only runs where training succeeds. It is not a cause. (CONFIRMED.)

**Recommended fix:** a one-line dtbo change (move usb3's first extcon from `extcon_gpio` to `pmic_typec`,
matching Unisoc's c2c overlays), plus moving the `dw_dptx` IRQ off CPU0 at boot. Neither needs a binary patch.
Back them up with a retrain-on-HPD fix in sprd-drm, which belongs to the main session's workflow. Section 4
has the options, section 5 the recommendation and section 6 the test plan.

---

## 1. What the test 41 run shows (timeline)

Kernel time from `dock-tv-run/dmesg.txt`. The dock's charger was plugged first, then the TV on its HDMI port.

| t (s) | Event | Line |
|---|---|---|
| 172.95 | typec interrupt, `sc27xx_connect_set_status_use_pdhubc2c sink connect !` (c2c attach path) | 15894, 15902 |
| 173.03 | `dwc3-sprd …: vbus:1 event received` (from `extcon_gpio`) | 15906 |
| 173.14 | `IRQ: PD received hardreset`. VBUS drops: `vbus:0 event received` at 173.17 | 15920 |
| 173.43 | dwc3 `charger = 0`, `charge detect finished`, then `ext event: id 1, vbus 0`, `Cable disconnected` | 15992-15997 |
| 174.41 | VBUS back: `vbus:1 event received`. **BC1.2 detection starts again and runs until 178.29** | 16072 |
| 176.10 | `sc27xx_typec_set_pd_dr_swap_flag entered!`, `pd_swap_flag:4`, `extcon_drswap_work pd_swap_evt:4` | 16169-16172 |
| 176.14 | `dwc3-sprd 25100000.usb3: host:1 (id:0) event received` | 16177 |
| 176.19 | `dwc3_sprd_evt_prepare_work enter` → **`vbus charger detect not finished`**. Nothing else from dwc3 | 16179, 16183 |
| 177.98 | `sc27xx_pd_dp_altmode_notify: vdo:0x9a, hpd_status = 1` (pin D, HPD high) | 16291 |
| 178.10 | `dptx_irq: DPTX_ISTS_HPD`, training starts | 16298-16299 |
| 178.29 | dwc3 `charger = 0`, `charge detect finished`. No evt work queued (type 0 sets "charging mode") | 16305-16306 |
| 178.34 | WCN `sdiohal_tx_thread` takes `xmit_lock` in `sdio_writebyte` (time stamp 178341505526 ns) and holds it until 185.51 | 16398 |
| 178.57 … 185.35 | 14 × `dptx_phy_wait_busy: PHY BUSY timed out`, about 0.5 s apart | |
| 179.15 | **`sched: RT throttling activated`** | 16318 |
| 180.04 … | `ptn38003a hadn't been probe.` × 3 around each `dp_link_rate_set` / `dp_lane_set` | 16330… |
| 183.52 | `fw assert:[CMD] CMD_LLSTAT, [REASON] CMD_RSP_TIMEOUT_ERROR`, `Task(sdiohal_tx_thre) holds xmit_lock!!!` | 16390-16401 |
| 185.49 | `[drm:dptx_link_training] *ERROR* Link training failed -71` | 16505 |
| 185.51 | `sdiohal_tx_thre … expire_1000ms_cnt 1(178341505526, 185506008021)`: the tx thread finally ran, 7.16 s later | 16508 |
| 185.53 | `wcn_reset_process reset begin`, `marlin chip en pull down`, chip power-cycled | 16521-16541 |
| 185.57 … | `mmc2: sprd-sdhci: CMD5/CMD52 Error` loop. Wi-Fi never comes back | |
| 188.80 | `NETDEV WATCHDOG: wlan0 (): transmit queue 0 timed out` (a WARN, not a panic) | 16651 |

`state.txt` shows `/sys/bus/usb/devices` = `1-0:1.0 usb1` for the whole run, so there was never a second bus. The
typec port's data role does become `[host]`. The `ep0out` message at 180.10 and the adbd restart at 186.0 are
init rewriting the gadget config (`sys.usb.config=none`), not real gadget traffic.

---

## 2. Failure by failure

### 2.0 Starting point: the test 37 run where everything worked

The owner's lead, checked against `out/.dock/dock-watch.log`, `klog-1.txt` and `FINDINGS.md` (all CONFIRMED):

- **The working state** (07:06, test 37 hand-edited v3 dtbo, no c2c). DR_Swap accepted at 07:06:25.098
  (`set roles [sink:host]`). The `hostfix` script forced dwc3 to host. Attention `0x9a` with HPD at
  07:06:28.008. `Link training succeeded rate=2 lanes=2` at 07:06:29.755 after only 3 `PHY BUSY timed out`.
  `DP-1=connected modes=1920x1080`, battery `Charging` at +640-678 mA.
- **The bus list in that run:** bus 3 had the USB3 hub 2109:0817; bus 2 had the USB2 hub 2109:2817, the
  billboard 2109:8817, a keyboard (05ac:024f) and a receiver (046d:c53f). So the hardware does all three at
  once, through this dock, on pin D, with no redriver. That rules the PTN38003A out a second time.
- **The blocker in test 37 was one step earlier than in test 41.** In test 37 the swap was dropped inside
  sc27xx_typec (`typec has disconnected!`, R2 in `usb_c_dock_plan.md`), so dwc3 never got a USB_HOST event and
  the typec extcon stayed USB=1 USB-HOST=0. Test 40's c2c flags fixed that step: in test 41 the event reaches
  dwc3 (`host:1 (id:0) event received`, 176.14). It is then dropped by the next gate, described in 2.1.
- **That run used a debugfs write to force host. It is not part of any fix here**, and nothing in this plan
  reads or writes `/sys/kernel/debug/*dwc3*/mode` (reading it panicked the kernel in `dwc3_mode_show`,
  `panic-dmesg.txt`). The fix has to make the vendor DR_Swap path itself start the host. Fix A in section 4
  does that by giving dwc3 the typec VBUS edge its c2c code expects.

**The USB2 "second toggle" (FINDINGS.md lines 61-63) and why A shouldn't need it** (INFERRED, test 5 in 6.3
checks it):
- In test 37, BC1.2 was still running after the swap. The swap was at 06:23:58.322; the hsphy detection
  finished at 06:23:58.801 and the ssphy detection at 06:24:00.357. During BC1.2 the D+/D- lines belong to
  the detector (`switch dp/dm to other`). A debugfs switch within about 1 s started the host on lines it didn't
  own, so USB2 enumeration failed with -71. A second toggle after BC1.2 ended worked.
- On the vendor path with fix A:
  - The swap's USB=0 makes `dwc3_sprd_vbus_notifier` call `flush_delayed_work(chg_detect_work)`
    (`.text+0x2330`). That work's `ss_phy->charger_detect` call blocks until BC1.2 completes; in test 41 one
    call spanned 174.41-178.29.
  - The hotplug state machine then waits for the core to runtime-suspend and switches D+/D- to the controller
    (`call_sprd_usbphy_event_notifiers(0,1)`, c2c only, `.text+0x1bdc`) before `dwc3_sprd_otg_start_host`.
    That is exactly the ordering the second toggle got by luck.
  - Once the PHY is in host mode, the hsphy's own VBUS notifier ignores VBUS changes and starts no new BC1.2
    (`phy-sprd-qogirn6pro` `sprd_hsphy_vbus_notify` `.text+0x1080-0x1088`: flag +492, logs `USB PHY is host
    mode`).
- If USB2 still fails first time, A+ (PHYs on the typec extcon, so a PD hard reset no longer re-triggers
  BC1.2 at all) is the next step.

**Wi-Fi in the working run:** `dock-watch.log` shows 3 PHY waits (about 1.5 s) for that training and no WCN
assert. That log captured only filtered lines, so it is a hint, not proof. The DP-only runs (3 waits each)
have no asserts in full dmesg. Expect Wi-Fi to survive once training succeeds first time. Fix C is insurance
for any training that still fails (a dock whose HPD beats host start, a bad cable). Tests 1 and 5 check both.

### 2.1 The USB host never starts after the DR_Swap (root cause)

**The mechanism.** `dwc3-sprd.ko` CONFIRMED in the disassembly, and the public source says the same:

- `dwc3_sprd_id_notifier` (`.text+0x23a4`). On every id change it sets `chg_state = UNDETECT`,
  `charging_mode = false` and the retry count to 0 (`.text+0x248c`/`0x2490`/`0x2494`: `str wzr,[x19,#76]`,
  `strb wzr,[x19,#89]`, `str wzr,[x19,#104]`, i.e. struct +0xf4, +0x101, +0x110). Then it queues
  `evt_prepare_work`. It leaves `vbus_active` (+0x100) unchanged.
- `dwc3_sprd_evt_prepare_work` (`.text+0x18b4`):
  `if (vbus_active && chg_state != DETECTED && chg_state != RETRY_DETECTED) { "vbus charger detect not finished"; return; }`
  (`.text+0x1900-0x1914`: `ldrb w8,[x19,#256]; cbz …; ldr w8,[x19,#244]; orr w8,w8,#2; cmp w8,#3; b.ne`).
  Next, `if (charging_mode || boot_charging) return;`. Only after both checks does it call `dwc3_sprd_ext_event_notify`.
- Nothing restarts charger detection after this. `chg_detect_work` is queued only by the **vbus** notifier
  (`.text+0x239c`). When it finishes with type UNKNOWN or DCP, it sets `charging_mode = true` and queues no event
  work (`.text+0x20cc-0x20e4`). It re-queues only for SDP/CDP (`.text+0x20bc-0x2104`).
- So with VBUS present, **an id change can never start the host** unless something first drives `vbus_active`
  to 0.

**Why Unisoc's design works and the Cube's doesn't.** The typec driver's DR-swap work
(`sc27xx_typec_extcon_drswap_work`, `sc27xx_typec.ko` `.text+0x1704`; `typec-drswap-work.dis`) does, for swap
flag 4 (device→host): `dr_swap_executing = 1`; extcon **USB = 0** (`.text+0x17bc`); if attached, **USB_HOST = 1**
(`.text+0x17d8`); `dr_swap_executing = 0`.
- If dwc3's VBUS extcon is the typec extcon, USB=0 reaches `dwc3_sprd_vbus_notifier`. Its c2c filter ignores the
  event only when the typec is *detached* (`.text+0x229c-0x22b0`). The dock is attached, so the event is
  processed: `vbus_active = 0`, chg_state and charging_mode cleared (`.text+0x2320-0x2348`). The next USB_HOST=1
  then passes the gate and starts the host.
- On the Cube, `usb3@25100000` has `extcon = <0x116 0x64>`: `extcon_gpio` (VBUS) and `pmic_typec` (id) (live
  DT). In dtbo entry 2 this is `fragment@8/__overlay__/extcon`, fixed up through `__fixups__/extcon_gpio`
  (`/fragment@8/__overlay__:extcon:0`) and `__fixups__/pmic_typec` (`…:extcon:4`). The GPIO VBUS never changes
  during a DR swap, so `vbus_active` stays 1 and the gate stays shut.
- Unisoc's own c2c overlays rewire this. Nubia `ov-camellia.dts` (c2c) has `&usb3 { extcon = <&pmic_typec>,
  <&pmic_typec>; use_pdhub_c2c; }`. The non-c2c sibling `ov-camelia.dts` has `<&extcon_gpio>, <&pmic_typec>`. The
  c2c board also moves `&usb`, `&hsphy` and `&ssphy` to `pmic_typec`. `usb_c_dock_plan.md` 4a lists this as
  stage "1d" and predicted R5 might persist without it. Test 41 shows it is required, not optional.

**The timing made it worse but is not the cause.** The PD hard reset at 173.14 bounced the GPIO VBUS. That
re-ran BC1.2, which took 3.9 s (174.41 → 178.29), so the swap at 176.10 landed in the middle of it. Even without
that overlap, the id notifier's reset plus the UNKNOWN-type result (`charging_mode = true`) would have blocked
the host.

**What the debugfs workaround did (test 37):** writing `host` to the debugfs `mode` file bypasses this glue
state machine entirely. That is why it worked and is no fix. It also panics; never touch it.

### 2.2 DisplayPort link training fails (-71)

- The same VIA hub, same pin D (Attention 0x9a), trained at `rate=2 lanes=2` with a 1080p picture while
  charging in test 37, once dwc3 was forced into host mode. Before that, in device mode, it failed with
  -71 after `PHY BUSY timed out` (`usb_c_dock_plan.md` section 2). CONFIRMED.
- The DP-only adapter (Cube is source, so the host starts at once: `dwc3_sprd_otg_start_host: turn on host` at
  70.87 s) trains every time (`dmesg-run2-ring.txt` lines 403, 2917, 4858). CONFIRMED.
- In test 41, dwc3 went `idle` at 173.43 (`Cable disconnected`) and never left that state. In idle the
  controller and its PHYs are runtime-suspended.
- **Why the USB side matters to DP (INFERRED from code):** `phy-sprd-usb31-qogirn6pro.ko` `sprd_ssphy_init`
  (`.text+0xd14`) enables the PHY's `vdd` regulator, turns the SS PHY on through `sprd_usbm_ssphy_set_onoff`,
  and programs the `sprd,syscon-ipa-usb31-dp` / `-usb31-dptx` / `aon-apb` registers. It also waits for the
  Type-C Assist block ("`%s tca interrupt not ready`"). sprd-drm imports nothing from the PHY driver: its only
  imports in this area are the three `ptn38003a_*` calls. It programs the DP side of the combo PHY directly and
  assumes the USB side has powered it. `dptx_phy_wait_busy` (`sprd-drm.ko` `.text+0x32e28`) polls bits
  0x1000/0x3000/0xf000 of the PHY interface register at +0xa00 and never sees them clear while the PHY is off.
- Pin assignment is not the problem: pin D worked in test 37 and is the right choice for a hub (2 DP lanes plus
  USB3). The Cube's DP controller does 2 lanes either way.

**Ordering risk after the fix.** With the fix, the host starts at the swap (176.1 s in this run). The
Attention with HPD came 1.9 s later, and the DP-only adapter needed about 350 ms from id event to host start.
So the order should hold for this hub (INFERRED). A dock that raises HPD faster than the host comes up would
still fail. Test 40's DP-B behaviour then leaves DP "on" and ignores later HPD-high
(`usb_c_dock_plan.md` 4b DP-D). That is why a retrain-on-HPD fix is in the recommendation (4.D).

### 2.3 `ptn38003a hadn't been probe.`

- **Device tree:** `/soc/ap-ahb/i2c@22a0000/btn38003a_redriver@30` (note the "btn" typo in the node name),
  `compatible = "sprd,ptn38003a redriver"`, `reg = <0x30>`, no supplies, GPIOs, or enable/reset pins. The device
  `6-0030` exists on I2C bus 6, the bus shared with the AW32257 charger (0x6a) and the speaker amp (0x58). No
  driver is bound. CONFIRMED on the device.
- **The driver matches.** The module's of_match string is exactly `"sprd,ptn38003a redriver"` (with the space).
  `ptn38003a_i2c_probe` (`.text+0x624`) allocates, creates the regmap, then does `regmap_read(reg 0)`
  (`.text+0x744`). On error it logs `detect ptn38003a failed.`, frees its state and leaves the global
  `ptn38003a_redriver` NULL (`.text+0x74c-0x76c`). Any value read would have passed: only an I2C failure fails
  the probe.
- **The log:** `ptn38003a 6-0030: detect ptn38003a failed.` / `probe of 6-0030 failed with error -5` at boot
  (`klog-1.txt` line 3109). -EIO means no ACK at 0x30. The chip is either not fitted on the Cube or unpowered.
  The node is Unisoc reference-board DT that Anbernic didn't remove. The board's real fit is UNKNOWN without a
  teardown, but nothing in the DT powers one.
- **Callers:** `sprd-drm` (`ptn38003a_dp_mode_set` `.text+0x34b2c`, `_dp_link_rate_set` `0x34b6c`/`0x375e8`,
  `_dp_lane_set` `0x34bf4`/`0x375f0`), `phy-sprd-usb31-qogirn6pro` (`ptn38003a_mode_usb32_set`, `.text+0xd54` in
  `sprd_ssphy_init`, and `0x1498`), and `dwc3-sprd` (`limit_dwc3_max_speed`, `.text+0x2be4`). With the global
  NULL each returns -ENODEV after the printk. The DP and PHY callers ignore the result. dwc3's
  `limit_dwc3_max_speed` treats nonzero as "redriver needs USB 3.0" and caps **gadget** speed at 5 Gb/s
  (`limit dwc3 max speed to usb30` in both logs). This has no effect in host mode.
- **Not a cause:** the DP-only run has 16 of these messages and three successful trainings. CONFIRMED.
- **Lane mux and orientation without it:** the SoC's combo PHY drives the connector directly. Its TCA block
  muxes USB3/DP lanes. sprd_typec_displayport picks the pin assignment (C or D) and sprd-drm sets 2 lanes. How
  plug orientation reaches the TCA is UNKNOWN: `sprd_ssphy_typec_notifier` only switches D+/D-. No flipped-plug
  dock test exists; it is in the test plan.

### 2.4 Wi-Fi crash

**What links it to DP training (CONFIRMED on the device, 2026-11-25):**

| Thread / IRQ | Policy | CPU |
|---|---|---|
| `irq/76-dw_dptx` (DP IRQ thread, runs link training) | SCHED_FIFO 50 | pinned to CPU0 (`/proc/irq/76/smp_affinity_list` = 0) |
| `irq/24-mmc2` (SDIO host of the Marlin3 Wi-Fi chip) | SCHED_FIFO 50 | pinned to CPU0 (`/proc/irq/24` = 0) |
| `sdiohal_tx_thre` / `sdiohal_rx_thre` | FIFO 94 / 95 | 0-7 |

All IRQs on this kernel sit on CPU0. `dptx_phy_wait_busy` busy-polls with `__const_udelay` up to 100001 times
(`.text+0x32ec8`: `mov w21,#0x86a1; movk w21,#0x1,lsl #16`), without sleeping. Each timeout is about 0.47 s of
CPU0 at FIFO 50.
- A **failed** training here ran 14 timeouts back to back (178.57-185.35). `sched: RT throttling activated` at
  179.15. Test 37's klog prints `c0 [C0] sched: RT throttling activated`, which names CPU0.
- Same-priority FIFO threads on one CPU don't preempt each other, and RT throttling only frees time for CFS
  tasks. So `irq/24-mmc2` gets no CPU time until the DP thread blocks.
- The SDIO tx thread entered `xmit_lock` in `sdio_writebyte` at 178.3415 and only ran again at 185.506. That
  is 15 ms after `Link training failed`, the moment the DP thread stopped spinning.
- Meanwhile the Wi-Fi firmware timed out a command (`CMD_LLSTAT … CMD_RSP_TIMEOUT_ERROR`, 183.52) and the WCN
  driver did its own deliberate reset (`wcn_reset_process`, `marlin chip en pull down`). The reset is the
  driver's recovery, not a brown-out.
- **Successful trainings** (DP-only adapter) take 3 timeouts, about 1.4 s, and also trigger RT throttling
  (`dmesg-run2-ring.txt` line 374). Wi-Fi survives them (0 asserts). Test 37's two failed trainings each
  produced a firmware assert (4 assert lines in `klog-1.txt`).

**INFERRED:** the starved piece is `irq/24-mmc2`, i.e. sdhci request completion runs in its IRQ thread (5.4
sdhci finishes requests in the threaded handler since the finish tasklet was removed). The test in 6.2 checks
this directly.

**Ruled out or unlikely:**
- Shared power rails or a brown-out: the chip power-down is logged as the driver's own reset sequence, and
  nothing in the log shows a regulator event.
- PCIe: Wi-Fi is SDIO (`mmc2`, `22210000.sdio`); `pcie_sprd` only appears in the module list.
- RF from USB3: USB3 never came up in this run.

**Wi-Fi never recovered:** after the reset, `sdiohal_card_lock failed` and endless `CMD5`/`CMD52` errors (card
re-init fails), and `sprd_uninit_fw failed`. That is a separate weakness in the WCN driver's recovery path.
Removing the starvation avoids triggering it; fixing it is out of scope.

### 2.5 Charging warnings

`failed to enable primary charger, ret = -22`, `set wl type to primary charge fail`, `Fail to set present`,
`failed to aw32257 set power_supply_property[37]` and `cant not get input_cur.` start at boot (17.6 s in this
log, before any dock) and appear in non-dock logs (`dmesg-1440only.txt`, `dmesg-try1440.txt`, `klog-1.txt`).
They are charger-manager asking the AW32257 driver for properties it doesn't implement (no input-current limit,
`usb_c_dock_plan.md` R11). Not dock related. Charging worked: target 1.05 A for the UNKNOWN BC1.2 type, about
+750 mA measured. Leave as is.

### 2.6 Unplugging crashes the Cube

Panic in `dptx_irq` / `dptx_threaded_irq` / `dpu_clk_disable`. The main session's `sprd_drm.py` workflow owns
it. Until that lands, every dock test ends with **Power off from the power menu before unplugging**.

---

## 3. The questions, answered

| Question | Answer |
|---|---|
| Does the Cube have a PTN38003A? | A DT node yes (`btn38003a_redriver@30` on `i2c@22a0000` = bus 6, compatible `"sprd,ptn38003a redriver"`); a chip that answers, no. Probe fails with -EIO at boot. The module loads and the other modules link to it; its calls return -ENODEV. Not fitted or unpowered (UNKNOWN which), and harmless (2.3) |
| Why isn't it probed? | The compatible matches and there are no supply or GPIO dependencies. The I2C read of register 0 isn't ACKed |
| How are pin C/D and orientation set? | Pin: `sprd_typec_displayport` (`usb_c_dock_plan.md` R10; this hub offers C+D with the multi-function bit, so D). Lanes: sprd-drm sets 2. Mux: the combo PHY's TCA, set up by `sprd_ssphy_init` (needs the USB side up). Orientation: UNKNOWN; test the flipped plug |
| Which driver calls `ptn38003a_*`, and what if it's absent? | sprd-drm (mode, link rate, lane count), phy-sprd-usb31 (`mode_usb32_set` in `sprd_ssphy_init`), dwc3-sprd (`limit_dwc3_max_speed`, gadget only). All get -ENODEV and carry on |
| Stock GammaOS 1.2 / Anbernic firmware with docks? | Same vendor modules and the stock dtbo (no `data-role`, 400 mA source PDO, no c2c). Test 37 on the stock DT: the hub without a charger got DP entry but no HPD (underpowered); with the charger, the Cube sank and refused every DR_Swap. Charge plus picture plus USB never worked there. Anbernic's support page says the RG556 can't output video while charging, "not a hardware defect". No stock configuration ever ran the c2c host path |
| Why doesn't the USB 3 host start after DR_Swap? | 2.1: VBUS from `extcon_gpio` plus the id notifier's charger-state reset leave `evt_prepare_work` stuck at "vbus charger detect not finished". c2c needs the VBUS extcon on `pmic_typec` |
| What links the Wi-Fi crash to DP training? | 2.4: the DP IRQ thread busy-waits at FIFO 50 on CPU0 for about 7 s during a failed training. The SDIO host's IRQ thread is also FIFO 50 on CPU0 and starves, so the firmware times out a command and resets |

---

## 4. Fix options

| # | Fix | Fixes | Effort | Risk | Confidence |
|---|---|---|---|---|---|
| **A** | dtbo entry 2: usb3 VBUS extcon → `pmic_typec` | host start, so DP and USB3 too | small (one `__fixups__` edit in the patcher, new `tested_sha256`) | medium-low | high for host start; medium-high for DP |
| A+ | A, plus Unisoc's full c2c wiring (`usb`, `hsphy`, `ssphy` → `pmic_typec`) | same, plus BC1.2 keyed to typec | small | medium (charger detection and charger-manager paths change) | medium |
| B | dwc3-sprd one-word patch: id notifier also clears `vbus_active` | host start | small (but new module-patch plumbing for dwc3-sprd) | medium | medium-high |
| **C** | Move the `dw_dptx` IRQ off CPU0 at boot (`/proc/irq/N/smp_affinity_list`) | Wi-Fi starvation during any training | small, userspace | low | medium (mechanism INFERRED) |
| **D** | sprd-drm: bound the PHY-busy poll; retrain on a later HPD-high (DP-D) | CPU hog at the source; recovery when HPD beats host start | medium (main session's file) | medium | medium |
| E | Force pin C or a DP-only configuration | nothing here | small | loses USB3 | rejected |
| F | Disable the PTN38003A node | cosmetic | small | none | not needed |

### A. Rewire the usb3 VBUS extcon (recommended)

**Change**, in dtbo entry 2 only, the same image `scripts/patch_dtbo.py` already patches:
- In `/__fixups__/extcon_gpio`, remove the string `/fragment@8/__overlay__:extcon:0`.
- Append `/fragment@8/__overlay__:extcon:0` to `/__fixups__/pmic_typec`.
- `fragment@8/__overlay__/extcon` stays `<0xffffffff 0xffffffff>`; both cells now resolve to `pmic_typec`.
- No new node or property, and no phandle number is hard-coded. The patcher locates the entries by string.

**Expected result after merge:** `usb3@25100000 { extcon = <0x64 0x64>; }`. Check it offline with `fdtoverlay`
against vendor_boot's dtb, as `patch_dtbo.py` already does.

**Why it works:** section 2.1. This is Unisoc's production c2c wiring for the same driver (Nubia c2c overlays).
The swap then sends USB=0, `vbus_active` goes to 0, and USB_HOST=1 starts the host about 0.35 s later, before
this hub's Attention.

**Side effects to test** (INFERRED unless marked):
- **Plain charger or PC:** the typec extcon reports USB=1 on any sink attach (CONFIRMED on the device today:
  `sc27xx-typec USB=1` while sinking, same as `extcon-gpio`). dwc3 runs BC1.2 on that edge as before. PC: SDP,
  so the gadget starts and adb works. Wall charger: charging mode as before.
- **PD hard reset:** the typec extcon doesn't change across a hard reset, so dwc3 no longer sees the VBUS dip
  and doesn't re-run BC1.2. That is better, and it removes the 3.9 s detection seen here.
- **Swap during BC1.2:** the USB=0 path calls `flush_delayed_work(chg_detect_work)` (`.text+0x2330`). If
  detection is still running, the typec DR-swap work waits for it (0.4 s on the first attach in this run).
  That delays host start, it doesn't break it.
- **Cube as source** (dongle, bus-powered hub, OTG): typec USB stays 0 and USB_HOST=1 drives the host as today.
  No change.
- **Dock charger pulled while docked:** UNKNOWN. Test it (matrix row 5 of the earlier plan).
- **Boot with the dock attached:** `dwc3_sprd_detect_cable` reads the initial state from the typec extcon
  instead of the GPIO. Test it.
- **Recovery** uses the same dtbo: test adb in recovery over a PC cable.
- `usb@a00000` (musb) keeps `extcon_gpio`. musb logs `ignored in slave mode` for both cables on this board.
  `hsphy`, `ssphy` and `charger@6a` keep `extcon_gpio` for BC1.2 and charger presence. That is option A+'s
  difference.

**Risk:** a malformed overlay is the same class of risk as test 40 (no boot or a broken board config), with
the same recovery. The edit is smaller than test 40's: only two fixup strings change.

### A+. Unisoc's full c2c wiring

Also move `fragment@6` (`usb`), `fragment@5` (`hsphy`) and `fragment@7` (`ssphy`) `extcon:0` entries to
`pmic_typec`, as in Nubia `ov-camellia.dts`. Only if A leaves BC1.2 or USB2 problems (the R5 symptom: USB2
`device descriptor read/64 error -71` right after the swap). Then the PHYs' charger detection follows typec
too. It changes charger-manager inputs (`usb_charger_absent` on the swap's USB=0). That is Unisoc's intended
path (charger-manager logs `is_pd_hub = 1`), but it has not run on the Cube.

### B. dwc3-sprd binary patch (fallback if A fails)

One word in `dwc3_sprd_id_notifier`: `.text+0x2490` (file 0x4490), `0x3901667f` `strb wzr,[x19,#89]` →
`0x7900b27f` `strh wzr,[x19,#88]`. It clears `vbus_active` (+0x100) together with `charging_mode` (+0x101) on
every id change, which reproduces the effect of the typec USB=0.
- Original word CONFIRMED in the file (module sha256 `1e0cb758…`). +0x100/+0x101 are both byte fields and
  +0x100 is 2-byte aligned. CONFIRMED from the field accesses.
- Risk: after a host→device swap with VBUS still present, `vbus_active` is 0 and the GPIO won't re-announce it,
  so there's no gadget until a re-plug. It also needs dwc3-sprd patch plumbing like `sprd_drm.py` (module sha
  check, original-word check, vendor_dlkm repack) and a Cube-only table entry (see the portability rules in
  `usb_c_dock_plan.md` section 7).
- Prefer A. B is for the case where the extcon rewiring causes trouble elsewhere.

### C. Move the DP IRQ off CPU0 (recommended)

At boot, find the IRQ by name and pin it elsewhere. The IRQ number (76 today) isn't guaranteed stable, so look
it up:

```sh
irq=$(awk -F: '/dw_dptx/ {gsub(/ /,"",$1); print $1}' /proc/interrupts)
[ -n "$irq" ] && echo 3 > /proc/irq/$irq/smp_affinity_list
```

- CPU 3 has no pinned IRQ threads (every IRQ is on CPU0 today); any CPU other than 0 works. The IRQ thread
  follows the new affinity the next time it runs. Verify with `grep Cpus_allowed_list
  /proc/<pid of irq/NN-dw_dptx>/status`.
- Where: a vendor `init` `.rc` action (for example `on property:sys.boot_completed=1` running a small script),
  or Nano on the 1.4.4 base. It needs root/`init` context; SELinux for `/proc/irq` writes from a vendor script
  is UNKNOWN. Check it in the test build, permissive logs first.
- Risk: low and reversible (reboot clears it). A failed training still burns about 7 s of CPU3, but only CFS
  tasks there are affected, and those migrate.
- Alternative: move `mmc2` instead. Moving the offender is better, because then nothing else that is pinned to
  CPU0 starves either.

### D. sprd-drm changes (for the main session's driver workflow; not edited here)

1. **Bound the busy-wait.** `dptx_phy_wait_busy` `.text+0x32ec8`: the loop count 0x186a1 (100001 ×
   `udelay(1)`). Every training in every log hits at least 3 full timeouts, even the successful ones, so the
   BUSY bit seems never to clear and the wait acts as a fixed 0.47 s CPU-burning delay. Cutting the count
   (for example to 0x3e9, about 1 ms) would make failure cheap. It is UNKNOWN whether the PHY relies on the
   delay; measure on a DP-only adapter first. A sleeping wait would be better still but needs code space.
2. **DP-D: retrain on a later HPD-high when the link is down** (`usb_c_dock_plan.md` 4b/4c). This covers a dock
   whose HPD arrives before the host is up, and recovery after any failed training.

### E. Forcing pin C or DP-only: rejected

The failure isn't lane layout. Pin D trained in test 37 once the host was up, and test 41 failed before any
lane question arose (the PHY was unpowered). Pin C would cost USB3 on the dock and fix nothing. A "DP-only"
mode that skips USB entirely would still need the combo PHY powered by the USB side.

### F. PTN38003A: nothing to fix

Disabling the node would only drop a probe attempt and a log line. The `ptn38003a hadn't been probe.` printk
comes from the exported functions and would stay. Leave it alone.

---

## 5. Recommended fix

1. **A** in the dtbo patcher (owner of `scripts/patch_dtbo.py` and `usb_c_dock_plan.md`). It extends test
   40's core set, so one atomic DT change keeps `patch_dtbo.py`'s all-or-nothing rule. Add the two fixup strings
   to the per-device table, re-derive `tested_sha256` after the hand test.
2. **C** as a boot-time action.
3. **D.2 (DP-D)** and **D.1** passed to the main session's sprd-drm workflow; not blocking A and C.
4. A+ and B held back, used only if the test plan shows A isn't enough.

Expected result with the VIA hub, charger first: charge at about +750 mA as now, DR_Swap → host at once, USB3
and USB2 buses appear (`usb2`/`usb3` in `/sys/bus/usb/devices`, hub 2109:0817 / 2109:2817), Attention 0x9a →
training succeeds first time (3 PHY waits) → 1080p on the TV, Wi-Fi stays up.

---

## 6. Device test plan

Rules: only with the owner's go-ahead in that session. **No partition or dtbo writes without explicit
approval.** Never read or write `/sys/kernel/debug/*dwc3*/mode`. End every dock test with **Power off from the
power menu, then unplug**.

### 6.1 Capturing logs when Wi-Fi dies

Use the on-device logger, `/data/local/tmp/dock/log.sh`. It moves itself and its logcat into the root cgroup so
it survives adbd restarts, sets printk to 8, drains `dmesg -c` into `dmesg.txt`, writes `state.txt` once per
second and syncs every second. Add these lines to the state block for these tests:

```sh
cat /sys/class/extcon/extcon0/cable.0/state /sys/class/extcon/extcon1/cable.0/state /sys/class/extcon/extcon1/cable.1/state
grep -E 'dw_dptx|mmc2' /proc/interrupts
```

1. Before plugging, start it detached: `adb shell 'nohup sh /data/local/tmp/dock/log.sh >/dev/null 2>&1 &'`.
   Check that `state.txt` grows.
2. Run the test. Expect adb over Wi-Fi to drop; that doesn't matter.
3. Power off from the menu, unplug, power on, then pull `/data/local/tmp/dock/*` over USB or Wi-Fi. After a
   panic, also pull `/sys/fs/pstore/*`.
4. Copy the results to `out/dp-evidence/<run-name>/` (gitignored).

### 6.2 Test 1: Wi-Fi isolation (runtime only, no dtbo change)

On the current test 41 install, with the logger running:
1. `echo 3 > /proc/irq/76/smp_affinity_list` (look the number up by name first), and check that the
   `irq/76-dw_dptx` thread's `Cpus_allowed_list` is 3.
2. Repeat the dock run (charger first, TV on HDMI). DP will still fail, since the host fix isn't in.
3. **Pass:** no `fw assert`, no `wcn_reset_process`, `ping` to the router keeps answering through the 7 s
   training, and `RT throttling` (if logged) is on CPU3.
4. Reboot to undo the change.

This proves or disproves the starvation mechanism (2.4) cheaply, before any build.

### 6.3 Test 2: the dtbo fix (A), needs approval to write dtbo

1. Build the dtbo with A (plus test 40's core set) on the PC. Merge it offline with `fdtoverlay` and diff
   against the live DT: the only difference should be `usb3@25100000/extcon` = `<0x64 0x64>`.
2. Back up both dtbo slots (`dd` to `/data/local/tmp/`, then pull to the PC). With approval, write only
   `dtbo_a`. Slot B and the backups stay untouched. Recovery: adb + `dd` of the backup if Android boots,
   otherwise a Full install.
3. Do C at runtime as in 6.2 (or include it in the build).
4. Matrix, logger running for each, Power off between dock cases:

| # | Case | Pass |
|---|---|---|
| 1 | Plain wall charger | charges; dwc3 `charge detect finished`; no gadget |
| 2 | PC USB cable | adb over USB works (SDP, gadget up) |
| 3 | DP-only adapter (Cube source) | trains first time; Wi-Fi up |
| 4 | HDMI dongle / bus-powered hub (Cube source) | picture; USB on the hub |
| 5 | **VIA dock, charger first, TV on HDMI** | the test 37 working state, reached through the DR_Swap alone: charging; after the swap `vbus:0 event received`, then `host:1`, then `dwc3_sprd_otg_start_host`; USB3 hub 2109:0817 **and** USB2 hub 2109:2817, billboard 2109:8817 and devices behind it, all first time with no second toggle; `Link training succeeded` with at most 3 PHY waits; picture; Wi-Fi up; a USB stick on the dock mounts |
| 6 | Same, plug flipped (CC2) | as 5 (orientation is UNKNOWN today) |
| 7 | Dock without charger, charger added later | record only (earlier plan matrix row 4) |
| 8 | Dock charger pulled while docked | record only; no back-feed (VBUS, charger IC status) |
| 9 | Reboot with dock attached | boots; same end state as 5 |
| 10 | Recovery mode, PC cable | adb works |

5. If 5 fails with USB2 `-71` enumeration errors after the swap, try A+. If the extcon rewiring breaks 1, 2 or
   9, go back to the stock-plus-test-40 dtbo and evaluate B.

### 6.4 What to look for in the logs

- `dwc3-sprd … vbus:0 event received` right after `extcon_drswap_work pd_swap_evt:4`, then `host:1`,
  `host_idle state`, `dwc3_sprd_otg_start_host: turn on host`, and xhci registering two buses.
- No `vbus charger detect not finished` after the swap.
- `Link training succeeded rate=2 lanes=2` with at most 3 `PHY BUSY timed out`.
- No `fw assert`, `wcn_reset_process` or `holds xmit_lock`.
- Battery current positive throughout (`state.txt`).

---

## 7. Open points

- Whether a PTN38003A is physically on the board (a teardown photo would settle it). It doesn't change the plan.
- Plug orientation handling with a dock (test 6).
- Whether sdhci completion on this vendor kernel runs in `irq/24-mmc2` (test 1 answers it in practice).
- The WCN driver's failed recovery after a reset: a separate bug, worth its own look if Wi-Fi resets happen
  for other reasons.
- The same extcon wiring question applies to the other T820 devices' c2c plans (`usb_c_dock_plan.md` section
  7): add "usb3 VBUS extcon source" to the per-device table.
