# Multi-device support: every GammaOS handheld

Date: 2026-11-24
Status: plan only, nothing implemented. Reviewed 2026-11-24 against the GammaOS repositories (release lists, release notes, wiki, OTA server) and both Thimsy repositories. Research done 2026-11-24 (GammaOS release pages, wikis, the GammaOSOTA server, GammaOSCoreVendor, AOSP compatibility rules, spec sites, and an audit of this repository and of `thimsy-os-installer`).
Builds on: `device_profiles_plan.md` (layers and profiles), `universal_installer.md` in the installer repo (device recipes, device packs, backends). This plan says what has to change so those plans can cover every device, and in what order.

Claims come from the research inputs listed in section 14. Anything not confirmed by a source is marked *(guess)* or *(unverified)*.

---

## 1. Goal and scope

Thimsy today is one build for one device: GammaOS Next v1.2 on the **Anbernic RG Cube** (Unisoc T820), optionally with a system image built from the GammaOS Next 1.4.4 AOSP tree (`SYSTEM_IMG`, Android 14 / LineageOS 21, `tdgsi_arm64_ab`). The goal is to support **every handheld GammaOS supports**, from one tree and one installer.

Rules the owner set:

- Where it is unclear, assume each device's Android drivers and firmware from its latest GammaOS release work with Android 14.
- Note devices where they are known, or likely, to be incompatible.
- The installer apps must support every device and every build profile seamlessly.

In scope: every device with a GammaOS **Android** release (GammaOS Next, GammaOS Nano, GammaOS Core). Out of scope: GammaOS Pico, which is not Android (section 3).

What does not change:

- Thimsy never redistributes vendor firmware. The installer fetches each device's official GammaOS release by hash and builds the Thimsy image on the user's machine (`universal_installer.md`).
- One Thimsy system image per release and profile. Device facts go into a per-device pack or are chosen at runtime (section 4).
- The RG Cube's output must stay byte-identical through the refactor.

## 2. Device matrix

49 entries (several entries cover more than one model). Verdicts:

- **already-android-14**: an Android 14 system runs on this device's vendor today: a GammaOS Android 14 / LOS21 build, or (RP Classic only) the stock Android 14 firmware.
- **likely-compatible**: no Android 14 build, but strong platform evidence.
- **assumed-compatible**: unclear; assumed to work under the owner's rule.

| Device | SoC (family) | GammaOS line, latest public release | Android: GammaOS / stock vendor | Format, flash tool | Android 14 verdict | Tier |
|---|---|---|---|---|---|---|
| **Anbernic RG Cube** | Unisoc T820 (ums9620) | Next, v.1.2.0-ANBERNICRGCUBE (2025-12-11) | 14 / 13 | Unisoc PAC, ResearchDownload or spd_dump; BROM unlock | already: Thimsy 1.4.4 system boots on the owner's unit | 0 (today) |
| Anbernic RG556 | T820 | Next, v.1.2.0-ANBERNICRG556 (2025-12-11) | 14 / 13 | PAC, BROM unlock | already | 1 |
| Anbernic RG406V | T820 | Next, v.1.2.0-ANBERNIC406V (2025-12-09) | 14 / 13 | PAC, BROM unlock | already | 1 |
| Anbernic RG406H | T820 | Next, v.1.2.0-ANBERNIC406H (2025-12-09) | 14 / 13 | PAC, BROM unlock | already | 1 |
| Anbernic RG476H | T820 | Next, v.1.2.0-ANBERNICRG476H (2025-12-09) | 14 / 13 | PAC, BROM unlock | already | 1 |
| Anbernic RG Slide | T820 | Next, v.1.2.0-ANBERNICRGSLIDE (2025-12-09) | 14 / 13 | PAC, BROM unlock; Back key is right of the screen | already | 1 |
| Anbernic RG Rotate | Unisoc T618 (ums512) | Next/Nano, v1.4.1-ANBERNICRGROTATE (2026-08-13) | 14 / 12 | PAC, UpgradeDownload or spd_dump; no unlock; custom SPL | already; its vendor is the closest to Thimsy's 1.4.4 system | 2 |
| Anbernic RG Vita | T618 (the release notes say T820; see §13 Q17) | Next, v1.3.1-ANBERNICRGVITA (2026-04-27) | 14 / 12 | PAC, UpgradeDownload; no unlock | already | 2 |
| Anbernic RG405M | T618 | Next, v.1.1.0-ANBERNICT618 (2025-11-23) | 14 / 12 | fastboot images + scripts; Unisoc fastboot unlock | already | 3 |
| Anbernic RG405V | T618 | Next, v.1.1.0-ANBERNICT618 | 14 / 12 | fastboot; Unisoc unlock | already (kernel 5.4.161 verified by a teardown) | 3 |
| Anbernic RG505 | T618 | Next, v.1.1.0-ANBERNICT618 | 14 / 12 | fastboot; Unisoc unlock | already | 3 |
| KTPocket KT-R1 | MediaTek Helio G99 (MT6789) | Next, v.1.0.0-KTR1 (2025-06-29) | **13** / 12 | fastboot after `fastboot flashing unlock` | likely: FCM 6 and 5.10 kernel fit Android 14; **KeyMint/TEE risk** | 3 |
| ZPG A1 Unicorn | Helio G99 | Next, v.1.0.0-ZPGA1UNICORN (2025-06-29) | **13** / 13 | fastboot after unlock | likely; same KeyMint risk | 3 |
| AYANEO Pocket Micro | Helio G99 | Next, v.1.0.0-AYANEOPOCKETMICRO (2025-07-15) | **13** / 13 | fastboot after unlock | likely; same KeyMint risk | 3 |
| Retroid Pocket 4 Pro | MediaTek Dimensity 1100 | Next, v.1.0.0-RETROIDPOCKET4PRO (2025-07-14) | **13** / 13 | fastboot after unlock | assumed: no A14 build or GSI report; 4.19 kernel *(guess)* is accepted by Android 14 | 3 |
| Retroid Pocket Classic | Qualcomm Snapdragon G1 Gen 2 | Next, v.1.0.0-RETROIDPOCKETCLASSIC (2025-07-14) | **13** / 14 (most sources; Retroid's own `Android14_RPClassic` stock image) | fastboot (`FlashPartitions` scripts); the wiki has no unlock step | already: stock is Android 14, but no GammaOS Android 14 build. **Units made from March 2026 boot loop** (hardware) | 3 |
| Retroid Pocket Duo Lite | Qualcomm QCS6125 | Next 1.5.0 (Lite only, boots into Nano), **private beta** only; the OTA server already has a `retroidpocketduolite` endpoint | 14 *(inferred)* / **15** | fastbootd scripts; no unlock | likely, but outside Treble's guarantee (older system on a newer vendor) | 5 |
| Anbernic RG DS | Rockchip RK3568 (RK3566 in GammaOS's tree) | Nano: latest public **v1.4.1-ANBERNICRGDS** (2026-08-13); v1.4.4 (2026-10-03) is supporter-only | 14 / 14 | v1.4.1: raw self-flashing SD image written with any imaging tool (dd); v1.4.4: RKFW update card that only rockchip_sd_tool writes. Both flash internal storage. OTA | already | 4 |
| Anbernic RG DS Plus | RK3568 | Nano Core build only. **No public download**: v1.4.2's files were removed, v1.4.3 and v1.4.4 are supporter-only | 14 / none (ships Linux) | RKFW SD boot card (runs from the card) | already, but **1 GB RAM**; whole BSP is GammaOS-built | 5 |
| Anbernic RG Vita Pro | Rockchip RK3576 | Next, v1.3.0-ANBERNICRGVITAPRO (2026-04-23), OTA 1.3.2 | 14 / 14 | raw SD self-install image (dd) | already | 4 |
| GKD 350H Ultra | RK3576S | Nano, v1.4.1-GKD350HULTRA (2026-08-13) | 14 / none (stock is Linux) | RKFW SD boot card; runs from SD | already; vendor built from Radxa ROCK 4D BSP | 4 |
| TrimUI Brick | Allwinner A133P | Nano, v1.4.1-TRIMUIBRICK (2026-08-13) | 14 / 10 vendor | PhoenixCard SD boot card | already; FCM 4 and kernel 4.9 are formally outside Android 14's kernel matrix but boot. **1 GB RAM** | 5 |
| MagicX Mini Zero 28 | A133P | Nano, v1.4.1-MAGICXMINIZERO28 | 14 / 10 | PhoenixCard SD | already (same caveat) | 4 |
| MagicX Zero 40 | A133P | Nano, v1.4.1-MAGICXZERO40 | 14 / 10 | PhoenixCard SD | already (same caveat) | 4 |
| MagicX XU Retro XU20 V32 | A133P | Nano, v1.4.1-MAGICXXURETROX20V32 | 14 / 10 | PhoenixCard SD | already (same caveat) | 4 |
| TrimUI Smart Pro | A133P | Core, beta3_a133p (2025-02-17) only | 13 TV / none (Linux) | PhoenixCard / raw SD | assumed: same SoC and vendor generation run A14 elsewhere; no TSP vendor branch | 5 |
| AYANEO Pocket Air Mini | MediaTek Helio G90T (MT6785) | Next, v1.3.2 (tag v1.3.1-AYANEOPOCKETAIRMINI), OTA 1.3.2 | 14 / 11 | SP Flash Tool v5 + scatter; upstream's clean install uses **Format All + Download**, its upgrade keeps data | already; serves the same universal 1.3.2 OTA | 4 |
| Anbernic RG557 | MediaTek Dimensity 8300 (MT6897) | Next, v.1.2.1-ANBERNICRG557 (2025-12-23) | 14 / 14 | SP Flash Tool V6: manual format of UFS LUA0 and LUA1 only, then flash_128GB/256GB.xml | already. **Units on stock v1.26+ are unsupported** (hardware revision) | 4 |
| Anbernic RG477M | Dimensity 8300 | Next, v.1.2.1-ANBERNICRG477M | 14 / 14 | SP Flash V6 | already | 4 |
| Anbernic RG477V | Dimensity 8300 | Next, v.1.2.1-ANBERNICRG477V | 14 / 14 | SP Flash V6 | already | 4 |
| MANGMI Air X | Qualcomm Snapdragon 662 (SM6115; `device-keys.json` wrongly says 680) | Next, v.1.2.0-MANGMIAIRX (2025-12-14) | 14 / 14 | QFIL flat build in EDL (`rawprogram_unsparse0.xml`; `_upgrade.xml` keeps data); separate MQ65 and MQ66 packages | already | 4 |
| GameMT E6 Max | Allwinner A527 | Next, v.1.1.0-GAMEMTE6MAX (2025-12-01); upstream marks the Full build NOT_RECOMMENDED (Google services disable Wi-Fi) | 14 / 13 | PhoenixSuit IMAGEWTY after `fastboot oem unlock` (Windows only) | already, **partial incompatibility: MTP crashes** (FUSE vs legacy kernel, no source) | 4 |
| Anbernic RG353P / RG353V / RG353M | Rockchip RK3566 | Core, beta3 (2025-02-07); also legacy GammaOS v1.5.1 (§2.2) | 13 TV (Core), 12 (legacy) / 11 | Core: RKFW, SDDiskTool SD boot or RKDevTool eMMC. Legacy: RKFW SD update, or fastbootd images on the stock vendor | likely: an Android 12 GSI (legacy GammaOS) already runs on the stock Android 11 vendor; 4.19 kernel and FCM 5/6 accepted; nothing has shipped on A14. 2 GB | 5 |
| Anbernic RG353VS / RG353PS | RK3566 | Core, beta3 | 13 TV / none (Linux) | RKFW SD | assumed; 1 GB, no Android vendor of their own | 5 |
| Anbernic RG ARC-D | RK3566 | Core, beta3; also legacy GammaOS v1.5.1 | 13 TV, 12 (legacy) / 11 | RKFW SD or eMMC; legacy fastbootd | likely (same evidence as the RG353P/V/M). 2 GB | 5 |
| Anbernic RG ARC-S | RK3566 | Core, beta3 | 13 TV / none (Linux) | RKFW SD or eMMC | assumed; 1 GB | 5 |
| Anbernic RG503 | RK3566 | Core, beta3 | 13 TV / none (Linux) | RKFW SD or eMMC | assumed; 1 GB | 5 |
| Powkiddy RGB30 / RGB20SX | RK3566 | Core, beta3 | 13 TV | RKFW SD; RGB30 v2 needs its own boot.img | assumed; 1 GB | 5 |
| Powkiddy RGB10 Max 3 | RK3566 | Core, beta3 | 13 TV | RKFW SD | assumed; 1 GB | 5 |
| Powkiddy RGB20 Pro | RK3566 | Core, beta3 | 13 TV | RKFW SD | assumed; 1 GB at 1024x768 | 5 |
| Powkiddy X55 | RK3566 | Core, beta3 | 13 TV | RKFW SD | assumed; 2 GB | 5 |
| Powkiddy X35H / X35S | RK3566 | Core, beta3 | 13 TV | RKFW SD | assumed; 1 GB | 5 |
| GameMT E5 Plus | RK3566 | Core, beta3 | 13 TV | RKFW eMMC (RKDevTool) | assumed | 5 |
| GameMT E6 Plus | RK3566 | Core, beta3 | 13 TV | RKFW eMMC | assumed | 5 |
| CB408 | RK3566 | Core, beta3 | 13 TV | RKFW eMMC | assumed; no spec sheet found | 5 |
| GKD Bubble | RK3566 | Core, beta3 | 13 TV | RKFW SD, after an SPI NOR bootloader flash | assumed; BT and OTG unsupported in Core | 5 |
| Miyoo Flip | RK3566 | Core, beta3 (Alternative image) | 13 TV | RKFW SD with GammaLoader | assumed; no 5 GHz Wi-Fi | 5 |
| Konkr Pocket Advance | MediaTek MT6785 | **Pico** v1.1.2 (not Android) | stock 12 | LK image via SP Flash or mtkclient | out of scope (no GammaOS Android build) | excluded |
| Konkr Pocket Block | MediaTek MT8786 | **Pico** v1.1.2 (not Android) | stock 12 | LK image | out of scope | excluded |

Counts (the 47 entries in scope; the two Konkr entries are excluded and get no verdict): 26 already-android-14 (one of them, the RP Classic, through its stock firmware), 6 likely-compatible (3 Helio G99, RP Duo Lite, RG353P/V/M, RG ARC-D), 15 assumed-compatible (TrimUI Smart Pro, RP4 Pro, and 13 RK3566 entries).

Tiers are the order of the roadmap (section 10): 1 same flash route as the Cube, 2 same tool with a new SoC, 3 fastboot backend, 4 SD, EDL, SP Flash and PhoenixSuit backends, 5 deferred (the TrimUI Brick is tier 5 for its 1 GB of RAM, until decision 9).

"Latest public release" matters: a supporter-only release has no public password, so the installer can't fetch it (decision 16).

### 2.2 Older GammaOS lines (LineageOS 19.1, Android 12)

Three older GammaOS repositories also ship Android builds for devices in the table. They are superseded and are not bases for Thimsy, but they are evidence and installation history:

- `TheGammaSqueeze/GammaOS` v1.5.1 (2024-01-08): RG405M, RG405V, RG505. GammaOS Next for T618 is a fresh install over it or over stock.
- `TheGammaSqueeze/GammaOS-RK3566` v1.5.1 (2024-01-28): RG353P/V/M and RG ARC-D. An Android 12 GSI (Andy Yan's LineageOS 19.1) on the stock Android 11 vendor, installed by RKFW SD update or by fastbootd. It shows that the RK3566 Android vendor is Treble-compliant with dynamic partitions, and gives these models a fastboot route that the Core (RKFW) images don't have.
- `TheGammaSqueeze/GammaOS-MTK` v1.5.1 (2024-01-13): KT-R1.

### 2.1 Known or likely incompatibilities

1. **GameMT E6 Max: MTP.** File transfer over MTP crashes or reboots the device. GammaOS says it is a FUSE incompatibility between modern Android and GameMT's legacy kernel, and GameMT publishes no kernel source, so it can't be fixed. Workaround: ADB transfer. Thimsy must not make MTP the default USB mode here (`mods/25-usb-mtp.sh`).
2. **Retroid Pocket Classic made from March 2026.** These units boot loop on the Retroid logo with the GammaOS package. This is a hardware revision, not Android 14. Rev Q and the Classic 6 share board and SoC IDs, so an electronic check may be impossible; the installer must block or ask for the manufacture date.
3. **RG557 on stock v1.26+.** GammaOS says do not install on units that ship with v1.26 (hardware changes). The 8 GB / 128 GB SKU from May 2026 probably falls under this *(guess)*.
4. **Helio G99 (KT-R1, ZPG A1, Pocket Micro): KeyMint/TEE OS-version lock.** On some MT6789 TrustKernel TEEs, Android 14+ GSIs hang at boot because KeyMint refuses a changed OS version, so `/data` never mounts (TrebleDroid UMIDIGI G7 Tab Pro page). The fix is to report Android 13 to the TEE. Which TEE these three use is unknown.
5. **RP Duo Lite: Android 14 system on an Android 15 vendor.** Treble guarantees the reverse, not this: Android 15 vendors target FCM 202404 and drop the VNDK APEX that an Android 14 system expects. It works in GammaOS 1.5.0 beta only through changes Thimsy's 1.4.4 tree may not have.
6. **A133P devices (Brick, MagicX, TSP): end of the line.** Their Android 10 vendors (FCM 4, kernel 4.9.170) boot Android 14, but Android 14's level-4 matrix formally lists only kernels 4.14 and 4.19, and Android 15 drops `compatibility_matrix.4.xml`. Anything that runs a strict VINTF check (update_engine OTA, `checkvintf`) will fail. Vulkan is 1.0 (PowerVR GE8300).
7. **RK3566 (GammaOS Core devices): unproven.** No Android 14 system has booted on any RK3566 vendor that we know of (an Android 12 GSI has: §2.2). GammaOS's changelog mentions in-progress "GCore+ initial A14 patches for older-SoC devices", which we could not find. Most units have 1 GB RAM, at or below the Android 14 CDD floor once carve-outs are counted. A phh wiki report says LineageOS 21 failed on a (MediaTek) 4.19 tablet.
8. **Kernel 5.4 on the T820 family.** AOSP says 5.4 is not supported from Android 14 on; it works because the vendor is frozen. It blocks a later move to Android 15 or 16. The T618's verified 5.4.161 is also below Android 15's 5.4.186 floor.
9. **1 GB devices** (RG DS Plus, TrimUI Brick, TSP, most RK3566 units): memory, not drivers. A full Android 14 system with Thimsy's apps may not be usable.

## 3. Devices excluded or deferred

| Device | Decision | Why |
|---|---|---|
| Konkr Pocket Advance, Konkr Pocket Block | **Excluded** | GammaOS Pico is a custom LK bootloader emulator, not Android. GammaOS ships no Android vendor or drivers for them. A Thimsy port would be a from-scratch port on Ayaneo's stock vendor. Pico also writes `lk_a`/`lk_b`, so a Thimsy install would remove it and the reverse. Point users to GammaOS Pico. |
| RK3566 GammaOS Core devices (15 entries, about 20 models) | **Deferred** to tier 5 | No Android 14 base exists; Core is Android 13 TV, frozen since February 2025. Thimsy would own every vendor fix. Start with a test boot on an RG353V or ARC-D (2 GB, eMMC, stock Android 11 vendor that already runs a GSI), flashed by fastbootd as legacy GammaOS did. |
| TrimUI Smart Pro | Deferred | Only GammaOS Core beta3 (Android 13 TV); no vendor branch in GammaOSCoreVendor. Thimsy would have to build or port a vendor. |
| RG DS Plus | Deferred | 1 GB RAM; upstream ships only a "Core" build (GammaOS Nano without Google apps, not GammaOS Core), and no public download exists. `device_profiles_plan.md` §4 already says: not a target until a low-RAM runtime variant is qualified. |
| TrimUI Brick | Deferred with the other 1 GB devices (decision 9) | 1 GB RAM, the same reason as the RG DS Plus. The three 2 GB A133P devices stay in phase 5. |
| Retroid Pocket Duo Lite | Deferred until GammaOS 1.5.0 is public | Private beta only; its system-side fixes for the Android 15 vendor are unknown. |
| Planned upstream, no release: AYANEO Pocket S, Pocket DMG, Pocket AIR, Retroid Pocket Mini v1/v2, Flip 2, Retroid Pocket 5, Powkiddy X28. Also named in Thimsy's `device-keys.json` or `HOTKEYS.md` with no GammaOS release: Retroid Pocket Duo (`retroidpocketduo`, keys from upstream's `RetroidPad.c`), AYN Thor | Not planned | Nothing to build on. Add each when GammaOS publishes a release. |

## 4. Target architecture

### 4.1 One shared system image

One Thimsy system image (per release, profile and runtime variant) can serve every device in tiers 1-4. The evidence:

- GammaOS already works this way. Its v1.3.2-OTA notes call the OTAs "universal system-only OTA packages, compatible with all GammaOS Next devices running 1.3.x+", and the Pocket Air Mini endpoint serves the same file (1,955,426,649 bytes). The RG Vita Pro's 1.3.2 OTA differs from it by 902 bytes *(inferred from sizes; zips not opened)*.
- Every device is arm64 with 64-bit binder, Treble, system-as-root and dynamic partitions, which is what `tdgsi_arm64_ab` targets.
- The 1.4.4 tree Thimsy patches already carries other devices' code: RG Rotate (slide sensor, square-as-landscape), RG DS (rk817 chime, dual-DSI follower display), MediaTek (HWC overlay workaround, an MT6789 chime comment), A133-class paths in Nano.
- `mods/device-keys.json` already has 20 entries, matched at runtime, covering about 30 GammaOS models (plus the unreleased RP Duo).

Two caveats:

1. **Version skew.** Thimsy's system is 1.4.4-based. Most vendors were paired with an older GammaOS system: 1.0 (Android 13) on the Helio G99, RP4 Pro and RP Classic; 1.1 on the T618 fastboot devices and the E6 Max; 1.2 on the T820 and Dimensity 8300; 1.3 on the RG Vita, Vita Pro and Pocket Air Mini. Device fixes GammaOS put in its system image must still be there in Thimsy's (keymaster lock-screen fix and logd deadlock fix on the Pocket Air Mini, fan daemon, GammaPad profiles, Nano DRM backend on Rockchip, the RG DS chime that Thimsy's `NanoBootChime` rewrite removed).
2. **Runtime variants.** Thimsy's patch flipped upstream's `PRODUCT_ENABLE_UFFD_GC := false` to `true` (in `lineage_arm64_bgN.mk`) because the Cube's kernel has userfaultfd. The 4.x kernels (A133P 4.9, Pocket Air Mini 4.14 *(guess)*, RK3566 4.19, RP4 Pro 4.19 *(guess)*) probably lack it. Upstream's removed comment says that on such kernels the collector mismatch makes zygote regenerate the boot image with dex2oat **on every cold boot** (16 to 20 s), not once; Thimsy's own measurement (about 23 s) is a first-boot figure from the Cube, where the mismatch runs the other way. Measure on one 4.x device before choosing between one image and a second system variant (decision 8).

### 4.2 Device descriptor

Every device gets one descriptor. It is the single source for the build, the installer catalog and `device-keys.json`. Today they disagree even on ids (`anbernic-rg-cube` in the catalog, `anbernic-rgcube` in device-keys), and the catalog has no entry for the RP Duo Lite, TrimUI Smart Pro or any RK3566 device.

Fields (from the build audit):

- **Identity:** id, names, match properties (`ro.product.vendor.model`, `ro.gammaos.device`, `ro.board.platform`), hardware variants (storage size, MQ65/MQ66, overclock or not (`NO_OC` on the Vita Pro, `NO_OC_UV` on the RG DS), Bluetooth model, RGB30 v1/v2).
- **Hardware:** SoC family, GPU family (mali-bifrost, mali-valhall, adreno, powervr), kernel version, vendor API and FCM level, RAM, ARMv8.2 or not.
- **Display:** width, height, panel rotation, natural orientation, number of screens, refresh rates, video out (none, DP alt mode, HDMI).
- **Base:** release tag, parts, sha256, archive password, inner file, line (Next, Nano, Core), upstream edition used as the base (Full, Lite or Core; for example Lite only on the E6 Max, Core only on Nano SD devices), and whether the release is public.
- **Container and partitions:** container backend, super image name, sparse or raw, slot suffix or none, which of vendor_boot, init_boot, vendor_dlkm, odm, dtbo and logo exist, AVB mode (testkey, verification disabled, locked).
- **Vendor steps:** an ordered list of optional steps with parameters (section 5).
- **Controls:** built-in pad VID:PID, face layout (Nintendo or Xbox), keylayout, `pads.json` id.
- **Features:** dp_out, typec_role_switch, fan, rgb, slide, dual_screen, cutout, charger_waits_battery.
- **Props:** Quick Settings tiles, defaults.
- **Profiles:** recommended and allowed profiles.

Shared defaults live in a SoC-family file (unisoc-ums9620, unisoc-ums512, mtk-mt6789, rk3568 and so on), so a device file holds only its overrides. This is `device_profiles_plan.md`'s layer model with the device layer filled in.

### 4.3 Where device facts go

The system image stays identical on every device. Device facts reach the device in one of two ways:

- **(a) The device pack:** odm (or vendor) overlays with per-device `build.prop`, keylayouts and init `.rc` fragments, written by the build's vendor stage (`universal_installer.md` 3.2).
- **(b) Runtime selection:** one early property, `ro.thimsy.device`, set by matching device-keys.json, plus first-boot setup steps that read the device's face layout and features.

Rules:

- Nothing device-specific is written into the shared system or product `build.prop`. `product` is parsed after `vendor` and `odm`, so a value there would override the device pack.
- Devices with no odm partition (4 of the 5 A133P devices, maybe others) need either a vendor rebuild or route (b) (decision 3).

### 4.4 What the RG Cube-only work becomes

| Today | Becomes |
|---|---|
| `scripts/patch_dtbo.py` (USB-C dock fix) | A `dtbo_dock_fix` vendor step, off by default, with one entry per device (stock sha, entry index, board id, approved sha). `tested` checks only the device being built. The node edits are Unisoc-PMIC rules; other SoCs get none. |
| `scripts/sprd_drm.py` (DP modes, hotplug, bandwidth) | A `sprd_drm` step registered by (SoC, module sha256). The other T820 models need their own re-derived patches; a device with an unknown module hash skips the step with a message. T618 has no wired video out; nothing to port. |
| `scripts/patch_vendor_dlkm.py`, `patch_vendor_boot.py` | Generic repack libraries plus per-device edit lists (module order, touch-driver order, deferred modules). Skipped where there is no vendor_dlkm or vendor_boot. Partition size read from the layout, not a constant. |
| `NanoTypec` power-role switching | Runs only where `typec_role_switch` is set. It was tested only on the Cube's tcpm. On RK3576 writing `port_type` can hang the SoC (GammaOS note). DP detection becomes a per-SoC list. |
| Monitor Mode limits (`GammaosScreenArrangement`, `NanoMenuMonitorMode`) | Limits (max width, pixel clock, UHD, auto mode) from per-SoC properties; hidden where `dp_out` is false. |
| `bootlogo.py` (Unisoc GZ logo, 720x720, quarter turn) | A per-format logo backend with size and rotation from the descriptor (section 5). |
| The usb_c_dock_plan and display_modes_plan | RG Cube feature packs. They extend to another T820 model only after that model has its own measurements (dock plan D8 already says so). |

### 4.5 Layers for content

As in `device_profiles_plan.md` §2. The GPU-family layer matters most for multi-device:

- **Mali Bifrost/Valhall:** PanVK and MaliSX2. PanVK must be packaged as `vulkan.<platform>.so` per SoC (today `vulkan.ums9620.so` only), and each kbase version needs testing. MaliSX2 likely does not apply to Bifrost *(guess)*.
- **Adreno** (Air X, RP Classic, RP Duo Lite): Turnip through adrenotools for the emulators that load custom drivers; no PanVK or MaliSX2.
- **PowerVR GE8300** (A133P): Vulkan 1.0 only; GLES everywhere; no custom drivers.
- **Mali-G615 CSF** (Dimensity 8300): PanVK support on kbase unverified; hide it until tested.

## 5. Build changes (thimsy-os-nano)

From the build audit. File by file:

| File | Today | Change |
|---|---|---|
| `scripts/build.sh` | Linear, RG Cube only: `NAME=RG_Cube_GammaOS_Next_Full_v1.2`, the 7z password, PAC entry names, `system_a`/`vendor_dlkm_a`/`odm_a`/`vendor_a`, `720 720` logo, `setinfo ... RG_Cube_Thimsy_OS`, every vendor step always run, DP tools always required | Require `DEVICE=<id>` (optional `PROFILE=`). Load the descriptor. Split into stages: unpack (container backend), super, system mods, device vendor steps, logo, repack. Check a tool only when its step is enabled. Output `Thimsy_OS_v<ver>-<device>.<ext>`. First milestone: the Cube output byte-identical. |
| `scripts/build.sh`: two system sources | Without `SYSTEM_IMG` (Docker builds, version 1.0) it modifies the release's own 1.2 system; with it (1.0a) it drops in the 1.4.4 system. Mods branch on this (`37-esde.sh` refuses 1.4.4, `THIMSY_DP_ALL_MODES`, `--load-leds`) | The release-system mode stays RG Cube only: other devices' own systems run from GammaOS 1.0 (Android 13) to 1.4.x Nano, and the mods were only tested on 1.2. Every other device builds from `SYSTEM_IMG` (decision 15). |
| `scripts/build.sh`: system sizing | `lptool grow system_a max`, then `resize2fs`, so the ext4 system fills the device's free super space; `zero_free_blocks.py` and `check_image_files.py` run on that per-device image | The mods run once on a minimum-size shared system; growing it to the device's super becomes a per-device step (build, and installer for format 2, §7.4). |
| `scripts/fetch.sh` | Only the RG Cube 1.2 tag and hashes | Read base release data from the descriptor. Support single files, zip, and importing a local file that matches a catalog hash (Patreon-first releases). |
| `scripts/pactool.py` | Unisoc PAC reader/writer | Keep as the `unisoc-pac` container backend (T820 and T618 PACs). Add backends: fastboot image folder (with sparse super), MTK scatter / flash XML, QFIL rawprogram, Rockchip RKFW/RKAF (a new `scripts/rkfw.py`, using the MIT `rockchip_sd_tool` `rkfw.rs` as the reference; `rk_image_tool` and `ums512_spl` publish no licence, so they can't be bundled or copied), Rockchip raw SD install image (RG DS up to v1.4.1, RG Vita Pro), Allwinner IMAGEWTY (`scripts/imagewty.py`, per-item `V*.fex` checksums). |
| `scripts/lptool.py` | Raw super only; slot-suffixed names | Sparse super in and out. Names and slot suffix from the descriptor (non-A/B Rockchip and Allwinner use `system`). Report when a group has no headroom. |
| `scripts/patch_vendor_dlkm.py` | Cube module names, exits if one is missing | Optional step with per-device rules; skipped where there is no vendor_dlkm (pre-GKI kernels). |
| `scripts/patch_vendor_boot.py` | Cube edits, `PARTITION_SIZE = 104857600` | Generic repack library plus per-device edits; size from the layout; refuse where the chain is not testkey-signed. |
| `scripts/sprd_drm.py` | One `MODULE_SHA256`, fixed offsets | Patch sets keyed by (SoC, module sha256); unknown hash skips. |
| `scripts/patch_dtbo.py` | One device in `DEVICES`; `tested` requires every entry approved | Per-device entry in the descriptor; `tested` checks only the target. |
| `scripts/avb_footer.py` | Assumes AOSP testkey | Read the descriptor's `avb` mode before re-signing. |
| `scripts/odm_sepolicy.py` | Writes the Cube's odm precompiled policy | Target from the descriptor (odm or vendor), EROFS and ext4, or skip. |
| `scripts/bootlogo.py` | Unisoc GZ format, square 720x720, quarter turn | Backends: unisoc-gz, MTK `logo.bin`, Rockchip RSCE resource (`logo.bmp`, `logo_kernel.bmp`), Allwinner boot-resource BMP *(guess)*, Qualcomm splash, none. Size and rotation from the descriptor; non-square frames. |
| `scripts/check_charger_rc.py` | Allows only `RGCUBE` condition | Accept any descriptor's gating property; enforce only where `charger_waits_battery`. |
| `scripts/check_charger_binary.py` | Requires the charger's battery rescan (a Cube vendor-module ordering problem); reads an ext4 system with `debugfs` | Keep (system-wide, harmless elsewhere); it is a system check, so it runs once on the shared system. |
| `scripts/check_buttonmap_assets.py` | Checks the `rg_cube` pad | Also check the target device's pad entry and drawing. |
| `scripts/build_panvk_driver.sh` | `PLATFORM=ums9620` | One `vulkan.<platform>.so` per Mali platform, or per-platform APKs. |
| `mods/48-panvk.sh` | `ro.gfx.driver.0` in the shared system build.prop | GPU-family layer; property only on Mali devices (device pack or runtime gate); `requires=gpu:mali` in `added-apps.txt`. |
| `mods/45-armsx2-malisx2.sh` | MaliSX2 for every device | Mali only; Turnip defaults on Adreno; nothing on PowerVR. |
| `mods/27-controller-layout.sh`, `mods/keylayout/Vendor_484b_Product_1101.kl` | Cube keylayout in `/product` for every device | Per-device keylayouts in the device pack; face-bind swaps applied by face layout at first boot. Check each Anbernic model's vendor `.kl` before sharing one file. |
| `mods/10-update-emulators.sh` | RetroArch bind swap for the Cube pad; installs `rg_cube_*` shaders | Swap by face layout; shaders from the display layer. |
| `mods/retroarch-shaders/rg_cube_*` | Named for the Cube | Rename (for example `thimsy_4_3_cropped`) with the old names kept as aliases; their maths is already size-generic. |
| `mods/50-nano.sh` | `no_slide=1`, RGB, performance mode and the Cube's Quick Settings tiles in system/product build.prop | Keep generic Nano defaults; move the rest to device and chipset layers. With no device entry, leave the vendor's tile list alone. |
| `mods/12-trim-overlays.sh` | Removes cutout overlays ("the RG Cube has no cutout") | Device option, default on. |
| `mods/58-bootanimation.sh` | One 480x480 animation | Optional per-panel variants (low priority). |
| `mods/25-usb-mtp.sh` | MTP default everywhere | Off on the E6 Max. |
| `mods/device-keys.json` | 20 entries, matched at runtime | Generated from the descriptors; add face layout, pad VID:PID, `pads.json` id, feature flags. |
| `mods/retroarch-cores.txt`, `mods/15-update-apps.sh` | T820-tuned comments; Magisk 30.6 pinned for the Cube's boot | `profiles=`/`requires=` columns; Magisk version per device descriptor. |
| `tests/` fixtures and unit/e2e tests | Cube-shaped (PAC, dtbo entry 2, 720x720 logo, RGCUBE) | Parametrise by descriptor. Add a second synthetic device (sparse super, no vendor_dlkm, vendor_boot, dtbo or PAC) to prove that steps skip. Add a descriptor schema lint. Keep a Cube golden test. |

New build-side work per family, beyond the table:

- **MediaTek SP Flash devices:** unsparse `super_full.img`/`super_lite.img`, replace system, re-sparse; never touch `preloader_a/b`; the thermal_safety_protection Magisk module folded into the device pack (Dimensity 8300). The RG557 family's install formats UFS LUA0 and LUA1 by hand first; the Pocket Air Mini's clean install is "Format All + Download" upstream (decision 17).
- **Rockchip:** RKFW unpack and repack, MD5 recomputed; RG DS (standard or `NO_OC_UV`) and Vita Pro (standard or `NO_OC`) clock variants; the Vita Pro's update card is FAT32, so `sdupdate.img` must stay under 4 GiB.
- **Allwinner A133P:** IMAGEWTY unpack and repack; keep `boot0` and `boot_package` byte-identical; device pack in vendor where there is no odm.
- **Qualcomm Air X:** two builds (MQ65, MQ66) sharing one system; never write `persist`.
- **Helio G99:** reuse GammaOS's exact vbmeta images (disabling verification can cause "data is corrupt" boot loops on MT6789).

## 6. Nano and framework changes (`patches/thimsy/0001-thimsy.patch`)

1. **`ro.thimsy.device`.** Set early from the device-keys match; every device gate below reads it.
2. **init.rc.** The `RGCUBE` blocks (`rgcube-display-wait`, `rgcube-leds-load`, the charger battery wait) move to per-device `.rc` fragments, in the device pack or gated on `ro.thimsy.device`. Remove the temporary `rgcube-chglog`. Make the display-wait node list and composer service name parameters.
3. **Built-in pad.** gammapad (`GamepadManager.cpp`) and `ThimsyDeviceKeys` hard-code 484b:1101 as the built-in, Nintendo-layout pad. Read VID:PID and face layout from the device entry. Retroid, AYANEO (Xbox layout), Mangmi, GameMT, KT, ZPG and the Nano-line pads are not 484b:1101.
4. **`pads.json` and controller drawings.** One entry per device, chosen by device id, not "first entry is the built-in pad".
5. **USB-C.** `NanoTypec` role switching only where `typec_role_switch` is set; DP detection by a per-SoC list (today `NanoMenuMtp.cpp` falls back to `/sys/module/sprd_typec_displayport`). The USB menu also reads `/sys/class/power_supply/usb/online` and `battery/capacity`, with a UDC-state fallback; supply names differ on other SoCs (check each in phase 0). `nano_powerwatch.cpp` only logs the Unisoc PMIC long-press settings (`sprd_7sreset`) and is harmless elsewhere.
6. **Monitor Mode limits** from per-SoC properties; hidden where there is no video out. Most handhelds in tiers 2-4 have no wired video out (T618, Helio G99 *(unverified)*, A133P, Air X, RP Classic, Pocket Air Mini).
7. **Crop projection.** `ThimsySurfaceFilter.h` hard-codes 800x600/1600x1200 and 800x720/1200x1080. Compute it from the display size, as the RetroArch shader already does.
8. **Portrait-native panels.** RG556 (1080x1920), RG405V (480x640), MagicX Zero 40 (480x800 in the catalog), RG Vita Pro and GKD 350H Ultra (ORIENTATION_270), Air X (rotation 270), RP Classic (1080x1240), RP4 Pro (750x1334), Duo Lite top panel, and the A133P panels (180, 90 or 0 degrees). Thimsy's SurfaceFlinger shader, Nano DRM rendering, Mirror and Dual Screen were only exercised on the square Cube panel. Test each rotation.
9. **Dual-screen devices.** RG DS (two DSI panels, already in GammaOS's tree) and RP Duo Lite (bottom panel on DisplayPort through an Analogix bridge, no hotplug at boot). Thimsy's monitor logic would treat the Duo Lite's bottom panel as an external monitor; it must be profiled as built-in. Thimsy's Dual Screen mode (`panelLaunch`, "on the Cube's screen") assumes one built-in panel plus a monitor; on the RG DS it overlaps upstream's Control Center (HOTKEYS.md notes the same clash for key 204). Define which mode owns the second screen per device.
10. **Regressions to check in Thimsy's patch.** The `NanoBootChime` rewrite removed the "RG DS rk817 direct-to-ALSA chime" text. Diff GammaOS 1.3/1.4 system images against Thimsy's for other device conditionals before each family's bring-up.
11. **Upstream updater.** Thimsy's own 1.4.4 system carries upstream's updaters: the `gammaos-ota` service and Nano's `NanoOtaCheck` (Quick Menu > Settings > System Update), and probably the Next updater app (Settings > System > System updates, 1.3.x and later) *(unverified in the tree)*. The GammaOS OTA server (`GammaOSOTA/api/v1`) has endpoints for `anbernicrgds`, `anbernicrgdsplus`, `anbernicrgrotate`, `anbernicrgvita`, `anbernicrgvitapro`, `pocketairmini` and `retroidpocketduolite`: the `ro.gammaos.device` values those vendors set. On those devices a Thimsy install would be offered GammaOS updates that replace it. The Cube has no endpoint today, which is why this hasn't shown up. Disable or repoint the updaters in the shared system before any of those devices ships (decision 18).
12. **Low RAM.** Honour `ro.config.low_ram`; keep lmkd tuning in vendor. A133P vendors need `zygote64_32` (don't set `ro.zygote.disable_secondary`), while the CDD wants a single ABI below 2 GB (Core has a `disable_32bit_mode` toggle). Per-device setting.
13. **KeyMint mitigation (Helio G99).** A per-family way to report Android 13 to the TEE, modelled on the GSI KeyMint Patcher *(source not reviewed)*.
14. **SurfaceFlinger DP workarounds.** The vsync watchdog and `mGammaUnisocDisplayListener` are harmless elsewhere *(unverified)*; gate by a per-SoC property if they misfire on MediaTek or Qualcomm DP.

## 7. Installer changes (thimsy-os-installer)

The installer must take any supported device from "pick your handheld" to a running Thimsy, on every desktop the device's flashing route allows, with every profile the device allows. Today every build and flash path assumes a PAC and `spd_dump`. The catalog already lists 30 devices (29 as `candidate`, only the Cube buildable), but `flash.rs` refuses any method other than `spd` and `pack.rs` only diffs PACs. `universal_installer.md` already designs most of what follows; the code does not have it yet.

### 7.1 Catalog schema

1. **SoC-family files** (`catalog/families/*.yaml`): default backend, USB IDs per mode, mode names, drivers per host, udev extras, protected partitions, backup ability, connect semantics, unlock recipe template, translated guide templates. Device files name their family and override it.
2. **Typed flash recipes.** `Flash` becomes a tagged enum per backend with validated fields:
   - `spd` {fdl, partition_list_4k, unchecked, never_write, update_check};
   - `fastboot` {images, logical (fastbootd), slots, wipe, reboot_to};
   - `spflash` {scatter or XML per storage variant, format ranges, preloader};
   - `qdl` {programmer, rawprogram, patch, storage};
   - `sdcard` {image, layout: raw, rk_update_card, rk_boot_card, phoenixcard}.
   Shared: writes_full, writes_update, erase_full, protected, wipes_internal, connect (before, tool_waits, none), mode_name. Unknown methods are rejected; `min_installer` per device and per recipe.
3. **Sources.** `sources: []`, each with an id, a variant key and a container format (pac, fastboot_images, mtk_scatter, qfil, gpt_disk, rk_rkfw, allwinner_imagewty). `contains` may list several files. Zip archives. Shared assets (UnisocTools.7z). A device's format can change between releases (RG DS: raw self-flashing image up to v1.4.1, RKFW update card from v1.4.4), so the format belongs to the source, not the device. A `public: false` source (supporter-only release) is never offered for download, only imported by hash. The hashes and passwords for the 30 catalogued devices are already in `installer/docs/gammaos-devices.md`.
4. **Variants and preconditions.** Packages keyed by (profile, variant). Typed questions (storage 128/256 GB, MQ65/MQ66, Bluetooth model, overclocked or not), confirmations (manufacture date, stock version) and detected checks (`getvar`, adb props). `blocks:` is enforced, not just listed: RG557 v1.26+, RP Classic from March 2026, Air X unknown serial prefixes.
5. **Hardware fields.** Structured SoC, GPU family, vendor API, kernel, FCM level, runtime variant, display geometry. Today's `hardware.android_api` mixes things: 33 on the five devices whose only GammaOS build is Android 13 (Helio G99 x3, RP4 Pro, RP Classic) describes GammaOS's system, while the RP Classic's vendor is stock Android 14 and the KT-R1's stock Android 12. Split it into system API and vendor API. Those five are marked as needing `check_vintf` and hardware qualification.
6. **Profiles per device.** Allowed and default profiles; a super-fit check (system size vs the device's super free space) before downloading; real exclusions instead of "installed but unsupported" (device_profiles_plan.md phase 1).
7. **New and corrected device files:** RP Duo Lite, TrimUI Smart Pro, RK3566 devices (when un-deferred). Correct `universal_installer.md` §2.2/8.1: the RG Vita and RG Rotate are T618 PAC devices with no unlock step. Catalog `gpu: Mali-G52` for RK3576 should say MC3; A133P `vulkan: true` should say Vulkan 1.0. `device-keys.json` says Snapdragon 680 for the Air X; it is a Snapdragon 662 (the catalog is right).
8. **Signed remote catalog** (minisign, monotonic version, expiry, revocation; `universal_installer.md` §10), so a device can be qualified without an app release. The bundled catalog stays as the offline fallback.

### 7.2 Detection, identity and selection

- Every T820 and T618 PAC device enumerates as 1782:4d00 in download mode; every RK3566 as 2207:350a in maskrom. **USB IDs never identify a model.** Flashing the wrong T820 model's PAC gave a garbage screen and a permanently broken touchscreen (GammaOSNext issue #129).
- Today the only guard is the partition-table match, which the six T820 siblings very likely pass with each other's packages.
- Add an **identify probe**: bundled adb (`ro.product.vendor.model`, `ro.gammaos.device`, `ro.product.device`, `ro.serialno` prefix for the Air X, stock build for RG557 v1.26) before the device leaves Android, and `fastboot getvar product/unlocked` on fastboot devices.
- In download mode, confirm by readback before the first write: dtbo_a (or another small per-model image) hashed against known stock and GammaOS hashes. An unknown hash stops the flash.
- Record the evidence in `device.json` and `job.json`. Re-check identity on retry.
- Detection reports family-level matches ("a Unisoc T820 device in download mode; the model is confirmed before installing"), so the Install button works for fastboot and EDL devices whose IDs are shared (today it never enables for them).
- `detect.rs`'s generic list today knows Unisoc download (1782:4d00), Android fastboot (18d1:4ee0, 18d1:d00d), Qualcomm EDL (05c6:9008) and MediaTek BROM/preloader (0e8d:0003, 0e8d:2000). Add Rockchip maskrom/loader (2207:350a for RK3566/RK3568, the RK3576 ID from phase 0), Allwinner FEL (1f3a:efe8), the Unisoc and MediaTek fastboot IDs (upstream installs Unisoc drivers for T618 fastboot and MTK plus Unisoc drivers for the Helio G99 and RP4 Pro), and "an adb device" read through the bundled adb rather than the USB product string `adb_model` matches today.

### 7.3 Flash backends per SoC and host

Engine: a `Backend` trait (plan, driver_ensure, wait_and_identify, backup, verify_layout, write, base_check, reboot, output parser) and a `Container` trait (open, list, read or replace an entry, lp access, finalize, hash). `flash.rs` today stops with "flashing method isn't available" for anything but `spd`; that becomes a dispatch. `drivers.rs` knows Windows (WinUSB or the Unisoc INF) and Linux (udev) and reports "needs no driver" for every other host; it grows per-backend driver sets.

| Backend | Devices | Windows | macOS | Linux | Tools |
|---|---|---|---|---|---|
| **Spd** (today's code) | T820 x6; T618 RG Vita, RG Rotate | sprdvcom (bundled) or WinUSB | libusb; Apple Silicon asks to allow the accessory | libusb + udev | spd_dump (patched; ums512 partition units, FDL addresses and the Rotate's SPL to verify) |
| **Fastboot** | T618 RG405M/V/505; Helio G99 x3; RP4 Pro; RP Classic; RP Duo Lite; E6 Max unlock | WinUSB via libwdi for each mode's ID (MTK fastboot uses VID 0e8d) | none | udev for 18d1 and 0e8d | AOSP adb and fastboot, bundled (drop the PATH fallback) |
| **SdCard** | RG DS, RG Vita Pro, GKD 350H Ultra, MagicX x3 (RG DS Plus, TrimUI Brick, TSP and RK3566 SD devices later) | admin, `\\.\PhysicalDriveN`, lock and dismount | `/dev/rdiskN` via authopen | udisks2 or pkexec | native writer; embed `rockchip_sd_tool` (MIT, Rust crate) for RKFW layouts; port OpenixCLI's MIT `convert` for IMAGEWTY |
| **Qdl** | MANGMI Air X (RP Duo Lite unbrick) | WinUSB or Qualcomm QDLoader | libusb | udev for 05c6:9008, ModemManager ignored | linux-msm/qdl (BSD-3) or qualcomm/qdlrs (Rust); never bundle QSaharaServer or fh_loader |
| **SpFlash** | RG557, RG477M, RG477V (V6 XML); Pocket Air Mini (v5 scatter) | MediaTek VCOM (fetched, pinned hash) | not supported by SP Flash; mtkclient only *(untested)* | SP Flash Linux build, udev, ModemManager ignored | SP Flash Tool fetched on first use; headless V6 CLI with the preloader Write Memory step unverified; mtkclient (GPL-3) or penumbra (AGPL) as separate processes |
| **Walkthrough** | E6 Max (PhoenixSuit, Windows only), and section 8.6 exceptions | guided | none | none | PhoenixSuit fetched on first use |
| Rockchip USB (optional) | RK3566 eMMC devices; unbrick | WinUSB via libwdi (breaks RKDevTool until reverted) | libusb | udev for 2207:350a | rkdeveloptool (GPL-2, separate binary) or xrock (MIT); needs RKFW unpacking |

Every backend keeps the rules of `universal_installer.md` §8.8: identify before writing, write allowlist, protected partitions, no relocking.

**Protected partitions per family** (from the catalog, not a shared list): Unisoc prodnv, miscdata, calinv; MediaTek nvram, nvdata, nvcfg, protect1/2, proinfo, seccfg, persist; Qualcomm persist, modemst1/2, fsg, fsc, devinfo, frp. Air X packages contain `persist.img`; it must stay in `never_write` unless proven harmless. Thimsy never uses SP Flash "Format All" on MediaTek, although upstream's own Pocket Air Mini clean install does: Thimsy's Pocket Air Mini recipe needs a download-only route that formats only what the install needs, proven on a unit (decision 17).

**Backups:** spd, qdl and SP Flash can read partitions back; fastboot can't. Fastboot devices get adb-side checks and an explicit "no NV backup possible" notice.

**Unlocks** become typed recipes: adb_reboot_bootloader, getvar unlocked, `fastboot flashing unlock` (Vol Up within 3 s), `fastboot oem unlock` + reboot (E6 Max), guided routes (T820 BROM unlock `UnlockBootloader.bat`; T618 WebUSB page or `unisoc_unlock`). Skip when already unlocked. RG Vita, RG Rotate, RP Classic and RP Duo Lite need none per the wiki, nor do the SP Flash, EDL and SD-card devices. The RG Rotate's (and possibly the RG Vita's) GammaOS PAC carries a patched SPL that skips Unisoc's signature checks (`UnisocBypass`, `ums512_spl`); writing it is upstream's bypass, not Thimsy's, but `universal_installer.md` §1 ("Unlock") forbids shipping or automating bypasses, so decision 5 covers it too.

### 7.4 Packages per device and profile

- **Format 2** of `pack.json` (format 1 stays readable for the RG Cube): a container type; generic ops (file delta or replace, lp grow/write/delta on raw or sparse super, GPT partitions, IMAGEWTY items, container finalize); per-output reference hashes; source id and variant.
- **Shared system artifact.** Every package today embeds its own copy of the system image (`<system>.img.zst`). With about 30 devices x 3 profiles that is about 90 near-identical system images. Reference the system by sha256 per (release, profile, runtime variant), download once, cache by hash. Today's system image is grown to fill each device's free super space (`build.sh`, §5), so its bytes differ per device: the shared artifact must be a minimum-size image, and the engine gains an ext4 grow step (or the package records a fixed size per device, and the device keeps unused super space).
- `pack.rs` grows a differ per container, streaming large SD images.
- `tools/make-thimsy-build.sh` takes each device's output format and gains a matrix mode (every device, profile and variant, plus one shared system per profile).
- Package names carry the variant (`fetch.rs`).
- Patreon-first releases: the catalog can only point at public releases; until then, import a local file that matches the hash. Today this blocks the RG DS Plus entirely (no public download) and pins the RG DS to v1.4.1.

### 7.5 App flows

- The plan carries the ordered steps for this device and mode, and the app renders them, replacing the fixed `WizardStep` list and its Unisoc special cases:
  - variant;
  - backup;
  - unlock;
  - ADB authorize;
  - connect;
  - card select;
  - write card;
  - insert card and boot;
  - review;
  - install;
  - first boot.
- **New screens:** Variant (questions from preconditions; Review summarises the answers), ADB authorize, Card select and write (removable-drive picker, capacity and A2 check, "everything on this card is erased", write and verify progress, eject), and a "Press Volume Up to confirm unlock" photo step.
- **Host fallbacks** from `support.install` ("Needs Windows or Linux"), not "Use a Windows PC" everywhere. Mode names and recovery wording from the plan (download mode, fastboot, EDL, BROM, maskrom, microSD card). Re-translate the affected strings in all 22 languages.
- Per-family translated guide templates, and `Step.image` for button-combination photos.
- `wipes_data` per mode, not always true: SD-boot devices (A133P, GKD 350H Ultra, RG DS Plus) leave internal storage alone, while the RG DS and RG Vita Pro cards flash internal storage and wipe it.
- Journeys for SD devices ("write a card", "update the card") and for no-unlock PAC devices.
- Android host: build and export first, then SD writing through a USB card reader, then OTG flashing (`universal_installer.md` §9).

### 7.6 Tests and CI

- Fake tools per backend (fastboot, qdl, spflash, rkdeveloptool, SD writer) next to today's fake `spd_dump`; fake-engine scenarios per backend in the app; flow tests parametrised over them.
- Synthetic fixtures per container: image folder with sparse super, scatter, rawprogram, GPT disk, IMAGEWTY.
- Catalog lint: every buildable device has a source, typed recipe, identify, drivers and protected partitions; every descriptor matches a device-keys entry and a `pads.json` entry.
- e2e matrix per family; a real-firmware golden rebuild for each qualified device; protocol samples per backend; hardware qualification records under `tests/hardware/<device>/`.
- CI builds and bundles adb, fastboot (from AOSP source), qdl, rkdeveloptool and the libwdi helper for every desktop.
- Docs: `engine-protocol.md` per-backend sections; `universal_installer.md` gets a multi-device architecture section and corrected device table.

## 8. Testing and hardware access

The owner has an RG Cube (logs in `out/.dock`). Every other device is plan-only until someone tests it. Each family needs at least one reference unit, owned or with a tester:

| Family | Reference device | Why that one |
|---|---|---|
| T820 | RG556 | Different panel (portrait AMOLED), same flash route |
| T618 PAC | RG Rotate | Vendor closest to 1.4.4; 720x720 like the Cube |
| T618 fastboot | RG405V | Public teardown with verified kernel and partitions |
| Helio G99 | ZPG A1 Unicorn | 720x720; settles the KeyMint question for the family |
| Retroid | RP4 Pro or RP Classic | Unproven A14 (RP4 Pro) or revision gate (Classic) |
| Rockchip SD | RG DS | Dual screen; public base v1.4.1 (v1.4.4 is supporter-only) |
| RK3576 | RG Vita Pro | SD self-install image format |
| A133P | MagicX Mini Zero 28 (2 GB) | PowerVR, PhoenixCard layout; the TrimUI Brick (1 GB) only if decision 9 brings 1 GB devices in |
| Qualcomm EDL | MANGMI Air X | Only EDL device |
| MediaTek SP Flash | RG557 (pre-v1.26) or Pocket Air Mini | V6 XML vs v5 scatter |
| Allwinner A527 | GameMT E6 Max | PhoenixSuit; MTP |

Hand tests per device: boot, touch and rotation, controls and face layout, Wi-Fi and Bluetooth, audio, sleep and resume, charging and off-mode charging, fan and RGB where present, video out where present, first-boot time, emulator defaults.

Without hardware:

1. **Unpack one image per family** (the archive passwords are public once early access ends). Record the partition table, super size and free space, vbmeta flags, vendor `build.prop` (VNDK, FCM, `ro.board.platform`, model), kernel version (module vermagic), dtbo entries and logo format.
2. **Diff system images.** GammaOS 1.2, 1.3 and 1.4 system images across devices against Thimsy's, to confirm one GSI and find device conditionals.
3. **Offline VINTF.** Run `checkvintf` on Thimsy's system against each vendor's manifest and matrices.
4. **Round trips.** Container backend unpack and repack must reproduce the input bytes.
5. **Fake-tool installer tests** per backend (section 7.6).

## 9. What each family needs, in short

| Family | Container | Vendor steps | Main per-device work |
|---|---|---|---|
| T820 | PAC | dtbo dock fix and sprd_drm per model (off until measured), vendor_dlkm and vendor_boot order | Panel, touch order, logo size, keylayout, slide sensor (Slide: no `no_slide`), 120 Hz lists (476H, Slide), model identity by readback |
| T618 PAC | PAC | none at first | Rotate SPL never written by default; SD-card warning; ums512 partition units; signing assumptions (Anbernic may sign with Unisoc reference keys *(guess)*) |
| T618 fastboot | image folder | none | Write list parsed from upstream's `FlashPartitions.sh`; first install always wipes |
| Helio G99, RP4 Pro | image folder | none | KeyMint mitigation; GammaOS vbmeta reused; MTK logo or none; Android 13 to 14 always wipes |
| RP Classic, Duo Lite | image folder | none | Revision gate (Classic); 1.5.0 system shims (Duo Lite); Adreno layer |
| RK3568, RK3576 | RKFW or raw SD | none | RKFW repack (and raw self-flashing images); RSCE logo; clock variant; 4 GiB update-card limit; non-A/B names; dual screen (RG DS) |
| A133P | IMAGEWTY | none | Byte-identical boot chain; device pack without odm; low RAM; PowerVR |
| Pocket Air Mini, Dimensity 8300 | MTK scatter / XML | none | Sparse super; storage-size XML; thermal module; preloader untouched |
| Air X | QFIL flat build | none | MQ65/MQ66 builds; persist untouched |
| E6 Max | IMAGEWTY | none | Secure boot chain untouched; MTP default off; base on upstream's Lite archive (upstream marks Full NOT_RECOMMENDED) |

## 10. Phased roadmap

**Phase 0: facts, no hardware.** Section 8 items 1-3 for one image per family. Read `FlashPartitions` scripts, scatter files, rawprogram XML and `parameter.txt`. Fill the descriptors' partition and AVB fields. Answer the §13 open questions 1-6 and get the owner's §12 decisions 1-6.

**Phase 1: descriptor refactor, Cube only.**
- Build: `devices/anbernic-rg-cube` descriptor; `build.sh DEVICE=`; container and logo backends behind interfaces; optional steps; byte-identical Cube PAC (golden test).
- System: `ro.thimsy.device`; Cube facts out of the shared system and product build.prop into the device pack; per-device init `.rc` fragments; gammapad and `pads.json` read the device entry.
- Installer: Backend and Container traits with Spd only; family files; typed recipes; identify probe and readback identity check; variants and preconditions schema; format 2 read path.

**Phase 2: the rest of the T820 family.** First RG556, then RG406H, RG406V, RG476H, RG Slide. Per-model hashes for update checks, logos, keylayouts, panels. Dock fix and sprd_drm stay off until each model is measured.

**Phase 3: T618 PAC.** RG Rotate, then RG Vita. spd_dump on ums512.

**Phase 4: fastboot backend.** T618 RG405V, RG405M, RG505; Helio G99 x3 (KeyMint); RP4 Pro (test boot first); RP Classic (revision gate).

**Phase 5: SD backend.** RG DS, RG Vita Pro, GKD 350H Ultra; then the 2 GB A133P devices: MagicX Mini Zero 28, Zero 40, XU20 V32 (IMAGEWTY).

**Phase 6: remaining backends.** qdl (Air X); SP Flash (Pocket Air Mini, then the Dimensity 8300 devices with the storage question); PhoenixSuit walkthrough after an automated `fastboot oem unlock` (E6 Max).

**Phase 7: deferred devices.** RP Duo Lite once 1.5.0 is public; RK3566 after an Android 14 test boot on an RG353V or ARC-D (fastbootd on the stock vendor, or SD); TrimUI Smart Pro; RG DS Plus (also needs a public release), TrimUI Brick and other 1 GB devices once decision 9 allows them.

In parallel from phase 1: the shared system artifact and the signed remote catalog.

### 10.1 The first device after the Cube: RG556

- Same SoC, same PAC, same `spd_dump` route, same UnisocTools and BROM unlock, so the installer needs no new backend.
- It still differs where the refactor must work: a different dtbo, panel (1080x1920 portrait AMOLED, used rotated), module hashes, logo size and keylayout check. That proves the optional steps skip cleanly and that rotation works.
- `device_profiles_plan.md` phase 3 already chose it.
- Known upstream bugs to expect: inverted auto-rotation (#168), deep-sleep drain (#277), HDMI docking leaves the AMOLED on (#235).
- The alternative is the RG Rotate (decision 1): its vendor is closest to Thimsy's 1.4.4 system and its 720x720 panel reuses the Cube's layouts, but it needs ums512 checks and the SPL handled first.

## 11. Risks

1. **Wrong-model flash** across shared USB IDs; can break a touchscreen for good (#129). Mitigation: section 7.2.
2. **Version skew** between Thimsy's 1.4.4 system and vendors paired with GammaOS 1.0-1.3 systems: lost device fixes, missing properties or HALs.
3. **No hardware** for every entry but the Cube (46 of 47 in scope). Every step past the Cube is plan-only until tested.
4. **Android 14 unproven** on RK3566, RP4 Pro and the Helio G99 devices; KeyMint lock on MT6789; older system on a newer vendor on the Duo Lite.
5. **Hardware revisions** (RP Classic from March 2026, RG557 v1.26, Air X serial prefixes) that may not be detectable electronically.
6. **Super capacity.** Thimsy's system may not fit next to vendor on small devices (RG Rotate 32 GB, Pocket Air Mini 32 GB, E6 Max 32 GB, A133P cards). Profiles must be checked per device.
7. **Low RAM** (1-3 GB) for Thimsy's apps and services; ARMv8.0-only devices (A133P, Air X, Duo Lite, RK3576) for ARMv8.2 emulator builds.
8. **GPU drivers.** PanVK and MaliSX2 depend on each vendor's kbase version; PowerVR is Vulkan 1.0; Turnip on Adreno 610 and the RP Classic's GPU unverified.
9. **Flash tooling.** Closed, Windows-only tools (SP Flash Tool, PhoenixSuit, PhoenixCard, SDDiskTool, RKDevTool); unclear redistribution; Windows driver conflicts (Unisoc driver vs WinUSB, MTK VCOM vs UsbDk); ModemManager on Linux. GPL and AGPL tools must stay separate processes.
10. **Bricks.** Rotate SPL (and the patched-SPL route upstream documents for the RG Vita); MTK preloader and nvram; Allwinner secure boot chain; E6 Max bricks if flashed before `fastboot oem unlock`; GKD Bubble SPI NOR; undocumented maskrom entry on RG DS, Vita Pro and GKD.
11. **Upstream releases** are Patreon-first with passwords for about 60 days, so new bases can't be fetched automatically at first.
12. **Future Android.** Kernel 5.4 (T820, T618), 4.19 and 4.9 kernels, and FCM 4 vendors cap these devices at Android 14 under a GSI approach.
13. **Kernel source.** None published for most devices (GameMT, TrimUI, Retroid, GKD), so kernel bugs can only be worked around.

## 12. Decisions for the owner

1. **First device after the Cube:** RG556 (recommended), or RG Rotate?
2. **One shared system image** per release and profile, with every device fact in the device pack or chosen at runtime by `ro.thimsy.device`. Confirm.
3. **Devices without odm** (most A133P units): rebuild vendor with the device pack, or keep device facts in the system image, selected by `ro.thimsy.device`?
4. **Scope:** exclude the two Konkr (Pico) devices; defer RK3566 Core, TrimUI Smart Pro, RG DS Plus, TrimUI Brick (with decision 9) and RP Duo Lite as section 3 says. Agree?
5. **T820 BROM unlock** (CVE-2022-38694): keep it guided (link out), as today, or bundle it? `universal_installer.md` forbids bundling tools that bypass signature checks. Upstream's `UnisocBypass` now documents a signature-preserving uboot unlock for the fused T820 too; it is still a bypass. The same rule decides whether the installer may write the RG Rotate's (and the RG Vita's, if its PAC has one) patched SPL from upstream's PAC, or must skip it.
6. **Hardware:** buy reference units (section 8), recruit testers, or both? Which first?
7. **Blocked revisions** (RP Classic from March 2026, RG557 v1.26+): hard block, or allow with an explicit confirmation?
8. **UFFD GC on old kernels:** one system image (accepting the boot-image recompile, which upstream saw on every cold boot, not once), or a second system variant built with `PRODUCT_ENABLE_UFFD_GC := false`? Measure on a 4.x device first (§4.1).
9. **1 GB devices:** exclude, or plan a slim low-RAM variant?
10. **Package format 2** with a shared system artifact cached by hash. Approve?
11. **Third-party flashing tools:** fetch SP Flash Tool, PhoenixSuit and the MTK driver on first use; mtkclient (GPL-3) and penumbra (AGPL) only as separate processes; embed rockchip_sd_tool (MIT) and qdl (BSD-3). Approve?
12. **Clock variant** on RG DS (standard or `NO_OC_UV`) and RG Vita Pro (standard or `NO_OC`): upstream recommends the standard (overclocked) build first. Same default, or the safer one?
13. **KeyMint mitigation** on Helio G99 (reporting Android 13 to the TEE). Acceptable, given it may limit security patch level bumps?
14. **Base per device:** always the latest public GammaOS release for that device, or pin one per Thimsy release? And which upstream edition (Full, Lite or Core) each device's base uses.
15. **Release-system builds** (no `SYSTEM_IMG`, today's Docker build on the 1.2 base): keep them RG Cube only, and build every other device from the 1.4.4 system? (Recommended: yes.)
16. **Supporter-only bases** (RG DS v1.4.4, every RG DS Plus release): target only public releases (the RG DS stays on v1.4.1 and the RG DS Plus waits), or also accept a local file the user imports by hash?
17. **Pocket Air Mini clean install:** upstream uses SP Flash "Format All + Download". Follow it, or require a download-only route that leaves nvram and calibration alone (needs a unit to prove)?
18. **Upstream updaters** in Thimsy's system (`gammaos-ota`, `NanoOtaCheck`, possibly the Next updater): remove them, or point them at a Thimsy update server?

## 13. Open questions

1. Super size, free space and group layout on each device.
2. Kernel version and vendor FCM level on each device (only the Cube, RG405V, RG DS and A133P vendors are read).
3. Do all six T820 models share the Cube's partition list, `partition_list_4k` and FDL addresses?
4. RG Vita and RG Rotate: which keys sign vbmeta and vendor_boot, and can Thimsy re-sign them? Upstream's `UnisocBypass` says its patched ums512 SPL skips the RSA checks on sml, trustos and uboot, but dtbo still fails hard on an AVB mismatch even when unlocked (`docs/AVB_RESIGN.md`): a dtbo change on T618 needs re-signing.
5. Fastboot and fastbootd USB IDs on T618, Helio G99, Retroid and Qualcomm devices (18d1:4ee0, 18d1:d00d, 0e8d:201c are guesses; upstream has Windows users install Unisoc drivers for T618 fastboot, which suggests a 1782 ID there).
6. Does the RG Vita Pro's `SDCARDINSTALL.img` hold a Rockchip update card with `sdupdate.img` in FAT32 *(guess)*?
7. Which TEE (TrustKernel or Microtrust) do the KT-R1, ZPG A1 and Pocket Micro use?
8. What did GammaOS 1.5.0 change to boot on the Duo Lite's Android 15 vendor, and will it be public?
9. Has GammaOS's "GCore+" Android 14 work for RK3566 been published anywhere?
10. Does SP Flash Tool V6 have a headless mode that takes a Download XML and the preloader Write Memory step?
11. Is the A133P system image byte-identical to the T820 GSI (same release)?
12. Is MQ66B covered by the MQ66 package?
13. Can RP Classic revisions be told apart electronically?
14. How do the RG DS, RG Vita Pro and GKD 350H Ultra enter maskrom for unbricking?
15. Does PanVK load on each Mali kbase (r34p0 on T618, Rockchip 4.19 and 6.1 bifrost_kbase, G615 CSF)?
16. Do the other Anbernic Unisoc models' pads match the Cube's 484b:1101 keylayout?
17. The RG Vita's GammaOS notes say "Unisoc T820"; Anbernic and the press say T618. Mostly settled: upstream's `UnisocBypass` names the RG Vita as its UMS512 (T618) test device, its UnisocTools bundle is byte-identical to the RG Rotate's, and HOTKEYS.md's key codes come from its device tree. Confirm the platform string from the PAC in phase 0.
18. Do the Helio G99 devices and the RP4 Pro have USB-C video out?
19. Which `power_supply` names (`usb`, `battery`) and DisplayPort detection nodes does each SoC family expose (§6 item 5)?
20. Does Thimsy's 1.4.4 system still include the Next updater app, and what exactly does `NanoOtaCheck` query (§6 item 11)?
21. What USB ID does the RK3576 maskrom use?

## 14. Sources

Upstream GammaOS:

- Releases and wiki: https://github.com/TheGammaSqueeze/GammaOSNext/releases, https://github.com/TheGammaSqueeze/GammaOSNext/wiki/GammaOS-Next-Installation, https://github.com/TheGammaSqueeze/GammaOSNext/wiki/GammaOS-Next-Changelog, https://github.com/TheGammaSqueeze/GammaOSNext/wiki/GammaOS-Next-Installation-for-Allwinner-SD-Card-Devices, https://github.com/TheGammaSqueeze/GammaOSNext/wiki/GammaOS-Installation-Guide-for-Anbernic-RG557,-RG477M,-and-RG477V-(SP-Flash-Tool,-Full-and-Lite-Builds)
- Release pages per device (tags in section 2), including v1.3.2-OTA: https://github.com/TheGammaSqueeze/GammaOSNext/releases/tag/v1.3.2-OTA
- GammaOS Core: https://github.com/TheGammaSqueeze/GammaOSCore/releases/tag/beta3, https://github.com/TheGammaSqueeze/GammaOSCore/releases/tag/beta3_a133p, https://github.com/TheGammaSqueeze/GammaOSCoreDistribution
- GammaOS Pico: https://github.com/TheGammaSqueeze/GammaOSPico
- Older GammaOS lines (§2.2): https://github.com/TheGammaSqueeze/GammaOS/releases/tag/GammaOS_v1.5.1, https://github.com/TheGammaSqueeze/GammaOS-RK3566 (release GammaOS_v1.5.1_RG353_RGARC), https://github.com/TheGammaSqueeze/GammaOS-MTK
- Supporter-only status: the release notes of v1.4.4-ANBERNICRGDS, v1.4.1-ANBERNICRGDS ("now public"), v1.4.2/1.4.3/1.4.4-ANBERNICRGDSPLUS (v1.4.2's files removed)
- OTA server: https://github.com/TheGammaSqueeze/GammaOSOTA/tree/main/api/v1
- Vendor trees: https://github.com/TheGammaSqueeze/GammaOSCoreVendor (branches trimui_brick, mini28, zero40, xu20, gkd_350h_ultra), https://github.com/TheGammaSqueeze/rg_ds_kernel
- Tools: https://github.com/TheGammaSqueeze/Bootloader_Unlock_Anbernic_T820, https://github.com/TheGammaSqueeze/rockchip_sd_tool, https://github.com/TheGammaSqueeze/rk_image_tool, https://github.com/TheGammaSqueeze/ums512_spl, https://github.com/TheGammaSqueeze/UnisocBypass (ums512 patched SPL on the RG Vita; `docs/UMS9620_PORT.md` for the T820). `rockchip_sd_tool` is MIT (a Rust crate with a library target); `rk_image_tool`, `ums512_spl` and `UnisocBypass` publish no licence
- Issues: GammaOSNext #67, #108, #129, #168, #196, #200, #217, #235, #264, #277, #290

AOSP:

- https://source.android.com/docs/core/architecture/vintf/fcm
- https://source.android.com/docs/core/architecture/kernel/android-common
- https://source.android.com/docs/core/architecture/vndk
- https://source.android.com/docs/whatsnew/android-14-release
- https://source.android.com/docs/compatibility/14/android-14-cdd
- https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/android14-release/compatibility_matrices/
- https://android.googlesource.com/kernel/configs/+/refs/heads/android14-release/
- https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/heads/android14-qpr3-release/netbpfload/NetBpfLoad.cpp

Community and tools:

- https://github.com/TrebleDroid/treble_experimentations/wiki/UMIDIGI-G7-Tab-Pro (MT6789 KeyMint)
- https://github.com/picaso1-cloud/rg405v-internals (RG405V teardown)
- https://github.com/ROCKNIX/distribution (Air X and RP Classic device trees, PRs 3498, 3521, 3522)
- https://github.com/testyfishy/duolinux (RP Duo Lite)
- https://github.com/beebono/GammaOS-Linux-Tools, https://github.com/beebono/rg-rotate-linux
- https://github.com/ilyakurdyukov/spreadtrum_flash, https://github.com/TomKing062/spreadtrum_flash, https://github.com/patrislav1/unisoc-unlock
- https://github.com/bkerler/mtkclient, https://github.com/shomykohai/penumbra
- https://github.com/linux-msm/qdl, https://github.com/qualcomm/qdlrs
- https://github.com/rockchip-linux/rkdeveloptool, https://github.com/xboot/xrock
- https://github.com/YuzukiTsuru/OpenixCLI, https://github.com/vicromao/imagewty-tool
- https://github.com/zenithblue-oss/panvk-kbase-android

Spec and review pages: Notebookcheck, Android Authority, retrohandhelds.gg, Retro Dodo, Joey's Retro Handhelds and Anbernic product pages, as cited per device in the research (for example https://www.androidauthority.com/retroid-pocket-classic-review-3557622/, https://www.joeysretrohandhelds.com/reviews/gkd-350h-ultra/, https://win.anbernic.com/product/685.html).

Local:

- `out/.dock/getprop.txt`, `uname.txt`, `cmdline.txt`, `partitions.txt` (RG Cube vendor facts)
- `patches/thimsy/0001-thimsy.patch`, `scripts/`, `mods/` (build audit)
- `thimsy-os-installer`: `installer/engine/crates/thimsy-engine/src/*.rs`, `installer/catalog/`, `installer/app/lib/`, `installer/docs/gammaos-devices.md` (hashes, passwords, install methods for 30 devices), `universal_installer.md` (installer audit)
- `device_profiles_plan.md`, `usb_c_dock_plan.md`, `display_modes_plan.md`
