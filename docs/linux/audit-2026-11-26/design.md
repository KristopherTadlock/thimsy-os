ARCHITECTURE AUDIT AND REDESIGN PROPOSAL: Nano platform seam (thimsy-os-linux, branch linux)

Sources: the five inventory reports (shell, props, apis, paths, arch). I spot-checked three load-bearing claims myself:
- ShellGate.cpp:341-346, the timeout skipper. It skips dash words and then exactly one word, so `timeout -s KILL 10 cmd` treats KILL as the duration and `10` as the command. Confirmed.
- compat/src/properties.cpp:233 `trySet` runs before `announce` at :242, and ControlKeys.cpp:231 `refuse()` is called at :263. A refused key is stored before it is "refused", and property_set returns success. Confirmed.
- nano/Android.bp:740-763 `drastic_nano_shared_srcs`. Under I24 these files may not call nanoplat: NanoMenuDrm, NanoJson, NanoI18n, NanoBidi, NanoDeviceKeys, NanoButtonMap*, NanoAppFiles, NanoBiosUtil, NanoBiosCatalog, NanoSystemsHeal, NanoEsdeSystemName and NanoDisplayTarget. Confirmed.
Items marked [inferred] are my reasoning, not something I read. I edited, built and ran nothing.

======================================================================
1. VERDICT AGAINST THE OWNER'S PRINCIPLE
======================================================================

Short answer: the current Linux design breaks the principle on purpose, and the docs say so. It is an "Android-on-Linux emulation" strategy, not a platform-layer strategy.
- IMPLEMENTATION.md:53-55 (Principles 1-3): merge-friendly first; cut the seam "at the lowest boundary that leaves call sites alone"; header shims, link-time substitution and filesystem mapping come before interfaces.
- B.1 :171: layers 1 to 3 give "a Linux binary with no edits to Nano's existing files".
- I15 :1042 is the shell gate, I14 :1041 the path links, I16 :1043 the inert stubs, I28 :1055 the control-key observers.
- No document describes an end state in which the gate or ControlKeys goes away (arch report, section 3).

1a. Where it complies
- Zero platform preprocessor switches in shared code. There is no `__ANDROID__`, `__linux__` or `NANO_PLATFORM_*` test; the ratchet budget is 0 (test_nano_ratchets.py:27; IMPLEMENTATION.md:1014; DECISIONS G0-14 :364). The only conditionals are for architecture (`__aarch64__`, NEON) or features.
- Pure portability shims, which are acceptable:
  - logging (compat/src/log.cpp)
  - clocks (utils.cpp)
  - RefBase and Thread (RefBase.cpp, Thread.cpp)
  - android-base file and unique_fd (file.cpp)
  - Bionic libc extras (nanocompat/bionic.h)
  - AImageDecoder, a real libpng, libjpeg, libwebp and giflib decoder (I30)
- Real seams that work: 9 abstract classes with 58 pure virtuals in NanoPlatform.h. Shared code calls `nanoplat::get()` 59 times in 10 files, 36 of them in NanoMenuSettings.cpp.
- 48 command sites already moved into platform/firmware (Network.cpp 34+5, Power.cpp 2, SystemActions.cpp 7).
- Linux implements several seams properly, for example Power::readBattery, which scans power_supply by type (platform/linux/Power.cpp:45, 158-168).
- Reverse direction: no Linux tool names, Linux paths or `__linux__` tests appear in shared code (apis report, section 1). Linux specifics live in platform/linux and compat. This direction is clean today.

1b. Where it does not comply (counts)

Shell and process calls (shell report)
- 102 raw spawner call sites in 21 shared files. A fork plus exec pair counts once, and the 8 generic helpers count once each.
- 138 command sites: 94 self-built commands plus 44 calls through the helpers (shellCmd, overlayShellCapture, nanoCaptureCmd, runCmdUnlocked, fetch, esdeRunProc, runProcess, runArgv).
- The arch report measured [core] alone with the repo's lexer: `system(` 73 in 13 files, `popen(` 25 in 12, `fork(` 4, execv 2, execvp 2.
- 106 of the 138 name an Android-only tool: am, cmd, settings, svc, pm, dumpsys, appops, ime, locksettings, monkey, screencap, logcat, setprop, restorecon, gammaos-net, or a /vendor script.
- Outcome on Linux:

  | Outcome | Sites |
  |---|---|
  | refused | 96 |
  | routed to the property store | 2 (+1 string that goes either way) |
  | no-op that reports success (restorecon) | 8 |
  | rewritten (gammaos-net to curl) | 2 |
  | passed to a real Linux program | 29 |

- Of the 29 passes, 24 carry Android-shaped arguments or paths: 22 hard-coded `/system/bin/<tool>` paths that the gate rewrites, pidof with a package name, and similar.
- 2 sites are Android `cmd` calls that reach the Linux /bin/sh because of the timeout misparse: NanoMenuMonitorMode.cpp:84 and NanoMenuPS3Menu.cpp:2469/2483.

Property IPC (props report; lexed, comments excluded)
- 1,009 property-API calls in 55 shared files.
- 716 distinct keys in 1,781 literal occurrences:

  | Class | Keys |
  |---|---|
  | 1a, Nano-private prefs (portable in substance) | 232 |
  | 1b, persisted but acted on by another Android process (upper estimate; 37 proven by patch evidence) | ~284 |
  | 2, IPC/control messages | 96 |
  | 3, reads of Android system state | 69 |
  | 4, Nano scratch state on the global bus | 35 |

- 99 keys in props.txt are not Nano's own (arch report).

Android API includes (apis report)
- 292 Android-header include lines in 88 shared files.
- 29 of those files include something other than logging, property or clock headers.
- 57 Linux-compiled translation units reach a layer-2 stub, counted transitively (include-census.json). NanoMenu.h alone pulls in binder, Thread, sensor, looper and system_properties, and 53 files include it.
- API references (grep, approximate):

  | API | References |
  |---|---|
  | NDK media | 366 in 11 files |
  | SurfaceFlinger client | 188 in 9 |
  | AImageDecoder | 94 |
  | AHardwareBuffer | 24 |
  | Lights and Health HAL | 19 |
  | AAudio | 17 |
  | binder service lookups | 15 |

- The stub budget is 446 names (stubs-budget.txt).

Platform paths (paths report)
- 784 absolute-path literals in shared code, not counting /dev/null:

  | Root | Literals |
  |---|---|
  | /data | 323 (/data/system 177, /data/media 70, /data/data 50) |
  | /storage | 142 |
  | /system | 106 |
  | /mnt | 59 |
  | /sys | 54 |
  | /proc | 36 |
  | /sdcard | 34 |
  | /dev | 21 |
  | /config | 6 |
  | /vendor | 3 |

- About 450 of them work only because the Linux root imitates Android's layout (10-android-links.sh:70-75; S00base:91-104).

Device-specific knowledge in shared code
- Unisoc: sprd_typec_displayport at NanoMenuMtp.cpp:316, and the ro.board.platform "ums/sc98/sc99/sharkl" branch at NanoMenu.cpp:432-436.
- Rockchip:
  - fde60000.gpu at NanoControlCenter.cpp:201-217
  - soc-thermal and gpu-thermal at :355-356
  - vop@fe040000 and route-dsi at NanoScreenCal.cpp:358-360
  - the baseparameter partition at NanoScreenCal.cpp:335
  - rk356x AFBC at NanoMenuDrm.cpp:1155
- RG DS: the dual-backlight names at NanoControlCenter.cpp:145 and :157.
- TrimUI Brick: rfkill0 at NanoMenuInput.cpp:932.
- RG Cube default key map at NanoDeviceKeys.cpp:24-28 and :181.
- Android or Unisoc node names: panel0-backlight and leds/lcd-backlight (NanoMenu.cpp:905-906, NanoBacklight.h:83-88), power_supply/battery and power_supply/usb, wlan0.
- Comment and grep mentions: "RG DS" 127, Rockchip 42, Brick 32, Cube 28, Unisoc 15.

The platform interface itself has Android shape, and it lives in shared code
- `Launcher::commit(const android::LaunchTuple&)` writes the property-handshake keys (NanoPlatform.h:82-99, :119).
- `Power::setBrightnessViaHal`, `mirrorBrightness` (from "syncBrightnessToAndroid") and `savedBrightness` (:212-229).
- `powerAction("recovery"|"safemode"|"android")`.
- `Display::startScanout(..., bool (*forceSfPath)())` (:66).
- Network's methods are "named after their call sites" (:268-270).
- There is no capability query. B.6 dropped `Cap`, `Platform::has` and `SystemActions::supported` (IMPLEMENTATION.md:263).

1c. What the Linux layers really do
None of these is a platform layer in the owner's sense. Each one translates Android semantics at run or link time, below unchanged Android code.

- ShellGate (ShellGate.cpp, 715 lines; process-wide `--wrap` of system, popen, pclose, execv, execvp and execve).
  - It is a shell-language interpreter and router. It tokenises Android shell strings and skips `env`, `nice` and `timeout` prefixes (:330-346).
  - It then classifies the first word: 39 Android tool words at :65-78 and 71 POSIX tools at :82-89.
  - Routes setprop, getprop, svc power and reboot to the store; no-ops restorecon and chcon; rewrites gammaos-net download into curl (:587-603); runs the tail of nsenter (:568-586); otherwise refuses with 127 or ENOENT.
  - A `/system/bin/X` POSIX tool becomes the Linux X with the arguments untouched. That is exactly the owner's "curl run identically" case.
- ControlKeys (ControlKeys.cpp, 288 lines; 23 rows at :50-74).
  - It turns Android IPC property writes into Linux actions after the store has written them. Observers cannot veto (B.2; I28).
  - Disposition of the 96 IPC keys:

    | Disposition | Keys |
    |---|---|
    | routed | 7 |
    | refused (logged once, but the write succeeds) | 10 |
    | read by thimsy-session | 3 |
    | read back as handshake keys | 9 |
    | silently stored, no log | 67 (58 of them in no table) |

  - The Linux platform adopted the Android property bus as its own IPC. The default session request is `sys.thimsy.session.request` (:113-120). Launcher.cpp catches its own do_launch write and re-reads the keys, a round trip inside one process (I28, G7-launch-b-14).
- compat/src/properties.cpp (536 lines) is a full Android property emulation:
  - the 92-byte value limit (the Linux launcher keeps a side copy because of it: Launcher.cpp:148-170, G7-launch-b-11)
  - serials that bump on a same-value set, `__system_property_wait` and WaitForProperty
- compat/stubs (446 names) make SurfaceFlinger, binder, HALs, NDK media, AAudio, sensors, perf hint and AHardwareBuffer fail inertly at run time, so Android-only paths stay in shared code.
- EglWraps.cpp wraps eglGetProcAddress and eglQueryString. It returns a stand-in for `eglGetNativeClientBufferANDROID` and adds two `EGL_ANDROID_*` names to Mesa's extension string. This is Linux impersonating Android so that unchanged code takes the Android zero-copy path (NanoMenuDrm.cpp:1118, 1142-1143).
- Generated stubs (stubgen.cmake, I29) are no-op bodies for [dropped-linux] functions that [core] calls: about 117 DrasticRunner methods, about 25 LibretroRunner, 13 Shares, and others.
- Path links (I14) rebuild /data, /sdcard, /system/{etc,fonts,bin} and /mnt/media_rw on the Linux root. Android-named state (/data/system/nano_*, Android/data/<pkg>, /data/data/<pkg>) becomes real folders on the user's card.
- Android uid shims: private/android_filesystem_config.h supplies AID_* numbers that are applied to Linux files (NanoBiosUtil.cpp:179, NanoMenuSyncthing.cpp:466).

1d. Concrete risks of today's approach, with evidence

1. Android commands reach Linux programs.
   - The timeout misparse (ShellGate.cpp:341-346, confirmed) lets `timeout -s KILL N cmd ...` through to /bin/sh. This affects NanoMenuMonitorMode.cpp:84 and NanoMenuPS3Menu.cpp:2469/2483. The tests cover only `timeout 5 pm` (platform/linux/tests/shellgate_test.cpp:111).
   - The deny-list is a parser race by design: any wrapper word the gate does not know (`nohup`, `busybox`, `xargs`, `sh -c`, `$(...)` shapes) can move an Android word out of first position. [inferred, from the first-word design]
2. Linux tools run with Android arguments.
   - `chown -R system:system` (NanoMenuEsdeMenu.cpp:990). No Linux users are defined, so it fails and the result is ignored.
   - `pidof org.vita3k.emulator` (NanoBiosWorker.cpp:863).
   - OTA curl and unzip against `ota.gammaos.sh/api/v1/{device}/{variant}`, downloading an Android OTA package onto Linux (NanoOtaCheck.cpp:79, 334, 544/557, 626).
   - `/data/...` paths that work only through the I14 links.
3. Partial and inconsistent translation.
   - gammaos-net download is translated through exec but refused in a shell string (ShellGate.cpp:412-414 vs :587-603).
   - Time zone is wired two ways. The XMB globe's `setprop persist.sys.timezone` (NanoMenuPS3Menu.cpp:18456) is stored but never reaches LinuxSystemActions::setTimeZone (actions/SystemActions.cpp:185-204). The wizard's path does reach it (NanoMenuSetupWizard.cpp:470).
   - The clock-from-internet curl passes (NanoMenuPS3Menu.cpp:18404) and feeds a refused `cmd alarm set-time` (:18419).
4. Silent false success instead of an explicit no-op or an error.
   - 8 restorecon no-ops return 0.
   - Properties nothing on Linux reads, though the write succeeds: the 2 setprop routes (split-brightness override, persist.sys.locale), plus the XMB time-zone route above.
   - 67 IPC keys are swallowed with no log. Factory Reset (NanoMenuSettingsTree.cpp:1113, factory_reset) silently does nothing.
   - Reboot to android, recovery or safemode (NanoMenuInput.cpp:531-539) silently does nothing.
   - The OTA progress poll never advances (NanoMenuOta.cpp:652/664).
   - About 284 class-1b settings rows store, read back and change nothing. The UI shows them as working (IMPLEMENTATION.md:445 defers hiding them to G7-caps, after the alpha).
   - ControlKeys `refuse()` (ControlKeys.cpp:231) logs, but property_set still returns 0 (properties.cpp:233 then :242).
5. Wrong answers, not no-ops.
   - The RetroArch config Nano edits (NanoRaConfig.h:33/36) is not the one Linux RetroArch reads (Launcher.h:81, Launcher.cpp:404-411).
   - "RetroArch is running" is never true, because it scans for a package name (NanoMenuPS3Folder.cpp:1057-1064).
   - With no /data/system/packages.list, checks fail open at NanoMenuXmb.cpp:1333 and closed at NanoMenuState.cpp:339 and NanoHakuxSetup.cpp:285, by accident.
   - Every Linux device gets the RG Cube's keys (NanoDeviceKeys.cpp:181).
   - The GPU and thermal gauges read Rockchip node names (NanoControlCenter.cpp:201-235). They read 0 on other SoCs.
   - The battery gauge bypasses Power::readBattery (NanoControlCenter.cpp:257-261; NanoMenuMtp.cpp:340).
6. Hardware hazard.
   - The typec role flipper starts unconditionally (NanoMenu.cpp:4040). It is gated only on sys.boot_completed, which ControlKeys fakes (ControlKeys.cpp:82).
   - It writes port_type and takes a wakelock (NanoMenuMtp.cpp:348-353) on whatever /sys/class/typec exists.
   - Its crash guard (`ctl.restart nano-typec-guard`) is refused (ControlKeys.cpp:64). So the write happens and the guard does not.
   - I consider this the most urgent item. The memory notes record earlier sysfs probes that crashed or hung the Cube; the paths report lists the typec risk.
7. Hangs and wasted work hidden by runtime flags.
   - The SurfaceFlinger wait loops (NanoMenu.cpp:231-240, 1060-1066) never end on Linux. They are avoided only by mDrmBootPath, sDrmActive and mOverlayMode (stub comment at compat/stubs/include/binder/IServiceManager.h:17-23).
   - The boot sound makes about 240 stub retries over 60 s (NanoMenuPS3Boot.cpp:201, 215-223).
   - Probable 10 s boot stall: ro.persistent_properties.ready, nano_skip and skip_nano have no writer (NanoMenu.cpp:493-560). [inferred by the props report; not run]
8. The gate fights the platform.
   - Because `--wrap` is process-wide, Linux platform code had to bypass it with raw SYS_execve (platform/linux/actions/Run.cpp:103-105; linux-ux-15, DECISIONS.md:1057). The tripwire has a deliberate hole.
9. Enforcement gaps.
   - test_shell_sites.py ratchets only the 3 fork-and-exec sites (EXEC_SITES :33-40). test_system_and_popen_are_used (:78) asserts a minimum, not a maximum. This contradicts H.4 (IMPLEMENTATION.md:1014).
   - shared_sources() counts platform/firmware/** as shared.
   - props.txt controls change but not kind.
   - Host E2E scenarios assert Android commands ("am start", "settings put system font_scale", "settings get"; G5-17, DECISIONS.md:747). The tests cement the Android shape.
10. Moved code still depends on shared shell helpers.
    - Firmware Network.cpp:46-47 and SystemActions.cpp:39 call `runCmd` and `runCmdUnlocked`, which are defined in shared NanoMenuSettings.cpp:72-118 (G7-power-2, G7-net-6). An Android shell helper lives in shared code.

======================================================================
2. TARGET ARCHITECTURE
======================================================================

Rule set (enforced, see 3.0)
- R1. Shared code expresses intent only. It calls nanoplat interfaces or portable facilities: logging, clocks, threads, a key-value prefs store, file I/O on paths the platform supplies, std::filesystem, JSON and image decoding. There is no system, popen, fork or exec*.
- R2. Every Android command, every property used as IPC, every Android API (SurfaceFlinger, binder, HAL, NDK media, AAudio, sensors, AHardwareBuffer) and every Android path lives in platform/firmware. The firmware implementation may be the original code moved verbatim (H.3).
- R3. Linux implements each capability natively, or declares it unsupported. Unsupported means:
  - `has(Cap)` returns false
  - its methods return an explicit `Unsupported` result and do nothing
  - they log once at most
  It never means "attempt the Android command" and never means "store and pretend".
- R4. The UI asks `has(Cap)` and hides or greys unsupported rows. This is G7-caps, brought before the alpha.
- R5. The interfaces are named for intent, not mechanism. No `android::` types, no "ViaHal", no "mirror to Android". The reverse also holds: no Linux names (ConfigStore, thimsy-session, /storage/.config) in shared code or in NanoPlatform.h.
- R6. Device knowledge (node names, key maps, SoC quirks) comes from the platform's device profile (Linux: linux/board/*/devices/*.json; Android: the device tree, props and the device_keys.json table), never from literals in shared code.

Interfaces
Existing interfaces to keep and clean:
- Display: add monitor mode, the external display toggle, saturation, rotation, screenshot and buffer import.
- Launcher: commit takes a LaunchRequest, not an android::LaunchTuple.
- Audio: add system volume, per-display volume and a decoder; see Media below.
- Volumes: make it the only source of storage roots.
- Power:
  - setBrightness and savedBrightness, without the "Android" names
  - a `PowerAction` enum with Caps for Recovery, SafeMode and RebootToAndroid
  - a `PerfProfile` (setclock, fixedperf)
  - battery and charger
- SystemActions: becomes SystemSettings (below).
- Network: add BT discoverable, inbound reject, Wi-Fi configure, and the airplane switch.
- InputPolicy.

New interfaces, sized from the inventories:
- Cap and `Platform::has(Cap)`. This is the single most important addition.
- SystemSettings
  - time format, auto time, auto zone, set time, time zone, locale
  - font scale, dark theme, DND, location
  - provisioning or setup finish, IME select
  - Today these are about 35 shell sites in SettingsTree, SetupWizard, NanoMenu, Overlay, Render, PS3Menu and Strings.
- Apps
  - installed?, running?, launch, stop, open system settings, calibrate pad, app home or config dir, app permissions
  - It absorbs about 43 shell sites (ControlCenter 5, Overlay 17, PS3Menu 6, PS3Folder 2, Xmb 1, State 1, SdMove 6, BiosWorker 5; counted from the shell report's tables) plus packages.list (7 sites) and the /proc package-name scans.
  - Linux: launch through Launcher; installed and running from its emulator table; the rest Unsupported.
  - The overlay, dual-stack and panel-app features sit behind Caps that are off on Linux.
- Session: sleep, wake, restart Nano, first frame drawn, shutdown or reboot, boot animation hand-off.
  - These replace the IPC keys dosleep, dowake, drm_active, nano_action, service.bootanim.*, nano.restart, ctl.start/stop bootanim and park_restart.
  - Firmware implementation: the same property_set lines, verbatim.
- Http (fetch to memory or file, HEAD, with a user agent and ranges)
  - Replaces /system/bin/curl in NanoHls (147/148, 249/251), NanoIcyDemux:44, NanoMenuIptv:107, NanoMenuRadio:108, NanoOtaCheck, the ES-DE gammaos-net downloads, the SetupWizard ipapi fetches and PS3Menu:18404.
  - Firmware: the verbatim popen of curl. Linux: libcurl, which the scraper already links.
- FileOps and Archive
  - mkdir -p, rm -rf and mv become std::filesystem (portable, can live in shared code).
  - unzip becomes Archive, through the platform or libzip and minizip.
  - restorecon, chcon and chown to Android uids become `Platform::fixupLabels(path)`: firmware runs restorecon and chown verbatim; on Linux it is an explicit no-op that returns Ok, the one honest no-op success because Linux has no SELinux labels.
- Paths: state root, asset root, font lookup, temp dir, the app-data root per app, and the RetroArch config path.
  - Firmware returns today's literals (/data/system, /system/etc, /system/fonts, /data/local/tmp).
  - Linux returns native locations, and the I14 links can then go.
- DeviceInfo: GPU load and frequency, CPU, thermal zones, backlight nodes, radio state, the panel-calibration store, and the source of the device key map.
- UsbPort / Typec: USB mode (MTP and none), role policy and charger online.
  - Linux reports Unsupported until a deliberate implementation exists, which removes the typec hazard.
- Media decode: a decoder interface over the 366 NDK media references in 11 files. Linux reports Unsupported until FFmpeg or GStreamer is chosen, so music, video, IPTV and radio rows hide instead of failing.
- Compositor and Display internals: move SurfaceFlinger, binder, ILights and IHealth, sensors, the perf hint, AAudio and AHardwareBuffer usage from NanoMenu.cpp, Overlay, Render and PS3Clock into firmware Display, Audio and Power.
  - This needs the G7D-2/3 owner decision to allow edits to NanoMenu.h.
  - On Linux, DRM buffer import becomes a Display method on dma-buf, and EglWraps goes away.
- Notifications: the list and dismiss, today `dumpsys notification` and `cmd notification snooze`.
- Ota: firmware keeps the GammaOS flow. Linux reports Unsupported until Thimsy has an update channel.
- Prefs: a key-value store for the 232 Nano-private keys (class 1a) and the 35 scratch keys (class 4).
  - Android: property_* on persist.gammaos.nano.*. Linux: ConfigStore.
  - Class-4 scratch state should become in-process state where only one process uses it. [inferred: most class-4 keys are written and read by Nano alone, per the props report]
  - Class-1b keys (about 284) are not prefs. Their effect is another Android process, so each family becomes a platform method with a Cap: BFI, shader, refresh lock, fan mode, ABXY swap, hotkeys, dual-stack, display tweaks and so on. Firmware keeps the property_set verbatim.
- Files that may not call nanoplat (I24; Android.bp:740-763 plus the scrape srcs) get their intent through an injected hook table: a plain struct of function pointers in a non-frozen header, filled by each host binary. Examples are NanoBiosUtil's restorecon and NanoDeviceKeys' key source. Alternatively, values are passed in by the caller.

What becomes of the Linux adaptation layers
- ShellGate becomes a deny-all tripwire, then is deleted.
  - Keep the wrap during migration, but refuse everything (no routes, no rewrites, no passes).
  - Log each hit with the caller, and count hits in a test hook. Any hit fails host E2E and the shellgate unit tests.
  - Platform code spawns through a sanctioned `thimsy::spawn` (posix_spawn, which is not wrapped). That removes the Run.cpp raw-SYS_execve hole (linux-ux-15).
  - When the [core] spawner ratchet reaches 0, delete the wrap for system, popen and exec entirely. A link-time check (nm shows no system or popen references from shared objects) takes over as the enforcement. [inferred: a cleaner end than a permanent wrap]
- ControlKeys is deleted. Once Session, Launcher and Display calls replace the property messages, no property write has a side effect on Linux. The Linux launcher's write-then-observe round trip (I28) goes with it.
- compat is split:
  - Keep as pure portability: log, utils (clocks), RefBase, Thread, android-base file and strings, bionic.h, and the imagedecoder, which is a portable library facade, not OS behaviour.
  - Keep properties.cpp only as the Linux backend of Prefs, if the Prefs facade is not yet in shared code. Drop its emulation features as callers move: observers, serial bumps, `__system_property_wait`, WaitForProperty, the 92-byte limit.
  - Remove as behaviour emulation:
    - all layer-2 stubs, budget 446 down to 0
    - private/android_filesystem_config.h, the AID uids
    - EglWraps
    - stubgen no-ops for dropped files: replace them with Caps (DS player, libretro runner, Shares, boot chime)
    - the I14 path links, last, after Paths

======================================================================
3. MIGRATION PLAN (ordered; each step lands on its own)
======================================================================

Legend:
- AS = android-surface: the step edits Android-compiled code and needs an `android-surface:` line (H.3) and a later cherry-pick to nano-144.
- LO = Linux or tooling only.
- Sizes come from the inventories. Where marked [est.], I estimated them.

Step 0. Enforcement ratchets. LO, tests only, about 1 file of tests plus baselines.
- Scope every ratchet to [core] + [drm] + [scrape] from sources.txt, and stop counting platform/firmware as shared (fix shared_sources()).
- Ratchets that may only shrink, with a target of 0:
  - (a) spawner calls system, popen, fork, exec*: today 102 raw sites, or 73/25/4/4 in [core]. Also count the 8 helper definitions.
  - (b) string literals whose first word is an Android tool (about 115).
  - (c) property keys outside the Nano-own `(persist|sys|ro).(gammaos|thimsy).nano.*` namespace: split props.txt into nano-own and android-system lists; 99 keys today. Also a list of IPC keys (96), which may only shrink.
  - (d) Android-only includes beyond the portability allow-list (log, clock, properties, RefBase, Thread, android-base file): 29 files today. Also the stub-reach count (57 units).
  - (e) Android path roots by family (784 literals; reuse the paths.py scanner).
  - (f) device node and SoC literals (fde60000, sprd_, soc-thermal, vop@, route-dsi, baseparameter, rfkill0, wlan0, panel0-backlight, lcd-backlight, power_supply/battery, ums/sc98/sharkl, rk356x).
  - (g) Android-named members in NanoPlatform.h: ViaHal, mirror, android::LaunchTuple.
- Fix the H.4 wording so it matches the test.
- Tested by running the tests.

Step 1. Hazard and correctness fixes. LO.
- (a) Typec flipper. Mask /sys/class/typec in Nano's mount namespace through NanoJail, which already masks eMMC nodes (DECISIONS G3b-20). [inferred feasible] Alternatively, have ControlKeys hold back sys.boot_completed for the typec thread; that is not possible separately, so masking is the Linux-only option.
- (b) Fix the timeout skipper for `-s SIG` and `-k DUR`, and add tests for `timeout -s KILL N cmd`.
- (c) Make gammaos-net consistent between shell and exec (refuse both).
- (d) Stop passing `/system/bin/chown`, `pidof` with a package-looking argument, and the OTA host.
- Tested by the shellgate unit tests and a typec namespace test.
- This is a stopgap that makes the current design safe. It does not implement the principle.

Step 2. Capabilities. AS, small: NanoPlatform.h plus 3 createPlatform files plus the UI builders.
- 2a. Declare `enum class Cap` and `Platform::has(Cap)`. The firmware returns true for everything it does today (behaviour-neutral), Linux returns false for unimplemented caps, and the host build is configurable.
- 2b. G7-caps: annotate settings-table rows (NanoMenuSettingsTree.cpp, the PS3Menu toolbox rows at NanoMenuPS3Menu.cpp:9910-9950) and the XMB and Quick Menu builders with a Cap, and hide or grey rows when has() is false.
  - Size [est.]: 54 kGlobal/kSecure/kSystem rows, about 284 class-1b rows, plus about 40 actions. Probably 4-6 files, but many lines in NanoMenuPS3Menu.cpp.
  - Android is unchanged because every cap is true there.
- Tested by host E2E with a Linux-like cap set (rows absent), plus a unit test that every Cap-annotated row resolves.

Step 3. Close the bypasses of seams that already exist. AS, verbatim moves, about 14 sites in 5 files.
- 7 shell sites:
  - NanoMenuPS3Menu.cpp:12189 time format
  - :18456 time zone
  - :16562 inbound reject
  - NanoMenuSettingsTree.cpp:1107-1111 airplane mode, using setWifiEnabled and setBluetoothEnabled
  - NanoControlCenter.cpp:1697 Wi-Fi tile
  - :1615 brightness, using mirrorBrightness
- Battery and charger reads at NanoControlCenter.cpp:257-261 and NanoMenuMtp.cpp:92 and :340, to Power::readBattery.
- Move runCmd, runCmdUnlocked and shellQuote out of shared NanoMenuSettings.cpp:72-118 into platform/firmware. Shared code keeps no shell helper.
- Fixes on Linux: the XMB time zone starts to work; brightness and Wi-Fi become explicit Linux behaviour.
- Tested by host E2E asserting nanoplat calls, not shell strings. Rewrite the host scenarios that match "am start" or "settings get" as platform-event assertions.

Step 4. Http, FileOps, Archive and fixupLabels. AS, verbatim moves; Linux gets native implementations.
- About 29 passing sites plus 8 restorecon no-ops plus 2 gammaos-net rewrites, in about 9 files: NanoHls, NanoIcyDemux, NanoMenuIptv, NanoMenuRadio, NanoOtaCheck, NanoMenuEsdeMenu, NanoBiosWorker, NanoBiosUtil, NanoSdMove, NanoMenuPS3Folder, NanoMenuSetupWizard, NanoMenuPS3Menu:18404.
- NanoBiosUtil is in drastic_nano_shared_srcs, so it uses the hook table, not nanoplat (I24).
- Linux: libcurl for Http, std::filesystem, libzip or minizip for Archive, and a no-op fixupLabels.
- The owner's curl case is now gone: Linux never runs an Android-shaped curl.
- After this step, the gate's PASS, REWR and NOOP classes have no callers, so the gate flips to deny-all (see 2, ShellGate).
- Tested by unit tests of the Linux implementations, host E2E for the ES-DE theme download and wizard zone, and a gate hit counter of 0.

Step 5. Session, the control keys as calls. AS, verbatim.
- The firmware keeps the property writes; the Linux implementation acts directly.
- Covers the 7 routed keys plus the refused and silent groups: about 96 keys, with writers in about 9 files (I28 says the keys are written from nine files).
- Includes reboot targets as Caps, Factory Reset as a Cap (off on Linux), the OTA hand-off as a Cap (off), the drastic-nano start as a DS-player Cap (off), and qs_summon and show_overlay as Overlay Caps (off).
- Delete ControlKeys at the end, along with the launcher's self-observe round trip.
- Tested by control-keys.json shrinking to rows with no Linux action, then by deleting test_control_keys.py.

Step 6. Area interfaces, one area per landing, all AS and verbatim. Sizes are shell sites from the shell report.

  | Order | Area | Sites | Linux |
  |---|---|---|---|
  | 6a | Apps | ~43 | Launcher-backed subset; rest Unsupported |
  | 6b | SystemSettings | ~35 | Native: zone (exists), time format, locale, set time; Unsupported: DND, location, uimode, IME |
  | 6c | Display extras | ~11: MonitorMode 84, PS3Menu 12392, SettingsTree 903, NanoMenu 5587, Render 439, ControlCenter 1693, Overlay 2907-3007 ×5 | KMS-native or Unsupported |
  | 6d | Audio volume | 4: ControlCenter 282, 307, 1730, 1744 | ALSA mixer through the existing Linux Audio |
  | 6e | Power/Perf | NanoMenu 1242, 1746, 2042 and fixedperf | cpufreq governor from the device profile, or Unsupported |
  | 6f | Network leftovers | 3: Wi-Fi configure (NanoMenuSettings 621), BT discoverable (16252, 16262) | |
  | 6g | UsbPort/Typec | 3 gammaos-net usb sites plus the MTP sysfs and configfs (NanoMenuMtp 75-420), NanoMenu 4039-4040 | Unsupported; lifts the Step 1 mask |
  | 6h | Notifications | 2 | |
  | 6i | Ota | 6 | Unsupported |

- Each landing is about 1-5 files and 2-45 sites. Each is tested by host E2E asserting platform events, plus the ratchet dropping.

Step 7. DeviceInfo and device knowledge. AS.
- About 30 sysfs and procfs sites plus 2 SoC property branches (NanoMenu.cpp:432-436, NanoMenuDrm.cpp:1155) plus the device-keys source.
- NanoDeviceKeys is I24-restricted, so it uses the hook table.
- The Linux profile JSON gains node names, so there is no RG Cube default on non-Cube devices.
- Tested by unit tests per profile.

Step 8. Paths. AS, mechanical, very large: up to 784 literals in about 60 files [est.].
- Do it by family: state root (/data/system, 177), assets (/system/etc, 43), fonts (19), temp (7), app data and RetroArch (/data/data, Android/data), storage through Volumes (/storage and /mnt/media_rw, 184).
- Then delete the I14 links.
- Lowest priority. The links are a filesystem compatibility layer, not command execution, so the principle is least violated here, except where they give wrong answers (the RetroArch config, packages.list, package-name scans). Those are fixed earlier, in 6a.

Step 9. Android APIs and compat cleanup. AS; needs the NanoMenu.h unfreeze (G7D-2/3).
- SurfaceFlinger, binder, HAL, sensor, perf hint and AAudio move into firmware Display, Power and Audio (188 SF references in 9 files, 15 binder, 19 HAL, 17 AAudio).
- Media decode interface (366 references in 11 files).
- AHardwareBuffer and EGL import to Display, then delete EglWraps.
- Drop the AID shims.
- The stub budget goes from 446 to 0, and properties.cpp is reduced to a plain store.
- Tested by stubs-budget reaching 0 and the include allow-list ratchet reaching 0.

Step 10. Delete ShellGate and linker-wraps (system, popen, exec, egl), delete the raw-execve hole in Run.cpp, and add a link-time nm check.

======================================================================
4. COSTS AND RISKS
======================================================================

Android-surface volume that cannot be compiled now
- Steps 2-9 edit Android-compiled code. Rough total [est.]:
  - 138 shell sites in about 21 files
  - about 96 IPC keys in about 9 files
  - about 284 class-1b rows (annotations)
  - about 30 sysfs sites
  - up to 784 path literals
  - plus the SurfaceFlinger and media move in NanoMenu.cpp, Overlay, Render and PS3Clock
- None of it compiles until the AOSP job ends. The firmware code is already uncompiled today (IMPLEMENTATION.md:47; DECISIONS.md:902, 1002).
- Mitigation:
  - Cap the number of uncompiled AS commits before an Android compile pass. Suggested: land only Steps 0-4 before the alpha, about 50 sites.
  - Keep each move a verbatim cut and paste into platform/firmware, so review can diff the strings.
  - Build a syntax-only Android check once headers are available. [inferred option]

Merge friction with nano-144
- The hot files are NanoMenuPS3Menu.cpp (40 Android-tool literals, 211 property hits), NanoMenuOverlay.cpp (29), NanoControlCenter.cpp (15), NanoMenu.cpp (241 property hits) and NanoMenuSettingsTree.cpp.
- Every moved line is a conflict site when upstream touches it. The conflicts are local (one call line each), but there are many.
- Principle 1 (IMPLEMENTATION.md:53) exists exactly to avoid this.
- Mitigation:
  - Do one area per commit, so cherry-picks stay small.
  - Push the seam upstream if possible. [unknown: whether GammaOS nano-144 would accept nanoplat calls; I found nothing on this]
  - Defer the largest mechanical set (Paths, Step 8) until the merge cadence is understood.

H.3 verbatim-move discipline
- Steps 3-9 fit H.3 if every firmware implementation is the original code, moved.
- Replacing Android's `/system/bin/curl` popen with libcurl on Android, or using std::filesystem in place of `rm -rf` on Android, would be a behaviour change and breaks H.3. Keep those changes Linux-only: firmware keeps the verbatim shell, Linux gets the native code.
- Unifying the two later is a separate, explicit Android change.
- I24 means files in drastic_nano_shared_srcs and the scrape srcs need the hook table or passed values, which adds a second small mechanism.

User-visible Linux regressions while capabilities are off
- Things that genuinely work on Linux today and would break if the gate went deny-all before Step 4:
  - the ES-DE theme downloader (REWR plus PASS)
  - the SetupWizard auto time zone (ipapi curl, TZ date)
  - streaming playlist fetches
- The streaming fetches already produce nothing user-visible, because NDK media decoding is stubbed and every create returns null (compat/stubs/src/ndk_media.cpp). So streaming loses nothing real.
- The OTA check "works" against the Android OTA server. Losing it is a fix.
- Therefore do Step 4 before deny-all, and the real loss is about zero.
- Rows that disappear under Caps (Step 2) are today silent no-ops or failures: DND, location, dark theme, notifications, Factory Reset, reboot to recovery, overlay app management, CPU profiles, MTP, the class-1b display and gamepad toggles. To a user, hiding them is an improvement, but the Linux build will look visibly smaller.

Schedule impact on the alpha [est.]
- Steps 0-1 are Linux-only: about 1-2 days.
- Step 2 (Caps plus G7-caps) is moved before the alpha. The docs put it after (IMPLEMENTATION.md:445). It is the largest pre-alpha AS item, a few days.
- Steps 3-4: about 3-5 days including Linux implementations.
- Total before the alpha: about 1.5-2 weeks [est.].
- Steps 5-10 come after the alpha, by area, over many weeks. Steps 8 and 9 are the long tail, and Step 9 is blocked on the NanoMenu.h unfreeze decision (G7D-2/3).

======================================================================
5. ALTERNATIVES AND RECOMMENDATION
======================================================================

(A) Full purity before the alpha: Steps 0-10.
- Pros: fully meets the principle; deletes the gate, ControlKeys, the stubs, EglWraps and the links.
- Cons: hundreds of uncompiled Android-surface edits, probably over 1,000 lines touched [est.], in nano-144's hottest files; a NanoMenu.h unfreeze; months of schedule; high risk of an Android regression found only after the AOSP job.
- Not recommended.

(B) Recommended: make it safe, then stop the bleeding, then migrate by area with a ratchet.
- Before the alpha:
  - Step 0, the ratchets.
  - Step 1, the hazard fixes (the typec mask above all).
  - Step 2, Caps plus UI hiding.
  - Step 3, close the seam bypasses.
  - Step 4, Http, FileOps, Archive and fixupLabels.
  - Then flip ShellGate to deny-all, with a hit counter that fails tests, and route platform spawns through posix_spawn to close linux-ux-15.
- After the alpha: Steps 5-10, one area per commit, each shrinking a ratchet, ending with ControlKeys, the stubs, EglWraps, the links and the gate deleted.
- Pros:
  - The owner's two concrete objections are fixed before the alpha. No Android command runs on Linux, and curl with Android arguments goes away.
  - Unsupported features become explicit (hidden, or Unsupported results) instead of silent.
  - Android changes stay small and verbatim, and the merge cost is spread out.
- Cons:
  - ControlKeys and the compat property emulation live until Step 5, so 67 silently swallowed keys remain for a while. The UI hides most of their features through Caps, which limits the damage.
  - The path links remain until Step 8.
  - Two seam styles coexist during migration: nanoplat and the I24 hook table.

(C) Keep the current approach, maybe with the Step 1 fixes.
- Pros: zero Android-surface churn and maximum merge friendliness, as designed (IMPLEMENTATION.md:53-55, I15).
- Cons:
  - It directly contradicts the principle.
  - The silent false successes stay: about 284 class-1b rows, 67 keys, 8 no-ops, Factory Reset.
  - The parser-race risk in the gate is permanent.
  - Wrong answers persist: RetroArch config, packages.list, RG Cube keys, Rockchip gauges.
  - The tests keep asserting Android commands.
  - Linux platform code needs a deliberate hole in its own gate.
  - Every new upstream feature arrives as more Android commands that Linux must classify.
- Not recommended. If chosen, at minimum do Step 0 and Step 1.

(B') Variant if the AOSP compile can't happen before the alpha: do only Steps 0 and 1 plus the Linux-only half of B.
- On the Linux side, make the gate refuse every PASS, REWR and NOOP except an explicit allow-list of pure-Linux strings. That allow-list is temporary.
- Land Steps 2-4 as the first post-AOSP batch.
- This keeps the alpha free of Android edits, at the cost of the ES-DE downloader and the wizard auto-zone for the alpha.

Recommendation: (B). Use (B') only if no Android compile pass is possible before the alpha.

The deciding facts:
1. The non-compliance is structural and deliberate (Principles 1-3, I14, I15, I16, I28). Small fixes to the gate do not reach the owner's principle; only moving call sites behind intent interfaces does.
2. The highest-value moves are small: about 50 sites in Steps 3-4, plus a Cap query. They remove every case where an Android command or Android-shaped arguments actually execute on Linux.
3. The large residue (paths, the SurfaceFlinger and media APIs, the class-1b rows) is mostly inert or hidden once Caps exist, so it can be migrated later under a shrinking ratchet without user-visible lies.
4. The typec flipper hazard (NanoMenu.cpp:4040, NanoMenuMtp.cpp:348-353, guard refused at ControlKeys.cpp:64) should be closed now, whichever option is chosen.

Owner decisions needed:
- Bring G7-caps before the alpha.
- Allow AS commits before an Android compile pass, and how many.
- The NanoMenu.h unfreeze (G7D-2/3), needed for Step 9.
- Whether to propose the nanoplat seam to GammaOS upstream.
- Accept the I24 hook-table mechanism for drastic_nano_shared_srcs files.

Key files:
- C:/Users/krist/code/thimsy-os-linux/nano/NanoPlatform.h
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ShellGate.cpp (timeout skipper :341-346)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ControlKeys.cpp (rows :50-74, refuse :231/:263)
- C:/Users/krist/code/thimsy-os-linux/nano/compat/src/properties.cpp (trySet :233, announce :242)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/linker-wraps.txt
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/display/EglWraps.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/actions/Run.cpp (:103-105)
- C:/Users/krist/code/thimsy-os-linux/nano/Android.bp (:740-763 drastic_nano_shared_srcs)
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/tests/test_shell_sites.py (:33, :78)
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/tests/test_nano_ratchets.py
- C:/Users/krist/code/thimsy-os-linux/docs/linux/IMPLEMENTATION.md (:53-55, :171, :263, :428, :445, :1013-1014, :1041-1055)
- C:/Users/krist/code/thimsy-os-linux/docs/linux/DECISIONS.md (:284, :902, :1057)
- Scratch inventories from the earlier reports, in C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\:
  - occ.json and cls.json (property keys and their classes)
  - all.txt and paths.py (path literals and the scanner)
  - count.py, count2.py and count3.py (the arch report's counting scripts)