# Thimsy library format: the card contract

Version 1 of the contract, written 2026-11-26, revised 2026-11-27 for the owner's rethink of config portability (OA-46 to OA-54). This is the document `android_launcher_plan.md` §3 and `linux_handheld_plan.md` §7 call `LIBRARY-FORMAT.md`. It says what every file and folder on a Thimsy game card means, who may write it, and how each side treats what it does not know. Android Thimsy and Linux Thimsy both implement it. Tests are written against this document, not against either build.

Where this comes from:

- The owner's answers OA-8, OA-19, OA-21, OA-22, OA-25, OA-35, OA-37 and OA-46 to OA-54, and the interop questions decided autonomously (Q4, Q6, Q12), in `docs/linux/DECISIONS.md`. OA-46 to OA-54 replace OA-20's card file, OA-23, OA-26, OA-32, OA-38, OA-39 and OA-40 where they differ, and OA-53 replaces OA-21's per-platform folders.
- `per_device_config_plan.md`, the design for OA-46 to OA-54. Section numbers written "plan §n" below are that plan's, unless another plan is named.
- The interop audit, `docs/linux/INTEROP-AUDIT.md`, and its raw reports in `docs/linux/interop-2026-11-26/` (a 2026-11-26 snapshot; where they differ from OA-46 to OA-54, this document wins).
- `user_emulators_plan.md` §1 and §1a, the emulator definition (OA-54, 4.27).
- `linux_handheld_plan.md` §6.2 to §7.10, superseded in part by OA-46 to OA-54.

**What changed on 2026-11-27.**

- Emulator settings and the launch configuration (which emulator, core or app launches each system and each game, `systems.json`'s launch fields included) are per device (OA-46). Each device's set lives in `emulator_config/devices/<id>/` (4.24, 4.25). No settings are shared between devices, and no pick made on one device changes another's.
- Emulated system storage (Vita3K's `ux0`, Eden's NAND and SD, Dolphin's Wii NAND, Azahar's NAND and SD, PPSSPP's memory stick, PS2 memory cards) is shared on the card, the same for every device, with no device mirror; the emulator's settings stay per device (OA-52, 4.15).
- In-game saves, save states and BIOS are shared on the card by every device and both platforms, with no device mirror. Different cores' files stay apart by name or folder, and a state is offered only to the core that made it (OA-53, 4.17). The per-platform folders `retroarch/linux/` and `<emu>/linux/` are withdrawn; no card has them.
- Every per-emulator rule here (which files are settings, which are shared storage, how an emulator is pointed at them, its launch entries) comes from emulator definition files, the shipped emulators included (OA-54, 4.27). Not implemented yet.
- Thimsy's own settings stay on the device (OA-47). `Thimsy/settings.ini` is gone (4.21).
- The theme is the one exception: the theme switches and each theme's settings are on the card in `Thimsy/theme.json`, shared by every device (OA-51, 4.26).
- Playlists, box art, media, gamelists and collections are on the card only, with no device mirror (OA-48, OA-49).
- PortMaster is never on the card (OA-50, 4.20).
- `systems.linux.json` is retired (4.9).

**Status.** Most of this describes what the Android build already writes. The new parts (`card.json`, `format.json`, the per-device sets, `picks.json`, `theme.json`, the playlists, the per-family save and per-core state folders, the emulator definitions) are not implemented by either build yet. Section 9 lists every known gap and where its fix lands. Anything marked ***proposed*** is a choice made while writing this contract or `per_device_config_plan.md` that the owner has not decided; section 10 lists them.

**Machine-checkable parts.** The JSON files have JSON Schemas (draft-07, the subset `linux/tools/thimsy-import/thimsy_import/devicejson.py` validates):

| File on the card | Schema |
|---|---|
| `emulator_config/card.json` | `linux/schema/card.schema.json` |
| `emulator_config/devices/<id>/device.json` | `linux/schema/emu-set.schema.json` |
| `emulator_config/devices/<id>/picks.json` | `linux/schema/picks.schema.json` |
| `Thimsy/theme.json` | `linux/schema/theme.schema.json` |
| `ES-DE/nano/format.json` | `linux/schema/format.schema.json` |
| `ES-DE/nano/systems.json` | `linux/schema/systems.schema.json` |
| `ES-DE/nano/emulators.json` | `linux/schema/emulators.schema.json` |
| `ES-DE/nano/names.json` | `linux/schema/names.schema.json` |
| `ES-DE/nano/index.json` | `linux/schema/index.schema.json` |

`linux/tests/test_card_contract.py` validates the test vectors in `linux/tests/fixtures/card/` against them and checks the rules between an Android-written card and the same card after a Linux visit (section 8).

Schemas of Thimsy's new files (`card.json`, a set's `device.json`, `picks.json`, `theme.json`, `format.json`) are strict at the top: they describe exactly what a writer of this major version emits. `picks.json` entries and `theme.json`'s keys stay open, so a newer writer's fields survive. Schemas of files already in users' hands (`systems.json`, `emulators.json`, `names.json`, `index.json`) allow unknown fields, because real cards carry them. In code, no reader ever refuses a file for an unknown field (rule 1.3).

## 1. Rules for every file

### 1.1 The card

- **One `games` volume, exFAT recommended, FAT32 accepted.** NTFS, ext4, f2fs and btrfs are not supported for interop (D8, `linux_handheld_plan.md` §7.8).
- **Regular files and folders only.** No symbolic links, device nodes or permission bits. A link an emulator needs lives on the device's internal storage and points at the card (`linux_handheld_plan.md` §7.4).
- **Names** are UTF-8 and avoid `: * ? " < > |` and trailing dots. No two names differ only by case.
- **The volume id must be usable.** Linux refuses an all-zero serial (`0000-0000`), as stock Android does. Two mounted cards with the same serial (a cloned card): neither is used, and the user is asked to remove one (decision 17).
- **Two cards that both hold a library** (OA-25): the first time, Nano asks which one to use and remembers the answer for that pair of card ids, on the device. It can be changed in Settings. With one library present it is used without asking. Only the chosen card's sets and theme are used; the other card is not read or written (plan §5.4).

### 1.2 Writes

- **Every structured file** (XML, JSON, INI, playlists) and every file a set sync writes is written to a temporary name in the same folder, `fsync`ed, renamed over the old file, and then the folder is `fsync`ed (`linux_handheld_plan.md` §7.8). The temporary names in use are `<name>.nanotmp`, `<name>.tmp` and `<name>.tmp.<pid>`.
- **Readers ignore temporary files.** A reader may remove a stale one only after the final file has parsed (`portable_settings_plan.md` §4.5).
- **An unreadable file is never overwritten.** A writer that cannot parse the current file leaves it as it is and logs why. Nano's writers already do this for gamelists, `emulators.json`, `names.json`, `index.json` and the music library; `picks.json` and `theme.json` follow the same rule (plan §5.3).
- **One device at a time.** Nothing writes the card from two devices at once. Unclean removal is the failure the format is designed for.

### 1.3 Unknown data is kept

- **Unknown files and folders** are left alone, on both sides. Another device's set (4.24) is unknown data to every other device.
- **Unknown XML tags** in gamelists, and unknown keys in `es_settings.xml`, are kept when the file is rewritten.
- **Unknown JSON fields** are kept by every writer that edits a file in place. Android rebuilds `systems.json`, `names.json` and `index.json` from its own structs, so a field only Linux knows does not survive an Android save there. That is why Linux puts nothing of its own in those files (sections 4.8 and 4.11).
- **Emulator settings files are copied as bytes.** A set sync never parses them (4.24). Where Nano edits one line of an emulator's config (RetroArch's hotkey provisioning, BIOS paths), every other line is kept byte for byte.
- **Values a side cannot use** (an emulator label or pick with no emulator on this device, a system with no emulator here, a path to a volume that is not in, a theme set that is not installed) are kept and shown as unavailable, never deleted.

### 1.4 Compatibility with builds already in users' hands

Shipped Android builds never check a `version` field (INTEROP-AUDIT.md). So:

- **Changes are additive only.** A new file, a new field or a new folder. Never a new meaning for an existing field. `devices/`, `slot-kept/`, `conflicts/`, `retroarch/thimsy-older/`, `retroarch/.thimsy-run.json`, `theme.json`, the playlists and the folders `retroarch/saves/<family>/<system>/` and `retroarch/states/<core>/<system>/` are new; shipped builds leave them alone (plan §6.3).
- **Labels are kept.** No writer removes or rewrites an existing `<altemulator>` or `<alternativeEmulator>`. From the per-device change on, neither build writes them at all; Android reads them once, at migration (***proposed***, plan §2.3, owner question 5). Older builds and the real ES-DE app read only these labels, and they stay valid.
- **`names.json` and `index.json` stay in their absolute form** until the Android compatibility floor (Q12). Relative keys would be pruned or cleared by every shipped build (check.md B1, B2).
- **The ROMDirectory re-root rule stays "only when the stored folder is missing"**, widened only to "or the stored id is a known alias of this card" (check.md B6).
- **Theme keys in `es_settings.xml`** are written only for a theme that exists in `<store>/themes` (4.1).

**The compatibility floor** is the first Thimsy Android release built after the AOSP job that carries this contract's Android changes (Q12), which is also the first with the per-device sets (plan §6.3). Until a card has been written by that release, Linux runs in compatibility mode: it never rewrites `names.json` or `index.json` and keeps every Android label. A card has been written by the floor when its `card.json` has an Android writer with `contract` 1 or more (***proposed***, W7-INTEROP-6).

### 1.5 JSON values

Android's JSON writer (`NanoJson`) prints numbers with `%.6g` and turns infinity into `null` and a lone surrogate escape into U+FFFD. So JSON files on the card hold only strings, booleans and whole numbers below 10^15 (`linux_handheld_plan.md` §7.6). `theme.json` holds strings only. Dates are strings, `YYYY-MM-DD`, and are informational only: handheld clocks are often wrong, so nothing is ordered by them.

### 1.6 What readers skip

`Android/`, `LOST.DIR`, `System Volume Information`, `.Trashes`, `.Spotlight-V100`, names starting with `.` (macOS `._*` files included) except Thimsy's own `.previous/` and `.thimsy-slot` (4.24), `.thimsy-legacy.json` and `.thimsy-run.json` (4.17), and the temporary names in 1.2. Syncthing markers, if Android Syncthing shares a card folder, are left alone.

## 2. Identity and paths

### 2.1 Volume ids

| Who | The card is `/storage/<id>` with `<id>` = | Notes |
|---|---|---|
| Linux Thimsy | the FAT or exFAT serial, `XXXX-XXXX`, upper case | thimsy-session mounts it there (`Blkid.cpp`, `VolumePolicy.cpp`) |
| Stock LineageOS 21 | the same serial | paths match Linux's |
| Thimsy Android (GammaOS Next's vold) | a mount-order index, `00000000-0000-0000-0000-NNNNNNNNNNNN` | not a property of the card: a USB drive mounted first, or another volume on the card (OA-2's FAT README entry), can shift it (`linux_handheld_plan.md` §7.3) |

OA-19 decides both fixes: Linux keeps an alias list (section 4.16) and resolves Cube paths through it, which works with every card already in use; and a later Thimsy Android build names public volumes by serial (D20(c)), so new paths match everywhere.

A device's own id (the unit, not the card) is a different thing: 4.24.

### 2.2 Path forms

Best first. A writer uses the first form the file allows.

| Form | Example | Used by |
|---|---|---|
| (a) relative to the owning folder | `./Crash Bandicoot (USA).chd` | gamelist `<path>`; playlist entries |
| (b) `%ROMPATH%/<system>/<rel>` | `%ROMPATH%/psx/Crash Bandicoot (USA).chd` | collections; `custom_systems` paths |
| (c) a game key, `<ES-DE folder>/<rel>` | `n64/Mario Kart 64 (USA).z64` | `picks.json` `games`; `emulators.json` (legacy) |
| (d) `%CARDID%` for the store's own volume | `/storage/%CARDID%/ROMs/psx` | `systems.json` scan sources |
| (e) absolute, `/storage/<id>/...` | `/storage/9602-C755/Homebrew/demo.nes` | only where the file's format needs an absolute path (ES-DE's ROMDirectory and collection lines outside ROMDirectory), and `names.json` and `index.json` until the floor |
| by name, no path | `downloaded_media/psx/covers/<name>.png` | media |

**Rules:**

- **Form (d) is spelled `/storage/%CARDID%/<rel>`**, as `systems.json` already writes it. Readers also accept `%CARDID%/<rel>`, the spelling `portable_settings_plan.md` and contract.md C1 use (***proposed***, W7-INTEROP-14).
- **Nothing new is written as an absolute path when another form fits.** Thimsy's own files (`card.json`, `format.json`, a set's `device.json` and `picks.json`, `theme.json`, the playlists) never hold one. Emulator settings files in a set are the emulator's own bytes and may (4.24).
- **Every reader accepts every form**, and these legacy spellings of (e), read-only: `/storage/<serial>`, `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN`, `/mnt/media_rw/<id>`, `/sdcard`, `/data/media/0`, `/storage/emulated/0`.
- **An id the card's alias list names is this card.** A path `/storage/<alias>/...` found in the card's own files resolves to the card. When `card.json` is missing, Linux builds the list from the `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN/` prefixes it finds in the card's own files (D20(b)): `es_settings.xml`, the collections, `nano/*.json` and the text configs in `emulator_config` (three levels deep, skipping `app/`).
- Limit: the Cube's `...0001` is the same string for every card first mounted on a Cube, so it identifies "this card" only for paths found in this card's own files. It is never used to find a card.

### 2.3 Foreign ids

A path to an id that is neither mounted nor an alias is a missing volume. Nothing creates folders under it. On Linux, `/storage/<id>` is a directory on the ext4 data partition, so a `mkdir -p` there would quietly write to internal storage (G12): shared code refuses, and adopted mount points are immutable (`linux_handheld_plan.md` §7.3).

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
    nano/systems.json                   the shared system list (4.8)              both; launch fields kept as read
    nano/systems.linux.json             retired, never written (4.9)              none
    nano/emulators.json                 legacy per-game picks (4.10)              shipped Android builds
    nano/names.json, nano/index.json    titles, scrape index (4.11)               Android until the floor
    nano/not-in-a-system-folder.txt     the move's report (4.12)                  the move
  ROMs/<es-de system name>/             games; saves shipped builds left (4.13)   user; shipped builds
  bios/                                 BIOS drop folder (4.14)                   user
  emulator_config/                      (4.15)
    card.json                           identity hints, aliases (4.16)            both
    devices/<id>/                       one device's emulator set (4.24, 4.25)    that device
    slot-kept/<id or legacy>/<emu>/     slot files kept before a fill (4.24)      Android
    conflicts/<id>/<card folder>/       shared files a link merge kept (4.15)     that device
    retroarch/system/, cheats/          BIOS, cheats (4.17)                       shared
    retroarch/saves/<family>/<system>/[<subdir>/]
                                        every RetroArch in-game save (4.17)       shared
    retroarch/states/<core>/<system>/[<subdir>/]
                                        every RetroArch save state (4.17)         shared
    retroarch/thimsy-older/             files swept out of a launch (4.17)        both
    retroarch/.thimsy-run.json          a launch's run marker (4.17)              both
    retroarch/retroarch.cfg, config/    Android's live slot (4.17, 4.24)          Android
    retroarch/playlists/, ...           Android's own data (4.17)                 Android
    ppsspp/...                          shared storage, Android's slot (4.18)     shared and Android
    scummvm/saves/                      shared saves (4.19)                       shared
    <emu>/<shared folders>              drives, saves, states, BIOS (4.15)        shared
    <emu>/<settings files>              Android's live slot (4.15, 4.24)          Android
    <emu>/app/                          legacy shared app-preference mirror       shipped Android builds
  Thimsy/
    theme.json                          the theme, shared (4.26)                  both
    playlists/<name>.m3u8               music playlists (4.22)                    both
    emulators/*.json                    user emulator definitions (4.23)          user
```

"Both" means both platforms read and write it under the rules of its section. "Shared" means one copy for every device on both platforms, which only the emulators write inside, with no device mirror (OA-52, OA-53). "Android" or "Linux" means only that platform writes it; the other reads it where its section says so and never deletes it. "That device" means only the device whose id the folder carries writes it; every other device leaves it alone (4.24). "Shipped Android builds" means only builds from before the floor write it; builds with this contract read it once, at migration, and never write it again.

Nothing in the layout is device specific except `devices/<id>/`: every device that uses the card sees the same library, gamelists, collections, media, playlists and theme (OA-48, OA-49, OA-51), and the same saves, save states, BIOS and emulated system storage (OA-52, OA-53). No folder is per platform.

## 4. File by file

### 4.1 `ES-DE/settings/es_settings.xml`

- **Owner:** ES-DE's format. ES-DE itself may write any key. Nano edits single `<type name="" value="" />` lines and keeps every other line, tag type and order (`esdeSettingsSet`).
- **Keys Nano writes, both platforms:**
  - `ROMDirectory`: absolute (OA-20), written only when empty, as `/storage/<id>/ROMs` for this card. Readers expand a leading `~` to the ES-DE folder (Android ES-DE's meaning), and re-root a stored folder that is missing (or whose id is an alias of this card) to the same relative path on the store's volume.
  - `CollectionSystemsCustom`: comma list of enabled custom collections.
  - `MediaDirectory`: cleared in store mode (the media is `<store>/downloaded_media`).
  - **The theme mirror** (OA-51, ***proposed*** shape, plan §2.4): the theme set, `ThemeVariant`, `ThemeColorScheme` and `ThemeFontSize`, copied from `Thimsy/theme.json` (4.26) so the standalone ES-DE app shows the same theme. Written only when the chosen set exists in `<store>/themes`. ES-DE 3.x names the set key `Theme`, older versions `ThemeSet`; Nano updates whichever the file has, and with neither, writes the name the bundled ES-DE version uses **(verify which name 3.5.0 writes)**. This is a display copy: `theme.json` is the truth. A theme change made in the ES-DE app is taken back into `theme.json` only under plan §2.4's checks against ES-DE's stale rewrites (owner question 4). Nano never writes `ThemeAspectRatio`: aspect ratio stays local to each device (OA-51, W7-INTEROP-9).
- **The 12 scraper keys** (`Scraper`, `ScraperRegion`, `ScraperOverwriteData`, `ScrapeScreenshots`, `ScrapeTitleScreens`, `ScrapeBackCovers`, `Scrape3DBoxes`, `ScrapePhysicalMedia`, `ScrapeMarquees`, `MiximageGenerate`, `ScrapeManuals`, `ScrapeVideos`, `kEsdeSettingProps`): shipped Android builds copy them from this file into local properties in store mode and write them back on a change. They are Thimsy settings, so under OA-47 they become local only: from the per-device change on, Nano neither adopts them from this file nor writes them, and the card keeps what the ES-DE app wrote last (***proposed***, plan §2.6, owner question 14). Linux never writes them.
- **No other Thimsy key is ever written here** (OA-20).
- **Version:** none; ES-DE's format.

### 4.2 `ES-DE/gamelists/<folder>/gamelist.xml`

- **Owner:** ES-DE's format. `<folder>` is the ES-DE system name of the ROM folder, or the folder's own name when ES-DE has no system for it.
- **Paths:** `<path>` is form (a), `./<rel>`, matched case-insensitively.
- **Fields both platforms write:** `favorite`, `hidden`, `name` (title overrides), `desc`, `rating` (0 to 1), `releasedate` (`YYYYMMDDT000000`), `developer`, `publisher`, `genre`, `players`, `playcount` (only ever incremented) and `lastplayed` (`%Y%m%dT%H%M%S`, local time, ES-DE's own format; a later time always wins).
- **Favourites, hidden games and Recently Played live here, on the card only** (OA-23, OA-49): every device using the card sees the same ones, and the card always takes priority. Recently Played is rebuilt from `<lastplayed>` on each side. The device keeps only a cache (`nano_xmb_recent.list`) and a journal of starts made with the card out (`nano_esde_played.txt`), flushed when the card is in. No play journal on the card (Q6).
- **Emulator labels:** `<altemulator>` per game and `<alternativeEmulator><label>` per gamelist hold ES-DE `android`-table labels, as ES-DE and shipped Android builds wrote them. They are not Thimsy's picks: Thimsy's picks are per device (4.25). From the per-device change on, neither build writes, changes or deletes either tag; Android reads them once, at migration (plan §6.1 step 3), and Linux never reads them (***proposed***, plan §2.3, owner question 5). The ES-DE app's picks therefore stay its own.
- **`<folder>` entries** (PSP folders seeded with `<folderlink>`) are handled like `<game>` entries, so an edit never adds a second entry for the same title (G20).
- **Unknown tags are kept.** An unreadable gamelist is never replaced.

### 4.3 `ES-DE/collections/custom-<name>.cfg`

- **Owner:** ES-DE's format. Both platforms read and write. On the card only, shared by every device (OA-49).
- **Lines:** form (b) for a game under ROMDirectory; else form (e), the absolute path with this card's current id (ES-DE's format has no other form, OA-20). Readers resolve (e) through the alias list.
- **A line that does not resolve is kept verbatim.** File names are cleaned of the characters FAT forbids.
- A collection loads only when `CollectionSystemsCustom` lists it.

### 4.4 `ES-DE/downloaded_media/<folder>/<type>/<name>.<ext>`

- **Owner:** ES-DE's layout. Both write (the scraper, the move). On the card only, with no device mirror (OA-49).
- **Found by name:** `<name>` is the game's path below its folder without the extension. No index is needed to find art, so covers show on both sides even when `index.json` cannot be used.
- **Types:** `covers`, `backcovers`, `3dboxes`, `physicalmedia`, `marquees`, `miximages`, `screenshots`, `titlescreens`, `fanart`, `manuals`, `videos`.
- **Extensions read:** images `png`, `jpg`, `jpeg`, `webp`; manuals `pdf`, `cbz`, `cbr`, `cb7`; videos `mp4`, `mkv`, `webm`, `avi`. Writers use only these. Known quirk: Nano names its own covers `.png` whatever the bytes (check.md row 11).
- **A side that cannot show a type shows the next one and deletes nothing.** Video and manuals are required on Linux for the alpha (OA-16, OA-24).

### 4.5 `ES-DE/themes/<set>/`

Read by both as a theme root. Downloaded themes go to the theme card's `ES-DE/themes` (4.26), not to the device, so a theme choice in `theme.json` finds its set on every device (contract A6, OA-37, plan §2.5). Today they still go to `/data/system/nano_esde_themes` (G14); Android's migration copies the sets there to the card (plan §6.1 step 8). The theme list also reads `<theme card>/ES-DE/themes` when the store is elsewhere.

### 4.6 `ES-DE/custom_systems/es_systems.xml`

- **Owner:** ES-DE's format. Append-only: one entry per folder in ROMDirectory that ES-DE has no system for. No writer ever changes or removes an existing entry.
- **Both read** `name`, `fullname`, `path` (form (b)), `extension`, `platform` and `theme`.
- `<command>` lines carry Android ES-DE commands (for the ES-DE app). A reader ignores commands it cannot interpret. Linux appends entries in the same form Android does.

### 4.7 `ES-DE/nano/format.json`

- `{"version": 1}`. Schema: `linux/schema/format.schema.json`.
- The contract major the library needs a reader to understand. A missing file means 1. A reader that meets a higher major uses the library read-only and says so.
- It guards only new builds against newer ones: no shipped build reads it. Major 1 is today's layout plus this contract's additive files, the per-device sets and `theme.json` included (plan §6.3). The launcher plan's `{"version": 2}` (relative `names.json` and `index.json`) needs the floor first (***proposed***, W7-INTEROP-5).

### 4.8 `ES-DE/nano/systems.json`

- **What it is now:** the shared system list of this card's library: which folders hold which system, how the systems are named and sorted, the custom systems the card adds. On the card only, the card wins, adopted as today (`esdeStoreAdoptSystems`) (OA-49; ***proposed***, plan §2.6, owner question 15).
- **Shared fields** (both may write): `id`, `builtin`, `enabled`, `order`, `displayName`, `shortname`, `romDir`, `scanSources` (the card's id written `%CARDID%`), `disabledDefaultFolders`, `extensions`, `icon`, and `scraper` with `override` and `platform` only. The scraper accounts (`user`, `pass`, `key`) are never on the card (OA-32 Q6, kept); the schema refuses them.
- **Launch fields are no longer picks** (`launchType`: `libretro-core`, `custom-package`, `retroarch-intent`; `coreSo`, `package`, `launchArgs`, `intentTemplate`). A device's system picks are in its set (4.25). A writer keeps each existing system's launch fields exactly as it read them, and writes this device's own launch fields only for a system it adds, because the format requires them and shipped builds launch from them. Reading, Nano takes the shared fields and keeps this device's launch fields (***proposed***, plan §2.3). This replaces Q4's "Android owns the launch fields": no build changes an existing system's launch fields.
- **Format:** `{"version": 1, "systems": [...]}`. Schema: `linux/schema/systems.schema.json`. Android rebuilds the file from structs; a field only Linux knows is lost on the next Android save.
- **Linux writing it:** only with every system's launch fields byte-identical to what it read, and every system kept, used or not. Until Linux's code does that, Linux does not write this file at all (***proposed***, W7-INTEROP-4, still holds).
- **Android, from the floor:** never replaces a launch tuple it does not know with its own (android_launcher_plan.md §3 item 4).

### 4.9 `ES-DE/nano/systems.linux.json` (retired)

- **Never written.** It was to hold Linux's own emulator choices (W7-INTEROP-2). OA-38 first moved those into the shared files, and OA-46 then made picks per device, so Linux's picks are in its set's `picks.json` (4.25). No Linux build shipped with it, so no card carries one.
- A reader that finds one ignores it and never deletes it. Its schema is gone.

### 4.10 `ES-DE/nano/emulators.json` (legacy)

- **Owner:** shipped Android builds. Per-game choices that ES-DE has no label for: `{"version": 1, "items": [{"game", "label", "am"}]}`. Schema: `linux/schema/emulators.schema.json`.
- Shipped builds edit it in place: unknown top-level keys and other games' items survive; the item of the game the user re-picks is rebuilt or removed (`linux_handheld_plan.md` §7.6). Android loads an item only when its `am` matches its catalogue.
- **From the per-device change on, no build writes it.** Android reads it once, at migration, and folds the picks it actually launched into its local pick files (plan §6.1 step 3). Linux never reads it: Linux does not import Android's picks (plan §6.2).
- The `key` field this contract once added (contract A3) is dropped: the file is no longer written.

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
- **No in-game saves or states are written beside the ROM** by launches from builds with this contract (OA-21, OA-53; 4.17). Shipped Android builds leave saves, save states and screenshots here (their `retroarch.cfg` has `savefiles_in_content_dir`, `savestates_in_content_dir` and `screenshots_in_content_dir` true), and keep doing so on a card a new build uses, as does RetroArch started outside Nano: Nano never writes a launch value into the Android slot, which keeps the shipped values or the user's (4.17). Those files stay where they are; a new build copies a game's save or state into the shared folders once, at a launch by the family or core it belongs to, and lists every other one in Older saves (4.17).
- **After a Linux launch, no save, state or screenshot appears under `ROMs/`** (interop row 14).
- `ROMs/androidgames/<name>.app` holds one package name per file (Android only). `ROMs/scummvm/*.scummvm` holds a ScummVM game id on its first line. `ROMs/ports/` is Android ES-DE's `ports` system; Linux's PortMaster ports are not here (4.20).
- Linux scans by extension, so `.srm`, `.state*` and screenshot files are never listed as games.

### 4.14 `bios/`

The drop folder the BIOS installer offers first, matched case-insensitively at the card root. Both read it; only the user writes it. Installed-file records stay on each device (`nano_bios/installed.json`).

BIOS files are shared storage (OA-53): one copy on the card for every device, with no device mirror, in `bios/`, `retroarch/system/` and each emulator's BIOS folder, which its definition's `bios` block names (4.27). Key files (Eden's `keys/`, Azahar's `sysdata/`) count as BIOS. The BIOS installer installs into those folders.

### 4.15 `emulator_config/`, in general

Every file an emulator keeps is one of the kinds below, and the emulator's definition says which (OA-54, 4.27). `per_device_config_plan.md` §2.7 has the first cut per emulator that the shipped definitions carry.

| Kind | Examples | Where | Rule |
|---|---|---|---|
| Shared storage | emulated system drives (installed games, updates, DLC, firmware, system save areas: Vita3K's `ux0`, Eden's NAND and SD, Dolphin's Wii NAND, Azahar's NAND and SD, PPSSPP's memory stick folders, PS2 memory cards); in-game saves; save states; BIOS and keys; cheats | the emulator's card folder `emulator_config/<emu>/<shared folders>`, `bios/`, `retroarch/system/`, `retroarch/saves/`, `retroarch/states/` | one copy on the card for every device and both platforms; never in a set; no device mirror (OA-52, OA-53) |
| Per-device settings | graphics, controls, performance, folders, per-game settings: RetroArch's main config, core options, overrides and remaps; PPSSPP's and ScummVM's ini files; Android app preferences | `devices/<id>/<emu>/` on the card (4.24), the device's local set, the live file | each device uses only its own (OA-46) |
| Android's live slots | Android's settings files directly under `<emu>/` (`retroarch/retroarch.cfg`, `config/`, `ppsspp/PSP/SYSTEM/`, Flycast's `emu.cfg` and the like) | the files an Android emulator reads from its card folder | filled from the Android device's set before a launch (4.24). Linux never reads or writes them |
| Left out | caches, shader caches, logs, crash dumps, temporary files | the emulator's local folders (Linux: `/storage/.config/<emu>`, `/storage/.local/share/<emu>`; Android: app data) | never synced, never on the card |

**Settings no longer travel between devices** (OA-46 replaces OA-26 and OA-39). There are no per-platform override layers and no shared settings files, so OA-26's per-emulator key lists are not needed.

**Shared storage has no mirror** (OA-52, OA-53). The local set holds settings only. With the card out, an emulator whose shared storage is on the card does not start ("<emulator> needs the card"), as shipped Android builds do today; a fallback to internal storage covers settings only, because a second copy of a drive or a save folder would split it (***proposed***, plan §2.7, owner question 20).

**Different cores' files stay apart** (OA-53, with OA-21): by name or folder on the card, so one core never overwrites another's. No converters are written. A save state is offered only to the core that made it (4.17).

**The shared root.** A definition's shared paths are under the emulator's shared root: on Linux, and on Android with the emulator files on the card (`emu_sd` on), `emulator_config/<card folder>/`. On Android with `emu_sd` off it is the emulator's internal folder that Move Emulator Files would move (RetroArch's `/storage/emulated/0/RetroArch`, ScummVM's `/data/data/org.scummvm.scummvm/files`): keys and links point there, no card is needed, and shared storage reaches the card only through Move Emulator Files, which moves the whole root so the paths under it stay the same. A key the user pointed at another folder is left as it is while `emu_sd` is off (***proposed***, plan §2.7, owner question 19).

**Where settings and storage share a folder**, the settings stay per device and the emulator is pointed at the shared storage on the card (OA-52), by a method its definition names (4.27): its folder is on the card and its settings files there are a live slot (Android); a link on internal storage to the card folder; a key in the emulator's settings, written before each launch from where the card is now; RetroArch's per-launch append config (Linux); or a bind mount for an emulator that refuses a link (Linux). Recommended order on Linux: a key, then a link, then a bind mount (***proposed***, plan §2.7, owner question 23). A real local folder where a link belongs is merged into the card folder before the link replaces it: missing files copied, identical ones skipped, and of two that differ the newer takes the card path while the other is kept in `emulator_config/conflicts/<id>/<card folder>/<rel>`; the local folder goes only when every file is on the card (***proposed***, plan §2.7).

**Rules for Linux in `emulator_config/`:**

- There is no `<emu>/linux/` folder: the per-platform folders are withdrawn (OA-53), and no card has one. Linux's saves and states go to the shared folders above.
- Linux points its emulators at shared storage with keys, the append config or links on `/storage` (ext4), never with a link on the card (1.1).
- Linux never reads or edits `<emu>/app/`, Android's live slots, `slot-kept/`, `Android/`, or another device's set (4.24). It writes `conflicts/linux-<hash>/` only through a link merge.
- An emulator folder that exists only for Android (Dolphin, Azahar, ARMSX2, hakuX, Vita3K, DraStic, drastic-nano, M64Plus FZ, SeedlessDS, Flycast's `emu.cfg`) is left untouched by Linux (OA-22). When Linux gets one of these emulators, its definition gains Linux entries and Linux shares the same storage.

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
- **`writers`:** one entry per device model and platform, for "last used in" wording. `device` is multi_device_plan.md's descriptor id, the same on both builds. `contract` is the major of this document the writer implements; it is how a reader learns the floor has written the card. Which units have sets is told by `devices/` (4.24), not here.
- **`emulators`:** the version of each emulator that last wrote its shared storage, per platform, for a warning when the other side's was newer (the same PPSSPP on both platforms shares one `SAVEDATA` and one `PPSSPP_STATE`, 4.18).
- **`copies`:** files one platform copied into another emulator's folder for its own use (Flycast BIOS into `retroarch/system/dc/`, DraStic BIOS under the melonDS DS names). Paths are relative to `emulator_config/`.
- **It is a hint, never a safety gate** (`linux_handheld_plan.md` §7.7): any build without this contract leaves it stale.
- **Safe for shipped builds:** they ignore it, and `repoint` rewrites an id only when `/emulator_config` follows it, so bare ids are never edited.
- **Where:** `emulator_config/card.json` (OA-19). Linux writes it only where `emulator_config/` already exists or Linux is creating it for its own data, so a card that holds only `ES-DE/` does not gain an `emulator_config/` folder that Android's foreign-card check might react to. On such a card the aliases come from the prefix scan alone (***proposed***, W7-INTEROP-8).

### 4.17 RetroArch, saves and save states (OA-21, OA-53)

| Path | Class | How RetroArch is pointed at it |
|---|---|---|
| `retroarch/system/` | shared BIOS | `system_directory`: Android in its live slot, Linux in the per-launch append config |
| `retroarch/cheats/` | shared | `cheat_database_path`, the same way |
| `retroarch/saves/<family>/<system>/[<subdir>/]` | shared, one folder per save family, system and game folder | `savefile_directory` = that folder, with `sort_savefiles_enable`, `sort_savefiles_by_content_enable` and `savefiles_in_content_dir` `"false"`, for that launch |
| `retroarch/states/<core>/<system>/[<subdir>/]` | shared, one folder per core, system and game folder | `savestate_directory` = that folder, with `sort_savestates_enable` and `savestates_in_content_dir` `"false"`, for that launch |
| `ROMs/<system>/[<subdir>/]<game>.srm`, `.rtc`, `.state*` | left by shipped builds and by RetroArch started outside Nano | never written by launches from builds with this contract; copied into the folders above once per game for the family they belong to, else listed in Older saves |
| `retroarch/retroarch.cfg`, `config/` | Android's live slot (4.24) | Linux never reads or writes it |
| `devices/<id>/retroarch/retroarch.cfg`, `config/` | that device's set (4.24) | Android fills its live slot from it; Linux syncs it with `/storage/.config/retroarch/retroarch.cfg` and its `config/` before and after each run |
| `retroarch/playlists/*.lpl`, `screenshots/`, ... | Android | Linux never reads or writes them. RetroArch `.lpl` playlists stay per platform (OA-23) |
| loose files directly in `retroarch/saves/` or `retroarch/states/`, RetroArch's own per-core folders there (`<core name>/<game>.srm`) and its content-directory folders (`<content dir>/<game>.srm`, `<core name>/<content dir>/<game>.srm`, `<content dir>` being the ROM's own folder name **(verify)**) | shipped Android builds (contentless cores, "save in content dir" off) | left where they are; taken in by the same rule as the files beside the ROM, a per-core folder for its core's family |
| `retroarch/saves/<family>/<system>/[<subdir>/].thimsy-legacy.json` | both | the legacy files taken in there: source path relative to the card, size, time, hash |
| `retroarch/thimsy-older/saves/...`, `retroarch/thimsy-older/states/...` | both | files of another game's name written into a launch's save or state folder during a Nano launch, moved out after the run at the same path below `thimsy-older/`, never over a file there (a taken name gets `.2`, `.3` added); RetroArch never reads them |
| `retroarch/.thimsy-run.json` | both | a launch's run marker: the set id, a run id and the launch's save and state folders, written before the launch and removed after its sweep |

- **One copy for every device and both platforms** (OA-53), on the card, with no device mirror.
- **In-game saves: one fixed folder per save family and system, on both platforms** (OA-21; no save converters). `<family>` is the launch entry's `save_family`, or its `core` when it has none; `<system>` is the game's system folder under `ROMs/`, and `<subdir>` the path of the game's own folder below it, empty for a game directly in `ROMs/<system>/`, so two games of one name in two subfolders (`ROMs/snes/USA/Game.sfc`, `ROMs/snes/Europe/Game.sfc`) keep two saves (***proposed***, plan owner question 28). The folder depends only on the entry being launched and the game, never on a pick or on `systems.json`'s launch fields, so no later change (a shipped build re-picking, a store created after saves exist) can move or hide a save. Two cores write one file only when their definitions name one family, which carries the family table's `saveCompatible` column (section 5) as data (***proposed***, plan §2.7, owner question 17). A core of one name on both platforms is checked by OA-21's save round trip (the Mesen case, plan §8.5); one that fails gets its own `save_family` on Linux (***proposed***, plan owner question 27).
- **Saves and states made outside these folders** (legacy files: beside the ROM, loose in `retroarch/saves/` or `retroarch/states/`, or in RetroArch's per-core or content-directory folders there) are copied in, never moved, and never hidden (***proposed***, plan §2.7, owner questions 17, 18 and 25):
  - *Whose file it is:* a per-core folder's file belongs to the family (for a state, the core) of the launch entry whose `core_name` names that folder. Any other belongs to the game's legacy family: that of the game's legacy pick (on a card with a store, its `<altemulator>`, else its `emulators.json` item; without a store, the device's `legacy-picks.json` written at Android's migration), else of the system's (the card's `systems.json` launch fields, 4.8, else the same file). These are read only to know whose a file is, never as a pick.
  - *Taken in* at a launch by that family (core) when its folder has no save (state) for the game; the newest of several; recorded in `.thimsy-legacy.json`. A place that does not name the game's system and folder (loose, a per-core or content-directory folder) is taken in on its own only when no other game on the card has that name. The original stays.
  - *Listed* in Game Options > Older saves otherwise: another or an unknown family, a folder that already has a save, a source that changed after it was taken in. The user can copy one in; the current file is kept with `.<date>.bak` added, or `.<date>.2.bak` and so on when that name is taken, so no `.bak` is overwritten. Older saves also lists every such `.bak` and every `thimsy-older/` file of the game's name. A state is listed only for its own core. Nano never copies another family's file by itself, so no two cores write one file.
- **Save states are shared** (OA-53), always in `retroarch/states/<core>/<system>/[<subdir>/]`, `<core>` being the launch entry's `core`. RetroArch reads states only from that folder, so a state is offered only to the core that made it. A core named differently on the two platforms is kept apart, which is safe. A core of one name on both platforms is checked by `linux_handheld_plan.md` question 11's round trip (plan §8.5); one that fails gets its own state folder on Linux through its launch entry's `state_core` (***proposed***, plan §2.7, owner question 27). This replaces OA-21's per-platform states and the shipped Android default of states beside the ROM.
- **Which fields name the folders and keys:** RetroArch's definition's `files.shared` entries of class `saves` and `states`: `card` (`saves/{save_family}/{system}/{subdir}`, `states/{core}/{system}/{subdir}`; `{subdir}` is empty for a game directly in its system folder), `point` (`config_key` on Android, `append` on Linux, naming `savefile_directory` and `savestate_directory`), `point.set` (the sort and content-dir keys, `false`), no `point.rest` (`retroarch.cfg` has a per-launch copy on Android, so no stored copy is reset: each keeps a shipped build's values, `savefiles_in_content_dir` and `savestates_in_content_dir` `true`, or the user's, and a shipped build or the ES-DE app using the slot saves beside the ROM as before), `legacy` (the places above), `names` (the per-game file names the after-run sweep checks), and each launch entry's `core`, `save_family`, `core_name` and `state_core` (4.27; `user_emulators_plan.md` §1a). Nano's code names no core and no key.
- **No other core or game during a Nano launch** (***proposed***, plan §2.7, owner question 26). The folders are fixed for the whole run, so RetroArch's own loading menus are hidden for it: the saves entry's `point.set` sets `menu_show_load_core`, `menu_show_load_content`, `menu_show_load_disc`, `menu_content_show_history`, `menu_content_show_favorites`, `menu_content_show_playlists` and `menu_content_show_explore` `"false"`, and `menu_content_show_contentless_cores` and `menu_content_show_netplay` **(verify)**; they are set only in the per-launch copy (below), and every stored copy keeps the user's own values. After the run, a file written during it directly into the launch's save or state folder whose name is not the launched game's (by the entry's `names` patterns, `{game}.srm`, `{game}.rtc`, `{game}.state*`) is moved to `retroarch/thimsy-older/` at the same path, never over a file there, and listed in Older saves, owner unknown; it is never left in place. A sweep cut short by a crash or a power cut runs at the next start only while the card's `.thimsy-run.json` still names that device's set and run; otherwise the listing is dropped and the files stay. A pending sweep runs before the next launch's own steps. A swept file newer than its game's save gives a one-line notice at that game's launch. A core's own files (`scd_U.brm`, `mame/`) and files there before the launch and unchanged by it are not touched.
- Files and folders a core keeps itself (`yabasanshiro/backup.bin`, `opera/per_game/`, Sega CD `scd_*.brm`, `mame/` with its `cfg/`, `fbneo/`, `cdata/`) go below the same folder, the game's, because RetroArch hands the core the save directory before it starts; so a core that keeps one file for every game (Sega CD's `scd_U.brm`) keeps one per subfolder (***proposed***, plan owner question 28). MAME's `cfg/` is shared with the rest of `mame/`: no option moves it alone **(verify)**, and pad controls stay per device in RetroArch's remaps (***proposed***, plan owner question 24).
- **Per-launch values reach no stored file** (***proposed***, plan §5.6). Android never writes them into the slot or any file another launcher reads: each Nano launch passes RetroArch a per-launch copy of its live `retroarch.cfg` with the keys set, on device storage at a path only Nano names (the definition's `files.launch_copy`, RetroArch's `files/thimsy-launch/retroarch.cfg`) **(verify)**, and once RetroArch's process has ended takes the settings changed in the copy back into the slot and the set, without the pointing keys, so each stored copy keeps the user's own value of those keys. No stored file holds a per-launch value, so RetroArch started outside Nano, after a Nano run, sees none. Linux sets them in the append config, written by Nano to `/run/thimsy/` at each launch, and after the run puts each of them back in the `--config` file as it was before the run (the value restored, or the key removed only when it was absent), in case RetroArch's `config_save_on_exit` wrote them there **(verify)**, so nothing on the card or in a set holds them and the user's own values survive. Folder keys are set from where the card is now at each launch and keep that path between launches, so RetroArch opened outside Nano uses the same one copy of the BIOS and cheats.
- This replaces `linux_handheld_plan.md` §6.2's sorted saves beside the ROM, the earlier card-core rule (saves beside the ROM for one family) and W7-INTEROP-15's `retroarch/linux/saves/<system>/` and `retroarch/linux/states/<system>/`. No card has those folders.
- Linux screenshots go to internal storage (***proposed***, W7-INTEROP-10).

### 4.18 PPSSPP

| Path under `emulator_config/ppsspp/` | Class |
|---|---|
| `PSP/SAVEDATA/`, `PSP/PPSSPP_STATE/`, `PSP/GAME/`, `PSP/Cheats/`, `PSP/TEXTURES/`, `PSP/flash0/font/` | shared storage: one copy for every device on both platforms (OA-52, OA-53) |
| `PSP/SYSTEM/ppsspp.ini`, `controls.ini`, `<GAMEID>_ppsspp.ini` | Android's live slot (4.24): filled from the Android device's set. Linux never reads or writes it |
| `PSP/SYSTEM/CACHE/`, shader caches | left out: never in a set **(verify the names)** |
| `PAUTH/`, `EXDATA/` | Android |

Each device's PPSSPP settings are in its set, `devices/<id>/ppsspp/PSP/SYSTEM/<same names>` (Android's also `devices/<id>/ppsspp/app/shared_prefs/`).

**At a Linux launch:**

1. With the card set switch on, Linux's set is synced card to local (4.24). Linux's live settings are its local memory stick's `PSP/SYSTEM` files, under `/storage/.config/ppsspp/` **(verify the path PPSSPP uses on Linux)**.
2. PPSSPP runs with that internal memory stick, whose shared folders are symbolic links on `/storage` to the card's folders. The settings sit inside the memory stick, so the stick cannot be pointed at the card whole. Bind mounts only if PPSSPP refuses the links (***proposed***, plan §2.7, owner question 23) **(verify)**.
3. After the game, changed settings files go to the local set, then to the card set.

The same emulator on both platforms shares one `SAVEDATA` and one `PPSSPP_STATE`; `card.json`'s `emulators` versions warn when the other side's build was newer (4.16). W7-INTEROP-11's override layer (`ppsspp/linux/PSP/SYSTEM/`) and Linux's `ppsspp/linux/PSP/PPSSPP_STATE/` are withdrawn (OA-46, OA-53; never built).

### 4.19 ScummVM

- `emulator_config/scummvm/saves/` is shared storage (OA-53), if both sides derive the same game target ids (to test). Each device points ScummVM at it through `savepath` in its own `scummvm.ini`, written before each launch from where the card is now (plan §2.7).
- **Settings are per device** (OA-46): Linux's `/storage/.config/scummvm/scummvm.ini` is in its set as `devices/<id>/scummvm/scummvm.ini`; Android's app file is in its set as `devices/<id>/scummvm/app/files/scummvm.ini`. The legacy shared mirror `scummvm/app/files/scummvm.ini` is read once, at Android's migration. There is no shared `scummvm/scummvm.ini` and no `scummvm/linux/scummvm.ini`: W7-INTEROP-12 and OA-39 are withdrawn.
- Game targets whose `path` is a `/saf/` or `content://` path are Android-only. Linux resolves a target's game files from `ROMs/scummvm/`.

### 4.20 Systems with no emulator on a device (OA-22), and PortMaster (OA-50)

- On Linux, Android's "Android Games" system is replaced by PortMaster's Ports.
- **PortMaster is never on the card** (OA-50). PortMaster, its runtimes, the ports, their saves and their launch scripts are on the system card's ext4 data partition (`/storage`), never the game library. Ports do not follow the library between devices, and ports that use symbolic links work. Linux's Ports system lists the local launch scripts, not `ROMs/ports/` (`docs/linux/research/portmaster.md` §4, §5).
- GameCube, Wii, 3DS, PS2, Switch and Vita are hidden on devices that cannot run them. Their games, saves and settings on the card are left untouched for Android.
- A system appears when an emulator definition on the device gives it a launch entry for this platform (4.27) and the device profile says the device can run it, so no card format changes when stronger devices arrive.

### 4.21 `Thimsy/settings.ini` (removed)

- **Not part of the contract** (OA-47 replaces OA-32 and OA-20's card file). Thimsy's own settings live only in the device's settings store: Android's `persist.*` properties, Linux's ConfigStore (`/storage/.config/thimsy/persist.props`). No build writes `settings.ini`, and a file found there is neither read nor deleted. No build ever wrote one.
- `tools/nano-tree/prefs.json` stays as the settings inventory (OA-47). Its classes P, M, A and L no longer put anything on a card; its `card` field (`"theme"`, plan §2.4) marks the keys that go to `Thimsy/theme.json`.
- **The one exception is the theme** (OA-51): 4.26.
- The rules OA-32 set for the file (hotkey buttons, RGB settings, slide actions, scraper accounts and API keys never on the card) still hold for the card as a whole: none of them is ever written to it.

### 4.22 Music playlists (OA-48)

- Music files and any `.m3u` or `.m3u8` already in the user's music folders are on the card as they are; Nano rebuilds those playlists from each scan.
- **User-created playlists are on the card** as `.m3u8` files: UTF-8, first line `#EXTM3U`, optional `#EXTINF` lines, one entry per line, each entry a path relative to the playlist file's own folder with forward slashes. No absolute paths, no tokens, no URLs. A PC can edit them.
- **Where:** `Thimsy/playlists/<name>.m3u8` on the library's volume: the library card, else internal storage (Android `/storage/emulated/0`) when the library is there or no card is in; Linux always has a library card (OA-40, W7-INTEROP-13, plan §2.5). The playlist's name is its file name, cleaned of the characters FAT forbids.
- **No device mirror; the card always wins** (OA-48). Playlists are not device specific: every device using the card shows the same ones. The device's `nano_music.json` is only a cache of the scan (track metadata), never the playlists' home.
- **Migration** (Android): user playlists in `nano_music.json` are written to `Thimsy/playlists/`; a name already taken there is written as `<name> (<model name>).m3u8` (plan §6.1 step 7).
- Linux hides the music player until it has a media decoder (OA-23, OA-24); the files stay on the card meanwhile.

### 4.23 `Thimsy/emulators/*.json`

User emulator definitions to import (`user_emulators_plan.md` §1 and §1a: `schema` 1, with `launch.linux`, `files` and `save_family` for Linux; `linux_handheld_plan.md` §6.4's draft schema 2 is superseded). Neither platform acts on them straight from the card. Each imports through a confirm screen into internal storage and records the origin. Their format is that plan's, not this document's; 4.27 says what a definition decides on the card.

### 4.24 `emulator_config/devices/<id>/`: one device's emulator set (OA-46)

- **What:** one device's emulator settings files and its emulator picks. One card holds several devices' sets side by side, so different devices keep different settings on the same card. Design: plan §1 to §5. Neither build implements it yet (section 9).
- **The id** (plan §1, ***proposed***, owner question 8): `<platform>-<16 hex digits>`, lower case: `android` or `linux`, then FNV-1a 64 of `thimsy-unit:` followed by the unit's chip serial. A set is keyed by the unit and the platform (OA-46, decided by the coordinator for owner audit), so a dual-boot ARC-D keeps two sets on one card, and two units of one model keep one each. The raw serial is never written. With no usable serial, the id is random (`"source": "random"`); two units with one serial get a collision id (`"source": "serial+fp"`).
- **`device.json`:** the set's label, written last, so a set folder without it is partial (a copy that did not finish) and is never used as the truth. Schema `linux/schema/emu-set.schema.json`:

```json
{ "version": 1, "id": "linux-9c1d0e7a42b35f86", "platform": "linux",
  "model": "anbernic-rg-arc-d", "name": "Anbernic RG ARC-D", "source": "serial",
  "fp": "5e0a9c41d2b7f318", "thimsy": "0.2", "first": "2026-12-01", "last": "2026-12-03" }
```

  `model` is multi_device_plan.md's descriptor id and `name` its display name; the user's own device name is never written. `fp` is a hash of a second hardware value, used only to catch two units with one serial (plan §1). The id must equal the folder's name. The device's local copy also has `changed` and `migration`, which never go to the card.
- **`picks.json`:** this device's system and game picks (4.25).
- **Settings files:** `<emu>/<rel>`, each at the path it has in the emulator's folder, and `<emu>/app/<rel>` for Android app-private settings. Which files they are comes from the emulator's definition (`files.settings`, 4.27); Nano holds no list of its own. A first cut the shipped definitions carry (plan §2.7, ***proposed***; that table also covers Vita3K, Eden, Dolphin, Azahar, ARMSX2, DraStic and hakuX):

  | Emulator | In the set | Shared storage (4.15) or left out, never in the set |
  |---|---|---|
  | RetroArch | `retroarch.cfg`; `config/` (core options, overrides, remaps); user-made `autoconfig/` profiles (Android only) | shared: `system/`, `cheats/`, saves, states (4.17); left out: `.lpl` playlists, thumbnails, shaders, caches |
  | PPSSPP | `PSP/SYSTEM/ppsspp.ini`, `controls.ini`, `<GAMEID>_ppsspp.ini`; Android `app/shared_prefs` | shared: `PSP/SAVEDATA`, `PSP/PPSSPP_STATE`, `PSP/GAME`, `Cheats`, `TEXTURES`, `flash0/font` (4.18) |
  | ScummVM | `scummvm.ini` (Android `app/files/scummvm.ini`) | shared: `saves/` |
  | Flycast (Android) | `emu.cfg`, `mappings/` | shared: `data/` (BIOS, VMUs, states) |
  | Other Android emulators | their `app/` items and config files **(list to check per emulator)** | shared: emulated drives, saves, memory cards, states, BIOS; left out: caches |

  Caches, logs, shader caches and anything over 16 MiB are never in a set. Settings files are copied as bytes, never parsed, so they may hold absolute paths; Nano sets folder keys from where the card is now before each launch (plan §5.6 step 3).
- **`emulators/<def id>.json`:** with the switch on, a copy of this device's user emulator definitions, so a reinstalled device gets them back with its set. A copy changed on the card is used only after the user accepts it on the device through the import screen; shipped definitions are never copied (***proposed***, plan §2.8, owner question 21).
- **`.previous/`:** the set as it was before the last replace (turning the switch on with an old set, or Copy settings from another device), kept once.
- **Who writes it:** only the device whose id it is. Every other device leaves it alone and never deletes it. The one reader from outside is Settings > Game Settings > Copy settings from another device, which lists same-platform sets by their label and copies one after a confirm (***proposed***, plan §4.5, owner question 7).
- **The "SD card settings" switch** (OA-46): a per-device setting, `persist.gammaos.nano.emu_card_sets`, kept on the device (class D). Turning it on copies the device's local set to the card. While it is on and the card has no set for this id, the local set is copied there. While it is off, the card set is neither read nor written (***proposed***, plan §4.1, owner question 16). Defaults: on for Linux; on Android, from `emu_sd` (plan §4.6, owner question 10).
- **Lookup and the mirror** (OA-46): the device always keeps a local set, its mirror, so removing the card loses nothing. With the switch on, the card set takes priority and refreshes the mirror; changes made on the device go to both. Per file, three ways, as `emusync::decide` does today: a change on one side goes to the other; a change on both, or a set never synced, takes the card's. Picks merge per entry (4.25). The device copy that loses a clash is kept once in the local set (plan §5.1, owner question 11). Sync state stays on the device.
- **Android's live slots.** An Android emulator that reads its settings only from its folder on the card keeps reading `emulator_config/<emu>/`. That folder is a live slot: before a launch the device fills it from its own set, and a marker `emulator_config/<emu>/.thimsy-slot` records whose set it holds and each file's hash. A slot file another set wrote and that changed since its marker was written is kept first as `slot-kept/<that id>/<emu>/<rel>` (an unchanged one is not kept: that set already has it); one with no marker (a shipped build's, or a hand-made file) is kept as `slot-kept/legacy/<emu>/<rel>` and put back after the run, so a shipped build using that card finds its own settings again (plan §5.6). Linux never reads or writes a slot.
- **PC edits:** a file edited on the card from a PC counts as a card change and wins at the next sync. To change one device's settings from a PC, edit `devices/<id>/...`. A PC edit to an Android slot file goes to the set the slot's marker names (plan §5.3).
- **Single-card Linux:** `/storage` moves with the card, so the local set can belong to another unit. The local set is then parked on the device and this unit's own set put back (plan §5.4). Nothing on the card changes for it.
- **A set never holds shared storage** (4.15): no emulated drive, save, state, BIOS or cheat file, and no file the definition leaves out.
- **Shipped builds** ignore `devices/` and `slot-kept/`. `repoint` rewrites only an absent card id followed by `/emulator_config`, which is right for this card anyway.

### 4.25 `emulator_config/devices/<id>/picks.json`

- **Owner:** the device whose set it is (4.24). Schema `linux/schema/picks.schema.json`: strict at the top, open per entry.

```json
{ "version": 1,
  "systems": { "psx": { "launchType": "libretro-core", "coreSo": "pcsx_rearmed_libretro_android.so",
                        "key": "core:pcsx_rearmed" } },
  "games":   { "n64/Mario Kart 64 (USA).z64": { "label": "Mupen64Plus-Next", "am": "",
                                                  "key": "core:mupen64plus_next" } } }
```

- **`systems`** is keyed by `systems.json`'s `id`, with the launch fields' own names (`launchType`, `coreSo`, `package`, `launchArgs`, `intentTemplate`), which both builds use. Only systems whose pick differs from the device's image default (`nano_emu_defaults.json`, W8) are written; a system with no entry uses the image default.
- **`games`** is keyed by the game key, form (c), with `emulators.json`'s fields (`label`, `am`). On the device, local game picks are keyed `<card id>/<system>/<rel>`; only entries whose card id is this card go to and from `picks.json`, with the card id stripped or added, so a new volume id or a second card neither duplicates nor drops an entry (plan §2.2). Games outside a card's `ROMs/` stay local.
- **`key`** is the portable key of the same pick (section 5), informative only.
- **A pick this device cannot run** is kept and falls back at launch (section 5); a fallback is never written here.
- **Merge:** per entry, against a copy of `picks.json` as last synced, kept on the device: changed only on the card, the card's goes to the device; changed only on the device, the device's goes to the card; changed on both, the card's wins (plan §5.1).
- An unreadable `picks.json` is not used and not overwritten (1.2): the local picks are used, and Settings says so.

### 4.26 `Thimsy/theme.json` (OA-51)

- **What:** the theme switches (which interface Nano shows: XMB, ES-DE, Minima and the like, with their style switches) and each theme's settings (theme set, variant, colour scheme, font size). Not device specific: every device using the card shows the same theme. The card takes priority over the device's local value; with no card, the device's local value is used (OA-51).
- **Where:** `Thimsy/theme.json` on the theme card: the library card when the store is on a card, else the slot card (plan Conventions). So a device with a card in and no store on it still uses the card's theme.
- **Format:** schema `linux/schema/theme.schema.json`. Keys are `prefs.json`'s portable `ini` names; values are strings in the device store's own encoding, so nothing is converted. Unknown keys are kept, and a key missing from the file uses the device's own value until the theme next changes (***proposed*** shape, plan §2.4).

```json
{ "version": 1,
  "theme": { "home.theme": "3", "esde_theme.set": "colorful-simplified-es-de",
             "esde_theme.color_scheme": "colorful-dark", "esde.settings_theme": "minima" } }
```

- **The theme keys (31)** (***proposed*** list, plan §2.4; the rule: a key that chooses which interface Nano shows, or how it looks, and does not depend on the screen or the buttons):
  - interface: `home.theme`, `home.xmb`, `home.carousel`, `home.minima`;
  - look: `home.font`, `home.wallpaper_effect`, `home.wallpaper_dim`;
  - XMB style: `xmb.theme`, `xmb.color`, `xmb.color_custom`, `xmb.background`, `xmb.font`, `xmb.day_night`, `xmb.wave`, `xmb.wave_explicit`;
  - DSi style: `dsi.dark`, `dsi.effect`, `dsi.tile_icon`, `dsi.rom_title`;
  - Minima style: `minima.background`, `minima.long_names`;
  - ES-DE theme: `esde_theme.set`, `esde_theme.variant`, `esde_theme.color_scheme`, `esde_theme.font_size`;
  - ES-DE home: `esde.settings_theme`, `esde.menu_color_scheme`, `esde.menu_blur`, `esde.transition`, `esde.clock`, `esde.variant_triggers`.
- **Never in it, kept local:** the aspect ratio (OA-51, decided by the coordinator for owner audit) and the other screen-dependent keys; the two-screen keys; sounds; the battery percentage, boot target and theme download URL; custom wallpaper images (***proposed***, plan §2.4, owner question 3).
- **The device's own theme** (***proposed***, plan §4.7, owner question 1): the card's theme and the device's are separate values. Before a card's theme is shown, the device's own values are saved on the device; when the card leaves, they are put back. Theme edits made while a card is in go to `theme.json` and the live keys, not to the device's own values. There is no switch for it (owner question 2).
- **Seeding:** a theme card with no `theme.json` gets one written from the device's current values. On a new device those are the image's seeded defaults (OA-35).
- **A missing theme set** (OA-37): when `theme.json` names a set this device cannot find, Nano shows the default theme without changing the stored choice, and shows the chosen one again when it appears.
- **The ES-DE app:** four keys are mirrored into `es_settings.xml` (4.1).
- **Two cards** (OA-25): only the chosen card's `theme.json` is used.
- Shipped builds ignore it.

### 4.27 Emulator definitions (OA-54)

- **What:** every per-emulator rule in this contract comes from the emulator's definition file, `user_emulators_plan.md` §1's definition, extended in its §1a: which files are per-device settings (`files.settings`, 4.24), which are shared storage and where on the card (`files.shared`, `storage.card_folder`, `bios`; 4.15), which are left out (`files.exclude`), how the emulator is pointed at its files and how its config reaches it at launch (`files.live`, `files.launch_copy`, `files.shared[].point`, `files.after`; 4.15, 4.17), its launch entries per platform (`launch.android`, `launch.linux`, with each core's `save_family` and `state_core`, and its `core_file`, the core's file name per platform, through which `systems.json`'s `coreSo` is read and written; 4.8, 4.17, section 5), its hotkeys and controller map, and what needs the card (`files.shared[].place`, `storage.needs_card_to_start`).
- **The shipped emulators use the same files**, so a definition written by hand gets the same integration. The first-cut tables in 4.15 to 4.19 and 4.24 are what the shipped definitions carry; they are data, not code.
- **What stays in code:** the file formats, the pointing methods (4.15), the base folders a definition may name on each platform, and the safety rules no definition can loosen (the 16 MiB cap, caches never synced, the path limits and origin rules of `user_emulators_plan.md`). No list of emulators, packages, files or folders, except, for now, Android's framework launch of a `libretro-core` system from the home screen, compiled into system_server (plan §2.8 says why); Nano's own libretro launch reads the `frontend: libretro` definition.
- **Where:** shipped definitions are in the image (Android `/system/etc/gammaos/hotkeys/`, Linux `/usr/share/thimsy/emulators/` *(proposed)*); user definitions are local (Android `/data/system/gammaos/hotkeys/`, Linux `/storage/.config/thimsy/emulators/` *(proposed)*). On the card there are only `Thimsy/emulators/*.json` to import (4.23) and a set's copy of its device's user definitions (4.24).
- **Format:** plain, documented JSON a user can read and write. Its schema, `linux/schema/emulator-def.schema.json`, is shared with `user_emulators_plan.md` and not written yet.
- **Not implemented now** (OA-54): `per_device_config_plan.md` §2.8 and `user_emulators_plan.md` carry the design, and nothing built now may hard-code what a definition should say.

## 5. Emulator choice

**Portable keys** (the schemas' `emu_key`):

- `core:<libretro name without suffix>`, normalised through an alias map (`mupen64plus_next_gles3` is `mupen64plus_next`).
- `emu:<id>` for a standalone emulator, a platform-neutral id: `emu:ppsspp`, `emu:scummvm`, `emu:flycast`, `emu:dolphin`, `emu:azahar`, `emu:melonds`, `emu:drastic`, `emu:m64plus-fz`, `emu:armsx2`, `emu:hakux`, `emu:vita3k`, `emu:seedlessds`.
- `family:<name>` for a group of save-compatible members (`family:melonds`: WatermelonDS and the melonDS DS core).

**The family table** is shared data: generated from ES-DE's `android` and `linuxarm` tables at one pinned ES-DE commit, plus a hand-kept alias and `saveCompatible` list. It maps labels, `coreSo` names and Android packages to keys. It decides OA-21's save sharing (4.17) and fills the informative `key` fields. In the definitions it becomes data: each launch entry's `save_family` (4.27). A pair of cores is listed as save-compatible only after its round trip passes (`linux_handheld_plan.md` §6.2).

**Picks are per device** (OA-46, replacing OA-38's shared picks and W7-INTEROP-2's `systems.linux.json`). The launch configuration, which emulator, core or app launches each system and each game, is the device's own, `systems.json`'s launch fields included (4.8). A pick made on one device never changes another device's, on either platform.

| Where | Per system | Per game |
|---|---|---|
| The device (the local picks, OA-46's mirror) | `nano_systems.json`'s launch fields (Android `/data/system/`, Linux the same file under `/storage/.config/thimsy/data/system/`) | `nano_game_emulators.json`, keyed `<card id>/<system>/<rel>` |
| The card, this device's set (switch on) | `picks.json` `systems` (4.25) | `picks.json` `games` |
| Legacy, read once at Android's migration as picks; later only to know whose a legacy save is (4.17) | `<alternativeEmulator>`; `systems.json`'s launch fields | `<altemulator>`, else the `emulators.json` item |

**Resolution, per launch, both platforms** (plan §5.7): the first that can run here (`nanoplat::Emulators`, which answers from the definitions' launch entries, 4.27) of: the game's pick in this device's set; the system's pick in this device's set; the image default (seeded once into the device's store, W8); the system's own emulator (for a game pick that cannot run); a custom system's first catalog emulator; else "Emulator not installed", shown, with every stored value kept. A fallback is never recorded. `systems.json`'s launch fields are read only when this device has no pick and no image default for a system (a custom system first seen on this card).

**The ES-DE app** keeps its own picks in its labels; Thimsy's picks do not reach it, and its picks do not change Thimsy's (***proposed***, plan §2.3, owner question 5).

**Wave 8.** `step/w8-emu`, merged into `linux-next`, built OA-38's shared picks. Its platform interface, the seeded image defaults and the fallback chain stay; Linux writing labels and `emulators.json`, keeping an unrunnable label as a pick, and the store-mode heal skip are reverted. This is done on `linux-next` before `linux-next` merges into `linux` (OA-46, plan §7).

## 6. Direction of travel

Between any two devices that use one card, whatever their platform:

| Data | Rule |
|---|---|
| The library, gamelists (favourites, hidden games, play counts, Recently Played), collections, media, downloaded themes, `systems.json`'s shared fields, ES-DE settings | shared by every device; on the card only, the card wins (OA-49) |
| The theme (`Thimsy/theme.json`) | shared by every device; the card wins; the device's own value with no card (OA-51) |
| Music playlists | shared by every device; on the card only (OA-48) |
| BIOS, emulated system storage, in-game saves and save states | shared by every device and both platforms; on the card only, no mirror; different cores' saves and states apart by folder, a state offered only to its core (OA-52, OA-53, OA-21) |
| Emulator settings and the launch configuration (picks, `systems.json`'s launch fields) | per device: each device's set rides on the card in `devices/<id>/` and is used only by that device (OA-46) |
| RetroArch `.lpl` playlists | per platform (OA-23) |
| Thimsy's own settings, scraper accounts, PortMaster and ports | never on the card (OA-47, OA-50) |
| Systems with no emulator on a device | kept, hidden (OA-22) |

| From | To | Arrives | Does not arrive |
|---|---|---|---|
| Either Thimsy build | Thimsy Home, mainline ES-DE | ES-DE's own files (OA-20) | Nano's own files; Thimsy's picks |

## 7. What each side must tolerate

- **Both:**
  - unknown files, folders, tags, keys and fields (kept);
  - labels, picks and keys they cannot run (kept, shown as unavailable);
  - every legacy path form;
  - a missing `format.json`, `card.json`, `picks.json` or `theme.json`, and a partial set (no `device.json`);
  - other devices' sets in `devices/` (left alone, never deleted);
  - save and state folders of cores it does not have, and loose save and state files a shipped build left (left alone);
  - a `systems.linux.json` or `Thimsy/settings.ini` (ignored, never written);
  - gamelists and media from mainline ES-DE;
  - temporary files left by a pull.
- **Linux additionally:** never reads or edits Android's `app/` mirrors, Android's live slots, `slot-kept/`, `Android/`, Android's RetroArch config files or `emulators.json`; never reads another device's set except through Copy settings from another device, for Linux sets only; ignores the `<command>` lines in `custom_systems`; treats Android packages as not installed (fail closed).
- **Android additionally (from the floor):**
  - never deletes another device's set or another core's saves and states;
  - keeps a launch tuple it does not know;
  - resolves forms (c) and (d);
  - treats an id in `card.json`'s aliases as this card;
  - skips `devices/` and `slot-kept/` in its emulator_config move;
  - checks a slot's marker before filling it (4.24).

## 8. Tests

**Now:** `linux/tests/test_card_contract.py` validates every JSON file in `linux/tests/fixtures/card/` against its schema and checks that each schema refuses what this contract forbids: path aliases, absolute or `..` copy paths, scraper accounts in `systems.json`, relative keys in `names.json` and `index.json`, emulator keys that are paths or packages, set ids that are not the hashed form, local-only fields in a card set's `device.json`, and theme values that are not strings. It also checks the rules between `android-cube/` (a card as the Cube leaves it) and `linux-written/` (the same card after a Linux visit):

- `names.json`, `index.json`, `emulators.json`, `es_settings.xml` and Android's PPSSPP slot file are byte-identical;
- `systems.json`'s launch fields are as they were;
- labels and unknown tags survive;
- play counts only grow;
- collection lines are kept verbatim;
- every file Linux added is one this contract lets Linux write: its own set, `card.json`, `format.json`, `theme.json`, playlists, and saves and states in the shared RetroArch and per-emulator folders (4.17 to 4.19);
- saves Linux added sit in `retroarch/saves/<family>/<system>/` and states in `retroarch/states/<core>/<system>/` (4.17), and none beside a ROM;
- nothing Thimsy owns holds an absolute card path;
- every Cube prefix in the card's files is in `card.json`'s aliases;
- no `settings.ini`, `systems.linux.json`, `linux/` folder, PPSSPP override layer or shared `scummvm.ini` is written;
- `theme.json` holds only the 31 theme keys;
- playlists are relative `.m3u8`.

`multi-device/` holds a card with two Android sets, one Linux set, one partial set and a `theme.json` with a key from a newer writer; the test checks each set's folder name against its id and platform.

The vectors are hand-written: no build produced them.

**Later** (interop-2026-11-26/contract.md §4, plan §8.5, `linux_handheld_plan.md` Phase 3):

- T1: the shared parsers load and save each vector unchanged; Linux-only fields survive the Android writers compiled for the host.
- T2: the host Nano in the e2e jail mounts the Cube vector at its serial and replays the user actions above.
- T3: the same on loop-mounted exFAT and FAT32 images.
- T4: the device round trips on the ARC-D with the one spare card (OA-18): Android and Linux each keep their own set on the one card, and the theme follows both ways.
- Plan §8.5's host scenarios: two fake devices sharing one card image, single-card Linux moved between units, two units with one serial, a card from a shipped build, two devices running different cores for one game (two save folders, two state folders, neither core offered the other's state), PPSSPP on Android and Linux sharing `SAVEDATA` and states, a hand-written definition getting the same handling as a shipped one.

## 9. Known gaps and where each fix lands

From INTEROP-AUDIT.md and its reports, and from `per_device_config_plan.md` §8. "Android-surface" means the change alters what the Android build does or writes: an H.3 verified move, the AOSP build and the owner's smoke test before nano-144. Every fix in this table is later work; none is made by this document. Line numbers are approximate.

| Gap | Severity | Fix lands in | File and function |
|---|---|---|---|
| G1: Linux never enters store mode by itself | CRIT | Linux | `nano/platform/linux/Volumes.cpp` (a default-store hook), setting `persist.gammaos.nano.esde.dir=<cardRoot()>/ES-DE` at first run; shared `NanoEsdeStore.cpp` `esdeDataDir`/`esdeStoreMode` (120-132) read it. Not the internal path: `esdeStoreFollowCard` skips `emulated` (check.md C-1) |
| N1: every libretro launch from the XMB refused on Linux | CRIT | Linux, via OA-4 | `NanoMenuXmb.cpp` `NanoMenu::coreSoExists` (3345) to a new `nanoplat::Emulators::installed`; Linux answers from the shipped definitions' `launch.linux` entries, which are the image's emulator manifest (4.27) |
| G2, C-2, C-3: Cube paths do not resolve; names.json pruned, index.json art cleared | CRIT | Linux, then Android | alias resolution in an extended `nanoplat::Volumes` (`canonicalStorePath`, `cardAliases`); `NanoMenuScraper.cpp` `saveRomNameOverrides` (prune) and `loadScrapeIndex` (`artOk`) resolve aliases; Linux in compatibility mode (4.11) |
| OA-46: one emulator set and one set of picks shared by every device on a card | HIGH | shared, Android-surface, and Linux | plan §8: `NanoDeviceId`, `NanoEmuSets` (new), `NanoEmuSync` (set paths, keys with the set id), `NanoSdMove.cpp` (`emuSettingsSync`, slots, the switch, `emuSdForeignDialog` replaced), the `DeviceIdentity`, `DeviceStore` and `EmulatorFiles` interfaces, thimsy-session's `/run/thimsy/unit-serial` |
| G2: id repair ignores the Cube form | HIGH | shared, Android-surface | `NanoEmuSync.cpp` `emusync::isVolumeId` and `repoint` learn the 36-character form (check.md B9); `repoint` also walks `devices/` |
| G5, C-4: Linux deletes `<alternativeEmulator>`, rebuilds `<altemulator>` with Android `am`; Android per-game `am` choices applied on Linux then fail | HIGH | shared, Android-surface | `NanoMenuPS3EmuCatalog.cpp` `setGameEmulator` writes local and card picks (4.25), never `esdeStoreSaveGameEmu`; `esdeStoreSaveSystemEmus` and `esdeStoreApplySystemEmus` (833-913) stop writing and adopting labels; `NanoEsdeStore.cpp` `esdeStoreLoadGameEmus` and `esdeStoreSaveGameEmu` used only by Android's migration (plan §8.2) |
| C-5, G10: `systems.json` launch tuples ping-pong | HIGH | shared, then Android | `NanoMenuXmb.cpp` `esdeStoreAdoptSystems` (1123-1215) adopts the shared fields only; `esdeSystemsJson`, `esdeStoreSaveSystems` keep every existing system's launch fields as read (4.8); `loadSystemsConfig` applies this device's picks |
| G4, N2: no Linux fallback; Linux defaults are Android cores; PPSSPP and ScummVM refused | HIGH | Linux | `nano/platform/linux/Launcher.cpp` `argvFor` (398-415) and the missing-core path (196-197); the built-in table in `NanoMenuXmb.cpp` (`kXmbSystemDefs`, 233-248) to `Emulators::defaultFor` |
| G3, C-6: Linux RetroArch saves to ext4, states unset | HIGH | Linux | `Launcher.cpp` writes `/run/thimsy/ra-append.cfg` (4.17) and passes `--appendconfig`; its save and state keys come from RetroArch's definition |
| OA-52, OA-53: shared storage, per-family saves and per-core states | HIGH | shared, Android-surface, and Linux | plan §8: the generic save rule sets 4.17's save and state keys from RetroArch's definition (folders, keys, `save_family`) and copies beside-ROM files in once; `NanoSdMove.cpp` `emuMoveStart` moves the definitions' shared storage and makes settings files slot files; `EmulatorFiles::point` (links on `/storage`, keys, binds through thimsy-session) |
| OA-54: per-emulator tables in code | MED | shared, Android-surface | `nano/NanoEmuDefs.cpp` (new, `user_emulators_plan.md` phase 1) loads the definitions; `NanoEmuSync`'s `items()`, `raDirKeys` and the Dolphin and hakuX link lists become the shipped definitions' data, with no change of behaviour (plan §2.8) |
| G7: BIOS on the card unusable on Linux | HIGH | Linux, via OA-4 | `NanoBiosWorker.cpp` slot-base check (557-575) and `NanoBiosUtil.cpp` `canonical` behind the platform; `NanoRaConfig.cpp` `retroarchConfigFile` gains Linux's system folder; copy rules for Flycast and DraStic BIOS (4.16 `copies`) |
| G6, N5: `packageInstalled` fails open | HIGH | Linux, via OA-4 | `NanoMenuXmb.cpp` `NanoMenu::packageInstalled` (3357) to `Emulators::installed`, failing closed on Linux; emulator_config move and app-mirror sync report unsupported on Linux |
| G8, G9: paths Linux writes do not resolve on the Cube; the Cube id moves | HIGH | Android | Android reads `card.json` aliases (A4, A5); vold names volumes by serial (D20(c), OA-19) |
| G12: foreign-id writes land on ext4 | MED | shared and Linux | shared `makeDirs`/`sdMkdirs` refuse a foreign id (`Volumes::isForeignId`); thimsy-session keeps `/storage` from gaining unknown names |
| G14, OA-51: the theme is local properties only; downloaded themes stay on the device | MED | shared, Android-surface | `NanoThemeCard` (new): `theme.json`, the device's own values, the lock, the ES-DE mirror (4.1, 4.26); `NanoMenuEsdeMenu.cpp` `esdeMenuApplyOption` and `NanoMenuPS3Menu.cpp`'s bound rows through it; theme downloads to `<theme card>/ES-DE/themes` (plan §8.2) |
| G13: videos static on Linux | MED | Linux | the OA-24 media interface replacing `nano/compat/stubs/src/ndk_media.cpp` |
| G15: PPSSPP and ScummVM data not wired on Linux | MED | Linux | `Launcher.cpp` package launches: the card's PPSSPP shared folders linked into the local memory stick (4.18), ScummVM's shared saves (4.19), and the set's before and after syncs (4.24) |
| G16: kernel does not pin the FAT and NLS options | MED | Linux | `linux/tools/thimsy-import/kernel-critical.conf`: `CONFIG_EXFAT_DEFAULT_IOCHARSET="utf8"`, `CONFIG_VFAT_FS`, `CONFIG_NLS_UTF8`, `CONFIG_NLS_CODEPAGE_437` |
| G17: two-slot Linux prefers the boot card | MED | Linux | `nano/platform/linux/Volumes.cpp` `cardRoot` (103-116): OA-25's ask-and-remember |
| OA-48: user playlists only on the device | MED | shared, Android-surface | `NanoMenuMusic.cpp` `loadMusicConfig`/`saveMusicConfig` (103-241): playlists read from and written to `Thimsy/playlists/*.m3u8` on the library's volume (4.22), `nano_music.json` as the scan cache |
| OA-47: the scraper preferences are adopted from and written to `es_settings.xml` | LOW | shared, Android-surface | `NanoEsdeStore.cpp` `esdeStoreApplySettings` (1733-1757) and `esdeStoreSettingChanged` (1771) drop the 12 scraper keys (4.1; owner question 14) |
| G24: names.json and index.json renamed without fsync | LOW | shared, Android-surface | `NanoMenuScraper.cpp` `saveRomNameOverrides`, `saveScrapeIndex`: the shared write helper (1.2) |
| N3: temp files left after a pull | LOW | shared | readers skip them (1.2); a stale one is removed after the final file parses |
| N4: Linux refuses a `0000-0000` serial | LOW | none | kept as the rule (1.1); the installer gives every card it formats a non-zero serial |
| G11: the move can stamp an internal ROMDirectory | LOW (Linux) | shared, Android-surface | `NanoSdMove.cpp` (470-475, 780) |
| G18: `<lastplayed>` is local time | LOW | none | ES-DE's format; kept |
| G19: starts with the card out stay on the device | LOW | none | Q6: no journal on the card |
| G20: duplicate PSP folder entries | LOW | shared | `NanoEsdeStore.cpp` gamelist edit (399-404) matches `<folder>` entries |
| G21: `custom_systems` holds Android commands | LOW | none | harmless; readers ignore commands (4.6) |
| G22: Android-only files | LOW | rule | 4.15 and section 7; a Linux build of such an emulator shares its storage through its definition (4.27) |
| G23: fsck before mounting | LOW | done in design | OA-13: only when the filesystem is dirty |

The order of the work is plan §8.6's phases. Phase 2 (per-device picks, reworking W8) lands on `linux-next` before it merges into `linux`.

## 10. Choices this document made (proposed) and questions for the owner

Proposed in wave 7 (W7-INTEROP), and where each stands now:

- **W7-INTEROP-1:** this file lives at the repository root, as the plans list it. Holds.
- **W7-INTEROP-2:** Linux's per-game choices in `systems.linux.json` `games`. **Replaced**, first by OA-38, then by OA-46: picks are per device, in `devices/<id>/picks.json` (4.25); `systems.linux.json` is retired (4.9).
- **W7-INTEROP-3:** Linux never writes, changes or deletes `<altemulator>` or `<alternativeEmulator>`. **Replaced** by OA-38, then by OA-46. Its effect returns for both builds, for a new reason: labels are no longer picks, so no build writes them after migration (***proposed***, plan §2.3, owner question 5).
- **W7-INTEROP-4:** Linux does not write `systems.json` until its writer keeps the launch fields verbatim. Holds, and the same rule now binds Android (4.8).
- **W7-INTEROP-5:** `format.json` stays at major 1 until the floor. Holds; the per-device files are additive.
- **W7-INTEROP-6:** `card.json` aliases are objects, and a writer's `contract` field marks the floor. Holds.
- **W7-INTEROP-7:** in compatibility mode, Linux's scrape index stays internal. Holds.
- **W7-INTEROP-8:** `card.json` is not written to a card that holds only `ES-DE/`. Holds.
- **W7-INTEROP-9:** `ThemeAspectRatio` is never written. Holds: OA-51 keeps aspect ratio local (coordinator, for owner audit).
- **W7-INTEROP-10:** Linux screenshots stay internal. Holds.
- **W7-INTEROP-11:** PPSSPP's Linux override layer at `ppsspp/linux/PSP/SYSTEM/`. **Withdrawn** (OA-46; never built).
- **W7-INTEROP-12:** a new shared `scummvm/scummvm.ini`. **Withdrawn**: OA-39 confirmed it, and OA-46 replaces OA-39 (never built).
- **W7-INTEROP-13:** playlists in `Thimsy/playlists/`. **Confirmed** by OA-40 and OA-48.
- **W7-INTEROP-14:** form (d) is written `/storage/%CARDID%/<rel>`; `%CARDID%/<rel>` is also read. Holds.
- **W7-INTEROP-15:** OA-21's `linux/saves/<system>/` replaces `sort_savefiles_enable`. **Replaced** by OA-53: every save goes to `retroarch/saves/<family>/<system>/` and every state to `retroarch/states/<core>/<system>/`, on both platforms; there is no `linux/` folder (4.17).
- **W7-INTEROP-16:** schemas of Thimsy-new files are strict; schemas of Android files in the field allow unknown fields. Holds, with `picks.json` entries and `theme.json`'s keys kept open.

Wave 7's owner questions, all answered:

1. Should a choice made on Linux change Android's? OA-38 said yes; OA-46 replaces it: picks are per device, so no device's pick changes another's.
2. Linux's per-game choices in `systems.linux.json` or in `emulators.json`? Neither: in the device's set (OA-46).
3. ScummVM's shared settings? None: settings are per device (OA-46).
4. Where user music playlists live? `Thimsy/playlists/` (OA-40, OA-48).

Open now:

- `per_device_config_plan.md` §9, questions 1 to 28 except 13, which OA-53 answered (among them: the device's own theme while a card's is shown, a theme switch, borderline theme keys, ES-DE app theme edits, ES-DE labels, the switch's defaults and its off state, the id form, RetroArch's live config on Android, scraper preferences, `systems.json`'s shared fields, where RetroArch saves live, the beside-ROM saves and states shipped builds left, shared storage with the card out, user definitions on the card, how Linux points an emulator at the card, MAME's `cfg/`, a save of another family beside the ROM, RetroArch's loading menus during a Nano launch, a core whose states or saves fail the cross-platform test, and games of one name in two subfolders of one system).

Settled since the last revision:

- **Which core decides a shared save** (4.17): none. Every save sits in a fixed folder per save family and system, set by the launching entry's definition, so no core and no launch field decides it; the card's launch fields only choose the family a beside-ROM save from a shipped build is copied into, once (***proposed***, plan §2.7, owner question 17). Save states are shared per core (OA-53), so they need no such choice.
- **The schema name for a set's `device.json`**: `linux/schema/emu-set.schema.json`, which the plan now uses too.
