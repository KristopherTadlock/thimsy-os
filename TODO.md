# Thimsy to-do list

Built 2026-11-21 from the plan docs in this folder, each checked against the README and the source. Statuses in some plan files are stale; this list is the current view. Items that need a decision from the user are marked **(decide)**.

## Priority

1. **Network Shares security fix** (network_storage_plan.md phase 0). The shipping share client runs as root on an old libsmb2 with known memory bugs, and stores passwords in a form the public image can decode.
   - Update libsmb2 to 6.3.2.
   - Rename over an existing file, and SMB2 FLUSH on fsync.
   - A `statfs` fallback instead of reporting 0 bytes free.
   - Passwords out of properties: Keystore, AES-GCM, store only the NT hash.
   - `PR_SET_DUMPABLE` off.
   - A README section on Network Shares.
   - **(decide)** OK to ship this in the next build, ahead of the rest of the plan?
2. **Device checks that are still owed.** These need someone with a Cube; the code is in.
   - Dual Screen and Mirror: display_mirror_plan.md risks, dual_screen_plan.md risks.
   - The Docked checklist (external_display_modes_plan.md §8).
   - The button map checklist (button_map_overlay_plan.md §9.6).
   - First-boot game launch: capture a real first-boot log, then run acceptance tests 1-8 (first_boot_game_launch_plan.md).
   - The USB-C dock fix, which test 40 ships without a device test (the first run on a Cube is test 40 itself), and the display mode checks (Displays: USB-C docks and display modes).
3. **First-boot game launch fixes** (first_boot_game_launch_plan.md).
   - A: reload the config explicitly after setup, show "Preparing game library...", and give feedback on a launch tried during it.
   - B: add `mEsdeTheme` to the animation predicate, and record a pending reload while a submenu is open.
   - D: install `nano_systems.json` atomically in `mods/50-nano.sh`, and report a failure.
   - Launch diagnostics: config generation token, rejected-launch reasons.
4. **Multi-device support** (multi_device_plan.md). Phase 0 (facts from one firmware image per chipset family, no hardware needed) and the owner's decisions 1-6 (first device after the Cube, shared system image, devices without odm, scope, BROM unlock and patched SPLs, hardware) come before any code.
5. **Linux handhelds** (linux_handheld_plan.md). Decided 2026-11-25: a Thimsy Base (Buildroot, mainline kernel, Thimsy's own U-Boot, BusyBox init), a 3 s cold boot on RK3566, and ROCKNIX's coverage as the first goal, reached by "Thimsy Base + importer" (`thimsy-import` tracks ROCKNIX's hardware layer; families arrive in waves 0 to 4; decision 21). **SD-only (owner, 2026-11-25):** the Linux install never writes internal storage and erasing Android is dropped; writing a bootloader with a verified backup is deferred to later, so the Qualcomm families (wave 3) wait. Phase 0 spikes (the ARC-D's three SD-only routes, FlipBase ported to the ARC, the Panfrost and libmali A/B, the rotation contract, a `thimsy-padd` prototype) and the open decisions 2, 4 and 11 (first device, display stack, GPU driver: the most stable driver ships first), with 7 (the order of the six plans), 22 (kernel per family) and 23 (input), come before any code. Decision 18 (BL31) is decided for RK3566: the unit's own vendor BL31, as FlipBase does.

## Global shaders: multi-pass filters like RetroArch (user request, 2026-11-21)

- **Today:** the global shader runs in SurfaceFlinger, over the whole composition.
  - Built-in single-pass types: CRT Simple, LCD3x, LCD, Blur Fill.
  - The 4:3 and 8:7 Cropped surface scalers.
  - Three custom multi-pass chains already exist:
    - SkSL from `.slangp`: history and feedback are faked, and many shaders don't compile (CRT 26/116, LCD 36/80).
    - Vulkan `.slangp` (`custom-vk`): has history, feedback, LUTs and float/sRGB framebuffers, but waits on the CPU each frame.
    - GLES `.glslp` (`custom-gl`): up to 8 passes, no sRGB framebuffers, no history, no feedback.
- **To do:**
  - Make RetroArch presets work fully: per-pass `scale_type`/scale, `filter_linear`, `wrap_mode`, `mipmap_input`, `float_framebuffer`, `srgb_framebuffer`, `alias`, `frame_count_mod`, LUTs, `OriginalHistory`/`PrevTexture` (history), `PassFeedback`, and parameters.
  - One chain engine for both formats, with no pass limit.
  - Remove the CPU syncs and readbacks (`vkWaitForFences`, `readPixels`) so a chain fits the composition frame without added latency.
  - Run a chain on the game's own surface (the plan's `surface` target), not only on the whole composition, so the menus and overlays stay sharp. This needs per-surface render targets: their lifetime, resizing when the game's buffer changes, and composition-cache invalidation.
  - Per-shader parameters in Nano's menu (the `#pragma parameter` values), saved per preset.
  - Measure GPU cost and latency on the Cube's Mali-G57. Publish a compatibility list, as the README does today for the SkSL chain.
  - Fold in global_shader_extension_plan.md:
    - `#pragma gammaos_target surface|composition` (§4);
    - separate surface and composition slots (§5);
    - per-app profiles (§6);
    - touch remapped through the projection (§11);
    - geometry test image and cost measurement (§13).
  - The plan's status line ("not implemented") is stale and should be updated.

## Storage

### USB drives (usb_storage_plan.md): not started
- A drive model (`NanoDrives`) replacing `sdCardRoot`/`sdIsSlot`/`emuSdSlotCard`.
- Settings > Storage: **ES-DE Drive** and **Emulator Settings Drive**.
  - Removal falls back to internal storage, with a banner.
  - Copy From / Start Empty when the chosen drive has no files.
- Emulator settings adoption and sync on any drive. `repoint()` only to the chosen drive or internal storage. Migrate `emu_sd`/`emu_sd_card` and today's USB mixed state.
- Replace `esdeStoreFollowCard()`, and fsync after the ES-DE store's renames.
- "Move ... to Drive..." with a drive picker. Setup offers a USB drive when there is no card.
- Drive names on screen: "microSD card" / "USB drive <label>". Re-key about 60 strings in 21 languages.
- Refuse duplicate volume ids. Keep the 10-s grace.
- Reword the known issue about USB and the storage privacy rules.
- **(decide)** Internal fallback: A (a kept mirror, recommended) or B (plain)?
- **(decide)** Return to the drive automatically when it comes back?

### Network shares (network_storage_plan.md): not started, not to be implemented until approved
- Phase 0: see Priority 1.
- Phase 1: prove on a Cube (§4):
  - FUSE passthrough; a stub volume by hand; `getExternalFilesDirs`;
  - throughput; sleep and wake; mmap and locks; rename-over; 1 MiB reads.
- Phase 2: shares as vold stub volumes with stable UUIDs.
  - vold patches; no MediaProvider scan; the denylist enforced inside sharefs.
  - Nano drive kind "network"; correct launch links.
  - Depends on the USB plan.
- Phase 3:
  - an async context pool and libfuse's low-level API with caches;
  - BoringSSL crypto with GMAC and GCM;
  - reconnect and wake handling.
- Phase 4: streaming block cache, Keep on Device, warm on highlight, launch traces.
- Phase 5: Move Installed Content to Network (Vita3K, Eden, Dolphin, ARMSX2, PPSSPP); a library index and thumbnail cache.
- Phase 6: discovery (mDNS, WS-Discovery, NetBIOS), an Add Share wizard, clear errors, Wake-on-LAN, save backup to the share.
- Fix README: Dolphin does have settings for its Load and resource-pack folders. Review the libfuse static-link licence.
- **(decide)** Keep NFS, WebDAV and FTP, or hide them?
- **(decide)** Allow ES-DE and emulator settings on a share?
- **(decide)** Cache on internal storage only, or the card too?

### File sharing server (file_sharing_plan.md): not started
- Phase 1: libsmb2 server spike on the Cube, then the client compatibility matrix, then go/no-go.
- Phase 2: SMB MVP.
  - The `thimsy-fileshare` daemon with its SELinux domain, and a 0600 config.
  - Share roots on the lower filesystem with `openat2` beneath them.
  - mDNS (`thimsy.local`).
  - Settings > Network > File Sharing, with translations.
- Phase 3: FTP on port 2121.
- Phase 4: change notify, WS-Discovery, FTPS, SMB3 encryption, rescans, a status-bar icon.
- Also: a Wi-Fi high-performance lock, a failed-login lockout, `SCHED_IDLE` transfers, stress and game-impact tests.
- **(decide)** Drop root after binding?
- Shares the discovery code with the network plan.

## Controllers

### Controller layout (controller_layout_plan.md): partly done, differently from the plan
- **Built in test 36:** positional keylayout, ABXY Swap on by default, Nano un-swapping gammapad's pad so A confirms.
- **Test 37 (user's rule, 2026-11-21):** gammapad always gives the built-in (Nintendo-layout) pad by position, so the Cube's B is Xbox A in games; ABXY Swap (off by default again) swaps every controller. gammapad tags each face button's source layout (`EV_MSC`/`MSC_SERIAL`); Nano's menus read every pad by print; the DS player and Nano's Quick Resume players play by position, through the swap. Settles §16's question: ABXY Swap stays gammapad's input swap, for all pads.
- Only 484b:1101 counts as Nintendo layout; other handhelds' pads and a Switch Pro controller would need adding (Phase 1).
- A tester who saved ABXY Swap on in test 36 or earlier gets the swapped layout now (the saved value's meaning flipped).
- **(decide)** SeedlessDS's default map has DS A on BUTTON_A (the bottom button), WatermelonDS and drastic-nano on the right button (controller_layout_plan.md §1.7).
- A pad read directly (Controller Enable off, or a pad gammapad doesn't merge) keeps its print in the DS player and Nano's Quick Resume players.
- Phase 1: layout table (`families.json`, `face_layout`), per-device normalisers, the Xbox North/West and Switch Pro model fixes.
- Phase 2:
  - prompts follow the primary pad's printed labels;
  - a "Menus & Players" section: Merge Controllers, Primary Controller, Button Prompts.
- §11: migrate `abxy_swap`, `face_swap`, `face_glyphs` and the hotkeys.
- Phase 3: drastic-nano and the in-process runners use position-based roles.
- Phase 4: PhoneWindowManager tracks the primary controller; hotkeys by `FACE_*`.
- Phase 5: Merge Controllers actually merges (`persist.gammaos.gamepad.merge` is only logged today), and per-emulator positional provisioning.
- Phase 6: gammapad reads `_Version_` and `/product` keylayouts (a DS4 is scrambled today); per-source swap (done for the built-in pad in test 37).
- Phase 7 (optional): Identify Controller.
- README and HOTKEYS wording; the §13 device checklist.
- **(decide)** Q11 (other handhelds' layouts), and the §15 defaults "chosen pending objection".
- **Pending from the user:** RetroArch buttons with Controller Enable off reported wrong; waiting on the adb output (keylayout in use, `retroarch.cfg` binds, remaps).

### Button map overlay (button_map_overlay_plan.md): phases 1-5 done
- Phase 6:
  - pad drawings (`rg_cube`, `xbox`, `playstation`, `switch_pro`, `generic`);
  - an optional mouse-mode list;
  - a Table view in drastic-nano's panel.
- gammapad publishes the source pad's bottom button (`sys.gammaos.gamepad.south`).
- §10 side fixes:
  - README: the browser controls need Controller Enable;
  - stale HOTKEYS wording for drastic-nano;
  - default Dolphin (`GCPadNew.ini`) and Azahar pad binds;
  - hotkey rows labelled with Android keycodes.

## Displays

- **Monitor resolutions other than 1080p60:** implemented for test 40 on the 1.4.4 base, untested on a device; see "USB-C docks and display modes" below. external_display_modes_plan.md §R ("Resolution") still describes the old 1080p60-only rule.
- **Mirror mode** (display_mirror_plan.md): document it in README; the "External monitors" section still says nothing is mirrored. Known gaps:
  - clock drift between panel and monitor;
  - the boot splash on the panel only;
  - FLAG_SECURE layers black on the monitor;
  - shimmer below 720 lines;
  - touch depends on display order;
  - a two-panel device shows only the top screen;
  - unfiltered frames while switching.
- **Dual Screen known gaps to re-check on a device** (dual_screen_plan.md):
  - touch while the UI is on the monitor;
  - a panel app's keyboard only while it has the controller;
  - a stale monitor frame under a panel menu;
  - a menu staying on the monitor right after a game or plug-in.
- **(decide)** Confirm that brightness drives the Cube's screen in Dual Screen (marked "assumed").
- **(decide)** Unplugging the monitor closes the app on the Cube's screen. Keep that, or keep it open?
- external_display_modes_plan.md §8 host tests: check which still exist or are obsolete.
- external_display_routing_plan.md is superseded: archive it.

### USB-C docks and display modes (usb_c_dock_plan.md, display_modes_plan.md): implemented for test 40, nothing run on a device
- **Goal:** a powered USB-C dock gives power in, picture and USB at once; a monitor can run at modes other than 1080p60 (1440p60, 1080p120, later 4K30), chosen in Nano with confirm-and-revert.
- **Decisions taken** (dock plan §12; display plan §7):
  - D1 dtbo entry 2 only; D2 the PPS APDO dropped from sink-pdos; D3 device-tree route, no module patches; D4 DP-B ships, DP-A, DP-C and DP-D don't; D5/D11 no charging step 2 (unidentified sources stay at 1.05 A); D6 no F4; D7 source PDO v3 (5 V 900 mA); D8 no other devices' firmware; D9 back up first, recovery is a Full install; D10 the core c2c set (stage 1b) with `vbus-supply`, no `use-pdhub-c2c` on pd@e00, no 1d.
  - Test 40 ships the dtbo fix untested on a device (the owner's decision; no phase-1 hand test): `DOCK_FIX` defaults to 1 (`DOCK_FIX=0` keeps the stock dtbo), and `tested_sha256` in `scripts/patch_dtbo.py` holds `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536`, approved without a device test; `build.sh` still refuses any other image, and needs `fdtoverlay`. README tells testers to back up, be ready for a Full install, plug a dock's charger in first, and what to report.
  - Display modes: kernel route; link-bandwidth check with 1000 (no audio headroom); Auto stays at 1080p60 (stage 1); 3840x2160 only with Step 5 (hidden for now), DCI 4096 wide never; 1440p60 offered; kept per monitor model (a hash of `DeviceProductInfo`); modes below 50 Hz hidden in Dual Screen; 15 s with the cursor on Revert; physical and render range pinned to the mode's rate; no "render at 1080p, output 4K".
  - Every mode user-defined (1B) only with `SYSTEM_IMG` (`THIMSY_DP_ALL_MODES`, set by `build.sh`, in place of the plan's `--all-modes` flag); recovery never gets it.
- **Done:**
  - Build: `scripts/avb_footer.py`, `scripts/patch_dtbo.py` (pure Python, with an `fdtoverlay` merge check that `build.sh` runs against the build's vendor_boot, `fdtoverlay` from `device-tree-compiler` in the Docker image), the `DOCK_FIX` step in `build.sh`, `tests/unit/test_dtbo.py`. The approved image is `out/.dock/dtbo-c2c.img`, sha256 `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536`, checked offline against vendor_boot's base DT and `live.dtb`.
  - `scripts/sprd_drm.py`: the link-bandwidth check and the DCI reject (display plan 1A, 1C), DP-B as its own list (`HPD_PATCHES`), 1B behind `all_modes`. Checked statically against the original module only.
  - Framework (1.4.4): the monitor's mode filtered, held, tried, kept and reverted in system_server; the applying marker; `cmd display gammaos-monitor-mode`; `sys.gammaos.display.monitor_mode`.
  - Nano (1.4.4): Output Resolution, Refresh Rate, Keep This Resolution? and Forget Monitor Resolutions (`NanoMenuMonitorMode.cpp`, `NanoMonitorMode`); NanoTypec 4e items 1-4 and 7 (docks left alone, the flip branch, no dwc3 debugfs, port found by what it can do); `tests/unit/test_nano_units.py` builds both host test suites with g++.
  - Installer (repo thimsy-os, branch universal-installer): DTBO and DTBO_B written by Update and no longer read back, the `update`/`update_check` overlap check, `MIN_INSTALLER` 0.1.0-test20, e2e and package tests.
  - README: External monitors (the driver, Output Resolution, USB-C docks, the port lookup), Building (`DOCK_FIX`, `patch_dtbo.py`, `avb_footer.py`).
- **Still to do without a device:**
  - Display plan Step 3's SurfaceFlinger part: restart a monitor's vsync window (`gammaCheckMonitorVsync`) when its mode changes. Not implemented (§12 says so); without it a slow modeset can trip the "no composer vsync from monitor" fallback.
  - Step 5: client composition for a monitor at 3840 wide; then set `ro.gammaos.display.uhd=true` so 3840x2160 is listed.
  - A full `SYSTEM_IMG` build: the framework code was compiled only against stubs, and Nano's `NanoMenu.cpp`, `NanoMenu.h`, `NanoMenuMtp.cpp`, `NanoMenuPS3Menu.cpp` and `NanoMenuSettings.cpp` (the `ro.thimsy.release` suffix in System Information) edits were never compiled. The text shader's `highp` (`NanoMenuShaders.cpp`) is GLSL compiled only on the device.
  - A native speaker's review of the 19 new strings in each translation.
  - Run the installer's `installer/tests/e2e.sh` with a real `WORKSPACE` and `PACK_DIR` before release: `run-all.sh` skips it without them. Meanwhile the engine test `rg_cube_update_writes_dtbo_and_reads_back_the_rest` checks the RG Cube's update plan (both dtbo slots written, no dtbo read back, the rest of the boot chain read back).
  - Set the executable bit on `scripts/patch_dtbo.py` and `scripts/avb_footer.py` when committing (`git update-index --chmod=+x`).
  - Optional: `test_vendor_patches.py`'s footer tests onto `avb_footer`, then drop the `footer_info` wrapper in `patch_vendor_boot.py`.
  - Stale text: external_display_modes_plan.md §R.
  - Auto stage 2 and a "native" mark in Output Resolution: the framework can't tell the native mode once every mode is user-defined.
- **Needs the device:**
  - Phase 1a: back up, confirm download mode, `lpdump` and fastboot `--set-active` (Q6, Q11, Q12).
  - **The dtbo fix on a device, with test 40** (it ships untested; phase 1's hand test was skipped): boot, a plain charger, a dock with its charger plugged in first (power, HDMI, USB3 and USB2), an HDMI dongle or hub without a charger, and a self-powered dual-role USB-C monitor without PD (VBUS from tcpm `set_vbus` with c2c, never observed; test 40 is a 1.2-base build without NanoTypec, so only the first plug-in after a boot tests the fix, and the NanoTypec flip case waits for a 1.4.4 build); check the installed DTBO's sha256 is `94c0f3d4…`; an Update from a clean install and a Full install (phase 2). If it fails: a Full install of an earlier Thimsy or stock PAC, then debug against build 40 (dock plan §9 stage 1b on slot A) and record the results in dock plan §2.
  - Before test 40 leaves draft (DP-B changes today's behaviour): matrix rows 1, 3 and 5, and a dongle whose first Attention is 0x18a.
  - Display modes: display plan §6 for 1440p60, 1080p120 and (after Step 5) 4K30; the revert paths; the applying marker; SELinux for the `sys.` property and `/data/system/gammaos_monitor_modes.txt`; that Nano's `cmd` call passes the uid check; the side panels and dialog in every theme.
  - Nano's text shader at `highp` (`NanoMenuShaders.cpp`): it compiles on the Mali-G57 (no shader error in logcat at Nano's start), and glyphs packed past texel 1024 of the atlas are sharp. The atlas is 2048 texels tall on every device (`mAtlasH`, `NanoMenuRender.cpp`), so this applies everywhere, not only where it is 2048 wide.
  - DP-A (with variant (a), or with DP-D), phase 3, after phase 1; Nano's DP retry (4e.5) needs it.
  - Charging step 2 (phase 5): measure first; needs D5, D11 and Q5.
  - 4K60 over YCbCr 4:2:0 for video (display plan §8): only after the modes above work on the device.
  - Phase 6 dock matrix, phase 7 other T820 devices (D8), the Wi-Fi drops (dock plan §11), and the open questions Q1-Q13.

## Packaging

### Device profiles (device_profiles_plan.md): not started
- Phase 0: layer files and resolver (`devices/`, `displays/720x720`, `socs/unisoc-t820`, `gpus/mali-valhall`). The output must stay byte-identical.
- Phase 1: Small and Medium profiles; rename `heavy` to `large`; additions to `pack.json`.
- Phase 2: an installer profile picker with Recommended and fit labels.
- Phase 3: a second T820 device (RG556).
- Phase 4: more chipset families.
- Phase 5: Ultra (Eden, Winlator, GameNative, RPCSX).
- **(decide)** §11:
  - Ultra fetched from upstream, or not shipped?
  - label wording;
  - hide systems with no emulator;
  - Small's home theme;
  - Medium's DS default;
  - keep the H700? (linux_handheld_plan.md Phase 7 and its decision 12 depend on it)
- Phase 3 (RG556) is the same work as multi_device_plan.md phase 2.

### Multi-device support (multi_device_plan.md): not started (user request, 2026-11-24)
- **Goal:** every handheld GammaOS supports with an Android build (47 entries in scope; the two Konkr devices run GammaOS Pico, which isn't Android, and are excluded), from one tree, one shared system image and one installer.
- **Android 14:** 26 entries already run an Android 14 system on their vendor (25 a GammaOS build, the RP Classic its stock firmware), 6 likely work, 15 are assumed (plan §2).
- **Known or likely incompatible** (plan §2.1):
  - GameMT E6 Max: MTP crashes;
  - RP Classic made from March 2026 and RG557 on stock v1.26+: hardware revisions that boot loop;
  - Helio G99 devices: KeyMint/TEE OS-version lock;
  - RP Duo Lite: Android 14 system on an Android 15 vendor.
- **Works, with limits:** A133P vendors boot Android 14 but fail a strict VINTF check, and Android 14 is their last release; RK3566 has no Android 14 boot yet (an Android 12 GSI runs on the RG353P/V/M and ARC-D stock vendor); 1 GB devices (RG DS Plus, TrimUI Brick, TSP, most RK3566) are short of memory.
- **Bases:** only public GammaOS releases can be fetched: the RG DS is on v1.4.1 (v1.4.4 is supporter-only), and the RG DS Plus has no public download at all.
- [ ] Phase 0: unpack one image per family; record partition tables, super sizes, vbmeta, vendor props, kernels, power-supply names and maskrom IDs; diff GammaOS system images against Thimsy's; offline `checkvintf`.
- [ ] Phase 1: device descriptors and `build.sh DEVICE=` (the release-system build stays Cube only), container and logo backends, optional vendor steps, a byte-identical Cube golden test; `ro.thimsy.device` and Cube facts moved out of the shared system image; upstream's updaters (`gammaos-ota`, `NanoOtaCheck`) dealt with before any device that has a GammaOS OTA endpoint; installer Backend/Container traits, family files, typed recipes, identify probe and model check by readback, variants and preconditions.
- [ ] Phase 2: the other T820 devices, RG556 first, then RG406H, RG406V, RG476H, RG Slide.
- [ ] Phase 3: T618 PAC devices: RG Rotate, RG Vita.
- [ ] Phase 4: installer fastboot backend: RG405V, RG405M, RG505, the three Helio G99 devices, RP4 Pro, RP Classic.
- [ ] Phase 5: installer SD-card backend: RG DS, RG Vita Pro, GKD 350H Ultra, then the three 2 GB A133P devices (MagicX Mini Zero 28, Zero 40, XU20 V32).
- [ ] Phase 6: qdl (Air X), SP Flash (Pocket Air Mini, RG557, RG477M, RG477V), PhoenixSuit walkthrough (E6 Max).
- [ ] Phase 7: deferred: RP Duo Lite (after GammaOS 1.5.0 is public), RK3566 Core devices (after a test boot; kept: linux_handheld_plan.md decision 3, 2026-11-25, makes the ARC-D offer both Android and Linux Thimsy), TrimUI Smart Pro, RG DS Plus (also needs a public release), TrimUI Brick and other 1 GB devices.
- [ ] Alongside: package format 2 with a shared system artifact (a minimum-size system grown per device); signed remote catalog.
- **(decide)** The plan's §12 decisions 1-18. First: RG556 or RG Rotate as the first device after the Cube. Also: release-system builds Cube only (15), supporter-only bases (16), Format All on the Pocket Air Mini (17), upstream updaters (18).

### Redistribution clean-up (plan_remove_unlicensed_files.md): partly done
- Step 0.3: `scripts/banned_files.txt` and a build check that fails on banned files.
- Replace the PS3 Rodin font (used by the XMB and Minima) and the six PS3 sound effects. The Rodin replacement also blocks any Linux image (linux_handheld_plan.md §5.2).
- Replace the ASSETS.md REPLACE rows for the XMB:
  - icons, including the PSN and Walkman logos, and their normal maps;
  - the wave data;
  - `cinfo-bg-photogallery.jpg`;
  - the player control-panel icons.

  Drop the unused files (gradients, `noise.png`, `icon_fw_folder.png`, `wave_boot.bin`).
- Replace the DSi theme's Mirsany font (`dsvec.ttf`), its sprites, `menu_ambiance.wav` and the five navigation sounds.
- **(decide)** The ASK items: `icon_amb.png`/`texenv.png`, `canyon_normalmap.png`, and the 13 ES-DE controller badges.
- **(decide)** M64Plus FZ ships ad SDKs, so there's no complete source for it. Keep it or drop it?
- Step 6: NOTICE items and the vendor-blob origin note. §4: full check on a fresh flash.

### ES-DE on the 1.2 base (plan.md): mostly superseded
- Only matters for the 1.2-base build with a private APK.
  - Ask the ES-DE developers for permission and record the answer; no public build may include the APK without it.
  - Decide on the public-build preconfiguration step.
  - Release notes for installing a purchased APK.
  - On-device tests 1-5.

## Emulators

### User-installed emulators (user_emulators_plan.md): not started (user request, 2026-11-23)
- **Goal:** an emulator the user installs (example: DuckStation for PS1, which Thimsy can't ship) works like a pack-in. That means a launch option per system and per game, universal hotkeys, controller mapping and the button map, Move Emulator Files and the BIOS installer, all driven by an **emulator definition** file.
- **Today on 1.4.4:** a sideloaded DuckStation is already usable from Nano's catalog and ES-DE labels. Quit works through the fallback profile. Save, load, fast forward and rewind, the button map, card storage and All files access don't.
- **1.2 base:** ES-DE already lists DuckStation (Standalone). Everything else needs Nano (D8).
- Phase 0: DuckStation facts on a Cube: signer, launch templates, settings store, bind format, injected F-keys, BIOS folder.
- Phase 1: definition validator (Nano and Java), the deterministic profile precedence fix, Custom > Installed App..., a per-game Custom row, a `playerRepair` fallback, and `mods/hotkeys/duckstation.json` (launch only).
- Phase 2: hotkeys: provisioning into app data, provision before launch, Add Emulator... and Bind Mode, the Quit row in the button map.
- Phase 3: `ThimsyEmulatorRegistry` (signer pinning, consent), registry-owned All files access grants, Import Definition...
- Phase 4: declarative button map readers. Phase 5: data-driven storage groups and a definition BIOS slot. Phase 6: pad provisioning and players (with controller_layout_plan.md phase 5). Phase 7: a signed community definition index.
- **(decide)** D1-D10 in the plan's "Decisions for the user": profile vs separate definitions directory, community index, the All files access prop, fallback on uninstall, generic card storage, DuckStation's load and quit keys, 1.4.4 only or a 1.2 port, signer lineage, grants for imported definitions.

## Android launcher

### Thimsy Home: Nano as an installable Android home app (android_launcher_plan.md): not started (user request, 2026-11-24)
- **Goal:** Nano's features, UI and navigation as a launcher APK for stock-Android handhelds (Anbernic, AYN, Retroid Pocket), installable as the home app. Its game lists, media and theme are interchangeable with Thimsy OS through the `ES-DE` folder, `ROMs` and `Thimsy/theme.json`; like every device, it keeps its own emulator settings and picks in `emulator_config/devices/<id>/` (OA-46, `LIBRARY-FORMAT.md`).
- **Today:** Nano is a root native process tied to the patched framework (properties, raw evdev, DRM/SurfaceFlinger, `/data/system`), so nothing runs on stock Android. The library is portable only in store mode, which only a move to a card turns on. The installer speaks no ADB.
- **Approach (plan §1; D1 decided by the user, 2026-11-24):** one Nano source with a platform layer under it: today's code for the firmware, JNI to a Kotlin shell for the app. Features work by access level: plain, user grants (Accessibility, overlay), shell (Shizuku), root, firmware only (plan §6).
- Phase 0: facts and spikes, nothing run on a device yet:
  - build Nano outside AOSP with the NDK and list every non-NDK call. **Fetch, patch and list done 2026-11-24** (`tools/android-launcher/fetch_nano.sh`, `ndk_probe.py`; plan, "Phase 0 results"): 114 source files, 156,985 lines; 26 compile unchanged, 17 more need only easy headers; platform headers spread through `NanoMenu.h`, `NanoMenuDrm.h` and `NanoVideo.h`; 2,739 privileged uses, 1,639 of them property calls a link-time shim covers. Go so far. **Still open:** a build to object files with shim headers and FreeType, libpng and tinyxml2 from source;
  - draw the ES-DE theme in a plain activity;
  - the launch matrix (each pack-in emulator from an ordinary app) and the input matrix (what the Accessibility key filter sees per device);
  - hotkey drivers without system_server; store mode on the internal `ES-DE` folder; the licence and asset audit; vendor root switches.
- Phase 1: the platform seam in the Nano patch, firmware behaviour unchanged.
- Phase 2: launcher MVP: home role, ES-DE theme with Minima settings, store-mode library, launches, Applications, Android Games, keyboard, scraper, setup.
- Phase 3: the library contract: `LIBRARY-FORMAT.md`, `nano/format.json`, `names.json` and `index.json` without volume-bound paths, store mode on internal storage in the firmware, an ES-DE command and `custom_systems` reader on both sides, shared emulator definitions in `Thimsy/emulators`, round-trip tests.
- Phase 4: over a game: Accessibility key filter with `ThimsyHotkeyController`, the overlay window (Quick Menu, Applications, button map), Close Current App.
- Phase 5: emulator files: guided Move Emulator Files, Shizuku and root providers, hotkey provisioning, button-map readers and the BIOS installer by level.
- Phase 6: the installer's "Keep my Android: add Thimsy Home" journey: an `adb` module, `pack.json` format 2 (`kind: "launcher"`), catalog `launcher:` blocks, devices with no firmware source (AYN, Retroid Pocket 5 and Mini).
- Phase 7: XMB and DSi themes (after the redistribution clean-up), players, File Explorer, share client, Nano Keyboard as an input method, Kiosk Mode, second-screen devices. Phase 8: Thimsy Home as the Boot Android home.
- Shares work with: usb_storage_plan.md (ES-DE Drive = Internal Storage is the same switch as store mode on internal storage), user_emulators_plan.md (the emulator definition becomes the shared profile format), device_profiles_plan.md (profiles pick the launcher journey's emulators), controller_layout_plan.md Phase 1 (`face_layout`), multi_device_plan.md (one device descriptor for firmware, installer and app; one package format 2 covering both the shared system artifact and the launcher kind), plan_remove_unlicensed_files.md (blocks the XMB and DSi themes in an APK).
- **(decide)** D13 (new, from Phase 0): the scraper's built-in ScreenScraper developer credentials in an APK.
- **(decide)** D2-D12 in the plan's "Decisions for the user": name and package id, where the code lives, lowest Android version, distribution, whether Shizuku and root are in scope, store mode on internal storage by default, first themes, whether the app installs emulators, test devices, Thimsy Home as the Android-mode home, a card-less library export.

## Linux handhelds

### Thimsy OS on Linux firmware (linux_handheld_plan.md): not started (user request, 2026-11-24; base changed 2026-11-25)
- **Goal:** a Linux build of Thimsy OS for handhelds with no Android 14 base, with the same Nano UI, one card profile that works in Android and Linux devices, an installer that writes bootable cards, and boot time tracked from the first build. **Coverage (owner, 2026-11-25):** the broadest device support, initially ROCKNIX's (16 families, about 83 products, 97 DTBs, AYN, Retroid and AYANEO included); devices that already boot Linux first; the Odin 2 not a focus.
- **Base (decided 2026-11-25; decision 1):** a Thimsy Base in the FlipBase mould, not a ROCKNIX downstream: a Buildroot external tree forked from FlipBase, mainline kernel, BusyBox init with no systemd, no initramfs, an EROFS (or squashfs) root, Nano as the one frontend; one card per device.
- **Device layer (decided 2026-11-25; decision 21): Thimsy Base + importer.** `thimsy-import` reads ROCKNIX at a tag Thimsy chooses and writes kernel patch series (flattened, ROCKNIX's file names kept, each under ROCKNIX's licence with a provenance line; no author recovery for the about 160 bare diffs, owner 2026-11-25), device trees, `config.xml`'s DTB lists, firmware lists, and device facts as `device.json` data written fresh (no quirk script or InputPlumber YAML copied). U-Boot and the boot chain stay Thimsy's (FlipBase's series); ROCKNIX's U-Boot is only consulted. Imperative blocks are hand-ported with hash guards; CI compares against ROCKNIX's release images (`extract-ikconfig`, pre-fragment config, DTBs). Not chosen: hand imports per family; Nano as a ROCKNIX frontend package for the long tail; back to a ROCKNIX downstream; ROCKNIX-built hardware packs; Batocera's board tree (plan §3.1, §3.2).
- **Waves (decision 26, proposed):** 0 the ARC-D and ARC-S; 1 the rest of RK3566 and H700; 2 RK3326 image b, RK3399, RK3576, S922X; 3 the Qualcomm families through OpenABL, SM6450 included (about 32 to 35 Qualcomm products, depending on how variants are counted; the Odin 2 Portal and RP Duo as test units only), **deferred: later, needs bootloader writing**; 4 RK3326 image a, RK3588, not waiting for wave 3. AMD64 as QEMU only. Each product shows a status (built, boots, works, certified) instead of a support claim, fed by hardware-in-the-loop units, a Device check screen and opt-in tester reports (plan §2.4, §8.3, §11).
- **Boot target (decided 2026-11-25; decision 5):** 3 s cold on RK3566, power-on to Nano's first complete home frame (FlipBase's figure on the same SoC). 1.5 s is the owner's stretch goal: measured, not promised; realistic for wake from sleep or for the launcher's start. Other families have guessed targets; Qualcomm states measured totals (plan §9.4).
- **SD-only principle (owner, 2026-11-25):** "Putting Linux on an SD card should always be reversible and non-destructive." Now: the Linux install never writes internal storage; removing the card gives back the untouched device; erasing Android is dropped permanently (no erase guide, no last resort). Later, deferred and not excluded: writing a bootloader to internal storage with a verified backup (ARC-D route (c); OpenABL on the Qualcomm families, so wave 3 waits) (plan §1, §8.6).
- **First device (open question 2 answered; decision 2 reopened):** the owner's RG ARC-D, keeping Android on eMMC and booting Thimsy from card slot 1. Phase 0 reads the eMMC loader read-only, then tries three SD-only routes in order: (d) Anbernic's own eMMC U-Boot boots our kernel from SD directly (cheapest, may not boot a mainline kernel); (a) the stock eMMC loader hands off to our card's mainline U-Boot (JELOS and FlipBase precedent); (b) the vendor U-Boot chainloads it. Pass: boots from slot 1, Android untouched with the card out, deep sleep works, loader time measured. If none passes, the ARC-D waits for bootloader writing and the ARC-S becomes the first device; the RG353P/M/V follow the ARC-D. Recommended beside it: a Linux-only reference unit (RG ARC-S) for the 3 s number and the 1 GB floor (plan §8.6, §11.1).
- **Card contract (`LIBRARY-FORMAT.md`; plan §7; decision 8; OA-46 to OA-54):** the drive mounts at `/storage/<VOLID>` on Linux too; no symlink ever lives on a drive. Every device using a card shares the library, gamelists, collections, media, playlists, the theme, BIOS, emulated system storage, in-game saves and save states. Saves and states are kept apart per core: RetroArch's in `retroarch/saves/<family>/<system>/` and `retroarch/states/<core>/<system>/`, so cores of different save families (N64 and DS across the platforms, for example) never share a file (OA-21, OA-52, OA-53). Emulator settings and picks are per device (each device's set in `emulator_config/devices/<id>/`), not shared between platforms or devices. Emulator definitions drive all of it (OA-54). Thimsy's own settings and PortMaster stay on the device. `/storage/roms` is gone with ROCKNIX.
- **"Paper Plane" is FlipBase or BaseOS** (owner, 2026-11-25). FlipBase is the boot reference and the template for the base (plan §9).
- Phase 0: spikes, no product code:
  - the ARC-D dual-boot routes, nothing written to the eMMC: read the eMMC loader read-only, then (d), (a) and (b), each with deep sleep and the loader's boot-time cost measured;
  - FlipBase built unmodified (amd64 cross build in Docker), then ported to the ARC on a card: panel, pad, touch, Wi-Fi chip, audio, sleep current with the unit's vendor BL31 (decision 18; rkbin's as the fallback to confirm), boot timeline; no serial console (on-screen U-Boot, USB gadget console, arch-counter timing);
  - the DRM hand-off with RetroArch and PPSSPP on Panfrost; the GPU A/B (Panfrost against libmali on the ARC-D across the v1 emulator set; v1 ships the more stable one, performance second; both switchable until then); the rotation contract on the ARC's portrait panel (Nano, first cuts of the SDL3 KMSDRM and RetroArch `drm_ctx.c` patches, plane against blit, touch); a `thimsy-padd` prototype on the ARC's three input nodes (hiding under libudev-zero, intercept with a held button, an external pad);
  - build Nano outside AOSP (shared with android_launcher_plan.md Phase 0, whose fetch, patch and list are done: only the build to object files with shim headers and FreeType, libpng and tinyxml2 from source is left);
  - mount a Cube-made card at `/storage/<VOLID>` on the spike image and load a save;
  - a Windows raw-write spike (broker, lock, write, verify).
- **Where the work happens (owner, 2026-11-25):** a fork of this repository (the nano fork). The refactor lands there first, then the Linux build; this repository keeps moving and is merged into the fork often; the fork merges back into nano or mainline later (plan §4.3). Builds run on this machine, in a WSL2 distro on the 21.8 TB D: drive, one build at a time (16 GB RAM). Proposed build shape (decision 25): a userland per CPU tier, GPU packs (Mesa Panfrost, Mesa Freedreno), a hardware pack per family; a full rebuild roughly 10 to 20 h *(guess)*, incremental much less. GitHub-hosted runners are an optional later CI path that needs chained jobs (plan §10).
- Phase 1: the `NanoPlatform.h` seam, `compat/` shims, CMake, the config store. Same work as android_launcher_plan.md Phase 1; the Cube image stays byte-identical.
- Phase 2 (wave 0): Thimsy Base and the Linux alpha on the ARC, as a plain `.img.zst` per device. 2a, the base: FlipBase forked and made multi-board in the tier and pack layout, BaseOS-derived files per the owner (most replaced by Thimsy's own parts, the four C files used as-is), RK3566's kernel version chosen from the ROCKNIX tags of the day, a hand import of RK3566 in the importer's layout with FlipBase's series as Thimsy's overlay, the ARC-D and reference-unit boards, `thimsy-session` (mounting, hotplug, power key, sleep, launch hand-off), A/B update, RetroArch and core recipes, `make legal-info`. 2b, Nano: KMS/GBM display with the rotation contract (the SDL3 patch offered upstream through SDL issue 14905, the RetroArch patch), `thimsy-padd` and the ARC pad, config store, fork/exec with the DRM hand-off, Wi-Fi through `wpa_ctrl`, BlueZ with D-Bus on demand, ALSA, `thimsy-fonts`, boot instrumentation.
- Phase 3: card interop v1: `LIBRARY-FORMAT.md`, the drive model, the immutable mount point, links and re-pointing, the per-system core table, emulator definitions per `user_emulators_plan.md` §1a (`schema` 1 with `launch.linux`; the plan's schema 2 draft is superseded), `card.json`, the per-device emulator sets and the card theme (per_device_config_plan.md, below; `systems.linux.json` is retired), and the Android-side changes (no label writes, not overwriting unknown launch tuples, dialog wording).
- Phase 4: installer: the SD-card writer in the reserved `sdcard` crate (pulled forward from multi_device_plan.md Phase 5; decision 7), `pack.json` format 2, the card picker, the ARC-D's dual-boot card layout (no step writes the eMMC), the storage job (below), images with bmap and `legal-info` sources.
- **Storage job (owner requirement, 2026-11-25):** the universal installer writes microSD game cards and USB drives as a first-class job, with the same flow as the firmware install (pick device, pick card or drive with the same safety rules, pick contents, write, verify), for Android and Linux devices. All four parts: (1) format and lay out (exFAT, basic-data type, non-zero serial, the card contract's folders `ES-DE/`, `ROMs/<system>/`, `emulator_config/`, `bios/`); (2) pack-in games, art and gamelists for the chosen package profile; (3) ROMs and BIOS copied from the PC into system folders, BIOS checked by hash; (4) a non-destructive update or repair of an existing card, never a format. Replaces the plan's "Prepare a game card" step (plan §8.4; also a requirement in `universal_installer.md`).
- Open the ARC-D and wire its serial pads (tx/rx/gnd, 1.5 Mbaud, 3.3 V, CP2102N/FT232 adapter): owner, later. Phase 0 starts without it (plan §11.1, open question 13).
- Linux v1 release: RetroArch hotkeys (its own menu) and Close Current App, scraper, BIOS manager, File Explorer, setup wizard, update file, PPSSPP and ScummVM recipes, the ARC's HDMI in Docked mode (Nano and games on the TV, panel off; mirroring later), 3 s on the reference unit.
- Phase 5: Nano's Quick Menu through an in-process libretro host and padd's intercept mode; a DRM-lease spike for standalones; button map overlay. Phase 6: boot time and the 1.5 s stretch measured. Phase 7, coverage waves 1 to 4: 7a builds `thimsy-import` (generator, hash guards, release-image check, catalog with status levels, Device check, hardware-in-the-loop rig), done when it regenerates the RK3566 hand import and imports H700; 7b runs the waves, about 1 to 2 weeks per family plus testing, and per-device quirk hooks, pad sections and rotation and touch checks (wave 1's H700 needs the H700 kept in device_profiles_plan.md, its §11 question 6). Phase 8: bootloader writing with a verified backup once allowed (ARC-D route (c), wave 3), shared save states for cores that pass the state test *(superseded, OA-53: states are shared per core now; only question 11's verification remains)*, HDMI mirroring, heavy emulators on Qualcomm, media apps, Quick Resume, dual-screen layouts.
- Effort: about 22 to 36 person-weeks to the alpha and 38 to 62 to v1 on RK3566 *(guess)*; the importer 4 to 8 *(guess)*; the 14 further families 20 to 40 *(guess)* plus per-device work; **the whole program about 60 to 100+ person-weeks** *(guess)* (plan §12).
- Shares work with: android_launcher_plan.md (seam, library format), multi_device_plan.md (descriptors, SdCard backend, package format 2), device_profiles_plan.md (resolver, Small and Medium), user_emulators_plan.md (definitions), usb_storage_plan.md (drive model), controller_layout_plan.md Phase 1 (`face_layout`). Plan §4.5 names an owner for each piece.
- **Owner answers, 2026-11-25 (second batch; plan §17.5):** question 11, save states load across same-commit 64-bit cores, "yes, but we need to test it" (per core: save on the Cube, load on the ARC and back; shared later, after v1) *(superseded, OA-53: states are shared per core now; only question 11's verification remains)*; 12, test both GPU drivers, ship the most stable first; 13, serial later (to-do above); 14, Docked HDMI in v1, mirror later; 20 and 21, Phase 0 items with a pass rule; 22, "I don't care who wrote it. Use the ROCKNIX license." (no author recovery; a provenance line per patch); 23, the recommended recipe approach (our own recipes, ROCKNIX's `package.mk` ported into `gpl/`, cores built from the commits of `retroarch-cores.lock`), not objected to; 24, "use the FlipBase TF-A" (decision 18 for RK3566); 26, don't weigh the BaseOS licence (most files replaced anyway; the four C files used as-is). Questions 7 and 17 are moot (no erase). Question 10 is still being researched.
- **(decide)** The plan's §14 decisions. Decided: 1 (base; amended by 21: ROCKNIX's hardware layer is a tracked upstream, its runtime unused), 3 (the Android build stays, maintained separately; the ARC-D will offer both), 5 (boot target), 18 for RK3566 (the unit's own vendor BL31 with FlipBase's pm-config patch, never redistributed; rkbin's as the fallback; other families as proposed) and 21 (device layer: Thimsy Base + importer). Reopened, each with a recommendation: 2 (first device: ARC-D dual boot by an SD-only route plus an ARC-S; erasing Android out by the SD-only principle), 4 (display stack: KMS/GBM hand-off first, no compositor; amendment proposed: the rotation contract confirmed for the Mali families only, a per-family display setting on Qualcomm, a per-game kiosk fallback), 6 (an MIT build tree with a `gpl/` area), 9 (real ES-DE: not in v1), 11 (GPU: Panfrost, GLES, no Vulkan; extension proposed: Mesa only on every family; amended by the owner: v1 ships the more stable driver from the Phase 0 A/B, both switchable until then), 12 (package profiles), 13 (image distribution; reopened again, proposed: the installer assembles each card from verified parts, which would supersede the earlier complete-images recommendation), 14 (H700 panel: variant images), 16 (public alpha: credit for imported patches, each with a provenance line), 17 (one clause moot). New: 19 (fork FlipBase at a pinned commit), 20 (card identity on the Cube), 22 (kernel per family: ROCKNIX's version at a Thimsy-chosen tag, skipping tags freely; about 6 version changes a month across families; RK3566's version chosen at Phase 2a), 23 (input: `thimsy-padd` everywhere, InputPlumber never in the image), 24 (firmware: ship only known-licence `linux-firmware`, fetch the rest from the user's device), 25 (image structure: tiers, GPU packs, hardware packs), 26 (coverage waves, wave 3 deferred until bootloader writing is allowed, and per-product status). Still open: 7 (the order of the plans), 8, 10, 15. New open questions 28 to 35 include ROCKNIX's tag cadence (33), stable updates on its .0 kernels (34) and Vulkan rotation on Qualcomm (35).

### Non-root Nano on Linux and Android (nonroot_nano_plan.md): not started (follows OA-14, 2026-11-26)
- **Goal:** Nano runs below root on both builds, with privileged work behind the platform layer (OA-4): `thimsy-session` on Linux; init services, NanoNetBridge and a small root helper on Android.
- **Today:** Nano is root on both.
  - Linux: every capability except `CAP_MKNOD`, inside NanoJail (OA-14 accepted the partial eMMC guard).
  - Android: root in the reused `bootanim` domain with 11 capabilities, on a build that is permissive device-wide.
  - Every handheld distro checked runs its frontend as root too. ArkOS's `ark` user has `NOPASSWD: ALL` sudo.
- **Linux (recommended: before the alpha; about 10 to 16 days, guess):** almost no shared code changes.
  - A `nano` user, with device-node groups set by the session (there is no udev).
  - A group-owned config store and padd socket.
  - exFAT and vfat mounted with `uid=`/`gid=`.
  - A one-time chown of `/storage`.
  - Session verbs `wifi:`, `bt:` and `perf:`.
  - RLIMIT_RTPRIO, RLIMIT_NICE and RLIMIT_MEMLOCK in place of capabilities, then `setresuid` and `no_new_privs`.
  - The session checks that the game pid Nano names is its own.
  - A non-root mode in the headless E2E.
- **Android (recommended: groundwork now, switch later; about 31 to 51 days, guess):**
  - A1: remove `nsenter` and drop `SYS_ADMIN`, `SYS_PTRACE` and `SYS_CHROOT` at the next Android-surface batch.
  - Fold the seam moves (Wi-Fi and Bluetooth to NanoNetBridge, `Power`, `AppControl`, `AppFiles`) into the OA-4 migration.
  - Then a `thimsy-nanod` helper and a switch to uid `system` with `SYS_NICE` and `BLOCK_SUSPEND`.
- **Decided (2026-11-26, DECISIONS.md OA-27 to OA-31):** user `nano` (1000) before the Linux alpha; games run as `nano`; idmapped mounts for PC-made ext4 drives, else ask; the session runs the Wi-Fi and Bluetooth scripts; Android moves to uid `system` with `thimsy-nanod` and towards SELinux enforcing (Nano's domains first, device-wide later); one interim release may keep `DAC_OVERRIDE`.

### Per-device emulator config and the theme on the card, Linux and Android (per_device_config_plan.md): not started (OA-46 to OA-54, 2026-11-27)
- **Goal (owner's rethink of config portability):**
  - Emulator settings (RetroArch, PPSSPP, ScummVM and the rest of `emulator_config/`) and emulator picks are per device. One card holds several devices' sets side by side in `emulator_config/devices/<id>/` (OA-46).
  - A per-device "SD card settings" switch copies the device's set to the card. The device always keeps a local mirror; with the switch on, the card's set wins and changes go to both.
  - Thimsy's own settings stay on the device (OA-47), except the theme: the theme switches (XMB, ES-DE, Minima and the like) and each theme's settings are on the card in `Thimsy/theme.json`, shared by every device, card first, the local value with no card (OA-51).
  - Playlists, media, gamelists and collections are on the card only, with no mirror (OA-48, OA-49). PortMaster is local (OA-50).
  - Emulated system storage (Vita3K's `ux0`, Eden's NAND, Dolphin's Wii NAND and the like) is shared on the card, with no device mirror; the emulator's settings stay per device (OA-52).
  - In-game saves, save states and BIOS are shared on the card, kept apart per core, with no mirror. RetroArch's saves sit in a fixed folder per save family and system, so no change of picks can move or hide one (OA-53, plan §2.7).
  - Everything an emulator integration needs comes from editable emulator definition files, the shipped emulators included; no per-emulator cases in code (OA-54, plan §2.8, `user_emulators_plan.md` §1a).
- **Today:** every Android device using a card shares one emulator set (`NanoEmuSync`, the emulator folders on the card), and picks travel in the card's ES-DE labels and `emulators.json`. The theme is local properties only.
- **Design (plan §1 to §6, proposed):** a set id of the platform plus a hash of the unit's chip serial (`linux-9c1d0e7a42b35f86`), so a dual-boot ARC-D keeps two sets on one card; `device.json` written last; `picks.json` per entry; three-way sync per file, the card winning; Android's live slots in `emulator_config/<emu>/` filled before each launch, with a marker; ES-DE labels never written after migration; `theme.json` with 31 keys, four mirrored into `es_settings.xml`; the aspect ratio local. The card contract is in `LIBRARY-FORMAT.md` 4.24 to 4.26.
- [ ] Phase 0: the docs, the three schemas and fixtures, `prefs.json`'s `card` field. LO, 2-3 days *(guess)*. Done on step/w9-config, not yet merged: `LIBRARY-FORMAT.md`, the `emu-set`, `picks` and `theme` schemas, the card fixtures and `test_card_contract.py`, this entry, `prefs.json`'s `card` field on the 31 theme rows with `test_prefs.py`, the superseded notes in `docs/linux/INTEROP-AUDIT.md`, its raw reports, `linux_handheld_plan.md`, `usb_storage_plan.md`, `portable_settings_plan.md` and `docs/linux/research/portmaster.md`. Still open: `docs/linux/IMPLEMENTATION.md`'s new interfaces. The `emu_card_sets` row is added with the code that reads it (phase 3).
- [ ] Phase 0b: emulator definitions: `user_emulators_plan.md` §1a's fields in `NanoEmuDefs` and `linux/schema/emulator-def.schema.json`, the shipped definitions carrying today's tables (`NanoEmuSync`'s `items()`, `raDirKeys`, the link lists, RetroArch's argv), and the no-emulator-cases lint. AS + LO, 1-1.5 weeks *(guess)*. Needs `user_emulators_plan.md` phase 1's loader, or is its first part.
- [ ] Phase 1: device identity: `nanoplat::DeviceIdentity` on firmware, Linux and host, `NanoDeviceId`, thimsy-session publishing `/run/thimsy/unit-serial`. AS + LO, 2-3 days *(guess)*.
- [ ] Phase 2: per-device picks: rework wave 8's shared-pick code (OA-38, `step/w8-emu`, on `linux-next`); `picks.json`; systems adoption keeps the device's launch fields; labels never written. AS, 1-1.5 weeks *(guess)*. **`step/w8-emu`'s shared-pick code on `linux-next` must be reworked before `linux-next` merges into `linux`** (OA-46): kept are `nanoplat::Emulators`, the seeded image defaults and the fallback chain; reverted are Linux writing labels and `emulators.json`, an unrunnable label kept as a pick, and the store-mode heal skip (plan §7). Its `LIBRARY-FORMAT.md` rewrite (f933326, 350fcb8, f303d13) gives way to step/w9-config's at the merge.
- [ ] Phase 3: emulator sets: `NanoEmuSets`, `NanoEmuSync` generalised, `DeviceStore` and `EmulatorFiles`, the live slots and the slot check, parked sets for single-card Linux, the switch and its Settings rows, the before and after syncs, the pointing methods, the shared storage and the per-family save and per-core state folders with the one-time copy of beside-ROM saves (plan §2.7), Android's migration and its one question. AS + LO, 3.5-5 weeks *(guess)*.
- [ ] Phase 4: the theme on the card: `NanoThemeCard`, the call sites in `NanoMenuEsdeMenu.cpp` and `NanoMenuPS3Menu.cpp`, the ES-DE mirror, theme downloads to the card (G14). AS, 1-1.5 weeks *(guess)*.
- [ ] Phase 5: playlists on the card (OA-48), `nano_music.json` as the scan cache. AS, 3-5 days *(guess)*.
- [ ] Phase 6: host scenarios (two devices on one card image, single-card Linux moved between units, two units with one serial, a card from a shipped build), the card contract checks, the ARC-D round trip at the next build window. AS + LO, 1 week *(guess)*.
- About 9.5 to 13 person-weeks in all *(guess)*. Phases 0, 0b and 1 can start at once; 3 needs 0b and 1; 4 needs 1 and does not need 2 or 3. Every AS step is an `android-surface:` commit, cherry-picked after the AOSP build; nothing is compiled until the next build window (OA-36).
- **(decide)** The plan's §9, 28 questions (13 answered by OA-53), among them: the device's own theme while a card's is shown, a theme switch, the borderline theme keys (sounds, wallpaper images), taking ES-DE app theme edits back, ES-DE labels after migration, the switch's off state and defaults, the id form, RetroArch's live config on Android, cards shared with shipped builds, scraper preferences, `systems.json`'s shared fields, where RetroArch saves live and the beside-ROM saves and states shipped builds left, shared storage with the card out or on internal storage, user definitions on the card, PortMaster's space on RK3566, how Linux points an emulator at the card, MAME's `cfg/`, a save of another family beside the ROM, RetroArch's loading menus during a Nano launch, a core whose states or saves fail the cross-platform test, and games of one name in two subfolders of one system.

### Portable settings on the card, Linux and Android (portable_settings_plan.md): superseded in part (OA-47, 2026-11-27)
- **Dropped:** the card file. OA-47 makes Thimsy's own settings local only: no `Thimsy/settings.ini`, the device's settings store is the only copy, and Nano does not write ES-DE's `ThemeSet` for portability. OA-32's answers and the plan's §8 questions are closed.
- **Except the theme:** the theme switches and each theme's settings go on the card in `Thimsy/theme.json` (OA-51), done under per_device_config_plan.md's phase 4 (above), not this plan.
- **Still applies:** phase 0's inventory, `tools/nano-tree/prefs.json` with `prefs.py` and `test_prefs.py` (done, and it stays as the inventory); the device store (Android properties, Linux `ConfigStore`); OA-35's seeding of theme defaults; the `Prefs` facade in front of wave 6's `Settings` (plan §6 and phases 2, 3 and 5, OA-4 migration step 8), now with no card layer except the theme's.
- **No longer planned:** the INI dialect and file, the card merge, migration to the card, and phases 1, 4, 7, 8 and 9.

## Scraping

### ScummVM scraping (scummvm_scraping_plan.md): listing done, scraping phases open
- Phase 0: measure which `romnom` spelling ScreenScraper accepts, `jeuRecherche` on systems 123 and 135, folder queries.
- Phase 2: a second-spelling fallback; the ini description as the search title; cached ScreenScraper ids; a 30-day not-found back-off.
- Phase 3: a bundled `games.tsv`; TheGamesDB by title; the original-platform fallback (DOS, Amiga, Windows); Beneath a Steel Sky's pack-in media.
- Phase 4 (optional): detection fingerprints.
- **(decide)** §4 Q1-7 and R.8 Q8-10; some may be moot now that the listing comes from the ini.

## Small items from this session

- **Game list folders:** removing a default ROMs folder (e.g. ROMs/snes) removes it on every drive at once. **(decide)** Remove only the chosen location instead?
- **Dual-screen text diagnostics:** test 36 logs the panel's text drawing state every 10 s. Remove it once the block letters are confirmed fixed.
- **Mixed-up translations noticed in review:**
  - ru "Remove" and ja "Remove" read as "delete";
  - de "Open" is "Offen" where "Öffnen" is meant in places.
