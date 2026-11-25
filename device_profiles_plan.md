# Device, chipset and profile packaging plan

Date: 2026-11-18
Status: plan only, nothing implemented
Builds on: `universal_installer.md` (device recipes, device packs, the installer). This plan
replaces its section 3.3 (Light / Medium / Heavy) with four profiles, and fills in what ships
per chipset and per device.

The goal: one Thimsy tree that produces the right package for every supported handheld.

- Each device gets only its own changes. The RG Cube's 720×720 shaders, controller layout and
  Quick Settings tiles ship on the RG Cube, not on an RG556.
- Changes that belong to a chipset (Unisoc T820) or a GPU family (Mali Valhall) ship on every
  device with it, written once.
- A **profile** (Small, Medium, Large, Ultra) picks which emulators, RetroArch cores and config
  files ship, and **which emulator is the default for each platform**.
- The installer lets the user pick **any profile on any device**. It preselects the chipset's
  **recommended** profile, labels it "Recommended", and labels the others by how well they
  suit the device. It never blocks a choice.

---

## 1. Decisions at a glance

| Question | Proposal |
|---|---|
| Profiles | **Small, Medium, Large, Ultra.** Large is today's Thimsy. Ultra is Large plus Switch, Switch 2, PS3 and PC (Windows) emulation |
| What a profile decides | The emulators and cores that ship, the **default emulator for each platform**, Nano's home defaults, and baseline emulator settings |
| Who picks the recommended profile | The **chipset**. Every T820 device gets Large, the H700 class gets Small, and so on. A device can only lower it (for example, too little RAM). It never raises it |
| Choice | **Every profile is selectable on every chipset.** The recommendation is a preselection and a label, not a gate |
| Device-specific content | Goes in **layers** chosen by selectors: GPU family, chipset, display geometry, device. Nothing device-specific stays in the generic mods |
| Precedence | base < profile < GPU family < chipset < display < device < user choice < hard constraints |
| Default emulators | Each platform gets an **ordered fallback list** per profile, for example `psx: mednafen_psx_hw, pcsx_rearmed`. The resolver takes the first entry the device can run |
| Hard constraints | Each component declares `requires=` for what it **can't start without**: CPU ISA, Android API level, a GPU API. Only those drop a component, and the installer lists it. RAM and speed never drop anything; they only set the profile's label |
| Package | Still one self-describing `Thimsy-<release>-<device>-<profile>.tar` whose `pack.json` is the manifest. `pack.json` gains the profile, the chipset, whether this profile is recommended, and a contents summary for the installer UI |
| Naming | `heavy` becomes `large` in package names (today's `Thimsy-1.0a-anbernic-rg-cube-heavy.tar`) |
| Format | Extend the existing line-based lists in `mods/` (`retroarch-cores.txt`, `added-apps.txt`, `nano-systems.txt`) with `profiles=` and `requires=` columns. Add small per-layer directories under `packaging/`. No new file format |
| First milestone | Move today's RG Cube specifics into layers with **no change in output**: the RG Cube Large package must match today's build byte for byte. Then build Small and Medium for the RG Cube |

---

## 2. Layers

A package is resolved from these layers, lowest priority first:

| # | Layer | Selected by | Holds | Example |
|---|---|---|---|---|
| 1 | Base | always | Everything generic: Nano, hotkeys, boot animation, version, MTP, pack-in games | today's generic mods |
| 2 | Profile | the user's choice (preselected: recommended) | Which emulators, cores and apps ship; default emulator per platform; Nano home defaults; baseline emulator settings | `profiles/large` |
| 3 | GPU family | `gpu.family` | Driver packs, renderer choices, emulator settings that depend on the GPU | `gpus/mali-valhall`: MaliSX2 for ARMSX2 and hakuX, PanVK |
| 4 | Chipset | `soc` | Recommended profile, CPU-related tuning, chipset workarounds | `socs/unisoc-t820` |
| 5 | Display | `display` geometry | Shaders and aspect settings for a panel shape, shared by every device with that panel | `displays/720x720`: "4:3 Cropped", "8:7 Cropped" |
| 6 | Device | `device` id | Controls, keylayouts, Quick Settings tiles, hardware blocks (fan, RGB, slide, DP), device-keys entry | `devices/anbernic-rg-cube` |
| 7 | User | installer choices | Add or remove emulators, change a default, within what the profile marks editable | "Medium plus Dolphin" |
| 8 | Hard constraints | each component's `requires=` (ISA, Android API, GPU API only) | Always win: they remove a component that can't start, and the platform falls through to its next default | ARMSX2 needs ARMv8.2: dropped on an A133P, even if the user picks Large |

**Rules:**

- **Content.** Layers 3–6 **tune and restrict**. They never add an emulator the profile didn't choose.
  - The one exception is hardware-bound components: driver packs, and shaders for a panel.
  - So Large on a T820 and Large on a Dimensity 8300 ship the same emulators, with different
    drivers and settings.
- **Defaults.** A layer may reorder a platform's default list, but only among emulators the
  profile ships.
  - Example: a Mali chipset can put Flycast's GL renderer first.
- **Shared panel shapes.** The 720×720 shaders are needed by the RG Cube, the RG Rotate and the
  ZPG A1 Unicorn. That is why display geometry is its own layer, not a per-device list.
- **Recorded result.** The resolved result goes into `pack.json` and onto the device as
  `/odm/etc/thimsy/install-manifest.json`. Each value records the layer that set it
  (`universal_installer.md` 3.3).
- **Platform (Linux images).** `linux_handheld_plan.md` §6.3 builds Small and Medium for Linux
  handhelds. This plan has no platform dimension yet, and the content rule above stops layers
  3–6 from swapping a component. The Linux build needs:
  - a platform selector, either a layer or a per-component platform column, that is allowed to
    substitute components;
  - a `platform=` or `requires=platform:linux` key;
  - a Linux substitute for each Android-only default in Small and Medium (section 5.3).

  `requires=`'s Android API level means nothing on Linux. Which form the selector takes is open
  question 7.

---

## 3. What is device- or chipset-specific today

Today all of these are in generic mods that run on every build. Each moves to the layer named
here.

| Today | Where | Target layer |
|---|---|---|
| RG Cube positional ABXY keylayout (`Vendor_484b_Product_1101.kl`) | `mods/27-controller-layout.sh`, `mods/keylayout/` | device `anbernic-rg-cube` |
| PPSSPP and RetroArch binds moved to positions | `27-controller-layout.sh`, `10-update-emulators.sh` | device (the face-button layout is a device fact) |
| `rg_cube_4_3_cropped` / `rg_cube_8_7_cropped` RetroArch shaders | `mods/retroarch-shaders/`, `10-update-emulators.sh:217` | display `720x720` (rename them `720x720_*`; `multi_device_plan.md` §5 names them differently: open question 9) |
| `ro.gammaos.nano.no_slide=1` | `50-nano.sh:255` | device (the RG Slide must not get it) |
| RG Cube Quick Settings tiles (no HDMI tiles) | `50-nano.sh:265-279` | device |
| `persist.gammaos.performance_mode`, `gammargb.control` defaults | `50-nano.sh` | chipset (performance mode), device (RGB) |
| No display-cutout overlays | `12-trim-overlays.sh` | device (keep the trim generic; the cutout list per device) |
| MaliSX2 preinstalled in ARMSX2, offered to hakuX | `45-armsx2-malisx2.sh`, `47-hakux-folder.sh` | GPU family `mali-valhall` |
| PanVK alternate Vulkan driver | `48-panvk.sh` | GPU families `mali-bifrost`, `mali-valhall` |
| Function and Back keys per handheld | `mods/device-keys.json` (already per device, matched at runtime) | stays as it is: one file, entries matched by properties at runtime |
| `sprd-drm.ko` DP fix, vendor_boot module order, odm SELinux | `scripts/sprd_drm.py`, `patch_vendor_*.py`, `odm_sepolicy.py` | device pack (`universal_installer.md` 3.2), not this plan |
| SurfaceFlinger MTK workarounds, HWC rotation path | framework patch, gated by `debug.sf.disable_hwc_overlays` | already gated at runtime by a property. The chipset layer sets the property where needed |

Runtime-gated code in the system image (`drasticNanoAvailable()`, the MTK SurfaceFlinger gate,
`device-keys.json` matching) stays. The system image is the same everywhere, so it decides at
runtime. **Packaging layers decide what files and defaults exist.** Runtime gates decide how
shared code behaves.

---

## 4. Supported devices by chipset

From the GammaOS Next device list (upstream README, 2026-11-18): 27 released, 7 planned. Specs
come from upstream release names and spec databases (section 13). Entries marked *(verify)*
need checking against the device before its profile is qualified.

**Line** is upstream's product line:

- **Next:** full Android, flashed to internal storage.
- **Nano:** GammaOS Nano on a microSD card, upstream's line for low-RAM devices.

| Chipset | CPU | GPU (family) | Devices (RAM, screen, line) |
|---|---|---|---|
| **Allwinner H700** | 4× A53 1.5 GHz | Mali-G31 MP2 (Bifrost) | **No GammaOS device today.** Listed as Small's reference chip (RG35XX family and similar, if ever added) |
| **Allwinner A133P** | 4× A53 1.8 GHz | PowerVR GE8300 | TrimUI Brick *(1 GB, verify)*, 1024×768, Nano; MagicX Zero 40 (2 GB), Nano; MagicX Mini Zero 28 (2 GB), 640×480, Nano; MagicX XU Retro X20 V32 (2 GB), Nano |
| **Rockchip RK3568** | 4× A55 2.0 GHz | Mali-G52 2EE (Bifrost) | RG DS (3 GB, 2× 640×480), Nano; RG DS Plus (1 GB, 2× 1024×768), Nano |
| **Allwinner A527** | 8× A55 2.0 GHz | Mali-G57 MC1 (Valhall) | GameMT E6 MAX (4 GB, 1280×720), Next |
| **Rockchip RK3576 / RK3576S** | 4× A72 + 4× A53 | Mali-G52 MC3 (Bifrost) | RG Vita Pro (4 GB, 1920×1080), Next; GKD 350H Ultra, RK3576S (2 GB, 1600×1440), Nano |
| **Unisoc T618** | 2× A75 2.0 + 6× A55 | Mali-G52 MP2 (Bifrost) | RG405M, RG405V, RG505 (4 GB), Next; RG Vita (3 GB, 1280×720), Next; RG Rotate (3 GB, 720×720), Next; Powkiddy X28 (4 GB), planned |
| **MediaTek Helio G90T** | 2× A76 2.05 + 6× A55 | Mali-G76 MP4 (Bifrost) | AYANEO Pocket Air Mini (2–3 GB, 1280×960), Next |
| **MediaTek Helio G99** | 2× A76 2.2 + 6× A55 | Mali-G57 MC2 (Valhall) | AYANEO Pocket Micro *(6 GB, verify)*, Next; KT Pocket KT-R1 (4 GB), Next; ZPG A1 Unicorn (6/8 GB, 720×720), Next |
| **Qualcomm Snapdragon 662** | 4× A73 2.0 + 4× A53 | Adreno 610 | Mangmi Air X (4 GB, 5.5″), Next |
| **Qualcomm Snapdragon G1 Gen 2** | (verify) | Adreno A12 | Retroid Pocket Classic (4/6 GB, 1240×1080 AMOLED), Next |
| **Unisoc T820** | 1× A76 2.7 + 3× A76 2.3 + 4× A55 | Mali-G57 MC4 (Valhall) | **RG Cube** (8 GB, 720×720), RG556 (8 GB, 1920×1080 AMOLED), RG406H, RG406V (8 GB, 960×720), RG476H, RG Slide *(8 GB, verify)*, all Next |
| **MediaTek Dimensity 1100** | 4× A78 2.6 + 4× A55 | Mali-G77 MC9 (Valhall) | Retroid Pocket 4 Pro (8 GB), Next |
| **MediaTek Dimensity 1200** | 1× A78 3.0 + 3× A78 + 4× A55 | Mali-G77 MC9 (Valhall) | AYANEO Pocket AIR, planned |
| **MediaTek Dimensity 8300** | 1× A715 3.35 + 3× A715 + 4× A510 | Mali-G615 MC6 (Valhall 4th gen) | RG557 (12 GB, 1920×1080 AMOLED), RG477M *(verify RAM)*, RG477V *(verify RAM)*, all Next |
| **Qualcomm Snapdragon 865** | 1× A77 2.84 + 3× A77 + 4× A55 | Adreno 650 | Retroid Pocket 5, Retroid Pocket Mini v1/v2, Retroid Pocket Flip 2 (its SD865 model), all planned |
| **Qualcomm Snapdragon G3x Gen 1 / Gen 2** | 8 Gen 1 / 8 Gen 2 class | Adreno 730 / 740 | AYANEO Pocket DMG (Gen 1), AYANEO Pocket S (Gen 2), both planned |

Notes:

- **RG DS Plus (1 GB).** Upstream runs it only on its Nano card line. Thimsy's base is a full
  Android 14 system. Treat it as **not a Thimsy target** until a low-RAM runtime variant is
  qualified (`universal_installer.md` 3.1).
- **RK3576.** The same chip is Medium in the RG Vita Pro (4 GB) but Small in the GKD 350H Ultra
  (2 GB). This is why a device's RAM can lower its chipset's recommendation.
- **Retroid Pocket Flip 2** also has a Dimensity 1100 model. It is one device id with a
  `soc` chosen at identification (`ro.board.platform`).

---

## 5. Profiles

### 5.1 Recommended hardware (guidance, not a gate)

Every profile can be chosen on every device. This table only decides which profile is
**recommended** (section 6) and how the others are labelled.

| | Small | Medium | Large | Ultra |
|---|---|---|---|---|
| RAM | 2 GB | 3 GB | 6 GB | 8 GB (12 GB for Switch 2 and PS3) |
| CPU | ARMv8.0 | ARMv8.0, at least 2 big cores | ARMv8.2 | ARMv8.2, Cortex-A76 class or newer |
| GPU | GLES 3.2 | GLES 3.2, Vulkan 1.1 | Vulkan 1.1 | Vulkan 1.1 with a good driver: Adreno 6xx+ (Turnip), or Mali Valhall gen 4 (G615/G710 and up) |
| Free space in `super` | smallest | | about today's | largest; the overflow goes to the add-on pack (`universal_installer.md` 3.4) |

What a choice above the recommendation means:

- **The profile installs** with all its emulators. A device short on RAM or GPU gets them anyway;
  some games will run slowly or not at all.
- **Only components that can't start are left out.** Each component's `requires=` names the CPU
  ISA, Android API level or GPU API it needs. ARMSX2 on an ARMv8.0-only chip is the main case
  today. The installer lists anything left out, and why, before it builds.
- **Space never blocks.** What doesn't fit in `super` goes to the post-boot add-on pack.

### 5.2 Contents

**RetroArch cores** (from `mods/retroarch-cores.txt`; ● ships, D marks the profile's default
core for a platform):

| Core | Small | Medium | Large | Ultra |
|---|---|---|---|---|
| fceumm | ● D (NES, FDS) | ● | ● | ● |
| mesen | | ● D | ● D | ● D |
| sameboy | ● D | ● D | ● D | ● D |
| DoubleCherryGB | | ● | ● | ● |
| gpsp | ● D | ● | ● | ● |
| mgba | ● | ● D | ● D | ● D |
| snes9x | ● D | ● D | ● D | ● |
| bsnes | | | ● | ● D |
| mupen64plus_next_gles3 | ● D (N64) | ● | ● | ● |
| genesis_plus_gx, picodrive | ● D | ● D | ● D | ● D |
| yabasanshiro | | ● D | ● D | ● |
| mednafen_saturn | | | ● | ● D |
| flycast (core) | | | ● | ● |
| pcsx_rearmed | ● D | ● D | ● | ● |
| swanstation | | ● | ● | ● |
| mednafen_psx_hw | | | ● D | ● D |
| fbneo | ● D | ● D | ● D | ● D |
| mame2003_plus | ● D (arcade) | ● D | ● | ● |
| mamearcade (257 MB) | | | ● D | ● D |
| opera (3DO) | | ● D | ● D | ● D |
| mednafen_vb, mednafen_ngp, mednafen_pce_fast, mednafen_wswan, mednafen_lynx, stella, a5200, prosystem, gearcoleco, bluemsx, fake08 | ● D | ● D | ● D | ● D |

**Standalone emulators and apps** (from `added-apps.txt`, `apps.list`, the update mods):

| App | Small | Medium | Large | Ultra | Notes |
|---|---|---|---|---|---|
| RetroArch | ● | ● | ● | ● | |
| ScummVM | ● | ● | ● | ● | |
| PPSSPP | ● | ● | ● | ● | Small: 2D and light 3D titles |
| SeedlessDS | ● | ● | ● | ● | GPL-3, DraStic-class speed: the DS default on Small and Medium |
| WatermelonDS | | ● | ● | ● | DS default from Large |
| M64Plus FZ | | ● | ● | ● | Small uses the RetroArch core instead |
| Flycast | | ● | ● | ● | |
| Dolphin | | | ● | ● | |
| Azahar (3DS) | | | ● | ● | |
| ARMSX2 (PS2) | | | ● | ● | `requires=` ARMv8.2, Android 13 |
| Vita3K | | | ● | ● | |
| hakuX (Xbox) | | | ● | ● | |
| **Eden** (Switch) | | | | ● | GPL-3.0-or-later. Snapdragon (Adreno + Turnip) is upstream's best path; Mali is "generally poor". Upstream minimum: SD865, Tensor G1, Exynos 2200 or Dimensity 8200 class with 8 GB |
| **Switch 2** slot | | | | (●) | No stable Android emulator exists. See 5.4 |
| **PS3**: RPCSX-UI-Android | | | | ● | GPL-2.0, early alpha. aPS3e is more usable, but its repo declares **no licence**, so it can't be shipped |
| **Winlator** (PC) | | | | ● | LGPL-2.1. Adreno via Turnip; Mali through Vortek, slower |
| **GameNative** (PC, Steam library) | | | | ● | GPL-3.0 |
| Aurora Store, Shattered Pixel Dungeon | ● | ● | ● | ● | |
| Brave / Fulguris | Fulguris | by RAM (today's 1.3 GB rule) | Brave | Brave | |
| Android Games helper | ● | ● | ● | ● | |

**GPU driver packs** (GPU family layer, Large and Ultra only):

| Pack | When | Consumers |
|---|---|---|
| MaliSX2 0.6.1 | `mali-valhall` | ARMSX2, hakuX (today's behaviour) |
| Mesa PanVK | `mali-bifrost`, `mali-valhall` | Android's updatable driver slot, `/sdcard/GPU Drivers` (today's behaviour) |
| Turnip | `adreno-6xx`, `adreno-7xx` | Dolphin, ARMSX2, Eden, Winlator, hakuX |

**Nano home and settings:**

| | Small | Medium | Large | Ultra |
|---|---|---|---|---|
| Home theme | Minima, fewer animations, no video wallpaper | ES-DE Colorful (Simplified) | Same as today | Same as Large |
| RetroArch | Native resolution, no shader, rewind and run-ahead off | Today's settings | Today's settings | Today's settings, with higher internal resolution where the device layer has tested it |
| Global shader | Available, off | Available, off | Available, off | Available, off |
| Performance mode default | Normal | Max Performance | Max Performance (today) | Max Performance |

### 5.3 Default emulator per platform

This is the "profiles also set the default emulators" part. Each profile gives every Nano system
an **ordered list**; the resolver takes the first entry that ships and passes the device's
constraints. A dash means the system isn't in the profile, so Nano doesn't show it (open
question Q3). Large is exactly today's `nano-systems.txt`.

| System | Small | Medium | Large (today) | Ultra |
|---|---|---|---|---|
| NES | fceumm | mesen | mesen | mesen |
| Famicom Disk System | fceumm | mesen | mesen | mesen |
| SNES | snes9x | snes9x | snes9x | bsnes, snes9x |
| Game Boy / Color | sameboy | sameboy | sameboy | sameboy |
| Game Boy Advance | gpsp, mgba | mgba | mgba | mgba |
| Nintendo 64 | mupen64plus_next | M64Plus FZ | M64Plus FZ | M64Plus FZ |
| Nintendo DS | SeedlessDS | SeedlessDS | WatermelonDS | WatermelonDS |
| Genesis / Master System / Game Gear / Sega CD | genesis_plus_gx | genesis_plus_gx | genesis_plus_gx | genesis_plus_gx |
| 32X | picodrive | picodrive | picodrive | picodrive |
| PlayStation | pcsx_rearmed | pcsx_rearmed | mednafen_psx_hw | mednafen_psx_hw |
| PSP | PPSSPP | PPSSPP | PPSSPP | PPSSPP |
| Dreamcast | – | Flycast | Flycast | Flycast |
| Saturn | – | yabasanshiro | yabasanshiro | mednafen_saturn, yabasanshiro |
| 3DO | – | opera | opera | opera |
| GameCube / Wii | – | – | Dolphin | Dolphin |
| Nintendo 3DS | – | – | Azahar | Azahar |
| PlayStation 2 | – | – | ARMSX2 | ARMSX2 |
| Xbox | – | – | hakuX | hakuX |
| PS Vita (from the app) | – | – | Vita3K | Vita3K |
| **Switch** | – | – | – | Eden |
| **Switch 2** | – | – | – | none until qualified (5.4) |
| **PlayStation 3** | – | – | – | RPCSX-UI-Android |
| **Windows / PC** | – | – | – | Winlator (GameNative for a Steam library) |
| Arcade (MAME) | mame2003_plus | mame2003_plus | mamearcade | mamearcade |
| FinalBurn Neo / Neo Geo | fbneo | fbneo | fbneo | fbneo |
| Neo Geo Pocket, PC Engine / CD, Virtual Boy, Atari 2600/5200/7800, Lynx, ColecoVision, MSX, WonderSwan / Color, PICO-8 | today's player | today's player | today's player | today's player |
| ScummVM, Android Games | ● | ● | ● | ● |

**Chipset overrides of defaults** (examples of what layers 3–4 may do, only among shipped
emulators):

- **PowerVR GE8300 (A133P).** N64: try `mupen64plus_next` with GLideN64 off.
  - This may need `parallel_n64` as a Small-only core. Measure before adding it.
- **Mali Valhall gen 1 (T820, G99, A527).** Today's tested values stay as they are.
  - Saturn stays on `yabasanshiro`, because `mednafen_saturn` is "heavy for the T820" per
    `retroarch-cores.txt`.
- **Adreno.** Dolphin and ARMSX2 get Turnip selected.
  - On Ultra, Eden uses Turnip, or the system driver on 8 Elite class chips, per upstream.

**Linux substitutes** (`linux_handheld_plan.md` §6.3). Linux images need a substitute for each
Android-only default in Small and Medium. Known so far:

| System | Profiles | Android default | Linux substitute |
|---|---|---|---|
| Nintendo DS | Small, Medium | SeedlessDS | melonDS DS core (not yet in `mods/retroarch-cores.txt`) |
| Nintendo 64 | Medium | M64Plus FZ | mupen64plus standalone or the `mupen64plus_next` core: to be settled by `linux_handheld_plan.md` Phase 3's per-system core table |

The full list comes from the resolver once the platform selector exists (section 2, open
question 7).

### 5.4 Ultra: what is realistic

None of the 27 released GammaOS devices meets the hardware floor the emulator projects
themselves give for Switch 2 and PS3:

- **PS3 (all four Android projects).** "Snapdragon 8 Gen 2 or newer" with 12 GB RAM.
- **Switch 2 on Android.** Banana Native or the Eden SX2A fork, both experimental: Adreno only,
  12 GB or more.

Switch (Eden) and PC (Winlator) are usable from the SD865 and Dimensity 8200 class up, mainly on
Adreno. Proposal:

- **Ultra can be chosen on any device**, like every profile. Its label tells the user what to
  expect:
  - Dimensity 8300 (RG557, RG477M/V), the planned SD865 Retroids and the Snapdragon G3x Gen 1:
    "May run poorly".
  - T820, Dimensity 1100/1200 and anything below: "Not recommended".
- **Ultra is recommended** only on the Snapdragon G3x Gen 2 (AYANEO Pocket S, 12 GB and up).
  No released device gets it as the default.
- **The Switch 2 slot ships nothing yet.** The profile reserves the platform, its folder and its
  default list. It turns on when an Android build that is maintained, licensed for
  redistribution and qualified on a device exists.
  - The Android candidates today are a stopped fork (Eden SX2A, v0.3.2 on 2026-11-02) and a
    port of a Mac recompiler with no stated licence (Banana Native).
- **Distribution risk.** Citron Neo's repository was taken down by a Nintendo DMCA notice in
  November 2026.
  - Proposal: Ultra's Switch, Switch 2 and PS3 apps are **fetched from upstream at install time,
    by pinned URL and hash**. Thimsy never mirrors them in its releases.
  - This follows the existing rule for proprietary flashing tools (`universal_installer.md`
    2.3). Thimsy's own repo then holds only a URL and a hash.
  - This is a decision for you: Q1.
- **Nothing Ultra needs from the user's console ships**: prod.keys, title keys, firmware, PS3
  firmware (PUP), Windows. This is the same rule as BIOS files. Nano's BIOS installer flow
  (`BIOS.md`) can grow entries for them.

---

## 6. Recommended profile per chipset, and each device's pairing

**Rule.** The recommended profile is the chipset's tier, lowered one step for each line of 5.1
the device misses (usually RAM). Every other profile stays selectable, with a label:

| Label | When |
|---|---|
| **Recommended** | The chipset's tier, after the device's RAM adjustment. Preselected |
| **Lighter** | Below the recommendation: runs well, fewer systems |
| **May run poorly** | One step above the recommendation |
| **Not recommended** | Two or more steps above |

| Chipset | Recommended | Left out if a heavier profile is chosen (can't start) |
|---|---|---|
| Allwinner H700 | **Small** | ARMSX2 (ARMv8.0 CPU) |
| Allwinner A133P | **Small** | ARMSX2 (ARMv8.0 CPU) |
| Rockchip RK3568 | **Small** | – |
| Allwinner A527 | **Small** | – |
| Rockchip RK3576 | **Medium** | ARMSX2 (A72 + A53 are ARMv8.0) |
| Unisoc T618 | **Medium** | – |
| MediaTek Helio G90T | **Medium** | – |
| MediaTek Helio G99 | **Medium** | – |
| Qualcomm SD662 | **Medium** | ARMSX2 (A73 + A53 are ARMv8.0) |
| Qualcomm SD G1 Gen 2 | **Medium** | *(verify the CPU)* |
| Unisoc T820 | **Large** | – |
| MediaTek Dimensity 1100 / 1200 | **Large** | – |
| MediaTek Dimensity 8300 | **Large** | – |
| Qualcomm SD865 | **Large** | – |
| Qualcomm SD G3x Gen 1 | **Large** | – |
| Qualcomm SD G3x Gen 2 | **Ultra** (12 GB+) | – |

The last column is about components that can't start at all. Ultra's components get their own
`requires=` when they are qualified: Eden, RPCSX and Winlator may have ISA or Vulkan needs to
verify.

**Device pairing.** Every device can choose all four profiles; the others are labelled by the
rule above.

| Device | Chipset | RAM | Recommended | Device layer needs |
|---|---|---|---|---|
| **Anbernic RG Cube** | T820 | 8 GB | **Large** | ABXY positional keylayout, 720×720 shaders, QS tiles, RGB, fan, DP fixes (device pack), `no_slide` |
| Anbernic RG556 | T820 | 8 GB | **Large** | 1080p AMOLED: no crop shaders; fan; its keylayout |
| Anbernic RG406H | T820 | 8 GB | **Large** | 4:3 960×720: no crop shaders |
| Anbernic RG406V | T820 | 8 GB | **Large** | Vertical body: controls layout |
| Anbernic RG476H | T820 | 8 GB *(verify)* | **Large** | 4:3 panel |
| Anbernic RG Slide | T820 | 8 GB *(verify)* | **Large** | Slide behaviour **on** (no `no_slide`) |
| Anbernic RG557 | Dimensity 8300 | 12 GB | **Large** | Mali-G615: MaliSX2 support to verify |
| Anbernic RG477M | Dimensity 8300 | *(verify)* | **Large** | |
| Anbernic RG477V | Dimensity 8300 | *(verify)* | **Large** | Vertical body |
| Retroid Pocket 4 Pro | Dimensity 1100 | 8 GB | **Large** | |
| AYANEO Pocket Micro | Helio G99 | 6 GB *(verify)* | **Medium** | 960×640 panel |
| KT Pocket KT-R1 | Helio G99 | 4 GB | **Medium** | |
| ZPG A1 Unicorn | Helio G99 | 6/8 GB | **Medium** | 720×720: shares the RG Cube's display layer |
| AYANEO Pocket Air Mini | Helio G90T | 2–3 GB | **Small** (RAM) | |
| Anbernic RG405M | T618 | 4 GB | **Medium** | 640×480 |
| Anbernic RG405V | T618 | 4 GB | **Medium** | 640×480, vertical |
| Anbernic RG505 | T618 | 4 GB | **Medium** | 960×544 OLED |
| Anbernic RG Vita | T618 | 3 GB | **Medium** | 1280×720 |
| Anbernic RG Rotate | T618 | 3 GB | **Medium** | 720×720: shares the display layer; swivel |
| Powkiddy X28 (planned) | T618 | 4 GB | **Medium** | |
| Mangmi Air X | SD662 | 4 GB | **Medium** | Adreno 610 |
| Retroid Pocket Classic | SD G1 Gen 2 | 4/6 GB | **Medium** | Vertical, 1240×1080 |
| Anbernic RG Vita Pro | RK3576 | 4 GB | **Medium** | 1920×1080 |
| GKD 350H Ultra | RK3576S | 2 GB | **Small** (RAM) | 1600×1440, Nano line |
| GameMT E6 MAX | A527 | 4 GB | **Small** | 1280×720 |
| Anbernic RG DS | RK3568 | 3 GB | **Small** | Two screens (Nano's dual-screen path) |
| Anbernic RG DS Plus | RK3568 | 1 GB | **Small** (below its 2 GB guidance) | See section 4 note |
| TrimUI Brick | A133P | 1 GB *(verify)* | **Small** | Nano line |
| MagicX Zero 40 | A133P | 2 GB | **Small** | Nano line |
| MagicX Mini Zero 28 | A133P | 2 GB | **Small** | 640×480, Nano line |
| MagicX XU Retro X20 V32 | A133P | 2 GB | **Small** | Nano line |
| Retroid Pocket 5 / Mini v1/v2 / Flip 2 (planned) | SD865 | 6–8 GB | **Large** | Adreno: Turnip |
| AYANEO Pocket AIR (planned) | Dimensity 1200 | (varies) | **Large** | |
| AYANEO Pocket DMG (planned) | SD G3x Gen 1 | (varies) | **Large** | |
| AYANEO Pocket S (planned) | SD G3x Gen 2 | 12–16 GB | **Ultra** | |


**Ultra on 8 GB devices.** Switch 2 and PS3 want 12 GB. On an 8 GB device they still install;
the installer notes that most games for them won't run there.

---

## 7. Packaging format and repository layout

### 7.1 Files

Keep the house style: line-based lists that scripts parse, plus one directory per layer.

```
packaging/
  profiles/
    small.txt  medium.txt  large.txt  ultra.txt
  socs/
    unisoc-t820.txt  unisoc-t618.txt  allwinner-a133p.txt  allwinner-h700.txt  ...
  gpus/
    mali-valhall/   mali-bifrost/   adreno-6xx/   adreno-7xx/   powervr-ge8300/
  displays/
    720x720/        (shaders/, retroarch overrides)
  devices/
    anbernic-rg-cube/
      device.txt    (soc, gpu, display, ram, capabilities)
      keylayout/    quick-settings.txt   build.prop.d/
mods/
  retroarch-cores.txt   + profiles=small,medium,large,ultra  + requires=
  added-apps.txt        + profiles=...  + requires=ram>=8G,isa=armv8.2,vulkan>=1.1
  nano-systems.txt      unchanged for system definitions; defaults move to profiles/*.txt
```

**Profile file:**

```
# packaging/profiles/medium.txt
extends        small
theme          esde colorful-simplified-es-de
default  nes   mesen
default  gba   mgba
default  nds   seedlessds  watermelonds
default  psx   pcsx_rearmed
default  saturn yabasanshiro
editable apps defaults
```

**Chipset file:**

```
# packaging/socs/unisoc-t820.txt
name         Unisoc T820
gpu          mali-valhall
isa          armv8.2
recommended  large
prop         persist.gammaos.performance_mode=2
```

**Device file:**

```
# packaging/devices/anbernic-rg-cube/device.txt
soc          unisoc-t820
display      720x720
ram          8G
capabilities fan rgb displayport touch
prop         ro.gammaos.nano.no_slide=1
```

**Chipset ids.** The SoC key and id scheme are settled by `multi_device_plan.md` Phase 1's
descriptor (its decision 19, still open). Its SoC-family list mixes `unisoc-ums9620` and `rk3568`
today; it recommends one vendor-prefixed scheme such as `rockchip-rk3566`. This plan's ids
(`unisoc-t820`, `allwinner-h700`) follow whatever it fixes.

A Python resolver (`scripts/resolve_package.py`, tested like `nano_systems.py`) reads these files.

- **Input:** a device id and a profile.
- **Output:** one resolved manifest:
  - the components and their sources;
  - the default emulator for each system;
  - properties and files, each with the layer that set it;
  - the components dropped, and why.
- **The mods** read that manifest instead of their hard-coded lists.
  - Example: `10-update-emulators.sh` installs the cores the manifest names, and
    `50-nano.sh` writes the defaults it gives.
  - A mod whose content doesn't apply simply finds nothing to do.

### 7.2 What goes where on the device

| Content | Phase 1 (now) | Later (`universal_installer.md` 3.4) |
|---|---|---|
| Emulator APKs, cores, first-boot payloads | `/system/etc` + `apps.list`, as today, built per (device, profile) | One system image per profile, shared by every device |
| Chipset and GPU properties, driver packs | system `build.prop` / `/system/etc` | `/odm/etc/thimsy/` |
| Display shaders, device keylayout, QS tiles, device props | system image, from the device layer | odm overlay in the device pack |
| Default emulators | `nano_systems.json` seeded at first boot | same |

Phase 1 keeps today's build (one system image per package). Every layer still lands in it, but
only the right layers do. Moving device content to odm later doesn't change the layer files.

### 7.3 `pack.json` additions

```json
{
  "device": "anbernic-rg-cube",
  "soc": "unisoc-t820",
  "profile": "large",
  "recommended": true,
  "fit": "recommended",
  "contents": {
    "emulators": ["retroarch", "ppsspp", "dolphin", "..."],
    "defaults": { "nds": "watermelonds", "psx": "mednafen_psx_hw", "...": "..." },
    "dropped": [{ "component": "armsx2", "reason": "needs an ARMv8.2 CPU" }]
  },
  "layers": ["base@<rev>", "profile/large", "gpu/mali-valhall", "soc/unisoc-t820",
             "display/720x720", "device/anbernic-rg-cube"]
}
```

`fit` is `recommended`, `lighter`, `may_run_poorly` or `not_recommended` (section 6).

---

## 8. Installer behaviour

- **Device.** The user picks the handheld, or it is detected. The installer reads the device's
  `soc`.
- **Profile picker.** The installer lists all four profiles. The recommended one is preselected,
  and the list says **Recommended** next to it, for example "Large (Recommended)". Each card
  shows:
  - the platforms it adds over the profile below;
  - the download and install size;
  - the default emulator for each platform, behind "Details".
- **Every profile is selectable.** Nothing is greyed out. Profiles above the recommendation
  carry "May run poorly" or "Not recommended", with the reason (for example, "Large suggests
  6 GB of RAM; this device has 4 GB"). Picking one shows that reason once more before the
  build; it doesn't ask for anything else.
  - Proposed exception: Linux images offer only Small and Medium (`linux_handheld_plan.md` §8.4
    and decision 12). Large and Ultra need heavy emulators that the RK3566 and H700 images don't
    contain. Not decided here: open question 8.
- **Left out** components (those that can't start on this CPU or GPU) are listed on the card.
- **Customise.** Within a profile's `editable` fields: add or remove an emulator, or change a
  platform's default. The extra apps go to the post-boot add-on pack (`universal_installer.md`
  3.4).
- **Where "recommended" comes from.** Thimsy stamps it into each package's `pack.json`. The
  installer catalog lists, per device, the profiles published and the recommended one, so the
  picker works before anything downloads. If the two disagree, `pack.json` wins.
  - The installer stays release-independent, as decided on 2026-11-15: it accepts any
    well-formed package.

---

## 9. Build, CI and validation

- **Build matrix.** Per release: every device × all four profiles.
  - With only the RG Cube today, that is 4 packages. Ultra's Switch, Switch 2 and PS3 slots stay
    empty until those components are qualified and their distribution is decided (Q1).
- **Checks** (new tests in `tests/`):
  1. Every profile gives every system either a default that ships in it or "absent". No default
     names a missing emulator.
  2. Every component a profile names passes the device's constraints, or appears in `dropped`
     with a reason.
  3. **Isolation.** A device's package contains no file from another device's or display's layer.
     For example, an RG556 package has no `720x720` shaders and no RG Cube keylayout.
  4. The package fits the device's free space in `super`.
  5. **Regression.** RG Cube Large equals today's `heavy` output byte for byte, until a later
     change intends otherwise.
  6. Licences: every shipped component has `license=` and `source=`. A component with no
     redistributable licence (aPS3e today) can only be `fetch=upstream`.
- **Sizes.** Measure each profile's payload once per release and put it in `pack.json` for the
  picker.

---

## 10. Rollout

| Phase | Work | Done when |
|---|---|---|
| 0 | Layer files and resolver. Move the RG Cube specifics from section 3 into `devices/`, `displays/720x720`, `socs/unisoc-t820`, `gpus/mali-valhall` | RG Cube Large is byte-identical to today's build |
| 1 | Small and Medium profiles; `profiles=` / `requires=` columns; `heavy` → `large`; `pack.json` additions | Three RG Cube packages build and boot; each profile's defaults launch a game for every system it shows |
| 2 | Installer: profile picker, Recommended preselection, fit labels with reasons, contents summary | The installer installs each RG Cube profile |
| 3 | A second T820 device (RG556) | Isolation check passes; RG556 Large boots with none of the RG Cube files |
| 4 | Further chipset families, in the installer's device order (`universal_installer.md` 4.4) | Per device qualification |
| 5 | Ultra: Eden, Winlator, GameNative, RPCSX; Switch 2 slot | A device in the Ultra column qualifies; licence and distribution questions answered |

---

## 11. Open questions for you

1. **Ultra distribution.** Should Switch, Switch 2 and PS3 emulators be fetched from upstream at
   install time (pinned URL + hash, never mirrored), or not shipped at all? Recommended: fetch.
2. **Labels.** Are "Lighter", "May run poorly" and "Not recommended" the right words for the
   profiles other than the recommended one?
3. **A system with no emulator in the profile.** Hide it in Nano (proposed), or show it with
   "Not in this profile; add an emulator"?
4. **Small's home.** Should it default to the Minima theme (proposed, lighter), or keep the ES-DE
   theme with fewer animations?
5. **Medium's DS default.** SeedlessDS (proposed: faster on T618 / G99) or WatermelonDS (today's
   choice)?
6. **The H700.** No GammaOS device uses it today. Keep it in the table as Small's reference chip for
   future devices (proposed), or drop it? `linux_handheld_plan.md` Phase 7 (H700 with its two
   images) and its decision 12 (H700 as Small) depend on keeping it.
7. **Platform selector** (section 2). How does a Linux image swap Android-only components: a
   platform layer, or a per-component platform column; and is the key `platform=` or
   `requires=platform:linux`? `linux_handheld_plan.md` §6.3 leaves the choice to this plan.
   Recommended: a `requires=platform:<name>` hard constraint on each platform-bound component, and
   each profile's default list names the Linux entry after the Android ones
   (`default nds seedlessds watermelonds <melonDS DS core>`). Android-only entries then drop on
   Linux and the platform falls through to the next default, so layers 3–6 still add nothing.
8. **Linux images and the no-gating rule** (section 8). `linux_handheld_plan.md` §8.4 and
   decision 12 propose that Linux images offer only Small and Medium, because Large and Ultra need
   heavy emulators those images don't contain. Accept it as an exception to "every profile on
   every device", or offer all four with fit labels? `multi_device_plan.md` §7.1 item 6 also
   speaks of "allowed" profiles per device. `linux_handheld_plan.md` decision 12 settles it for
   the Linux side.
9. **Shader names** (section 3). `multi_device_plan.md` §5 renames `rg_cube_*` to names like
   `thimsy_4_3_cropped`, with the old names kept as aliases; section 3 here says `720x720_*`.
   Recommended: whichever plan lands first settles the names, and the other follows.

---

## 12. Out of scope here

- Flashing, unlock routes, device packs for kernel and vendor fixes, the installer's engine:
  `universal_installer.md`.
- Changing the shipped emulators' versions, or Large's contents beyond the rename.

---

## 13. Sources

- GammaOS Next supported devices: upstream README, `github.com/TheGammaSqueeze/GammaOSNext`
  (fetched 2026-11-18)
- RG Vita / RG Vita Pro: Notebookcheck, "Anbernic releases RG Vita and RG Vita Pro…"
- RG Rotate, MagicX Zero 40, KT-R1, AYANEO Pocket Air Mini: handhelddatabase.com device pages
- RG DS / RG DS Plus: anbernic.com, "RG DS vs. RG DS Plus"; pocketretrogamer.com
- MagicX Mini Zero 28, XU Retro XU20 V32, GKD 350H Ultra: retrohandhelds.gg reviews and spec
  posts
- ZPG A1 Unicorn: handheldwiki.com; GameMT E6 MAX, Mangmi Air X, Retroid Pocket Classic:
  lostinconsoles.com
- RG477V Dimensity 8300; Powkiddy X28 T618; Mangmi Air X SD662: Notebookcheck and
  androidguias.com titles
- Switch 2 emulation status (Nov 2026): lovegame.space, "Switch 2 Emulator Status"
- Switch emulators on Android (Eden, Citron DMCA, Kenji-NX): lovegame.space, "Best Switch
  Emulator 2026"; Eden licence: `git.eden-emu.dev/eden-emu/eden`
- PS3 on Android: heldgames.com, "PS3 Emulation on Android Handhelds in 2026"
- PC on Android: tech-insider.org, "Winlator vs GameNative vs GameHub 2026"
- Licences from GitHub repository metadata: Winlator LGPL-2.1, GameNative GPL-3.0,
  RPCSX-UI-Android GPL-2.0, RPCS3 GPL-2.0, aPS3e none declared
