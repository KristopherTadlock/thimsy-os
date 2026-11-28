> **Superseded where it differs (2026-11-27).** This is a raw report, a snapshot of 2026-11-26. `docs/linux/DECISIONS.md` OA-46 to OA-54 replaced its model: settings and picks are per device, saves, states and BIOS are shared on the card apart per core, and there is no `<emu>/linux/`, `retroarch/linux/` or `systems.linux.json`. The current contract is `LIBRARY-FORMAT.md`.

## Card layout and mount paths: Android and Linux (OA-8 audit, paths slice)

Repo: C:/Users/krist/code/thimsy-os-linux (branch linux). I only read files. "Plan" means linux_handheld_plan.md. Anything marked (inference) is my reading and has not been checked on a device.

### 1. Verdict
- **Stock-style Android (vold names the volume by its serial):** the absolute path is the same on both sides. Both use `/storage/<exFAT serial, XXXX-XXXX upper case>`, so `/storage/<id>/...` paths stay valid in both directions.
- **The RG Cube (the Thimsy Android build's own vold):** the path is NOT the same. The Cube names every public volume by mount order, `/storage/00000000-0000-0000-0000-00000000000N` (plan:800-801). Linux never builds that path, and nothing on Linux maps it to the card yet. Decision D20's prefix map is decided (DECISIONS.md:144-148) but not implemented: grep finds no code for it, `card.json` or `systems.linux.json`.
- So today, every absolute path a Cube wrote on a card does not resolve on Linux, except where Nano already re-roots by relative path (section 4).

### 2. How each side names the card

| | Android (firmware Nano) | Linux |
|---|---|---|
| Card mount | vold makes `/storage/<id>` and `/mnt/media_rw/<id>` only while the card is mounted. `FirmwareVolumes` scans those folders (platform/firmware/Volumes.cpp:63-93). | `/storage` is the ext4 data partition (S00base:51-53, 72-85). thimsy-session mounts each drive at `/storage/<id>` (VolumePolicy.cpp:151; Session.cpp:680-694). |
| Id | Stock: the FAT/exFAT serial from blkid. Cube: the mount-order index (plan:799-803). | The serial, read from the exFAT boot sector offset 100 and printed `%04X-%04X` (Blkid.cpp:72-85). All-zero serials are refused (VolumePolicy.cpp:105-115). ext4: lower-case UUID. |
| Raw alias | `/mnt/media_rw/<id>`, a real mount | Root link `/mnt/media_rw` -> `/run/thimsy/media_rw` (10-android-links.sh:75). The session links `<id>` -> `/storage/<id>` after mounting (Session.cpp:688-690) and removes it on unmount or pull (Session.cpp:699). |
| Who is in | The folder exists | `/run/thimsy/volumes.json` (VolumePolicy.cpp:408-433). Nano believes it only when `state=="mounted"` and `st_dev` differs from `/storage` (nano/platform/linux/Volumes.cpp:141-147, 226-228). |
| Mount point when the card is out | Gone | A permanent empty directory on ext4 with the immutable flag (Session.cpp:684; Mounter.cpp:26-41; session.md:58) |
| Internal storage | `/storage/emulated/0`, `/sdcard`, `/data/media/0` | `/storage/emulated/0` is a folder on the 256 MiB data partition (S00base:100). `/sdcard`, `/data/media/0` and `/storage/self/primary` are links to it (10-android-links.sh:70-71; S00base:103-104). |

- The e2e fixture uses `/storage/1234-ABCD` with `/mnt/media_rw/<drive>` linked to it (tests/e2e/nano_jail.py:13, 94, 150-151; test_e2e.py:426-437).
- Both fixture drives are plain host directories. No test runs on a real exFAT or FAT mount, so none covers case folding, forbidden characters or refused symlinks (inference from the fixture code).

### 3. Mount and filesystem semantics
**Mount options.**
- Linux exFAT: `dirsync,noatime,nodev,nosuid,noexec` (VolumePolicy.cpp:117-127).
- Linux vfat: `utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush`.
- These match vold's flags (plan:828-829). vold also passes `uid/gid/fmask/dmask`; Linux passes none, so files are root-owned with the session's umask. Nano runs as root (NanoJail.h:4-6), so this works.
- No `quiet` option: a `chown` to non-root, or a `chmod` with extra bits, on the card returns EPERM on Linux (inference from upstream exFAT driver behaviour). On-disk permission bits do not exist on exFAT or FAT beyond the read-only attribute.

**Checks before mounting.**
- Linux runs `fsck.exfat -p` on every exFAT mount, including cards Android uses (Session.cpp:647; AUDIT.md:17, "a repair can change files Android wrote").
- vold checks before every mount too.

**Case.**
- exFAT and vfat ignore case and keep it on both sides. Nano already folds ASCII case for keys (NanoEsdeStore.cpp:194-199) and finds `ROMs` without regard to case (NanoMenuXmb.cpp:1433, 1817).
- The Linux internal store (`/storage/emulated/0`) is ext4 without casefold, so it is case-sensitive (20-data.sh:36). That matters only for an internal store or an export, not the card.

**Symlinks.**
- exFAT and FAT cannot hold them (plan:753).
- Android's Dolphin and hakuX links live on `/data/media/0/Android/data/...` and point into `/storage/<id>/emulator_config/...`; the card itself holds plain folders (NanoSdMove.cpp:1940-1970; plan:846-865).
- Linux keeps links on ext4 by the same rule (plan:859-865). No Dolphin is planned on RK3566 or H700 (plan:877).
- `treeCopyRec` copies symlinks as links (NanoFileJob.cpp:211-218), so moving a folder that contains links onto the card fails on both platforms.
- Linux has none of these links yet.

**Timestamps.**
- exFAT stores a UTC offset with each time. Linux never sets the kernel time zone (only the localtime link: S00base:105-106; D40time), so it writes offset 0, which is still correct (inference).
- FAT32 has no offset. Android writes local time and Linux reads it as UTC, so file mtimes shift by the zone offset (inference). Nano mtimes are only change detectors (NanoEsdeStore.cpp:191-196), and the sync uses content hashes (plan:898).
- Gamelist `lastplayed` and `playcount` are XML text, so they are not affected.

**Character sets.**
- vfat passes `utf8` explicitly.
- exFAT relies on the kernel default iocharset (utf8 by default upstream).
- `kernel-critical.conf` pins only `EXFAT_FS`, `EXT4_FS` and `EFI_PARTITION` (linux/tools/thimsy-import/kernel-critical.conf). It does not pin `VFAT_FS`, `NLS_UTF8`, `NLS_CODEPAGE_437` or `EXFAT_DEFAULT_IOCHARSET`, although FAT32 is "accepted" by D8 (gap; inference that ROCKNIX's configs have them).
- FAT and exFAT forbid `:*?"<>|`; collection names are cleaned of these (NanoEsdeStore.cpp:679-684). Media are named after ROM files, which already exist on the card.

### 4. Android-written absolute paths: how Linux handles them
- **`es_settings.xml` `ROMDirectory`:** Linux re-roots a missing volume to the store's own volume (NanoEsdeStore.cpp:637-650). This works for Cube ids and for `/storage/emulated/0/ROMs`.
- **ES-DE store property pointing at an absent id:**
  - `looksLikeVolumeId` accepts any dashed 36-character UUID, the Cube form included (Volumes.cpp:281-295).
  - So Linux sees a Cube id as absent, and `esdeStoreFollowCard` moves to `/storage/<serial>/ES-DE` after 30 s uptime plus 10 s (NanoEsdeStore.cpp:157-185).
  - That property is device state, though: on a fresh Linux device the store defaults to `/storage/emulated/0/ES-DE` (NanoEsdeStore.cpp:58-59, 120-126).
  - I found no Linux code that defaults the store to the games drive, although I44 and plan:793 say it should.
- **`nano/names.json`, `index.json`:** absolute `/storage/<id>` paths. A Cube id never matches on Linux, and a Linux serial path is pruned on the Cube (plan:912).
- **`emulator_config` id repair:**
  - `emusync::isVolumeId` accepts only 9-character or 32-hex ids (NanoEmuSync.cpp:131-140).
  - `repoint` stops scanning after 32 characters (NanoEmuSync.cpp:150-153).
  - So Cube paths in emulator configs are never rewritten, on either platform.
  - The RetroArch cfg, Vita3K `pref-path` and similar keep Cube paths that are dead on Linux.
- **Foreign-id writes:**
  - The immutable flag covers only adopted mount points.
  - A `mkdir -p` of `/storage/00000000-...0001/...` (Nano's `sdMkdirs` or `makeDirs`, or an emulator) would quietly create folders on the ext4 data partition (inference; plan:820-824 covers only the `<VOLID>` case).
- **Other path forms Linux cannot use:** `content://` tree URIs and ScummVM `/saf/` paths in the `app/` mirrors (plan:754). `/storage/emulated/0/...` resolves on Linux, but to different contents.

### 5. What Linux puts on the card that Android does not expect
- **Today:**
  - The GPT entry 5 and the exFAT labelled THIMSY (D60firstboot:259-307).
  - fsck repairs.
  - ES-DE files written by the same shared code.
  - RetroArch saves and states: none of `savefile_directory`, `savestate_directory` or `system_directory` is set anywhere in linux/ (grep). With the main config at `/storage/.config/retroarch/retroarch.cfg` (Launcher.h:81), Linux RetroArch could write `.srm`/`.state` beside the ROMs in `ROMs/<system>/`, where Android's states also live (plan:774). That would clash with Android's states for different cores (inference: depends on RetroArch's Linux defaults).
- **Planned, not built:** `<emu>/linux/`, `retroarch/linux/states/`, `nano/systems.linux.json`, `emulator_config/card.json` (plan:773-791; TODO.md:312).
- **On the Linux side itself:** `/storage/<id>` folders pile up on ext4 (immutable, one per card ever seen). The XMB scan (`readdir("/storage")`, NanoMenuXmb.cpp:1426-1437, 1835-1846) treats `/storage/thimsy` (S00base:99) and every empty mount point as volumes. They are scanned but stay empty, so this is harmless (inference).

### 6. Android-only things Linux must tolerate
- `emulator_config/<emu>/app/` mirrors and Android's per-platform files under `<emu>/` (Flycast `emu.cfg`, ARMSX2 inis, PPSSPP `PSP/SYSTEM`, `PPSSPP_STATE`; plan:776-779).
- `<altemulator>` labels that exist only in the Android table (plan:955).
- Android launch tuples in `systems.json` (plan:910).
- vold and app folders on public volumes (`Android/data|media|obb`, `LOST.DIR`), plus `System Volume Information` from Windows (inference about which folders appear). Nano has no special handling (grep), but scans are per system folder.
- On FAT32: a dirty boot-sector bit that Android's `fsck_msdos` never clears, so Linux checks once (plan:831).
- On the Cube: exFAT volumes flagged clean but torn. Linux always checks, so this is covered (plan:832, 932).

### 7. Android mounting a Linux card
- **Types:** vold makes public volumes only for basic data and Linux filesystem data types (plan:844). The Linux card's entries are:
  - 1 and 2: Linux reserved
  - 3: arm64 root (B921B045)
  - 4: Linux home (933AC7E1)
  - 5: `games`, basic data (rk3566 genimage.cfg; IMPLEMENTATION.md:795-799)
- So vold should mount only `games`. Entry 5 is within the minor limit of 8 (entry 7 or lower, plan:844).
- **Id:**
  - Stock Android: the exFAT serial, the same as Linux.
  - Cube: `...0001` only if it is the first volume mounted. A USB drive mounted first, or OA-2's planned FAT README entry (DECISIONS.md:1090-1091, not yet in genimage.cfg), would take a lower index and change the games id (inference; plan:801-802 says extra volumes on the card shift it).
  - Phase 0 device test still open (plan:844).
- **Before Linux's first boot:** no volume, so Android posts "Unsupported SD card" and its format flow (IMPLEMENTATION.md:807); OA-2 is decided but not built.
- **One-slot devices:** the boot card's `games` partition is the only way to interoperate. Two-card setups are the supported interop path until the device test passes (plan:844).
- **Two-slot Linux devices:** `cardRoot()` prefers the boot card's games drive over an Android card in the second slot (Volumes.cpp:176-186; volumes_test.cpp:256-266). An Android card in slot 2 is therefore not the default card on Linux (inference about its effect on adoption).

### 8. Work this implies (gaps)
1. **Implement D20:**
   - Read Cube prefixes from the card's configs (and later `card.json`) and map them to the adopted drive.
   - Extend `isVolumeId` and `repoint` to the 36-character form.
   - Make foreign-id paths under `/storage` fail instead of creating folders on ext4. Making `/storage` itself non-writable for unknown names, or a guard in Nano's makeDirs, are options.
   - Alternatively, fix the Cube's vold to use the serial (D20 option (c)).
2. **Store mode on the games drive by default on Linux** (I44, plan:793); no setter exists today.
3. **Set RetroArch's save, state and system directories** (append config, plan:875) before any launch from a shared card.
4. **Pin the filesystem kernel options:** `CONFIG_VFAT_FS`, `NLS_UTF8`, `NLS_CODEPAGE_437`, `EXFAT_DEFAULT_IOCHARSET="utf8"` in kernel-critical.conf. Consider exFAT `quiet`, or document EPERM.
5. **Tests on real card filesystems:** a loop-mounted exFAT and FAT fixture (case pairs, forbidden characters, symlink refusal, mtimes and the UTC offset). Also a Cube-id round-trip fixture for names.json, index.json, collections, es_settings and emulator_config.
6. **Before OA-2 ships:** check its effect on the Cube's mount-order id, and decide the default card when two SD drives are present.
