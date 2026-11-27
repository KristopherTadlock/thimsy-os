emulator_config and emulator choice: Android against Linux (task "emu")

Scope: read-only. Repo C:/Users/krist/code/thimsy-os-linux. "(inf)" marks inference, not checked on a device or a build.

## 0. Headline findings

1. **Almost none of the Linux side of the emulator_config contract exists yet.** These are not implemented anywhere in nano/, linux/src or linux/board (grep finds nothing):
   - `nano/systems.linux.json`
   - `emulator_config/card.json`
   - `<emu>/linux/` and `retroarch/linux/states/`
   - the per-launch `--appendconfig`
   - the `linuxarm` label table
   - the Cube-id prefix map (D20)
   - the alias map (`mupen64plus_next_gles3` to `mupen64plus_next`)

   The design for all of these is in linux_handheld_plan.md §6.2–§7.10 (lines 597–958).

2. **The Linux launcher only runs RetroArch cores.**
   - Its argv is `retroarch -L /usr/lib/libretro/<name>_libretro.so --config /storage/.config/retroarch/retroarch.cfg <rom>` (nano/platform/linux/Launcher.cpp:398-408; defaults at Launcher.h:78-81).
   - It passes no `system_directory`, `savestate_directory` or append config.
   - Every package (standalone) launch except RetroArch fails with "no Linux emulator for package" (Launcher.cpp:414-415). That includes PPSSPP and ScummVM, which the image does build.
   - It does not fall back to the device's default when a core is missing. The launch just fails with "core ...: No such file" (Launcher.cpp:196-197). Plan §6.4 (line 703) says it should fall back.

3. **Nano thinks every Android emulator is installed on Linux.** `NanoMenu::packageInstalled` reads `/data/system/packages.list` and returns true when that file can't be read (NanoMenuXmb.cpp:3353-3366). On Linux the file doesn't exist. So the move, sync and BIOS code (NanoSdMove.cpp:1102 onward, NanoBiosWorker.cpp:595) would treat every Android package as present (inf). Nothing on Linux gates emuMove or emuSettingsSync (grep of platform/linux, linux/src and the docs finds no reference).

4. **BIOS installs on Linux would be refused (inf, from code).**
   - The worker accepts a target folder only if its realpath is under `/data/media/0`, `/mnt/media_rw/<vol>` or `/data/data/<pkg>` (NanoBiosWorker.cpp:558-575; `rawView`/`canonical` in NanoBiosUtil.cpp:65-80).
   - Linux makes those paths links: `/data/media/0` → `/storage/emulated/0` (S00base:103), and `/mnt/media_rw` → `/run/thimsy/media_rw`, whose entries point to `/storage/<id>` (10-android-links.sh:75; test_e2e.py:436).
   - realpath() therefore returns `/storage/...`, every check fails, and every slot reports "not one files are installed to".
   - Separately, the RetroArch slot reads `system_directory` from `retroarchConfigFile()`, which looks only at Android paths (NanoRaConfig.cpp:463-466, NanoRaConfig.h:31-36). It then falls back to `/storage/emulated/0/RetroArch/system` (NanoBiosWorker.cpp:54, 519), which is internal storage, not the card.

5. **The key derived from an emulator choice is Android-shaped.**
   - `esdeEmulatorKey` makes `core:<x>` only from a `_libretro_android.so` name; a Linux `<x>_libretro.so` gives no key (NanoEsdeSystemName.cpp:149-158).
   - The only label table compiled in is ES-DE's `android` one (NanoEsdeEmulators.inc:1-4); grep finds no `linuxarm` anywhere.
   - Per-game choices that have no ES-DE label are stored as Android `am` strings in `nano/emulators.json` as `{game, label, am}` (NanoEsdeStore.cpp:1520-1572).

6. **What works by accident today:** a system whose `nano/systems.json` names a RetroArch core that Linux also builds. `coreName()` strips `_libretro_android.so` (Launcher.cpp:385-396), so the Android core path maps to the Linux .so.

## 1. What each emulator keeps in emulator_config, and how portable it is

Sources for the Android side: the move is in NanoSdMove.cpp:1076-1365, the re-pointing in NanoSdMove.cpp:2248-2386, the app/ mirrors in NanoEmuSync.cpp:33-56, and the BIOS slots in NanoBiosCatalog.cpp:877-916. Sources for Linux: apps-v1.fragment, the linux/package recipes, and plan §6.3 (lines 668-680) and §7.4 (lines 869-877).

### RetroArch (com.retroarch.aarch64), `emulator_config/retroarch/`

**What is there:** the whole RetroArch folder moves (NanoMenuPS3Folder.cpp:1018-1036), with these directory keys pointed at the card (NanoRaConfig.cpp:49-63):
- `retroarch.cfg`
- `system/` (BIOS)
- `saves/`, `states/`
- `screenshots/`, `recordings/`, `records_config/`
- `config/` (core overrides, `.opt` files, `remaps/`)
- `cheats/`, `playlists/` (`.lpl`, including `builtin/content_*.lpl`)
- `downloads/`, `logs/`, `temp/`

Kept off the card:
- `thumbnails/` stays internal when moved from the default location (NanoRaConfig.cpp:75, 88).
- shaders, overlays, filters and autoconfig stay in `/data/user/0/com.retroarch.aarch64` (NanoRaConfig.cpp:39, 64-68).
- `app/autoconfig` is a mirror of the controller profiles (NanoEmuSync.cpp:35).

The shipped config has `savefiles_in_content_dir = "true"` and `savestates_in_content_dir = "true"` (thimsy-os-nano work/.../fixtures/retroarch/retroarch_shipped.cfg:41-48). So real saves and states sit beside the ROMs, and the card's `saves/` and `states/` folders are mostly unused.

**Linux counterpart:** the same RetroArch, 1.22.2 (linux/package/retroarch/retroarch.mk:7-12), built for KMS, EGL, GLES3, ALSA and udev (retroarch.mk:38-60).

**Portable:**
- BIOS in `system/` (same file names; cores take them from `system_directory`).
- `cheats/`.
- `.srm` saves beside the ROM when both sides run the same core.

**Not portable:**
- `retroarch.cfg`: it holds `libretro_directory` and `libretro_info_path` under `/data/user/0/...`, `/data/user/0` overlay, shader and autoconfig paths (fixture lines 27-34, 54), and Android drivers (input `android`, audio `opensl`/`aaudio`, video `gl`/`vulkan`) (inf for the driver values).
- `config/`: per-device `.opt` files, and overrides that name `/data/user/0/...overlays` and `.slangp` presets (plan line 610).
- `playlists/*.lpl`: they name Android core paths (inf).

The plan keeps Linux's config and `config/` internal and points only `system_directory` and `cheat_database_path` at the card (plan lines 601-613). States are per platform, Linux's going to `retroarch/linux/states/<system>/` (plan line 613). None of this is wired in Launcher.cpp yet.

### PPSSPP (org.ppsspp.ppsspp), `emulator_config/ppsspp/`

**What is there:** `PSP/` (SAVEDATA, PPSSPP_STATE, SYSTEM/ppsspp.ini, TEXTURES, Cheats, flash0/font), `PAUTH/` and `EXDATA/` (NanoSdMove.cpp:1147-1166). Android points PPSSPP there with `memstick_dir.txt` in its private data (NanoSdMove.cpp:2283-2293). `app/shared_prefs` is mirrored (NanoEmuSync.cpp:36). The BIOS font slot is `PSP/flash0/font` (NanoBiosCatalog.cpp:887, 907).

**Linux counterpart:** the same PPSSPP, v1.20.4, SDL with GLES and no Vulkan (linux/package/ppsspp/ppsspp.mk:11-38), installed at `/usr/share/ppsspp` and linked from `/usr/bin/ppsspp`. It is not launchable yet (Launcher.cpp:414).

**Portable:**
- `PSP/SAVEDATA`: same format.
- `PSP/Cheats`, `TEXTURES`.
- `PPSSPP_STATE`: same version, so possibly portable (inf). The plan still keeps it per platform in `ppsspp/linux/` (plan line 872).

**Not portable:**
- `PSP/SYSTEM/ppsspp.ini` and `controls.ini`: graphics backend, Android key codes against SDL key codes (inf). The plan keeps them internal and links only individual folders (plan line 872).

### Flycast (com.flycast.emulator), `emulator_config/flycast/`

**What is there:** `emu.cfg`, `data/` (dc_boot.bin, dc_flash.bin, VMUs), `mappings/` and `textures/` (NanoSdMove.cpp:1169-1186). Android points it there with the `home_directory` preference (NanoSdMove.cpp:2295-2306), and paths inside `emu.cfg` are rewritten (NanoSdMove.cpp:2212-2228). The BIOS slot is `<home>/data` (NanoBiosCatalog.cpp:883, 903).

**Linux counterpart:**
- Only the upstream flycast libretro core (libretro-flycast.mk:15-17; version still TODO), in the 2 GB Medium image only (DECISIONS D12, line 94).
- There is no standalone Flycast package.
- The core wants its BIOS in `system/dc/` and names VMUs differently. Saves are per platform in v1 (plan line 638).

**Portable:** the BIOS files, copied into RetroArch's `system/dc/` (inf). `emu.cfg` and `mappings/` are Android-only.

### DraStic app (com.dsemu.drastic), `emulator_config/drastic/`

**What is there:** the whole system folder: BIOS in `system/`, config, saves (`backup/*.dsv`), states and cheats (NanoSdMove.cpp:1188-1198). Android points it there with the `_SystemPath` preference (NanoSdMove.cpp:2309-2317). `app/shared_prefs` is mirrored (NanoEmuSync.cpp:53).

**Linux counterpart:** none shipped. DraStic's Linux build is closed (plan line 673), and DrasticRunner is firmware-only on Linux (DECISIONS G4-4 line 721; linux-ux-3 line 1047).

**Portable:**
- The BIOS (bios7, bios9, firmware) into RetroArch's `system/` for the melonDS DS core (inf).
- `.dsv` saves: a raw save plus a DeSmuME footer (inf). They need a rename or trim rule to become melonDS DS `.srm` files. Plan: "sorted", no rule yet (line 637).

### Built-in DS player (drastic-nano), `emulator_config/drastic-nano/` (+ cheats)

Moved from `persist.gammaos.drastic.data_dir` (default `/storage/emulated/0/drastic-nano`) (NanoSdMove.cpp:1123-1145, 2270-2282). Its BIOS stays in `/data/system/drastic-nano/DraStic/system` (NanoBiosCatalog.cpp:885, 905).

Not on Linux: the DS player is cut from v1 (DECISIONS D10, line 72). Same notes as DraStic.

### M64Plus FZ (org.mupen64plusae.v3.fzurita), `emulator_config/m64plus/`

**What is there:** `GameData/<header name> <country> <MD5>/` with SRAM and states (NanoSdMove.cpp:1200-1211). Pointed by the `gameDataStoragePath` preference (2318-2328). `app/shared_prefs` and `app/files/Profiles` are mirrored (NanoEmuSync.cpp:37-38). Its BIOS (64DD IPL) is in `Android/data/.../files` and does not move (NanoBiosCatalog.cpp:886, 906).

**Linux counterpart:** the mupen64plus_next core (GLES3). FZ writes separate `.eep`/`.mpk`/`.sra`/`.fla` files; the core writes one `.srm`. Not portable without a converter (plan lines 636, 875).

### ARMSX2 (com.armsx2), `emulator_config/armsx2/`

**What is there:** its data folder without `bios`, `cache`, `resources` or `logs`: inis (`PCSX2-Android.ini`, `inis/PCSX2.ini`), memcards and sstates (NanoSdMove.cpp:1213-1229). Pointed by the `systemDir` preference, with the inis rewritten (2232-2245, 2329-2339). The BIOS stays internal (NanoBiosCatalog.cpp:881, 901).

**Linux counterpart:** none on RK3566/H700. An ARMSX2 Linux AppImage exists but is not viable there (plan line 676). PCSX2-format memcards would likely be portable to a future Linux build (inf).

### ScummVM (org.scummvm.scummvm), `emulator_config/scummvm/saves/`

**What is there:** the saves (NanoSdMove.cpp:1231-1245). Android sets `savepath` in `scummvm.ini` (2340-2348). `app/shared_prefs` and `app/files/scummvm.ini` are mirrored (NanoEmuSync.cpp:41-42). The ini holds `/saf/` and content:// paths (NanoEmuSync.cpp:178-197).

**Linux counterpart:** the same ScummVM, 2026.3.0, SDL back end (linux/package/scummvm/scummvm.mk:7-38). Not launchable yet.

**Portable:** the saves, if both sides derive the same game target ids (plan line 873, marked guess). The ini is Android-only.

### Vita3K (org.vita3k.emulator), `emulator_config/vita3k/`

**What is there:** the whole pref-path: memory card, firmware, installed games (NanoSdMove.cpp:1247-1262; `config.yml` `pref-path`, 2349-2357). `app/external/config.yml` and `app/shared_prefs` are mirrored (NanoEmuSync.cpp:43-44). The firmware is installed by Vita3K itself (NanoBiosCatalog.cpp:891, 911).

**Linux counterpart:** none. It needs Vulkan (plan line 678). The ux0 layout matches desktop Vita3K (inf). `pref-path` can be `/mnt/media_rw/<id>` (plan line 754).

### Azahar (org.azahar_emu.azahar), `emulator_config/azahar/`

**What is there:** the whole folder without `cache`, `log` or `shaders`: config, sdmc, nand, sysdata, states (NanoSdMove.cpp:1264-1277). Pointed by a SAF tree grant plus `CITRA_DIRECTORY` (2358-2370). `app/shared_prefs` is mirrored (NanoEmuSync.cpp:45). The BIOS/keys go to the Azahar folder (NanoBiosCatalog.cpp:889, 909).

**Linux counterpart:** none. There is no official aarch64 Linux build (plan line 675). sdmc and nand data likely match desktop Azahar (inf); the config is Android-only.

### Dolphin (org.dolphinemu.dolphinemu), `emulator_config/dolphin/`

**What is there:** Config, GameSettings, StateSaves, GC, Wii, Load, ResourcePacks, ScreenShots, GBA. Each folder is replaced by a symlink on /data (NanoSdMove.cpp:1342-1349; plan lines 848-855). `app/shared_prefs` is mirrored (NanoEmuSync.cpp:39). The GC BIOS (IPL) slot moves to `dolphin/GC` (NanoBiosCatalog.cpp:882, 902).

**Linux counterpart:** none on RK3566/H700 (plan lines 674, 877).
- GC (memory cards, IPL) and Wii (NAND saves) use the same layout as desktop Dolphin (inf).
- `Config/*.ini` holds Android backend and controller device names (inf).

### hakuX (com.rfandango.haku_x), `emulator_config/hakux/native`

**What is there:** the hard disk, states and caches, via a link (NanoSdMove.cpp:1350-1365). `app/shared_prefs` and `app/files/controllers.json` are mirrored (NanoEmuSync.cpp:51-52). The BIOS stays in `files/console` (NanoBiosCatalog.cpp:890, 910).

**Linux counterpart:** none. xemu would be a different emulator (plan line 677).

### SeedlessDS (com.seedlessds.app), `emulator_config/seedlessds/`

**What is there:** `backup/` and `savestates/` (NanoSdMove.cpp:1279-1305), via a SAF grant plus `_SaveFolder` (2371-2383). Mirrored: `app/shared_prefs` and `app/files/SeedlessDS/user` (NanoEmuSync.cpp:48-50). The BIOS stays internal (NanoBiosCatalog.cpp:892).

**Linux counterpart:** none (Android only).

### WatermelonDS (me.magnum.melondualds)

**What is there:** nothing moves; saves stay beside the ROM as `.sav` (android_launcher_plan.md:87). `app/shared_prefs` and `app/files/controller_config.json` are mirrored (NanoEmuSync.cpp:46-47).

**Linux counterpart:** the melonDS DS core (libretro-melondsds; version TODO). The `.sav` and `.srm` bytes likely match, so only a rename is needed (plan line 637, guess).

### MAME4droid 2024

It has a BIOS slot (`Android/data/com.seleuco.mame4d2024/files/roms`; NanoBiosCatalog.cpp:893) but no emulator_config folder. No Linux counterpart; on Linux, MAME sets go through the mamearcade or mame2003_plus core.

### All `app/` mirrors

Android-only by design (plan lines 779, 888). Linux must not read or write them, except that the id-repair pass skips a first-level `app` (plan line 864).

## 2. Mapping Android emulator choices to Linux

**Where a choice is stored:**
- **System choice:** `nano/systems.json`, as `launchType`/`coreSo`/`package`/`intentTemplate` (NanoMenuXmb.cpp:702-706, 755-759). `coreSo` is an Android path such as `/data/data/com.retroarch.aarch64/cores/<x>_libretro_android.so`.
- **ES-DE's system label:** `<alternativeEmulator>` (seeded from mods/esde-emulators.txt).
- **Per-game choice:** `<altemulator>` (a label), or `nano/emulators.json` `{game, label, am}` (NanoEsdeStore.cpp:1520-1572).

| System | Android default (mods/nano-systems.txt, esde-emulators.txt) | Linux equivalent (apps-v1.fragment) | Status |
|---|---|---|---|
| NES, FDS, SNES, GB/GBC, GBA, Genesis/SMS/GG/SCD, 32X, Saturn, VB, NGP, PCE/CD, WS, Lynx, 2600, 5200, 7800, Coleco, MSX, 3DO, PICO-8, arcade, fbneo, neogeo | RetroArch cores `mesen`, `snes9x`, `sameboy`, `mgba`, `genesis_plus_gx`, `picodrive`, `yabasanshiro`, `mednafen_*`, `stella`, `a5200`, `prosystem`, `gearcoleco`, `bluemsx`, `opera`, `fake08`, `mamearcade`, `fbneo` (nano-systems.txt:20-66) | same names built (fragment) | launches today through coreName(); `.srm` shared |
| PSX | Beetle PSX HW (`mednafen_psx_hw`) (nano-systems.txt:30; esde-emulators.txt:12) | `pcsx_rearmed` only; mednafen_psx_hw is not built | launch fails (missing .so, no fallback); plan: sorted until the Phase 3 round trip (line 620) |
| N64 | M64Plus FZ app (nano-systems.txt:25; esde-emulators.txt:25) | `mupen64plus_next` core | needs the family/alias map; `mupen64plus_next_gles3` (Android Small) has no .so on Linux; no alias column in mods/retroarch-cores.txt (grep) |
| NDS | WatermelonDS (nano-systems.txt:26; esde-emulators.txt:27); drastic-nano when `persist.gammaos.nano.drastic_nano` is set (NanoMenuXmb.cpp:3358-3364); DraStic and SeedlessDS also possible | melonDS DS core (Android doesn't ship it: DECISIONS G2d-9, line 484) | **no Linux counterpart for DraStic, drastic-nano or SeedlessDS**; all map to the melonDS DS family; saves sorted |
| PSP | PPSSPP (Standalone) (nano-systems.txt:31) | PPSSPP 1.20.4 standalone | built but Launcher refuses packages (Launcher.cpp:414) |
| Dreamcast | Flycast (Standalone) 2.7 (nano-systems.txt:32) | flycast core (Medium only) | different emulator form; VMUs per platform |
| ScummVM | ScummVM (Standalone) (nano-systems.txt:63) | ScummVM 2026.3.0 standalone | built, not launchable yet |
| GC, Wii | Dolphin (Standalone) (nano-systems.txt:36-37) | none on RK3566/H700 | unavailable; keep the stored choice |
| 3DS | Azahar (Standalone) (nano-systems.txt:38) | none | unavailable |
| Xbox | hakuX (nano-systems.txt:39) | none (xemu later?) | unavailable |
| PS2 | ARMSX2 (nano-systems.txt:40) | none | unavailable |
| PS Vita | Vita3K (not a Nano system) | none (needs Vulkan) | unavailable |
| Android games | org.thimsy.androidgames (nano-systems.txt:69) | none | Android only |

**Label problems for a future real ES-DE on Linux** (plan table, lines 731-737):
- In the `android` table but not `linuxarm`: SameBoy, M64Plus FZ, WatermelonDS, hakuX and Fake-08.
- In the `linuxarm` table but not `android`: Mupen64Plus (Standalone).
- Android already writes such labels into cards (plan line 955), so Linux Nano must read them through a family table, which does not exist yet.

## 3. BIOS path expectations

**Android, before a move** (NanoBiosCatalog.cpp:877-896):

| Slot | Folder |
|---|---|
| RetroArch | `system_directory`, default `/storage/emulated/0/RetroArch/system` |
| ARMSX2 | `Android/data/com.armsx2/files/bios` |
| Dolphin | `Android/data/org.dolphinemu.dolphinemu/files/GC` |
| Flycast | `<home>/data` |
| DraStic | `<system>/system` |
| drastic-nano | `/data/system/drastic-nano/DraStic/system` |
| M64Plus FZ | `Android/data/org.mupen64plusae.v3.fzurita/files` |
| PPSSPP | `<memstick>/PSP/flash0/font` |
| ScummVM | `extrapath` |
| Azahar | `CITRA_DIRECTORY` |
| hakuX | `files/console` |
| Vita3K | pref-path (installed by Vita3K itself) |
| SeedlessDS | `files/SeedlessDS/system` |
| MAME4droid | `Android/data/com.seleuco.mame4d2024/files/roms` |

**Android, after a move** (NanoBiosCatalog.cpp:898-916):
- On the card: `emulator_config/retroarch/system`, `dolphin/GC`, `flycast/data`, `drastic/system`, `ppsspp/PSP/flash0/font`, `azahar`, `vita3k`.
- Unchanged (internal): ARMSX2, drastic-nano, M64Plus FZ, ScummVM, hakuX, SeedlessDS, MAME4droid.
- The drop folder is `<card>/bios/`, offered first in setup (BIOS.md:9; plan line 785).
- Installed files are recorded in `/data/system/nano_bios/installed.json` (BIOS.md:33).

**Linux, as designed:**
- RetroArch's `system_directory` points at the drive's `emulator_config/retroarch/system`, through the append config (plan lines 605, 611).
- BIOS files are shared data (plan line 887).
- Per-emulator folders under `/storage/.config/<emu>` or `/storage/.local/share/<emu>` (plan line 815).

**Linux, as implemented:**
- No system_directory is set; RetroArch uses whatever `/storage/.config/retroarch/retroarch.cfg` says. S00base:99 only creates the folder, and no seeded config was found.
- The BIOS worker's checks refuse every target (§0 item 4) (inf).
- The extra BIOS folders that cores replacing Android standalones need are not mapped: `system/dc/` for flycast, and the DS BIOS for melonDS DS (inf).

## 4. What OA-8 still needs from the emulator side

1. Linux Nano must not use Android-only emulator data:
   - Gate the move, the sync, the app/ mirrors and BIOS installs on Linux.
   - Replace `packageInstalled`'s fail-open with the image's emulator list.
2. Launcher:
   - A per-launch append config that sets `system_directory` and `cheat_database_path` on the card, states under `retroarch/linux/states/<system>`, and `sort_savefiles_enable` when cores differ.
   - Package launches for PPSSPP and ScummVM.
   - Fall back to the device's default when a core or emulator is missing.
3. A family and alias table plus the `linuxarm` labels. Map `pkg:` values (FZ, WatermelonDS, DraStic, SeedlessDS, Flycast, PPSSPP, ScummVM) and `core:` aliases to Linux players. Teach `esdeEmulatorKey` the `_libretro.so` suffix.
4. A Linux BIOS base resolver: Linux paths, `/storage/<id>` accepted, RetroArch's system folder on the card.
5. A rule for which folders link to the card on Linux: PPSSPP `PSP/SAVEDATA`, `TEXTURES` and `Cheats`; ScummVM saves; and possibly Flycast `data`.
6. Write `systems.linux.json` and `card.json`, and keep `<emu>/linux/` free of absolute drive paths (plan line 864).
7. The DS and N64 save converters or rename rules are open (plan line 662). DraStic and drastic-nano have no Linux counterpart.
