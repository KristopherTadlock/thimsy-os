# Nano platform architecture audit (2026-11-26)

This audit measures the fork against the owner's decision OA-4 (DECISIONS.md):

- Shared Nano code holds the model, views and controllers.
- Every low-level platform action lives in that platform's implementation.
- No Android command runs on Linux, or the reverse.
- A feature a platform has not implemented reports itself unsupported.

The audit was read-only and nothing was built or run. Five inventories, one design, a fact-check and a critique are in [audit-2026-11-26/](audit-2026-11-26/). The fact-check corrects the design in places, and this summary uses the corrected numbers. The claims marked "checked" below were re-read in the code while writing this summary.

## Verdict

The Linux build does not have a platform layer in OA-4's sense. It emulates Android underneath unchanged Android code, which is what IMPLEMENTATION.md's principles 1 to 3 chose on purpose: merge-friendliness first, and no edits at the call sites. The adaptation layers that do this are:

- **ShellGate:** a link-time wrap of `system`, `popen` and `exec*`. It parses Android shell strings, then routes, rewrites, no-ops or refuses each command. Its default verdict is *pass* (`ShellGate.h:55`, `ShellGate.cpp:383-384`).
- **ControlKeys:** turns property writes meant for Android daemons into Linux actions, after the store has already accepted the write.
- **compat:** an Android property emulation (`compat/src/properties.cpp`, with the 92-byte limit, serials and waits), 446 inert stub symbols for SurfaceFlinger, binder, HALs, NDK media and AAudio, AID uid shims, and `EglWraps`, which makes Mesa look like Android EGL.
- **Path links (I14):** rebuild `/data`, `/sdcard`, `/system/...` and `/mnt/media_rw` on the Linux root.

Already clean:

- No platform `#ifdef`s in shared code.
- No Linux names in shared code.
- Nine `nanoplat` interfaces (58 methods) with 48 command sites moved behind them.

## Size of what is left (Linux-compiled shared code)

| Leak | Count |
|---|---|
| Raw `system`/`popen`/`fork`/`exec` sites | 100 in 20 files; about 133 command sites counting helpers. NanoOtaCheck and NanoArchive are compiled out on Linux |
| Their fate on Linux | most refused; 24 pass to a real Linux program, 18 of them via hard-coded `/system/bin` paths; 8 `restorecon` no-ops that report success; 2 property routes; 2 rewrites |
| Property API calls | about 1,300 in 57 files; `property_set` uses 162 distinct literal keys. About 96 keys are IPC to other Android processes, and about 67 of those are stored silently on Linux with no effect and no log |
| Android-header includes | 295 lines in 90 files; `NanoMenu.h` alone pulls binder, Thread, sensor, looper and system_properties into 53 files |
| Android path literals | 783 (`/data` 322, `/storage` 142, `/system` 106, `/mnt` 59, `/sys` 54, ...) |
| Device-specific literals | Rockchip GPU and thermal nodes, Unisoc Type-C and SoC branches, RG DS backlights, TrimUI Brick rfkill, RG Cube key map as the default |
| Android shape in the interface itself | `Launcher::commit(android::LaunchTuple)`, `Power::setBrightnessViaHal`/`mirrorBrightness`, `powerAction("recovery"\|"safemode"\|"android")`, `Display::startScanout(..., forceSfPath)`; no capability query |

## Defects found (Linux; none run yet, because nano-headless has never been built)

1. **Nano exits during every Linux boot** (checked, `NanoMenu.cpp:491-561`, in `readyToRun`):
   - Android's boot-mode decision ("run Nano or Android's boot animation") runs in shared code.
   - On Linux nothing writes `/data/misc/bootanim/nano_skip` or `persist.bootanim.skip_nano`, so Nano waits 10 s for `ro.persistent_properties.ready`, takes the boot-animation branch, writes a refused `ctl.start bootanim` and calls `_exit(0)`.
   - The session then restarts it, so Linux would never reach the home screen.
   - The host E2E scenarios that would have caught this skip because nano-headless is unbuilt (G10).
2. **Android commands reach Linux's shell** (checked):
   - The gate's `timeout` skipper (`ShellGate.cpp:340-345`) takes `KILL` as the duration in `timeout -s KILL 10 cmd ...`, so the `cmd` passes. Two sites hit this: `NanoMenuMonitorMode.cpp:84` and `NanoMenuPS3Menu.cpp:2469`.
   - More generally, any command the gate does not recognise runs.
3. **Linux tools run with Android-shaped arguments:**
   - `chown -R system:system`;
   - `pidof <package name>`;
   - `/system/bin/curl` and `unzip` through the path links.
4. **The USB-C role flip can write sysfs on Linux** (checked, `NanoMenuMtp.cpp:340-353`):
   - The Android Type-C helper writes `port_type` when a PD port with the right conditions exists.
   - Its crash guard (`ctl.restart nano-typec-guard`) is refused on Linux.
   - It is conditional, but it is a hardware hazard of the kind recorded for the RG Cube.
5. **Silent false success:**
   - Factory Reset, reboot to recovery and safe mode, about 67 control keys and the 8 `restorecon` calls succeed and do nothing.
   - A property value over 91 bytes fails silently.
   - About 284 settings rows store a value nothing reads.
6. **Wrong answers:**
   - The RetroArch config Nano edits is not the one Linux RetroArch reads.
   - "RetroArch running" scans for an Android package name.
   - Every Linux device gets the RG Cube's key map.
   - The GPU and thermal gauges read Rockchip node names.
   - The XMB time-zone picker writes a property nothing on Linux reads; the wizard's path does work.
   - The battery gauge bypasses `Power::readBattery`.
7. **Enforcement gaps:**
   - `test_shell_sites.py` sets a floor on `system`/`popen` sites, not a ceiling (contradicting H.4).
   - The ratchets count `platform/firmware` as shared code.
   - The ratchet regex misses `__linux__`.
   - Host scenarios assert Android command strings ("am start", "settings get").

## Target architecture

- **R1:** Shared code expresses intent only. It may use `nanoplat` interfaces and portable facilities: logging, clocks, threads, JSON, image decoding, and file I/O on paths the platform supplies. It runs no processes.
- **R2:** Every Android command, IPC property, Android API and Android path lives in `platform/firmware`. Moves are verbatim (H.3).
- **R3:** Linux implements each capability natively or declares it unsupported. `has(Cap)` returns false, the methods return `Unsupported`, do nothing and log once. A capability is never attempted the Android way, and never stored as if it worked.
- **R4:** The UI asks `has(Cap)` and hides unsupported entries.
- **R5:** Interfaces are named for intent. No `android::` types, no "ViaHal", no Linux names.
- **R6:** Device knowledge (nodes, key maps, SoC quirks) comes from each platform's device profile, never from literals in shared code.

Two amendments from the critique, adopted in this summary:

- **Cut the seam at the settings dispatcher, not row by row.**
  - `readSettingValue`/`writeSettingValue` and their post-write hooks (`NanoMenuSettingsTree.cpp:828-945`) move verbatim into `platform/firmware` as `Settings::read`, `write` and `supports(source, key)`.
  - One move covers about 492 table rows and 82 call sites, with no row edits in `NanoMenuPS3Menu.cpp`.
  - Linux answers from a keyed table, so an unknown key, including any new upstream row, is unsupported by default.
- **A Nano-owned preferences API.** Today the property API is Nano's settings store in disguise: 384 `PROPERTY_VALUE_MAX` buffers, serial and wait calls for change detection. Shared code moves to a `Prefs` facade with a watch primitive. Android keeps `persist.gammaos.nano.*` underneath, and Linux uses its own store. Linux's own daemons (thimsy-session, thimsy-padd, init scripts) using the property store as a bus is a Linux-internal choice and outside OA-4's shared-code rule.

What happens to the adaptation layers:

- **ShellGate:** becomes deny-all (no routes, rewrites or passes; every hit logged and failing the tests), then is deleted with an `nm` link check in its place.
- **Platform process starts:** use one sanctioned spawn, which closes linux-ux-15's raw `execve`.
- **ControlKeys:** deleted after a `Session` interface replaces the control keys. This includes `bootCompleted`, which ControlKeys fakes today.
- **compat:** keeps only pure portability (log, clocks, RefBase, Thread, android-base file, bionic.h, the image decoder). The stubs, AID shims, EglWraps and path links go as their callers move.

## Migration order (proposed)

Legend: **AS** = edits Android-compiled code (an `android-surface:` commit, cherry-picked after the AOSP build); **LO** = Linux or tooling only.

| Step | What | Kind | Size |
|---|---|---|---|
| 0 | Ratchets scoped to Linux-compiled shared code, shrinking to zero: spawns, Android tool literals, non-Nano property keys, Android includes, Android paths, device literals, Android-named interface members; fix H.4 and the `__linux__` regex | LO | tests |
| 1 | Safety now: gate default flips to refuse, with a short temporary allow-list; fix the `timeout` skipper; mask `/sys/class/typec` from Nano with a read-only tmpfs (not a `/dev/null` bind, which would stop Nano starting); host scenarios assert platform events instead of Android commands | LO | small |
| 2 | Boot-mode decision behind the platform: Android keeps the `nano_skip`/`skip_nano`/bootanim logic verbatim, Linux always runs Nano (fixes defect 1) | AS | 1 file, small |
| 3 | `Cap` and `Platform::has()`, plus the settings-dispatcher seam (`Settings::read/write/supports`) | AS | NanoPlatform.h, SettingsTree, 3 platforms |
| 4 | `Http`, `FileOps`, `Archive` and "make readable by emulators" as interfaces: firmware keeps the shell verbatim, Linux uses libcurl, std::filesystem and libzip; then the gate goes deny-all | AS | about 9 files, about 40 sites |
| 5 | `Session` (sleep, wake, restart, first frame, boot completed, power actions, boot animation hand-off) replaces the control keys; ControlKeys deleted | AS | about 96 keys in about 9 files |
| 6 | Area interfaces, one per landing: Apps (about 43 sites), SystemSettings (about 35), display extras, audio volume, perf, network leftovers, USB/Type-C, notifications, OTA | AS | 1-5 files each |
| 7 | DeviceInfo from device profiles (nodes, key map, SoC quirks) | AS | about 30 sites |
| 8 | `Prefs` facade and `Paths` (state root, assets, fonts, temp, app data); then delete the path links | AS | large, mechanical |
| 9 | Android APIs into firmware: SurfaceFlinger, binder, HALs, sensors, AAudio and AHardwareBuffer (OA-11 allows the `NanoMenu.h` edits), plus a media decoder interface; stubs to 0, EglWraps deleted | AS | large |
| 10 | Delete ShellGate and the wraps; `nm` check | LO | small |

Cautions from the critique:

- **Verbatim moves only.** Re-routing a call site onto an *existing* interface whose firmware code differs from the call site's (the Wi-Fi tile, `reset_wifi_bt`, brightness, battery) changes Android behaviour. Each such change is an explicit Android change, never presented as a move.
- **I24-restricted files need build-chosen translation units, not `nanoplat`.** These are the files compiled into drastic-nano, the scraper or host tests: `NanoBiosUtil`, `NanoBiosWorker`, `NanoEsdeTheme`, `NanoDeviceKeys`, `NanoMenuDrm`, `NanoMenuDrmBuffers` and others. A hook table is not enough, because drastic-nano cannot fill it.
- **The Linux state root (where Nano's data lives on the card) should be decided before the alpha.** Users' alpha data then doesn't need migrating, and the decision interacts with OA-8 (card interoperability).
- **There is already an uncompiled Android backlog:** the nine G7 moves. More Android-surface commits stack on it until the AOSP job ends.

## Owner decisions this raises

1. How much of the migration lands before the Linux alpha.
2. Whether Android-surface work keeps stacking, uncompiled, before the existing G7 backlog is compiled and smoke-tested.
3. The settings-dispatcher seam and the `Prefs` facade (recommended).
4. The Linux state root, before the alpha.
5. Whether to offer the `nanoplat` seam to upstream GammaOS.
