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

- **Monitor resolutions other than 1080p60** (external_display_modes_plan.md §R). README still says a monitor always runs at 1080p 60 Hz.
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
