# Thimsy Installer

One app, start to finish, that installs Thimsy OS on a supported handheld:
pick the device and a Light / Medium / Heavy profile, download the device's
official GammaOS firmware and the Thimsy pieces, build the patched firmware on
your computer, unlock the bootloader (first install only), and flash it over
USB. Design: [`../universal_installer.md`](../universal_installer.md).

| Folder | What |
|---|---|
| `engine/` | Rust workspace: `thimsy-engine` (library) and `thimsy-installer` (the CLI the app runs in the background) |
| `app/` | Flutter app (Windows, macOS, Linux; Android shell) |
| `catalog/` | Devices (with their official firmware and where their Thimsy packages are published), profiles, translations (`i18n/`). Embedded in the engine at build time |
| `docs/engine-protocol.md` | The JSON Lines protocol between app and engine |
| `tests/` | End-to-end test on real RG Cube firmware (`e2e.sh`), the published-firmware check (`verify-drafts.sh`) and a fake `spd_dump` |
| `tools/make-thimsy-build.sh` | Turns a firmware from `scripts/build.sh` into the Thimsy package the installer takes (system image + patches + `pack.json`), checking the rebuild is byte-identical |
| `tools/apply_pack.py` | Points a device's default package download at a new file name or release (rarely needed) |
| `tools/spd_dump.patch` | Applied to `spd_dump` on every platform: FDL2's raw data writes and larger reads, progress during reads and writes, and the one-byte first message to FDL1. On the 32-bit Windows build it also opens the device through Unisoc's own driver (`Channel9.dll` the way Unisoc's tools use it, then Windows serial I/O done the same way) or WinUSB, and logs what Windows has for Unisoc USB devices |

## Status (0.1.0 test 1)

- **RG Cube, Heavy profile**: builds on every desktop and matches the Thimsy
  1.0a reference PAC byte for byte; flashes with the bundled `spd_dump` on
  Windows and Linux; macOS untested. On Windows it flashes through
  Unisoc's own USB driver, which unlocking needs too; the app installs it
  (one permission prompt) when it's missing. The Windows build bundles
  Unisoc's driver and `Channel9.dll` from GammaOS's UnisocTools (see
  `UNISOC-NOTICE.txt` next to the tools).
- Light and Medium are defined but their system images aren't published yet.
- Other devices are listed as candidates. Devices that need Windows-only
  vendor tools say so on other computers.
- Android: shows a "later version" notice; no flashing from Android yet.

## Engine

```sh
cd installer/engine
cargo test --workspace                 # unit + CLI tests (synthetic firmware)
cargo build --release
./target/release/thimsy-installer catalog
./target/release/thimsy-installer resolve --device anbernic-rg-cube --profile heavy
./target/release/thimsy-installer space --device anbernic-rg-cube --profile heavy --workspace ~/thimsy-ws
./target/release/thimsy-installer build --device anbernic-rg-cube --profile heavy --workspace ~/thimsy-ws --import ~/Downloads
./target/release/thimsy-installer flash --job ~/thimsy-ws/jobs/anbernic-rg-cube-heavy --dry-run
```

In Docker (the root README's "Building with Docker"), run these from
`docker compose run --rm build bash`; the image has Rust, Flutter and the
libraries `spd_dump` and the Linux app need. The container sees only the
checkout and its `work/` volume, so use
`--workspace /workspaces/thimsy-os/work/thimsy-ws` (kept between runs) and put
the firmware to import in `out/` (`--import /workspaces/thimsy-os/out`).
Flashing over USB and the app's window don't work from the container on any
OS: it has no display, and its `/dev` is a copy from when it started, without
the device that appears in download mode. Build and run those natively.

`installer/tests/e2e.sh WORKSPACE PACK_DIR` (the package and the official
firmware in `PACK_DIR`) runs the real-firmware test: build,
byte-for-byte comparison, dry runs, a full flash through the fake `spd_dump`,
host checks and cleanup.

## Making a Thimsy package

`scripts/build.sh` still builds the full firmware as before. From its output:

```sh
installer/tools/make-thimsy-build.sh --release "Thimsy 1.0a test 31" out/RG_Cube_Thimsy_OS_v1.0a.pac ~/thimsy-build-out
```

This downloads the official GammaOS firmware (or takes `--source PAC`), makes
the package, rebuilds the PAC with the installer from the package alone and
stops unless the result is byte-identical. The package is **one archive**,
`thimsy-build/Thimsy-<release>-<device>-<profile>.tar`: a plain tar of
`pack.json`, the system image (zstd level 19) and the device's `.bsdiff`
patches, so 7-Zip or `tar` can open it. It's split into 1 GB `.tar.001`,
`.002`, ... parts only when it's over GitHub's 2 GiB asset limit (the RG
Cube's heavy package is about 2.2 GB, so it ships as `.tar.001` to
`.tar.003`). The engine reads the system image
straight out of the archive (no unpacking to disk).

`pack.json` is the package's manifest: `format` (1), `device`, `profile`,
`release`, `min_installer`, each file's size and sha256, the build steps, the
sha256 of the official firmware they start from (`source_sha256`) and of the
PAC they must produce (`reference_sha256`). The installer accepts **any**
package whose manifest checks out for the selected device: the catalog pins
no build, so a new Thimsy build needs no new installer (see
`universal_installer.md`, section 10).

Upload the archive to the release, or ship it next to the app (the engine
looks in `<app>/thimsy-build` and `THIMSY_BUILD_DIR`). Users can also pick it
with "Use a file I already downloaded" (`--import <file>`; any file name
works). The catalog's default download (`packages:` in the device file)
points at the latest release; `--apply` (with `--tag TAG` for a given
release) rewrites it, which is only needed when the file name changes.

`thimsy-installer dev bundle --out X.tar FILES...` and `dev bundle-list X.tar`
make and list packages by hand (`pack.json` first).

The build also accepts the unpacked official firmware (for example
`RG_Cube_GammaOS_Next_Full_v1.2.pac`) in an import folder instead of
downloading the 7z.

`installer/tests/verify-drafts.sh WORKDIR SOURCE_PAC TAG...` repeats the
byte-identity check for published firmware releases.

## Making a package by hand

```sh
thimsy-installer dev pack --source official.pac --reference thimsy.pac \
  --out pack/ --device anbernic-rg-cube --profile heavy --system-name thimsy-system-1.0a-heavy
(cd pack && thimsy-installer dev bundle --out ../Thimsy-1.0a-anbernic-rg-cube-heavy.tar \
  pack.json thimsy-system-1.0a-heavy.img.zst rgcube-*.bsdiff)
```

The package is the shared system image (zstd) plus bsdiff deltas for each
other changed partition, each checked by input and output sha256, and the
typed build steps that rebuild the reference exactly. `dev pack` refuses
official firmware other than the catalog's for the device.

## Translations

`catalog/i18n/en.yaml` lists every engine/catalog string
(`thimsy-installer dev i18n-strings --src engine/crates/thimsy-engine/src --out catalog/i18n/en.yaml`);
each `i18n/<code>.yaml` translates it, keyed by the English text like Thimsy's
own `i18n/<code>.json`. A unit test fails when a language is missing a string
or changes a placeholder. The app's own strings are Flutter ARB files in
`app/lib/l10n/`.
