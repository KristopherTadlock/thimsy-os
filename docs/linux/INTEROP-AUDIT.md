# Card interoperability audit, Linux and Android (OA-8, 2026-11-26)

This was a read-only audit: nothing was built or run. The raw reports are in [interop-2026-11-26/](interop-2026-11-26/): three inventories, a proposed contract, and an adversarial check. The check corrects the contract in places, and this summary uses the corrected findings. Items marked "checked" were re-read in the code while writing this summary.

## What already carries over both ways

- Gamelist `<path>` values are relative and matched case-insensitively. Unknown tags are kept.
- Downloaded media is found by name, so box art shows on both.
- `%ROMPATH%` lines in collections, `CollectionSystemsCustom`, and the scraper keys in es_settings.xml.
- `systems.json` scan sources are written as `%CARDID%`.
- Store writes are atomic (temp file, fsync, rename), except names.json and index.json, which skip the fsync.

## Critical and high gaps

1. **Linux never enters store mode by itself.** Store mode needs `persist.gammaos.nano.esde.dir` to be set (checked, `NanoEsdeStore.cpp:128-132`), and nothing on Linux sets it. Until the user runs Setup's card step, the card's ES-DE folder is ignored. Setting the property to the internal path does not help, because the follow-card logic skips "emulated". It must be set to `<card>/ES-DE`.
2. **Every libretro launch from the XMB is refused on Linux** (checked, `NanoMenuXmb.cpp:3345-3351` and `:4034`). `coreSoExists` looks for cores at `/data/data/com.retroarch.aarch64/cores/`, which nothing on Linux creates, so Linux shows "emulator missing". The host tests miss it because they call `commitLaunch` directly.
3. **Cube-style paths on Android cards don't resolve on Linux.** The Thimsy Android build's vold names the card `/storage/00000000-0000-0000-0000-00000000000N`, and Linux uses the exFAT serial. Possibly every Thimsy Android device on that base does this, not just the Cube. Decision D20's alias map was never implemented. The consequences:
   - `names.json` titles are **deleted** on the first save (entries whose path fails `stat` are pruned).
   - `index.json` box and fan-art paths are cleared, in both directions.
   - Absolute collection lines and paths in emulator configs break.
4. **Emulator choices are Android-only and get overwritten.**
   - Labels come only from ES-DE's Android table.
   - A Linux choice with no Android label deletes the gamelist's `<alternativeEmulator>`.
   - `systems.json` launch tuples ping-pong between platforms. Linux adopts Android's tuples wholesale, for example PSX on `mednafen_psx_hw`, which Linux doesn't build.
   - Android per-game standalone choices in `emulators.json` apply on Linux and then fail to launch.
5. **Linux falls back to nothing.**
   - A missing core is a hard error.
   - Standalone emulators are refused, even PPSSPP and ScummVM, which Linux builds.
   - Linux's own default table uses Android cores that Linux doesn't build.
6. **Saves and BIOS don't connect.**
   - Linux RetroArch gets no system, save or state directories. By upstream defaults its saves go to the ext4 data partition, so Android's `.srm` beside the ROMs isn't seen, and Linux saves don't reach the card. This is inferred from RetroArch's defaults.
   - The BIOS worker's path whitelist refuses every Linux target.
   - The RetroArch system folder is read only from Android config paths.
7. **Linux treats Android packages as installed.** `packageInstalled` fails open when `/data/system/packages.list` is missing. Emulator-file moves and app-mirror syncs could act on Linux as if Android apps were present.

## Medium and low gaps

- The theme and downloaded themes don't travel. They live in `persist.gammaos.nano.esde.*` and `/data/system`.
- Game and theme videos fall back to a static image on Linux, because the NDK media stubs are inert.
- A two-slot Linux device prefers the boot card's games drive over an Android card in slot 2.
- Linux refuses to mount an exFAT card with serial `0000-0000`.
- The kernel doesn't pin `EXFAT_DEFAULT_IOCHARSET=utf8`, `VFAT_FS` or the NLS options.
- Play starts made with the card out stay on the device.
- `<lastplayed>` is local time.
- Leftover temp files can be left behind after a pull.

## Backward-compatibility rules (Android builds already in users' hands)

Shipped Android builds never check a `version` field, so any format change must be additive or dual-written.

- **names.json and index.json:** relative keys or relative art paths in the existing files would be pruned or cleared by every shipped build. Use new files beside the old ones, or keep writing the absolute form, until a floor Android release ships.
- **Emulator labels:** keep writing a label wherever one exists, because older builds and the real ES-DE app read only `<altemulator>`. Add a portable key beside it.
- **Theme keys:** write ES-DE's `ThemeSet` only for themes that exist on the card, or use Nano-prefixed keys.
- **ROMDirectory re-root:** keep the "stored folder missing" rule. A wider rule would redirect Android set-ups that use two volumes.

## Proposed direction (to become the card contract)

The contract is now written: [LIBRARY-FORMAT.md](../../LIBRARY-FORMAT.md), with schemas in `linux/schema/` and test vectors in `linux/tests/fixtures/card/`. Its section 9 maps each gap above to where its fix lands.

- **Nothing new is written as an absolute path for the store's own volume.** Paths are relative, `%ROMPATH%`, folder-relative game keys or `%CARDID%`. Readers accept every legacy form. `emulator_config/card.json` lists every id each platform has seen for this card.
- **Each platform keeps its own choices.** Linux uses `nano/systems.linux.json` and its own folders under `emulator_config/<emu>/linux/`. Shared emulator data (RetroArch BIOS and cheats, PPSSPP saves, ScummVM saves) lives in one place. Save states never cross platforms.
  - Superseded for emulator picks by OA-38 (`docs/linux/DECISIONS.md`): the last pick wins on both platforms, in the shared files (`systems.json` launch fields, `<alternativeEmulator>`, `<altemulator>`, `emulators.json`), and a platform that cannot run a pick falls back for that launch only. `systems.linux.json` is retired (W8-EMU-1, LIBRARY-FORMAT.md 4.9). The `emulator_config/<emu>/linux/` folders are unchanged.
- **Emulator choice uses a portable family key** (`core:pcsx_rearmed`, `emu:ppsspp`, `family:melonds`) from a shared table generated from ES-DE's `android` and `linuxarm` tables. Resolution order: this platform's choice, then the shared key, then the device default, then "unavailable on this device". A missing emulator never ends in a launch error.
- **OA-4 fit:** `packageInstalled`/`coreSoExists`, the BIOS path whitelist, the Android path normalisation and the store default move behind the platform layer, in a new `Emulators` interface and an extended `Volumes`.

## Open questions for the owner

These are in [interop-2026-11-26/contract.md](interop-2026-11-26/contract.md), section 5:

- Q1: Will the Android build's vold name volumes by serial?
- Q2: The ROMDirectory form, and whether the card must stay usable by the real ES-DE app.
- Q3: Does the theme travel?
- Q4: Who owns systems.json?
- Q5: Save states and different-core saves.
- Q6: A play journal on the card.
- Q7: Which card wins on a two-slot device.
- Q8: Standalone settings: shared or per platform?
- Q9: Systems with no Linux emulator.
- Q10: What "playlists" means.
- Q11: Are videos and manuals required on Linux?
- Q12: The Android compatibility floor.
