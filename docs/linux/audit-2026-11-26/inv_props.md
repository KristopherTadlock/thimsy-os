# Android system properties in shared Nano code: inventory, Linux handling, hidden platform calls

## Scope and how I counted
- **Shared code** means the [core] and [drm] sections of `nano/build/sources.txt` (106 .cpp files, one of them `bootscreen/NanoBootArt.cpp`) plus all 98 top-level `nano/*.h` and `*.inc`. That is 204 files. [firmware] and [dropped-linux] files (main.cpp, Drastic*, LibretroRunner, NanoBtStable, etc.) are left out.
- **API calls.** I lexed the files with comments and strings blanked (the same lexer as `tools/nano-tree/nanotree/ratchets.py`) and counted call tokens. Result: **1,009 property-API calls in 55 shared files**:
  - property_set 311, property_get 307, property_get_bool 157, property_get_int32 81
  - android::base::GetProperty 42, GetBoolProperty 33, SetProperty 28, GetIntProperty 5
  - `__system_property_serial` 25, `__system_property_find` 18, `__system_property_wait` 2
  - WaitForProperty 0
  - Top files by property-API matches (raw grep, so these include comment mentions): NanoMenu.cpp 241, NanoMenuPS3Menu.cpp 211, NanoMenuOverlay.cpp 143, NanoMenuInput.cpp 85, NanoControlCenter.cpp 63, NanoMenuXmb.cpp 56, NanoSdMove.cpp 52.
- **Keys.** I took every string literal matching ratchets.py's key regex (namespaces persist/sys/ro/service/init/ctl/debug/vendor/dalvik), plus keys inside `"setprop K V"` shell strings. Result: **716 distinct keys (or key templates/prefixes such as `persist.gammaos.gamepad.pa%d_pkg`), 1,781 literal occurrences**.
  - Each occurrence is tagged set, get or other. "Other" is mostly settings-table rows; these reach `writeSettingValue`'s property_set at NanoMenuSettingsTree.cpp:863-868.
  - For comparison, `tools/nano-tree/props.txt` holds 994 keys, but it covers all Android-compiled nano files, not just the shared ones.
- **Consumers.** I worked out who reacts to each key from:
  - the non-Nano sections of `patches/thimsy/0001-thimsy.patch`
  - `gammaos-nano.rc`'s `on property:` triggers
  - `tools/nano-tree/control-keys.json`
  - comments at the call sites
  
  Consumers in GammaOS base code that the patch does not show are inferred from those comments and marked as such.

## Totals by class (distinct keys / literal occurrences)

| Class | Keys | Occurrences | set / get / other |
|---|---|---|---|
| 1 settings storage (persist.*) | 516 | 1,206 | 200 set (3 via shell setprop), 436 get, 567 other (table rows) |
| — 1a Nano-private prefs | 232 | 580 | 149 / 266 / 165 |
| — 1b persisted, but the effect comes from another Android process | 284 (upper estimate; 37 exact keys proven by patch evidence) | 626 | 51 + 3 shell / 170 / 402 |
| 2 IPC/control (Nano writes, another process acts) | 96 | 346 | 221 set + 1 shell / 102 / 22 |
| 3 reads of Android or system state | 69 | 137 | 3 set (Nano clearing request keys) / 96 / 38 |
| 4 (extra) Nano's own scratch state and debug knobs in sys.* | 35 | 92 | 48 / 42 / 2 |

## Class 1: settings storage (persist.*)

### 1a: Nano-private (232 keys)
- Families: persist.gammaos.nano.* 207, persist.gammaos.scraper.* 22, retroarch 2, persist.thimsy.nano.fpslog 1.
- Samples:
  - NanoMenuXmb.cpp:351/356/1862: `persist.gammaos.nano.xmb.%s.dir|core|path`
  - NanoMenuUtils.h:155/172/183: qr_rom, qr_prepared, qr_chain
  - NanoScreenCal.h:44: screencal prefix
  - NanoMenuScraper.cpp:1363: scraper.media.*
- This is a real portable key-value use.

### 1b: persisted, but consumed by an Android process (~284 keys)
**Patch-evidenced consumers (37 exact quoted keys):**

| Keys | Consumer |
|---|---|
| bfi.enable, rgb.sample.pre_fx, display.tweaks, refresh.* | SurfaceFlinger |
| shader.enable, shader.type | renderengine/skia filters, PhoneWindowManager, ShaderControl |
| rotate.enabled, ime.localdisplay, retroarch.dir, nano.orientation | WindowManager |
| gamepad.abxy_swap, device_vid/pid, remap_btn, browser_pkgs | PhoneWindowManager / gammapad |
| hotkeys.* | PhoneWindowManager, drastic-nano |
| dualstack.enabled, display.mode, nano.primary_display | display service, SurfaceFlinger, inputflinger, SystemUI |
| nano.ctrl_screen | inputflinger |
| nano.mode_quick_settings | PhoneWindowManager |
| persist.sys.locale | framework |

**Settings-table rows whose only effect is an Android component** (comments say so, consumer not in patch). From NanoMenuPS3Menu.cpp:9910-9950 ("GammaOS Toolbox (persist.gammaos.* props)"):
- fan_mode, immersive, refresh.lock/rate ("Scheduler.cpp reads refresh.lock")
- drm.force_l3, display.tweaks, force_client_comp, desktop.fullscreen, audio.multivolume
- ultra_low_power_saving_mode
- swap.size_mb ("gammaos-swap.sh (init service) reads")
- retroarchoverride.backbutton, startselectled ("The RG477V tile reads"), usbcontrollerswitch, dcdimmingemulation, taskbar.phone

From NanoMenuSettingsTree.cpp:
- :298-311: force_client_comp, desktop.fullscreen, sf.keep_underlay_on_shade, renderengine.backend, vsync_period_ns
- :517-519: unisoc/allwinner hdmi.enable
- :644-657: bg_process_limit, gesture_wake_ignore, performance_mode, fan_mode

**Other Android-consumed persist families:**
- persist.sys.* (56 keys): spk.*/gammaeq.* written by Android Settings and read by GammaEqDsp.h:24-483; locale; timezone; device_provisioned
- persist.vendor.* (NanoScreenCal.cpp:523), persist.gsf.rot.%d and sec_rot (NanoMenuDrm.cpp:231/233), persist.gammargb.control, persist.bootanim.skip_nano

**writeSettingValue mixes in Android side effects** (NanoMenuSettingsTree.cpp:863-935):
- bumps `persist.gammaos.gamepad.config_version` for the gammapad daemon on every gamepad.* write (:918-930)
- `persist.gammaos.nano.orientation` → `settings put system accelerometer_rotation` (:910-913)
- saturation → `cmd color_display` (:895-902)
- kGlobal/kSecure/kSystem rows (31 in SettingsTree, 23 in PS3Menu) shell out to `settings get/put` (:838-858, :870-878)

### What Linux does with class 1
- Every key goes through `nano/compat/src/properties.cpp` (property_set :416 → setProperty :226 → `thimsy::ConfigStore::trySet`). It is stored in `/storage/.config/thimsy/persist.props` (IMPLEMENTATION B.3).
- control-keys.json's single `persist.*` row is store-only. No observer exists for any persist key, and no Linux daemon reads one: the only persist key in linux/src is `persist.thimsy.audio.volume`.
- So the ~284 class-1b writes succeed, return 0, read back correctly and change nothing. The settings rows look as if they work. Hiding them is deferred to G7-caps, after the alpha (IMPLEMENTATION.md:445).

## Class 2: IPC/control (96 keys)

### Linux disposition
I counted these against kControlRows (ControlKeys.cpp:50-74: 23 rows over 19 distinct keys), control-keys.json (66 rows: 12 routed, 15 refused, 18 store-only, 21 not-reached) and the keys thimsy-session reads (Session.cpp:63-69).

| Linux handling | Keys | Which |
|---|---|---|
| Routed to an action | 7 | dosleep → session:sleep; dowake → reset only; fixedperf → family hook (control-keys.json notes the hooks lack the action, so it does nothing today); do_launch and service.bootanim.nano_retroarch → launcher; drm_active → first-frame; service.bootanim.nano_action=reboot/shutdown → session. park_restart is in the table but not written by shared code |
| Refused (log once) | 10 | ctl.start/stop/restart, ota.prestage, drastic_nano.start, bt.pair_request/pair_reply, qs_summon, show_overlay, nano.restart; plus other nano_action values |
| Read by thimsy-session | 3 | scrape_run, app_launched, app_drawn |
| Read back as handshake keys when the trigger fires | 9 | ControlKeys.cpp kLaunchKeys |
| **Silently stored, nothing happens, no log** | **67** | 9 listed in control-keys.json as store-only (service.bootanim.exit, dualstack.active, drop_input, focus.display, focus.notice, killing, pending_exit, touch_display, shutdown_started); **58 not in any table** |

The 58 untabled keys: bt.inbound, inbound_open, passkey; drastic_nano.quit, qr_resume; dualstack.enabled; gamepad.mouse_active; multidisplay.split_brightness.d1.override; nano.app_action, app_uninstall, appinfo_req, apps_refresh_req, cache_op, cache_ready, cc.bottomapp, clear_forced_size, clip_req, drop_fence_ns, factory_reset, force_drm, force_orientation, handoff_fired, kill_pkg, launch_time, media*, media_playing, menu_active, net_cmd, osk_done, osk_gen, ota_busy, overlay_esc, overlay_ran, overlay_wallpaper, panel_app, pspclock_summon, qr_send_esc, restart_after_cancel, scrape_cancel, scrape_reload_ack, screenoff, shutting_down, typec, typec_port, usb_applied; ota.autoinstall, package, staged, stageprog, theme; retroarch.healed; rotate.state; syncthing.restart.

### "Refused" is not an error to the caller
- ControlKeys observes after the store has already written: `announce` (properties.cpp:106) runs after `trySet`, and observers cannot veto, by design (B.2: "the store writes first, and handlers observe").
- `refuse()` (ControlKeys.cpp:231) only logs. property_set still returns 0 and the value stays in the store.
- So shared code gets neither a no-op signal nor an error. It proceeds as if Android had acted.

## Class 3: Android system-state reads (69 keys)

| Key(s) | Where read | Linux source |
|---|---|---|
| sys.boot_completed (27 reads) | NanoBootScreen.cpp:56/96, NanoMenu.cpp:1895/2878… | Faked by ControlKeys `firstFrame()` (:218-229) when Nano writes drm_active=1 |
| ro.product.model / vendor.model, ro.board.platform | NanoMenu.cpp:432, NanoMenuDrm.cpp:1155, NanoVideo.cpp:57, NanoMenuPS3Menu.cpp:10457 | Synthesised by `thimsy-prop boot` from device.json (libthimsy/tools/thimsy-prop.cpp:224-238; I24 relies on this) |
| ro.thimsy.version / release / commit | NanoMenuPS3Menu.cpp:1940/1942/1981 | Linux writes `ro.thimsy.build.version` / `.commit` instead (linux/board/common/post-build.d/20-build-props.sh:74-75), so the names do not match. **Likely the Quick Menu's release/commit rows are empty on Linux** |
| ro.build.*, ro.lineage.*, ro.gammaos.*, ro.serialno, ro.product.locale, ro.surface_flinger.*, ro.input_flinger.* | various | Nothing writes them unless device.json passes them through |
| init.svc.zygote / gammapad / gammaos-ota / gammaos-nano-scrape, sys.system_server.start_count, sys.user.0.ce_available, sys.screen.state | NanoBootScreen.cpp:92-96, NanoMenu.cpp:3647/5074 | Never written; behaviour falls to defaults by accident |
| Framework-published sys.gammaos.* (bmap_dev, screen_swap, panel_touch, uri_grant, retroarch.cfg_dir, display.monitor_mode, bindings.enabled, *_generation, appinfo_gen, clip_ready, ext_storage_ready, home_launching, ime_visible) | various | Never written |

**Probable boot defect (inferred from code, not run; G10 has not run).** In readyToRun, NanoMenu.cpp:493-560, the run/exit decision depends on three things nothing on Linux writes:
- `/data/misc/bootanim/nano_skip`: path-roots.json:28 says "misc/bootanim … not made"
- `ro.persistent_properties.ready`: a 10 s wait loop at :527-534
- `persist.bootanim.skip_nano`: :537

A repo-wide grep finds no writer for any of them outside nano/ and patches/. The code would wait 10 s, then set fileBootanim, then `property_set("ctl.start","bootanim")` at :558 (refused), then `_exit(0)`.

## Class 4 (extra): Nano-internal state (35 keys)
- xmb_return_sys/game, xmb_color_phase, nds_return, esde_return, return_apps, cc.active, setup_active, setup_step_jump, nav (test hook), shot/shot2, globe.*, dualscreen_detected, drastic_launch_rom, direct_test, drm_setcrtc, drm_zc
- 16 `sys.gammaos.drastic_nano.*` tuning knobs read in NanoMenuDrm.cpp:266-2929
- These are written and read only by Nano. They are portable but use a global property bus as process memory.

## How the Linux layers participate
- **compat/src/properties.cpp** is a full emulation of Android's property API: serials, `__system_property_wait`, WaitForProperty, and Android's 91-byte value limit, kept on purpose.
  - Because of that limit, the Linux launcher keeps the "full value it wrote" beside the truncated stored value (DECISIONS G7-launch-b-11; Launcher.cpp:148-170 `note()`).
- **ControlKeys.cpp** turns Android IPC keys into Linux actions after the fact. Its default session request is itself a property, `sys.thimsy.session.request` (:113-120). The Linux platform adopted the Android property bus as its own IPC.
- **ShellGate.cpp** maps `setprop`/`getprop` to RouteSetprop/RouteGetprop (:68-69, routeSet :157) and writes them into the same store. Shared shell setprop sites:
  - NanoMenuStrings.cpp:663 (persist.sys.locale)
  - NanoMenuPS3Menu.cpp:18454 (persist.sys.timezone)
  - NanoControlCenter.cpp:1601-1602 (split_brightness d1 override and last)
- **The Linux platform writes Android-namespace keys too**, in 4 files: ControlKeys, Launcher (handshake keys, app_launched at :452), SystemActions (persist.sys.timezone :57), Power (persist.gammaos.nano.brightness :46).

## Places where shared behaviour depends on an Android daemon reacting (hidden platform calls)
1. **Launch.** NanoLaunchCommit.cpp:34-47 (69 commitLaunch calls) goes through `Launcher::commit`. That seam exists, but the Linux implementation writes the keys and then catches its own do_launch write through ControlKeys (`fireLaunch` :201) to re-read them. That is a property round-trip inside one process (I28, G7-launch-b-14).
2. **Boot animation and init.**
   - ctl.start bootanim (NanoMenu.cpp:558), ctl.stop bootanim (NanoMenuRender.cpp:5832), ctl.restart nano-typec-guard (NanoMenuMtp.cpp:351): refused.
   - service.bootanim.exit (NanoMenu.cpp:679, 1460): stored only.
3. **Reboot to android, recovery or safemode.** service.bootanim.nano_action (NanoMenuInput.cpp:531-539) is refused, and the menu item silently does nothing.
4. **Sleep.** dosleep (NanoMenuInput.cpp:1049, 1136, 1139, 1281) is routed.
5. **OTA.** NanoMenuOta.cpp:746-758 writes theme, package and autoinstall (stored), then prestage (refused). The progress poll of ota.staged and stageprog (:652/:664) never advances.
6. **Framework bridge RPC.** All silently stored; any reply never arrives.
   - kill_pkg (NanoMenu.cpp:754, 861)
   - app_action (NanoMenuPS3Menu.cpp:7132)
   - app_uninstall (:12352)
   - appinfo_req (:14362, 18528) → appinfo_gen (:3911)
   - apps_refresh_req (:14403) → apps_generation (NanoControlCenter.cpp:912)
   - clip_req (NanoOsk.cpp:678) → clip_ready (:882)
   - net_cmd (NanoMenuSettings.cpp:141; its liveness test reads the bridge's own keys at :119-123, so on Linux shared code picks its Bluetooth/Wi-Fi path from whether an Android daemon is present, e.g. :1381-1391 and NanoSdMove.cpp:1038)
   - **factory_reset (NanoMenuSettingsTree.cpp:1113): Factory Reset silently does nothing on Linux**
7. **Overlay, Quick Menu and input dispatcher.**
   - qs_summon and show_overlay (NanoMenu.cpp:4524-4525), nano.restart (NanoMenuOverlay.cpp:1141): refused.
   - overlay_esc (NanoMenuOverlay.cpp:1274; NanoMenuSystem.cpp:730), focus.* (NanoControlCenter.cpp:1385, NanoMenuOverlay.cpp:2605/2619), touch_display (:2635), force_orientation (:2989), drop_input and drop_fence_ns: silent.
8. **drastic-nano.**
   - start (NanoMenu.cpp:2035/2980/4694, NanoMenuOverlay.cpp:1941) is refused, followed by `_exit(0)`.
   - quit (NanoMenuSystem.cpp:805) and qr_resume are silent.
9. **Bluetooth via gammaos-net.** pair_request and pair_reply (NanoMenuSettings.cpp:1593-1594) are refused; inbound, inbound_open and passkey (NanoMenuPS3Menu.cpp:16240-16963) are silent.
10. **Display, SurfaceFlinger and gammapad.** All stored only:
    - rotate.state (NanoMenuInput.cpp:3047)
    - dualstack.enabled (NanoControlCenter.cpp:1103)
    - split_brightness via shell setprop (:1601)
    - gamepad.mouse_active (:1673)
    - config_version bump (NanoMenuSettingsTree.cpp:918-930)
    - every class-1b settings row
11. **Time zone.** The XMB time-zone globe runs `system("setprop persist.sys.timezone …")` at NanoMenuPS3Menu.cpp:18454. ShellGate stores it and /etc/localtime is not changed, while the setup wizard uses the platform seam `SystemActions::setTimeZone` (NanoMenuSetupWizard.cpp:470; Linux implementation at actions/SystemActions.cpp:185-205). The same feature is wired two ways, and the shell one is a silent no-op on Linux. Locale at NanoMenuStrings.cpp:663 is the same pattern.
12. **SoC branching on an Android key.** `nanoForceSfPath` (NanoMenu.cpp:429-437) branches on ro.board.platform SoC strings in shared code.

## Bottom line against the stated principle
- Properties are the de facto platform API in shared code: 1,009 calls, 96 IPC keys, about 284 settings consumed by other Android processes.
- NanoPlatform.h has no settings or store interface and no seam for any of the class-2 groups above except launch, power and some network and system actions.
- Linux makes every Android property write succeed against a real store. It then translates 7 keys, logs 10 and silently swallows 67.
- So Android commands are still issued on Linux, and most unsupported features become invisible no-ops rather than explicit no-ops or reported errors.

Scratch inventory: per-key occurrences with file:line and set/get tags are in C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\occ.json, and the class of each key is in C:\Users\krist\AppData\Local\Temp\claude\a05db0c91d486b22\216a381f-418c-48c8-9542-04375b17814b\scratchpad\cls.json.