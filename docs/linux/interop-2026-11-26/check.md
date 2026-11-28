> **Superseded where it differs (2026-11-27).** This is a raw report, a snapshot of 2026-11-26. `docs/linux/DECISIONS.md` OA-46 to OA-54 replaced its model: settings and picks are per device, saves, states and BIOS are shared on the card apart per core, and there is no `<emu>/linux/`, `retroarch/linux/` or `systems.linux.json`. The current contract is `LIBRARY-FORMAT.md`.

OA-8 adversarial check of the interop contract and gap list (read-only; nothing edited, built or run). Repo: C:/Users/krist/code/thimsy-os-linux. "(inf)" marks inference. In this report, file:line is `nano/` unless another path is given.

## 1. Spot-checks (18 claims)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | G1: store dir property, `/storage/emulated/0/ES-DE` fallback, store mode only when the property starts with `/` | CONFIRMED | NanoEsdeStore.cpp:58-59, 120-132 |
| 2 | L1: "store mode is on whenever the dir is absolute, which it already is" | WRONG | `esdeStoreMode()` reads the raw property, not `esdeDataDir()` (NanoEsdeStore.cpp:128-132). The fallback never counts as store mode. See C-1. |
| 3 | G2: `isVolumeId` (9 or 32 hex) and `repoint`'s 32-character cap | CONFIRMED | NanoEmuSync.cpp:131-140, 150-153. A Cube id read to the cap holds dashes and fails `isVolumeId`. |
| 4 | G5: `esdeStoreSaveSystemEmus` deletes `<alternativeEmulator>` when Nano's choice has no label | CONFIRMED | NanoMenuPS3EmuCatalog.cpp:908-913. It fires only on a change, not on adoption (872-883). |
| 5 | `esdeEmulatorKey` knows only `_libretro_android.so` | CONFIRMED | NanoEsdeSystemName.cpp:149-158. |
| 6 | G6: `packageInstalled` fails open; `coreSoExists` checks the Android path | CONFIRMED | NanoMenuXmb.cpp:3345-3366. |
| 7 | G7: BIOS realpath whitelist refuses targets on Linux | CONFIRMED (inf at runtime) | NanoBiosWorker.cpp:557-575; `canonical()` calls realpath (NanoBiosUtil.cpp:71-80). `/data` is a link to `/storage/.config/thimsy/data` (tools/nano-tree/path-roots.json "/data" row), so `R+"/data/data/<pkg>"` never matches either. |
| 8 | G4: launcher argv, hard failure on a missing core, packages refused | CONFIRMED | platform/linux/Launcher.cpp:196-197, 398-415. |
| 9 | G17: `cardRoot()` prefers the boot card when both cards are SD | CONFIRMED, wrong lines | platform/linux/Volumes.cpp:103-116, not 176-186 (those lines are `isSlot`). |
| 10 | G11: the move stamps an internal ROMDirectory | PARTLY | NanoSdMove.cpp:470-475 uses the internal value only when it `access()`es on this device. On Linux, `/storage/emulated/0/ROMs` is not created (S00base:96-100), and Linux has no internal es_settings.xml. This is essentially an Android-to-Android problem; downgrade it to LOW for Linux. |
| 11 | Scraper writes png, jpg and webp only | PARTLY | Images: yes (NanoScraper.cpp:144-146). Videos keep mp4, mkv, webm or avi, and manuals pdf (NanoScraper.cpp:500-503). Covers always get a `.png` name whatever the bytes (NanoEsdeStore.cpp:325; NanoScraper.cpp:807-812). `dropOtherArt` deletes the same stem's other image extensions (NanoScraper.cpp:151-158). C4 "mp4 only" does not match today's Android writer. |
| 12 | G16: kernel-critical.conf pins only `EXFAT_FS`, `EXT4_FS` and `EFI_PARTITION` | CONFIRMED | linux/tools/thimsy-import/kernel-critical.conf. |
| 13 | G14: theme keys are not in `kEsdeSettingProps`; themes download to /data/system | CONFIRMED | NanoEsdeStore.cpp:1704-1723; NanoMenuEsdeMenu.cpp:53-59, ~896-932. |
| 14 | Collection line forms; unresolved lines kept | CONFIRMED | NanoEsdeStore.cpp:677-689, 760-772. |
| 15 | G3 source: `*_in_content_dir=true` | CONFIRMED, wrong file | The fixture is thimsy-os-linux/nano/test/fixtures/retroarch/retroarch_shipped.cfg:42, 44, 46, not thimsy-os-nano. Line 46 also has `screenshots_in_content_dir="true"`, so Android puts screenshot PNGs beside the ROMs too (an on-card file the inventories missed). |
| 16 | G24: "whether writers rename atomically: unknown" | RESOLVED | `esdeWriteAtomic` writes a temp file, fsyncs, then renames (NanoEsdeStore.cpp:72-85). It is used for gamelists (470), collections (832), es_settings (939), emulators.json (1571) and custom_systems (1695). systems.json fsyncs and renames (NanoMenuXmb.cpp:1107-1112). names.json (NanoMenuScraper.cpp:443-451) and index.json (199-205) rename **without** fsync. No writer fsyncs the directory; the Linux exFAT mount has `dirsync` (VolumePolicy.cpp:117-127). L12 shrinks to names.json and index.json. |
| 17 | Contract C5: `emulators.json` keeps unknown fields | PARTLY | Other games' items are copied whole, so extra fields survive (NanoEsdeStore.cpp:1556-1561). The item for the game being changed is rebuilt as `{game,label,am}`, so its `key`/`linux` fields are lost whenever Android changes that game's emulator. |
| 18 | systems.json is volume-bound on the Cube | NOT A GAP | Scan sources are written as `%CARDID%` and substituted on read (NanoMenuXmb.cpp:1023-1028, 1144-1145). Cube-written systems.json is portable. |

## 2. Corrections to the gap list

**C-1 (L1).** Setting a default only through `Volumes::defaultStoreDir()` does not turn store mode on (row 2).
- If Linux sets the property to `/storage/emulated/0/ES-DE`, `esdeStoreFollowCard` refuses to follow a card. It skips `vol=="emulated"` (NanoEsdeStore.cpp:161).
- So L1 must set the property to `<cardRoot()>/ES-DE` (as `esdeMoveAuto` does, NanoSdMove.cpp:983-987).
- On a card with no ES-DE folder, `esdeMoveAuto` creates nothing (NanoSdMove.cpp:975-990).
- Making the internal path count as store mode in shared code would change Android's internal-mode behaviour (android-surface).

**C-2 (G2, names.json): today's code deletes, it does not just fail to resolve. CRIT.**
- `saveRomNameOverrides` prunes every entry whose `rom` fails `stat`, on both platforms (NanoMenuScraper.cpp:430-438).
- On Linux, the first names.json save on a Cube-written card drops every Cube-path title. Titles survive only where they are mirrored into a gamelist `<name>` (`esdeStoreSaveNames`).
- The reverse also holds: the Cube prunes Linux's serial paths.

**C-3 (G2, index.json): box and fan paths are cleared on load, then saved cleared. Both directions.**
- `artOk` keeps `box`/`fan` only when they start with `esdeDataDir()+"/downloaded_media/"` (NanoMenuScraper.cpp:154-161).
- A Cube path read on Linux (`/storage/0000…1/ES-DE/...`, against `/storage/<serial>/ES-DE`) fails that check, and the next save writes the entry without art. The Cube does the same to Linux paths.
- Media are still found by name, so covers show. The index loses information on every crossing.
- The inventory also missed index fields: `scraper` (NanoMenuScraper.cpp:218).

**C-4 (G5): two install checks fail in opposite directions on Linux, and Android per-game choices *are* applied.**
- `packageInstalled` fails open (NanoMenuXmb.cpp:3366).
- `installedPackages()` returns an empty set when packages.list is missing (NanoMenuPS3EmuCatalog.cpp:187-199), so it fails closed. `esdeCatalogForLabel` uses it, plus `coreSoExists` (NanoMenuPS3EmuCatalog.cpp:766-782). So `<altemulator>` labels are not applied on Linux, as claimed.
- But `nano/emulators.json` entries load with **no install check**: only the `am` must match the catalog, which is the same catalog on Linux (NanoEsdeStore.cpp:1501-1510).
- So an Android per-game standalone choice is applied on Linux, and that game's launch then fails with "no Linux emulator for package" (Launcher.cpp:414-415) (inf from code flow).
- Shared code already has the fail-open guard pattern: the systems heal skips when packages.list is unreadable (NanoMenuXmb.cpp:1331-1336, 1369).

**C-5 (G10): severity is HIGH, not MED, and it ping-pongs.**
- Android tuples are "known" on Linux: any `esdeCoreNameOk` core, or any catalog tuple (NanoMenuXmb.cpp:1156-1171). On first contact the card wins (synced hash 0; NanoMenuXmb.cpp:1137-1140).
- So Linux adopts Android's tuples wholesale. For example PSX becomes `mednafen_psx_hw`, which Linux does not build.
- A later Linux edit pushes Linux tuples, which Android also accepts as known.
- Each platform's change overwrites the other's system choices through the one file.

**C-6 (G3): the premise is probably inverted (inf).**
- With no save or state keys in the config, RetroArch's Unix frontend defaults `savefile_directory`/`savestate_directory` to `$XDG_CONFIG_HOME` (or `$HOME/.config`)`/retroarch/{saves,states}`, not to the content folder. This is upstream behaviour I did not check against 1.22.2's source.
- No Linux code seeds a retroarch.cfg (linux/package/retroarch has only Config.in, .mk and .hash) or sets HOME or XDG (grep).
- So the likely present-day effect is:
  - Android's `.srm` files beside the ROMs are **not seen** on Linux (in-game saves do not carry A->L).
  - Linux saves stay on the ext4 data partition (they do not carry L->A).
  - On a release build with SSH off, `/root` stays read-only (S00base:113-131), so saves may fail outright if HOME is `/root` (unknown).
- The overwrite risk exists only if a config turns `*_in_content_dir` on. L4 is still the fix.

**C-7 (G2, G8, G9 scope).** "Cube-only" may understate it.
- The plan says the mount-order naming is GammaOS Next's vold, "which the Thimsy Android build uses" (linux_handheld_plan.md:800).
- So it may apply to every Thimsy Android device on that base, not just the Cube (inf).
- "Stock-vold Android matches" holds only for non-Thimsy LineageOS.

**C-8 (L5).** Teaching `esdeEmulatorKey` the `_libretro.so` suffix is not needed today.
- Linux keeps `_libretro_android.so` names in its model (NanoMenuPS3EmuCatalog.cpp:593; Launcher.cpp:385-396).
- The real blocker is `coreSoExists`'s hard-coded `/data/data/com.retroarch.aarch64/cores/` (see N1).

**C-9 (G17).** The line reference is wrong (row 9). The behaviour is confirmed.

## 3. Gaps the inventories missed

**N1. HIGH (inf). Every libretro launch from the XMB is gated on an Android path.**
- `coreSoExists` stats `/data/data/com.retroarch.aarch64/cores/<x>_libretro_android.so`.
- Nothing on Linux creates it: grep of linux/ for `com.retroarch.aarch64` and `cores` finds nothing; S00base:96-104 makes only `data/data`.
- So NanoMenuXmb.cpp:4034-4036 (also 3834-3836 and NanoMenuOverlay.cpp:1838-1840) shows "emulator missing" before the launcher is reached.
- The host tests call `commitLaunch` directly (nano/test/host/headless/tests/host_platform_test.cpp:185-188), so this path is untested.
- This also makes `emuEntryUsable` and `emuPlatformChoices` report nothing usable (NanoMenuPS3EmuCatalog.cpp:211-224).
- It belongs in I-2 `Emulators::installed`, and it is a precondition for G4 and G5.

**N2. MED. Linux's own defaults are Android cores.**
- The built-in system table uses `nestopia`, `gambatte` and `gpsp` (NanoMenuXmb.cpp:235-243), plus the M64Plus FZ package for N64 (245-248). None of those cores are in Linux's 27-core set (IMPLEMENTATION.md:745).
- Linux has no bundled `/system/etc/gammaos/nano_systems.json` (NanoSystemsHeal.h:33; not in linux/package/thimsy-data/assets.list).
- `Emulators::defaultFor(system)` needs a Linux table; there is none to fall back to today.

**N3. MED. Other files on the card.**
- RetroArch screenshots beside the ROMs (row 15).
- Temp files: `*.nanotmp` (NanoEsdeStore.cpp:74; NanoMenuXmb.cpp:1107), `index.json.tmp.<pid>` (NanoMenuScraper.cpp:199) and `names.json.tmp` (NanoMenuScraper.cpp:445). They can be left behind after a pull.
- The `bios/` drop folder, matched case-insensitively at the card root (NanoBiosInstall.cpp:175).
- `.app` Android-game stubs in ROM folders (NanoThemeEngine.cpp:1949; NanoMenuXmb.cpp:3618).
- Syncthing folder markers, if Android Syncthing shares a card folder (NanoSyncthing.cpp:588-590) (inf).
- The sweep also watches `custom_systems/es_systems.xml` (NanoEsdeStore.cpp:1258).
- C0's ignore list and the test fixtures should include these.

**N4. LOW. Linux refuses some cards.**
- Linux does not mount an exFAT or FAT card whose serial is `0000-0000` (VolumePolicy.cpp:104-108). Android would mount it.
- Linux mounts with `noexec` (VolumePolicy.cpp:117-127). That is harmless for data; mention it in C0.

**N5. LOW (inf). App mirrors get pulled onto ext4.**
- On Linux the app/ mirror sync sees no previous sync, so it would copy the card's mirrors into `/data/data/<pkg>` on ext4.
- The mass-deletion guard (NanoEmuSync.cpp:10-14, 89-91) makes deleting the card copies unlikely. This refines G6's risk.

**N6. MED. There is no version gate on read anywhere.**
- names.json, index.json, emulators.json and systems.json loaders never check `version` (NanoMenuScraper.cpp:398-416, 151-185; NanoEsdeStore.cpp:1495-1510).
- C3 `format.json` therefore protects nothing on Android builds already in users' hands (see B1-B3).

## 4. Proposed steps that would break existing Android cards and builds

- **B1. CRIT. C1/A1: relative (form c) keys in names.json.**
  - Every shipped Android build prunes any `rom` that fails `stat` (NanoMenuScraper.cpp:430-438). A relative key never stats.
  - So the first save on an old build deletes every converted title, and its lookups by absolute path miss them anyway (NanoMenuScraper.cpp:455-460).
  - Use a new file (for example `nano/names.v2.json`) beside the legacy one, or dual-write the absolute form for the id in use, until the Q12 floor.
  - "Convert on next save" must never rewrite the legacy file in place.
- **B2. CRIT. C5: index.json `box`/`fan` relative to downloaded_media.**
  - Old builds' `artOk` clears any path not prefixed by `esdeDataDir()+"/downloaded_media/"` (NanoMenuScraper.cpp:154-161), then saves the cleared entry.
  - Relative `rom` keys also never match old builds' lookups. Same remedy as B1.
- **B3. HIGH. C3 `format.json` read-only mode.** It is ignored by every build in the field (N6). Every change must be additive (new files or fields) or dual-written, never a replacement of an existing field's meaning.
- **B4. HIGH. A3: stop writing Android-only labels.**
  - Older Android builds, and the real ES-DE app that the build ships when given its APK (thimsy-os-nano/mods/37-esde.sh:28-35), apply per-game choices only from `<altemulator>`.
  - Writing only a `key` would lose per-game choices on mixed-version fleets and in ES-DE itself. Keep writing the label; add the key.
- **B5. MED. A6/C4 theme keys in es_settings.xml.**
  - Mainline ES-DE reads `ThemeSet`. A Nano-only theme name written there would leave the ES-DE app on Android with an unknown theme (inf).
  - Use Nano-prefixed keys, or write `ThemeSet` only for themes that exist under `<store>/themes`.
- **B6. MED. L8: re-root whenever the stored volume differs.**
  - This would redirect Android two-volume set-ups (ES-DE on card A, ROMs on internal storage or card B) whenever card A also holds the same relative folder.
  - Today's rule re-roots only when the stored folder is missing (NanoEsdeStore.cpp:645-648). Keep "missing", or require that the stored id is a known alias of the store volume.
- **B7. MED. C1 `%CARDID%` in ROMDirectory.**
  - Old Nano rejects it (`esdeStoragePath`, NanoEsdeStore.cpp:632-637) and falls back to `<parent>/ROMs`, so it survives by accident.
  - The ES-DE app and desktop ES-DE do not understand it. Q2 must decide before anything writes it.
- **B8. LOW. L5's "keep the label, write the key" for system choice.**
  - For labels in both tables (for example PCSX ReARMed for PSX), Linux's choice still becomes Android's system choice through `<alternativeEmulator>`.
  - Android's apply pass adopts it when the core is installed (NanoMenuPS3EmuCatalog.cpp:855-866) (inf).
  - Decide whether a system choice is shared or per platform; C6 currently makes it shared for common labels.
- **B9. LOW. L2: widening `isVolumeId` to 36 characters is shared code.** Android builds with the change will start rewriting stale Cube ids in emulator configs, including the `app/` mirrors (NanoEmuSync.cpp:142-163). This is desirable, but it is an Android behaviour change and needs H.3 and the smoke test.
- **Safe for old builds as proposed:** `systems.linux.json`, `card.json` (bare ids; `repoint` touches only an id followed by `/emulator_config`, NanoEmuSync.cpp:143, 155-157), `<emu>/linux/`, and `key` fields on *other* games' emulators.json items. The ES-DE move merges file by file and keeps unknown files (NanoSdMove.cpp:937-969).

## 5. Test-plan additions

- **T1:**
  - The fixtures must include the **shipped Android writers' behaviour** for names.json pruning and index.json `artOk`.
  - Assert legacy files are byte-identical after a Linux cycle until the floor build.
  - Add a two-version case: Linux to an old Android build to Linux.
- **T2:**
  - Assert that an XMB launch of a libretro system succeeds through the real `coreSoExists` path, not `commitLaunch` (N1).
  - Assert that systems.json adoption does not replace Linux's working tuples (C-5).
  - Assert that an `emulators.json` `am` entry for a package does not make a game unlaunchable on Linux (C-4).
- **T3:** exFAT serial `0000-0000`; leftover `*.nanotmp` and `.tmp.<pid>` files after a kill; RetroArch screenshots beside ROMs not listed as games.

Key files:
- C:/Users/krist/code/thimsy-os-linux/nano/NanoEsdeStore.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuScraper.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuXmb.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoMenuPS3EmuCatalog.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoEmuSync.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoBiosWorker.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/NanoScraper.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/Launcher.cpp
- C:/Users/krist/code/thimsy-os-linux/nano/platform/linux/Volumes.cpp
- C:/Users/krist/code/thimsy-os-linux/linux/src/thimsy-session/src/core/VolumePolicy.cpp
- C:/Users/krist/code/thimsy-os-linux/linux/board/common/rootfs-overlay/etc/init.d/S00base
- C:/Users/krist/code/thimsy-os-linux/nano/test/fixtures/retroarch/retroarch_shipped.cfg