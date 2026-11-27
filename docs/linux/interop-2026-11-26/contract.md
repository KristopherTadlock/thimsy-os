OA-8 synthesis: Thimsy card interoperability between Android and Linux (ES-DE store and emulator_config)

Scope: read-only. Repo C:/Users/krist/code/thimsy-os-linux (branch linux), plus C:/Users/krist/code/thimsy-os-nano. I edited, built and ran nothing. Evidence comes from the three inventories plus my own checks. "(inf)" marks inference. "android-surface" marks a change to Android behaviour (platform/firmware, or shared code whose output Android writes).

My own checks this pass:
- DECISIONS.md:56-59 (D8), 144-148 (D20) and 1096-1102 (OA-4, OA-8).
- linux_handheld_plan.md §7.2-7.10 (lines 760-958).
- NanoMenuXmb.cpp:3353-3366. `packageInstalled` fails open: `if (fd < 0) return true;`.
- A grep of nano/ for `format.json`, `systems.linux` and `card.json`: zero hits.
- NanoPlatform.h. Its interfaces are Display, Launcher, Audio, Volumes, Power, SystemActions and Network. There is no Emulators, CardPaths or Settings interface.
- compat/stubs/src/ndk_media.cpp. The NDK media stubs on Linux are inert ("nothing can be created... each call logs once and fails").
- NanoThemeEngine.cpp:4823-4845 (a theme `<video>` falls back to the static image) and 1768-1774 (media extensions).
- NanoScraper.cpp:144, 501 (the scraper writes png, jpg and webp only).
- IMPLEMENTATION.md:1057, I30 (no HEIF or AVIF on Linux).

The owner's latest words ("keep the exfat", and Linux and Android supporting each other's formats) fit D8: exFAT is recommended and FAT32 accepted. The plan below assumes the card stays exFAT, with FAT32 best-effort.

======================================================================
## 1. Gap list (A->L = Android-written card used on Linux; L->A = the reverse)
======================================================================

Severity: CRIT means data loss or the feature unusable. HIGH means a major part does not carry over. MED means a visible degrade. LOW means cosmetic or rare.

**G1 CRIT (A->L, L->A). Linux never enters store mode by itself.**
- The ES-DE store is located by `persist.gammaos.nano.esde.dir`, falling back to `/storage/emulated/0/ES-DE` (NanoEsdeStore.cpp:58-59, 120-126). Store mode needs the property to start with `/` (128-132).
- Nothing under linux/ sets it (grep for esde, ES-DE and esde.dir: asset lists only).
- So on first boot the card's ES-DE folder is ignored. Gamelist, collection and setting writes return early (NanoEsdeStore.cpp:335, 357, 971) until the user runs Setup's microSD step (NanoMenuSetupWizard.cpp:623-624) or Move ES-DE to microSD.
- This contradicts linux_handheld_plan.md:793 ("Store mode is always on in the Linux build") and I44.

**G2 CRIT (A->L, Cube cards). D20 (Cube-id prefix map) is not implemented.**
- The Cube writes `/storage/00000000-0000-0000-0000-00000000000N/...` (plan:800-803). Linux mounts the card at `/storage/<exFAT serial>` (Blkid.cpp:72-85).
- No mapping code exists (grep).
- `emusync::isVolumeId` accepts only 9-character or 32-hex ids (NanoEmuSync.cpp:131-140), and `repoint` stops after 32 characters (150-153). So Cube paths in emulator configs are never repaired on either platform.
- Broken on Linux as a result:
  - `nano/names.json` (custom titles; NanoMenuScraper.cpp:420-440).
  - `nano/index.json` (scrape metadata and the box and fan paths; NanoMenuScraper.cpp:208-244).
  - Absolute lines in `collections/custom-*.cfg` (NanoEsdeStore.cpp:677-689, 767-772).
  - Paths inside emulator_config files (RetroArch cfg, Vita3K pref-path and others).
- Partly saved:
  - `ROMDirectory` is re-rooted (NanoEsdeStore.cpp:647-648).
  - Media are found by name (NanoThemeEngine.cpp:1781-1860), so box art still shows.
  - Gamelist metadata is in the gamelist (NanoEsdeStore.cpp:1036-1079).

**G3 CRIT (A->L). A Linux RetroArch can overwrite Android's save states and clash on saves.**
- Android ships `savestates_in_content_dir = "true"` and `savefiles_in_content_dir = "true"` (thimsy-os-nano fixtures retroarch_shipped.cfg:41-48).
- The Linux launcher passes only `-L <core> --config /storage/.config/retroarch/retroarch.cfg <rom>` (nano/platform/linux/Launcher.cpp:398-408; Launcher.h:78-81). It sets no `savestate_directory`, `savefile_directory` or `system_directory` (grep of linux/).
- If Linux RetroArch writes states beside the ROM (inf; it depends on the Linux cfg defaults), `ROMs/<sys>/<Game>.state*` from a different core or build can overwrite Android's states. Section 7.5 of the plan says states are per platform.
- The same problem hits `.srm` where the cores differ (for example PSX: Beetle on Android, pcsx_rearmed on Linux). Likely byte-compatible memory cards, but not checked (inf).

**G4 HIGH (A->L). Linux cannot launch most Android system choices, and nothing falls back.**
- Package launches are refused: "no Linux emulator for package" (Launcher.cpp:414-415). That covers PSP (PPSSPP), ScummVM, Dreamcast (Flycast standalone), N64 (M64Plus FZ) and NDS (WatermelonDS, DraStic, SeedlessDS). PPSSPP 1.20.4 and ScummVM 2026.3.0 are built (ppsspp.mk, scummvm.mk).
- A missing core gives a hard failure, "core ...: No such file" (Launcher.cpp:196-197), with no fallback. PSX's Android default `mednafen_psx_hw` is not built on Linux. Plan §6.4 (line 703) requires a fallback.

**G5 HIGH (A->L, L->A). Emulator labels are Android-only and get destroyed.**
- Labels come only from ES-DE's `android` table (NanoEsdeEmulators.inc:1-4). There is no `linuxarm` table (grep).
- `esdeEmulatorKey` recognises only `*_libretro_android.so` (NanoEsdeSystemName.cpp:151-155).
- Applying a label needs `coreSoExists` on `/data/data/com.retroarch.aarch64/cores/...` (NanoMenuXmb.cpp:3345-3351) or `packages.list`. So per-game `<altemulator>` and system `<alternativeEmulator>` labels are not applied on Linux (inf).
- L->A: when a Linux user picks an emulator with no Android label, `esdeStoreSaveSystemEmus` **deletes** `<alternativeEmulator>` (NanoMenuPS3EmuCatalog.cpp:911-915). Android then loses its system choice.
- `esdeStoreSaveGameEmu` clears `<altemulator>` and writes `{game,label,am}` with an Android `am` string (NanoEsdeStore.cpp:1538-1571). Android loads an entry only when `am` matches its catalogue (1505-1508).

**G6 HIGH (Linux safety). `packageInstalled` fails open on Linux.**
- `/data/system/packages.list` does not exist on Linux, so every Android package reads as installed (NanoMenuXmb.cpp:3353-3366).
- So the Linux paths for the emulator_config move and sync, the app/ mirrors and BIOS installs (NanoSdMove.cpp:1076-1365, 2248-2386; NanoEmuSync.cpp:33-56; NanoBiosWorker.cpp:595) would act as though DraStic, Dolphin and the other Android apps were present (inf).
- Risk: writes into `/data/data/<pkg>`-shaped paths on Linux ext4, or edits to the card's Android `app/` mirrors.
- Under OA-4 this check belongs in platform/firmware.

**G7 HIGH (A->L). BIOS on the card is unusable on Linux.**
- The BIOS worker accepts a target only when its realpath is under `/data/media/0`, `/mnt/media_rw/<vol>` or `/data/data/<pkg>` (NanoBiosWorker.cpp:558-575; NanoBiosUtil.cpp:65-80). On Linux those are links to `/storage/...` (S00base:103; 10-android-links.sh:75), so every target is refused (inf).
- The RetroArch system folder is read only from Android cfg paths (NanoRaConfig.cpp:463-466). The fallback is internal `/storage/emulated/0/RetroArch/system` (NanoBiosWorker.cpp:54, 519).
- RetroArch on Linux is never pointed at `emulator_config/retroarch/system` (G3), so cores find no BIOS from the card.
- BIOS for cores that replace Android standalones are not mapped: Flycast's `data/` to `system/dc/`, DraStic's `system/` to melonDS DS (inf).

**G8 HIGH (L->A, Cube). Paths Linux writes do not resolve on the Cube.**
- Linux writes `/storage/<serial>/...` absolute collection lines (outside ROMDirectory), names.json and index.json, and possibly ROMDirectory.
- The Cube mounts the same card at `/storage/0000...N`.
- Android prunes a names.json entry whose path fails stat (plan:912). Collections keep the line verbatim but it does not resolve (NanoEsdeStore.cpp:772, 830).
- On stock-vold Android the serial path matches, so the problem is Cube-only.

**G9 HIGH (L->A, Cube, future). The Cube's mount-order id moves.**
- OA-2's planned FAT README entry (DECISIONS.md:1090-1091), or a USB drive mounted first, can take index 1. The games volume becomes ...0002, and every Cube-written absolute path changes between visits (plan:801-802; inf).

**G10 MED (A->L, L->A). `nano/systems.json` launch tuples are Android-shaped.**
- They hold `coreSo` (it must end in `_libretro_android.so`: `esdeCoreNameOk`, NanoMenuXmb.cpp:1015-1021), `packageName` and `launchIntent`/`launchArgs` (`am`).
- On adoption, an unknown tuple is replaced by this device's tuple (NanoMenuXmb.cpp:1152-1215).
- Android rebuilds the file from structs (plan §7.6), so any Linux value written there is erased on the next Android visit. Linux reusing Android's catalogue to validate also misjudges what Linux can run (inf).

**G11 MED (A->L). An internal ROMDirectory can be stamped onto the card.**
- The move takes `romDir` from the card, else the internal one, else `sd/ROMs` (NanoSdMove.cpp:470-475, 780). esde_config.py seeds `/storage/emulated/0/ROMs` (thimsy-os-nano/scripts/esde_config.py:24-27, 147-152).
- On Linux `/storage/emulated/0` exists (S00base:100). If `/storage/emulated/0/ROMs` exists there, the re-root does not fire (it fires only when the folder is missing; NanoEsdeStore.cpp:639-652), and Linux shows the internal ROMs instead of the card's (inf).

**G12 MED (both ways). Foreign-id writes land on ext4.**
- `mkdir -p` of `/storage/00000000-...N/...` (`sdMkdirs`, `makeDirs`, an emulator) creates folders on the data partition, because only adopted mount points are immutable (Session.cpp:684; Mounter.cpp:26-41; inf).
- The data seems to vanish from the card.

**G13 MED (A->L). Video media and theme videos do not play on Linux.**
- The NDK media stubs are inert (compat/stubs/src/ndk_media.cpp header comment). Android plays `downloaded_media/<sys>/videos/*.mp4|mkv|webm|avi` and theme background videos through `AMediaExtractor`/`AMediaCodec` (NanoMenuVideo.cpp:1238; NanoThemeEngine.cpp:4823-4845).
- Linux falls back to the static image, so this is a degrade, not data loss.
- Manuals (`pdf|cbz|cbr|cb7`, NanoThemeEngine.cpp:1771): whether either side opens them was not checked (unknown).

**G14 MED (both ways). Theme choice and downloaded themes do not travel.**
- `ThemeSet`, variant, colour scheme, font size and aspect ratio live only in `persist.gammaos.nano.esde.*` (NanoMenuEsdeMenu.cpp:53-59; NanoThemeEngine.cpp:504-508). They are not in `kEsdeSettingProps`.
- Downloaded themes go to `/data/system/nano_esde_themes` (NanoMenuEsdeMenu.cpp:898-905, 931-932).
- The owner listed "ES-DE settings", which in ES-DE proper include `ThemeSet` in es_settings.xml.

**G15 MED (A->L). Emulator data of the same emulator is not wired on Linux.**
- PPSSPP `PSP/SAVEDATA` and ScummVM saves are format-compatible (inf), but no Linux mapping exists.
- Flycast BIOS and VMUs, and DS (`.dsv` against melonDS DS `.srm`/`.sav`) and N64 (FZ `.eep/.mpk/.sra/.fla` against one `.srm`) saves, need converters (plan:636-637, 662; open).

**G16 MED (FAT32 only).**
- The kernel pins only `EXFAT_FS`, `EXT4_FS` and `EFI_PARTITION` (linux/tools/thimsy-import/kernel-critical.conf). It does not pin `VFAT_FS`, `NLS_UTF8`, `NLS_CODEPAGE_437` or `EXFAT_DEFAULT_IOCHARSET="utf8"`.
- Non-ASCII ROM and media names (Japanese titles) depend on the exFAT iocharset (inf).
- With exFAT kept, only the iocharset matters.

**G17 MED (two-slot Linux).** `cardRoot()` prefers the boot card's games drive over an Android card in slot 2 (nano/platform/linux/Volumes.cpp:176-186). An Android card inserted in a second slot is not adopted as the store.

**G18 LOW (both ways). `<lastplayed>` is local time with no zone** (`%Y%m%dT%H%M%S`, NanoEsdeStore.cpp:957-964). Devices in different time zones mis-order Recently Played. ES-DE uses the same format, so this is not a contract break.

**G19 LOW (both ways). Play starts are kept on the device until flushed.** `nano_esde_played.txt` stays in `/data/system` until the card is in (NanoEsdeStore.cpp:968-1028). Starts made with the card out never reach it if the card is moved to the other device first.

**G20 LOW (A->L). Duplicate PSP folder entries.** The `<folder><folderlink>` PSP entries seeded by esde_config.py:107-124 are matched only during the move. Store edits may add a second `<game>` for the same title (NanoEsdeStore.cpp:399-404 against NanoSdMove.cpp:603-614).

**G21 LOW. `custom_systems/es_systems.xml` holds Android-only `<command>` strings** (NanoEsdeEmulators.inc:7-60, 953-954). Nano never reads them, so this is harmless until a real ES-DE runs on Linux (D9, not shipped).

**G22 LOW. Android-only files Linux must leave alone:**
- `emulator_config/<emu>/app/`.
- RetroArch `retroarch.cfg`, `config/` and `playlists/*.lpl` (Android core paths).
- Android-side per-emulator inis (Flycast `emu.cfg`, ARMSX2 inis, PPSSPP `PSP/SYSTEM`).
- `Android/`, `LOST.DIR`.
- Linux has no code that touches these today, so this is a rule to keep, not a bug.

**G23 LOW (A->L). fsck repairs.** Linux runs `fsck.exfat -p` before every mount (Session.cpp:647; AUDIT.md:17). It can change files Android wrote. This is intended (the Cube's torn-clean exFAT, plan §7.8).

**G24 LOW (both ways). Non-atomic writes.**
- Plan §7.8 wants temp, `fsync`, rename, then `fsync` of the directory for every store write.
- `esdeGamelistFlush` re-prints whole documents (NanoEsdeStore.cpp:368-481). Whether every writer renames atomically was not checked (unknown).
- A pull during a write can tear a gamelist on either side.

Formats that are already fine both ways:
- Gamelist `<path>` (relative, case-insensitive match).
- The media layout and names (png, jpg, jpeg and webp, read and written: NanoScraper.cpp:144, 501).
- `%ROMPATH%` collection lines.
- `CollectionSystemsCustom`, and the scraper keys in es_settings.xml.
- Unknown gamelist tags are kept.
- On stock-vold Android, `/storage/<serial>` paths match.
- HEIF and AVIF: not a card-media gap, because Nano's media extension list excludes them on both sides (NanoThemeEngine.cpp:1769-1774). They matter only for theme images (I30).

======================================================================
## 2. Proposed card contract (to become LIBRARY-FORMAT.md; extends plan §7.2-7.10, android_launcher_plan.md:158-161)
======================================================================

**C0. Filesystem.**
- One exFAT `games` volume (FAT32 accepted, best-effort).
- No symlinks on the card.
- Names avoid `: * ? " < > |` and trailing dots.
- UTF-8 names.
- Every structured file is written as temp, fsync, rename, then fsync of the directory.
- Readers ignore `Android/`, `LOST.DIR`, `System Volume Information` and dot-files.

**C1. Identity and paths. Nothing new is absolute.** Path forms, best first:
- (a) Relative to the owning folder: gamelist `./rel`; media found by name.
- (b) `%ROMPATH%/<system>/<rel>`, for collections.
- (c) `<folder>/<rel>`, a game key relative to ROMDirectory, for names.json, index.json and emulators.json.
- (d) `%CARDID%/<rel>`, anything else on the store's own volume.
- (e) Absolute `/storage/<id>/...`, only for files on another volume. It is never written for the store's own volume.

Every reader:
- Accepts (a) to (e).
- Accepts the legacy forms `/storage/<serial>`, `/storage/00000000-0000-0000-0000-NNNNNNNNNNNN`, `/mnt/media_rw/<id>`, `/sdcard`, `/data/media/0` and `/storage/emulated/0` (legacy is read-only).
- Resolves any id that the card's own alias list (C2) names to the store's volume.
- Converts legacy paths to (b), (c) or (d) on its next save.
- `ROMDirectory` is written only when empty, preferably as `%CARDID%/ROMs` or `~/../ROMs`. The token form needs the owner's decision (Q2), because mainline ES-DE does not understand `%CARDID%`. Readers apply today's re-root rule, extended in the implementation steps below to "the stored volume is not the store's volume, and the same relative path exists on the store's volume".

**C2. `emulator_config/card.json` (hint plus alias list).**
- `{"version":1, "aliases":["0000...0001","9602-C755"], "writers":[...], "emulators":{...}}`.
- Every platform appends the id it currently sees for this volume to `aliases`. The Cube adds its mount-order id; Linux and stock Android add the serial.
- Readers treat every listed id as "this card". When the file is missing, Linux also builds the alias list from the Cube prefixes it finds in the card's own files (D20(b)).
- `card.json` stays a hint, never a safety gate (plan §7.7).

**C3. `nano/format.json`.**
- `{"version":2}` once C1 lands (android_launcher_plan.md:158).
- A reader that meets a higher major version opens the card read-only and says so.

**C4. Shared ES-DE files (both platforms read and write them in ES-DE's own formats):**
- `settings/es_settings.xml`: `ROMDirectory`, `CollectionSystemsCustom`, the scraper keys, and (if Q3 = yes) `ThemeSet`, `ThemeVariant`, `ThemeColorScheme`, `ThemeFontSize` and `ThemeAspectRatio` with ES-DE's key names. Flat line edits only; unknown keys are kept.
- `gamelists/<folder>/gamelist.xml`:
  - Keep unknown tags. Write `lastplayed` as today.
  - Handle `<folder>` entries the way `<game>` entries are handled.
  - Emulator labels follow C6.
- `collections/custom-*.cfg`: forms (b), (d) or (e). Unresolved lines are kept verbatim.
- `downloaded_media/<folder>/<type>/<name>.<ext>`:
  - Writers use png, jpg or webp only (images), and mp4 only (videos).
  - Readers accept everything in NanoThemeEngine.cpp:1769-1774.
  - A side that cannot render a type shows the next type, never deletes.
- `themes/<set>`: readable by both. If Q3 = yes, downloads go here instead of `/data/system`.
- `custom_systems/es_systems.xml`:
  - Append-only.
  - Both read `name`, `fullname`, `path`, `extension`, `platform` and `theme`.
  - `<command>` lines are written per C6 and ignored when they cannot be interpreted.

**C5. Nano files.**
- **`nano/systems.json`:** Android's launch tuples. Linux reads it and never writes launch fields to it. Linux may write the shared non-launch fields (folders, extensions, icon); whether it may is Q4.
- **`nano/systems.linux.json` (new):** `{"version":1, "systems":{"<sysId>":{"key":"core:pcsx_rearmed"|"emu:ppsspp", "set":"<iso date>"}}}`. Linux's per-system choice. Android ignores it.
- **`nano/emulators.json`:** `{version:1, items:[{game:"<folder>/<rel>", label?, am?, key?, linux?:{key}}]}`. `key` is the portable family key (C6). Android keeps unknown fields (plan §7.6 confirms the in-place edit).
- **`nano/names.json`, `nano/index.json`:** game keys in form (c). Box and fan paths are relative to `downloaded_media`. No platform-only fields, because Android rebuilds both from structs. Linux does not write them until Android's C1 change ships (plan §7.6). Until then Linux writes titles to gamelist `<name>`.
- **`nano/played.journal` (optional, Q6):** unflushed play starts travel with the card.

**C6. Emulator choice, stored portably.**
- **Family key vocabulary:**
  - `core:<libretro name without suffix>`, normalised through an alias map (`mupen64plus_next_gles3` to `mupen64plus_next`, `mednafen_psx_hw` to family `psx-beetle`, and so on).
  - `emu:<id>` for a standalone, platform-neutral id (`emu:ppsspp`, `emu:scummvm`, `emu:flycast`, `emu:dolphin`, `emu:azahar`, `emu:melonds`, `emu:drastic`, `emu:m64plus-fz`, `emu:armsx2`, `emu:hakux`, `emu:vita3k`, `emu:seedlessds`).
  - `family:<name>` where a family has save-compatible members (`family:melonds`: WatermelonDS, the melonDS DS core).
- **Gamelist labels:** a writer writes `<altemulator>` or `<alternativeEmulator>` only when the label exists in BOTH the `android` and the `linuxarm` ES-DE table for that system (plan §7.6). Otherwise it writes the key into emulators.json (per game) or systems.linux.json (per system) and **leaves the existing label untouched**. Never delete another platform's label.
- **Resolution on each platform:**
  1. The per-platform choice (systems.linux.json, or Android's own fields).
  2. The shared key or label mapped through the family table.
  3. The device default for that system.
  4. "Unavailable on this device", shown, with the entry kept.
  
  A missing core or emulator always falls through to the next step, never to a launch error.

**C7. emulator_config.**
- **Shared data** (both read and write the same files):
  - `retroarch/system/` (BIOS), `retroarch/cheats/`.
  - `ppsspp/PSP/{SAVEDATA,Cheats,TEXTURES}`.
  - `scummvm/saves/` (once Q8 confirms the target ids match).
  - `bios/`, the drop folder.
- **Shared by copy rule, never by file identity:** Flycast `data/` BIOS imported into Linux `retroarch/system/dc/`, and DraStic `system/` BIOS into the melonDS DS core's BIOS names. These are copies on Linux's side, recorded in `card.json`.
- **Per platform, on the card:**
  - Android: `<emu>/app/**`, plus the Android-written files directly under `<emu>/` (`retroarch.cfg`, `config/`, `playlists/`, Flycast `emu.cfg`, ARMSX2 inis, PPSSPP `PSP/SYSTEM`, `PPSSPP_STATE`, DraStic, drastic-nano, FZ, Azahar, Dolphin, hakuX, Vita3K and SeedlessDS folders).
  - Linux: `<emu>/linux/**` only. `retroarch/linux/states/<system>/` and `retroarch/linux/saves/<system>/` hold saves only when a system's Linux core differs from Android's.
  - `<emu>/linux/` never holds an absolute card path.
- **Per device, internal, never on the card from Linux:** RetroArch main cfg, core options, remaps, overrides, shader caches, autoconfig (plan §7.5).
- **In-game saves:** beside the ROM (Android's current behaviour) only for systems whose Linux and Android emulators share the save format. That is the family table's `saveCompatible` column, plus its round-trip test. Other systems on Linux save to `retroarch/linux/saves/<system>/` until a converter exists.
- **Save states:** never beside the ROM from Linux; always under `retroarch/linux/states/`.

**C8. What each side must tolerate.**
- **Both:** unknown files and folders, unknown XML tags and JSON fields (kept on rewrite), labels and keys they cannot run (kept, shown as unavailable), legacy absolute paths (C1), a missing `format.json` or `card.json`, and gamelists or media from mainline ES-DE.
- **Linux additionally:**
  - Android's `app/`, `Android/` and RetroArch Android config files: never read, never edited.
  - `custom_systems` Android commands: ignored.
- **Android additionally (android-surface):**
  - `linux/` folders, `systems.linux.json` and the `key` fields: never deleted.
  - A launch tuple it does not know: kept, not replaced (android_launcher_plan.md:161, Phase 3).
  - Form (c) and (d) paths: resolved.
  - Serial ids listed in `card.json` aliases: treated as this card.

======================================================================
## 3. Implementation steps (OA-4: platform specifics only in nano/platform/*, shared code platform-free)
======================================================================

New or extended nanoplat interfaces (NanoPlatform.h currently has Display, Launcher, Audio, Volumes, Power, SystemActions and Network):

**I-1. Extend `nanoplat::Volumes`:**
- `canonicalStorePath(path)`.
- `cardAliases(vol)`.
- `isForeignId(path)`.

Firmware implements the Android forms: `/mnt/media_rw`, `/data/media/0` and `/sdcard` normalisation, which today sits in shared code (NanoEsdeStore.cpp:108-113, 135-139, 632-637). Linux implements `/storage/<serial>` plus the alias map. Move `esdeAppPath`, `esdeRawPath` and the prefix whitelist behind it.

**I-2. A new `nanoplat::Emulators` interface:**
- `installed(key)`. This replaces `packageInstalled`. Firmware keeps the packages.list logic; Linux answers from the image's emulator manifest (apps-v1.fragment) and fails **closed**.
- `familyOf(label | coreSo | pkg)`.
- `labelTable()`. Firmware: `android`. Linux: `linuxarm`.
- `defaultFor(system)`.
- `biosBase(slot)`.
- `emulatorConfigRules()`: which `<emu>` subfolders are shared or per-platform.
- `supports(Capability)` for "emu move", "app mirror sync" and "BIOS install".

The shared family and alias table (pure data, `nano/NanoEmuFamilies.inc`, generated from ES-DE's `android` and `linuxarm` tables at one pinned ES-DE commit plus a hand-kept alias and `saveCompatible` list) is platform-free.

**I-3. A new `nanoplat::Settings` (portable settings store, per OA-4's agreed shape).** Theme keys and ES-DE keys go through it. The shared code maps them to es_settings.xml when store mode is on (Q3).

Linux steps (platform/linux unless noted):

- **L1. Store mode on by default (G1).**
  - At first run, platform/linux supplies a default store `<cardRoot()>/ES-DE` when `cardRoot()` is non-empty, else `/storage/emulated/0/ES-DE`. This runs through a `Volumes::defaultStoreDir()` hook, so the shared code no longer hard-codes the fallback (NanoEsdeStore.cpp:58-59, 120-126).
  - The shared store must also accept the internal store as store mode (store mode is on whenever the dir is absolute, which it already is).
  - Run the merge path of `esdeMoveAuto` without the Android `am force-stop` (IMPLEMENTATION.md:255). The force-stop goes behind `Launcher` or `SystemActions` with an unsupported return on Linux.

- **L2. D20 alias map (G2, G9).**
  - At adoption, scan es_settings.xml, collections, names.json, index.json, emulators.json and the text configs in emulator_config (three levels deep, skipping `app/`) for the prefix `/storage/00000000-0000-0000-0000-[0-9A-F]{12}/`. Add any that is absent on this device to the alias list. Merge with `card.json` aliases.
  - Resolve those aliases on read in `canonicalStorePath`.
  - Extend `emusync::isVolumeId` and `repoint` to 36 characters (NanoEmuSync.cpp:131-153). This is shared parsing. Linux repoint writes only under `<emu>/linux/`.

- **L3. Foreign-id guard (G12).**
  - Shared `makeDirs`/`sdMkdirs` call `Volumes::isForeignId()` and refuse.
  - Linux additionally keeps `/storage` itself non-writable for unknown top-level names. Options: a root-owned `/storage` that is chattr'd, or a guard in thimsy-session. The choice is a design decision.

- **L4. Launcher (G3, G4).**
  - Write a per-launch `--appendconfig` (`/run/thimsy/ra-append.cfg`) with:
    - `system_directory=<card>/emulator_config/retroarch/system`
    - `cheat_database_path=<card>/emulator_config/retroarch/cheats`
    - `savestate_directory=<card>/emulator_config/retroarch/linux/states/<system>`, with `savestates_in_content_dir=false`
    - save settings from the family table: `savefiles_in_content_dir=true` for compatible systems, else `savefile_directory=<card>/emulator_config/retroarch/linux/saves/<system>` with `savefiles_in_content_dir=false`
  - Add package launches for `emu:ppsspp` (`--memstick`, or PPSSPP's `memstick_dir.txt` equivalent pointing at `<card>/emulator_config/ppsspp/linux/` with `PSP/SAVEDATA`, `Cheats` and `TEXTURES` bind-mounted from the shared folders; bind mounts, because exFAT has no links) and `emu:scummvm` (`--savepath=<card>/emulator_config/scummvm/saves`, `--config=/storage/.config/scummvm/scummvm.ini`).
  - Fall back to `Emulators::defaultFor(system)` when a core is missing (Launcher.cpp:196-197, 414-415).

- **L5. Labels (G5).**
  - Teach `esdeEmulatorKey` both suffixes (NanoEsdeSystemName.cpp:149-158), as shared parsing.
  - Replace `coreSoExists`'s Android path (NanoMenuXmb.cpp:3345-3351) with `Emulators::installed`.
  - Change the deletion at NanoMenuPS3EmuCatalog.cpp:911-915 and the per-game clear at NanoEsdeStore.cpp:1538-1571 to "keep the existing label, write the key". This is shared code, so it is android-surface (it changes Android writes too).

- **L6. Systems.** Read `nano/systems.json` and map `coreSo` and `package` through the family table. Write Linux's choices only to `nano/systems.linux.json`. Never let the adoption code (NanoMenuXmb.cpp:1152-1215) write Linux tuples back.

- **L7. BIOS (G7).**
  - Linux `Emulators::biosBase` returns card paths under `/storage/<id>/...`.
  - Move the realpath whitelist (NanoBiosWorker.cpp:558-575; NanoBiosUtil.cpp:65-80) behind the platform.
  - Add copy rules for Flycast to `system/dc/` and DraStic to melonDS DS.
  - Linux reports "unsupported" for the app/ mirror sync and the emulator_config move from Android app folders (G6).

- **L8. ROMDirectory re-root fix (G11).** Shared code (android-surface): prefer the store volume's relative path over a device-internal path whenever the stored ROMDirectory is on a different volume from the store and the store volume has that folder. Stop the move from stamping an internal path onto the card (NanoSdMove.cpp:470-475, 780).

- **L9. Kernel and filesystem (G16).** Pin `CONFIG_EXFAT_DEFAULT_IOCHARSET="utf8"`, `CONFIG_VFAT_FS`, `CONFIG_NLS_UTF8` and `CONFIG_NLS_CODEPAGE_437` in kernel-critical.conf. Optionally mount exFAT with `quiet` (so chmod and chown on the card do not fail with EPERM).

- **L10. Video (G13, optional for v1).** Either a Linux backend for the NDK media shim (FFmpeg or V4L2 stateless on RK3566/H700), or accept static fallback and document it in the contract as "videos: optional to render".

- **L11. Two-slot policy (G17).** `cardRoot()` prefers whichever card already holds `ES-DE/` with a newer `card.json` writer entry, and asks the user when both do (Q7).

- **L12. Durability (G24).** Shared write helper: temp, fsync, rename, then fsync of the directory, for gamelist, collections, settings and nano/*.json (android-surface).

Android changes needed (android-surface; per OA-4, verified moves H.3 and the AOSP build plus the owner's smoke test before nano-144):

- **A1.** C1 for `names.json` and `index.json` (android_launcher_plan.md:160), plus `format.json` v2 and conversion on first save. **This must ship before Linux writes either file.**
- **A2.** Keep unknown launch tuples, `systems.linux.json` and the `key` fields (android_launcher_plan.md:161; plan §7.6).
- **A3.** The label rule in C6. Stop writing Android-only labels (for example "M64Plus FZ (Standalone)") into gamelists; write the key instead. Old cards still carry such labels, and Linux reads them through the family table.
- **A4.** Write a `card.json` alias for the Cube id, and read the serial alias. Better still, D20(c): GammaOS Next vold names public volumes by serial. That removes G2, G8 and G9 at the root for new writes.
- **A5.** Resolve `%CARDID%` and form (c) everywhere. Accept a serial path whose id is in `card.json` aliases as this card (fixes G8).
- **A6.** The theme keys go into es_settings.xml through the shared settings store, and themes download to `<store>/themes` (if Q3 = yes).
- **A7.** Linux-aware wording for the `emuSdForeign` dialog (plan §7.10). Android's emulator_config move and id-repair skip `linux/`; the repair already edits text there but finds no absolute paths (rule 6).
- **A8.** Move the Android-only parts of NanoEsdeStore, NanoSdMove, NanoEmuSync and NanoBiosWorker (`am`, `/data/data`, packages.list, `/mnt/media_rw`) into platform/firmware behind I-1 and I-2 (the OA-4 ratchet).

Suggested order:
1. L1, L3, L4 (append config, states off the ROM folder) and L5's "never delete labels". These stop data loss first.
2. L2 and A4.
3. A1, A2 and A3, then L6.
4. L7 and the PPSSPP and ScummVM launches.
5. L8, L9 and L12.
6. Theme and video work.

======================================================================
## 4. Round-trip test plan (host-side, no device needed for the first three tiers)
======================================================================

Fixtures (`tests/fixtures/card/`, built as directory trees and also as loop-mountable exFAT and FAT32 images by a host script):

- **F-A-stock:** an Android-written card at `/storage/9602-C755`. Its contents:
  - es_settings.xml with an absolute ROMDirectory and the scraper keys.
  - Gamelists for nes, psx, n64, nds, psp and scummvm, with favourite, hidden, name, desc, rating, releasedate, playcount and lastplayed.
  - `<altemulator>` for "M64Plus FZ (Standalone)", "Beetle PSX HW" and "PPSSPP (Standalone)".
  - `<alternativeEmulator>`.
  - A PSP `<folder><folderlink>`.
  - Two collections, mixing `%ROMPATH%` and absolute lines.
  - custom_systems with Android commands.
  - Media in every type and extension.
  - nano/systems.json, names.json, index.json (pack-in `/data/media/0` entries) and emulators.json (an `am` entry).
  - emulator_config: retroarch (cfg, system/ BIOS, cheats, config/, playlists/), ppsspp PSP/SAVEDATA plus PSP/SYSTEM, flycast data/, drastic system/ plus backup/*.dsv, m64plus GameData, `app/` mirrors.
  - `ROMs/<sys>/Game.srm` and `Game.state1` (Android states beside the ROMs).
- **F-A-cube:** the same with every absolute path at `/storage/00000000-0000-0000-0000-000000000001`. Also a variant at `...0002` (the G9 shift).
- **F-L:** a Linux-written card (generated by running the Linux host build of nano against F-A-stock after the user actions listed below).
- **F-edge:**
  - Non-ASCII names (`ドラゴン.zip`), case pairs (`Game.zip` and `game.ZIP`), a file name with a trailing dot.
  - An unreadable or truncated gamelist.
  - A `format.json` version 99.
  - A Windows `System Volume Information`.

Tiers:

**T1. Parser round trip (unit, host).** For each fixture:
- Load every file through the shared parsers, save it unchanged, and diff. Expected: byte-identical, or only C1 conversions.
- Unknown tags and fields must survive.
- Linux-only fields must survive a pass through the *Android* code's writers, compiled for host with the firmware platform stubbed (OA-8's "Android code's parsers on the host"): `esdeStoreSaveGameEmu`, `esdeSystemsJson`, the names.json and index.json writers, and `esdeStoreSaveSystemEmus`.

**T2. Behavioural round trip (host nano, e2e jail, tests/e2e/nano_jail.py extended).**
1. Mount F-A-cube in the Linux jail at `/storage/9602-C755`, with no card.json.
2. Assert:
   - Store mode is on without Setup (L1).
   - The alias map holds `...0001`.
   - Collections resolve all lines.
   - Titles from names.json and metadata from index.json show.
   - Covers show.
   - PSX launches pcsx_rearmed through the fallback.
   - N64 maps to mupen64plus_next.
   - PSP and ScummVM launch through the fake launcher with the expected argv.
   - The append cfg names the card's `system/` and `linux/states/`.
   - Nothing is created under `/storage/00000000-*` (L3).
   - `app/` is untouched (hash of the tree).
3. User actions on Linux: favourite a game; play twice; rename a title; add to a collection; set a per-game emulator with no common label (N64 core) and a system emulator for PSX; scrape one game.
4. Assert:
   - Gamelist favourite, playcount and lastplayed updated.
   - `<altemulator>` and `<alternativeEmulator>` Android labels still present.
   - `key` written to emulators.json and `systems.linux.json`.
   - systems.json launch fields byte-identical.
   - No new absolute paths anywhere (grep `/storage/` across ES-DE/ outside allowed forms).
   - card.json has the serial alias.
   - Android's `Game.state1` unchanged.
5. Load the result (F-L) in the host-compiled **firmware** nano with `/storage/00000000-...0001` as the card's mount. Assert:
   - Every Linux change is visible.
   - Android system and per-game emulator choices are intact.
   - `linux/` and `systems.linux.json` survive an Android save.
   - names.json entries are not pruned.
6. Repeat with F-A-stock and the serial mount on both sides. Repeat A to L to A to L twice and assert a fixed point (the second cycle produces no diff).

**T3. Filesystem tier (host, loop-mounted exFAT and FAT32 images, run as root in the devcontainer).** Run T2 on real filesystems. Add:
- Case-insensitive lookups.
- Symlink refusal handled (the move fails cleanly).
- Mtimes and the exFAT UTC offset.
- FAT32 local-time shift does not trigger false "changed" syncs (content hashes).
- Pull simulation: kill during a gamelist write, then assert old or new content, never torn (L12).
- `fsck.exfat -p` on an Android-dirty image leaves ES-DE files byte-identical.

**T4. Device tier (Phase 3, owner).**
- Cube to ARC (or RK3566 device) to Cube with one real card, both the one-card (games partition) and two-card set-ups. Check the T2 assertions on screen.
- Load a PPSSPP save made on Android on Linux and back.
- A RetroArch NES `.srm` both ways.
- A PSX memory card across Beetle and pcsx_rearmed (this decides the `saveCompatible` row).
- The OA-2 README partition's effect on the Cube id.

**Ratchet tests (OA-4).**
- Shared code contains no `/data/data`, `packages.list`, `_libretro_android.so` literal, `am ` or `/mnt/media_rw` outside platform/firmware.
- The family table covers every label in `mods/esde-emulators.txt` and every core in apps-v1.fragment.

======================================================================
## 5. Open questions only the owner can answer
======================================================================

**Q1. D20(c).** Will the Android build's vold be changed to name public volumes by serial (removing the Cube mount-order id), or must Linux carry the alias map indefinitely? This also decides whether OA-2's FAT README entry is safe on the Cube.

**Q2. ROMDirectory form.** Keep absolute (ES-DE-compatible, but bound to a volume id) or write a token or relative form (`~/../ROMs` works on Android ES-DE, where `~` is the ES-DE folder, but not on desktop ES-DE)? Must the card stay usable by the real ES-DE app on Android (Thimsy Home or mainline ES-DE)? That constrains every format choice.

**Q3. Should the theme travel?** That covers ThemeSet, variant, colour scheme, font size and aspect ratio in es_settings.xml, and downloaded themes in `ES-DE/themes` on the card. Some theme choices are screen-specific (aspect ratio), which may argue for per-device.

**Q4. Who owns `nano/systems.json`?** May Linux write the non-launch fields (folders, extensions, icons)? Or is the system list Android-owned, with Linux strictly a reader plus `systems.linux.json`?

**Q5. Save states.** Is it acceptable that states never cross platforms in v1, which is what the plan says? And should in-game saves for systems with different cores (PSX Beetle against pcsx_rearmed; N64 FZ against mupen64plus_next; NDS WatermelonDS, DraStic or SeedlessDS against melonDS DS) stay separate per platform until a converter is written? Or do you want the converters (N64 split files to `.srm`, `.dsv` trim) in scope for OA-8?

**Q6. Play journal.** Should unflushed play starts be written to the card (a `nano/played.journal`) so they travel, at the cost of more card writes?

**Q7. Two-slot devices.** When both the boot card's games drive and a second card hold an `ES-DE/` folder, which wins: ask, newest writer, or always the slot-2 card?

**Q8. Standalones present on both sides (PPSSPP, ScummVM).** Is linking only the save folders, while settings stay per platform, the intended behaviour? Or should controls and graphics settings also stay per device?

**Q9. Emulators with no Linux counterpart** (Dolphin, Azahar, ARMSX2, hakuX, Vita3K, Android games). Show those systems on Linux as "unavailable on this device", or hide them?

**Q10. "Playlists".** Does "playlists" mean ES-DE custom collections only, or also RetroArch `.lpl` playlists? The latter are Android-only today and the plan keeps them internal on Linux.

**Q11. Videos and manuals.** Is a static image in place of game and theme videos acceptable for Linux v1, or is a Linux video decoder (L10) required for OA-8? Should manual (PDF and comic archive) viewing be part of the contract?

**Q12. Compatibility floor.** Which Android release is the floor that implements this contract (A1 to A8)? Until it ships, Linux must hold back on names.json and index.json writes, and Android-only labels keep arriving from older builds.

Key files:
- C:/Users/krist/code/thimsy-os-linux/nano/NanoEsdeStore.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoSdMove.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuXmb.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuPS3EmuCatalog.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoEsdeSystemName.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoEmuSync.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoBiosWorker.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoThemeEngine.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoPlatform.h
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/Launcher.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/Volumes.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/compat/stubs/src/ndk_media.cpp
- C:/Users/krist/code/thimsy-os-linux/linux_handheld_plan.md (§7.2-7.10, lines 760-958)
- C:/Users/krist/code/thimsy-os-linux/android_launcher_plan.md (lines 54-61, 158-161)
- C:/Users/krist/code/thimsy-os-linux/docs/linux/DECISIONS.md (D8:56-59, D20:144-148, OA-4/OA-8:1096-1102)