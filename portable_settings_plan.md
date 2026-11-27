# Portable settings plan: Thimsy settings in a user-editable file on the card

Status: draft for review (2026-11-26). Phase 0 (the schema inventory, §7.1) is done; nothing else has been built or run. Line numbers are at `linux` 1f45bd6. Facts that were not checked on a device or in a tool's source are marked **(verify)**; estimates are marked *(guess)*.

## Requirement (owner, 2026-11-26, OA-20)

- The card stays fully usable by the real ES-DE app. ES-DE's own files hold only ES-DE's formats; Thimsy-only data lives in Thimsy's own files.
- Every relevant Thimsy setting is written to a user-editable file on the card, so settings are truly portable between Thimsy devices and between the Android and Linux builds. Today they live in Android properties on the device.
- The owner accepts a significant change to how settings are stored, if needed.
- This plan feeds OA-4's `Prefs` facade (migration step 8 in `docs/linux/ARCHITECTURE-AUDIT.md`) and the wave 6 `Settings` interface. It also has to respect OA-8 (Linux and Android cards are interoperable) and OA-19 (card aliases, serial naming).

## Conventions

- **Pref**: a setting Nano owns. It has a schema entry and is read and written through the `Prefs` facade (§6).
- **Device store**: the per-device copy of every pref. On Android this is the existing `persist.*` properties under their **legacy names**. On Linux it is libthimsy's `ConfigStore` (`/storage/.config/thimsy/persist.props`). In Thimsy Home it is a key/value file in the app's data.
- **Card file**: the new `settings.ini` on the library's volume (§3.1).
- **Store**: the ES-DE folder Nano uses in store mode (`esdeDataDir()`, `NanoEsdeStore.cpp:120-126`).
- **AS** / **LO**: a step that edits Android-compiled code (an `android-surface:` commit, cherry-picked after the AOSP build), or one that touches only Linux or tooling. These are the legend of `ARCHITECTURE-AUDIT.md`.
- Classes, used in every table below:

  | Class | Meaning | On the card? |
  |---|---|---|
  | **P** portable | a user preference that should follow the user to any Thimsy device | yes, in a shared section |
  | **M** portable per model | a portable intent whose right value depends on the screen or buttons | yes, in a `[device.<model>]` section, falling back to the shared value |
  | **A** / **L** platform | meaningful only on Android (or only on Linux) | yes, in `[android]` (or `[linux]`); the other platform keeps it unread |
  | **E** ES-DE-owned | a setting ES-DE has too | lives in `es_settings.xml` only, as today |
  | **D** device | calibration, hardware identity, device paths, security, developer tuning | never; device store only |
  | **S** secret | passwords, API keys | never |
  | **R** runtime | state, IPC, one-shot flags | never, and not a pref at all |

## Where things stand

### Settings are properties, and nothing travels

- **One dispatcher writes both settings tables.** `readSettingValue` and `writeSettingValue` are at `NanoMenuSettingsTree.cpp:829-964`.
  - `kProp` rows call `property_get` and `property_set` (`:832-835`, `:866-867`).
  - `kGlobal`, `kSecure` and `kSystem` rows shell out to `settings get/put` (`:837-858`, `:868-878`).
  - The dispatcher then runs side effects:
    - Font Size is mirrored to `persist.gammaos.nano.fontscale` (`:884-889`).
    - A Screen Calibration change calls `screencal::applyFromProps` (`:892-894`).
    - Saturation calls `cmd color_display` (`:897-905`).
    - Orientation is coupled to `accelerometer_rotation` (`:910-913`).
    - Every `persist.gammaos.gamepad.*` write bumps `config_version` for the gammapad daemon (`:920-930`).
    - The parallax rows are recomposed into `pspclock.tilt.cal`, keeping its per-device `rot` field (`:938-962`).
- **The tables.** The legacy tree has 178 value rows, 158 of them `kProp` (`NanoMenuSettingsTree.cpp:193-759`). The XMB binding table `kPs3Bindings` has 230 rows, 207 of them `kProp` (`NanoMenuPS3Menu.cpp:9633-10058`). A bound row is written at `NanoMenuPS3Menu.cpp:12657` and `:12666`; an ES-DE-shared key is then also pushed to es_settings.xml (`:12667`).
- **Writes outside the tables.** Many prefs are written directly:
  - `applyThemeSetting` writes the XMB theme, colour, background, font, day/night, date and time format, wallpaper effect and time zone (`NanoMenuPS3Menu.cpp:12169-12240`).
  - The setup wizard writes the date, time and clock format (`NanoMenuSetupWizard.cpp:461-463`).
  - Brightness is written from the Control Centre and the system code (`NanoControlCenter.cpp:1614`, `NanoMenuSystem.cpp:147`).
  - The ES-DE theme rows go through `property_set(prop, …)` (`NanoMenuEsdeMenu.cpp:247`, table at `:53-59`).
  - Language is set with a shell `setprop` (`NanoMenuStrings.cpp:663`).
- **Many prefs are read once, at start.** For example `NanoMenu.cpp:1535-1600` latches `quick_resume`, `xmb_mode`, `ps3xmb`, `ndstheme`, `gamesort`, the folder views and the half-resolution switches into members. A value changed underneath Nano at run time is not seen until a restart, unless a watch reloads it.
- **Other processes read the same keys** (`docs/linux/audit-2026-11-26/inv_props.md`, class 1b):
  - PhoneWindowManager reads `hotkeys.*`; gammapad reads `gamepad.*`; SurfaceFlinger reads BFI, shaders and the refresh rate.
  - drastic-nano reads `quick_resume` and the hotkeys.
  - The scraper process reads `persist.gammaos.scraper.*` (`NanoScrapeQueue.cpp:497-498`).
  - Android Settings writes `persist.sys.spk.*`, which `GammaEqDsp.h:24-483` reads.
  - So on Android the property, under its current name, is an interface other programs depend on, not just storage.
- **Linux** emulates the whole property API on `ConfigStore`: `nano/compat/src/properties.cpp` (property_set at :416, then setProperty at :226, then `trySet`). The persist keys go to `/storage/.config/thimsy/persist.props`, on the boot card's ext4 data partition (`docs/linux/IMPLEMENTATION.md:218`). A PC cannot edit that file, and it does not move with a games card in a second slot.
- **Counts** (from inv_props.md): about 232 Nano-private prefs, about 284 persisted keys acted on by another Android process, 96 IPC keys, 69 Android-state reads and 35 `sys.*` scratch keys.

### What the card already carries

From `docs/linux/interop-2026-11-26/inv_esde.md`:
- **`es_settings.xml`:**
  - `ROMDirectory`, `CollectionSystemsCustom` and `MediaDirectory`.
  - Twelve scraper keys, mirrored both ways through `kEsdeSettingProps` (`NanoEsdeStore.cpp:1704-1723`).
  - The card's values win when the store loads or changes (`esdeStoreApplySettings`, `:1733-1757`). A Nano change is written back (`esdeStoreSettingChanged`, `:1771-1780`; called at `NanoMenuPS3Menu.cpp:12667`).
  - A value Nano does not accept is skipped (`esdeSettingValueOk`, `:1725-1729`).
- **Nano's own files** under `ES-DE/nano/`: `systems.json`, `names.json`, `emulators.json`, `index.json`.
- **Theme choice does not travel** (interop gap G14). The theme set, variant, colour scheme, font size and aspect ratio live only in `persist.gammaos.nano.esde.*` (`NanoThemeEngine.cpp:504-508`). Downloaded themes go to `/data/system/nano_esde_themes` (`NanoMenuEsdeMenu.cpp:894-905`).

### Machinery this plan reuses

- **`esdeWriteAtomic`** writes a temp file, fsyncs it and renames it (`NanoEsdeStore.cpp:72-85`). It does not fsync the directory; the contract's L12 adds that.
- **`esdeStoreSweep`** (`NanoEsdeStore.cpp:1274-1311`) runs every 2 s while the home is up and no game is running.
  - It compares mtime and size stamps (`esdeStamp`, `:195-199`) of every store file.
  - It detects a different card through `esdeStoreIdentity` (folder plus `st_dev`, `:187-192`).
  - `esdeNoteWrite` (`:1265-1272`) keeps Nano's own writes from counting as outside edits.
- **NanoJson** (`NanoJson.h`) is an ordered, dependency-free JSON reader and writer with no comments. tinyxml2 is already linked.
- **libthimsy** (Linux only) has `writeFileAtomic` with the directory fsync and a kill-mid-write test hook (`libthimsy/include/thimsy/atomic_file.h`). It also has `ConfigStore` observers and an inotify `Watcher` (`config.h`, `watch.h`).

## 1. Inventory by user-facing area

Every persist key in shared code was given a first-pass class by a rule over its prefix. The rule ran over the 516 class-1 keys in the audit's scratch inventory, `occ.json`/`cls.json`. Result *(first pass; Phase 0 replaces it with a reviewed table)*:

| Class | Keys |
|---|---|
| P | about 95 |
| M | about 18 |
| A | about 38 |
| E | 13 (12 keys plus the media prefix) |
| D | about 333 |
| S | 5 |
| R | about 14 |

Of the D keys, about 56 are `persist.sys.*` and about 120 are Android display and daemon tuning. Add 5 to 8 Android Settings-provider rows that become portable prefs (§1.4).

Evidence is file:line at `linux` HEAD. "Row" means a settings-table row: it is written through the dispatcher, at `NanoMenuPS3Menu.cpp:12657`/`12666` for the XMB and at `NanoMenuSettingsTree.cpp:863` for the legacy tree.

### 1.1 Language, date and time

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Language | `persist.sys.locale` | `NanoMenuStrings.cpp:611`, `NanoThemeEngine.cpp:509`, `NanoBiosInstall.cpp:88` | shell `setprop`, `NanoMenuStrings.cpp:663`; row `NanoMenuSettingsTree.cpp:207-231` | **P**, applied by the platform |
| Date format, time format, 12-hour clock | `nano.datetime.date_format`, `.time_format`, `nano.clock12`; `time_12_24` (kSystem) | `NanoMenuPS3Menu.cpp:9322-9324`, `NanoMenuSystem.cpp:239` | `NanoMenuPS3Menu.cpp:12176-12190`, `NanoMenuSetupWizard.cpp:461-463`; row `NanoMenuSettingsTree.cpp:713` | **P** (the framework copy is applied by the platform: `SystemActions::setTimeFormat` exists) |
| Time zone, automatic time | `persist.sys.timezone`, `nano.tz_olson`; `auto_time`, `auto_time_zone` (kGlobal) | `NanoMenuPS3Menu.cpp:16344`, `:18447`; `NanoMenuSetupWizard.cpp:270` | `NanoMenuPS3Menu.cpp:12219`, shell at `:18454`; wizard `SystemActions::setTimeZone` (`NanoMenuSetupWizard.cpp:470`) | **D** (the device's clock; open question 9) |

### 1.2 Home, themes and look

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Home theme (XMB, DSi, Minima, ES-DE) and mode | `nano.ndstheme`, `nano.minima`, `nano.ps3xmb`, `nano.xmb_mode`, `nano.nds.dark` | `NanoMenu.cpp:1537-1560` | rows `NanoMenuPS3Menu.cpp:9869`, `:9887`; `NanoMenuInput.cpp:333`, `:3987`; `NanoMenuPS3Menu.cpp:12714` | **P** |
| XMB theme, colour, background, font, day/night, wave | `nano.ps3xmb.theme/.color/.color_custom/.bg/.font/.daynight/.wave/.wave_explicit/.bottomclock(.fps)/.controlcenter` | `NanoMenuPS3Menu.cpp:9298-9354`, `NanoMenu.cpp:1577-1588`, `NanoMenuRender.cpp:6653` | `NanoMenuPS3Menu.cpp:9239-9344`, `:12172-12176`, `:12758`; rows `:9748`, `:9783-9784` | **P** |
| Wallpaper effect, DSi effect, dimming | `nano.wallpaper`, `nano.nds.effect`, `nano.wp.scrim` | `NanoMenu.cpp:211`, `:1595`, `:1612`; `NanoMenuPS3Menu.cpp:9465` | `NanoMenuPS3Menu.cpp:12233-12234`; row `:9781` | **P** |
| Custom wallpaper images | `nano.wp.xmb.top/.bottom`, `nano.wp.dsi.top/.bottom` | `NanoMenuPS3Menu.cpp:9365-9366` | `NanoMenuPhotos.cpp:2657`, `:2717-2718` | **P** when the image is on the store's volume (written in `%CARDID%` form, §3.3), else **D** |
| ES-DE theme set, variant, colour scheme, font size | `nano.esde.themeset/.variant/.colorscheme/.fontsize` | `NanoThemeEngine.cpp:504-507`, `:613`, `:659-661` | `NanoMenuEsdeMenu.cpp:247` (rows `:53-57`) | **P**; ES-DE mirror per §5.3 |
| ES-DE theme aspect ratio | `nano.esde.aspectratio` | `NanoThemeEngine.cpp:508`, `:662` | `NanoMenuEsdeMenu.cpp:247` (row `:58`) | **M** |
| ES-DE menu look, transitions, clock, video audio | `nano.esde.settingstheme/.menucolorscheme/.menublur/.transition/.gotosys/.clock/.variant_triggers/.videoaudio` | `NanoMenuEsdeMenu.cpp:98`, `:313`, `:541-544`; `NanoThemeEngine.cpp:248`, `:856`, `:4084`, `:6256`; `NanoMenuVideo.cpp:3225` | rows `NanoMenuPS3Menu.cpp:9871`, `:9873` | **P** |
| Theme download URL | `nano.esde.themes_url` | `NanoMenuEsdeMenu.cpp:894` | none (default only) | **P** |
| Minima background, long names; battery percent | `nano.minima.bg`, `.namescroll`, `nano.battpct` | `NanoMenuMinima.cpp:81`, `:137`, `:264`; `NanoMenuRender.cpp:3882` | rows `NanoMenuPS3Menu.cpp:9769-9778` | **P** |
| DSi options | `nano.nds.romtitle/.tileicon/.ambiance`, `nano.ndstheme.stack/.gap` | `NanoMenuNdsBanner.cpp:35`, `NanoMenuScraper.cpp:949`, `NanoMenu.cpp:5330`, `NanoMenuRender.cpp:975`, `:2323` | rows `NanoMenuPS3Menu.cpp:9755-9764`, `:9788-9791` | **P**; stack and gap **M** |
| UI scale, font scale, half-resolution switches | `nano.ps3xmb.uiscale`, `nano.fontscale` (+ `font_scale` kSystem), `nano.ps3xmb.halfres.*` | `NanoMenuPS3Menu.cpp:716`, `NanoMenuRender.cpp:6257-6282`, `NanoMenu.cpp:1582-1584` | `NanoMenuSettingsTree.cpp:888`, `NanoMenuRender.cpp:6267`; rows `NanoMenuPS3Menu.cpp:9637`, `:9749-9750` | **M** |
| Font | `nano.font` | `NanoMenuRender.cpp:4713` | `NanoMenuButtonMap.cpp:221` | **P** |
| Sort, folder views, display names | `nano.gamesort`, `nano.photo/.video/.music.folderview`, `nano.rom.show_display_names` | `NanoMenu.cpp:1560-1568` | `NanoMenuPS3Menu.cpp:1076`, `:5121`; `NanoMenuPhotos.cpp:987`; `NanoMenuVideo.cpp:557`; `NanoMenuMusic.cpp:599`; row `NanoMenuSettingsTree.cpp:677` | **P** |
| Home category order and visibility | `/data/system/nano_categories.json` (a file, not a property) | `NanoMenuPS3Menu.cpp:37` (NanoJson), reloaded at `NanoMenu.cpp:6044` | the category editor | **P** (a list value, §3.2) |
| Start-up menu, start-up and navigation sounds | `nano.boot_to`, `nano.boot_sound`, `nano.nav_sounds` | `NanoMenuInput.cpp:1931`, `NanoMenuPS3Boot.cpp:96`, `:205` | rows `NanoMenuPS3Menu.cpp:9753`, `:9874`, `:9883` | **P** |

### 1.3 Quick Menu, Control Centre and clock

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Function button opens Quick Menu | `nano.mode_quick_settings` | `NanoMenuInput.cpp:1517`, `:4055` | row `NanoMenuPS3Menu.cpp:9913` | **P** |
| Quick Resume | `nano.quick_resume` | `NanoMenu.cpp:228`, `:1535`; drastic-nano | `NanoMenuInput.cpp:4031`, `NanoMenuPS3Menu.cpp:12385`; row `:9759` | **P** |
| Control Centre double tap, timeout | `nano.cc.doubletapwake`, `nano.cc.sleeptimeout` | `NanoControlCenter.cpp:1022-1023` | `NanoControlCenter.cpp:1068`, `:1075`; rows `NanoMenuPS3Menu.cpp:9927-9928` | **P** |
| Slide clock and parallax rows | `nano.pspclock`, `.freezeapp`, `.liveapp`, `.parallax(.strength/.dir)` | `NanoMenu.cpp:4824`, `NanoMenuPS3Clock.cpp:487`, `:1266`; `NanoMenuSettingsTree.cpp:946-950` | rows `NanoMenuPS3Menu.cpp:9747`, `:9793-9801` | **P** |
| Tilt calibration | `nano.pspclock.tilt.cal` (its `rot` field is the accelerometer mount) | `NanoMenuPS3Clock.cpp:394` | `NanoMenuSettingsTree.cpp:962` | **D** (recomposed from the P rows, keeping `rot`) |
| Overlay dim, wave, pause, home | `nano.overlay.dim/.wave/.pause`, `nano.overlay_home` | `NanoMenuRender.cpp:107`, `:6811`; `NanoMenuOverlay.cpp:353`, `:994` | defaults only | **P** (dim, wave, pause); **D** (overlay_home) |
| Clock and renderer tuning knobs | `nano.pspclock.{prof,fpslog,glow*,mirror*,lensres,no*,capms,sampleiv,scrim,appdim,appsrc,facemsaa,faceprof,tilt}` | `NanoMenuPS3Clock.cpp:132-2589` | none (developer) | **D** |

### 1.4 Display and power

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Brightness | `nano.brightness` | `NanoMenu.cpp:914`, `:5516` | `NanoControlCenter.cpp:1614`, `NanoMenuSystem.cpp:147`; Linux `platform/linux/Power.cpp:46`; row `NanoMenuPS3Menu.cpp:9668` | **D** |
| LiveDisplay: reading mode, R/G/B calibration, saturation, Night Light | `nano.display.reading/.cal_r/g/b/.saturation`; `night_display_*` (kSecure) | `NanoMenu.cpp:5582` | rows `NanoMenuPS3Menu.cpp:9681-9696`; apply at `NanoMenuSettingsTree.cpp:897-905` | **D** (panel) |
| Screen calibration (RG DS) | `nano.screencal.top.*`, `.bottom.*` (14) | `NanoScreenCal.h:44` prefix | rows `NanoMenuPS3Menu.cpp:9702-9715`; apply at `NanoMenuSettingsTree.cpp:892` | **D** |
| Orientation, forced orientation, controller screen | `nano.orientation`, `nano.orient_mode`, `nano.ctrl_screen` | `NanoMenuOverlay.cpp:2593`, `:2901`, `:2958` | rows `NanoMenuPS3Menu.cpp:9655-9660`, `:9847`; coupling at `NanoMenuSettingsTree.cpp:910-913` | **M** |
| Screen timeout | `screen_off_timeout` (kSystem) | `NanoMenuSettingsTree.cpp:837-858` | rows `NanoMenuSettingsTree.cpp:241`, `NanoMenuPS3Menu.cpp:9634` | **P**, applied by the platform (Linux: thimsy-session's idle timer) |
| Dark theme, screen saver | `ui_night_mode`, `screensaver_enabled` (kSecure) | dispatcher | rows `NanoMenuPS3Menu.cpp:9650`, `:9670` | **A** |
| Slide hardware and actions | `persist.gammaos.rotate.*` (10) | `NanoMenu.cpp:4072-4087`, `NanoMenuInput.cpp:1167-1173` | rows `NanoMenuPS3Menu.cpp:9724-9746` | **D** (only slide devices; open question 6) |
| CRT and LCD shaders | `persist.gammaos.shader.*` (about 30) | `NanoControlCenter.cpp:428`, `:1655`; `NanoMenuInput.cpp:3143`; `NanoMenuPS3Menu.cpp:11568` | `NanoControlCenter.cpp:1656`, `NanoMenuInput.cpp:3145`, `NanoMenuPS3Menu.cpp:11690-11693`; rows `NanoMenuSettingsTree.cpp:376-451` | **A** (renderengine filters; global_shader_extension_plan.md may make them portable later) |
| BFI, refresh lock and rate, display tweaks, SurfaceFlinger tuning | `bfi.*` (25), `refresh.*`, `display.tweaks`, `force_client_comp`, `vsync_period_ns`, `renderengine.backend`, `sf.*`, `square.sticky_ms` | SurfaceFlinger (patch) | rows `NanoMenuSettingsTree.cpp:289-373` | **D** (Android and panel) |
| Dual-stack, external display, multi-display, IME pinning, primary display | `dualstack.*`, `sec_force_on`, `secondary_home`, `display.mode`, `display.delay.*`, `multidisplay.*`, `ime.pin.*`, `nano.primary_display` | `NanoDisplayTargetSf.h:58`, `:98`; display service | rows `NanoMenuSettingsTree.cpp:454-510` | **D** |
| Performance mode, fan, ultra-low power, swap, background limit | `performance_mode`, `fan_mode`, `ultra_low_power_*`, `swap.size_mb`, `bg_process_limit`, `gesture_wake_ignore` | `NanoControlCenter.cpp:423`, `:1641`; `NanoMenu.cpp:1249` | `NanoControlCenter.cpp:1646`, `NanoMenu.cpp:1262`, `NanoMenuPS3Menu.cpp:12242`; rows `NanoMenuSettingsTree.cpp:639-662` | **D** |
| Battery percentage (system), battery saver | `status_bar_show_battery_percent`, `low_power` | dispatcher | rows `NanoMenuPS3Menu.cpp:9648-9649` | **A** (Nano's own `battpct` is the P one) |

### 1.5 Sound

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Volume, maximum volume | `nano.volume`, `nano.volmax` | `NanoMenu.cpp:1028-1031`, `NanoMenuSystem.cpp:304`, `NanoMenuPS3Boot.cpp:52-53` | the volume path | **D** (speaker) |
| GammaEQ and speaker DSP | `persist.sys.gammaeq.*` (6), `persist.sys.spk.*` (47) | `GammaEqDsp.h:24-483`, `NanoControlCenter.cpp:429` | rows `NanoMenuPS3Menu.cpp:9989-10030`; Android Settings | **D** (tuned to the speaker) |
| Menu music, video audio | `nano.nds.ambiance`, `nano.esde.videoaudio` | see 1.2 | see 1.2 | **P** |
| Touch, charging and lock sounds | kSystem/kSecure | dispatcher | rows `NanoMenuPS3Menu.cpp:9645-9647` | **A** |
| Bluetooth audio routes | `nano.bt.ad_in/.ad_out/.ad_mic` | `NanoMenuPS3Menu.cpp:16237-16239` | `NanoMenuPS3Menu.cpp:16599-16603` | **D** |
| Multi-volume, HDMI audio | `audio.multivolume`, `unisoc.hdmi.enable`, `allwinner.hdmi.enable` | Android audio | rows `NanoMenuSettingsTree.cpp:513-520` | **D** |

### 1.6 Controls

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Button prompts, OK button | `nano.face_glyphs`, `nano.face_swap` | `NanoMenuPS3Menu.cpp:11053-11106` | rows `NanoMenuPS3Menu.cpp:9862-9864` | **P**; the default comes from the device's `face_layout` (controller_layout_plan.md) |
| Universal hotkeys: on/off, Start+Select, hold to quit | `hotkeys.enable`, `.startselect`, `.quithold` | `NanoHotkeys.cpp:496`; PhoneWindowManager, drastic-nano | rows `NanoMenuPS3Menu.cpp:9836`, `:9841`, `:9843` | **P**, applied by the platform |
| Hotkey buttons | `hotkeys.save/.load/.fastforward/.rewind/.quit/.buttonmap/.screen` | `NanoMenuButtonMap.cpp:387`, `:701-710` | rows `NanoMenuPS3Menu.cpp:9837-9845`; `hotkeyTakeButton` `:2863` | **M**, validated against the device's buttons |
| Pad transforms | `gamepad.invert_*`, `.analog_to_dpad`, `.dpad_to_analog`, `.dpad_threshold`, `.global_sensitivity`, `.mouse_*_speed`, `.mouse_boost` | gammapad (Android); thimsy-padd (Linux) | rows `NanoMenuSettingsTree.cpp:523-599`, `NanoMenuPS3Menu.cpp:9820-9908` | **P**, applied by the platform (`supports` per key) |
| Pad hardware: capture devices, virtual name, remaps, combos, vibration device, blacklist, PWM, enable/merge/hide | `gamepad.devices/.device_name/.remap_*/.combo_map/.axis_btn/.btn_axis/.ff_vibrate_device/.blacklist_pass/.pwm_*/.enable/.merge/.hide_source` | gammapad; `NanoMenuSettingsTree.cpp:543` | `NanoMenuPS3Menu.cpp:6926`; rows as above | **D** |
| Touch input on/off | `nano.touch_input` | `NanoMenuInput.cpp:3194` | row `NanoMenuPS3Menu.cpp:9857` | **P** |
| Touch and key devices, flips, Control Centre displays | `nano.cc.touchdev/.keydev/.topdev/.topdisplay/.bottomdisplay/.bl_*/.touch_*`, `nano.osk_touch_*`, `nano.touch_orientation`, `screenmap.enabled` | `NanoControlCenter.cpp:144-157`, `:1154-1518`; `NanoMenu.cpp:1772-1775`; `NanoMenuInput.cpp:3311-3313` | defaults; row `NanoMenuPS3Menu.cpp:9856` | **D** |
| RGB LED | `rgb.*` (15), `primary.rgb_hex_custom`, `persist.gammargb.control` | the RGB daemon | rows `NanoMenuPS3Menu.cpp:9974-9987` | **D** for now (open question 6) |

### 1.7 Library, games and scraper

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| Scan subfolders, group .m3u | `nano.rom.recursive`, `nano.rom.m3u_group` | `NanoMenuXmb.cpp:2119`, `:2159` | rows `NanoMenuPS3Menu.cpp:9946-9947` | **P** |
| Scraper engine, region, overwrite, media types | `persist.gammaos.scraper.engine/.region/.overwrite/.media.*` | `NanoEsdeStore.cpp:1704-1723`; the scraper process | rows `NanoMenuPS3Menu.cpp:10035-10057` | **E** |
| Box art as icons, hover background | `scraper.boxart`, `scraper.fanart` | scraper UI | rows `NanoMenuPS3Menu.cpp:10041-10042` | **P** |
| Scraper accounts | `scraper.ssuser/.sspass/.tgdbkey` | `NanoScrapeQueue.cpp:497-498` | rows `NanoMenuPS3Menu.cpp:10046-10048` | **S** (Export Library already excludes them: `android_launcher_plan.md` §3 item 9) |
| Where the store is | `nano.esde.dir`, `scraper.dir`, `nano.esde.mediadir` | `NanoEsdeStore.cpp:120-132`, `NanoThemeEngine.cpp:67`, `:1746` | `NanoSdMove.cpp:984-987`, `:2250-2253`; `NanoEsdeStore.cpp:177` | **D** (a file on the store cannot say where the store is) |
| Keep emulator files on microSD | `nano.emu_sd`, `.emu_sd_card`, `.emu_sd_appwins` | `NanoSdMove.cpp:100-107` | `NanoBiosUi.cpp:1153`; row `NanoMenuPS3Menu.cpp:9760` | **D** |
| Legacy per-system overrides | `nano.xmb.<id>.dir/.core/.path` | `NanoMenuXmb.cpp:351-356`, `:1862` | none in shared code | **D**, superseded by `nano/systems.json` and `systems.linux.json` (contract C5) |
| Apps lists | `nano.apps.showall`, `.extra_apps`, `.pinned_pkgs`, `.primary_pkgs`, `.background_pkgs`, `.screen_*_pkgs`, `.browser_pkg` | `NanoMenuPS3Menu.cpp:138`, `:13067-13083`, `:13436`; `NanoMenuState.cpp:362` | `NanoMenuPS3Menu.cpp:14395`; row `:9721` | **A** (Android package names) |
| Per-app orientation | `/data/system/nano_app_orient.json` | `NanoMenuOverlay.cpp:2084-2092` | `NanoMenuOverlay.cpp:2118` | **A** |

### 1.8 Network, services and system

| Setting | Key(s) | Read | Written | Class |
|---|---|---|---|---|
| IPTV and radio on/off and playlist URLs; search engine | `nano.iptv(.url)`, `nano.radio(.url)`, `nano.search_engine` | `NanoMenuIptv.cpp:61`, `:340`; `NanoMenuRadio.cpp:68`, `:351`; `NanoMenuPS3Menu.cpp:5928` | rows `NanoMenuPS3Menu.cpp:9643`, `:10037-10040` | **P** |
| IPTV and radio disclaimers accepted | `nano.iptv.agreed`, `nano.radio.agreed` | `NanoMenuPS3Menu.cpp:6574`, `:6601` | `NanoMenuPS3Menu.cpp:6138`, `:6145` | **D** (asked once per device) |
| System (network) name | `nano.system_name` | `NanoMenuPS3Menu.cpp:10638`, `:10763` | row `NanoMenuPS3Menu.cpp:9640` | **D** (two devices with one name collide on the network) |
| Kiosk Mode | `nano.kiosk` | `NanoMenuPS3Menu.cpp:2337`, `:12675` | row `NanoMenuPS3Menu.cpp:9875` | **D** (a restriction the card must not be able to lift) |
| SSH switch, password, any-address (Linux, OA-3) | not built yet | none | none | **D** (security) |
| Wi-Fi, Bluetooth, airplane, Do Not Disturb, location, USB debugging, animation scales, show touches | kGlobal/kSecure/kSystem | dispatcher | rows `NanoMenuPS3Menu.cpp:9804-9812`, `:9966-9971` | **A**, and not prefs at all: Android system state behind `Settings` |
| Widevine L3, launch guard, QS lists, taskbar, desktop, wallpaper, USB switch, DC dimming, Start+Select LED, RetroArch back button | `drm.force_l3`, `launch.guard.*`, `qs.*`, `taskbar.*`, `desktop.fullscreen`, `wallpaper.force_multidisplay`, `usbcontrollerswitch`, `dcdimmingemulation`, `startselectled`, `retroarchoverride.backbutton` | Android components | rows `NanoMenuSettingsTree.cpp:629-702`, `NanoMenuPS3Menu.cpp:9915-9951` | **D** (Android and device) |

### 1.9 Never prefs

- **Set-up and one-shot state (R):**
  - `nano.setup_done`, `.setup_prev_perf`, `.dp_wizard`, `setupwizard_*` (`NanoMenu.cpp:1509-1521`, `NanoMenuSetupWizard.cpp:158-232`).
  - `nano.dualscreen_dismissed`, `.accel_seeded`, `.urot_reset`, `.scrape_seq`, `.usb_mode`.
- **Quick Resume state (R):** `nano.qr_*` (`NanoMenu.cpp:619-3249`, `NanoMenuUtils.h:155-197`).
- **Developer knobs (D, never on the card):**
  - `nano.mlockall`, `.triple_buffer`, `.vsync`, `.force_sf`, `.prime_no_fence`, `.perf.*`, `.vid.*`, `.drm_flip_*`, `.nds.idle_redraw_ms`, `.globe.*`, `.ps3xmb.fpslog/.idlefps`, `persist.thimsy.nano.fpslog`, `.drastic_*`, `.cc.cache`, `.grab_input`, `.btlpmsleep`, `.focusring`, `.ime`.
- **Not settings at all (R):** class 2 (96 IPC keys), class 3 (69 Android-state reads) and class 4 (35 `sys.*` scratch keys) of inv_props.md. They go to step 5 (`Session`) and step 6 (area interfaces) of the OA-4 migration.

### 1.10 Where per-device overrides of portable settings are needed

- **Screen-dependent (M):**
  - UI scale, font scale and the half-resolution switches: a 640x480 3.5" panel against the Cube's 720x720 3.95".
  - DSi stack and gap, the theme's aspect ratio, orientation, forced orientation, controller screen.
- **Button-dependent (M):** the hotkey assignments. A device without L2/R2, or with a different function button, cannot honour another model's choice. The device profile supplies the default, and the value is checked against the device's button set (`HOTKEYS.md`, `controller_layout_plan.md`).
- **Defaults that come from the device but are portable once set:** `face_glyphs`/`face_swap` (from `face_layout`) and `mode_quick_settings` (its label names the device's own button, `NanoMenuPS3Menu.cpp:9913`). These need a device-profile default layer, not an override.
- **The key is the device model, not the unit.** Overrides key on the model, so two RG Cubes share them and the ARC-D's Android and Linux builds share them. Per-unit values (calibration, brightness) stay class D on the device.

## 2. Classification summary and the rule behind it

**The rule:** a setting goes on the card when a user would expect it to follow them to another Thimsy device. It stays on the device when any of these holds:
- its value is a measurement of this physical unit (calibration, brightness, volume, speaker EQ, accelerometer mount);
- it names this unit's hardware (input nodes, key codes, display ids);
- it is security-relevant (Kiosk Mode, SSH, passwords);
- it tunes a driver or daemon (SurfaceFlinger, BFI, developer knobs).

A capability-dependent portable setting still travels. A device without the capability (no LEDs, no second screen) keeps the value in the file, ignores it, and hides the row (R3 and R4 in `ARCHITECTURE-AUDIT.md`).

**Platform-specific values travel in their own sections.** `[android]` holds Android-only portable preferences such as the CRT shader choice and the Apps lists, so they carry between two Android Thimsy devices. Linux keeps `[android]` unread and preserves it, and the reverse holds for `[linux]`. Android system state (Wi-Fi, ADB, animation scales) is not a pref and is not in the file.

## 3. File design

### 3.1 Location

| Option | For | Against |
|---|---|---|
| **(a) `<volume>/Thimsy/settings.ini`, beside the store's `ES-DE/` folder** (recommended) | Users who open the card on a PC see a folder named after the product, next to `ES-DE/` and `ROMs/`. It sits beside `Thimsy/emulators/` (user_emulators_plan.md, linux_handheld_plan.md §7.2). It is outside ES-DE's folder, so it is out of the way of an ES-DE reset or reinstall, and OA-20's "Thimsy-only data in Thimsy's own files" holds without argument. The internal store gets `/storage/emulated/0/Thimsy/settings.ini`, which is visible over MTP and USB. | One more path to resolve. Export Library (D12) must add it, though it already zips `Thimsy/emulators`. |
| (b) `<store>/nano/settings.ini` | Resolution is free: it is the store. It travels with any copy of `ES-DE/`. The sweep already watches `nano/`. | Hidden inside ES-DE's folder. A user who deletes or resets `ES-DE/` loses it. It reads as part of ES-DE's data. |

**ES-DE compatibility.** Both are safe: the real ES-DE app ignores folders it does not know, and `ES-DE/nano/` already ships without trouble.

**Resolution rule (a):**
- The file is `dirname(esdeDataDir()) + "/Thimsy/settings.ini"`, so it follows the store when `esdeStoreFollowCard` moves it (`NanoEsdeStore.cpp:157-184`).
- With store mode off, no card file is used and Nano runs on the device store. This is today's behaviour, and the contract's L1 makes store mode the Linux default anyway.
- Nano ignores macOS `._settings.ini` AppleDouble files and the like.

**Trust.** Any Android app with storage access can write the file. That is why security-relevant keys are class D and never read from it (§2), and why a value is accepted only when it passes the schema's check (§3.4). The trust rule for emulator definitions ("import, never read live") does not apply, because a pref can only take a value the UI could set.

### 3.2 Format: a strict INI dialect

| Format | Edit on Windows / macOS | C++ cost | Comments and user layout kept | Verdict |
|---|---|---|---|---|
| **INI** | `.ini` opens in Notepad on a double-click. TextEdit opens it as plain text **(verify)**. | about 300 lines, no dependency | yes, with a line-preserving editor (as `esdeSettingsSet` does for es_settings.xml) | **recommended** |
| JSON | needs care with quotes and commas; no comments | none (NanoJson) | no: NanoJson rebuilds the text, and there are no comments to keep | rejected for a hand-edited file |
| TOML | `.toml` has no default program on Windows; strings must be quoted | toml++ (about 17k lines, header-only) or a hand subset | toml++ drops comments on re-emit **(verify)** | rejected: heavy, and fussy for users |
| XML | ES-DE's style; awkward by hand | none (tinyxml2) | partly | rejected |

The dialect, written down in `SETTINGS-FORMAT.md` (or a section of `LIBRARY-FORMAT.md`):

- **Encoding:** UTF-8, with an optional BOM. LF or CRLF line ends; the file's existing style is kept when Nano rewrites it.
- **Comments:** whole lines starting with `#` or `;`. There are no inline comments: a `#` after a value is part of the value, which a URL fragment needs. A value then fails its check and is logged with a hint ("inline comments are not supported").
- **Sections and keys:** `[section]` headers. Keys match `[a-z0-9_.]+`, as `key = value` with the spaces trimmed.
- **Quoting:** a value may be wrapped in `"…"` to keep leading or trailing spaces, with `\"` and `\\` inside. Curly quotes (“ ”) wrapping a whole value are taken as straight quotes, because macOS TextEdit substitutes them as the user types **(verify)**.
- **Typed values:**
  - Booleans accept `on/off/true/false/yes/no/1/0` in any case; Nano writes `on`/`off`.
  - Lists are comma-separated, with `\,` for a comma inside an item.
  - Colours are six hex digits without `#`.
  - Paths are relative to the volume (`%CARDID%/…`, contract C1 form d) or absolute for another volume.
- **Duplicates:** the last one wins, with a warning. A section may appear twice; the two are merged.
- **Limits:** 1 MiB per file and 4 KiB per line. A larger file is ignored and the card is treated as read-only for settings (logged).
- **Writing:**
  - Nano rewrites only the lines whose values change.
  - A new key goes at the end of its section. A new section goes at the end of the file, in the schema's order.
  - Unknown keys, unknown sections, comments, blank lines and order are kept byte for byte.
  - A file Nano creates begins with a header comment that says what the file is and how to reset it (delete the file). It then lists every class P, M and A key in schema order:
    - as a commented template line, `# key = default    (allowed values)`, while unset;
    - as an active line once set.
  - So the file documents itself, and a release that changes a default still reaches users who never touched that key.

Example *(illustrative names; Phase 0 fixes them)*:

```ini
# Thimsy settings. Edit with any text editor while the card is out of the device.
# Delete a line (or this file) to go back to the defaults. Docs: SETTINGS-FORMAT.md
[meta]
format = 1
id = 6f1c2e0a-…            ; written once, identifies this file for sync (§4)

[general]
language = de-DE
# date_format = 0         (0 = DD/MM/YYYY, 1 = MM/DD/YYYY, 2 = YYYY-MM-DD)
clock_12h = off

[home]
theme = xmb               ; xmb, dsi, minima, esde
xmb_color = 7
categories = games,apps,music,video,photo,settings

[esde_theme]
set = colorful-simplified
variant = default

[hotkeys]
enable = on
hold_to_quit = on

[device.anbernic-rg-cube]
ui.font_scale = 1.15
hotkeys.fast_forward = BUTTON_R2

[android]
shader.enable = on
shader.type = crt-simple
```

### 3.3 Schema, version and validation

- **One schema table in shared code.** `NanoPrefsSchema.inc` is generated from `tools/nano-tree/prefs.json`, the same pattern as `control-keys.json` and `path-roots.json`. Each entry has:
  - the file key (`section.name`);
  - the class (P/M/A/L/E/D/S);
  - the type, with its range, options or pattern;
  - the default, and whether the device profile may override it;
  - the **legacy device-store key and its encoding** (for example `persist.gammaos.nano.iptv` stores `true/false`, `quick_resume` stores `1/0`, `ps3xmb.color` an index);
  - the "home" (`ini`, `esde`, or `device`);
  - whether the platform applies it (§6).
- **A ratchet test fails** when a `persist.*` literal in shared code has no row, as `test_control_keys.py` does for triggers.
- **Version.** `[meta] format = 1` is a major number, bumped only when a key's meaning changes incompatibly. Adding keys never bumps it.
  - A reader that meets a higher major treats the card file as read-only: it applies the keys it knows and never writes, and the settings page says so. This is the same rule as `format.json` in contract C3.
  - Shipped Android builds never read the file, so the version guards only new builds against newer ones.
- **Unknown keys** (from a newer build, a typo, the other platform's section) are kept. A typo is logged once with the nearest known key ("did you mean `xmb_color`?").
- **Bad values** (out of range, not an option, wrong type):
  - the default is used (or the device-profile default, for M keys);
  - a warning is logged with the line number, once per distinct problem;
  - the line is **left as written**, so Nano never "fixes" a value it does not understand, perhaps one a newer build wrote. It is replaced only when the user sets that key in the UI.
  - The settings page shows a "Settings file: N problems" row that lists them.
- **Paths** to images (custom wallpapers) are written in `%CARDID%` form when they are on the store's volume (contract C1), resolved through `card.json` aliases on read (OA-19), and treated as class D when they point elsewhere.
- **Secrets** (class S) and class D keys are never written. A host test fails if the writer emits any key whose class is not P, M, A or L.

### 3.4 Layering

Values are resolved per key, lowest first:

1. **Built-in default** from the schema.
2. **Device-profile default** (`device.json` on Linux, the descriptor or `device-keys.json` on Android, multi_device_plan.md §4.2). Examples are the hotkey buttons, `face_layout` and the default orientation of a portrait panel.
3. **Card file, shared section**, for P keys, plus A or L keys on the matching platform.
4. **Card file, `[device.<model>]`**, for M keys and only for this model.
5. **Pending edits on this device** not yet written to the card (§4).

**Class D and S keys** use only layers 1, 2 and the device store.

**Writing an M key from the UI.** It goes to `[device.<model>]` and never to the shared section. A user who wants one value everywhere can move the line by hand: the shared value is the fallback for models without an entry.

**The model id** is supplied by the platform (`DeviceInfo::modelId()`, migration step 7). It must be the same string on the Android and Linux builds for one model, because the ARC-D runs both (OA-8, OA-18). It uses the descriptor id scheme that multi_device_plan.md decision 19 settles (`anbernic-rg-cube` against device-keys.json's `anbernic-rgcube` today).

## 4. Behaviour and sync semantics

### 4.1 Model: the device store is the working copy, the card file is the portable truth

- **The device store always holds the effective value of every pref.** Nano reads it in one step, and other processes (gammapad, PhoneWindowManager, SurfaceFlinger, drastic-nano, the scraper) keep reading their legacy property names on Android. Nano can start, draw and run without the card, which matters because Nano starts at early boot, before vold mounts the card on Android.
- **The card file is authoritative for portable keys whenever it is present.**
- **The device keeps three small pieces of sync state** (one JSON file in its state directory, `nano_prefs_sync.json`):
  - **pending edits:** portable keys the user changed on this device and that are not yet in any card file, each with a timestamp;
  - **per-card base:** for each card file id seen, the hash of the file as Nano last read or wrote it, and its parsed values. Kept for the last 8 ids, oldest dropped;
  - **a random install id** for logging which device wrote what. It is never the hardware serial.

### 4.2 Events

| Event | What happens |
|---|---|
| **Boot, card not yet mounted** | Run from the device store. Nothing is waiting. |
| **Card appears (boot, insert, or the store follows a new card)** | Read the file. Hash equals the base for that `id`: nothing changed on the card, so only pending edits are written. Hash differs (a PC edit, or another device wrote it): merge (below), apply the result to the device store, fire watches, write back if pending edits or migration filled anything. |
| **No file on the card** | First use of this card: write a new file from this device's set portable values (the migration, §5.2), with a fresh `id`. |
| **A file whose `id` this device has never seen** (another device's card) | Adopt: the card's values win for every key it sets. Pending edits from card-less use are then applied on top and written. The user sees a notice: "Settings from this microSD card applied: 7 changed", with **Undo** for 30 s; Undo restores the previous values and writes them to the card as edits. The device-local option **Settings from microSD: Always / Ask / Never** decides whether this happens silently (open question 2). |
| **User changes a portable setting, card present** | Write the device store at once and record a pending edit. Write the card file after a 3 s debounce, then clear the pending edit and update the base. Flush at once before a game launch (on Linux Nano exits on launch, `Launcher::nanoExitsOnLaunch`), before sleep and in `prepareShutdown`. |
| **User changes a portable setting, card absent** | Device store plus pending edit. It goes to the next card that appears. |
| **Card removed** | Nothing to do. Pending edits stay, and so does the base. |
| **Card swapped** (A out, B in) | Treated as "card appears" for B. B's values replace A's in the device store. A's base is kept, so when A returns its unchanged file applies cleanly and **no B value leaks onto A**: only pending edits are written to a card, never "the device store differs from this card". |
| **PC edit while the card is out** | Seen as a hash change on the next appearance; merged. |
| **PC edit while the card is in** (Android over MTP, a network share, or Linux over SSH) | The sweep sees a new stamp. Nano waits until the stamp has been stable for one more sweep (2 s), because Notepad and TextEdit write in place, truncating and then writing, not with a rename **(verify)**. Then it merges. |
| **Another process changes a pref's device-store value** (a SystemUI tile writes `persist.gammaos.gamepad.*`; Android Settings writes `persist.sys.locale`, `font_scale`; thimsy-session) | The watch sees it (§4.4) and records it as a pending edit with "external" as its origin, so it reaches the card too. |

### 4.3 Merge rule

- **Per key, three-way:** the card's value, the base for that card, and the pending edit.
  - Only the card changed: the card's value applies.
  - Only a pending edit exists: it applies and is written.
  - **Both changed** (a PC edit plus a device edit of the same key while the card was out): the **pending device edit wins** and the card's value is overwritten. A line is logged and the notice counts it as a conflict.
    - Why the device wins: the device edit was made deliberately in Nano's UI, and the overwritten PC value can be seen again in the log.
    - Wall-clock comparison is rejected, because handhelds without network time often have wrong clocks.
    - Open question 3.
- **"Two devices edit the same card"** is never concurrent: a card is in one device at a time. The real case is device A writing, the card moving to B, and B having card-less pending edits. The rule above covers it.
- **The same card in two slots of one device** cannot happen. **Two cards with settings in a two-slot device:** only the store's volume counts (contract L11 and interop Q7 choose the store).
- **A copied file** (the user copies `settings.ini` to a new card on purpose) keeps its `id`. The base for that id still matches its hash, so it applies like the original card. That is the intended outcome.

### 4.4 Change detection and the watch primitive

- **The card file** joins the existing sweep:
  - `esdeStoreSweepBuild` adds `<volume>/Thimsy/settings.ini` (`NanoEsdeStore.cpp:1226-1263`);
  - a new kind `kSwPrefs` routes it to the merge;
  - `esdeNoteWrite` marks Nano's own writes, so they do not count as outside edits (`:1265-1272`);
  - the sweep pauses while a game runs, as now (`:1279`).
- **The stamp misses some edits.** It is mtime plus size. exFAT keeps 10 ms timestamps; FAT32 keeps 2 s ones **(verify)**. So an edit in the same instant with the same size could be missed. The engine therefore also compares a content hash:
  - on every stamp change;
  - on every card appearance;
  - when Nano returns from a game.
  - The file is small (about 10 to 20 KB *(guess)*), so the hash costs nothing next to the gamelist reads.
- **`Prefs::watch(keyOrPrefix, callback)`** runs callbacks on the UI thread when an effective value changes, whatever the source:
  - Nano's own `set`;
  - a card merge;
  - another process writing the device store. On Android the firmware store polls `__system_property_serial` for the watched keys once a second, as the code does today for its own keys. On Linux `ConfigStore::addObserver` delivers it (`config.h`).
- **Members latched at start** (`NanoMenu.cpp:1535-1600` and the like) get watches during migration step 8. Until a key has one, its schema entry says `applies: restart`, and a card merge that changes such a key shows "Some settings apply after a restart".

### 4.5 Durability on exFAT

- **The write path** is one shared helper (contract L12, also wanted for gamelists, collections and `nano/*.json`):
  1. write `settings.ini.nanotmp`;
  2. `fsync` it;
  3. `rename` it over `settings.ini`;
  4. `fsync` the `Thimsy/` directory.
- **Recovery on read:** if `settings.ini` is missing or empty and a complete `.nanotmp` exists, the temp file is read and then renamed into place. Stale temps are removed only when the final file parses.
- **A Nano-written file ends with a trailer comment line.** A file without it that also fails to parse cleanly is treated as **damaged**: Nano reads what it can, uses the device store for the rest, and writes nothing until the user confirms "Repair settings file" on the settings page. A user who deletes the trailer by hand while the file still parses is not affected, because the trailer is only a hint.
- **Power-loss behaviour of rename on the kernel's exFAT driver** and on Android's public-volume path (Nano writes through `/mnt/media_rw` as root) is not proven atomic. Phase 4 includes a pull-the-card test on the ARC-D **(verify)**. The OA-13 dirty-flag check repairs the volume at the next mount.
- **One writer.** Only the main Nano process writes the card file. drastic-nano and the scraper read only the device store.

## 5. Compatibility

### 5.1 Additive, for builds already in users' hands

- **Shipped Android builds never read `Thimsy/settings.ini`.** They keep using their properties, so nothing they do breaks.
- **What a user sees moving between an old and a new build:**
  - an old build's setting changes do not reach the card;
  - a new build sees the card unchanged (same hash) and keeps its own device values.
- **Downgrading a device keeps its latest settings.** The new Android build keeps writing the **same legacy property names** as its device store, so after a downgrade the old build reads the latest effective values.
- **No file the shipped builds read changes:**
  - `es_settings.xml` gains no new keys from this plan (§5.3);
  - `nano/*.json` is untouched.
  - This meets INTEROP-AUDIT.md's rule: "Shipped Android builds never check a `version` field, so any format change must be additive or dual-written".

### 5.2 Migration from properties, on first run

- **Android (first run of a build with the engine):**
  - **No card file:** create it from every portable key that is **set** (`property_get` returns a non-empty value) and **differs from the schema default**. Skipping default values avoids pinning a default that `gammaos-nano.rc` or a boot script may set **(verify which keys rc files seed)**.
  - **The card already has a file** (it came from another device): adopt it as in §4.2, with the notice.
  - Either way, the props stay. They remain the device store.
- **Linux:** the same, from `persist.props`, whose keys have the same names. The alpha's `persist.props` carries over; no separate migration is needed.
- **Thimsy Home (launcher app):** its device store starts empty. The first card or internal store it opens gives it its settings. This is the "settings that travel" the launcher plan's Settings row needs (`android_launcher_plan.md` §1: "key/value file in the app's data, same key names").
- **The internal store** (no card; contract L1, launcher plan D7) gets `/storage/emulated/0/Thimsy/settings.ini`, so Export Library has a file to carry.
- **Export and Import Library (D12)** add `Thimsy/settings.ini`. On import, the imported file is adopted with the notice, never blindly copied over the device's own.

### 5.3 ES-DE's es_settings.xml

- **The ES-DE-owned keys (class E) stay in es_settings.xml only.** These are the 12 scraper keys in `kEsdeSettingProps`, plus `ROMDirectory`, `CollectionSystemsCustom` and `MediaDirectory`, which are library contract keys, not prefs.
  - The two-way mirror stays as it is (`NanoEsdeStore.cpp:1733-1780`), moved behind `Prefs` with `home = esde`.
  - They are **not** duplicated in settings.ini, because two homes would need a precedence rule and give the user two places to edit.
  - settings.ini's `[scraper]` section holds only Nano's own `boxart` and `fanart`, plus a comment pointing to es_settings.xml for the rest.
  - This keeps OA-20's "ES-DE's files hold only ES-DE's own formats": those keys are ES-DE's own.
- **Theme keys (interop Q3, not yet answered):**
  - Their home is settings.ini `[esde_theme]`, which always travels and never breaks the ES-DE app.
  - If the owner wants the real ES-DE app to show the same theme, Nano also writes ES-DE's `ThemeSet`, `ThemeVariant`, `ThemeColorScheme`, `ThemeFontSize` and `ThemeAspectRatio`. It does so **only when that theme exists in `<store>/themes/`**, as the backward-compatibility rule requires.
  - Nano reads them back when the ES-DE app changed them and Nano has that theme.
  - The guard is the rule from INTEROP-AUDIT.md: write ES-DE's `ThemeSet` only for themes that exist on the card.
  - Downloaded themes move to `<store>/themes` (contract A6).
- **No other Thimsy key is ever written into es_settings.xml.**

## 6. Fit with OA-4: the Prefs facade and the Settings interface

### 6.1 Who owns what

| Piece | Layer | Why |
|---|---|---|
| `settings.ini` format, schema, validation, merge, sync state, notices | **shared** (`nano/NanoIniDoc.*`, `NanoPrefs.*`, `NanoPortableSettings.*`, `NanoPrefsSchema.inc`) | The file is portable data, like gamelists and `nano/*.json`, which `NanoEsdeStore` (shared) already owns. Under R1 shared code may do file I/O on paths the platform supplies. All of it is pure and host-testable. |
| Where the store and volume are | **platform** (`Volumes`, and `Volumes::defaultStoreDir()` from contract L1) | Already the seam (`NanoSdMove.cpp:404-406` calls `volumes->cardRoot()`). |
| The device store (`get`, `set`, `isSet`, change serial) | **platform**, new `nanoplat::PrefStore` | Android: `property_get`/`property_set` on the legacy names, moved verbatim, together with the post-write hooks that are Android behaviour (the gammapad `config_version` bump, `color_display`, `accelerometer_rotation`). Linux: `ConfigStore`. Host: a map. Thimsy Home: the app's file. |
| Applying a pref that something outside Nano acts on (locale, screen timeout, time format, hotkeys, pad transforms, shaders) | **platform**, through intent interfaces: `SystemActions::setTimeFormat` (exists), `setLocale` (new), `Power::setScreenTimeout` (new), an `Input` or `Hotkeys` apply hook, the display extras of step 6 | R2 and R3: Android keeps its property or `settings put`; Linux does it natively (thimsy-padd, thimsy-session, `/storage/.config/thimsy/localtime`) or reports unsupported, and the row hides. |
| Model id | **platform**, `DeviceInfo::modelId()` (step 7) | R6. |
| Android system state (Wi-Fi, ADB, animation scales, night light) | **platform** `Settings` (wave 6) | Not prefs. |

### 6.2 How wave 6's Settings and the Prefs facade meet

- **Wave 6 moves the dispatcher.** `readSettingValue`/`writeSettingValue` and their hooks move into `platform/firmware` as `Settings::read`, `write` and `supports(source, key)` over `SettingSource` kProp, kGlobal, kSecure and kSystem. Linux answers from a keyed table.
- **This plan puts one shared check in front of it.** A binding row's key that is in the Prefs schema goes to `Prefs::set`/`get`, which writes the device store through `PrefStore`, records the edit and calls the pref's platform apply hook. Any other key goes to `nanoplat::Settings`.
- **So the 158 legacy and 207 XMB `kProp` rows need no edits:** the routing is by key, at one place (`NanoMenuPS3Menu.cpp:12657`/`12666` and the legacy tree's write).
- **Four kSystem/kSecure rows become prefs with a platform apply:** `font_scale`, `time_12_24`, `screen_off_timeout` and `status_bar_show_battery_percent`. Nano already shadows two of them in its own keys (`fontscale` at `NanoMenuSettingsTree.cpp:884-889`, `clock12` at `NanoMenuPS3Menu.cpp:12188`). Their `SettingSource` stays in the table for Android's apply hook.
- **`supports(source, key)` keeps its meaning** for non-pref keys. For a pref, the schema's apply hook answers through `has(Cap)`: a P key whose apply is unsupported on this platform (for example the `[input]` pad transforms before thimsy-padd has them) is kept in the file and hidden in the UI.

### 6.3 Keys that stay platform implementations, not prefs

- **Android system settings provider rows:** airplane, Wi-Fi, Bluetooth, Do Not Disturb, location, USB debugging, stay awake, show touches, pointer location, the three animation scales, dark theme, screen saver, Night Light, touch, charging and lock sounds, battery saver. These are class A or D state behind `Settings`.
- **IPC and control keys** (class 2): they go to `Session` (step 5) and the area interfaces (step 6).
- **Android-state reads** (class 3): `ro.*`, `sys.boot_completed` and the like go to `DeviceInfo` and `Session`.
- **Daemon tuning that the user never sees as a setting:** SurfaceFlinger, BFI, dual-stack, gammapad device and remap keys. These are class D: on Android they are `PrefStore` keys with no card presence, and on Linux they are unsupported.

## 7. Plan

### 7.1 Phases

| # | Step | Kind | Size *(guess)* |
|---|---|---|---|
| 0 | **Schema inventory.** `tools/nano-tree/prefs.json`: every `persist.*` key in shared code (from the audit's lexer) with its class, file key, type, default, legacy encoding, home and apply hook, reviewed by hand from §1. The ratchet test fails on an unclassified key. The owner's answers to §8 fill the open cells. | LO | 3-5 days |
| 1 | **`NanoIniDoc`:** the line-preserving INI reader and writer (§3.2): BOM, CRLF, quotes, curly quotes, duplicates, size limits, an edit API that touches only changed lines, and a validation report. Host tests and a fuzz target. Inert in the Android build. | AS (new file, not called) | 3-4 days |
| 2 | **`Prefs` facade and `PrefStore`:** `NanoPrefs.*` with an in-memory cache, typed getters, `set`, `watch` and the generated schema. `PrefStore` on firmware (verbatim property calls), Linux (`ConfigStore`) and host. No call site moves yet, so Android behaviour is byte-identical. This is the first half of migration step 8. | AS | 1-1.5 weeks |
| 3 | **Route the dispatcher through Prefs.** Schema keys in the binding rows go to Prefs. The post-write hooks move into `PrefStore` (firmware) or into platform apply hooks, verbatim (H.3). Lands on top of wave 6's `Settings` seam. | AS | 3-5 days |
| 4 | **`NanoPortableSettings`, the sync engine:** file location (§3.1), sweep integration, merge (§4.3), sync state, migration (§5.2), adoption notice and Undo, flush before launch, sleep and shutdown, the durable write helper (shared with contract L12), the damaged-file handling. **Off by default on Android** behind a device-local switch for the first build that carries it; **on by default on Linux.** | AS | 2-3 weeks |
| 5 | **Move direct call sites to Prefs**, area by area, in §1's order (home and themes first, then the Quick Menu, controls, library): about 232 Nano-private keys over about 40 files. Each latched member gets a watch. This is the mechanical bulk of migration step 8. | AS | 3-5 weeks |
| 6 | **Platform-applied prefs and per-model overrides:** `DeviceInfo::modelId()` and the device-profile default layer. Apply hooks for locale, screen timeout, time format, hotkeys and pad transforms. Linux implementations (thimsy-padd hotkeys, thimsy-session idle, locale) or unsupported. Hotkey validation against the device's buttons. | AS + LO | 1.5-2 weeks |
| 7 | **ES-DE and theme:** move the scraper mirror behind `home = esde`; theme keys into `[esde_theme]`; the guarded ES-DE theme mirror and downloads to `<store>/themes` if Q3 = yes (contract A6). | AS | 1 week |
| 8 | **UI and docs.** Settings > System > **Settings on microSD**: where the file is, last sync, problems, **Save to microSD now**, **Settings from microSD: Always / Ask / Never**, **Repair settings file**. Export and Import Library carry the file. `SETTINGS-FORMAT.md`, a README section, and a link from `LIBRARY-FORMAT.md`. | AS + docs | 1 week |
| 9 | **Turn it on for Android** after the AOSP build and the owner's smoke test of steps 1-8, including the ARC-D interop round trip (§7.2, device tests). | AS (default flip) | small |

**Phase 0 result (2026-11-26).**
- `tools/nano-tree/prefs.json` has a row for each of the 1,097 setting keys the scan finds in `nano/`, `linux/src` and `linux/board`'s root file system overlays (`test_prefs.py --list`): 443 rows, 20 of them globs over whole families (`sys.*`, `persist.gammaos.bfi.*` and the like). By key: 93 P, 11 M, 53 A, 0 L, 13 E, 458 D, 6 S, 449 R, and 14 not yet classified. 12 of the keys are named only outside `nano/` (thimsy-session and the init scripts).
- `tools/nano-tree/nanotree/prefs.py` finds the keys: property literals, key prefixes, `%d` templates, `setprop`/`getprop` and `settings get|put` shell strings, chooser specs (`"K|v1:l1,...|Title"`), and the settings-provider rows (written `global:K`, `secure:K`, `system:K`). It scans the Linux-only files too, and leaves out tests and the host's fake platform. It also reads `property:`, `setprop` and `getprop` in the init and shell scripts under `nano/` (`gammaos-nano.rc`, `thimsy-dexopt.sh`).
- Outside `nano/` it reads the C and C++ sources under `linux/src` (tests left out) and the shell scripts in `linux/board`'s root file system overlays: the key of `thimsy_persist`, `thimsy_ro`, `thimsy_build_prop`, `setprop` and `getprop`, bare or quoted. A key it cannot read fails the test: a variable (`thimsy_persist "$KEY" 1`), a key with a `$` expansion in it, or `persist.*` text in a `grep`, `sed` or `awk` line. It does not scan `libthimsy/tools/thimsy-prop.cpp` or `linux/board/common/post-build.d/20-build-props.sh`: they write only `ro.*` keys, all under the `ro.*` row (R).
- Keys built at run time are listed through `RUNTIME_FAMILIES` in `prefs.py`: a prefix joined to a table's column (the shader parameters from the `GsParamDef` tables, the hotkey buttons, the scraper's extra media, the composer's per-display BCSH), and `%s` templates with their run-time prefixes (the button-action rules, the `_1`, `_2` continuation keys of long package lists). The scan cannot see a key built some other way. Three checks narrow that gap. A `%s` key template no family claims fails the test (`"%sact%d_code"`, `"%s.top"`, `"%s.%s"`). So does a key literal joined to a run-time value with `+` (`"persist.gammaos.nano.slot" + std::to_string(i)`) that is not a family's prefix. And so does a key prefix with an exact row and no family.
- On Linux a property key is the same name in libthimsy's `ConfigStore` (no renaming). The settings-provider keys have no Linux store: ShellGate refuses `settings`, so reads give the default and writes are dropped. The one portable such key, `system:screen_off_timeout`, says so in its `linux` field until phase 6 gives it a store.
- `tools/nano-tree/tests/test_prefs.py` fails on a key without a row, a stale row, a portable row without a settings.ini key, type or default, a portable bool without its device-store encoding (`1/0` or `true/false`), an enum whose default is not one of its values, a default the code never uses, and a new unclassified key (the 14 are listed in `prefs.json` and may only shrink). It also checks the OA-32 answers. A glob row takes in any new key under it, so the keys under the `persist.*` globs are listed in `tools/nano-tree/prefs-globs.txt` and a change there fails the test until someone checks the new key's class.
- A long package list is stored as the key, then `<key>_1`, `<key>_2` and so on. Those rows say `part_of` the list's row and share its class; the list is one pref.
- Changes from §1: hotkey buttons, RGB and slide actions are D (OA-32 Q5); `[device.<model>]` holds only screen-dependent keys; `esde.gotosys` is D (a debug aid); `fontscale` and `clock12` are the prefs, and `system:font_scale` and `system:time_12_24` are their Android copies (R). §1.8's Wi-Fi, Bluetooth, airplane, Do Not Disturb, location, USB debugging, animation scales and show touches are D, not A: they are system state, not prefs (§2), and D says that. §3.3's `home` field is not written: it follows from the class (P, M, A and L are `ini`, E is `esde`, D, S and R are `device`).

**Total:** about 11 to 17 person-weeks *(guess)*. Steps 2, 3 and 5 are migration step 8 work that OA-4 needs anyway. The portable part proper (1, 4, 6-8) is about 6 to 9 weeks.

**Which steps change Android-compiled code:** every step except 0, so every one is an `android-surface:` commit stacking on the uncompiled G7 backlog (ARCHITECTURE-AUDIT.md's caution). Two things keep each reviewable:
- step 1 adds code nothing calls;
- steps 2-3 change no behaviour, which can be checked with the existing host tests plus a property-trace diff (every property write before and after, in the same order).

**Order against the other plans:**
- Step 0 can start at once.
- Steps 1-3 follow wave 6's `Settings` seam (migration step 3).
- Step 4 needs contract L1 (store mode on by default on Linux) and L12 (durable writes).
- Step 6 needs migration step 7 (`DeviceInfo`).
- Step 7 waits on Q3.

### 7.2 Tests

**Host, on the fake platform** (`tests/` and `nano/platform/host`):

- **Round trips:**
  - every schema key, set to every allowed value, through the UI path → device store → card file → a fresh device → the same effective value and the same legacy property encoding;
  - a Nano-written file re-read and re-written is **byte-identical**.
- **PC-edited files:**
  - CRLF, BOM, tabs, keys in any case, quoted and curly-quoted values, `yes`/`no` booleans;
  - comments, blank lines and reordered sections kept;
  - duplicate keys and sections;
  - a `#` after a value;
  - unknown keys and sections (kept byte for byte), a typo (warned with a suggestion);
  - an `[android]` section read on Linux (kept, ignored);
  - a `format = 2` file (read-only, applies known keys, never writes).
- **Corrupt files:**
  - 0 bytes, binary garbage, NUL bytes, invalid UTF-8;
  - truncated mid-line, no trailer;
  - 10 MB, a 100 KB line;
  - a `.nanotmp` left beside the file, or one alone;
  - in every case Nano runs on the device store, never writes a damaged file, and logs.
- **Kill mid-write:** a hook between the temp file and the rename (as libthimsy's `setAtomicWriteTestHook`) leaves the old file and recovers.
- **Two-device scenarios:**
  - A writes; B, with no pending edits, adopts.
  - B has card-less pending edits; it adopts, then applies and writes them.
  - Both changed the same key: device wins, conflict logged.
  - A↔B card swaps, ten in a row, with edits in between: no value from one card reaches the other unless the user edited it while that card was in.
  - A copied file with the same `id`.
  - An Undo of an adoption.
- **Change detection:**
  - a PC edit while the card is in, with an in-place truncate-then-write: debounced, read once;
  - a same-size edit within the stamp granularity: caught by the hash;
  - Nano's own write: not seen as an outside edit;
  - another process writing a watched device-store key: recorded as a pending edit.
- **Compatibility:**
  - a device with props only and no card file → the migration writes only set, non-default portable keys;
  - a simulated shipped build (props only, never reads the file) alternating with the new build on one card: no loss on either side;
  - a downgrade: the props hold the latest values.
- **Safety:**
  - no class D or S key is ever written (scan of every produced file);
  - an M key written from the UI lands in `[device.<model>]`;
  - Kiosk Mode and the system name never follow a card;
  - an invalid hotkey button for this model falls back to the device default.
- **Ratchets:** `prefs.json` covers every key; every schema `applies: restart` key is either watched or listed.

**On the ARC-D (OA-18), once step 4 runs on both builds:**
- Android from the eMMC changes the theme, language and hotkeys → Linux from the card shows them → Linux changes them back → Android shows them.
- Edit the file on Windows (Notepad) and macOS (TextEdit) while the card is out, then insert it.
- Pull the card mid-write (step 4's durability check).
- Cube and ARC-D with disk images for the two-slot case.

### 7.3 Risks

- **Two copies drift.** The device store and the card file can disagree after a bug. Mitigation: the card is re-read on every appearance and every return from a game; **Save to microSD now** and the problems row exist; the sync state is plain JSON and is logged.
- **Hot-path reads.** Some prefs are read every frame (font scale, `NanoMenuRender.cpp:6257-6282`). `Prefs::get` must be a cache hit with no lock on the read path. Measured in step 2 against the property-call baseline.
- **Values set underneath Nano.** SystemUI tiles, Android Settings and gammapad write device-store keys directly. Missing one means a change that never reaches the card. Mitigation: the watch covers every P, M and A key's legacy name, not just the ones Nano writes.
- **The 91-byte property limit on Android.** A URL or a category list over 91 bytes cannot live in one property. Long values (IPTV and radio URLs, `categories`) keep a device-side file in `PrefStore`, as `nanoPkgListRead` already splits package lists.
- **Surprise on adoption.** A borrowed card changes the language. Mitigation: the notice with Undo, the Always/Ask/Never switch, and language changes shown in their own line of the notice.
- **User edits that break things.** For example a font scale that makes the menu unusable. Every value is range-checked, and M keys have tight ranges. Deleting the line or the file always restores the defaults, and the header says so.
- **TextEdit, Notepad and other editors.** Smart quotes, RTF saving, BOMs, `._` files. Covered by the format rules and by step 8's device check; RTF is detected (`{\rtf`) and reported, not parsed.
- **Merge pain and the uncompiled Android backlog.** Steps 2-5 touch many shared files that upstream also edits. Mitigation: keep the edits mechanical, cut the seam at the dispatcher, and put the new logic in new files.
- **The model id scheme is unsettled** (multi_device_plan.md decision 19). A rename later would orphan `[device.*]` sections. Mitigation: an alias table in the schema (old id → new id), read on load.
- **Secrets.** The scraper account must never reach the card. This is enforced by class S plus the writer test, and it stays aligned with I46 (Linux `persist.props` is 0600 and redacted).

### 7.4 Recommendation

- **The file:** `Thimsy/settings.ini` beside the store's `ES-DE/` folder, in a strict, line-preserving INI dialect with commented templates for unset keys.
  - A shared schema generated from `tools/nano-tree/prefs.json` classifies every key.
  - Only class P, M, A and L keys are written. M keys go to `[device.<model>]` sections.
  - ES-DE's own keys stay only in es_settings.xml.
- **The engine:** the device store, under the legacy names, remains the always-valid working copy that Android's other processes and older builds read.
  - The card file is authoritative when present, merged three ways per key against a per-card base, with pending device edits winning a conflict.
  - Changes are found by the existing 2 s sweep plus a content hash.
  - Writes use temp, fsync, rename and directory fsync.
- **The layers:** the format and merge are shared code. Locating the store, the device store, the model id and applying a pref outside Nano are the platform's. The `Prefs` facade sits in front of wave 6's `Settings` and routes by key.
- **Rollout:** ship it off by default on Android until the AOSP build is smoke-tested, and on by default on Linux.

## 8. Decisions for the owner

1. **Location:** `<volume>/Thimsy/settings.ini` (recommended) or `<store>/nano/settings.ini`.
2. **Adopting another card's settings:** apply silently with a notice and Undo (recommended default **Always**), **Ask** each time, or **Never** (card settings ignored on this device). This is a device-local switch either way; which is the default?
3. **Conflict rule:** when a PC edit and a card-less device edit change the same key, the device edit wins (recommended), or the card's value wins?
4. **Theme travel (interop Q3):** the theme in settings.ini (recommended, always); and also mirror ES-DE's `ThemeSet` and the rest into es_settings.xml when the theme exists in `<store>/themes`?
5. **Hotkey assignments:** per model on the card (recommended), or device-only as the requirement's list suggests? Universal hotkeys on/off, Start+Select and hold to quit are proposed as fully portable.
6. **Hardware-dependent preferences:** should the RGB LED colours and effect, and the slide actions (`rotate.down_action/up_action/launch_target`), travel per model, or stay device-only (the first pass keeps them on the device)?
7. **Platform sections:** should `[android]` (CRT shader choice, Apps lists, per-app orientation) travel between Android devices, or should Android-only preferences stay on the device?
8. **Secrets:** scraper account, password and TheGamesDB key never on the card (recommended), or an opt-in?
9. **Language and time zone:** language portable (recommended). Time zone and automatic time device-local (recommended), or portable?
10. **Unset keys:** written as commented templates with defaults and options (recommended, self-documenting), or only the keys the user changed?
11. **Rollout and floor:** off by default in the first Android build that carries it, then on (recommended)? Which Android release is the floor (ties to interop Q12)?
12. **Model ids:** confirm that `[device.<model>]` uses multi_device_plan.md's descriptor ids (decision 19), identical on the Android and Linux builds of one model.
13. **Kiosk Mode and the system name:** device-only (recommended)?

## Docs

- `SETTINGS-FORMAT.md` (new), or a section of `LIBRARY-FORMAT.md`: the dialect, every key with its type, default and allowed values (generated from `prefs.json`), the classes, the layering and the merge rule.
- README: "Settings on the microSD card": where the file is, editing it on a PC, resetting it.
- `docs/linux/ARCHITECTURE-AUDIT.md` step 8 and `docs/linux/interop-2026-11-26/contract.md` I-3 and A6: point to this plan.
- `android_launcher_plan.md` §1 (the Settings row) and §3 item 9 (Export and Import Library), and `linux_handheld_plan.md` §5 (the settings-store row): a note on the shared engine and the card file.
