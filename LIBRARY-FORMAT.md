# Thimsy library format: the card contract

Version 1 of the contract, written 2026-11-26. This is the document `android_launcher_plan.md` §3 and `linux_handheld_plan.md` §7 call `LIBRARY-FORMAT.md`. It says what every file and folder on a Thimsy game card means, who may write it, and how each side treats what it does not know. Android Thimsy and Linux Thimsy both implement it. Tests are written against this document, not against either build.

Where this comes from:

- The owner's answers OA-8, OA-19 to OA-26, OA-32 and OA-38 (emulator picks across platforms), and the interop questions decided autonomously (Q4, Q6, Q12), in `docs/linux/DECISIONS.md`.
- The interop audit, `docs/linux/INTEROP-AUDIT.md`, and its raw reports in `docs/linux/interop-2026-11-26/`.
- `linux_handheld_plan.md` §6.2 to §7.10 and `portable_settings_plan.md`.

**Status.** Most of this describes what the Android build already writes. The new parts (`card.json`, `format.json`, the Linux folders, `Thimsy/settings.ini`, the playlists) are not implemented by either build yet. Section 9 lists every known gap and where its fix lands. Anything marked ***proposed*** is a choice made while writing this contract that the owner has not decided; section 10 lists them.

**Machine-checkable parts.** The JSON files have JSON Schemas (draft-07, the subset `linux/tools/thimsy-import/thimsy_import/devicejson.py` validates):

| File on the card | Schema |
|---|---|
| `emulator_config/card.json` | `linux/schema/card.schema.json` |
| `ES-DE/nano/format.json` | `linux/schema/format.schema.json` |
| `ES-DE/nano/systems.json` | `linux/schema/systems.schema.json` |
| `ES-DE/nano/emulators.json` | `linux/schema/emulators.schema.json` |
| `ES-DE/nano/names.json` | `linux/schema/names.schema.json` |
| `ES-DE/nano/index.json` | `linux/schema/index.schema.json` |

`linux/tests/test_card_contract.py` validates the test vectors in `linux/tests/fixtures/card/` against them and checks the rules between an Android-written card and the same card after a Linux visit (section 8).

Schemas of Thimsy's new files (`card.json`, `format.json`) are strict: they describe exactly what a writer of this major version emits. Schemas of files already in users' hands (`systems.json`, `emulators.json`, `names.json`, `index.json`) allow unknown fields, because real cards carry them. In code, no reader ever refuses a file for an unknown field (rule 1.3).

## 1. Rules for every file

### 1.1 The card

- **One `games` volume, exFAT recommended, FAT32 accepted.** NTFS, ext4, f2fs and btrfs are not supported for interop (D8, plan §7.8).
- **Regular files and folders only.** No symbolic links, device nodes or permission bits. A link an emulator needs lives on the device's internal storage and points at the card (plan §7.4).
- **Names** are UTF-8 and avoid `: * ? " < > |` and trailing dots. No two names differ only by case.
- **The volume id must be usable.** Linux refuses an all-zero serial (`0000-0000`), as stock Android does. Two mounted cards with the same serial (a cloned card): neither is used, and the user is asked to remove one (decision 17).
- **Two cards that both hold a library** (OA-25): the first time, Nano asks which one to use and remembers the answer for that pair of card ids, on the device. It can be changed in Settings. With one library present it is used without asking.

### 1.2 Writes

- **Every structured file** (XML, JSON, INI, playlists) is written to a temporary name in the same folder, `fsync`ed, renamed over the old file, and then the folder is `fsync`ed (plan §7.8). The temporary names in use are `<name>.nanotmp`, `<name>.tmp` and `<name>.tmp.<pid>`.
- **Readers ignore temporary files.** A reader may remove a stale one only after the final file has parsed (`portable_settings_plan.md` §4.5).
- **An unreadable file is never overwritten.** A writer that cannot parse the current file leaves it as it is and logs why. Nano's writers already do this for gamelists, `emulators.json`, `names.json`, `index.json` and the music library.
- **One device at a time.** Nothing writes the card from two devices at once. Unclean removal is the failure the format is designed for.

### 1.3 Unknown data is kept

- **Unknown files and folders** are left alone, on both sides.
- **Unknown XML tags** in gamelists, and unknown keys in `es_settings.xml`, are kept when the file is rewritten.
- **Unknown JSON fields** are kept by every writer that edits a file in place. Android rebuilds `systems.json`, `names.json` and `index.json` from its own structs, so a field only Linux knows does not survive an Android save there. That is why Linux puts nothing of its own in those files (sections 4.8 and 4.11).
- **Unknown INI keys, sections and comments** are kept byte for byte (`settings.ini`, the emulator override layers).
- **Values a side cannot use** (an emulator label with no emulator on this device, a system with no emulator here, a path to a volume that is not in) are kept and shown as unavailable, never deleted.

### 1.4 Compatibility with builds already in users' hands

Shipped Android builds never check a `version` field (INTEROP-AUDIT.md). So:

- **Changes are additive only.** A new file, a new field or a new folder. Never a new meaning for an existing field.
- **Labels are kept.** A writer keeps writing an ES-DE emulator label wherever one exists. Older builds and the real ES-DE app read only `<altemulator>` and `<alternativeEmulator>`.
- **`names.json` and `index.json` stay in their absolute form** until the Android compatibility floor (Q12). Relative keys would be pruned or cleared by every shipped build (check.md B1, B2).
- **The ROMDirectory re-root rule stays "only when the stored folder is missing"**, widened only to "or the stored id is a known alias of this card" (check.md B6).
- **Theme keys in `es_settings.xml`** are written only for a theme that exists in `<store>/themes` (OA-32 Q4).

**The compatibility floor** is the first Thimsy Android release built after the AOSP job that carries this contract's Android changes (Q12). Until a card has been written by that release, Linux runs in compatibility mode: it never rewrites `names.json` or `index.json`, and it changes a label only where the user picks an emulator on Linux (OA-38, section 5). A card has been written by the floor when its `card.json` has an Android writer with `contract` 1 or more (***proposed***, W7-INTEROP-6).

### 1.5 JSON values

Android's JSON writer (`NanoJson`) prints numbers with `%.6g` and turns infinity into `null` and a lone surrogate escape into U+FFFD. So JSON files on the card hold only strings, booleans and whole numbers below 10^15 (plan §7.6). Dates are strings, `YYYY-MM-DD`, and are informational only: handheld clocks are often wrong, so nothing is ordered by them.

### 1.6 What readers skip

`Android/`, `LOST.DIR`, `System Volume Information`, `.Trashes`, `.Spotlight-V100`, names starting with `.` (macOS `._*` files included), and the temporary names in 1.2. Syncthing markers, if Android Syncthing shares a card folder, are left alone.

## 2. Identity and paths

### 2.1 Volume ids

| Who | The card is `/storage/<id>` with `<id>` = | Notes |
|---|---|---|
| Linux Thimsy | the FAT or exFAT serial, `XXXX-XXXX`, upper case | thimsy-session mounts it there (`Blkid.cpp`, `VolumePolicy.cpp`) |
| Stock LineageOS 21 | the same serial | paths match Linux's |
| Thimsy Android (GammaOS Next's vold) | a mount-order index, `00000000-0000-0000-0000-NNNNNNNNNNNN` | not a property of the card: a USB drive mounted first, or another volume on the card (OA-2's FAT README entry), can shift it (plan §7.3) |

OA-19 decides both fixes: Linux keeps an alias list (section 4.16) and resolves Cube paths through it, which works with every card already in use; and a later Thimsy Android build names public volumes by serial (D20(c)), so new paths match everywhere.

### 2.2 Path forms

Best first. A writer uses the first form the file allows.

| Form | Example | Used by |
|---|---|---|
| (a) relative to the owning folder | `./Crash Bandicoot (USA).chd` | gamelist `<path>`; playlist entries |
| (b) `%ROMPATH%/<system>/<rel>` | `%ROMPATH%/psx/Crash Bandicoot (USA).chd` | collections; `custom_systems` paths |
| (c) a game key, `<ES-DE folder>/<rel>` | `n64/Mario Kart 64 (USA).z64` | `emulators.json` |
| (d) `%CARDID%` for the store's own volume | `/storage/%CARDID%/ROMs/psx` | `systems.json` scan sources; `settings.ini` paths; `<emu>/linux/` files |
| (e) absolute, `/storage/<id>/...` | `/storage/9602-C755/Homebrew/demo.nes` | only where the file's format needs an absolute path (ES-DE's ROMDirectory and collection lines outside ROMDirectory), and `names.json` and `index.json` until the floor |
| by name, no path | `downloaded_media/psx/covers/<name>.png` | media |

**Rules:**

- **Form (d) is spelled `/storage/%CARDID%/<rel>`**, as `systems.json` already writes it. Readers also accept `%CARDID%/<rel>`, the spelling `portable_settings_plan.md` and contract.md C1 use (***proposed***, W7-INTEROP-14).
- **Nothing new is written as an absolute path when another form fits.** Files Linux owns (section 3) never hold one.
- **Every reader accepts every form**, and these legacy spellings of (e), read-only: `/storage/<serial>`, `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN`, `/mnt/media_rw/<id>`, `/sdcard`, `/data/media/0`, `/storage/emulated/0`.
- **An id the card's alias list names is this card.** A path `/storage/<alias>/...` found in the card's own files resolves to the card. When `card.json` is missing, Linux builds the list from the `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN/` prefixes it finds in the card's own files (D20(b)): `es_settings.xml`, the collections, `nano/*.json` and the text configs in `emulator_config` (three levels deep, skipping `app/`).
- Limit: the Cube's `...0001` is the same string for every card first mounted on a Cube, so it identifies "this card" only for paths found in this card's own files. It is never used to find a card.

### 2.3 Foreign ids

A path to an id that is neither mounted nor an alias is a missing volume. Nothing creates folders under it. On Linux, `/storage/<id>` is a directory on the ext4 data partition, so a `mkdir -p` there would quietly write to internal storage (G12): shared code refuses, and adopted mount points are immutable (plan §7.3).

## 3. Layout and owners

```
<card>/                                 exFAT (recommended) or FAT32
  ES-DE/                                the library, in ES-DE's own formats       both
    settings/es_settings.xml            listed keys only (4.1)                    both
    gamelists/<folder>/gamelist.xml     favourites, play stats, metadata (4.2)    both
    collections/custom-<name>.cfg       (4.3)                                     both
    downloaded_media/<folder>/<type>/   box art and the rest, by name (4.4)       both
    themes/<set>/                       (4.5)                                     both
    custom_systems/es_systems.xml       append-only (4.6)                         both
    nano/format.json                    contract major (4.7)                      both
    nano/systems.json                   Nano's systems (4.8)                      both (last pick, OA-38)
    nano/emulators.json                 per-game picks with no label (4.10)       both
    nano/names.json, nano/index.json    titles, scrape index (4.11)               Android until the floor
    nano/not-in-a-system-folder.txt     the move's report (4.12)                  the move
  ROMs/<es-de system name>/             games; same-core in-game saves (4.13)     user; emulators
  bios/                                 BIOS drop folder (4.14)                   user
  emulator_config/                      (4.15)
    card.json                           identity hints, aliases (4.16)            both
    retroarch/system/, cheats/          BIOS, cheats (4.17)                       shared
    retroarch/retroarch.cfg, config/, playlists/, ...                             Android
    retroarch/linux/saves/<system>/     Linux saves, cores differ (4.17)          Linux
    retroarch/linux/states/<system>/    Linux save states (4.17)                  Linux
    ppsspp/...                          (4.18)                                    shared and per platform
    scummvm/...                         (4.19)                                    shared and per platform
    <emu>/app/                          Android app-preference mirror             Android
    <emu>/linux/                        Linux-only data and override layers       Linux
  Thimsy/
    settings.ini                        portable settings (4.21)                  both
    playlists/<name>.m3u8               music playlists (4.22, proposed place)    both
    emulators/*.json                    user emulator definitions (4.23)          user
```

"Both" means both platforms read and write it under the rules of its section. "Android" or "Linux" means only that platform writes it; the other reads it where its section says so and never deletes it.

## 4. File by file

### 4.1 `ES-DE/settings/es_settings.xml`

- **Owner:** ES-DE's format. ES-DE itself may write any key. Nano edits single `<type name="" value="" />` lines and keeps every other line, tag type and order (`esdeSettingsSet`).
- **Keys Nano writes, both platforms:**
  - `ROMDirectory`: absolute (OA-20), written only when empty, as `/storage/<id>/ROMs` for this card. Readers expand a leading `~` to the ES-DE folder (Android ES-DE's meaning), and re-root a stored folder that is missing (or whose id is an alias of this card) to the same relative path on the store's volume.
  - `CollectionSystemsCustom`: comma list of enabled custom collections.
  - `MediaDirectory`: cleared in store mode (the media is `<store>/downloaded_media`).
  - The 12 scraper keys: `Scraper`, `ScraperRegion`, `ScraperOverwriteData`, `ScrapeScreenshots`, `ScrapeTitleScreens`, `ScrapeBackCovers`, `Scrape3DBoxes`, `ScrapePhysicalMedia`, `ScrapeMarquees`, `MiximageGenerate`, `ScrapeManuals`, `ScrapeVideos` (`kEsdeSettingProps`). They are ES-DE's own keys, so they live here and not in `settings.ini`.
  - From OA-32 Q4: `ThemeSet`, `ThemeVariant`, `ThemeColorScheme` and `ThemeFontSize`, only when the chosen theme exists in `<store>/themes`. Nano never writes `ThemeAspectRatio`: aspect ratio stays per device model (***proposed reading*** of OA-32 Q4, W7-INTEROP-9).
- **No other Thimsy key is ever written here** (OA-20).
- **Version:** none; ES-DE's format.

### 4.2 `ES-DE/gamelists/<folder>/gamelist.xml`

- **Owner:** ES-DE's format. `<folder>` is the ES-DE system name of the ROM folder, or the folder's own name when ES-DE has no system for it.
- **Paths:** `<path>` is form (a), `./<rel>`, matched case-insensitively.
- **Fields both platforms write:** `favorite`, `hidden`, `name` (title overrides), `desc`, `rating` (0 to 1), `releasedate` (`YYYYMMDDT000000`), `developer`, `publisher`, `genre`, `players`, `playcount` (only ever incremented) and `lastplayed` (`%Y%m%dT%H%M%S`, local time, ES-DE's own format; a later time always wins).
- **Favourites, hidden games and Recently Played travel here** (OA-23). Recently Played is rebuilt from `<lastplayed>` on each side. The device keeps a cache (`nano_xmb_recent.list`) and a journal of starts made with the card out (`nano_esde_played.txt`), flushed when the card is in. No play journal on the card (Q6).
- **Emulator labels:** `<altemulator>` per game and `<alternativeEmulator><label>` per gamelist hold ES-DE labels: the user's last emulator pick, made on either platform (OA-38, section 5). Both platforms write them with the same code: the label of ES-DE's command that starts the picked emulator. A writer changes a tag only for the game or system the user just picked for, and only removes one when the new pick has no ES-DE label (the pick then goes to `emulators.json` or `systems.json`). A label a platform cannot run is kept, never rewritten by a fallback.
- **`<folder>` entries** (PSP folders seeded with `<folderlink>`) are handled like `<game>` entries, so an edit never adds a second entry for the same title (G20).
- **Unknown tags are kept.** An unreadable gamelist is never replaced.

### 4.3 `ES-DE/collections/custom-<name>.cfg`

- **Owner:** ES-DE's format. Both platforms read and write.
- **Lines:** form (b) for a game under ROMDirectory; else form (e), the absolute path with this card's current id (ES-DE's format has no other form, OA-20). Readers resolve (e) through the alias list.
- **A line that does not resolve is kept verbatim.** File names are cleaned of the characters FAT forbids.
- A collection loads only when `CollectionSystemsCustom` lists it.

### 4.4 `ES-DE/downloaded_media/<folder>/<type>/<name>.<ext>`

- **Owner:** ES-DE's layout. Both write (the scraper, the move).
- **Found by name:** `<name>` is the game's path below its folder without the extension. No index is needed to find art, so covers show on both sides even when `index.json` cannot be used.
- **Types:** `covers`, `backcovers`, `3dboxes`, `physicalmedia`, `marquees`, `miximages`, `screenshots`, `titlescreens`, `fanart`, `manuals`, `videos`.
- **Extensions read:** images `png`, `jpg`, `jpeg`, `webp`; manuals `pdf`, `cbz`, `cbr`, `cb7`; videos `mp4`, `mkv`, `webm`, `avi`. Writers use only these. Known quirk: Nano names its own covers `.png` whatever the bytes (check.md row 11).
- **A side that cannot show a type shows the next one and deletes nothing.** Video and manuals are required on Linux for the alpha (OA-16, OA-24).

### 4.5 `ES-DE/themes/<set>/`

Read by both as a theme root. Downloaded themes go here, not to the device (contract A6, OA-32 Q4), so they travel. Today they still go to `/data/system/nano_esde_themes` (G14).

### 4.6 `ES-DE/custom_systems/es_systems.xml`

- **Owner:** ES-DE's format. Append-only: one entry per folder in ROMDirectory that ES-DE has no system for. No writer ever changes or removes an existing entry.
- **Both read** `name`, `fullname`, `path` (form (b)), `extension`, `platform` and `theme`.
- `<command>` lines carry Android ES-DE commands (for the ES-DE app). A reader ignores commands it cannot interpret. Linux appends entries in the same form Android does.

### 4.7 `ES-DE/nano/format.json`

- `{"version": 1}`. Schema: `linux/schema/format.schema.json`.
- The contract major the library needs a reader to understand. A missing file means 1. A reader that meets a higher major uses the library read-only and says so.
- It guards only new builds against newer ones: no shipped build reads it. Major 1 is today's layout plus this contract's additive files. The launcher plan's `{"version": 2}` (relative `names.json` and `index.json`) needs the floor first (***proposed***, W7-INTEROP-5).

### 4.8 `ES-DE/nano/systems.json`

- **Launch fields:** `launchType` (`libretro-core`, `custom-package`, `retroarch-intent`), `coreSo` (`<core>_libretro_android.so`), `package`, `launchArgs` and `intentTemplate` are each system's emulator pick, in Nano's own terms on both platforms: `coreSo` is the Android core file name everywhere, and Linux maps it to its own core. The last pick wins, made on either platform (OA-38, replacing Q4's "Android owns them"). A writer changes a system's launch fields only for a pick: the user picking its emulator in Nano on either platform, or Nano adopting the `<alternativeEmulator>` the ES-DE app wrote (`esdeStoreApplySystemEmus`), which is a pick made in the ES-DE app. A platform that cannot run a pick starts the system with its own default and leaves the fields as they are (section 5). One exception remains on Android, not yet aligned with OA-38: its once-per-config systems heal (`systemsHealTick`) rewrites a custom system's emulator that cannot run there (`nanosys::playerRepair`) and a built-in system's stale Nano core (`nanosys::staleCoreRepairs`), and in ES-DE store mode that save reaches this file (W8-EMU, open).
- **Shared fields** (both may write): `id`, `builtin`, `enabled`, `order`, `displayName`, `shortname`, `romDir`, `scanSources` (the card's id written `%CARDID%`), `disabledDefaultFolders`, `extensions`, `icon`, and `scraper` with `override` and `platform` only. The scraper accounts (`user`, `pass`, `key`) are never on the card (OA-32 Q6); the schema refuses them.
- **Format:** `{"version": 1, "systems": [...]}`. Schema: `linux/schema/systems.schema.json`. Android rebuilds the file from structs; a field only Linux knows is lost on the next Android save.
- **Linux writing it:** every system kept, used or not, and every system's launch fields byte-identical to what it read except the systems the user picked an emulator for on Linux (W7-INTEROP-4 as OA-38 amends it). Linux writes it with the shared writer Android uses (`esdeSystemsJson`), whose systems are the card's as adopted, so a fallback, which is never stored, cannot reach the file.
- **Android, from the floor:** never replaces a launch tuple it does not know with its own (android_launcher_plan.md §3 item 4).

### 4.9 `ES-DE/nano/systems.linux.json` (retired)

`systems.linux.json` is retired by OA-38: Linux's picks, per system and per game, go in the shared files (4.2, 4.8, 4.10), so a pick made on one platform is the other's too. No build writes it and the contract has no schema for it. No Linux card exists yet, so no card holds one; a reader that finds one ignores it and leaves it alone (rule 1.3).

### 4.10 `ES-DE/nano/emulators.json`

- **Owner:** both (OA-38). Per-game picks that ES-DE has no label for: `{"version": 1, "items": [{"game", "label", "am"}]}`. Schema: `linux/schema/emulators.schema.json`.
- Each platform edits it in place with the same code (`esdeStoreSaveGameEmu`): unknown top-level keys and other games' items survive; the item of the game the user re-picks is rebuilt, or removed when the new pick has an ES-DE label (which then goes in `<altemulator>`). The last pick wins (plan §7.6's rule, made shared).
- `am` names the pick in Nano's emulator catalog (Daijishou's am start arguments), the same catalog on both platforms, so each finds the same entry. A reader loads an item only when its `am` matches its catalog. A platform that cannot run the item's emulator keeps the item and starts the game with its own default (section 5).
- **New field `key`:** the portable key of the same choice `am` describes, written by Android from the floor on (contract A3).
- Linux adds no field of its own to an item: a re-pick on Android rebuilds the item (contract.md C5's `linux` field would be lost).

### 4.11 `ES-DE/nano/names.json` and `ES-DE/nano/index.json`

- **Owner:** Android, until the floor.
- **names.json**, version 1: `{"rom": <absolute path>, "name"}` items. Schema: `linux/schema/names.schema.json`. Every shipped build prunes an item whose `rom` fails `stat`.
- **index.json**, version 2: items `rom`, `box`, `fan` (absolute), `title`, `scraper`, `desc`, `genre`, `players`, `rating`, `date`, `dev`, `pub`, `when` (32-bit), `kept`, `xmiss`, `xwhen`, plus a `journal`. Schema: `linux/schema/index.schema.json`. On a card, shipped builds clear `box` and `fan` unless they start with `<store>/downloaded_media/` or the scrape cache.
- **Neither may hold a platform-only field:** Android rebuilds both from structs.
- **Linux, in compatibility mode** (every card today): reads both, resolving aliases, and never writes either. Titles go to the gamelist `<name>`, which Android already mirrors. Linux's scrape index lives on internal storage, and its art goes to `downloaded_media` by name (***proposed***, W7-INTEROP-7).
- **After the floor** (android_launcher_plan.md §3 item 3): portable keys go in new files beside these (for example `names.v2.json`), and the legacy files are still written in their absolute form for the id in use. A legacy file is never rewritten in place with relative keys (check.md B1). The names of the new files are for the Phase 3 work.

### 4.12 Other files in `ES-DE/nano/`

- `not-in-a-system-folder.txt`: the move's report of favourites, hidden games, titles and emulators it could not place, as tab-separated absolute paths. Informational; readers do not act on it.
- Temporary files: rule 1.2.

### 4.13 `ROMs/<es-de system name>/`

- Folder names are ES-DE system names. Aliases are read (`esdeCanonSystemName`), never written.
- **In-game saves beside the ROM** only for a game whose Linux emulator and Android emulator are the same core or a save-compatible family (OA-21; 4.17). Android's save states and screenshots also sit here (its shipped `retroarch.cfg` has `savestates_in_content_dir`, `savefiles_in_content_dir` and `screenshots_in_content_dir` true).
- **After a Linux launch, nothing but same-core in-game saves appears under `ROMs/`** (plan §6.2, interop row 14).
- `ROMs/androidgames/<name>.app` holds one package name per file (Android only). `ROMs/scummvm/*.scummvm` holds a ScummVM game id on its first line.
- Linux scans by extension, so `.srm`, `.state*` and screenshot files are never listed as games.

### 4.14 `bios/`

The drop folder the BIOS installer offers first, matched case-insensitively at the card root. Both read it; only the user writes it. Installed-file records stay on each device (`nano_bios/installed.json`).

### 4.15 `emulator_config/`, in general

Each emulator's folder holds three classes of data (plan §7.5):

| Class | Where | Rule |
|---|---|---|
| Shared data | `<emu>/<shared subfolders>` | one copy, read and written by both: BIOS, cheats, same-core saves, PPSSPP `SAVEDATA`, ScummVM saves |
| Per platform, on the card | Android: `<emu>/app/` and Android's files directly under `<emu>/`; Linux: `<emu>/linux/` only | each platform reads and writes only its own; the other never deletes it |
| Per device, internal | `/storage/.config/<emu>`, `/storage/.local/share/<emu>` on Linux; app data on Android | never on the card from Linux: RetroArch's main config, core options, remaps, overrides, shader caches, autoconfig, hotkey bindings |

**Settings that travel** (OA-26): for an emulator both platforms run, a settings file on the card is shared, and each platform keeps an override layer for keys that cannot mean the same on the other side. Section 4.18 gives the shape. Key classes:

- **shared:** means the same on both (resolution scale, frame skip, language, per-game options);
- **per platform:** absolute paths, the graphics backend and driver, Android-only input bindings, the memory-stick path;
- **per device:** values tuned to one model's screen or pad. Never on the card.

The class of each key is data, one list per emulator, checked against the emulator's config format. That list is later work.

**Rules for `<emu>/linux/`:**

- It never holds an absolute card path. Android's id-repair pass (`emusync::repoint`) edits text configs three levels deep, including `linux/`, and must find nothing to rewrite there (plan §7.4 rule 6).
- Linux never reads or edits `<emu>/app/`, `Android/`, or Android's RetroArch config files.
- An emulator folder that exists only for Android (Dolphin, Azahar, ARMSX2, hakuX, Vita3K, DraStic, drastic-nano, M64Plus FZ, SeedlessDS, Flycast's `emu.cfg`) is left untouched by Linux (OA-22).

### 4.16 `emulator_config/card.json` (new)

- **Owner:** both. Linux writes it first; Android from the floor (OA-19).
- **Format:** schema `linux/schema/card.schema.json`.

```json
{ "version": 1,
  "aliases": [
    { "id": "9602-C755", "kind": "serial", "platform": "linux", "source": "self", "first": "2026-11-22", "last": "2026-11-22" },
    { "id": "00000000-0000-0000-0000-000000000001", "kind": "android-index", "platform": "android", "source": "found" } ],
  "writers": [ { "device": "anbernic-rg-arc-d", "platform": "linux", "thimsy": "0.1", "contract": 1, "last": "2026-11-22" } ],
  "emulators": { "ppsspp": { "android": "1.20.4", "linux": "1.20.4" } },
  "copies": [ { "from": "flycast/data/dc_boot.bin", "to": "retroarch/system/dc/dc_boot.bin", "by": "linux" } ] }
```

- **`aliases`:** every id the card has been seen under, one entry per id. Each platform adds the id it sees now (`source: self`); Linux also adds the Cube prefixes it finds in the card's files (`source: found`). Entries are objects, not bare strings, so a reader knows each id's kind (***proposed***, W7-INTEROP-6).
- **`writers`:** one entry per device model and platform, for "last used in" in the foreign-card dialog. `device` is multi_device_plan.md's descriptor id, the same on both builds. `contract` is the major of this document the writer implements; it is how a reader learns the floor has written the card.
- **`emulators`:** the version of each shared emulator that last wrote its shared data, per platform, for a warning when the other side's was newer.
- **`copies`:** files one platform copied into another emulator's folder for its own use (Flycast BIOS into `retroarch/system/dc/`, DraStic BIOS under the melonDS DS names). Paths are relative to `emulator_config/`.
- **It is a hint, never a safety gate** (plan §7.7): any build without this contract leaves it stale.
- **Safe for shipped builds:** they ignore it, and `repoint` rewrites an id only when `/emulator_config` follows it, so bare ids are never edited.
- **Where:** `emulator_config/card.json` (OA-19). Linux writes it only where `emulator_config/` already exists or Linux is creating it for its own data, so a card that holds only `ES-DE/` does not gain an `emulator_config/` folder that Android's foreign-card check might react to. On such a card the aliases come from the prefix scan alone (***proposed***, W7-INTEROP-8).

### 4.17 RetroArch and in-game saves (OA-21)

| Path | Class | Linux |
|---|---|---|
| `retroarch/system/` | shared BIOS | `system_directory` in the per-launch append config |
| `retroarch/cheats/` | shared | `cheat_database_path` in the append config |
| `retroarch/retroarch.cfg`, `config/`, `playlists/*.lpl`, `saves/`, `states/`, `screenshots/`, ... | Android | never read or written. RetroArch `.lpl` playlists stay per platform (OA-23) |
| `ROMs/<system>/<game>.srm` | shared, same core only | `savefiles_in_content_dir = "true"` for that launch |
| `retroarch/linux/saves/<system>/` | Linux | `savefile_directory`, with `savefiles_in_content_dir = "false"`, when the cores differ |
| `retroarch/linux/states/<system>/` | Linux | `savestate_directory`, with `savestates_in_content_dir = "false"`, always |

- **No save converters** (OA-21). The share-or-sort choice is made per launch: Linux compares the core it will run with the one Android would run for that game (its per-game choice, else its system default), through the family table's `saveCompatible` column. Same core, or a listed save-compatible family: the save is shared beside the ROM. Otherwise, or when either is unknown: Linux's save goes to `retroarch/linux/saves/<system>/`.
- This replaces `linux_handheld_plan.md` §6.2's `sort_savefiles_enable` (a per-core folder beside the ROM): OA-21 puts the separate saves under `linux/`, which also keeps `ROMs/` clean.
- Files and folders a core keeps itself (`yabasanshiro/backup.bin`, `opera/per_game/`, Sega CD `scd_*.brm`, `mame/` without `cfg/`, `fbneo/`, `cdata/`) follow the same choice, because RetroArch hands the core the save directory before it starts.
- **Save states never cross platforms** in v1 (OA-21).
- Linux screenshots go to internal storage (***proposed***, W7-INTEROP-10).
- The append config is written by Nano to `/run/thimsy/` at each launch; nothing on the card is rewritten per launch.

### 4.18 PPSSPP (OA-26)

| Path under `emulator_config/ppsspp/` | Class |
|---|---|
| `PSP/SAVEDATA/`, `PSP/Cheats/`, `PSP/TEXTURES/` | shared data |
| `PSP/SYSTEM/ppsspp.ini`, `controls.ini`, `<GAMEID>_ppsspp.ini` | the shared settings files. Android writes them in full, its per-platform values included |
| `PSP/PPSSPP_STATE/` | Android's save states |
| `PSP/flash0/font/` | shared BIOS font slot |
| `PAUTH/`, `EXDATA/` | Android |
| `linux/PSP/SYSTEM/<same names>` | Linux's override layer: per-platform keys only (***proposed*** path, W7-INTEROP-11) |
| `linux/PSP/PPSSPP_STATE/` | Linux's save states |

**At a Linux launch** (***proposed*** mechanism, W7-INTEROP-11):

1. The effective `ppsspp.ini` is built on internal storage: the shared file, then Linux's override layer, then the device's own keys.
2. PPSSPP runs with an internal memory stick whose `SAVEDATA`, `Cheats` and `TEXTURES` are the card's folders (bind mounts, since the card holds no links).
3. After the game, changed shared keys are written back to the shared file by editing only their lines. Changed per-platform keys go to the override layer. Android's per-platform values in the shared file are never touched.

Unknown keys stay where they are.

### 4.19 ScummVM (OA-26)

- `emulator_config/scummvm/saves/` is shared (Android's `savepath`), if both sides derive the same game target ids (to test).
- **Settings:** today the card's only copy of Android's `scummvm.ini` is the app mirror `scummvm/app/files/scummvm.ini`, which Linux never reads. So OA-26's shared ScummVM settings need a new shared file, `emulator_config/scummvm/scummvm.ini`, holding shared keys only, written by Linux and by Android from the floor. Linux's per-platform keys (paths, graphics) go in `scummvm/linux/scummvm.ini` (***proposed***, W7-INTEROP-12; owner question 3).
- Game targets whose `path` is a `/saf/` or `content://` path are Android-only. Linux resolves a target's game files from `ROMs/scummvm/`.

### 4.20 Systems with no emulator on a device (OA-22)

- On Linux, Android's "Android Games" system is replaced by PortMaster's Ports. Ports live on internal storage, not the card: they need POSIX files (plan §7.4).
- GameCube, Wii, 3DS, PS2, Switch and Vita are hidden on devices that cannot run them. Their games, saves and settings on the card are left untouched for Android.
- A system appears when the image ships an emulator for it and the device profile says the device can run it, so no card format changes when stronger devices arrive.

### 4.21 `Thimsy/settings.ini` (OA-20, OA-32)

- **Where:** `<card>/Thimsy/settings.ini`, beside `ES-DE/` and `ROMs/`, on the store's volume (OA-32 Q1). With no card, `/storage/emulated/0/Thimsy/settings.ini`.
- **Owner:** both. The full design is `portable_settings_plan.md` §3 and §4. Its dialect is the contract:
  - UTF-8 (BOM allowed), LF or CRLF kept as found.
  - Whole-line `#` or `;` comments only; `[section]` headers; keys match `[a-z0-9_.]+`, written `key = value`.
  - Booleans `on`/`off` when Nano writes, any of `on off true false yes no 1 0` read.
  - Paths in form (d).
  - At most 1 MiB, and 4 KiB per line.
  - `[meta] format = 1` is the file's major; adding keys never bumps it.
  - Nano rewrites only the lines whose values change. Unknown keys, sections, comments and order are kept byte for byte. A file Nano creates lists unset keys as commented templates and ends with a trailer comment.
- **Sections:** shared sections; `[device.<model>]` for screen-dependent keys, with multi_device_plan.md's descriptor ids; `[android]` and `[linux]`, which travel between devices of that platform and are kept unread by the other.
- **Never in the file** (OA-32 Q5, Q6 and its autonomous answers): hotkey buttons, RGB light settings, slide actions, scraper accounts and API keys, SSH and any password, Kiosk Mode, the device's system name, time zone and automatic time. A key whose class is device or secret is never written.
- **Precedence:** a card whose settings differ from the device's: Nano asks first ("Use this card's settings?") (Q2). The same key changed on the device and in the card file: **the card's value wins** (Q3). A device edit made while the card was out is written to the card only for keys the card file did not change since this device last saw it.
- **The theme** travels here (OA-32 Q4), and is mirrored into `es_settings.xml` under 4.1's rule.
- **Rollout:** off by default in the first Android build that carries it, on by default on Linux.
- Shipped Android builds never read it.
- Key names come from `tools/nano-tree/prefs.json`, later work.

### 4.22 Music playlists (OA-23)

- Music files and any `.m3u` or `.m3u8` already in the user's music folders travel as they are; Nano rebuilds those playlists from each scan.
- **User-created playlists move to the card** as `.m3u8` files: UTF-8, first line `#EXTM3U`, optional `#EXTINF` lines, one entry per line, each entry a path relative to the playlist file's own folder with forward slashes. No absolute paths, no tokens, no URLs. A PC can edit them.
- **Where:** `<card>/Thimsy/playlists/<name>.m3u8`, beside `settings.ini` (***proposed***, W7-INTEROP-13). The playlist's name is its file name, cleaned of the characters FAT forbids.
- The device's `nano_music.json` stays as a cache of the scan (track metadata), not the playlists' home.
- Linux hides the music player until it has a media decoder (OA-23, OA-24); the files stay portable meanwhile.

### 4.23 `Thimsy/emulators/*.json`

User emulator definitions to import (`user_emulators_plan.md`; plan §6.4's schema 2 with Linux blocks). Neither platform acts on them straight from the card. Each imports through a confirm screen into internal storage and records the origin. Their format is that plan's, not this document's.

## 5. Emulator choice

**Portable keys** (the schemas' `emu_key`):

- `core:<libretro name without suffix>`, normalised through an alias map (`mupen64plus_next_gles3` is `mupen64plus_next`).
- `emu:<id>` for a standalone emulator, a platform-neutral id: `emu:ppsspp`, `emu:scummvm`, `emu:flycast`, `emu:dolphin`, `emu:azahar`, `emu:melonds`, `emu:drastic`, `emu:m64plus-fz`, `emu:armsx2`, `emu:hakux`, `emu:vita3k`, `emu:seedlessds`.
- `family:<name>` for a group of save-compatible members (`family:melonds`: WatermelonDS and the melonDS DS core).

**The family table** is shared data: generated from ES-DE's `android` and `linuxarm` tables at one pinned ES-DE commit, plus a hand-kept alias and `saveCompatible` list. It maps labels, `coreSo` names and Android packages to keys. A pair of cores is listed as save-compatible only after its round trip passes (plan §6.2).

**Where the picks are** (OA-38): the same place on both platforms, so the last pick wins, whichever platform made it.

| | Per system | Per game |
|---|---|---|
| Both | `systems.json` launch fields, with `<alternativeEmulator>` | `<altemulator>`, else `emulators.json` (`am`, and `key` from the floor) |

- **A pick is written with ES-DE's label** for the emulator: the label of the ES-DE command that starts it, from ES-DE's table (`NanoEsdeEmulators.inc`, ES-DE's `android` table at a pinned commit). Core labels are the same in ES-DE's `android` and `linuxarm` tables, so a Linux pick of a core carries the label the ES-DE app on either platform reads.
- **What is written is the pick, never a fallback.** No writer changes a tag, an item or a system's launch fields except for the game or system the user just picked for (or the ES-DE app picked for, 4.8), and other games' labels and items are left as they are. Android's systems heal is the one exception still open (4.8).
- **Nano's catalog is the same on both platforms** (`nano_xmb/daijishou`), so a pick is stored in Nano's own terms (`coreSo` as the Android core file name, `am` as the catalog's) and read back the same way on the other platform.

**Resolution, per launch, on each platform:**

1. The game's pick (`<altemulator>`, else its `emulators.json` item), else the system's (`systems.json`'s launch fields, kept in step with `<alternativeEmulator>`).
2. If this platform cannot run that emulator (the core is not in the image, the app is not installed, or this platform cannot start apps of that kind), this platform's image default for the system, for this launch only. The shared pick stays, so the platform that made it still uses it.
3. If the default cannot run either (or there is none) and the emulator was the game's own pick, the system's emulator, when this platform can run it, for this launch only, as such a game started before OA-38 kept its pick.
4. Otherwise: "Emulator not installed", shown, with every stored value kept.

Nothing a fallback chooses is stored: not on the card, not in the game's or system's pick, and not in Recently Played, which keeps the emulator the game was recorded with.

**Each platform's image defaults are stored on the device** (OA-38, like OA-35's theme defaults): on first run Nano writes the image's default emulator for each system into the device's store (`nano_emu_defaults.json` in Nano's device state, beside `nano_systems.json`), and a later release that changes a default reaches new installs only. Android's come from the image's own table (the build's `nano_systems.json`, else Nano's built-in systems); Linux's from Linux's table (`nanoplat::Emulators::imageDefaults`, `platform/linux/Emulators.cpp`), cores Linux builds; a launch uses a default only when the image ships its core. Fallbacks read only this store, never the card.

**Whether this platform can run an emulator** is the platform's answer (`nanoplat::Emulators`): Android's firmware looks for the core in RetroArch's cores and the app in the package list; Linux looks for the core in the image's core folder and fails closed for every app (it starts none yet).

**The ES-DE app and a label from the other platform.** A label only one of ES-DE's tables has (an Android app's "(Standalone)" label read by ES-DE on a PC, say) is not one the ES-DE app can run there: it shows its usual notice that the game's or system's alternative emulator is invalid, and uses its default. That is ES-DE's own handling, documented here and not worked around: the label is kept, so the platform that made the pick still uses it.

**Later:** the family table and portable keys (`key`, `family:`) map one platform's core to the other's save-compatible member (`mupen64plus_next_gles3` on Android, `mupen64plus_next` on Linux) instead of falling back to the default.

## 6. Direction of travel

| From | To | Arrives | Does not arrive (v1) |
|---|---|---|---|
| Android card | Linux | the library, favourites, hidden games, play counts, Recently Played, collections, media, titles, ES-DE settings, the theme (where it exists on the card), `settings.ini`, music playlists, BIOS, same-core saves, PPSSPP saves and shared settings, ScummVM saves | save states, different-core saves, RetroArch settings and `.lpl` playlists, Android app preferences, systems with no Linux emulator (kept, hidden) |
| Linux card | Android | the same | the same, mirrored; Android does not use `linux/` or Linux's override layers; emulator picks are shared (section 5) |
| Either | Thimsy Home, mainline ES-DE | ES-DE's own files (OA-20) | Nano's own files |

## 7. What each side must tolerate

- **Both:**
  - unknown files, folders, tags, keys and fields (kept);
  - labels, picks and keys they cannot run (kept; the game starts with this platform's default, section 5);
  - every legacy path form;
  - a missing `format.json`, `card.json` or `settings.ini`;
  - gamelists and media from mainline ES-DE;
  - temporary files left by a pull.
- **Linux additionally:** never reads or edits Android's `app/` mirrors, `Android/`, or Android's RetroArch config files; ignores the `<command>` lines in `custom_systems`; treats Android packages as not installed (fail closed).
- **Android additionally (from the floor):**
  - never deletes `linux/` folders or the `key` fields;
  - keeps a launch tuple it does not know;
  - resolves forms (c) and (d);
  - treats an id in `card.json`'s aliases as this card;
  - skips `linux/` in its emulator_config move;
  - shows Linux-aware wording in the foreign-card dialog.

## 8. Tests

**Now:** `linux/tests/test_card_contract.py` validates every JSON file in `linux/tests/fixtures/card/` against its schema and checks that each schema refuses what this contract forbids: path aliases, absolute or `..` copy paths, scraper accounts in `systems.json`, relative keys in `names.json` and `index.json`, emulator keys that are paths or packages. It also checks the rules between `android-cube/` (a card as the Cube leaves it) and `linux-written/` (the same card after a Linux visit):

- `names.json`, `index.json` and `es_settings.xml` are byte-identical;
- `systems.json`'s launch fields change only for the systems Linux picked, and each such pick carries its ES-DE label (OA-38);
- every label on either card is one ES-DE's table has for that system;
- labels and `emulators.json` items Linux did not pick survive, and a Linux game pick replaces the game's older pick (its label, or its item);
- `systems.linux.json` is on neither card and has no schema;
- unknown tags survive;
- play counts only grow;
- collection lines are kept verbatim;
- nothing Linux owns holds an absolute card path;
- every Cube prefix in the card's files is in `card.json`'s aliases;
- PPSSPP's shared file keeps Android's per-platform values;
- `settings.ini` follows the dialect and holds no device-only key;
- playlists are relative `.m3u8`.

The vectors are hand-written: no build produced them.

**Later** (interop-2026-11-26/contract.md §4, plan Phase 3):

- T1: the shared parsers load and save each vector unchanged; Linux-only fields survive the Android writers compiled for the host.
- T2: the host Nano in the e2e jail mounts the Cube vector at its serial and replays the user actions above.
- T3: the same on loop-mounted exFAT and FAT32 images.
- T4: the device round trips on the ARC-D with the one spare card (OA-18).

## 9. Known gaps and where each fix lands

From INTEROP-AUDIT.md and its reports. "Android-surface" means the change alters what the Android build does or writes: an H.3 verified move, the AOSP build and the owner's smoke test before nano-144. Every fix in this table is later work; none is made by this document. Line numbers are approximate.

| Gap | Severity | Fix lands in | File and function |
|---|---|---|---|
| G1: Linux never enters store mode by itself | CRIT | Linux | `nano/platform/linux/Volumes.cpp` (a default-store hook), setting `persist.gammaos.nano.esde.dir=<cardRoot()>/ES-DE` at first run; shared `NanoEsdeStore.cpp` `esdeDataDir`/`esdeStoreMode` (120-132) read it. Not the internal path: `esdeStoreFollowCard` skips `emulated` (check.md C-1) |
| N1: every libretro launch from the XMB refused on Linux | CRIT | Linux, via OA-4 | `NanoMenuXmb.cpp` `NanoMenu::coreSoExists` (3345) to `nanoplat::Emulators::coreInstalled` (W8-EMU); Linux answers from the image's core folder |
| G2, C-2, C-3: Cube paths do not resolve; names.json pruned, index.json art cleared | CRIT | Linux, then Android | alias resolution in an extended `nanoplat::Volumes` (`canonicalStorePath`, `cardAliases`); `NanoMenuScraper.cpp` `saveRomNameOverrides` (prune) and `loadScrapeIndex` (`artOk`) resolve aliases; Linux in compatibility mode (4.11) |
| G2: id repair ignores the Cube form | HIGH | shared, Android-surface | `NanoEmuSync.cpp` `emusync::isVolumeId` and `repoint` learn the 36-character form (check.md B9) |
| G5, C-4: Linux deletes `<alternativeEmulator>`, rebuilds `<altemulator>` with Android `am`; Android per-game `am` choices applied on Linux then fail | HIGH | shared, Android-surface | OA-38 makes the shared writes the rule: `esdeStoreSaveSystemEmus` and `esdeStoreSaveGameEmu` write the last pick on both platforms, with ES-DE's label, and touch only the picked game or system; a pick this platform cannot run starts with its image default (the launch sites in `NanoMenuXmb.cpp` and `NanoMenuOverlay.cpp`; W8-EMU) |
| C-5, G10: `systems.json` launch tuples ping-pong | HIGH | shared | OA-38: a tuple is the last pick, from either platform; a platform that cannot run it falls back per launch and never rewrites it (4.8), so a tuple changes only for a pick (in Nano on either platform, or the ES-DE app's label adopted), except Android's once-per-config systems heal, still open (4.8). `esdeStoreAdoptSystems` already takes a tuple from Nano's catalog, which both platforms share |
| G4, N2: no Linux fallback; Linux defaults are Android cores; PPSSPP and ScummVM refused | HIGH | Linux, and shared | the fallback to the device's stored image defaults (section 5); Linux's defaults from `Emulators::imageDefaults` (`platform/linux/Emulators.cpp`) instead of `kXmbSystemDefs`; PPSSPP and ScummVM in `Launcher.cpp` `argvFor` are later work |
| G3, C-6: Linux RetroArch saves to ext4, states unset | HIGH | Linux | `Launcher.cpp` writes `/run/thimsy/ra-append.cfg` (4.17) and passes `--appendconfig` |
| G7: BIOS on the card unusable on Linux | HIGH | Linux, via OA-4 | `NanoBiosWorker.cpp` slot-base check (557-575) and `NanoBiosUtil.cpp` `canonical` behind the platform; `NanoRaConfig.cpp` `retroarchConfigFile` gains Linux's system folder; copy rules for Flycast and DraStic BIOS (4.16 `copies`) |
| G6, N5: `packageInstalled` fails open | HIGH | Linux, via OA-4 | `NanoMenuXmb.cpp` `NanoMenu::packageInstalled` (3357) to `Emulators::appInstalled` (W8-EMU), failing closed on Linux, so the emulator_config move and app-mirror sync find no Android app there |
| G8, G9: paths Linux writes do not resolve on the Cube; the Cube id moves | HIGH | Android | Android reads `card.json` aliases (A4, A5); vold names volumes by serial (D20(c), OA-19) |
| G12: foreign-id writes land on ext4 | MED | shared and Linux | shared `makeDirs`/`sdMkdirs` refuse a foreign id (`Volumes::isForeignId`); thimsy-session keeps `/storage` from gaining unknown names |
| G14: theme and downloaded themes do not travel | MED | shared, Android-surface | `NanoMenuEsdeMenu.cpp` theme choice into `settings.ini` (OA-32) and `es_settings.xml` (4.1); theme downloads to `<store>/themes` |
| G13: videos static on Linux | MED | Linux | the OA-24 media interface replacing `nano/compat/stubs/src/ndk_media.cpp` |
| G15: PPSSPP and ScummVM data not wired on Linux | MED | Linux | `Launcher.cpp` package launches plus the OA-26 launch-time merge (4.18, 4.19) |
| G16: kernel does not pin the FAT and NLS options | MED | Linux | `linux/tools/thimsy-import/kernel-critical.conf`: `CONFIG_EXFAT_DEFAULT_IOCHARSET="utf8"`, `CONFIG_VFAT_FS`, `CONFIG_NLS_UTF8`, `CONFIG_NLS_CODEPAGE_437` |
| G17: two-slot Linux prefers the boot card | MED | Linux | `nano/platform/linux/Volumes.cpp` `cardRoot` (103-116): OA-25's ask-and-remember |
| OA-23: user playlists only on the device | MED | shared, Android-surface | `NanoMenuMusic.cpp` `loadMusicConfig`/`saveMusicConfig` (103-241): playlists read from and written to `Thimsy/playlists/*.m3u8` (4.22) |
| OA-20, OA-32: settings are device properties | MED | shared, Android-surface | the Prefs facade and `settings.ini` engine (`portable_settings_plan.md` §7) |
| G24: names.json and index.json renamed without fsync | LOW | shared, Android-surface | `NanoMenuScraper.cpp` `saveRomNameOverrides`, `saveScrapeIndex`: the shared write helper (1.2) |
| N3: temp files left after a pull | LOW | shared | readers skip them (1.2); a stale one is removed after the final file parses |
| N4: Linux refuses a `0000-0000` serial | LOW | none | kept as the rule (1.1); the installer gives every card it formats a non-zero serial |
| G11: the move can stamp an internal ROMDirectory | LOW (Linux) | shared, Android-surface | `NanoSdMove.cpp` (470-475, 780) |
| G18: `<lastplayed>` is local time | LOW | none | ES-DE's format; kept |
| G19: starts with the card out stay on the device | LOW | none | Q6: no journal on the card |
| G20: duplicate PSP folder entries | LOW | shared | `NanoEsdeStore.cpp` gamelist edit (399-404) matches `<folder>` entries |
| G21: `custom_systems` holds Android commands | LOW | none | harmless; readers ignore commands (4.6) |
| G22: Android-only files | LOW | rule | 4.15 and section 7 |
| G23: fsck before mounting | LOW | done in design | OA-13: only when the filesystem is dirty |

## 10. Choices this document made (proposed) and questions for the owner

Proposed here, not decided by the owner:

- **W7-INTEROP-1:** this file lives at the repository root, as the plans list it.
- ~~**W7-INTEROP-2**~~ and ~~**W7-INTEROP-3**~~: replaced by the owner's OA-38 (emulator picks are shared; the last pick wins).
- **W7-INTEROP-4:** Linux writes `systems.json` only with every system's launch fields as it read them, except the systems the user picked on Linux (as OA-38 amends it).
- **W7-INTEROP-5:** `format.json` stays at major 1 until the floor.
- **W7-INTEROP-6:** `card.json` aliases are objects, and a writer's `contract` field marks the floor.
- **W7-INTEROP-7:** in compatibility mode, Linux's scrape index stays internal.
- **W7-INTEROP-8:** `card.json` is not written to a card that holds only `ES-DE/`.
- **W7-INTEROP-9:** `ThemeAspectRatio` is never written.
- **W7-INTEROP-10:** Linux screenshots stay internal.
- **W7-INTEROP-11:** PPSSPP's Linux override layer at `ppsspp/linux/PSP/SYSTEM/`, with the merge-and-split launch.
- **W7-INTEROP-12:** a new shared `scummvm/scummvm.ini`.
- **W7-INTEROP-13:** playlists in `Thimsy/playlists/`.
- **W7-INTEROP-14:** form (d) is written `/storage/%CARDID%/<rel>`; `%CARDID%/<rel>` is also read.

Questions only the owner can answer:

1. ~~Should a choice made on Linux ever change Android's choice?~~ Answered by OA-38: yes, the last pick wins on both platforms.
2. ~~Where should Linux's per-game choices live?~~ Answered by OA-38: in the shared files (`<altemulator>` and `emulators.json`); `systems.linux.json` is retired.
3. ScummVM's shared settings: a new shared `scummvm/scummvm.ini` (W7-INTEROP-12), or may Linux import read-only from the Android mirror `scummvm/app/files/scummvm.ini`?
4. Where should user music playlists live: `Thimsy/playlists/` (W7-INTEROP-13) or inside a music folder the user chose?
