CRITIQUE OF THE NANO PLATFORM-SEAM REDESIGN PROPOSAL (read-only; I built and ran nothing)

Counting method: grep -o over the top-level nano/*.cpp, *.h and *.inc files unless I say otherwise. Comments are included, so treat these as upper bounds.

======================================================================
0. CLAIMS CHECKED AGAINST THE CODE
======================================================================

Confirmed:
- Timeout skipper, nano/platform/linux/ShellGate.cpp:340-345. It skips dash words, then exactly one word. Real affected sites:
  - nano/NanoMenuMonitorMode.cpp:84 `timeout -s KILL 10 cmd display ...`
  - nano/NanoMenuPS3Menu.cpp:2469, inside nanoCaptureCmd. It has one caller, at :2483.
- compat/src/properties.cpp:233 trySet runs before :242 announce. ControlKeys.cpp:231 refuse() only logs, and property_set has already returned through the store path. A refused key is stored and the call returns 0.
- The gate is worse than "a deny-list by design": its default verdict is Pass.
  - ShellGate.h:55 and :60 declare `Verdict verdict = Verdict::Pass;`.
  - ShellGate.cpp:383-384: `const ToolRow* row = findTool(...); if (!row) return c;`
  - So any first word the gate does not recognise runs.
- The time-zone and time-format moves in Step 3 really are verbatim:
  - firmware SystemActions.cpp:96-103 matches NanoMenuPS3Menu.cpp:12189-12190 and :18454-18456.
- The RG Cube key default is used only when /system/etc/gammaos/device_keys.json is missing (NanoDeviceKeys.h:14, NanoDeviceKeys.cpp:180-182).
  - The only producer I found is mods/55-hotkeys.sh, which runs on Android.
  - Nothing under linux/ mentions device_keys, so the claim plausibly holds on Linux.
- No `__ANDROID__`, `__linux__`, `__BIONIC__` or `__GLIBC__` appears in top-level shared files. The ratchet regex (tools/nano-tree/nanotree/ratchets.py:14) checks only `__ANDROID__` and `NANO_PLATFORM_*`, so a future `__linux__` would not be caught.

Corrected:
- NanoMenuSettingsTree.cpp:1107-1111 is the "reset_wifi_bt" action (Wi-Fi and BT off, then on), not airplane mode.
  - Airplane mode is NanoMenuPS3Menu.cpp:12850-12851 (`cmd connectivity airplane-mode enable/disable`).
  - IMPLEMENTATION.md:445 already lists "the airplane commands" as not moved.
- Typec citations:
  - NanoMenu.cpp:4040 is `typecStart()`, called from the render loop.
  - The thread is NanoMenuMtp.cpp:277-289. Its sys.boot_completed gate is :293-297.
  - ControlKeys fakes boot_completed at ControlKeys.cpp:227 (:82 is only the key constant).
  - The port_type write and the refused guard are NanoMenuMtp.cpp:345-353.
- The I24-restricted set is larger than the proposal lists.
  - drastic_nano_shared_srcs (Android.bp:739-765) also holds NanoMenuDrmBuffers.cpp, DrasticRunner.cpp, DrasticGpu3d.cpp, DrasticSettings.cpp and FakeJNI.cpp.
  - I24 (IMPLEMENTATION.md:1051, B.6 :262) also covers every cc_test_host's srcs. That makes these restricted too:
    - NanoBiosWorker.cpp (gammaos_nano_bios_e2e_tests, Android.bp around :905-912)
    - NanoEsdeTheme.cpp (test modules at :800 and :1102)
    - NanoBiosUtil.cpp, which is listed 7 times (:148, 758, 804, 890, 912, 970, 1010) across gammaos-nano, the drastic filegroup, test modules and the nano-buttonmap host binary.

======================================================================
1. PURIST LENS (the owner's principle)
======================================================================

P1. Option (B) still attempts Android commands before the alpha. It only stops them from running.
- After Steps 0-4 the 96 "refused" sites and the 2 property-routed sites still build and issue Android shell strings on Linux. A deny-all wrap catches them.
- The owner said "Linux shouldn't need a gate". A deny-all gate is still a gate.
- So the pro "No Android command runs on Linux" is true only in the sense of "executes", never of "attempted". Say so to the owner.

P2. Step 1 keeps the gate default-allow (ShellGate.h:55). Flipping the default to Refuse, with an explicit temporary allow-list, is cheap and Linux-only. That is exactly what (B') proposes. It belongs in (B) Step 1, not only in the fallback.

P3. "Prefs" is the Android property API in disguise.
- Shared code has 384 `PROPERTY_VALUE_MAX` buffers in 53 files, which is the 92-byte Android limit baked into call sites.
- It has 60 low-level calls in 8 files: `__system_property_find` 24, `__system_property_serial` 33, `__system_property_wait` 3. The files are NanoMenu, Input, Overlay, PS3Clock, PS3Menu, Render, ThemeEngine and DrasticPark.
- The keys are named `persist.gammaos.nano.*`.
- No step migrates the 1,009 property calls to a Nano-owned Prefs API. Step 9 ends with "properties.cpp reduced to a plain store", so `cutils/properties.h` remains a shared-code API on Linux permanently.
- The serial and wait users are cross-process change detection. "Dropping serial bumps and `__system_property_wait`" needs a Prefs watch primitive first. The plan has none.

P4. On Linux the property store is a system IPC bus, not Nano's prefs.
- thimsy-session reads it: src/core/Requests.h, SleepPlan.cpp, Session.cpp.
- thimsy-padd reads it: src/io/Control.cpp, main.cpp.
- The D30net and D35bluetooth init scripts read `persist.thimsy.*` keys that the Linux SystemActions writes through property_set (platform/linux/actions/SystemActions.h:19-24, .cpp:156-170).
- `thimsy-prop boot` writes Android `ro.product.*` from device.json so that NanoDeviceKeys can match (I24 text :1051; S01props:5). That is Linux impersonating Android system state.
- The plan does not list thimsy-prop's `ro.*` emulation as an adaptation layer to remove. It also cannot reduce properties.cpp to a "plain store" while thimsy-session and thimsy-padd use it as a bus. [inferred]

P5. Settings rows carry the Android mechanism in shared data.
- `enum class SettingSource { kNone, kProp, kGlobal, kSecure, kSystem }` (NanoMenuSettingsTree.h:35-40) names Android's property system and the Settings provider tables.
- There are 497 row references: 437 kProp, 23 kGlobal, 12 kSecure, 20 kSystem and 5 kNone. 295 are in NanoMenuPS3Menu.cpp and 201 in NanoMenuSettingsTree.cpp.
- The dispatcher readSettingValue/writeSettingValue (NanoMenuSettingsTree.cpp:828-945) runs `settings get/put` through popen or system.
- Its post-write hooks (:887-940) are Android IPC that no inventory step covers:
  - `cmd color_display set-saturation`
  - the accelerometer_rotation write
  - the gammapad `config_version` bump
  - screencal
- Step 2b adds Cap annotations to these rows but leaves the mechanism in shared code.
- "Class-1b: each family becomes a platform method" has no step in 0-10 and is not sized.

P6. The I24 hook table leaves Android code in shared files.
- drastic-nano's main.cpp sits outside the Nano block and may not be edited (I24; B.6 :262), so drastic-nano cannot fill the table.
- Each hook therefore needs a default. The only safe default is the Android implementation, kept in the shared file.
- The clean form already exists in the design's vocabulary: link-time substitution, but at the intent boundary. For example, `NanoBiosUtilFixup_firmware.cpp`, listed by every Android.bp module that lists NanoBiosUtil.cpp (all inside the Nano block), and a Linux translation unit in CMake.

P7. Caps hide UI. They do not change behaviour.
- R3 requires unsupported methods to return `Unsupported`. Until Steps 5-9 those methods do not exist.
- Hidden rows still have unhidden entry points that reach the Android code through the gate:
  - hotkeys
  - Quick Menu
  - the Setup Wizard
  - XMB actions
  - writeSettingValue's hooks

P8. Class-3 reads have no step.
- 69 keys of Android system state are read, sys.boot_completed among them. ControlKeys.cpp:224-227 fakes it on the first frame.
- Step 7 covers only the 2 SoC branches.

P9. The Linux user-data layout is set by the I14 links, and Step 8 defers paths.
- State lives on the card under Android names (/data/system/nano_*, Android/data/<pkg>, /data/data/<pkg>).
- Deferring paths to the end means a user-data migration after people already hold alpha data. The Linux state root should be decided before the alpha even if the links stay.

P10. Minor: `fixupLabels` is a mechanism name (SELinux), which breaks R5. Name the intent, for example "make a file readable by emulators". The Linux no-op returning Ok is otherwise defensible.

======================================================================
2. ANDROID MAINTAINER LENS
======================================================================

A1. There is already an uncompiled Android backlog, and the plan stacks more on top.
- No P4 move (Display, Launch, Audio, Volumes, Power and SystemActions, Input, Network) has been compiled with the Android toolchain (IMPLEMENTATION.md:455; H.7; the I35 decision).
- DECISIONS.md:286 records an open AAudio callback use-after-free risk that waits for AOSP review.
- Steps 2-4 add more Android-surface code before the first compile. The ordering should start with: compile the existing G7 backlog, test it on a device, cherry-pick it to nano-144, then land new Android-surface commits.

A2. Step 3 is not a set of verbatim moves. It fails H.3's color-moved rule (IMPLEMENTATION.md:1013) and changes Android behaviour.

| Site | Today | After Step 3 | Drift |
|---|---|---|---|
| Wi-Fi tile, NanoControlCenter.cpp:1697 | `svc wifi enable/disable` through shellCmd (bare popen, no timeout, no lock; :1450-1453) | firmware setWifiEnabled (SystemActions.cpp:45-48) | tries nanoNetBridgeCmd first, which is fire-and-forget (the same file's comment at :51-62 documents the bridge silently doing nothing for forget); falls back to `cmd wifi set-wifi-enabled` through runCmd, which takes netHelperMutex and adds `timeout 3` (NanoMenuSettings.cpp:74-77, :96-97) |
| reset_wifi_bt, SettingsTree.cpp:1107-1111 | direct `cmd wifi` / `cmd bluetooth_manager` | setWifiEnabled / setBluetoothEnabled | gains the same bridge-first path, mutex and timeout |
| Brightness, ControlCenter.cpp:1615-1617 | synchronous `settings put` | mirrorBrightness (Power.cpp:227-247) | a detached, coalescing worker that skips a level it already pushed |
| Battery, ControlCenter.cpp:255-262 | reads capacity, voltage_now, current_now and status to compute watts | firmware readBattery (Power.cpp:170-180) | goes through the health HAL over binder first; has no voltage or current, so the interface would need extending; adds binder calls to the Control Centre loop; returns -1 while the HAL is not registered |

A3. Thread and lock timing.
- runCmd's mutex and 3 s cap were sized against the 8 s render watchdog (comment at NanoMenuSettings.cpp:86-95).
- Routing Control Centre touch actions through runCmd puts them behind any thread holding netHelperMutex.

A4. Some steps break the I24 ratchet as written.
- Step 4 and 6a route NanoBiosWorker (5 sites) through nanoplat, but NanoBiosWorker is in a cc_test_host module.
- Step 7 edits NanoDeviceKeys. I24 says it "is not edited", and that decision is missing from the owner-decision list.
- Step 7's rk356x branch (NanoMenuDrm.cpp:1155) and Step 9's "Display method for AHardwareBuffer/EGL import" sit in I24-restricted NanoMenuDrm and NanoMenuDrmBuffers.
  - The compliant route is the existing back-end header (I26).
  - The documented EglWraps fallback, a `drmBuffersAvailable(eglDpy)` gate move (IMPLEMENTATION.md:485), also stays inside that back end.

A5. Step 2b touches nano-144's hottest file row by row.
- NanoMenuPS3Menu.cpp has 295 SettingSource references and 74 dispatcher calls. Per-row Cap annotations produce many conflict hunks on every upstream edit to those tables.
- "All caps true on firmware" makes the behaviour neutral, not the merge.
- Filtering table-driven menus may shift index-based selection. [inferred; not checked]

A6. Internal contradiction on FileOps.
- Section 2 says mkdir, rm and mv become std::filesystem, "portable, can live in shared code".
- Section 4 says replacing `rm -rf` with std::filesystem on Android breaks H.3.
- IMPLEMENTATION.md:41 also records that Nano uses no std::filesystem today.
- So FileOps must be a platform interface with a verbatim firmware side.

A7. Hook-table defaults in drastic-nano.
- An unfilled hook would skip NanoBiosUtil's restorecon and chown to Android uids. BIOS files would then carry wrong labels, and the emulator would be denied. [inferred]
- So the defaults must be Android's code verbatim, which is P6's purity failure.

A8. Step 5 ordering on Linux.
- Deleting ControlKeys removes the first-frame `sys.boot_completed=1` write (ControlKeys.cpp:227).
- These readers then never proceed: NanoMenu.cpp:1895, 2878, 3651 and 3936; NanoMenuMtp.cpp:295 and 377.
- Per the comment at ControlKeys.cpp:224-226, idle work, the background scan and real sleep are gated on it.
- A `Session::bootCompleted()` (class 3) has to land before the deletion.

A9. Step 1a's mask mechanism does not work as stated.
- NanoJail bind-mounts /dev/null over each path (linux/src/thimsy-session/src/core/NanoJail.cpp:62-70) and fails on any error except ENOENT.
- Session.cpp:550 runs it as preExec, so a failure aborts the Nano start.
- Binding a file over the /sys/class/typec directory gives ENOTDIR, so Nano would not start.
- The fix is a read-only tmpfs, or an empty-directory bind, over the class directory. That is still Linux-only and feasible. Nano reaches typec only through `typec::kClass` (NanoTypec.h:11; NanoMenuMtp.cpp:223, 315-319). The docs never mention the typec risk.

A10. Upstream merge cost repeats.
- Every new upstream `system()` call or settings row has to be moved again by the fork.
- The ratchet detects it but does not reduce the work. Only a seam at a dispatcher (below) makes new upstream rows cost nothing.

======================================================================
3. JUDGEMENT
======================================================================

The diagnosis is accurate. (B) is the right direction over (C), and the typec item is rightly urgent. Three things are wrong:
- The pre-alpha scope rests on two weak pieces: Step 2b's per-row Caps, and Step 3's non-verbatim redirects.
- It overstates what (B) delivers: refused is not the same as not attempted.
- It misses the Prefs, class-3 and SettingSource residue.

The single change that would most improve it: cut the seam at the settings dispatcher instead of annotating rows.
- Move readSettingValue and writeSettingValue, with their post-write hooks (NanoMenuSettingsTree.cpp:828-945), verbatim into platform/firmware as `Settings::read`, `Settings::write` and `Settings::supports(source, key)`.
- Use supports() where rows are built or filtered. There is already a skip loop at NanoMenuSettingsTree.cpp:974.
- Linux implements a keyed table: native where it exists, Unsupported otherwise. An unknown key fails closed.

What this buys:
- Its 82 call sites (74 PS3Menu, 7 SettingsTree, 1 Input) and the 492 table rows go through one Android-surface move that passes H.3.
- No row lines change in NanoMenuPS3Menu.cpp.
- It replaces Step 2b's ~284 annotations and the unscheduled "class-1b families become methods".
- New upstream rows are automatically Unsupported on Linux.
- It removes `settings get/put` and the gammapad, saturation and rotation IPC from shared code.
- The keys remain as opaque ids. That is the remaining purity cost.

Secondary amendments, in order:
1. Add a step before Step 2: compile and device-test the existing G7 firmware backlog.
2. Limit Step 3 to true moves (time zone, time format). Treat the Wi-Fi tile, reset_wifi_bt, brightness and battery as explicit Android changes, or drop them.
3. Flip the gate's default to Refuse in Step 1, with a temporary allow-list.
4. Replace the hook table with per-platform translation units chosen by the build.
5. Add Session::bootCompleted and the class-3 reads before deleting ControlKeys.
6. Fix the I24 file list: NanoBiosWorker, NanoEsdeTheme, NanoMenuDrm and NanoMenuDrmBuffers, NanoDeviceKeys.
7. Mask typec with tmpfs, not a /dev/null bind.
8. Decide the Linux state root before the alpha.
9. Add a Prefs watch API, and decide whether Linux Prefs keep the 92-byte limit.

Key files:
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuSettingsTree.cpp (:828-945, :1107-1111)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuSettingsTree.h (:35-40)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ShellGate.h (:55, :60)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/ShellGate.cpp (:340-345, :383-384)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/firmware/SystemActions.cpp (:45-48, :96-103)
- C:/Users/krist/code/thimsy-os-linux/nano/platform/firmware/Power.cpp (:170-180, :227-247)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuSettings.cpp (:74-118)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoControlCenter.cpp (:255-262, :1450-1453, :1615-1617, :1697)
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuMtp.cpp (:277-353)
- C:/Users/krist/code/thimsy-os-linux/linux/src/thimsy-session/src/core/NanoJail.cpp (:54-75)
- C:/Users/krist/code/thimsy-os-linux/nano/Android.bp (:739-765, :888-1010)
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/nanotree/ratchets.py (:14, :17-29)
- C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/upstream.json (fork_only)