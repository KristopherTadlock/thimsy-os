## Shell and process call inventory for Nano's shared code (thimsy-os-linux, branch linux)

**The short answer:** shared code has 138 places that name a command and send it to a shell or exec. On Linux the gate refuses 96 of them, turns 10 into routes or no-ops, translates 2, and passes 29 to a real Linux program. One string goes either way depending on settings. Of the 29 that pass, 24 carry Android-shaped arguments or Android paths. 2 more are Android `cmd` calls that reach Linux `/bin/sh` because the gate misreads them. 48 command sites have already moved behind nanoplat, but 7 shared sites still do the same features directly.

### Scope and how I counted
- **Shared files:** `build/sources.txt` sections [core] (106 files), [drm] (NanoMenuDrm.cpp) and the scrape worker.
- **Not counted:** top-level files in the [firmware] section (Android only, never linked on Linux). They hold 4 more sites, listed at the end.
- **Search:** grep for `system|popen|execv|execvp|execve|fork` (also `::`-qualified, `execl*`, `posix_spawn`, `vfork`) over `*.cpp`, `*.h`, `*.inc` and `bootscreen/`. Comments and words like "system (" in prose were removed by reading each hit.
  - No `posix_spawn`, `execl*` or `vfork` call exists. No .h or .inc file has a call.
- **102 raw call sites** in 21 files. A fork plus exec pair counts as 1.
  - 8 are generic helpers that take a command from their caller: `NanoControlCenter.cpp:1451` shellCmd, `NanoMenuOverlay.cpp:1176` overlayShellCapture, `NanoMenuPS3Menu.cpp:2470` nanoCaptureCmd, `NanoMenuSettings.cpp:100` runCmdUnlocked, `NanoMenuSetupWizard.cpp:386` fetch, `NanoMenuEsdeMenu.cpp:838/843` esdeRunProc, `NanoOtaCheck.cpp:185/198` runProcess, `NanoSdMove.cpp:265/283` runArgv.
- **Command sites = 94 + 44 = 138.**
  - 94 raw sites that build their own command.
  - 44 call sites of the helpers above. That includes the `RunFn` callbacks in NanoBiosUtil and NanoBiosWorker, which are runArgv.
- **No shell at all:** Syncthing (REST plus properties), the scraper (libcurl) and Shares ([firmware]).

### What the gate does (ShellGate.cpp)
- **Android tools table** (`ShellGate.cpp:65-79`):
  - `setprop` and `getprop` are routed to the property store.
  - `reboot` and `svc power reboot|shutdown` are routed to `sys.powerctl`.
  - `restorecon` and `chcon` are no-ops that report success.
  - `gammaos-net` is translated only through exec: `download URL FILE` becomes `curl -fsSL --retry 2 -o FILE URL` (`:587-603`). In a shell string it falls to the default case and is refused (`:412-414`).
  - `nsenter` through exec runs its tail (`:568-586`).
  - Everything else in the table is refused: exit 127 from system()/pclose(), or ENOENT from exec, after which the caller does `_exit(127)`.
- **Linux tools list** (`:82-90`): `/system/bin/<tool>` is rewritten to the bare name and run from PATH. This list includes curl, rm, mkdir, unzip, mv, chown, pidof, date and timeout.
- **Android roots** (`:93-95`) such as `/vendor/...` are refused unless the image has the file. `/vendor` is never created (G9-20). `/system/bin` links to a curated directory that is empty in the alpha.
- **Shell strings:** one refused command refuses the whole string. Routes are taken only when every command in the string is a route.

**Gate bug found:** the `timeout` skipper (`ShellGate.cpp:341-346`) skips dash flags and then one word as the duration. In `timeout -s KILL 10 cmd …` it takes `KILL` as the duration, so it sees the command word as `10` and passes the string. The Android `cmd` then reaches the real Linux `sh`, which fails with "cmd: not found" (127). The tests only cover `timeout 5 pm …` (`platform/linux/tests/shellgate_test.cpp:111`). Two sites hit this: `NanoMenuMonitorMode.cpp:84` and `NanoMenuPS3Menu.cpp:2469` (from `:2483`).

**Outcome codes:** REF = refused (127 or ENOENT); ROUTE = property store; NOOP = gate returns 0 without running anything; REWR = gammaos-net turned into curl; PASS/sb = `/system/bin/X` run as the Linux X; PASS = run as written; PASS! = Android command passed by the misparse.

### Inventory by feature area

**Apps and activities (am, monkey, pm, cmd package, dumpsys activity, appops, am broadcast)**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoControlCenter.cpp:1121 | `am force-stop OLD; monkey -p PKG …` | dual-stack launch | REF |
| NanoControlCenter.cpp:1178 | `am force-stop; ACT=$(cmd package resolve-activity…); am start --display` | leave dual-stack | REF |
| NanoControlCenter.cpp:1209 | same shape | bottom-panel app launch | REF |
| NanoControlCenter.cpp:1246 | `D=$(dumpsys activity activities)…grep` | bottom app exit poll | REF |
| NanoControlCenter.cpp:1274 | `am force-stop` | end bottom app | REF |
| NanoMenuOverlay.cpp:219 | `dumpsys activity processes PKG` | overlayGetPids (used at :255, :1267) | REF |
| NanoMenuOverlay.cpp:316 | `dumpsys activity activities \| grep ResumedActivity` | foreground app | REF |
| NanoMenuOverlay.cpp:412 | `am start … NanoPauseActivity` | overlay pause | REF |
| NanoMenuOverlay.cpp:1107, 1126, 1217, 1291, 1310 | `am force-stop PKG` | overlay quit, kill all, switch | REF ×5 |
| NanoMenuOverlay.cpp:1163 | `dumpsys activity services` | foreground-service check (:1099, :1118, :1303) | REF |
| NanoMenuOverlay.cpp:1200 (helper) | `pm list packages -3` | kill all apps | REF |
| NanoMenuOverlay.cpp:1977 (helper) | `pm path com.retroarch.aarch64` | RetroArch launch | REF |
| NanoMenuOverlay.cpp:1400 | finalCmd = `am start … --display` | overlay app launch | REF |
| NanoMenuOverlay.cpp:2579 | `am force-stop; ACT=$(cmd package…); am start --display` | panel app | REF |
| NanoMenuOverlay.cpp:2679, 2758, 2790 | `am force-stop` | panel app move or close | REF ×3 |
| NanoMenuOverlay.cpp:2827 | `D=$(dumpsys activity activities)…` | panel app poll | REF |
| NanoMenuPS3Menu.cpp:2483 (via nanoCaptureCmd :2469) | `timeout -s KILL 3 cmd package resolve-activity` | resolve the Settings app | PASS! |
| NanoMenuPS3Menu.cpp:2554, 2585 | `am start -a android.settings.SETTINGS` | open Android Settings | REF ×2 |
| NanoMenuPS3Menu.cpp:7254, 7257 | `am start … lineageparts` | gamepad calibrate or remap | REF ×2 |
| NanoMenuPS3Menu.cpp:12839 | `am start-foreground-service` or `am stopservice` screenmapper | Screen Map | REF |
| NanoMenuPS3Folder.cpp:881 | `am force-stop com.retroarch.aarch64` | file job | REF |
| NanoMenuPS3Folder.cpp:1199 | `appops set … MANAGE_EXTERNAL_STORAGE` | RetroArch folder | REF |
| NanoMenuXmb.cpp:3714 | `am broadcast … androidgames.SYNC` | Android Games | REF |
| NanoMenuState.cpp:393 | `pm enable documentsui… &` | Files launcher | REF |

**Android settings and system UI (settings, ime, locksettings, cmd uimode, notification, location)**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuSettingsTree.cpp:845 | `settings get NS KEY` | generic setting read (every kGlobal, kSecure, kSystem row) | REF (returns the default) |
| NanoMenuSettingsTree.cpp:877 | `settings put NS KEY VAL` | generic setting write | REF |
| NanoMenuSetupWizard.cpp:202, 203, 204, 208 | `settings put` provisioning and IME keys | setup finish | REF ×4 |
| NanoMenuSetupWizard.cpp:209 | `ime reset` | setup finish | REF |
| NanoMenuSetupWizard.cpp:212 | `locksettings clear` | setup finish | REF |
| NanoMenu.cpp:4729 | `settings put global device_provisioned 1 &` | wizard | REF |
| NanoMenuOverlay.cpp:1513 | `ime enable X; ime set X` | select IME | REF |
| NanoMenuOverlay.cpp:1987 (helper) | `settings get secure default_input_method` | RetroArch launch | REF |
| NanoMenuRender.cpp:6259 | `settings get system font_scale` | font seed | REF |
| NanoMenuPS3Menu.cpp:12772 | `cmd uimode night yes/no` | Dark Theme | REF |
| NanoMenuPS3Menu.cpp:12876 | `cmd notification set_dnd` or `cmd location set-location-enabled` | quick toggles | REF |
| NanoMenuPS3Menu.cpp:4979 | `dumpsys notification --noredact` | notifications list | REF |
| NanoMenuPS3Menu.cpp:7268 | `cmd notification snooze` | dismiss notification | REF |

**Display**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuMonitorMode.cpp:84/85 (caller :167) | `timeout -s KILL 10 cmd display gammaos-monitor-mode ARGS` | monitor resolution | PASS! |
| NanoMenuPS3Menu.cpp:12392 | `cmd display enable-display 2` or `disable-display 2` | External Display | REF |
| NanoMenuSettingsTree.cpp:903 | `cmd color_display set-saturation` | saturation | REF |
| NanoMenu.cpp:5587 | same | saturation on boot | REF |
| NanoMenuRender.cpp:439 | `screencap -p /data/local/tmp/…` | overlay background | REF |
| NanoControlCenter.cpp:1693 (shellCmd) | `mkdir -p /sdcard/…; screencap …` | screenshot | REF (whole string) |
| NanoMenuOverlay.cpp:2907, 2909, 2928, 2930, 3007 | `settings put/get system accelerometer_rotation` or `user_rotation` | orientation | REF ×5 |

**Audio**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoControlCenter.cpp:282 | `cmd media_session volume --get` | Control Center volume read | REF |
| NanoControlCenter.cpp:1730 (shellCmd) | `cmd media_session volume --set` | Control Center volume set | REF |
| NanoControlCenter.cpp:307 | `settings get global gammaos_audio_display_volume_map` | per-display volume read | REF |
| NanoControlCenter.cpp:1744 (shellCmd) | `settings put global gammaos_audio_display_volume_map` | per-display volume write | REF |

**Power, performance and brightness**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenu.cpp:1242 | `/vendor/bin/setclock_%s.sh &` | CPU clock profile | REF |
| NanoMenu.cpp:1746, 2042 | `/vendor/bin/setclock_max.sh` | DS quick-resume (DrasticRunner is stubbed on Linux) | REF ×2 |
| NanoControlCenter.cpp:1617 (shellCmd), split mode | `setprop …split_brightness.d1.override; setprop …last` | Control Center brightness | ROUTE; nothing on Linux reads these keys |
| NanoControlCenter.cpp:1617 (shellCmd), unified mode | `settings put system screen_brightness` | Control Center brightness | REF |

**Wi-Fi**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuSettings.cpp:621 (runCmd) | `timeout 30 gammaos-net wifi configure SSID SEC … --ip --dns…` | wizard static IP, DNS, proxy, MTU | REF |
| NanoMenuSettingsTree.cpp:1107, 1110 | `cmd wifi set-wifi-enabled` | reset Wi-Fi and BT | REF ×2 |
| NanoControlCenter.cpp:334 | `settings get global wifi_on` | Control Center tile | REF |
| NanoControlCenter.cpp:1697 (shellCmd) | `svc wifi enable/disable` | Control Center tile | REF |

**Bluetooth**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuSettingsTree.cpp:1108, 1111 | `cmd bluetooth_manager enable/disable` | reset | REF ×2 |
| NanoMenuPS3Menu.cpp:16252, 16262 | `gammaos-net bt discoverable 120` or `0` | inbound pairing | REF ×2 (shell form) |
| NanoMenuPS3Menu.cpp:16562 | `gammaos-net bt confirm ADDR reject` | inbound reject | REF |

**Time, date and locale**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuPS3Menu.cpp:12189 | `settings put system time_12_24` | 12/24 h | REF |
| NanoMenuPS3Menu.cpp:12221, 16348, 16368, 18403 | `settings put global auto_time_zone` or `auto_time` | auto time | REF ×4 |
| NanoMenuPS3Menu.cpp:12223, 16349 | `cmd alarm set-timezone` | zone | REF ×2 |
| NanoMenuPS3Menu.cpp:16372, 18419 | `cmd alarm set-time MS` | set clock | REF ×2 |
| NanoMenuPS3Menu.cpp:18404 | `curl -sI http://example.com \| grep date:` | clock from internet | PASS; the result goes to the refused set-time at :18419, so the fetch is wasted |
| NanoMenuPS3Menu.cpp:18456 | `setprop persist.sys.timezone Z` | XMB time zone globe | ROUTE; only the property is written, see the bypass list |
| NanoMenuStrings.cpp:664 | `setprop persist.sys.locale X` | system locale | ROUTE; nothing on Linux reads it |
| NanoMenuSetupWizard.cpp:318 | `TZ=ID date -d @N +%z` | zone offset | PASS (busybox date) |
| NanoMenuSetupWizard.cpp:404, 405 (fetch) | `curl -s https://ipapi.co/timezone/`, `curl … ip-api.com` | auto zone | PASS ×2 |

**Streaming (IPTV, radio, HLS)**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoHls.cpp:147/148 | `/system/bin/curl … -A 'gammaos-nano-iptv' URL` (curlText; called at :307, :317, :354, :424) | HLS playlists | PASS/sb |
| NanoHls.cpp:249/251 | same, plus ICY options (called at :272, :278, :289) | HLS segments | PASS/sb |
| NanoIcyDemux.cpp:44/46 | `/system/bin/curl -r 0-8191 …` | radio codec probe | PASS/sb |
| NanoMenuIptv.cpp:107/109 | `/system/bin/curl -o /data/system/nano_iptv_ftv.m3u8` | IPTV index | PASS/sb |
| NanoMenuRadio.cpp:108/110 | `/system/bin/curl -o /data/system/nano_radio.m3u` | radio index | PASS/sb |

**ES-DE theme downloader** (esdeRunProc at `NanoMenuEsdeMenu.cpp:831-855`)
- The nsenter prefix is only added when `netns` is true, and no caller passes it. Child output goes to `/data/local/tmp/esde_dl.log`.
- Each call builds an argv that starts with an Android `/system/bin/` path:

| Site | Command | Linux |
|---|---|---|
| :902, :960 | `/system/bin/mkdir -p /data/system/nano_esde_themes…` | PASS/sb ×2 |
| :906, :965 | `/system/bin/gammaos-net download URL FILE` | REWR ×2 (curl) |
| :959, :970, :973, :978, :980, :982, :985 | `/system/bin/rm -rf …` | PASS/sb ×7 |
| :972 | `/system/bin/unzip` | PASS/sb |
| :981 | `/system/bin/mv` | PASS/sb |
| :990 | `/system/bin/chown -R system:system` | PASS/sb with an Android user name; `linux/board/common/users.table` defines no users, so chown fails and the result is ignored |
| :991 | `/system/bin/restorecon -R` | NOOP |

**OTA** (`NanoOtaCheck.cpp`)
- The target is the Android OTA service `https://ota.gammaos.sh/api/v1/{device}/{variant}` (`:79`), working under `/data/gammaos_ota`.

| Site | Command | Linux |
|---|---|---|
| :245 (runProcess) | `/system/bin/rm -rf` | PASS/sb |
| :250 (runProcess) | `/system/bin/mkdir -p` | PASS/sb |
| :334 (runProcess) | `/system/bin/curl -sL … manifest` | PASS/sb, to the Android OTA server |
| :544/557 | fork plus execvp `/system/bin/curl -fL -o zip` | PASS/sb, Android OTA package |
| :626 (runProcess) | `/system/bin/unzip` of that package | PASS/sb |

- The install hand-off is refused separately, as a property: `ControlKeys.cpp:59` (`sys.gammaos.ota.prestage`).

**Storage, SD move, app files and BIOS** (runArgv at `NanoSdMove.cpp:255`; `RunFn` callbacks go through it via `NanoBiosInstall.cpp:242` and `NanoSdMove.cpp:332, 1657, 1904`)
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoSdMove.cpp:250 (sdStopIfRunning), :1770 | `/system/bin/am force-stop` | SD move, emu sync | REF ×2 (ENOENT) |
| NanoSdMove.cpp:1355, 1787, 1971 | `/system/bin/restorecon` | SD move | NOOP ×3 |
| NanoSdMove.cpp:458 | `am force-stop org.es_de.frontend` | SD move | REF |
| NanoSdMove.cpp:2268, 2292, 2303 | `appops set …` | SD move | REF ×3 |
| NanoMenuPS3Folder.cpp:888 | `restorecon PATH` | file job | NOOP |
| NanoBiosUtil.cpp:448, 486 | `/system/bin/restorecon` | appFileWrite | NOOP ×2 |
| NanoBiosWorker.cpp:124 | `/system/bin/logcat -d -T` | Vita3K install wait | REF |
| NanoBiosWorker.cpp:179, 868, 890 | `/system/bin/am force-stop` / `am start -W` / `am start-foreground-service` | BIOS, Vita3K | REF ×3 |
| NanoBiosWorker.cpp:717 | `restorecon -R` | BIOS | NOOP |
| NanoBiosWorker.cpp:863 | `/system/bin/pidof org.vita3k.emulator` | Vita3K install | PASS/sb with an Android package name |

**MTP and USB**
| Site | Command | Feature | Linux |
|---|---|---|---|
| NanoMenuMtp.cpp:182, 184, 198 | `gammaos-net usb none`, `mtp`, `MODE` | USB mode | REF ×3 |

- The USB-C guard is started by property, not shell: `NanoMenuMtp.cpp:351` sets `ctl.restart nano-typec-guard`, which ControlKeys refuses (`ControlKeys.cpp:64`).

### Totals (138 command sites)
| Outcome | Count |
|---|---|
| Refused (REF) | 96 |
| Routed to the property store (ROUTE) | 2, plus the either-way string at ControlCenter:1617 |
| No-op success (NOOP, restorecon) | 8 |
| Translated (REWR, gammaos-net to curl) | 2 |
| Passed to a real Linux program | 29 |

- **Android-only tools:** 106 of 138 name one (am, cmd, settings, svc, pm, dumpsys, appops, ime, locksettings, monkey, screencap, logcat, setprop, restorecon, gammaos-net, or a `/vendor` script). That is the 96 refused, the 2 routed, the 8 no-ops, plus the either-way site, the 2 translated and the 2 misparsed ones.
- **Passes with Android-shaped arguments or paths (your curl concern): 24 of the 29.**
  - 22 use a hard-coded `/system/bin/<tool>` path that the gate rewrites: curl ×5 in streaming, OTA ×5, ES-DE ×12. The other 2 carry Android arguments: pidof with a package name, and the 2 curl ×2 via gammaos-net in ES-DE are counted under REWR, not here.
  - Their arguments are Android-specific in these cases: `chown system:system` (ES-DE :990), `pidof <android package>` (BiosWorker :863), OTA curl and unzip against the Android OTA server and package (OTA :334, :557, :626), and `/data/...` paths that only work because of the I14 links.
  - The 2 PASS! sites are Android `cmd` handed to the Linux `sh` because of the timeout misparse.
- **Plain Linux passes:** 3 (`curl|grep` at PS3Menu:18404, the 2 ipapi fetches) plus `TZ= date` at SetupWizard:318.

### Already moved behind nanoplat, and what still bypasses it
- **48 command sites moved** into platform/firmware:
  - `Network.cpp`: 34 runCmd or runCmdUnlocked calls, plus 5 runCmdShellout calls and its own popen helper at :60.
  - `Power.cpp`: :242 and :251.
  - `SystemActions.cpp`: :47, :65, :70, :90, :92, :97, :104.
  - Audio, Display, Launcher, Volumes and InputPolicy in platform/firmware have no shell calls.
- **The moved code still calls shared code:** firmware `Network.cpp:46-47` and `SystemActions.cpp:39` call `runCmd` and `runCmdUnlocked`, which are defined in shared `NanoMenuSettings.cpp:72-118`. Those definitions add a toybox-style `timeout` and are commented for `cmd wifi` and `gammaos-net`. So that Android shell helper still lives in shared code.
- **Moved features that shared code still does directly, past the platform layer (7 sites):**
  - `NanoMenuPS3Menu.cpp:12189` does time format itself (`SystemActions::setTimeFormat`; Linux makes it a no-op on purpose).
  - `NanoMenuPS3Menu.cpp:18456` sets the time zone by setprop. On Linux the gate only writes the property, so `LinuxSystemActions::setTimeZone` (`platform/linux/actions/SystemActions.cpp:185-204`), which makes the `localtime` link, never runs. The XMB globe therefore does not change the clock's zone.
  - `NanoMenuSettingsTree.cpp:1107-1111` turns Wi-Fi and Bluetooth off and on itself (`setWifiEnabled`, `setBluetoothEnabled`).
  - `NanoControlCenter.cpp:1697` switches Wi-Fi itself.
  - `NanoControlCenter.cpp:1615` writes brightness itself (`Power::mirrorBrightness`; Linux makes it a no-op).
  - `NanoMenuPS3Menu.cpp:16562` rejects an inbound pairing itself; `Network::btConfirmInbound` exists for this.
- **Features with no platform method yet:** BT discoverable (`:16252`, `:16262`), the Wi-Fi configure screen (`NanoMenuSettings.cpp:621`), monitor mode, display enable, saturation, uimode, DND, location, notifications, volume, CPU clocks, app management in the overlay and Control Center, OTA, ES-DE download, SD move tools, BIOS install, and MTP.

### Not linked on Linux (for completeness)
- 4 sites in [firmware] top-level files: `NanoBtStable.cpp:125` (`dumpsys bluetooth_manager`), `:151` (`dumpsys audio`), `:210` (`/vendor/bin/setclock_%s.sh`), and `DrasticRunner.cpp:5367` (`dumpsys media.audio_flinger`).
- `NanoMenuShares.cpp` has no shell calls.

### Shell scripts Nano ships
| Script | Lines |
|---|---|
| `joypad_uevent_fixup.sh` | 47 |
| `nano-ce-unlock.sh` | 53 |
| `nano-pmdown.sh` | 29 |
| `nano-typec-guard.sh` | 23 |
| `thimsy-dexopt.sh` | 29 |

- All five are installed only by `Android.bp` and run as init services from `gammaos-nano.rc`. CMake and `cmake/*.cmake` reference no `.sh` file.
- No shared code runs any of them. The only reference is the `ctl.restart nano-typec-guard` property at `NanoMenuMtp.cpp:351`, which Linux refuses.
- The `/vendor/bin/setclock_*.sh` scripts are not in `nano/`; they come from the device tree. Shared code calls them at `NanoMenu.cpp:1242`, `:1746` and `:2042`.

### Against your principle
- **Platform specifics in shared code:** 106 of the 138 shared sites name an Android-only tool. The rest use Android paths or arguments, or are Linux-valid curl or date calls. On Linux they are mostly filtered at link time by name rather than handled in the platform layer.
- **Commands that do run "identically" on Linux:** 22 run because `/system/bin/X` is rewritten to the Linux X. Of those, about 5 carry Android arguments or targets: OTA server and package ×3, `chown system:system`, `pidof <package>`.
- **Same tool, different treatment:** `gammaos-net` is translated when it comes through exec but refused when it comes through a shell string.
- **Silent false success:** 8 no-ops report success, and 2 property routes report success with nothing on Linux to act on them (the split-brightness keys, `persist.sys.locale`). The time-zone route reports success but only half-works.
- **Missing ratchet:** `tools/nano-tree/tests/test_shell_sites.py` ratchets only the 3 fork-and-exec files (EXEC_SITES at :33-40), not the system() and popen() sites.
