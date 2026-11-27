FACT-CHECK OF THE NANO PLATFORM-SEAM AUDIT (thimsy-os-linux, branch linux, HEAD 0e09194). Read only. I edited, built and ran nothing. My counting script and its output are in C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\ (fc.py, fc_linux.json, fc_top.json, fc_linux_sites.json).

Bottom line: the design's main conclusion holds. The Linux build runs Android-shaped code through emulation layers, by design. But several claims it leans on are wrong or overstated:
- The OTA claims are wrong: the OTA checker is not in the Linux binary.
- The boot problem is worse than the design says: Nano exits on boot, it does not just stall.
- The typec hazard is narrower than stated.
- The examples of gaps in the shell gate are wrong.
- The fixedperf hook does work.
- The property-call total in the props report is too low by about 290.

== 1. Re-counted numbers ==

Method:
- Scope "Linux-compiled shared": the .cpp files in sources.txt [core]+[drm]+[scrape] (107 files), plus every top-level nano/*.h and *.inc (99). 206 files in all.
- Lexer: my own (fc.py). It blanks comments and string contents, and skips declaration forms.
- Scope "all top-level": all 118 top-level .cpp files plus the headers, so firmware-only files are included.

| Item | Audit said | My count | Verdict |
|---|---|---|---|
| system( | 73 in 13 files | 73 in 13 | Confirmed |
| popen( | 25 in 12 | 25 in 12 | Confirmed |
| fork( | 4 | 4 in 3 | Confirmed |
| execv | 2 | 2 | Confirmed |
| execvp | 2 | 2 (NanoOtaCheck only) | Confirmed |
| Raw spawner sites (fork+exec as one) | 102 in 21 files | 102 in 21 files | Confirmed |
| All top-level (incl. DrasticRunner, NanoBtStable) | system 74/14, popen 28/14 | 74/14, 28/14 | Confirmed |
| Helper call sites (definitions excluded) | 44 | 43 | Consistent (see below) |
| Property-API calls (props report) | 1,009 in 55 files | about 1,300 in 57 files | Wrong (see below) |
| property_set literal keys | not stated | 162 distinct keys in 435 calls | New number |
| Android-header #include lines | 292 in 88 files | 295 in 90 files | Consistent |
| Platform #if/#ifdef switches | 0 | 0 | Confirmed |
| Absolute-path literals, excluding /dev/null | 784 | 783 | Confirmed |
| NanoPlatform.h pure virtuals | 58 | 58 | Confirmed |
| `nanoplat::get()` uses | 59 in 10 files | 63 textual in 11 files (comments included) | Consistent |
| props.txt keys | 994; 99 not Nano's own | 994; 99 | Confirmed |
| control-keys.json rows | 66 (arch report: 67) | 66 | 66 is right |
| control-keys.json dispositions | 12 / 18 / 15 / 21 | routed 12, store-only 18, refused 15, not-reached 21 | Confirmed |
| kAndroidTools / kPosixTools | 39 / 71 | 39 / 71 | Confirmed |
| kControlRows | 23 rows over 19 keys | 23 rows over 19 keys | Confirmed |
| stubs-budget.txt | 446 | 446 | Confirmed |
| Files including NanoMenu.h | 53 | 53 | Confirmed |
| restorecon no-op sites | 8 | 8 | Confirmed |

Notes on the rows that need them:
- **Helper call sites (43).** runArgv 10, esdeRunProc 15, shellCmd 5, runProcess 4, overlayShellCapture 3, runCmdUnlocked 2, and 1 each for runCmd, nanoCaptureCmd, fetch and runMonitorModeCmd.
- **Property-API calls.** The full breakdown:
  - property_set 491 (not 311; a plain grep also gives 491)
  - property_get 380, property_get_bool 178, property_get_int32 91
  - GetProperty 42, GetBoolProperty 34, SetProperty 29, GetIntProperty 5
  - `__system_property_serial` 28, `__system_property_find` 20, `__system_property_wait` 2
  - The apis report's property_set 491 was right; the props report undercounts.
- **#include lines.** My pattern is slightly wider (it adds hidl/ and hardware/) and also counts firmware-only headers.
- **Platform switches.** No `__ANDROID__`, `__linux__`, `__BIONIC__`, NANO_*, THIMSY or NANOCOMPAT tests. Only 2 lines match "ANDROID": `#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID` and `#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID` (NanoMenuDrm.h:114 and :117). These are fallback definitions of Android EGL macros, not platform tests.
- **Path literals, by root:**

  | Root | Literals | Files |
  |---|---|---|
  | /data | 322 | 64 |
  | /storage | 142 | 28 |
  | /system | 106 | 31 |
  | /mnt | 59 | 22 |
  | /sys | 54 | 11 |
  | /proc | 36 | 17 |
  | /sdcard | 34 | 16 |
  | /dev | 21 | 5 |
  | /config | 6 | 4 |
  | /vendor | 3 | 1 |

- **Control keys with a writer.** 17 of the 19 kControlRows keys have a writer in Linux-compiled shared code. `park_restart` is written only by NanoMenuDrasticPark.cpp ([dropped-linux]). `bt_autopair` is written only by platform/firmware/Network.cpp. So 2 of the 23 rows are dead on Linux.
- **Shell report arithmetic.** It says "106 of 138 name an Android-only tool", but its own parts (96 refused, 2 routed, 8 no-ops, 1 either-way, 2 translated, 2 misparsed) add up to 111.
- **Shell report /system/bin passes.** They add up to 23, not 22: streaming 5, OTA 5, ES-DE 12, pidof 1.
- **Line counts are stale.** ShellGate.cpp is 783 lines, not 715. ControlKeys.cpp is 324, not 288.

Correction that changes the Linux numbers:
- NanoOtaCheck.cpp and NanoArchive.cpp are not compiled on Linux:
  - nano/cmake/features.cmake: `NANO_FEATURE_FILES_ARCHIVE NanoArchive.cpp NanoOtaCheck.cpp`
  - CMakeLists.txt:87: `NANO_WITH_ARCHIVE` defaults to OFF
  - linux/package/thimsy-nano/thimsy-nano.mk:54: `-DNANO_WITH_ARCHIVE=OFF`
  - Generated stubs stand in for them.
- In the shipped Linux binary that leaves:
  - system 73, popen 25, fork 2, execv 2, execvp 0
  - 100 raw sites in 20 files
  - about 133 command sites
  - 24 passes, not 29; 18 of them use hard-coded /system/bin paths
- tools/nano-tree/tests/test_shell_sites.py:33-40 (EXEC_SITES) still describes NanoOtaCheck as "curl passes".

== 2. Spot-checks of specific claims ==

1. ShellGate.cpp:341-346, the timeout skipper. ACCURATE.
   - It skips dash words and then one more word, so `timeout -s KILL 10 cmd` gives the first word "10". findTool fails, so the command passes.
   - Sites that hit it: NanoMenuMonitorMode.cpp:84 and NanoMenuPS3Menu.cpp:2469 (from :2483).
   - The only test of this form is shellgate_test.cpp:111 (`timeout 5 pm`).
2. ShellGate.cpp:65-79 and :82-90 (39 and 71 entries; restorecon and chcon are no-ops; `/system/bin/<posix>` becomes the bare name with the arguments unchanged, :356-360 and :549-553). ACCURATE.
3. gammaos-net is rewritten to curl only through exec (:587-603) and is refused in a shell string (default branch :412-414). ACCURATE.
   - nsenter: through exec it runs its tail (:568-586). In a shell string it is refused (also the default branch).
4. compat/src/properties.cpp: trySet at :233, announce at :242, then return 0; observers cannot veto (:123-128). ACCURATE.
5. ControlKeys.cpp. MOSTLY ACCURATE.
   - Accurate: 23 rows at :50-74; refuse() at :231 only logs; it is called at :263; `ctl.restart` is refused at :64; the default session request at :113-120 writes `sys.thimsy.session.request`.
   - Inexact: :82 is only the constant `kBootCompletedKey`. The fake `sys.boot_completed=1` is written at :227, in firstFrame() on drm_active=1.
6. Run.cpp:103-105 (`syscall(SYS_execve)` at :105). ACCURATE.
   - The code comment's reason is async-signal-safety of the wrapper as well as the gate.
7. NanoMenuPS3Menu.cpp:18454/18456 (`setprop persist.sys.timezone`) against LinuxSystemActions::setTimeZone (actions/SystemActions.cpp:185-206), which makes the localtime link and then sets the property. ACCURATE.
   - Nothing on Linux observes persist.sys.timezone, so the XMB globe does not change the clock's zone.
   - The wizard uses the seam (NanoMenuSetupWizard.cpp:470).
8. NanoDeviceKeys.cpp:180-182 and :24-28. ACCURATE: with no table it uses the RG Cube's keys. No device_keys.json exists anywhere under linux/.
9. NanoRaConfig.h:31-36 against platform/linux/Launcher.h:81 and Launcher.cpp:404-411. ACCURATE: the two use different RetroArch configs.
10. NanoMenuPS3Folder.cpp:1057-1064. ACCURATE: it compares /proc cmdline with "com.retroarch.aarch64".
11. test_shell_sites.py:33-40 (3 EXEC_SITES) and :78-81 (a floor of more than 50 system and more than 10 popen calls, no ceiling); IMPLEMENTATION.md:1014 (H.4 says the sites "may not grow without a row"). ACCURATE: the test and H.4 contradict each other.
    - ratchets.py:17-29 shared_sources() counts everything Android compiles, including platform/firmware. ACCURATE.
12. Android.bp:740-763 (`drastic_nano_shared_srcs`). ACCURATE.
13. IMPLEMENTATION.md :53-55, :171, :263, :428, :445, :1014, :1041-1043, :1051 and :1055. ACCURATE quotes.
    - But :445 says the unsupported rows "fail at run time with a message". Property-backed rows actually succeed silently.
    - :247 refers to a `Cap::MediaPlayers` that does not exist.
14. NanoPlatform.h:66 (forceSfPath), :119 (`commit(const android::LaunchTuple&)`), :215-229 (ViaHal, mirror, saved, powerAction) and :268 ("named after their call sites"). ACCURATE.
15. EglWraps.cpp:84-125. ACCURATE.
    - It returns a stand-in for `eglGetNativeClientBufferANDROID` when the driver lacks it.
    - It appends the two `EGL_ANDROID_*` names only when the display has `EGL_EXT_image_dma_buf_import` and `EGL_KHR_image_base`.
16. NanoMenuSettingsTree.cpp:1107-1113. PARTLY WRONG.
    - The factory_reset write (:1113) is correct.
    - Lines 1107-1111 are the "reset_wifi_bt" action, not "airplane mode" as the arch report and the design's Step 3 call it.
    - The real airplane-mode command is at NanoMenuPS3Menu.cpp:12849-12851 (`cmd connectivity airplane-mode`).
17. NanoMenu.cpp:4040 and NanoMenuMtp.cpp:348-353, the typec hazard. LINES ACCURATE, SCOPE OVERSTATED.
    - The lines: the wakelock at :350, `ctl.restart nano-typec-guard` at :351, the port_type write at :353.
    - The design says it "writes port_type on whatever /sys/class/typec exists". In fact typecTick (NanoMenuMtp.cpp:291-323) and typec::step (NanoTypec.cpp:35-38, 120-125) write only when all of these hold:
      - a port has a writable port_type and a USB PD revision above 0
      - the port has DisplayPort alt mode, or the module sprd_typec_displayport is loaded
      - a non-PD partner is attached and the port is sinking
      - the port is dual role, the battery is not low, and no computer is attached
      - the lock /data/system/nano_typec.lock can be taken
    - The missing crash guard on Linux is real. It is a conditional hazard, not an unconditional one.
    - Also, ControlKeys logs a refusal once per key, not once per value. So the typec ctl.restart may be refused with no log line at all.
18. The OTA "PASS against the Android OTA server" claims. WRONG for the Linux binary (see section 1). This affects the shell report's OTA table, design 1d.2, Step 4's file list, and section 4's "the OTA check works ... losing it is a fix".
    - NanoMenuOta.cpp is compiled, so the prestage refusal and the poll that never advances (NanoMenuOta.cpp:652/664, 758) do stand.
19. NanoMenu.cpp:493-560, the "probable 10 s boot stall". UNDERSTATED.
    - What the code does: no /data/misc/bootanim/nano_skip, then a 10 s wait for ro.persistent_properties.ready, then persist.bootanim.skip_nano is "" (not "0"), so fileBootanim is true. Then `property_set("ctl.start","bootanim")` (refused) and `_exit(0)` at about :560.
    - Who writes these: only mods/50-nano.sh (Android build.prop) and Android files. Nothing in linux/, libthimsy/, nano/platform/linux, nano/test/host, main_linux.cpp or main_headless.cpp. The host boot_home scenario does not seed them either.
    - By code reading, Nano exits after about 10 s on every Linux and host boot.
    - Not run. This is the highest-priority item to verify on a device; it outranks the typec hazard if it is real.
20. "fixedperf → family hook ... does nothing today" (props report, carried into Step 6e). WRONG for rk3566.
    - linux/board/rk3566/common/rootfs-overlay/usr/share/thimsy/family/rk3566/hook:35-40 implements fixedperf-on and fixedperf-off (performance and schedutil governors), started from ControlKeys.cpp:156-168.
    - At the same time, the CPU-profile shell call (NanoMenu.cpp:1242, `/vendor/bin/setclock_%s.sh`) is refused. So perf is half-working and inconsistent.
21. NanoControlCenter.cpp:201, :257-261 and :355-356 (fde60000.gpu, power_supply/battery, soc-thermal and gpu-thermal); NanoMenu.cpp:432-436 (ums, sc98, sc99, sharkl). ACCURATE.
    - On Linux, ro.board.platform is the device family, for example "rk3566" (libthimsy/tools/thimsy-prop.cpp:228, 238). So the `strcmp(plat,"rk356x")` branch at NanoMenuDrm.cpp:1155-1156 is always false (inert), and nanoForceSfPath's Unisoc test is false on every Linux board that exists today (rk3566, qemu).
22. Firmware Network.cpp:46 and :72 declare runCmd and runCmdUnlocked from shared NanoMenuSettings.cpp (:74, :82). ACCURATE.
    - shellQuote is at NanoMenuSettings.cpp:146, outside the cited :72-118.
    - A second shell quoter, `nanolaunch::shellQuote`, is used at NanoMenuPS3Folder.cpp:888.
23. NanoMenuEsdeMenu.cpp:831-855 and :902-991 (15 calls; nsenter only when netns is set, and no caller sets it; `chown -R system:system` at :990). ACCURATE.
    - linux/board/common/users.table has only comments and the rootfs overlay ships no passwd, so the chown fails.
24. Host scenarios match "am start", "settings put system font_scale " and "settings get " (launch_game.jsonl:7, setting_persisted.jsonl:6). ACCURATE.

== 3. Other wrong claims ==

- **The gate's parser gaps (design 1d.1).** The examples "nohup, busybox, sh -c, $(...)" are all handled:
  - exec, nohup, command and busybox are skipped as wrappers (ShellGate.cpp:331).
  - toybox and toolbox are stripped (:332-336).
  - `sh -c` and `bash -c` are classified recursively, and routes are refused inside them (:362-375).
  - `$(` and backticks are separators, so the command inside is classified (:206-209, isOpChar).
  - What actually leaks:
    - `timeout -s SIG` and `timeout -k DUR` (live today at 2 sites)
    - xargs (which is in kPosixTools)
    - shell keywords as the first word (if, then, do, else, {, !)
    - unknown wrappers (su, time, stdbuf, chrt, taskset, ionice)
    - a command word held in a variable
  - A grep of shared literals finds none of the keyword or wrapper forms in use today.
- **Raw execve.** The design says Linux platform code "had to bypass" the gate with raw execve. Only Run.cpp does. Launcher.cpp:128 calls the wrapped execve in its forked child; it passes because the path is a Linux path.
  - ControlKeys.cpp:159 already uses posix_spawn, which is not wrapped. So the proposed "sanctioned posix_spawn" already exists, informally.
- **"Linux Power writes persist.gammaos.nano.brightness (Power :46)".** Wrong: :46 is the key constant. platform/linux/Power.cpp only reads it (savedBrightness :135-139), and mirrorBrightness is empty (:133).
- **ControlKeys "Routed 7".** This includes park_restart. It is in the table, but nothing on Linux writes it.

== 4. Important things the inventories missed ==

1. **The boot exit above (item 19).** If confirmed, Nano does not reach the home screen on Linux without a seeded nano_skip file or skip_nano=0. This is the Android boot-mode decision running in shared code on Linux, the owner's principle exactly.
2. **The OTA and archive code is compiled out on Linux**, and generated stubs answer instead. Every OTA row in the shell and paths inventories is a non-site for the Linux binary.
3. **More bypasses of existing seams.**
   - The XMB quick toggles at NanoMenuPS3Menu.cpp:12848-12876 turn Wi-Fi (`cmd wifi set-wifi-enabled`) and Bluetooth (`cmd bluetooth_manager`) on and off directly. SystemActions::setWifiEnabled and setBluetoothEnabled exist for this.
   - The same block holds airplane mode (`cmd connectivity`), DND and location, and does a writeSettingValue(kGlobal) mirror first.
   - The shell report folded all of this into one "DND/location" site.
4. **A second silent-failure mode in the property store.** A value over 91 bytes makes property_set return -1 (ValueTooLong, libthimsy/include/thimsy/config.h:34 and :59). properties.cpp:235-239 logs only IoError, so the failure is silent.
5. **ControlKeys refusals are logged once per key, not per value** (gRefused at :235). Later refusals of other values (other ctl.restart services, other nano_action values) are silent.
6. **sys.powerctl.** The gate routes `reboot` and `svc power` there (ShellGate.cpp:395-406), and it is consumed by thimsy-session (Session.cpp:63; core/Requests.h:16, 40). So `reboot recovery` becomes `sys.powerctl=reboot,recovery`, which Linux handles as an ordinary reboot request. I did not read how thimsy-session treats the reason.
7. **IMPLEMENTATION.md:445 is contradicted by the code.** The doc promises run-time failure "with a message" for unsupported rows. Property-backed rows (class 1b, the untabled IPC keys) store, return 0 and log nothing.

== 5. Claims I did not verify ==

- include-census "57 Linux units reach stubs"
- the stubgen method counts (117 DrasticRunner, 25 LibretroRunner and so on)
- the props classification counts (232 / 284 / 96 / 69 / 35) and the 67 silently stored keys. I confirmed only samples: factory_reset, kill_pkg, app_action, net_cmd, typec, typec_port, ota.package and dualstack.enabled are absent from control-keys.json and kControlRows.
- the Android-tool literal count (115)
- NDK media, SurfaceFlinger and other API reference counts (grep-approximate in the reports)

Key files:
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenu.cpp (:493-560 boot decision, :4039-4040)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuMtp.cpp (:277-363)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoTypec.cpp (:35-38, :120-125)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ShellGate.cpp (:65-95, :326-415, :541-605)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ControlKeys.cpp (:50-74, :156-168, :218-238)
- C:/Users/krist/code/thimsy-os-linux/nano/compat/src/properties.cpp (:226-244)
- C:/Users/krist/code/thimsy-os-linux/nano/cmake/features.cmake
- C:/Users/krist/code/thimsy-os-linux/linux/package/thimsy-nano/thimsy-nano.mk (:54)
- C:/Users/krist/code/thimsy-os-linux/linux/board/rk3566/common/rootfs-overlay/usr/share/thimsy/family/rk3566/hook (:35-40)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuPS3Menu.cpp (:12836-12876, :18454)
- C:/Users/krist/code/thimsy-os-linux/libthimsy/tools/thimsy-prop.cpp (:228, :238)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/Launcher.cpp (:128)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/actions/Run.cpp (:105)