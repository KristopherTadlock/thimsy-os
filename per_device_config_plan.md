# Per-device config plan: emulator sets per device, the theme on the card

Status: draft for review (2026-11-27), revised after the first review. Nothing here is built or run. It designs the owner's rethink of config portability (`docs/linux/DECISIONS.md` OA-46 to OA-51) for both builds. Line numbers are at `linux` 9cdec1c. Facts not checked on a device or in a tool's source are marked **(verify)**; estimates are marked *(guess)*.

## Requirement (owner, 2026-11-27)

- **OA-46:** emulator settings (RetroArch, PPSSPP, ScummVM and the rest of `emulator_config/`) and emulator picks (which emulator launches each system and each game) are per device. One card holds several devices' sets side by side. Thimsy uses the card's set for this device, else the device's local set. A per-device "SD card settings" switch: turning it on copies the local set to the card; when it is on and the card has no set for this device, the local set is copied to the card. The device always keeps a local mirror; with the switch on, the card's set wins and refreshes the mirror, and device changes go to both. A set is keyed by the unit's chip serial and the platform (coordinator, for audit).
- **OA-47:** Thimsy's own settings are local only. No `settings.ini`. `prefs.json` stays as the inventory. OA-35's seeding stays.
- **OA-48:** music playlists on the card (`Thimsy/playlists/<name>.m3u8`), no device mirror, card wins. `nano_music.json` becomes a scan cache.
- **OA-49:** box art, media, gamelists and collections on the card only, no mirror.
- **OA-50:** PortMaster and everything it installs is local.
- **OA-51:** the theme switches (XMB, ES-DE, Minima and the like, with their style switches) and each theme's settings are on the card, shared by every device, card first; the local value is used with no card. Aspect ratio stays local (coordinator). Where on the card it lives is settled here (§2.4).

## Conventions

- **Set:** one device's emulator settings files and picks.
- **Card set:** this device's set on the card, `emulator_config/devices/<id>/`.
- **Local set:** this device's set on its own storage. It is OA-46's mirror.
- **Live file:** the file an emulator actually reads (an Android app's private folder, a folder on the card the emulator was pointed at, or Linux's `/storage/.config/<emu>`).
- **Library card:** the card whose `ES-DE` folder Nano uses as the store (OA-25 chooses between two). The emulator sets use the card `usb_storage_plan.md` calls the Emulator Settings Drive where that setting exists, else the slot card (`emuSdSlotCard`, never a USB drive). On today's devices the two are the same card.
- **Theme card:** the card that holds `Thimsy/theme.json` (OA-51): the library card when the store is on a card, else the slot card (`emuSdSlotCard`). Store mode is opt-in on Android: it is off while `persist.gammaos.nano.esde.dir` is empty (`esdeStoreMode`, `NanoEsdeStore.cpp` 128-132), and the default store is internal (`/storage/emulated/0/ES-DE`, line 59). So a device with a card in and no store on it still has a theme card. On Linux it is the library card, which is always a card.
- **Parked set:** a local set that belongs to another id, kept beside the live one (§5.4).
- **AS** / **LO**: a step that changes Android-compiled code (an `android-surface:` commit, cherry-picked after the AOSP build), or one that touches only Linux, tools or docs.

## Where things stand

### Android (shipped)

- **Emulator folders on the card.** Move Emulator Files to microSD (`NanoSdMove.cpp` `emuMoveStart`, 1076) points each emulator's whole folder at `<card>/emulator_config/<emu>`: settings, saves and states together. RetroArch's folder is `persist.gammaos.retroarch.dir`, and its `retroarch.cfg` and `config/` (core options, overrides, remaps) live there (`NanoRaConfig.h` `raDirKeys`; launch config `raLaunchConfig`, mirrored by the framework's `NanoRetroArchConfig.java`). PPSSPP's memory stick holds `PSP/SYSTEM/*.ini`; Flycast's `emu.cfg`, DraStic's and drastic-nano's configs are in their folders too. `persist.gammaos.nano.emu_sd` says the files are on the card.
- **App-private settings mirrored.** `NanoEmuSync` keeps 18 items (Android preferences, ScummVM's `scummvm.ini`, Vita3K's `config.yml`, RetroArch autoconfig and the rest, `NanoEmuSync.cpp` `items`) in step with `<card>/emulator_config/<emu>/app/`, three ways per file with a base in `/data/system/nano_emu_sync.txt`; on a clash the card wins (`decide`). It runs at card adoption, every 10 s at the home screen and just before an emulator starts (`emuSettingsSyncLaunch`). A card new to the device with emulator files asks "Use the Card's / Keep This Device's" (`emuSdForeignDialog`).
- **So every Android device using a card shares one set today.** That is what OA-46 replaces.
- **Picks.** Per system: the launch fields of `/data/system/nano_systems.json`, and in store mode `ES-DE/nano/systems.json` on the card, whose copy wins (`esdeStoreAdoptSystems`, `NanoMenuXmb.cpp` 1123), plus ES-DE's `<alternativeEmulator>` (written by `esdeStoreSaveSystemEmus`, adopted by `esdeStoreApplySystemEmus`, `NanoMenuPS3EmuCatalog.cpp` 833-913). Per game: `/data/system/nano_game_emulators.json` outside store mode; in store mode `<altemulator>` or `ES-DE/nano/emulators.json` (`setGameEmulator`, 708; `esdeStoreSaveGameEmu`, `NanoEsdeStore.cpp` 1522).
- **Theme.** Local properties only (`persist.gammaos.nano.ndstheme`, `.esde.*`, `.ps3xmb.*` and the rest), read at start (`NanoMenu.cpp` 1535-1600) and by the theme engine (`NanoThemeEngine.cpp` 504-508). The ES-DE rows write through `esdeMenuApplyOption` (`NanoMenuEsdeMenu.cpp` 246); the XMB rows through the binding table (`NanoMenuPS3Menu.cpp` 9869 and the write at 12657-12667) and `applyThemeSetting` (12169). Defaults come from `mods/nano-theme.props` (OA-1, OA-35). Downloaded themes go to `/data/system/nano_esde_themes` (gap G14).
- **Thimsy settings read from the card.** In store mode, 12 scraper preferences (`persist.gammaos.scraper.*`, `kEsdeSettingProps`, `NanoEsdeStore.cpp` 1703-1723) are copied from the card's `es_settings.xml` into local properties (`esdeStoreApplySettings`, 1733-1757) and written back on a change (`esdeStoreSettingChanged`, 1771, called from `NanoMenuPS3Menu.cpp` 12667). `systems.json`'s shared fields are adopted from the card too (`esdeStoreAdoptSystems`). §2.6 decides both.

### Linux

- **Live config is on the system card's ext4** (`/storage/.config/retroarch/retroarch.cfg`, `Launcher.h` 81). RetroArch gets the card's shared folders through a per-launch append config (OA-21, `LIBRARY-FORMAT.md` 4.17). The PPSSPP and ScummVM merge layers (W7-INTEROP-11, -12) are designed, not built.
- **Wave 8 (`step/w8-emu`, merged into `linux-next`, uncompiled)** built OA-38: `nanoplat::Emulators` (`coreInstalled`, `appInstalled`, `imageDefaults`, 4c800fc); the image's defaults seeded once into `nano_emu_defaults.json` (ebc12c7); a launch whose pick cannot run starts with the image default, then the system's own emulator, then a custom system's first catalog emulator, for that launch only and never recorded (7712435, 9db53ae, 4aad6c0); a label for an emulator that cannot run kept as the pick (`esdeStoreLoadGameEmus`); the systems heal skipped in store mode (W8-EMU-20); `systems.linux.json` retired; `LIBRARY-FORMAT.md` rewritten for shared picks (f933326, 350fcb8, f303d13).
- **Single-card devices:** `/storage` is on the same physical card as the games partition, so the local set and Thimsy's local settings move with the card when it moves to another unit. §1 and §5.4 deal with that.

## 1. Device identity

**The id is the platform plus a hash of the unit's chip serial**, written `<platform>-<16 hex digits>`, lower case: `android-3f9a0c1e5b7d2a64`, `linux-9c1d0e7a42b35f86`.

- **Hash:** FNV-1a 64 of `thimsy-unit:` followed by the serial as read, printed as 16 lower-case hex digits. It is the hash Nano already uses for card files (`esdeTextHash`), so no new library. It keeps the raw serial off the card. It is not a secrecy guarantee: serials are short.
- **Platform:** `android` or `linux`. A dual-boot ARC-D therefore has two sets on one card, as OA-46 decided. The two sides' serial strings may differ (different loaders write them); nothing depends on them matching.
- **Label:** `device.json` beside the set (§2.2) holds the model's descriptor id and display name (`multi_device_plan.md` decision 19, the same on both builds), where the id came from, and dates. The user's own device name is not written (it stays on the device).

**How each platform reads the serial** (behind `nanoplat::DeviceIdentity::unitSerial()`, first good answer wins):

| Platform | Order | Notes |
|---|---|---|
| Android | `ro.serialno` (`ro.boot.serialno`, from the loader's `androidboot.serialno`); then `/sys/devices/soc0/serial_number` (Qualcomm); then a `Serial` line in `/proc/cpuinfo`; then the eMMC CID (`/sys/block/mmcblk0/device/cid`) | Unisoc loaders take the serial from the factory data; Rockchip's take vendor storage, else derive one from the chip id **(verify on the Cube and the ARC-D)**. The CID is the storage chip's, unique per unit, used only when no SoC serial is usable. |
| Linux, RK3566 and other Rockchip | the chip id from nvmem, read by thimsy-session as root and published as `/run/thimsy/unit-serial` (`rockchip-otp0` on RK3566/RK3568, `rockchip-efuse0` on RK3326, offsets per SoC **(verify)**); then `/proc/device-tree/serial-number` (U-Boot's `serial#`, NUL-terminated); then a `Serial` line in `/proc/cpuinfo` | The chip id comes first because it cannot move. U-Boot sets `serial#` only when the variable is empty, so if its environment is ever saved to the system card (env in MMC), `serial#` would follow the card to another unit **(verify per device where U-Boot keeps its environment)**. arm64 mainline kernels print no `Serial` line, so the last step only helps 32-bit kernels. |
| Linux, later families | that family's nvmem through thimsy-session first; then the device-tree node (Allwinner U-Boot sets `serial#` from the SID **(verify)**) | added with each family |

**A serial is refused** when it is empty, shorter than 6 characters, `unknown`, all one repeated character, or on a short list of factory placeholders (`0123456789ABCDEF`, `0000000000000000`, `1234567890`). The list is data, with a test.

**Waiting for the serial (Linux).** thimsy-session always writes `/run/thimsy/unit-serial`, empty when it found no chip id, so the file's presence means the answer is in. Nano waits for it (up to 15 s *(guess)* after start). Until then the id is pending: no set is synced, nothing is written to the card, and launches use the live files as they are. A random id is made only when every source of the platform has answered and none is usable, never because a source is not ready yet.

**Two units with the same serial.** A model whose units share one serial (a factory string, or a vendor-storage value cloned across units) would give one id to two units. To catch it, `device.json` records `fp`: FNV-1a 64 of a second hardware value from a different source than the serial: on Android the eMMC CID, or the Wi-Fi MAC when the CID was the serial; on Linux the Wi-Fi MAC, or the nvmem chip id when the serial came from elsewhere. Never anything stored on the system card **(verify on the Cube and the ARC-D which values each exposes, and that the MAC read is the factory one, not a randomised one)**. When the card holds a set for this id whose `fp` differs from this unit's, the two units collide: this unit takes the id `<platform>-` plus the hash of the serial and the fingerprint together, with `"source": "serial+fp"`, and offers §4.5's copy. The choice is kept in a local list, fingerprint hash to id (§3), so the id stays the same with the card out. With no readable fingerprint there is no check; Settings > About shows the id, so a clash can still be seen.

**With no usable serial**, the id is `<platform>-` plus 16 random hex digits, made once and kept locally (Android `/data/system/nano_device_id`, Linux `/storage/.config/thimsy/device-id`), with `"source": "random"` in `device.json`. Limits: a wiped Android device gets a new id (its old set stays on the card; §4.5 lets the user copy it); a Linux card with a random id that moves to another unit with no serial carries the id with it.

**The serial wins every start.** The id is recomputed from the serial at each start and the cached file is used only when no serial is readable. Reason: on single-card Linux, `/storage` moves with the card, so a cached id would follow the card to another unit and that unit would use the first unit's set. The fingerprint list is the only exception, and it is keyed by the fingerprint, so it never applies on another unit.

## 2. Card layout

### 2.1 Tree

```
<card>/
  ES-DE/                                   shared, card only (OA-49): ES-DE's own formats
    settings/es_settings.xml               ES-DE's keys; Nano also mirrors 4 theme keys (2.4)
    gamelists/  collections/  downloaded_media/  themes/  custom_systems/
    nano/systems.json                      shared system list; launch fields kept as read (2.3)
    nano/emulators.json                    legacy, read once at migration, never written again
    nano/names.json  index.json  format.json
  ROMs/<system>/                           games; same-core in-game saves (OA-21)
  bios/                                    BIOS drop folder
  emulator_config/
    card.json                              identity hints, aliases (unchanged)
    devices/<id>/                          one device's set (2.2)
      device.json                          label; written last, so it marks a complete set
      picks.json                           system and game picks
      <emu>/<rel>                          settings files, at the path they have in the emulator's folder
      <emu>/app/<rel>                      Android app-private settings (was <emu>/app/<rel>)
      .previous/                           the card set as it was before the last replace (4.2, 4.5)
    slot-kept/<set id or legacy>/<emu>/<rel>  slot files kept before a fill (5.6)
    retroarch/system/  cheats/             shared data, as today
    retroarch/linux/saves/  states/        Linux saves and states (OA-21), as today
    <emu>/...                              shared data (saves, memory cards, states) as today; Android's
                                           settings files here are now a live slot (5.6)
    <emu>/app/                             legacy shared mirror: read once at migration (6)
  Thimsy/
    theme.json                             the theme, shared; on the theme card (2.4)
    playlists/<name>.m3u8                  music playlists (OA-48); on the library's volume (2.5)
    emulators/*.json                       user emulator definitions to import (unchanged)
```

Nothing that exists moves or is deleted. New: `emulator_config/devices/`, `emulator_config/slot-kept/`, `Thimsy/theme.json`. `systems.linux.json` stays retired (W8-EMU-1).

### 2.2 A set

`device.json` (schema `linux/schema/device.schema.json`, new, strict):

```json
{ "version": 1, "id": "linux-9c1d0e7a42b35f86", "platform": "linux",
  "model": "anbernic-rg-arc-d", "name": "Anbernic RG ARC-D", "source": "serial",
  "fp": "5e0a9c41d2b7f318", "thimsy": "0.2", "first": "2026-12-01", "last": "2026-12-03" }
```

The local set's `device.json` has the same form. Its `id` names the set whose files are live (§5.4), and it adds `changed` (false until a file of the set is first changed after install, §4.2) and `migration` (§6.1).

`picks.json` (schema `linux/schema/picks.schema.json`, new, strict at the top, open per entry):

```json
{ "version": 1,
  "systems": { "psx": { "launchType": "libretro-core", "coreSo": "pcsx_rearmed_libretro_android.so",
                        "key": "core:pcsx_rearmed" } },
  "games":   { "n64/Mario Kart 64 (USA).z64": { "label": "Mupen64Plus-Next", "am": "",
                                                  "key": "core:mupen64plus_next" } } }
```

- `systems` is keyed by `systems.json`'s `id`; the fields are the launch fields' own names (`launchType`, `coreSo`, `package`, `launchArgs`, `intentTemplate`), which both builds use (`NanoPlatform.h`'s `Emulators` names emulators the Android way on every platform). `games` is keyed by the game key, form (c) of `LIBRARY-FORMAT.md` 2.2, with `emulators.json`'s fields. `key` is the portable key, informative only.
- **Local game picks use the same key plus the card id:** `<card id>/<system>/<rel>`, with the card id the sync base uses (§5.2), for a game under a card's `ROMs/`. The launch path converts the path apps open (`gameEmuKey`, `NanoMenuPS3EmuCatalog.cpp` 651-655) by stripping `/storage/<vol>/ROMs/`. Only entries whose card id is the set's card go to and from `picks.json`, with the card id stripped or added; so a card with a new volume id, or a second card (OA-25), neither duplicates nor drops an entry. Games elsewhere (internal storage, outside `ROMs/`) keep their path and stay local.
- **A system pick** is a system whose launch fields in `nano_systems.json` differ from its entry in `nano_emu_defaults.json` (W8). Only those are written to `picks.json`; a system with no entry uses the image default (§5.7).
- **Settings files per emulator** are data, one row per file or folder: the set path, the live path on each platform, and flags (`many`: not checked just before a launch; `skip`: subfolders left out). Android's table is `NanoEmuSync`'s `items()` plus the settings files now in the card folders; Linux's comes from the emulator manifest. A first cut:

| Emulator | In the set | Stays shared (not in the set) |
|---|---|---|
| RetroArch | `retroarch.cfg`; `config/` (core options, overrides, remaps); user-made `autoconfig/` profiles (Android only, as today) | `system/`, `cheats/`, saves, states, `.lpl` playlists, thumbnails, shaders, caches |
| PPSSPP | `PSP/SYSTEM/ppsspp.ini`, `controls.ini`, `<GAMEID>_ppsspp.ini`; Android `app/shared_prefs` | `PSP/SAVEDATA`, `PSP/PPSSPP_STATE`, `Cheats`, `TEXTURES`, `flash0/font` |
| ScummVM | `scummvm.ini` (Android `app/files/scummvm.ini`; Linux `/storage/.config/scummvm/scummvm.ini`) | `saves/` |
| Flycast (Android) | `emu.cfg`, `mappings/` | `data/` (BIOS, VMUs) |
| DraStic, drastic-nano (Android) | their config files and `app/shared_prefs` **(list to check per emulator)** | BIOS, saves, states, cheats |
| Dolphin, Azahar, ARMSX2, Vita3K, M64Plus FZ, hakuX, SeedlessDS, WatermelonDS (Android) | `app/` items as today, plus their config files on the card **(list to check per emulator)** | saves, memory cards, states, textures |

  Caches, logs, shader caches and anything over 16 MiB are never in a set.

  SeedlessDS's `files/SeedlessDS/user` item (`NanoEmuSync.cpp` 51) syncs the whole folder except `backup` and `savestates`, so it may hold in-game saves **(verify what it holds)**. It stays out of the set and keeps today's shared mirror until checked; then only its settings files move into the set, and its saves stay shared (OA-21).

### 2.3 Picks and ES-DE's files

- **Thimsy keeps picks only in its per-device sets.** From this change on, neither build writes ES-DE's `<altemulator>` or `<alternativeEmulator>` labels or `nano/emulators.json`, and neither adopts a label change afterwards. Existing labels are read once, at migration (§6), and then left as they are.
- **What the standalone ES-DE app sees:** the labels it or a shipped Thimsy build wrote last, unchanged. Its picks are its own and do not change Thimsy's; Thimsy's picks do not reach it. Its files stay valid, since nothing removes or rewrites a label.
- **`nano/systems.json`** stays the shared system list (folders, names, order, extensions, scan sources). Its launch fields stop being picks: a writer keeps each existing system's launch fields exactly as it read them, and writes this device's pick only for a system it adds (the format requires launch fields, and shipped builds use them). Reading it, Nano takes the shared fields and keeps this device's launch fields (§5.7). Why the shared fields stay on the card: §2.6.

### 2.4 The theme on the card (OA-51)

**Where:** both a Thimsy file and ES-DE's own keys.

- **`<theme card>/Thimsy/theme.json` is the truth.** It holds every theme key below, by the portable names `prefs.json`'s `ini` column already gives them, with values in the device store's own encoding (so no conversion):

  ```json
  { "version": 1,
    "theme": { "home.theme": "3", "esde_theme.set": "colorful-simplified-es-de",
               "esde_theme.color_scheme": "colorful-dark", "esde.settings_theme": "minima" } }
  ```

  Schema `linux/schema/theme.schema.json` (new). Unknown keys are kept. JSON, not INI: `settings.ini` and its dialect are dropped (OA-47), and Nano already reads and writes JSON (`NanoJson`) on both builds.
- **ES-DE's `es_settings.xml` gets four keys mirrored**, so the standalone ES-DE app shows the same theme: the set, `ThemeVariant`, `ThemeColorScheme`, `ThemeFontSize`. They go into the `es_settings.xml` of the store Nano uses, only when the chosen set exists in that store's `themes` (the rule `LIBRARY-FORMAT.md` 1.4 already has). This is a display copy for the ES-DE app under OA-51, not OA-32 Q4's portability write: `theme.json` is the truth. The bundled ES-DE app keeps its settings in `/sdcard/ES-DE` (`mods/37-esde.sh`), so the mirror reaches it only when that is the store **(verify which `es_settings.xml` the bundled ES-DE app reads)**. ES-DE 3.x names the set key `Theme`, older versions `ThemeSet`: the writer updates whichever the file has, and when it has neither, uses the name of the app version Thimsy bundles (`mods/37-esde.sh`, `VERSION="3.5.0"`) **(verify which name 3.5.0 writes)**. `ThemeAspectRatio` is never written (W7-INTEROP-9 stays). The other ES-DE keys Nano has counterparts for (`MenuColorScheme`, `MenuBlurBackground`, `ThemeTransitions`) are not mirrored in v1: their values differ from Nano's **(verify)**.
- **A theme edit made in the ES-DE app** is taken into `theme.json` at the next sweep only when all of these hold: `es_settings.xml` is newer than Nano's last mirror write; the ES-DE app is not running; one of the four keys differs from what Nano mirrored last and is not the value it mirrored before that; and the set exists on the card. Reason: ES-DE rewrites the whole file from memory whenever any of its settings is saved, so an app that read an older theme at start writes that theme back; without these checks Nano would take it and revert the theme on every device (owner question 4).
- **The theme keys (31).** The rule: a key that chooses which interface Nano shows, or how it looks, and does not depend on the screen or the buttons.

  | Group | `theme.json` key (`prefs.json` `ini`) | Device-store key (`persist.gammaos.nano.`...) |
  |---|---|---|
  | Interface | `home.theme`, `home.xmb`, `home.carousel`, `home.minima` | `ndstheme`, `ps3xmb`, `xmb_mode`, `minima` |
  | Look | `home.font`, `home.wallpaper_effect`, `home.wallpaper_dim` | `font`, `wallpaper`, `wp.scrim` |
  | XMB style | `xmb.theme`, `xmb.color`, `xmb.color_custom`, `xmb.background`, `xmb.font`, `xmb.day_night`, `xmb.wave`, `xmb.wave_explicit` | `ps3xmb.theme`, `.color`, `.color_custom`, `.bg`, `.font`, `.daynight`, `.wave`, `.wave_explicit` |
  | DSi style | `dsi.dark`, `dsi.effect`, `dsi.tile_icon`, `dsi.rom_title` | `nds.dark`, `nds.effect`, `nds.tileicon`, `nds.romtitle` |
  | Minima style | `minima.background`, `minima.long_names` | `minima.bg`, `minima.namescroll` |
  | ES-DE theme | `esde_theme.set`, `esde_theme.variant`, `esde_theme.color_scheme`, `esde_theme.font_size` | `esde.themeset`, `.variant`, `.colorscheme`, `.fontsize` |
  | ES-DE home | `esde.settings_theme`, `esde.menu_color_scheme`, `esde.menu_blur`, `esde.transition`, `esde.clock`, `esde.variant_triggers` | `esde.settingstheme`, `.menucolorscheme`, `.menublur`, `.transition`, `.clock`, `.variant_triggers` |

- **Kept local:** the aspect ratio (`esde.aspectratio`) and the other screen-dependent keys (`ndstheme.stack`, `ndstheme.gap`, `ps3xmb.uiscale`, `fontscale`, `ps3xmb.halfres.*`); the two-screen keys (`ps3xmb.bottomclock`, `.bottomclock.fps`, `.controlcenter`); sounds (`nav_sounds`, `boot_sound`, `nds.ambiance`, `esde.videoaudio`); `battpct`, `boot_to`, `esde.themes_url`; custom wallpaper images (`wp.*.top`, `wp.*.bottom`) unless owner question 3 says otherwise.
- `prefs.json` gains a `card` field, `"theme"` on these 31 rows; `test_prefs.py` checks the list against the code's table.

### 2.5 What else is shared, and how

| Data | Where | Rule |
|---|---|---|
| Theme | `Thimsy/theme.json` on the theme card, mirror in `es_settings.xml` | card first; local value with no card (§4.7) |
| Playlists | `Thimsy/playlists/*.m3u8` on the library's volume: the library card, else internal storage (Android `/storage/emulated/0`) when the library is there or no card is in; Linux always has a library card | no mirror; the card's win while it is in (OA-48, `LIBRARY-FORMAT.md` 4.22) |
| Gamelists, collections, media, downloaded themes | `ES-DE/` | card only (OA-49); downloads go to `<theme card>/ES-DE/themes` and existing ones are copied there at migration (G14, §6.1 step 8), so a theme choice finds its set on any device |
| `systems.json`'s shared fields | `ES-DE/nano/systems.json` | library data, card wins, as today (§2.6) |
| Scraper preferences | local properties | local only (OA-47, §2.6) |
| BIOS, in-game saves | `bios/`, `emulator_config/<emu>/...`, beside the ROMs | shared where the core matches (OA-21, unchanged) |
| Save states | the emulator folders, `linux/states/` | per platform (OA-21, unchanged); not per device |

### 2.6 Thimsy data the card holds today

OA-47 makes Thimsy's settings local. Two existing paths take Thimsy data from the card; each is decided here.

- **The 12 scraper preferences** (`kEsdeSettingProps`): they choose how Nano's scraper works, so they are Thimsy settings and local. Recommended: `esdeStoreApplySettings` stops copying them from `es_settings.xml`, and `esdeStoreSettingChanged` stops writing them, so the card keeps whatever the ES-DE app wrote last. Its `ROMDirectory` fill-in stays (that is ES-DE's own data). `LIBRARY-FORMAT.md`'s list of them (157) goes. Owner question 14.
- **`systems.json`'s shared fields** (`enabled`, `order`, `displayName`, `shortname`, `romDir`, `scanSources`, `disabledDefaultFolders`, `extensions`, `icon`, `scraper.override`, `scraper.platform`): recommended, library data under OA-49, on the card only, card wins, adopted as today (`esdeStoreAdoptSystems`, `NanoMenuXmb.cpp` 1123). Reason: they describe this card's library (which folders hold which system, how the systems are named and sorted, the custom systems the card adds); they mean nothing without the card, and shipped builds already share them. The launch fields are not shared (§2.3). Owner question 15.

## 3. Local storage

| | Android | Linux |
|---|---|---|
| Local set root (`nanoplat::DeviceStore::localSetDir()`) | `/data/system/nano_emu_set/` | `/storage/.config/thimsy/emu-set/` |
| Its `device.json`, sync state | `nano_emu_set/device.json`; `/data/system/nano_emu_sync.txt` (today's base, new key form §5.2) | `emu-set/device.json`, `emu-set/sync.txt` |
| Parked sets (§5.4) | `nano_emu_set/parked/<id>/` | `emu-set/parked/<id>/` |
| Local picks | `/data/system/nano_systems.json` launch fields, `/data/system/nano_game_emulators.json` (game keys §2.2) | the same files under the `/data` link (`/storage/.config/thimsy/data/system/`) |
| Image default emulators | `nano_emu_defaults.json` (W8, kept) | the same |
| Local copies of settings files | app-private files: the app's own folder (the live file is the local copy); files whose live path is on the card (§5.6): `nano_emu_set/<emu>/<rel>`; with the emulator folders on internal storage (`emu_sd` off), those folders | the live files themselves (`/storage/.config/<emu>`, `/storage/.local/share/<emu>`) |
| Thimsy settings (OA-47) | `persist.*` properties | ConfigStore, `/storage/.config/thimsy/persist.props` |
| Local theme value | the same theme properties | the same keys in ConfigStore |
| The device's own theme while a card's is shown (§4.7) | `/data/system/nano_theme_local.json` | `/storage/.config/thimsy/theme-local.json` |
| Theme lock (§4.7) | `/data/system/nano_theme.lock` | `/storage/.config/thimsy/theme.lock` |
| Device id cache (random ids only) | `/data/system/nano_device_id` | `/storage/.config/thimsy/device-id` |
| Fingerprint ids (§1) | `/data/system/nano_device_ids.txt` | `/storage/.config/thimsy/device-ids.txt` |
| PortMaster (OA-50) | none | `/storage` (unchanged by this plan) |

Every path in this table comes from `nanoplat::DeviceStore` (§8.1), so the shared code holds none of them (OA-4).

On single-card Linux the whole Linux column is on the card that also holds the games partition. That is fine: the per-device sets on the games partition are what let each unit get its own settings back (§5.4).

## 4. The "SD card settings" switch (emulator sets)

### 4.1 Key and states

`persist.gammaos.nano.emu_card_sets`, class D (local), in `prefs.json`. Values: unset, `1`, `0`.

| State | Meaning |
|---|---|
| Off (`0`) | Only the local set is used. The card set, if any, is neither read nor written. |
| On, in use | The card set exists and is synced with the local set; the card wins. |
| On, waiting | On, but no card, a read-only card, or a card set Nano cannot read: the local set is used and the card is retried at each sweep. Settings shows why. |

OA-46's lookup says: "Thimsy uses the card's set that matches this device; when there is none, it uses the device's local set." The plan reads it with the Mirror rule, which gives the card priority "when the switch is on": with the switch off, a card set for this id is ignored, not used read-only. Reason: a read-only card set would take edits that are never written back, so each edit would vanish at the next start while the card is in. Owner question 16.

The row: Settings > Game Settings > **SD Card Settings**, with a status line ("Using this card's settings for this device", "No card", "Card is read-only", "Card settings unreadable: using this device's").

### 4.2 Turning it on

1. The card has **no set for this id**: the local set is copied to the card: each settings file, then `picks.json`, then `device.json` last. A copy that stops part-way has no `device.json`; it is a partial set and the copy resumes at the next sweep (§5.5).
2. The card **already has a set for this id** (made before the switch was last turned off, or before a reinstall): Nano asks "This card has emulator settings for this device from <date>." with "Use the card's" and "Use this device's". The first choice is "Use the card's" when the local set is unchanged since install (local `device.json` `changed` is false: a reinstall or a wipe, the usual reason the card already has this id's set), else "Use this device's". The replaced copy is kept once: a card set in `devices/<id>/.previous/`, a local set in `<local set>/previous/`, each overwriting the copy kept before. Owner question 6.
3. The switch is then on, in use.

### 4.3 Turning it off

The sync stops. The card set stays on the card as it is (a stale copy). Nothing is deleted on either side. Turning it on again is §4.2 case 2.

### 4.4 Already on, card has no set for this device

A card new to this device (from another device, a fresh card, a card from before this change): the local set is copied to the card as in §4.2 case 1, with no question. Other devices' sets are never read or changed.

### 4.5 Copying another device's set

Settings > Game Settings > **Copy settings from another device** lists the card's sets of the same platform by their label (model, last used). Choosing one replaces this device's local set (and card set, if the switch is on) after a confirm. Before the replace, the local set is kept in `<local set>/previous/` and the card set in `devices/<id>/.previous/`, so the copy can be undone. The list also shows the slot files kept from shipped builds (`slot-kept/legacy/`, §5.6) as "Old shared settings". Never automatic. It covers a new unit of the same model, and a device whose id changed (a random id after a wipe). Owner question 7.

### 4.6 First-boot defaults

- **Linux:** on.
- **Android, new install:** on when setup's "Keep emulator files on the microSD card" is chosen (today it sets `emu_sd`), else off.
- **Android, update from a shipped build:** on when `emu_sd` is `1`, else off (§6).

### 4.7 The theme needs no switch

OA-51's theme always applies when a theme card is present (recommended; owner question 2). With the store on internal storage, the theme card is the slot card; with no card in, the local value is used, wherever the store is. There is nothing per device to protect, and a switch would bring back the "Use this card's settings?" question OA-32 had.

How the local value and the card's relate (recommended, owner question 1): **they are separate values.**

- Card in: the card's `theme.json` is shown. Before its values go into the live theme keys, the device's own values are saved to the local theme file (§3), once. Theme edits go to the live keys and to `theme.json`; the device's own values are not touched.
- Card out: the device's own values are put back from the local theme file, and the file is removed.
- No `theme.json` on the card yet: it is written from the device's current values (the card is seeded, as with emulator sets in §4.4).
- A key missing from `theme.json` (an older writer, a PC edit) uses the device's own value; it is written to the card the next time the theme changes.
- At start, a leftover local theme file with no card present is applied back first, so a power cut while a card's theme was shown leaves nothing behind.
- **One writer.** The home and the overlay are two Nano processes, so both take `DeviceStore::themeLock()` (a file lock, like `nano_emu_sd.lock`) around apply, restore and every `theme.json` write; only the home process applies and restores, and the overlay writes edits under the lock. The local theme file is written atomically and `fsync`ed (§5.5) before the first live key is overwritten, and removed only after every key is put back.

The live keys stay the ones every reader uses today (both Nano processes read them), so no reader changes. When the `Prefs` facade (OA-4 step 8) lands, this becomes a card layer in it instead.

## 5. Sync rules

### 5.1 Card wins

- **Emulator sets (switch on):** three ways per file, as `emusync::decide` does today: changed only on the card, the card's copy goes to the device; changed only on the device, the device's goes to the card; changed on both, or never synced with this card set, the card's wins. What looks like a mass deletion on one side is taken as never synced (today's rule).
- **Picks:** per entry, not per file. The base is a copy of `picks.json` as last synced, kept in the local set (`picks.synced.json`). An entry changed only on the card goes to the local pick files; one changed only locally goes to `picks.json`; one changed on both takes the card's.
- **Theme:** always the card's while it is in (§4.7).
- The device copy that loses a clash is kept once, as `<local set>/conflicts/<emu>/<rel>`, overwriting the previous one (owner question 11).

### 5.2 Writes

- **Nano's own changes** (a pick, the hotkey provisioning in `retroarch.cfg`, BIOS paths written into an emulator's config, a theme row) go to the local copy at once and to the card at once when the switch is on and the card is in.
- **An emulator's own changes** during a game come back after it exits: the after-run sync (§5.6) for that emulator, and the sweep.
- **Sync state** stays one line per file, with the set in the key: `<card id>\x1f<set id>\x1f<emu>\x1f<rel>`, then the device side's stamp, the card side's stamp and the card copy's hash (`emusync::baseKey`, `baseSave`). The hash is what tells a FAT time change from an edit.
- **When:** at card adoption, before and after each emulator run, every 10 s at the home screen (today's `emuSdTick` timing), and before sleep and shutdown. Never into an emulator that is running (today's rule).

### 5.3 PC edits

A file edited on the card from a PC changes its hash, so it counts as a card change and wins. `picks.json` and `theme.json` edits are read at the next sweep (`esdeStoreSweep` watches both, `esdeNoteWrite` keeps Nano's own writes from counting). A `picks.json` or `theme.json` that does not parse is not used and not overwritten (`LIBRARY-FORMAT.md` 1.2); the local values are used and Settings says so. A PC edit to an Android slot file (`emulator_config/retroarch/retroarch.cfg`, the path users know) goes to the set the slot's marker names (§5.6); to change one device's settings from a PC, edit `devices/<id>/...`.

### 5.4 A card from another device, card removal, two cards

- **Another device's card:** no set for this id, so §4.4: the local set is copied there. The card's theme is shown (OA-51), and the device's own theme comes back when the card leaves.
- **Single-card Linux, the card moved to another unit:** `/storage` moves with the card, so its live files hold the last unit's settings, and the local `device.json`'s `id` says whose. At start, before any three-way pass, when that id is not this unit's:
  1. The live files and local picks are parked as that id's set (`emu-set/parked/<that id>/`). Its sync base stays as it is (keyed by set id, §5.2).
  2. If this id has a parked set, it is put back into the live files. Else, with the switch on and a set for this id on the card, that card set is taken whole, with no base and no three-way pass. Else this unit starts from a copy of the parked files as its own set.
  3. `device.json` is rewritten with this id, last; then the normal sync runs.

  So unit A's settings come back on A however long the card spent in unit B; B's never reach A's card set, and they stay parked (and in B's card set) for B's next visit. With the switch off, the units are still parked and swapped; nothing reaches the card. The same check runs on Android, where it fires only when the id changes (a random id after a wipe, a collision id, §1).
- **Card removal:** the local set is complete, so nothing is lost; launches use it. Edits made with the card out are local; at the next insertion the three-way rule sends them to the card unless the card's copy changed too (then the card wins and the device's goes to `conflicts/`).
- **Two cards (OA-25):** only the chosen card's sets and theme are used. The other card is not read or written.

### 5.5 Partial and damaged files, atomic writes

- Every file a sync writes, on the card or on the device, is written to a temporary name in the same folder, `fsync`ed, renamed over the old one, and the folder `fsync`ed (`LIBRARY-FORMAT.md` 1.2). App-private files keep `sdWriteAppFile`'s owner, mode and SELinux label.
- A set folder without `device.json` is partial: a copy that did not finish. When this device made it, the copy resumes; it is never used as the truth until `device.json` is there.
- A settings file is copied as bytes; Nano does not parse it. Files over 16 MiB and the temporary names are skipped. A missing or empty local copy is never written over a live file (§5.6).
- A card that turns read-only or fails a write: the switch goes to "On, waiting" and the local set is used.

### 5.6 Emulators that read their own storage

Before an emulator starts (`emuSettingsSyncLaunch` on Android, the launch path on Linux):

1. With the switch on and the card in, that emulator's files are synced card to local (§5.1).
2. Its live files are brought up to date from the local copy:
   - **Live file is the local copy** (Android app-private files, all Linux files): nothing more to do.
   - **Live file on the card** (Android only: `retroarch.cfg`, `config/`, PPSSPP's `PSP/SYSTEM`, Flycast's `emu.cfg` and the others in the emulator's own folder on the card, since that is the only place the emulator reads them): the folder is a **live slot**. A marker `emulator_config/<emu>/.thimsy-slot` records the set id, each file's hash as last written or taken, and `restore` when the slot's earlier files are owed back (step 5). Before the fill, each slot file is checked against the marker:

     | Slot file | Marker | Before the fill |
     |---|---|---|
     | as the marker records | this set | nothing |
     | changed | this set | taken into the local set, then the card set: a change made outside the after-run step (a run cut short, the ES-DE app or the launcher, a PC edit) |
     | changed | another set | kept as `slot-kept/<that id>/<emu>/<rel>`; that device takes it as a card-side change at its next sync |
     | any | none | kept as `slot-kept/legacy/<emu>/<rel>` (a shipped build's or a hand-made file), and `restore` set |

     Then the local copy is written in. A file whose local copy is missing or empty is not filled: the slot's file is left as it is.
   - **Emulator folders on internal storage** (`emu_sd` off): they are local copies, not slots.
3. Folder keys are set from where the card is now (RetroArch's `raDirKeys` and `raHealConfig`, then the hotkey provisioning), so a stored path never points at a card that is out.

After it exits (Android: when Nano is in front again; Linux: `pollChild`):

4. Live files changed during the run are copied to the local copy, then to the card set. For a slot, only when the marker names this device's set; a slot another device or a shipped build wrote last is never taken as this device's settings.
5. When the marker has `restore`, the slot's earlier files are put back from `slot-kept/legacy/` and the marker is removed, so a shipped build using that card finds its own settings again.

The slot check (step 2's table, then step 5 when owed) also runs at start, at card adoption and at each sweep, for every emulator that is not running. So a run cut short by a crash, a power cut or a flat battery is taken in and put back at the next start, and no slot is overwritten without the check.

RetroArch on Android keeps its launch config where the framework expects it (`raLaunchConfig`), so the framework's `NanoRetroArchConfig.java` does not change. A RetroArch started outside Nano (the ES-DE app) reads whatever the slot holds: this device's set on a card it migrated with, the earlier files on a card with no marker. Moving RetroArch's live config to device storage instead is owner question 9.

### 5.7 Which emulator a launch uses

Per launch, first that can run here (`nanoplat::Emulators`): the game's pick in this device's set; the system's pick in this device's set; the image default (W8's seeded store); the system's own emulator (for a game pick that cannot run); a custom system's first catalog emulator (W8-EMU-20 to -22); else "Emulator not installed". A fallback is never recorded (W8, kept). `systems.json`'s launch fields are read only when this device has no pick and no image default for a system (a custom system first seen on this card).

## 6. Migration and the compatibility floor

### 6.1 Android, updating from a shipped build

Run once, on the first start of the build that carries this change. Nothing is deleted.

1. The device id is computed; the local set root and its `device.json` are made.
2. The switch takes its first value from `emu_sd` (§4.6).
3. **Picks:** the local pick files stay as they are, except that `nano_game_emulators.json`'s keys for games under a card's `ROMs/` are rewritten to §2.2's `<card id>/<system>/<rel>`. In store mode, the picks the device actually launches today are folded into them: each game's `<altemulator>`, else its `emulators.json` item, as `esdeStoreLoadGameEmus` resolves them, goes into `nano_game_emulators.json`; each system's `<alternativeEmulator>` adopted today is already in `nano_systems.json`'s launch fields. Labels for emulators that cannot run here are not folded in.
4. **Settings files:** taken only while the card named by `kEmuSdCardProp` (the card the device adopted, `NanoSdMove.cpp` 1997-1998) is in. Its slot files become the local copies, and each slot gets a marker naming this set. Until then the local `device.json` has `"migration": "slots-pending"`: no slot is filled, the card set stays partial (no `device.json`, §5.5), and launches use the slots as they are, as shipped builds do. A different card in the slot gives nothing: its files are not taken and its slots are not filled. With the emulator folders on internal storage nothing is pending. App-private files are already local.
5. **Switch on, card in, step 4 done:** §4.4 copies the local set to the card.
6. **Theme:** the live theme keys are this device's own values. If the theme card has no `theme.json`, it is written from them. If it has one (another device got there first), it is shown, and the device's own values go to the local theme file. A one-line notice says the theme came from the card.
7. **Playlists:** user playlists in `nano_music.json` are written to `Thimsy/playlists/` on the library's volume (§2.5). A name already taken there is written as `<name> (<model name>).m3u8`. `nano_music.json` keeps each until its file is written, then holds only the scan cache.
8. **Downloaded themes:** each set in `/data/system/nano_esde_themes` (`NanoMenuEsdeMenu.cpp` 197) that the theme card's `ES-DE/themes` lacks is copied there (copied, not moved; skipped when the card lacks room), so a `theme.json` naming it finds it on every device (OA-37). The theme list also reads `<theme card>/ES-DE/themes` when the store is elsewhere.

A device with no settings of its own (a fresh install) meeting a card with legacy shared files (`<emu>/app/`, settings in the card folders) and no set for its id is asked once: "This card has emulator settings from before. Use them on this device?" Yes copies them in as this device's set. This replaces `emuSdForeignDialog` and keeps today's restore-after-reinstall for old cards.

### 6.2 Linux

No Linux build has shipped, so devices need no migration. `linux-next`'s shared-pick code is reworked before it merges (§7). Linux never imports Android's settings files or picks (different emulators). On a card from Android, Linux's set starts from the image defaults.

### 6.3 The floor and older builds

- **Additive only.** `devices/`, `theme.json` and the playlists are new files and folders. `format.json` stays at major 1. Shipped builds ignore them: they leave unknown folders alone, and `repoint` rewrites only absent card ids followed by `/emulator_config`, which is right for this card anyway.
- **A card used by both a shipped build and a new build:** the shipped build keeps using and writing the shared legacy files (`<emu>/app/`) and labels; the new build does not read them after migration, so such a change does not reach the new one (owner question 12). The slots are different, since there they are the shipped build's only copy of its RetroArch and PPSSPP settings: on the card the new build migrated with, the slots carry its marker, so a shipped build's edit there is taken into the migrated device's set, and the two keep sharing those files as before; on any other card the slot has no marker, so the shipped build's files are kept and put back after each run (§5.6). No setting is lost either way.
- **The floor** (Q12) is the first Android release with this change. From it, Android behaves as §2 to §5 say. Linux has the behaviour from its first build.
- Legacy files are never cleaned up automatically. A later "Remove old shared settings" action can be considered once every device the owner uses is past the floor.

## 7. What this replaces

| Earlier decision | Now |
|---|---|
| **OA-38, `step/w8-emu`** (picks shared, last pick wins) | Picks are per device (§2.3, §5.7). Kept from W8: `nanoplat::Emulators` (4c800fc); seeding image defaults (ebc12c7); the launch fallback chain and "a fallback is never recorded" (7712435, 9db53ae, 4aad6c0's CatalogFirst). Reverted: Linux writing labels and `emulators.json`; keeping an unrunnable label as a pick (`esdeStoreLoadGameEmus`'s `runnableOnly=false` call); the store-mode heal skip (W8-EMU-20): the heal repairs local picks again and never writes `systems.json`'s launch fields. `LIBRARY-FORMAT.md` f933326, 350fcb8 and f303d13 are rewritten. Done on `linux-next` before it merges into `linux`, as OA-46 says. |
| **OA-26, OA-39** (PPSSPP and ScummVM settings shared, per-platform override layers, a shared `scummvm/scummvm.ini`) | Settings are per device. No override layers, no shared `scummvm.ini`; W7-INTEROP-11 and -12 are withdrawn (never built). `<emu>/linux/` keeps only saves and states. PPSSPP's `SAVEDATA` stays shared (OA-21), bound into Linux's local memory stick at launch (`LIBRARY-FORMAT.md` 4.18 step 2). |
| **OA-32, OA-20's card file** (`Thimsy/settings.ini`) | Dropped (OA-47). `LIBRARY-FORMAT.md` 4.21 goes. `portable_settings_plan.md` is superseded except its phase 0 inventory (`prefs.json`). Classes P, M, A and L no longer put anything on a card; the new `card` field marks the theme keys. |
| **OA-32 Q4** (theme in settings.ini, mirrored to ES-DE) | The theme in `Thimsy/theme.json`, mirrored to ES-DE's four keys (§2.4). |
| **OA-23, OA-40** (playlists travel, device file kept as a cache) | OA-48: on the card only, no mirror, card wins. `nano_music.json` is the scan cache. |
| **OA-50** | PortMaster is local; not in any set. |
| **OA-35** (seeded defaults) | Unchanged and local. The first card a device uses gets its theme from the device's seeded values (§4.7, §6.1 step 6). Seeded image default emulators (W8) are the fallback, not picks. |
| **OA-37** (a missing theme falls back) | Unchanged. A card's `theme.json` can name a set that is not on this device (a downloaded set kept on another card or on another device): the default is shown and the stored choice kept. Downloads going to `<card>/ES-DE/themes` (G14) makes this rare. |
| **OA-21** (saves) | Unchanged. SeedlessDS's `user` folder stays shared until checked (§2.2). |
| **Scraper preferences in `es_settings.xml`** (store mode) | Local only: not adopted from or written to the card (§2.6, owner question 14). |
| **`systems.json`'s shared fields** | Library data under OA-49, adopted from the card as today (§2.6, owner question 15). |
| **G14** (downloaded themes on the device) | Downloads go to the theme card; existing sets are copied there (§6.1 step 8). |

## 8. Code impact

AS marks Android-compiled code. Every AS change is an `android-surface:` commit, verified as a move where it is one (H.3), cherry-picked to nano-144 after the AOSP build and the owner's smoke test. Nothing is compiled until the next build window (OA-36).

### 8.1 Platform interfaces (OA-4)

New in `nano/NanoPlatform.h` (AS, header), each with firmware, Linux and host implementations:

- **`DeviceIdentity`:** `unitSerial()` (raw, may be empty), `serialReady()`, `fingerprint()` (§1), `modelId()`, `modelName()`, `platform()`. Firmware: `platform/firmware/DeviceIdentity.cpp` (AS) reads §1's Android order. Linux: `platform/linux/DeviceIdentity.cpp` (LO) reads `/run/thimsy/unit-serial`, then the device tree. Host: `platform/host/FakeDeviceIdentity.cpp`.
- **`DeviceStore`:** every per-platform path this plan adds (§3's table): `localSetDir()`, `parkedSetDir(id)`, `syncStateFile()`, `localPickFiles()`, `localThemeFile()`, `themeLock()`, `deviceIdCache()`, `deviceIdList()`. Firmware `platform/firmware/DeviceStore.cpp` (AS), Linux `platform/linux/DeviceStore.cpp` (LO), host `FakeDeviceStore.cpp`. `NanoDeviceId`, `NanoEmuSets` and `NanoThemeCard` take every path from it.
- **`EmulatorFiles`:** `settingsItems()` (§2.2's table, with live paths), `readLive`, `writeLive` (the app stopped first and its owner and label kept on Android; plain atomic writes on Linux), `emulatorRunning(emu)`. Firmware: `platform/firmware/EmulatorFiles.cpp` (AS), taking `NanoEmuSync`'s `items()` and `NanoSdMove.cpp`'s `sdWriteAppFile`, `sdPkgRunning`, `sdStopIfRunning` as moves. Linux: `platform/linux/EmulatorFiles.cpp` (LO), from the emulator manifest. Host: `FakeEmulatorFiles.cpp`.
- `Emulators` (W8) stays as it is.

### 8.2 Shared code

| File | Change | Kind |
|---|---|---|
| `nano/NanoDeviceId.cpp/.h` (new) | id from platform and serial, the refusal list, the pending state, the fingerprint and collision ids, the random fallback and its cache | AS (new file) |
| `nano/NanoEmuSets.cpp/.h` (new) | set paths, `device.json` read and write, the owner check and parked sets, the switch states, turning on and off, `.previous` copies, partial sets, `picks.json` and its per-entry merge and key conversion, slot markers and the slot check, the migration's pending state | AS (new file) |
| `nano/NanoEmuSync.cpp/.h` | `Item` gains its live kind (app-private, slot, local); `cardPath` goes to `devices/<id>/<emu>/[app/]<rel>`; `baseKey` gains the set id; `items()` moves to the platform; `repoint` also walks `devices/` | AS |
| `nano/NanoSdMove.cpp` | `emuSettingsSync` (target set, slot steps), `emuSettingsSyncLaunch` plus a new after-run sync, the slot check at start, adoption and each sweep, `emuSdTick` (switch states instead of `emu_sd` adoption), `emuSdForeign`, `emuSdForeignDialog`, `emuSdForeignChoice` replaced by §6.1's legacy question, `emuSdAdoptBegin`/`Done`, `emuSdMode` (the move and the switch separated), `emuMoveStart` (moves shared data; settings files become slot files) | AS, the largest |
| `nano/NanoRaConfig.cpp/.h` | RetroArch's slot file list; `raHealConfig` after a slot is filled | AS |
| `nano/NanoHotkeys.cpp` | provisioning writes the local copy, then the slot | AS |
| `nano/NanoBiosWorker.cpp`, `NanoBiosInstall.cpp` | emulator configs they write go through the set write path **(verify which files)** | AS |
| `nano/NanoMenuPS3EmuCatalog.cpp` | `setGameEmulator` writes local and card picks, never `esdeStoreSaveGameEmu`; local game keys in §2.2's form; `loadGameEmulators` merges the card's; `esdeStoreSaveSystemEmus` and `esdeStoreApplySystemEmus` stop writing and adopting labels; W8's fallback code kept | AS |
| `nano/NanoEsdeStore.cpp` | `esdeStoreLoadGameEmus` and `esdeStoreSaveGameEmu` used only by the migration; `esdeStoreApplySettings` and `esdeStoreSettingChanged` drop the 12 scraper keys (§2.6); the sweep (`esdeStoreSweepBuild`, `esdeStoreSweep`) watches `theme.json` and this device's `picks.json`; the theme mirror through `esdeSettingsSet` | AS |
| `nano/NanoMenuXmb.cpp` | `esdeStoreAdoptSystems` adopts shared fields only; `esdeSystemsJson`/`esdeStoreSaveSystems` keep launch fields as read; `systemsHealTick` without W8's store-mode skip; `loadSystemsConfig` applies picks | AS |
| `nano/NanoThemeCard.cpp/.h` (new) | the 31-key table, the theme card, `theme.json`, the local theme file, the lock, apply and restore, seeding, the ES-DE mirror and the adoption of ES-DE app edits | AS (new file) |
| `nano/NanoMenuEsdeMenu.cpp` | `esdeMenuApplyOption` through `NanoThemeCard` (aspect ratio stays local); theme downloads to `<theme card>/ES-DE/themes` and the migration copy (G14); the theme list (`esdeMenuEnumerateInstalled`, 194) also reads that folder | AS |
| `nano/NanoMenuPS3Menu.cpp` | the bound-row write (12657-12667) and `applyThemeSetting` (12169) through `NanoThemeCard` for the 31 keys; the Home Theme live switch (12690) reused when a card's theme arrives; the new Game Settings rows | AS |
| `nano/NanoMenu.cpp` | the start-up theme latch (1535-1600) after `NanoThemeCard` has applied the card's or restored the device's values; a card that mounts later takes the live switch | AS |
| `nano/NanoMenuMusic.cpp` | `loadMusicConfig`/`saveMusicConfig` (107-241): playlists from and to `Thimsy/playlists/` on the library's volume, clash names, `nano_music.json` as the scan cache | AS |
| `nano/NanoMenu.h` | members and declarations for the above | AS |

### 8.3 Linux only

- `nano/platform/linux/Launcher.cpp`: no sync code of its own; the shared launch path calls the before and after syncs on both platforms. `argvFor` keeps `/storage/.config/retroarch/retroarch.cfg`. LO.
- `nano/platform/linux/` settings dispatcher (W6-caps keyed table): the two new rows. LO.
- `linux/src/thimsy-session`: always publishes `/run/thimsy/unit-serial` at start, from nvmem, empty when there is no chip id (§1). LO.

### 8.4 Data, schemas, docs

- `linux/schema/device.schema.json`, `picks.schema.json`, `theme.schema.json` (new); fixtures in `linux/tests/fixtures/card/` (a card with two Android sets and one Linux set, a partial set, a damaged `picks.json`, a `theme.json` with an unknown key). LO.
- `tools/nano-tree/prefs.json`, `nanotree/prefs.py`, `tests/test_prefs.py`: the `card` field; rows for `emu_card_sets` (D). LO.
- `LIBRARY-FORMAT.md`: section 3's tree; 4.1's theme keys; 4.8 to 4.10 (launch fields kept as read, labels and `emulators.json` legacy); 4.15 to 4.19 rewritten for per-device sets; 4.21 removed; 4.22 per OA-48; new sections for `devices/<id>/`, `picks.json` and `theme.json`; sections 5 to 10. `docs/linux/INTEROP-AUDIT.md`, `linux_handheld_plan.md` §7.2, §7.5, §7.6 and `portable_settings_plan.md`: a superseded note. `docs/linux/IMPLEMENTATION.md`: the new interfaces. `TODO.md`: a link to this plan. LO.

### 8.5 Tests

- **Unit (host):** `NanoDeviceId_test` (hash form, refused serials, random fallback, serial wins over cache, pending while `unit-serial` is missing and no random id meanwhile, a fingerprint clash gives the collision id and keeps it with the card out); `NanoEmuSets_test` (on, off, on with an old set and the default choice with an unchanged local set, `.previous` copies on every replace, partial set resumed, damaged `picks.json`, per-entry merge, game keys converted for a new volume id and a second card, system picks only where they differ from the image defaults, owner id differs: parked and swapped, migration pending with the adopted card out and with another card in: nothing taken, no slot filled); slot check (unchanged, changed with this set's marker taken in, another set's marker kept in `slot-kept/<id>`, no marker kept and put back after the run, missing or empty local copy never filled, a run cut short recovered at start); `NanoEmuSync_test` (new keys, card paths under `devices/`); `NanoThemeCard_test` (seed, show and restore, power cut with the local file left, local file written before the keys, the lock, missing keys, the theme card with store mode off and with the store on internal storage, mirror only when the set is in the store, aspect ratio never written, `Theme` and `ThemeSet` spellings, an ES-DE app edit adopted, not adopted while the app runs or when it is the value mirrored before last).
- **Platform:** `platform/linux/tests/deviceidentity_test.cpp` (fake device tree, NUL byte, missing node, `/run` fallback); a firmware unit test of the property order with a fake property source.
- **Host scenarios:** two fake devices sharing one card image (each keeps its own RetroArch config and picks, both show one theme, the card removed and put back, a PC edit wins); single-card Linux: the card in unit A, then in unit B with edits, then back in A (A's settings come back, B's are parked and in B's card set, none of B's in A's set); two units with the same serial (a collision id, two sets); a card from a shipped build with no slot marker (its slot files are back after a new-build run).
- **Card contract** (`test_card_contract.py`): a Linux visit leaves every other device's set, every label, `emulators.json` and the legacy folders byte-identical; `theme.json` holds only `card: theme` keys.
- **Device:** on the ARC-D, Android and Linux each keep their own set on the one card, and the theme follows both ways (OA-18).
- **Verify list (next build window, the Cube and the ARC-D):** the serial each source gives, on two units of one model where possible; the fingerprint sources and whether the MAC is the factory one; where U-Boot keeps its environment (Linux); which `es_settings.xml` the bundled ES-DE app reads, and which set key 3.5.0 writes; what SeedlessDS's `user` folder holds.

### 8.6 Phases

| # | Step | Kind | Size *(guess)* |
|---|---|---|---|
| 0 | `LIBRARY-FORMAT.md` and the other docs, the three schemas and fixtures, `prefs.json`'s `card` field | LO | 2-3 days |
| 1 | Device identity: `DeviceIdentity` on three platforms, `NanoDeviceId`, thimsy-session's file | AS + LO | 2-3 days |
| 2 | Per-device picks: rework W8 on `linux-next` (§7), `picks.json`, systems adoption keeps local launch fields, labels never written | AS | 1-1.5 weeks |
| 3 | Emulator sets: `NanoEmuSets`, `NanoEmuSync` generalised, `DeviceStore`, `EmulatorFiles` (moves), slots and the slot check, parked sets, the switch and its rows, the before and after syncs, §6.1's migration and question | AS + LO | 3-4.5 weeks |
| 4 | Theme on the card: `NanoThemeCard`, the call sites, the ES-DE mirror, downloads to the card | AS | 1-1.5 weeks |
| 5 | Playlists on the card (OA-48) | AS | 3-5 days |
| 6 | Host scenarios, the card contract checks, the ARC-D round trip at the next build window | AS + LO | 1 week |

Total about 8 to 11 person-weeks *(guess)*. Phase 2 must land on `linux-next` before it merges into `linux` (OA-46). Phases 1 and 0 can start at once; 3 and 4 depend on 1; 4 is independent of 2 and 3.

## 9. Questions for the owner

1. **The local theme while a card's is shown.** Separate values: the device's own theme comes back when the card leaves (recommended, §4.7, closest to "the card takes priority over the device's local value"). Or a mirror: the device keeps the last theme it showed.
2. **A switch for the card theme.** None: it always applies with a theme card in (recommended). Or a per-device switch like the emulator sets'.
3. **Borderline theme keys.** Recommended: sounds stay local (including `nav_sounds`, although `nano-theme.props` sets it with the theme), and custom wallpaper images stay local. Alternative: a wallpaper image on the library card travels, written `%CARDID%`.
4. **ES-DE app theme edits.** Mirror the four keys to `es_settings.xml` and take the ES-DE app's own theme changes back into `theme.json`, with §2.4's checks against stale rewrites (recommended). Or mirror only.
5. **ES-DE labels.** Never written or adopted after migration, so the ES-DE app and Thimsy keep separate picks (recommended). Or Android keeps writing labels on a pick, so the ES-DE app follows the last Android device's choice.
6. **Turning the switch on when the card already has this device's set.** Ask, with "Use the card's" first when the local set is unchanged since install, else "Use this device's", and keep the replaced copy once in `.previous` (recommended). Or follow OA-46 literally and copy the local set over with no question (the copy is still kept).
7. **Copying another device's set.** A manual Settings action for same-platform sets (recommended). Or also offer it automatically to a new unit of the same model.
8. **The id on the card.** The platform and a hash of the serial (recommended, §1); the model label is in `device.json`, not in the id. Or the raw serial, easier to match by hand but personal data on a card that may be lent.
9. **RetroArch's live config on Android.** A slot in its card folder, filled before each launch (recommended for v1: no framework change). Or move it to device storage, which changes `raLaunchConfig` and the framework's `NanoRetroArchConfig.java`, and stops RetroArch launched outside Nano from reading another device's settings.
10. **Switch defaults.** Linux on; Android from `emu_sd` (recommended).
11. **Clashes.** File-level, the card wins (OA-46), and the losing device copy is kept once in `conflicts/` (recommended). Or no copy kept.
12. **Cards shared with shipped Android builds.** Their changes to the old shared files (`<emu>/app/`, labels) are not seen by new builds after migration; slot files stay shared with the migrated device, and are kept and put back on other cards (§6.3) (recommended: accept and document). Or keep importing the shared files, which brings back the shared model for those devices.
13. **Save states.** Stay per platform and shared between devices of a platform (recommended, OA-21 unchanged). Or per device too.
14. **Scraper preferences.** Local only: neither adopted from nor written to the card's `es_settings.xml` (recommended, §2.6, OA-47). Or keep writing them (never adopting), so the ES-DE app scrapes the way the last device that changed one does.
15. **`systems.json`'s shared fields** (which systems show, their order and names, custom systems). Library data on the card, card wins, as today (recommended, §2.6, OA-49). Or local per device, which needs a local layer over the card's list.
16. **Switch off, card holds this device's set.** Ignore the card set (recommended, §4.1: a read-only set would drop every edit at the next start). Or use it read-only, as OA-46's lookup sentence alone suggests.
