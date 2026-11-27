ARCH AUDIT: Nano platform seam as designed and as built (thimsy-os-linux, branch linux). Read only, nothing edited. All counts were made by me unless I say otherwise. Code counts used the repo's own lexer (`tools/nano-tree/nanotree/ratchets.py` `lex`, so comments and strings are blanked out) over `nano/build/sources.txt [core]` (105 files) or over `shared_sources()` minus test/ and compat/. My scratch scripts are C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\count.py, count2.py and count3.py.

== 0. The seam as built ==

**NanoPlatform.h** (460 lines) declares 9 abstract classes with 58 pure-virtual methods. I counted `) = 0;` per class.
- Display 3: startScanout, releaseForChild, reacquire.
- Launcher 6: nanoExitsOnLaunch, commit(android::LaunchTuple), launch, childRunning, pollChild, requestChildQuit.
- AudioStream 4 and Audio 1 (open).
- Volumes 5: cardRoot, isSlot, present, absent, writableFolders.
- Power 5: readBattery, setBrightnessViaHal, mirrorBrightness, savedBrightness, powerAction.
- SystemActions 6: setWifiEnabled, forgetWifiNetwork, setBluetoothEnabled, connectBluetooth, setTimeFormat, setTimeZone.
- Network 25.
- InputPolicy 3: deviceOpened, deviceClosing, silenceGamePad.
- `struct Platform` has 8 unique_ptr members (NanoPlatform.h:431-441). NanoPlatform.cpp (53 lines) holds only install() and get(), with an atomic pointer that is never freed.
- There is no Cap, `has()` or `supported()`. B.6 says why: "Its members that no moved code used are not declared: ... `SystemActions`' `Action` enum, `run` and `supported`; ... and `Cap` with `Platform::has`" (IMPLEMENTATION.md:263).

**createPlatform() in each platform**
- platform/firmware/Platform.cpp:89-101, Linux LinuxPlatform.cpp:134-146 and host HostPlatform.cpp:85-144 each fill all 8 members, one line each.
- HostPlatform also opens the store, sets the record-only shell rules (`shell::setRules`, :117) and records every property write as an event (:119-123).

**Shared code calling the seam:** 59 `nanoplat::get()` uses in 10 shared files (NanoMenuSettings.cpp 36, NanoMenuInput 5, NanoMenuSystem 5, NanoSdMove 3, NanoAudio 2, NanoEsdeStore 2, NanoMenuNet 2, NanoMenuSetupWizard 2, NanoLaunchCommit 1, NanoMenu.cpp 1).

**Interfaces that keep Android semantics in their shape:**
- `Launcher::commit(const android::LaunchTuple&)` is "the property writes in the call site's order". On Linux, "Linux's writes the same keys, and the handshake's trigger starts the game ... through the control keys" (NanoPlatform.h:82-86). LaunchRequest's fields are the handshake keys (:90-99).
- Power: `setBrightnessViaHal`, `mirrorBrightness` (from "syncBrightnessToAndroid"), `savedBrightness` (from "readAndroidBrightness"), and `powerAction` takes "recovery", "safemode", "android", "which only the firmware has" (:212-229).
- Network's methods are "named after their call sites", and NanoNetBridge "stays at the call sites" (:268-270).

**Layer-3 Linux adaptation**
- linker-wraps.txt (14 lines) wraps 8 symbols: system, popen, pclose, execv, execvp, execve, eglGetProcAddress, eglQueryString.
- ShellGate.cpp:65-78, kAndroidTools: 39 tool words.
  - Routed: svc, setprop, getprop, reboot.
  - No-op: restorecon, chcon.
  - Rewritten: gammaos-net, nsenter.
  - Refused: the other 31.
- ShellGate.cpp:82-89, kPosixTools: 71 names whose `/system/bin/<tool>` path is rewritten to the Linux tool. curl is one of them.
- Absolute Android roots /system, /vendor, /product, /odm, /apex and /system_ext pass only if the image has the program (:92-94).
- ControlKeys.cpp `kControlRows` has 23 rows (:50-74), checked against `control-keys.json` (67 rows, G9-8).

== 1. Feature areas behind nanoplat versus still in shared code ==

**Behind nanoplat (B.6 table, IMPLEMENTATION.md:432-441):**
- Display: the readyToRun DRM splash decision and the hand-off.
- Launch: commitLaunch's writes, 69 calls in 9 files.
- Audio: the AAudio streams.
- Volumes: the /storage scans.
- Power: battery, HAL brightness, Settings mirror, the prepareShutdown action.
- SystemActions: the Wi-Fi and BT radios, forget, BT connect, time format, time zone, from Settings and the SetupWizard.
- Network: the Settings Wi-Fi and BT screens and NanoMenuNet's poll.
- InputPolicy: EVIOCGRAB per device.

**Still in shared code, per the docs:**
- Display: "Not moved: the headless pbuffer EGL set-up, the SurfaceFlinger window surface and NanoMenuRender.cpp's present (G7D-2, G7D-3: NanoMenu.h is frozen)" (IMPLEMENTATION.md:434).
- Audio: "Not moved: the player's gain, the system volume (G7-audio-2) and NanoBootChime.cpp" (:436). G7-audio-2 adds: "the system volume (`cmd media_session volume` in NanoControlCenter.cpp) is not this step's" (DECISIONS.md:936).
- Power: "Not moved: NanoControlCenter.cpp (already sysfs; its svc/settings calls are built into shared strings), NanoMenuNet.cpp's poll, sleep ... and finishSetupWizard's provisioning block (calls a private member)" (DECISIONS.md:954, G7-power-3 to -6).
- Network: "Not moved: NanoControlCenter.cpp's wifi_on read, NanoBtStable.cpp and the airplane commands" (IMPLEMENTATION.md:440). G7-net-7: connectWithWizardSettings (`gammaos-net wifi configure`) was not moved because "it is built from a dozen private wizard members" (DECISIONS.md:1037).
- "Still Android-only: NanoControlCenter's wifi_on tile, NanoBtStable's A2DP dumpsys, the PS3 menu's discoverable and radio rows (G7-caps)" (DECISIONS.md:1042).
- "The PS3 menu's own time, zone, radio and auto-time commands are not moved (G7-caps); the shell gate routes its timezone setprop to the store, but the localtime link is not rewritten" (DECISIONS.md:1014).
- Input: NanoMenu.cpp's exit-time EVIOCGRAB and the destructor release are direct ioctls; NanoDeviceKeys reads ro.product.* through the shim (IMPLEMENTATION.md:441; DECISIONS.md:1015).
- Settings store and paths are not interfaces at all: "Nano reaches it through the property shims (layer 1)"; "Paths are not an interface yet: the filesystem mapping (B.5) covers Linux" (IMPLEMENTATION.md:428).
- Control-key messages are still Android property writes in shared code, translated by ControlKeys: dosleep, park_restart, fixedperf, do_launch, nano_retroarch, drm_active, nano_action.

**What I measured in [core]:**
- Spawner calls (system, popen, fork, execv, execvp):
  - `system(` 73 calls in 13 files: NanoControlCenter, NanoMenu, NanoMenuMtp, NanoMenuOverlay, NanoMenuPS3Folder, NanoMenuPS3Menu, NanoMenuRender, NanoMenuSettingsTree, NanoMenuSetupWizard, NanoMenuState, NanoMenuStrings, NanoMenuXmb, NanoSdMove.
  - `popen(` 25 calls in 12 files: NanoControlCenter, NanoHls, NanoIcyDemux, NanoMenuIptv, NanoMenuMonitorMode, NanoMenuOverlay, NanoMenuPS3Menu, NanoMenuRadio, NanoMenuRender, NanoMenuSettings, NanoMenuSettingsTree, NanoMenuSetupWizard.
  - `fork(` 4 calls in 3 files; `execv` 2 (NanoMenuEsdeMenu, NanoSdMove); `execvp` 2 (NanoOtaCheck).
  - Wrapper helpers multiply these: esdeRunProc 16 uses, runArgv 11, shellCmd 6, runProcess 5, overlayShellCapture 4, runMonitorModeCmd 2, nanoCaptureCmd 2.
- Over all shared top-level files (adding the [firmware] DrasticRunner and NanoBtStable): system 74 in 14 files, popen 28 in 14, pclose 28 in 14.
- Android-tool literals: 115 string literals in 14 [core] files whose first word is one of 23 Android tool words followed by a space (heuristic: a few English false positives are possible, and commands assembled from variables are missed). Top files: NanoMenuPS3Menu 40, NanoMenuOverlay 29, NanoControlCenter 15, NanoMenuSettingsTree 7, NanoMenuSetupWizard 6, NanoSdMove 4.
- 48 literals of the form `/system|vendor/.../bin/...` in 14 top-level files.

**Examples (file:line):**
- NanoMenuPS3Menu.cpp: 2554 `am start -a android.settings.SETTINGS`; 4979 `dumpsys notification`; 7254 `am start ...GAMEPAD_CALIBRATION`; 12189 `settings put system time_12_24`.
- NanoMenuSettingsTree.cpp:1107-1111: airplane mode via `cmd wifi` and `cmd bluetooth_manager`.
- NanoMenuMtp.cpp:182-198: `gammaos-net usb`.
- NanoMenuRender.cpp: 439 `screencap`; 6259 `settings get system font_scale`.
- NanoMenu.cpp: 1746 and 2042 `/vendor/bin/setclock_max.sh`; 4729 `settings put global device_provisioned`.
- NanoMenuPS3Folder.cpp: 881 `am force-stop`; 888 `restorecon`; 1199 `appops`.
- NanoMenuState.cpp:393: `pm enable`.
- NanoMenuXmb.cpp:3714: `am broadcast`.
- NanoControlCenter.cpp: 282 `cmd media_session volume`; 307 and 334 `settings get global ...`; 1274 `am force-stop`.
- NanoHls.cpp:147 and 249, NanoMenuRadio.cpp:108 (also NanoMenuIptv.cpp): `/system/bin/curl ...`. The gate rewrites these to the Linux curl and runs them unchanged. This is the owner's curl case.

**Platform code already in place:**
- platform/firmware/*.cpp holds 3 system, 2 popen and 3 pclose calls, 42 runCmd/runCmdUnlocked uses, and 49 Android-tool literals (Network.cpp 37, SystemActions.cpp 10, Power.cpp 2).
- The moved firmware code calls back into shared helpers: `runCmd` "gets external linkage" (G7-power-2, DECISIONS.md:953), as do `runCmdUnlocked` and `shellQuote` (G7-net-6). So firmware code depends on helpers that live in shared NanoMenuSettings.cpp.

**Property keys:** props.txt has 994 key literals in shared files. 99 are not Nano's own `(persist|sys|ro).(gammaos|thimsy).*` keys: ctl.start/stop/restart, service.bootanim.*, sys.powerctl, sys.boot_completed, persist.sys.timezone, persist.sys.locale, 56 persist.sys.* keys (mostly spk/gammaeq DSP), ro.surface_flinger.primary_display_orientation, ro.input_flinger.*, ro.build.*, ro.lineage.*, init.svc.*, sys.system_server.start_count, vendor.audio-hal and others.

**Host E2E scenarios assert on Android commands:** they script `"match": "am start"`, `"settings put system font_scale "` and `"settings get "` (nano/test/host/scenarios). An unmatched command fails the scenario (G5-17, DECISIONS.md:747).

**The gate also catches the platform's own commands:** "Nano's runProgram ... calls the raw SYS_execve, which bypasses the shell gate's execve wrap (these are Thimsy's own fixed commands) ... For owner audit, since it is a deliberate hole in the gate" (linux-ux-15, DECISIONS.md:1057; code at platform/linux/actions/Run.cpp:103-105). Because the wrap is process-wide, Linux platform code had to bypass it.

== 2. Why the docs chose gate + ControlKeys + compat ==

**Principles:**
- "Merge-friendly first. nano-144 keeps moving. Every step prefers a change that adds files over one that edits Nano's existing files" (IMPLEMENTATION.md:53).
- "The seam is cut at the lowest boundary that leaves call sites alone. In order: header shims (compat/), link-time substitution ..., filesystem mapping ..., and only then interfaces in NanoPlatform.h, where behaviour really differs" (:54).
- "Android behaviour does not change in any step that is not explicitly an Android change" (:55).

**Layers 1 to 3:** "give a Linux binary with no edits to Nano's existing files, so the Android build is unchanged and nano-144 merges stay one-sided. Layer 4 is the comprehensive refactor" (:171).

**I15, the shell gate (:1042):**
- Why: "zero edits at the call sites; nothing Android-only runs as root on Linux; three files fork and exec directly, which a system-only gate missed".
- Would change it: "--wrap failing with the toolchain's LTO (then no LTO, or a nanocompat_system rename in P4)".
- Its mechanics are in B.5 (:255).

**Control keys:** I28 says "Control-key handlers observe after the store writes and never consume ... the keys are written from nine files and some are read back as state; a consuming handler would break those readers" (:1055; B.2 at :194).

**Upstream source of the approach:** linux_handheld_plan.md:399 says "Rewriting those call sites would make every later upstream merge conflict ... Real NanoPlatform.h interfaces are cut only where behaviour differs: display, input source, launch, audio, storage, network, power, system actions". android_launcher_plan.md:335 says "The hand work is the 114 shell commands in 27 files ... Those are the platform layer's real surface."

**Constraints:**
- Android code can't be compiled now. "Whether all of Nano compiles under g++ was not checked ... the development headers it needs are not installed" (IMPLEMENTATION.md:47). "None of the firmware code has been compiled with the Android toolchain" (DECISIONS.md:902). "None of the G7 firmware code is compiled" (:1002).
- P4 moves must be verbatim. "P4 edits are moves, not rewrites ... A new behaviour on Android is never part of a P4 commit. Every P4 commit ... carries its android-surface: line" (H.3, IMPLEMENTATION.md:1013). Each is flagged for cherry-pick to nano-144 after the AOSP job (DECISIONS.md:902-904).
- NanoMenu.h is frozen. "NanoMenu.h is not edited by any G7 step" (IMPLEMENTATION.md:990). Stubs rather than edits, because NanoMenu "derives from Thread and IBinder::DeathRecipient ... can't be made portable without touching the most-edited file in nano-144" (B.4, :249). G7D-2 and G7D-3 are deferred to the owner: "allow a G7-display-b step that adds accessors to NanoMenu.h" (DECISIONS.md:284).
- I24, the frozen-API rule. Files compiled by drastic-nano, gammaos-nano-scrape and the host tests "may not include NanoPlatform.h or call nanoplat::; those files reach platform behaviour through layers 1 to 3" (:1051; B.6 :265).
- I27: interfaces are "declared in the G7 step that moves their code, sized to it ... interfaces designed ahead of use would be frozen wrong" (:1054).
- I14, path links: "zero edits to shared files" (:1041). I16, inert stubs: "editing NanoMenu.h would conflict with nano-144" (:1043).

== 3. Documented intended end state ==

No document states an end state in which the shell gate or ControlKeys goes away, shrinks to zero, or becomes a tripwire. What the docs do say:
- Layer 4 is "the comprehensive refactor ... one area per commit" (:171).
- "layer 4 lowers the [stub] budget as it moves code out" (B.4, :247).
- I15's only exit is "a nanocompat_system rename in P4", and that applies only if LTO breaks `--wrap`.
- G7-caps (after the alpha) hides Settings and XMB rows by Cap. "Until then the Linux build shows those rows, and the firmware-only features behind them fail at run time with a message (their stubs fail, B.4)" (:445, :988).
- The curated `/system/bin` is "empty in the alpha" (:255).
- The resident launcher: "a resident Nano that hands the panel over and polls the child is the intended contract, not yet built" (:472).
- The Paths interface waits until the launcher app needs it (:428).
- Plan 4.1: "Nano's shared code never includes an Android or Linux system header. It calls NanoPlatform.h, or a compat/ shim" (linux_handheld_plan.md:363). This is a header rule, not a rule about which commands shared code may run.
- The launcher plan's end state for system actions is "Android APIs where one exists; shell or root where not; otherwise the row is hidden" (android_launcher_plan.md:134).

**Pending owner items on the gate:**
- "Accept the shell gate's documented limits, or have a G7 step narrow them" (DECISIONS.md:847; AUDIT.md:62).
- linux-ux-15, the raw-execve hole (AUDIT.md:97).

== 4. How tests enforce the seam today, and what a stricter rule would need ==

**Today (tools/nano-tree/tests, fork-only, never in tests/ quick):**
- FrozenApi (test_nano_ratchets.py:78-158): Android.bp's drastic_nano_shared_srcs plus every cc_* module except gammaos-nano, followed transitively through includes. It fails if any of them includes NanoPlatform.h or names `nanoplat::`. G5-4: wider than I24.
- PLATFORM_CONDITIONAL_BUDGET = 0 (test_nano_ratchets.py:27): no `__ANDROID__` or `NANO_PLATFORM_*` in shared code. This already matches the owner's "no platform specifics" rule for #ifdefs.
- test_property_keys_match_baseline: the set of property keys must equal props.txt (994 keys). It controls change, not kind; Android-only keys are allowed.
- test_shell_sites.py:
  - test_every_spawner_is_wrapped: each spawner Nano calls must be in linker-wraps.txt.
  - test_exec_sites_have_rows: exactly 3 exec sites (EXEC_SITES :33-40).
  - test_system_and_popen_are_used only asserts at least 50 system and 10 popen calls (:78-81).
  - So there is no ratchet on how many system/popen calls shared code makes. This contradicts H.4, which claims "system/popen call sites ... in shared files may not grow without a row" (IMPLEMENTATION.md:1014). shared_sources() also counts platform/firmware/** as shared, because it is not fork-only.
- check-stubs (test_nano_stubs.py):
  - test_budget_only_shrinks: stubs-budget.txt = 446 declared names.
  - test_core_reaches_no_unstubbed_android_header, the Bionic-identifier and block-device baselines, and the source split from the include census.
- The sources census (test_nano_sources.py): sources.txt partitions Android.bp: [core] 105, [firmware] 21, [dropped-linux] 10, [drm] 1, [linux] 19, [host] 16, [scrape] 6, [test-unit] 54. It also checks [dropped-linux] ⊆ [firmware].
- test_control_keys.py: every rc trigger and listener has a disposition, and kControlRows matches the JSON.
- test_path_roots.py: every absolute root has a disposition (33 rows).
- test_android_surface.py: every Android-surface diff has an `android-surface:` line.

**What "shared code calls no system/popen and no Android-only property keys" would need:**
1. New ratchets over [core]. Count spawner calls (today system 73, popen 25, fork 4, exec 4, plus the wrapper helpers above) and Android-tool command literals (about 115). Both should only shrink, toward a target of 0.
2. Split props.txt into Nano-own keys and an Android-system list (the 99 keys above, plus the routed control-key triggers), and ratchet that list down.
3. Make test_shell_sites count only [core], not platform/firmware, and fix the H.4 wording mismatch.
4. Declare a capability query: the dropped `Cap`/`Platform::has` or `SystemActions::supported()`. Today shared code cannot ask whether a feature exists, so an unsupported feature can only fail through the gate or a stub. This is the G7-caps step, which edits NanoMenuSettingsTree.cpp and the XMB builders.
5. Interface methods for what is still in shared code:
   - System settings: font_scale, time_12_24, provisioning, airplane mode.
   - App control: am start, am force-stop, am broadcast, pm enable, appops.
   - Media volume.
   - USB mode: `gammaos-net usb`.
   - Screenshot: `screencap`.
   - Perf clock: `setclock_max.sh`, fixedperf.
   - HTTP fetch, for curl in HLS, Radio, IPTV and the OTA check.
   - nsenter/unzip in EsdeMenu.
   - am and restorecon in SdMove.
   - Explicit launch, sleep, restart and first-frame calls in place of the do_launch, dosleep, park_restart and drm_active property messages. ControlKeys exists only because these are property writes.
   - Brightness and power without the Android-named methods.
   - The whole Overlay and the PS3 quick actions behind a Cap.
6. Each of these moves conflicts with the stated constraints:
   - It edits about 14 to 19 hot shared files, against principle 1 and nano-144 merges.
   - Each move is an Android-surface change that can't be compiled now.
   - Several moves need private NanoMenu members, and NanoMenu.h is frozen: G7D-2/3, G7-power-6, G7-net-7.
   - Files in drastic_nano_shared_srcs and the scrape srcs cannot call nanoplat:: (I24). They would need values passed in, or a declared exception landed on nano-144 first.
7. Once the call sites are gone, the gate could shrink to an assert-or-log tripwire (refuse everything, with a test that it is never hit), and linux-ux-15's raw-execve bypass would no longer be needed. No document proposes this today.

**Key files:**
- C:/Users/krist/code/thimsy-os-linux/nano/NanoPlatform.h
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ShellGate.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ControlKeys.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/linker-wraps.txt
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/actions/Run.cpp
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/tests/test_shell_sites.py
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/tests/test_nano_ratchets.py
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/props.txt
- C:/Users/krist/code/thimsy-os-linux/nano/build/sources.txt
- C:/Users/krist/code/thimsy-os-linux/docs/linux/IMPLEMENTATION.md
- C:/Users/krist/code/thimsy-os-linux/docs/linux/DECISIONS.md