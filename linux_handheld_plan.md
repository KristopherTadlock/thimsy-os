# Linux handheld plan: Thimsy OS on devices that run Linux firmware

Date: 2026-11-24; revised 2026-11-25 (base changed to a Thimsy Base, section 17.2; device layer and coverage, section 17.4; the owner's second batch of answers and the SD-only principle, section 17.5)
Status: plan only, nothing implemented, nothing run on a device. Research done 2026-11-24: the ROCKNIX, Batocera, Knulli, AmberELEC, dArkOS, muOS, MinUI, NextUI, spruce, Onion, FlipBase and BaseOS repositories and wikis; ES-DE, RetroArch and emulator sources; Raspberry Pi Imager and bb-imager-rs sources; and an audit of this repository and of `thimsy-os-installer`. Reviewed 2026-11-24 by five adversarial reviewers, each checked by a second pass: 60 findings, none refuted, all folded in (section 17). Reviewed again 2026-11-25 by six reviewers with adversarial checks: 57 findings applied, 53 once duplicates are merged (section 17.1). Revised 2026-11-25 after the owner's decisions: the base is a Thimsy Base in the FlipBase mould, not a ROCKNIX downstream; the boot target is 3 s; the owner has an RG ARC-D and wants Android kept on it. Research for that revision: FlipBase and its RG353PS fork, Buildroot, the Batocera, Recalbox and Knulli package trees, ROCKNIX's device patches, the mainline kernel and U-Boot sources for the ARC, and the no-systemd backends of FlipBase and RetroStoneOS (section 17.2). That revision has not been through an adversarial review. Revised again 2026-11-25 for the owner's coverage goal: four researchers (ROCKNIX's device inventory, importing its hardware layer, display and rotation, input), a designer's options, and one adversarial critique whose corrections take precedence (section 17.4). Revised again 2026-11-25 with the owner's second batch of answers (section 17.5): the SD-only principle, open questions 11 to 14 and 20 to 26, a storage job in the installer, and corrections left over from the last verification.
Builds on: `multi_device_plan.md` (device descriptors, installer backends, package format 2), `android_launcher_plan.md` (the `NanoPlatform.h` seam and `LIBRARY-FORMAT.md`), `device_profiles_plan.md` (layers, profiles, the resolver), `user_emulators_plan.md` (emulator definitions), `usb_storage_plan.md` (the drive model), and `universal_installer.md` in the installer repo. This plan does not edit them. Where it needs something from them, or proposes a change to them, it says so.

Claims come from the sources in section 16. Anything not confirmed by a source is marked *(guess)*. Patch references give the function name and `P:<line>` in `patches/thimsy/0001-thimsy.patch` as of this date; the lines move.

Request (user, 2026-11-24, condensed): support handhelds that can't run Android but can run Linux custom firmware, "for example the Anbernic RG ARC-D and similar devices"; port all of Thimsy's UI and features where possible (the Nano home, profile export through ES-DE, symlinked emulator configs); make profiles interchangeable between Android and Linux devices on one SD card; have ThimsyInstaller build and write bootable images; adopt the fast-boot work of "Paper Plane"; and refactor the 1.4.4 build so Android and Linux share as much code as feasible.

Owner decisions (2026-11-25): a "Thimsy Base" instead of a ROCKNIX downstream (decision 1); a 3 s cold boot to Nano's first home frame, with 1.5 s as a stretch goal (decision 5); the owner has an RG ARC-D only, and Thimsy should boot from card slot 1 with Android kept on the eMMC (open question 2). Later the same day: the broadest device support, initially ROCKNIX's coverage, with devices that already boot Linux first and the Odin 2 not a focus; reached by "Thimsy Base + importer", one Linux flavour whose `thimsy-import` tool tracks ROCKNIX's hardware layer, with families arriving in waves on Thimsy's own fast boot (decision 21). Later still: **the SD-only principle**, "Putting Linux on an SD card should always be reversible and non-destructive." For now the Linux install never writes internal storage, and erasing Android is dropped for good; writing a bootloader to internal storage with a verified backup is deferred to later, not excluded (section 1; section 17.5).

---

## 0. Short answers

| Question | Answer |
|---|---|
| Base | **A Thimsy Base** (decided 2026-11-25): our own Buildroot external tree in the FlipBase mould. Mainline kernel and U-Boot, BusyBox init with no systemd, no initramfs, a read-only EROFS (or squashfs) root, one frontend: Nano. **The device layer is ROCKNIX's, tracked by an importer** (decided 2026-11-25, decision 21): `thimsy-import` reads ROCKNIX at a tag Thimsy chooses and writes kernel patch series, device trees, DTB and firmware lists and per-device facts; U-Boot and the boot chain stay Thimsy's. Proposed: the kernel version is ROCKNIX's per family, at a Thimsy-chosen tag (decision 22). Proposed build shape (decision 25): one userland per CPU tier, a GPU pack per driver, a hardware pack per family, put together into a per-device card. ROCKNIX's runtime (systemd, Sway, InputPlumber, its scripts) is not used (section 3). |
| Coverage goal | **The broadest device support, initially ROCKNIX's coverage** (owner, 2026-11-25): its tree has 16 families (15 ARM plus AMD64); `config.xml` on `next` lists the 15 ARM ones, about 83 products and 97 DTBs, AYN, Retroid and AYANEO units included. Devices that already boot Linux come first; the Odin 2 is not a focus. Each product carries a status (built, boots, works, certified) rather than a blanket support claim (decision 26; section 8.3) |
| First device | The owner's **RG ARC-D, dual-booting**: Android stays on the eMMC and Thimsy boots from card slot 1. Phase 0 tries three SD-only routes, (d), (a) and (b), none of which writes the eMMC; if none passes, the ARC-D waits for bootloader writing (later) and the ARC-S becomes the first device. Erasing Android is not offered (section 1; section 8.6). Recommended beside it: a **Linux-only reference unit** (the RG ARC-S, or the RG353VS) for the 3 s number and the 1 GB floor (decision 2, reopened). Both ARCs' device trees are in mainline Linux, and mainline U-Boot detects both. |
| Coverage waves | Proposed (decision 26). **Wave 0:** ARC-D and ARC-S. **Wave 1:** the rest of RK3566 (Generic and Specific images) and Allwinner **H700** (RG35XX Plus/H/SP, RG40XX, RG CubeXX; DDR3 and DDR4), the largest group with no Android at all. **Wave 2:** RK3326 image b, RK3399, RK3576, S922X. **Wave 3, deferred (later: needs bootloader writing):** the Qualcomm families through OpenABL (about 32 to 35 Qualcomm products, depending on how variants are counted), SM6450 among them once a ROCKNIX tag carries it; OpenABL writes a bootloader to internal storage, which the SD-only principle defers (section 1). **Wave 4:** RK3326 image a, RK3588 (vendor BSP kernel); it does not wait for wave 3. AMD64 is a QEMU test target only (section 2.4). |
| UI | The same Nano C++ source, with a third implementation of the `NanoPlatform.h` seam that `android_launcher_plan.md` introduces. About two thirds of the 204k lines port once the seam exists; about 35k lines are Android-bound and are replaced or dropped (section 5). On Linux, the recommendation is Nano as a KMS/GBM client with no compositor, handing the display to each emulator it starts (decision 4, reopened; section 5.3). |
| Card interop | The contract is what the Android build already writes: `ES-DE/`, `ROMs/`, `emulator_config/`. The Linux build mounts the card at the **same path stock LineageOS uses** (`/storage/<volume id>`, the filesystem serial). On the RG Cube, Android's volume id is a mount-order index, not the serial, so how Linux resolves the paths the Cube wrote is decision 20. No symlink ever lives on the card. v1 shares the library, BIOS files and in-game saves where both builds run the same core; settings and save states stay per platform (section 7). |
| Installer | A `disk_image` path on the SD-card backend `multi_device_plan.md` already plans: a verified per-device image streamed to the card with bmap and read back (section 8). With about 83 products, the proposal is that the installer puts each card together from verified parts (tier root, GPU pack, hardware pack, `device.json`); the card is still per device (decision 13, reopened). For the ARC-D, the card layout of the SD-only route Phase 0 finds; no install step writes internal storage. Qualcomm products would add an OpenABL step from Android, deferred with wave 3. The installer also writes game cards and USB drives as a job of its own: format and lay out, pack-ins, ROMs and BIOS from the PC, and a non-destructive update (section 8.4). |
| "Paper Plane" | **The owner's name for FlipBase or BaseOS** (answered 2026-11-25; no project of that name exists). The references used are FlipBase (RK3566, mainline, MIT; on its boot counter, whose zero is about 0.5 s after power-on: the frontend's launcher started at 1.3 s on November's warm reboots and at 2.0 s on October's cold boots on its vendor kernel; first frame 2.9 to 3.0 s on September's warm reboots, also on the vendor kernel; a filmed cold boot showed the menu 3.97 s after the power LED), BaseOS and RetroStoneOS (section 9). FlipBase is now also the template for the base. |
| Boot target | **3 s cold, power-on to Nano's first complete home frame** (decided 2026-11-25): FlipBase's first-frame figure on the same SoC, but FlipBase reads its figures on a counter that starts about 0.5 s after power-on, and it has published no cold first frame on mainline; from power-on its figures put one at about 2.9 to 3.5 s *(guess)*, so 3 s is at the edge of what it has demonstrated (section 9.4). **1.5 s is the owner's stretch goal.** Nobody has published a 1.5 s cold boot to a visible menu on RK3566; 1.5 s is realistic for wake from sleep or for the launcher's start, and is measured in Phases 0 and 6, not promised (section 9.4). Other families get their own targets in section 9.4; only RK3566's 3 s is decided, the rest are guesses, and the Qualcomm families, whose first stages are not Thimsy's code, state measured totals. |
| Effort | About **38 to 62 person-weeks** to a first public release on RK3566 *(guess)*, with a plain-image alpha after 22 to 36 (section 12). More than the ROCKNIX plan's 31 to 51, because the base, its packages and each board's support are now Thimsy's; in exchange there is no rebase queue, and the 3 s target is in reach. ROCKNIX's coverage adds the importer (4 to 8 weeks *(guess)*), waves 1 to 4 and per-device quirk work: **about 60 to 100+ person-weeks for the whole program** *(guess)* (section 12). |
| Biggest unknowns | Whether the ARC-D's stock eMMC loader boots a mainline card with Android kept, by one of the three SD-only routes, and whether deep sleep works under it (section 8.6). The rotation contract (Nano, an SDL3 patch, a RetroArch patch) across Panfrost and Panthor, and how the Qualcomm families rotate Vulkan with no compositor; `thimsy-padd` hiding source nodes under libudev-zero; a Quick Menu over standalone emulators (section 5.3). How often ROCKNIX's kernel bumps and tags land, which sets the importer's real cost (decision 22). |

Three corrections to the brief:

1. **The RG ARC-D can run Android.** It ships Android 11 on eMMC and boots its stock Linux from a card; GammaOS (Android 12, archived) and GammaOS Core (Android 13 TV, frozen since February 2025) support it. What it lacks is an **Android 14 base**, which is what Thimsy needs; `multi_device_plan.md` defers every RK3566 device to tier 5 for that reason. The RG ARC-S (1 GB, no eMMC) and the RG353VS/PS have no stock Android and no Android 14 base; GammaOS Core (Android 13 TV) is their only Android. The whole H700 family has no Android at all.
2. **ROCKNIX and Batocera don't share an ARC-D with Android; this plan tries to.** ROCKNIX's install page says: "You will need to wipe the Android partition to boot into ROCKNIX" (`dd if=/dev/zero of=/dev/block/mmcblk0` over adb), and gives no technical reason. Batocera says the same from v42: the eMMC bootloader "is not compatible with later Linux kernels". The same holds for the RG353P/M/V. Units with the "legacy bootloader" are not supported at all. But the ARC-D's stock loader boots Anbernic's own Linux card from slot 1 with Android left on the eMMC, and JELOS, ROCKNIX's predecessor, booted from a card on Anbernic RK3566 units with Android installed (ROCKNIX PR #154). Section 8.6 has the routes.
3. **"Profile" has seven meanings in this code base** (installer profiles, hotkey profiles, RetroArch controller profiles, M64Plus profiles, shader profiles, the device catalog, and the planned Small/Medium/Large/Ultra). None is a portable user profile. In this plan **"the card profile"** means the `ES-DE/`, `ROMs/` and `emulator_config/` trees on a drive. **"Package profile"** means the installer's choice: `light`/`medium`/`heavy` in today's catalog, Small/Medium/Large/Ultra once `device_profiles_plan.md` lands.

---

## 1. Goal and scope

In scope:

- A Linux build of Thimsy OS for handhelds with **no Android 14 base**, on the **Thimsy Base**: our own Buildroot-based OS, with one card per device. RK3566 first (the ARC pair), then H700: mainline-kernel families where ROCKNIX's GPL device patches give a head start.
- **The broadest device support, initially ROCKNIX's coverage, in waves** (owner, 2026-11-25; decisions 21 and 26): ROCKNIX's hardware layer tracked by `thimsy-import`, devices that already boot Linux first, and a per-product status instead of a support claim. Every device keeps what is on its internal storage (the SD-only principle, below).
- Dual boot on the owner's RG ARC-D: Android kept on the eMMC, Thimsy from card slot 1, by an SD-only route (section 8.6). If no SD-only route passes, the ARC-D waits for bootloader writing (later) and the ARC-S is the first device.
- A storage job in the installer: game cards and USB drives formatted, laid out, filled and updated, for Android and Linux devices (section 8.4).
- Nano as the home and frontend of that build, with the features in section 5.4.
- One card profile that works in an Android Thimsy device and a Linux Thimsy device.
- ThimsyInstaller writing a bootable card on Windows, macOS and Linux.
- Boot time as a tracked number from the first build, against a 3 s target (section 9.4).
- One Nano source, one set of data files and generators, two firmware builds (three with the Thimsy Home app).

Out of scope:

- Devices whose only kernel is a closed vendor BSP with a framebuffer-only or PowerVR GPU driver (A133P, Knulli-style H700, Miyoo Mini, A30). Section 2 marks them `skip`.
- Unisoc T820/T618 and Dimensity 8300 devices. No Linux exists for them. They stay on the Android build.
- GammaOS Pico devices (`multi_device_plan.md` §3).
- AMD64 hardware. ROCKNIX ships no AMD64 image (release `20261101`); AMD64 is a QEMU test target only.
- RK3588 and RK3326 image a until wave 4: RK3588 runs a vendor BSP 6.1 kernel and a 2024 vendor U-Boot fork, and image a needs Hardkernel's vendor U-Boot.
- Writing internal storage, for now (the SD-only principle, below). Writing a bootloader to internal storage with a verified backup is deferred to later, not excluded: ARC-D route (c) (section 8.6) and OpenABL on the Qualcomm families, so wave 3 waits for it (section 2.4). Erasing Android is out permanently.
- The Odin 2 as a focus. The owner's Odin 2 Portal and Retroid Pocket Duo are test units for SM8550 and SM8750 in the deferred wave 3, with no device-specific work before then.
- ROCKNIX's runtime: systemd, Sway, InputPlumber, its quirk scripts and its launch scripts.
- Selling or bundling. The bundle contains non-commercial cores.

What does not change:

- The RG Cube's Android output stays byte-identical through the refactor (the rule `multi_device_plan.md` sets).
- **The SD-only principle** (owner, 2026-11-25): "Putting Linux on an SD card should always be reversible and non-destructive." Now: the Linux install never writes internal storage, and removing the card gives back the untouched device. Erasing Android is dropped permanently: there is no erase guide and no "last resort". Later, deferred and not excluded: writing a bootloader to internal storage with a verified backup (ARC-D route (c); OpenABL on the Qualcomm families). Until then the Qualcomm families wait (wave 3), and the ARC-D boots by an SD-only route or waits too. Reading internal storage (the ARC-D's loader, a unit's vendor BL31, firmware to fetch) is allowed.
- Thimsy never redistributes a handheld maker's firmware (decision 24, proposed: the firmware policy). Broad coverage tests this: ROCKNIX's `extra-firmware` package is marked `proprietary` with no licence file; for SM8750 its kernel embeds an AW88261 calibration blob "extracted from the stock Android image"; the Qualcomm GPU zap shaders and DSP images, and the H700 `.panel` files derived from vendor DTBs, carry the same kind of risk. The proposed rule: ship only `linux-firmware` files with known licences, and fetch the rest from the user's own device at install time (over adb on the Qualcomm units, which keep Android), onto a partition, never into the kernel image; a device whose firmware can be neither shipped nor fetched is not covered. The Linux image contains no maker's firmware. It does contain redistributable binaries: the Rockchip DDR init from `rkbin` (mainline U-Boot needs it as its TPL), rkbin's BL31 only for an RK3566 unit with no vendor BL31 to read (decision 18, the fallback), Wi-Fi and Bluetooth firmware (`linux-firmware` and per-board files), and `libmali` only if decision 11's A/B picks it (section 3.3). Under the ARC-D's SD-only routes (d), (a) and (b), Anbernic's own loader runs first from the unit's eMMC; it is never shipped. On RK3566 the BL31 is the unit's own vendor BL31 (decision 18), read from the unit and never redistributed.

---

## 2. Device matrix

One row per image-sharing group. CFW short names: RX ROCKNIX, BT Batocera, KN Knulli, AE AmberELEC, dArk dArkOS (ArkOS is archived), uOS UnofficialOS, mu muOS, Min MinUI (archived), Nx NextUI, spr spruce, On Onion. GON is GammaOS Next (Android 14), GOC is GammaOS Core (Android 13 TV).

Priority: **P0** first target; **P1** same image or next family; **P2** later; **P3** only if asked; **skip** not viable or better served by the Android build.

**These tables are hand-written and will be generated.** ROCKNIX's `projects/ROCKNIX/config.xml` (on `next`, commit `0f4806b`) lists 15 ARM families (AMD64, the 16th, has a device directory but no `config.xml` entry), about 83 products and 97 unique DTB names, AYN, Retroid and AYANEO units among them. Once `thimsy-import` exists, the device matrix is generated from it: one row per product, with its DTB variants, its family's wave and its status (built, boots, works, certified; section 8.3). Until then the rows below cover the families this plan has read closely; the full family list and its waves are in section 2.4.

Kernel versions for ROCKNIX (RX) are read from its `next` branch on 2026-11-24. They differ from its newest stable tag, `20261101`, for five ARM families, not one (six with AMD64, 7.1.2 at the tag and 7.2 on `next`): there RK3566, RK3576 and S922X build 7.2.11, and SM6115 and SM8550 plain 7.2, where `next` has 7.3.5, 7.3.5, 7.3.4, 7.3.5 and 7.3.5. H700 (7.2), RK3326 (7.1.2) and RK3399 (6.18.54) are the same on both. RK3566's 7.3.5, the version FlipBase pins, is on `next` only: ROCKNIX's commit is dated 2026-11-22 and no release has shipped it. Which version RK3566 starts on is decided at the start of Phase 2a, by which time tags `20261201` and `20270101` should exist (decision 22).

### 2.1 Rockchip RK3566 / RK3568 (Mali-G52)

| Device | CFWs | Kernel | GPU driver | Display / input quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|
| **Anbernic RG ARC-D** (2 GB, 32 GB eMMC) | RX (RK3566 "Generic" image), BT (RGxx3), uOS (BSP image). KN: backer-only alpha. Not AE, dArk, mu | RX/BT mainline (RX 7.3.5); uOS vendor BSP | RX builds both: **libmali (GLES 3.2) is its default**, Panfrost (GLES 3.1) is chosen with its `gpudriver` setting | 4" 640x480 DSI panel, natively portrait (`rotation = <90>`), RX patches its timings; touchscreen with an axis-swap patch; Saturn-style 6 face buttons, **no analog sticks**. RX presents the pad through InputPlumber; mainline has `gpio-keys` and an `adc-keys` home key | RX: OS card plus an optional second card for games, **only after Android is erased from eMMC**; "legacy bootloader" eMMC units unsupported. Thimsy: card slot 1 with Android kept, by an SD-only route Phase 0 proves ((d), (a) or (b); section 8.6); if none passes, it waits for bootloader writing (later) | Stock Android 11 (with its own Linux card); GammaOS (A12), GOC. No Android 14 | **P0** |
| **RG ARC-S** (1 GB, no eMMC) | RX, BT. KN: device tree only | as ARC-D (KN: BSP 6.1) | as ARC-D (KN: blob) | same panel, no touch | SD only; nothing to erase (`multi_device_plan.md`'s row gives "RKFW SD or eMMC"; the droix review and the mainline device tree say there is no eMMC) | Stock is Linux. GOC lists it | **P0** (same SoC build; 1 GB floor; the recommended Linux-only reference unit) |
| RG353P / M / V / VS / PS | RX, BT, AE, dArk (M, V), uOS | RX 7.3.5; AE/dArk BSP 5.10 | as ARC-D; AE/dArk libmali | 3.5" 640x480, two panel revisions (both handled); touch on P/V; analog sticks through an ADC mux | SD; P/M/V also eMMC, **with the same Android wipe** under RX. Thimsy: P/M/V follow the ARC-D's SD-only route | P/M/V: Android 11, GammaOS A12, GOC. VS/PS: no stock Android; GOC only | **P1** |
| RG503 | RX, BT, AE, dArk | as RG353 | as RG353 | 4.95" OLED initialised over SPI (960x544 *(guess)*) | SD | GOC lists it | **P1** |
| Powkiddy RGB30, RGB10 Max 3, RGB20SX, RGB20 Pro, RK2023 | RX, BT, dArk (some), spr (some, base image) | RX 7.3.5 | as ARC-D | RGB30 is 720x720 (1:1), like the Cube | SD | GOC lists most | **P1** |
| Powkiddy X55, X35S, X35H | RX ("Specific" image), BT, uOS (X55) | 7.3.5 | as ARC-D | X35H needs the `extlinux.conf` FDT line edited by hand | SD | GOC | **P2** (second image, manual FDT) |
| Miyoo Flip | KN (BSP 6.1, needs GammaLoader), spr, Min, FlipBase (mainline). Not RX | vendor BSP; FlipBase mainline 7.3.5 | Mali blob; FlipBase Panfrost or libmali | 3.5" 640x480 | Internal storage; no SD boot without a preloader change | GOC | **P2** |
| RG DS / DS Plus (RK3568) | RX, BT, KN alpha | 7.3.5 | as ARC-D | dual screen | SD; Android on eMMC | **GON, a GammaOS Nano target** | **P3** (Android Thimsy covers it) |
| Miniloong Pocket 1, GKD Bubble, GameMT E5/E6 Plus, CB408 | dArk, KN, spr (some) | mixed | mixed | - | SD or eMMC | GOC | **P3** |

### 2.2 Allwinner H700 (Mali-G31, 1 GB LPDDR4 or LPDDR3): no Android at all

| Device | CFWs | Kernel | GPU driver | Display / input quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|
| **RG35XX Plus / H / SP / 2024 / Pro, RG40XX H / V** | RX (**two images: DDR4 and DDR3, with different SPLs**), BT, KN (main target), mu (main platform), Nx, spr (base image), Min, GarlicOS 2, BaseOS | RX mainline 7.2, BT 7.2.8; KN vendor BSP 4.9.170; mu, Nx, BaseOS vendor BSP (version *(guess)* for mu and Nx) | RX/BT Panfrost, no Vulkan; KN `mali-g31-fbdev` blob, **framebuffer only** | 640x480. **Panel revision can't be detected**: the user copies a DTB to `dtb.img`, and a wrong one shows garbage. RX uses its `rocknix-joypad` kernel driver here, not InputPlumber. RG40XX: RGB LEDs, inverted right stick | SD only (two slots) | none | **P1** |
| RG CubeXX | RX, BT, KN, mu, Nx, spr | as above | as above | 720x720 (1:1) | SD | none | **P1** |
| RG34XX / 34XX SP, RG SP, RG28XX | RX, BT, KN, mu | as above | as above | 34XX is 3:2 *(guess)*; RG28XX panel is natively portrait (`rotation = <270>`) | SD | none | **P2** |

### 2.3 Other families

| Device | SoC / GPU | CFWs | Kernel | GPU driver | Quirks | Boot medium | Android | Priority |
|---|---|---|---|---|---|---|---|---|
| RG351P/M/V, ODROID-GO Advance/Super, RGB10, GameForce Chi, XU10, G350, GKD Pixel 2 (RG351MP: AE, dArk; not RX) | RK3326 / Mali-G31 | RX (two images, list frozen: "do not add any new RK3326 images"), BT, AE, dArk, KN, mu, spr | RX/BT mainline 7.1; others BSP 4.4 | Panfrost or libmali | many panel variants (EE clones on `generic-dsi`; open question 32); half the units portrait | SD | none | **wave 2** for image b (mainline U-Boot); **wave 4** for image a (Hardkernel vendor U-Boot) |
| R36S and clones | RK3326 | RX, BT, KN, arkos4clone | as above | as above | many board variants; the wrong build blinks a red LED | SD | none | **wave 2**, only where `config.xml` lists the board |
| RG552 | RK3399 / Mali-T860 | RX, AE, uOS | RX mainline 6.18.54 | Panfrost | portrait panel; `rocknix-joypad` | SD | Android on eMMC *(guess)* | **wave 2** |
| ODROID-GO Ultra, RGB10 Max 3 Pro | S922X / Mali-G52 | RX, BT (OGU), uOS | mainline 7.3.4 on `next` | RX ships libmali and Panfrost (its `gpudriver` switch) | portrait panels; U-Boot at sector 1 behind Amlogic's binary FIP; `rocknix-joypad` | SD | none | **wave 2** |
| GameForce Ace, CM5 handhelds | RK3588S / Mali-G610 | RX, uOS | **vendor BSP 6.1** (Armbian `rk-6.1-rkr3`) | libmali, Panfrost or Panthor | portrait panel | SD | *(guess)* | **wave 4** |
| Retroid Pocket 5, Mini, Flip 2, AYN Thor Lite | SM8250 / Adreno 650 | RX, BT, KN (no build) | mainline 7.2 | Freedreno + Turnip | portrait panels; MCU pad driver; a Visionox panel variant that needs an installer question | SD **after OpenABL is flashed from Android** | stock Android; GON only "planned" | **wave 3, deferred** (later: OpenABL writes internal storage; section 1) |
| AYN Odin 2 family, RP6, AYANEO Pocket, AYN Thor *(family a guess)* | SM8550 | RX, BT | mainline 7.3.5 on `next` (7.2 at `20261101`) | Freedreno + Turnip | portrait panels (11 of 12 SM8550 products); dual screens on the Thor and the Pocket DS | SD or UFS through OpenABL | GON or stock Android | **wave 3, deferred** (the owner's Odin 2 Portal is a test unit; not a focus) |
| Other Qualcomm families: SDM845, SM4450, SM6115, SM6450, SM8650, SM8750 | Adreno | RX | 7.1.2 to 7.3.5; SM6450 has no ROCKNIX release image yet | Freedreno + Turnip | MCU pad drivers; mostly portrait | SD through OpenABL | stock Android | **wave 3, deferred**; SM6450 once a ROCKNIX tag carries it (the owner's RP Duo is an SM8750 test unit). Rows are generated from `config.xml` (about 32 to 35 Qualcomm products in all, depending on how variants are counted) |
| RG Vita Pro | RK3576 | RX, BT, KN, mu | mainline 7.3.5 on `next` | libmali or Panfrost | portrait panel; SPI pad MCU | SD | **GON** | **wave 2** |
| TrimUI Brick, Smart Pro, MagicX Zero 28/40 | A133P / PowerVR GE8300 | KN, mu, Nx, spr, Min, CrossMix | closed vendor BSP | PowerVR blob | - | SD | **GON; the Brick is GammaOS Nano's main target** | **skip** |
| TrimUI Smart Pro S | A523 / Mali-G57 | KN, Nx, spr | vendor BSP 5.15 | blob | - | SD | unknown | **P3** |
| Miyoo A30 | A33 / Mali-400, 512 MB | spr, Min | stock BSP | Mali-400 blob (GLES 2) | - | SD | none | **skip** |
| Miyoo Mini family | SSD202D, 128 MB | On, Allium, Koriki, Min, spr | stock BSP | no 3D GPU *(guess)* | - | internal + SD overlay | none | **skip** |
| RG Cube, RG556, RG406H/V, RG476H, RG Slide | Unisoc T820 | **none** (bootloader locked, no mainline) | - | - | - | - | **GON: today's Thimsy** | **skip** |
| RG405M/V, RG505; RG557, RG477M/V | T618; Dimensity 8300 | none | - | - | - | - | GON | **skip** |
| x86 handhelds | AMD64 | RX (tree only, no image), BT, mu (qemu) | mainline | Mesa | - | USB or internal | n/a | **out of scope as hardware; a QEMU test target** |

### 2.4 What the matrix says

- **ROCKNIX and Batocera are the only CFWs on mainline kernels with open GPU drivers** across RK3566, H700, RK3326 and Qualcomm. Knulli, muOS, NextUI, AmberELEC and dArkOS run vendor kernels with Mali blobs. So ROCKNIX's per-SoC device patches (GPL-2.0) are the largest body of mainline handheld fixes, and the Thimsy Base takes them through `thimsy-import` (section 3.2). A Nano written against Mesa runs on each of those boards once the base supports it, and nowhere on the vendor-kernel H700 firmwares.
- **One card per device, built from shared parts.** ROCKNIX ships one image per SoC family, sometimes split in two (RK3566 Generic and Specific; H700 DDR4 and DDR3; RK3326 a and b). The proposal (decision 25) builds one userland per CPU tier, a GPU pack per driver and a hardware pack per family, and puts them together per device with that device's `device.json` (section 3.2). Mainline U-Boot's `anbernic-rgxx3-rk3566` board already detects the ARC-D, the ARC-S, the RG353 family, the RG503 and the Powkiddy RGB30, RK2023 and RGB10 Max 3 by ADC, so one RK3566 hardware pack reaches them all. Each still needs its own screen, pad and sleep testing.
- **The pad.** ROCKNIX uses two input models: its `rocknix-joypad` kernel driver on H700, RK3326, RK3399 and S922X (and a `gameforce-gamepad` patch on RK3588), and InputPlumber, a D-Bus service presenting one virtual DualSense, over mainline device-tree nodes and out-of-tree MCU drivers on RK3566/RK3568, RK3576, SDM845 and every SM* family: 10 families, about 50 of the 83 products. The Thimsy Base has no InputPlumber. The ARC is three source nodes on mainline device trees, not one: `gpio-keys-control`, `adc-keys-home` and `pwm-vibrator` (ROCKNIX's `01-rg-arc.yaml`). Thimsy's proposal is one small daemon, `thimsy-padd`, that merges each device's sources into one pad with one Thimsy identity (decision 23; section 5.3).
- **Rotation is the common case.** 47 of about 83 products have a natively portrait panel (device-tree `rotation` 90 or 270): nearly every Qualcomm unit, half the RK3326 units, the RG552, the RG Vita Pro, every RK3588 device, both S922X devices and the ARC. Most RK3566 units (RG353, RGB30, RG503) and 11 of 12 H700 products (18 of 19 DTBs) are landscape; only the RG28XX is portrait. Section 5.3 names one rotation contract.
- **Revisions that can't be detected become questions.** H700 panel revision (rev6, v2) and DDR type, the RK3326 EE clones on `generic-dsi`, and SM8250's Visionox panel can't be detected. They become typed installer questions (section 8.4).
- **Overlay-style firmwares (MinUI, NextUI, spruce, Onion) are not a base.** They run on the stock kernel, have no GPU path Nano can use on most devices, and their licences block reuse (MinUI has none; NextUI is PolyForm Noncommercial since 2026-08-15; spruce is CC BY-NC 4.0).
- **Every Linux CFW uses its own card layout** (`Roms/Name (TAG)`, `ROMS/<any>`, `/userdata/roms`, `EASYROMS`). Thimsy keeps its own (ES-DE names) and does not try to read theirs in the first release. Nano's folder-alias table (`esdeCanonSystemName`, P:20449) already maps MinUI-style names.

**Families and waves** (decision 26, proposed; wave 3 deferred by the SD-only principle, section 1: every Qualcomm family needs OpenABL written to internal storage, so wave 3 waits until writing a bootloader with a verified backup is allowed, and wave 4 does not wait for it). Kernel versions are ROCKNIX's on `next`, with tag `20261101` in brackets where it differs. CPU tiers follow ROCKNIX's `TARGET_CPU` in `devices/*/options`. "Mali + Panfrost" means ROCKNIX ships both and its `gpudriver` script defaults to libmali: 5 families, the 4 with `GRAPHIC_DRIVERS="mali panfrost"` and S922X, whose `GRAPHIC_DRIVERS="panfrost"` but whose `ADDITIONAL_PACKAGES` adds `libmali`.

| Family | Wave | CPU tier | ROCKNIX GPU | Boot path | ROCKNIX pad | Kernel |
|---|---|---|---|---|---|---|
| RK3566 / RK3568 | 0 (ARCs), 1 (the rest) | v8.2 (A55) | Mali + Panfrost | rkbin DDR TPL, mainline U-Boot | InputPlumber | 7.3.5 (7.2.11) |
| H700 | 1 | v8.0 (A53) | Panfrost | mainline SPL and U-Boot, DDR3 and DDR4 images | `rocknix-joypad` | 7.2 |
| RK3326 image b | 2 | v8.0 (A35) | Mali + Panfrost | rkbin DDR, mainline U-Boot v2025.10 | `rocknix-joypad` | 7.1.2 |
| RK3399 | 2 | v8.0 (A72/A53) | Panfrost | rkbin DDR, mainline U-Boot 2025.10 | `rocknix-joypad` | 6.18.54 |
| RK3576 | 2 | v8.0 (A72/A53) | Mali + Panfrost | U-Boot git snapshot `88dc2788` | InputPlumber (SPI MCU) | 7.3.5 (7.2.11) |
| S922X | 2 | v8.0 (A73/A53) | Mali + Panfrost | Amlogic binary FIP, mainline U-Boot v2026.10 | `rocknix-joypad` | 7.3.4 (7.2.11) |
| SDM845, SM4450, SM6115, SM8250, SM8550, SM8650, SM8750 | 3, deferred (later: needs bootloader writing) | v8.0 (SM6115), v8.2 (SDM845, SM4450, SM8250), v9 (SM8550, SM8650, SM8750) | Freedreno + Turnip | XBL, ABL, OpenABL, Boot Loader Specification | InputPlumber (MCU drivers) | 7.1.2 to 7.3.5 |
| RK3326 image a | 4 | v8.0 | Mali + Panfrost | Hardkernel vendor U-Boot | `rocknix-joypad` | 7.1.2 |
| RK3588 | 4 | v8.2 (A76/A55) | Mali + Panfrost | vendor U-Boot fork (2024) | `gameforce-gamepad` | vendor BSP 6.1 |
| SM6450 | 3, deferred (later: needs bootloader writing; once a ROCKNIX tag carries it) | v8.2 | Freedreno + Turnip | OpenABL | InputPlumber | 7.2 |
| AMD64 | QEMU only | - | Mesa | EFI | - | 7.2 (7.1.2) |

RK3326 is frozen upstream: its `config.xml` entry says "DO NOT ADD ANY NEW RK3326 IMAGES". In ROCKNIX's release `20261101`, 17 images cover 14 families; SM6450 and AMD64 have none.

---

## 3. Base choice and licensing

### 3.1 Options

**Decided 2026-11-25 (decision 1): option (d), a Thimsy Base derived from FlipBase.** The table keeps the analysis that led there.

| Option | Device support | Cost to Thimsy | Verdict |
|---|---|---|---|
| (a) Pinned downstream of ROCKNIX (LibreELEC-style build): `distributions/THIMSY`, new packages, a patch series, rebased on a schedule | 16 SoC families, mainline kernel and U-Boot, per-device quirk scripts. 127 authors and about 6,800 commits in the last 12 months, of which roughly half are merges of LibreELEC upstream work *(share is a guess)* | A patch queue to rebase (about a dozen patch points); 200 GB and about ten hours for a first build; GPL-2.0 for the distro layer; systemd and an initramfs in front of the frontend | Recommended until 2026-11-25. **Not chosen:** a 3 s boot does not fit a systemd-based distribution (section 9), and the patch points sit in scripts that change often |
| (b) Free-running fork of ROCKNIX | same on day one | Merge conflicts against a tree that takes about 130 commits a week (many of them outside handheld files). A lone fork has no upstream to fall back on: JELOS, ROCKNIX's own predecessor, was ended by its maintainers in 2024 | No |
| (c) Knulli or Batocera fork (Buildroot) | Batocera: mainline, wide. Knulli: vendor kernels, blobs, binary bootloaders with no source for some devices | Three stacked layers for Knulli; Batocera is 74% one committer, and its recipes are LGPL-3.0+. Batocera's own Docker-on-Windows note says "not stable enough" (2022) | No. Batocera's recipes are a reference for versions, flags and patches, not copied (section 3.3) |
| **(d) Thimsy Base: our own Buildroot external tree, forked from FlipBase** | None at the start: every board is ours. ROCKNIX's GPL device patches, out-of-tree device trees and quirk knowledge are imported per SoC family, by `thimsy-import` since decision 21 (the table below). Both ARCs' device trees are in mainline Linux, and mainline U-Boot detects both | Smallest image and fastest boot (FlipBase: first frame 2.3 to 3.0 s on its boot counter on RK3566, whose zero is about 0.5 s after power-on); `make legal-info`; a first build of a few hours, about 30 GB for FlipBase's base. Every panel, pad, sleep, audio and Wi-Fi quirk is ours to carry, and every emulator recipe is ours (Buildroot upstream packages none of the v1 set: no RetroArch, libretro core, PPSSPP, ScummVM or mupen64plus; Stella is its only console emulator) | **Chosen** (decision 1, 2026-11-25) |
| (e) Yocto | none; no maintained handheld BSP layer *(guess)* | 140 GB, 32 GB RAM, 16 cores recommended; steepest learning curve | No |
| (f) Thin image on vendor BSP kernels (muOS, BaseOS style) | H700, A133 | We would ship GPL kernels whose source we can't supply; framebuffer-only GPU on H700 | No |

**Why FlipBase is the template.** It is MIT. It is a Buildroot external tree on the ARC's SoC, pinned to Buildroot 2026.08.2, Linux 7.3.5 and U-Boot 2026.10, with BusyBox init, libudev-zero, an EROFS root, a raw kernel FIT read by partition name, A/B updates by GPT flip, offline tests, and CI on an arm64 runner. It has measured every step of its boot (section 9.2).

**What FlipBase is not:**

- **Multi-board.** `build.sh` hard-codes the board (`my355`: the defconfig path, image names, the SDK prefix), `tools/mkfit.py` holds the Flip's DRAM map and a `MY355FIT` header, `root=/dev/mmcblk1p3` is baked into the DTB, and the U-Boot script calls a board command, `my355`. Making the tree multi-board is the first task of Phase 2.
- **An upstream.** It has one active maintainer (175 of its 213 commits; 15 stars; created 2026-08-23, last push 2026-11-22). Tag `v2.0.0` is the move to mainline; the newest published release is still v1.3.1, on the vendor kernel. Thimsy forks it once, at a pinned commit, and takes later fixes by hand (decision 19).
- **Clean in every file.** Its `NOTICE` lists files that remain the work of BaseOS, which has no licence file (section 3.3). The owner's direction (open question 26): don't weigh the licence; most of those files are replaced anyway, and the four small C files are used as-is.

**ROCKNIX's role.** It stops being the base. Its hardware layer becomes a tracked upstream: where most mainline handheld device patches, out-of-tree device trees and per-device quirks live, read by `thimsy-import` at a tag Thimsy chooses (section 3.2). Its runtime is not used.

**Device layer: decided 2026-11-25 (decision 21), option (g).** The owner's goal is the broadest device support, initially ROCKNIX's coverage. Asked how to reach it, the owner chose (g). The rest were considered and not chosen:

| Option | Verdict |
|---|---|
| **(g) Thimsy Base + importer:** one Linux flavour; `thimsy-import` reads ROCKNIX at a Thimsy-chosen tag and writes kernel patch series, device trees, DTB and firmware lists and device facts; U-Boot and the boot chain stay Thimsy's; families arrive in waves | **Chosen** (decision 21) |
| (h) Hand imports per family: pick, renumber and rebase ROCKNIX's patches onto Thimsy's own kernel pin, as Batocera and Knulli do | Not chosen: at 6 to 10 weeks a family *(guess)*, 14 more families are out of reach for one developer, and hand-picked patches drift |
| (i) Nano as a ROCKNIX frontend package (a Wayland client under ROCKNIX's Sway) for the long tail, alone or before the base reaches a family | Not chosen: two Nano display targets and two interop paths, and the owner wants one Linux flavour |
| (j) Back to a ROCKNIX downstream (option (a) above) | Not chosen: gives up the 3 s target and brings systemd, D-Bus and InputPlumber back |
| (k) ROCKNIX-built hardware packs: run ROCKNIX's own build for the kernel, U-Boot and firmware, consume the outputs | Not chosen: patch points in ROCKNIX's build scripts return, two toolchains, and `make legal-info` no longer covers the kernel. Its one good idea, comparing against ROCKNIX's release images, is kept as a CI check (section 3.2) |
| (l) Batocera's board tree as the upstream | Not chosen: a second-hand copy of the same patches, renumbered and on Batocera's kernel schedule; its recipes are LGPL-3.0+ |

### 3.2 What the Thimsy Base repository contains

A new repository, `thimsy-base` *(the name is part of decision 6)*, forked from FlipBase at a pinned commit:

```
thimsy-base/
  external.desc, external.mk, Config.in   the BR2_EXTERNAL tree (name THIMSY); MIT
  build.sh                                Buildroot in Docker; one target per run (a tier, a GPU pack or a
                                            family's hardware pack); no board names in it
  configs/thimsy_<tier>_defconfig         one userland per CPU tier: v8.0, v8.2, later v9
  configs/thimsy_<family>_hw_defconfig    one hardware pack per family; includes the generated fragment
  board/<family>/common/                  Thimsy's own per-family files: rootfs overlay, rcS hooks
  board/<family>/uboot/                   Thimsy-owned U-Boot config and boot script for U-Boot families
  thimsy/<family>/overrides/              Thimsy's own data on top of the generated data, small
  generated/<family>/                     written by thimsy-import, never edited by hand: defconfig
                                            fragment, DTB list, firmware list, product catalog,
                                            device.json facts, the ROCKNIX commit and the hash guards
  package/                                our own recipes: thimsy-nano, thimsy-session, thimsy-padd,
                                            thimsy-fonts, thimsy-data, retroarch, libretro-*, ppsspp,
                                            scummvm, mupen64plus, the GPU packs
  src/                                    small C tools: splash, boot clock, GPT slot, card helpers
  tools/                                  FIT, GPT, image, pack and update-file packaging
  tools/thimsy-import/                    the importer: generator, hash guards, the release-image check
  tests/                                  offline tests and importer tests, run in CI
  docs/                                   boot chain, card layout, boot time, one page per family
  gpl/                                    GPL-2.0: everything under it
    generated/<family>/linux/patches/     the flattened series thimsy-import writes, ROCKNIX's file
                                            names kept, each with a one-line provenance note
                                            (ROCKNIX path and commit)
    generated/<family>/linux/config       ROCKNIX's kernel config for the family, before Thimsy's fragment
    generated/<family>/dts/               ROCKNIX's out-of-tree device trees
    board/<family>/linux/patches/         Thimsy's overlay: FlipBase's RK3566-generic patches, ours
    board/<family>/uboot/patches/         the Thimsy-owned U-Boot series (FlipBase's, ours)
    package/                              recipes ported from GPL sources (rocknix-joypad, ROCKNIX's
                                            package.mk files)
```

**How the importer works** (decision 21). `thimsy-import` reads one ROCKNIX tag and, per family, writes:

- the kernel patch series, flattened in the order ROCKNIX's `scripts/unpack` applies them (the project's mainline patches, then `mainline-rockchip` and the version directory, then `devices/<FAMILY>/patches/linux`), **keeping ROCKNIX's file names** so the next tag's diff maps one to one;
- ROCKNIX's kernel config for the family as the base config, plus `CUSTOM_DTS_DIR` and the `INTREE_DTS_NAME` list from `config.xml`;
- the DTB list and product catalog from `config.xml` (sub-images, DTB names, `fdt` and overlays);
- the firmware list from `kernel-firmware.dat`, filtered by the firmware policy (decision 24);
- **device facts as data, written fresh**: the `KEY="value"` lines of ROCKNIX's quirk files (`DEVICE_HAS_TOUCHSCREEN`, `DEVICE_PWM_MOTOR`, `ALSA_PRIMARY_CARD`, playback paths, LED and fan paths) and the pad's source nodes and button codes become `device.json` rows. No quirk script is copied: 563 of the 668 quirk files are GPL-2.0 or GPL-2.0+, 21 are Apache-2.0, and 405 depend on ROCKNIX's `profile.d` runtime. No InputPlumber YAML is copied either: those files have no SPDX header and are written to InputPlumber's GPL-3.0 schema, so their licence is unverified. Device directories are keyed by device-tree `model` or DMI string, as ROCKNIX keys them.

**What is consulted, not imported.** ROCKNIX's U-Boot, TF-A and rkbin recipes and U-Boot patches are read for each family's versions, defconfigs, DDR blob and BL31 names. **U-Boot and the boot chain stay Thimsy's**: FlipBase's 2026.10 series carries the 3 s target's largest wins (the SD clock fix, about 1.0 s; data cache before relocation, 570 → 40 ms), and ROCKNIX's RK3566 U-Boot is v2026.01.

**Imperative code is hand-ported once and guarded.** ROCKNIX's build logic that is not data (each family's case branch in `packages/linux/package.mk`, with its implicit patch-directory rules, the `generic-dsi` injection and the per-family `CONFIG_EXTRA_FIRMWARE` blocks; `u-boot*/package.mk`; `bootloader/*`; `quirks/package.mk`) is ported by hand into Thimsy's recipes. The hand-port copies what ROCKNIX actually does, not what its code appears to mean. One example: the condition that adds `rocknix-joypad`'s patch directory quotes its regex, and bash matches a quoted `=~` pattern literally, so that directory is never added. The importer records the ROCKNIX commit and a hash of each block, per family's case text rather than per file, and stops the import when one changes. `packages/linux` takes about 13 commits a month, so whole-file hashes would fire constantly.

**Thimsy's kernel fragment** sits on top of ROCKNIX's full config (about 6,900 to 8,900 lines on the ARM families; RK3566 is 7,511, with 1,779 `=y`). It blanks `CONFIG_INITRAMFS_SOURCE` (`"@INITRAMFS_SOURCE@"` in every ROCKNIX config but SDM845's, which holds a developer's absolute build path), turns on EROFS (`# CONFIG_EROFS_FS is not set` in RK3566's), builds in the pad HID drivers (`JOYSTICK_XPAD` is `=m` there; nothing loads modules on hotplug), keeps Panfrost and Freedreno, and trims what the boot path does not need (ROCKNIX's RK3566 config builds in XFS, BTRFS, NFS, CFG80211, USB and sound). The fragment will be large *(guess)*. A conformance check fails the build when an imported config flips a Thimsy-critical option.

**The release-image check (CI).** ROCKNIX's kernels have `CONFIG_IKCONFIG=y`. For each family and tag, CI downloads ROCKNIX's release image (about 1.3 to 1.6 GB each, about 24 GB a tag), runs `scripts/extract-ikconfig` on its kernel, and compares that config with Thimsy's **pre-fragment** config, and its DTBs (through `dtc`) with Thimsy's. Comparing after the fragment is not well defined once `olddefconfig` has resolved dependencies. This catches a mistranslated imperative block without running ROCKNIX's build.

**Credit and provenance** (open question 22, answered 2026-11-25). About 160 of ROCKNIX's 580 kernel patches have no `From:` line (a census of `next` at `0f4806b` finds 158 to 160); 547 of the 580 are under `devices/*/patches/linux`, and 33 are shared. The owner: "I don't care who wrote it. Use the ROCKNIX license." So no author is recovered: every imported patch is used under ROCKNIX's licence (GPL-2.0, in `gpl/`), and the importer writes a one-line provenance note on each (its ROCKNIX path and commit). Headers that exist are kept. Upstreaming a header-less patch is out of scope.

**Build steps per release** (decision 25, proposed):

1. **One shared SDK and toolchain:** aarch64 glibc. AMD64 separately, for QEMU.
2. **A userland per CPU tier:** v8.0 (A35, A53, A72, A73: RK3326, H700, RK3399, RK3576, S922X, SM6115), v8.2 (A55, A75, A76: RK3566, RK3588, SDM845, SM4450, SM6450, SM8250), and later v9 (X3, X4, A710: SM8550, SM8650, SM8750), which starts folded into v8.2. Each is one EROFS: Nano, RetroArch, cores, standalones, libraries. Generic tuning costs emulators little *(guess: under 5 to 10%)*; NEON dynarecs are hand-written either way.
3. **GPU packs**, kept out of the shared root so a card reads only its own driver (FlipBase's 0.45 s GPU readahead shows library size is on the boot path): Mesa Panfrost (Panthor included) and Mesa Freedreno with Turnip; a libmali pack as well if decision 11's Phase 0 A/B picks libmali. Mesa needs host LLVM; section 10 says how the packs share it.
4. **A hardware pack per family**, from the generated and Thimsy-owned files: kernel, modules, DTBs, U-Boot, TF-A and rkbin or the OpenABL bundle, firmware list, board data.
5. **A card per device**, with no compilation: tier root, GPU pack, hardware pack and `device.json`. Decision 1's per-device card stands; who puts it together, the build or the installer, is decision 13.

Revisions that can't be detected (H700 panels and DDR type; RK3326 EE clones; SM8250's Visionox panel) are typed installer questions (section 8.4).

**Two boot contracts:**

- **U-Boot families** (Rockchip, Allwinner, Amlogic): FlipBase's raw FIT read by partition name, behind a Thimsy-owned U-Boot.
- **EFI families** (all 8 Qualcomm, deferred with wave 3; AMD64 in QEMU): a FAT ESP with one Boot Loader Specification entry, `\loader\entries\thimsy.conf`, that names the device's devicetree, and `loader.conf` with timeout 0. OpenABL (v1.2, as ROCKNIX pins it) boots BLS Type #1 entries, UKIs and `extlinux.conf`; an entry's own devicetree takes precedence over its device-model menu, and timeout 0 boots the default entry at once, so a per-device card needs no menu step. ROCKNIX moved its Qualcomm images from `boot.img` to BLS on 2026-10-24 (PR #3547, opened 2026-10-22).

**What is taken from where:**

| Source | Taken | Not taken |
|---|---|---|
| FlipBase (MIT) | The tree layout and Docker build; `/init` with BusyBox init and the `rcS`/`rcK` discipline; libudev-zero; the EROFS root; the raw FIT read by partition name; GPT A/B updates; the `bootclock` and frame-probe instrumentation; the GPU packages (Mesa Panfrost and PanVK installed beside libmali, a `libmali.so.1` shim, the `gpu=` switch). Its RK3566-generic kernel patches: rk356x idle states, missing `dma-names`, RK3568 DMC devfreq, cpufreq nvmem, the pm-config sleep driver, a Panfrost PM fix, the fbdev mode commit at bind, the mmc tuning timeout. Its U-Boot patches 0003 to 0007: unaligned access for decompressors, the RK3568 SD clock divider, data cache before relocation, 1608 and 1800 MHz CPU rates, zstd at `-O2` | The `stock/` kernel layer (13 patches) and the `modetest` and `amixer` wrappers, which exist so NextUI binaries built for the vendor kernel run unmodified; the preloader installer and stock-firmware harvesting (the Flip's SPI NAND preloader has no counterpart on the ARC); the `my355` board command |
| FlipBase: the files its `NOTICE` names as BaseOS's | The four small C files, `fbsplash.c`, `gptgrow.c`, `gptslot.c` and `gpt.h`, used as-is and rewritten when convenient *(guess: a few days)*. The owner's direction (open question 26): don't weigh the licence; files with no stated licence are treated as following BaseOS's | Replaced by Thimsy's own parts: `flash-card.sh` by the installer, `flipbase-update` and `mkupdate.py` by Thimsy's update, `flipbase-config` (with its `flipbase.conf` template and `tests/test-flipbase-config.sh`) by Nano's settings. BaseOS has no licence file, so default copyright applies to all of them |
| ROCKNIX (GPL-2.0), through `thimsy-import` at a Thimsy-chosen tag | `config.xml` (sub-images, DTB lists, `fdt` and overlays); `devices/<FAMILY>/options`; the family's case branch in `packages/linux/package.mk` (version, patch directories); the kernel patch series (580 files: 547 under `devices/*/patches/linux`, 33 shared; 36 for RK3566, 31 for H700) flattened with ROCKNIX's file names; out-of-tree device trees (for example 15 H700 Anbernic ones, with the rev6 and v2 panel variants); `linux/linux.aarch64.conf` as the base config; `kernel-firmware.dat`; `filesystem/` overlays (ALSA UCM, sysctl; udev rules translated to `rcS` hooks); `rocknix-joypad` and the MCU pad drivers | Imported under `gpl/generated/`, under ROCKNIX's licence, each with a provenance line (ROCKNIX path and commit); no author recovery for the about 160 bare diffs (open question 22). Imperative blocks are hand-ported once, with hash guards |
| ROCKNIX's U-Boot, TF-A and rkbin recipes and U-Boot patches (19 across families) | Consulted for versions, defconfigs, DDR blob and BL31 names | Not imported: U-Boot and the boot chain are Thimsy's (FlipBase's series) |
| ROCKNIX quirk files (`packages/hardware/quirks/devices/<name>/`, 93 devices; `quirks/platforms/`, 16) and InputPlumber device and capability YAML | The facts: `KEY="value"` lines, GPIOs, LED and fan paths, pad source nodes and button codes, written fresh as `device.json` rows | The scripts and the YAML. Sysfs actions (governors, fan curves, thermal trips, PWM rumble export) are rewritten once as Thimsy's `rcS` hooks; per-game configs and remaps are written fresh |
| ROCKNIX package recipes (GPL-2.0, LibreELEC format) | Configure flags, patches and per-SoC options for RetroArch, cores and emulators | Ported into Buildroot `.mk` files under `gpl/package/`, with credit (open question 23, recommended). Each core is built from the commit the Android build's `retroarch-cores.lock` pins, not ROCKNIX's |
| Batocera (LGPL-3.0+), Recalbox (non-free), Knulli (unclear) | Which versions, flags and patches emulators need on bare KMS (for example Batocera's PPSSPP `003-fullscreen_drm` and RetroArch `006-drm-conn`) | Their recipe files: Thimsy writes its own (section 3.3). A patch to a GPL emulator may be taken, since a patch carries the patched software's licence |
| Mainline Linux and U-Boot | The ARC-D and ARC-S device trees (`rk3566-anbernic-rg-arc-d.dts`, `rk3566-anbernic-rg-arc-s.dts`, and `rk3566-anbernic-rg-arc.dtsi`, which includes `rk3566-anbernic-rgxx3.dtsi`); U-Boot's `anbernic-rgxx3-rk3566_defconfig` | - |

**The first import, for the ARC, is by hand** (Phase 2a), where FlipBase's series dominates anyway; the importer is built afterwards and must regenerate it (section 12). The kernel side is small, because both device trees are in mainline:

| ROCKNIX patch (RK3566, `next`) | What it does | Note |
|---|---|---|
| 0014 | st7701 RG-Arc mode: clock 25600 → 26000 kHz, vertical porches 10/4/16 → 8/4/13, for 59.96 Hz and more visible rows (one row is still missing) | full git header (sydarn, 2024-04-29) |
| 0025 | ARC-D touch: `touchscreen-inverted-y` becomes `touchscreen-swapped-x-y` | bare diff: used under ROCKNIX's licence, with a provenance line |
| 0029 | `rgxx3.dtsi`: the adc-keys `BTN_MODE` becomes an `adc-keys-home` `BTN_BACK` at 41000 uV; reaches both ARCs through the include | bare diff: used under ROCKNIX's licence, with a provenance line |
| 1011 | Goodix touch usability fixes | |
| 1001 | rk356x idle states | FlipBase carries its own version; one of the two is kept |
| 0008, 1012 | the mali-bifrost device-tree node; `mali_kbase` owns the GPU clocks | only if libmali is kept (decision 11) |

Still unverified on mainline for the ARC: the Wi-Fi and Bluetooth chip and its firmware, audio routing (card name `rk817_ext`), deep-sleep current, and the panel's init waits (FlipBase found a hex `wait=` bug on the Flip that cost 288 + 50 ms). The mainline device trees name no Wi-Fi chip (SDIO on `sdmmc2`; an RTL8821CS *(guess)*) and have no Bluetooth node; audio is fully described. Open question 25 lists the facts; Phase 0 (b) checks the rest.

**U-Boot for the Anbernic RK3566 boards.** Mainline `anbernic-rgxx3-rk3566_defconfig` detects the board by ADC channel 1 and sets `fdtfile`; its documentation says "eMMC booting has not been tested". Mainline SPL's boot order on rk356x is `same-as-spl`, `sdmmc0`, `sdhci`. ROCKNIX builds it from v2026.01 with rkbin's `rk3568_ddr_1560MHz_v1.26` as TPL and its own TF-A BL31. Thimsy adds FlipBase's U-Boot series and the RG353PS fork's two Anbernic fixes: no 200 ms start-up buzz (an `mdelay(200)` in `board/anbernic/rgxx3_rk3566`, run on every rgxx3 board) and no panel-ID probe (34.5 ms; mainline runs it only on the RG353P, V, PS and VS, whose `detect_panel` is set, not on the ARCs) (section 9.3).

**Kernel version (decision 22, proposed): ROCKNIX's version for each family at a Thimsy-chosen ROCKNIX tag.** There is no single Thimsy kernel: on `next` today that is vendor BSP 6.1 for RK3588; 6.18.54 for RK3399; 7.1.2 for RK3326 and SDM845; 7.2 for H700, SM4450, SM6450, SM8250, SM8650 and SM8750; 7.3.4 for S922X; 7.3.5 for RK3566, RK3576, SM6115 and SM8550. ROCKNIX's patches stay importable without rebasing only while Thimsy uses the version they were written for. FlipBase's RK3566 series and Thimsy's own boot patches are an overlay, re-checked on every bump. The rule's terms:

- **Thimsy chooses the tag and may skip tags freely.** The real bump rate is high: `packages/linux/package.mk` took 58 non-merge commits on `next` since 2026-05-25, about 40 of them version changes (about 6 a month across families), 5 of them reverts or rollbacks. S922X alone went 6.18.38 → 6.18.43 → 7.1.9 (reverted) → 6.18.45 → 6.18.49 → 6.18.54 → 7.2.9 → 7.2.11 → 7.3.1 → 7.3.4 in five months. Following every bump is not affordable; a family moves when a tag brings something it needs, or at most a few times a year *(guess)*.
- **Unpatched .0 kernels.** H700 and five Qualcomm families pin `PKG_VERSION="7.2"` with no stable point release; H700 has been on 7.2.0 since 2026-08-17. Whether Thimsy may apply stable updates on top, at the cost of the one-to-one import diff, is open question 34.
- **RK3566's version is decided at the start of Phase 2a**, not now: 7.3.5 is `next`-only and three days old, and tags `20261201` and `20270101` should exist by then (open question 30).
- **If ROCKNIX's monthly tags stop.** Monthly stable tags exist only from `20260601` to `20261101`; before that there were 12.5 months with none. Proposed: a family stays frozen on its last tag; if it needs a fix, it imports a named `next` commit after a soak on its hardware-in-the-loop unit and the release-image check is skipped for that import (open question 33).

**Pins and updates.** Buildroot, Linux and U-Boot are pinned by version and hash (FlipBase pins Buildroot by sha256); each release also pins the ROCKNIX tag and every pack's hash. They move deliberately, behind the conformance tests of section 11, not on a schedule. CI runs `thimsy-import` nightly against ROCKNIX `next` as an early warning, with no artifacts. Device fixes go upstream where possible; FlipBase marks its U-Boot 0004, 0005 and 0006 as worth sending. The README credits FlipBase and ROCKNIX (each imported patch carries its provenance line) and says "not affiliated".

### 3.3 Licences

| Piece | Licence | Consequence |
|---|---|---|
| The Thimsy Base build tree: our recipes, scripts, overlays and tools | MIT, like FlipBase (decision 6) | Holds only files Thimsy writes or takes from MIT sources (FlipBase, with its notice kept). Published. |
| FlipBase | MIT (`LICENSE`, `NOTICE`) | Forked with its notice. The files its `NOTICE` names as BaseOS's are BaseOS's (next row). Zetarancio's Miyoo Flip reverse-engineering repository, on which FlipBase's mainline port builds, has no licence file either; kernel patches from it are GPL by nature and keep their author headers. |
| BaseOS | **no licence file**, so default copyright applies | The owner's direction (open question 26): don't weigh the licence; files with no stated licence are treated as following BaseOS's. Most of FlipBase's BaseOS files are replaced anyway; the four small C files are used as-is and rewritten when convenient (section 3.2). |
| Buildroot | GPL-2.0-or-later. Patches it bundles take the licence of the software they patch. Its `.mk` files have no per-file headers, so a copied recipe brings GPL-2.0+ with it | Buildroot is fetched at a pinned release, not vendored. A copied Buildroot recipe goes under `gpl/`. Buildroot's developers count Buildroot itself among the "scripts used to control compilation and installation" (their stated opinion, not a licence term); the manual does not name external trees, but ours is the same kind of material *(our reading)*, so both are published with each release. |
| Kernel, U-Boot and device-tree patches: ours, FlipBase's, ROCKNIX's; ROCKNIX's kernel configs | GPL-2.0 | Under `gpl/`. ROCKNIX's are used under ROCKNIX's licence, each with a one-line provenance note (ROCKNIX path and commit) the importer writes; headers that exist are kept. About 160 of its 580 have no `From:` line (0025 and 0029 among them); no author is recovered (open question 22, the owner's answer), and upstreaming those is out of scope, since the kernel's DCO needs the author's `Signed-off-by`. |
| ROCKNIX's own scripts and recipes | GPL-2.0 (`LICENSE.md`; per-file SPDX headers; 21 quirk files Apache-2.0) | A recipe ported from a ROCKNIX `package.mk` goes under `gpl/package/`, with credit. Quirk scripts are not copied; their facts are written fresh as data. |
| `device.json` facts from ROCKNIX's quirk files and InputPlumber YAML | facts, written fresh; ROCKNIX's InputPlumber YAML has no SPDX header and follows InputPlumber's GPL-3.0 schema, so its licence is unverified | No YAML or script text is copied; `thimsy-import` writes Thimsy's own rows from the facts. Kept in `generated/`, outside the `gpl/` area only because nothing in it is copied text *(our reading)*. |
| `rocknix-joypad` | GPL-2.0-or-later (SPDX headers; the repository has no `LICENSE` file) | A kernel-module recipe under `gpl/package/`, credited by its SPDX headers and `CREDITS`. |
| ROCKNIX's MCU pad drivers (`rsinput`, `retroid`, `mangmi`, `trimui-mcu`, the SPI MCU) | GPL-2.0 | Imported as kernel patches under `gpl/generated/`. |
| InputPlumber | GPL-3.0 | Never in the image (decision 23). |
| OpenABL | GPL-3.0 | Shipped as a separate binary for the Qualcomm families, with its source published at every release, once wave 3 is allowed (it writes a bootloader to internal storage; section 1). |
| ROCKNIX branding and images | CC BY-NC-SA 4.0, for branding and images only. Settled from `LICENSE.md`: the README's bare NonCommercial/ShareAlike list is this clause with its heading dropped. The FAQ agrees: "ROCKNIX only prohibits commercial use through our branding" | Don't reuse or remix. Thimsy's own logo is already BY-NC. |
| Batocera recipes | LGPL-3.0-or-later (`COPYING`), with Buildroot-style exceptions | They can't become MIT or GPL-2.0-only, so they are not copied: reference only. The wiki's line against commercial forks is policy, not a licence term. |
| Knulli recipes | `COPYING` is Buildroot's GPL-2.0+ text, but the package tree derives from Batocera's LGPL-3.0+ files | Reference only. |
| Recalbox | A custom non-free licence: no sale or commercial use without authorization; modified redistributions must include complete source | Nothing copied; reference only. |
| The bundle | "This distribution includes components licensed for non-commercial use only" (ROCKNIX's `LICENSE.md`; true of Thimsy's bundle too) | Same stance as the Android build: never sold, never bundled with hardware. |
| Kernel, U-Boot, BusyBox and every other GPL package | GPL | Each release publishes `make legal-info`'s output beside the images: the source of every target and host package with its applied patches and a `series` file, manifests with versions and licences, and the licence texts. Also the Buildroot release tarball and the external tree at the release tag, because `legal-info` saves neither (nor some external-toolchain sources). Every package in our tree sets `_LICENSE` and `_LICENSE_FILES`; the output is declarative and is checked by hand. Packages marked `_REDISTRIBUTE = NO` are left out, so each non-commercial core is reviewed for it. |
| Nano | Apache-2.0 (GammaOS Next's licence; Thimsy's files carry Apache headers) | A separate program in the image ("mere aggregation"), fetched by a recipe. **Never paste Nano code into GPL-2.0 files or the reverse**: Apache-2.0 is not compatible with GPLv2-only. |
| Nano's GPL parts | `NanoAc3.h` and `NanoOskKanji.h` carry GPL headers; `liba52` is GPL | An existing question for the Android build too. The Linux build can drop AC-3 (media players are cut from v1). The Kanji table (derived from SKK-JISYO) needs an answer. `plan_remove_unlicensed_files.md`'s audit does not mention `NanoAc3`, `liba52` or `NanoOskKanji` yet; it has to be extended (risk 11). |
| Mali blob (`libmali`, `mali_kbase`) | Arm EULA: redistributable with conditions, revocable | Leaves this table if Panfrost wins decision 11's Phase 0 A/B and the Mesa-only extension stands. Shipped only if the A/B picks libmali (then its EULA, the out-of-tree `mali_kbase` and per-family packaging apply): `libmali` g29p1 fetched from `JeffyCN/mirrors` at a pinned commit, on ROCKNIX's out-of-tree `mali_kbase` r54p2, as FlipBase does. Panfrost alone removes the blob and the EULA. |
| Rockchip DDR init and BL31 (`rkbin`) | Rockchip binary licence | The DDR blob is in every RK3566 image: mainline U-Boot needs it as TPL. The BL31 is the unit's own vendor BL31, read from the unit and never redistributed (decision 18, decided for RK3566); rkbin's binary BL31 is the fallback for a unit with none to read (to confirm in Phase 0). Read the terms once. |
| Wi-Fi, Bluetooth, GPU and DSP firmware | per file in `linux-firmware`; ROCKNIX's `extra-firmware` is marked `proprietary` with no licence file; the SM8750 AW88261 calibration blob is "extracted from the stock Android image" | Decision 24 (proposed): ship only `linux-firmware` files with a known licence, each licence read once; fetch the rest from the user's own device at install time onto a partition; leave out a device whose firmware can be neither shipped nor fetched. |
| H700 `.panel` files | no stated licence; derived from vendor DTBs | Not shipped until their provenance is known; under decision 24, rewritten from facts or fetched. |
| Amlogic FIP (S922X) and Qualcomm XBL/ABL | vendor binaries | The FIP is handled like rkbin: read the terms first. XBL and ABL stay on the unit and are never shipped. |
| Non-commercial cores | Snes9x, Genesis Plus GX, PicoDrive, FBNeo, MAME 2003 | Shipped as on Android (never sold), with their `_LICENSE` fields set honestly for `legal-info` (open question 23, recommended). |
| DuckStation | CC BY-NC-ND since 2024-09; "pre-configured settings and packages are considered modifications" | **Can't be shipped configured.** PlayStation stays on Beetle PSX HW, PCSX ReARMed and SwanStation. |
| DraStic, BigPEmu | freeware terms; ROCKNIX bundles them | Not in v1. |
| ES-DE | MIT on Linux (the Android app is paid and partly closed) | Can be built and shipped (section 6.1). |
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
   platform/linux/                KMS/GBM, evdev, config store, fork/exec, sysfs, wpa_ctrl   <- this plan
   compat/                        header shims so shared files compile off Android           <- this plan
 scripts/build.sh, mods/*.sh    the Android firmware build (unchanged)
 thimsy-base (new repo)         Buildroot tree (MIT): boards, packages, tools; gpl/ patches  <- this plan
 thimsy-os-installer            engine, app, catalog (generated from the descriptors)
 LIBRARY-FORMAT.md + vectors    the card contract (android_launcher_plan.md §3; section 7 here)
```

Rules:

- **Nano's shared code never includes an Android or Linux system header.** It calls `NanoPlatform.h`, or a `compat/` shim.
- **Device facts come from one descriptor** (`multi_device_plan.md` §4.2). The Linux build adds fields to it (section 8.3).
- **A system, a core or an emulator is named in one list.** Each platform's label, package or binary is a column of that list, not a copy.
- **The resolver runs once per build.** `scripts/resolve_package.py` produces the resolved manifest; the Android build and the Linux build are two consumers of it.
- **Licence boundaries are directory and repository boundaries.** `thimsy-base` consumes this repository's lists, scripts and Nano source at a pinned commit. Nano (Apache-2.0) is fetched by a recipe and never pasted into the MIT tree or its `gpl/` area. Nothing from `gpl/` or from ROCKNIX comes back here.

### 4.2 Today's files, by reuse class

| Today | Class | For Linux |
|---|---|---|
| `mods/retroarch-cores.txt` (with `D -` defaults), `retroarch-cores.lock`, `scripts/refresh_cores.sh` | shared list; the lock is Android's | A `linux=` column where the core name or default differs. v1 builds cores from source with the Thimsy Base's own recipes, each at the commit the lock pins (open question 23); `wanted_cores()` in `modlib.sh` hard-codes `_libretro_android.so` and needs a platform suffix |
| `mods/nano-systems.txt` (41 systems), `scripts/nano_systems.py` | shared; players are Android (Daijisho catalog ids, intents) | A per-platform player column |
| `tools/gen_esde_emulators.py` (inside the Nano source tree, P:163379) | shared; reads ES-DE's Android tables today | Also read `linuxarm` (and `linux` for an x86 target) |
| `mods/esde-emulators.txt`, `scripts/esde_config.py` | **1.2 base with the real ES-DE app only** (`mods/37-esde.sh` is their only caller and refuses 1.4.4) | Needed on Linux only if decision 9 ships ES-DE |
| `mods/bundled-roms.txt`, `pack-in-media.txt`, `pack-in-art/`, `rom-files/`, `rom-licenses/`, `scripts/bundled_roms.py` | shared data; the script rebuilds GammaOS's ROM pack and requires it as input | Two changes: the two `upstream` games become `add` lines with URL and hash, and the script gets a no-input-pack mode and a root argument |
| `mods/hotkeys/*.json`, `mods/device-keys.json` | shared | Device matching on Linux by device-tree `model`; pad identity differs (section 5.3) |
| Theme repack in `mods/50-nano.sh`, `scripts/check_system_art.py` | shared | as is |
| `ps3xmb/i18n/*.json` (21 languages), `ps3xmb/daijishou/*.json`, controller drawings | shared (inside the Nano source) | as is |
| `mods/retroarch-shaders/rg_cube_*` | shared; `multi_device_plan.md` §5 renames the files, `device_profiles_plan.md` moves panel shaders to `packaging/displays/<WxH>/shaders` | follows those plans, which disagree on the new names (`multi_device_plan.md` §5: `thimsy_4_3_cropped` and similar, old names kept as aliases; `device_profiles_plan.md`: `720x720_*`); whichever lands first settles it. `.slang` presets need a Vulkan or glcore RetroArch driver |
| Boot sound, logo sources, `tools/bootanimation` | sources shared; formats per platform | Nano draws the Linux splash |
| `scripts/modlib.sh` | half shared (payload tar helpers, `buildbot_get`); half Android | split |
| `pactool.py`, `lptool.py`, `erofs_image.py`, `avb_footer.py`, `sprd_drm.py`, `patch_*`, `odm_sepolicy.py`, `bootlogo.py`, `apk_home.py`, `apps_list.py`, `tar_roots.py`, `zero_free_blocks.py`, `check_charger_binary.py`, `check_charger_rc.py`, `build_144.sh`, `build_android_games.sh`, `build_panvk_driver.sh`, `scripts/aosp/`; mods 05 (`ro.*` build properties), 10 (Android APKs and `_libretro_android` cores), 12, 15, 25, 27, 40, 45, 46 (ScummVM's Android entry), 47, 48 | Android only | - |
| Framework Java, SurfaceFlinger, MediaProvider, sepolicy, init.rc hunks of the patch | Android only | - |

A wholesale move to `data/`, `gen/` and `platforms/` folders was considered and dropped: it would contradict `device_profiles_plan.md` §7.1 and `multi_device_plan.md` §5, which edit these files where they are.

### 4.3 Getting Nano out of the patch

Today Nano exists only as GammaOS's tree at commit `0bc531e2d` plus the patch's hunks for `frameworks/base/cmds/gammaos-nano` (486 of the patch's 841 files). **The fetch already exists:** `tools/android-launcher/fetch_nano.sh` builds that tree for the launcher plan, and `tools/android-launcher/ndk_probe.py` lists non-portable calls. Its header records a constraint that matters here too: the upstream tree "holds file names Windows can't store", so it is never checked out on Windows.

1. **Reuse and extend those two tools** (moving them to a neutral path such as `tools/nano-tree/` when the Linux build needs them). The patch stays the source of truth. The Android firmware build is untouched. The APK and Linux builds compile the fetched tree with CMake.
2. **The seam and the compat shims land in the patch**, so all three builds see the same source.
3. **Later (decision 7):** make the extracted tree the source of truth and have the Android build copy it into the AOSP tree. The patch shrinks to the framework parts, and upstream GammaOS changes arrive as a three-way merge. Only once the seam has settled, because it changes how every backport in `BACKPORTS.md` is made.

**Where the work happens (the owner, 2026-11-25).** Implementation starts in a fork of this repository, the nano fork of Thimsy OS. The refactor (the seam and the shims, Phase 1) lands there first, then the Linux build. This repository keeps moving on its own, and the fork is merged back into it, or into Thimsy's mainline, later. That makes merging easy to keep possible, as a design rule: shims first and interfaces second (below); the Cube image byte-identical after every refactor step; and this repository merged into the fork often, so the two drift apart as little as possible. The Android build stays maintained separately (decision 3).

**Shims first, interfaces second.** `ndk_probe.py`, whose counts the launcher plan quotes, finds 985 property reads in 58 files and 114 shell commands (`system`, `popen`, `exec`) in 27. A plain grep of the same tree finds `ALOG*` 1,491 times in 70 files. Rewriting those call sites would make every later upstream merge conflict. Instead `compat/` provides `cutils/properties.h`, `log/log.h`, `utils/Thread.h`, `utils/Mutex.h` and `utils/StrongPointer.h` for non-Android builds, backed by a key/value store and `std::` types. Real `NanoPlatform.h` interfaces are cut only where behaviour differs: display, input source, launch, audio, storage, network, power, system actions.

### 4.4 Build system

- **Nano:** CMake, three targets: `nano_core` (static), one `nano_platform_*`, and the `thimsy-nano` executable. The 29 production files that already build on a host in `cc_test_host` targets become the first CTest suite, without `DsScreenLayout_test.cpp`: it needs `drastic-nano`'s layout sources (`:drastic_nano_layout_srcs`), which `fetch_nano.sh` does not fetch, and the DS player is dropped on Linux.
- **Linux image:** the Thimsy Base's `build.sh`: Buildroot in Docker, once per CPU tier, GPU pack and family hardware pack, then a per-device card (sections 3.2 and 10). `thimsy-nano` is one more package in the external tree, built with Buildroot's CMake support.
- **Android image:** unchanged `scripts/build.sh` (with `DEVICE=` once `multi_device_plan.md` Phase 1 lands).
- **CI:** one workflow per platform plus the card conformance tests (section 11).

### 4.5 Shared work with the sibling plans

Six plans now lean on the same foundations. Each piece should be built once.

| Piece | Owned by | This plan needs it for |
|---|---|---|
| `NanoPlatform.h` seam, Nano built outside AOSP | `android_launcher_plan.md` Phase 0 and 1 | everything in section 5 |
| Device descriptors | `multi_device_plan.md` Phase 1 | catalog entries, per-device pad and display facts |
| Layer resolver, profile names | `device_profiles_plan.md` Phase 0 and 1 | what each Linux image contains (section 8.4) |
| Emulator definitions with `storage` blocks | `user_emulators_plan.md` Phase 1 and 5 (not started) | launch and link rules (sections 6.4, 7.4). If that plan is later, this one lands a minimal definition schema itself |
| `face_layout` (the Nintendo or Xbox label convention in `device-keys.json`) | `controller_layout_plan.md` Phase 1 | per-device button labels; the ARC needs a new value (section 5.3) |
| `pack.json` format 2 | `multi_device_plan.md` §7.4; also the launcher plan's Phase 6 | disk-image packages (section 8.3) |
| Raw SD-card writer (the reserved `sdcard` crate, the SdCard backend) | `multi_device_plan.md` Phase 5 | this plan's Phase 4 builds it, which pulls that phase forward; whichever comes first builds it for both (decision 7) |
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
- **The ES-DE store is file I/O plus a few settings and state paths** (`NanoEsdeStore.cpp`: `persist.gammaos.nano.esde.dir`, `persist.gammaos.scraper.dir`, `sys.gammaos.nano.app_launched`, and state files such as `nano_esde_played.txt` under `/data/system`). It ports once the config store and the state root exist (section 5.2).
- **Nano has no binder interface of its own.** It talks to system_server and init through properties and files under `/data/system`.

### 5.2 Android-specific parts and their Linux replacements

The Thimsy Base runs the session as root, as FlipBase does: `/init` starts BusyBox init, `inittab` runs `rcS` once and respawns the session script, which starts `thimsy-session` and Nano. There is no systemd, no udev daemon (libudev-zero stands in for libudev), no D-Bus at boot, no PipeWire and no compositor. The replacements below follow FlipBase and RetroStoneOS, which both run this way, except that FlipBase starts `dbus-daemon` in the background at every boot, because NextUI's `audiomon` connects to the system bus whether or not Bluetooth is used; Nano has no such client. Line counts are *(guesses)*.

| Subsystem | Android today | Linux replacement | Effort |
|---|---|---|---|
| Display and GL | KMS ioctls with AHardwareBuffer/gralloc buffers imported through `EGL_NATIVE_BUFFER_ANDROID`; SurfaceFlinger fallback | One target behind `NanoDisplayTarget`: **KMS/GBM** with EGL on Mesa, Nano as DRM master on `card0`, handing the display to each emulator at launch (section 5.3; decision 4's recommendation). A Wayland or SDL target exists only for work on the PC (section 10) | medium |
| Panel rotation | sizes from the active mode; rotation untested off the Cube (`multi_device_plan.md` §6.8) | **One rotation contract, carried in three places** (section 5.3; decision 4, amendment proposed). The device's rotation comes from the DRM connector's panel orientation property, then `/sys/class/graphics/fbcon/rotate`, then a `device.json` override (the property went missing on the RG28XX, ROCKNIX PR #3353). Every client renders landscape; rotation happens once, at present time, in Nano's KMS target, one SDL3 KMSDRM patch (upstream SDL3 does not rotate, and SDL2 has no orientation code; confirmed) and one RetroArch `drm_ctx.c` patch (`video_rotation` alone leaves the menu unrotated). A KMS plane rotates where it advertises 90/270, a GPU blit otherwise. Touch is rotated from the same value. The ARC panel's mainline mode is 480x640, portrait, with `rotation = <90>`; RK3566's VOP2 rotates only on Cluster0, with AFBC buffers. RK3326, RK3399, RK3576, H700 and S922X have no 90/270 plane in mainline. For the Qualcomm families the contract is not confirmed: Mesa's display WSI does identity transforms only for Vulkan, so they get a per-family setting (section 5.3) | medium to hard |
| Fonts | `/system/fonts`: Roboto, DroidSans, NotoSansCJK, NotoNaskhArabic, NotoSansThai, NotoSansHebrew, NotoColorEmoji (`NanoMenuRender.cpp`'s `fontPaths`, P:51647; `NanoBootScreen`, P:10435) | A `thimsy-fonts` package (Apache-2.0 and OFL fonts, with replacements for DroidSans and NotoColorEmoji too) and a path mapping. The base has no other fonts; without the full set, Arabic, Hebrew and Thai render as missing glyphs. The Rodin replacement (`ASSETS.md`; TODO.md) must land before any Linux image ships | easy |
| Themes, XMB, OSK, text | GLES 2, FreeType, libpng, stb. Three exceptions: `AImageDecoder`, screen capture for the clock blur, the accelerometer | stb_image/libjpeg-turbo/libwebp; read back Nano's own framebuffer; drop the sensor | easy |
| Quick Menu over a game | second process (`--overlay`) drawing a SurfaceFlinger layer above the app; summoned by properties; hotkeys in `PhoneWindowManager`; input cut off through `drop_input` in InputDispatcher | section 5.3 | **hard** |
| Pad model | one built-in pad per device: `device-keys.json` matches the device by system properties (`ro.product.vendor.model`, `ro.board.platform` and others); `pads.json` and gammapad match pads by vendor and product id | section 5.3: `thimsy-padd` (decision 23, proposed) grabs each device's source nodes (`gpio-keys-control`, `adc-keys-home` and `pwm-vibrator` on the ARC; `rocknix-joypad` or an MCU driver elsewhere) and presents one uinput pad with one Thimsy identity; `device.json` carries the source list and button facts; a 6-button pad with no sticks on the first target | medium |
| System keys | Android's own (volume, power) plus `ThimsyHotkeyController` in system_server | `thimsy-session` reads evdev: `KEY_POWER` from `rk805-pwrkey` (the RK817 PMIC is a wakeup source in the ARC's device tree), the volume keys, `SW_LID` from `gpio-keys` where a device has a lid (H700 SP models), the quit hotkey. Brightness through `/sys/class/backlight` (`brightness`, `bl_power`). A tiny watcher powers off cleanly on a long press when Nano is not running (FlipBase's `pwrkeyd` idea). The hotkey state machine is ported from the Java, which already has host tests. About 300 lines plus the port | medium |
| Settings store | 539 `persist.gammaos.*` and 343 `sys.gammaos.*` property keys | **the same key names** in a config store: `persist.*` in one file under `/storage/.config/thimsy/` (atomic rename), `sys.*` under `/run/thimsy/`, change notice by inotify, a `thimsy-prop` CLI for scripts. The launcher plan needs the same store | medium |
| State files | 127 paths under `/data/system/nano_*` | `/storage/.config/thimsy/state/`, same file names | easy |
| Launching | `am start` argument strings; Nano writes an intent file, sets a property and exits; `RootWindowContainer` starts the activity | Nano stays resident and fork+execs the emulator in its own process group, with a direct argv from the emulator definition's Linux block (sections 5.3 and 6.4). No launch script. `NanoLaunch.cpp`'s token code is reused | medium |
| Memory | Nano exits when a game starts | On 1 GB devices a resident Nano must free its textures at launch, or exit and restart as on Android. Measure its resident size in Phase 2 | medium |
| Audio (menu sounds, chime) | AAudio, tinyalsa | **ALSA directly**, no sound server: alsa-lib's `dmix` as the default PCM, so menu sounds play while a game holds the card; `plug:bluealsa` when Bluetooth headphones are connected (RetroStoneOS's routing); `amixer` controls for volume. RetroArch uses its `alsa` or `alsathread` driver; SDL emulators get `SDL_AUDIODRIVER=alsa` and `AUDIODEV` from Nano at launch. About 300 lines of output and 200 of mixer | medium |
| Media engines (video, music, radio, IPTV) | AMediaCodec into a GL texture | FFmpeg, with V4L2 hardware decode later. **Not in v1** | hard |
| DS player, in-process runners | loads DraStic's Android `.so` through a fake JNI | **Dropped** as a DraStic runner. Nintendo DS runs on a melonDS core. `LibretroRunner` comes back in Phase 5 as the in-process libretro host for the Quick Menu (section 5.3); a DS screen layout through it is a question for then, not v1 | drop |
| Quick Resume | a property plus the runners | RetroArch auto save and auto load. Later | later |
| Emulator files on the card | `NanoSdMove` (SAF grants, `pm`, SELinux contexts), `NanoEmuSync` (three-way sync of app-private files) | mostly deletion: Linux emulators read ordinary directories (section 7.4). The adoption and re-pointing logic stays | medium |
| Storage and volumes | vold paths `/storage/<id>`, `/mnt/media_rw/<id>` (245 uses in 44 files), MediaProvider denylist | **keep the paths** (section 7.3); mounting done by `thimsy-session` | medium |
| Hotplug | `ueventd`, vold | devtmpfs creates the nodes. Nano already watches `/dev/input` with inotify. A small `NETLINK_KOBJECT_UEVENT` listener in `thimsy-session` handles block add and remove (about 200 lines). libudev-zero serves SDL and RetroArch; SDL keeps libudev enumeration through it, since `SDL_JOYSTICK_DISABLE_UDEV` falls back to capability guessing and would see raw source nodes. External-pad hotplug in SDL and RetroArch needs `mdevd -O 4` or libudev-zero's helper. Pad HID drivers (`hid-playstation`, `hid-nintendo`, `xpad`, `uhid`, `hidraw`), `uinput` and USB audio are built into the kernel, since nothing loads a module on hotplug without udev (FlipBase does the same); `rocknix-joypad` is an out-of-tree module, loaded by `rcS` at boot on the families that use it | easy to medium |
| System settings | shell: `settings`, `cmd wifi`, `svc`, `cmd bluetooth_manager`, `cmd alarm`; the `NanoNetBridge` property RPC | A system-actions table. **Wi-Fi:** `wpa_supplicant` through its UNIX control socket (Nano links hostap's BSD-licensed `wpa_ctrl.c`), BusyBox `udhcpc` for DHCP; not NetworkManager, and not iwd (upstream iwd needs D-Bus; the D-Bus-free fork eiwd last released in October 2024). About 600 to 900 lines. **Bluetooth:** BlueZ and bluez-alsa, which need a system bus, so `dbus-daemon` starts only when the user turns Bluetooth on (RetroStoneOS's `rsos-net`; FlipBase's `S40bluetooth` also starts the bus if it is not already running): "no D-Bus at boot", not "no D-Bus". Nano shells out to `bluetoothctl` first, as NextUI does (about 500 lines), or uses libdbus later (about 1.5k). BTstack is ruled out by its non-commercial clause. **Time:** the kernel sets the clock from the RK817's RTC, BusyBox `ntpd` refines it, `hwclock -w` after a manual set. `/etc/localtime` is a link in the read-only root to `/storage/.config/thimsy/localtime`, which Nano rewrites; it then calls `tzset()` and exports `TZ` to children (glibc's `localtime_r()` does not re-read the file *(guess, from its source)*). About 150 lines. **Brightness and volume:** sysfs and ALSA, as above | medium |
| Battery, charging, LEDs | health and lights HALs, with a sysfs fallback for battery | `/sys/class/power_supply`, `/sys/class/leds`; low-battery warning in Nano | easy |
| Sleep, power | `/sys/power/wake_lock`, `input keyevent 223/224`, `sys.powerctl`, `nano-powerwatch` | `thimsy-session` stops the game's process group (`SIGSTOP`), stops `wpa_supplicant`, writes `deep` to `/sys/power/mem_sleep` and `mem` to `/sys/power/state`, then reverses it: the NextUI and FlipBase sequence, about 0.7 s round trip on the Flip. RK3566 deep sleep is a Phase 0 item: FlipBase needed the vendor BL31's sleep mode (its pm-config kernel driver) and device-tree rail fixes to reach about 12 mA asleep instead of 26.6 mA. The BL31 is the unit's own vendor BL31, with FlipBase's pm-config patch (decision 18, decided for RK3566). Nano holds its own "busy" state (RetroStoneOS's status-pipe idea) in place of logind inhibitors. About 300 lines, plus per-board device-tree work | medium to hard |
| USB-C, dock, monitor modes | `NanoTypec` (plain sysfs), DisplayPort through the Unisoc driver, `GammaosScreenArrangement` | The ARC has mini-HDMI. **Docked in v1** (owner, open question 14): with HDMI connected, Nano and its games go to the TV and the panel is off; the emulator picks the HDMI connector. Mirroring later, by a per-game compositor or the display patches (FlipBase drives the panel and HDMI at once on separate VOP2 windows, with hot-plug). Other monitor modes later | v1 (ARC docked); the rest later |
| Network extras | libcurl (scraper, OTA check), Syncthing binary, SMB through `gammaos-sharefs` | the same libcurl; Samba, Syncthing and SSH (dropbear) are base packages started on demand, after the first frame | easy to medium |
| Update | `gammaos-ota` | A FlipBase-style A/B update: the update file is written to the spare half of each slot, verified by read-back, then the GPT entries are flipped; rollback after three failed boots; no signing key yet. Thimsy's updater is written anew, since FlipBase's `gptslot` and `mkupdate.py` are BaseOS's files | medium |
| Init, SELinux | `gammaos-nano.rc`, the `bootanim` domain, capabilities | BusyBox `inittab` with `rcS` (a budget under 1 s, everything slow in the background) and `rcK`; no SELinux | easy |
| Framework helpers | `NanoNetBridge`, `NanoRetroArchConfig`, `NanoUriGrant` (Java, in system_server) | folded into `thimsy-session` or dropped | medium |

### 5.3 The display stack, the Quick Menu and the pad

**Display (decision 4, reopened 2026-11-25).** The earlier recommendation, Nano as a Wayland client under ROCKNIX's Sway, left with ROCKNIX. The facts that shape the choice now:

- **DRM master.** One master per device at a time. Opening the primary node when there is none makes the opener master. `SET_MASTER` and `DROP_MASTER` need `CAP_SYS_ADMIN`, except for the process that opened the file when that file was master before (Linux 5.8 and later, `drm_master_check_perm`); a forked child never passes that check with its parent's file. Mode setting needs the current master. When the master's file closes, including on a crash, the device is free again.
- **FlipBase** runs with no compositor on RK3566: DRM fbdev emulation for the splash, the frontend as DRM master on `card0` through SDL's KMSDRM backend, with libmali or Panfrost (Panfrost loaded as a module after `rockchip-drm`, so `card0` stays the display), and the panel and HDMI at once.
- **RetroStoneOS's hand-off** is a pattern Nano can copy. The UI calls `drmDropMaster` (keeping its fd, buffers and state), fork+execs the game, which becomes master on open, and waits. Then it calls `drmSetMaster`, re-checks hotplug and re-commits everything. Each game is its own process, with its in-game menu drawn by that process.
- **MinUI and NextUI** use no compositor either: the launcher exits, the emulator runs alone, and the in-game menu belongs to `minarch`, their in-process libretro frontend.
- **RK3566 planes.** Mainline VOP2 skips the mirror windows (Cluster1, Esmart1, Smart1), which leaves Smart0, Esmart0 and Cluster0. At probe, each video port that has an endpoint in the device tree takes one as its primary plane, whether or not its output is connected. The ARC's tree wires HDMI to VP0 and the panel to VP1, so Smart0 and Esmart0 are both primaries and **only Cluster0 is left as an overlay, at all times**; Cluster0 takes AFBC buffers only, and rotating on it needs a 64-pixel-aligned AFBC stride (a check in `vop2_plane_atomic_check`, mainline since 7.0; ROCKNIX's tree carries it as LibreELEC's backport `rockchip-0012`). Disabling the primary plane while Cluster0 shows a rotated frame is untested. A board tree without the HDMI endpoint would leave Esmart0 free as a linear overlay *(guess)*.
- **cage** has no `wlr-layer-shell` (its issues #95 and #327 are open), so a cage-style kiosk can't host a layer-surface Quick Menu without patches.
- **A compositor before the home screen** costs *(guess: 0.3 to 1 s)*, plus a seatd session and libinput, which wants udev properties that libudev-zero may not supply. It would be the only component between `rcS` and Nano's first frame.

| Option | How a menu draws over a game | Cost |
|---|---|---|
| **(A) Nano as a KMS/GBM client that hands the display over** | No cross-process overlay. A hotkey drives RetroArch's own menu through its network command interface (`MENU_TOGGLE`, `SAVE_STATE`, `QUIT` on UDP port 55355); standalones show their own menus; quit is `SIGTERM` to the process group | Fastest first frame; FlipBase and RetroStoneOS prove the model. No Nano Quick Menu over a game |
| **(B) (A) plus an in-process libretro host** | Nano's `LibretroRunner`, ported and run as a separate `nano --run` process: the `minarch` and RetroStoneOS model. That process draws the game on the primary plane and Nano's Quick Menu on Cluster0 as an AFBC buffer (Esmart0 is VP0's primary on the ARC), or composites it in GL | Reuses the Android Quick Menu code; no compositor; covers the libretro systems, not standalones |
| (C) (B) plus DRM leases for SDL standalones | Nano stays the DRM owner, leases the CRTC, connector and primary plane to the emulator through an `LD_PRELOAD` `open()` shim, and commits its menu on Cluster0 (AFBC) | Allowed by `drm_lease.c`, but no handheld project is known to do it *(guess)*; races the child's page flips; Cluster0 is the only spare plane on the ARC. Spike only |
| (D) Our own minimal wlroots compositor with layer-shell | The overlay instance opens a `wlr-layer-shell` surface over the game, as the earlier draft planned under Sway | The boot cost and session stack above; a new program to maintain. cage can't do it as it is |
| (E) gamescope | its own overlay protocol | On the Mali families, no: it needs Vulkan, which PanVK refuses on Bifrost without an override (section 6.3). On the Qualcomm families (Turnip), re-evaluated in wave 3: ROCKNIX maintains a gamescope package with DPU-rotation patches (3.16.31, 2026-10-06; a rotated-copy composite blit), and it would cover Vulkan rotation too. Its libseat and udev needs are unverified |

**Recommended: in stages, with no compositor in front of the home screen.** (A) for the alpha and v1. (B) in Phase 5, for the libretro systems. (C) as a Phase 5 spike. (D) only if (C) fails and a Nano Quick Menu over standalone emulators becomes a requirement. Any compositor in front of the home screen works against the 3 s target (decision 5). Decision 4.

**The rotation contract** (decision 4, amendment proposed). With 47 of about 83 products portrait-native (section 2.4), per-emulator rotation would be a matrix of emulators by devices. Instead:

1. **One source of truth.** The DRM connector's panel orientation property, set from the device tree's `rotation` (mainline `st7701` and ROCKNIX's `panel-generic-dsi.c` both set it); then `/sys/class/graphics/fbcon/rotate`; then a `device.json` override. ROCKNIX PR #3353 shows the property can go missing (RG28XX on `generic-dsi`).
2. **Every client renders landscape**, as ROCKNIX's do (RetroArch `video_rotation=0`, PPSSPP locked horizontal).
3. **Rotation happens once, at present time, in three carried code paths:** Nano's own KMS target; one SDL3 KMSDRM patch that rotates at `SwapWindow` and also fixes `KMSDRM_CrtcGetOrientation`'s `enums[0]` and CRTC-id bug, sent upstream through SDL issue #14905, where the maintainer asked for a tested PR (SDL2 apps through `sdl2-compat`, open question 28); and one RetroArch `drm_ctx.c` patch that rotates at swap, because the menu draws with `mvp_no_rot` and `video_rotation` leaves it unrotated. ScummVM uses its own `rotation_mode`.
4. **Plane where advertised, GPU blit otherwise.** In mainline, 90/270 planes exist only on RK3566/RK3568 Cluster0 (AFBC only; overlay type) and RK3588 Cluster0 to 3; ROCKNIX's RK3588 kernel is the vendor BSP, so the mainline fact does not carry over there. RK3326, RK3399, RK3576, H700 and S922X have no 90/270 plane. Mainline has no RGA node on RK3326; RK3566 has one (`rk356x-base.dtsi`), but the RGA2 is a V4L2 memory-to-memory device, not a plane, and whether it can rotate a frame in time is untested *(guess)*. So the blit is the base mechanism and the plane a per-family optimisation. The blit's cost is **to be measured** in Phase 0 on the ARC and in wave 3 on one Qualcomm unit.
5. **Touch** is rotated by Nano and by the SDL patch from the same value; without Sway there is no `map_to_output`. This matters for the ARC-D, the RG DS and the Qualcomm units.

The P0 device is portrait, so the SDL3 and RetroArch patches are Phase 2b work.

**Not confirmed for the Qualcomm families.** Mesa's `VK_KHR_display` WSI advertises only the identity transform (`wsi_common_display.c`), and an SDL `SwapWindow` hook can't reach a Vulkan app's own swapchain. On the about 32 to 35 Qualcomm products (depending on how variants are counted; Turnip, mostly portrait), the heavy standalones prefer Vulkan, so a compositor would likely be the common launch path there, not an exception *(guess)*. So each Qualcomm family gets a **display setting**: a compositor by default for its games (Nano's home still needs none), with gamescope re-evaluated for Turnip and a fourth carried path (a Mesa `wsi_display` rotation patch or a present-time Vulkan layer) as the alternative; measured in wave 3 (open question 35). Qualcomm plane rotation needs ROCKNIX's DPU kernel patches (SSPP inline rotation and the SDE rotator; upstream sets inline rotation only for SC7280; UBWC only, height at most 1088), and the cheap blit would likely want ROCKNIX's Mesa patches 0007 to 0010 (rotated blits in gallium, the util blitter, Freedreno and `dri2_blit_image`), which are not Wayland-specific. ROCKNIX's figure for moving Sway's composite to the plane (PR #3377) is an estimate, not a measurement ("Don't have hard numbers yet"); Thimsy measures its own.

**A per-game compositor fallback.** The emulator definition gets `display: kms | kiosk`. A `kiosk` launch runs a small compositor as Nano's DRM-master child, for the game only, never at boot. It covers Dolphin (only Wayland and X11 nogui platforms), Qt GL frontends (`eglfs` rotation is software-only), Vulkan-only paths, and per-game dual-screen layouts. Weston's kiosk-shell honours panel orientation itself; cage needs `wlr-randr` for the transform and has no layer-shell. Launch cost *(guess: 0.2 to 0.6 s per game)*; whether Weston runs as root with libudev-zero and libseat's builtin backend is open question 29.

**Dual-screen devices** (RG DS and DS Plus, AYN Thor and Thor Lite, AYANEO Pocket DS, Retroid Duo Lite) are all Sway window rules in ROCKNIX. Nano drives both CRTCs itself for its home; DS and vertical-arcade split layouts use the `kiosk` path. Dual-screen features are tested per device and are not in a family's first release.

**The hand-off.** Nano opens the DRM device with `O_CLOEXEC`, so the child never inherits its fd. Before exec, Nano waits for its in-flight page flip, disables its non-primary planes (or they stay over the child's output), calls `drmDropMaster`, and keeps its fd and buffers; on 1 GB devices it also frees its GPU textures. The child opens `/dev/dri/card0` and becomes master: RetroArch through its own KMS context (`gfx/drivers_context/drm_ctx.c`, GBM and EGL), PPSSPP and other SDL standalones through SDL's KMSDRM backend. Nano waits on the pid, then calls `drmSetMaster` and re-commits with a full modeset: closing the child's file removes its framebuffers and can turn the CRTC off. A crashed child frees master by itself. About 150 lines, plus the per-emulator launch work section 6.4 counts. Known patch needs: FlipBase patches SDL3's KMSDRM backend to keep its surfaces when the mode has not changed; Batocera's PPSSPP (`003-fullscreen_drm`) and RetroArch (`006-drm-conn`) patches show which DRM fixes a no-compositor setup needs. Qt-based standalones (DuckStation Qt, Dolphin Qt) need Wayland, X11 or `eglfs`; on bare KMS only their SDL, no-GUI or `eglfs` builds run *(guess)*.

**Silencing the pad while a menu is open.** On Android the framework drops input to the game. On the Thimsy Base:

- **The built-in pad.** There is no InputPlumber. The ARC is three source nodes on mainline device trees: `gpio-keys-control` (`BTN_A`, `BTN_B`, `BTN_C`, `BTN_X`, `BTN_Y`, `BTN_Z`, the d-pad, the shoulders), `adc-keys-home` and `pwm-vibrator`, so an `EVIOCGRAB` of `gpio-keys-control` alone would miss the home key and the vibrator. With `thimsy-padd` (below) the game only ever sees padd's pad, and silencing is padd's **intercept mode**: it stops forwarding and synthesises a release for every held button, so a hotkey held while the menu opens no longer sticks in the game. Nano's own `EVIOCGRAB` silencing goes away.
- **External pads** are handled by the kernel's HID drivers. SDL's HIDAPI driver may read an external pad's `hidraw` node behind an evdev grab *(guess)*; if Phase 0 shows it does, emulators are launched with `SDL_JOYSTICK_HIDAPI=0`. padd can grab external pads too while the Quick Menu is open.

Phase 0 tests this with RetroArch and one SDL standalone emulator, including the held-button case and an external pad.

**`thimsy-padd`** (decision 23, proposed). InputPlumber is never in the image: it needs a system D-Bus at boot (`Connection::system()` in its `main.rs`), udevd and `udevadm` to hide source nodes, is GPL-3.0, sits on the path before the pad exists, and took about 35 ROCKNIX PRs in six months, including the v0.85.0 revert merged 2026-11-22 (PR #3690). Its one real benefit on ROCKNIX, that every built-in pad looks like the same DualSense so one set of emulator input files serves all (PR #2685 deleted the per-device PCSX2, Azahar, DuckStation and mupen64plus files), Thimsy gets without the daemon:

- **The contract.** Each built-in pad is exactly one evdev gamepad with one Thimsy identity: one name and one bus, vendor, product and version, with buttons in one normalised layout. SDL's GUID hashes the name, so it matches too. Raw source nodes are never visible to SDL or RetroArch. Volume, power and lid stay separate nodes, read by `thimsy-session`.
- **What that buys.** One `gamecontrollerdb` line (passed through `SDL_GAMECONTROLLERCONFIG_FILE`), one RetroArch autoconfig file and one set of standalone input files serve every built-in pad. External pads use SDL_GameControllerDB (zlib) and `retroarch-joypad-autoconfig` (MIT).
- **padd.** A small C program in `thimsy-session`'s mould: no D-Bus, no udev, `uinput` only (`CONFIG_INPUT_UINPUT=y` in ROCKNIX's RK3566 config). Started from `rcS` beside Nano, not on the path to the first frame *(guess: under 20 ms to a pad)*. It opens and `EVIOCGRAB`s the source nodes `device.json` names, applies remaps, axis inversion and calibration, emits the Thimsy-identity pad, and forwards force feedback to the `pwm-vibrator` or `pm8xxx` vibrator. *(guess: 800 to 1,200 lines, about 1 MB resident.)*
- **Hiding sources.** padd unlinks the source nodes from devtmpfs after opening them, so libudev-zero consumers can't open them; the fallback is a small libudev-zero patch that drops `ID_INPUT_JOYSTICK` for listed nodes. Both are guesses to prove in Phase 0, since libudev-zero ignores udev rules (open question 4). If hiding proves unreliable on a family, that family alone falls back to kernel normalisation (`rocknix-joypad` device trees) under the same contract.
- **Sources per family.** On `rocknix-joypad` families (H700, RK3326, RK3399, S922X) padd re-exposes the one module node; the module (GPL-2.0-or-later, pinned at `ac1be0fa`) is a Buildroot kernel-module package. On InputPlumber families (RK3566/RK3568, RK3576, SDM845, every SM*) padd merges the mainline nodes and the MCU driver's node, plus separate paddle and vibrator nodes where they exist (Odin 2's M1 and M2, the `pm8xxx` vibrator). RK3588's `gameforce-gamepad` and the AYANEO serial "Xbox 360" MCU mode get an `rcS` hook plus a padd source list. Hackable Android-only devices later fit the same shape.
- **Accepted costs.** One uinput hop per event *(guess: tens of microseconds)*. `rocknix-joypad` polls its buttons (about 5 ms average at a 10 ms poll), kept as ROCKNIX ships it. Mainline `adc-joystick` polls sticks every 60 ms on RG353-class device trees; a poll-interval tweak is the one per-board patch worth carrying.

**The pad itself.** Neither of Nano's keys carries over as is. `device-keys.json` matches Android system properties, which Linux does not have; on Linux it matches the device-tree `model` (section 4.2). `pads.json` and gammapad match by vendor and product id, which a built-in `gpio-keys` pad does not have, so the device descriptor names it. And the ARC is unusual:

- six face buttons (A, B, C, X, Y, Z) and no sticks;
- ROCKNIX maps Z and C to the left and right stick clicks, and swaps South and East, in its InputPlumber capability map: facts to write into the ARC's `device.json` pad section, which padd applies;
- Nano's hotkey options include L3 and R3 (`HOTKEYS.md:206`), which on the ARC would fire from Z and C;
- ROCKNIX ships ARC remap files for seven cores (Genesis Plus GX, Flycast, YabaSanshiro and others).

So the device descriptor gains, per device: the number of face buttons (4 or 6), whether sticks exist, the printed label of each logical button, the hotkey button, the pad producer (padd over which sources, or a kernel joypad) and the source-node list. The labels go through `face_layout`, the label convention (`nintendo` or `xbox`) that `controller_layout_plan.md` Phase 1 adds to `device-keys.json`, extended with a value for the ARC's Saturn-style print; the button count and sticks are separate fields. Nano hides stick-dependent actions where there are none. These facts live in one `device.json` row for Nano and padd and are never copied into emulator files. RetroArch remaps are per device, never shared on a card (section 7.5). Each device costs one importer-generated `device.json` pad section, written from ROCKNIX's facts and checked on the unit, not hand-written `device-keys.json` and SDL controller-database entries.

### 5.4 Feature map

"Alpha" is the first build that runs (roadmap phase 2). "v1" is the first public release with the card contract and the installer. "Later" has a phase in section 12. "n/a" does not apply to a Linux device.

| Feature (Android Thimsy 1.4.4) | Linux |
|---|---|
| XMB home, the ES-DE theme engine, Minima; navigation, sounds, clock | **alpha** |
| 21 languages, right-to-left text, CJK on-screen keyboard | **alpha**, with `thimsy-fonts` (the Kanji table depends on the licence answer in section 3.3) |
| Game library: scan, favourites, hidden, collections, search, Recently Played | **alpha** |
| Launching RetroArch cores; per-system and per-game emulator choice | **alpha** |
| Settings: Wi-Fi, Bluetooth, brightness, volume, time, language, sleep | **alpha** through system actions |
| Volume and power keys, sleep and wake | **alpha** (`thimsy-session`) |
| The quit hotkey; "Close Current App" by hotkey; universal hotkeys for RetroArch | **v1** (`thimsy-session`) |
| ES-DE store: **Move ES-DE to Drive**, adoption of an existing `ES-DE` folder | **v1**. On Linux the store is always on (section 7.2) |
| **Move Emulator Files to Drive**, the Emulator Settings Drive, the foreign-card dialog | **v1**, in its Linux form (section 7.4) |
| Setup wizard | **v1**, reduced: language, Wi-Fi, time zone, drive adoption |
| Scraper | **v1** (libcurl; same code) |
| BIOS manager (by SHA-1) | **v1**; targets are Linux paths |
| File Explorer, archives (zip, 7z) | **v1** (`NanoFileJob`, `NanoArchive` are host-tested). PDF viewer later |
| Standalone emulators with their hotkeys (PPSSPP first) | **v1** for PPSSPP and ScummVM; others as added |
| System update | **v1**, the A/B update file from Thimsy's release page (section 5.2) |
| Quick Menu and Control Center over a game; button map overlay | **Later** (Phase 5, through the in-process libretro host; at v1 a hotkey opens RetroArch's own menu; section 5.3) |
| Quick Resume | **Later** |
| Built-in DS player (drastic-nano) | **Dropped**; melonDS core instead |
| Video, music, photos, radio, IPTV | **Later**, on FFmpeg; photos first |
| Syncthing, network shares, file sharing server | **Later**. Samba and Syncthing are base packages started on demand; Nano's menus need wiring |
| USB file transfer (MTP) | **Later** (USB gadget; depends on the device's OTG support) |
| External display, monitor modes, dock, Dual Screen | **v1** for the ARC's HDMI in Docked mode (Nano and games on the TV, panel off; open question 14). Mirroring **later** (a per-game compositor or the display patches); other modes and Dual Screen **later**, per device |
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
| Needs to run on a handheld | - | a self-built binary with **`GLES=on`** (GLES 3.0) and, on the Thimsy Base's bare KMS, `DEINIT_ON_LAUNCH=on`; one more recipe |
| Data directory | chosen in ES-DE's first-run wizard; Thimsy seeds `/sdcard/ES-DE` | `~/ES-DE`, or `ESDE_APPDATA_DIR`, or `--home` |
| System and label tables | `resources/systems/android` | **`resources/systems/linuxarm`** on AArch64 (every Thimsy Linux target); `linux` is the x86 table |
| Find rules | `androidpackage` rules | `systempath`, then `staticpath` |
| Launch commands | Android-only tokens: `%EXTRA_*%`, `%ROMSAF%`, `%ANDROIDPACKAGE%` | `%ROM%`, `%EMULATOR_*%`, `%CORE_*%` |

So "export through ES-DE" on Linux is Nano's store, not the ES-DE program. The files are identical: `gamelists/<system>/gamelist.xml`, `collections/custom-*.cfg` with `%ROMPATH%`, `downloaded_media/`, plus `nano/`.

**Shipping the real ES-DE on Linux is optional** (decision 9). If shipped, it must not share `settings/` and `custom_systems/` commands with Android. It gets its own data directory on internal storage with `collections` and `downloaded_media` linked to the drive (the links live on internal storage, section 7.4) and Linux find rules that call the launcher. **Gamelists are not linked until the label rule of section 7.6 is the compatibility floor:** released Android builds already write Android-only labels into them (section 7.10), and a Linux user's "CLEAR INVALID ENTRY" would erase the Android per-game choice. Until then it works on its own copies.

### 6.2 RetroArch

| | Android | Linux (Thimsy Base) |
|---|---|---|
| Main config | `Android/data/com.retroarch.aarch64/files/retroarch.cfg`; after a move, `<card>/emulator_config/retroarch/retroarch.cfg` | `/storage/.config/retroarch/retroarch.cfg`, seeded per device from the quirks overlay and passed by Nano with `--config`. Per-launch keys go in an append config Nano writes under `/run/thimsy/` (`--appendconfig`). No script rewrites the main config |
| Cores | `<core>_libretro_android.so` from the buildbot nightlies pinned in `retroarch-cores.lock` | `<core>_libretro.so`, built from source by the base's own recipes (Buildroot packages none), at the commit the Android build's `retroarch-cores.lock` pins, so saves and states can match (open question 23). A 64-bit userland only, unless Phase 3 finds a core that needs 32-bit: ROCKNIX runs `pcsx_rearmed32`, `gpsp`, `yabadabadoo` and `desmume` in a 32-bit `retroarch32`; their 64-bit builds are tried first *(guess)* |
| In-game saves | beside the ROM (`savefiles_in_content_dir = "true"`, fixture P:152007) | beside the ROM |
| Save states | beside the ROM (`savestates_in_content_dir = "true"`) | `savestate_directory` in the append config: `<drive>/emulator_config/retroarch/linux/states/<system>/` (below) |
| BIOS | `system_directory`; after a move `<card>/emulator_config/retroarch/system` | `system_directory` in the append config: the drive's `emulator_config/retroarch/system` |
| Per-core files | `config/<Core>/*.cfg`, `*.opt`, shader presets; `config/remaps/` | same names, seeded once per device from the quirks overlay; nothing rewrites them per launch (ROCKNIX's `setsettings.sh` rewrote several `.opt` files on every launch) |

What Thimsy Linux does:

- **The main config and `config/` stay on internal storage.** They hold drivers, device-tuned core options, per-device remaps and the device's own paths. They are not a card format. (Android's `retroarch/config/` on a card is no better as a shared folder: Thimsy's own test fixture shows per-core overrides there naming `/data/user/0/com.retroarch.aarch64/overlays/...` and `.slangp` presets; `RaConfig.TreeRewritesConfigsAndPresetsOnly`, P:161105.)
- **Directory keys point at the card** for what is shared: `system_directory` (BIOS) and `cheat_database_path`. Nano writes them into the append config at each launch.
- **In-game saves are shared only between the same core** (section 7.5). A `.srm` is named after the ROM, so two different cores would overwrite each other's file.
- **Save states are per platform in v1.** A state file is also named after the ROM only; a 64-bit Beetle PSX HW state and a 32-bit PCSX ReARMed state would share one path, and auto-save would overwrite the other platform's `.state.auto`. Linux writes states to `<card>/emulator_config/retroarch/linux/states/<system>/`. Android keeps writing beside the ROM. Sharing states comes later, after v1, and only for cores that pass the state test of open question 11.
- **Default cores are chosen to match Android's where the hardware allows**, because that is what makes saves travel. A per-system table of both platforms' default cores is a Phase 3 deliverable. **The share-or-sort decision is made per launch, not per system:** Linux compares the core it will actually run with the core Android would run for that game (the game's per-game choice, from `<altemulator>` or `nano/emulators.json`, resolved through the label and family tables; else Android's system default). Where the two differ, where either is unknown, or where they are not known to be save-compatible, Linux turns on `sort_savefiles_enable` for that launch, which puts its saves in a per-core folder beside the ROM (RetroArch does this under the content directory; checked in `runloop.c`). Comparing system defaults alone is not enough: a game set to core Y on Android, with Y missing on Linux, would fall back to the shared default X and overwrite Y's `.srm`.

### 6.3 Standalone emulators

Checked 2026-11-24 against each project's release assets.

| Emulator | Android (Thimsy) | Linux aarch64 | Config redirection on Linux | Viable on RK3566 / H700 |
|---|---|---|---|---|
| PPSSPP | `org.ppsspp.ppsspp` 1.20.4 | official aarch64 AppImage; our recipe (ROCKNIX and Batocera build it) | memstick directory; `PSP/SYSTEM/ppsspp.ini` (graphics backend, resolution) seeded per device; ROCKNIX's `start_ppsspp.sh` rewrote it on every launch | yes / light games |
| Flycast | `com.flycast.emulator` 2.7 | **no official aarch64 binary**; our recipe (ROCKNIX builds it); or the core | `emu.cfg`, data dir (XDG) | yes / marginal |
| N64 | M64Plus FZ (Android only) | mupen64plus standalone, or the `mupen64plus_next` core | `--set Core[SaveSRAMPath]=...`; there is no save-directory flag | yes / some |
| Nintendo DS | WatermelonDS, SeedlessDS, built-in DraStic player (Android only) | melonDS DS core, melonDS 1.1 (aarch64 AppImage), DraStic's Linux build (closed) | `melonDS.toml` | core: yes |
| Dolphin | `org.dolphinemu.dolphinemu` | no official aarch64 binary; ROCKNIX builds it | `-u`, `DOLPHIN_EMU_USERPATH` | no *(guess)* |
| Azahar (3DS) | `org.azahar_emu.azahar` | no official aarch64 Linux build | XDG dirs | no |
| PS2 | ARMSX2 | ARMSX2 2.9.6 Linux arm64 AppImage | PCSX2-style `inis/` *(guess)* | no |
| Xbox | hakuX (Android only) | xemu aarch64 AppImage | `xemu.toml` *(guess)* | no |
| Vita | Vita3K | aarch64 AppImage; needs Vulkan | `config.yml` `pref-path` | no |
| ScummVM | `org.scummvm.scummvm` | our recipe (ROCKNIX builds it) | `--config`, `scummvm.ini` | yes |
| PlayStation standalone | - | DuckStation exists but **can't be shipped configured** | - | - |

**Recipes and the GPU.** Buildroot upstream packages none of RetroArch, the libretro cores, PPSSPP, ScummVM, mupen64plus, melonDS or Flycast, so each is a short `.mk` file of Thimsy's own: about 10 to 15 cores and 4 to 5 standalones for v1 (section 3.2; open question 23). Panfrost gives GLES 3.1 on both the RK3566's Mali-G52 and the H700's Mali-G31. PanVK, Mesa's Vulkan driver, refuses Bifrost GPUs unless `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is set, so emulators with a Vulkan backend (PPSSPP, Flycast) default to GLES on RK3566 and H700.

**Mapping to package profiles.** `device_profiles_plan.md` already recommends **Small** for the H700 and for RK3568-class chips. Small has no Dreamcast and no standalone N64 emulator (N64 is the `mupen64plus_next` core), and names SeedlessDS as the DS default. So:

- RK3566 and H700 Linux images are profile **Small**, with a `linux` column for substitutions: DS default is the melonDS DS core, since SeedlessDS is Android only.
- On the 2 GB RK3566 devices, **Medium** is offered; that is where Flycast and PPSSPP's heavier defaults sit. Whether Small on Linux should gain Flycast is a proposed change to `device_profiles_plan.md` §5, not something this plan decides.
- The heavy systems (GameCube, Wii, 3DS, PS2, Xbox, Vita, Switch) are absent from RK3566 and H700 images. They return on Qualcomm devices.

The `linux` column has no place in `device_profiles_plan.md` as written, and needs a change there. That plan has no platform dimension: its precedence is base < profile < GPU family < chipset < display < device < user choice < hard constraints; layers 3 to 6 "never add an emulator the profile didn't choose"; and its `requires=` constraints know CPU ISA, Android API level and GPU API. The change this plan needs:

- a platform selector (a layer, or a per-component platform column) that is allowed to substitute components;
- a `requires=platform:linux` key, or a `platform=` key (that plan's choice);
- a Linux substitute for every Android-only default in Small and Medium. Known so far: the DS default (SeedlessDS in both) becomes the melonDS DS core, which is not in `mods/retroarch-cores.txt` yet; Medium's N64 default (M64Plus FZ) becomes mupen64plus standalone or the `mupen64plus_next` core (Phase 3 chooses). The full list comes from the resolver once the selector exists.

### 6.4 How a game is launched

| Step | Android firmware | Linux |
|---|---|---|
| What Nano stores | a system: `launchType` (`libretro-core` or `custom-package`), `coreSo`, `package`, `intentTemplate` (`ES-DE/nano/systems.json`); a game: an ES-DE label in `<altemulator>`, or `am` arguments in `nano/emulators.json` | reads the same files |
| Resolving | a `core:<name>` or `pkg:<package>` key (`esdeEmulatorKey`, P:20546) to an ES-DE label and back | the same key. `core:` strips `_libretro_android.so`. `pkg:` is looked up in the emulator definitions to find the emulator, then its Linux block |
| Starting | intent file plus property; system_server starts the activity | Nano fork+execs a direct argv after the DRM hand-off (section 5.3): `retroarch -L <core>.so --config <internal cfg> --appendconfig <per-launch cfg> "<rom>"` for a core; for a standalone, the image's launch template for the emulator the definition's Linux block names. No `runemu.sh`, `setsettings.sh` or `swaymsg`, and no platform-name mapping (ROCKNIX's `-P` names differed from four of Nano's folder names) |
| When the choice doesn't exist here | "Install" or "Choose Emulator" | fall back to this device's default for the system and leave the stored value alone (section 7.6 says where that needs new code on Android too). The save rule of section 6.2 then sorts that launch's save. A family member that is in the image but not enabled by the device's package profile counts as missing: the profile wins (decision 17) |

**Emulator definitions.** The definition is `user_emulators_plan.md`'s (not started), which `android_launcher_plan.md` §3.8 makes the shared profile format. Its `players` are Daijisho-schema entries (`platform` is the console id; `amStartArguments`), and its `storage.link` is `[{base, dirs}]`. The Linux data therefore goes in **new, separately named keys**, and the file becomes `schema: 2`:

```json
{ "schema": 2, "id": "ppsspp", "name": "PPSSPP",
  "packages": ["org.ppsspp.ppsspp"],
  "players": [ { "platform": "psp", "name": "PPSSPP", "amStartArguments": "..." } ],
  "family": "psp.ppsspp",
  "esde_labels": { "android": "PPSSPP (Standalone)", "linuxarm": "PPSSPP (Standalone)" },
  "linux": {
    "players": [ { "systems": ["psp"], "emulator": "ppsspp" } ],
    "storage": [ { "link": "ppsspp/PSP/SAVEDATA", "to": "PSP/SAVEDATA" } ] } }
```

- In `linux.storage`, `link` is relative to the internal emulator roots (`/storage/.config/` or `/storage/.local/share/`; trust rules below) and `to` is relative to the emulator's folder on the drive, `emulator_config/<card_folder>/` (`user_emulators_plan.md`'s `card_folder`).
- A Linux player names an `emulator`, and for RetroArch a `core`, from the image's own list; the image's launch template for that emulator gives the argv and the environment (`SDL_AUDIODRIVER`, `AUDIODEV`, rotation hints). A definition shipped in the image may give a full `argv` instead.
- Both validators that plan names (`NanoEmuDefs` and `ThimsyHotkeyController`) must add the new keys before any such file ships; a schema-1 reader refuses a schema-2 file, by that plan's own rule.
- `family` joins emulators that are the same engine under different names (M64Plus FZ and mupen64plus; WatermelonDS and melonDS). A per-game choice made on one platform maps to the same family on the other. If the matching member is in the image but not enabled by the device's package profile, the lookup falls back to the system's default, as for a missing emulator (section 6.4 and decision 17).

**Trust rules.** A definition can name a command and links, and everything on the Thimsy Base runs as root. So, as on Android (`user_emulators_plan.md`: "Import, not live read"):

- Linux never acts on `Thimsy/emulators/*.json` straight from a drive. It imports through the same confirm screen into internal storage and records the origin.
- For an imported definition, each player must name an emulator and core from the image's own list. A free `argv` is accepted only in definitions shipped in the image.
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

"The card" below means **the drive each setting names**. `usb_storage_plan.md` replaces "the microSD card" with a drive list and two settings, **ES-DE Drive** and **Emulator Settings Drive** (`persist.gammaos.nano.storage.esde_drive` and `.emu_drive`), each a volume id or `internal`, with an automatic fall-back to internal storage after a 10 s grace. The Linux build implements that model from the start; it never had the single-card one. On Linux the drive list is filtered **per partition, not per disk**, and not by that plan's `/mmc` test: every eMMC partition and the root device's own partitions (U-Boot, boot, root filesystem, data) are left out; a partition on the root device that carries the Thimsy data-partition label is let in, so the one-card setup (section 7.3) has a drive. (The label is fixed in Phase 3; the partition's type is Microsoft basic data, section 8.2.) Excluding the whole root block device would exclude that data partition too. A drive is identified per partition by its volume id, as in `usb_storage_plan.md` (`/storage/<id>`). That plan, and the earlier drafts of this one, took the volume id to be the FAT or exFAT serial. That holds on stock LineageOS 21 and on Linux, **but not on the RG Cube**: the Thimsy Android build's vold (GammaOS Next's) names every public volume by a mount-order index, `00000000-0000-0000-0000-NNNNNNNNNNNN` (section 7.3). Which id Linux matches a Cube-written path against is decision 20.

**At boot, the 10 s grace waits for the first mount.** Nano starts before the drive is mounted (section 9.3), and a dirty-flag check can take longer than 10 s. Until `thimsy-session` reports that the first mount attempt for the adopted drive has finished (check included), Nano treats the drive as pending: the grace timer does not run, nothing is re-pointed to internal storage, and launches that need the drive wait or say why.

### 7.1 Principles

1. **A drive holds only regular files and folders.** No symlinks, device nodes, sockets, permissions or case-sensitive name pairs. FAT32 and exFAT have none of these, and Android's `/storage/<id>` view would hide them anyway.
2. **Paths stored on a drive are relative, tokenised (`%ROMPATH%`, `%CARDID%`), or the canonical absolute form `/storage/<volume id>/...`.** Each platform resolves that form to the drive when it names the drive by the same id. On the RG Cube it does not: Android's id there is a mount-order index, not the serial Linux mounts at (section 7.3), so an absolute path the Cube wrote does not resolve on Linux by itself (decision 20). Tokens and relative paths resolve on both. (On a card today there are also `/mnt/media_rw/<id>` in Vita3K's `pref-path`, and `content://` tree URIs and ScummVM `/saf/` paths inside the Android-only `app/` mirrors.)
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
    <emu>/...                         shared data (saves, memory cards, BIOS) mixed with Android's per-platform
                                      files after a move (Flycast emu.cfg, ARMSX2 inis, PPSSPP PSP/SYSTEM and
                                      PSP/PPSSPP_STATE); the definition's storage block names the shared subfolders
    <emu>/app/...                     Android only: mirror of app-private preferences (exists today)
    <emu>/linux/...                   NEW, Linux only: states and any config that must travel between Linux devices
    retroarch/{system,cheats}/        shared
    retroarch/retroarch.cfg, config/, playlists/, ...   Android's (exist today); Linux does not use them
    retroarch/linux/states/<system>/  NEW: Linux's save states
    card.json                         NEW: see 7.7
  bios/                               drop folder the BIOS installer offers first (a convention today)
  Thimsy/emulators/*.json             user emulator definitions to import (user_emulators_plan.md; see below)
```

Where the import folder lives is not settled across the plans: `user_emulators_plan.md` §8 imports from `/sdcard/Thimsy/emulators` (internal shared storage), and `android_launcher_plan.md` §3 item 8 says the definitions travel with the card. Recommended: Linux imports from both internal `Thimsy/emulators/` and the ES-DE Drive's, always through the import screen (section 6.4's trust rules). Decision 17.

Nothing existing moves. New: `<emu>/linux/`, `nano/systems.linux.json`, and `emulator_config/card.json`.

**Store mode is always on in the Linux build.** With no drive chosen, the store is `/storage/emulated/0/ES-DE` on internal storage: the same switch as `usb_storage_plan.md`'s "ES-DE Drive = Internal Storage" and the launcher plan's D7.

### 7.3 Path mapping and mounting

The Android build writes absolute paths of the form `/storage/<id>/emulator_config/...` into configs on the card (`emusync::repoint`, P:17477), where `<id>` is the name vold gave the volume.

- **On stock LineageOS 21** that name is the FAT or exFAT serial, upper-case `XXXX-XXXX`. Linux prints the same serial in the same form: e2fsprogs (Android's blkid) and util-linux use the same format string and byte order for both filesystems.
- **On the RG Cube it is not** (found from code and the Cube's own logs, 2026-11-25). GammaOS Next's vold, which the Thimsy Android build uses, names every public volume `00000000-0000-0000-0000-NNNNNNNNNNNN` (`PublicVolume.cpp:78-92`; stock LineageOS 21 has none of this). `NNN` is the lowest free index from 1 to 100, taken when the mount starts, in mount order (`VolumeBase.cpp:41-66`). A volume keeps its index across remounts and frees it when it is destroyed; the pool resets once no volume is left. Volumes that fail to mount take an index too. The Cube's exFAT card, serial `9602-C755`, is `/storage/00000000-0000-0000-0000-000000000001`, and every absolute path the Thimsy Android build has written to a card uses that form.
- **The Cube's id is not a property of the drive.** It shifts with mount order: a USB drive mounted first, or extra volumes on the same card, take the lower indices.
- `emusync::isVolumeId` (P:17466) accepts only 9-character (`XXXX-XXXX`) and 32-hex-digit ids, so it rejects the Cube's 36-character form.

Below, **`<VOLID>` means the drive's FAT or exFAT serial**, the id stock LineageOS uses and the only one Linux can compute from the drive itself. Where the Cube's form matters it is written out. Whether Linux also makes the Cube's paths resolve is decision 20.

**The Linux build mounts the drive at `/storage/<VOLID>`.** The Thimsy Base mounts its writable ext4 data partition at `/storage` (FlipBase mounts its own at `/data`; Thimsy uses `/storage` so the paths below hold), and `/storage/<VOLID>` is a directory on it used as a mount point. A path the Cube wrote, `/storage/00000000-0000-0000-0000-00000000000N/...`, does not point there unless decision 20 makes it.

| Meaning | Android firmware | Linux build |
|---|---|---|
| The drive, as stored in files | `/storage/<VOLID>` on stock LineageOS; `/storage/00000000-0000-0000-0000-00000000000N` on the Cube | `/storage/<VOLID>` (the mount point) |
| The drive, root-side alias | `/mnt/media_rw/<id>`, the same id | `/mnt/media_rw/<VOLID>` → symlink to the mount |
| Internal shared storage | `/storage/emulated/0`, `/sdcard`, `/data/media/0` | `/storage/emulated/0` (a directory on the ext4 data partition); `/sdcard` → symlink |
| Nano state | `/data/system/nano_*` | `/storage/.config/thimsy/state/nano_*` |
| Nano settings | `persist.gammaos.*` properties | the config store (section 5.2), same keys |
| Emulator private files | `/data/data/<pkg>`, `Android/data/<pkg>/files` | `/storage/.config/<emu>`, `/storage/.local/share/<emu>` |
| Read-only assets, fonts | `/system/etc/nano_xmb`, `/system/etc/gammaos-nano`, `/system/fonts` | `/usr/share/thimsy/nano`, `/usr/share/fonts/thimsy` |

**`/storage/roms` is gone.** It existed for ROCKNIX's launch scripts, which built paths from it (`savestates`, `bios`, `bezels` and others) that would have landed in the shared ROM tree. On the Thimsy Base, Nano gives each emulator its ROM path and its directories itself, so there is no bind and nothing to redirect; open question 19 and decision 17's bind clause are moot. The rule they served stays: after a launch, nothing but in-game saves appears under `<drive>/ROMs` (principles 3 and 5; interop row 14).

**The empty-mount-point hazard.** On Android, vold creates `/storage/<id>` only while the volume is mounted (GammaOS's vold also creates it for a volume that then fails to mount, and probably leaves it behind; section 13). On Linux the mount point is an ordinary directory, writable by root, and everything on the Thimsy Base runs as root. With the drive out, anything that creates its whole path (Nano's own `mkdir -p`, an emulator, RetroArch's sorted-save folders) would quietly write saves to internal storage, and the drive would later mount over them. Rules:

- One permanent, empty mount-point directory per adopted drive, with the ext4 immutable flag set (`chattr +i`). It can still be mounted over; nothing can be created in it.
- Before every launch, `thimsy-session` checks that each target is a mount point (compare `st_dev` with `/storage`). If not: the internal fall-back, or refuse.
- A test: drive out, launch each emulator, assert nothing appeared under `/storage/<VOLID>`.

**Mounting.** `thimsy-session` mounts drives itself. The base has no automounter, udev rule or udevil to mask: `thimsy-session` hears of cards from its uevent listener (section 5.2). Thimsy's mount:

- vfat: `utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush` (Android's vold mounts vfat with `utf8,shortname=mixed` and both filesystems with `dirsync`; without `utf8` a FAT32 card's non-Latin file names read differently and stop matching `gamelist.xml`);
- exfat: `dirsync,noatime,nodev,nosuid,noexec`;
- the check before the mount (open question 18, answered from code 2026-11-25; decision 17). vold checks before every mount (`fsck_msdos -p -f -y` or `fsck.exfat -y`), so the Linux-to-Android direction never depends on a flag. On Linux the rule differs by filesystem:
  - **vfat: `fsck.fat -a` only when a dirty flag is set.** Since Linux 3.9 (b88a105802e9) vfat sets the boot-sector dirty bit when a read-write mount completes and clears it only at unmount or an rw->ro remount, on the Cube's 5.4 kernel and on Linux alike, so a FAT card pulled while mounted read-write always arrives dirty. Read both flags, as dosfstools' `check.c` does: the boot-sector state bit (offset 0x41 on FAT32, 0x25 on FAT12/16, bit 0x01) and the FAT[1] clean-shutdown bit (FAT32 entry 1 bit 0x08000000, FAT16 bit 0x8000). Android's `fsck_msdos` clears only the FAT[1] bits, never the boot-sector byte, so expect one `fsck.fat -a` on Linux after any unclean removal from Android; that check clears it.
  - **exFAT: the dirty flag is not enough.** Run `fsck.exfat -p` when VolumeDirty is set (offset 0x6A, bit 0x0002) **or** when the drive was not last cleanly unmounted by this device. The Cube's exFAT driver can leave a torn volume flagged clean (section 7.8); the Thimsy Base's kernels (each family's ROCKNIX version, decision 22; all newer than 6.16, which carries the exFAT fix) set the flag before the first metadata write and keep it until unmount, so a drive pulled from Linux is caught by the flag. The exception is RK3588's vendor BSP 6.1 kernel, older than 6.16: it needs the exFAT fixes backported, or is checked in wave 4. Phase 0 times an unconditional `fsck.exfat` on real cards: **if it takes a few seconds or less, always check exFAT** and keep no record. Otherwise "cleanly unmounted by this device" is a record on internal storage, written at Thimsy's own clean unmount: the `<VOLID>` plus a hash of the **whole allocation bitmap** (about 64 KiB on a 64 GB card with 128 KiB clusters) and the root directory's clusters; a drive whose hash differs is checked. Not the boot sector plus the first bitmap sector: the Cube's driver does not otherwise change the boot sector, and one bitmap sector covers only the first 4096 clusters (512 MiB on that card), so most Cube sessions would go unseen.
  - This applies to **every exFAT drive**, whatever its size: a 16 GB card formatted by the installer's storage job (section 8.4) and the one-card exFAT partition (below) included. Only a card left in its factory FAT32 format gets the vfat rule.
- no folders created on the drive except by Nano's own moves;
- `sync` when a game exits, on sleep and on shutdown, as now: since Linux 6.16 (46a557694b46) `sync` no longer clears exFAT's VolumeDirty;
- a "Remove Drive" action that does a **full `umount`**, never a remount. On the Thimsy Base's kernels (all before 7.4), `exfat_reconfigure` clears VolumeDirty on every remount, and a rw->rw option remount or a forced read-only remount can race writers that dirty the volume again after its unlocked `sync_filesystem()`; 7d390ca06fe7 fixes that in 7.4-rc1, not in 7.3.5 stable. So the Linux build never remounts a mounted exFAT drive (a normal, unforced rw->ro remount is not racy, but Remove Drive does not need one), or it backports 7d390ca06fe7.

**Volume ids that can't be used:**

- An all-zero serial. Android's blkid gives `0000-0000`; util-linux reports no UUID at all. Linux rejects it, since it names the drive by the serial; stock LineageOS, which does the same, rejects it too. The Cube's vold never uses the serial as the id, so on the Cube an all-zero serial does not stop Android using the drive; the rule binds Linux and stock LineageOS. The installer's storage job gives every card it formats a non-zero serial (section 8.4).
- Two drives with the same serial (a cloned card). Linux uses neither, with a message asking to remove one, as `usb_storage_plan.md` (Risks) says for Android; the rule goes into `LIBRARY-FORMAT.md` for both platforms. (The first draft used the first and refused the second.) Decision 17. On the Cube the two clones get different ids (`...0001` and `...0002`), so a check keyed on Android's volume id never sees them as duplicates; the Android side has to compare the filesystem serial to apply the rule (vold's blkid probe sees it, `UUID="9602-C755"` in the Cube's log, but vold does not use it as the id).
- `emusync::isVolumeId` (P:17466) accepts 9-character and 32-hex-digit ids only. The 32-hex ids are not FAT or exFAT and are not interop drives. It rejects the Cube's 36-character synthetic id, so it should learn that form, or its role on the Cube be re-checked: Android-side work that goes with decision 20.

**One-card devices.** With one slot, or one card, the drive is an exFAT data partition on the boot card, created on first boot (AmberELEC, Knulli and dArkOS all do this, and FlipBase grows its exFAT partition to fill the card on first boot). **Android's vold accepts that fifth partition** (open question 8, answered from code 2026-11-25), if it carries the Microsoft basic data type GUID and the card's other entries carry types vold ignores (section 8.2). vold makes a public volume of every GPT entry typed basic data or Linux filesystem, whatever its number and whatever comes before it; the only limit is the MMC minor count (8 by default), so the drive stays at entry 7 or lower. The Cube already mounts exFAT on a GPT basic data partition, with its kernel driver and `/system/bin/fsck.exfat`. Typed as FlipBase types its card, the other four partitions would become volumes too (section 13). Still for the Phase 0 device test: the Cube's `perdev_minors`, the id the partition actually gets (`...0001` with the section 8.2 typing), whether Nano picks it as the card, and whether a USB drive shifts its id. Two cards are the supported interop setup until that test passes. On the Linux side the drive list must offer that partition (the per-partition rule in section 7's introduction); interop row 12 tests both sides.

### 7.4 Symlinks: how link-based configs survive FAT, exFAT and FUSE

Today, on Android, only two emulators are linked (Dolphin and hakuX, which have no folder setting). The link sits on `/data` and points at the card:

```
/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/GC   (link, on f2fs/ext4)
        -> /storage/<id>/emulator_config/dolphin/GC             (target, on the card, through FUSE)
```

It works because apps reach `Android/data` on the lower filesystem directly, not through FUSE, so they follow the link; and the target is an ordinary folder the app may open with all-files access (`README.md:604-608`; `linkFolder`, P:21157).

The rules that generalise this to both platforms:

1. **A link never lives on a drive.** It lives on the device's internal filesystem: `/data` on Android, the ext4 storage partition on Linux.
2. **A link's target is always a folder under `/storage/<id>/emulator_config/<emu>/`** (or `ES-DE/`), where `<id>` is the drive's id on that platform: `<VOLID>` on Linux and stock LineageOS, the mount-order id on the Cube (section 7.3). The existing check stays: the target must start with that prefix and contain no `/.` (`emuRepointAbsent`, P:67500).
3. **Links are device state, rebuilt from data.** The drive stores no record of them. Each device recreates its links when it adopts a drive, from the definitions shipped in its image.
4. **Links are replaced atomically** (`symlinkat` to a temporary name, then `renameat`), as today.
5. **When the drive is absent.** The link dangles on both platforms: on Linux only the empty mount point `/storage/<VOLID>` exists, not the target under it. The immutable mount point stops anything from recreating the target, and the mount check before launch catches the missing drive; code that follows links still has to handle a dangling one. Either way the device re-points to the adopted drive's id if it changed, or to the internal fall-back.
6. **Re-pointing after an id change.** Android's repair pass walks `emulator_config` three levels deep, skips dot-names and a first-level `app`, and rewrites the old id in `.cfg .ini .txt .xml .yml .json` files under 1 MiB. So it does edit files under `linux/`, to that depth. **Files under `<emu>/linux/` therefore never contain an absolute `/storage/<id>/.../emulator_config` path**, whatever the id: they use `%CARDID%` or relative paths (principle 2), so a released build's pass finds nothing in them to rewrite. The Linux build runs the same pass, over all depths under `linux/` and over its internal configs. On the Cube the id can change with mount order alone (section 7.3), so the id in the Cube's own configs can go stale with no change of card.
7. **Nano's own readers still refuse to follow links** inside app trees (`O_NOFOLLOW`, `sdAppPathSafe`), on both platforms.

On Linux there are no scoped-storage rules, so a link or the emulator's own directory option replaces everything `NanoSdMove` does with grants:

| Emulator | Android points it by | Linux points it by | Shared in v1 |
|---|---|---|---|
| RetroArch | paths in `retroarch.cfg`, `CONFIGFILE` at launch | directory keys in the append config (section 6.2); main config internal | BIOS, cheats, same-core saves |
| PPSSPP | `memstick_dir.txt` | **per-folder links**: `PSP/SAVEDATA`, `PSP/TEXTURES`, `PSP/Cheats`. `PSP/SYSTEM` stays internal (it holds the device's `ppsspp.ini`); `PSP/PPSSPP_STATE` goes to `ppsspp/linux/` | `SAVEDATA` |
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
| **Per platform, on the drive** | each platform has its own copy in `app/` (Android) or `linux/`; Android also keeps some directly under `<emu>/` (section 7.2), so only the subfolders a definition's storage block names are shared | save states of every emulator; Android `shared_prefs`; Flycast `emu.cfg`, ARMSX2 inis, PPSSPP `PSP/SYSTEM` and `PSP/PPSSPP_STATE` on Android; anything a Linux device should hand to another Linux device |
| **Per platform or per device, internal** | never on the drive from Linux | RetroArch's main config, `config/` (core options, overrides, shader presets), remaps, playlists; emulator ini files that name a renderer; shader caches, logs, controller autoconfig, hotkey bindings generated for this device's pad |

The class is a column of the emulator definition's storage block, not code. The Android move already skips caches (`cache`, `shaders`, `log` for Azahar; `cache`, `resources`, `logs` for ARMSX2).

Why so little is shared in v1, and what it would take to share more:

- **Core options and overrides.** A `.opt` file holds renderer, resolution and dynarec choices that are right for a T820 and wrong for an RK3566, and they are tuned per device. Later: a one-way import of an allow-list of keys per core.
- **Save states.** Named after the ROM only; core, version and word size differ per platform. Later ("share states later", after v1): only cores that pass open question 11's test (same-commit 64-bit builds; save on the Cube, load on the ARC and back), with a sidecar stamp per state (core library name, commit, word size) and a refusal to auto-load on mismatch.
- **File names.** FAT forbids `: * ? " < > |` and trailing dots. RetroArch names saves after the ROM file, which is already on the drive; standalone emulators that name files after a game title need checking.
- **Timestamps.** FAT stores local time at 2 s resolution; the sync already uses a content hash for that reason (P:17598).

### 7.6 Library mapping

| Item | Rule |
|---|---|
| ROM folders | ES-DE system names. All 41 `romDir`s in `mods/nano-systems.txt` already are. Aliases are read (`esdeCanonSystemName`), never written |
| `gamelist.xml` | as today: paths relative to the system folder; unknown tags kept |
| `<altemulator>` / `<alternativeEmulator>` | **A writer writes a label only if it exists in both the `android` and the `linuxarm` table for that system.** Otherwise the choice goes into `nano/emulators.json` as a family key and the gamelist label is left alone. Reason: mainline ES-DE treats an unknown label as invalid. It launches with the system's default, logs a warning, shows "INVALID ENTRY" with a "CLEAR INVALID ENTRY" action, and a bad per-game label also stops a valid system-wide one from applying (`FileData.cpp`, `GamelistFileParser.cpp`) |
| `collections/*.cfg` | `%ROMPATH%/<system>/<game>` inside the ROM folder; else the canonical absolute form `/storage/<volume id>/...` (principle 2) |
| `settings/es_settings.xml` | `ROMDirectory` is written only when empty. Linux Nano reads it with the same re-rooting rule |
| `custom_systems/es_systems.xml` | Linux Nano **reads** it for a system's name, path, extensions, platform and theme (the reader the launcher plan's Phase 3 adds on both Android sides), ignores `<command>` lines it can't interpret, never rewrites an entry, and appends an entry for a system in a non-ES-DE folder in the same append-only way Android does |
| `nano/systems.json` | **Linux never writes launch fields into it.** Today it is one whole file, compared as text, and a released Android build that meets a launch tuple it doesn't know logs "this device's kept", substitutes its own and saves (P:58602). So a Linux value there would be erased on the next Android visit. Linux reads the file, maps `coreSo` and `package` to its own emulators, and stores its own per-system choice in `nano/systems.linux.json`, which Android ignores. The Android-side change (never overwrite a launch tuple it doesn't know) is shared `LIBRARY-FORMAT.md` work with the launcher plan's Phase 3. That plan's §3 item 4 says the firmware keeps a card's systems it can't validate; that holds only where the device has no system of that id. The code confirms the rule (2026-11-25): `esdeSystemsJson` (P:58464-58480) rebuilds the file from structs. A system Android uses loses its unknown fields, any `scanSource` path not on Android storage (P:58591-58595) and an unknown launch tuple. Only systems Android leaves out entirely (a bad name, a duplicated id, an unknown emulator with no local system of that id) are kept raw, and only while the card path is the same one (P:58478, P:58620-58630). So `nano/systems.linux.json` stays the only place for Linux system data |
| `nano/emulators.json` | gains a `key` field (`core:<name>` or a family) beside the Android-only `am` string. **Released builds keep it** (open question 9, answered from code 2026-11-25, for the test 34 to test 41 private draft builds; the writer is unchanged since b49719b). It is the only one of these files Android edits in place: `esdeStoreSaveGameEmu` (P:20131-20182, called only from P:41094) reuses the parsed root object, so unknown top-level keys and every other game's entry survive, nested content included, and an unreadable file is never overwritten. The exception is the one game the user re-picks on Android: its entry is deleted, and replaced by `{game,label,am}` only when ES-DE has no label for the new emulator; otherwise (an `<altemulator>` label, or a reset) it is gone. Last writer wins per game, which is acceptable. Consequences: (a) when Linux reads an entry with no `key`, it derives the family from `am`, or from the `<altemulator>` label if the entry is gone; (b) Linux never relies on a top-level `"version"` other than 1 here, since Android forces it to 1: the library is versioned by `nano/format.json` (section 7.7); (c) Linux stores no non-whole numbers and none of 1e15 or more (Android rewrites them with `%.6g`; infinity becomes null), and no `\u` escape of a lone surrogate (it becomes U+FFFD): strings and small whole numbers are safe; (d) an entry Linux writes with a `key` and no `am` is invisible to Android and survives its rewrites; (e) Linux spells `game` exactly as Android does (ES-DE folder + `/` + the path relative to it, case kept, no `./`) and keeps at most one entry per game: Android matches a re-pick by that exact string, and on load the first entry that resolves to a game wins, so a differently spelled Linux entry with an `am` would survive Android's re-pick and could win over it; (f) on Android a per-game `<altemulator>` label beats the JSON entry (P:20095-20101). The Phase 3 device check confirms this |
| `nano/names.json`, `index.json` | the launcher plan's change (no volume-bound paths) applies to all three. **Both are rebuilt from structs on every Android save**, so no Linux-only field may live in them, top-level or per entry. Android prunes a `names.json` entry whose `rom` path fails `stat` on Android (`content://` and `/mnt/shares/` paths are exempt), and also drops a name that differs from the gamelist's; so the no-volume-bound-paths change must land on the Android side before Linux writes `names.json` at all, and until then Linux keeps its titles in the gamelists' `<name>` or in a Linux-only file. On the card, Android clears `index.json` box and fan paths outside its own media folders (`downloaded_media` or the scrape cache), so Linux cover paths use the same folders and path form, or stay out of `index.json`. `when` is clamped to a 32-bit int on load |

### 7.7 Identity and `emulator_config/card.json`

Identity stays the volume id, as today: the FAT or exFAT serial on Linux and stock LineageOS, and a mount-order index on the RG Cube (section 7.3). How Linux relates the two is decision 20; one of its options has Android record its id in this file.

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
- **The Cube's kernel** (found 2026-11-25 from the release `.pac`): Unisoc's non-GKI **5.4.233**, not GKI 5.15, with vfat built in and a backported `fs/exfat`. Strings in the `.pac` and the absence of 5.18's `keep_last_dots` point to a snapshot from before 5.18 *(likely; a vendor cherry-pick can't be ruled out without the vendor source or a device test)*. That driver does not set VolumeDirty at mount; it sets it on each metadata change and clears it again right after each directory operation and on every `sync(2)`, writing the clear lazily and the set synchronously. So there is a window: a file has finished allocating and its size update is still pending; another directory operation or a sync clears the flag; the clean boot sector reaches the card; the card is pulled. Such a card looks clean and holds lost or torn allocations. a4a3d8c52d95 (5.18) and 46a557694b46 (6.16) fixed exactly this upstream, and the Thimsy Base's kernels (each family's ROCKNIX version, decision 22; all newer than 6.16, which carries the exFAT fix) have both. RK3588's vendor BSP 6.1 kernel is older than 6.16: it needs the backport, or is checked in wave 4. Section 7.3's exFAT check rule follows from it.

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
| a `key` field in `nano/emulators.json` | kept, except on a game the user re-picks on Android, whose entry is replaced or removed (section 7.6; from code, builds from test 34 on) |
| absolute paths to the drive | **the Cube's builds write `/storage/00000000-0000-0000-0000-00000000000N/...`, not `/storage/<serial>/...`** (section 7.3): GammaOS Next's vold names each public volume by a mount-order index from 1 to 100, where stock LineageOS 21 uses the serial. Every card the Cube has already written holds paths in that form, and the index can differ between visits (a USB drive mounted first, extra volumes on the card). `emusync::isVolumeId` rejects the form. Whether Linux makes those paths resolve is decision 20 |
| an exFAT drive pulled from the Cube | the Cube's pre-5.18 exFAT driver can leave a torn volume flagged clean (section 7.8), so Linux does not trust the flag on exFAT (section 7.3). A FAT32 drive arrives flagged dirty, and Android's `fsck_msdos` never clears the boot-sector bit, so Linux checks it once |
| a drive first set up on Linux | with Emulator Files on card on and the drive in the microSD slot (a USB drive is never adopted), one of two things. If `emuSdForeign` (P:67621) is true (a sync base that names other cards but not this one, or no base while its emulators already hold settings), it shows "This microSD Card Has Emulator Files... from another device or an older copy", with a wholesale choice. Otherwise it adopts the drive with no question (`emuSdAdoptBegin(card, false)`, the automatic move) and the card's copy wins. The dialog wording needs a Linux-aware variant in the next Android build; Phase 3 tests both paths (interop row 3) |
| per-game emulator labels in gamelists | writes `<altemulator>` whenever ES-DE's `android` table has the label (P:20129-20147, P:66137), so Android-only labels such as "M64Plus FZ (Standalone)" are already on cards. The label rule of section 7.6 binds only builds from the floor on. Linux Nano reads them through the family table; a shipped Linux ES-DE gets no linked gamelists until then (section 6.1) |
| a drive with `ES-DE/` and no `format.json` | every existing card. Linux reads it as today's layout |

The first Android release that implements this spec is the compatibility floor. Below it, the rule is only "nothing is lost": Linux-only values live in files old builds ignore, or in unknown fields of `nano/emulators.json` entries that old builds do not themselves change, or under `linux/`, where the id-repair pass edits text configs but finds no absolute drive path to rewrite (section 7.4, rule 6).

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
| **A. A full per-device image built by Thimsy, streamed to the card** | decompress, bmap, write, verify | **v1.** No ext4, EROFS or squashfs code on the user's machine; works on Windows without WSL |
| B. Download the upstream ROCKNIX image and swap `KERNEL` and `SYSTEM` inside its FAT | - | **Moot since 2026-11-25:** there is no upstream image to start from |
| C. Assemble on the client from parts (partition table, U-Boot blob or OpenABL ESP, the tier's prebuilt EROFS root, GPU pack, hardware pack, `device.json`) | `gptman`/`mbrman`; no filesystem creation beyond the FAT the writer already edits *(guess)* | **Proposed for v1** (decision 13, reopened): about 83 complete images per release would be about 66 GB at 800 MB each *(guess)*, against about 3 to 4 GB of tier roots and packs *(guess)*. Plain `.img.zst` files stay for certified devices |
| Creating ext4, EROFS or squashfs on the client | no mature pure-Rust ext4 writer; `mke2fs` is not on Windows | **Avoid.** |

The image follows FlipBase's card layout (`docs/card.md`), with sizes set per SoC family:

- GPT. A `uboot` partition at LBA 16384 (FlipBase: 8 MiB, plus an 8 MiB spare).
- A `boot` partition holding a raw FIT: the zstd kernel and the DTB with its bootargs, behind a 512-byte header, with no hash nodes (FlipBase: 40 MiB, plus a 40 MiB spare).
- The root filesystem: EROFS, lz4hc with 16 KiB pclusters, mounted read-only (FlipBase: 512 MiB, plus a 512 MiB spare; Thimsy's, with Nano and the emulators, is larger *(guess)*).
- A data partition, ext4 (FlipBase: 128 MiB), mounted at `/storage`.
- An exFAT partition, grown to fill the card on first boot.
- **Partition type GUIDs** (open question 8, answered from code 2026-11-25). Android's vold makes a public volume of every GPT entry typed Microsoft basic data or Linux filesystem (`0FC63DAF-8483-4772-8E79-3D69D8477DE4`), whatever its number (`Disk.cpp:433-437`; byte-identical in GammaOS Next 14 and LineageOS 21). So:
  - the exFAT drive, entry 5, is **Microsoft basic data, `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`** (as FlipBase types its `primary`), and stays at **entry 7 or lower**: vold takes the partition's minor as the disk's minor plus N, and MMC defaults to 8 minors per device (`perdev_minors`; the check even accepts N = 8, which collides with the next device);
  - entries 1 to 4 carry types vold ignores, never basic data and never Linux filesystem. Suggested: `uboot` and `boot` as Linux reserved `8DA63339-0007-60C0-C436-083AC8230908`; the root filesystem as the arm64 root type `B921B045-1DF0-41C3-AF44-4C6F280D3FAE`; the ext4 data partition as Linux home `933AC7E1-2EB4-4F13-B844-0E14E2AEF915` or server data `3B8F8425-20E0-4F3B-907F-1A25A76F98E8`. FlipBase types all four as Linux filesystem (`tools/mkgpt.py:51-80`); copying that would make five volumes on an Android device, three of them "unmountable" with a format prompt (section 13). On the Linux side nothing depends on these types: FlipBase's U-Boot finds `boot` by name, and `gptgrow` finds `primary` by name and checks only its type. Whether the kernel finds the root by entry number is not yet verified;
  - the rule holds for every layout, the dual-boot ones of section 8.6 included;
  - **the image build and the writer check it**: the written GPT has exactly one entry with a type vold mounts, at entry 7 or lower, so an Android device sees exactly one public volume per Thimsy card (section 8.5's verify). The Phase 0 device test confirms it on the Cube.
- No initramfs; `init=/init` is required. The A/B spare halves stay unallocated until an update writes them (section 5.2).
- On Linux-only RK3566 units the idbloader (rkbin's DDR init and the mainline SPL) goes at sector 64. On the ARC-D the layout follows the SD-only dual-boot route Phase 0 proves (section 8.6). H700 keeps its SPL at 8 KiB; its layout is settled when that family starts. S922X's FIP starts at sector 1. The Qualcomm families have no U-Boot on the card: a FAT ESP with the BLS entry (section 3.2) takes the place of the `uboot` and `boot` partitions.

**No FAT edit for the device choice.** Each image already holds its board's U-Boot, DTB, quirks overlay and `thimsy/device.json` (section 8.4), so steps 6 and 7 of section 8.5 are skipped for these images. The writer keeps them for the other layouts the SdCard backend serves (`multi_device_plan.md`) and for any image that does carry a FAT to customise. The one per-install choice left is the package profile: the setup wizard asks for it on first boot, or the installer writes it if Phase 4 gives the image a place for it *(open; decision 12)*.

Release artifacts: per device or variant, `thimsy-linux-<ver>-<device>.img.zst`, its `.bmap` (block map with per-range SHA-256), a `pack.json` and the update file; once per SoC-family build, the `legal-info` sources, the Buildroot tarball and the external tree (section 3.3). Split into 2 GiB parts if GitHub requires (the engine's `MultiVolume` reader handles that). If the installer assembles cards (decision 13, proposed), complete images are published only for certified devices, and each release publishes the tier roots, GPU packs and hardware packs, each with its hash, instead.

### 8.3 Catalog and package additions

**Package format.** A disk-image package does not fit format 1: it has no official base, and one image serves many devices. It needs **`pack.json` format 2**, which `multi_device_plan.md` §7.4 defines and the launcher plan's Phase 6 also extends. For this plan format 2 must allow:

- `system` and `source_sha256` optional; `reference_sha256` is the image's hash;
- a binding to a list of devices or an image id instead of one `device`, and no `profile` binding;
- `format: 2`, which an older installer refuses by the format number with a clear message; `min_installer`, which format 1 already has, stays for gating newer installers within format 2;
- new operations, same closed-set rule. They extend the `sdcard:` actions `universal_installer.md` already lists (`select_card`, `write_image`, `verify`) rather than replace them: `disk_image` is `write_image` with a bmap, `require_card` belongs with `select_card`, and the `fat_*` operations are new `sdcard:` actions (names settled with that design):
  - `disk_image { artifact, bmap, image_size, image_sha256 }`
  - `fat_write { partition, path, artifact }` and `fat_template { partition, path, template }`
  - `fat_copy { partition, from, to }` (copy a file already in the FAT, such as an H700 panel's DTB to `dtb.img`; not needed if each H700 variant is its own image, decision 14)
  - `require_card { min_bytes }`

If the installer assembles cards (decision 13), format 2 also needs a `card_assemble { tier_root, gpu_pack, hardware_pack, device_json }` operation that writes the parts, each verified by its hash, into the family's layout.

**Catalog.** The Linux entries are **generated** by `thimsy-import` from `config.xml` (about 83 products from 97 DTB variants), plus Thimsy's overrides and each product's status:

- **built**: in `config.xml`, never reported on;
- **boots**: a tester's report with a first frame;
- **works**: every Device check item passes (section 11.2);
- **certified**: a hardware-in-the-loop unit, or "works" on two consecutive releases.

The installer offers a "built" product only behind an "untested device" confirmation. A product drops one level when a report fails on a new release; if it was certified, its family's import is held until that is fixed. In the schema `multi_device_plan.md` §4.2 and §7.1 introduce (today's keys are `identify`, `soc` and a required `hardware.android_api`, so `catalog.rs` changes first):

```yaml
id: anbernic-rg-arc-d
platform: linux                      # new: android (default) | linux
soc: rockchip-rk3566                 # key and id scheme: whatever multi_device_plan.md Phase 1's descriptor fixes
hardware: { arch: aarch64, ram_gb: 2, gpu_family: mali-bifrost }   # android_api becomes optional
match: { dt_model: "Anbernic RG ARC-D" }
controls: { face_layout: saturn, face_buttons: 6, sticks: none, pad_producer: padd }   # saturn: a new face_layout value
status: built                        # built | boots | works | certified (generated)
parts: { tier: v8.2, gpu: panfrost, hardware: rk3566 }   # if the installer assembles the card (decision 13)
flash:
  method: sdcard
  layout: raw                        # multi-device plan's enum: raw | rk_update_card | rk_boot_card | phoenixcard
  image: thimsy-linux-rk3566-anbernic-rg-arc-d   # one image per device or variant (section 8.4)
  min_bytes: 8000000000
boot: { dual_boot: android_emmc }   # ARC-D: the card layout of the SD-only route Phase 0 proves; nothing written to the eMMC (section 8.6)
variants: []                         # H700: ddr and panel; each combination is its own image
profiles: { recommended: small, allowed: [small, medium] }   # a departure from device_profiles_plan.md (section 8.4)
support: { install: { windows: automated, macos: automated, linux: automated } }
```

### 8.4 Per-device and per-profile builds

- **Per device.** One card per device, from a tier root, a GPU pack and a family's hardware pack (section 3.2). The card holds its board's U-Boot (or the ESP), DTB, kernel and `thimsy/device.json`, so nothing is edited for the device choice beyond what assembly writes (decision 13).
- **Variants that can't be detected.** H700 DDR3 and DDR4 units need different SPLs, and the panel revision (rev6, v2) can't be read; the RK3326 EE clones on `generic-dsi` and SM8250's Visionox panel are the same kind (open question 32). The variants come from `config.xml`. Each combination is its own variant card, chosen by catalog variant questions (typed, with pictures). A "change panel choice" flow rewrites the `boot` partition (kernel and DTB) from the other variant on an existing Thimsy card *(guess: settled with each family's layout)*, and a wrong-panel recovery is documented per family. Decision 14 (reopened).
- **Qualcomm products (wave 3, deferred: later, needs bootloader writing).** OpenABL is flashed from Android first, through the installer's `adb` backend with `backup_abl.sh` and `flash_abl.sh` semantics; Android stays on all of them (about 32 to 35 products, depending on how variants are counted). Firmware that can't be shipped is fetched from the unit over adb at the same step (decision 24). After that the unit boots from SD. Flashing OpenABL writes a bootloader to internal storage, so this step waits until that is allowed with a verified backup (section 1); the analysis stands for then.
- **Per package profile.** `device_profiles_plan.md` resolves a package per (device, profile) at build time and says "the profile decides what ships". v1 departs from that in two ways. First, Linux images offer only Small and Medium (`allowed` in the catalog), while that plan makes every profile selectable on every device and never greys one out: Large and Ultra need the heavy emulators that RK3566 and H700 images don't contain (section 6.3), so they are not offered there. Decision 12. Second, each image's root filesystem contains the superset its family can run, the resolver runs **at image build time** and writes one resolved manifest per (device, profile) into `/usr/share/thimsy/`, and first boot picks one by `thimsy/device.json` and the chosen profile. So the profile enables and sets defaults; it does not shrink the download. On RK3566 and H700 the difference would be small *(guess: a few hundred MB)*. Per-device packaging would also allow one image per profile; v1 keeps the superset (decision 12). If the difference grows on Qualcomm devices, optional packs become extra EROFS images on the data partition *(guess)*.
- **Seeding.** Pack-in games and their gamelists (`bundled_roms.py`, after the changes in section 4.2) go into the image's internal `ROMs` and `ES-DE`, as the Android build seeds `/sdcard`.

**The storage job** (owner requirement, 2026-11-25). The installer writes storage devices, microSD game cards and USB drives, as a first-class job of its own, for Android and Linux devices alike. It replaces the earlier optional "Prepare a game card" step. It uses the same flow as the firmware install: pick the device, pick the card or drive (with the same safety rules, section 8.5), pick the contents, write, verify. It has four parts:

| Part | What it does |
|---|---|
| 1. Format and lay out | exFAT on a partition of the Microsoft basic-data type (section 8.2), a non-zero serial (section 7.3), and the card contract's folders: `ES-DE/`, `ROMs/<system>/` for each system, `emulator_config/`, `bios/` (section 7.2) |
| 2. Pack-ins | the pack-in games for the chosen package profile, with their art and gamelists (`bundled_roms.py`'s data, section 4.2) |
| 3. ROMs and BIOS from the PC | the user's ROMs and BIOS files copied from the computer, sorted into system folders; each BIOS file checked by hash |
| 4. Update or repair | a non-destructive update of an existing card: the layout and the pack-ins brought up to date, never a format |

The same requirement goes into `universal_installer.md` in the installer repo (branch `universal-installer`), whose flow and `sdcard` crate it shares; its section 7.3 holds the job's design and its open decisions 11 to 19. The handheld can still format a card itself.

### 8.5 The raw disk writer

The crate `installer/engine/crates/sdcard/` that `universal_installer.md` §5.1 reserves ("raw card writer with verification"): enumerate, open, lock, aligned I/O and eject, plus source sniffing, bmap, write, FAT edit and verify. One writer serves this plan and `multi_device_plan.md`'s SdCard backend (section 4.5). Closest existing Rust template: BeagleBoard's `bb-imager-rs` (MIT). Reference behaviour: Raspberry Pi Imager.

**Which drives are offered** (enforced in the engine; the app only displays):

| Rule | Detail |
|---|---|
| Each disk is classified | **card** (SD or MMC bus, or USB with removable media), **USB flash**, or **USB fixed disk** (external hard disks and SSDs) |
| Cards and USB flash are offered | Windows: SetupDi disk enumeration, `IOCTL_STORAGE_QUERY_PROPERTY`. macOS (open question 16, answered from public sources 2026-11-25): every physical whole disk (`diskutil list -plist physical`, then `diskutil info -plist`). **A card is `BusProtocol` == `"Secure Digital"`** (the text output's "Protocol:"; IOKit's kIOPropertyPhysicalInterconnectTypeSecureDigital, which DiskArbitration copies unchanged from the reader's Physical Interconnect), the built-in AppleSDXC slot on Intel and Apple silicon MacBook Pros. A USB disk with `RemovableMedia` true is also offered. An internal disk is offered only when its `BusProtocol` is Secure Digital, or when it is `Internal` && `RemovableMedia` && `BusProtocol` `"USB"` (the internal USB readers of some older Macs). The built-in slot reports `Internal` true, so `Internal` and "Device Location" never tell a card from a disk, and `SolidState` is not used either (nothing documents its value for SD media); `RemovableMedia` does (true for the card, false for the boot SSD). Optional extra signal: the media icon's `IOBundleResourceFile` == `"SD.icns"`, the test drivelist, Raspberry Pi Imager and bb-drivelist use; it catches only built-in slots (USB readers give `Removable.icns`) and is not in diskutil's plist, so it needs DiskArbitration or `ioreg`. Consider reading DiskArbitration directly in the `sdcard` crate, as bb-drivelist does, instead of parsing diskutil. Linux: UDisks2, with `lsblk` and `/sys/block/*/device/type` as fallback |
| USB fixed disks always need a typed confirmation, whatever their size | they are the likeliest wrong target and usually hold backups |
| Any disk also needs the typed confirmation when it is above a size threshold *(guess: 512 GB)*, or holds NTFS, APFS, HFS+, ext4 or BitLocker volumes | the removable-media bit is set by the device, so an external SSD that reports it would otherwise be classed as a card |
| System disks are never offered | Windows: any disk holding `%SystemRoot%`, the EFI system partition, a page or hibernation file, a spanned or dynamic volume. macOS: any disk with `Internal` true and `RemovableMedia` false, whatever `BusProtocol` says ("Apple Fabric" on Apple silicon, "PCI-Express" or "SATA" on Intel; drivelist's and bb-drivelist's isSystem, which Raspberry Pi Imager also applies only to non-cards); and the physical store behind the running system, traced from `diskutil info -plist /` (a synthesized APFS disk) through `diskutil apfs list -plist`, or an IORegistry walk like rpi-imager's `findAPFSParent`, to its whole disk. Synthesized APFS containers report Virtual and Internal, so they are traced, not merely skipped. Linux: every disk reached from a mounted filesystem or swap through `/sys/block/*/holders` and `slaves` (dm, md, LUKS, bcache, loop backing files), btrfs and ZFS members, anything in `/etc/fstab` or `crypttab`, and the stick a live session booted from |
| "Show all drives" (advanced) adds other buses | it never adds a system disk |
| Virtual disks are hidden | VHD, Storage Spaces, loop devices; on macOS, `BusProtocol` `"Disk Image"` (current macOS), `"Virtual Interface"` (older), or `VirtualOrPhysical` == `"Virtual"` (shown only under a `test-hooks` build feature). Not a missing `Internal` key: a current hdiutil image sets `Internal` false and otherwise looks like an external removable disk, and the "Virtual Interface" test drivelist, rpi-imager and bb-drivelist use does not catch it |
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

1. Zero the first and last 1 MiB (old partition tables, including the backup GPT), with the probe's timeouts. Doing this first means a cancelled or failed probe never leaves the old partition table over data the probe has overwritten.
2. **Capacity probe.** Write a random block at the last MiB and at offsets spread over the reported capacity, flush, read them back uncached, zero them. Every end-of-device write has a timeout. A mismatch, a wrap-around or a timeout fails with `drive_fake_capacity` and nothing more is written. Counterfeit cards are common, and they pass a read-back of the first few GB.
3. Write the bmap's mapped ranges, **holding back the first 1 MiB** in memory. Aligned 4 MiB buffers; decompression, hashing and writing on separate threads.
4. Flush.
5. **Verify** every mapped range against the bmap's per-range SHA-256, read back with the cache bypassed. A range that overlaps the first MiB (H700's SPL at 8 KiB runs past it) is checked from the held-back buffer for that part.
6. **Then** edit the boot FAT in place through the same exclusive handle (pure-Rust `fatfs`; the partition's offset comes from the held-back first MiB, not from the card). `fatfs` writes through a wrapper that records every block it writes and the bytes written. The full bmap check does not run again after this step, because the edit changes the FAT's blocks.
7. Flush; read back uncached every block the wrapper recorded (FAT tables, directory entries, file data) and compare it with what was written; re-check every bmap range in the boot partition that the edit did not touch against its SHA-256 (so the boot partition's other files are verified after the edit too); and re-read each edited FAT file and compare size and hash.
8. Write the held-back first 1 MiB (partition table, start of the bootloader), flush, verify it. Before writing it, check the GPT's type rule (section 8.2): exactly one entry with a type Android's vold mounts, at entry 7 or lower. A Thimsy image that fails it is refused.
9. Eject through the OS before releasing the handle where possible (`diskutil eject`; UDisks2 `Drive.Eject`). On Windows, eject at once and tell the user to ignore any "format this disk" prompt for the ext4 partition.

Cancelling at any point leaves a card with no partition table, never a half-bootable one. Verification is on by default, with an advanced "skip" switch. A `verify_failed` message says the card may be failing or write-protected.

**Bootloader layouts.** The writer knows four, each explicit in the pack: Rockchip idbloader at sector 64; sunxi SPL at 8 KiB; Amlogic FIP from sector 1, skipping the MBR; and EFI (a FAT ESP only) for OpenABL families. The held-back first MiB covers all three raw offsets.

**Sources and formats:** sniff by magic bytes, not extension: zstd (Thimsy's images), gzip and xz (other distributions' images). Write from the verified cache, never straight from the network.

**USB drives.** The same writer. No RK3566 or H700 handheld boots from USB by default, so this matters for x86 handhelds and QEMU targets, and for SD cards in USB readers.

### 8.6 Flows and other changes

- **Wizard:** for `method: sdcard` the steps "unlock" and "connect" are replaced by "choose card" (`home_shell.dart`'s step list). New `drive_screen.dart` with a live list (`drives --json --watch`).
- **Protocol:** `drives`, `write --drive PATH --fingerprint FP`; stages `check`, `unmount`, `elevate`, `probe`, `write`, `verify`, `customize`, `eject`; errors `drive_changed`, `drive_protected`, `drive_too_small`, `drive_busy`, `drive_fake_capacity`, `elevation_declined`, `write_failed`, `verify_failed`.
- **Qualcomm devices (wave 3, deferred: later, needs bootloader writing):** OpenABL is flashed from Android first, through the same `adb` and fastboot work, with Android kept on all of them (about 32 to 35 products, depending on how variants are counted); the step is done by hand once on each hardware-in-the-loop unit, which boots from SD after that. It writes a bootloader to internal storage, so it waits until that is allowed with a verified backup (section 1).
- **Tests:** a regular file as a disk under `test-hooks`; loop devices in Linux CI, including a wrap-around device-mapper target for the capacity probe; `hdiutil` images on macOS (which report `BusProtocol` "Disk Image", `Internal` false and `VirtualOrPhysical` "Virtual" on current macOS, and must be refused as virtual without `test-hooks`); a mounted VHD on Windows (which must be refused as virtual without `test-hooks`); fuzzing of the bmap parser and the format sniffer.

**The ARC-D: dual boot (open question 2, answered 2026-11-25).** The owner has an ARC-D only, and wants Android kept on its eMMC with Thimsy booting from card slot 1. Under the SD-only principle (section 1) the install never writes the eMMC. The ARC-D's stock eMMC loader already does this for Anbernic's own Linux card. ROCKNIX and Batocera say mainline images need Android wiped; ROCKNIX's page gives no technical reason. What frames it:

- **The boot ROM always runs the eMMC loader first.** The RK3566 BootROM order is fixed: SPI NOR, SPI NAND, eMMC, then SD, then USB maskrom only when no medium holds a valid idbloader (at sector 64). On an ARC-D with Android, the eMMC's vendor loader runs first and the card's own idbloader is never read *(likely)*.
- **JELOS did it.** JELOS, ROCKNIX's predecessor, booted from a card on Anbernic RK3566 units with Android installed: its image had GPT partitions `uboot` (sectors 16384 to 24575, `u-boot.itb`) and `trust` (24576 to 32767, `trust.img`), with the OS on partition 3, "where the Anbernic Android bootloader expects it to be". ROCKNIX PR #154 (2024, closed unmerged) would have restored that. It says "The Anbernic bootloader is not compatible with mainline sleep" (the PR turned sleep off when Android's partitions exist) and that mainline U-Boot "would also break sleep on Android with the BSP kernel".
- **FlipBase does the same thing on another RK3566 device.** On the Flip, the vendor DDR blob and vendor SPL from internal flash load a FIT of mainline U-Boot plus the vendor BL31 and OP-TEE from an SD partition named `uboot` (LBA 16384). The vendor SPL finds the partition by name (the Miyoo build falls back to sector 0x4000), checks a SHA-256 per image and no signature. ROCKNIX's and Zlyme's mainline `u-boot.itb` also boot through the Flip's stock SPL. That is proven on the Flip, not on Anbernic's eMMC loader.
- **A community account** (Reddit, 2024, not verified) says the RG353's stock first stage looks for its second stage on SD and then eMMC, that holding F at power-on ignores the card and boots Android, and that the stock first stage "has issues" with a ROCKNIX card *(guess)*.
- **Not established:** whether the ARC-D's loader is a U-Boot SPL that loads a FIT (FlipBase's case) or a legacy miniloader that loads separate `uboot.img` and `trust.img` (which JELOS's `trust` partition suggests), and which BL31 runs under route (a): the vendor's from the eMMC, or the one in our card's FIT. Open question 20.

Phase 0 spikes three SD-only routes, none of which writes the eMMC: (d), then (a), then (b). Route (c) writes the eMMC's bootloader area, so it is **deferred** until writing a bootloader to internal storage with a verified backup is allowed (section 1); it stays in the table for then. Erasing Android is not a route: the owner dropped it permanently.

| Route | What | Writes the eMMC | Known risk |
|---|---|---|---|
| **(a)** | The stock eMMC loader hands off to our card's mainline U-Boot, packaged as its SD stage expects: a `uboot` partition at LBA 16384 and, if the loader is a miniloader, a `trust` partition at 24576. JELOS and FlipBase are the precedent | no | **Sleep**: "not compatible with mainline sleep" (PR #154). FlipBase solved the same class of problem on the same SoC (vendor BL31, mainline kernel) with its pm-config kernel patch, which sends BL31 the stock sleep mode; it is ported first. **DDR blob and BL31 pairing**: FlipBase keeps the unit's own BL31 for that reason; under route (a) the eMMC loader already runs Anbernic's BL31, which is decision 18's choice for RK3566 anyway; Phase 0 confirms which BL31 runs (open question 20) |
| **(b)** | The vendor U-Boot from the eMMC chainloads a mainline U-Boot stored on our card | no | the same sleep question; one more stage of boot time |
| (c), **deferred** | Only the eMMC's bootloader is replaced, by a mainline U-Boot that boots Android from eMMC and Thimsy from SD | **yes**, the bootloader area | not tried now (section 1). For later: shown on the Retro Lite CM5, not on the ARC-D; per PR #154, mainline U-Boot "would also break sleep on Android with the BSP kernel"; recovery through Rockchip maskrom, but once a valid loader is on the eMMC the BootROM never falls through to SD, and reaching maskrom on an ARC-D may mean opening the unit to disable the eMMC unless U-Boot offers rockusb or the board has a maskrom key *(neither verified)*; a verified backup of the bootloader area first |
| **(d)** | Anbernic's own eMMC U-Boot boots our kernel from SD directly, with no mainline U-Boot: ROCKNIX PR #154 put `/flash` on partition 3 "which is where the Anbernic Android bootloader expects it" | no | Batocera says the eMMC bootloader "is not compatible with later Linux kernels", so it may not boot a mainline kernel at all *(guess)*; the same sleep question as (a); no U-Boot boot-time fixes apply |

**Phase 0's order and pass rule** (open questions 20 and 21, made Phase 0 items 2026-11-25). Nothing is written to the eMMC at any step:

1. Read the ARC-D's eMMC loader, read-only, over adb as root: sector 64 of `mmcblk0`. Find whether it is a U-Boot SPL that loads a FIT by partition name (FlipBase's case) or a miniloader that expects `uboot` and `trust` partitions (JELOS's layout), and extract its `spl-boot-order`, as FlipBase did with the Flip's `mtd5`.
2. Route (d).
3. Route (a).
4. Route (b).

A route passes when: Thimsy boots from slot 1; Android boots untouched with the card out; deep sleep works under Thimsy (FlipBase's pm-config patch is ported first) and its current is measured; and the loader's added boot time is measured with the arch-counter method (section 9.2; open question 21). The RG353P/M/V, which also keep Android on eMMC, follow whichever route works on the ARC-D.

**If no route passes.** The ARC-D waits for bootloader writing (later, route (c)), and the ARC-S becomes the first device. No erase is offered.

**Boot time under (a) and (b).** Anbernic's loader runs before our U-Boot and can cost time *(guess: 0.5 to 1.5 s; open question 21)*. Section 9.4 says why that makes the 3 s reference unit a Linux-only one (or, once bootloader writing is allowed, a route-(c) ARC-D).

**In the installer.** It writes the card layout of the route Phase 0 proves, and nothing else: no install step writes the eMMC, and removing the card gives back the untouched ARC-D. The ARC-S, RG353VS/PS and RG503 need none of this.

---

## 9. Boot-time plan

### 9.1 "Paper Plane"

**No project of that name was found.** Searched: the web (standard and extended), GitHub repository search (about 740 `paperplane` repositories), code and issue search in the spruce, NextUI and ROCKNIX organisations, the release code names of muOS, Knulli and spruce, and every document, release note, commit message and author profile of the two projects below. Reddit, Discord and two wikis could not be queried. The name collides only with unrelated projects. **Answered by the owner (2026-11-25): "Paper Plane" is FlipBase or BaseOS under a misremembered name.** FlipBase, on the ARC's SoC, is the reference.

The plan takes its techniques from the projects that do publish very fast boots:

| Project | What it is | Published boot | Licence |
|---|---|---|---|
| **FlipBase** 2.0.0 (tag) | A minimal base for the Miyoo Flip (**RK3566, the ARC's SoC**): mainline U-Boot 2026.10 and Linux 7.3.5, Panfrost or libmali, Buildroot, EROFS root, no initramfs, BusyBox init, no udev. It mounts the user's card and hands off to the frontend there (NextUI, spruce) | On its boot counter, whose zero is about 0.5 s after power-on. Warm reboots: U-Boot done 0.65 s, `/init` 1.1 s, logo 1.2 s, **init execs the frontend's launcher at 1.3 s**, `nextui.elf` starts at 1.9 s. Vendor kernel, mainline U-Boot: launcher 2.0 s and `nextui.elf` 2.4 s on cold boots, first NextUI frame 2.9 to 3.0 s on warm reboots (U-Boot's stages are the same cold). A filmed cold boot (before the last fixes) showed the menu 3.97 s after the power LED. Stock: 15.8 s to the launcher | MIT |
| **BaseOS-RG353PS** 1.2.10 | A fork of FlipBase for the RG353PS (RK3566), branched from the pre-Buildroot tree: shell build scripts in Docker, Linux 7.2.7 with ROCKNIX's RK3566 config and device patches, mainline U-Boot v2026.01 with rkbin's DDR blob and BL31, Panfrost, extlinux through a bootflow scan | Warm, counted from near U-Boot's start, so the bootrom, TPL and SPL are **not** counted: before Linux 2.545 → 1.475 s (data cache before relocation, no panel-ID probe, no 200 ms start-up buzz, `blkcache`); to the menu 3.70 to 3.92 s. The SD-clock fix alone was worth about 1.0 s | MIT |
| **BaseOS** 1.5 (pre-release) | The H700 base FlipBase was forked from; spruce and NextUI use it on TF1. **Vendor 4.9 kernel and Mali blob, kept byte-identical**; the gains are in the boot chain and init | to frontend hand-off: 6.7 s (v1.0) → 3.16 s (v1.5.1) | **no licence file** |
| **RetroStoneOS** | Buildroot, mainline 6.18, one C frontend, libretro cores, no X or Wayland; Allwinner A20 | power-on to logo 2.09 s (SPL 468 ms, U-Boot 240, kernel read 231, LZ4 188, kernel 427, init to frontend 435); menu about 0.3 s later | code MIT; images non-commercial |

All of them get there the same way: they own the whole stack from the SPL to one frontend process, with BusyBox init and no service manager. FlipBase matters most here. It shows what the ARC's SoC can do: **a first frame about 3 s on its boot counter**, plus about 0.5 s from power-on (with mainline U-Boot; 2.3 to 2.7 s on warm reboots of the mainline kernel, section 9.2). That is now the target (decision 5), on a base of FlipBase's kind (decision 1).

### 9.2 Where the time goes, and how it is measured

FlipBase's measured boot on the Flip with mainline U-Boot, in seconds on its counter. The warm column is on Linux 7.3.5. The cold column predates the mainline kernel: FlipBase's `docs/boot-time.md` says that table was measured on the vendor 5.10 kernel. On the mainline kernel, FlipBase reports warm first frames of 2.57 to 2.67 s on libmali and 2.34 to 2.39 s on Panfrost (2026-10-29 and 10-30, `docs/mainline-compat.md`, before the November fixes), and the filmed cold boot below; no cold first-frame table on mainline is published.

| Event | Warm reboots (2026-11-22, four runs, Linux 7.3.5) | Vendor 5.10 kernel: warm reboots on 2026-09-21 (U-Boot hand-off, `nextui.elf`, first frame; FlipBase finds U-Boot's stages the same cold) and cold boots on 2026-10-07 (`Run /init`, hand-off) |
|---|---|---|
| U-Boot hands off | 0.645 | 0.94 |
| Kernel clock starts | 0.722 | - |
| `Run /init` | 1.10 to 1.12 | 1.83 |
| Logo drawn | 1.16 to 1.19 (on the panel about 1.21) | - |
| Frontend hand-off | 1.29 to 1.34 | 1.99 |
| `nextui.elf` starts | 1.87 to 1.97 | 2.40 to 2.42 |
| First NextUI frame | - | 2.93 to 2.98 |

A filmed cold boot (2026-10-31) showed the menu 3.97 s after the power LED, about 3.45 s on the counter; the LED leads the counter by about 0.5 s. Stock firmware: 15.79 s to the hand-off, 31.50 s to the first frame. The U-Boot time left on the Flip is 0.62 s: reading 0.30 s, zstd about 0.1 s, initialisation 0.13 s.

**How Thimsy measures, from the first build** (FlipBase's method):

- The SoC's architected counter, from reset. U-Boot's bootstage records go into the kernel's device tree (`/proc/device-tree/bootstage`), and a small tool (FlipBase's `bootclock`) offsets them to `CLOCK_MONOTONIC_RAW`.
- Breadcrumbs under `/run/boot-*`, written with shell builtins; process start times from `/proc/<pid>/stat`.
- A diagnostic-only probe that polls DRM state every 20 ms for the first frame. Nano also marks its own first complete home frame.
- The counter's zero is not power-on: U-Boot's first record reads 45 ms, after a bootrom, DDR init, SPL and BL31 that FlipBase puts at about 0.39 s, and a filmed boot showed the power LED about 0.5 s ahead of the counter. Thimsy states that offset with every figure, and films power-on to frame for releases.
- USB unplugged: a cable makes U-Boot check the charger.

For comparison, ROCKNIX's path, and why the base changed: SPL and U-Boot; the kernel plus a BusyBox initramfs that loop-mounts `SYSTEM` from the FAT and `switch_root`s to systemd; systemd up to `graphical`; then `rocknix-autostart`, after `network.target`, running quirk scripts, the automount and start scripts in series; only then Sway and the frontend. No figure is published *(guess: 10 to 20 s to the frontend on RK3566)*.

### 9.3 Techniques, and which apply

Gains in the "measured" column are the source project's own numbers on its own board. They show where time hides; they are not promises for the ARC. The base builds most of them in from Phase 2.

| Technique | Source | Applies to the Thimsy Base? | Measured or expected gain | Cost |
|---|---|---|---|---|
| **BusyBox init instead of systemd, no udev daemon** | all of them; their largest saving (9.9 s of stock userland on the Flip) | **yes: the base** | seconds | the base itself (Phase 2) |
| **Frontend first:** nothing between `rcS` and Nano's first frame but mounting `/storage` and loading the GPU driver; the drive mounts in the background; Wi-Fi, Bluetooth, D-Bus, NTP, Samba and SSH start after the first frame or on demand; `rcS` kept under 0.1 s (FlipBase cut its own to 0.06 s) | RetroStoneOS, FlipBase | **yes: by design** | built in | low; Nano must tolerate a late drive (the drive's grace waits for its first mount, section 7) and a late network |
| **No compositor for the home screen** | FlipBase, RetroStoneOS; section 5.3 | **yes** (decision 4, stage A), on every family; on the Qualcomm families a compositor may still run per game (section 5.3) | *(guess: 0.3 to 1 s avoided)* | the hand-off and the rotation contract (section 5.3) |
| **Kernel in a raw FIT read by partition name; no initramfs; root on its own partition** | FlipBase, RetroStoneOS, BaseOS (0.5 s) | **yes: the base's layout** (section 8.2). Unhashed, and loaded where it needs no copy: the RG353PS fork measured a hashed FIT with a copy as 613 ms slower and dropped it | 0.1 to 0.5 s | in the base |
| First frame from a snapshot: draw the home from a cached state, load lists in the background | RetroStoneOS ("the menu is drawn from a snapshot") | yes; fits Nano's existing state files | about 0.3 s and no "empty" flash | medium |
| Serial console and debug shell off in release builds | BaseOS (serial off: about 0.7 s on H700, in U-Boot) | yes: kernel parameters and a U-Boot option | measure | low; keep them in debug builds |
| U-Boot: no delay, no scan, no probing: `BOOTDELAY=-2`, no PCIe, USB, network or SFC driver, no environment, no bootflow or extlinux scan | FlipBase, RetroStoneOS, Bootlin | yes on RK3566 and H700; not on Qualcomm (ABL). An ARC-D on route (c), once bootloader writing is allowed, keeps U-Boot's eMMC driver | 0.1 to 0.3 s | low |
| **RK3566 U-Boot fixes:** SD clock at 50 MHz instead of 25 (a mainline RK3568 bug); data cache on before relocation (570 → 40 ms); `blkcache` sizing (202 → 53 ms); CPU at full clock during decompression; decompressor built at `-O2` without `-mstrict-align` (336 → 107 ms) | FlipBase (`docs/uboot.md`) | yes: same SoC, mainline U-Boot. Patches to carry, and to send upstream | about 1 s in total on the Flip; the SD-clock fix alone about 1.0 s on the RG353PS | low to medium |
| **Anbernic rgxx3 U-Boot:** no 200 ms start-up buzz, no panel-ID probe (34.5 ms) | RG353PS fork | the buzz on every rgxx3 board, both ARCs included; the panel-ID probe runs only on the RG353P, V, PS and VS | about 0.2 s on the ARCs, 0.23 s on the RG353s | trivial |
| Device tree: PMIC I2C at 400 kHz; panel init delays audited; display commit at driver bind so the panel powers up during kernel init | FlipBase | yes, per board | 0.1 to 0.3 s | low to medium |
| SD slot at SDR104 | FlipBase (1.06 s on the Flip) | **no gain: mainline already enables it for the ARC** | - | - |
| Kernel compression by benchmark (LZ4 or zstd against gzip) | RetroStoneOS, BaseOS, FlipBase, Bootlin | yes | 0.1 to 0.5 s | trivial |
| Root filesystem: EROFS, lz4hc, 16 KiB pclusters, read-only | BaseOS (LZ4HC: 0.7 s earlier first frame on a 20 MB/s card), FlipBase | yes: the base's root | measure | trivial |
| Trimmed per-SoC kernel config, no tracing; Wi-Fi, Bluetooth and USB host as modules loaded after the frontend | Bootlin (about 0.8 s on a slow board), FlipBase, RetroStoneOS | yes: the family's kernel config | 0.5 to 1 s | medium |
| **Trim ROCKNIX's full config with Thimsy's fragment** (section 3.2): ROCKNIX's RK3566 config builds in XFS, BTRFS, NFS, CFG80211, USB and sound, and leaves EROFS off | this plan; the conformance check guards it, and the release-image check compares the pre-fragment config | yes, per family; RK3566 first | measure; more than a trimmed FlipBase config costs *(guess)* until each family is tuned | medium per family; the fragment will be large |
| fsck the data drive only when needed: a vfat drive when its dirty flag is set; an exFAT drive when flagged or not last cleanly unmounted here, or always if Phase 0 finds `fsck.exfat` cheap | RetroStoneOS | yes; section 7.3 | avoids seconds on most boots; one pass is budgeted for the first boot after a drive moves between platforms (section 9.4) | low |
| Read the GPU library ahead at idle priority; `performance` governor until the menu is up | FlipBase (about 0.45 s to first frame, with the EROFS root), RetroStoneOS | yes | 0.1 to 0.5 s | trivial |
| Mesa shader cache on the writable partition, warmed at first boot | Mesa docs | yes | first-frame time on first run | low |
| **Splash from `/init`:** a statically linked splash through DRM fbdev emulation, started before BusyBox init, with the fbdev mode committed at registration so the panel powers up during kernel init; then Nano's home | FlipBase (logo on the panel about 1.2 s after reset, warm) | yes; FlipBase's `fbsplash.c` (BaseOS's) is used as-is and rewritten when convenient (open question 26) | perceived; the panel powers up earlier | low |
| U-Boot logo (a VOP2 driver in U-Boot) | FlipBase evaluated it: a logo about 0.65 s earlier, for "weeks of work" | possible, after v1 | perceived | high |
| `quiet` | Bootlin | yes | - | trivial |
| U-Boot Falcon mode | Bootlin (about 0.5 s) | possible *(guess)* on RK3566/H700; never on Qualcomm | 0.1 to 0.5 s | high. Defer |
| Kernel parameters `rcu_expedited`, `nokaslr`; `driver_async_probe=`; musl; I2C and PMIC drivers at `subsys_initcall` | BaseOS (0.2 s on the vendor kernel); FlipBase tried each on mainline and dropped it (`driver_async_probe` about 10 ms; the initcall change 60 ms for three driver patches) | no | none to small | - |
| Vendor-kernel tricks (U-Boot gzip selector patch, boot0 debug byte, late USB module) | BaseOS, FlipBase's vendor-kernel era | no | - | - |
| Hibernation or snapshot resume | Microchip | no: restoring 1 to 2 GB from SD is likely slower than a short cold boot *(guess)* | - | high |
| Resume the last game at boot from a save state | RetroStoneOS | yes (it is Quick Resume) | "time to playing" | later |

### 9.4 Targets

**Decided 2026-11-25 (decision 5): 3 s cold boot, power-on to Nano's first complete home frame**, on a Linux-only reference unit (or, once bootloader writing is allowed, an ARC-D on route (c)). That is close to FlipBase's demonstrated figure on the same SoC, but stricter: its first frame at 2.93 to 2.98 s (mainline U-Boot on its vendor kernel, warm reboots; section 9.2) is read on a counter whose zero is about 0.5 s after power-on, so it is about 3.4 to 3.5 s from power-on. Meeting 3 s from power-on needs a first frame by about 2.5 s on such a counter, from a cold start: FlipBase's mainline warm figures below are near that, and it has published no cold one *(guess)*. On the mainline kernel it has published warm first frames of 2.34 to 2.67 s and a filmed cold boot with the menu about 3.45 s on the counter (3.97 s after the power LED), both before its November fixes, which then took about 0.25 to 0.3 s off its warm hand-off (1.56 → 1.29 to 1.34 s).

**The owner's stretch goal is 1.5 s.** Nobody has published a 1.5 s cold boot to a visible menu on RK3566. FlipBase's 1.29 to 1.34 s is a warm frontend exec on its counter, not a first frame; its cold launcher exec was 2.0 s on the vendor kernel, and its first frame about 3 s on the counter. 1.5 s is realistic for wake from sleep (FlipBase's suspend and resume round trip is about 0.7 s) or for the launcher's start. It is measured in Phases 0 and 6, not promised.

| Stage | Target | How |
|---|---|---|
| Phase 0 | baselines, no number: Anbernic's stock Linux card on the ARC-D; the spike image (FlipBase ported to the ARC) on the ARC-D and on the reference unit; the vendor loader's cost under routes (a) and (b) | instrumentation only |
| Alpha | measured and recorded on every build; no number set | the base as Phase 2 builds it: BusyBox init, EROFS, raw FIT, U-Boot fixes, frontend first, no compositor |
| v1 | **3 s** cold on the reference unit. On an ARC-D on route (a) or (b): 3 s plus the vendor loader's measured cost, stated as such. If v1 misses it, the figure ships stated and Phase 6 closes the gap | the rest of section 9.3 that Phase 2 did not do |
| Stretch | **1.5 s**: measured for wake from sleep, for Nano's start, and for a cold first frame | snapshot frame, U-Boot logo, Falcon mode (Phase 6 and after) |

**Comparing numbers.** FlipBase's counter does not start at power-on: U-Boot's first record reads 45 ms after a bootrom, DDR init, SPL and BL31 put at 0.39 s, and its power LED leads the counter by about 0.5 s. The RG353PS fork counts from near U-Boot's start, so its 3.7 to 3.9 s leaves out the bootrom, TPL and SPL: the two are not directly comparable. Thimsy reports power-on to first frame and states the counter's offset.

**Why the reference unit carries the number.** On an ARC-D on route (a) or (b), Anbernic's loader runs before our U-Boot and can cost time *(guess: 0.5 to 1.5 s)*. It may cost much less: on the Flip the vendor bootrom, DDR init, SPL and BL31 take about 0.39 s together, and FlipBase measured no difference between two vendor SPLs (3.114 against 3.118 s); route (a) replaces only what follows the SPL. It could also cost more, if Anbernic's loader probes the panel, buzzes or waits on keys. Until it is measured (open question 21), the 3 s reference is a Linux-only unit (or, once bootloader writing is allowed, a route-(c) ARC-D). The ARC-D is one device among many: most target devices have no Android on eMMC (the ARC-S, the RG353VS/PS, the H700 family, RK3326).

**Other families.** All figures run from power-on to Nano's first complete home frame, cold, on a Linux-only unit; they are targets, not promises. Only RK3566's 3 s is decided (decision 5); every other figure is a *(guess)* until that family's first measurement replaces it. They are measured with FlipBase's `bootclock` where U-Boot exists, and by kernel timestamps plus filming elsewhere.

| Family | Target | Why |
|---|---|---|
| RK3566 Generic and Specific (reference unit, RG353x, RGB30 and others) | **3.0 s** (decided); 1.5 s stretch, measured | decision 5. ROCKNIX's full config must be trimmed by the fragment first. An ARC-D on route (a) or (b): 3 s plus the vendor loader's cost, stated |
| RK3568 RG DS / DS Plus | 3.5 s *(guess)* | two panels to bring up; dual-screen features are not in the first release |
| H700 (DDR3 and DDR4) | 3.0 s target, 3.5 s accepted floor *(guess)* | mainline SPL with open DRAM init and A53 at 1.5 GHz. RetroStoneOS reached its logo at 2.09 s on a slower A20; BaseOS reached hand-off at 3.16 s on the H700 vendor kernel; serial off saved about 0.7 s there |
| RK3326 image b | 3.5 s *(guess)* | Cortex-A35 at 1.3 GHz, rkbin DDR, mainline U-Boot v2025.10 |
| RK3326 image a | 4 to 4.5 s, stated as is *(guess)* | Hardkernel's vendor U-Boot is not Thimsy's to trim |
| RK3399 (RG552) | 3.0 s *(guess)* | fast CPU, but a 1920x1152 first frame |
| RK3576 (RG Vita Pro) | 3.5 s *(guess)* | a U-Boot git snapshot; the SPI MCU pad comes up in the kernel |
| S922X (ODROID-GO Ultra, RGB10 Max 3 Pro) | 3.5 s *(guess)* | Amlogic's binary FIP (BL2 DDR training, BL30, BL31) before mainline U-Boot |
| RK3588 | 5 s, stated as is *(guess)* | vendor BSP kernel and vendor U-Boot fork; wave 4 |
| Qualcomm, all 8 families | no power-on target; **measured totals stated**. Aim: 1.5 s from kernel start (EFI stub) to first frame *(guess)*; power-on total about 4 to 6 s *(guess)* | XBL, ABL and OpenABL are not Thimsy's code, and no published millisecond figures for them exist. OpenABL keeps XBL's splash on screen (`splash_region`), so the perceived boot is better. The GPU's zap shader is loaded when Nano first opens the render node (`msm_open()` calls `load_gpu()`), after the root is mounted, so GPU firmware can live on the root or a firmware partition and need not be built into the kernel; the display needs none, and DSP firmware is for audio (open question 31) |
| AMD64 | none | QEMU only |

A family's figure goes in its release notes. The regression check is per family: a regression of more than 10% on whichever unit carries that family's number fails the release check (section 11.2).

**The drive check's budget** (open question 18). The first boot after a drive moves between platforms runs one `fsck.exfat` or `fsck.fat` pass: a FAT32 card pulled from Android stays flagged until Linux's check clears it, and an exFAT card from the Cube is checked because its flag can't be trusted (section 7.3). The drive mounts in the background after the first frame (section 9.3), so the pass delays the drive, not the 3 s figure, and the drive's grace waits for it (section 7). Phase 0 measures it on the owner's 16 GB card in both FAT32 and exFAT and on a 64 GB exFAT card; that figure also decides whether exFAT is simply always checked.

### 9.5 The chosen route

The previous version of this section said that if a 3 s boot were a requirement, the base was wrong and decision 1 had to be reopened. The owner did that on 2026-11-25 and chose the Thimsy Base. It reuses Nano's shared code and the card contract with a KMS/GBM display target. What it needs, and where this plan covers it:

- a Linux platform backend with no systemd, no D-Bus at boot and no PipeWire: section 5.2;
- its own launcher in place of `runemu.sh`: sections 5.3 and 6.4;
- its own per-device quirk hooks over imported device facts, its own U-Boot per family, and `thimsy-padd` for input: sections 3.2 and 5.3, and Phase 7;
- a boot chain per device. Linux-only RK3566 units boot the card's own idbloader at sector 64 (rkbin's DDR blob as TPL, then mainline SPL), then mainline U-Boot from the `uboot` partition. The ARC-D uses an SD-only dual-boot route, or waits (section 8.6);
- **a BL31 for RK3566 (decision 18, decided 2026-11-25: "use the FlipBase TF-A").** FlipBase's approach: the unit's own vendor BL31, read from the unit and never redistributed, with FlipBase's pm-config kernel patch so mainline sleep works with it. For a unit with no vendor BL31 to read, rkbin's BL31 (a binary, in section 1's redistributables) is the fallback, to confirm in Phase 0. Not chosen for RK3566: upstream TF-A built from source (ROCKNIX's choice: `ATF_PLATFORM=rk3568`, with `mem` as its default suspend mode; its sleep current is not published). FlipBase keeps the vendor BL31 and OP-TEE on purpose: "the DDR blob is paired with its BL31", the kernel's DMC driver calls BL31 to scale DDR, and a generic 2021 loader blob caused intermittent hangs on the Flip. Where OP-TEE is present, mainline U-Boot must add a `/reserved-memory` node for it (16 MiB at `0x08400000` on the Flip), or the kernel overwrites it and dies. Phase 0 measures deep-sleep current with the vendor BL31 on the reference unit (FlipBase: about 12 mA; open question 24) and confirms the rkbin fallback.

Size: Phase 2 for the base and the first boards, Phase 7 for each further board (section 12).

---

## 10. Build system and build host

Host (checked 2026-11-25; the owner's answer to open question 6): **this machine builds everything**, releases included. Ryzen 9 5900HX (8 cores, 16 threads), **16 GB RAM**, Windows 11, Docker Desktop. Internal NVMe C: has about 255 GB free. **Builds live on the owner's 21.8 TB Sabrent USB drive (D:)**, as the Android build already does: the `thimsy-aosp` WSL2 distro keeps its ext4 disk at `D:\WSL\thimsy-aosp`. `.wslconfig` gives WSL2 `memory=13GB` and `swap=40GB`, shared by Docker Desktop's VM and every distro, so the Android and Linux builds don't run at the same time.

| Build | Needs | On this host |
|---|---|---|
| Thimsy Base: tiers, GPU packs, hardware packs | Buildroot in Docker, FlipBase's pattern: a `debian:trixie-slim` container, the output tree in a Docker named volume, downloads bind-mounted, `FORCE_UNSAFE_CONFIGURE=1` because some packages' `configure` scripts (GNU tar, coreutils) refuse to run as root. FlipBase's clean tree is about 30 GB, most of it host LLVM and Clang for Mesa's Panfrost shader compiler; its from-scratch build takes about 3 h on GitHub's 4-vCPU arm64 runner (2 h 45 min for v2.0.0, images and SDK), and the same tree built natively on GitHub's 4-vCPU x86_64 runner took 4 h 6 min. With RetroArch, 10 to 15 cores, PPSSPP, mupen64plus and ScummVM: about 40 to 70 GB, and a first build of 2 to 4 h on the 5900HX with per-package directories and parallel make *(guess)*; minutes afterwards with ccache. **With the tier and pack split** (decision 25), host LLVM is the catch: Buildroot builds host packages, host LLVM among them, inside each output tree, so separate GPU-pack trees would each rebuild it (the larger part of FlipBase's 30 GB and of its build time) unless it is shared. Proposed: each GPU pack is built in a tier tree that already builds Mesa's host LLVM, and packed as its own EROFS image, so host LLVM is built once per tier tree, not per pack (Panfrost in the v8.0 tree, since v8.0 code runs on the v8.2 cores *(guess: the libraries it links match across tiers)*; Freedreno in the v8.2 tree). The alternative, one prebuilt SDK carrying host LLVM and Clang that separate GPU-pack trees use, needs Mesa's recipe pointed at that LLVM and is to be proved *(guess)*. Hardware packs need no LLVM; they build against the shared SDK as an external toolchain. Times, all *(guesses)*: 2 to 3 tier userlands, 2 to 4 h each the first time with host LLVM and their GPU pack; a GPU pack rebuilt alone in its tier tree, 20 to 40 min; up to 15 hardware packs, 15 to 30 min each with ccache. A full rebuild of everything is roughly **10 to 20 h on this host** *(guess)*; an incremental build is much less: after a ROCKNIX import, only the changed families' hardware packs (15 to 30 min each); after a Nano change, the tier roots *(guess: under an hour)*. One full Buildroot build per family instead would be about 30 to 60 h and 0.6 to 1 TB *(guess)*, which is why the split is proposed | Fits easily on D:; the ROCKNIX plan's 200 GB per family is gone. Build as an **amd64 cross build** (FlipBase: `FLIPBASE_BUILD_PLATFORM=linux/amd64`; the target is aarch64 either way). FlipBase's default, arm64 under QEMU emulation, "would not finish within a job's 6 hours" on x86 |
| Where the tree lives | **A WSL2 distro whose ext4 disk is on D:** (recommended: a new `thimsy-linux` distro at `D:\WSL\thimsy-linux`, so its host packages and its disk stay apart from `thimsy-aosp`; reusing `thimsy-aosp`, 779 GB free, also works). One Buildroot output tree per tier userland (its GPU pack built in it, above) and per family hardware pack (decision 25, proposed), with the download cache (`dl/`) and ccache shared between them on the same disk. The drive is USB and its media type isn't reported; if it proves slow for builds, the active output tree can move to the NVMe (about 40 to 70 GB). ext4 only: a Docker named volume or the WSL2 disk, never NTFS or `/mnt/c`. The kernel tree has file names that differ only by case, GammaOS's tree has names Windows can't store, and the Windows mount is slow. In WSL2, Buildroot refuses a `PATH` with spaces, which WSL's appended Windows entries (`Program Files`) have: set `[interop] appendWindowsPath=false` in `/etc/wsl.conf`, or run with a Linux-only `PATH` | No published report of a Buildroot build failing on current Docker Desktop with the WSL2 backend was found; Phase 0 proves it. Batocera's 2022 "not stable enough" note was about its 80 to 250 GB trees |
| RAM | not stated by FlipBase *(guess: 16 GB+ is comfortable at 16 threads)* | 16 GB, 13 GB of it for WSL2, with 40 GB swap on the NVMe. Enough for one build at a time; host LLVM's link steps are the peak, so cap parallel jobs (`BR2_JLEVEL`, about 8 *(guess)*) and never build two trees or the Android tree alongside. The owner may upgrade to 64 GB later; then `.wslconfig` goes to `memory=48GB` and `swap=16GB` (its own comment says so), and the job cap can rise |
| Disk image growth | the WSL `ext4.vhdx` grows and does not shrink by itself; sparse VHD is experimental and has had data-corruption reports | keep the build tree on its own virtual disk or volume, and delete it to reclaim space |
| Nano alone | CMake, with the Buildroot SDK as cross toolchain and sysroot (FlipBase builds one) | Minutes. Build out of tree; copy to the device over SSH (dropbear) or adb (FlipBase runs `adbd` over a USB gadget). For UI work on the PC, a Wayland or SDL development target behind `NanoDisplayTarget`, used only on the PC, runs in WSLg *(guess: GLES through WSLg is good enough)* |
| Android firmware build | unchanged: about 40 GB, privileged container | unchanged |
| Installer | Rust, Flutter 3.50 | unchanged; macOS writer testing needs a Mac (decision 15). The SD-reader field (open question 16) is answered from public sources; a Mac check in Phase 4 confirms it |

Recurring cost: no rebase queue. Buildroot, Linux and U-Boot move deliberately. With the importer, each imported ROCKNIX tag is mostly mechanical: a person is needed when a hash guard fires or a check fails. ROCKNIX's real bump rate (about 6 kernel version changes a month across families, reverts included; section 3.2) means Thimsy skips most of them; each family's kernel move that carries Thimsy's overlay costs about 1 to 2 weeks *(guess)*, so how often families move sets the budget, not the import itself *(guess: 2 to 5 developer-days a month for imports and checks across all families, plus the moves Thimsy chooses)*.

**CI on this machine.** Per chosen ROCKNIX tag: the hash guards, the series applied to each family's kernel, every `config.xml` DTB built, the release-image check (section 3.2), FlipBase's offline tests, and a QEMU AMD64 boot to Nano's first frame on virtio-gpu *(guess)*. Nightly, `thimsy-import` against ROCKNIX `next` as an early warning, with no artifacts. The release-image check downloads about 24 GB per tag.

Release builds: the same `build.sh`, on this machine, in Docker (open question 6, answered). FlipBase builds its tags on GitHub's `ubuntu-24.04-arm` runner, and ROCKNIX builds all its families nightly on GitHub-hosted runners as chained per-stage jobs (`.github/workflows/build-nightly.yml`, `build-device.yml`). **GitHub-hosted runners stay an optional later CI path, not the default.** Their standard public-repository runners document 4 CPUs, 16 GB RAM and 14 GB of SSD, so a tier userland (40 to 70 GB *(guess)*) needs a chained-job pipeline that passes artifacts between stages, as ROCKNIX's does: real engineering work, budgeted only if this host falls short.

---

## 11. Testing and hardware

### 11.1 Hardware

| Item | Why | Needed for |
|---|---|---|
| **The owner's RG ARC-D**, dual-booting, with Android kept | first device, if one of its SD-only routes passes (section 8.6), else it waits for bootloader writing and the ARC-S is the first device; Android and Linux on the same unit. If it is the only unit, Phase 2's memory check runs it with `mem=1G` on the kernel command line, so the 1 GB floor is tested before v1 | Phase 0 |
| **A Linux-only reference unit: RG ARC-S (recommended) or RG353VS** | the 3 s number with no vendor loader in front of it (section 9.4); the 1 GB floor (ARC-S); a unit a bad card can't brick, since nothing bootable is on eMMC. The ARC-S shares the ARC-D's panel, device-tree include and pad, so it costs one more board folder; the RG353VS has sticks instead | Phase 0 (decision 2) |
| A second RK3566 device with sticks, ideally a 1:1 screen (RGB30), or the RG353VS if the ARC-S is the reference unit | the ARC has no sticks; layouts beyond 4:3 | Phase 7 |
| H700 devices (RG35XX H or Plus): one of each DDR type, ideally two panel revisions | wave 1; panel and DDR selection | Phase 7 |
| An SD multiplexer (SDWire-class, *(guess: about US$40)*) and a switchable USB power line per owned unit, plus USB serial where exposed | hardware-in-the-loop CI: write the card, power-cycle, capture `bootclock` and serial, run the smoke script over SSH (boot timeline, one game per system launched and exited, sleep and wake) | Phase 7, before wave 1 |
| Community testers, opt-in | the families the owner can't hold: they run the Device check (section 11.2) and attach its report to a GitHub issue form by hand | from wave 1 |
| The RG Cube (owned) | the Android side of every interop test | Phase 3 |
| Cards: exFAT 64 GB and 128 GB, FAT32 32 GB, one known counterfeit; two USB readers. The owner has spare microSD cards and one USB reader (sizes to be listed); a second reader and the counterfeit card are still needed | filesystem matrix; writer tests | Phase 0 (spare cards), 3, 4 |
| USB serial: open the ARC-D and wire its serial pads (owner, later; open question 13). Likely tx, rx and gnd pads under the back cover (the RG556 precedent; macromorgan); RK3566's debug UART (`ttyS2`) runs at 1,500,000 baud, 3.3 V, so it needs a CP2102N- or FT232-class adapter, not a CH340 | U-Boot and early kernel timing; the ARC-D's loader under the SD-only routes. Phase 0 starts without it: on-screen U-Boot output, a USB gadget console, and arch-counter timing (section 9.2) | when the owner wires it; not on Phase 0's path |
| A Mac | macOS writer | Phase 4 |
| The owner's AYN Odin 2 Portal and Retroid Pocket Duo | hardware-in-the-loop units for SM8550 and SM8750 in wave 3, which is deferred until bootloader writing is allowed (section 1; the Duo boots ROCKNIX's SM8750 image), with no device-specific work before wave 3; the Odin 2 is not a focus. Both already boot Linux through OpenABL with Android kept (the Odin 2 Portal under ROCKNIX, Batocera and Armada; the Duo under Armada). Both also serve as Android-side test units for Thimsy Home (`android_launcher_plan.md`) | wave 3 (deferred) |

### 11.2 Tests

| Layer | Test |
|---|---|
| Nano shared code | the existing host unit tests (29 production files), under CTest, on Linux and in the Android host build; sanitizers on |
| Platform seam | a fake platform (in memory) so menus, the store and the sync can be driven without a device; golden screenshots at 640x480, 720x720 and 1280x720 |
| Card spec | **conformance vectors**: sample drives (as directory trees and as exFAT and FAT32 images, including a FAT32 one with non-ASCII file names and one whose only definition of a system is in `custom_systems`) written by each platform; each implementation must read the others' and round-trip without changing foreign entries |
| Interop, on devices | the matrix below |
| Image | the base's offline shell tests in CI (FlipBase's `tests/`); a smoke script over SSH on real devices; a QEMU AMD64 boot to Nano's first frame on virtio-gpu *(guess)* |
| Installer | section 8.6 |
| Importer | per chosen ROCKNIX tag: the hash guards; the generated series applies to each family's kernel; every `config.xml` DTB builds; the release-image check (pre-fragment kernel config by `extract-ikconfig`, DTBs through `dtc`); the conformance check on Thimsy-critical options; and the importer regenerates the hand-made RK3566 import of Phase 2a |
| Hardware in the loop | on owned units with an SD multiplexer: card written, power-cycled, `bootclock` and serial captured, the smoke script run over SSH (section 11.1) |
| Device check | a Nano screen generated from `device.json`: an orientation arrow the tester confirms; a touch target in each corner on touch panels; every listed button and stick range, through padd; rumble; audio left and right; one sleep and wake with its time; a Wi-Fi scan, a battery reading and the `bootclock` timeline. It writes `thimsy/reports/<release>-<dtb>.json` to the card with only the release, DTB name, `config.xml` variant, results and timings: no serials or identifiers. The tester attaches it to a GitHub issue form by hand; nothing uploads by itself |
| Product status | the catalog's levels (built, boots, works, certified; section 8.3) are recomputed from reports and hardware-in-the-loop runs at each release. Channels: a nightly per family (CI artifacts), a candidate after each chosen ROCKNIX tag, and stable. Testers are credited in release notes |
| Boot time | the per-boot log, compared against the previous release and, on the reference unit, against the 3 s target; per family, against its section 9.4 figure; a regression of more than 10% fails that family's release check |
| Golden | the RG Cube Android image stays byte-identical through every refactor phase |

Interop matrix (each row on exFAT and FAT32):

| # | Scenario | Pass means |
|---|---|---|
| 1 | Drive made on the Cube (ES-DE and emulator files moved), put in the ARC | library, favourites, collections, covers show; a `.srm` from a system on the same core and a PSP save load; nothing under `app/`, `retroarch/config/` or `custom_systems/` changed |
| 2 | Play on the ARC, return the drive to the Cube | new saves load; play counts and last-played merged; the Cube's own states untouched |
| 3 | Drive made on the ARC, put in a Cube (a) whose sync base names other cards, and (b) with no base and no emulator settings | (a) the foreign-drive dialog; "Use the Card's" adopts it. (b) the silent adoption, the card's copy winning; nothing on the drive lost (section 7.10) |
| 4 | Per-game emulator set to M64Plus FZ on the Cube | on Small, the ARC launches the system's default (the `mupen64plus_next` core), since standalone mupen64plus is off in that profile; on Medium (2 GB), the same family. Either way the label on the drive is unchanged and N64 saves are stated as not shared |
| 5 | A system with no Linux emulator (Xbox) on the drive | shown as unavailable on the ARC; entry kept |
| 6 | Drive pulled during a write, on each platform | no corrupted gamelist; the other platform mounts it after its check |
| 7 | Drive out, then launch each emulator on the ARC | nothing is written under `/storage/<VOLID>`; the game uses the fall-back or refuses |
| 8 | A system whose default core differs between the builds; and a game whose per-game core on the Cube does not exist on the ARC | saves do not overwrite each other (the second case sorts its save, section 6.2) |
| 9 | Drive cloned to a larger card; both inserted | neither is used; a message asks to remove one |
| 10 | A released (pre-spec) Android build visits the drive | nothing Linux wrote is lost (section 7.10) |
| 11 | Mainline ES-DE (Android, and aarch64 Linux) pointed at the drive | ES-DE reads everything it has a field for; no "INVALID ENTRY" for a label Thimsy wrote |
| 12 | One card in the ARC: OS and an exFAT data partition on the boot card; then the same card in the Cube | on the ARC, the data partition is listed and can be adopted; the U-Boot, boot, root and data partitions are never listed. On the Cube, exactly one public volume appears, mounted read-write after `fsck.exfat`, with no "unmountable" notification (section 8.2) |
| 13 | Power pulled a few seconds after a game exits, on each platform | the save and the gamelist are intact; the next mount checks the drive |
| 14 | Launch each emulator on the ARC | nothing but in-game saves appears under `<drive>/ROMs` (no `savestates`, `bios` or other folders) |

---

## 12. Phased roadmap

Sizes are person-weeks of one full-time developer, all *(guesses)*.

**First target: the owner's RG ARC-D, dual-booting, and a Linux-only reference unit (the ARC-S recommended)**, decision 2's recommendation, because:

- The ARC-D is the device the owner named and has.
- Both ARCs' device trees are in mainline Linux, and mainline U-Boot's rgxx3 board detects both. ROCKNIX's ARC patches are few and small (section 3.2).
- FlipBase proves the base on the same SoC.
- The same RK3566 hardware pack reaches the RG353 family, the RG503 and the Powkiddy RGB30 family: each a `device.json`, a card and testing (wave 1).
- Two card slots: OS on one, the shared drive on the other. That is exactly the interop setup.
- Their limits are useful early: a portrait-native panel that exercises the rotation contract, a six-button pad with no sticks split over three source nodes, and 1 GB on the ARC-S.
- A bad Thimsy card can't brick the reference unit: with nothing bootable on eMMC, the boot ROM falls through to the SD card. On the ARC-D, none of the SD-only routes writes the eMMC (section 1).

Against it: the ARC-D's dual boot is unproven (section 8.6), and its vendor loader may sit in front of the 3 s number. If no SD-only route passes, the ARC-D waits for bootloader writing (later) and the ARC-S becomes the first device; erasing Android is not offered.

| Phase | Work | Done when | Weeks |
|---|---|---|---|
| **0. Spikes** (no product code) | (a) **Dual boot on the ARC-D, nothing written to the eMMC:** read its eMMC loader read-only (sector 64 of `mmcblk0`, over adb as root), then routes (d), (a) and (b) in that order, each against section 8.6's pass rule (boots from slot 1; Android untouched with the card out; deep sleep, with FlipBase's pm-config patch ported first; the loader's time measured; open questions 20 and 21). (b) **FlipBase on the ARC:** build FlipBase unmodified, as an amd64 cross build in Docker on this host; then a quick port to the ARC on a card (the mainline ARC device trees, mainline rgxx3 U-Boot with FlipBase's U-Boot patches, ROCKNIX's 0014, 0025 and 0029): the panel the right way up, pad, touch, the Wi-Fi chip identified, audio routing (open question 25's checklist), deep-sleep current with the unit's vendor BL31 (decision 18; open question 24) and the rkbin fallback confirmed, and the boot timeline with FlipBase's instrumentation, on the ARC-D and on the reference unit. No serial console: on-screen U-Boot output, a USB gadget console and arch-counter timing (open question 13). (c) **Display, input and emulators:** the DRM hand-off with RetroArch and PPSSPP on Panfrost; **the GPU A/B** (decision 11; open question 12): Panfrost against libmali on the ARC-D across the v1 emulator set, stability first (crashes, GPU hangs, rendering errors, sleep and resume), then performance and first-frame time; the rotation contract on the 480x640 panel (Nano, a first cut of the SDL3 and RetroArch patches, Cluster0 against the blit, touch) with the blit's cost measured; a spot check of the v1 emulator set; a `thimsy-padd` prototype on the ARC's three nodes: hiding by unlinking against the libudev-zero patch, intercept mode with a held button, an external pad, and `rocknix-joypad`-style button latency. (d) Build Nano outside AOSP from `tools/android-launcher/fetch_nano.sh` (shared with the launcher plan's Phase 0). (e) Mount a Cube-made card at `/storage/<VOLID>` on the spike image and load a save. (f) Windows raw-write spike: the broker, lock, write, verify. (g) **Cube card tests** (open questions 8 and 18). The one-card layout: on the Cube, insert card A typed as FlipBase types its card and card B typed per section 8.2. For each, record `perdev_minors`, how many public volumes appear and their states, the id each gets, any "Issue with SD card" notification (never tap Format), the `/storage` directories left behind, which volume Nano treats as the microSD card, and that `fsck.exfat` runs on the fifth partition and it mounts read-write; with card B, also boot with a USB drive inserted to see whether the card's id moves off `...0001`. The dirty flags, as root over adb: read exFAT's VolumeFlags (offset 106) and FAT32's state byte (offset 65) **with `O_DIRECT`** (`dd iflag=direct`), or only after the card is pulled, since a buffered read of the mounted device shows the page cache; for the exFAT gap, let a large `dd` write **finish**, then `touch` another file, then pull at several delays (about 1, 5 and 15 s) and run `fsck.exfat -n` on a Linux PC; check the FAT32 bit is set while mounted and clear after Eject; on the Thimsy Base's RK3566 kernel (each family's ROCKNIX version, decision 22; all newer than 6.16, which carries the exFAT fix), check VolumeDirty is clear before the first write and set after it until umount; confirm `fsck.exfat` and `fsck_msdos` run on each insert (logcat). Time `fsck.exfat` and `fsck.fat` on the owner's 16 GB card (FAT32 and exFAT) and on a 64 GB exFAT card (section 9.4) | each guess in sections 3.2, 5.2, 5.3, 7.3, 8.5, 8.6 and 9 is confirmed or replaced; an SD-only route is chosen for the ARC-D, or the ARC-S is named the first device; the GPU A/B has a winner; decisions 4, 11 and 23 can be made, and decision 18's rkbin fallback is confirmed | 4 to 7 |
| **1. The seam** (Android behaviour unchanged) | `NanoPlatform.h`, `compat/` shims, CMake, the config store interface. This is the launcher plan's Phase 1; do it once | the Cube image is byte-identical; Nano's shared code builds and passes its unit tests on a Linux host | 6 to 10 |
| **2. Thimsy Base and the Linux alpha on the ARC** (wave 0) | **2a, the base:** `thimsy-base` forked from FlipBase and made multi-board (the per-family common layer, `build.sh` and the tools free of board names, the tier, GPU-pack and hardware-pack layout of decision 25 with one of each: v8.2, Panfrost, RK3566), with the BaseOS-derived files handled as open question 26 says (most replaced by Thimsy's own parts, the four C files used as-is); RK3566's kernel version chosen from the ROCKNIX tags that exist then (decision 22; open question 30); **a hand import of RK3566** in the layout the importer will later generate (ROCKNIX's series flattened with its file names and a provenance line per patch, its config with Thimsy's fragment, the `device.json` facts), with FlipBase's generic patches and U-Boot series as Thimsy's overlay; the ARC-D's board (if Phase 0 found an SD-only route) and the reference unit's; BusyBox init; `thimsy-session` (mounting, hotplug, power key, sleep, the launch hand-off); the A/B update; per-device packaging; recipes for RetroArch and the alpha cores; `make legal-info` in the release build; the serial console off in release builds and the U-Boot options of section 9.3. **2b, Nano:** `platform/linux` (the KMS/GBM display with the rotation contract, since the ARC is portrait: Nano's rotation, the SDL3 KMSDRM patch offered upstream through SDL issue #14905, and the RetroArch `drm_ctx.c` patch; `thimsy-padd` and the ARC pad model; the config store, fork/exec with the DRM hand-off, system actions: `wpa_ctrl`, BlueZ on demand, ALSA, time); `thimsy-fonts`; XMB and the ES-DE theme; store mode on internal storage; boot instrumentation | **An alpha as a plain `.img.zst` per device**, written with any imaging tool: boots to Nano, launches and exits a game on every alpha system, sleeps and wakes; boot time recorded; resident memory measured with 1 GB (an ARC-S, or the ARC-D with `mem=1G`). **For the alpha to be public** (decision 16), also: the `legal-info` sources, the Buildroot tarball and the external tree published beside it; credit in place for FlipBase and ROCKNIX, each imported patch with its provenance line; and for the ARC-D only on an SD-only route Phase 0 proved. Otherwise it stays private until Phase 4 | 12 to 19 (2a: 6 to 10; 2b: 6 to 9) |
| **3. Card interop v1** | `LIBRARY-FORMAT.md` (with the launcher plan); the drive model; `/storage/<VOLID>` mounting with the immutable mount point; Linux adoption, links and re-pointing; the per-system core table; emulator definitions schema 2 with Linux blocks, families and trust rules; `systems.linux.json`; `card.json`; the Android-side changes (both label tables, the label-writing rule, not overwriting unknown launch tuples, the dialog wording); decision 20's choice for the Cube's volume ids; **the `nano/emulators.json` device check** (open question 9): on a Cube on test 34 or later, with the ES-DE store on its card, write a file with an unknown top-level key and entries carrying `key` and nested unknown fields (one with no `am`), and an entry for the same ROM spelled with different case or a `./` prefix before Android's; pick an unlabelled emulator for a third game, then re-pick one for the first; confirm the unknown fields survive except on the re-picked game, that a labelled pick writes `<altemulator>` and no JSON entry, which entry launches, and so whether `esdeFindGame` ignores case | interop matrix rows 1 to 14 pass between the Cube and the ARC, except row 1's PSP save, which waits for PPSSPP in the v1 row | 6 to 10 |
| **4. Installer** | the `sdcard` crate (the SD-card writer, section 8.5), `pack.json` format 2 and catalog changes (with the multi-device plan), the card picker, the ARC-D's dual-boot card layout (no step writes the eMMC), **the storage job** (section 8.4: format and lay out, pack-ins, ROMs and BIOS from the PC with BIOS hash checks, non-destructive update), release packaging with bmap and the `legal-info` sources; **a Mac check of the disk rules** (open question 16): on an Apple silicon MacBook Pro and, if possible, an Intel one from 2012 to 2015, with a card in the built-in slot, in a USB reader, and an `hdiutil attach -nomount` image, record `BusProtocol`, `Internal`, `RemovableMedia`, `VirtualOrPhysical` and the media icon; confirm Apple silicon's `RemovableMedia` and `SD.icns` values, whether `diskutil list physical` already leaves images out, and, if one is available, a Mac Studio slot's `BusProtocol`; then check the engine offers both cards and never `disk0` or the image | a user on Windows, macOS and Linux goes from "pick RG ARC-S" or "pick RG ARC-D" to a booted device, with Android still booting on the ARC-D; the wrong-drive and fake-card tests pass; the storage job writes, fills and updates a game card that an Android Thimsy device and a Linux Thimsy device both read | 6 to 10 |
| **Linux v1 release** | hotkeys for RetroArch (its own menu by hotkey) and "Close Current App"; scraper, BIOS manager, File Explorer, setup wizard, update file; recipes and launch templates for PPSSPP and ScummVM; the ARC's HDMI in Docked mode (Nano and games on the TV, panel off; open question 14); 21 languages checked on 640x480; the boot work left for 3 s on the reference unit | first public release for the RK3566 boards that were tested; the full interop matrix, row 1's PSP save included, passes again; boot time at 3 s on the reference unit, or the measured figure stated (section 9.4) | 4 to 6 |
| **5. Over a game** | the in-process libretro host (`LibretroRunner` ported, run as `nano --run`) with Nano's Quick Menu over libretro games; a DRM-lease spike for SDL standalones (section 5.3); button map overlay; hotkey provisioning for standalone emulators | the Quick Menu opens over every libretro system and the game sees no input meanwhile; standalones keep their own menus unless the lease spike succeeds | 6 to 10 |
| **6. Boot time** | the rest of section 9.3; the 1.5 s stretch goal measured (wake from sleep, Nano's start, cold) | 3 s holds on the reference unit; the stretch figures are recorded | 2 to 4 |
| **7. Coverage waves 1 to 4** | **7a, `thimsy-import`:** the generator, hash guards, the release-image check, the conformance check, provenance lines, the catalog with status levels, the Device check screen and report, and the hardware-in-the-loop rig; done when it regenerates Phase 2a's hand import of RK3566 and imports H700 *(guess: 4 to 8 weeks; ROCKNIX's patch-directory rules, `post_patch` injections and six `CONFIG_EXTRA_FIRMWARE` sites make it more than a copy)*. **7b, the waves** (section 2.4): wave 1, the rest of RK3566 and H700 (DDR3 and DDR4 U-Boot, panel variants); wave 2, RK3326 image b, RK3399, RK3576, S922X; wave 4, RK3326 image a and RK3588. Wave 3, the Qualcomm families with SM6450 through OpenABL, with each family's display setting measured (section 5.3) and the firmware fetch (decision 24), is deferred until writing a bootloader to internal storage with a verified backup is allowed (section 1). Per family: the imperative port and U-Boot or OpenABL bring-up, about 1 to 2 weeks, plus testing *(guess)*. **Per device**, not per family: the `rcS` quirk hooks (fans, governors, sleep), the `device.json` pad section checked on the unit, and rotation and touch validation *(guess: days to a week each, much of it on testers' units)* | each product reaches "boots" or better in the catalog; each certified product passes the smoke script, the Device check and a sleep-current check | 7a: 4 to 8; 7b: 20 to 40 for the families *(guess)*, plus per-device work |
| **8. Later** | bootloader writing with a verified backup, once allowed: ARC-D route (c) and wave 3 (OpenABL); shared save states for cores that pass open question 11's test; HDMI mirroring; heavy emulators on Qualcomm (Turnip); photos, music, video on FFmpeg; Quick Resume; optional real ES-DE; network shares and Syncthing menus; dual-screen layouts; our own compositor only if section 5.3's stages fall short | - | - |

**To the alpha: 22 to 36 weeks. To v1: 38 to 62 weeks**, for a developer who also ships the Android build and has five other plans open. The alpha does not need the installer or the card contract, so phases 3 and 4 are not on its path; they can run in either order or overlap. Against the ROCKNIX plan (16 to 26 and 31 to 51), Phase 0 grows by the dual-boot and port spikes, Phase 2 by the base itself, and v1 by its own emulator recipes; the monthly rebase is gone. For scale: FlipBase went from its fork (2026-08-23) to mainline Linux and U-Boot on one board in about three months of one maintainer's commits, building on Zetarancio's reverse engineering of the Flip. The `thimsy-padd` prototype and the two rotation patches are counted inside Phases 0 and 2b's ranges *(guess)*.

**The whole program: about 60 to 100+ person-weeks** *(guess)*: v1 on RK3566 (38 to 62), then the importer (4 to 8), the 14 further families (20 to 40; the 8 Qualcomm families among them wait for bootloader writing), and per-device work that is not yet sized and grows with the number of products. Nothing here is a promise of ROCKNIX's full list: each product's status says where it stands.

The weeks are full-time effort, not calendar time. Against section 4.5, the estimate includes the seam (Phase 1) and minimal versions of `LIBRARY-FORMAT.md`, the drive model, the emulator definition's Linux keys, format 2 and the SD-card writer (Phases 3 and 4). It does not include the device descriptors (`multi_device_plan.md` Phase 1), the layer resolver and the platform selector (`device_profiles_plan.md` Phases 0 and 1, and section 6.3), or `face_layout` (`controller_layout_plan.md` Phase 1). Phases 3 and 4 assume those are done; if they are not, each adds its own plan's size, which this plan does not estimate.

Dependencies outside this plan: section 4.5.

---

## 13. Risks

1. **The Nano port is large, and it pulls against upstream.** 204k lines hang off one class; 110 files touch Android. Every seam cut is a diff against GammaOS's Nano, which Thimsy still rebases onto (`BACKPORTS.md`). *Mitigation:* shims before interfaces; land the seam once for the launcher and Linux plans; keep the Cube byte-identical; consider proposing the seam upstream.
2. **The display and the Quick Menu have no proven design on Linux.** With no compositor there is no layer for a Quick Menu over a game; the rotation contract rests on two carried patches (SDL3, RetroArch) and is not confirmed for Vulkan on the Qualcomm families; SDL's HIDAPI driver may read an external pad behind an evdev grab. *Mitigation:* the Phase 0 spike; the staged design of section 5.3; the SDL3 patch sent upstream; a per-family display setting on Qualcomm, measured in wave 3; v1 ships with RetroArch's own menu and without Nano's Quick Menu over a game.
3. **The device layer is ROCKNIX's; the rest is Thimsy's.** Thimsy owns the userland and the boot chain; ROCKNIX remains the source of truth for kernel patches, device trees and device facts. Every quirk hook, pad section and per-device test is still Thimsy's work, and FlipBase is a template, not an upstream. *Mitigation:* `thimsy-import` rather than hand-picking; send fixes upstream; a per-product status rather than a supported-SoC claim; few boards until v1.
4. **Card interop can lose data.** Two platforms write the same folders on a filesystem with no journal; released Android builds overwrite what they don't know; an empty mount point looks like a drive; cards get pulled. *Mitigation:* v1 shares little; Linux-only values live in files old builds ignore or leave unchanged (section 7.10); `dirsync` mounts; the immutable mount point and the launch check; write-then-rename with fsync; the on-device matrix.
5. **Raw disk writing can destroy the wrong drive.** Three operating systems, three elevation models, internal SD readers that look like system disks, external SSDs that look like cards, counterfeit cards. *Mitigation:* the engine's classification and system-disk rules, fingerprint re-checks in the privileged process, the capacity probe, first-megabyte-last ordering, read-back verification, and tests that prove system and virtual disks are refused.
6. **The ARC-D's dual boot is unproven.** ROCKNIX and Batocera require an erase, which Thimsy does not offer (the SD-only principle, section 1). The known blocker is sleep under Anbernic's loader ("not compatible with mainline sleep"). *Mitigation:* three SD-only routes in order, none of which writes the eMMC; FlipBase's pm-config patch ported first, for sleep; the Linux-only reference unit carries the schedule meanwhile, and becomes the first device if no route passes. Route (c), which writes the eMMC and, per ROCKNIX PR #154, may break Android's own sleep, waits for bootloader writing with a verified backup (later), with maskrom as its recovery (section 8.6).
7. **The 3 s target depends on the unit and on the boot work landing.** The ARC-D's vendor loader under routes (a) and (b) adds time that is not yet measured, and nobody has published a 1.5 s cold boot to a visible menu on RK3566. *Mitigation:* a Linux-only reference unit for the number; measurement from Phase 0 with FlipBase's method; the stretch goal measured, not promised (section 9.4).
8. **One developer, six plans** that share foundations. *Mitigation:* section 4.5 names an owner for each shared piece; decision 7 picks the order.
9. **Panfrost against libmali.** Panfrost is GLES 3.1, not 3.2. PanVK refuses Bifrost without an override, so there is no dependable Vulkan on RK3566 or H700. Of the two, only the G52 has a conformance submission (GLES 3.1, 2021); the G31 has none. *Mitigation:* GLES by default and Vulkan off; both drivers buildable and switchable (FlipBase's `gpu=` switch) until Phase 0's stability-first A/B picks the one v1 ships (decision 11).
10. **Hardware variety.** H700 panel revisions and DDR types that can't be detected; RGB30 v1 and v2; RK3326 EE clones; SM8250's Visionox panel. *Mitigation:* per-variant cards chosen by installer questions; recovery guides; a per-product status rather than a supported-SoC claim (risk 16).
11. **Licence loose ends in Nano itself** (GPL headers on two files, liba52). They matter more once Nano ships outside GammaOS. *Mitigation:* extend the audit in `plan_remove_unlicensed_files.md`, which does not yet mention `NanoAc3`, `liba52` or `NanoOskKanji` (derived from SKK-JISYO); the launcher plan's Phase 0 item 7 (the terms of the `gammaos-nano` directory) covers them only in part. The Linux build can drop AC-3 in v1.
12. **Licence mix in the base.** An MIT tree sits beside GPL patches and recipes. Batocera's LGPL-3.0+ recipes and Recalbox's non-free ones must not leak in; FlipBase carries BaseOS files that have no licence; about 160 ROCKNIX patches have no author header. *Mitigation:* the `gpl/` area; recipes Thimsy writes itself; the owner's direction on the BaseOS files (most replaced by Thimsy's own parts, the four C files used as-is and rewritten when convenient; open question 26); ROCKNIX's patches used under its licence with a provenance line each (open question 22); `make legal-info` checked by hand at each release.
13. **Android's format wizard can wipe a Thimsy boot card.** If a one-card Thimsy card is typed as FlipBase types its card (entries 1 to 4 Linux filesystem), Android's vold makes every partition a public volume. Its blkid can't identify the raw U-Boot partition, the boot FIT or the EROFS root, so each of those three fails to mount and posts its own "unmountable" notification; tapping one opens the format wizard, whose portable format repartitions the whole card (`StorageNotification.java:472-489`, `StorageManagerService.java:2558`). The ext4 data partition gets `e2fsck -y` and shows as a second drive. The failed volumes also take ids (pushing the real drive probably to `...0005`) and leave empty `/storage/00000000-...-00000000000N` directories that may confuse Nano's `/storage` scan. *Mitigation:* the type GUIDs of section 8.2, checked by the image build and the writer; the Phase 0 device test with both typings; interop row 12 on the Cube.
14. **The Cube's volume ids are not the serial, and move.** Every absolute path the Cube has written to a card names `/storage/00000000-0000-0000-0000-00000000000N`, an index in mount order, not the serial Linux mounts at; the index can change with a USB drive or extra volumes, which breaks Android's own saved paths too, and `emusync::isVolumeId` rejects the form. Until decision 20 is made and built, configs the Cube wrote with absolute paths do not resolve on Linux. *Mitigation:* tokens and relative paths for everything new (principle 2); decision 20; the Phase 0 and Phase 3 checks of which id the Cube gives a card.
15. **Importer drift and ROCKNIX's cadence.** ROCKNIX's imperative blocks change often (`packages/linux` takes about 13 commits a month), its kernels bump about 6 times a month across families with reverts among them, several families sit on unpatched .0 kernels, and its monthly tags have stopped for a year before. The importer has no precedent: no project imports ROCKNIX mechanically. *Mitigation:* per-family case-text hash guards; the release-image check; Thimsy chooses tags and skips freely; frozen families when tags stop, with a soaked `next` commit only for a needed fix (decision 22; open questions 33 and 34).
16. **Tester capacity and wrong-variant cards.** The owner can't hold 83 products; undetectable panels and DDR types can show garbage or not boot. *Mitigation:* the status levels with "untested device" confirmation; the Device check and opt-in reports; typed variant questions and a wrong-panel recovery documented per family (section 8.4).
17. **Firmware provenance.** ROCKNIX's `extra-firmware` is proprietary with no licence file, and some blobs come from stock Android images. *Mitigation:* decision 24: ship only known-licence `linux-firmware`, fetch the rest from the user's own device, leave out what can't be reached.

---

## 14. Decisions for the owner

Numbers are stable: decided, reopened and moot items are marked in place, and new ones are appended.

1. **Base. Decided 2026-11-25:** a Thimsy Base in the FlipBase mould instead of a ROCKNIX downstream. Buildroot (an external tree), mainline kernel and U-Boot, BusyBox init with no systemd, no initramfs, an EROFS or squashfs root, one frontend (Nano). One build per SoC family, then a cheap per-device packaging step (that board's U-Boot, DTB, kernel config fragment and quirks overlay); as proposed by decision 25, that build is now a userland per CPU tier, a GPU pack per driver and a hardware pack per family, put together per device; revisions that can't be detected, such as H700 panels and DDR3 against DDR4, are per-variant images or an installer question. Device and driver fixes are ordinary kernel, U-Boot and device-tree patches carried per SoC family and sent upstream where possible. ROCKNIX's GPL-2.0 device patches (such as its 36 RK3566 kernel patches on `next`, including 0014, 0025 and 0029 for the ARC) are imported, not rewritten. ROCKNIX stops being the base. **Amended 2026-11-25 by decision 21:** ROCKNIX's hardware layer is a tracked upstream through `thimsy-import`; its runtime is not used. The per-device card stands; how it is built is decision 25, and who puts it together is decision 13.
2. **First device. Reopened 2026-11-25** (its premise was ROCKNIX's erase). The owner has an ARC-D only, and wants Android kept (open question 2). Options: (a) the ARC-D alone, dual-booting, with `mem=1G` standing in for the 1 GB floor; (b) the ARC-D dual-booting plus a Linux-only reference unit, the ARC-S or the RG353VS; (c) erase the ARC-D's Android. Recommended: **(b) with the ARC-S**. It shares the ARC-D's panel, device-tree include and pad, it is the 1 GB floor, and with no vendor loader in front it carries the 3 s number (section 9.4). **The SD-only principle (owner, 2026-11-25)** settles option (c): "Putting Linux on an SD card should always be reversible and non-destructive." Now, the Linux install never writes internal storage, and removing the card gives back the untouched device; erasing Android is dropped permanently, so (c) is out. Writing a bootloader to internal storage with a verified backup is deferred to later, not excluded (ARC-D route (c); OpenABL). The ARC-D's routes are (d), (a) and (b) only; if none passes, the ARC-D waits for bootloader writing and an ARC-S becomes the first device. The RG353P/M/V follow the ARC-D.
3. **Scope against the Android plan. Decided 2026-11-25:** no. The Android build is maintained separately from the Linux build, and `multi_device_plan.md` keeps its tier-5 RK3566 work. The ARC-D will eventually offer both: Android Thimsy and Linux Thimsy as options on the same unit, which the dual-boot routes of section 8.6 make possible (Android on the eMMC, Linux on the card).
4. **Display stack. Reopened 2026-11-25:** Sway went with ROCKNIX. Options (section 5.3's table lists them as A to E): Nano as a KMS/GBM DRM client that hands the display to emulators (A, extended by B and C there); a minimal compositor of our own, for the Quick Menu (D); or both, in stages. Recommended: **stages with no compositor first** (section 5.3). KMS/GBM with the drop-master and set-master hand-off, and RetroArch's own menu by hotkey, for the alpha and v1; an in-process libretro host for Nano's Quick Menu over libretro games in Phase 5; a DRM-lease spike for SDL standalones; our own compositor only if that spike fails and a Quick Menu over standalones becomes a requirement. Any compositor in front of the home screen works against decision 5.

    **Amendment proposed 2026-11-25** (coverage design): confirm, **for the Mali families** (RK3566/RK3568, H700, RK3326, RK3399, RK3576, S922X, RK3588), no compositor in front of the home screen and the rotation contract of section 5.3: Nano's rotation, one SDL3 KMSDRM patch sent upstream through SDL issue #14905, one RetroArch `drm_ctx.c` patch, plane rotation where a plane advertises it, a GPU blit otherwise, and touch rotated from the same value; plus a per-game `display: kms | kiosk` fallback for Dolphin, Qt GL frontends, Vulkan-only paths and dual-screen layouts. **Not confirmed for the Qualcomm families:** Mesa's display WSI does only identity transforms for Vulkan, so each gets a per-family display setting, a compositor by default for its games and gamescope re-evaluated for Turnip, measured in wave 3 (open question 35). Nano's home has no compositor on any family. Recommended: yes.
5. **Boot target. Decided 2026-11-25:** 3 s cold boot, power-on to Nano's first complete home frame: about FlipBase's figure on the same SoC, but FlipBase's is on a counter that starts about 0.5 s after power-on, so 3 s from power-on asks a cold boot to match FlipBase's best warm figures on mainline; it has published no cold first frame on mainline. The owner's stretch goal is 1.5 s, with the honest note that nobody has published a 1.5 s cold boot to a visible menu on RK3566 (FlipBase, on its counter: launcher exec 2.0 s cold on its vendor kernel and 1.3 s warm on mainline; first frame 2.9 to 3.0 s on warm reboots of the vendor kernel). 1.5 s is realistic for wake from sleep or for the launcher's start, and is measured in Phases 0 and 6, not promised. The 3 s reference is a Linux-only unit (or, once bootloader writing is allowed, a route-(c) ARC-D) (section 9.4).
6. **Repositories and licences. Reopened 2026-11-25.** Options: (a) one repository, `thimsy-base`: an MIT build tree like FlipBase, with a separate `gpl/` directory for kernel, U-Boot and device-tree patches and any recipe ported from ROCKNIX; (b) the same split across two repositories, MIT and GPL; (c) everything GPL-2.0, as the ROCKNIX plan had it. The owner's workflow (section 4.3) puts the work in a fork of `thimsy-os-nano`; this decision still settles whether the Buildroot tree lives inside that fork or in its own repository. Recommended: **(a)**. The MIT tree keeps FlipBase's files under their own licence and stays reusable; the `gpl/` line is a directory line that `make legal-info` and the release's source publication follow; GPL packages are published with their source at every release (section 3.3). Nano and the shared lists stay Apache-2.0 in this repository. Batocera's and Recalbox's files are never copied.
7. **The shared foundations.** In which order do the six plans go (section 4.5)? Does this plan's Phase 4 build the SD-card writer ahead of `multi_device_plan.md` Phase 5 (recommended: whichever comes first builds it once, in the reserved `sdcard` crate)? And later, does the extracted Nano tree become the source of truth instead of the patch?
8. **The card contract.** Accept `/storage/<VOLID>` (the filesystem serial) as the canonical path on Linux, with the Cube's paths settled by decision 20; the small v1 sharing set (library, BIOS, same-core saves; no shared states or settings); `linux/` folders, `systems.linux.json` and `card.json`; exFAT recommended?
9. **Real ES-DE on Linux. Reopened 2026-11-25** (its premise was ROCKNIX's EmulationStation packaging and Sway). On the Thimsy Base, ES-DE would be one more recipe and one more KMS client that hands the display over (`DEINIT_ON_LAUNCH=on`). Options: ship a self-built ES-DE as an optional second frontend, or Nano's ES-DE theme only. Recommended: Nano's ES-DE theme only for v1. If shipped later, it gets no linked gamelists until the label rule is the compatibility floor (section 6.1).
10. **Feature cut.** Accept an alpha with RetroArch only and internal storage, and a v1 without Nano's Quick Menu over a game, Quick Resume, the DS player and the media apps (section 5.4)?
11. **GPU driver. Reopened 2026-11-25; amended 2026-11-25 (below):** in a base Thimsy owns, nothing defaults to libmali. Options: Panfrost only; libmali only; both, with FlipBase's `gpu=` switch. Facts: Panfrost gives GLES 3.1 on the G52 (conformant since 2021) and on the G31; PanVK refuses Bifrost unless `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` is set; on the Flip, Panfrost starts the frontend about 0.25 s sooner; libmali is an Arm EULA blob on an out-of-tree `mali_kbase`. Recommended: **Panfrost with GLES by default, Vulkan off on RK3566 and H700, libmali left out** unless Phase 0 shows an emulator regression; FlipBase's switch is the proven fallback. **Extension proposed 2026-11-25:** Mesa only on every family: Panfrost and Panthor on the Mali families, Freedreno and Turnip on Qualcomm, no libmali. That removes one GPU stack from the test matrix on the 5 families where ROCKNIX ships both (RK3326, RK3566, RK3576, RK3588, S922X) and makes the GPU packs of decision 25 two. Recommended: yes, unless Phase 0 shows an emulator regression. **Amended 2026-11-25 (owner, open question 12):** test both drivers in Phase 0 and ship the most stable one first. v1 ships whichever driver the Phase 0 A/B on the ARC-D finds more stable (crashes, GPU hangs, rendering errors, sleep and resume, across the v1 emulator set), with performance and first-frame time second. Both stay buildable and switchable (FlipBase-style `gpu=` switch) until then. If libmali wins, its EULA, the out-of-tree `mali_kbase` and per-family packaging apply (section 3.3), and the Panfrost-only extension to all Mali families is reconsidered.
12. **Package profiles on Linux. Reopened 2026-11-25, in part:** the images are built by Thimsy's own recipes, so what Small and Medium contain on Linux is Thimsy's to choose, with no ROCKNIX emulator set behind it, and per-device packaging would allow one image per profile. Recommended, unchanged: RK3566 and H700 as Small, Medium offered on 2 GB devices, Large and Ultra not offered (their emulators are not in these images), a departure from `device_profiles_plan.md`'s "every profile on every device" (section 8.4); one superset image per device for v1, with the profile chosen on first boot or written by the installer; the platform selector that plan would need (section 6.3). Should Small on Linux gain Flycast (a change to `device_profiles_plan.md`)?
13. **Image distribution. Reopened 2026-11-25:** the alternative, the installer rebuilding from ROCKNIX's release on the user's machine, is moot, since there is no upstream release. Options left: Thimsy publishes complete per-device images with their `legal-info` sources; or, later, the installer assembles a card from published parts (section 8.2's option C). Recommended: **complete per-device images** for v1 (section 8.2) *(superseded if the installer-assembly amendment below is accepted)*. **Reopened again 2026-11-25, proposed:** with about 83 products, about 66 GB of complete images per release *(guess)* against about 3 to 4 GB of tier roots and packs *(guess)*, the installer assembles each card from verified parts (tier root, GPU pack, hardware pack, `device.json`; section 8.2's option C), and plain `.img.zst` files are published only for certified devices. The card that results is still per device (decision 1). Recommended: installer assembly from v1.
14. **H700 panel choice. Reopened 2026-11-25:** per-device packaging is cheap. Options: (a) a variant image per panel and DDR type, chosen in the installer; (b) one image per DDR type, with the installer copying the panel's DTB; (c) a first-boot chooser that cycles device trees until the user confirms a picture. Recommended: **(a)**, since it needs no FAT edit and no first-boot logic; (c) later, for users who don't know their revision.
15. **Hardware.** Which of the devices in section 11.1 to buy (decision 2 recommends an ARC-S), and is a Mac available for the writer?
16. **Public or private alpha. Reopened 2026-11-25:** the condition "ask ROCKNIX's maintainers first" becomes **credit for imported patches**: FlipBase's notice kept, ROCKNIX's patches used under ROCKNIX's licence with a provenance line each (open question 22, answered: no author recovery), and a "based on FlipBase, with device patches from ROCKNIX; not affiliated" line. Recommended: publish the Phase 2 alpha with its `legal-info` sources and the external tree, credits in place, for the ARC-D only on an SD-only route Phase 0 proved; or keep it private until Phase 4.
17. **Rules added on the second review.** Accept these, each the recommended choice: a cloned volume id makes neither drive usable (as `usb_storage_plan.md`); `dirsync` on both filesystems; fsck on vfat only when a dirty flag is set, and on exFAT when flagged or not last cleanly unmounted by this device, or always if Phase 0 finds it cheap (section 7.3, after open question 18; vold checks every time); a per-game emulator that is in the image but off in the device's profile falls back to the system default; emulator definitions are imported from internal storage and from the ES-DE Drive, always through the import screen (section 7.2)? **Moot since 2026-11-25:** the clause "`/storage/roms` stays a bind of the drive's `ROMs`, with every other ROCKNIX path redirected", since the Thimsy Base has no `/storage/roms` (section 7.3).
18. **BL31 for RK3566** (new, 2026-11-25; **decided for RK3566 2026-11-25**, below). Options: rkbin's BL31 binary (redistributed; what the RG353PS fork uses); upstream TF-A built from source (ROCKNIX's RK3566 choice; no published sleep current); the unit's own vendor BL31, as FlipBase keeps it (its sleep figure depends on it; taken from the unit, never redistributed). Recommended: decide on Phase 0's deep-sleep current and stability. Upstream TF-A if it reaches deep sleep near FlipBase's 12 mA without hangs; otherwise the vendor-BL31 path FlipBase proved where the unit has one (the ARC-D), and rkbin's where it does not (section 9.5). **Rewrite proposed 2026-11-25:** the BL31 per family starts from ROCKNIX's choice (upstream TF-A on RK3566, RK3576, RK3399 and H700; rkbin's binary on RK3326 and RK3588; Amlogic's FIP on S922X), inside Thimsy's own boot chain; only RK3566 is decided on Phase 0's sleep data, as above. Recommended: yes. **Decided for RK3566 2026-11-25** (owner: "use the FlipBase TF-A"): FlipBase's approach, the unit's own vendor BL31 with FlipBase's pm-config kernel patch so mainline sleep works with it, read from the device and never redistributed. On the ARC-D under SD-only route (a), the eMMC loader already runs Anbernic's BL31. For an RK3566 unit with no vendor BL31 to read, the fallback is rkbin's BL31 (to confirm in Phase 0). The other families keep the proposed "start from ROCKNIX's choice".
19. **How to take FlipBase** (new, 2026-11-25). Options: fork it with its history at a pinned commit; or start a fresh tree and copy its MIT files with the notice. Recommended: **fork at a pinned commit**, which keeps the history and the credit, and take later FlipBase fixes by hand. The BaseOS-derived files follow the owner's direction (open question 26): most are replaced by Thimsy's own parts, and the four small C files are used as-is and rewritten when convenient.
20. **Card identity on the Cube** (new, 2026-11-25). The Thimsy Android build's vold (GammaOS Next's) names every public volume `00000000-0000-0000-0000-NNNNNNNNNNNN`, the lowest free index from 1 to 100 in mount order, where stock LineageOS 21 uses the FAT or exFAT serial. The Cube's card is `/storage/00000000-0000-0000-0000-000000000001`; the index shifts when a USB drive mounts first or the card has extra volumes; `emusync::isVolumeId` (P:17466) accepts only 9-character or 32-hex ids. Section 7's premise that the id is the serial is wrong for the Cube (section 7.3). Options:
    - (a) The card contract uses tokens and relative paths only (principle 2), and Linux rewrites nothing. Paths the Cube already wrote do not resolve on Linux.
    - (b) Linux also resolves the Android-side path: it finds the `/storage/<id>` prefixes Android wrote on the card (or a field Android records in `emulator_config/card.json`) and makes those paths resolve to the adopted drive, while mounting at `/storage/<serial>` itself.
    - (c) The Android build's vold is patched to name volumes by serial, with a migration of existing saved paths. Android-side work; the Android build is maintained separately (decision 3).
    - (d) Combinations.

    Recommended: **(b)**, for compatibility with cards the Android builds have already written, **plus (a) as the rule for everything new**; and raise (c) with the Android build's maintainers as a separate fix, because mount-order ids also break Android's own saved paths.
21. **Device layer. Decided 2026-11-25: Thimsy Base + importer.** The owner's goal is the broadest device support, initially ROCKNIX's coverage, with devices that already boot Linux first and the Odin 2 not a focus. One Linux flavour: `thimsy-import` tracks ROCKNIX's hardware layer at tags Thimsy chooses, and families arrive in waves on the Thimsy Base with Thimsy's own fast boot. It imports kernel patches (flattened series keeping ROCKNIX's file names), device trees, `config.xml`'s device and DTB lists, firmware lists, and device facts written fresh as `device.json` data (no quirk script and no InputPlumber YAML is copied). U-Boot and the boot chain stay Thimsy's (FlipBase's series); ROCKNIX's U-Boot data and patches are only consulted. Imperative blocks are hand-ported with hash guards, and the release-image check runs in CI (section 3.2). Considered and not chosen (section 3.1): hand imports per family; Nano as a ROCKNIX frontend package for the long tail, alone or first; back to a ROCKNIX downstream; ROCKNIX-built hardware packs; Batocera's board tree. Earlier answers stand: the Thimsy Base (decision 1), per-device cards, 3 s on RK3566 (decision 5), builds on this machine (open question 6), and the Android build separate (decision 3).
22. **Kernel per family** (new, 2026-11-25). Accept "the kernel version is ROCKNIX's for each family at a Thimsy-chosen ROCKNIX tag", with several kernel bases in parallel (vendor BSP 6.1, 6.18, 7.1, 7.2, 7.3 today) and FlipBase's and Thimsy's patches as an overlay re-checked on each bump. Thimsy skips tags freely: ROCKNIX changes a family's kernel version about 6 times a month across families, reverts included, and a family move that carries Thimsy's overlay costs about 1 to 2 weeks *(guess)*. RK3566's version is chosen at the start of Phase 2a from the tags that exist then. If ROCKNIX's monthly tags stop, a family stays frozen on its last tag and imports a named `next` commit only for a needed fix, after a soak on its hardware-in-the-loop unit. Open: stable point releases on top of ROCKNIX's unpatched .0 kernels (open question 34). Recommended: yes.
23. **Input** (new, 2026-11-25). Options: (a) `thimsy-padd` (uinput, no D-Bus, no udev; grabs and hides each device's source nodes; one Thimsy-identity pad; intercept mode for the Quick Menu) on every family, with `device.json` pad data written from facts; (b) kernel normalisation: convert InputPlumber families' device trees to `rocknix-singleadc-joypad` (Batocera's rk3568 patches 0004 and 0005), a permanent per-board patch that can't go upstream and fights the importer; (c) InputPlumber on those families, which brings D-Bus, udev and GPL-3.0 back. InputPlumber is never in the image under (a) or (b). Recommended: **(a)**, with (b) only for a family where Phase 0 shows hiding fails (section 5.3).
24. **Firmware policy** (new, 2026-11-25). It changes section 1's "never redistributes a handheld maker's firmware" in practice. Options: (a) ship only `linux-firmware` files with known licences, and fetch the rest from the user's own device at install time (over adb on the Qualcomm units, which keep Android), onto a partition, not into the kernel image; (b) ship ROCKNIX's `extra-firmware` as ROCKNIX does (marked `proprietary`, no licence file; includes a blob extracted from a stock Android image); (c) leave out devices whose firmware can't be shipped or fetched. Recommended: **(a)**, with (c) for anything (a) can't reach.
25. **Image structure** (new, 2026-11-25). One userland per CPU tier (v8.0, v8.2, later v9), GPU packs (Mesa Panfrost with Panthor; Mesa Freedreno with Turnip), and a hardware pack per family, put together per device, instead of one full Buildroot build per family. Per-device cards stay the result (decision 1). It keeps a full rebuild at roughly 10 to 20 h on this host instead of 30 to 60 *(guess)*. Recommended: yes.
26. **Coverage order and the support claim** (new, 2026-11-25). Confirm waves 0 to 4 (section 2.4): wave 0 the ARCs; wave 1 the rest of RK3566 and H700; wave 2 RK3326 image b, RK3399, RK3576, S922X; wave 3 the Qualcomm families through OpenABL, SM6450 among them once a ROCKNIX tag carries it, with the Odin 2 Portal and RP Duo as test units only, **deferred** (later: needs bootloader writing, under the SD-only principle of section 1); wave 4 RK3326 image a and RK3588, not waiting for wave 3; AMD64 as QEMU only. Publish a per-product status (built, boots, works, certified) instead of a "supports ROCKNIX's devices" claim, with the Device check and opt-in tester reports feeding it (section 11.2). Recommended: yes.

---

## 15. Open questions

1. **Answered 2026-11-25.** What and where is "Paper Plane"? The owner: FlipBase or BaseOS, under a misremembered name. FlipBase is the boot reference (section 9.1).
2. **Answered 2026-11-25.** Which ARC does the owner have? An RG ARC-D only. The goal is to keep Android on the eMMC and boot Thimsy from card slot 1, which the ARC-D's stock loader already does for Anbernic's own Linux card (section 8.6). Erasing Android is dropped permanently by the SD-only principle (section 1), so whether it is a "legacy bootloader" unit no longer matters (question 17 is moot).
3. **Moot 2026-11-25.** How do ROCKNIX's maintainers see a renamed downstream? There is no downstream now. What remains is credit for imported patches (decision 16) and their provenance (question 22).
4. **Reworded again 2026-11-25** (`thimsy-padd`). Does padd's grab-and-unlink hiding keep the ARC's three source nodes (`gpio-keys-control`, `adc-keys-home`, `pwm-vibrator`) away from RetroArch and an SDL standalone under libudev-zero, or is the libudev-zero patch that drops `ID_INPUT_JOYSTICK` needed; does intercept mode release a held button cleanly; and does SDL read an external pad's `hidraw` node behind a grab (section 5.3)?
5. **Answered 2026-11-25.** Which test devices and cards exist already? The owner has an RG Cube (Android Thimsy), an RG ARC-D, an AYN Odin 2 Portal (SM8550; Linux through OpenABL, not a focus now) and a Retroid Pocket Duo (Qualcomm Dragonwing Q-7790, the IoT flavour of SM7750 "Eliza"; it already runs Linux: Armada marks it tested, booting through OpenABL's SM8750 image with Android kept, and Retroid publishes a mainline-based 7.3 kernel tree with its device tree), plus spare microSD cards and a USB card reader. Everything else in section 11.1 is still to be bought.
6. **Answered 2026-11-25.** Where does the build tree go? On this machine: a WSL2 distro whose ext4 disk is on the owner's 21.8 TB USB drive (D:), like the Android build's `thimsy-aosp`; one output tree per tier userland, with its GPU pack built in it so host LLVM is built once per tier tree, and one per family hardware pack, all sharing `dl/` and ccache (decision 25's layout, as proposed; section 10); releases built here too. The machine has 16 GB RAM (13 GB for WSL2), so one tree builds at a time (section 10).
7. **Moot 2026-11-25.** Does restoring an `adb pull` eMMC backup bring an ARC-D's Android back? Erasing Android is dropped permanently (the SD-only principle, section 1), so there is no erase guide and no restore to try.
8. **Answered from code 2026-11-25; device test in Phase 0.** Does Android (LineageOS 21 vold) mount an exFAT data partition that is the fifth partition of a boot card (section 8.2's layout)? Yes, if that entry is typed Microsoft basic data and sits at entry 7 or lower; the card's other entries must carry types vold ignores, or they become "unmountable" volumes that lead to the format wizard (sections 7.3, 8.2, 13). Also found: the Cube's vold names volumes by a mount-order index, not the serial (decision 20). Still for the device: `perdev_minors`, the id the partition gets, leftover `/storage` directories, which drive Nano picks, and USB mount-order shifts (Phase 0 item (g)).
9. **Answered from code 2026-11-25; device check in Phase 3.** Do released Android builds keep an unknown field in `nano/emulators.json` when they rewrite it? Yes, on every entry except the game the user re-picks on Android, for the test 34 to test 41 private draft builds (all builds with this writer). `names.json`, `index.json` and `systems.json` are rebuilt from structs and drop unknown fields at every level (section 7.6).
10. Which default cores can match across the builds on RK3566, and which pairs are save-compatible (PCSX ReARMed and Beetle PSX memory cards; gpSP and mGBA)?
11. **Answered 2026-11-25 (owner: "yes, but we need to test it").** Do save states from the Android buildbot cores load in the Thimsy Base's source-built cores of the same commit? Yes for same-commit 64-bit cores (AAPCS64 on both sides; the same commit per question 23), confirmed per core by a test: save on the Cube, load on the ARC and back, one game per system. Only cores that pass share states later; scheduled with "share states later", after v1 (section 7.5). Risks: snapshots that depend on build options, cores that check build details, core options that change the emulated hardware, and the gap between the two RetroArch versions.
12. **Reworded 2026-11-25** (owner: test both, ship the most stable first; decision 11 amended). The Phase 0 A/B on the ARC-D: Panfrost against libmali across the v1 emulator set. Pass rule: v1 ships the more stable driver (crashes, GPU hangs, rendering errors, sleep and resume); performance and first-frame time count second. Both stay buildable and switchable until then.
13. **Answered 2026-11-25; deferred.** Does the ARC board expose `ttyS2`, and what does U-Boot's `bootstage` report on it? The owner can open the ARC-D and solder serial wires, but not now: a to-do for later (`TODO.md`). Phase 0 starts without serial: on-screen U-Boot output, a USB gadget console, arch-counter timing. Facts: likely tx, rx and gnd pads under the back cover (the RG556 precedent, macromorgan); RK3566's debug UART runs at 1,500,000 baud, 3.3 V; it needs a CP2102N- or FT232-class adapter, not a CH340 (section 11.1). What `bootstage` reports on it waits for the wiring.
14. **Answered 2026-11-25.** RG ARC HDMI on the Thimsy Base: panel and HDMI at once, as FlipBase drives them; how mirroring is drawn with no compositor; and which planes are left for a menu while HDMI is active (section 5.3)? The owner chose **Docked in v1**: with HDMI connected, Nano and its games go to the TV and the panel is off; the emulator picks the HDMI connector. Mirroring later, by a per-game compositor or the display patches (sections 5.2 and 5.4).
15. **Answered 2026-11-25.** Do any of GammaOS's own plans (a Linux Nano, "GCore+" Android 14 for RK3566) overlap with this? The owner: GammaOS is upstream of Thimsy's Android build, but Thimsy takes its own direction for its Linux builds, so GammaOS's Linux plans, if any, do not steer this plan. Nano's shared code still comes from GammaOS's tree (section 4.3), so upstream changes to it keep merging through the seam. `multi_device_plan.md` question 9 still asks the Android half (GCore+).
16. **Answered from public sources 2026-11-25; Mac check in Phase 4.** The macOS `diskutil` field that marks a built-in SD reader: `BusProtocol` = "Secure Digital". `Internal` is true for the slot, so it can't tell a card from a disk; `RemovableMedia` separates the card from the SSD. Current macOS disk images report `BusProtocol` "Disk Image" and `Internal` false (section 8.5).
17. **Moot 2026-11-25.** How can a user tell a "legacy bootloader" ARC-D or RG353P/M/V before erasing it? Erasing Android is dropped permanently (the SD-only principle, section 1), so nothing is erased.
18. **Answered from code 2026-11-25; device test in Phase 0.** Do the kernel's vfat and exFAT drivers on both platforms set the dirty flag while mounted, so that a dirty-flag-only check catches every unclean removal (section 7.3)? vfat: yes, on both, so dirty-flag-only is sound. exFAT: yes on the Thimsy Base's kernels (each family's ROCKNIX version, decision 22; all newer than 6.16, which carries the exFAT fix), **no on the Cube**, whose vendor 5.4.233 kernel has a pre-5.18 exFAT backport that can leave a torn volume flagged clean (section 7.8). So Linux checks exFAT when flagged or not last cleanly unmounted by this device, or always if cheap. RK3588's vendor BSP 6.1 kernel predates 6.16: it needs the backport, or is checked in wave 4. This depends on the filesystem, not the card's size: a 16 GB card is safe with the flag alone only while it stays FAT32. Still for the device: the backport's exact revision, whether the clean boot sector reaches the card before the pending writes in practice, and how long the checks take (Phase 0 item (g)).
19. **Moot 2026-11-25.** The full list of paths ROCKNIX's scripts build under `/storage/roms`: the Thimsy Base has no `/storage/roms` (section 7.3).
20. **Made a Phase 0 item 2026-11-25** (section 8.6's order and pass rule; nothing written to the eMMC). Which dual-boot route works on the ARC-D? Is its eMMC loader a U-Boot SPL that loads a FIT by partition name, or a legacy miniloader that loads `uboot.img` and `trust.img`; which BL31 runs under route (a); and does deep sleep work under it (section 8.6)? Order: read the eMMC loader read-only over adb (sector 64), then route (d), route (a) (JELOS and FlipBase precedent), route (b). Pass: boots from slot 1; Android boots untouched with the card out; deep sleep works (FlipBase's pm-config patch ported first); the loader's time measured (question 21). If none passes, the ARC-D waits for bootloader writing (later) and the ARC-S becomes the first device.
21. **Made a Phase 0 item 2026-11-25**, part of question 20's pass rule. How long does the ARC-D's vendor loader run before our U-Boot under routes (a) and (b) *(guess: 0.5 to 1.5 s; possibly much less, as on the Flip)*?
22. **Answered 2026-11-25.** Who wrote ROCKNIX's bare-diff patches (0025, 0029 and the others without a header)? The owner: "I don't care who wrote it. Use the ROCKNIX license." No author is recovered: the earlier author-recovery step is dropped. Imported patches are used under ROCKNIX's licence (GPL-2.0, in `gpl/`), each with a one-line provenance note (ROCKNIX path and commit) that the importer writes automatically. Upstreaming header-less patches is out of scope. Count corrected: about 160 of the 580 kernel patches lack a `From:` line (a census of `next` at `0f4806b`: 158 to 160), not 169; 547 of the 580 are under `devices/*/patches/linux`, and 33 are shared (section 3.2).
23. **Recommended 2026-11-25; the owner did not object.** Buildroot package sources. For RetroArch, each v1 core, PPSSPP, ScummVM, mupen64plus, melonDS and Flycast: which reference to follow for versions, flags and patches, and which non-commercial cores need a `_REDISTRIBUTE` review? Thimsy writes its own recipes; ROCKNIX's `package.mk` files (GPL-2.0) are ported into `gpl/` for versions, flags and patches; Batocera's LGPL recipes are reference only; Recalbox's never. Each core is built from the same commit as the Android build's `retroarch-cores.lock`, so saves and states match (this ties to question 10). The non-commercial cores (Snes9x, Genesis Plus GX, PicoDrive, FBNeo, MAME 2003) ship as on Android, never sold, with their licence fields set honestly for `legal-info` (section 3.3).
24. **Reworded 2026-11-25** (decision 18 decided for RK3566: the unit's own vendor BL31). What is the deep-sleep current with the vendor BL31 on the reference unit (FlipBase: about 12 mA)? Measured in Phase 0.
25. **Read from the mainline device trees 2026-11-25; the rest is a Phase 0 checklist.** On mainline, for the ARC: which Wi-Fi and Bluetooth chip and firmware; does audio route correctly through `rk817_ext`; what is the deep-sleep current; and are the panel's init waits right (FlipBase found a hex `wait=` bug on the Flip)? From the device trees: Wi-Fi is SDIO on `sdmmc2`, with the chip not named (an RTL8821CS, with `rtw88_8821cs` and `linux-firmware` *(guess)*); `uart1` has flow control but mainline has **no Bluetooth node** (ROCKNIX may patch one in; the importer brings it if so); audio is fully described (`simple-audio-card` `rk817_ext`, the RK817 codec, a GPIO speaker amplifier, headphone detect on GPIO4 C6, routing); the panel is `anbernic,rg-arc-panel` on st7701 (ROCKNIX's 0014 timings); the ARC-D's touch is a Goodix GT927 (ROCKNIX's 0025 axes). Phase 0 (b) checks: the Wi-Fi chip id and a connection, the Bluetooth node and pairing, speaker and headphone switching, the panel's init delays audited, touch orientation, and sleep current (question 24).
26. **Answered 2026-11-25.** Will BaseOS's author license the files FlipBase still carries from it, or are they rewritten (section 3.3)? The owner: don't weigh the licence; files with no stated licence are treated as following BaseOS's. The fact: BaseOS has no licence file, so default copyright applies. Most such files are replaced anyway: `flash-card.sh` by the installer, `flipbase-update` and `mkupdate.py` by Thimsy's update, `flipbase-config` by Nano's settings. The four small C files (`fbsplash.c`, `gptgrow.c`, `gptslot.c`, `gpt.h`) are used as-is and rewritten when convenient *(guess: a few days)*.
27. **Reworded 2026-11-25.** Does the rotation contract (Nano, the SDL3 KMSDRM patch, the RetroArch `drm_ctx.c` patch, a plane where one is advertised, a blit otherwise) hold on Panfrost and Panthor, touch included, on the ARC and the RG28XX, and what does the blit cost? On one Qualcomm portrait unit, which display setting does each family need (open question 35)? SDL's KMSDRM backend does need a patch: upstream SDL3 does not rotate (section 5.3). **Nothing to decide (2026-11-25):** the rotation contract covers it, and it becomes a Phase 0 and Phase 2 test per v1 emulator: picture, menus and touch the right way up, with no visible frame-rate cost. An emulator that fails uses the per-game compositor fallback (`display: kiosk`, section 5.3). The Qualcomm part waits with wave 3.
28. Does `sdl2-compat` carry the SDL3 rotation to SDL2 apps, and which standalones link the system SDL rather than their own?
29. Does Weston's kiosk-shell run as root with libudev-zero and libseat's builtin backend, and what is its per-launch cost on RK3566 *(guess: 0.2 to 0.6 s)*?
30. Which ROCKNIX tag first carries RK3566 on 7.3.5, and which version does RK3566 start on at Phase 2a (decision 22)?
31. On SM8550, how long from kernel start (EFI stub) to Nano's first frame, with the GPU firmware loaded from the root or a firmware partition at first open, and what is the power-on total through XBL, ABL and OpenABL?
32. Do the RK3326 EE-clone and `generic-dsi` panels need an installer question, or can the panel be detected?
33. **ROCKNIX's tag cadence.** Will ROCKNIX keep monthly stable tags (they exist only from `20260601` to `20261101`, after a 12.5-month gap), and is decision 22's fallback (freeze, then a soaked `next` commit for a needed fix) enough if they stop?
34. May Thimsy apply stable point releases on top of ROCKNIX's unpatched .0 kernels (H700 on 7.2.0 since 2026-08-17; five Qualcomm families on plain 7.2), at the cost of the one-to-one import diff, or only security fixes, or none?
35. How do the Qualcomm families rotate Vulkan with no compositor in front of the game: a compositor per game, gamescope (ROCKNIX's DPU-rotation patches; libseat and udev needs unverified), a Mesa `wsi_display` rotation patch, or a present-time Vulkan layer; and what does each cost on one Qualcomm unit (section 5.3)?

---

## 16. Sources

**ROCKNIX**
- https://github.com/ROCKNIX/distribution (`next`): `LICENSE.md`, `README.md`, `config/options`, `Makefile`, `scripts/mkimage`, `scripts/image`, `projects/ROCKNIX/config.xml`, `devices/{RK3566,H700}/options`, `packages/linux/package.mk`, `packages/virtual/image/package.mk`, `packages/virtual/emulators/package.mk`, `packages/sysutils/busybox/scripts/init`, `packages/rocknix/sources/scripts/{runemu.sh,setsettings.sh,automount,autostart,input_sense,wifictl}`, `rocknix-update`, `99-automount.rules` (in the `emulators` package), udevil's rule, `distributions/ROCKNIX/{version,options,config/functions,kernel_options}`, the same files at tag `20261101`, the `gpudriver` package, `packages/hardware/quirks/` (`090-ui_service`, `111-sway-init`, the ARC-D remappings), the InputPlumber device and capability maps for the ARC, `start_ppsspp.sh`, `start_mupen64plus.sh`; for the 2026-11-25 revision: `projects/ROCKNIX/devices/RK3566/{patches/linux,options,linux/linux.aarch64.conf,packages/u-boot-Generic/package.mk,filesystem/usr/share/inputplumber}`, `devices/H700/` (kernel patches, device trees, the DDR3 and DDR4 U-Boot), `packages/tools/atf/package.mk`, `packages/hardware/quirks/platforms/RK3566/030-suspend_mode`, `packages/hardware/quirks/devices/`
- https://github.com/ROCKNIX/rocknix.org: `docs/devices/anbernic/rgarc.md`, `rg353pmvvs.md`, `rg35xx-h.md`, `includes/platforms/android-wipe.md`, `docs/contribute/build.md`; https://rocknix.org/configure/h700-installation/ , https://rocknix.org/faqs/ , https://rocknix.org/play/update/
- https://github.com/OpenHandheldCollective/OpenABL , https://github.com/ROCKNIX/rocknix-joypad , https://github.com/ShadowBlip/InputPlumber , https://github.com/JustEnoughLinuxOS/distribution (`scripts/mkimage`, `projects/Rockchip/bootloader/mkimage`); ROCKNIX PR #154: https://github.com/ROCKNIX/distribution/pull/154
- For the coverage design (2026-11-25): `projects/ROCKNIX/config.xml` on `next` (commit `0f4806b`) and at tag `20261101`; `devices/*/options` (`TARGET_CPU`, `GRAPHIC_DRIVERS`, `WINDOWMANAGER`); `packages/linux/package.mk` at `next` and `20261101` and its commit history since 2026-05-25 (GitHub API, `commits?path=`); `devices/RK3566/linux/linux.aarch64.conf` (lines 159-160, 226, 2601, 6747); `devices/*/packages/u-boot-*/package.mk`, `packages/tools/{atf,openabl}/package.mk`, `kernel-firmware.dat`, the `extra-firmware` package (`PKG_LICENSE`); `scripts/unpack`; `packages/hardware/quirks/` (a census of 668 files' licences and dependencies; `020-set-xbox-gamepad`; `platforms/RK3566/090-ui_service`); the InputPlumber device and capability YAML (`01-rg-arc.yaml`, SM8550 `02-ayn-controller.yaml`); the `gamescope` package and its patches; `.github/workflows/{build-nightly.yml,build-device.yml}`; https://github.com/ROCKNIX/distribution-nightly (`nightly-20261125`); the releases API (tags `20250517` and `20260601` to `20261101`) and the compare API (`20261101...next`); PRs #2685, #3282, #3353, #3377, #3547 and #3690; `packages/graphics/gpudriver/sources/bin/gpudriver` (default `libmali`); `packages/linux/patches/rockchip/rockchip-0012-*` (Collabora's AFBC stride check); `rocknix-joypad` at `ac1be0fa`
- InputPlumber `src/main.rs` (line 53, `Connection::system()`); OpenABL's README (BLS Type #1, devicetree precedence, `loader.conf` timeout, `splash_region`)
- Batocera `board/batocera/rockchip/rk3568/linux_patches` (0004, 0005) and its H700 board tree; Knulli's tree (no importer found by code or commit search)

**Other firmware**
- Batocera: https://github.com/batocera-linux/batocera.linux/tree/master/configs , `COPYING` and `package/batocera/emulators/` (RetroArch with its `libretro-*` packages, PPSSPP, ScummVM, mupen64plus, melonDS, Flycast) , https://wiki.batocera.org/choose_a_handheld , https://wiki.batocera.org/license , https://wiki.batocera.org/compile_batocera.linux
- Knulli: https://github.com/knulli-cfw/distribution (`COPYING`, `README.md`), https://github.com/knulli-cfw/knulli-linux (`configs/knulli-rk3566.board`, H700 `genimage.cfg`), https://knulli.org/devices/ , https://knulli.org/faq/knulli/ , https://knulli.org/guides/partitioning/
- AmberELEC: https://github.com/AmberELEC/AmberELEC ; dArkOS: https://github.com/christianhaitian/dArkOS ; UnofficialOS: https://github.com/RetroGFX/UnofficialOS
- Recalbox: https://gitlab.com/recalbox/recalbox (`LICENSE.md`, `package/`)
- muOS: https://github.com/MustardOS/internal , https://muos.dev/docs/infrastructure/storage ; MinUI: https://github.com/shauninman/MinUI ; NextUI: https://github.com/LoveRetro/NextUI ; spruce: https://github.com/spruceUI/spruceOS , https://github.com/spruceUI/oakMOSS ; Onion: https://github.com/OnionUI/Onion ; Allium: https://github.com/goweiwen/Allium ; CrossMix: https://github.com/cizia64/CrossMix-OS
- GammaOS: https://github.com/TheGammaSqueeze/GammaOSNext , https://github.com/TheGammaSqueeze/GammaOSCore , https://github.com/TheGammaSqueeze/GammaOS-RK3566 , https://github.com/TheGammaSqueeze/GammaOSNextDistribution-14 (commit `0bc531e2d`)

**Devices and kernels**
- https://github.com/torvalds/linux/tree/master/arch/arm64/boot/dts/rockchip : `rk3566-anbernic-rg-arc.dtsi`, `rk3566-anbernic-rg-arc-d.dts`, `rk3566-anbernic-rg-arc-s.dts`, `rk3566-anbernic-rgxx3.dtsi`
- Linux `drivers/gpu/drm/panel/panel-sitronix-st7701.c`, `drivers/gpu/drm/rockchip/{rockchip_drm_vop2.c,rockchip_vop2_reg.c}`, `drivers/gpu/drm/{drm_lease.c,drm_auth.c}`, `arch/arm64/boot/dts/rockchip/rk356x-base.dtsi` (the RGA node, v6.18 and v7.3); https://docs.kernel.org/gpu/drm-uapi.html ; https://lore.kernel.org/linux-arm-kernel/12a8c0ef-90ee-cf7e-50a0-e00add8af147@rock-chips.com/
- U-Boot: https://docs.u-boot.org/en/latest/board/anbernic/rgxx3.html , `board/anbernic/rgxx3_rk3566/rgxx3-rk3566.c`, `arch/arm/dts/rk356x-u-boot.dtsi`. RK3566/RK3568 boot order: https://barebox.org/doc/latest/boards/rk35xx.html ; lore.kernel.org/all/20230517182624.1765359-9-jonas@kwiboo.se/
- A community account of the RG353 boot process: https://www.reddit.com/r/RG353M/comments/1fv7eaa/rg353m_boot_process (not fetched; read through a search summary)
- https://www.androidcentral.com/gaming/anbernic-rg-arc-d-arc-s-announced , https://droix.net/blogs/anbernic-rg-arc-s-arc-d-review-with-video/
- https://macromorgan.blogspot.com/2024/04/anbernic-rg556.html (T820 bootloader)
- https://docs.u-boot.org/en/latest/board/rockchip/rockchip.html , https://docs.u-boot.org/en/latest/board/allwinner/sunxi.html
- https://docs.mesa3d.org/drivers/panfrost.html ; Mesa `docs/drivers/panfrost.rst`, `src/panfrost/vulkan/panvk_physical_device.c`, https://gitlab.freedesktop.org/mesa/mesa/-/tags ; https://www.collabora.com/news-and-blog/news-and-events/panfrost-achieves-opengl-es-3.1-conformance-on-mali-g52.html
- Rotation and planes (v6.18 unless noted): `drivers/gpu/drm/rockchip/{rockchip_vop_reg.c,rockchip_vop2_reg.c}`, `drivers/gpu/drm/sun4i/{sun8i_ui_layer.c,sun8i_vi_layer.c}`, `drivers/gpu/drm/meson/meson_plane.c`, `drivers/gpu/drm/drm_client_modeset.c`, `drivers/gpu/drm/msm/disp/dpu1/` (`DPU_SSPP_INLINE_ROTATION`), `drivers/gpu/drm/msm/msm_drv.c` (lines 200-208 and 267-272: `msm_open()` and `load_gpu()`); ROCKNIX's `panel-generic-dsi.c` and its DPU and Mesa rotation patches; Mesa `src/vulkan/wsi/wsi_common_display.c` (lines 837, 1317-1318); SDL issue #14905 and #9682, `src/joystick/SDL_joystick.c` (lines 2263-2321); RetroArch `gfx/drivers/gl2.c` (`mvp_no_rot`); weston MR 315; libudev-zero `udev_device.c` (lines 450-513)
- Boot of other families: NXP weston-imx figure, https://lists.freedesktop.org/archives/wayland-devel/2020-July/041546.html
- GitHub-hosted runners: docs.github.com, the GitHub-hosted runners reference (4 CPUs, 16 GB RAM, 14 GB SSD on standard public-repository `ubuntu-24.04` and `ubuntu-24.04-arm` runners)
- Buildroot `linux/Config.in` and `boot/uboot/Config.in` (custom version, patches, config fragments, custom DTS directory, `NEEDS_ATF_BL31`, `NEEDS_ROCKCHIP_RKBIN`); Linux `scripts/extract-ikconfig`

**ES-DE, RetroArch, emulators**
- ES-DE: `INSTALL.md`, `USERGUIDE.md`, `FAQ.md`, `ANDROID.md`, `LINUX-AARCH64.md`, `CHANGELOG.md`, `CMakeLists.txt`, `es-app/src/FileData.cpp`, `GamelistFileParser.cpp`, `GuiAlternativeEmulators.cpp` and `resources/systems/{android,linuxarm,linux}` at https://gitlab.com/es-de/emulationstation-de (label tables at commit `50e4b600a`)
- RetroArch: https://github.com/libretro/RetroArch (`frontend/drivers/platform_unix.c`, `runloop.c`, `gfx/drivers_context/drm_ctx.c`, `command.c`), https://docs.libretro.com/guides/overrides/ , https://docs.libretro.com/guides/kms-mode , https://docs.libretro.com/development/licenses/
- mupen64plus-ui-console `src/main.c`; mupen64plus-ae `GamePrefs.java`
- SDL: https://github.com/libsdl-org/SDL (`src/joystick/linux/SDL_sysjoystick.c`, `SDL_hints.h`); MinUI `skeleton/SYSTEM/rg35xxplus/paks/MinUI.pak/launch.sh`, `workspace/all/minarch/minarch.c`
- https://dolphin-emu.org/docs/guides/controlling-global-user-directory/ , https://ppsspp.org/docs/faq , https://docs.scummvm.org/en/latest/advanced_topics/configuration_file.html , https://vita3k.org/faq , https://github.com/stenzek/duckstation
- https://source.android.com/docs/core/storage/scoped (FUSE bypass for `Android/data`); vold (`android14`) `Vfat.cpp`, `Exfat.cpp`, `PublicVolume.cpp` (mount flags and fsck); e2fsprogs `lib/blkid/probe.c`, util-linux `libblkid/src/superblocks/vfat.c` (volume id format)
- vold in GammaOS Next 14 (`develop`) and LineageOS `lineage-21.0`: `Disk.cpp` (partition types, `perdev_minors`), `PublicVolume.cpp` (synthetic volume ids, mount and fsck), `VolumeBase.cpp` (the index allocator), `Utils.cpp` (`IsFilesystemSupported`), `fs/Vfat.cpp`, `fs/Exfat.cpp`; frameworks `StorageManagerService.java`, SystemUI `StorageNotification.java`; FlipBase `tools/mkgpt.py`, `src/gptgrow.c`; the RG Cube's own logcat and `getprop` captures on Thimsy_OS_v1.0a
- Linux `fs/fat/inode.c`, `fs/exfat/super.c` and `fs/exfat/inode.c` (v5.10 and v7.3), `fs/exfat/namei.c` (v5.17, v5.18); commits b88a105802e9, a4a3d8c52d95, 46a557694b46, 7d390ca06fe7; AOSP `external/fsck_msdos` `fat.c`; dosfstools `src/check.c`; the RG Cube's release `.pac` (kernel version strings)

**Disk writing**
- Raspberry Pi Imager: https://github.com/raspberrypi/rpi-imager (`src/drivelist/*`, `src/downloadthread.cpp`, `src/windows/*`, `src/mac/*`, `src/linux/*`)
- bb-imager-rs: https://github.com/beagleboard/bb-imager-rs (`bb-drivelist/src/pal/macos.rs`); drivelist: https://github.com/balena-io-modules/drivelist (`src/darwin/list.mm`); rpi-imager `src/drivelist/drivelist_darwin.mm`
- macOS disk classification: Apple's IOKit `IOStorageProtocolCharacteristics.h`, DiskArbitration `diskarbitrationd/DADisk.c`; ghw `pkg/block/block_darwin.go` (diskutil plist keys); public `diskutil info` output: radugin.com (2025-05-04, Apple silicon built-in reader), GitHub gist arwagner/5a43bd6f (internal USB reader), a December 2021 comment at deciphertools.com (disk image), https://github.com/cwmoriarty/montserrat/issues/1
- https://storaged.org/doc/udisks2-api/latest/ , https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering , https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_lock_volume
- https://github.com/yoctoproject/bmaptool , https://github.com/collabora/bmap-rs , https://github.com/rafalh/rust-fatfs , https://docs.rs/gptman , https://docs.rs/mbrman

**Boot time**
- FlipBase: https://github.com/apommel/FlipBase (`docs/boot-time.md`, `docs/uboot.md`, `docs/decisions.md`, `docs/boot-chain.md`, `docs/card.md`, `docs/power.md`); its RG353PS fork: https://github.com/dengzhan007/BaseOS-RG353PS (`platforms/rg353ps/{README.md,BOOT-TIME.md,RAW-FIT.md}`, `kernel/UPSTREAM.md`)
- BaseOS: https://github.com/pvaibhav/BaseOS (`docs/00` to `docs/12`, `experiments/h700-boot-speed/DESIGN.md`, releases; licence: none); Zetarancio: https://github.com/Zetarancio/Miyoo-Flip-Mainline-Linux-Reverse-Engineering (licence: none)
- RetroStoneOS: https://github.com/PaddleStroke/RetroStoneOS (`docs/build.md`, `docs/host-design.md`, `docs/display-design.md`, `docs/bluetooth-audio.md`, `buildroot-external/configs/retrostone2_release_defconfig`, `buildroot-external/board/common/rootfs-overlay/usr/bin/rsos-net`)
- Bootlin, ELCE 2019: https://bootlin.com/pub/conferences/2019/elce/opdenacker-timing-boot-time-reduction-techniques/opdenacker-timing-boot-time-reduction-techniques.pdf
- SD multiplexers for hardware-in-the-loop testing: SDWire-class boards (price a guess)
- Raspberry Pi, "Optimising boot time" whitepaper (2026); https://docs.u-boot.org/en/latest/develop/falcon.html ; https://docs.kernel.org/admin-guide/kernel-parameters.html ; https://docs.mesa3d.org/envvars.html
- REG-Linux: https://github.com/REG-Linux/REG-Linux

**Thimsy Base: build tree, packages, backend**
- FlipBase's tree (GitHub API: the recursive tree, tags, releases, contributors): `LICENSE`, `NOTICE`, `README.md`, `CONTRIBUTING.md`, `VERSION`, `build.sh`, `.github/workflows/build.yml`, `configs/flipbase_my355_defconfig`, `board/my355/` (`linux/hardware/`, `linux/stock/`, `uboot/patches/`, `rootfs-overlay/init`, `rootfs-overlay/etc/init.d/{rcS,S40bluetooth,S49ntp}`, `rootfs-overlay/usr/sbin/frontend-session`), `package/mesa-panfrost/mesa-panfrost.mk`, `patches/flipbase-sdl3/`, `docs/{rootfs.md,rootfs-interface.md,mainline-compat.md,hardware.md}`
- Buildroot: https://github.com/buildroot/buildroot (`COPYING`, `docs/manual/legal-notice.adoc`, `docs/manual/prerequisite.adoc`, `support/dependencies/dependencies.sh`, `utils/docker-run`, `support/docker/Dockerfile`, `package/mesa3d/{Config.in,mesa3d.mk}`, the `package/` listing on `master`); tags 2026.08.2 and 2026.11-rc2
- FlipBase's CI: https://github.com/apommel/FlipBase/actions (the v2.0.0 `build` run, 2026-11-22: job times); `board/my355/rootfs-overlay/etc/init.d/rcS` (D-Bus at boot); `patches/flipbase-sdl3/`. U-Boot's `doc/board/anbernic/rgxx3.rst` and `board/anbernic/rgxx3_rk3566/rgxx3-rk3566.c` (`detect_panel`, `startup_buzz`); Linux `drivers/gpu/drm/rockchip/rockchip_drm_vop2.c` (`vop2_create_crtcs`, `vop2_is_mirror_win`), `drivers/gpu/drm/drm_auth.c` (`drm_master_check_perm`), `arch/arm64/boot/dts/rockchip/rk3566-anbernic-rgxx3.dtsi` (HDMI on VP0); Mesa `src/panfrost/vulkan/panvk_physical_device.c` (v6, v7 refused without the override); Buildroot `package/iwd/Config.in` (selects D-Bus), `package/stella`
- Backend: https://w1.fi/wpa_supplicant/ ; iwd and D-Bus: https://lists.buildroot.org/pipermail/buildroot/2020-July/286562.html , https://github.com/illiliti/eiwd ; https://github.com/bluekitchen/btstack/blob/master/LICENSE ; https://github.com/illiliti/libudev-zero ; glibc `time/localtime.c`, `time/tzset.c`
- Display: https://github.com/cage-kiosk/cage/issues/95 , https://github.com/cage-kiosk/cage/issues/327 ; AGL's drm-lease-manager: https://docs.automotivelinux.org/en/trout/06_Component_Documentation/05_drm_lease_manager/ ; Qt's eglfs platform plugin documentation

**Licences and build hosts**
- https://www.gnu.org/licenses/gpl-faq.html#MereAggregation , https://www.apache.org/licenses/GPL-compatibility.html , https://www.gnu.org/licenses/license-list.html
- https://learn.microsoft.com/windows/wsl/case-sensitivity , https://learn.microsoft.com/en-us/windows/wsl/filesystems , https://openwrt.org/docs/guide-developer/toolchain/wsl
- https://buildroot.org/downloads/manual/manual.html , https://docs.yoctoproject.org/brief-yoctoprojectqs/index.html

**This project**
- `README.md`, `BIOS.md`, `HOTKEYS.md`, `BACKPORTS.md`, `NOTICE`, `mods/37-esde.sh`, `scripts/esde_config.py`, `scripts/bundled_roms.py`, `mods/esde-emulators.txt`, `mods/nano-systems.txt`, `scripts/build.sh`, `scripts/modlib.sh`, `tools/android-launcher/{fetch_nano.sh,ndk_probe.py}`, `patches/thimsy/0001-thimsy.patch`; the patched Nano tree (`NanoJson.cpp`, `NanoJson.h`, `NanoMenuScraper.cpp`, `NanoMenuXmb.cpp`) and the git history of the `nano/emulators.json` writer (b49719b, test 34) for open question 9
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

### 17.1 Second review, 2026-11-25

Six reviewers (code references, sibling plans, external facts, the card contract, feasibility, and the TODO), each finding checked by an adversarial pass. 57 findings on this plan survived it (53 once duplicates are merged), and all were applied; seven others on this plan were refuted and not applied. Findings on sibling plans are noted where this plan depends on them; those plans were not edited. What changed:

**Facts corrected**
- Four decision numbers were off by one (display stack is decision 4, real ES-DE decision 9, the H700 panel chooser decision 14) and the Mac is decision 15, not open question 7.
- Kernel versions in section 2 come from ROCKNIX `next`; the pinned tag `20261101` builds 7.2.11 on RK3566, RK3576 and S922X. H700 units can be LPDDR3. RK3576 also uses InputPlumber.
- The ARC-S and RG353VS/PS have GammaOS Core (Android 13 TV); only the H700 family has no Android at all.
- `distributions/THIMSY/` carries build-critical copies (`version`, `options`, `config/functions`, `kernel_options`), and the build needs `DISTRO=THIMSY`.
- ROCKNIX's automount is also started by a udev rule, and udevil mounts USB storage; all of it is removed or masked, not just the script.
- `device-keys.json` matches by system properties; vendor and product ids are `pads.json`'s and gammapad's. `NanoEsdeStore.cpp` uses settings and state paths. Nano's font list includes DroidSans and NotoColorEmoji, and the citation pointed at the DS player's code.
- The call counts are `ndk_probe.py`'s, as the launcher plan quotes them. The host test suite leaves out `DsScreenLayout_test.cpp`. The Android-only file list was completed.
- ROCKNIX documents GitHub-hosted builds; that replaces the guess that they can't be done. FlipBase's 1.3 s is a warm reboot (2.0 s cold). The alpha comes after about half the effort, not a third.
- `min_installer` already exists in format 1; format 2 is refused by its number.
- The licence audit does not cover Nano's GPL files yet.

**Design changed**
- The drive list is filtered per partition, so the one-card data partition is a drive.
- The share-or-sort rule for saves is decided per launch, from the cores that will actually run.
- Every path ROCKNIX builds under `/storage/roms` is redirected, with a test that only saves appear in the drive's `ROMs`.
- Mounts use `dirsync`; dirty-flag-only fsck is kept for boot time, with its reason stated and an open question.
- At boot the drive's 10 s grace waits for its first mount.
- Files under `<emu>/linux/` hold no absolute drive paths, so released builds' repair pass leaves them alone.
- A shipped Linux ES-DE gets no linked gamelists until the label rule is the floor, because released builds already write Android-only labels.
- A cloned volume id makes neither drive usable, as in `usb_storage_plan.md`.
- The installer zeroes the partition tables before the capacity probe, verifies every block the FAT edit wrote and the rest of the boot partition after it, and asks for typed confirmation on large disks or disks with other filesystems.
- The erase guide makes the backup the default, names a restore route that is tried before the guide is public, and warns about legacy-bootloader units.
- `face_layout` stays a label convention; button count and sticks are separate fields. `soc_family` gave way to the descriptor's key. The writer uses the reserved `sdcard` crate, and the new operations extend the `sdcard:` actions.
- Linux images offering only Small and Medium is stated as a departure; the platform selector `device_profiles_plan.md` would need is spelled out.

**Roadmap**
- A public alpha needs the source tarball and the ROCKNIX contact first (decision 16); the alpha checks memory at 1 GB.
- Serial-off and the U-Boot options moved into Phase 2; the snapshot frame moved after v1; the 8 s placeholder was dropped.
- Phase 3's exit covers rows 1 to 14 without the PSP save, which waits for PPSSPP at v1. The interop matrix gained rows 12 to 14.
- The estimate says what it includes from section 4.5 and that it is full-time effort. The build host is sized for two families.

**Added**
- Section 4.5 rows for `face_layout` (`controller_layout_plan.md`) and the SD-card writer. Decisions 16 and 17; open questions 17 to 19.

### 17.2 Base changed to Thimsy Base, 2026-11-25

The owner decided three things on 2026-11-25:

- **The base** (decision 1): a "Thimsy Base" in the FlipBase mould instead of a ROCKNIX downstream. Buildroot (an external tree), mainline kernel and U-Boot, BusyBox init with no systemd, no initramfs, an EROFS or squashfs root, one frontend (Nano). One build per SoC family, then a per-device packaging step; revisions that can't be detected are per-variant images or an installer question. Device fixes are ordinary source patches per SoC family, sent upstream where possible; ROCKNIX's GPL-2.0 device patches are imported rather than rewritten.
- **The boot target** (decision 5): 3 s cold, power-on to Nano's first complete home frame; 1.5 s as the owner's stretch goal, measured rather than promised.
- **The device** (open question 2): the owner has an RG ARC-D only, and wants Android kept on its eMMC with Thimsy booting from card slot 1. Erasing Android is the last resort.

Three researchers then read FlipBase and its RG353PS fork; the Buildroot, Batocera, Recalbox, Knulli and ROCKNIX package and patch trees; the ARC's mainline device trees and U-Boot board; and the no-systemd backends and display hand-offs of FlipBase, RetroStoneOS and MinUI. Their findings, with sources and confidence, are in this revision; anything they marked as a guess is marked *(guess)* here. What changed:

**Rewritten** (their premise was ROCKNIX)
- Sections 0, 1 and 2.4: the short answers, the scope and the redistributables, and what the device matrix means for a base of our own.
- Section 3: option (d) chosen; FlipBase as the template, and what it is not (multi-board, an upstream); what the `thimsy-base` repository contains and where each part comes from; licences for an MIT tree with a `gpl/` area, `make legal-info`, and which package sources may be copied (none of Batocera's or Recalbox's, and not FlipBase's BaseOS-derived files).
- Section 5.2: the backend for a BusyBox-init base: `wpa_supplicant`'s control socket, BlueZ with D-Bus only on demand, ALSA with `dmix` and bluealsa, the sysfs sleep sequence, uevent hotplug, an A/B updater, `inittab` and `rcS`.
- Section 5.3: no Sway. Nano is a KMS/GBM client that hands DRM master to each emulator; the Quick Menu comes in stages; every client rotates the ARC's portrait panel itself, since RK3566's VOP2 can't rotate a linear plane. The pad is the kernel's `gpio-keys`, so an evdev grab silences SDL emulators too, and the InputPlumber problem is gone.
- Sections 6.2 and 6.4: launching without `runemu.sh` or `setsettings.sh`; Nano writes RetroArch's per-launch config itself; emulator recipes are Thimsy's own.
- Section 8.6: the ARC-D's three dual-boot routes; the erase guide kept as the last resort.
- Sections 9 and 10: FlipBase's measured timeline and method; the techniques that were "not on this base" now apply; the 3 s target and the stretch goal; the BL31 choice; Buildroot sizes (about 30 GB for FlipBase's base, 40 to 70 GB with emulators *(guess)*) in place of ROCKNIX's 200 GB.
- Sections 11.1, 12 and 13: a Linux-only reference unit beside the ARC-D; the phases re-planned (Phase 2 now builds the base, then Nano on it); the estimate redone (alpha 22 to 36 weeks, v1 38 to 62, both guesses); the ROCKNIX-dependence risk replaced by owning every device, the erase risk by the dual-boot risk; a licence-mix risk added.

**Simplified or moot**
- `/storage/roms` and every ROCKNIX path under it (section 7.3; open question 19; decision 17's bind clause).
- ROCKNIX's automount, its udev rule and udevil: nothing to mask.
- The ROCKNIX platform-name column and the patched launch scripts.
- Downloading and repacking ROCKNIX's image (section 8.2's option B; decision 13's alternative).
- The device choice by editing a boot FAT: images are per device.
- Interop row 14 no longer needs a bind; its pass rule stands.

**Reopened, each with options and a recommendation**
- Decisions 2 (first device), 4 (display stack), 6 (repositories and licences), 9 (real ES-DE), 11 (GPU driver), 12 (package profiles, in part), 13 (image distribution: its alternative moot), 14 (H700 panel choice), 16 (public alpha: credit for imported patches replaces asking ROCKNIX), and 17 (one clause moot).

**Added**
- Decisions 18 (BL31) and 19 (how to take FlipBase); open questions 20 to 27; risk 12.
- Sources for FlipBase's tree, its RG353PS fork, Buildroot, the package trees, ROCKNIX's device patches and PR #154, JELOS's image layout, the ARC's mainline device trees and U-Boot board, VOP2 planes, DRM master and leases, Mesa's Panfrost and PanVK, and the backends.

**Unchanged:** the device matrix rows apart from the ARC pair's boot notes, the card contract (sections 7.1, 7.2 and 7.4 to 7.10), the Nano audit (section 5.1), the installer's writer (section 8.5) and the interop matrix's rows.

Not reviewed: this revision has not been through an adversarial review.

### 17.3 Four open questions answered, 2026-11-25

Researched and then checked by a second, adversarial pass; nothing here has yet been run on a device or a Mac:

- **Question 8** (vold and the one-card layout): vold mounts the exFAT fifth partition if it is typed Microsoft basic data at entry 7 or lower; the other entries get types vold ignores (section 8.2), or Android's format wizard can wipe the card (risk 13). Also found: the Cube's vold names volumes by a mount-order index, not the serial, so section 7's statements that the id is the serial were corrected and decision 20 added (risk 14, section 7.10). Device test kept for Phase 0.
- **Question 9** (`nano/emulators.json` and the other library files): unknown fields survive in `emulators.json` except on a game re-picked on Android; the other three files are rebuilt and drop them (sections 7.6, 7.10). Device check in Phase 3.
- **Question 16** (macOS disk classification): a card is `BusProtocol` "Secure Digital"; `RemovableMedia`, not `Internal`, separates it from the SSD; modern disk images report "Disk Image" and are hidden by that (section 8.5). Mac check in Phase 4.
- **Question 18** (dirty flags): vfat's flag is sound on both platforms; the Cube's vendor 5.4.233 exFAT backport is not, so exFAT is checked when flagged or not last cleanly unmounted here, or always if cheap; Remove Drive is a full umount, with no exFAT remounts on 7.3.5 unless 7d390ca06fe7 is backported (sections 7.3, 7.8, 9.3, 9.4). Device test in Phase 0.

### 17.4 Coverage design, 2026-11-25

**The owner's decision.** The broadest device support, initially ROCKNIX's coverage; devices that already boot Linux first; the Odin 2 not a focus. Asked how to reach that coverage, the owner chose "Thimsy Base + importer" (decision 21): one Linux flavour, a `thimsy-import` tool that tracks ROCKNIX's hardware layer, families in waves on the Thimsy Base with Thimsy's own fast boot. Earlier answers stand: the Thimsy Base, per-device cards, 3 s on RK3566, builds on this machine (open question 6), the Android build separate. Not chosen, one line each in section 3.1: hand imports per family; Nano as a ROCKNIX frontend package for the long tail, alone or first; back to a ROCKNIX downstream; ROCKNIX-built hardware packs; Batocera's board tree.

**How it was made.** Four researchers read ROCKNIX's device inventory (`config.xml`, options, release assets), how its hardware layer could be imported (`packages/linux`, the U-Boot recipes, the quirk census, Batocera's and Knulli's practice), display and rotation (mainline plane drivers, SDL, RetroArch, ROCKNIX's Sway and DPU patches), and input (InputPlumber, `rocknix-joypad`, the MCU drivers, SDL's GUID). A designer weighed five device-layer options and proposed the waves, the build shape, the boot targets and the testing. One adversarial critique checked the design against sources; where they conflict, the critique's corrections are applied.

**The critique's corrections, applied.**
- The importer's scope is narrowed: kernel patches, device trees, `config.xml` lists, firmware lists and facts; U-Boot and the boot chain stay Thimsy's (FlipBase's 2026.10 series holds the 3 s wins; ROCKNIX's RK3566 U-Boot is v2026.01). Facts are written fresh, not copied from quirk scripts or InputPlumber YAML, whose licence is unverified.
- The kernel rule is "ROCKNIX's version at a Thimsy-chosen tag, skipping tags freely", with the real bump rate stated (about 6 version changes a month across families, 5 reverts or rollbacks in six months), the unpatched .0 kernels raised (open question 34), RK3566's version left to Phase 2a, and a rule for when tags stop. The designer's "pin a `next` commit for RK3566 now" was dropped. At tag `20261101` five families differ from `next`, not one.
- The rotation contract is confirmed only for the Mali families. Mesa's display WSI does identity transforms only, so the Qualcomm families get a per-family display setting, gamescope is re-evaluated for Turnip, and ROCKNIX's Mesa blit patches are noted as not Wayland-specific. The "5 to 30%" figure (an estimate in PR #3377, not a measurement) and the bandwidth arithmetic are replaced by "to be measured".
- The claim that Adreno zap and DSP firmware must be built into the kernel is removed: `msm_open()` loads the GPU at first open, after the root is mounted. Firmware fetched from the device can live on a partition.
- The importer estimate is 4 to 8 weeks *(guess)*, built after a hand import of RK3566. The release-image check compares the pre-fragment config. ROCKNIX's RK3566 config leaves EROFS off and has `JOYSTICK_XPAD=m`, so Thimsy's fragment is large. Per-device quirk work is stated as per device, and the program total (about 60 to 100+ weeks *(guess)*) is stated.
- GitHub-hosted runners (4 CPUs, 16 GB RAM, 14 GB SSD) are an optional later path that needs chained jobs; the owner's answer to open question 6 (builds on this machine) stands, so the designer's "where builds run" decision was not added.
- Counts corrected: 17 release images over 14 families at `20261101` (no SM6450 or AMD64 image); ROCKNIX ships libmali beside Panfrost on 5 families. (The critique said 4, with S922X Panfrost only; the plan review of 2026-11-25 found S922X's `ADDITIONAL_PACKAGES="libmali"` and its `gpudriver` switch, so 5 stands.)

**Changed in this plan.** Sections 0 (base, coverage goal, waves, boot target, installer, effort, unknowns), 1 (scope, out of scope, firmware), 2 (generated matrix, the RK3566 version note, family and wave table, Qualcomm rows, pad and rotation facts), 3.1 (device-layer options), 3.2 (tree, importer, kernel rule, build steps, boot contracts), 3.3 (licences for OpenABL, `rocknix-joypad`, MCU drivers, generated facts, firmware, `.panel` files), 5.2 (rotation, pad, hotplug rows), 5.3 (rotation contract, Qualcomm display setting, kiosk fallback, dual screens, `thimsy-padd`), 8.2 to 8.6 (assembly, catalog status levels, variant questions, OpenABL step, writer layouts), 9.3 and 9.4 (config trimming; per-family targets), 9.5, 10 (sizing and CI), 11 (hardware-in-the-loop, testers, importer tests, Device check), 12 (Phase 2a hand import, Phase 7 as importer and waves, program total), 13 (risks 2, 3, 10 and 15 to 17), 14 (decision 1 amended; amendments proposed to 4, 11, 13 and 18; decision 21 decided; 22 to 26 new), 15 (questions 4, 22 and 27 reworded; 28 to 35 new) and 16.

Not reviewed: this revision has had only the design's own critique, not a separate adversarial review of the plan text.

### 17.5 Owner answers, 2026-11-25 (second batch)

Recorded as the owner gave them; nothing here has been run on a device. Open question 10 is still being researched and is not changed here.

**The SD-only principle** (new, owner): "Putting Linux on an SD card should always be reversible and non-destructive." Asked whether a bootloader may be written to internal storage with a backup, the owner first said "Never write internal storage", then refined it: "we can overwrite the boot loader later, for now let's focus on devices that can just boot off an SD card". Recorded:

- **Now:** the Linux install never writes internal storage; removing the card gives back the untouched device. Erasing Android is dropped permanently: the erase guide, the "last resort" rows and risk 6's erase parts are gone, and open questions 7 and 17 are moot.
- **Later, deferred and not excluded:** writing a bootloader to internal storage with a verified backup: ARC-D route (c), and OpenABL on the Qualcomm families. Wave 3 (the Qualcomm families, SM6450 included) moves to "later, needs bootloader writing"; its analysis is kept. Wave 4 does not wait for it.
- **The ARC-D:** routes (d), (a) and (b) only. If none passes, the ARC-D waits for bootloader writing and an ARC-S becomes the first device. The RG353P/M/V follow the ARC-D.
- Changed: sections 0, 1 (scope, out of scope, "What does not change"), 2.1, 2.3, 2.4, 3.3 (OpenABL), 8.3, 8.4, 8.6, 9.3 to 9.5, 11.1, 12, 13 (risk 6), 14 (decisions 2, 5, 16 and 26) and 15.

**Questions answered or moved:**

| Question | Answer, as recorded | Where |
|---|---|---|
| 11 (save states across builds) | "yes, but we need to test it": yes for same-commit 64-bit cores, confirmed per core (save on the Cube, load on the ARC and back, one game per system); only passing cores share states, later, after v1 | sections 6.2, 7.5, 12 (Phase 8) |
| 12 (Panfrost against libmali) | test both in Phase 0, ship the most stable driver first; both switchable until then. Decision 11 amended | decision 11; sections 3.2, 3.3, 12 (Phase 0), 13 (risk 9) |
| 13 (ARC serial) | the owner can wire it, but not now: a to-do for later; Phase 0 starts without serial | section 11.1; `TODO.md` |
| 14 (HDMI) | Docked in v1 (Nano and games on the TV, panel off; the emulator picks the HDMI connector); mirror later | sections 5.2, 5.4, 12 (v1) |
| 20, 21 (ARC-D route and loader time) | Phase 0 items with an order and a pass rule; nothing written to the eMMC | section 8.6; section 12 (Phase 0) |
| 22 (bare-diff authors) | "I don't care who wrote it. Use the ROCKNIX license." No author recovery; a provenance line per patch; upstreaming header-less patches out of scope; count corrected to about 160 of 580 (547 device, 33 shared) | sections 3.2, 3.3, 12, 13 (risk 12); decision 16 |
| 23 (package recipes) | recommended, and the owner did not object: our own recipes, ROCKNIX's `package.mk` ported into `gpl/`, Batocera reference only, Recalbox never; cores built from the commits of `retroarch-cores.lock`; non-commercial cores shipped as on Android | sections 3.2, 3.3, 4.2, 6.2 |
| 24 (BL31) | "use the FlipBase TF-A": decision 18 decided for RK3566 (the unit's own vendor BL31 with FlipBase's pm-config patch, never redistributed; rkbin's as the fallback, to confirm); other families as proposed. Question 24 now measures sleep current | sections 1, 3.3, 5.2, 9.5, 12; decision 18 |
| 26 (BaseOS files) | don't weigh the licence; files with no stated licence follow BaseOS's. Most are replaced anyway; the four C files are used as-is | sections 3.1, 3.2, 3.3, 9.3, 12, 13 (risk 12); decision 19 |
| 25 (the ARC on mainline) | not an owner answer: facts read from the mainline device trees (Wi-Fi on SDIO with the chip not named, no Bluetooth node, audio fully described, the panel and touch); the rest is a Phase 0 checklist | section 3.2; section 12 (Phase 0 (b)) |
| 27 (rotation) | not an owner answer: nothing to decide; covered by the rotation contract, tested per v1 emulator in Phases 0 and 2, with the per-game compositor fallback for a failure | open question 27 |

**New owner requirement: the storage job.** The universal installer writes microSD game cards and USB drives as a first-class job, with the same flow as the firmware install, for Android and Linux devices. All four parts: format and lay out; pack-ins for the chosen package profile; ROMs and BIOS from the PC, BIOS checked by hash; a non-destructive update or repair of an existing card. Section 8.4's "Prepare a game card" step is replaced by it; it is in Phase 4 and in `TODO.md`. The same requirement goes into `universal_installer.md` in the installer repo; that file is edited separately.

**Leftover corrections from the last verification:**

- Sections 7.3, 7.8, Phase 0 item (g) and open question 18 said "Thimsy Linux on 7.3.5"; they now say the Thimsy Base's kernels (each family's ROCKNIX version, decision 22; all newer than 6.16, which carries the exFAT fix), and that RK3588's vendor BSP 6.1 kernel needs the backport or is checked in wave 4. The "Remove Drive" rule says "all before 7.4".
- Decision 13's earlier "complete per-device images for v1" recommendation is marked superseded if the installer-assembly amendment is accepted.
- Open question 6 and decision 1 now follow decision 25's tier, GPU-pack and hardware-pack layout (as proposed), not "one build per family".
- Section 10: Buildroot builds host LLVM in each output tree, so the GPU packs are built in a tier tree that already builds it (or, to be proved, from a prebuilt SDK); the times are restated as guesses.
- Section 2.4: 11 of 12 H700 products (18 of 19 DTBs) are landscape; only the RG28XX is portrait.
- "31 Qualcomm products" is now "about 32 to 35, depending on how variants are counted".
- Section 2.3: the Odin 2 row is SM8550 only, so it no longer overlaps the "Other Qualcomm families" row.
- Section 3.2: ROCKNIX's `rocknix-joypad` patch-directory condition quotes its regex, which bash matches literally, so that directory is never added; the hand-port copies what ROCKNIX actually does.

Not reviewed: these edits have not been through an adversarial review.
