ES-DE store on the card: inventory and Linux resolution (OA-8 audit, esde task)

Root: C:/Users/krist/code/thimsy-os-linux/nano (shared by the Android and Linux builds). Android seeding: C:/Users/krist/code/thimsy-os-nano. I edited nothing and built nothing.

## A. How the store is located (both builds, same code)

- **The ES-DE folder.** `esdeDataDir()` reads `persist.gammaos.nano.esde.dir`. If that is unset or not absolute it falls back to `/storage/emulated/0/ES-DE` (NanoEsdeStore.cpp:58-59, 120-126).
- **Store mode** is on only when that property starts with `/` (NanoEsdeStore.cpp:128-132).
- **Other readers of the same property:**
  - Themes, with a duplicate fallback (NanoThemeEngine.cpp:65-71).
  - Media roots (NanoThemeEngine.cpp:1739-1766). In store mode the only media root is `<store>/downloaded_media/`. `MediaDirectory` and `persist.gammaos.nano.esde.mediadir` are ignored.
- **Who sets it** (Android and Linux alike):
  - `fileJobRelocate` after Move ES-DE to microSD (NanoSdMove.cpp:2250-2253). This also sets `persist.gammaos.scraper.dir=<card>/ES-DE/nano` and clears `mediadir`.
  - Setup's "use the card's folder" path (NanoSdMove.cpp:983-987).
  - `esdeStoreFollowCard`, which switches to the same relative folder on another card once the first card has been out 10 s, and only after 30 s of uptime (NanoEsdeStore.cpp:157-184).
- **The card's path** comes from `sdCardRoot()`, which calls `nanoplat::get().volumes->cardRoot()` (NanoSdMove.cpp:404-406). The move target is `sd + "/ES-DE"` (NanoSdMove.cpp:459).
- **Path normalisation.** `esdeAppPath` maps `/mnt/media_rw/X`, `/data/media/0/X` and `/sdcard/X` to their `/storage/...` forms (NanoEsdeStore.cpp:108-113). `esdeRawPath` maps back for stat (NanoEsdeStore.cpp:135-139). `esdeStoragePath` is the whitelist of accepted prefixes: `/storage/`, `/mnt/media_rw/`, `/mnt/shares/`, `/data/media/0/`, `/sdcard/`, `content://` (NanoEsdeStore.cpp:632-637).
- **The ROM folder** (`esdeRomDirectoryNow`, NanoEsdeStore.cpp:639-652), tried in order:
  1. `es_settings.xml` `ROMDirectory`, with a leading `~` expanded to the ES-DE folder ("~ is the ES-DE folder on Android", line 617).
  2. If that folder is missing, the same relative path on the ES-DE folder's own volume (the re-root).
  3. Otherwise `<parent of ES-DE>/ROMs`.

  The result is cached for 3 s (NanoEsdeStore.cpp:620-627). It is used for `%ROMPATH%`, the folder names in `esdeRootFolder` (NanoThemeEngine.cpp:106-121) and `custom_systems`.

## B. File-by-file inventory (`<card>/ES-DE/...`)

### 1. settings/es_settings.xml
- **Writers:**
  - ES-DE itself.
  - Nano's `esdeSettingsSet`/`SetIn`, a flat `<type name="" value="" />` line editor that keeps the existing tag type (NanoEsdeStore.cpp:888-951).
  - The move's staging (NanoSdMove.cpp:751-783).
  - The 1.2-base seed `esde_config.py`. It writes `ROMDirectory=/storage/emulated/0/ROMs` and `InputTouchOverlay=false` to `/sdcard/ES-DE` (thimsy-os-nano/scripts/esde_config.py:24-27, 147-152). This goes to internal storage, not the card, and only when the build is given the ES-DE APK on the 1.2 base (mods/37-esde.sh:28-35).
- **Keys Nano reads and writes:**
  - `ROMDirectory`.
  - `CollectionSystemsCustom` (comma list; NanoEsdeStore.cpp:723, 842-851).
  - `MediaDirectory`, cleared on the move (NanoSdMove.cpp:765, 791-792).
  - The scraper keys mirrored to and from `persist.gammaos.scraper.*` (NanoEsdeStore.cpp:1704-1723): `Scraper`, `ScraperRegion`, `ScraperOverwriteData`, `ScrapeScreenshots`, `ScrapeTitleScreens`, `ScrapeBackCovers`, `Scrape3DBoxes`, `ScrapePhysicalMedia`, `ScrapeMarquees`, `MiximageGenerate`, `ScrapeManuals`, `ScrapeVideos`.
  - The card's values become device properties when read (NanoEsdeStore.cpp:1733-1750).
- **Absolute and platform values inside:**
  - `ROMDirectory` is absolute. Nano writes it only when it is empty and the path is already in `/storage` form (NanoEsdeStore.cpp:1755-1756; NanoSdMove.cpp:780).
  - The move's `romDir` comes from the card's `ROMDirectory`, else the internal one, else `sd/ROMs` (NanoSdMove.cpp:470-475). A device with an internal `ROMDirectory` (for example `/storage/emulated/0/ROMs` from esde_config.py) can therefore stamp a device-internal path onto the card. *Inference:* another device that also has `/storage/emulated/0/ROMs` would use its own internal ROMs.
  - On the Cube, `ROMDirectory` is `/storage/00000000-0000-0000-0000-00000000000N/ROMs` (linux_handheld_plan.md:801-803). Linux fixes this through the re-root branch (NanoEsdeStore.cpp:647-648): the volume differs from the store's, so the store's volume plus the relative path is used.
  - `~` semantics differ: Android ES-DE (as the code assumes) versus mainline Linux ES-DE, where `~` is `$HOME`.
- **Not synced to the card.** Nano keeps the theme settings (`ThemeSet`, variant, colour scheme, font size, aspect ratio) only in `persist.gammaos.nano.esde.themeset/variant/colorscheme/fontsize/aspectratio` (NanoMenuEsdeMenu.cpp:53-59; NanoThemeEngine.cpp:504-508). They are not in `kEsdeSettingProps`, so they do not travel.

### 2. gamelists/<folder>/gamelist.xml
- **Writers:**
  - ES-DE.
  - Nano's `esdeGamelistFlush`: a tinyxml2 load, an in-place edit, then a re-print of the whole document, keeping unknown tags; an unreadable file is never replaced (NanoEsdeStore.cpp:368-481).
  - The move's merge (NanoSdMove.cpp:494-698).
  - Seeds: esde_config.py writes `<alternativeEmulator>` and `<folder><folderlink>` entries for PSP folders (esde_config.py:107-124). bundled_roms.py writes pack-in gamelists to `sdcard/ES-DE` on internal storage (thimsy-os-nano/scripts/bundled_roms.py:38, 196-214).
- **Folder name.** `<folder>` is the ES-DE system name of the ROM folder, or the folder's own name for a folder in ROMDirectory that ES-DE has no system for (NanoThemeEngine.cpp:106-133).
- **Fields Nano writes:**
  - `favorite`, `hidden`.
  - `name` (title overrides; NanoEsdeStore.cpp:1082-1094).
  - `desc`, `rating` (0-1), `releasedate` (`YYYYMMDDT000000`), `developer`, `publisher`, `genre`, `players` (NanoEsdeStore.cpp:1036-1079).
  - `playcount` (increment) and `lastplayed` (`%Y%m%dT%H%M%S`, local time, only later times win; NanoEsdeStore.cpp:957-964, 1018-1019).
  - `altemulator` per game (NanoEsdeStore.cpp:1538).
  - `<alternativeEmulator><label>` per gamelist, written before `<gameList>` (NanoEsdeStore.cpp:406-421).
- **Read back:**
  - favourites, hidden, `name`, `altemulator`, `lastplayed` (ignored when it starts with `19700101`) and `alternativeEmulator` (NanoEsdeStore.cpp:540-553).
  - Recently Played is rebuilt from `<lastplayed>` (NanoEsdeStore.cpp:1577-1618).
- **Paths inside:** `<path>` is relative (`./rel`), matched case-insensitively (NanoEsdeStore.cpp:398-404, 437). This form is portable.
- **Platform-specific values: the emulator labels.**
  - Labels are ES-DE **Android** command labels from `NanoEsdeEmulators.inc`, generated from ES-DE's `resources/systems/android/es_systems.xml` and `es_find_rules.xml` at 50e4b600a (NanoEsdeEmulators.inc:1-4).
  - A label maps to keys `core:<name>` or `pkg:<android package>` (NanoEsdeSystemName.cpp:149-186). `esdeEmulatorKey` only recognises `*_libretro_android.so` (NanoEsdeSystemName.cpp:151-155).
  - Seeded Android labels include `SameBoy`, `Beetle PSX HW`, `Fake-08`, `M64Plus FZ (Standalone)`, `WatermelonDS (Standalone)`, `hakuX (Standalone)`, `Azahar (Standalone)`, `Dolphin (Standalone)`, `ARMSX2 (Standalone)`, `PPSSPP (Standalone)`, `ScummVM (Standalone)`, `Flycast (Standalone)` (thimsy-os-nano/mods/esde-emulators.txt:9-34).
  - Linux ES-DE's linuxarm table lacks several of these, for example `SameBoy`, `M64Plus FZ`, `WatermelonDS` and `Fake-08` (linux_handheld_plan.md:729-739).
- **How Linux treats those labels:**
  - Linux keeps `_libretro_android.so` names in its model: the catalog builds `coreSo = libretro + "_libretro_android.so"` (NanoMenuPS3EmuCatalog.cpp:593), and the Linux launcher strips either suffix (platform/linux/Launcher.cpp:388, 404). So Linux computes and writes the same Android labels.
  - Applying a label needs `esdeCatalogForLabel`, which requires `coreSoExists` (`/data/data/com.retroarch.aarch64/cores/<x>_libretro_android.so`; NanoMenuXmb.cpp:3345-3351) or an installed package from `packages.list` (NanoMenuPS3EmuCatalog.cpp:202, 767-782).
  - *Inference (no Linux provisioning of either found under linux/):* Android-written `<altemulator>` and `<alternativeEmulator>` labels are not applied on Linux. They are left in the file, because Linux overwrites only after the user changes the emulator on Linux (NanoMenuPS3EmuCatalog.cpp:869-883).
  - If Linux's choice has no Android label, `esdeStoreSaveSystemEmus` deletes `<alternativeEmulator>` (NanoMenuPS3EmuCatalog.cpp:911-915).
  - `esdeStoreSaveGameEmu` clears `<altemulator>` and records the game in `nano/emulators.json` (NanoEsdeStore.cpp:1538-1571).

### 3. collections/custom-<name>.cfg
- **Writers:** ES-DE and Nano (NanoEsdeStore.cpp:784-855; move merge at NanoSdMove.cpp:700-749).
- **Format:** one line per game. Nano writes `%ROMPATH%/<system>/<rel>` when the game is under the ROM folder, otherwise the absolute `/storage/...` path (NanoEsdeStore.cpp:677-689). The file name is sanitised (NanoEsdeStore.cpp:693-697).
- **Enabled list:** the collection must be listed in `CollectionSystemsCustom` to load (NanoEsdeStore.cpp:723-726).
- **Reading:**
  - `%ROMPATH%` is tried against the ROM folder, then against the parents of every system folder, same volume first (NanoEsdeStore.cpp:656-672, 764-766).
  - An absolute line is looked up as-is (NanoEsdeStore.cpp:767-770).
  - A line that does not resolve is kept verbatim (NanoEsdeStore.cpp:772, 830).
- **Platform values:** absolute lines carry `/storage/<VOLID>/...`, or the Cube's `/storage/00000000-...-N/...`, which does not resolve on Linux. These are written only for games outside ROMDirectory.

### 4. custom_systems/es_systems.xml
- **Writer:** Nano, append-only, one entry per folder in ROMDirectory that ES-DE has no system for. Existing entries are never touched (NanoEsdeStore.cpp:1627-1698).
- **Fields:** `name`, `fullname`, `path`=`%ROMPATH%/<folder>`, `extension`, `platform`, `theme`, and `<command label="...">` strings copied from the **Android** table (NanoEsdeStore.cpp:1678-1687).
- **Platform values inside the commands:**
  - Android ES-DE find-rule and variable names: `%EMULATOR_RETROARCH%`, `%EMULATOR_MAME4DROID-CURRENT%`, `%EMULATOR_COLEM%`, `%EMULATOR_FLYCAST%` and so on.
  - `%EXTRA_CONFIGFILE%=%EXTERNALDATA%/Android/data/%ANDROIDPACKAGE%/files/retroarch.cfg` and `%EXTRA_LIBRETRO%=%INTERNALDATA%/%ANDROIDPACKAGE%/cores/{core}_libretro_android.so` (NanoEsdeEmulators.inc:953-954).
  - `%ROMSAF%`, `%ROMPROVIDER%`, `%ACTION%=android.intent.action.VIEW`, `%ACTIVITY_CLEAR_TASK%`, `%ANDROIDAPP%=%FILEINJECT%` (NanoEsdeEmulators.inc:7-60).
- **Readers:** Nano never reads `custom_systems` or `es_find_rules.xml` (android_launcher_plan.md:65). Linux writing this file is harmless to Android; it would break a real Linux ES-DE (D9, not shipped).

### 5. downloaded_media/<folder>/<type>/<name>.<ext>
- **Writers:** ES-DE, the Nano scraper and the move.
- **Created when the store loads** (NanoEsdeStore.cpp:1157).
- **Nano's own art:** in store mode it goes to `covers/<name>.png` and `fanart/<name>.jpg` (NanoEsdeStore.cpp:320-328). `<name>` is the game's sub-path without extension; the first name tried is the game's own folder (NanoThemeEngine.cpp:1781-1860).
- **Types read:** covers, backcovers, 3dboxes, physicalmedia, marquees, miximages, screenshots, titlescreens, fanart, manuals, videos (NanoThemeEngine.cpp:1886-1888, 1918-1925). Extensions read: png, jpg, jpeg, webp; manuals pdf, cbz, cbr, cb7; videos mp4, mkv, webm, avi (NanoThemeEngine.cpp:1769-1774).
- **The move:** Nano's hash-named cache art is moved into this layout (NanoSdMove.cpp:827-865).
- **Paths inside:** none; the media is found by name. Portable.
- **Linux decoding:** PNG, JPEG, WebP and GIF; no HEIF or AVIF (IMPLEMENTATION.md I30). Video and PDF playback on Linux was not checked (*unknown*).

### 6. themes/<set>
- **Read** as the third theme root (NanoThemeEngine.cpp:73-81; NanoMenuEsdeMenu.cpp:197).
- **Not written there:** downloaded themes go to `/data/system/nano_esde_themes` on the device (NanoMenuEsdeMenu.cpp:898-905, 931-932), so they do not travel.

### 7. nano/ (Nano-only)
- **`nano/systems.json`.** Nano's systems without the scraper accounts. The card's volume id in scan sources is replaced by `%CARDID%` (NanoMenuXmb.cpp:984-1083). `esdeCardId` only recognises `/storage/<id>` (NanoMenuXmb.cpp:1023-1028).
  - Holds `launchType`, `coreSo` (it must end in `_libretro_android.so`: `esdeCoreNameOk`, NanoMenuXmb.cpp:1015-1021), `packageName`, `launchIntent`/`launchArgs` (Android `am` arguments) and `iconRef`.
  - Adopting the card's copy keeps launch tuples only if they are bundled or catalog-known. An unknown one is replaced by this device's tuple (NanoMenuXmb.cpp:1152-1215). Rejected systems are stashed (`/data/system/nano_esde_kept_systems.json`) and written back.
- **`nano/emulators.json`.** `{"version":1,"items":[{"game":"<folder>/<rel>","label","am":"<Android am args>"}]}` (NanoEsdeStore.cpp:1476, 1560-1571; NanoSdMove.cpp:533-537). It is loaded only when `am` matches Nano's catalog (NanoEsdeStore.cpp:1505-1508). Platform values: Android `am` and intent strings.
- **`nano/names.json`.** `{"version":1,"items":[{"rom":<absolute path>,"name"}]}` (NanoMenuScraper.cpp:420-440; NanoEsdeStore.cpp:257-259). Absolute ROM paths are tied to the volume id (android_launcher_plan.md:61).
- **`nano/index.json`** (the scrape index, in `persist.gammaos.scraper.dir=<ES-DE>/nano`). Version 2, with items `rom`, `box`, `fan` (absolute paths), `title`, `desc`, `genre`, `players`, `rating`, `date`, `dev`, `pub`, `when`, `kept`, `xmiss`, `xwhen` and a `journal` (NanoMenuScraper.cpp:208-244).
  - Pack-in entries use `/data/media/0/ROMs/...` and `/data/media/0/ES-DE/downloaded_media/...` (bundled_roms.py:41-42, 184-188).
  - On Linux `/data/media/0` is a link to `/storage/emulated/0` (tools/nano-tree/path-roots.json:25-26). Cube-id paths do not resolve on Linux.
- **`nano/not-in-a-system-folder.txt`.** Written by the move: favourites, hidden games, titles and emulators that could not be placed, as tab-separated absolute paths (NanoSdMove.cpp:569-576).
- **Watched by the sweep:** `nano/systems.json`, `nano/names.json`, `nano/emulators.json` (NanoEsdeStore.cpp:1257-1261).

## C. State kept off the card (`/data/system`; on Linux `/storage/.config/thimsy/data/system`, path-roots.json:29-30)

**Store-related and device-local:**
- `nano_esde_played.txt` and `.wait`: the journal of game starts, flushed into `playcount`/`lastplayed` when the card is in. Waiting entries are kept 30 days (NanoEsdeStore.cpp:968-1028). Starts not yet flushed stay on the device.
- `nano_esde_sysemu.sync`: per-folder system-emulator labels last agreed with the card (NanoMenuPS3EmuCatalog.cpp:802-827).
- `nano_esde_systems.sync`: the FNV hash of `systems.json` last agreed (NanoMenuXmb.cpp:989-1012).
- `nano_esde_kept_systems.json`: the card's systems Nano did not adopt, keyed `"for"` by ES-DE folder path (NanoMenuXmb.cpp:1035-1083).
- `nano_systems.json`: the live systems, written from the card when adopted.
- `nano_xmb_recent.list`: rebuilt from `<lastplayed>` (NanoEsdeStore.cpp:1607).
- `nano_xmb_cache/`.

**Device-local by design:** the theme properties and downloaded themes (`nano_esde_themes`), the scrape queue and progress under `nano_scrape/queue.d` (NanoScrapeQueue.h:38-41), the scraper accounts.

**Internal-mode stores** (merged during the move, then deleted at NanoSdMove.cpp:2473-2475): `nano_favorites.txt`, `nano_esde_unfavorites.txt`, `nano_hidden_games.txt`, `nano_collections.txt`, `nano_scrape/names.json` and `index.json`, `nano_game_emulators.json`.

**Move scratch:** `nano_esde_stage`, `nano_esde_stage_merged`, `nano_esde_card_orig` (NanoSdMove.cpp:79-81).

**Properties that do not travel:** `persist.gammaos.nano.esde.dir`, `persist.gammaos.scraper.dir`, `persist.gammaos.nano.esde.mediadir`, and the theme keys above. On Linux the property store is `/storage/.config/thimsy/persist.props` (linux/src/thimsy-session/src/Session.cpp:49; nano/compat/include/nanocompat/nanocompat.h:50).

## D. How the Linux build resolves esdeDataDir() and the ROM folder today

1. **Same code, same property name, new backing file.** The property is served from `persist.props`.
   - Nothing on the Linux side sets `persist.gammaos.nano.esde.dir`. Searching `linux/` for esde, ES-DE and esde.dir found only asset lists; there are no init or session defaults.
   - So on first boot `esdeDataDir()` is `/storage/emulated/0/ES-DE` on the 256 MiB data partition (I44), and **store mode is off**: gamelist, collection and setting writes return early (NanoEsdeStore.cpp:335, 357, 971).
   - This contradicts linux_handheld_plan.md:793 ("Store mode is always on in the Linux build"), which is not implemented.
2. **Entering store mode on Linux:** Setup's microSD step, when `sdCardRoot()` is non-empty (NanoMenuSetupWizard.cpp:623-624, then `esdeMoveAuto`), or Settings > Move ES-DE to microSD.
   - `cardRoot()` comes from `/run/thimsy/volumes.json` and prefers the SD slot, then the boot disk's games drive (nano/platform/linux/Volumes.cpp:103-116).
   - The folder is therefore `/storage/<VOLID>/ES-DE`. VOLID is the FAT or exFAT serial `XXXX-XXXX` (linux/src/thimsy-session/src/core/VolumePolicy.h:18-23), the same id stock LineageOS uses.
   - On the Cube the Android id is `00000000-0000-0000-0000-00000000000N` (linux_handheld_plan.md:801-806). D20 plans a prefix map; none is implemented in `NanoEsdeStore.cpp`.
   - `am force-stop org.es_de.frontend` in the move is refused by the shell gate (IMPLEMENTATION.md:255).
3. **Alternate spellings:** `/mnt/media_rw/<id>` links to `/run/thimsy/media_rw/<id>`, which links to `/storage/<id>`; `/sdcard` links to `/storage/emulated/0`; `/data/media/0` links to `/storage/emulated/0` (linux/board/common/post-build.d/10-android-links.sh:9-15, 71-75; path-roots.json:25, 43, 53). Android-spelled paths therefore resolve on Linux for the same VOLID.
4. **ROM folder:** `esdeRomDirectoryNow` as in section A.
   - With store mode off it reads `/storage/emulated/0/ES-DE/settings/es_settings.xml` (usually absent) and falls back to `/storage/emulated/0/ROMs` if it exists.
   - In store mode it is the card's `ROMDirectory`, re-rooted onto `/storage/<VOLID>` when the stored volume is missing (this covers Cube-written `ROMDirectory`), else `/storage/<VOLID>/ROMs`.
   - *Inference:* the alpha's library on the games drive comes from the system scan (`gsAutoAddFromRoot`, NanoSdMove.cpp:415-419), not from this function, until store mode is entered.

## E. Gaps against OA-8 (summary; inference unless cited)

1. Linux never enters store mode on its own (D.1), so nothing it changes goes onto the card until the user runs Setup's microSD step or Move ES-DE to microSD.
2. Gamelist emulator labels and `<alternativeEmulator>` come from the Android table. Linux cannot apply them (`coreSoExists`/`packages.list`) and deletes `<alternativeEmulator>` when the user picks a Linux emulator with no Android label (NanoMenuPS3EmuCatalog.cpp:911-915). The plan wants both the `android` and `linuxarm` tables (linux_handheld_plan.md:739).
3. `nano/names.json` and `index.json` use absolute, volume-bound paths, and pack-in entries use `/data/media/0`. Cube-id paths break on Linux (android_launcher_plan.md:61, 160).
4. `nano/emulators.json` and `systems.json` launch fields are Android `am` and intent strings plus `_libretro_android.so` names. Linux keeps those names; standalone packages have no Linux meaning.
5. `custom_systems` commands are Android-only (harmless unless a real ES-DE runs on Linux).
6. Theme choice and downloaded themes stay on the device.
7. An internal `ROMDirectory` can be copied onto the card by the move (NanoSdMove.cpp:470-475, 780).
8. Collection lines that are absolute carry the volume id.
9. `<lastplayed>` is local time with no zone (NanoEsdeStore.cpp:957-963); devices set to different time zones mis-order Recently Played.
10. The playtime journal is on the device: starts not yet flushed do not travel.
11. `<folder>`/`<folderlink>` entries (PSP folders) from esde_config.py are matched only for merging during the move. Store edits key on `<game>` paths and may add a second `<game>` entry for the same title (NanoEsdeStore.cpp:399-404 against NanoSdMove.cpp:603-614).
