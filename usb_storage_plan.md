# USB storage plan: a USB drive works like the microSD card

Status: draft for review (2026-11-21), revised with the user's storage rules the same day. Not started. The network storage plan (`network_storage_plan.md`) builds on this one: a network share becomes one more drive in the same model.

## Requirement (user, 2026-11-21)

- A USB drive attached to the device works essentially the same as a microSD card.
- Emulator settings can live on it, and so can ROMs and ES-DE settings.
- Apps and emulators can use it by default.

Refined by the user the same day:
- **Setup offers to move the emulator settings and ES-DE files to the microSD card,** which is the default storage device.
- **Another card or USB drive alongside it** can be chosen in the menus for **ES-DE, for emulator settings, or for both**, separately.
- **A storage device can be added at any time.** The user changes the setting, and ES-DE files and emulator settings are then read from that drive.
- **Removing the drive a setting uses falls back to internal storage** for that setting. Removing the microSD card with no other drive in reverts to internal storage.

## Where things stand

### Platform: already fine
- **The vendor fstab manages USB drives:** `voldmanaged=usbdisk:auto` on both the USB 2 (musb) and USB 3 (dwc3/xhci) controllers. That is `fstab.ums9620_2h10` on the 1.2 vendor partition, checked 2026-11-21.
- **phh's vold makes every managed disk adoptable** (`system/vold/main.cpp:280`). `StorageManagerService.java:1696` then makes it visible for write, so a USB drive is a FUSE `/storage/<uuid>` volume like the card.
- **App access:** apps get storage permissions, and the pack-in emulators have All files access (`NanoPermGrantBridge.java`).
  - Thimsy's storage denylist (`ThimsyStorageDenylist.java`) applies to every volume, USB included. It blocks other apps' `Android/data|obb|media`, Nano Backup, and owned files in DCIM and the other media folders.
  - `emulator_config`, `ES-DE` and `ROMs` are not blocked.
  - The test 35/36 known issue "USB drives are outside the storage privacy rules" doesn't match the code. All-files apps are exempt on every volume, which is what that note describes. Reword the note.
- **ROM scanning** already covers every `/storage/*` and `/mnt/media_rw/*` volume (`NanoMenuXmb.cpp:1815-1855`).
- **The path helpers** treat any volume the same (`NanoLaunch`, `NanoRaConfig`, `NanoBiosUtil`, `NanoAppFiles`, `NanoScraper`, the button-map readers).

### Nano: microSD-only in a few places
- **`sdIsSlot()` (`NanoSdMove.cpp:397`)** is the only place that tells microSD from USB. It looks for `/mmc` in the block device's sysfs path.
- **`sdCardRoot()` (:407)** prefers the slot's card but falls back to a USB drive. So Move ES-DE, Move Emulator Files, the BIOS default folder and the setup steps already target USB when there is no card.
- **`emuSdSlotCard()` (:2006)** excludes USB from emulator-settings adoption and sync:
  - `emuSdTick` ("a USB drive is never adopted");
  - `emuSettingsSyncLaunch`;
  - `emuMoveAuto`;
  - `sdMoveFinish`;
  - the foreign-card dialog;
  - setup.
- **The result today is a mixed state.** Move Emulator Files with only a USB drive points the emulators at it and sets `emu_sd=1`, but never adopts or syncs it. A card inserted later gets adopted on top.
- **`esdeStoreFollowCard()` (`NanoEsdeStore.cpp:157`)** permanently rewrites `persist.gammaos.nano.esde.dir` to the first removable volume, sorted by name, that has the same folder, once the saved one has been gone 10 s. With two drives that both hold ES-DE, pulling one silently switches to the other.
- **`emusync::repoint()`** rewrites every emulator setting that names an absent volume's `emulator_config` to the adopted card. With two drives, pulling one would retarget everything.
- **Wording:** about 60 strings in code and 46 keys in each of the 21 i18n files say "microSD" or "SD card". The settings rows are dispatched by matching their English label (`NanoMenuPS3Menu.cpp:6639`).

## Design

### 1. One drive model
Replace `sdCardRoot` / `sdIsSlot` / `emuSdSlotCard` with a drive list (`NanoDrives.h/.cpp`). Each drive has:
- the volume id (`/storage/<id>`, the vold fsUuid), its kind (microSD, USB, and later network), a label and free space;
- whether it holds `ES-DE/` and/or `emulator_config/`.

The label comes from `/dev/block/vold/public:M,m` (FAT/exFAT volume label), read once per mount. The kind comes from the sysfs block path (`/mmc` or `/usb`). The list refreshes on the existing one-second storage tick, plus the mount watcher.

### 2. Two drive settings, with internal storage as the fallback

> **Superseded in part (2026-11-27, `docs/linux/DECISIONS.md` OA-49, OA-52, OA-53).** Gamelists, collections and media live on the drive only, with no device mirror (OA-49), so option A's internal copy of `gamelist.xml`, collections and ES-DE settings is dropped: with the drive out, ES-DE uses the internal store's own files, never a copy of the drive's. Saves, save states, BIOS and emulated system storage are shared storage with no mirror either (OA-52, OA-53), so the fallback covers emulator settings files only, and an emulator whose shared storage is on the removed drive does not start until the drive is back (`per_device_config_plan.md` §2.7 and its question 20). Lines below that this changes are marked *(superseded)*.
Two settings under **Settings > Storage**:
- **ES-DE Drive** (`persist.gammaos.nano.storage.esde_drive`)
- **Emulator Settings Drive** (`persist.gammaos.nano.storage.emu_drive`)

Each holds a volume id, or `internal`. Each row lists Internal Storage and every drive present, as "microSD card" or "USB drive "<label>"", with free space and whether that drive already holds `ES-DE/` or `emulator_config/`.

- **Default:** setup's "keep ES-DE / emulator files on the microSD card" sets both to the card (today's setup move, unchanged). Without that, both are `internal`.
- **Choosing a drive at any time:** the setting changes, and ES-DE files or emulator settings are read from that drive's `ES-DE/` or `emulator_config/` from then on. No files are copied.
  - If the drive has none yet, Nano asks: **Copy from <current location>** (the move rules: copy without overwriting, the drive wins, the leftovers prompt) or **Start Empty**.
  - Re-pointing goes through `fileJobRelocate` with nothing to copy, which rewrites the emulators' paths, the SAF grants and the symlinks.
- **The drive is removed:** that setting falls back to internal storage within the existing 10-s grace.
  - ES-DE reads `/sdcard/ES-DE` again *(superseded, OA-49: the internal store's own files, not a mirror of the drive's)*.
  - The emulators are re-pointed to their internal settings (see section 3) *(superseded, OA-52, OA-53: settings only; an emulator with shared storage on the drive waits for it)*.
  - A banner says "<drive> was removed: ES-DE / emulator settings are on internal storage now".
  - The saved choice stays: when the same drive (same volume id) comes back, both settings return to it, with a banner.
  - A different drive inserted meanwhile is never adopted automatically. It shows up in the two rows for the user to choose.
- **Other drives are never picked on their own.** Only the user's choice or internal storage is used, so pulling one of two drives can't silently switch to the other.

### What the internal fallback contains (decision needed)
While a drive holds the settings, internal storage has either nothing (the move deleted the internal copies) or an old copy. Two options:
- **A. Mirror (recommended):** Nano keeps an internal copy in step with the drive.
  - ES-DE: `gamelist.xml`, collections and settings, not the media *(superseded, OA-49: no mirror of library data; this line is dropped)*.
  - Emulators: the settings files `NanoEmuSync` already tracks, not saves or large data.
  - It is updated after each sync, so a fallback picks up where the drive left off.
  - Changes made on internal storage while the drive was out are synced back when it returns, with the drive winning clashes as in `emusync::decide`.
- **B. Plain fallback:** internal storage has whatever was last there, possibly nothing. Nothing is synced back when the drive returns.

### 3. Emulator settings on any drive
- **Adoption and sync** use the Emulator Settings Drive, whatever its kind. `emuSdSlotCard()` becomes `emuSettingsDrive()`, with the same callers.
- **The sync base** (`/data/system/nano_emu_sync.txt`) is already keyed by volume id, so each drive keeps its own history.
- **`repoint()`** only ever retargets to the chosen drive or to internal storage, never to "any present drive". On removal it re-points the emulators that named the drive's `emulator_config` to their internal locations *(superseded, OA-52, OA-53: their settings only; shared storage is never re-pointed to an internal copy)*.
- **The leftover mixed state** (emulators pointed at USB, `emu_sd=1`, nothing adopted) becomes Emulator Settings Drive = that USB drive the first time this build runs.
- **`persist.gammaos.nano.emu_sd` and `emu_sd_card`** migrate into `emu_drive`.

### 4. ES-DE on any drive
- **`esdeStoreFollowCard()`** is replaced by the ES-DE Drive setting.
  - `esde.dir` is derived: `<drive>/ES-DE`, or `/sdcard/ES-DE` while the drive is out.
  - Nothing is rewritten permanently on a removal.
- **The ROM folder and the scraper's media folder** follow `esde.dir` as today.
- **USB drives get pulled more often than cards:** fsync the directory after the ES-DE store's atomic renames (`NanoEsdeStore.cpp:69`). The emulator sync writes already fsync.

### 5. Moves and setup
- **"Move ES-DE to microSD" and "Move Emulator Files to microSD"** become **Move ES-DE to Drive...** and **Move Emulator Files to Drive...**. They pick a drive, keep the move rules, and set the matching drive setting when done.
- **First-boot setup** is unchanged: it offers the microSD card, with its automatic, prompt-free move. With no card but a USB drive, it offers the USB drive.
- **"Keep Emulator Files on microSD"** is replaced by the Emulator Settings Drive row.

### 6. Names on screen
- Drives show as **microSD card** or **USB drive "<label>"**, with the id in small text when two drives share a label. This applies to the folder picker (`NanoMenuPS3Folder.cpp:470`), `sdShown`, the BIOS paths (`NanoBiosWorker.cpp:198`) and drastic-nano's overlay (`OverlayMenu.cpp:1839`).
- Generic wording ("storage drive", "the Settings Drive") replaces "microSD" where either kind applies. The i18n files are re-keyed with the existing generator scripts.

### 7. What stays the same
- Nano's ROM scan, the ES-DE ROM folder and the emulators' ROM access, which already work from USB.
- The move semantics (copy without overwriting, the drive wins, the leftovers prompt).
- The storage denylist rules, applied to USB as to the card.

## Risks
- **Duplicate volume ids:** two FAT drives cloned from one image share a serial and so an id. vold's behaviour with both inserted is unknown. Detect two mounted volumes with one id and refuse to choose between them, with a message: neither drive is used. This is the card contract on both platforms: `linux_handheld_plan.md` §7.3 adopts the same rule (decision 17), and it goes into `LIBRARY-FORMAT.md` (`android_launcher_plan.md` §3).
- **Drives that come and go** (USB hubs, docks that power-cycle the port) must not trigger the drive-out banner flapping. Keep the existing 10-s grace before treating a drive as gone.
- **Hard-coded ids in about 12 emulator configs, the SAF grants and the Dolphin/hakuX symlinks:** every change of drive goes through `fileJobRelocate`, never a property flip.

## Questions for the user
1. The internal fallback: A (mirror, recommended) or B (plain)? See section 2. *(Narrowed by OA-49, OA-52, OA-53: the choice now covers emulator settings files only.)*
2. When the drive that was removed comes back, should the settings return to it automatically (as designed), or stay on internal storage until the user chooses it again?

## Tests
- **Unit:** the two settings and their fallback and return; `repoint()` only to the chosen drive or internal storage; the mirror sync (if A); the label and kind parser (from sysfs fixtures).
- **On hardware:**
  - USB only, a card only, both;
  - pull and reinsert each, during a game and at the home;
  - two drives with ES-DE, each setting on a different drive;
  - a cloned pair;
  - USB 2 and USB 3 ports, and a USB-C hub with the monitor.
