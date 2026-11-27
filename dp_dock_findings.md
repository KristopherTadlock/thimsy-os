# RG Cube DisplayPort, 1440p and USB-C dock findings (builds 36/37 and 41)

Scope: every finding and observation from three test campaigns on the Anbernic RG Cube:

- the **build 36/37 USB-C dock tests** (evening of 2026-11-23 local, 06:00-07:31 UTC on 2026-11-24);
- the **test 41 DisplayPort and 1440p tests** on an H27S17 monitor (2026-11-25, 00:3x-01:20 local);
- the **test 41 dock test** (2026-11-25, 01:24-01:37 local).

How to read this document:

- Every finding has an ID (S = setup, D = build 36/37 dock, M = test 41 modes, K = test 41 dock, P = crashes, R = root causes, C = charging, T = device state, Q = open questions).
- Every finding ends with a status tag. **[C]** confirmed: a saved log, file or disassembly shows it. **[I]** inferred: follows from the evidence but is not shown directly. **[O]** open: not recorded, unverifiable, or contradicted.
- Log quotes are exact, short, and give the file. Paths are relative to the repository root unless they are absolute; section 11 lists every file.
- Time bases. Build 36/37: kernel logs (`klog-1.txt`, `pdlog-1.txt`, `dock-watch.log` kernel lines) are UTC; the host-side watchers (`watch.log`, `typec-watch.log`, `dock-watch.log` record headers) are local PST, so UTC = local + 8 h. Test 41: dmesg is seconds of uptime; logcat, `state.txt` and the host logs are local PST. For the dock run, local time = dmesg seconds + 01:33:00.97. For the boot that ran tests 4-10, local time is about uptime + 3344 s after 00:00 (accurate to about 0.2 s).
- "The record" means the main session's own written record of the test 41 day; corrections to it are marked as such.
- Where the monitor showed a picture or not, that is the tester's observation. The kernel only shows a stable link or an HPD drop.

---

## 1. Summary

### What works

- **Bus-powered HDMI/DP dongles with the Cube as source** work on the original DT, on DT v3 (build 36/37 session) and on test 41: an HDMI-only dongle (057e:2003) trained "rate=2 lanes=2" (FINDINGS.md), and on test 41 the USB-C to HDMI adapter (Anker 310, 291a:8312; a DP-to-HDMI converter, S-25) trained 22 times at "rate=2 lanes=2" and its USB device enumerated. [C]
- **Build 36/37: charging + 1080p HDMI + USB3 at the same time** was recorded at 23:06:24 local (`dock-watch.log`), with DT fix v3 written by hand into dtbo_a and dwc3 forced to host through debugfs by root writes (the `hostfix.sh` loop was running, but its log has no write for that plug-in, D-38). Adding USB2 needed a second device-to-host toggle (`retoggle.sh`); that four-way state was reached but no single saved snapshot shows it. [C]
- **Test 41 H27S17 monitor: the only logged stable link is CEA 1080p60 at 148.5 MHz**, forced through a debugfs EDID override (TEST 7): full `dpu_clk_init` with "pixelclock is 148500000", no HPD drop for the remaining 33 s of the log. [C] The picture is the tester's observation. A 1080p120 (297 MHz) "try + keep" also gave a picture per the record, but that run is not in any log. [O]
- **Charging from a PD dock** works on the DT fix and on test 41: a 5 V 1 A contract, about +0.4 to +0.76 A into the battery. [C]

### What does not work

- **Test 41, stock driver: no picture on the H27S17.** The composer starts every connect at 3840x2160 in bypass at 297 MHz, then Android's Auto mode switches to the monitor's 138.5 MHz 1920x1080 about 3.2 s later through a path that never reprograms the DPU clock; the monitor drops HPD 8-26 s later and the cycle repeats every 20-38 s. [C]
- **2560x1440@60 at its native 241.5 MHz fails** even with a full enable and clock setup (1440-only driver), in Docked and in Mirror mode: HPD drops 3.8-7.8 s after the enable. [C]
- **2560x1440@60 forced to 297 MHz fails** with two different timings (TESTS 9 and 10). [C]
- **Test 41 dock: charging and the DR_Swap work, but DP link training fails with -71, USB host never starts, and Wi-Fi dies and does not recover.** [C]
- **Unplugging crashes the kernel.** Four panics on test 41 (A twice, B, C) and one in the build 36/37 session (a userspace read of dwc3 debugfs). A hard lock-up on the first plug-in of test 41 is reported but not logged. [C] / [O]

### Root causes, proven and inferred

- **The mode-switch path skips the DPU clock setup.** When the mode changes on a live DP connection, `sprd_dpu_mode_set_nofb` runs without `sprd_dpu_atomic_enable`/`dpu_clk_init`, so DPU1 keeps the previous pixel clock. [C] That this is why those switches lose the picture is [I].
- **241.5 MHz has no matching DPI clock source**: the driver logs "use default DPI_CLK_SRC" and uses 297 MHz. [C log] That this mismatch is why 1440p fails is [I].
- **The "panel blank kills PIXELPLL" theory is not the cause** (correction): Mirror mode and all EDID-override runs never touch PIXELPLL and still fail at 241.5 and 297 MHz. [I]
- **A pixel clock that divides 297 MHz is necessary in practice but not sufficient**: 148.5 MHz works; 2560x1440 at 297 MHz does not. [I]
- **Dock host mode**: on build 36/37, `sc27xx_typec` dropped the DR swap ("typec has disconnected!") because Unisoc's c2c mode was off. [C] On test 41 the c2c work runs and dwc3 receives the host event, but it arrives while BC1.2 detection is running ("vbus charger detect not finished") and host never starts. [C]
- **Dock DP training -71** happens whenever dwc3 is not in host mode (both builds); with the debugfs host override it trained at the first attempt. [C] That the combo PHY is held in device configuration is [I].
- **Wi-Fi death during dock training**: the SDIO TX thread stalls in the same window as the DP PHY busy-waits, which spin without sleeping on CPU0 where the SDIO IRQ thread is pinned. [I]
- **Unplug panics**: DP interrupt handlers touch DP registers after "dp suspend OK", and the CRTC disable touches a clock mux after the VPU power domain is off. [C traces] [I mechanism]
- **15 W charging is not reachable** on the AW32257 charger (4-6 V input, OVP 6.4 V). [I, from datasheet research]

---

## 2. Devices, builds and methods

### 2.1 Builds

| Build | Base | Dock/DP content | Where it ran | Evidence |
|---|---|---|---|---|
| test 36 (`d8578f2`, 2026-11-21 13:48 PST) | LineageOS 21 / GammaOS 1.4.4 framework over the 1.2 vendor image | Dual Screen focus, Mirror audio, plug-in during a game, ABXY Swap. No dock change. | Most likely the system image on the Cube during the "test 37" dock session (S-03) | `gh release view v1.0a-test36` |
| test 37 (`8c1fe5c`, 2026-11-22 17:34 PST; release created 2026-11-23T01:36:01Z) | same | Controller layout, folder removal. No dock change. | Labelled in FINDINGS.md as the build under test | `gh release view v1.0a-test37` |
| test 40 (`v1.0-test40`, draft created 2026-11-24T20:04:40Z) | **Thimsy 1.0 on the GammaOS Next 1.2 base** (`RG_Cube_GammaOS_Next_Full_v1.2`), `nano-144` at `aa9b131`, built in Docker; not the 1.4.4 system (correction: earlier drafts of this table said "same") | Ships the c2c dtbo (sha256 `94c0f3d4...`); its warning says the dtbo change "has never run on a Cube". Notes: "Wi-Fi dropouts seen while testing docks on test 37 aren't investigated yet." | Possibly installed at 15:10 local on 2026-11-24 (S-43); whether it booted is not recorded | `gh release view v1.0-test40`, installer backup folder |
| test 41 (`v1.0a-test41`, draft prerelease created 2026-11-25T07:14:08Z) | `nano-144` at `35f6c9b`, 1.4.4 system with the RG Cube's 1.2 vendor, kernel and bootloader; first packaged with installer test 21 (universal-installer `505e36a`). The release page now names installer test 22 (`8bc13c4`, "it replaces the test 21 installers first attached here", and writes a log for every install); the job on record used catalog `0.1.0-test21` (S-05), which fits the missing install log in S-08 | Same c2c dtbo (`94c0f3d4...`); release notes say the dtbo change "has not yet run on a Cube". `sprd_drm` patched with all modes enabled. | TESTS 1-3, PANIC A, PANIC B | job.json, release notes |
| test 41 + "1440-only" `sprd-drm.ko` (local) | test 41 with one patched word restored | Only 2560x1440@60 and 640x480@60 offered | TESTS 4-10, PANIC A', PANIC C, the dock test | `out/dp-evidence/` |

### 2.2 Device and software

**S-01 Device under test.** One Anbernic RG Cube: Unisoc T820 (UMS9620-AB), board `ums9620_2h10`, dtbo index 2, kernel 5.4.233 built 28 Feb 2025, Android 14 / LineageOS 21 with the GammaOS 1.4.4 framework over the 1.2 vendor image, bootloader unlocked (orange), userdebug. Evidence: `uname.txt` "Linux localhost 5.4.233 #1 SMP PREEMPT Fri Feb 28 22:44:43 CST 2025 aarch64"; `cmdline.txt` "androidboot.hardware=ums9620_2h10", "androidboot.dtbo_idx=2", "androidboot.auto.chipid=UMS9620-AB", "androidboot.verifiedbootstate=orange"; `getprop.txt` "[ro.gammaos.build.version]: [1.4.4]", "[ro.vendor.build.date]: [Sat Mar  1 00:10:02 CST 2025]" (all in `out/.dock/`). [C]

**S-02 Debug access in both campaigns.** adb root over Wi-Fi on TCP 5555. Evidence: `out/.dock/getprop.txt` "[persist.adb.tcp.port]: [5555]"; test 41 `dock-tv-run/logcat.txt` "11-25 01:33:06.170  1382  1382 I adbd    : adbd listening on tcp:5555". The address appears in `cube-display/watch.log` ("device '<cube-ip>:5555' not found"). [C]

**S-03 Which build the "test 37" dock session really ran.** FINDINGS.md labels it "(2026-11-24, Thimsy 1.0a test 37 on the device)", but the system image was built before the test 37 commit: `getprop.txt` "[ro.build.date]: [Sat Nov 21 13:06:02 PST 2026]", "[ro.modversion]: [21.0-20261121-UNOFFICIAL-arm64_bgN]", "[ro.lineage.version]: [Thimsy_OS_v1.0a]"; git log `d8578f2 2026-11-21 13:48:15 -0800 ... (test 36)`, `8c1fe5c 2026-11-22 17:34:21 -0800 ... (test 37)`. The device most likely ran the test 36 system image or a build from the test 36 tree. This document calls that session "build 36/37". [O]

**S-04 Neither test 36 nor test 37 shipped a dock fix.** Their release notes have no USB-C, dock, charging-while-docked or dtbo item; every dock result in section 3 comes from live hand overrides on the device. [C]

**S-05 Test 41 install job.** Installer job folder `jobs\anbernic-rg-cube-heavy` (ThimsyInstaller 0.1.0, catalog test 21, profile heavy, no overrides) assembled a 7,722,216,910-byte PAC with sha256 `c7625fec3da0204c28783ec91159a521a5ed09b40bec7a1ab34779e8cd7ba5bc`, "matches_reference": true, "release": "Thimsy 1.0a test 41"; DTBO/DTBO_B sha256 `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536`. The release notes give the same two hashes. [C]

**S-06 Test 41 install timeline.** The package tar (2,084,008,448 bytes) arrived at 23:41:41 local on 2026-11-24; the PAC was rebuilt by 23:44:11; job.json, plan.json and state.json were written at 23:44:29. The GammaOS Next 1.2 download was reused from the cache (5 parts, 15:03-15:04 that day). Flashing therefore happened between 23:47 and about 23:59 local (07:47-07:59 UTC on 2026-11-25). [C]

**S-07 Install plan.** Automated Windows Full install of the heavy profile: "wipes_data": true, erases userdata and metadata, flashes 59 partitions including splloader/splloader_bak, uboot, trustos, vbmeta*, dtbo_a/b, vendor_boot_a/b and super. An Update would write only logo, fbootlogo, vendor_boot_a/b, dtbo_a/b and super (`plan.json`). [C]

**S-08 First test 41 install attempt.** It reached download mode, read the partition table (device-partitions.xml 23:47:45) and backed up prodnv and miscdata (23:47:56). It then stopped some time before 23:52:16, when the second run began. It left no installer log, no `flash\` folder and no `flash\prepared.json`; the UART log has only a session banner "[2026-11-24 23:47:41:604] ===...spd_dump.exe" and no error. The record's "failed partway through flashing" cannot be checked: nothing shows whether any partition was written. [O]

**S-09 Second test 41 install attempt.** Started at 23:52:16 local, but spd_dump was launched only at 23:56:30 ("[2026-11-24 23:56:30:227] ===...spd_dump.exe"), 4 min 14 s later, which fits waiting for the Cube to re-enter download mode [I]. Backup at 23:56:45; the job folder was last modified at 23:59; no error lines. `state.json` reads {"extracted": false, "assembling": false}. [C]

**S-10 Device data changed between the two attempts.** prodnv differs by 294 bytes and miscdata by 4 bytes (offsets 10500-10503) between the 23:47:56 and 23:56:45 backups; prodnv also differs by 4491 bytes from the 15:12 backup. Neither partition is in the flash plan, so something else wrote them, most likely the device booting between attempts or the interrupted flash session. [I]

**S-11 "Booted only by Full install after a forced restart".** No file records the forced restart or the first boot after flashing. The earliest test 41 boot on record is "reboot,1795595539" (00:32:19 local) in `pstore-crash4.txt`. [O]

**S-12 The test 41 dtbo boots on a Cube.** Trusty rejects its key but the unlocked state lets it boot on slot _a: `dock-tv-run/dmesg.txt` "[    3.586245] trusty: ... avb_slot_verify result is ERROR_PUBLIC_KEY_REJECTED." then "unlock status is matched, return unlock.", "androidboot.slot_suffix=_a". This answers the release's warning that the dtbo had not yet run on a Cube, and with K-02 (charging from a PC port) it answers report item 2 ("It boots, charges from a plain USB-C charger"). The third part of item 2, "Quick Menu > Device shows test 41 and `35f6c9b`", was not checked in any saved log. [C] [O Quick Menu]

**S-13 Test 41 runs the c2c paths.** The test 41 logs show the Unisoc "PD hub c2c" functions: `dmesg-run2-ring.txt` "[   70.848232] sc27xx-typec sc27xx-typec: sc27xx_connect_set_status_use_pdhubc2c source connect!" and, at the dock DR swap, `dock-tv-run/dmesg.txt` "[  176.130114] sc27xx-typec sc27xx-typec: sc27xx_typec_extcon_drswap_work pd_swap_evt:4!". The PD driver itself still logs "[    2.623340] sc27xx-typec-pd sc27xx-pd: use_pdhub_c2c = 0" (the test 40/41 dtbo adds the flag to typec@380, usb@a00000 and usb3@25100000, not to pd@e00). The "sc27xx-pd supply vbus not found, using dummy regulator" warning seen on build 36/37 is absent from the test 41 boot log, which fits the added vbus-supply on pd@e00. [C lines] [I meaning]

**S-14 Test 41 still powers bus-powered adapters (Cube as source).** With the USB-C to HDMI adapter (S-25), test 41 runs "dwc3_sprd_otg_start_host: turn on host" (70.865306) and "vbus := On" (70.893725), registers xHCI buses 2 and 3, and enumerates the adapter's USB device (`dmesg-run2-ring.txt`). This partly answers plan question Q1 (does c2c with vbus-supply still power dongles): yes for this adapter. [C]

### 2.3 Hardware

**S-15 Build 36/37 test hub.** VIA Labs USB-C hub, PD identity 2109:0102, product type HUB/AMA, DP alt mode SVID 0xff01 with caps VDO 0x00000c05 (UFP_D, pins C and D). Behind it: VIA USB2 hub 2109:2817, VIA USB3 hub 2109:0817, VIA billboard 2109:8817; HDMI out; USB-C PD power-in pass-through. Evidence: `pdlog-1.txt` "11-24 06:28:08.119 ... PD RX, data[1]=0x6c002109", "Identity: 2109:0102.0001", "06:28:08.125 ...  Alternate mode 0: SVID 0xff01, VDO 1: 0x00000c05"; `cube-display/typec-kernel2.txt` "06:02:09.794 ... usb 2-1 : Product: USB2.0 Hub", "06:02:10.062 ... usb 3-1 : New USB device found, idVendor=2109, idProduct=0817", "06:02:14.940 ... usb 2-1.5: Product: USB Billboard Device". [C]

**S-16 Hub controller chip.** Probably a VIA VL103 ("DP Alt-mode & PD 3.0 Controller", datasheet in `out/.dock/work/dock-matrix/vl103/vl103.txt`); the hub was never opened. [I]

**S-17 Hub Source_Capabilities with its charger.** Three fixed PDOs: 5 V 1000 mA (0x2e019064, flags [RUDE]: dual-role power, unconstrained, USB comms, dual-role data), 9 V 1660 mA, 12 V 1250 mA. `dock-watch.log` "11-24 07:06:24.223 ... PDO 0: type 0, 5000 mV, 1000 mA [RUDE]", "PDO 1: type 0, 9000 mV, 1660 mA []", "PDO 2: type 0, 12000 mV, 1250 mA []". The hub's charger was never identified. [C]

**S-18 USB devices behind the hub.** Logitech USB Receiver 046d:c53f (G305 mouse) and a "2.4G Wireless Receiver" reporting Apple's vendor ID 05ac:024f; both full-speed on the USB2 hub. The billboard enumerated about 5 s after the USB2 hub. `typec-kernel2.txt` "06:02:10.663 ... usb 2-1.1: new full-speed USB device number 3", "06:02:11.131 ... input   : Logitech G305", "06:02:14.839 ... usb 2-1.5: new high-speed USB device number 5". [C]

**S-19 Build 36/37 display.** A 4K TV behind the hub's HDMI that lists 4096x2160. The kernel marked CEA 1080p60 (148.5 MHz) 0x48 (preferred), 640x480 0x60, all else 0x40, so Android saw about three modes: `dock-watch.log` "DP-1=connected modes=1920x1080 1920x1080 4096x2160", "\"1920x1080\": 60 148500 1920 2008 2052 2200 1080 1084 1089 1125 0x48 0x5". [C]

**S-20 Reference HDMI-only dongle.** PD identity 057e:2003, DP caps 0x405 (pin C only), status 0xa. With the Cube as source it requested 5 V 900 mA, was configured for pin C, sent Attentions 0x8a then 0x18a, logged "PHY BUSY timed out" three times and then "Link training succeeded rate=2 lanes=2" (FINDINGS.md; no raw log kept). [C, note only]

**S-21 Earlier HDMI+USB dongle on test 36.** A different HDMI+USB dongle without power-in worked on test 36 with the Cube as source (FINDINGS.md); model and logs not recorded. [C, note only]

**S-22 Test 41 monitor: H27S17.** EDID 1.3, manufacturer bytes 4d 67 = PNP ID "SKG", product 0x2748, serial field 1, week 14 of 2025, 60x33 cm, name "H27S17", serial descriptor thirteen "0" characters, one CEA extension, both checksums valid (`edid-H27S17-first256.hex`). [C]

**S-23 H27S17 preferred mode and limits.** Preferred DTD 2560x1440 at 241.50 MHz, total 2720x1481, 59.951 Hz, 88.79 kHz (CVT-RB porches). Range descriptor: 48-144 Hz, 30-240 kHz, 600 MHz max. Feature byte 0xaf sets continuous frequency, which on EDID 1.3 lets DRM add inferred DMT/CVT modes. [C]

**S-24 H27S17 CEA block (correction to the record).** VICs 3, 19, 4, 41, 49, 4, 4, 31, 4, 64, 63, 93, 95, 97. The 3840x2160 VICs are 93 (24 Hz), 95 (30 Hz) and 97 (60 Hz, 594 MHz); there is **no VIC 94 (25 Hz)**, so the record's "3840x2160@24/25/30" is wrong. VIC 16 (1080p60 148.5 MHz) is absent; VIC 31 (1080p50), 63 (1080p120, 297 MHz) and 64 (1080p100) are present. The 296.703 MHz 1080p120 is DRM's 1000/1001 variant, not an EDID entry. CEA DTDs: 1920x1080 at 138.50 MHz (2080x1111, 59.934 Hz), 2560x1440 at 497.75 MHz (120 Hz), 2560x1440 at 583.61 MHz (144 Hz). Also: HDMI VSDB, HF-VSDB (600 MHz max TMDS), AMD vendor block (60/144), HDR static metadata, colorimetry and a YCbCr 4:2:0 capability map. [C]

**S-25 Test 41 USB-C to HDMI adapter (earlier notes called it a DP adapter).** It is the Anker 310 USB-C to HDMI adapter (model A8312), a DP-to-HDMI converter: the same 291a:8312 device and PD identity 057e:2003 (caps 0x405, pin C only) appear in the HDMI-only TV runs, so every H27S17 picture and EDID so far came through its HDMI input. Bus-powered; the Cube sources it ("pr_mode:4"). Its USB side enumerates as an Anker device: `dmesg-run2-ring.txt` "[   71.600018] usb 2-1: new low-speed USB device number 2 using xhci-hcd", "[   71.965833] usb 2-1: New USB device found, idVendor=291a, idProduct=8312", "[   72.054485] usb 2-1: Product: Anker USB-C Hub Device". Its DP status VDOs are 0x8a (HPD high), 0x18a (IRQ_HPD) and 0x0a (HPD low); unlike the hub it does not set multi-function preferred. Every successful training was "rate=2 lanes=2" (22 times). [C]

**S-26 Test 41 dock.** Not identified in the logs; charger plugged into it first, TV on its HDMI, a USB device on it (record). Its DP Status VDO 0x9a is the same value the VIA hub sent, so it may be the same hub. [I]

### 2.4 How logs were captured

**S-27 Build 36/37 capture.** Host-side PowerShell watchers polled over adb and logged local time: `watch.ps1` (display size/density, `watch.log`), `typec-watch.ps1` (roles, partner, DP active, battery, `typec-watch.log`), `dock-watch.ps1` (per-plug records with grepped PD/DP kernel lines, lsusb, extcon, Wi-Fi, hostfix tail, `dock-watch.log`), `hpd2.ps1`. On-device scripts in `cube-display\` did the overrides (section 3.6). Kernel logs `klog-1.txt` and `pdlog-1.txt` were pulled; the panic was read from pstore (`panic-dmesg.txt`). `dump.sh` backed up dtb/dtbo/vbmeta before any change. [C]

**S-28 Test 41 capture.** Wireless adb at `<cube-ip>:5555`, printk raised to 8, dmesg/logcat pulled after each test, pstore read after each panic, debugfs mounted by hand for EDID overrides, `cmd display gammaos-monitor-mode try ...` for mode trials, and an on-device logger for the dock (record). The logger (`log.sh`, pid 2510) wrote `state.txt` about once a second (time, DP status, blank field, data role, power role, battery status, current_now in µA, USB buses) from 01:34:07 to 01:37:15, plus dmesg and logcat streams: `dock-tv-run/logcat.txt` "11-25 01:34:26.146  1454  1518 I am_cpu  : [2510,0,log.sh,0,0,0]". [C] Its path `/data/local/tmp/dock/log.sh` and the move to the root cgroup come from the record only. [O]

### 2.5 Pitfalls found

**S-29 adbd restarts at every USB role change.** Build 36/37: after each swap init killed and restarted adbd twice (`klog-1.txt` "06:24:07.425 ... Sending signal 9 to service 'adbd' (pid 1337)", "06:24:13.045 ... started service 'adbd' has pid 2070", "06:24:15.843 ... signal 9 ... (pid 2070)"). Test 41 dock: restarts at 01:33:27, 01:34:26 (cable pull), 01:36:12 and 01:36:14, and "[  186.050770] init: Service 'adbd' (pid 2859) received signal 9". This kills adb-over-Wi-Fi sessions even while Wi-Fi is up. [C]

**S-30 Loggers started from adb die with adbd** (record: "even with setsid"). The root-cgroup logger survived adbd's restarts and init's SIGKILL of services at shutdown: `dock-tv-run/dmesg.txt` continues past "[  253.747956] init: Service 'adbd' (pid 4580) received signal 9" to 254.29 s. [C for survival] [O for the setsid claim]

**S-31 USB gadget restart loop while the HDMI adapter is attached.** adbd's FunctionFS connection ends and restarts about once a second: `logcat-640.txt` "I adbd    : UsbFfs: connection terminated: monitor thread finished" 446 times, about 56 per minute from 00:59 (monitor connected 00:58:54 per `plug1440.log`). `dmesg-run2-ring.txt` repeats "init: Command 'rm /config/usb_gadget/g1/configs/b.1/f5' ..." about every 1.3 s; `pstore-crash4.txt` alternates "waiting dwc3 udc start" / "idle state" about every 60 ms. This is the likely reason wireless adb was needed. [I]

**S-32 printk 8 hides the PD/TCPM log.** The Unisoc TCPM log dump refuses to print at console level 8: `dock-tv-run/dmesg.txt` "[   83.615194] sprd_tcpm_log_print_work:console_loglevel=8, don't log". The dock's PD negotiation (source caps, requested PDO) was therefore not logged. Leave the level at default, or read the TCPM log another way, when PD details matter. [C]

**S-33 pstore keeps very little (correction).** Not "about 25 s": console-ramoops held 0.45-11 s before the panic and dmesg-ramoops 1.5-22 s (console 544.25 vs panic 546.51 in reboot3; 1053.31 vs 1054.61 in crash4; 157.48 vs 157.93 in crash5; 455.63 vs 466.85 in dock). Counting post-panic lines, the files span 4.8-14.8 s (console) and 3.0-23.1 s (dmesg). [C]

**S-34 pstore is not cleared on read.** `pstore-crash5.txt` still holds PANIC B's dmesg-ramoops-1 (first line "<6>[ 1051.755892]c7 [ T5893] sprd-wlan: ..."). [C]

**S-35 SError panics record a plain "reboot".** After an asynchronous SError panic (A, A') the next boot reports "reboot"; after a synchronous external abort (B, C) it reports "kernel_panic,fatal_exception" and pstore holds two dmesg records plus a call trace. A boot reason of "reboot" does not mean there was no panic. Evidence: `pstore-crash5.txt` header "reboot" with "sprd-sysdump: reason: Asynchronous SError Interrupt" in the record; `pstore-crash4.txt` "kernel_panic,fatal_exception". [C]

**S-36 Charger boot overwriting pstore.** The record says a forced restart with a cable attached first boots the off-mode charger ("reboot,charger"), overwriting pstore, and that this lost the TEST 1 lock-up record. No evidence file contains "reboot,charger"; all six "androidboot.mode" values seen are "normal". Neither confirmed nor refuted. [O]

**S-37 adb remount overlay does not survive a reboot; a truncated sprd-drm.ko was once written during a reboot; adb disable-verity was used.** Record only; nothing in the evidence shows these steps. [O]

**S-38 Wi-Fi dies when a dock fails DP training.** Seen on build 36/37 (D-30) and test 41 (K-13, K-17); on test 41 it did not recover until shutdown, so adb over Wi-Fi is lost for the rest of a dock run. [C]

**S-39 Never read `/sys/kernel/debug/25100000.dwc3/mode`.** A userspace `cat` of that file panicked the kernel on build 36/37 (P-01). [C]

**S-40 Duplicate watchers.** On build 36/37, dock-watch logged every event three times ("2026-11-23T23:03:47 unplugged" x3), so three instances ran; hostfix logged "07:10:14 sink + data host, dwc3 was device: set host" twice, suggesting overlapping instances. dock-watch also read the dwc3 debugfs mode file on every record ("dwc3=" printed empty), adding another reader of the file that later panicked. [I]

**S-41 Wi-Fi at dock-watch records was up** (build 36/37): every record from 23:03:13 to 23:15:12 shows 11ac on 5785 MHz, RSSI -14 to -21 dBm, 433 Mbps (390 Mbps once, in the 23:11 record). Correction: the 23:30:15 record (after the panic reboot, only in the longer scratchpad copy of `dock-watch.log`) shows "Wi-Fi standard: 11n, RSSI: -17, Link speed: 433Mbps, ... Frequency: 2457MHz", so the Cube had joined the 2.4 GHz band by then. The "Broken pipe" in each record ("adb.exe : Failed to write while dumping service wifi: Broken pipe") comes from the script's own `grep -m3` closing the pipe, not from adb. So adb drops at those moments were adbd restarts, not Wi-Fi loss. [C]

**S-42 Two sets of hard numbers for the same event can differ by clock base.** Build 36/37 kernel lines are UTC, record headers local; test 41 dmesg is uptime. Mapping offsets are given at the top of this document. The test 41 00:55:44 boot offset (~3344 s) is estimated from `edid.log` "drm_blank dpms 0" at 01:10:19.297 matching the atomic_enable at 874.86 s. [I]

**S-43 Three installer backup runs in the same job folder.** `backup\run-20261124-231055` (device.json "2026-11-24T23:12:37Z" = 15:12 local), `run-20261125-074735` and `run-20261125-075216` (the two test 41 attempts); all mode full, all with the same partition table ("partition_table_sha256": "80821c75..."). The first run is an earlier install, most likely test 40: the installer root's `pack.json` (14:54) says "release": "Thimsy 1.0 test 40". Whether test 40 was flashed and booted is not recorded. [I]

**S-44 The only installer errors are from that earlier 15:11 session**, not test 41: `UartComm_COM_8_2026_11_24.log` "[2026-11-24 15:11:21:794] Call CreateFile() fail, [ErrorCode:0x00000002].", "[2026-11-24 15:11:38:360] Write: Call GetOverlappedResult() fail, [ErrorCode: 0x0000001F].", "UartWatchProc: Call WaitCommEvent() fail". A third start at 15:12:21 went on to back up the device. [C]

---

## 3. Build 36/37 dock tests (2026-11-23 evening local)

All on the VIA hub (S-15) and the 4K TV (S-19), in time order. Kernel times are UTC (local + 8 h).

### 3.1 Baseline with the original device tree

**D-01 Two Type-C ports in sysfs.** port0 is `sc27xx_typec`'s own port (PD revision 0, vconn_source=yes, partner without PD); port1 is tcpm's (PD revision 3, preferred_role=sink, partner with PD and DP alt mode). Nano uses port1: "[sys.gammaos.nano.typec_port]: [/sys/class/typec/port1]". In c2c mode `sc27xx_typec` stops registering port0 (plan 4a), so the numbering can change. Evidence: `cube-display/typec-1.txt` "usb_power_delivery_revision=0", "vconn_source=yes", "preferred_role=sink", "usb_power_delivery_revision=3". [C]

**D-02 Android's view of the port.** UsbPortManager saw "can_change_power_role=false" and "can_change_data_role=false", with only source/host and sink/device combinations (`typec-1.txt`, dumpsys usb). [C]

**D-03 extcon states, Cube as source.** "extcon0 name=extcon-gpio state=USB=1 USB-HOST=0", "extcon1 name=sc27xx-typec state=USB=0 USB-HOST=1", "extcon2 name=sc27xx-pd state=DP=1 PD=0", "audio3 name=31890000.dptx state=HDMI=0" (`typec-1.txt`). The extcon-gpio VBUS cable that dwc3 uses still read USB=1 while the Cube was sourcing. [C]

**D-04 The PTN38003A redriver/mux is not present.** It fails to probe at boot: `klog-1.txt` "11-24 06:23:53.708   331   331 E ptn38003a 6-0030: detect ptn38003a failed."; sprd-drm then logs "ptn38003a hadn't been probe." at every lane/rate set, yet training succeeds at 2 lanes. The chip is probably not fitted on the Cube. [C log] [I not fitted]

**D-05 musb ignores VBUS.** "musb-sprd 64a00000.usb: musb_sprd_vbus_notifier, event(1) ignored in slave mode" (`klog-1.txt` 06:23:56.540); `getprop.txt` "[sys.usb.controller]: [25100000.dwc3]". All host and gadget work goes through dwc3. [C]

**D-06 Original connector DT (stock dtbo entry 2).** Under pd@e00: power-role "dual", try-power-role "sink", no data-role; source PDO 0x04019028 (5 V 400 mA); sink PDOs 0x0401912c (5 V 3 A) and PPS APDO 0xc0dc213c (3.3-11 V 3 A); op-sink-microwatt 10 W (0x989680). No PDO has the DRP or DRD bit. Evidence: `out/.dock/work/dtbo-a1/stock-vs-live.diff` "< sink-pdos = <0x401912c 0xc0dc213c>;", "< source-pdos = <0x4019028>;". [C]

**D-07 PD driver flags at boot.** `klog-1.txt` "06:23:54.043 ... sc27xx-pd supply vbus not found, using dummy regulator", "06:23:54.049 ... sc27xx-pd: use_pdhub_c2c = 0", "sc27xx-pd: vconn NULL!!!", "06:23:54.061 ... charger-manager: use_typec_extcon = 0". [C]

**D-08 Backups before any change.** `dump.sh` (22:15 local) copied dtb_a/b, dtbo_a/b and vbmeta_a/b off the device into `out/.dock/` and `cube-display/dt-backup/`. `dtbo_a.img` is the stock image (sha256 `ca5a55ff...`). `dtb_a.img` is all zeros and is not a usable backup. [C]

### 3.2 Original DT, hub without its charger (22:02 local)

**D-09 The Cube sourced through the vendor host path.** `typec-kernel2.txt` "11-24 06:02:08.030 ... dwc3_sprd_otg_start_host: turn on host" (the path that drives the AW32257 OTG boost), and in parallel "06:02:08.044 ... sc27xx-pd: vbus := On" on the dummy regulator, then "06:02:08.232 ... failed to get vbus status, ret = -110" and three "IRQ: tx error failed" (06:02:08.153, .361, .569). This is the first recorded Cube-as-source session through the PD driver, and it confirms plan R12 (R-11 here): the PD driver's set_vbus switches nothing real. [C]

**D-10 USB worked at once.** xHCI registered ("06:02:08.216 ... new USB bus registered, assigned bus number 2"), "06:02:08.606 ... sprd-ssphy ... USB PHY is host mode", USB2 hub at 06:02:09.643 (high-speed) and USB3 hub at 06:02:09.944 ("new SuperSpeed Gen 1 USB device"), then both receivers and the billboard. USB2 and USB3 together on the first try. [C]

**D-11 No picture.** DP alt mode entered (active=yes); Status Update reply 0x1a (UFP_D connected, enabled, multi-function preferred, HPD low): `typec-1.txt` "11-24 06:02:08.834 ... PD RX, data[1]=0x1a". HPD never rose: `typec-watch.log` from "2026-11-23T22:07:42 p1=[source] ... dpactive=yes ... ibat=-811000 DP1=disconnected" to "22:08:31 ... ibat=-850000 DP1=disconnected". The battery discharged at 0.74-0.88 A. [C] FINDINGS.md says the hub requested 400 mA and was probably underpowered; no log of the request exists. [I]

### 3.3 Original DT, role experiments (22:08-22:12 local)

**D-12 Charger added after the hub: nothing.** The hub sent no PR_Swap, because the Cube's source PDO has no DRP bit; the Cube kept sourcing (FINDINGS.md; plan matrix row 4 "keeps sourcing (seen)"). [C, note only]

**D-13 sysfs power and data role swaps refused.** `prswap-1.txt`: "write rc=0", then "-- after10s: power_role=[source] sink data_role=[host] device", "status=Discharging ibat=-760000". FINDINGS.md: "[sprd_tcpm_pr_set] can't power role swap", "can't data role swap". In that window init re-applied sys.usb.config=adb about once a second and each UDC bind failed ("[ 2113.741079] init: Command 'write /config/usb_gadget/g1/UDC ${sys.usb.controller}' ... faile", again at 2114.78 and 2115.82) because the controller was in host mode. [C]

**D-14 Port forced sink-only (`sinktest.sh`).** `sinktest.sh` writes "sink" to `port1/port_type`, waits 8 s, then writes "host" to `data_role`. The port type changed at once, but the Cube kept sourcing until the hub was re-attached: `typec-watch.log` "2026-11-23T22:09:30 p1=[source] sik/[host] device ... type=dual source [sik] partner=0 ... chg=Discharging ibat=-816000" and the same at 22:10:48-22:11:38. After the re-attach it charged as UFP at about 5 V 1 A: "22:11:40 p1=source [sik]/host d ... ibat=1155000" (one transient sample), "22:11:42 ... usb=1 chg=Charging ibat=97000", "22:11:47 ... ibat=631000 DP1=disconnected". At 22:14:58 the port type was back to "[dual]" while it still charged ("ibat=637000"). FINDINGS.md: the hub sent DR_Swap about every 1.6 s and the Cube refused each (no data-role, so DFP-only caps). No DP, no USB. The DR_Swap log itself and the output of `sinktest.sh` were not kept. [C watcher] [C note for DR_Swap]

**D-15 adb dropped at the role change.** `typec-watch.log` "2026-11-23T22:11:45 adb.exe : adb.exe: device offline", 3 s after the switch to sink. [C]

### 3.4 Device-tree fix iterations (22:15-22:58 local)

**D-16 How the DT fix was applied.** Written by hand into dtbo_a only; slot B and the backups untouched (FINDINGS.md). It adds `data-role = "dual"` and sets DRP|DRD on the 5 V sink PDO (0x2601912c): `stock-vs-live.diff` "> data-role = \"dual\";", "> sink-pdos = <0x2601912c 0xc0dc213c>;", "> source-pdos = <0x401905a>;". No script for the write was kept, so the exact method is unknown. The DT scripts that were kept are read-only: `dt.sh`, `dt2.sh`, `dt3.sh`, `dt4.sh` (22:12-22:14 local) list the connector and PD nodes, and `dtcheck.sh` (22:28) prints the slot, `dtbo_idx`, the live `data-role`, `source-pdos` and `sink-pdos` and the port roles, which fits a check after one of the variant reboots; none of their outputs was saved. The raw image was edited without a new AVB footer, which the plan later warns a bootloader may load truncated ("the old footer records 538536 bytes"). The DT captured at 23:19 shows the fix live. [C] [O method]

**D-17 Source PDO v1: 0x26019028 (DRP|DRD, 400 mA).** No log kept and no result written down. [O]

**D-18 Source PDO v2: 0x2601905a (DRP|DRD, 900 mA).** The hub never answered Source_Capabilities, giving endless hard resets (FINDINGS.md). No PD log was kept, so which bit caused it and what failed (no GoodCRC, PR_Swap, Get_*) are unknown (plan Q3). [C note] [O cause]

**D-19 Source PDO v3 (final): 0x0401905a (original flags, 900 mA).** The hub negotiates 900 mA, the HDMI-only dongle works, and the bus-powered hub gives a picture and light USB (plan matrix row 3 "display + light USB devices (hub negotiates 900 mA, seen)"). It was the variant running at the 23:19 capture (`live.dts` "source-pdos = <0x401905a>;"). [C]

**D-20 Reboots during the iteration.** `watch.log` shows the device going away at 22:18-22:25 (the klog-1 boot at 22:23:51), 22:28-22:30, 22:36-22:46, 22:50 and 22:56-22:58: "2026-11-23T22:18:44 adb.exe : adb.exe: device offline", "22:25:07 ... not found", "22:36:20 ... not found", "22:50:16 adb.exe : error: closed", "22:56:40 adb.exe : adb.exe: device offline". These fit reboots after dtbo writes. Which variant ran after which reboot was not recorded. klog-1's boot already accepted DR_Swap, so some data-role variant was active by 22:23. [I]

### 3.5 DT fix, charger first, no override (klog-1: 22:23:51-22:33:21 local)

**D-21 Booting already attached to the hub and charger.** The hub sent no Source_Capabilities within 460 ms, so tcpm sent a hard reset, and then the caps came: `klog-1.txt` "06:23:55.233 ... SNK_WAIT_CAPABILITIES -> HARD_RESET_SEND [delayed 460 ms]", "charger-manager: srpd_vchg: receive USB PD hard reset request", "06:23:57.381 ... SNK_HARD_RESET_SINK_OFF -> SNK_HARD_RESET_SINK_ON [delayed 1925 ms]", "06:23:57.463 ... Requesting PDO 0: 5000 mV, 1000 mA [mismatch]". [C]

**D-22 Sink contract.** The Cube always requests PDO 0 (5 V 1 A) "[mismatch]" and never 9 V or 12 V: its sink caps are 5 V fixed plus a PPS APDO that the selector ignored. The contract is reached about 250 ms after the caps: `pdlog-1.txt` "06:28:07.240 ... Requesting PDO 0", "06:28:07.496 ... Setting voltage/current limit 5000 mV 1000 mA", "SNK_TRANSITION_SINK -> SNK_READY". [C] The "[mismatch]" is because the 10 W op-sink wish exceeds the 5 W on offer. [I]

**D-23 DR_Swap now accepted.** About 0.6-0.9 s after SNK_READY: `pdlog-1.txt` "06:28:08.114 ... SNK_READY -> DR_SWAP_ACCEPT", "06:28:08.115 ... set roles [sink:host]", "Requesting mux state 1, usb-role 1, orientation 1". The Cube, now DFP, ran Discover Identity, SVIDs, Modes, Enter Mode, Status and Configure in about 20 ms (06:28:08.119-.135). The same happened at 07:06:25.096 and 07:11:28.440 in `dock-watch.log`. [C]

**D-24 Pin assignment D chosen.** The hub's Status 0x1a sets multi-function preferred, so the Cube configures pin D: `dock-watch.log` "pin=C [D] conf=USB [sink]". [C]

**D-25 The typec driver drops the swap.** At every accepted DR_Swap: `pdlog-1.txt` "06:28:08.115   109   109 I ... sc27xx_typec_set_pd_dr_swap_flag entered!", "sc27xx_typec_set_pd_swap_event pd_swap_flag:4!", "typec has disconnected!" (also klog-1 06:23:58.322). The typec extcon stays "USB=1 USB-HOST=0" and dwc3 stays in device mode (FINDINGS.md). Mechanism in R-10. [C]

**D-26 USB host never starts.** The Cube sat at sink:host twice (06:23:58.321, 06:28:08.115), but klog-1 (26,470 lines, 2.7 MB) contains no "xhci" line, so the hub's USB ports were dead. `/sys/class/usb_role/25100000.dwc3-role-switch` has no readable role (`pdlog-1.txt` "role="). [C]

**D-27 DP training fails with -71.** The hub's Attention 0x9a starts training. 14 "PHY BUSY timed out" waits follow, about 0.5 s apart, then failure about 7 s later: `klog-1.txt` "06:24:00.154 ... sc27xx_pd_dp_altmode_notify: vdo:0x9a, hpd_status = 1", "06:24:00.722 ... dptx_phy_wait_busy: PHY BUSY timed out" ... "06:24:07.408 ... [drm:dptx_link_training [sprd_drm]] *ERROR* Link training failed -71". The same again after a replug: "06:28:09.949 ... hpd_status = 1" -> "06:28:17.233 ... Link training failed -71". FINDINGS.md: pin D and pin C failed alike, so the lane layout is not the cause. [C] The combo PHY being held in device configuration is the likely cause (R-15). [I]

**D-28 BC1.2 runs after the swap.** BC1.2 detection on hsphy and ssphy runs 0.5-2 s after the swap, when D+/D- would be needed for USB2 host: "06:23:58.321 ... set roles [sink:host]", then "06:23:58.801 ... sprd-hsphy 64300000.hsphy: sprd_bc1p2_charger_detect:line378: type = 0", "06:24:00.357 ... sprd-ssphy 25310000.ssphy: sprd_bc1p2_charger_detect:line378: type = 0" (and 06:28:09.373 / 06:28:10.913 after the 06:28:08.115 swap). [C] This overlap is the suspected reason early host fails for USB2 (R-13). [I]

**D-29 init blocked during failed training.** "06:24:07.409     1     1 I init    : Command 'write /config/usb_gadget/g1/UDC none' ... took 4178ms and succeeded" (training failed at 06:24:07.408), and "06:28:17.233 ... took 7457ms" (training failed at 06:28:17.233). Each write ended within 1 ms of the -71. [C] That the gadget unbind waits on the training loop is [I].

**D-30 Wi-Fi firmware asserts during both failed trainings.** Boot window: "06:24:03.457 ... E WCN BASE: error: fw assert:[CMD] CMD_REGISTER_FRAME, [REASON] CMD_RSP_TIMEOUT_ERROR", "06:24:06.753 ... NETDEV WATCHDOG: wlan0 (): transmit queue 0 timed out", "06:24:07.409 ... WCN SDIO: sdiohal_tx_thre ... expire_1000ms_cnt 1", "06:24:07.411 ... wcn_reset_process reset begin". Second window: "06:28:13.317 ... fw assert:[CMD] CMD_LLSTAT, [REASON] CMD_RSP_TIMEOUT_ERROR" and a reset in the same millisecond ("06:28:13.317 ... WCN BASE: : wcn_reset_process reset begin"), then "06:28:17.233 ... WCN SDIO: sdiohal_rx_thre ... expire_1000ms_cnt 2", then "sdiohal_card_lock failed" from 06:28:17.233, the same millisecond as the -71 (1167 such lines in all), and "06:28:17.242 ... WARNING: CPU: 0 PID: 978 at kernel/irq/chip.c:243 irq_startup". The log has 34 iface_tx_timeout lines in total. The cause is not established here (see R-18). [C]

**D-31 Wi-Fi association drops.** "06:28:43.672 ... wlan0   : sprd_cfg80211_disconnect ... reason: 3", "06:30:50.320 ... reason: 3", reconnect "06:30:53.426 ... sprd_cfg80211_connect". Reason 3 is a locally requested deauth, which fits the owner toggling Wi-Fi to recover. FINDINGS.md reports that the Cube lost Wi-Fi at least twice and that toggling fixed it. [I]

### 3.6 Overrides that were tried (22:5x-23:15 local)

**D-32 Overrides that did not work.**
- `hostrole.sh` (`echo host > /sys/class/usb_role/25100000.dwc3-role-switch/role`): nothing happened, because the class has no role attribute. [C]
- `forcec.sh`, `pinc.sh`, `dwc3host2.sh` re-sending the DP configuration with pin C: Configure was sent, but the hub did not re-send HPD (FINDINGS.md "Writing sysfs pin_assignment / configuration sends Configure but the hub doesn't re-send HPD, so DP stays off."). [C]
- `reenter.sh` (active=no/yes) and `redetect.sh`: see D-35. [C]
- `udc.sh` (unbind the gadget UDC) and `usb2retry.sh` (toggle usb2-port1/disable): outputs not saved, results unknown. [O]

**D-33 The override that worked: the dwc3 debugfs mode.** After the DR swap, with the Cube as sink and charging: `echo host > /sys/kernel/debug/25100000.dwc3/mode` (debugfs mounted if needed; `dwc3host2.sh`). The USB3 hub enumerated, and the hub's Attention 0x9a then trained the link at HBR2 x2, giving a 1080p picture while charging. Pin D works (FINDINGS.md). [C]

**D-34 `hostfix.sh`, exactly.** It ran as root on the device, detached for 60 minutes, logging to `/data/local/tmp/hostfix.log`. Every 0.1 s it read port1's power_role and data_role and the dwc3 debugfs mode. When the Cube was sink with data host and dwc3 was in device mode, it wrote host at once. When the data role was device and dwc3 was host, it wrote device. v2 adds a retry: 5 s after setting host, if `/sys/bus/usb/devices/2-1` (the USB2 hub) is missing, it writes device, sleeps 2 s and writes host. Source-side connections are left to the vendor driver. v1 started at 07:03:00 and again at 07:10:14 UTC (pid 24502); v2 at 07:13:56 (pid 30591) (`dock-watch.log` hostfix tail). [C]

**D-35 HPD carried in the Status reply is ignored.** When DP mode was re-entered with the monitor already connected (`reenter.sh` about 23:01 local), the Status reply already carried HPD high and nothing followed: `dock-watch.log` "07:01:29.240 ... PD RX, data[0]=0xff01a150", "07:01:29.240 ... PD RX, data[1]=0x9a", "07:01:29.243 ... Rx VDM cmd 0xff01a151 type 1 cmd 17 len 1", then "DP-1=disconnected". The driver acts only on HPD from an ATTENTION (R-07). [C]

**D-36 Recorded: charging + USB2 + USB3 (23:03:13 local).** `dock-watch.log` "power=source [sink] data=[host] device partner=1", "DP-1=disconnected modes= hdmi=HDMI=1 battery=Charging ibat=577000"; lsusb "ID 2109:2817", "ID 2109:8817", "ID 05ac:024f", "ID 046d:c53f", "ID 2109:0817". DP was off because of D-35. [C] This state predates hostfix: the record's hostfix tail holds only "07:03:00 hostfix started", and the USB tree was already up when the record started, so dwc3 had been switched to host by hand before 23:03 (`dwc3host2.sh`, file time 22:59:50, writes host and then re-sends the DP configuration with pin C), late enough after the swap for USB2 to come up. Which writes were made by hand, and when, was not logged. [I]

**D-37 Unidentified source-side attaches.** At 07:03:48 UTC the sink contract ended ("SNK_READY -> SNK_UNATTACHED"), and at 07:03:52.732 the Cube attached as "[source:host]" to a PD partner. No pin was configured, DP-1 stayed disconnected, lsusb listed only root hubs, and the record shows "battery=Discharging ibat=-460000". This may be the hub after its charger was pulled. At 07:15:07.452 the Cube attached as source:host to a partner that sysfs did not register (partner=0) for about 19 s ("07:15:26.363 ... CC1: 2 -> 0 ... disconnected"). dock-watch's grep did not capture the PD traffic. [O]

**D-38 Recorded: charging + HDMI + USB3 (23:06:24 local, hostfix v1).** `dock-watch.log` record 23:06:24-23:06:46: "power=source [sink] data=[host] device partner=1", "DP-1=connected modes=1920x1080 1920x1080 4096x2160  hdmi=HDMI=1 battery=Charging ibat=640000", "11-24 07:06:29.755   418   418 I         : [drm] Link training succeeded rate=2 lanes=2", lsusb "Bus 003 Device 003: ID 2109:0817" with no 2109:2817. `hpd2.log` "pin=C [D] DP-1=connected". This is the recorded proof of power, picture and USB3 at once. [C] Caveat on "hostfix v1": the hostfix tail in both 23:06 records holds only "07:03:00 hostfix started", with no "set host" line for 07:06, while the instance restarted at 07:10:14 does log each write. So the log does not show who wrote host at 07:06: the 07:03 instance (an earlier v1 that may not have logged its writes) or a hand write. The 07:03 instance may also have been killed with adbd at the 07:03:48 replug (S-30). [O]

**D-39 Android display state while docked.** Each successful training switched display 0 to the monitor at 1920x1080 with density 213, and it went back to the 720x720 panel on unplug: `watch.log` "2026-11-23T22:52:00 ... displaymode=dual perf=max size=1920x1080 density=213", "22:54:35 ... size=720x720 density=192", "23:06:32 ... 1920x1080", "23:09:41 ... 1920x1080" (the first sample after an adb outage from 23:07:47 to 23:09:41), "23:11:01 ... 720x720", "23:11:34 ... 1920x1080", "23:15:00 ... 720x720". No external display appears between 22:54:35 and 23:06:32, so the 23:01 re-entry (D-35) and the 23:03 record (D-36) had no picture. The display mode property was "dual" throughout. [C] What was plugged in at 22:52 (probably the HDMI-only dongle after the v3 reboot) was not recorded. [O]

**D-40 USB2 fails when host is set too early (23:11 replug).** hostfix set device at the attach and host in the same second as the swap: "07:11:27 sink + data device, dwc3 was host: set device", "07:11:28 sink + data host, dwc3 was device: set host"; tcpm "07:11:28.442 ... set roles [sink:host]"; lsusb at 23:11:50 shows only "ID 2109:0817". FINDINGS.md: USB2 then shows "new full-speed", "device descriptor read/64 error -71" and "unable to enumerate", while USB3 is unaffected. The raw USB2 failure lines were not saved. [C watcher] [C note]

**D-41 All four at once: charging, HDMI, USB3 and USB2.** A second toggle a few seconds after the first (`retoggle.sh`: write device, sleep 2 s, write host; file time 23:13) brought up the USB2 hub, the billboard and the devices behind it, and DP retrained because the hub re-sent its Attention (FINDINGS.md). `retoggle.sh` wrote its result to `/data/local/tmp/retoggle.out`, which was not copied off, so no single saved snapshot shows all four. [C note] [O snapshot]

**D-42 hostfix v2's automatic USB2 retry never ran.** After "07:13:56 hostfix v2 started (pid 30591)", the hostfix tail in the 23:15 and 23:30 records has no "USB 2 side is up" or "toggling device -> host" line before the panic. The working second toggle was manual. [C]

**D-43 The plug order that works.** In every working case the charger went into the hub first and then the hub into the Cube, so the Cube attached as sink and the hub sent DR_Swap. Hub first and charger later never gave charging: the Cube keeps sourcing, because its source PDO has no DRP bit and PR swaps are gated (plan 4e.8 and matrix row 4). [C]

### 3.7 The debugfs panic (about 23:15:40 local)

**D-44** hostfix v2's polling `cat` of the dwc3 debugfs mode file read a register on a runtime-suspended controller and panicked the kernel. The full trace and trigger are in P-01. [C]

### 3.8 After the panic (23:30 local)

**D-45 The only flipped-plug (CC2) attach on record, and not with the dock.** `cube-display/dock-watch.log` "11-24 07:30:47.558 ... CC1: 0 -> 0, CC2: 0 -> 3 [state TOGGLING, polarity 0, connected]", "set roles [sink:device]", "Requesting mux state 1, usb-role 2, orientation 2", then two "HARD_RESET_SEND [delayed 460 ms]" (07:30:48.059, 07:30:50.679); "battery=Charging ibat=422000". The partner sent no Source_Capabilities; it is likely a non-PD charger or a PC port, but it was not identified. [C] The USB3+DP mux in flipped orientation with a dock remains untested (plan Q8). [O]

### 3.9 Further observations from the session

**D-46 Status vs Attention VDOs.** The hub answers Status Update with 0x1a (HPD low) and sends Attention 0x9a (HPD high) 1.8-2.9 s later. That Attention started every successful training: `pdlog-1.txt` "06:28:08.132 ... data[1]=0x1a" -> "06:28:09.949 ... data[1]=0x9a"; `dock-watch.log` 07:06:25.117 -> 07:06:28.008 and 07:11:28.458 -> 07:11:30.456. [C]

**D-47 Successful training timeline.** HOT_PLUG comes about 100 ms after the Attention, then three PHY BUSY waits of about 0.5 s, then success 1.5-1.75 s after the Attention. The mode is set at once, but `sprd_dpu_atomic_enable` comes about 6 s later: "07:06:28.008 ... CMD_ATTENTION", "07:06:28.108 ... DPTX_HPDSTS_HOT_PLUG", "07:06:29.755 ... Link training succeeded rate=2 lanes=2", "07:06:29.968 ... sprd_dp_timing_set() set mode 1920x1080", "07:06:35.877 ... sprd_dpu_atomic_enable()". From the DR swap to a trained link took about 4.7 s (07:06) and 3.5 s (07:11). [C]

**D-48 Link rate and lanes.** Every successful training, with the hub (pin D) and with the dongle (pin C), was "rate=2 lanes=2" (HBR2 x2). [C] Correction to the earlier wording "the most the Cube's DP controller does": `display_modes_plan.md` section 2 records `max_lanes`=2 and `max_rate`=HBR3 hard-coded in `dptx_init` (`strh 0x0203`), with training taking the minimum of the sink's and those limits. So 2 lanes is the Cube's cap, but HBR2 is below its rate cap and was set by the partner or the link, not by the Cube. Two lanes at HBR2 carry about 1080p60, 1440p60 or 4K30. The plan also records that a failed training ends with -71 only after falling back to 1 lane at RBR, which fits the six rate/lane rounds before each -71 (K-09). [C plan]

**D-49 Every cable change tears DP down.** Each attach or detach sends hpd_status 3 to sprd-drm ("07:03:48.022 ... [drm] sprd_dp_pd_event() hpd_status:3", also 07:06:24.102 and 07:11:27.553). After a detach, DP is disabled through "07:14:59.482 ... DPTX_HPDSTS_HOT_UNPLUG", "sprd_dp_encoder_disable()" and "sprd_dpu_atomic_disable()". These hub unplugs caused no panic. [C]

**D-50 The HDMI extcon lags.** audio3 read "HDMI=1" while DP-1 was disconnected (`dock-watch.log` 23:03:13; `dpwarn-1.txt`). It is not a reliable "monitor present" signal. [C]

**D-51 The Cube's own DP alt mode VDO** is 0x003c3c46 ("/sys/class/typec/port1/port1.0 active=yes vdo=0x003c3c46"), a constant in `sc27xx_pd.ko` .rodata (file offset 0x19bc), not set in the DT. [C]

**D-52 adb drops line up with role events**: 22:11:45 (the switch to sink), 23:11:30 (the replug and the hostfix toggles at 23:11:27-28) and 23:15:57 (the panic). The others came during the DT reboots (`typec-watch.log`, `watch.log`). [C] One drop has no recorded cause: `watch.log` "2026-11-23T23:07:47 adb.exe : error: closed", "23:07:51 adb.exe : adb.exe: device offline", back at 23:09:41 with the monitor still at 1920x1080; dock-watch logged no unplug or plug-in then. A hand toggle (for example an early run of the device-then-host toggle) would fit, but nothing records it. [O] After the panic reboot, `watch.log` also shows "error: closed" seven times between 23:33:56 and 23:43:30, each back within about 8 s, with no dock event on record. [C]

**D-53 Wi-Fi after a successful training is unknown.** dock-watch grepped only PD and DP lines (the `dock-watch.ps1` pattern has no WCN or wlan term), and no full kernel log of 07:00-07:16 UTC was saved. So whether the Wi-Fi asserts also happen with 3 PHY waits instead of 14 is not known. This is the cheapest test of the busy-wait hypothesis (plan section 11, hypothesis 1). [O]

### 3.10 Dock matrix as observed

**D-54** Only four rows of the plan's dock matrix were observed on the device (plan section 6). Rows 2 and 6-16 were never tested. [C]

| Row | Case | Original DT | DT fix (v3) | Test 41 (c2c dtbo) |
|---|---|---|---|---|
| 1 | Bus-powered HDMI/DP dongle (pin C) | works | works | not tested |
| 2 | USB-C to HDMI adapter (Anker 310, S-25; no native DP cable tested yet) | not tested | not tested | works: the Cube sources it, its USB device enumerates, 22 trainings at HBR2 x2 (S-14, S-25); unplugging it can panic (section 6) |
| 3 | Bus-powered hub, no power-in | HDMI dark at 400 mA | display + light USB (900 mA) | not tested |
| 4 | PD hub, Cube first, charger later | keeps sourcing | not re-tested | not tested |
| 5 | PD hub, charger first (main case) | charges only, DR_Swap refused | charges, DR_Swap accepted, USB dead, DP -71; with the debugfs override: charging + HDMI + USB3, and USB2 after a second toggle | charges, DR_Swap accepted through c2c, USB host never starts, DP -71, Wi-Fi lost (section 5) |
| 12/13 | Non-PD charger or PC port | not tested | an unidentified non-PD partner on CC2 charged at +0.42 A (D-45) | PC port: SDP 500 mA, about +0.15 to +0.21 A (K-02) |

The test 41 column is added here from section 5 and S-14; the plan's matrix has no column for it. The build 36/37 VIA hub and the test 41 dock may or may not be the same hardware (S-26).

### 3.11 How charging + HDMI + USB were made to work together

**D-55 The recipe that worked** (FINDINGS.md, `hostfix.sh`, `retoggle.sh`, `dock-watch.log`): [C]

1. Edit dtbo_a by hand: `data-role = "dual"`, sink PDO 0x2601912c (DRP|DRD), source PDO 0x0401905a (900 mA, original flags).
2. Plug the charger into the hub first, then the hub into the Cube. The Cube attaches as sink, takes a 5 V 1 A contract and accepts the hub's DR_Swap (sink:host).
3. Immediately after the swap, as root: `echo host > /sys/kernel/debug/25100000.dwc3/mode` (hostfix.sh did this within 0.1 s). USB3 enumerates, the hub's Attention 0x9a arrives and the link trains at HBR2 x2, giving a 1080p picture while charging (D-38).
4. A few seconds later, write `device`, sleep 2 s and write `host` (`retoggle.sh`). The USB2 hub, billboard and devices enumerate, and DP retrains because the hub re-sends its Attention (D-41).

### 3.12 Why that method cannot ship

**D-56** Each point is [I] unless marked otherwise.
- It needs a hand-edited dtbo_a written without a new AVB footer (D-16), which the plan warns a bootloader may load truncated. Tests 40 and 41 instead ship a properly re-signed c2c dtbo (D-57, D-59).
- It needs a root process writing to debugfs. debugfs must be mounted by hand, and the `mode` file is a debug interface, not a supported role switch.
- Reading that same file panics the kernel when the controller is runtime-suspended (P-01) [C], so any polling helper is dangerous.
- USB2 depends on timing: host set too early fails (D-40) [C]. The fix is an extra toggle, and the automatic retry was never shown working (D-42) [C].
- Every toggle is a role change that tears DP down (D-49) [C] and restarts adbd (S-29) [C].
- It only works in one plug order (D-43) [C].
- The vendor's own path for host-while-sink is the c2c mode (R-12), which does the D+/D- switch and the suspend wait that the debugfs write skips.

### 3.13 Work done after the session

**D-57 The c2c dtbo built for test 40 never ran in the 36/37 tests.** `dtbo-c2c.img` (sha256 `94c0f3d4...`) differs from v3. It adds `vbus-supply = <0x223>` (the AW32257 otg-vbus) on pd@e00, drops the PPS APDO ("< sink-pdos = <0x2601912c>;"), and adds `use_pdhub_c2c` to typec@380, usb@a00000 and usb3@25100000 (`merged-c2c-vs-live.diff`). It was checked offline with fdtoverlay only, and none of the 36/37 results comes from it. Its first device run is the test 41 dock test (section 5). [C]

**D-58 dtbo image layout.** The dtbo is a Unisoc dt_table with 5 entries. The header says total_size 538488 while the entries end at 538536 (the AVB original image size), and the rebuild keeps that 48-byte difference (`dtbo_usbc.py`). [C]

**D-59 dtbo signing.** The stock dtbo is signed with the AOSP test key (SHA256_RSA4096). Re-signing the unchanged raw image with the same salt reproduces it byte for byte (`try.sh` "RESIGN OF ORIGINAL IS BYTE-IDENTICAL"). vbmeta is not touched. [C]

**D-60 Display-mode facts confirmed in the dock sessions.** The patched sprd-drm marks only CEA 1080p60 as preferred (0x48), which is why monitors ran at 1080p60. 1080p60, 720p60 and 4K30 bypass all program pixelpll to 2673 MHz. The earlier 4K hotplug panic (a composer commit with no framebuffer in bypass) had been fixed by Thimsy's 0x90f4 patch before these tests (`display_modes_plan.md`). [C]

---

## 4. Test 41 DisplayPort and 1440p tests (2026-11-25, H27S17 monitor)

All through the Anker USB-C to HDMI adapter (S-25), so this is the monitor's HDMI-input EDID. The monitor's own modes are in S-22 to S-24.

### 4.1 Common observations

**M-01 Boot sequence of the day** (boot-reason epochs in the pstore headers converted to PST, plus bootstat lines): [I]

| # | Boot started | Recorded reason | What ran | How it ended |
|---|---|---|---|---|
| 1 | before 00:32 | - | not recorded (uptime reached 546 s) | PANIC A (`pstore-reboot3.txt`) |
| 2 | 00:32:19 | "reboot" | `pstore-reboot3.txt` read (file time 00:43:07; the file has no header), TEST 1 (run2, uptime ~67-190 s), TEST 2 (try1440 at 00:42 = ~587 s), TEST 3 (unlogged) | PANIC B at uptime 1054.6 (~00:49:54) |
| 3 | 00:50:24 | "kernel_panic,fatal_exception" | `pstore-crash4.txt` read (00:50:33), 1440-only `vendor_dlkm_a` written (image file time 00:53:43) | "reboot,shell" |
| 4 | ~00:55:44 | "reboot,shell" | TESTS 4-10, to uptime ~1479 s (~01:20) | the next boot (01:21:43) records "reboot"; whether that was a command or an SError panic (S-35) is not recorded, since pstore was read only after PANIC A' |
| 5 | 01:21:43 | "reboot" | not recorded | PANIC A' at uptime 157.9 (~01:24:21) |
| 6 | 01:24:50 | "reboot" | ad-hoc dock attempt | PANIC C at uptime 466.8 (~01:32:37) |
| 7 | ~01:33:01 | "kernel_panic,fatal_exception" | logged dock test (`dock-tv-run`) | graceful shutdown 01:37:07 |

Evidence: `pstore-crash4.txt` "00:50:33 up 0 min", "kernel_panic,fatal_exception,1795596624", "reboot,1795595539"; `pstore-crash5.txt` "01:26:06 up 1 min", "reboot,1795598690", "reboot,1795598503"; `logcat-640.txt` "00:55:48.639 ... bootstat: Normalized last reboot reason : reboot,shell"; `dock-tv-run/logcat.txt` "01:33:05.669 ... Normalized last reboot reason : kernel_panic,fatal_exception"; `pstore-dock.txt` "01:33:27 up 0 min".

**M-02 Provenance of the two drivers.** The 1440-only module differs from the stock test 41 module in exactly one 32-bit word, at file offset 0x38D94 = .text+0x8d94 (inside `sprd_dp_connector_mode_valid`, 0x8cb8, size 0x1b4). The word changes from NOP `d503201f` to `0x36180608` (`tbz w8,#3`), the original word that the ALL_MODES patch replaces (`git show 35f6c9b:scripts/sprd_drm.py` "ALL_MODES = (0x8d8c, 2, 0x36180608, NOP)"). `cmp -l` prints only bytes 232853-232856 (1f 20 03 d5 -> 08 06 18 36). The rebuilt EROFS image differs from the original in 38,931 bytes from offset 0x381989, as expected for one recompressed cluster. Hashes: `sprd-drm-1440only.ko` 4c2b7eb7cf021941a9cc23d67abd6e86dc63c52a1ba82deb08ee4154b1bff14c; `sprd-drm-test41.ko` 489154a1964a890dc682ed8bd5c87cf618e55d005188d1b32390277e2df3a92c; `vendor_dlkm_a-1440only.img` 1a35d0859552ae6fc3268ed5231a7d257aaaf21fb75241fc425fea8c9cb37761; `vendor_dlkm_a-test41.img` ec24e7bbd9076970f296d5ad1e54a3d75c36b536fc9fa764b15f176e19fd03c2. [C]

**M-03 Modes the kernel enumerated with the stock test 41 driver (all modes on).** Every DP mode is type 0x60 (user-defined + driver); nothing is preferred. The 3840x2160 modes are 297000 and 296703 kHz at 24 and 30 Hz; VIC 97 (594 MHz) and the 2560x1440 120/144 Hz modes are missing because of the 299 MHz cap. 1080p120 appears at 297000 and 296703. The only 1920x1080 near 60 Hz is the 138.5 MHz CVT-RB mode. 2560x1600 at 268.5 MHz is listed although the EDID does not contain it (inferred from the range descriptor). Some inferred modes, such as 2048x1536 at 267027 and 1600x900 at 255598, are type 0x20 without the driver bit. Evidence `dmesg-try1440.txt`: "\"3840x2160\": 30 297000 3840 4016 4104 4400 2160 2168 2178 2250 0x60 0x5", "\"1920x1080\": 60 138500 1920 1968 2000 2080 1080 1083 1088 1111 0x60 0x5", "\"2560x1600\": 0 268500 ... 0x60 0x9", "\"2560x1440\": 60 241500 ... 0x60 0x9". [C]

**M-04 Android's view in Docked mode (run2).** The monitor becomes logical display 0: "DisplayViewport{type=EXTERNAL, valid=true, isActive=true, displayId=0, uniqueId='local:1'", the panel is "DisplayViewport{type=INTERNAL, valid=true, isActive=false, displayId=3", and "mOverrideDisplayInfo=DisplayInfo{\"HDMI Screen\", displayId 0 ... real 1920 x 1080 ... mode 99", density 213 (`dumpsys-display-run2.txt`). Android lists 80 monitor modes (ids 84-163) and **none is 3840 or 4096 wide**, although the composer starts the link at 3840x2160 (M-09). [C]

**M-05 DP link with the adapter.** All 22 successful trainings were "rate=2 lanes=2". Each is preceded by exactly three "PHY BUSY timed out" (9 per dmesg file for 3 connects), so these timeouts are normal with this adapter and not dock-specific: `dmesg-run2-ring.txt` "[   73.286747] [drm] dptx_phy_wait_busy: PHY BUSY timed out" (also 73.74 and 74.28), then "[   74.346501] [drm] Link training succeeded rate=2 lanes=2". Altmode counts over all files: 22 x "vdo:0x8a, hpd_status = 1", 22 x "vdo:0x18a, hpd_status = 2", 14 x "vdo:0xa, hpd_status = 0". [C]

**M-06 DP transfer-unit values per pixel clock.** The DP controller recomputes its transfer unit on every modeset, including switches that skip the DPU clock setup, so the DP side gets the new rate even when DPU1 does not: 297 MHz -> "tu = 52, tu_frac = 8"; 241.5 -> 42/9; 148.5 -> 26/4; 138.5 -> 24/6; 25.175 -> 4/4 (`dmesg-run2-ring.txt` 79.94 and 83.10, `dmesg-try1440.txt` 587.64, `dmesg-edid.txt` 875.00, `dmesg-try640.txt` 618.45). This proves that the 1920x1080 used by Auto in TESTS 1 and 2 is the 138.5 MHz CVT-RB mode, not 148.5. [C]

**M-07 Recurring noise that is not a symptom.**
- "ptn38003a hadn't been probe." is printed on every Type-C/USB3 PHY event, with the plain adapter and at boot (8-18 per dmesg file; `dock-tv-run/dmesg.txt` "[    2.532039] ptn38003a hadn't been probe."). [C]
- Bursts of "[drm:dptx_aux_transfer [sprd_drm]] *ERROR* dptx_aux_transfer: Not connected" when an EDID read runs during an IRQ_HPD (vdo 0x18a), followed by a normal connect: `dmesg-1080p120.txt` "[ 1209.628936] ... vdo:0x18a, hpd_status = 2" then "[ 1209.629361] ... Not connected" (x3); also `dmesg-try1440.txt` 601.59 (x7) and `dmesg-1440tall.txt` 1449.56 (x5). [C]
- Every panel enable logs "[drm][        dpu_dpi_init] Warning: (vsync + vbp) < 32, underflow risk!" (7 times, one per DSI enable). Unrelated to DP. [C]
- At boot HWC logs "E SPRDHWComposer: Failed to first GetConfigs for display, err:0, num_configs:0" for display 0 (`logcat-640.txt` 00:55:46.916); the panel works, so probably harmless. [I]

**M-08 The H27S17's HPD behaviour on a bad signal.** When the stream is unusable, the monitor drops HPD 3.8-10.3 s after the last modeset (25.5 s in one Docked case) and re-asserts it about 2 s later: `dmesg-mirror.txt` 754.832627 -> 757.030999; `dmesg-1440at297.txt` 1323.831938 -> 1325.920016; `dmesg-1440only.txt` 200.461432 -> 202.579546; `dmesg-1080p120.txt` 1187.707983 -> 1189.771879. The only exception is the 640x480 trial in TEST 5 (M-22). [C]

### 4.2 TEST 1: stock test 41 driver, all modes, Docked mode

Setup: test 41 as installed, Docked mode (built-in panel off when a monitor is connected), monitor plugged in through the adapter. On the monitor: no signal (record).

**M-09 Every connect starts at 3840x2160 in bypass at 297 MHz.** `dmesg-run2-ring.txt` "[   79.844021] [drm] sprd_dpu_mode_set_nofb() set mode: 3840x2160", "[   79.850764] [drm][         dpu_modeset] bypass_mode", "[   79.887900] [drm] sprd_dpu_atomic_enable()", "[   79.892983] [drm][        dpu_clk_init] DPU_CORE_CLK = 512000000, DPI_CLK_SRC = 297000000", "[   79.902032] [drm][        dpu_clk_init] pixelclock is 297000000"; the same at 116.07 and 141.32, and in `dmesg-try1440.txt` at 578.24 and 605.28. [C] The kernel lines do not say which 297 MHz 3840 mode (24 or 30 Hz) it is, and no logcat exists for this run, so "ChosePreferredConfig" and "setActiveConfig 0" are from the record and the plan only. [O]

**M-10 Android's Auto switch to 1080p60 skips the DPU clock setup.** About 3.2 s after the 4K start the DP CRTC is switched to 1920x1080 through "[   83.041266] [drm] sprd_dp_encoder_disable()", "[   83.059352] [drm][            dpu_stop] dpu1 stop", "[   83.064973] [drm] dp suspend OK", "[   83.069022] [drm] sprd_dpu_mode_set_nofb() set mode: 1920x1080", "[   83.075748] [drm][         dpu_modeset] normal mode", "[   83.095130] [drm] sprd_dp_encoder_enable()", "[   83.115135] [drm][             dpu_run] dpu run". No `sprd_dpu_atomic_enable` or `dpu_clk_init` runs for DP, so DPU1 keeps the bypass setup and the 297 MHz clock. Same at 118.61 and 144.33, and in `dmesg-try1440.txt` at 581.92 and 609.31. [C]

**M-11 Docked mode turns the panel off in the same window.** About 2.6 s after the DP enable, immediately before the DP switch: "[   82.455147] [drm] sprd_dsi_encoder_disable(last_dpms=0, dpms=0)", "[   82.523340] [drm][            dpu_stop] dpu stop", "[   82.792237] [drm] sprd_panel_unprepare()", "[   82.941979] [drm] sprd_dpu_atomic_disable()", "[   82.953815] sprd-vpu-pd: PMU_APB_PIXELPLL pw off". PIXELPLL turns on about 0.3 s after the switch ("[   83.411933] sprd-vpu-pd: PMU_APB_PIXELPLL pw on ") and off again 1.26-1.38 s later ("[   84.789518] ... pw off"; pairs 118.99/120.28, 145.22/146.48, 582.68/584.02, 609.55/610.87). [C]

**M-12 HPD-drop timing and cycle period (correction).** The record's "repeats every ~35 s" is approximate: the gaps between plug-ins were 37.9, 28.7, 27.1 and 19.9 s (HOT_PLUG at 72.81, 110.74, 139.39 in `dmesg-run2-ring.txt`; 572.80, 599.94, 619.88 in `dmesg-try1440.txt`), and HPD dropped 8.4-25.5 s after the 1080 switch (83.07 -> "[  108.565694] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG"; 118.61 -> 137.30; 609.31 -> 617.79). On each drop in Docked mode the panel is turned back on ("[  108.994827] [drm] sprd_dsi_encoder_enable(last_dpms=0, dpms=0)", "[  109.692194] ... DPU_CORE_CLK = 614400000, DPI_CLK_DIV = 6"). The monitor re-asserts HPD about 2 s later and the cycle starts again at 3840x2160. [C]

**M-13 Hard lock-up on the first plug-in after a fresh boot.** Reported in the record (needed a forced restart; the record was lost to the charger boot). No log or pstore record of it exists. [O]

Conclusion: with all modes enabled the composer starts at a 297 MHz 4K bypass mode that Android does not even list, and Android's Auto choice of the 138.5 MHz 1080p is applied without a clock change. The monitor never keeps a picture. [C sequence] [I cause]

### 4.3 TEST 2: trial of 2560x1440@60 on the stock driver

Setup: `cmd display gammaos-monitor-mode try 2560 1440 60` (15-s trial) in the same boot. On the monitor: no signal (record).

**M-14 The trial switch skips the clock setup.** `dmesg-try1440.txt` "[  587.602404] [drm] sprd_dpu_mode_set_nofb() set mode: 2560x1440", "[  587.609142] [drm][         dpu_modeset] normal mode", "[  587.637527] [drm] tu = 42, tu_frac = 9", "[  587.648525] [drm][             dpu_run] dpu run"; no atomic_enable or dpu_clk_init. [C]

**M-15 HPD dropped about 10.2 s after the switch (correction: not ~7 s).** "[  597.819855] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG". The "+7s" in `try1440.log` ("00:42:00 TRY 2560 1440 60:" ... "00:42:11 +7s disconnected 2560x1440@60;trial") is a poll counter, not seconds. PIXELPLL came on 0.25 s after the switch ("[  587.849228] sprd-vpu-pd: PMU_APB_PIXELPLL pw on ") and stayed on until the drop, so the PLL was powered during this failure. [C]

**M-16 How the trial ended (correction).** The logs do not show the 15-s timer reverting the mode: the HPD drop at about 11 s ended it. After the re-plug the composer started at 3840x2160 again ("[  605.281641] ... set mode: 3840x2160") and Auto switched to 1920x1080 ("[  609.305620] ... set mode: 1920x1080"), which is when `try1440.log` first shows "00:42:23 +14s connected 1920x1080@60;auto", 23 s after the TRY. [C]

### 4.4 TEST 3: screen off and on through keyevents

**M-17** `input keyevent KEYCODE_SLEEP` / `KEYCODE_WAKEUP` to force a full DP enable. Record: only the panel turned off, no DP atomic_enable, no help. No log of this test is in `dp-evidence`: there is no extra DSI disable/enable pair outside the connect cycles in `dmesg-run2-ring.txt` or `dmesg-try1440.txt`, and no logcat for that period. The plan section 9 idea it tested was itself marked inferred. [O]

### 4.5 TEST 4: the 1440-only driver, Docked mode

Setup: `vendor_dlkm_a` rebuilt with the one-word restore (M-02) and written with dd (user-approved). Monitor plugged in, Docked mode. On the monitor: no picture (record).

**M-18 Only two modes, and a full enable at 241.5 MHz.** `list-1440only.txt` "mode 2560x1440@60", "mode 640x480@60"; `logcat-640.txt` 00:58:57.534 HDMI supportedModes 2560x1440 fps 60 and 640x480 fps 60. The first modeset is the native mode with a clock setup, and no switch follows: `dmesg-1440only.txt` "[  192.673800] [drm] sprd_dpu_mode_set_nofb() set mode: 2560x1440", "[  192.682670] [drm] sprd_dpu_atomic_enable()", "[  192.682778] [drm][    calc_dpi_clk_src] use default DPI_CLK_SRC", "[  192.682781] [drm][        dpu_clk_init] DPU_CORE_CLK = 512000000, DPI_CLK_SRC = 297000000", "[  192.682782] [drm][        dpu_clk_init] pixelclock is 241500000", "[  192.683394] [drm] tu = 42, tu_frac = 9". [C]

**M-19 The disable 2.5 s after connect is the panel's DPU0, not DP (correction to the record and to `README-1440only.txt`).** The `sprd_dpu_atomic_disable` at 195.18 follows the panel's teardown: "[  193.708266] [drm] sprd_dsi_encoder_disable(last_dpms=0, dpms=0)", "[  193.941201] [drm][            dpu_stop] dpu stop" (DPU0; DP's would be "dpu1 stop"), "[  194.787134] [drm] sprd_panel_disable()", "[  195.048052] [drm] sprd_panel_unprepare()", "[  195.183111] [drm] sprd_dpu_atomic_disable()", "[  195.183418] sprd-vpu-pd: PMU_APB_PIXELPLL pw off". The DP CRTC stays enabled until the HPD drop; its "dpu1 stop" comes only at "[  200.489915]" after "[  200.461432] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG". `README-root-cause.txt` item 2 has this right. [C]

**M-20 PIXELPLL and HPD timing.** After the panel-off, PIXELPLL goes off, on (195.30), off (196.62) and on (197.48), and it stays on for the last 3 s before the HPD drop. HPD dropped 7.8 s after the DP enable; later cycles dropped sooner (208.90 enable -> 212.66 drop, 3.8 s). Re-plugs re-enable DP with atomic_enable alone (no mode_set_nofb, since the mode is unchanged). The panel turns back on at each drop and off again about 2 s after the next enable (210.83). `plug1440.log` "00:59:04 +6s disconnected". [C]

**M-21 What logcat shows.** Docked mode turns the built-in screen off right after the monitor is added, and HWC's blank takes 1.6 s, matching kernel 193.71-195.18: `logcat-640.txt` "00:58:56.649 ... SprdDrm:: setActiveConfig: 0, DisplayType:1", "00:58:57.529 ... setActiveConfig: 0, DisplayType:1", "00:58:57.560 ... DisplayPowerController2[3]: BrightnessEvent: disp=3, physDisp=local:0, displayState=OFF", "00:58:59.159 ... Excessive delay in drm_blank(): 1595ms, dpms_value:3", "00:58:59.160 ... setActiveConfig: 0, DisplayType:1". HWC sets config 0 three times for one kernel modeset. On each unplug: "00:59:04.586 ... E SPRDHWComposer: Could not find active mode for 0", "Failed to get_current_vsync_period_by_config, err = 1", "00:59:04.601 ... HWCDisplay::destroy_layer <208>: mInit is false". [C]

Conclusion: the native 1440p mode gets a full enable with clock setup and still fails. The record's FINDING that the panel blank kills the PLL is not supported (R-04). [C] [I]

### 4.6 TEST 5: trial of 640x480@60 (1440-only driver, Docked)

**M-22** The switch path ran without clock setup ("[  618.420033] [drm] sprd_dpu_mode_set_nofb() set mode: 640x480", "[  618.454635] [drm] tu = 4, tu_frac = 4") and PIXELPLL went off 1.47 s later ("[  619.890451] sprd-vpu-pd: PMU_APB_PIXELPLL pw off"). The panel was already off (no DSI lines). On the monitor: no signal (record). This time the 15-s trial timer did revert: `logcat-640.txt` "01:06:02.320 ... GammaOS: monitor mode trial of 640x480@60", "01:06:17.321 ... trial of 640x480@60 ended: not kept in time", and the revert to 2560x1440 ("[  633.437526] ... set mode: 2560x1440") also skipped atomic_enable. Unlike every other failing case, HPD never dropped during the 33-s log (no HOT_UNPLUG in 617.1-650.5 s; `try640.log` connected at every poll 01:06:01-01:06:31). HWC also logged "01:06:02.471 ... E SPRDHWComposer: Failed to commit pset ret=-28" (ENOSPC) and "HWCDisplay::buildSyncData <925>: input fencefd illegal" when the trial started. [C]

### 4.7 TEST 6: Mirror mode at 241.5 MHz (1440-only driver)

Setup: `setprop persist.gammaos.display.mode mirror`. On the monitor: no signal (record).

**M-23** The panel stays on and DP gets a full enable at 241.5 MHz: `dmesg-mirror.txt` "[  748.977243] [drm] sprd_dpu_atomic_enable()", "[  748.977402] [drm][    calc_dpi_clk_src] use default DPI_CLK_SRC", "[  748.977406] [drm][        dpu_clk_init] pixelclock is 241500000". There is no 2.5-s disable and no PIXELPLL line at all, but HPD still drops 5.9 s and 3.9 s after the enables ("[  754.832627] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG"; 763.12 -> 767.02). [C]

**M-24 Android in Mirror mode.** The framework disables the monitor's logical display (it mirrors port 0) while SurfaceFlinger keeps display 1 at power mode 2 (on): `logcat-mirror.txt` "01:08:16.999 ... LogicalDisplayMapper: GammaOS: display 0 is {port=0}, switched off: [{port=1}], switched on: [], mirroring: [{port=1}]", "01:08:17.002 ... SetEnabled on display 8: false", "01:08:17.006 ... Setting power mode 2 on display 1". The "displayState=OFF" for display 8 (physDisp=local:1) is that disabled logical display and is benign. HWC's "drm_blank(): 124ms, dpms_value:3" at 01:08:19.024 (about kernel 755.0) comes after the 754.83 HPD drop: a consequence, not the cause. [C] [I benign]

### 4.8 TEST 7: EDID override, CEA 1080p60 at 148.5 MHz

Setup: debugfs mounted by hand; `edid-1080p60cea.bin` written to `/sys/kernel/debug/dri/0/DP-1/edid_override`; applied by unplug and re-plug. Mirror mode. On the monitor: **picture** (record).

**M-25** The override EDID has one 148.5 MHz DTD (2200x1125, 60.000 Hz, 67.5 kHz) and no extension. The kernel types it preferred: `dmesg-edid.txt` "\"1920x1080\": 60 148500 1920 2008 2052 2200 1080 1084 1089 1125 0x48 0x5". Android sees 1920x1080@60 and 640x480@60 ("01:10:20.190 ... External display 1 initial mode: {{id=0, hwcId=0, resolution=1920x1080, vsyncRate=60.00 Hz"). The connect does a full enable: "[  860.747060] HOT_UNPLUG", "[  870.464007] HOT_PLUG", "[  874.861188] [drm] sprd_dpu_atomic_enable()", "[  874.921735] [drm][        dpu_clk_init] pixelclock is 148500000", tu 26/4. No HPD drop follows for the remaining 33 s (the file ends at 908.20); `edid.log` shows 25 connected polls ending "01:10:49 +25s connected 1920x1080@60;auto". This is the only logged stable link of the day. [C]

### 4.9 TEST 8: EDID override, CEA 1080p120 at 297 MHz

**M-26 Auto switched it away.** The composer started at 1080p120 with a full enable at 297 MHz; 51 ms later Auto switched to 640x480 through the switch path (no clock setup, so DPU1 stays at 297 MHz) and HPD dropped about 10.2 s later, on every re-plug: `dmesg-1080p120.txt` "[ 1177.387311] [drm] sprd_dpu_atomic_enable()", "[ 1177.387475] ... pixelclock is 297000000", "[ 1177.437930] [drm] sprd_dpu_mode_set_nofb() set mode: 640x480", "[ 1177.437952] tu = 4, tu_frac = 4", "[ 1187.707983] ... HOT_UNPLUG"; repeats 1194.21/1195.77 -> 1205.95 and 1209.64/1209.70. `e120.log` shows "640x480@60;auto" at every connected poll. On the monitor: no picture (record). [C] Auto chose 640x480 because it was the only mode in 50-61 Hz (record). [I]

**M-27 DRM still adds inferred DMT modes to the override.** The overridden EDID produced a 148.5 MHz 1920x1080@60 with -hsync -vsync that stayed type 0x40: "\"1920x1080\": 0 148500 1920 2008 2052 2200 1080 1084 1089 1125 0x40 0xa". [C] It was not treated as CEA VIC 16, unlike the CEA DTD with the same timings and +hsync +vsync (0x48, M-25); this contradicts plan section 1's [I] note that a monitor's DMT 1080p60 also counts as VIC 16. The likely reason is that `drm_match_cea_mode` also compares sync flags. [I]

**M-28 try + keep at 1080p120, and forget.** Record: `try 1920 1080 120` + `keep` gave a **picture** at 1080p120 (kept only until unplugged: monitor key none); `forget` returned to Auto 640x480 and the picture went away. These runs fall in the unlogged gap between "01:16:09 done" (end of `e120.log`, dmesg ends at 1224.05) and "01:16:47 mode set; waiting for the next plug-in" (start of `e1440.log`). The claim that 1920-wide at 297 MHz shows a picture rests only on this observation. [O]

### 4.10 TEST 9: EDID override, 2560x1440@60 at 297 MHz, wide horizontal blank

**M-29** Override DTD 2560x1440 at 297 MHz, total 3300x1500, 60.000 Hz, 90.00 kHz, the same sync flags (0x1a) as the monitor's own DTD. Full enable in normal mode at 297 MHz with tu 52/8 (the same values as 4K30 bypass and 1080p120): `dmesg-1440at297.txt` "\"2560x1440\": 60 297000 2560 2608 2640 3300 1440 1443 1448 1500 0x60 0x9", "[ 1316.418846] ... set mode: 2560x1440", "dpu_modeset] normal mode", "[ 1316.419586] [drm][        dpu_clk_init] pixelclock is 297000000", "[ 1316.419913] tu = 52, tu_frac = 8". HPD dropped 7.4 s after the enable ("[ 1323.831938] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG") and 5.8 s after the next (1330.21 -> 1336.01). On the monitor: no picture (record). [C]

### 4.11 TEST 10: EDID override, 2560x1440 at 297 MHz, CVT-RB horizontal and tall vertical blank

**M-30** Total 2720x1820, 59.995 Hz, 109.19 kHz. Same path at 297 MHz (tu 52/8); HPD dropped 5.4 s and 8.5 s after the enables: `dmesg-1440tall.txt` "\"2560x1440\": 60 297000 2560 2608 2640 2720 1440 1443 1448 1820 0x60 0x9", "[ 1440.483036] [drm] sprd_dpu_atomic_enable()", "[ 1440.503882] ... pixelclock is 297000000", "[ 1445.848365] ... HOT_UNPLUG", "[ 1449.573815] sprd_dpu_atomic_enable()" -> "[ 1458.073247] ... HOT_UNPLUG". On the monitor: no picture (record). [C]

**M-31 Not tried.** `out/dp-evidence/research-modes/edids/` holds 17 prepared override EDIDs (x1-x13, y1-y4, for example `x2-720p60-cea.bin`, `x7-2560x1080p60-297.bin`, `y3-2560x1440p75-297.bin`) with no results. All 17 files carry the time 2026-11-25 01:37:36, after the last mode test (about 01:20) and at the end of the dock run, so none of them ran in these tests. [C]

### 4.12 Every mode tried

PIXELPLL rates are computed from the plan's formula `pixelpll = 9 * floor(297 / p) * p` (`display_modes_plan.md` section 2); no log prints the PLL rate, and no "pixelpll clk rate failed" line exists anywhere. "Full enable" means `sprd_dpu_atomic_enable` + `dpu_clk_init` ran; "switch" means only `mode_set_nofb` (DPU1 keeps the previous clock).

| Test | Mode | Pixel clock | How applied | Path | DPI source logged | PIXELPLL (formula) | HPD | Monitor (record) |
|---|---|---|---|---|---|---|---|---|
| 1, 2 | 3840x2160 @24 or 30, bypass | 297 MHz | composer start (stock driver) | full enable | 297 MHz | 2673 (bypass, p = 148.5) | up until the switch | no signal |
| 1, 2 | 1920x1080@60 CVT-RB | 138.5 MHz | Auto, 3.2 s after 4K (5 times: run2 83.07, 118.61, 144.33; try1440 581.92, 609.31) | switch | none (stays 297) | 2493, never attempted | drop after 8.4-25.5 s | no signal |
| 2 | 2560x1440@60 CVT-RB | 241.5 MHz | `try 2560 1440 60` (stock) | switch | none | 2173.5, never attempted | drop after 10.2 s | no signal |
| 4 | 2560x1440@60 CVT-RB | 241.5 MHz | 1440-only driver, Docked | full enable | "use default", 297 MHz | 2173.5 (not logged) | drop after 7.8 s, then 3.8 s | no picture |
| 5 | 640x480@60 | 25.175 MHz | `try 640 480 60` (1440-only) | switch | none | 2492.3, never attempted | stayed up 33 s | no signal |
| 5 | 2560x1440@60 (trial revert) | 241.5 MHz | trial timer | switch | none | - | stayed up | no signal |
| 6 | 2560x1440@60 CVT-RB | 241.5 MHz | 1440-only driver, Mirror | full enable | "use default", 297 MHz | 2173.5 (not logged) | drop after 5.9 s, 3.9 s | no signal |
| 7 | 1920x1080@60 CEA | 148.5 MHz | EDID override | full enable | 297 MHz | 2673 | stable 33 s | **picture** |
| 8 | 1920x1080@120 CEA | 297 MHz | EDID override, composer start | full enable | 297 MHz | 2673 | (switched 51 ms later) | - |
| 8 | 640x480@60 after 1080p120 | 25.175 MHz | Auto | switch (DPU1 stays 297) | none | - | drop after 10.2 s | no picture |
| 8 | 1920x1080@120 CEA | 297 MHz | `try` + `keep` | not logged | - | 2673 | not logged | **picture** (unlogged, M-28) |
| 9 | 2560x1440@60, 3300x1500 | 297 MHz | EDID override | full enable | 297 MHz | 2673 | drop after 7.4 s, 5.8 s | no picture |
| 10 | 2560x1440@59.995, 2720x1820 | 297 MHz | EDID override | full enable | 297 MHz | 2673 | drop after 5.4 s, 8.5 s | no picture |
| 36/37 dock | 1920x1080@60 CEA (0x48) on the 4K TV behind the VIA hub (the HDMI-only dongle's mode was not recorded) | 148.5 MHz | composer start (the only preferred mode, D-60) | `sprd_dpu_atomic_enable` logged ("07:06:35.877"); `dpu_clk_init` lines are not printed at the default log level | not logged | 2673 (plan section 2) | stable while connected (trained 23:06:29 local, unplugged 23:10:59) | **picture** (FINDINGS.md) |

Read across both campaigns, the only modes with a picture are 148.5 MHz CEA 1080p60 from a full enable (TEST 7 and the build 36/37 dock) and the unlogged 297 MHz 1080p120 keep (M-28). Every switch-path change and every 241.5 MHz or 2560-wide mode failed. [C table] [O for M-28]

---

## 5. Test 41 dock test (2026-11-25, 01:33-01:37 local)

Setup (record): test 41 with the 1440-only driver, Mirror mode, the dock's charger plugged into the dock first, a TV on the dock's HDMI, a USB device on the dock, and the on-device logger running. This is the first device run of the c2c dtbo (D-57). Local time = dmesg seconds + 01:33:00.97 (anchor: dmesg "[  246.292788] init: Reboot start, reason: shutdown" = logcat "11-25 01:37:07.266 ... Reboot start, reason: shutdown"; cross-check: the cable pull at 83.505 s = `state.txt` 01:34:25 "Discharging").

### 5.1 Timeline

| Local | dmesg (s) | Event | Source line |
|---|---|---|---|
| 01:33:01 | 0 | Boot after PANIC C | logcat "01:33:05.669 ... Normalized last reboot reason : kernel_panic,fatal_exception" |
| 01:33:23.7 | 22.71 | PC USB cable: sink attach, no PD | "[   22.707549] ... sc27xx_connect_set_status_use_pdhubc2c sink connect !" |
| 01:33:24 | 23.12 | SDP, 500 mA | "[   23.124967] charger-manager ... srpd_vchg: charger type = 1, limit = 500" |
| 01:34:07 | - | Logger's first sample, +187 mA | `state.txt` "01:34:07 / disconnected / / host [device] / source [sink] / Charging / 187000 / 1-0:1.0 usb1" |
| 01:34:24.5 | 83.51 | PC cable pulled | "[   83.505372] ... sc27xx_disconnect_set_status_use_pdhubc2c dr_mode:1  pr_mode:3!" |
| 01:35:53.9 | 173.02 | Dock attached; Cube is sink | "[  173.015574] ... sc27xx_connect_set_status_use_pdhubc2c sink connect !" |
| 01:35:54.1 | 173.14-173.17 | Dock sends a PD hard reset; rx read times out; PD off | "[  173.136460] ... IRQ: PD received hardreset", "[  173.154810] ... failed to read rx status, ret = -110", "[  173.171775] ... pd := off" |
| 01:35:55.4 | 174.41 | VBUS back | "[  174.408526] dwc3-sprd 25100000.usb3: vbus:1 event received" |
| 01:35:56.1 | 175.13 | PD on again | "[  175.133322] sc27xx-typec-pd sc27xx-pd: pd := on" |
| 01:35:57.1 | 176.10-176.14 | DR_Swap accepted; c2c drswap work; host event | "[  176.108498] ... pd_swap_flag:4!", "[  176.130114] ... sc27xx_typec_extcon_drswap_work pd_swap_evt:4!", "[  176.138901] dwc3-sprd 25100000.usb3: host:1 (id:0) event received" |
| 01:35:57.2 | 176.19 | Host start deferred | "[  176.191974] dwc3-sprd 25100000.usb3: vbus charger detect not finished" |
| 01:35:57.7-.9 | 176.71-176.94 | BC1.2 type 0; 1.05 A target; 30 W thermal limit | "[  176.711341] ... srpd_vchg: charger type = 0, limit = 500", "[  176.861598] ... target current = 1050000", "[  176.936565] ... thm_pwr = 30000mW" |
| 01:35:58.1 | - | Nano applies MTP | logcat "01:35:58.073 ... GammaOSNano: usb: applying mtp" |
| 01:35:58.9 | 177.98 | DP alt mode, HPD high | "[  177.979928] ... sc27xx_pd_dp_altmode_notify: vdo:0x9a, hpd_status = 1" |
| 01:35:59.1 | 178.11 | HOT_PLUG | "[  178.105709] [drm] dptx_irq: DPTX_HPDSTS_HOT_PLUG" |
| 01:35:59.3 | 178.30 | BC1.2 done; still no host | "[  178.295184] dwc3-sprd 25100000.usb3: charge detect finished" |
| 01:35:59.3 | 178.34 | Wi-Fi SDIO TX thread takes xmit_lock and never releases it | "time=178341505526" in the 183.61 dump |
| 01:35:59.5 | 178.57 | First PHY BUSY timeout | "[  178.568825] [drm] dptx_phy_wait_busy: PHY BUSY timed out" |
| 01:35:59-01:36:07 | 178-186 | Logger gap (no samples) | `state.txt` "01:35:59" then "01:36:07" |
| 01:36:04.5 | 183.52 | Wi-Fi firmware assert | "[  183.521842] WCN BASE:  error: fw assert:[CMD] CMD_LLSTAT, [REASON] CMD_RSP_TIMEOUT_ERROR" |
| 01:36:06.5 | 185.49 | Link training failed -71 | "[  185.490953] [drm:dptx_link_training [sprd_drm]] *ERROR* Link training failed -71" |
| 01:36:06.5 | 185.51-185.54 | UDC none; WCN reset | "[  185.506268] init: Command 'write /config/usb_gadget/g1/UDC none' ...", "[  185.529916] WCN BASE: : wcn_reset_process reset begin" |
| 01:36:07 | 186.0-186.75 | 54 SDIO card errors | "[  186.001615] mmc2: sprd-sdhci: CMD5 Error 0x00018001 ..." |
| 01:36:07 | 186.05 | adbd killed | "[  186.050770] init: Service 'adbd' (pid 2859) received signal 9" |
| 01:36:09.8 | 188.80 | netdev watchdog | "[  188.801505] NETDEV WATCHDOG: wlan0 (): transmit queue 0 timed out" |
| 01:36:02-01:36:15 | 191.70, 193.96 | USB function changes fail; UDC busy | logcat "01:36:05.081 ... E UsbDeviceManager: Unable to set any USB functions!"; dmesg "... Device or resource busy" |
| 01:37:06.7 | - | Graceful shutdown from Nano's menu, still docked | logcat "01:37:06.668 ... NanoMenu: dismissing overlay before graceful shutdown" |
| 01:37:15 | - | Last logger sample: charging +751 mA | `state.txt` "01:37:15 ... [host] device ... Charging ... 751000" |

### 5.2 Findings

**K-01 The logged dock run is not the one that crashed (correction to the record).** PANIC C (`pstore-dock.txt`) happened in the boot before, at uptime 466.8 s of the boot started 01:24:50 (about 01:32:37): that was an earlier ad-hoc dock attempt (K-17). The logged run (`dock-tv-run`) contains no unplug and no crash; it ended with a graceful shutdown chosen from Nano's menu at 01:37:07 with the dock still attached: logcat "01:37:07.266 ... init: Reboot start, reason: shutdown", dmesg "[  246.032578] init: Received sys.powerctl='shutdown' from pid: 1". Files were pulled at 01:39:53, so the Cube was started again afterwards; that boot is not logged. [C]

**K-02 PC cable phase before the dock.** At boot the Cube was on a PC USB port with no PD: TCPM sent two hard resets that timed out and then settled as a non-PD sink ("[   32.732501] [42 / 118] [   22.720427] [sprd_tcpm_log] sprd_tcpm_snk_attach:line3176 pd_capable flase", "[ 62 / 118] ... PD TX time out", "[117 / 118] ... sprd_tcpm_typec_connect:line3006 pd_capable = 0"). Typed SDP at a 500 mA limit, the battery took about +150 to +210 mA. After the pull the battery went to -556 mA and adbd restarted one second later ("01:34:26.069 ... adbd listening on tcp:5555"). This also shows the test 41 dtbo charges from a plain cable (release report item 2). [C]

**K-03 Attach and PD.** The Cube attached as sink; the dock then sent a PD hard reset, an rx-status read timed out (-110), VBUS dropped and returned at 174.41, and PD came on again at 175.13 (timeline). charger-manager logged "[  173.193212] ... srpd_vchg, typec not disconnect while pd hard reset." The TCPM state log was not printed (S-32), so the dock's source capabilities and the requested PDO are unknown. [C] [O caps]

**K-04 DR_Swap to host, now through the c2c path.** Unlike build 36/37 (D-25: "typec has disconnected!"), test 41 runs the vendor c2c work and raises USB host: "[  176.100228] ... sc27xx_typec_set_pd_dr_swap_flag entered!", "[  176.108498] ... pd_swap_flag:4!", "[  176.120919] charger-manager ... cm_check_pd_update_ac_usb_online is_pd_hub = 1", "[  176.130114] ... sc27xx_typec_extcon_drswap_work pd_swap_evt:4!", "[  176.138901] dwc3-sprd 25100000.usb3: host:1 (id:0) event received". There is no "typec has disconnected!" anywhere in the boot. The logger shows data role "[host]" from 01:35:58 and power role sink throughout. [C]

**K-05 The swap's extcon change sends hpd_status 3.** Right after the drswap work: "[  176.138844] [drm] sprd_dp_pd_event() hpd_status:3", "[  176.138886] sc27xx-typec-pd sc27xx-pd: sc27xx_pd_extcon_event:hpd_status = 3" (again at 176.18). This is plan R8's prediction (R-17): in c2c mode the DR swap's own extcon change counts as a cable change. Here it came before DP was up, so it did no harm. [C]

**K-06 USB host never started.** The host event arrived during BC1.2 detection and was not acted on: "[  176.191974] dwc3-sprd 25100000.usb3: vbus charger detect not finished"; detection finished at "[  178.295184] ... charge detect finished", but no xHCI bus was ever registered in this boot (0 "xhci" lines in `dock-tv-run/dmesg.txt`), and "[  180.105360] dwc3-unisoc 25100000.dwc3: request 0000000000000000 was not queued to ep0out" (again 193.34) shows the gadget side still active. The logger saw the same single bus "1-0:1.0 usb1" in all 169 samples, before and after the dock, so the dock's USB device never enumerated. This answers plan Q2 for this attempt: in c2c mode the host event does overlap BC1.2, and host did not come up later on its own. [C]

**K-07 Nano drives gadget functions while the port is host.** One second after the swap: "01:35:58.073 ... GammaOSNano: usb: applying mtp", then "01:36:02.063 ... E UsbDeviceManager: waitForState(none) FAILED: got adb", "01:36:05.081 ... E UsbDeviceManager: Unable to set any USB functions!", "01:36:11.408 ... W GammaOSNano: usb: mtp attempt 1 did not bring MTP up", "01:36:15.094 ... I GammaOSNano: usb: mtp applied"; dmesg "[  191.704150] init: Command 'write /config/usb_gadget/g1/UDC ${sys.usb.controller}' ... failed: ... Device or resource busy" (also 193.96). The release says Nano no longer changes the port's roles; it does not here, but it still drives device-mode functions while the Cube should be host. [C] Whether this interferes with host start is not known. [O]

**K-08 DP alt mode entered.** Status VDO 0x9a = UFP_D connected (bits 1:0 = 10), enabled, multi-function preferred, HPD high. The DP block powered up on the alt-mode notify, as the test 40/41 change intends: "[  177.989318] [drm] dp resume OK", "[  178.100830] [drm] dptx_irq: DPTX_ISTS_HPD", "[  178.105709] [drm] dptx_irq: DPTX_HPDSTS_HOT_PLUG". [C]

**K-09 Link training failed; no picture on the TV.** 7.39 s of training: 14 "PHY BUSY timed out" (in pairs about 0.47-0.53 s apart, 178.57 to "[  185.346752]") and 6 "dp_link_rate_set"/"dp_lane_set" rounds (180.04 to "[  185.368122] dp_link_rate_set", "[  185.375756] dp_lane_set"), then "[  185.490953] [drm:dptx_link_training [sprd_drm]] *ERROR* Link training failed -71" and "[  185.500786] [drm] sprd_dp_connector_detect()". No EDID was read, no rate was logged, training was never retried (one HOT_PLUG, one altmode notify in the file). The DP connector read "disconnected" in all 169 logger samples (01:34:07-01:37:15). SurfaceFlinger and DisplayManager logged no external display. This is the same failure as build 36/37 without the override (D-27): dwc3 not in host mode. [C]

**K-10 ptn38003a count (correction).** 23 times in the whole boot but 18 during the dock (timestamps >= 172 s), around each rate/lane set ("[  180.041107] ptn38003a hadn't been probe." then "[  180.042273] dp_link_rate_set"). `dock-research/device-readonly-2026-11-25.txt`: "/sys/bus/i2c/devices/6-0030 name=ptn38003a, no driver bound." and, from test 37, "probe of 6-0030 failed with error -5". The same message appears with the plain adapter that trains fine (M-07), so it is not by itself a dock symptom. [C] Whether a missing redriver contributes to the dock's PHY BUSY timeouts is open. [O]

**K-11 Charging through the dock.** VBUS 4.606-4.619 V, battery +499 to +751 mA in the logger after 01:35:59 (566-686 mA in charger-manager samples), charge limit 868 mA, input limit 2050 mA, charger type 0, no fast charge, battery about 91 %: "[  185.547716] charger-manager ... vbat: 4220000, ... ibat: 566000, ... vbus: 4619000, msoc: 912, ... chg_lmt_cur: 868000, inpt_lmt_cur: 2050000, chgr_type: 0, ... is_fchg: 0", "[  231.422702] ... ibat: 686000, ... vbus: 4606000". The AW32257 was set to 2.05 A and then, 0.11 s later, to 1.05 A ("[  176.769595] ... uA:2050000", "[  176.881367] ... uA:1050000"). Before the dock the battery read -0.27 to -0.75 A (01:34:26-01:35:58). The +751 mA last sample was taken during the shutdown. [C]

**K-12 "failed to enable primary charger, ret = -22" is not dock-specific.** It already appears at boot ("[   23.135299] charger-manager charger-manager: failed to enable primary charger, ret = -22") and repeats about every 15 s while docked (176.96, 192.26, 207.38, 222.47, 237.58), with "set wl type to primary charge fail, ret = -22". Charging continued. [C]

**K-13 Wi-Fi chip hang, assert and failed recovery.** The SDIO TX thread took xmit_lock at 178.34 s, 0.24 s after HOT_PLUG, and never released it; the last Wi-Fi RX was at 177.63 s: "[  183.521831] sprd-wlan: sc2355_assert_cmd latest rx chn 22 (177631054526 177631069296).", "[  183.521842] WCN BASE:  error: fw assert:[CMD] CMD_LLSTAT, [REASON] CMD_RSP_TIMEOUT_ERROR", "[  183.613758] WCN SDIO: Last xmit_lock: Enter task(sdiohal_tx_thre) caller: sdio_writebyte [wcn_bsp], time=178341505526", "[  183.637092] WCN SDIO: WARNING:Task(sdiohal_tx_thre) holds xmit_lock!!!". The reset ran ("[  185.529916] ... wcn_reset_process reset begin", "[  185.543596] ... reset end") but the SDIO card did not come back: 54 mmc2 CMD5/CMD52 errors between 186.0 and 186.75, "[  188.801505] NETDEV WATCHDOG: wlan0 (): transmit queue 0 timed out" (a kernel warning, not a panic), "[  190.345958] wlan0: iface_tx_timeout", "[  190.356046] uniber: ACTION_RESET triggered". Userspace: "01:36:06.510 ... E WifiHAL : wifi_get_link_stats: requestResponse Error:-1", 20 wificond "Out of memory" errors, ending "01:37:08.206 ... E WifiNl80211Manager: No valid wificond scanner interface handler for iface=wlan0". Wi-Fi never recovered before the shutdown, about 60 s later. Test 40's notes had already recorded Wi-Fi loss with docks on test 37. [C] Mechanism: R-18. [I]

**K-14 The logger stalled during training.** The one gap over 2 s in 169 samples is 01:35:59 -> 01:36:07, which is dmesg 178.0-186.0 s, exactly the HPD-to-failure window. Either userspace was starved during the PHY busy-waits, or the logger's read of the DP connector status blocked behind the training. The logs do not distinguish the two. [I]

**K-15 adb during the dock run.** adbd restarted at boot (pid 1382), 01:33:27 (2283), 01:34:26 (2859, cable pull), and twice during the swap ("01:36:12.272 ... adb wifi started on port 43619", "01:36:14.491 ... adb wifi started on port 36725"); init killed it at 186.05 s on sys.usb.config=none ("[  186.027614] init: Command 'stop adbd' action=sys.usb.config=none ... took 521ms"). Wi-Fi was dead from 183.5 s, so wireless adb could not reach the Cube for the rest of the run; this is why the on-device logger was needed. [C]

**K-16 Display mode during the dock run.** The record says Mirror mode. The logs cannot confirm it: no Docked-mode panel blank appears, but no external display ever came up to show the difference. [O]

**K-17 The earlier ad-hoc dock attempt (`pstore-dock.txt`).** In the boot 01:24:50-~01:32:37 a dock attempt also lost Wi-Fi ("<3>[  445.043997]c7 [ T1251] sprd-wlan: sc2355_tx_get_msg can not get msg: hif->exit", "[  448.764019] wlan0: iface_tx_timeout", again 464.76). It was charging at 678-764 mA with VBUS 4.606 V ("[  456.955486] ... ibat: 678000 ... vbus: 4606000", "[  459.996637] ... bat_current_ma = 764"), data role host, and DP HPD high until the unplug that caused PANIC C (P-07). Nano Settings (SubSettings) was open (pmsg). Whether the TV showed a picture is not recorded: pstore kept only the last ~22 s. [C] [O picture] The unplug in that record logs one "dp suspend OK" (the pd_event path, 466.704479) and no DP encoder disable or `sprd_dpu_atomic_disable`, so the DP encoder was not running at the unplug (`display_modes_plan.md` section 9 calls this "the encoder already off"). So at that moment no picture was being driven; whether training had failed earlier, as in the logged run, is outside the record. [C lines] [I]

### 5.3 Build 36/37 vs test 41 with the same kind of dock

**K-18** [C]

| | Build 36/37, DT fix (klog-1 boot; which variant is not recorded, D-20), no override | Build 36/37, DT v3 + debugfs host | Test 41 (c2c dtbo) |
|---|---|---|---|
| Contract | 5 V 1 A sink | 5 V 1 A sink | sink after a PD hard reset; PDO not logged |
| DR_Swap | accepted | accepted | accepted |
| typec driver on the swap | "typec has disconnected!" | same | `extcon_drswap_work`, dwc3 "host:1" |
| USB host | none | USB3 at once; USB2 after a second toggle | none ("vbus charger detect not finished") |
| DP training | -71 after 14 PHY waits | HBR2 x2 after 3 PHY waits | -71 after 14 PHY waits |
| Wi-Fi | firmware asserts, recovered by toggling | not logged | assert, reset failed, never recovered |
| Charging | +0.41-0.72 A (`klog-1.txt` charger-manager samples) | +0.55-0.68 A (`dock-watch.log` "ibat=552000" to "678000") | +0.50-0.75 A |

---

## 6. Crashes

### 6.1 Overview

| ID | When (local) | Build | Trigger | Where it faulted | Kind | Record |
|---|---|---|---|---|---|---|
| P-01 | 2026-11-23 ~23:15:40 | 36/37, DT v3 + hostfix v2 | userspace `cat` of dwc3 debugfs `mode` 31 ms after runtime suspend | `dwc3_mode_show+0x48` | async SError | `out/.dock/panic-dmesg.txt` |
| P-02 | 2026-11-25, before 00:32 (?) | test 41 stock | first monitor plug-in on a fresh boot | unknown | hard lock-up | none (record only) |
| P-03 | before 00:32, uptime 546.5 | test 41 stock, Docked | adapter unplug, then DPTX_ISTS_TYPE_C IRQ | `dptx_irq` (lr +0x16c) | async SError | `pstore-reboot3.txt` |
| P-05 | ~01:24:21, uptime 157.9 | test 41 + 1440-only | adapter unplug, then DPTX_ISTS_TYPE_C IRQ | `dptx_irq` (lr +0x16c) | async SError | `pstore-crash5.txt` |
| P-06 | ~00:49:54, uptime 1054.6 | test 41 stock | adapter unplug, stale 1080 switch, then DPMS-off commit | `regmap_mmio_read32le` from `dpu_clk_disable` | sync external abort | `pstore-crash4.txt` |
| P-07 | ~01:32:37, uptime 466.8 | test 41 + 1440-only | dock unplug while charging | `dptx_threaded_irq+0x64` | sync external abort | `pstore-dock.txt` |

Every panic followed an unplug or a debugfs read; none happened while a link was steady. [C]

### 6.2 Details

**P-01 Build 36/37: kernel panic from reading the dwc3 debugfs mode file.** `panic-dmesg.txt`: "<6>[ 1519.694322]c2 [T24533] dwc3-sprd 25100000.usb3: dwc3_sprd_runtime_suspend: enter", "<2>[ 1519.725505]c7 [    C7] SError Interrupt on CPU7, code 0xbe000011 -- SError", "CPU: 7 PID: 4765 Comm: cat", "pc : dwc3_mode_show+0x48/0x1bc [dwc3_sprd_core]", "<0>[ 1519.725563]c7 [    C7] Kernel panic - not syncing: Asynchronous SError Interrupt". The read came 31 ms after the controller runtime-suspended: a register read on a suspended controller. [C]
- Trigger sequence: about 0.9 s before, a cable carrying VBUS was attached; the dummy PD VBUS regulator underflowed; dwc3 went from host to device (xHCI torn down, root hub -108, core soft-reset, gadget initialised, controller suspended). Before that, dwc3-sprd looped "idle state" / "waiting dwc3 udc start" every 56 ms (1517.56-1518.79). Lines: "[ 1518.829109] ... now works as DRP and is in 2 state, event 1", "[ 1518.834318] ... dwc3-sprd 25100000.usb3: vbus:1 event received", "[ 1518.848401] ... regulator-dummy: Underflow of regulator enable count", "[ 1518.848408] ... sc27xx-pd: cannot disable vbus regulator, ret=-22", "[ 1518.913648] ... current_dr_role 1, desired_dr_role 2", "[ 1518.940040] ... hub 2-0:1.0: activate --> -108", "[ 1519.048033] ... do dwc3 core soft reset!", "[ 1519.693989] ... dwc3_suspend_common, current_dr_role:2". [C]
- When and which reader: uptime 1519.7 with adb lost at 22:50:16 (the boot), an unplug logged at 23:15:37 and adb closed at 23:15:57 put the panic at about 23:15:40 local. The `cat` was most likely hostfix v2's 0.1 s loop (`m=$(cat $M 2>/dev/null)` ... `sleep 0.1`), started at 23:13:56; dock-watch also read the file (S-40). [I]
- Status: cause understood (R-16); avoided by never reading the file. [C]

**P-02 Test 41: hard lock-up on the first plug-in after a fresh boot.** Record only: needed a forced restart, and the pstore record was lost to the off-mode charger boot. Which boot it was, and whether it is the same as P-03, is not known. [O]

**P-03 PANIC A: SError in `dptx_irq` after an adapter unplug (`pstore-reboot3.txt`, stock driver, Docked mode).** A physical unplug while the Cube sourced VBUS (pr_mode 4), HOT_UNPLUG, both "dp suspend OK", the DP atomic_disable, the panel re-enable (Docked), the USB3 PHY shutdown, then DPTX_ISTS_TYPE_C and the SError 15.6 ms later: "[  545.282272]c0 [  T173] sc27xx-typec sc27xx-typec: sc27xx_disconnect_set_status_use_pdhubc2c dr_mode:2  pr_mode:4!", "[  545.296137]c0 [    C0] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG", "[  545.306791]c0 [  T173] [drm] dp suspend OK", "[  545.688206]c3 [  T605] [drm] dp suspend OK", "[  545.699314]c3 [  T605] [drm] sprd_dpu_atomic_disable()", "[  546.474187] sprd-ssphy 25310000.ssphy: [sprd_ssphy_shutdown]enter", "[  546.495873]c0 [    C0] [drm] dptx_irq: DPTX_ISTS_TYPE_C", "[  546.511474]c0 [    C0] SError Interrupt on CPU0, code 0xbe000011 -- SError", "pc : el1_da+0xc/0xc0", "lr : dptx_irq+0x16c/0x5a0 [sprd_drm]", "[  546.511845]c0 [    C0] Lost 279 message(s)!", "[  549.746419]c0 [    C0] sprd-sysdump: reason: Asynchronous SError Interrupt". No call trace was saved. The TYPE_C IRQ came 1.19 s after "dp suspend OK". [C]
- Context: between the unplug and the crash Android was turning the panel back on: "[  545.761002] [drm] sprd_dpu_atomic_enable()", "[  545.775786] sprd-vpu-pd: PMU_APB_PIXELPLL pw on", "[  545.793099] [drm] sprd_panel_prepare()", "[  546.058151] [drm] sprd_panel_enable()", "[  546.492186] ... DPU_CORE_CLK = 614400000, DPI_CLK_DIV = 6" (3.7 ms before the IRQ). P-05 shows the panel restore is not required. [C]
- Status: not fixed in test 41. [C]

**P-04 PANIC A: what the faulting code does (correction to the record).** The record says `dptx_irq+0x16c` (0x37cbc) writes 0x80 to regs+0xd00. In fact +0x16c is only the return address of the `printk` that logs DPTX_ISTS_TYPE_C. The words in `sprd-drm-test41.ko` (.text at file offset 0x30000): +0x168 `94000000` (bl printk), +0x16c `d50332bf` (dmb oshst), +0x170 `f946b668` (ldr x8,[x19,#0xd68]), +0x174 `52801009` (mov w9,#0x80), +0x178 `91340108` (add x8,x8,#0xd00), +0x17c `b9000109` (str w9,[x8]) = the TYPE_C acknowledge, +0x184 `91302108` (add x8,x8,#0xc08), +0x188 `b9400109` (ldr w9,[x8]). Fault registers: "x9 : 0000000000000080 x8 : ffffffc017f00c08", "x3 : ffffffc017f00c08", "x1 : 0000000096000010". With pc in `el1_da` (the data-abort handler), x3 = FAR = base+0xc08 and x1 = ESR 0x96000010 (a read): the read of +0xc08 at +0x188 took a synchronous external abort, and the SError (presumably from the posted write to +0xd00) was taken inside that handler when it unmasked asynchronous aborts. `dptx_irq` is .text 0x37b50 (size 0x5a0); `dptx_threaded_irq` is 0x374dc (size 0x674). [C words] [I interpretation]

**P-05 PANIC A again (`pstore-crash5.txt`, 1440-only driver).** Same lr, same fault registers (base ffffffc017700000): "[  156.636325] ... dr_mode:2  pr_mode:4!", "[  156.703467]c0 [    C0] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG", "[  156.707174]c0 [  T174] [drm] dp suspend OK", "[  157.296671]c3 [  T662] [drm] sprd_dpu_atomic_disable()", "[  157.882111] sprd-ssphy 25310000.ssphy: [sprd_ssphy_shutdown]enter", "[  157.897548]c0 [    C0] [drm] dptx_irq: DPTX_ISTS_TYPE_C", "[  157.929320]c0 [    C0] SError Interrupt on CPU0, code 0xbe000011 -- SError", "lr : dptx_irq+0x16c/0x5a0 [sprd_drm]", "x9 : 0000000000000080 x8 : ffffffc017700c08", "x1 : 0000000096000010". There was no panel re-enable, so the panel restore is not the trigger (the boot was probably in Mirror mode, which persists; not recorded). In both A and A' the TYPE_C IRQ fired 1.189-1.190 s after "dp suspend OK", on an already suspended controller. What test was running is not recorded. [C] [I Mirror]

**P-06 PANIC B: DPU1 clock teardown abort (`pstore-crash4.txt`, stock driver).** After the unplug, the panel went off and its DSI CRTC disable powered off PIXELPLL and the VPU domain; then the composer still committed 1920x1080 to the DP CRTC 1.5-1.9 s after HPD was gone (a TYPE_C IRQ during that resume did not crash, because the controller was powered); then a DPMS-off commit from HWC's `drm-event-liste` thread faulted on a clock-mux register read (offset 0x7c). Lines: "[ 1052.300891] usb 2-1: USB disconnect, device number 2", "[ 1052.335054] ... dr_mode:2  pr_mode:4", "<6>[ 1052.380541] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG", "[ 1053.404066] [drm] sprd_panel_unprepare()", "[ 1053.539120] [drm] sprd_dpu_atomic_disable()", "[ 1053.551638] sprd-vpu-pd: PMU_APB_PIXELPLL pw off", "[ 1053.551641] sprd-vpu-pd: power-domain: vpu_pw_off OK", "[ 1053.891558] [drm] sprd_dpu_mode_set_nofb() set mode: 1920x1080", "[ 1054.060954] [drm] dptx_irq: DPTX_ISTS_TYPE_C", "[ 1054.075687] [drm] dp resume OK", "[ 1054.273509] [drm][             dpu_run] dpu run", "[ 1054.606893] [drm] dp suspend OK", "[ 1054.606978] [drm] sprd_dpu_atomic_disable()", "[ 1054.606997]c2 [  T672] Internal error: synchronous external abort: 96000010 [#1] PREEMPT SMP", "CPU: 2 PID: 672 Comm: drm-event-liste", "pc : regmap_mmio_read32le+0x8/0x20", "x1 : 000000000000007c", "x8 : ffffffc012be807c", "[ 1054.668694] Kernel panic - not syncing: Fatal exception". Call trace: `regmap_mmio_read32le+0x8` <- `_regmap_bus_reg_read` <- `_regmap_read` <- `regmap_read` <- `sprd_mux_set_parent+0x4c [clk_sprd]` <- `__clk_set_parent` <- `clk_core_set_parent_nolock` <- `clk_set_parent` <- `dpu_clk_disable+0x68 [sprd_drm]` <- `sprd_dpu_atomic_disable.202+0xb4` <- `sprd_crtc_atomic_disable+0x5c` <- `drm_atomic_helper_commit_modeset_disables` <- `sprd_atomic_helper_commit+0x19c` <- `drm_atomic_connector_commit_dpms` <- `set_property_atomic` <- `drm_mode_obj_set_property_ioctl` <- `drm_connector_property_set_ioctl` <- `drm_ioctl` <- `el0_svc`. Next boot at 00:50:24 ("kernel_panic,fatal_exception,1795596624"). [C] Why the clock register faulted is inferred, and the two write-ups differ: this document's reading is that the VPU domain was already off ("[ 1053.551641] sprd-vpu-pd: power-domain: vpu_pw_off OK"); `display_modes_plan.md` section 9 says the abort came "after DP's glb->disable gated the IPA clocks". Neither is shown directly, and the plan lists this abort as "not addressed yet". [I]

**P-07 PANIC C: `dptx_threaded_irq` abort after a dock unplug (`pstore-dock.txt`, 1440-only driver, ad-hoc attempt).** The dock was unplugged while it charged the Cube (pr_mode 3, Cube as sink, data host); the IRQ thread faulted 151 ms after HOT_UNPLUG and 140 ms after "dp suspend OK", reading DP register 0xd08 on the suspended controller: "[  466.679899] ... sc27xx_disconnect_set_status_use_pdhubc2c dr_mode:2  pr_mode:3!", "[  466.693794] [drm] dptx_irq: DPTX_HPDSTS_HOT_UNPLUG", "[  466.704479] [drm] dp suspend OK", "[  466.769462] dwc3-sprd 25100000.usb3: host:0 (id:1) event received", "[  466.844832]c0 [  T577] Internal error: synchronous external abort: 96000010 [#1] PREEMPT SMP", "CPU: 0 PID: 577 Comm: irq/76-dw_dptx", "pc : dptx_threaded_irq+0x64/0x674 [sprd_drm]", "lr : dptx_threaded_irq+0x50/0x674 [sprd_drm]", "x8 : ffffffc016800d08", "Code: f0000061 91222421 52800020 91342108 (b9400103)", "[  467.929200] Kernel panic - not syncing: Fatal exception". Call trace: `dptx_threaded_irq+0x64` <- `irq_thread_fn` <- `irq_thread` <- `kthread` <- `ret_from_fork`. Module words: +0x60 `91342108` (add x8,x8,#0xd08), +0x64 `b9400103` (ldr w3,[x8]). Battery current at the moment: "[  466.845784] sc27xx-fgu sc27xx-fgu: bat_current_ma = 761". [C]

**P-08 PANIC A is intermittent.** The same post-unplug DPTX_ISTS_TYPE_C IRQ, after the same pr_mode-4 unplug, HOT_UNPLUG, DP atomic_disable and USB3 PHY shutdown, was survived six times: `dmesg-mirror.txt` 736.271060, `dmesg-edid.txt` 861.805177, `dmesg-1080p120.txt` 1167.206275, `dmesg-1440at297.txt` 1307.673025, `dmesg-1440tall.txt` 1428.444361, `pstore-crash4.txt` 1054.060954 (powered controller). The IPA power-off came about as soon after the IRQ in survived and fatal cases (0.33 ms in `dmesg-edid.txt` "sipa power off success" 861.805506; 0.23 ms in reboot3 546.496098), so it does not explain which unplugs panic. [C] Why only some panic is unexplained. [O]

**P-09 Thimsy's patches do not touch the IRQ handlers (correction to the record's patch list).** Test 41's PATCHES cover 0x8cdc, 0x8d04, 0x8d18, 0x8d88, 0x8d8c, 0x8db8, 0x8dd8, 0x8df0, 0x8e14 and 0x8e3c inside `sprd_dp_connector_mode_valid` (0x8cb8-0x8e6c), plus 0x90f4 inside `sprd_dp_encoder_mode_set` (0x90b4-0x9188); HPD_PATCHES cover 0x89b4 inside `sprd_dp_pd_event` (0x8964-0x8c18) (`git show 35f6c9b:scripts/sprd_drm.py` lines 49-124; `work-irq-guard/syms.txt`). Nothing is patched in `dptx_irq` (0x37b50-0x380f0) or `dptx_threaded_irq` (0x374dc-0x37b50). [C]

**P-10 Common pattern.** All three DPTX-side panics (A, A', C) touch DP controller registers from the IRQ path after the unplug path has logged "dp suspend OK"; the ioremap base differs per boot (ffffffc017f00000, ffffffc017700000, ffffffc016800000). PANIC B is the CRTC-disable clock path touching a clock mux after the VPU domain was powered off. [C traces] [I common cause]

**P-11 With the HDMI adapter the Cube is a USB host.** Every monitor unplug (A, A', B) starts with "usb 2-1: USB disconnect, device number 2" (the adapter's Anker USB device, S-25) followed by the usb2/usb3 root hubs going away ("[ 1053.084205] usb usb3: USB disconnect", "[ 1053.250446] usb usb2: USB disconnect" in crash4). The dock run never reached host mode. [C]

---

## 7. Root causes and mechanisms

### 7.1 Display modes (test 41)

**R-01 The "297 MHz" rule: what the logs support.** The plan's formula `pixelpll = 9 * floor(297 / p) * p`, `dpi = 9 * p` gives 2673 MHz for 148.5 and 297 MHz (and 4K30 bypass at p = 148.5), 2493 for 138.5, 2173.5 for 241.5 and 2492.3 for 25.175. The record concluded "only pixel clocks that divide 297 MHz work". The logs refine this: [I]
- For the failing 138.5 and 25.175 MHz cases (TESTS 1, 2, 5, 8) `dpu_clk_init` never ran ("pixelclock is" counts over all files: 148500000 x1, 241500000 x6, 297000000 x14; never 138500000 or 25175000), so those PLL rates were never attempted. Those failures come from the missing clock reprogram (R-02), not from a PLL limit. [C counts]
- Every DP `dpu_clk_init` logs "DPI_CLK_SRC = 297000000" (21 lines), and 241.5 MHz is the only non-divisor that got a full init: the driver then logged "use default DPI_CLK_SRC" (R-03). A pixel clock reached by an integer division of 297 MHz (148.5 = 297/2, 297 = 297/1) is what the logged success has in common. [C lines] [I mechanism]
- A 297-divisible clock is not sufficient: 2560x1440 at 297 MHz failed twice (TESTS 9, 10). [C]
- The only logged stable link is 148.5 MHz (TEST 7). [C]

**R-02 The mode-switch clock bug.** A mode change on a live connector runs `sprd_dp_encoder_disable` -> "dpu1 stop" -> "dp suspend OK" -> `sprd_dpu_mode_set_nofb` -> `sprd_dp_encoder_enable` -> "dp resume OK" -> "dpu run", without `sprd_dpu_atomic_enable` or `dpu_clk_init` (M-10, M-14, M-22, M-26). The DP controller's transfer unit is recomputed for the new clock (M-06), but DPU1 keeps the old pixel clock and, after a 4K start, the bypass setup. [C] The resulting DPU/DP rate mismatch is the likely reason those streams are rejected and the monitor drops HPD. [I] Note that the TEST 5 640x480 switch kept HPD up for 33 s, still without a picture (M-22). [C] Correction: `out/dp-evidence/work-modeswitch-clock/` is not empty. It holds 14 files written 00:45-00:59 local on 2026-11-25, among them `stock.ko`, `dpclk.s`/`dpclk.o` and a built `test41-modeswitch.ko`; it was not reviewed here, and no log shows that module running. `display_modes_plan.md` section 9 describes the planned fix as patch D (`MODE_CHANGE_PATCHES`: a pure DP mode change calls DPU1's atomic_disable and atomic_enable), "None has run on the device". [C files] [O effect]

**R-03 241.5 MHz falls back to a 297 MHz DPI source.** `calc_dpi_clk_src` prints "use default DPI_CLK_SRC" only for the six 241.5 MHz enables (3 in `dmesg-1440only.txt`, 3 in `dmesg-mirror.txt`), never for 148.5 or 297 MHz. DPU_CORE_CLK stays 512 MHz on every DP enable (all 21 lines). So the fallback is in the DPI source, which does not match the "Core clock" bullet of plan section 2 (core clock set to 297 MHz when the pixel clock does not divide 128/192/297/307.2). [C lines] That a 241.5 MHz pixel clock cannot be produced from a 297 MHz source, so the native 1440p mode is driven at the wrong rate, is [I].

**R-04 Docked-mode PLL power-off is not the cause (correction to the record's FINDING and to `README-root-cause.txt` item 2).** In Docked mode the panel-off and "PIXELPLL pw off" do coincide with the failures (M-11, M-19, M-21). But: [I]
- In Mirror mode and in all five EDID-override runs there is no PIXELPLL line at all and no DSI disable (counts 0 in `dmesg-mirror.txt`, `dmesg-edid.txt`, `dmesg-1080p120.txt`, `dmesg-1440at297.txt`, `dmesg-1440tall.txt`), yet 241.5 MHz and 2560x1440 at 297 MHz still failed (M-23, M-29, M-30). [C]
- During the TEST 2 failure the PLL was on ("[  587.849228] ... PIXELPLL pw on " with no "pw off" before the 597.82 drop). [C]
- Plan section 9 already marks PIXELPLL as the panel's `vpu_pd_top` domain [I]. Whether DP's pixel clock depends on PIXELPLL at all is still open. [O]

**R-05 The 4K start.** With all modes enabled every DP mode is type 0x60 and nothing is preferred (M-03), because the patched driver marks only CEA 1080p60 preferred and this monitor has no VIC 16 (S-24). The composer then starts every connect at a 297 MHz 3840x2160 in bypass (M-09), a mode Android does not list (M-04), and Android's Auto immediately switches away through the broken switch path (R-02). [C] Why the composer picks that mode (config 0 per the record's "ChosePreferredConfig"/"setActiveConfig 0") is not in any test 41 log. [O] Whether the monitor showed anything during the 3.2 s of 4K is not recorded. [O] `work-no-4k-start/` and `work-safe-start/` exist in `out/dp-evidence/` but were not reviewed.

**R-06 The 2560-wide question.** At the same 297 MHz clock with identical transfer units (tu 52/8 = 82.5 % of 2-lane HBR2 for 4K30 bypass, 1080p120 and both 2560x1440 overrides), 2560-wide modes fail and the 1920-wide 1080p120 reportedly works. Both 2560 timings are inside the monitor's range limits, so link bandwidth does not explain it. Hypotheses: a DPU1 line-width limit outside bypass (bypass is used only for 3840x2160), or the monitor rejecting both custom timings. The comparison case (1920 wide works at 297) is itself only the unlogged keep test (M-28). [O]

**R-07 HPD in the Status reply and IRQ_HPD handling** (plan R7 and R9, disassembly): only the ATTENTION branch of `sprd_tcpm_pd_rx_handler` calls `dp_altmode_notify` (`sprd_tcpm.ko` .text+0xc618-0xc678); HPD in a Status Update or Configure ACK is dropped (live: D-35). With DP off, only hpd 1 powers DP up; a first Attention that also carries IRQ_HPD (0x18a, hpd 2) is ignored (`sprd-drm.ko` `sprd_dp_pd_event` compare at .text+0x89b4). Neither the test dongle nor the H27S17 adapter sent 0x18a first (they send 0x8a first, M-05). The plan's DP-B patch ("ships in test 40"; `drm_hpd.S` "eor  w8, w8, w8, lsr #1  // hpd 1 or 2 -> bit0 set; 0 or 3 -> clear") changes this. [C]

**R-08 Pin selection** (plan R10, `sprd_typec_displayport.ko` `sprd_dp_altmode_status_update` .text+0xcb0-0xce4): a partner offering C+E or D+F gets a Configure with two pin bits (invalid); a hub offering C+D without the multi-function bit gets pin C and loses USB3. The test hub sets the multi-function bit, so it got D (D-24). The plan's patch always prefers D (`dp_pin.S` "mov  w8, #0x10"). [C code] [I effect on other docks]

**R-20 `display_modes_plan.md` section 9 checked against the logs.** Its findings 1-3 match the logs (the 4K bypass start, the mode change that skips DPU1's disable and enable, and PANIC A's ack at +0xd00 and read of +0xc08), with these differences: [C]
- Finding 1 marks the start mode "3840x2160@30 (297 MHz, bypass) [C]"; the kernel lines cannot tell 30 Hz from 24 Hz, both 297 MHz (M-09).
- Finding 2 gives the cycle as "about every 35 s [C]"; the measured gaps are 19.9-37.9 s (M-12). Its [I] note that the PIXELPLL toggling "is not the cause" agrees with the Mirror and override runs (R-04).
- Finding 3 describes `pstore-crash4.txt` as "the same trigger and survived it, then a separate abort"; it leaves out the stale 1920x1080 mode switch the composer ran on the unplugged DP before the abort (P-06), and gives a different inferred reason for the abort (IPA clocks gated by DP's glb->disable, against the VPU domain being off here).
- Finding 4 gives the 241.5 MHz HPD drop as "6-8 s later"; the logs show 3.8-7.8 s (M-20, M-23). It marks "At 297 MHz, 1920x1080@120 CEA showed a picture **[C]**"; no saved log shows that run (M-28), so it is the tester's report only. [O]
- Its fixes A (nothing 3840 or wider), B (IRQ guard), C (safe start at the best 1080p60 at or below 148.5 MHz) and D (clock setup on a DP mode change) are in `scripts/sprd_drm.py` per the plan, and "None has run on the device". The plan itself warns that C starts the H27S17 at its 138.5 MHz mode, a rate that "has never shown a picture". [C plan] [O on the device]

### 7.2 USB-C docks

**R-09 No data-role and a 400 mA source PDO in the stock DT** (plan R1). The connector DT has no `data-role`, so the port is DFP-only and refuses DR_Swap, and 400 mA is too little for bus-powered hubs. The values equal Unisoc's reference overlay and dtbo entries 0, 3 and 4; entry 1 (UMS9620 1H10) differs ("sink-pdos <0x040190c8 0xc0dc213c>", 5 V 2 A). [C]

**R-10 The c2c flag gated the swap on build 36/37** (plan R2-R4, module disassembly). Only Unisoc's "PD hub" mode (`use_pdhub_c2c`) sets the "partner connected" byte (+49) that `sc27xx_typec_set_pd_swap_event` checks (.text+0x594 "ldrb w8,[x22,#49]; cbz w8,.text+0x670"); when clear it logs "typec has disconnected!" (D-25). The same flag, copied through `sc27xx_pd`, gates sysfs PR and DR swaps in tcpm (`sprd_tcpm_pr_set` .text+0x4648, `sprd_tcpm_dr_set` .text+0x4384 read port+0x1068) (D-13). None of the c2c properties existed in the live DT or any of the 5 dtbo entries; the log showed "use_pdhub_c2c = 0". [C]

**R-11 The PD driver had no real VBUS supply** (plan R12). pd@e00 had no `vbus-supply`, so tcpm's set_vbus switched a dummy regulator ("sc27xx-pd supply vbus not found, using dummy regulator"; "regulator-dummy: Underflow of regulator enable count" in P-01; "vbus := On" next to "dwc3_sprd_otg_start_host: turn on host" in D-09). The only real VBUS source is the AW32257 boost (`vddvbus`), reached through dwc3's and musb's vbus-supply when host mode starts; c2c mode skips exactly those calls, so c2c without a vbus-supply on pd@e00 would leave the Cube unable to power dongles [I]. The test 40/41 dtbo adds the vbus-supply (D-57); on test 41 the "supply vbus not found" warning is gone and the bus-powered HDMI adapter is powered and enumerates (S-13, S-14). [C]

**R-12 The c2c path is the vendor's design for host-while-sink** (plan section 3, disassembly). With the flag on, swap flag 4 queues `sc27xx_typec_extcon_drswap_work`, which raises USB_HOST on the typec extcon; dwc3-sprd's hotplug state machine then waits for the core to runtime-suspend, switches D+/D- to the controller (`call_sprd_usbphy_event_notifiers(0,1,NULL)` at .text+0x1bdc, c2c only) and starts host without the OTG boost: exactly the two steps the debugfs override skipped (R-13). [I from disassembly] Test 41 shows the first half running on the device (K-04: drswap work, "host:1 (id:0) event received") and then stopping at "vbus charger detect not finished" (K-06). [C]

**R-13 Why USB2 failed after a fast debugfs switch** (plan R5). The debugfs write skips the vendor steps (switch D+/D- to the controller, wait for runtime suspend) and overlaps BC1.2 charger detection, which takes over D+/D- after the PD contract (D-28, D-40). [I] On test 41 the overlap is confirmed in c2c mode as well: dwc3 refused the host event because detection was still running (K-06). [C]

**R-14 The dock role-swap/host-mode gap, per build.** [C facts] [I cause]
- Build 36/37: the swap never reached dwc3 (R-10); no host; DP -71.
- Build 36/37 + debugfs: host forced; USB3 at once, USB2 needs a later second toggle (R-13); DP trains.
- Test 41: the swap reaches dwc3 through c2c, but the host start is refused while BC1.2 runs, and nothing retries it after "charge detect finished" 2.1 s later (K-06). Nano's MTP switching runs at the same time (K-07).

**R-15 Dock DP link training -71.** It fails whenever dwc3 is not in host mode (D-27, K-09) and succeeds at the first attempt with three PHY waits once host is forced (D-38, D-47). Pin D and pin C failed alike (D-27), and the plain adapter trains fine without the PTN38003A (M-05), so neither the lane layout nor the redriver alone explains it. [C] The likely mechanism is the USB3/DP combo PHY being held in its device configuration until host mode starts. [I]

**R-16 The debugfs mode-file panic** (plan R6): `dwc3_mode_show+0x48` in `dwc3_sprd_core` (vendor_boot) reads a controller register without checking runtime PM; reading the file while the controller is suspended raises an SError (P-01). [C]

**R-17 Any extcon change kills DP** (plan R8, `sc27xx_pd_extcon_event` .text+0x1358): `sc27xx_pd` listens to every cable on the typec extcon and sends hpd_status 3 on any change; only a new Attention brings DP back (D-49). In c2c mode the drswap work's own USB=0/USB_HOST=1 changes count too, so a DR swap after DP is up would turn DP off [I]; on test 41 the swap's hpd_status 3 came before DP was up (K-05). [C]

### 7.3 Wi-Fi

**R-18 The Wi-Fi chip reset during dock training.** [I]
- Every dock training failure with 14 PHY busy-waits coincides with a Wi-Fi firmware assert: build 36/37 boot window and replug (D-30), test 41 (K-13), and the ad-hoc test 41 attempt (K-17). [C]
- None of the plain-adapter runs, whose trainings take three waits, contains a "fw assert", "wcn_reset_process reset begin" or "NETDEV WATCHDOG" line (0 in all nine `dmesg-*.txt` files and the three adapter pstore files). [C]
- `dptx_phy_wait_busy` spins up to 100001 times with `__const_udelay` (about 1 µs each) and does not sleep (`dock-research/dptx_phy_wait_busy.dis` "mov w21, #0x86a1" / "movk w21, #0x1, lsl #16" / "subs w21, w21, #0x1" / "bl 0 <__const_udelay>"). [C]
- On the device, `irq/76-dw_dptx` and `irq/24-mmc2` are both SCHED_FIFO 50 and pinned to CPU0 (`device-readonly-2026-11-25.txt`). [C]
- In test 41 the SDIO TX thread entered xmit_lock 0.24 s after HOT_PLUG and 0.23 s before the first PHY BUSY timeout. [C]
- Hypothesis: when training runs in that context, the busy-waits starve the SDIO host's interrupt handling, the WCN command times out, and the chip asserts. On build 36/37 the chip recovered (Wi-Fi toggled by hand); on test 41 the reset failed at the SDIO level (54 CMD5/CMD52 errors) and Wi-Fi never came back. Whether the asserts vanish once training succeeds at the first try is untested (D-53).

### 7.4 Unplug panics

**R-19 Unplug panic mechanisms.** [C traces] [I mechanism]
- PANIC A/A': after the unplug path has suspended the DP controller ("dp suspend OK"), a late DPTX_ISTS_TYPE_C interrupt (about 1.19 s later, right after the USB3 PHY shutdown) runs the hard IRQ handler, which acknowledges at +0xd00 and reads +0xc08 on the powered-down block (P-03, P-04, P-05). It is intermittent (P-08).
- PANIC C: the threaded handler reads +0xd08 140 ms after the suspend (P-07).
- PANIC B: after the panel's DSI disable has powered off the VPU domain, the composer still runs a stale mode switch on the unplugged DP and then a DPMS-off commit whose `dpu_clk_disable` -> `clk_set_parent` reads a clock-mux register (P-06).
- None of these handlers is patched by Thimsy (P-09). `out/dp-evidence/work-irq-guard/` contains a built `sprd-drm-test41-irqguard.ko`, which was not reviewed or tested in these runs.

---

## 8. Charging observations (all builds)

**C-01 Contract.** As a sink the Cube always requests PDO 0, 5 V 1 A, "[mismatch]" (D-22), and never 9 V or 12 V. [C]

**C-02 Current into the battery.** Build 36/37 with the hub: about 0.4-0.72 A with the screen on, VBUS 4.646-4.672 V (31 samples at 4646000, 3 at 4659000, 2 at 4672000 µV in `klog-1.txt`): `klog-1.txt` "06:24:24.708 ... vbat: 3951000, ... ibat: 695000, ibat_avg: 705000, ibus: 0, vbus: 4646000, ... chg_lmt_cur: 868000, inpt_lmt_cur: 2050000, chgr_type: 0", "06:26:55.752 ... ibat: 720000"; `dock-watch.log` "ibat=577000", "640000", "678000", "552000", "558000" (552-678 mA). Test 41 dock: +0.50 to +0.75 A, VBUS 4.606-4.619 V (K-11); ad-hoc dock: 678-764 mA (K-17). That is about 5 W from the source and about 3 W into the battery. [C]

**C-03 Non-PD sources.** Original DT, port forced sink: about +0.63 A (D-14). Flipped-plug non-PD partner: +0.42 A (D-45). Test 41 on a PC port (SDP, 500 mA): about +0.15 to +0.21 A (K-02). [C]

**C-04 Charger type and current votes.** The hub's charger is BC1.2 type 0 (UNKNOWN). On every attach the AW32257 is set to 2050000 µA and then to 1050000 µA (the UNKNOWN table): `klog-1.txt` "06:23:58.801 ... srpd_vchg: charger type = 0, limit = 500", "aw32257_set_charge_current uA:2050000", "06:23:58.802 ... uA:1050000"; the same on test 41 ("[  176.769595] ... uA:2050000", "[  176.881367] ... uA:1050000"). PD contract details do not change it. On test 41 the JEITA target is 1.05 A ("[  176.861598] ... target terminate voltage = 4340000, target current = 1050000") with a 30 W thermal power limit. [C]

**C-05 868 mA readback vs a 1050 mA request.** charger-manager reports "chg_lmt_cur: 868000" on both builds. 868 mA = 496 + 3 x 124 mA, a step of the AW32257 charge-current register; the plan's estimate was 992 mA. Which register code is really set has not been checked. [O]

**C-06 Charger-manager configuration.** 2.05 A max charge and input limits, 6.5 V max charge voltage, and charge-pump fields the Cube cannot use: `klog-1.txt` "06:23:54.579 ... cm_update_charge_info, chgr type= 0, fchg_en= 0, cp_running= 0, ... max chg_lmt_cur= 2050000uA, max inpt_lmt_cur= 2050000uA, max chg_volt= 6500000uV", "SPRD_BATTERY_INFO: ... cp_max_ibat= 5000000". The plan treats the 15 W values as Unisoc reference-board leftovers (bq2560x + bq2597x charge pump, still in dtbo entry 0). [C log] [I origin]

**C-07 Charging limits** (plan R11, `aw32257_charger.ko`, charger-manager, datasheet). The AW32257 buck has no power path and no input-current-limit register, takes 4-6 V input (OVP 6.4 V), charges at 2.48 A at most (496-2480 mA, REG04), and its OTG boost gives 5.05 V at 1 A. Charge current depends only on the BC1.2 type: DCP 2.0 A (0x1e8480), UNKNOWN 1.05 A (0x100590), CDP 1.5 A (0x16e360), SDP 0.5 A (0x7a120) in the 15-52 °C band. Anbernic's driver caps requests at 2.3 A (`aw32257_set_charge_current` .text+0x2c30). [C from code and datasheet; not in the test 41 logs]

**C-08 15 W is not reachable.** From C-07: the realistic ceiling is about 10-11 W; any 9 V, 12 V, PPS or fast-charge setting would risk the chip's OVP. PD sources are held at 1.05 A ("step 2 not shipped", record). [I]

**C-09 "failed to enable primary charger, ret = -22"** repeats on test 41 from boot and while docked without stopping charging (K-12). [C]

**C-10 Off-mode charger boot.** The record says a forced restart with a cable attached boots the off-mode charger ("reboot,charger"); no log shows it (S-36). [O]

---

## 9. State of the test Cube after test 41 and how to restore it

This section is the state after the test 41 dock test (2026-11-25, 01:37) and is kept as it was then. The test 42 runs changed it since: at the end of them the Cube runs `vendor_dlkm_a` MOD-Y1-E-CM12 (`a96ec7ee…`: MOD-Y1 with fix E and the CM12 charger manager) and `dtbo_a` DK1-TH2 (`c87ece8c…`). For the current state and how to restore each step, see `out/dp-evidence/test42/RESTORE.txt`.

**T-01 What the logs confirm (last boot on record, 01:33-01:37).** Slot _a active ("androidboot.slot_suffix=_a"); adb listening on TCP 5555 after a reboot; SELinux permissive from the boot command line ("androidboot.selinux=permissive" in the logcat ENGPC cmdline token); Magisk running at boot ("01:33:04.729 ... Magisk  : * Mounting mirrors"; when it was added is not shown); the c2c dtbo booting with the key rejected (S-12). [C]

**T-02 What only the record says.** Verity disabled (`adb disable-verity`); `vendor_dlkm_a` holds the 1440-only driver (nothing shows it reverted after 00:53); `persist.gammaos.display.mode=mirror` (each Mirror/override log begins with the line "mirror", which supports it for 01:08-01:20); debugfs EDID overrides are lost on reboot; `persist.adb.tcp.port=5555`. [I]

**T-03 Last recorded state.** Graceful shutdown at 01:37:07 with the Cube still on the dock, charging, data role host (K-01); the Cube was started again before 01:39:53 (file pull), and that boot is not logged. [C]

**T-04 How to restore (not done; each step needs device access and the owner's go-ahead).** [I]

| Item | Current (per record) | Restore |
|---|---|---|
| sprd-drm driver | 1440-only `vendor_dlkm_a` (sha256 `1a35d085...`) | write back `out/dp-evidence/vendor_dlkm_a-test41.img` (sha256 `ec24e7bbd9076970f296d5ad1e54a3d75c36b536fc9fa764b15f176e19fd03c2`) to `vendor_dlkm_a` the same way the 1440-only image was written (dd, user-approved), and check the hash; or run an installer Update of test 41, which rewrites `super` (where `vendor_dlkm_a` is expected to live as a logical partition; not verified) |
| dm-verity | disabled | `adb enable-verity`, then reboot |
| Display mode | `persist.gammaos.display.mode=mirror` | set it back to the mode the owner uses; TESTS 1-5 ran in Docked mode and the build 36/37 session showed "dual" (D-39); the value before TEST 6 was not recorded |
| EDID override | lost on reboot | nothing to do after a reboot |
| adb over TCP | `persist.adb.tcp.port=5555` | clear the property if wireless adb should not persist |
| dtbo | test 41 c2c dtbo on both slots (installer Full install writes dtbo_a/b, S-07) | stock `dtbo_a.img` is in `out/.dock/` (from before the build 36/37 hand edit, D-08) |

**T-05 The build 36/37 hand-edited dtbo_a** was overwritten by the test 41 Full install, which writes dtbo_a and dtbo_b (S-07); if test 40 was flashed at 15:10 (S-43), it was already replaced then. [I]

---

## 10. Open questions and next experiments

**Q-01 Mode-switch clock (R-02).** Make every DP mode change go through a full CRTC disable/enable (or call the clock setup in the switch path), then re-run TEST 1 with the stock mode list. Expected: the 138.5 MHz 1080p at least gets a `dpu_clk_init` line, which tests R-01's PLL question directly.

**Q-02 241.5 MHz (R-03).** Find what DPI clock sources exist and whether one can produce 241.5 MHz; check what the "use default DPI_CLK_SRC" path actually programs.

**Q-03 The width question (R-06).** Log a 1080p120 try + keep (M-28) so the 1920-wide-at-297 success is on record; then try a 2560-wide mode at 148.5 MHz-divisible clocks within the monitor's range, and a non-bypass 3840 mode, to separate a DPU1 line-width limit from the monitor rejecting custom timings. The 17 prepared EDIDs in `research-modes/edids/` (M-31) are candidates.

**Q-04 Does DP depend on PIXELPLL at all (R-04)?**

**Q-05 The 4K start (R-05).** Why does the composer pick 3840x2160 bypass as config 0, and does the monitor show anything in those 3.2 s? Capture logcat (ChosePreferredConfig) on a stock-driver connect.

**Q-06 Unplug panics (R-19).** Test a guarded IRQ handler (`work-irq-guard/sprd-drm-test41-irqguard.ko`, not yet reviewed) against repeated unplugs; PANIC A is intermittent (P-08), so it needs many unplugs per build. Separately, stop the composer from committing to an unplugged connector after the VPU domain is off (PANIC B).

**Q-07 TEST 1 lock-up (P-02).** Reproduce with no cable attached at the forced restart, so pstore survives (S-36).

**Q-08 Test 41 dock host start (R-14).** Why is the host event not retried after "charge detect finished"? Does Nano's MTP switch (K-07) interfere? Test with Nano's USB function handling paused.

**Q-09 Dock DP training (R-15).** Once host starts through c2c, does training succeed at the first attempt as it did with debugfs? Does a DR swap after DP is up turn DP off (R-17)?

**Q-10 Wi-Fi (R-18).** With a successful dock training (3 waits), do the Wi-Fi asserts disappear? Keep a full dmesg (WCN/wlan lines) through the training window; try moving the dw_dptx IRQ thread off CPU0.

**Q-11 PD details.** Re-run the dock with printk at its default so the TCPM log prints (S-32): the dock's source caps, the requested PDO, and why it sent a hard reset at attach (K-03).

**Q-12 The plan's other open questions** (plan section 10): AW32257 REG06 limit (Q4), off-mode charging hooks (Q5), slot switching (Q6, Q11, Q12), the other T820 devices (Q7), the flipped plug with a dock (Q8, D-45), charger-manager reacting to USB=0 while sinking in c2c (Q10), IRQ_HPD on an untrained link (Q13). TODO.md lists the c2c dtbo device test as not yet run; the test 41 dock run (section 5) is that test, with the result: charging and swap yes, host and DP no.

**Q-13 868 mA (C-05).** Read the AW32257 charge-current register to see which code is set.

**Q-14 Build 36/37 gaps.** PD logs of v1 and v2 (D-17, D-18); the raw USB2 failure (D-40); `retoggle.out` (D-41); logs of the HDMI-only dongle and the test 36 dongle (S-20, S-21); the hub's 400 mA request (D-11); the DR_Swap-every-1.6 s log (D-14); how dtbo_a was written and which reboot loaded which variant (D-16, D-20); outputs of `udc.sh` and `usb2retry.sh` (D-32); hostfix v1's script and who wrote host at the 07:06 replug (D-38); the cause of the 23:07:47 adb drop (D-52); the outputs of `dtcheck.sh` and `sinktest.sh` (D-14, D-16). None of these exists anywhere.

**Q-15 Install.** Why the first test 41 attempt stopped (S-08), and whether test 40 was ever flashed and booted (S-43).

---

## 11. Evidence index

Paths are relative to the repository root. Files marked "session scratch (not kept)" were in a temporary working folder of the debugging session and are no longer available; the findings quote what mattered from them. The installer used was ThimsyInstaller 0.1.0 for Windows x64.

### 11.1 Build 36/37 (`out\.dock\`)

| File | Holds |
|---|---|
| `out\.dock\FINDINGS.md` | the session's own notes: overrides, DT variants, dongle results |
| `out\.dock\uname.txt`, `cmdline.txt`, `getprop.txt`, `partitions.txt`, `mounts.txt`, `proc_modules.txt`, `module_dir.txt`, `modules*` | device identity and software (S-01, S-03) |
| `out\.dock\klog-1.txt` | full kernel log 06:23:51-06:33:21 UTC, DT fix, no override (D-21 to D-31) |
| `out\.dock\pdlog-1.txt` | PD/typec log and sysfs dump around the 06:28 replug (D-22 to D-26) |
| `out\.dock\dock-watch.log` | dock-watch records 23:0x-23:15 local with grepped PD/DP lines, lsusb, hostfix tail (D-34 to D-42) |
| `out\.dock\panic-dmesg.txt` | pstore of the debugfs panic (P-01) |
| `out\.dock\dtb_a/b.img`, `dtbo_a/b.img`, `vbmeta_a/b.img` | backups before any change (D-08); `dtbo_a.img` is stock, `dtb_a.img` is all zeros |
| `out\.dock\dtbo-c2c.img`, `live.dtb`, `vendor_boot_a.img` | the later c2c dtbo, the live DT, vendor_boot (D-57) |
| `out\.dock\work\dtbo-a1\stock-vs-live.diff`, `live.dts`, `merged-c2c-vs-live.diff`, `merged-c2c.dts` | DT diffs (D-06, D-16, D-57) |
| `out\.dock\work\build-integration\dtbo_usbc.py`, `try.sh` | dtbo rebuild and re-sign checks (D-58, D-59) |
| `out\.dock\work\dock-matrix\vl103\vl103.txt` | VL103 datasheet text (S-16) |
| `out\.dock\work\dp-driver\patch\drm_hpd.S`, `dp_pin.S` | DP-B and pin patches (R-07, R-08) |
| `out\.dock\work\usb_c_dock_plan.md` and repo `usb_c_dock_plan.md` | the dock plan: R1-R12, matrix, open questions (section 7) |
| session scratch (not kept): `cube-display\typec-1.txt`, `typec-kernel2.txt`, `typec-watch.log`, `prswap-1.txt` | original DT, Cube as source, role experiments (D-01 to D-15) |
| session scratch (not kept): `cube-display\watch.log`, `hpd2.log`, `dpwarn-1.txt`, `dock-watch.log` (longer copy with the 23:30 record) | host-side watchers (D-20, D-39, D-45, D-50) |
| session scratch (not kept): `cube-display\hostfix.sh`, `retoggle.sh`, `dwc3host2.sh`, `hostrole.sh`, `forcec.sh`, `pinc.sh`, `reenter.sh`, `redetect.sh`, `udc.sh`, `usb2retry.sh`, `sinktest.sh`, `dump.sh`, `*.ps1` | the exact override and watcher scripts (D-32 to D-34, S-27). `hostfix.sh` on disk is v2 (file time 23:13:50); v1 was not kept |
| session scratch (not kept): `cube-display\dt.sh`, `dt2.sh`, `dt3.sh`, `dt4.sh`, `dtcheck.sh`, `dwc3host.sh`, `dpres.sh`, `hpdres.sh`, `ptn.sh`, `fresh.sh`, `collect.sh`, `prswap.sh`, `pdlog.sh`, `dpwarn.sh`, `typec.sh`, `state.sh`, `state2.sh`, `snapshot.sh` | read-only helpers: DT and connector dumps (D-16), the debugfs mode-file finder (`dwc3host.sh`, mounts debugfs), the per-record capture that dock-watch ran (`dpres.sh`), the redriver check (`ptn.sh`), the file collection into `out\.dock` (`collect.sh`) |
| session scratch (not kept): `cube-display\typec-kernel.txt`, `typec-log.txt`, `snap-1.txt`, `hpdwatch.log`, `panic-console.txt`, `screen-1.png`, `screen-2.png` | earlier captures (21:52-22:05 local), an empty HPD watch, the console-ramoops half of the debugfs panic (P-01), two screenshots |
| `display_modes_plan.md` | display-mode rules, PIXELPLL formula, section 9 findings (D-60, R-01) |
| GitHub releases `v1.0a-test36`, `v1.0a-test37`, `v1.0-test40`, `v1.0a-test41` | release notes (S-04, section 2.1) |

### 11.2 Test 41 modes and panics (`out\dp-evidence\`)

| File | Holds |
|---|---|
| `dmesg-run2-ring.txt` | TEST 1 kernel ring, stock driver, Docked; adapter enumeration |
| `dumpsys-display-run2.txt` | Android display state in TEST 1 (M-04) |
| `dmesg-try1440.txt`, `try1440.log` | TEST 2 trial and the stock mode list (M-03, M-14 to M-16) |
| `dmesg-1440only.txt`, `list-1440only.txt`, `plug1440.log`, `logcat-640.txt` | TEST 4 (M-18 to M-21); `logcat-640.txt` also covers TEST 5 and the 00:55 boot |
| `dmesg-try640.txt`, `try640.log` | TEST 5 (M-22) |
| `dmesg-mirror.txt`, `logcat-mirror.txt`, `mirror.log` | TEST 6 (M-23, M-24) |
| `dmesg-edid.txt`, `logcat-edid.txt`, `edid.log`, `edid-1080p60cea.bin` | TEST 7 (M-25) |
| `dmesg-1080p120.txt`, `e120.log`, `edid-1080p120cea.bin` | TEST 8 (M-26, M-27) |
| `dmesg-1440at297.txt`, `e1440.log`, `edid-1440p60at297.bin` | TEST 9 (M-29) |
| `dmesg-1440tall.txt`, `etall.log`, `edid-1440p60at297-tall.bin` | TEST 10 (M-30) |
| `edid-H27S17-first256.hex` | the monitor's EDID (S-22 to S-24) |
| `pstore-reboot3.txt`, `pstore-crash4.txt`, `pstore-crash5.txt`, `pstore-dock.txt` | PANIC A, B, A', C (section 6) and boot-reason histories (M-01) |
| `sprd-drm-test41.ko`, `sprd-drm-1440only.ko`, `vendor_dlkm_a-test41.img` (rollback), `vendor_dlkm_a-1440only.img` | drivers and images (M-02, T-04) |
| `README-1440only.txt`, `README-root-cause.txt` | the session's own notes (corrected in M-19, R-04) |
| `research-modes\edids\` | 17 prepared, unrun override EDIDs (M-31) |
| `work-irq-guard\text.dis`, `syms.txt`, `sprd-drm-test41-irqguard.ko` | disassembly and symbols used for P-04, P-07, P-09; an unreviewed guard build |
| `work-impl\`, `work-no-4k-start\`, `work-safe-start\`, `work-modeswitch-clock\` (14 files, including `stock.ko` and `test41-modeswitch.ko`; correction: not empty) | fix work folders, not reviewed |

### 11.3 Test 41 dock and install

| File | Holds |
|---|---|
| `out\dp-evidence\dock-tv-run\dmesg.txt`, `logcat.txt`, `state.txt` | the logged dock run (section 5) |
| `out\dp-evidence\dock-research\device-readonly-2026-11-25.txt` | read-only device facts: ptn38003a unbound, IRQ thread priorities and affinity (K-10, R-18) |
| `out\dp-evidence\dock-research\dptx_phy_wait_busy.dis` | the busy-wait loop (R-18) |
| installer `jobs\anbernic-rg-cube-heavy\job.json`, `plan.json`, `state.json` | the test 41 job (S-05 to S-07, S-09) |
| installer `jobs\anbernic-rg-cube-heavy\backup\run-20261124-231055\`, `run-20261125-074735\`, `run-20261125-075216\` | three backup runs (S-08 to S-10, S-43) |
| installer `jobs\anbernic-rg-cube-heavy\out\anbernic-rg-cube-thimsy-heavy.pac`, `Thimsy-1.0a-test41-anbernic-rg-cube-heavy.tar`, `cache\RG_Cube_GammaOS_Next_Full_v1.2.7z.00*` | package and cache (S-06) |
| installer `engine\tools\Log\UartComm_COM_8_2026_11_24.log` | spd_dump session banners and the 15:11 COM errors (S-08, S-09, S-44) |
| installer `pack.json` | "Thimsy 1.0 test 40" (S-43) |

### 11.4 Gaps in the evidence

- No logcat for TESTS 1, 2, 9 and 10; no log at all for TEST 3, the TEST 8 keep/forget, or the TEST 1 lock-up.
- Which 297 MHz 3840x2160 mode (24 or 30 Hz) the composer chose cannot be told from the kernel lines.
- What ran before PANIC A (the boot before 00:32) and before PANIC A' is not recorded beyond the pstore window.
- The dock and TV models are not identified; the dock's PD negotiation was not logged.
- Every "picture" / "no signal" judgement is the tester's.
- Not reviewed: the `work-*` fix builds in `out\dp-evidence\`.
- Not checked against any log: the record's claim that `display_modes_plan.md` section 9 calls confirmed, "1920x1080@120 CEA showed a picture" (R-20, M-28).


## 12. Test 42 experiments, 2026-11-25 to 2026-11-26 (Phase A and B, then the device tests)

Evidence: `out\dp-evidence\test42\` (one folder and `.log` per run; `exp2.sh` streams the kernel log for the whole run). Monitor: the 1440p monitor through the DisplayPort-only adapter unless noted. Mirror mode unless noted.

**Driver on the Cube.** Phase A ran on `vendor_dlkm_a-1440only.img` (`1a35d085…`), which differs from test 41 by one word (M-02), not on the test 41 image as planned. Phase B runs on MOD-P (`vendor_dlkm_a-MOD-P.img`, `5343cbf9…`; module `07c4bb84…`): test 41's patches, fixes A-D, and P2, FLIP and MP.

| Run | Setup | Result |
|---|---|---|
| A0 | CEA 1080p60 EDID | Picture, 2 min. `pixelclock is 148500000`, link rate=2 lanes=2, 0 HPD drops. |
| A1 | Same, Docked | Switching to Docked live: panel off, `PMU_APB_PIXELPLL pw off`, monitor kept 1080p60 for ~9 min. Re-plug while Docked: picture, 5 min, 0 drops. PIXELPLL power-off does not affect DP. |
| A2 | CEA 1080p120 EDID; `try 1920 1080 120` + `keep` | Picture, OSD 1920x1080 120 Hz, 7.5 min, 0 drops; `pixelclock is 297000000`, tu 52/8; SurfaceFlinger at 8.33 ms vsync. One `no composer vsync from monitor 1` 17 s after the switch (also once in A1), not repeated. The 640x480 detour after connect ran at the 120 Hz clock (mode change without clock reinit, the bug fix D addresses). |
| A3 | CEA 720p60 EDID | Picture, OSD 1280x720, 2 min; `pixelclock is 74250000`, no underflow. |
| A7 | `dri/0/state` | DPU1 (dispc1) has 4 planes (plane-6..9); DPU0 has 6. At 720p a stale 1920x1080 composer layer stayed attached to plane-7 beside the 1280x720 one. |
| A4 | 4K TV, HDMI-only adapter; 4K30 CEA EDID, `ro.gammaos.display.uhd` set live by resetprop (the framework reads it per hotplug, no restart needed), HW overlays off | Bypass at 3840x2160, `pixelclock is 297000000`, tu 52/8, link rate=2 lanes=2, kept 90 s: **no picture**. |
| A4c | Same adapter and TV, CEA 1080p60 | Picture. So A4's failure is the 4K bypass path with the DPTX in single-pixel mode, not the adapter. |
| B1 | MOD-P, CEA 1080p60 | Picture, 2 min, normal mode, 148.5 MHz. P2 leaves 1920 and narrower alone. |
| B2 (auto) | MOD-P, y1c 1440p EDID, Auto | Auto chose a **1920x1080 148.5 MHz mode with negative syncs** (DRM flags 0xa) that the y1c EDID's range descriptor adds and fix C made preferred; the monitor dropped HPD 3-10 s after each enable, six times. 1440p never ran. |
| B2 | MOD-P, y1c EDID, `try 2560 1440 60` + `keep` | `bypass_mode`, `pixelclock is 297000000`, tu 52/8, fix D's disable/enable ran. DRM state: one plane on DPU1, 2560x1440 AB24, from the composer. **The monitor locked on (OSD 2560x1440 60 Hz) but the picture was blank.** DISPC1 interrupts kept counting. |

**Other findings from this session**
- Reading the files of `/sys/class/display/dispc1/` in a loop (while B2 ran) caused a **CFI-failure kernel panic** (pstore `B2-hang\dmesg-ramoops-0.txt`: "Kernel panic - not syncing: CFI failure", then list corruption in `dpu_isr` → `__wake_up`). Self-inflicted, not caused by 1440p. Never read those files.
- `stop; start` over adb with a monitor connected at 720p locked the Cube up (no panic record; `lockup-0900\`).
- **Booting with a monitor connected crash-loops system_server** (`B2-hang\logcat-afterboot.txt`): 158 restarts in 13 min, until the monitor was unplugged. Two NullPointerExceptions in AudioService under Nano's minimal boot: `onAccessoryPlugMediaUnmute` (NotificationManager is null) and `postAccessoryPlugMediaUnmute` → `sendMsg` (the audio handler is null). The DP sink's audio device connects before those exist. Settings and wireless debugging can't be reached meanwhile. Test 42 must fix this. *Update (test 42 release review, 2026-11-27): not fixed in test 42 (no change touches `onAccessoryPlugMediaUnmute`); a known issue, listed in the test 42 release notes.*
- With the monitor's own EDID, fix C picked the monitor's **138.5 MHz** 1080p60 (`pixelclock is 138500000`): signal but blank, as the clock rule predicts (138.5 does not divide 297). The test 42 mode table, which replaces fix C, avoids it.
- Re-plugging a monitor in the same mode skips `mode_set_nofb` and `encoder_mode_set`; only `atomic_enable` (with `dpu_clk_init`) runs. Anything set in encoder_mode_set (MP's pixel mode) must survive a DPTX power cycle, which B6 has to check.
- An unexplained HPD connect with nobody touching the cable (03:08, `A0-glitch-0308\`).
- `dptx_phy_wait_busy: PHY BUSY timed out` appears 3 times per link training in every run, passing ones included.

**State of the Cube after this session:** MOD-P in `vendor_dlkm_a`; restore with `vendor_dlkm_a-test41.img` (`ec24e7bb…`) or a test 42 install. Runtime settings used (EDID override, `ro.gammaos.display.uhd`, SurfaceFlinger 1008) clear on reboot. `out\dp-evidence\test42\RESTORE.txt` tracks it.

### 12.1 Phase B on MOD-Q (2026-11-25, late morning)

The research (`out\dp-evidence\test42\bypass-research\SYNTHESIS.md`) found why B2 was black: DPU1's bypass scans out **hardware layer 7 only**. Unisoc's 4.14 `dpu_lite_r3p0.c` says "bypass mode only use layer7" and sets `layers[0].index = 7`; the 5.4 port in the Cube's module writes `planes[7].state->layer.index = 0` instead (an out-of-bounds read of DPU1's 4-plane array, and the wrong value), so the single layer stays on hardware layer 0 and the screen shows the black background. A4's 4K failure had the same cause, so A4 did not show whether MP is needed. Bypass was built for 3840x2160@60 YCbCr 4:2:0 video; RGB at 2560x1440 works once the layer index is right.

MOD-Q (`modq\vendor_dlkm_a-MOD-Q.img`, `5091c0a1…`, module `275511dc…`) = MOD-P with FLIP replaced by FLIP7 (`planes[0]`'s layer index = 7 in bypass), TH40 (DPTX init threshold 40 in dual-pixel mode) and BG (diagnostic red/blue DPU1 background). Three independent reviews passed it before the write.

| Run | Setup | Result |
|---|---|---|
| B2 (MOD-Q) | y1c EDID, `try 2560 1440 60` + `keep`, Mirror | **Picture at 2560x1440 60 Hz**, colours, sharpness and stability judged fine; 5 min, 0 drops; `bypass_mode`, 297 MHz, tu 52/8; no underflow or layer messages. |
| B4 | Same, Docked | Picture at 1440p for 5 min, 0 drops; the panel's DPU0 stops and `PIXELPLL pw off` is logged with no effect on DP. |
| B5 | y5 EDID (CEA 1080p60 + 1440p), 5 × try/revert in Mirror, then in Docked | All 19 switches that reached the Cube worked (one try lost to a Wi-Fi adb drop); every switch is a full disable/enable (fix D) with the right mode, clock and normal/bypass; no errors. |
| B6 | 10 re-plugs (monitor end, then the adapter), 1440p re-applied after each by a watcher on the Cube | Every re-plug trained the link and gave a picture; every 1440p switch issued worked (the first watcher sometimes fired before Android had added the display, a harness issue); no panic, pstore unchanged, no `nobody cared`, dw_dptx interrupts 31 in total. |

Notes:
- Each re-plug restarts adbd (and once Wi-Fi disconnected), so Wi-Fi adb drops for seconds to minutes; test harnesses that must react to a plug have to run on the Cube.
- The negative-sync 1080p148.5 that B2 (auto) blamed for HPD drops showed a picture when B4 reached it by a mode switch, so sync polarity was probably not the cause of B2 (auto)'s drops. Unexplained; moot for test 42, whose table has only CEA 1080p60.
- FLIP7 and TH40 are now in `scripts/sprd_drm.py` (WIDE and WIDE_MP); `patch_module(stock, True)` equals MOD-Q without BG (`b12699c6…`).

### 12.2 MOD-M and the dock dtbo on the Cube (2026-11-25, afternoon)

MOD-M (`modm\vendor_dlkm_a-MOD-M.img`, `ad201ffb…`, module `8648b533…`) and the DK1 dtbo (`dock\dtbo_a-DK1.img`, `1b727faa…`) were written with the owner's approval; both read back correctly and the Cube booted cleanly (usb3 extcon `<0x64 0x64>`, no pstore record). Old dtbo backed up in `test42\backup\` (test 40, `94c0f3d4…`, both slots).

| Run | Setup | Result |
|---|---|---|
| B7 | MOD-M, the H27S17's own EDID, Docked → 1080p120 → 720p60 → 1440p60 → Auto → Dual → Mirror (script on the Cube) | The connector lists exactly the table: 1080p60 148.5 (type 0x68, the only preferred), 1080p120 297, 720p60 74.25, 2560x1440 297 (y1c). Connect came up at 1080p60; every step showed a picture. The monitor's 138.5 and 241.5 MHz modes are gone. |
| B9 | 20 unplugs: 5 at the monitor end, 5 at the adapter, 5 with the Cube asleep, 5 with the monitor's power off | A picture after every re-plug; every link training succeeded; dw_dptx interrupts 1 → 28; no oops, no `nobody cared`, pstore unchanged. |

**The monitor has no identity for the framework** (`key none` in `gammaos-monitor-mode list`), so a per-monitor stored mode does not persist; test 42 stores the Display Modes choice globally. *Update (test 42 release review, 2026-11-27): test 42 kept per-model storage (`monitorKey`); a monitor with key none, like this one, keeps its Output Resolution choice only until it is unplugged (README, Output Resolution).*

**Sleep with a monitor attached (new).** A power-button press with the monitor connected puts Android to sleep (`Going to sleep due to power_button`; SurfaceFlinger "Setting power mode 0 on display 1"), but DPU1 stays active (`dri/0/state`: dispc1 `active=1`) and the monitor keeps showing the picture, so to the user nothing happens. The vendor composer does not turn the external display off on power mode OFF. Not yet investigated.

| B8 | MOD-M, Hisense 65" 6 Series TV through the HDMI-only adapter | A picture at 1920x1080 60 Hz, Wi-Fi up, typec role host. The connector lists only 1080p60 (preferred) and 720p60, as the table's gates predict for this EDID. The TV's EDID (`test42\B8\edid.bin`): preferred DTD 3840x2160@30 at 297 MHz, VDB 95/93/100/98/16/34/32/4/62/60/2/1, **YCbCr 4:2:0 VDB with VIC 97 and 102** (4K60 is 4:2:0-only), HDMI VSDB max TMDS 300 MHz. |

**Correction to A4 (and to section 12.1's reading of it).** A4 never presented a frame, so it says nothing about bypass, layer 7 or the pixel mode. At an external mode exactly 3840 wide, the vendor composer's `SprdDrm::AddFlushData` (0x5f610-0x5f700) keeps only layers whose integer source-crop width (SprdHWLayer+1196) is 3840. `SprdDisplayClient::SET_CLIENT_TARGET` builds a fresh layer whose integer crop is zero, and only the primary and virtual displays' commits fill it in, so with HW overlays off (client composition) every frame is dropped. A4's logcat has 3633 `present failed for display 1` lines from the 3840x2160 setActiveConfig to the revert, about one per frame; A4c and B2/B2q/B4 have 0-2. With HW overlays on, `updateGeometry` (0x4ec6c) forces DPU composition and full-screen 3840-wide layers survive. So a 3840-wide test must run with HW overlays **on**, and shipping 4K30 for the UI needs either a composer patch (fill in the external client target's integer crop, or keep every layer in AddFlushData) or a SurfaceFlinger rule that sends one full-screen device layer (`test42\y420\SYNTHESIS.md`, MOD-K review). Found by the MOD-K system review (2026-11-25).

| K1 | MOD-K (`modk\vendor_dlkm_a-MOD-K.img`, `40de4f7e…`, module `cdbe8bd9…` = MOD-M + UHD), Hisense TV, HDMI-only adapter, `ro.gammaos.display.uhd` by resetprop, **HW overlays on**, `try 3840 2160 30` + `keep` | The connector lists 3840x2160@30 (297 MHz, type 0x60), 1080p60 (preferred) and 720p60; the TV came up at 1080p60. At 4K30: `bypass_mode`, `pixelclock is 297000000`, tu 52/8; 4 `present failed` during the switch and none after; DISPC1 interrupts running. **Picture at 3840x2160 30 Hz** on the TV. |
- K1 detail: at 3840x2160@30 with HW overlays on, SurfaceFlinger sends the TV one DEVICE layer (0,0 3840x2160); the Quick Menu shows, as Nano draws it in its own full-screen surface. Other apps' pop-ups (IME, toasts, system dialogs) are separate layers and are expected to be dropped by the composer's 3840-wide rule; not yet tried.

| Y1 | MOD-Y1 (`mody\vendor_dlkm_a-MOD-Y1.img`, `0aca9eca…`, module `9ae14445…` = MOD-K + Y420 without the flip guard), Hisense TV, HDMI-only adapter, uhd by resetprop, HW overlays on, `try 3840 2160 60` + `keep` | The connector lists 3840x2160@60 (594 MHz, type 0x60) beside 4K30, 1080p60 (preferred) and 720p60; the TV came up at 1080p60 (link HBR2 x2). At 4K60: `bypass_mode`, `use default DPI_CLK_SRC`, `DPU_CORE_CLK = 512000000, DPI_CLK_SRC = 297000000`, `pixelclock is 594000000`, tu 52/8; 5 `present failed` during the switch; DISPC1 interrupts at about 60/s; HPD steady. **The TV shows 3840x2160 60 Hz with Nano's UI in wrong colours**, as expected for RGB data sent as YCbCr 4:2:0: link, timing, MSA/VSC and single-pixel 4:2:0 work, and the adapter passes 4:2:0. |
- Y1 detail: back at 1080p60 (Auto; `pixelclock is 148500000`, tu 26/4) the colours are normal again, so the RGB enable clears the VSC SDP and MISC1. A game at 4K60 also showed wrong colours, as expected: DPU1 (dpu_lite_r3p0) has a YUV-to-RGB converter per layer (y2r) but nothing from RGB to YUV and no colour matrix (the `enhance` colour matrix belongs to DPU0, the panel), so only an NV12 layer can show correct colours at 4K60.
- Y1 stage 1.3 (script `test42\y1_cycles.sh` on the Cube): 20 x (try 4K60, revert): every trial reached 3840x2160@60 and every revert 1920x1080@60, HPD connected throughout; then 4K60 -> 4K30 -> Auto, each in the right mode. No HOT_UNPLUG, oops, underflow or `layer count error` in the kernel log; about 3 `present failed` per switch (61 over the run), none in steady state.

| T2-sleep | MOD-Y1, the patched composer (`scripts/sprd_hwc.py` SLEEP + CT4K, `86ea7072…`) bind-mounted over `/vendor/lib64/hw/hwcomposer.unisoc.so` with the TV unplugged, `ctl.restart vendor.hwcomposer-2-4`; Hisense TV at 1080p60; power press, then power again | **The TV went dark and came back.** SurfaceFlinger power mode 0 on display 1 -> `Excessive delay in drm_blank(): 12ms, dpms_value:3` -> `sprd_dp_encoder_disable`, `dp suspend OK`, `sprd_dpu_atomic_disable`; wake: power mode 2 -> `dpms_value:0` -> `sprd_dpu_atomic_enable`, `sprd_dp_encoder_enable`, back at 1080p60. |
| T2-ct4k | Same; `try 3840 2160 30` + `keep`, `service call SurfaceFlinger 1008 i32 1` (HW overlays off: the A4 setup), `debug.hwc.info 1` | **Picture at 3840x2160 30 Hz.** SurfaceFlinger sends one CLIENT layer (0,0 3840x2160); the composer logs `Total layer: 1, FB layer: 1` and `AddFlushData disp: 1, LayerCount: 1` every frame (452 in about 15 s); 6 `present failed` during the switch, none after (A4: 3633). So CT4K lets the client target through at 3840, the route the NV12 output will use. |

**Bug found after T2-sleep: the sleep fix makes the DPTX interrupt storm, and the kernel disables it.** At 5184.29 s, right after the wake (`dp suspend OK` at 5184.04 s), the kernel logged `irq 76: nobody cared` and `Disabling IRQ #76` (dw_dptx, GICv3 155, handlers `sprd_drm` hard + threaded); `/proc/interrupts` then read 100001 for it (earlier runs: 1-31). With the composer's sleep fix, a power-button sleep turns DisplayPort off by DPMS (`sprd_dp_encoder_disable`, `dp suspend OK`) while the monitor is still connected, so HPD stays asserted on a level interrupt; the IRQ guard (fix B) sees both power flags clear and returns IRQ_NONE without touching the controller, so the line never clears. The unplug path (pd_event) masks the Type-C interrupt before DisplayPort's last clock goes off; the DPMS path does not. Until a reboot the dw_dptx interrupt stays disabled. Fix needed in sprd-drm.ko before the sleep fix ships: mask (and on enable unmask) the DPTX/Type-C interrupt in the encoder disable/enable path as pd_event does, or have the guard disable the line while DisplayPort holds no clock. The 4K30 CT4K run and the 1080p picture after the wake worked because no new HPD event was needed.

**Fix E for it, run on the Cube (irqfix run 1, 2026-11-26 21:48; `out/dp-evidence/test42/irqfix/NOTES.md`): steps 1, 2, 3a and 4 PASS** (sleep and wake, 9 quick sleep cycles, unplug at the monitor end while asleep, replug while asleep through the dock; no `nobody cared`, no `Disabling IRQ`, no IRQ storm); step 3b (unplug at the adapter or Cube end while asleep) and a deep-suspend sleep were not run (`scripts/sprd_drm.py` IRQ_MASK_PATCHES). A correction to the reading above: pd_event's clock reference stays held through a DPMS sleep (0xbb9 is cleared only by an unplug), so the guard lets the vendor handler run, and the storm left no log lines; its IRQ_NONE is then the vendor's own for ISTS[7:0] == 0, from a source above bit 7 that IEN's reset value enables **[I]** (the raw T2 kmsg is not on disk). Fix E writes IEN 0 in the encoder's disable (clocked), 0x15 | Type-C-if-pd_event at the end of its enable, 0x15 or 0 in pd_event off, and exactly 0x95 in dptx_core_init. Test image `irqfix/vendor_dlkm_a-MOD-Y1-E-CM12.img` (`a96ec7ee…`, module `6eefac58…`): MOD-Y1-CM12 with only sprd-drm.ko changed, in fix E's 27 words. If S1 there still storms, ship the composer without SLEEP (`COMPOSER_FIX=ct4k`).

| DK1 | MOD-Y1 (stock composer after a reboot), the DK1 dtbo (`dock\dtbo_a-DK1.img`, `1b727faa…`, fix A), the multi-function dock with its charger, the H27S17 1440p monitor, a Logitech receiver and a 2.4 GHz receiver | **Picture at 1080p60** (Auto; the list offers 2560x1440@60, 1080p120, 1080p60, 720p60). Type-C: data role host, power role sink, PD 3.0; USB 3.0 hub (2109:0817) at 5 Gbit/s on usb3, USB 2.0 hub (2109:2817) with both receivers and the dock's billboard device on usb2; **the USB devices work; the Cube charges**; Wi-Fi up and the gateway pinging throughout; dw_dptx interrupts 1. So fix A works. `patch_dtbo.py`'s tested_sha256 is now `1b727faa…`. |

**Charging through the dock is slow (about 2.4 W).** The dock offers PD PDOs 5 V/1 A, 9 V/1.66 A and 12 V/1.25 A; the Cube requests PDO 0 (5 V, 1 A, flagged `[mismatch]`, its sink caps want more current), and then charger-manager classifies the port by BC1.2 as SDP (`chgr type= 1`) and limits input to 500 mA (`inpt_lmt_cur= 500000`), so about 0.2 A reaches the battery at 4.83 V. Two separate limits: the SDP cap ignores the PD contract's 1 A, and only 5 V is requested although the Cube's charger may accept 9 V (`fchg_en= 0`).

| CH1 | The dock's charger plugged straight into the Cube (no dock), stock charging code | PD 3.0 sink; the charger's 5 V contract is 3 A (`sprd-tcpm-source-psy` current_max 3000000, PD_PPS listed); charger-manager `chgr_type: 0`, `inpt_lmt_cur: 2050000`, **`chg_lmt_cur: 868000`**; vbus 4.79-4.80 V; ibat about 0.93-0.96 A at 3.71 V (about 3.5 W into the battery, about 4.6 W input); `is_fchg: 0, fchg_en: 0` (no 9 V). So even straight from a 15 W+ charger the stock setup charges at about 0.9 A: the charge current limit (868 mA) is the binding limit, not the input. Through the dock the SDP 500 mA input limit binds first. |

| T1 | `thimsy_yuvtest` (`out/dp-evidence/test42/y420/nv12/gralloc-mali/yuvtest`, built in the AOSP tree, `45545d48…`) on the Cube, no display involved | **PASS on the path SurfaceFlinger uses** (EGL_ANDROID_image_native_buffer + GL_EXT_YUV_target, Mali-G57 r40p0). Gralloc gives HAL 0x15 at 3840x2160 as linear NV12 (fourcc NV12, modifier 0, Y stride 3840, CbCr at 8294400 with stride 3840, 16-byte aligned: exactly the layout the composer computes); the NV12 buffer is a complete FBO attachment; colour bars come out as exact BT.709 limited range (white 235/128/128, black 16/128/128, red 63/102/240 ...) with both the explicit matrix and `rgb_2_yuv(itu_709)`; luma error at most 1; chroma takes the top-left pixel of each 2x2 (co-sited, no averaging). One 3840x2160 RGBA -> NV12 pass takes **1.45 ms** (from a linear or a GPU-compressed source). The dma-buf import path (not used) stopped after creating its FBO; EGL_EXT_image_dma_buf_import is not exposed on this EGL. |

**CM1 (dock at ~5 W), built for the device test.** `scripts/sprd_cm.py` (tests/unit/test_sprd_cm.py): in `cm_update_charge_info` the charger-type load at .text 0x21a4 becomes `bl 0x12040`, a 7-word cave in the never-registered `sprd_fchg_change` that returns type 0 (UNKNOWN, 1.05 A at 15-52 C) when the type is SDP and `is_pd_hub` (desc+912, set by sprd_tcpm for a PD hub or AMA, cleared on unplug) is set; every other type is unchanged. llvm-objdump (AOSP prebuilt) confirms the words. Module `0818274e…` (stock `ed5b042e…`). Test image `charge\cm1\vendor_dlkm_a-MOD-Y1-CM1.img` (`e74fab7e…`, 8159232 bytes; built in WSL with the AOSP host erofs tools because Docker stopped running containers overnight): only lib/modules/sprd-drm.ko (= MOD-Y1, `9ae14445…`) and sprd-charger-manager.ko differ from the test41 image, both byte-identical to the intended modules. Not shipped until the PD-hub contract risk (a hub/AMA with a 5 V PDO below 1 A) is covered by Nano or accepted. Recovery keeps the stock module.

**CM2 (direct PD charger toward 10 W), built for the device test.** `sprd_cm.py --pd10w` on top of CM1: for a type-0 (UNKNOWN) port without `is_pd_hub`, the cave looks up `sprd-tcpm-source-psy-sc27xx-pd` (through the module's own relocated calls to power_supply_get_by_name and power_supply_get_drvdata) and returns type 2 (DCP: 2.0 A at 15-52 C) only when the port's usb_type is PD/PD_DRP/PD_PPS, the supply voltage is 5000 mV, min(current_limit, req_current_limit) >= 3000 mA and explicit_contract is 1; otherwise type 0 as before. Offsets from sprd_tcpm.ko `b3ec27ea…`. REG06 (the AW32257 safety limit) is set by u-boot to 0xE7 (2356 mA, 4.34 V) every boot, so it does not cap 2.0 A. Module `fe3b3de4…`; image `charge\cm2\vendor_dlkm_a-MOD-Y1-CM12.img` (`3d161cb9…`, only sprd-drm.ko = MOD-Y1 and sprd-charger-manager.ko differ). Not for shipping until D12 (no re-check if a source lowers its offer mid-session).

**CM12 on the device (2026-11-26, vendor_dlkm_a = MOD-Y1-CM12 `3d161cb9…`).** Charge limits as designed (battery full in this first run, msoc 996-1000, so ibat stays at the CV taper; the real-power runs are in §12.3):
- Direct 5 V 3 A PD charger: chg_lmt_cur 1860000 / inpt_lmt_cur 2050000 (CH1 stock: 868000 / 2050000); dumpsys "Max charging current: 2000000". 1.86 A, not the ~1.98 A expected; cause not checked.
- Dock (PD 5 V 1 A, SDP, is_pd_hub): chg_lmt_cur 868000 (DK1 stock: 496000); aw32257 constant_charge_current 868000; inpt_lmt_cur 500000 is only reported (no input limit register). Logs: `charge/cm2/CM12-boot1.kmsg`, `CM12-dock1.kmsg`, results in `charge/cm2/NOTES.md`.

**DK2: the dock without power-in gives no picture.** With the monitor showing a picture through the dock, pulling the charger from the dock turned the monitor off (no crash; SF and system_server kept running). The Cube re-attached as source within 0.15 s (SNK_READY -> SNK_UNATTACHED 6782.32, SRC_ATTACHED 6782.48, "Requested 5000 mV, 900 mA for 900 / 900 mA", SRC_READY 6783.05), the dock's USB2/USB3 hubs and devices re-enumerated, and DP alt mode was entered and configured (Discover Modes VDO 0xc05, Status 0x1a, Configure 0x806). The dock never sent HPD high: no Attention after replugging HDMI at the monitor end, nor after a fresh Cube-first attach (7144.77 SRC_ATTACHED, same 900 mA request, Status 0x1a, Configure acked); card0-DP-1 stayed disconnected. With power-in (DK1) the same dock gives Status 0x1a at configure and then Attention 0x9a (HPD high) about 7 s later (DK1.kmsg 101.92 / 108.88). VBUS held 5.00-5.02 V and the Cube drew about -0.6 A from the battery. So this dock's HDMI side needs its own power (or more than the 900 mA source PDO; the AW32257 boost is rated 1 A, C-07). Logs: `charge/cm2/CM12-dock-unpowered{,2,3}.kmsg`. [C]

**T3: 4K60 4:2:0 with correct colours (2026-11-26 09:37).** `t3_prepare.sh` (TV unplugged) bind-mounted SurfaceFlinger `65438efb…` and the CT4K-only composer `dce32571…`, set ro.gammaos.display.uhd/y420, restarted the display stack (adb stayed up). TV (Hisense, HDMI-only adapter) came up at 1080p60; `cmd display gammaos-monitor-mode try 3840 2160 60` then `keep`. Owner: **colours are right at 4K60** (Y1 had wrong colours with RGB scanned out as 4:2:0). `dumpsys SurfaceFlinger`: HDMI output `thimsyYuv420=ClientTarget`, usesClientComposition=true, one CLIENT layer 3840x2160 at 60 Hz, three `ThimsyNv12` buffers 3840x2160 fourcc NV12 modifier 0 (linear), usage 0xb00, dataspace 0x104; no ThimsyYuv errors; missed frames 2 (HWC) / 0 (GPU) after the switch; GPU at 850 MHz. Evidence: `t3/T3-sf-4k60.txt`, `t3/T3-logcat-4k60.txt`, on-device watcher `/data/local/tmp/t3/T3-watch.log` (every 10 s). Note: the first `try` reverted after its 15 s trial because `keep` was not sent in time. [C]
T3 follow-up: owner reports menus, the Quick Menu and a game (RetroArch) all smooth at 4K60 with correct colours. The watcher covered 09:38:47-09:39:38 at 4K60 (RetroArch in front): thimsyYuv420=ClientTarget throughout, HDMI missed frames stayed at 2 (both from the mode switch), GPU missed 0, GPU 850 MHz, battery 29.1-29.2 C, ibat -0.64 to -0.67 A. By 09:39:46 the TV was unplugged (DP disconnected, no errors). The session was short; a long soak and the 4K60 <-> 1080p60 switch through the menu were not logged. `t3/T3-watch.log`. [C]

### 12.3 Charging power, DK3 and TH2 on the device (2026-11-26)

Evidence: `out/dp-evidence/test42/charge/cm2/NOTES.md`, `dk3/NOTES.md`, `thermal/NOTES.md`, `irqfix/NOTES.md`.

| Run | Setup | Result |
|---|---|---|
| CH2 | MOD-Y1-CM12, the 5 V 3 A PD charger straight into the Cube, battery 32-34 %, screen on (18:00) | ibat +1.72 A mean at 3.84 V, about 6.6 W net into the battery (stock CH1: about 0.95 A). System draw unplugged 1.3 W, so about 7.9 W reaches the charger and **an estimated** 8.8 W at the port at about 90 % efficiency (no input current sensor). [C] for ibat, [I] for port power. |
| CH2b | Same, the charger unplugged and plugged back | Charging resumed at once with the same limits (1860000/2050000), ibat 1.71-1.76 A. PASS. Only the direct charger was replugged here. [C] |
| CH2b dock | MOD-Y1-CM12, the multi-function dock with its charger, monitor on the dock (18:06, battery 35 %) | 5 V 1 A contract, SDP behind a PD hub -> CM1 limits 868000; ibat +0.76 A, 2.84 W net; **an estimated** 4.6 W at the port. Stock DK1: about 0.2 A. [C] for ibat, [I] for port power. |
| Dock on MOD-Y1-E | irqfix run 1 dock check (22:16), the shipping display driver | Picture and charging, 0.59 A. [C] |
| CH3 | MOD-Y1-E-CM12 + DK1-TH2, dock with charger, no monitor (22:08, battery 49-50 %, 29 C) | ibat +0.64-0.65 A, about 2.46 W net. So a powered dock gives about 0.6-0.75 A into the battery; the 0.76 A run was the high end. [C] |
| Dock charger replug | Same; the dock's charger pulled for about 30 s and plugged back, the dock left on the Cube (22:11) | **The Cube stayed the source**: caps unanswered, SRC_READY without a contract, not charging, ibat -0.69 to -0.75 A, no picture. Recovery: unplug the dock from the Cube and plug it in again with its charger in. Known issue in test 42. [C] |
| DK3 | Test dtbos offering the dock 1.0 A (and 1.5 A, `--source-ma`) instead of DK1's 900 mA, the dock **without** its charger (2026-11-26 11:14-18:18) | **No picture at any offer.** The dock's request keeps Capability Mismatch set at 0.9, 1.0 and 1.5 A; with a contract it enters DP alt mode (Status 0x1a, Configure acked) but never raises HPD, and the Cube's PD link as a source is unreliable with it (unACKed caps, PD collisions, hard resets). Its firmware likely wants a 3 A source, which the AW32257 boost can't supply (1.0 A rated); never use the 3000 mA image. After a PD ERROR_RECOVERY the port doesn't detect the partner again until a replug. DK1 (900 mA) ships unchanged. [C] for the logs, [I] for the 3 A reading. |
| TH2 | DK1-TH2 (`c87ece8c…`: DK1 plus the owner's charge temperature steps, `patch_dtbo.py --jeita thermal2`) written to dtbo_a (19:54) | Read back c87ece8c; live DT: the dcp, unknown and cdp tables are the 6-row thermal2 tables byte for byte, sdp unchanged, source-pdos 900 mA. While charging (CH3) charger-manager logs `jeita status:(2) ... jeita_size:6` every 15 s, so the tables are live, in the 15-45 C band. The 45 and 49 C steps were not reached, so their currents weren't seen. The build ships DK1 (`1b727faa…`); shipping TH2 is an owner decision. [C] |
