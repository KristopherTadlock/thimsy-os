# Linux handheld plan: Thimsy OS on devices that run Linux firmware

Date: 2026-11-24
Status: plan only, nothing implemented, nothing run on a device. Research done 2026-11-24: the ROCKNIX, Batocera, Knulli, AmberELEC, dArkOS, muOS, MinUI, NextUI, spruce, Onion, FlipBase and BaseOS repositories and wikis; ES-DE, RetroArch and emulator sources; Raspberry Pi Imager and bb-imager-rs sources; and an audit of this repository and of `thimsy-os-installer`. Reviewed 2026-11-24 by five adversarial reviewers, each checked by a second pass: 60 findings, none refuted, all folded in (section 17).
Builds on: `multi_device_plan.md` (device descriptors, installer backends, package format 2), `android_launcher_plan.md` (the `NanoPlatform.h` seam and `LIBRARY-FORMAT.md`), `device_profiles_plan.md` (layers, profiles, the resolver), `user_emulators_plan.md` (emulator definitions), `usb_storage_plan.md` (the drive model), and `universal_installer.md` in the installer repo. This plan does not edit them. Where it needs something from them, or proposes a change to them, it says so.

Claims come from the sources in section 16. Anything not confirmed by a source is marked *(guess)*. Patch references give the function name and `P:<line>` in `patches/thimsy/0001-thimsy.patch` as of this date; the lines move.

Request (user, 2026-11-24, condensed): support handhelds that can't run Android but can run Linux custom firmware, "for example the Anbernic RG ARC-D and similar devices"; port all of Thimsy's UI and features where possible (the Nano home, profile export through ES-DE, symlinked emulator configs); make profiles interchangeable between Android and Linux devices on one SD card; have ThimsyInstaller build and write bootable images; adopt the fast-boot work of "Paper Plane"; and refactor the 1.4.4 build so Android and Linux share as much code as feasible.

---

## 0. Short answers

| Question | Answer |
|---|---|
| Base | A **pinned, patch-based downstream of ROCKNIX** (`next` branch): a `distributions/THIMSY` folder, new packages, and a patch series. Not a free-running fork, not our own Buildroot or Yocto (section 3). |
| First device | The **Anbernic RG ARC pair** on ROCKNIX's RK3566 "Generic" image. **The ARC-D boots it only after Android is erased from its eMMC**; the ARC-S (no eMMC) needs nothing but a card. The same image then reaches about 10 more boards (section 12). |
| Second family | Allwinner **H700** (RG35XX Plus/H/SP, RG40XX, RG CubeXX): the largest group with no Android at all. |
| UI | The same Nano C++ source, with a third implementation of the `NanoPlatform.h` seam that `android_launcher_plan.md` introduces. About two thirds of the 204k lines port once the seam exists; about 35k lines are Android-bound and are replaced or dropped (section 5). |
| Card interop | The contract is what the Android build already writes: `ES-DE/`, `ROMs/`, `emulator_config/`. The Linux build mounts the card at the **same path Android uses** (`/storage/<volume id>`). No symlink ever lives on the card. v1 shares the library, BIOS files and in-game saves where both builds run the same core; settings and save states stay per platform (section 7). |
| Installer | A `disk_image` path on the SD-card backend `multi_device_plan.md` already plans: a verified image streamed to the card with bmap, read back, then the boot FAT edited in place (section 8). |
| "Paper Plane" | **Not found** under that name. The references used are FlipBase (RK3566, mainline, MIT: frontend started 1.3 s after reset, menu visible at about 3 to 3.5 s), BaseOS and RetroStoneOS. The owner is asked for a link (section 9). |
| Effort | About **31 to 51 person-weeks** to a first public release *(guess)*, with a plain-image alpha after roughly the first third (section 12). |
| Biggest unknowns | The Quick Menu over a running emulator (no SurfaceFlinger on Linux), and silencing the pad while a menu is open on InputPlumber devices (section 5.3). |

Three corrections to the brief:

1. **The RG ARC-D can run Android.** It ships Android 11 on eMMC and boots its stock Linux from a card; GammaOS (Android 12, archived) and GammaOS Core (Android 13 TV, frozen since February 2025) support it. What it lacks is an **Android 14 base**, which is what Thimsy needs; `multi_device_plan.md` defers every RK3566 device to tier 5 for that reason. The truly Linux-only devices are the RG ARC-S (1 GB, no eMMC), the RG353VS/PS, and the whole H700 family.
2. **ROCKNIX and Android can't share an ARC-D.** ROCKNIX's install page says: "You will need to wipe the Android partition to boot into ROCKNIX" (`dd if=/dev/zero of=/dev/block/mmcblk0` over adb). Batocera says the same from v42: the eMMC bootloader "is not compatible with later Linux kernels". The same holds for the RG353P/M/V. Units with the "legacy bootloader" are not supported at all.
3. **"Profile" has seven meanings in this code base** (installer profiles, hotkey profiles, RetroArch controller profiles, M64Plus profiles, shader profiles, the device catalog, and the planned Small/Medium/Large/Ultra). None is a portable user profile. In this plan **"the card profile"** means the `ES-DE/`, `ROMs/` and `emulator_config/` trees on a drive. **"Package profile"** means the installer's choice: `light`/`medium`/`heavy` in today's catalog, Small/Medium/Large/Ultra once `device_profiles_plan.md` lands.

---

## 1. Goal and scope

In scope:

- A Linux build of Thimsy OS for handhelds with **no Android 14 base**, starting with the mainline-kernel families ROCKNIX supports.
- Nano as the home and frontend of that build, with the features in section 5.4.
- One card profile that works in an Android Thimsy device and a Linux Thimsy device.
- ThimsyInstaller writing a bootable card on Windows, macOS and Linux.
- Boot time as a tracked number from the first build.
- One Nano source, one set of data files and generators, two firmware builds (three with the Thimsy Home app).

Out of scope:

- Devices whose only kernel is a closed vendor BSP with a framebuffer-only or PowerVR GPU driver (A133P, Knulli-style H700, Miyoo Mini, A30). Section 2 marks them `skip`.
- Unisoc T820/T618 and Dimensity 8300 devices. No Linux exists for them. They stay on the Android build.
- GammaOS Pico devices (`multi_device_plan.md` §3).
- Selling or bundling. The bundle contains non-commercial cores, and the branding of every base considered forbids it.

What does not change:

- The RG Cube's Android output stays byte-identical through the refactor (the rule `multi_device_plan.md` sets).
- Thimsy never redistributes a handheld maker's firmware. The Linux image contains none. It does contain redistributable binaries: the Rockchip DDR init from `rkbin` (TF-A is built from source), Wi-Fi and Bluetooth firmware (`linux-firmware` and ROCKNIX's `extra-firmware`, which is marked proprietary), and `libmali` unless it is patched out (section 3.3).

---

## 2. Device matrix

One row per image-sharing group. CFW short names: RX ROCKNIX, BT Batocera, KN Knulli, AE AmberELEC, dArk dArkOS (ArkOS is archived), uOS UnofficialOS, mu muOS, Min MinUI (archived), Nx NextUI, spr spruce, On Onion. GON is GammaOS Next (Android 14), GOC is GammaOS Core (Android 13 TV).

Priority: **P0** first target; **P1** same image or next family; **P2** later; **P3** only if asked; **skip** not viable or better served by the Android build.

### 2.1 Rockchip RK3566 / RK3568 (Mali-G52)

| Device | CFWs | Kernel | GPU driver | Display / input quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|
| **Anbernic RG ARC-D** (2 GB, 32 GB eMMC) | RX (RK3566 "Generic" image), BT (RGxx3), uOS (BSP image). KN: backer-only alpha. Not AE, dArk, mu | RX/BT mainline (RX 7.3.5); uOS vendor BSP | RX builds both: **libmali (GLES 3.2) is its default**, Panfrost (GLES 3.1) is chosen with its `gpudriver` setting | 4" 640x480 DSI panel, natively portrait (`rotation = <90>`), RX patches its timings; touchscreen with an axis-swap patch; Saturn-style 6 face buttons, **no analog sticks**. RX presents the pad through InputPlumber | RX: OS card plus an optional second card for games. **Only after Android is erased from eMMC.** "Legacy bootloader" eMMC units unsupported | Stock Android 11 (with its own Linux card); GammaOS (A12), GOC. No Android 14 | **P0** |
| **RG ARC-S** (1 GB, no eMMC) | RX, BT. KN: device tree only | as ARC-D (KN: BSP 6.1) | as ARC-D (KN: blob) | same panel, no touch | SD only; nothing to erase | Stock is Linux. GOC lists it | **P0** (same image; 1 GB floor; the zero-risk unit) |
| RG353P / M / V / VS / PS | RX, BT, AE, dArk (M, V), uOS | RX 7.3.5; AE/dArk BSP 5.10 | as ARC-D; AE/dArk libmali | 3.5" 640x480, two panel revisions (both handled); touch on P/V; analog sticks through an ADC mux | SD; P/M/V also eMMC, **with the same Android wipe** | P/M/V: Android 11, GammaOS A12, GOC. VS/PS: Linux only | **P1** |
| RG503 | RX, BT, AE, dArk | as RG353 | as RG353 | 4.95" OLED initialised over SPI (960x544 *(guess)*) | SD | GOC lists it | **P1** |
| Powkiddy RGB30, RGB10 Max 3, RGB20SX, RGB20 Pro, RK2023 | RX, BT, dArk (some), spr (some, base image) | RX 7.3.5 | as ARC-D | RGB30 is 720x720 (1:1), like the Cube | SD | GOC lists most | **P1** |
| Powkiddy X55, X35S, X35H | RX ("Specific" image), BT, uOS (X55) | 7.3.5 | as ARC-D | X35H needs the `extlinux.conf` FDT line edited by hand | SD | GOC | **P2** (second image, manual FDT) |
| Miyoo Flip | KN (BSP 6.1, needs GammaLoader), spr, Min, FlipBase (mainline). Not RX | vendor BSP; FlipBase mainline 7.3.5 | Mali blob; FlipBase Panfrost or libmali | 3.5" 640x480 | Internal storage; no SD boot without a preloader change | GOC | **P2** |
| RG DS / DS Plus (RK3568) | RX, BT, KN alpha | 7.3.5 | as ARC-D | dual screen | SD; Android on eMMC | **GON, a GammaOS Nano target** | **P3** (Android Thimsy covers it) |
| Miniloong Pocket 1, GKD Bubble, GameMT E5/E6 Plus, CB408 | dArk, KN, spr (some) | mixed | mixed | - | SD or eMMC | GOC | **P3** |

### 2.2 Allwinner H700 (Mali-G31, 1 GB LPDDR4): no Android at all

| Device | CFWs | Kernel | GPU driver | Display / input quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|
| **RG35XX Plus / H / SP / 2024 / Pro, RG40XX H / V** | RX (**two images: DDR4 and DDR3, with different SPLs**), BT, KN (main target), mu (main platform), Nx, spr (base image), Min, GarlicOS 2, BaseOS | RX mainline 7.2, BT 7.2.8; KN vendor BSP 4.9.170; mu, Nx, BaseOS vendor BSP (version *(guess)* for mu and Nx) | RX/BT Panfrost, no Vulkan; KN `mali-g31-fbdev` blob, **framebuffer only** | 640x480. **Panel revision can't be detected**: the user copies a DTB to `dtb.img`, and a wrong one shows garbage. RX uses its `rocknix-joypad` kernel driver here, not InputPlumber. RG40XX: RGB LEDs, inverted right stick | SD only (two slots) | none | **P1** |
| RG CubeXX | RX, BT, KN, mu, Nx, spr | as above | as above | 720x720 (1:1) | SD | none | **P1** |
| RG34XX / 34XX SP, RG SP, RG28XX | RX, BT, KN, mu | as above | as above | 34XX is 3:2 *(guess)*; RG28XX panel is natively portrait (`rotation = <270>`) | SD | none | **P2** |

### 2.3 Other families

| Device | SoC / GPU | CFWs | Kernel | GPU driver | Quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|---|
| RG351P/M/V, ODROID-GO Advance/Super, RGB10, GameForce Chi, XU10, G350, GKD Pixel 2 (RG351MP: AE, dArk; not RX) | RK3326 / Mali-G31 | RX (two images, list frozen: "do not add any new RK3326 images"), BT, AE, dArk, KN, mu, spr | RX/BT mainline 7.1; others BSP 4.4 | Panfrost or libmali | many panel variants | SD | none | **P2** (weak SoC) |
| R36S and clones | RK3326 | RX, BT, KN, arkos4clone | as above | as above | many board variants; the wrong build blinks a red LED | SD | none | **P3** |
| RG552 | RK3399 / Mali-T860 | RX, AE, uOS | RX mainline 6.18 | Panfrost *(guess)* | - | SD | Android on eMMC *(guess)* | **P3** |
| ODROID-GO Ultra, RGB10 Max 3 Pro | S922X / Mali-G52 | RX, BT (OGU), uOS | mainline 7.3 | Panfrost | U-Boot at sector 1 | SD | none | **P3** |
| GameForce Ace, CM5 handhelds | RK3588S / Mali-G610 | RX, uOS | **vendor BSP 6.1** | libmali, Panfrost or Panthor | portrait panel | SD | *(guess)* | **P3** |
| Retroid Pocket 5, Mini, Flip 2, AYN Thor Lite | SM8250 / Adreno 650 | RX, BT, KN (no build) | mainline 7.2 | Freedreno + Turnip | portrait panels; InputPlumber | SD **after OpenABL is flashed from Android** | stock Android; GON only "planned" | **P2** |
| AYN Odin 2 family, RP6, AYANEO Pocket | SM8550 and newer | RX, BT | mainline 7.x | Freedreno + Turnip | - | SD or UFS through OpenABL | GON or stock Android | **P3** |
| RG Vita Pro | RK3576 | RX, BT, KN, mu | mainline 7.3.5 | libmali or Panfrost | SPI pad MCU | SD | **GON** | **P3** |
| TrimUI Brick, Smart Pro, MagicX Zero 28/40 | A133P / PowerVR GE8300 | KN, mu, Nx, spr, Min, CrossMix | closed vendor BSP | PowerVR blob | - | SD | **GON; the Brick is GammaOS Nano's main target** | **skip** |
| TrimUI Smart Pro S | A523 / Mali-G57 | KN, Nx, spr | vendor BSP 5.15 | blob | - | SD | unknown | **P3** |
| Miyoo A30 | A33 / Mali-400, 512 MB | spr, Min | stock BSP | Mali-400 blob (GLES 2) | - | SD | none | **skip** |
| Miyoo Mini family | SSD202D, 128 MB | On, Allium, Koriki, Min, spr | stock BSP | no 3D GPU *(guess)* | - | internal + SD overlay | none | **skip** |
| RG Cube, RG556, RG406H/V, RG476H, RG Slide | Unisoc T820 | **none** (bootloader locked, no mainline) | - | - | - | - | **GON: today's Thimsy** | **skip** |
| RG405M/V, RG505; RG557, RG477M/V | T618; Dimensity 8300 | none | - | - | - | - | GON | **skip** |
| x86 handhelds | AMD64 | RX (tree only), BT, mu (qemu) | mainline | Mesa | - | USB or internal | n/a | **P3 as hardware; useful as a QEMU test target** |

### 2.4 What the matrix says

- **ROCKNIX and Batocera are the only CFWs on mainline kernels with open GPU drivers** across RK3566, H700, RK3326 and Qualcomm. Knulli, muOS, NextUI, AmberELEC and dArkOS run vendor kernels with Mali blobs. A Nano written against Mesa runs wherever ROCKNIX does, and nowhere on the vendor-kernel H700 firmwares.
- **ROCKNIX ships one image per SoC family, sometimes split in two** (RK3566 Generic and Specific; H700 DDR4 and DDR3; RK3326 a and b). The RK3566 Generic image's U-Boot detects the board and picks one of 12 DTBs. So "support the ARC" is nearly "support twelve boards", minus per-device screen and pad testing.
- **Two input models.** RK3566 and Qualcomm devices go through InputPlumber, which presents one virtual pad; H700, RK3326, RK3399 and S922X use the `rocknix-joypad` kernel driver. Nano has to handle both (section 5.3).
- **H700 needs the user's help once**: panel revision and DDR type can't be detected. The installer has to ask (section 8.4).
- **Overlay-style firmwares (MinUI, NextUI, spruce, Onion) are not a base.** They run on the stock kernel, have no GPU path Nano can use on most devices, and their licences block reuse (MinUI has none; NextUI is PolyForm Noncommercial since 2026-08-15; spruce is CC BY-NC 4.0).
- **Every Linux CFW uses its own card layout** (`Roms/Name (TAG)`, `ROMS/<any>`, `/userdata/roms`, `EASYROMS`). Thimsy keeps its own (ES-DE names) and does not try to read theirs in the first release. Nano's folder-alias table (`esdeCanonSystemName`, P:20449) already maps MinUI-style names.

---

## 3. Base choice and licensing

### 3.1 Options

| Option | Device support | Cost to Thimsy | Verdict |
|---|---|---|---|
| **(a) Pinned downstream of ROCKNIX** (LibreELEC-style build): `distributions/THIMSY`, new packages, a patch series, rebased on a schedule | 16 SoC families, mainline kernel and U-Boot, per-device quirk scripts. 127 authors and about 6,800 commits in the last 12 months, of which roughly half are merges of LibreELEC upstream work *(share is a guess)* | A patch queue to rebase (section 3.2 lists about a dozen patch points); 200 GB and about ten hours for a first build; GPL-2.0 for the distro layer | **Recommended** |
| (b) Free-running fork of ROCKNIX | same on day one | Merge conflicts against a tree that takes about 130 commits a week (many of them outside handheld files). A lone fork has no upstream to fall back on: JELOS, ROCKNIX's own predecessor, was ended by its maintainers in 2024 | No |
| (c) Knulli or Batocera fork (Buildroot) | Batocera: mainline, wide. Knulli: vendor kernels, blobs, binary bootloaders with no source for some devices | Three stacked layers for Knulli; Batocera is 74% one committer. Batocera's own Docker-on-Windows note says "not stable enough" (2022) | No. Batocera is the fallback if ROCKNIX becomes unusable |
| (d) Our own Buildroot | none: every panel, pad, rumble, sleep and audio quirk is ours | Smallest image and fastest boot (FlipBase is this, for one board); best licence tooling (`make legal-info`) | No for "every device". See section 9.5 |
| (e) Yocto | none; no maintained handheld BSP layer *(guess)* | 140 GB, 32 GB RAM, 16 cores recommended; steepest learning curve | No |
| (f) Thin image on vendor BSP kernels (muOS, BaseOS style) | H700, A133 | We would ship GPL kernels whose source we can't supply; framebuffer-only GPU on H700 | No |

A pure "layer with no patches" is not possible on ROCKNIX: its image recipe hard-codes the frontend, and a device folder cannot shadow a project package. So (a) is "pinned commit plus patch queue", the way this repository already treats GammaOS.

### 3.2 What the downstream consists of

A new repository, `thimsy-os-linux`, GPL-2.0:

- `rocknix.ref`: the pinned upstream commit. Start from a **stable release tag** (the latest is `20261101`), not a nightly.
- `distributions/THIMSY/`: name, version, update URL, logos. `DISTRO` is an existing switch (`config/options`: `DISTRO="${DISTRO:-ROCKNIX}"`).
- `packages/thimsy/`: `thimsy-nano`, `thimsy-session` (section 5.2), `thimsy-fonts`, `thimsy-data` (systems, cores, hotkey and emulator definitions generated from the shared lists), and optionally `thimsy-esde` (section 6.1).
- `patches/`: a quilt-style series against the ROCKNIX tree.
- `build.sh`: clone the pinned commit, apply patches and overlays, run `make docker-RK3566`.

Patch points, read from ROCKNIX `next` on 2026-11-24:

| What | Where in ROCKNIX | Change |
|---|---|---|
| Frontend package list | `projects/ROCKNIX/packages/virtual/image/package.mk:18`: `PKG_UI="emulationstation es-themes textviewer lowerdeck"` | Nano instead of EmulationStation |
| Frontend start | The bash script `/usr/bin/autostart` runs quirks, `rocknix-automount` and start scripts in series, then `systemctl start ${UI_SERVICE}`. The quirk `090-ui_service` sets `UI_SERVICE="sway.service essway.service"`; `111-sway-init` writes Sway's config and matches windows by `app_id="emulationstation"` | A `thimsy-nano.service` in place of `essway.service`; the autostart script split so display set-up runs first (section 9) |
| Card mounting | `automount`: mounts a second card at `/storage/games-external` with no mount options, then creates every system folder on it (`systemd-tmpfiles`) | **Replaced**, not patched (section 7.3) |
| Launching | `runemu.sh` hard-codes the RetroArch config path, uses `systemd-run` and `swaymsg`, and calls `setsettings.sh`, which rewrites the main config and several per-core option files on every launch. `start_<emu>.sh` scripts do the same for standalone emulators (`start_ppsspp.sh` rewrites `ppsspp.ini`) | Patched so nothing writes into a shared card folder; ES-DE system names mapped to ROCKNIX platform names (section 6.4) |
| System keys | `input_sense` (`input.service`): volume, power key, lid, brightness and Wi-Fi combos, the global quit hotkey | Replaced by `thimsy-session` |
| GPU driver | `devices/RK3566/options`: `GRAPHIC_DRIVERS="mali panfrost"`, `ADDITIONAL_DRIVERS="mali-bifrost"`, `libmali` in both userlands; the `gpudriver` script defaults to libmali | Panfrost only, if decision 11 says so |
| Emulator set | the `emulators` meta-package (where DraStic and BigPEmu come from) | Cut to the package profile's list |
| Updates, telemetry | `update.rocknix.org`; `rocknix-report-stats` | Pointed at Thimsy's releases; reporting off |
| Branding | `distributions/ROCKNIX/logos`, names | Replaced |

Rebase rule: move the pin only to ROCKNIX stable tags, at most once a month, and only with the conformance tests of section 11 passing. Generic fixes go upstream. The README says "based on ROCKNIX, not affiliated".

### 3.3 Licences

| Piece | Licence | Consequence |
|---|---|---|
| ROCKNIX's own software and scripts | GPL-2.0 (`LICENSE.md`; per-file SPDX headers; some quirk scripts Apache-2.0) | `thimsy-os-linux` is GPL-2.0. Publish it. |
| ROCKNIX branding and images | CC BY-NC-SA 4.0, for branding and images only. Settled from `LICENSE.md`: the README's bare NonCommercial/ShareAlike list is this clause with its heading dropped. The FAQ agrees: "ROCKNIX only prohibits commercial use through our branding" | Don't reuse or remix. Thimsy's own logo is already BY-NC. |
| The bundle | "This distribution includes components licensed for non-commercial use only" (`LICENSE.md`) | Same stance as the Android build: never sold, never bundled with hardware. |
| Kernel, U-Boot, BusyBox and other GPL packages | GPL | Each release publishes the matching source tarball beside the image (ROCKNIX's `make src-pkg`). |
| Nano | Apache-2.0 (GammaOS Next's licence; Thimsy's files carry Apache headers) | A separate program in the image ("mere aggregation"), fetched by a recipe. **Never paste Nano code into GPL-2.0 files or the reverse**: Apache-2.0 is not compatible with GPLv2-only. |
| Nano's GPL parts | `NanoAc3.h` and `NanoOskKanji.h` carry GPL headers; `liba52` is GPL | An existing question for the Android build too. The Linux build can drop AC-3 (media players are cut from v1). The Kanji table needs an answer in `plan_remove_unlicensed_files.md`'s audit. |
| Mali blob (`libmali`, `mali-bifrost` kernel driver) | Arm EULA: redistributable with conditions, revocable | **ROCKNIX's default on RK3566.** Shipping Panfrost only removes the blob and the EULA, but means the emulator set is re-qualified on the non-default driver, and ROCKNIX's Panfrost mode enables a Vulkan driver it calls broken. Decision 11. |
| Rockchip DDR init (`rkbin`) | Rockchip binary licence | Used by ROCKNIX's RK3566 U-Boot (`UBOOT_FIRMWARE="rkbin atf"`). Read the terms once. |
| Wi-Fi and Bluetooth firmware | per file, in `linux-firmware`; ROCKNIX's `extra-firmware` is marked `proprietary` | Read each licence file once. |
| Non-commercial cores | Snes9x, Genesis Plus GX, PicoDrive, FBNeo, MAME 2003 | As above. |
| DuckStation | CC BY-NC-ND since 2024-09; "pre-configured settings and packages are considered modifications" | **Can't be shipped configured.** PlayStation stays on Beetle PSX HW, PCSX ReARMed and SwanStation. |
| DraStic, BigPEmu | freeware terms; ROCKNIX bundles them | Not in v1. |
| ES-DE | MIT on Linux (the Android app is paid and partly closed) | Can be built and shipped (section 6.1). |
| FlipBase / BaseOS | MIT / **no licence file** | FlipBase's patches may be reused; BaseOS can be read but nothing copied. |
| MinUI / NextUI / spruce | none / PolyForm NC / CC BY-NC | Take nothing from them. |

This is analysis, not legal advice.

---

## 4. Target architecture and module boundaries

### 4.1 Layers

The sibling plans already place most of this. This plan adds the Linux column and nothing else.

```
 mods/*.txt, mods/hotkeys/      shared, line-based lists: systems, cores, pack-ins, emulators, hotkeys
                                (device_profiles_plan.md: "extend the existing lists; no new file format")
 packaging/                     layers: profiles, socs, gpus, displays, devices
                                (device_profiles_plan.md §7; multi_device_plan.md §4.2 device descriptors)
 scripts/*.py                   generators and scripts/resolve_package.py (the layer resolver)
 nano (source)                  one C++ source: GammaOS's tree at the pinned commit plus the patch's Nano hunks
   NanoPlatform.h                 the seam (android_launcher_plan.md §1)
   platform/firmware/             today's code: DRM/SurfaceFlinger, properties, root `am`, /data/system
   platform/android/              JNI to a Kotlin shell (Thimsy Home)
   platform/linux/                Wayland, evdev, config store, fork/exec, sysfs, D-Bus      <- this plan
   compat/                        header shims so shared files compile off Android           <- this plan
 scripts/build.sh, mods/*.sh    the Android firmware build (unchanged)
 thimsy-os-linux (new repo)     ROCKNIX pin, THIMSY distro, packages, patches; GPL-2.0       <- this plan
 thimsy-os-installer            engine, app, catalog (generated from the descriptors)
 LIBRARY-FORMAT.md + vectors    the card contract (android_launcher_plan.md §3; section 7 here)
```

Rules:

- **Nano's shared code never includes an Android or Linux system header.** It calls `NanoPlatform.h`, or a `compat/` shim.
- **Device facts come from one descriptor** (`multi_device_plan.md` §4.2). The Linux build adds fields to it (section 8.3).
- **A system, a core or an emulator is named in one list.** Each platform's label, package or binary is a column of that list, not a copy.
- **The resolver runs once per build.** `scripts/resolve_package.py` produces the resolved manifest; the Android build and the Linux build are two consumers of it.
- **The GPL boundary is a repository boundary.** `thimsy-os-linux` consumes this repository's lists, scripts and Nano source at a pinned commit and copies nothing from ROCKNIX back.

### 4.2 Today's files, by reuse class

| Today | Class | For Linux |
|---|---|---|
| `mods/retroarch-cores.txt` (with `D -` defaults), `retroarch-cores.lock`, `scripts/refresh_cores.sh` | shared list; the lock is Android's | A `linux=` column where the core name or default differs. v1 uses ROCKNIX's core packages; `wanted_cores()` in `modlib.sh` hard-codes `_libretro_android.so` and needs a platform suffix |
| `mods/nano-systems.txt` (41 systems), `scripts/nano_systems.py` | shared; players are Android (Daijisho catalog ids, intents) | A per-platform player column; a `rocknix_platform` column where the names differ (section 6.4) |
| `tools/gen_esde_emulators.py` (inside the Nano source tree, P:163379) | shared; reads ES-DE's Android tables today | Also read `linuxarm` (and `linux` for an x86 target) |
| `mods/esde-emulators.txt`, `scripts/esde_config.py` | **1.2 base with the real ES-DE app only** (`mods/37-esde.sh` is their only caller and refuses 1.4.4) | Needed on Linux only if decision 8 ships ES-DE |
| `mods/bundled-roms.txt`, `pack-in-media.txt`, `pack-in-art/`, `rom-files/`, `rom-licenses/`, `scripts/bundled_roms.py` | shared data; the script rebuilds GammaOS's ROM pack and requires it as input | Two changes: the two `upstream` games become `add` lines with URL and hash, and the script gets a no-input-pack mode and a root argument |
| `mods/hotkeys/*.json`, `mods/device-keys.json` | shared | Device matching on Linux by device-tree `model`; pad identity differs (section 5.3) |
| Theme repack in `mods/50-nano.sh`, `scripts/check_system_art.py` | shared | as is |
| `ps3xmb/i18n/*.json` (21 languages), `ps3xmb/daijishou/*.json`, controller drawings | shared (inside the Nano source) | as is |
| `mods/retroarch-shaders/rg_cube_*` | shared; `multi_device_plan.md` §5 renames the files, `device_profiles_plan.md` moves panel shaders to `packaging/displays/<WxH>/shaders` | follows those plans; `.slang` presets need a Vulkan or glcore RetroArch driver |
| Boot sound, logo sources, `tools/bootanimation` | sources shared; formats per platform | Nano draws the Linux splash |
| `scripts/modlib.sh` | half shared (payload tar helpers, `buildbot_get`); half Android | split |
| `pactool.py`, `lptool.py`, `erofs_image.py`, `avb_footer.py`, `sprd_drm.py`, `patch_*`, `odm_sepolicy.py`, `bootlogo.py`, `apk_home.py`, `apps_list.py`, mods 12, 15, 25, 27, 40, 45, 47, 48 | Android only | - |
| Framework Java, SurfaceFlinger, MediaProvider, sepolicy, init.rc hunks of the patch | Android only | - |

A wholesale move to `data/`, `gen/` and `platforms/` folders was considered and dropped: it would contradict `device_profiles_plan.md` §7.1 and `multi_device_plan.md` §5, which edit these files where they are.

### 4.3 Getting Nano out of the patch

Today Nano exists only as GammaOS's tree at commit `0bc531e2d` plus the patch's hunks for `frameworks/base/cmds/gammaos-nano` (486 of the patch's 841 files). **The fetch already exists:** `tools/android-launcher/fetch_nano.sh` builds that tree for the launcher plan, and `tools/android-launcher/ndk_probe.py` lists non-portable calls. Its header records a constraint that matters here too: the upstream tree "holds file names Windows can't store", so it is never checked out on Windows.

1. **Reuse and extend those two tools** (moving them to a neutral path such as `tools/nano-tree/` when the Linux build needs them). The patch stays the source of truth. The Android firmware build is untouched. The APK and Linux builds compile the fetched tree with CMake.
2. **The seam and the compat shims land in the patch**, so all three builds see the same source.
3. **Later (decision 7):** make the extracted tree the source of truth and have the Android build copy it into the AOSP tree. The patch shrinks to the framework parts, and upstream GammaOS changes arrive as a three-way merge. Only once the seam has settled, because it changes how every backport in `BACKPORTS.md` is made.

**Shims first, interfaces second.** Nano calls `property_get` 1,020 times in 66 files, `ALOG*` 1,491 times in 70, and `system()`/`popen()` 118 times in 24. Rewriting those call sites would make every later upstream merge conflict. Instead `compat/` provides `cutils/properties.h`, `log/log.h`, `utils/Thread.h`, `utils/Mutex.h` and `utils/StrongPointer.h` for non-Android builds, backed by a key/value store and `std::` types. Real `NanoPlatform.h` interfaces are cut only where behaviour differs: display, input source, launch, audio, storage, network, power, system actions.

### 4.4 Build system

- **Nano:** CMake, three targets: `nano_core` (static), one `nano_platform_*`, and the `thimsy-nano` executable. The 29 production files that already build on a host in `cc_test_host` targets become the first CTest suite.
- **Linux image:** ROCKNIX's `make docker-<SOC>` inside WSL2 (section 10). `thimsy-nano` is one more `package.mk`.
- **Android image:** unchanged `scripts/build.sh` (with `DEVICE=` once `multi_device_plan.md` Phase 1 lands).
- **CI:** one workflow per platform plus the card conformance tests (section 11).

### 4.5 Shared work with the sibling plans

Five plans now lean on the same foundations. Each piece should be built once.

| Piece | Owned by | This plan needs it for |
|---|---|---|
| `NanoPlatform.h` seam, Nano built outside AOSP | `android_launcher_plan.md` Phase 0 and 1 | everything in section 5 |
| Device descriptors | `multi_device_plan.md` Phase 1 | catalog entries, per-device pad and display facts |
| Layer resolver, profile names | `device_profiles_plan.md` Phase 0 and 1 | what each Linux image contains (section 8.4) |
| Emulator definitions with `storage` blocks | `user_emulators_plan.md` Phase 1 and 5 (not started) | launch and link rules (sections 6.4, 7.4). If that plan is later, this one lands a minimal definition schema itself |
| `pack.json` format 2 | `multi_device_plan.md` §7.4; also the launcher plan's Phase 6 | disk-image packages (section 8.3) |
| The drive model (ES-DE Drive, Emulator Settings Drive) | `usb_storage_plan.md` | which drive is "the card" (section 7.3) |
| `LIBRARY-FORMAT.md`, `nano/format.json` | `android_launcher_plan.md` Phase 3 | section 7 |

---

## 5. Nano on Linux

### 5.1 How big the job is

From the audit of the rebuilt tree (203,854 lines in 208 top-level files):

| Part | Lines | What happens |
|---|---|---|
| Generated or table data (OSK dictionaries, `stb_image`, icon tables, ES-DE tables) | about 28k (14%) | compiles as is |
| UI and app logic | about 141k (69%) | ports once the seam, the config store and the shims exist |
| Android-bound | about 35k (17%) | replaced or dropped: overlay 3k, media engines 4.6k, the DraStic runner stack 18k, `NanoSdMove` 2.7k, MTP 0.4k, most of `NanoMenuDrm.cpp` (3.6k), `main.cpp` 1.2k |

Only 33 files include Android framework headers, but 110 files (147.6k lines) touch at least one Android API, path or command. The difficulty is not the count; it is that everything hangs off one `NanoMenu` class (`NanoMenu.h` is 6,629 lines) and platform calls sit inline in UI files.

What helps:

- **Input is raw evdev** (`/dev/input`, `EVIOCGRAB`, inotify hotplug; 577 uses of `KEY_`/`BTN_`/`ABS_` codes against 14 of `AKEYCODE`). The readers port; what they read differs per device (section 5.3).
- **The display code has a target interface** (`NanoDisplayTarget`) and sizes everything from the active mode. Its KMS path is legacy `drmModeSetCrtc` with Android buffers and a pbuffer context, so the mode-setting logic carries over while buffer allocation, EGL set-up and presentation are rewritten.
- **The ES-DE store is pure file I/O** (`NanoEsdeStore.cpp`).
- **Nano has no binder interface of its own.** It talks to system_server and init through properties and files under `/data/system`.

### 5.2 Android-specific parts and their Linux replacements

Everything on ROCKNIX runs as root, including Sway and the frontend.

| Subsystem | Android today | Linux replacement | Effort |
|---|---|---|---|
| Display and GL | KMS ioctls with AHardwareBuffer/gralloc buffers imported through `EGL_NATIVE_BUFFER_ANDROID`; SurfaceFlinger fallback | Two targets behind `NanoDisplayTarget`: **Wayland** first (xdg-shell fullscreen, `wl_egl_window`), **KMS/GBM** later (section 5.3) | medium |
| Panel rotation | sizes from the active mode; rotation untested off the Cube (`multi_device_plan.md` §6.8) | Sway applies the panel's orientation. Nano's start script must set the same rotation environment ROCKNIX's `start_es.sh` does (`prerotate_env`). The ARC, RG28XX, RP5 and Odin 2 panels are all natively portrait | medium |
| Fonts | `/system/fonts`: Roboto, NotoSansCJK, NotoNaskhArabic, NotoSansThai, NotoSansHebrew (patch lines 507-511, 10435) | A `thimsy-fonts` package (Apache-2.0 and OFL fonts) and a path mapping. ROCKNIX ships only `corefonts` and `noto-sans-cjk`; without the rest, Arabic, Hebrew and Thai render as missing glyphs | easy |
| Themes, XMB, OSK, text | GLES 2, FreeType, libpng, stb. Three exceptions: `AImageDecoder`, screen capture for the clock blur, the accelerometer | stb_image/libjpeg-turbo/libwebp; read back Nano's own framebuffer; drop the sensor | easy |
| Quick Menu over a game | second process (`--overlay`) drawing a SurfaceFlinger layer above the app; summoned by properties; hotkeys in `PhoneWindowManager`; input cut off through `drop_input` in InputDispatcher | section 5.3 | **hard** |
| Pad model | one built-in pad per device, matched by vendor and product id (`device-keys.json`), plus gammapad | section 5.3: two input stacks, a 6-button pad with no sticks on the first target | medium |
| System keys | Android's own (volume, power) plus `ThimsyHotkeyController` in system_server | `thimsy-session` takes over what ROCKNIX's `input_sense` does: volume, power key and suspend, lid, brightness, the quit hotkey. The hotkey state machine is ported from the Java, which already has host tests | medium |
| Settings store | 539 `persist.gammaos.*` and 343 `sys.gammaos.*` property keys | **the same key names** in a config store: `persist.*` in one file under `/storage/.config/thimsy/` (atomic rename), `sys.*` under `/run/thimsy/`, change notice by inotify, a `thimsy-prop` CLI for scripts. The launcher plan needs the same store | medium |
| State files | 127 paths under `/data/system/nano_*` | `/storage/.config/thimsy/state/`, same file names | easy |
| Launching | `am start` argument strings; Nano writes an intent file, sets a property and exits; `RootWindowContainer` starts the activity | Nano stays resident, runs the (patched) `runemu.sh`, waits on the pid. Templates come from the emulator definition's Linux block (section 6.4). `NanoLaunch.cpp`'s token code is reused | medium |
| Memory | Nano exits when a game starts | On 1 GB devices a resident Nano must free its textures at launch, or exit and restart as on Android. Measure its resident size in Phase 2 | medium |
| Audio (menu sounds, chime) | AAudio, tinyalsa | PipeWire (ROCKNIX runs PipeWire with WirePlumber and the PulseAudio API) | medium |
| Media engines (video, music, radio, IPTV) | AMediaCodec into a GL texture | FFmpeg, with V4L2 hardware decode later. **Not in v1** | hard |
| DS player, in-process runners | loads DraStic's Android `.so` through a fake JNI | **Dropped.** Nintendo DS runs on a melonDS core. `LibretroRunner` can port later | drop |
| Quick Resume | a property plus the runners | RetroArch auto save and auto load. Later | later |
| Emulator files on the card | `NanoSdMove` (SAF grants, `pm`, SELinux contexts), `NanoEmuSync` (three-way sync of app-private files) | mostly deletion: Linux emulators read ordinary directories (section 7.4). The adoption and re-pointing logic stays | medium |
| Storage and volumes | vold paths `/storage/<id>`, `/mnt/media_rw/<id>` (245 uses in 44 files), MediaProvider denylist | **keep the paths** (section 7.3); mounting done by `thimsy-session` | medium |
| System settings | shell: `settings`, `cmd wifi`, `svc`, `cmd bluetooth_manager`, `cmd alarm`; the `NanoNetBridge` property RPC | a system-actions table: **NetworkManager (iwd backend) and BlueZ over D-Bus**, `timedatectl`, `/sys/class/backlight`, the PipeWire mixer | medium |
| Battery, charging, LEDs | health and lights HALs, with a sysfs fallback for battery | `/sys/class/power_supply`, `/sys/class/leds`; low-battery warning in Nano | easy |
| Sleep, power | `/sys/power/wake_lock`, `input keyevent 223/224`, `sys.powerctl`, `nano-powerwatch` | `systemctl suspend`; logind inhibitors; the power key through `thimsy-session` | easy to medium |
| USB-C, dock, monitor modes | `NanoTypec` (plain sysfs), DisplayPort through the Unisoc driver, `GammaosScreenArrangement` | The ARC has mini-HDMI: mirror through Sway first. Monitor modes later | later |
| Network extras | libcurl (scraper, OTA check), Syncthing binary, SMB through `gammaos-sharefs` | same libcurl; Syncthing and Samba are in ROCKNIX | easy to medium |
| Update | `gammaos-ota` | ROCKNIX's mechanism (a `.tar` in `/storage/.update`, applied by the initramfs) pointed at Thimsy's releases | medium |
| Init, SELinux | `gammaos-nano.rc`, the `bootanim` domain, capabilities | systemd units; no SELinux | easy |
| Framework helpers | `NanoNetBridge`, `NanoRetroArchConfig`, `NanoUriGrant` (Java, in system_server) | folded into `thimsy-session` or dropped | medium |

### 5.3 The display stack, the Quick Menu and the pad

**Display.** This is the one architectural fork.

| Option | How the Quick Menu draws over a game | Cost |
|---|---|---|
| **(A) Nano as a Wayland client under ROCKNIX's Sway** | The overlay instance opens a `wlr-layer-shell` surface on the overlay layer with exclusive keyboard focus: the direct analogue of the SurfaceFlinger layer. Sway implements the protocol, and ROCKNIX already ships a layer-shell client (`mako-osd`) | Keeps ROCKNIX's launch scripts, which need a running Sway (`runemu.sh` calls `swaymsg`), and its rotation handling |
| (B) Bare KMS, Nano is DRM master | No compositor, so no layer. Either the emulator gives up the screen, which SDL and RetroArch do not handle gracefully *(guess)*, or the menu is each emulator's own | Fastest first frame. Every emulator re-qualified on KMS; ROCKNIX's launcher rewritten; the Quick Menu becomes per-emulator work |
| (C) Our own minimal wlroots compositor | as (A) | Removes Sway's weight and gives full control of ordering and the splash. A new program to write and maintain |
| (D) gamescope | its own overlay protocol | Needs Vulkan; H700 has none under Panfrost in ROCKNIX |

**Recommended: (A).** The reasons are ROCKNIX's launch path and rotation handling, and that little of Nano's KMS code is reusable anyway. A KMS/GBM target is a later experiment. It can't be combined with a resident Nano and a layer-shell menu (Sway can't take the display while Nano holds DRM master), so it only makes sense together with (C) or a Thimsy Base (section 9.5). Decision 3.

**Silencing the pad while a menu is open.** On Android the framework drops input to the game. On Linux:

- **RK3566 (InputPlumber).** The ARC's pad is exposed as an emulated DualSense plus a keyboard. That DualSense is a `uhid` device with a `hidraw` node, and ROCKNIX builds SDL2 with its HIDAPI joystick driver on. So `EVIOCGRAB` on the evdev node silences RetroArch (udev driver) but **not** SDL-based standalone emulators (PPSSPP, Flycast, mupen64plus). Use InputPlumber's D-Bus intercept mode, which exists for overlays, or switch the composite device's target while the menu is open; as a fallback launch emulators with `SDL_JOYSTICK_HIDAPI=0`.
- **H700, RK3326 (`rocknix-joypad`).** `EVIOCGRAB` on the kernel pad node.
- **Either way:** a grab taken while the hotkey button is held leaves that button stuck down in the game; release it first.

Phase 0 tests this with one SDL standalone emulator and RetroArch, including the held-button case.

**The pad itself.** Nano's device matching by vendor and product id does not carry over: InputPlumber presents the same virtual pad on every device it manages. And the ARC is unusual:

- six face buttons (A, B, C, X, Y, Z) and no sticks;
- ROCKNIX maps Z and C to the left and right stick clicks, and swaps South and East in its capability map;
- Nano's hotkey options include L3 and R3 (`HOTKEYS.md:206`), which on the ARC would fire from Z and C;
- ROCKNIX ships ARC remap files for seven cores (Genesis Plus GX, Flycast, YabaSanshiro and others).

So the device descriptor gains, per device: face layout (4 or 6 buttons), whether sticks exist, the printed label of each logical button, and which input stack the base uses. Nano hides stick-dependent actions where there are none. RetroArch remaps are per device, never shared on a card (section 7.5).

### 5.4 Feature map

"Alpha" is the first build that runs (roadmap phase 2). "v1" is the first public release with the card contract and the installer. "Later" has a phase in section 12. "n/a" does not apply to a Linux device.

| Feature (Android Thimsy 1.4.4) | Linux |
|---|---|
| XMB home, the ES-DE theme engine, Minima; navigation, sounds, clock | **alpha** |
| 21 languages, right-to-left text, CJK on-screen keyboard | **alpha**, with `thimsy-fonts` (the Kanji table depends on the licence answer in section 3.3) |
| Game library: scan, favourites, hidden, collections, search, Recently Played | **alpha** |
| Launching RetroArch cores; per-system and per-game emulator choice | **alpha** |
| Settings: Wi-Fi, Bluetooth, brightness, volume, time, language, sleep | **alpha** through system actions |
| Volume, power and quit keys; "Close Current App" by hotkey; universal hotkeys for RetroArch | **v1** (`thimsy-session`) |
| ES-DE store: **Move ES-DE to Drive**, adoption of an existing `ES-DE` folder | **v1**. On Linux the store is always on (section 7.2) |
| **Move Emulator Files to Drive**, the Emulator Settings Drive, the foreign-card dialog | **v1**, in its Linux form (section 7.4) |
| Setup wizard | **v1**, reduced: language, Wi-Fi, time zone, drive adoption |
| Scraper | **v1** (libcurl; same code) |
| BIOS manager (by SHA-1) | **v1**; targets are Linux paths |
| File Explorer, archives (zip, 7z) | **v1** (`NanoFileJob`, `NanoArchive` are host-tested). PDF viewer later |
| Standalone emulators with their hotkeys (PPSSPP first) | **v1** for PPSSPP and ScummVM; others as added |
| System update | **v1**, ROCKNIX's update file from Thimsy's release page |
| Quick Menu and Control Center over a game; button map overlay | **Later** (phase 5) |
| Quick Resume | **Later** |
| Built-in DS player (drastic-nano) | **Dropped**; melonDS core instead |
| Video, music, photos, radio, IPTV | **Later**, on FFmpeg; photos first |
| Syncthing, network shares, file sharing server | **Later**. ROCKNIX ships Samba and Syncthing; Nano's menus need wiring |
| USB file transfer (MTP) | **Later** (USB gadget; depends on the device's OTG support) |
| External display, monitor modes, dock, Dual Screen | **Later**, per device. The ARC's HDMI mirror first |
| RGB LEDs | **Later**, where the device has them (RG40XX, CubeXX) |
| Global shaders (SurfaceFlinger) | **n/a** as built; RetroArch shaders cover cores |
| Android Games, Applications, Boot Android | **n/a** |

---

## 6. ES-DE, RetroArch and standalone emulators on each platform

### 6.1 ES-DE

| | Android (Thimsy today) | Linux |
|---|---|---|
| The app | Paid, partly closed. 1.2 base only, when the build is given `ESDE_APK`. **Refused on 1.4.4** (`mods/37-esde.sh:31-34`) | MIT. An official `aarch64` AppImage exists since 3.4.1 but is desktop-GL only ("will only run in a desktop environment") |
| What Thimsy uses instead | Nano's store mode: an `ES-DE/` folder in ES-DE's own formats is Nano's library database | the same code |
| Needs to run on a handheld | - | a self-built binary with **`GLES=on`** (GLES 3.0); under Sway nothing more; on bare KMS also `DEINIT_ON_LAUNCH=on` |
| Data directory | chosen in ES-DE's first-run wizard; Thimsy seeds `/sdcard/ES-DE` | `~/ES-DE`, or `ESDE_APPDATA_DIR`, or `--home` |
| System and label tables | `resources/systems/android` | **`resources/systems/linuxarm`** on AArch64 (every Thimsy Linux target); `linux` is the x86 table |
| Find rules | `androidpackage` rules | `systempath`, then `staticpath` |
| Launch commands | Android-only tokens: `%EXTRA_*%`, `%ROMSAF%`, `%ANDROIDPACKAGE%` | `%ROM%`, `%EMULATOR_*%`, `%CORE_*%` |

So "export through ES-DE" on Linux is Nano's store, not the ES-DE program. The files are identical: `gamelists/<system>/gamelist.xml`, `collections/custom-*.cfg` with `%ROMPATH%`, `downloaded_media/`, plus `nano/`.

**Shipping the real ES-DE on Linux is optional** (decision 8). If shipped, it must not share `settings/` and `custom_systems/` commands with Android. It gets its own data directory on internal storage with `gamelists`, `collections` and `downloaded_media` linked to the drive (the links live on internal storage, section 7.4) and Linux find rules that call the launcher.

### 6.2 RetroArch

| | Android | Linux (ROCKNIX) |
|---|---|---|
| Main config | `Android/data/com.retroarch.aarch64/files/retroarch.cfg`; after a move, `<card>/emulator_config/retroarch/retroarch.cfg` | `/storage/.config/retroarch/retroarch.cfg`, **hard-coded in `runemu.sh`** and edited in place by `setsettings.sh` on every launch, plus an append config in `/tmp` |
| Cores | `<core>_libretro_android.so` from the buildbot nightlies pinned in `retroarch-cores.lock` | `<core>_libretro.so`, built from source by ROCKNIX. **Some default cores are 32-bit and run in `retroarch32`**: `pcsx_rearmed32`, `gpsp`, `yabadabadoo`, `desmume` |
| In-game saves | beside the ROM (`savefiles_in_content_dir = "true"`, fixture P:152007) | beside the ROM |
| Save states | beside the ROM (`savestates_in_content_dir = "true"`) | `/storage/roms/savestates/<platform>`: ROCKNIX ships `savestates_in_content_dir = "false"` and writes `savestate_directory` on every launch |
| BIOS | `system_directory`; after a move `<card>/emulator_config/retroarch/system` | `/storage/roms/bios` |
| Per-core files | `config/<Core>/*.cfg`, `*.opt`, shader presets; `config/remaps/` | same names. `setsettings.sh` rewrites several `.opt` files per launch (ParaLLEl N64, Flycast, melonDS DS and others) |

What Thimsy Linux does:

- **The main config and `config/` stay on internal storage.** They hold drivers, device-tuned core options, per-device remaps and paths that ROCKNIX's scripts regenerate. They are not a card format. (Android's `retroarch/config/` on a card is no better as a shared folder: Thimsy's own test fixture shows per-core overrides there naming `/data/user/0/com.retroarch.aarch64/overlays/...` and `.slangp` presets; `RaConfig.TreeRewritesConfigsAndPresetsOnly`, P:161105.)
- **Directory keys point at the card** for what is shared: `system_directory` (BIOS) and `cheat_database_path`. Thimsy's patched `setsettings.sh` writes them into the append config.
- **In-game saves are shared only between the same core** (section 7.5). A `.srm` is named after the ROM, so two different cores would overwrite each other's file.
- **Save states are per platform in v1.** A state file is also named after the ROM only; a 64-bit Beetle PSX HW state and a 32-bit PCSX ReARMed state would share one path, and auto-save would overwrite the other platform's `.state.auto`. Linux writes states to `<card>/emulator_config/retroarch/linux/states/<system>/`. Android keeps writing beside the ROM.
- **Default cores are chosen to match Android's where the hardware allows**, because that is what makes saves travel. A per-system table of both platforms' default cores is a Phase 3 deliverable; where they differ and are not known to be save-compatible, Linux turns on `sort_savefiles_enable` for that launch, which puts its saves in a per-core folder beside the ROM (RetroArch does this under the content directory; checked in `runloop.c`).

### 6.3 Standalone emulators

Checked 2026-11-24 against each project's release assets.

| Emulator | Android (Thimsy) | Linux aarch64 | Config redirection on Linux | Viable on RK3566 / H700 |
|---|---|---|---|---|
| PPSSPP | `org.ppsspp.ppsspp` 1.20.4 | official aarch64 AppImage; ROCKNIX package | memstick directory; `start_ppsspp.sh` rewrites `PSP/SYSTEM/ppsspp.ini` (graphics backend, resolution) on every launch | yes / light games |
| Flycast | `com.flycast.emulator` 2.7 | **no official aarch64 binary**; ROCKNIX builds it; or the core | `emu.cfg`, data dir (XDG) | yes / marginal |
| N64 | M64Plus FZ (Android only) | mupen64plus standalone, or the `mupen64plus_next` core | `--set Core[SaveSRAMPath]=...`; there is no save-directory flag | yes / some |
| Nintendo DS | WatermelonDS, SeedlessDS, built-in DraStic player (Android only) | melonDS DS core, melonDS 1.1 (aarch64 AppImage), DraStic's Linux build (closed) | `melonDS.toml` | core: yes |
| Dolphin | `org.dolphinemu.dolphinemu` | no official aarch64 binary; ROCKNIX package | `-u`, `DOLPHIN_EMU_USERPATH` | no *(guess)* |
| Azahar (3DS) | `org.azahar_emu.azahar` | no official aarch64 Linux build | XDG dirs | no |
| PS2 | ARMSX2 | ARMSX2 2.9.6 Linux arm64 AppImage | PCSX2-style `inis/` *(guess)* | no |
| Xbox | hakuX (Android only) | xemu aarch64 AppImage | `xemu.toml` *(guess)* | no |
| Vita | Vita3K | aarch64 AppImage; needs Vulkan | `config.yml` `pref-path` | no |
| ScummVM | `org.scummvm.scummvm` | ROCKNIX package | `--config`, `scummvm.ini` | yes |
| PlayStation standalone | - | DuckStation exists but **can't be shipped configured** | - | - |

**Mapping to package profiles.** `device_profiles_plan.md` already recommends **Small** for the H700 and for RK3568-class chips. Small has no Dreamcast and no standalone N64 emulator (N64 is the `mupen64plus_next` core), and names SeedlessDS as the DS default. So:

- RK3566 and H700 Linux images are profile **Small**, with a `linux` column for substitutions: DS default is the melonDS DS core, since SeedlessDS is Android only.
- On the 2 GB RK3566 devices, **Medium** is offered; that is where Flycast and PPSSPP's heavier defaults sit. Whether Small on Linux should gain Flycast is a proposed change to `device_profiles_plan.md` §5, not something this plan decides.
- The heavy systems (GameCube, Wii, 3DS, PS2, Xbox, Vita, Switch) are absent from RK3566 and H700 images. They return on Qualcomm devices.

### 6.4 How a game is launched

| Step | Android firmware | Linux |
|---|---|---|
| What Nano stores | a system: `launchType` (`libretro-core` or `custom-package`), `coreSo`, `package`, `intentTemplate` (`ES-DE/nano/systems.json`); a game: an ES-DE label in `<altemulator>`, or `am` arguments in `nano/emulators.json` | reads the same files |
| Resolving | a `core:<name>` or `pkg:<package>` key (`esdeEmulatorKey`, P:20546) to an ES-DE label and back | the same key. `core:` strips `_libretro_android.so`. `pkg:` is looked up in the emulator definitions to find the emulator, then its Linux block |
| Starting | intent file plus property; system_server starts the activity | `runemu.sh "<rom>" -P<platform> --core=<core> --emulator=<emu>`. `-P` takes **ROCKNIX platform names**; four of Nano's folder names differ (`fbneo`/`fbn`, `gc`/`gamecube`, `n3ds`/`3ds`, `pico8`/`pico-8`), so the systems list gains a `rocknix_platform` column |
| When the choice doesn't exist here | "Install" or "Choose Emulator" | fall back to this device's default for the system and leave the stored value alone (section 7.6 says where that needs new code on Android too) |

**Emulator definitions.** The definition is `user_emulators_plan.md`'s (not started), which `android_launcher_plan.md` §3.8 makes the shared profile format. Its `players` are Daijisho-schema entries (`platform` is the console id; `amStartArguments`), and its `storage.link` is `[{base, dirs}]`. The Linux data therefore goes in **new, separately named keys**, and the file becomes `schema: 2`:

```json
{ "schema": 2, "id": "ppsspp",
  "packages": ["org.ppsspp.ppsspp"],
  "players": [ { "platform": "psp", "name": "PPSSPP", "amStartArguments": "..." } ],
  "family": "psp.ppsspp",
  "esde_labels": { "android": "PPSSPP (Standalone)", "linuxarm": "PPSSPP (Standalone)" },
  "linux": {
    "players": [ { "systems": ["psp"], "run": "runemu", "emulator": "ppsspp" } ],
    "storage": [ { "link": "ppsspp/PSP/SAVEDATA", "to": "PSP/SAVEDATA" } ] } }
```

- Both validators that plan names (`NanoEmuDefs` and `ThimsyHotkeyController`) must add the new keys before any such file ships; a schema-1 reader refuses a schema-2 file, by that plan's own rule.
- `family` joins emulators that are the same engine under different names (M64Plus FZ and mupen64plus; WatermelonDS and melonDS). A per-game choice made on one platform maps to the same family on the other.

**Trust rules.** A definition can name a command and links, and everything on ROCKNIX runs as root. So, as on Android (`user_emulators_plan.md`: "Import, not live read"):

- Linux never acts on `Thimsy/emulators/*.json` straight from a drive. It imports through the same confirm screen into internal storage and records the origin.
- For an imported definition, `run` must be `runemu` with an emulator and core from the image's own list. A free command line is accepted only in definitions shipped in the image.
- A link's source must lie under fixed roots (`/storage/.config/<dir>`, `/storage/.local/share/<dir>`) declared by a shipped definition; never an absolute path or `..`.

**Label tables.** Checked at ES-DE commit `50e4b600a`, counting `label="..."` in each table:

| Label | `android` | `linuxarm` | `linux` (x86) |
|---|---|---|---|
| Beetle PSX HW, YabaSanshiro, Beetle PCE FAST, Gearcoleco | yes | yes | yes |
| SameBoy (Thimsy's default for gb, gbc, sgb) | yes | **no** (only `SameBoy (Standalone)`) | yes |
| ARMSX2 (Standalone) | yes | yes | no |
| M64Plus FZ, WatermelonDS, hakuX (Standalone); Fake-08 | yes | no | no |
| Mupen64Plus (Standalone) | no | yes | yes |

Each Nano build carries the `android` and `linuxarm` tables (and `linux` for an x86 target) so it can read a label the other side wrote.

---

## 7. Card profile interop specification

This section is the Linux half of `LIBRARY-FORMAT.md`, the document `android_launcher_plan.md` §3 calls for. The spec and its test vectors are what the three implementations (firmware, app, Linux) are tested against.

"The card" below means **the drive each setting names**. `usb_storage_plan.md` replaces "the microSD card" with a drive list and two settings, **ES-DE Drive** and **Emulator Settings Drive** (`persist.gammaos.nano.storage.esde_drive` and `.emu_drive`), each a volume id or `internal`, with an automatic fall-back to internal storage after a 10 s grace. The Linux build implements that model from the start; it never had the single-card one. On Linux the boot card and eMMC are excluded from the drive list by the root block device, not by that plan's `/mmc` test.

### 7.1 Principles

1. **A drive holds only regular files and folders.** No symlinks, device nodes, sockets, permissions or case-sensitive name pairs. FAT32 and exFAT have none of these, and Android's `/storage/<id>` view would hide them anyway.
2. **Paths stored on a drive are relative, tokenised (`%ROMPATH%`, `%CARDID%`), or the canonical absolute form `/storage/<volume id>/...`.** Both platforms resolve that form to the drive. (On a card today there are also `/mnt/media_rw/<id>` in Vita3K's `pref-path`, and `content://` tree URIs and ScummVM `/saf/` paths inside the Android-only `app/` mirrors.)
3. **Share data, not configuration.** v1 shares the library, BIOS files and in-game saves made by the same core or emulator. Settings, core options, remaps, overrides and save states are per platform. Caches are per device.
4. **A new build never rewrites what it can't interpret.** Released Android builds do not all follow this (section 7.10), so the format avoids putting Linux-only values where they would.
5. **Nothing but an emulator writes into a shared folder.** No launcher script, no settings generator.
6. **On a clash, the drive's copy is kept and the device's is set aside.** For the Android-only `app/` settings mirror the rule is `NanoEmuSync`'s `decide` (P:17415): the card wins. For shared data folders the rule is the move's: the device's copy goes to the backup folder on internal storage (`Nano Backup` today) and the user chooses.
7. **One device at a time.** There is no concurrent writer. Unclean removal is the failure to design for.

### 7.2 Layout

```
<drive>/                              exFAT (recommended) or FAT32
  ES-DE/                              the library; ES-DE's own formats (exists today)
    gamelists/<system>/gamelist.xml
    collections/custom-<name>.cfg
    settings/es_settings.xml          ROMDirectory and scraper keys only
    custom_systems/es_systems.xml     systems in folders ES-DE has no system for
    downloaded_media/<system>/<type>/
    nano/format.json                  {"version": N}                         (launcher plan)
    nano/systems.json  names.json  emulators.json  index.json
    nano/systems.linux.json           NEW: Linux's own emulator choice per system
  ROMs/<es-de system name>/           games; in-game saves beside them; Android's states beside them
  emulator_config/
    <emu>/...                         shared data: saves, memory cards, BIOS
    <emu>/app/...                     Android only: mirror of app-private preferences (exists today)
    <emu>/linux/...                   NEW, Linux only: states and any config that must travel between Linux devices
    retroarch/{system,cheats}/        shared
    retroarch/retroarch.cfg, config/, playlists/, ...   Android's (exist today); Linux does not use them
    retroarch/linux/states/<system>/  NEW: Linux's save states
    card.json                         NEW: see 7.7
  bios/                               drop folder the BIOS installer offers first (a convention today)
  Thimsy/emulators/*.json             user emulator definitions to import (user_emulators_plan.md)
```

Nothing existing moves. New: `<emu>/linux/`, `nano/systems.linux.json`, and `emulator_config/card.json`.

**Store mode is always on in the Linux build.** With no drive chosen, the store is `/storage/emulated/0/ES-DE` on internal storage: the same switch as `usb_storage_plan.md`'s "ES-DE Drive = Internal Storage" and the launcher plan's D7.

### 7.3 Path mapping and mounting

The Android build writes absolute paths of the form `/storage/<VOLID>/emulator_config/...` into configs on the card (`emusync::repoint`, P:17477). `<VOLID>` is the FAT or exFAT serial, upper-case `XXXX-XXXX`. Linux prints the same serial in the same form: e2fsprogs (Android's blkid) and util-linux use the same format string and byte order for both filesystems.

**So the Linux build mounts the drive at `/storage/<VOLID>`.** ROCKNIX's writable partition is already mounted at `/storage`, so this is a directory on it used as a mount point.

| Meaning | Android firmware | Linux build |
|---|---|---|
| The drive, as stored in files | `/storage/<VOLID>` | `/storage/<VOLID>` (the mount point) |
| The drive, root-side alias | `/mnt/media_rw/<VOLID>` | `/mnt/media_rw/<VOLID>` → symlink to the mount |
| Internal shared storage | `/storage/emulated/0`, `/sdcard`, `/data/media/0` | `/storage/emulated/0` (a directory on the ext4 storage partition); `/sdcard` → symlink |
| Nano state | `/data/system/nano_*` | `/storage/.config/thimsy/state/nano_*` |
| Nano settings | `persist.gammaos.*` properties | the config store (section 5.2), same keys |
| Emulator private files | `/data/data/<pkg>`, `Android/data/<pkg>/files` | `/storage/.config/<emu>`, `/storage/.local/share/<emu>` |
| Read-only assets, fonts | `/system/etc/nano_xmb`, `/system/etc/gammaos-nano`, `/system/fonts` | `/usr/share/thimsy/nano`, `/usr/share/fonts/thimsy` |
| ROMs for ROCKNIX's own scripts | - | `/storage/roms` → bind of `<drive>/ROMs` (or the internal `ROMs`) |

**The empty-mount-point hazard.** On Android, vold creates `/storage/<VOLID>` only while the volume is mounted. On Linux the mount point is an ordinary directory, writable by root, and everything on ROCKNIX is root. With the drive out, anything that creates its whole path (a launch script, Nano's own `mkdir -p`, RetroArch's sorted-save folders) would quietly write saves to internal storage, and the drive would later mount over them. Rules:

- One permanent, empty mount-point directory per adopted drive, with the ext4 immutable flag set (`chattr +i`). It can still be mounted over; nothing can be created in it.
- Before every launch, `thimsy-session` checks that each target is a mount point (compare `st_dev` with `/storage`). If not: the internal fall-back, or refuse.
- A test: drive out, launch each emulator, assert nothing appeared under `/storage/<VOLID>`.

**Mounting.** `thimsy-session` replaces ROCKNIX's `automount`, which mounts with no options, never checks the filesystem unless a write fails, and creates `bios`, `savestates` and every system folder on the card. Thimsy's mount:

- vfat: `utf8,shortname=mixed,noatime,nodev,nosuid,noexec,flush` (Android mounts vfat `utf8,shortname=mixed`; without `utf8` a FAT32 card's non-Latin file names read differently and stop matching `gamelist.xml`);
- exfat: `noatime,nodev,nosuid,noexec`;
- `fsck.exfat -p` or `fsck.vfat -a` first, only when the dirty flag is set;
- no folders created on the drive except by Nano's own moves;
- `sync` when a game exits, on sleep and on shutdown; a "Remove Drive" action that unmounts.

**Volume ids that can't be used:**

- An all-zero serial. Android's blkid gives `0000-0000`; util-linux reports no UUID at all. Rejected on both platforms.
- Two drives with the same id (a cloned card). The second is not mounted, with a message.
- `emusync::isVolumeId` (P:17466) also accepts 32-hex-digit ids; those are not FAT or exFAT and are not interop drives.

**One-card devices.** With one slot, or one card, the drive is an exFAT data partition on the boot card, created on first boot (AmberELEC, Knulli and dArkOS all do this). Whether an Android device mounts that third partition when the card is moved is *(guess: unverified)*; two cards are the supported interop setup until tested.

### 7.4 Symlinks: how link-based configs survive FAT, exFAT and FUSE

Today, on Android, only two emulators are linked (Dolphin and hakuX, which have no folder setting). The link sits on `/data` and points at the card:

```
/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/GC   (link, on f2fs/ext4)
        -> /storage/<VOLID>/emulator_config/dolphin/GC          (target, on the card, through FUSE)
```

It works because apps reach `Android/data` on the lower filesystem directly, not through FUSE, so they follow the link; and the target is an ordinary folder the app may open with all-files access (`README.md:604-608`; `linkFolder`, P:21157).

The rules that generalise this to both platforms:

1. **A link never lives on a drive.** It lives on the device's internal filesystem: `/data` on Android, the ext4 storage partition on Linux.
2. **A link's target is always a folder under `/storage/<VOLID>/emulator_config/<emu>/`** (or `ES-DE/`). The existing check stays: the target must start with that prefix and contain no `/.` (`emuRepointAbsent`, P:67500).
3. **Links are device state, rebuilt from data.** The drive stores no record of them. Each device recreates its links when it adopts a drive, from the definitions shipped in its image.
4. **Links are replaced atomically** (`symlinkat` to a temporary name, then `renameat`), as today.
5. **When the drive is absent.** On Android the link dangles. On Linux it does not: the target directory exists (see the hazard above), so the mount check before launch is what protects it. Either way the device re-points to the adopted drive's id if it changed, or to the internal fall-back.
6. **Re-pointing after an id change.** Android's repair pass walks `emulator_config` three levels deep, skips dot-names and a first-level `app`, and rewrites the old id in `.cfg .ini .txt .xml .yml .json` files under 1 MiB. So it does edit files under `linux/`, to that depth. The Linux build runs the same pass, over all depths under `linux/` and over its internal configs.
7. **Nano's own readers still refuse to follow links** inside app trees (`O_NOFOLLOW`, `sdAppPathSafe`), on both platforms.

On Linux there are no scoped-storage rules, so a link or the emulator's own directory option replaces everything `NanoSdMove` does with grants:

| Emulator | Android points it by | Linux points it by | Shared in v1 |
|---|---|---|---|
| RetroArch | paths in `retroarch.cfg`, `CONFIGFILE` at launch | directory keys in the append config (section 6.2); main config internal | BIOS, cheats, same-core saves |
| PPSSPP | `memstick_dir.txt` | **per-folder links**: `PSP/SAVEDATA`, `PSP/TEXTURES`, `PSP/Cheats`. `PSP/SYSTEM` stays internal (the launch script rewrites `ppsspp.ini`); `PSP/PPSSPP_STATE` goes to `ppsspp/linux/` | `SAVEDATA` |
| ScummVM | `savepath` in `scummvm.ini` | `--config` on an internal ini, `savepath` on the drive | saves, if both sides derive game ids the same way *(guess; test)* |
| Flycast | preference `home_directory` | a link for `data` (VMUs, BIOS); `emu.cfg` internal | `data` *(guess; test)* |
| mupen64plus / M64Plus FZ | preference `gameDataStoragePath`; FZ keeps each game in `GameData/<header name> <country> <ROM MD5>/SramData/` | `--set Core[SaveSRAMPath]=...`, one flat directory | **no**: the layouts differ. Later: a helper that computes FZ's per-game folder |
| Nintendo DS | WatermelonDS saves beside the ROM as `.sav` | melonDS DS core `.srm` beside the ROM *(guess on the name)* | **no**, until a rename rule is specified and tested |
| Dolphin | **symlinks** per subfolder | `DOLPHIN_EMU_USERPATH`, or the same links | not on RK3566 or H700 |

**Content that itself needs POSIX features** (PortMaster ports, native Linux games, anything with executable bits or internal symlinks) can't live on a FAT or exFAT drive as files. It stays on internal storage, or later in an ext4 image file on the drive that only Linux mounts. Not in v1.

**ext4 or f2fs cards** would hold links, but Android's public-volume support for them is *(guess: LineageOS-specific and still behind FUSE)*, Windows and macOS can't read them, and rule 1 makes them unnecessary. Not supported for interop.

### 7.5 What is shared

| Class | Rule | Examples |
|---|---|---|
| **Shared data** | one copy on the drive, read and written by both platforms | ROMs; BIOS files; ES-DE gamelists, collections and media; cheats; in-game saves of a system when both builds run **the same core or the same emulator**; PPSSPP `SAVEDATA` |
| **Per platform, on the drive** | each platform has its own copy in `app/` (Android) or `linux/` | save states of every emulator; Android `shared_prefs`; anything a Linux device should hand to another Linux device |
| **Per platform or per device, internal** | never on the drive from Linux | RetroArch's main config, `config/` (core options, overrides, shader presets), remaps, playlists; emulator ini files that name a renderer; shader caches, logs, controller autoconfig, hotkey bindings generated for this device's pad |

The class is a column of the emulator definition's storage block, not code. The Android move already skips caches (`cache`, `shaders`, `log` for Azahar; `cache`, `resources`, `logs` for ARMSX2).

Why so little is shared in v1, and what it would take to share more:

- **Core options and overrides.** A `.opt` file holds renderer, resolution and dynarec choices that are right for a T820 and wrong for an RK3566, and ROCKNIX's scripts rewrite them per device. Later: a one-way import of an allow-list of keys per core.
- **Save states.** Named after the ROM only; core, version and word size differ per platform. Later: a sidecar stamp per state (core library name, commit, word size) and a refusal to auto-load on mismatch.
- **File names.** FAT forbids `: * ? " < > |` and trailing dots. RetroArch names saves after the ROM file, which is already on the drive; standalone emulators that name files after a game title need checking.
- **Timestamps.** FAT stores local time at 2 s resolution; the sync already uses a content hash for that reason (P:17598).

### 7.6 Library mapping

| Item | Rule |
|---|---|
| ROM folders | ES-DE system names. All 41 `romDir`s in `mods/nano-systems.txt` already are. Aliases are read (`esdeCanonSystemName`), never written |
| `gamelist.xml` | as today: paths relative to the system folder; unknown tags kept |
| `<altemulator>` / `<alternativeEmulator>` | **A writer writes a label only if it exists in both the `android` and the `linuxarm` table for that system.** Otherwise the choice goes into `nano/emulators.json` as a family key and the gamelist label is left alone. Reason: mainline ES-DE treats an unknown label as invalid. It launches with the system's default, logs a warning, shows "INVALID ENTRY" with a "CLEAR INVALID ENTRY" action, and a bad per-game label also stops a valid system-wide one from applying (`FileData.cpp`, `GamelistFileParser.cpp`) |
| `collections/*.cfg` | `%ROMPATH%/<system>/<game>` inside the ROM folder; else the canonical `/storage/<VOLID>/...` |
| `settings/es_settings.xml` | `ROMDirectory` is written only when empty. Linux Nano reads it with the same re-rooting rule |
| `custom_systems/es_systems.xml` | Linux Nano **reads** it for a system's name, path, extensions, platform and theme (the reader the launcher plan's Phase 3 adds on both Android sides), ignores `<command>` lines it can't interpret, never rewrites an entry, and appends an entry for a system in a non-ES-DE folder in the same append-only way Android does |
| `nano/systems.json` | **Linux never writes launch fields into it.** Today it is one whole file, compared as text, and a released Android build that meets a launch tuple it doesn't know logs "this device's kept", substitutes its own and saves (P:58602). So a Linux value there would be erased on the next Android visit. Linux reads the file, maps `coreSo` and `package` to its own emulators, and stores its own per-system choice in `nano/systems.linux.json`, which Android ignores |
| `nano/emulators.json` | gains a `key` field (`core:<name>` or a family) beside the Android-only `am` string. Whether released Android builds keep an unknown field when they rewrite this file is *(guess: test before relying on it)* |
| `nano/names.json`, `index.json` | the launcher plan's change (no volume-bound paths) applies to all three |

### 7.7 Identity and `emulator_config/card.json`

Identity stays the volume id, as today.

```json
{ "writers": [
    { "device": "anbernic-rg-cube",  "platform": "android", "thimsy": "1.1",  "last": "2026-12-20" },
    { "device": "anbernic-rg-arc-d", "platform": "linux",   "thimsy": "0.1",  "last": "2026-12-22" } ],
  "emulators": { "ppsspp": { "android": "1.20.4", "linux": "1.20.4" } } }
```

It lives beside the data it describes, because ES-DE and emulator files may be on different drives. Uses: "last used in: RG Cube" in the foreign-drive dialog, and a warning when a shared emulator's data was last written by a newer version. **It is a hint, never a safety check**: no released build writes it, so after a visit to one of those it is stale. Versioning of the library itself is `nano/format.json`'s job.

### 7.8 Filesystem

- **exFAT recommended** (files over 4 GiB; what SDXC cards ship with). FAT32 accepted. NTFS, ext4, f2fs, btrfs not supported for interop.
- **Durability:** writes that matter go to a temporary name, `fsync`, then rename, then `fsync` the directory. The usb plan already lists "fsync after the ES-DE store's renames" as a fix for Android; both platforms take it.
- Mount options, checks and safe removal: section 7.3.

### 7.9 Direction of travel

| From | To | What arrives in v1 | What does not |
|---|---|---|---|
| Android Thimsy, drive | Linux Thimsy | library, favourites, collections, media; per-game emulator choices where the label is common or a family key exists; BIOS; in-game saves for systems on the same core; PPSSPP saves | save states; N64 and DS saves; RetroArch settings, core options, remaps; Android app preferences; systems with no Linux emulator on this device (kept, shown as unavailable) |
| Linux Thimsy, drive | Android Thimsy | the same. Android adopts the drive as today | the same, mirrored. Android does not use `linux/` |
| Either | Thimsy Home (APK) or mainline ES-DE | what `android_launcher_plan.md` §3 lists | - |

### 7.10 Compatibility with released Android builds

No released or current Android code reads `nano/format.json` or `card.json` (the patch has no reference to either). Version gates only bind builds that don't exist yet. What a build from before this spec does with each new thing:

| New on the drive | A pre-spec Android build |
|---|---|
| `<emu>/linux/`, `retroarch/linux/` | does not use them; its id-repair pass edits text configs in them up to three folders deep |
| `emulator_config/card.json` | ignores it and never updates it |
| `nano/systems.linux.json` | ignores it |
| a `key` field in `nano/emulators.json` | *(guess)* may drop it on rewrite |
| a drive first set up on Linux | sees `emulator_config` with files and shows "This microSD Card Has Emulator Files... from another device or an older copy" (`emuSdForeign`, P:67621), with a wholesale choice. The wording needs a Linux-aware variant in the next Android build |
| a drive with `ES-DE/` and no `format.json` | every existing card. Linux reads it as today's layout |

The first Android release that implements this spec is the compatibility floor. Below it, the rule is only "nothing is lost": Linux-only values live in files old builds ignore.

---

## 8. Installer changes

### 8.1 What exists

- The Flutter app runs the `thimsy-installer` engine as a subprocess and reads JSON Lines (`installer/docs/engine-protocol.md`). The app is never elevated.
- The engine only flashes through `spd` (`flash.rs:1428`). Packages are `pack.json` **format 1**: the manifest denies unknown fields, requires `system`, `reference_sha256` and `source_sha256` (the official firmware it starts from), and operations are a closed set.
- The catalog's `Hardware` requires `android_api`; devices are identified by USB ids or adb model.
- The design reserves an `sdcard/` crate (`universal_installer.md` §5.1), and `multi_device_plan.md` §7.3 lists an **SdCard** backend for the RG DS, RG Vita Pro, GKD 350H Ultra and the A133P devices. **The Linux images use the same backend with the simplest layout (`raw`).** Build the writer once.

### 8.2 Where the image comes from

Unlike the Android path, nothing here is a handheld maker's firmware, so Thimsy may publish a complete image (with its source, section 3.3).

| Option | Client work | Verdict |
|---|---|---|
| **A. Full image built by Thimsy, streamed to the card, boot FAT edited afterwards** | decompress, bmap, write, verify, edit FAT | **v1.** No ext4 or squashfs code on the user's machine; works on Windows without WSL |
| B. Download the upstream ROCKNIX image and swap `KERNEL` and `SYSTEM` inside its FAT | the same plus dependence on upstream's FAT size and file names | Fallback only. It mirrors the Android "official base plus package" model but buys nothing when the whole image is redistributable |
| C. Assemble on the client from parts (partition table, U-Boot blob, FAT, a prebuilt empty ext4) | `gptman`/`mbrman`, `fatfs` | Later, if bandwidth matters |
| Creating ext4 or squashfs on the client | no mature pure-Rust ext4 writer; `mke2fs` is not on Windows | **Avoid.** |

The image follows ROCKNIX's layout: a FAT32 boot partition (2048 MiB, starting at sector 32768) holding `KERNEL`, `SYSTEM` (squashfs), `extlinux/extlinux.conf`, `device_trees/` and `overlays/`; a small ext4 storage partition that grows on first boot; U-Boot written raw (RK3566: sector 64; H700: 8 KiB; S922X: sector 1). GPT on RK3566 and Qualcomm, MBR on H700 and RK3326.

Release artifacts per image: `thimsy-linux-<ver>-<image>.img.zst`, its `.bmap` (block map with per-range SHA-256), a `pack.json`, and the GPL source tarball. Split into 2 GiB parts if GitHub requires (the engine's `MultiVolume` reader handles that).

### 8.3 Catalog and package additions

**Package format.** A disk-image package does not fit format 1: it has no official base, and one image serves many devices. It needs **`pack.json` format 2**, which `multi_device_plan.md` §7.4 defines and the launcher plan's Phase 6 also extends. For this plan format 2 must allow:

- `system` and `source_sha256` optional; `reference_sha256` is the image's hash;
- a binding to a list of devices or an image id instead of one `device`, and no `profile` binding;
- `min_installer`, so a format-1 installer refuses by format number with a clear message;
- new operations, same closed-set rule:
  - `disk_image { artifact, bmap, image_size, image_sha256 }`
  - `fat_write { partition, path, artifact }` and `fat_template { partition, path, template }`
  - `fat_copy { partition, from, to }` (copy a file already in the FAT: an H700 panel's DTB to `dtb.img`)
  - `require_card { min_bytes }`

**Catalog.** In the schema `multi_device_plan.md` §4.2 and §7.1 introduce (today's keys are `identify`, `soc` and a required `hardware.android_api`, so `catalog.rs` changes first):

```yaml
id: anbernic-rg-arc-d
platform: linux                      # new: android (default) | linux
soc_family: rk3566
hardware: { arch: aarch64, ram_gb: 2, gpu_family: mali-bifrost }   # android_api becomes optional
match: { dt_model: "Anbernic RG ARC-D" }
controls: { face_layout: saturn6, sticks: none, input_stack: inputplumber }
flash:
  method: sdcard
  layout: raw                        # multi-device plan's enum: raw | rk_update_card | rk_boot_card | phoenixcard
  image: thimsy-linux-rk3566-generic
  boot_partition: 1
  fdt: auto                          # auto (U-Boot picks) | ask (variants)
  min_bytes: 8000000000
preconditions: [ erase_internal_android ]    # ARC-D, RG353P/M/V (section 8.6)
variants: []                         # H700: ddr (image choice), panel (dtb.img)
profiles: { recommended: small, allowed: [small, medium] }
support: { install: { windows: automated, macos: automated, linux: automated } }
```

### 8.4 Per-device and per-profile builds

- **Per device.** One image per SoC family where ROCKNIX has one. The device choice becomes small files on the boot FAT: the FDT line or `dtb.img`, and `thimsy/device.json` (the descriptor's id).
- **H700 is the exception.** DDR3 and DDR4 units need different SPLs, written raw at 8 KiB, so they are **two images**, chosen by a catalog variant (a typed question with pictures). The panel revision is a second question, answered by `fat_copy` of one DTB to `dtb.img`. A "change panel choice" flow rewrites only `dtb.img` on an existing Thimsy card. Decision 13 asks whether to add a first-boot panel chooser.
- **Per package profile.** `device_profiles_plan.md` resolves a package per (device, profile) at build time and says "the profile decides what ships". v1 departs from that in one way: each image's `SYSTEM` contains the superset its family can run, the resolver runs **at image build time** and writes one resolved manifest per (device, profile) into `/usr/share/thimsy/`, and first boot picks one by `thimsy/device.json` and `thimsy/profile.json`. So the profile enables and sets defaults; it does not shrink the download. On RK3566 and H700 the difference would be small *(guess: a few hundred MB)*. If that changes on Qualcomm devices, optional packs become extra squashfs files on the FAT.
- **Seeding.** Pack-in games and their gamelists (`bundled_roms.py`, after the changes in section 4.2) go into the image's internal `ROMs` and `ES-DE`, as the Android build seeds `/sdcard`.
- **Game card (optional step).** "Prepare a game card" formats a second card exFAT with the OS's own formatter, gives it a non-zero serial, and creates `ROMs/` and `bios/`. The handheld can also do this.

### 8.5 The raw disk writer

A new crate `installer/engine/crates/thimsy-disk` (enumerate, open, lock, aligned I/O, eject) and a module `thimsy-engine/src/sdcard/` (source sniffing, bmap, write, FAT edit, verify). Closest existing Rust template: BeagleBoard's `bb-imager-rs` (MIT). Reference behaviour: Raspberry Pi Imager.

**Which drives are offered** (enforced in the engine; the app only displays):

| Rule | Detail |
|---|---|
| Each disk is classified | **card** (SD or MMC bus, or USB with removable media), **USB flash**, or **USB fixed disk** (external hard disks and SSDs) |
| Cards and USB flash are offered | Windows: SetupDi disk enumeration, `IOCTL_STORAGE_QUERY_PROPERTY`. macOS: every physical whole disk (`diskutil list -plist physical`, then `diskutil info -plist`); external disks, and internal disks only when they are an SD reader (a MacBook's built-in slot reports as internal; the field to test is *(guess: `Protocol` = Secure Digital; settle on a Mac)*). Linux: UDisks2, with `lsblk` and `/sys/block/*/device/type` as fallback |
| USB fixed disks always need a typed confirmation, whatever their size | they are the likeliest wrong target and usually hold backups |
| System disks are never offered | Windows: any disk holding `%SystemRoot%`, the EFI system partition, a page or hibernation file, a spanned or dynamic volume. macOS: internal and not a card; any disk backing the running system. Linux: every disk reached from a mounted filesystem or swap through `/sys/block/*/holders` and `slaves` (dm, md, LUKS, bcache, loop backing files), btrfs and ZFS members, anything in `/etc/fstab` or `crypttab`, and the stick a live session booted from |
| "Show all drives" (advanced) adds other buses | it never adds a system disk |
| Virtual disks are hidden | VHD, Storage Spaces, loop devices (shown only under a `test-hooks` build feature) |
| Read-only is shown, disabled | "unlock the SD card switch" |
| Too small is refused | |
| The user confirms model, size and current volume labels | the approval is bound to a **fingerprint** (path, bus, vendor, model, serial, size, sector size, partition-table id, partition offsets) |
| The process holding the privileged handle re-checks | it recomputes the fingerprint **from that handle**, re-applies these rules, and verifies the package hash itself before the first write. A mismatch fails with `drive_changed` |

**Elevation.** The Flutter app is never elevated.

| OS | Unmount and lock | Open | Prompt |
|---|---|---|---|
| Windows | per volume: `FSCTL_LOCK_VOLUME` (retry), `FSCTL_DISMOUNT_VOLUME`, for the prepare step; `IOCTL_DISK_DELETE_DRIVE_LAYOUT`; then the open physical-drive handle itself keeps Windows from re-scanning partitions until it closes | `\\.\PhysicalDriveN` with `FILE_FLAG_NO_BUFFERING | WRITE_THROUGH`, in a small `thimsy-installer disk-broker` started with the `runas` verb by a new `elevate` module. (The existing code in `drivers.rs` launches a hidden PowerShell and inherits no handles; it is not reusable as is.) The broker ties its life to the engine by watching the parent process and the pipe, runs the write itself, and streams events over a named pipe | one UAC prompt |
| macOS | `diskutil unmountDisk`; a mount-approval veto or immediate unmount so nothing mounts the new FAT afterwards | `/usr/libexec/authopen -stdoutpipe -extauth -o 2 /dev/rdiskN`, file descriptor received over `SCM_RIGHTS`, then `F_NOCACHE`. No privileged helper daemon | one authorization dialog |
| Linux | UDisks2 `Filesystem.Unmount` | UDisks2 `Block.OpenDevice` with `O_EXCL | O_DIRECT` (works inside Flatpak with `--system-talk-name=org.freedesktop.UDisks2`). Fallback: `pkexec` running the broker | one polkit prompt |

**Write order** (Raspberry Pi Imager's, with one addition):

1. **Capacity probe.** Write a random block at the last MiB and at offsets spread over the reported capacity, flush, read them back uncached, zero them. Every end-of-device write has a timeout. A mismatch, a wrap-around or a timeout fails with `drive_fake_capacity` and nothing else is written. Counterfeit cards are common, and they pass a read-back of the first few GB.
2. Zero the first and last 1 MiB (old partition tables, including the backup GPT).
3. Write the bmap's mapped ranges, **holding back the first 1 MiB** in memory. Aligned 4 MiB buffers; decompression, hashing and writing on separate threads.
4. Flush.
5. **Verify** every mapped range against the bmap's per-range SHA-256, read back with the cache bypassed. A range that overlaps the first MiB (H700's SPL at 8 KiB runs past it) is checked from the held-back buffer for that part.
6. **Then** edit the boot FAT in place through the same exclusive handle (pure-Rust `fatfs`; the partition's offset comes from the held-back first MiB, not from the card). The bmap check never runs after this step, because the edit changes the FAT's blocks.
7. Flush; re-read each edited FAT file through an uncached view and compare size and hash.
8. Write the held-back first 1 MiB (partition table, start of the bootloader), flush, verify it.
9. Eject through the OS before releasing the handle where possible (`diskutil eject`; UDisks2 `Drive.Eject`). On Windows, eject at once and tell the user to ignore any "format this disk" prompt for the ext4 partition.

Cancelling at any point leaves a card with no partition table, never a half-bootable one. Verification is on by default, with an advanced "skip" switch. A `verify_failed` message says the card may be failing or write-protected.

**Sources and formats:** sniff by magic bytes, not extension: zstd (Thimsy's images), gzip (ROCKNIX), xz (others). Write from the verified cache, never straight from the network.

**USB drives.** The same writer. No RK3566 or H700 handheld boots from USB by default, so this matters for x86 handhelds and QEMU targets, and for SD cards in USB readers.

### 8.6 Flows and other changes

- **Wizard:** for `method: sdcard` the steps "unlock" and "connect" are replaced by "choose card" (`home_shell.dart`'s step list). New `drive_screen.dart` with a live list (`drives --json --watch`).
- **Protocol:** `drives`, `write --drive PATH --fingerprint FP`; stages `check`, `unmount`, `elevate`, `probe`, `write`, `verify`, `customize`, `eject`; errors `drive_changed`, `drive_protected`, `drive_too_small`, `drive_busy`, `drive_fake_capacity`, `elevation_declined`, `write_failed`, `verify_failed`.
- **ARC-D and RG353P/M/V: erasing Android.** The card boots only after the eMMC is zeroed over adb from recovery (ROCKNIX's steps: back up with `adb pull /dev/block/mmcblk0`, then `dd if=/dev/zero of=/dev/block/mmcblk0`). In v1 the installer shows this as a guided manual step with a typed confirmation and a warning that restoring the backup is unproven. Automating it needs the `adb` module the launcher plan's Phase 6 brings. The ARC-S, RG353VS/PS and RG503 need none of this.
- **Qualcomm devices (later):** OpenABL must be flashed from Android first; the same `adb` and fastboot work.
- **Tests:** a regular file as a disk under `test-hooks`; loop devices in Linux CI, including a wrap-around device-mapper target for the capacity probe; `hdiutil` images on macOS; a mounted VHD on Windows (which must be refused as virtual without `test-hooks`); fuzzing of the bmap parser and the format sniffer.

---

## 9. Boot-time plan

### 9.1 "Paper Plane"

**No project of that name was found.** Searched: the web (standard and extended), GitHub repository search (about 740 `paperplane` repositories), code and issue search in the spruce, NextUI and ROCKNIX organisations, the release code names of muOS, Knulli and spruce, and every document, release note, commit message and author profile of the two projects below. Reddit, Discord and two wikis could not be queried. The name collides only with unrelated projects. **Open question 1 asks the owner for a link.** It may be Discord-only, private, or a misremembered name; the description fits BaseOS and FlipBase closely *(guess)*.

The plan takes its techniques from the projects that do publish very fast boots:

| Project | What it is | Published boot | Licence |
|---|---|---|---|
| **FlipBase** 2.0.0 (tag) | A minimal base for the Miyoo Flip (**RK3566, the ARC's SoC**): mainline U-Boot 2026.10 and Linux 7.3.5, Panfrost or libmali, Buildroot, EROFS root, no initramfs, BusyBox init, no udev. It mounts the user's card and hands off to the frontend there (NextUI, spruce) | From SoC reset. Warm reboots: U-Boot done 0.65 s, `/init` 1.1 s, logo 1.2 s, **init execs the frontend's launcher at 1.3 s**, `nextui.elf` starts at 1.9 s. Cold boots: launcher 2.0 s, `nextui.elf` 2.4 s, first NextUI frame 2.9 to 3.0 s. A filmed cold boot (before the last fixes) showed the menu 3.97 s after the power LED. Stock: 15.8 s to the launcher | MIT |
| **BaseOS** 1.5 (pre-release) | The H700 base FlipBase was forked from; spruce and NextUI use it on TF1. **Vendor 4.9 kernel and Mali blob, kept byte-identical**; the gains are in the boot chain and init | to frontend hand-off: 6.7 s (v1.0) → 3.16 s (v1.5.1) | **no licence file** |
| **RetroStoneOS** | Buildroot, mainline 6.18, one C frontend, libretro cores, no X or Wayland; Allwinner A20 | power-on to logo 2.09 s (SPL 468 ms, U-Boot 240, kernel read 231, LZ4 188, kernel 427, init to frontend 435); menu about 0.3 s later | code MIT; images non-commercial |

All three get there the same way: they own the whole stack from the SPL to one frontend process, with BusyBox init and no service manager. FlipBase matters most here. It shows what the ARC's SoC can do on mainline: **a usable menu about 3 to 3.5 s after power-on**. That is the floor for any design, and a systemd-based image will not reach it.

### 9.2 Where ROCKNIX spends time

No published figure exists. *(guess: 10 to 20 s to the frontend on RK3566.)* The path, read from the source:

1. SPL and U-Boot; kernel plus a BusyBox initramfs.
2. The initramfs mounts the FAT, loop-mounts the `SYSTEM` squashfs, optionally runs fsck, shows a splash, and `switch_root`s to systemd.
3. systemd reaches `multi-user` and `graphical`.
4. `rocknix-autostart` runs, **after `network.target`**. It is one bash script that runs, in series: platform quirks, device quirks, the card automount, common start scripts (including `003-gpudriver` and `111-sway-init`), device scripts.
5. Only then: `systemctl start sway.service essway.service`.

So the frontend is the last thing started, behind the network and every quirk script. The kernel command line already has `quiet`, and also `console=ttyS2,1500000` and `systemd.debug_shell=ttyS2` (a serial console and a debug shell at 1.5 Mbaud). The first boot also resizes storage and reboots.

**Step one is measurement, not tuning:** U-Boot `bootstage`, `initcall_debug` in a development build, `systemd-analyze`, and a mark when Nano presents its first frame, in one per-boot log. Times are read from the SoC's architected counter against `CLOCK_MONOTONIC_RAW` (FlipBase's method), so they start at reset and not at kernel start.

### 9.3 Techniques, and which apply

Gains in the "measured" column are the source project's own numbers on its own board. They show where time hides; they are not promises for the ARC.

| Technique | Source | Applies to a ROCKNIX-derived build? | Measured or expected gain | Cost |
|---|---|---|---|---|
| **Frontend first:** split `autostart` so the GPU driver, device config and Sway's set-up run first, then Sway and Nano, then the network, the remaining quirks, Samba, SSH and the ROM scan | RetroStoneOS, FlipBase | yes, by rewriting the autostart script and the order of its units | the largest gain available on this base *(guess: several seconds)* | medium; Nano must tolerate a late drive and late network |
| **First frame from a snapshot:** draw the home from a cached state, load lists in the background | RetroStoneOS ("the menu is drawn from a snapshot") | yes; fits Nano's existing state files | about 0.3 s and no "empty" flash | medium |
| Serial console and debug shell off in release builds | BaseOS (serial off: about 0.7 s on H700, in U-Boot) | yes: two kernel parameters and a U-Boot option | measure | low; keep them in debug builds |
| U-Boot: `bootdelay=0`, no video/USB/network probing | RetroStoneOS, Bootlin | yes on RK3566 and H700; not on Qualcomm (ABL) | 0.1 to 0.3 s | low |
| **RK3566 U-Boot fixes:** SD clock at 50 MHz instead of 25 (a mainline RK3568 bug); data cache on before relocation (570 → 40 ms); `blkcache` sizing (202 → 53 ms); CPU at full clock during decompression; decompressor built at `-O2` without `-mstrict-align` (336 → 107 ms) | FlipBase (`docs/uboot.md`) | yes: same SoC, mainline U-Boot. Patches to carry, or to send upstream | about 1 s in total on the Flip | low to medium |
| Device tree: PMIC I2C at 400 kHz; panel init delays audited; display commit at driver bind so the panel powers up during kernel init | FlipBase | yes, per board | 0.1 to 0.3 s | low to medium |
| SD slot at SDR104 | FlipBase (1.06 s on the Flip) | **no gain: mainline already enables it for the ARC** | - | - |
| Kernel compression by benchmark (LZ4 or zstd against gzip) | RetroStoneOS, BaseOS, FlipBase, Bootlin | yes | 0.1 to 0.5 s | trivial |
| System image compression: LZ4HC rather than zstd | BaseOS (0.7 s earlier first frame on a 20 MB/s card), FlipBase | yes: one setting; the image grows | measure | trivial |
| Trimmed per-SoC kernel config, no tracing; Wi-Fi, Bluetooth and USB host as modules loaded after the frontend | Bootlin (about 0.8 s on a slow board), FlipBase, RetroStoneOS | yes, as a config fragment per SoC family | 0.5 to 1 s | medium: diverges from ROCKNIX's configs |
| `driver_async_probe=` for UART, USB, touch, PMIC | RetroStoneOS | yes | tens to hundreds of ms | low |
| fsck the data drive only when the dirty flag is set | RetroStoneOS | yes; section 7.3 | avoids seconds after an unclean removal | low |
| Read the GPU library ahead at idle priority; `performance` governor until the menu is up | FlipBase (about 0.45 s to first frame), RetroStoneOS | yes | 0.1 to 0.5 s | trivial |
| Mesa shader cache on the writable partition, warmed at first boot | Mesa docs | yes | first-frame time on first run | low |
| Nano draws its own splash early; no U-Boot logo | RetroStoneOS (a U-Boot logo blanks when the DRM driver binds) | yes | perceived only | low |
| `quiet` | Bootlin | **already set by ROCKNIX** | - | - |
| Kernel in a raw FIT read by partition name; no initramfs; system image on its own partition | FlipBase, RetroStoneOS, BaseOS (0.5 s) | only with a layout change: ROCKNIX's initramfs loop-mounts `SYSTEM` from the FAT, applies updates and resizes storage | 0.1 to 0.5 s | medium to high |
| No compositor for the home screen | section 5.3 | not with a resident Nano and a layer-shell Quick Menu; only with our own compositor | *(guess: 0.5 to 2 s)* | high |
| BusyBox init instead of systemd, no udev | all three; their largest saving (9.9 s of stock userland on the Flip) | **no, not on this base** | seconds | very high (section 9.5) |
| U-Boot Falcon mode | Bootlin (about 0.5 s) | possible *(guess)* on RK3566/H700; never on Qualcomm | 0.1 to 0.5 s | high. Defer |
| Kernel parameters `rcu_expedited`, `nokaslr` | BaseOS (0.2 s on the vendor kernel); FlipBase measured no gain on mainline | no | none | - |
| Vendor-kernel tricks (U-Boot gzip selector patch, boot0 debug byte, late USB module) | BaseOS, FlipBase's vendor-kernel era | no | - | - |
| Hibernation or snapshot resume | Microchip | no: restoring 1 to 2 GB from SD is likely slower than a short cold boot *(guess)* | - | high |
| Resume the last game at boot from a save state | RetroStoneOS | yes (it is Quick Resume) | "time to playing" | later |

### 9.4 Targets

No absolute number is honest before a measurement. The metric is **power-on to Nano's first complete home frame, cold boot**, the same event FlipBase films.

| Stage | Target | How |
|---|---|---|
| Phase 0 | the baseline: stock ROCKNIX on the ARC, power-on to EmulationStation | instrumentation only |
| Alpha | no slower than the baseline | Nano in place of EmulationStation |
| v1 | the baseline minus the frontend-first and snapshot gains. *(guess, a placeholder until Phase 0: under 8 s)* | autostart split, snapshot frame, serial console off, U-Boot options, dirty-only fsck |
| After v1 | *(guess: about 5 s)* | U-Boot fixes, trimmed kernel, LZ4HC, read-ahead |
| Floor | about 3 to 3.5 s, FlipBase's figure with no systemd, no initramfs and no compositor | not reachable on this base |

### 9.5 If a 3-second boot is a requirement

Then the base is wrong, and decision 1 has to be reopened rather than patched. A "Thimsy Base" in the FlipBase mould (a Buildroot external tree, MIT, mainline, EROFS, BusyBox init) could reuse Nano's shared code, a KMS/GBM display target and the card contract. It would need:

- a second Linux platform backend: network, audio, power, time and update with no systemd, D-Bus or PipeWire;
- its own launcher in place of `runemu.sh`, and its own per-device quirks;
- U-Boot, device-tree and input bring-up per board (FlipBase supports one; a fork exists for the RG353PS).

That is section 3's option (d). Size: large per board family *(guess)*; not scheduled. Decision 5.

---

## 10. Build system and build host

Host: Ryzen 9 5900HX (8 cores, 16 threads), 64 GB RAM, Windows 11, Docker Desktop. The C: drive had about 300 GB free on 2026-11-24.

| Build | Needs | On this host |
|---|---|---|
| ROCKNIX, one device family | Ubuntu 22.04 container (`ghcr.io/rocknix/rocknix-build`), **200 GB free for one device** (1 TB for all), "on the order of ten hours" first build, minutes afterwards | Fits, **but 200 GB of the 300 GB free is tight** beside the Android work volume. Plan for an external SSD or a dedicated virtual disk (open question 6) |
| Smaller builds | `ENABLE_32BIT=false` skips the 32-bit userland (and the 32-bit cores). `BASE_ONLY=true` builds a bare system: Sway, no emulators, no EmulationStation, no audio stack | `BASE_ONLY` is the right base for the Phase 0 "Nano under Sway" spike |
| RAM | not stated by ROCKNIX *(guess: 16 GB+ is comfortable at 16 threads)* | 64 GB is ample. Give WSL 48 GB and 16 processors in `.wslconfig` |
| Where the tree lives | **Inside the WSL2 ext4 filesystem**, never under `/mnt/c`: the kernel tree has file names that differ only by case, GammaOS's tree has names Windows can't store, and the Windows mount is slow | `make docker-RK3566` run from a WSL shell; Docker Desktop's WSL2 backend works. ROCKNIX calls WSL reports "not definitive", so Phase 0 proves it |
| Disk image growth | the WSL `ext4.vhdx` grows and does not shrink by itself; sparse VHD is experimental and has had data-corruption reports | keep the build tree on its own virtual disk and delete it to reclaim space |
| Nano alone | CMake, a cross toolchain and the ROCKNIX sysroot | Minutes. Build out of tree, copy to the device over SSH. Nano's Wayland target also runs in WSLg, so most UI work needs no device *(guess: GLES through WSLg is good enough)* |
| Android firmware build | unchanged: about 40 GB, privileged container | unchanged |
| Installer | Rust, Flutter 3.50 | unchanged; macOS writer testing needs a Mac (open question 7) |

Recurring cost: a monthly rebase onto a ROCKNIX stable tag means re-applying the patch series and a rebuild of what changed *(guess: 2 to 3 days a month)*.

Release builds: the same scripts, on this machine first. A GitHub-hosted runner can't hold a 200 GB build *(guess)*; a self-hosted runner is the later answer.

---

## 11. Testing and hardware

### 11.1 Hardware

| Item | Why | Needed for |
|---|---|---|
| **RG ARC-S**, or the owner's **RG ARC-D with Android erased** | first device. The ARC-S needs no erase and is the 1 GB floor. An erased ARC-D can't be used for Android comparisons unless the eMMC backup restores | Phase 0 |
| A second RK3566 device with sticks, ideally a 1:1 screen (RGB30) or the RG353VS | the ARC has no sticks; layouts beyond 4:3 | Phase 7 |
| An H700 device (RG35XX H or Plus), ideally two panel revisions | second family; panel and DDR selection | Phase 7 |
| The RG Cube (owned) | the Android side of every interop test | Phase 3 |
| Cards: exFAT 64 GB and 128 GB, FAT32 32 GB, one known counterfeit; two USB readers | filesystem matrix; writer tests | Phase 3, 4 |
| USB serial adapter (ROCKNIX's RK3566 console is `ttyS2` at 1.5 Mbaud; whether the ARC board exposes it is unknown) | U-Boot and early kernel timing | Phase 6 |
| A Mac | macOS writer | Phase 4 |

### 11.2 Tests

| Layer | Test |
|---|---|
| Nano shared code | the existing host unit tests (29 production files), under CTest, on Linux and in the Android host build; sanitizers on |
| Platform seam | a fake platform (in memory) so menus, the store and the sync can be driven without a device; golden screenshots at 640x480, 720x720 and 1280x720 |
| Card spec | **conformance vectors**: sample drives (as directory trees and as exFAT and FAT32 images, including a FAT32 one with non-ASCII file names and one whose only definition of a system is in `custom_systems`) written by each platform; each implementation must read the others' and round-trip without changing foreign entries |
| Interop, on devices | the matrix below |
| Image | a smoke script over SSH on real devices; QEMU in CI if a virtio-gpu target can be made to boot the image *(guess)* |
| Installer | section 8.6 |
| Boot time | the per-boot log, compared against the previous release; a regression of more than 10% fails the release check |
| Golden | the RG Cube Android image stays byte-identical through every refactor phase |

Interop matrix (each row on exFAT and FAT32):

| # | Scenario | Pass means |
|---|---|---|
| 1 | Drive made on the Cube (ES-DE and emulator files moved), put in the ARC | library, favourites, collections, covers show; a `.srm` from a system on the same core and a PSP save load; nothing under `app/`, `retroarch/config/` or `custom_systems/` changed |
| 2 | Play on the ARC, return the drive to the Cube | new saves load; play counts and last-played merged; the Cube's own states untouched |
| 3 | Drive made on the ARC, put in a Cube that has its own internal library | the foreign-drive dialog; "Use the Card's" adopts it |
| 4 | Per-game emulator set to M64Plus FZ on the Cube | the ARC launches the same family; the label on the drive is unchanged; N64 saves are stated as not shared |
| 5 | A system with no Linux emulator (Xbox) on the drive | shown as unavailable on the ARC; entry kept |
| 6 | Drive pulled during a write, on each platform | no corrupted gamelist; the other platform mounts it after its check |
| 7 | Drive out, then launch each emulator on the ARC | nothing is written under `/storage/<VOLID>`; the game uses the fall-back or refuses |
| 8 | A system whose default core differs between the builds | saves do not overwrite each other |
| 9 | Drive cloned to a larger card; both inserted | the first is used, the second refused with a message |
| 10 | A released (pre-spec) Android build visits the drive | nothing Linux wrote is lost (section 7.10) |
| 11 | Mainline ES-DE (Android, and aarch64 Linux) pointed at the drive | ES-DE reads everything it has a field for; no "INVALID ENTRY" for a label Thimsy wrote |

---

## 12. Phased roadmap

Sizes are person-weeks of one full-time developer, all *(guesses)*.

**First target: the RG ARC pair on the RK3566 Generic image**, because:

- The ARC-D is the device the owner named.
- ROCKNIX supports both on a mainline kernel, and their device trees are in mainline Linux.
- The same image covers the RG353 family, RG503 and the Powkiddy RGB30 family.
- Two card slots: OS on one, the shared drive on the other. That is exactly the interop setup.
- Their limits are useful early: a portrait-native panel, a six-button pad with no sticks, InputPlumber, and 1 GB on the ARC-S.
- A bad Thimsy card can't brick either: with nothing bootable on eMMC, the boot ROM falls through to the SD card.

Against it: the ARC-D needs Android erased first (section 8.6). If the owner wants to keep Android on that unit, an ARC-S or an RG353VS is the unit to buy.

| Phase | Work | Done when | Weeks |
|---|---|---|---|
| **0. Spikes** (no product code) | (a) Stock ROCKNIX on the ARC: the boot timeline; the v1 emulator set on Panfrost (`gpudriver panfrost`); silencing the pad for RetroArch and for PPSSPP. (b) Build unmodified ROCKNIX RK3566, then `BASE_ONLY`, in WSL2. (c) Build Nano outside AOSP from `tools/android-launcher/fetch_nano.sh` (shared with the launcher plan's Phase 0). (d) Mount a Cube-made card at `/storage/<VOLID>` on ROCKNIX and load a save. (e) Windows raw-write spike: the broker, lock, write, verify. (f) The owner's "Paper Plane" link | each guess in sections 5.3, 7.3, 8.5 and 9 is confirmed or replaced; a go/no-go on the base | 2 to 4 |
| **1. The seam** (Android behaviour unchanged) | `NanoPlatform.h`, `compat/` shims, CMake, the config store interface. This is the launcher plan's Phase 1; do it once | the Cube image is byte-identical; Nano's shared code builds and passes its unit tests on a Linux host | 6 to 10 |
| **2. Linux alpha on the ARC** | `thimsy-os-linux` with the THIMSY distro; `platform/linux` (Wayland display, the ARC pad model, config store, launch through the patched `runemu.sh`, system actions); `thimsy-session` for mounting, launch hand-off, volume and power keys; `thimsy-fonts`; XMB and the ES-DE theme; store mode on internal storage; RetroArch cores; boot instrumentation and the autostart split | **A public alpha as a plain `.img.gz`**, written with any imaging tool: boots to Nano, launches and exits a game on every alpha system, sleeps and wakes; boot time recorded | 8 to 12 |
| **3. Card interop v1** | `LIBRARY-FORMAT.md` (with the launcher plan); the drive model; `/storage/<VOLID>` mounting with the immutable mount point; Linux adoption, links and re-pointing; the per-system core table; emulator definitions schema 2 with Linux blocks, families and trust rules; `systems.linux.json`; `card.json`; the Android-side changes (both label tables, the label-writing rule, not overwriting unknown launch tuples, the dialog wording) | interop matrix rows 1 to 11 pass between the Cube and the ARC | 6 to 10 |
| **4. Installer** | `thimsy-disk`, the `sdcard` module, `pack.json` format 2 and catalog changes (with the multi-device plan), the card picker, the erase-Android guide, release packaging with bmap and source tarballs | a user on Windows, macOS and Linux goes from "pick RG ARC-S" to a booted device; the wrong-drive and fake-card tests pass | 6 to 10 |
| **Linux v1 release** | hotkeys for RetroArch and "Close Current App"; scraper, BIOS manager, File Explorer, setup wizard, update file; PPSSPP and ScummVM; 21 languages checked on 640x480 | first public release for the RK3566 Generic boards that were tested | 3 to 5 |
| **5. Over a game** | the Quick Menu as a layer surface; button map overlay; hotkey provisioning for standalone emulators | the Quick Menu opens over RetroArch and each standalone emulator, and the game sees no input meanwhile | 6 to 10 |
| **6. Boot time** | the rest of section 9.3; decision 5's outcome | the after-v1 target | 3 to 6 |
| **7. More devices** | qualify RG353, RG503, RGB30 (1:1, sticks); then H700 with its two images and the panel question | each device passes the smoke script and the layout check | 2 to 4 each family |
| **8. Later** | Qualcomm (OpenABL step, heavy emulators, Turnip); photos, music, video on FFmpeg; Quick Resume; optional real ES-DE; network shares and Syncthing menus; RK3326 | - | - |

**To the alpha: 16 to 26 weeks. To v1: 31 to 51 weeks**, plus the monthly rebase, for a developer who also ships the Android build and has four other plans open. The alpha does not need the installer or the card contract, so phases 3 and 4 are not on its path; they can run in either order or overlap.

Dependencies outside this plan: section 4.5.

---

## 13. Risks

1. **The Nano port is large, and it pulls against upstream.** 204k lines hang off one class; 110 files touch Android. Every seam cut is a diff against GammaOS's Nano, which Thimsy still rebases onto (`BACKPORTS.md`). *Mitigation:* shims before interfaces; land the seam once for the launcher and Linux plans; keep the Cube byte-identical; consider proposing the seam upstream.
2. **Input and the Quick Menu have no proven design on Linux.** Two input stacks; a virtual pad that SDL reads behind an evdev grab; no compositor layer without Sway. *Mitigation:* the Phase 0 spike; v1 ships without the Quick Menu.
3. **Dependence on ROCKNIX.** About a dozen patch points, some of them in scripts that change often (`runemu.sh`, `setsettings.sh`, `autostart`); a first build of ten hours and 200 GB; a community that may not welcome a rebrand. *Mitigation:* pin stable tags, rebase monthly behind conformance tests, replace all branding, upstream generic fixes, keep Batocera as the documented fallback.
4. **Card interop can lose data.** Two platforms write the same folders on a filesystem with no journal; released Android builds overwrite what they don't know; an empty mount point looks like a drive; cards get pulled. *Mitigation:* v1 shares little; Linux-only values live in files old builds ignore; the immutable mount point and the launch check; write-then-rename with fsync; the on-device matrix.
5. **Raw disk writing can destroy the wrong drive.** Three operating systems, three elevation models, internal SD readers that look like system disks, external SSDs that look like cards, counterfeit cards. *Mitigation:* the engine's classification and system-disk rules, fingerprint re-checks in the privileged process, the capacity probe, first-megabyte-last ordering, read-back verification, and tests that prove system and virtual disks are refused.
6. **Erasing Android on the ARC-D is irreversible in practice.** The backup's restore path is unproven, and "legacy bootloader" units can't run the image at all. *Mitigation:* a typed confirmation; the ARC-S as the reference unit; say so in the device list.
7. **"Paper Plane" is unidentified, and a 3 s boot does not fit a systemd-based distribution.** *Mitigation:* open question 1; a measured baseline before any number; decision 5.
8. **One developer, five plans** that share foundations. *Mitigation:* section 4.5 names an owner for each shared piece; decision 7 picks the order.
9. **Panfrost against libmali.** Panfrost-only is cleaner but is not what ROCKNIX tests on RK3566, has GLES 3.1 instead of 3.2, and no Vulkan on H700. *Mitigation:* the Phase 0 qualification pass; decision 11 made on its result.
10. **Hardware variety inside "one image".** H700 panel revisions and DDR types that can't be detected; RGB30 v1 and v2. *Mitigation:* installer questions and recovery guides; a tested-devices list rather than a supported-SoC claim.
11. **Licence loose ends in Nano itself** (GPL headers on two files, liba52). They matter more once Nano ships outside GammaOS. *Mitigation:* the audit `plan_remove_unlicensed_files.md` and the launcher plan's Phase 0 already call for.

---

## 14. Decisions for the owner

1. **Base.** A pinned downstream of ROCKNIX (recommended), or our own Buildroot for a smaller, faster image at the cost of every device quirk?
2. **First device.** Erase Android on the ARC-D (after a backup), or buy an ARC-S or RG353VS as the reference unit and leave the ARC-D alone?
3. **Scope against the Android plan.** If the Linux build covers RK3566, does `multi_device_plan.md` drop its tier-5 Android work for the same devices?
4. **Display stack.** Nano as a Wayland client under Sway (recommended), or our own compositor from the start?
5. **Boot target.** Is "the measured baseline minus the cheap wins at v1, about 5 s later" acceptable, or is a 3 s class boot a requirement (which reopens decision 1)? And which project is "Paper Plane"?
6. **Repositories and licences.** A new GPL-2.0 `thimsy-os-linux` repository, with Nano and the shared lists staying Apache-2.0 here?
7. **The shared foundations.** In which order do the five plans go (section 4.5)? And later, does the extracted Nano tree become the source of truth instead of the patch?
8. **The card contract.** Accept `/storage/<VOLID>` as the canonical path on Linux; the small v1 sharing set (library, BIOS, same-core saves; no shared states or settings); `linux/` folders, `systems.linux.json` and `card.json`; exFAT recommended?
9. **Real ES-DE on Linux.** Ship a self-built ES-DE as an optional second frontend, or Nano's ES-DE theme only (recommended for v1)?
10. **Feature cut.** Accept an alpha with RetroArch only and internal storage, and a v1 without the Quick Menu over a game, Quick Resume, the DS player and the media apps (section 5.4)?
11. **GPU driver.** Panfrost only (no blob, no EULA, but re-qualified), or ROCKNIX's default libmali on RK3566?
12. **Package profiles on Linux.** RK3566 and H700 as Small, Medium offered on 2 GB devices; should Small on Linux gain Flycast (a change to `device_profiles_plan.md`)?
13. **Image distribution.** Thimsy publishes complete images with source tarballs (recommended), or the installer rebuilds from the ROCKNIX release on the user's machine?
14. **H700 panel choice.** Ask in the installer (recommended first), or build a first-boot chooser that cycles device trees until the user confirms a picture?
15. **Hardware.** Which of the devices in section 11.1 to buy, and is a Mac available for the writer?

---

## 15. Open questions

1. What and where is "Paper Plane"? A link, a Discord, a device it runs on.
2. Which ARC does the owner have, and if an ARC-D: is it a "legacy bootloader" unit, and may Android be erased?
3. How do ROCKNIX's maintainers see a renamed downstream? Ask before the first public image. (Their licence text is settled: section 3.3.)
4. Does InputPlumber's intercept mode silence both RetroArch and SDL emulators on the ARC, including the held-button case?
5. Which test devices and cards exist already.
6. Where the 200 GB build tree goes (free space on C: is about 300 GB).
7. Does restoring an `adb pull` eMMC backup bring an ARC-D's Android back?
8. Does Android (LineageOS 21 vold) mount an exFAT data partition that is the third partition of a boot card?
9. Do released Android builds keep an unknown field in `nano/emulators.json` when they rewrite it?
10. Which default cores can match across the builds on RK3566, and which pairs are save-compatible (PCSX ReARMed and Beetle PSX memory cards; gpSP and mGBA)?
11. Do save states from the Android buildbot cores load in ROCKNIX's source-built cores of the same commit? (Needed only when states are shared later.)
12. How much does Panfrost cost against libmali on the v1 emulator set?
13. Does the ARC board expose `ttyS2`, and what does U-Boot's `bootstage` report on it?
14. RG ARC HDMI under ROCKNIX: mirrored by Sway by default, and at what modes?
15. Do any of GammaOS's own plans (a Linux Nano, "GCore+" Android 14 for RK3566) overlap with this? `multi_device_plan.md` question 9 asks the same.
16. The macOS `diskutil` field that marks a built-in SD reader.

---

## 16. Sources

**ROCKNIX**
- https://github.com/ROCKNIX/distribution (`next`): `LICENSE.md`, `README.md`, `config/options`, `Makefile`, `scripts/mkimage`, `scripts/image`, `projects/ROCKNIX/config.xml`, `devices/{RK3566,H700}/options`, `packages/linux/package.mk`, `packages/virtual/image/package.mk`, `packages/virtual/emulators/package.mk`, `packages/sysutils/busybox/scripts/init`, `packages/rocknix/sources/scripts/{runemu.sh,setsettings.sh,automount,autostart,input_sense,wifictl}`, the `gpudriver` package, `packages/hardware/quirks/` (`090-ui_service`, `111-sway-init`, the ARC-D remappings), the InputPlumber device and capability maps for the ARC, `start_ppsspp.sh`, `start_mupen64plus.sh`
- https://github.com/ROCKNIX/rocknix.org: `docs/devices/anbernic/rgarc.md`, `rg353pmvvs.md`, `rg35xx-h.md`, `includes/platforms/android-wipe.md`, `docs/contribute/build.md`; https://rocknix.org/configure/h700-installation/ , https://rocknix.org/faqs/ , https://rocknix.org/play/update/
- https://github.com/OpenHandheldCollective/OpenABL , https://github.com/ROCKNIX/rocknix-joypad , https://github.com/ShadowBlip/InputPlumber , https://github.com/JustEnoughLinuxOS/distribution

**Other firmware**
- Batocera: https://github.com/batocera-linux/batocera.linux/tree/master/configs , https://wiki.batocera.org/choose_a_handheld , https://wiki.batocera.org/license , https://wiki.batocera.org/compile_batocera.linux
- Knulli: https://github.com/knulli-cfw/knulli-linux (`configs/knulli-rk3566.board`, H700 `genimage.cfg`), https://knulli.org/devices/ , https://knulli.org/faq/knulli/ , https://knulli.org/guides/partitioning/
- AmberELEC: https://github.com/AmberELEC/AmberELEC ; dArkOS: https://github.com/christianhaitian/dArkOS ; UnofficialOS: https://github.com/RetroGFX/UnofficialOS
- muOS: https://github.com/MustardOS/internal , https://muos.dev/docs/infrastructure/storage ; MinUI: https://github.com/shauninman/MinUI ; NextUI: https://github.com/LoveRetro/NextUI ; spruce: https://github.com/spruceUI/spruceOS , https://github.com/spruceUI/oakMOSS ; Onion: https://github.com/OnionUI/Onion ; Allium: https://github.com/goweiwen/Allium ; CrossMix: https://github.com/cizia64/CrossMix-OS
- GammaOS: https://github.com/TheGammaSqueeze/GammaOSNext , https://github.com/TheGammaSqueeze/GammaOSCore , https://github.com/TheGammaSqueeze/GammaOS-RK3566 , https://github.com/TheGammaSqueeze/GammaOSNextDistribution-14 (commit `0bc531e2d`)

**Devices and kernels**
- https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/rockchip/rk3566-anbernic-rg-arc.dtsi
- https://www.androidcentral.com/gaming/anbernic-rg-arc-d-arc-s-announced , https://droix.net/blogs/anbernic-rg-arc-s-arc-d-review-with-video/
- https://macromorgan.blogspot.com/2024/04/anbernic-rg556.html (T820 bootloader)
- https://docs.u-boot.org/en/latest/board/rockchip/rockchip.html , https://docs.u-boot.org/en/latest/board/allwinner/sunxi.html
- https://docs.mesa3d.org/drivers/panfrost.html

**ES-DE, RetroArch, emulators**
- ES-DE: `INSTALL.md`, `USERGUIDE.md`, `FAQ.md`, `ANDROID.md`, `LINUX-AARCH64.md`, `CHANGELOG.md`, `CMakeLists.txt`, `es-app/src/FileData.cpp`, `GamelistFileParser.cpp`, `GuiAlternativeEmulators.cpp` and `resources/systems/{android,linuxarm,linux}` at https://gitlab.com/es-de/emulationstation-de (label tables at commit `50e4b600a`)
- RetroArch: https://github.com/libretro/RetroArch (`frontend/drivers/platform_unix.c`, `runloop.c`), https://docs.libretro.com/guides/overrides/ , https://docs.libretro.com/guides/kms-mode , https://docs.libretro.com/development/licenses/
- mupen64plus-ui-console `src/main.c`; mupen64plus-ae `GamePrefs.java`
- https://dolphin-emu.org/docs/guides/controlling-global-user-directory/ , https://ppsspp.org/docs/faq , https://docs.scummvm.org/en/latest/advanced_topics/configuration_file.html , https://vita3k.org/faq , https://github.com/stenzek/duckstation
- https://source.android.com/docs/core/storage/scoped (FUSE bypass for `Android/data`); e2fsprogs `lib/blkid/probe.c`, util-linux `libblkid/src/superblocks/vfat.c` (volume id format)

**Disk writing**
- Raspberry Pi Imager: https://github.com/raspberrypi/rpi-imager (`src/drivelist/*`, `src/downloadthread.cpp`, `src/windows/*`, `src/mac/*`, `src/linux/*`)
- bb-imager-rs: https://github.com/beagleboard/bb-imager-rs ; drivelist: https://github.com/balena-io-modules/drivelist
- https://storaged.org/doc/udisks2-api/latest/ , https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering , https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume
- https://github.com/yoctoproject/bmaptool , https://github.com/collabora/bmap-rs , https://github.com/rafalh/rust-fatfs , https://docs.rs/gptman , https://docs.rs/mbrman

**Boot time**
- FlipBase: https://github.com/apommel/FlipBase (`docs/boot-time.md`, `docs/uboot.md`, `docs/decisions.md`, `docs/boot-chain.md`, `docs/card.md`); its RG353PS fork: https://github.com/dengzhan007/BaseOS-RG353PS (not examined)
- BaseOS: https://github.com/pvaibhav/BaseOS (`docs/00` to `docs/12`, `experiments/h700-boot-speed/DESIGN.md`, releases)
- RetroStoneOS: https://github.com/PaddleStroke/RetroStoneOS (`docs/build.md`)
- Bootlin, ELCE 2019: https://bootlin.com/pub/conferences/2019/elce/opdenacker-timing-boot-time-reduction-techniques/opdenacker-timing-boot-time-reduction-techniques.pdf
- Raspberry Pi, "Optimising boot time" whitepaper (2026); https://docs.u-boot.org/en/latest/develop/falcon.html ; https://docs.kernel.org/admin-guide/kernel-parameters.html ; https://docs.mesa3d.org/envvars.html
- REG-Linux: https://github.com/REG-Linux/REG-Linux

**Licences and build hosts**
- https://www.gnu.org/licenses/gpl-faq.html#MereAggregation , https://www.apache.org/licenses/GPL-compatibility.html
- https://learn.microsoft.com/windows/wsl/case-sensitivity , https://learn.microsoft.com/en-us/windows/wsl/filesystems
- https://buildroot.org/downloads/manual/manual.html , https://docs.yoctoproject.org/brief-yoctoprojectqs/index.html

**This project**
- `README.md`, `BIOS.md`, `HOTKEYS.md`, `BACKPORTS.md`, `NOTICE`, `mods/37-esde.sh`, `scripts/esde_config.py`, `scripts/bundled_roms.py`, `mods/esde-emulators.txt`, `mods/nano-systems.txt`, `scripts/build.sh`, `scripts/modlib.sh`, `tools/android-launcher/{fetch_nano.sh,ndk_probe.py}`, `patches/thimsy/0001-thimsy.patch`
- `multi_device_plan.md`, `android_launcher_plan.md`, `device_profiles_plan.md`, `user_emulators_plan.md`, `usb_storage_plan.md`
- `thimsy-os-installer`: `universal_installer.md`, `installer/docs/engine-protocol.md`, `installer/engine/crates/thimsy-engine/src/{catalog,package,flash,drivers,tools,fetch}.rs`, `installer/catalog/`

---

## 17. Review notes

The first draft was reviewed on 2026-11-24 by five reviewers (external facts; this repository and the sibling plans; the card contract; Linux architecture and boot; the installer and completeness against the brief). A second pass tried to refute each finding. 60 findings: 2 critical, 34 major, 15 minor, 9 that settled a guess. None was refuted; 19 were corrected in detail. What changed:

**Facts corrected**
- ROCKNIX boots on an ARC-D (and RG353P/M/V) only after Android is erased from eMMC. The draft said nothing is wiped. This changed the first-device advice, the installer flow, a risk and a decision.
- ES-DE's AArch64 table is `linuxarm`, not `linux`. `linuxarm` has no `SameBoy` core label and does have `ARMSX2 (Standalone)`.
- FlipBase's 1.3 s is the moment init starts the frontend's launcher; the menu is visible at about 3 to 3.5 s. The draft's targets were rewritten around a measured baseline.
- ROCKNIX's RK3566 default GPU driver is libmali; the image is not blob-free without patches; the driver is chosen by a setting, not a device-tree overlay.
- ROCKNIX already boots with `quiet`; mainline already runs the ARC's card slot at SDR104. Both were removed from the list of gains.
- H700 DDR3 and DDR4 units need different SPLs, so they are two images.
- `nano-systems.txt` has 41 systems, not 51. Four patch line references pointed at the wrong function. RG351MP is not a ROCKNIX device. The JELOS remark and the commit statistics were over-read.
- ROCKNIX's licence wording was settled from `LICENSE.md`.

**Design changed**
- The card contract shares much less in v1: the library, BIOS and same-core saves. RetroArch's config folder, core options, remaps and all save states are per platform. Reasons: ROCKNIX's launch scripts rewrite configs on every launch; state and save files are named after the ROM only; the builds run different cores.
- Linux never writes launch fields into `nano/systems.json`, because released Android builds overwrite tuples they don't know. A label is written to a gamelist only when both platforms' ES-DE tables have it.
- The mount point at `/storage/<VOLID>` is permanent and immutable, with a mount check before every launch. The draft would have let saves fall onto internal storage with the card out.
- N64 and DS saves are not shared in v1 (M64Plus FZ's per-game folders; different file names).
- A new section 7.10 says what released Android builds do with each new file.
- Pad silencing uses InputPlumber's intercept mode on RK3566; an evdev grab does not stop SDL emulators there. A per-device pad model was added.
- The installer verifies before it edits the FAT (the draft's order would have failed its own check), probes for counterfeit cards, classifies external SSDs separately, and has the privileged process re-check the drive.
- Frontend-first boot ordering is a rewrite of ROCKNIX's autostart script, not unit ordering. "Thimsy Base" is no longer described as nearly free.

**Aligned with the sibling plans**
- Shared lists stay in `mods/` and layers in `packaging/` (`device_profiles_plan.md`); the draft's `data/`, `gen/`, `platforms/` move was dropped.
- The emulator definition example now extends `user_emulators_plan.md`'s real schema (schema 2, new keys) and takes its trust rules.
- Disk-image packages need `pack.json` format 2 (`multi_device_plan.md` §7.4).
- The drive model is `usb_storage_plan.md`'s two settings, not one card.
- Package profiles use that plan's Small and Medium, with a resolver that runs at image build time.
- `custom_systems/es_systems.xml` is read on Linux, as the launcher plan's Phase 3 does on Android.
- The Nano fetch script already exists (`tools/android-launcher/fetch_nano.sh`).

**Added**
- Fonts, system keys, memory on 1 GB devices, update and telemetry endpoints, the ROCKNIX platform-name mapping.
- Effort in person-weeks, a public alpha before the card contract and the installer, and a table of what each sibling plan owns.

Not re-reviewed: this revision itself. The full findings are kept outside the repository with the research notes.
