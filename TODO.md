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
  - keep the H700?

### Redistribution clean-up (plan_remove_unlicensed_files.md): partly done
- Step 0.3: `scripts/banned_files.txt` and a build check that fails on banned files.
- Replace the PS3 Rodin font (used by the XMB and Minima) and the six PS3 sound effects.
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
