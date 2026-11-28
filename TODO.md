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

### Rebindable hotkey combo (hotkey_combo_plan.md): not started (user request, 2026-11-27)
- **Goal:** the Start + Select hotkey root becomes a setting, **Hotkey Combo** (Gamepad Settings → Hotkeys): Start + Select (default), L3 + R3 (only where `device_keys.json` says the built-in pad has stick clicks), or Off. It replaces *Start + Select Hotkeys* (`persist.gammaos.hotkeys.combo`, migrated from `persist.gammaos.hotkeys.startselect`). With the default, the only change is that the button map hides the combo row while Controller Bindings is Off.
- The chosen combo does all Start + Select does over a game: combo + hotkey button, 3 s hold to quit, Controller Bindings Off turns it off. Its buttons still reach the game. L3 + R3 must be pressed within 200 ms of each other. In Nano's menus Start + Select always opens the Quick Menu, and the combo does too.
- Every text naming the combo uses the chosen one: `comboText` (`%1$s` function button, `%2$s` combo, both required; translations may reorder them) and `comboName` (one `%s`, the combo), each with an Off variant. The Controller Bindings rewording job's description and toast become those keys; its cross-reference is retired with the *Start + Select Hotkeys* row.
- [ ] Phase 0 (0.5 day): owner decisions D1-D10 (§0), Q1 (which pairs) first; upstream drastic-nano `btnL3`/`btnR3`, NanoMenu.h `mStartHeld`, gammapad MouseMode combo; RetroArch payload binds and Hold Start delay; emulators' own L3/R3 binds; `stick_clicks` per device from `getevent -lp`.
- [ ] Phase 1 (2.5 days): Java, C++ and gammapad resolvers with shared cases, `stick_clicks`, the Hotkey Combo row, `comboText`/`comboName` and the i18n checks (source pattern, both placeholders, positional reordering), Controller Bindings, toast and Quit Game Button texts, migration.
- [ ] Phase 2 (2 days): PhoneWindowManager: decision pulled into tested pure functions, then the detector, other-button-held list, gating and the 200 ms window on the resolver (read per event).
- [ ] Phase 3 (2 days): drastic-nano detector (pad buttons and hat as "other held", not all of `heldKeys`); Nano's menus with per-button hold and two pairs; combo + Button Map Button uses the pair's own codes.
- [ ] Phase 4 (1.5 days): button map combo row in `chordRows` from pad labels (hidden when Off or Controller Bindings Off), `kComboSouth`, browser.json `HOTKEY_COMBO`, gammapad chord only on the chosen pair and L3/R3 tab shortcuts on release, scene goldens.
- [ ] Phase 5 (1 day): RetroArch `input_menu_toggle_gamepad_combo` 2 → 7 (Hold Start) while the combo is L3 + R3 and Universal Hotkeys is On (shipped value only, with a marker, restored after; run in provisioning before its enable check, on the setting change and before starting RetroArch); "Hold Start" button map row; M64Plus FZ note.
- [ ] Phase 6 (1.5 days plus device time): 21 translations, retired keys removed, HOTKEYS.md, README, button_map_overlay_plan.md; the §9 device checks.
- Phases 2-4 ship together; phase 5 with or before the L3 + R3 option. Effort: about 11 days plus device time. Linux port's Nano copy follows.
- **(decide)** D1-D10 (§0): D1 pairs (recommended Start + Select, L3 + R3, Off), D2 default Start + Select, D3 combo buttons delivered, D4 hold follows the combo, D5 Start + Select always opens the Quick Menu in Nano's menus, D6 RetroArch's menu to Hold Start, D7 L3/R3 hotkeys allowed, D8 any controller (not now), D9 map row hidden with Controller Bindings Off, D10 200 ms window for L3 + R3; and the plan's §11 questions.

### Controller Bindings explanation (build 43)
- **Why:** the owner approved a new explanation for Quick Menu > Controller > Controller Bindings, and asked that it display on small screens such as 640x480. The approved text (285 characters) does not fit. The description under the focused row (`drawDesc`) is capped at 4 lines and cut with "...". The approved text needs 6 lines at 640x480 and 7 on the Cube (720x720), so the sentences about the %s button and the restart never show. The strings below keep its meaning and fit. The row keeps its name.
- **Final strings** (`%s` = the function button's name from `mods/device-keys.json`: Function, Anbernic, Home, Menu, Back, RG, F1 or ≡; fit checked with the widest, Function and Anbernic):
  - Description (124 characters, 130 with the button name): "Off until restart: games get Start + Select. Shortcuts, hold to quit, browser controls, mouse mode stop. %s opens this menu."
  - Values: keep plain "On" / "Off". Labels such as "On (Thimsy shortcuts)" / "Off (game gets all buttons)" do not fit. Next to the label "Controller Bindings" there is room for a value of 150 px at 640x480, 124 px at 720x720 and 52 px at 480x640. "Off (game gets all buttons)" is 197-297 px, so it scrolls or is clipped, and the side-panel chooser shrinks it or runs it off the screen. Plain On/Off already uses about all the room on the Cube once translated.
  - Toast when turned Off (24 characters): "Games get Start + Select".
  - Button Map hint: keep "Controller Bindings is off (Quick Menu)" unchanged. At 39 characters it is one line at every size, also with 35% longer translations, and its 21 translations stay valid.
  - Cross-reference, appended to Gamepad Settings > Hotkeys > Start + Select Hotkeys (74 characters): "If a game needs Start + Select, turn off Controller Bindings (Quick Menu)."
- **Where each goes** (lines in `patches/thimsy/0001-thimsy.patch`):
  - Description: `buildQuickControllerSubmenu`, `quickLeafItem(... "bindings", fnText(...))` ~42815-42817.
  - Values: the settings row ~45159 `{"Controller Bindings", kProp, "sys.gammaos.bindings.enabled", "1", "1:On,0:Off"}`. No change.
  - Toast: new. Turning a setting off shows the generic "Setting changed" toast today (~46113-46140 in the patch; `NanoMenuPS3Menu.cpp` 12587 / 12881 in the composed tree). When `bindings` goes to 0, show the new string instead. The toast is one line and its marquee scrolls only about 11 characters before it fades, so the string must fit without scrolling.
  - Button Map hint: `browserHint` ~11088 and the catalogue ~12066. No change.
  - Cross-reference: the Start + Select Hotkeys desc ~43537.
  - Translations: the old description key appears in all 21 `ps3xmb/i18n/*.json` files (e.g. ar ~91943, da ~94552). There are 21 files, not 23.
- **Fit**, from the model `out/dp-evidence/test43/ctrlbind/ctrlbind_fit.py`. The model reproduces `drawDesc` (greedy wrap on spaces, 4 lines, "..."), with the real Rodin and Roboto advances, menu zoom 1.12 and Font Size 1.0. Lines used out of the 4 available:

  | String | 640x480 | 720x720 | 1280x720 | 480x640 |
  |---|---|---|---|---|
  | Description, English | 3 | 3 | 2 | 5 (cut) |
  | Description, 35% longer | 4 | 4 | 2 | 7 (cut) |
  | Description, 39% longer (nl, the worst ratio) | 4 | 4 | 2 | 7 (cut) |
  | Widest text that still fits in 4 lines | 1.54x | 1.41x | 2.79x | 1.00x |
  | Cross-reference alone, English [35% longer] | 2 [2] | 2 [3] | 1 [2] | 3 [4] |
  | Start + Select Hotkeys desc + cross-reference (330 characters) | 7 (cut) | 8 (cut) | 4 | 12 (cut) |

  - Description elsewhere: at Font Size 1.3 it takes 4 lines at 640x480 and 720x720 (English only). On the DSi top screen it takes 3 of 5 lines, 5 of 5 at 35% longer. Options > Information wraps without a cap.
  - Toast: 162 px of the 244 px visible at 640x480 (219 px at 35% longer; 214 px in English at Font Size 1.3). It fits at 720x720, 1280x720 and 480x640 too, at 35% longer. Where there is no XMB clock bar to cover (DSi and Minima themes, the video player) the toast is a card sized to the text; that also fits, except a 35% longer text at 480x640 (198 px in a 179 px column), which scrolls.
  - Button Map hint: 272 px at 640x480 and 480x640 and 388 px at 720x720 and 1280x720, against line widths of 596, 436, 656 and 936 px, so it stays one line at 35% longer.
  - Checked again independently (own TTF parser and own geometry from `layoutCompute`, `drawDesc`, `drawPs3Clock` and `drawPhotoBanner`): every number above matches. The tightest case is the description on the Cube: it fits up to 1.41x the English width against 1.39x for nl, so translations there have almost no slack.
- **Not solved by wording:**
  - 480x640 portrait would need the description cut to 83 characters. Fix it in code instead: let `drawDesc` use more than 4 lines when the screen has room. The 4th line ends at y≈294 of 480 at 640x480.
  - The cross-reference shows only at 1280x720 and up, because the Start + Select Hotkeys description it follows (249 characters, 255 with the button name) is already cut on small screens. A shorter description that fits (125 characters, 131 with the button name; 4 lines at 640x480 and 720x720 at 35% longer) would drop details from an approved string: "On: Start + Select works like %s for hotkeys; hold it to quit. If a game needs it, turn off Controller Bindings (Quick Menu)."
- **Hotkey Combo:** if the Rebindable hotkey combo item lands first, write these strings with `comboText` (`%1$s` = the function button, `%2$s` = the combo) and add an Off variant ("Off until restart: browser controls and mouse mode stop. %s opens this menu."). "Start + Select" is the widest combo, so the fit numbers hold.
- **Translators' budget:**
  - Description: the translation, with the widest button name, must wrap into 4 lines at 720x720. That is at most about 1.4 times the English width, roughly 170 characters for Latin, Cyrillic and Greek.
  - Description in Japanese, Chinese and Thai: at most 80 characters, with a space at least every 20 characters. `drawDesc` breaks only at spaces (23 full-width characters per line at 720x720, 24 at 640x480).
  - Toast: at most 32 Latin characters, or 16 full-width characters.
  - Cross-reference: at most 100 Latin characters.
  - Values: keep the existing On/Off translations.
- [ ] Owner OK on the shortened wording, plain On/Off, the toast, keeping the Button Map hint, and (optionally) the shortened Start + Select Hotkeys description.
- [ ] Update the strings in the patch: the description, the new toast path for `bindings` = 0, and the cross-reference.
- [ ] README.md:750 and HOTKEYS.md:209: match the new wording (games get Start + Select; shortcuts, hold to quit, browser controls and mouse mode stop until restart; the function button still opens the Quick Menu).
- [ ] All 21 i18n files: the new description, toast and cross-reference keys translated within the budget above. Remove the old description key.
- [ ] Host test: run the wrap at 640x480, 720x720 and 1280x720 for these strings in every language, with the widest button name. Use the model, or better the real `drawDesc` and `wrapTextToWidth` with the device fonts. Fail when the description needs more than 4 lines or a line is wider than the box. The toast must fit its visible width without scrolling.
- [ ] Existing bugs found while measuring:
  - `drawDesc` breaks only at spaces, so Japanese, Chinese and Thai run off the screen. `wrapTextToWidth` already has the right break rules.
  - The `drawDesc` line pitch ignores Font Size.
  - The toast marquee moves by bytes, which splits UTF-8 characters, and the XMB toast runs past the right edge of the screen.
  - The DSi/Minima toast text renders at about 10 px at 640x480.

### Arcade and MAME layouts (arcade_controls_plan.md): not started (user request, 2026-11-27)
- **Goal:** the button map shows each arcade game's own controls (names, Neo Geo row, twin stick, spinner, gun...) instead of the generic "Btn 1 / Fire 1" row, from per-game data generated at build time for the shipped fbneo, mamearcade and mame2003_plus cores; unknown games keep today's row.
- Today's generic row is wrong for many FBNeo games (Street Fighter layout, Neo Geo macros, Mortal Kombat); it is right in order for MAME only because MAME's button profiles are off.
- [ ] Phase 0: fix the FBNeo Neo Geo shoulder macros in `retropad.json`, the hidden "Per-game layout" badge, the device ids in button_map_overlay_plan.md §3.1, and the CPS HP/HK swap in controller_layout_plan.md §10; read the payload for arcade `.opt`/`.rmp` files.
- [ ] Phase 1: TSV data format and lookup (`NanoButtonMapArcade`), MAME classes from `-listxml`, MAME profile table (whole chain, first match wins), MAME 2003-Plus layouts; `mame_buttons_profiles` on in `MAME.opt` (D1) with a one-time overwrite of the `disabled` RetroArch already wrote on existing installs, shipped together with the reader change. D2 must be decided first.
- [ ] Phase 2: FBNeo host harness (no ROMs; tens of minutes to compile, never during an AOSP build; source published per FBNeo's licence) for per-game maps and names per device type; lock-hash drift guard in `10-update-emulators.sh`; FBNeo licence text in NOTICE (also owed for the core itself).
- [ ] Phase 3: MAME button names and positions (`PORT_NAME` parser; also places buttons for dial, pedal, gun and trackball games), MAME 2003-Plus names from the same MAME names by set name (D5), translated arcade vocabulary.
- [ ] Phase 4: panel drawings per class (arcade1-4, twin stick, dial, trackball, light gun, driving, flight), header notes, Coin/Start hint, P2 note, FBNeo service row; size tests on the panel and a monitor.
- [ ] Phase 5: Thimsy-owned per-game remaps at launch that even out the cores (FBNeo Neo Geo device or MAME Neo Geo button remaps per D2, MAME's 11 odd Neo Geo titles, MAME 2003-Plus 6-button and MK) with the ownership sidecar and a "Thimsy's layout" badge; Game Options "Arcade layout"; reading MAME's own `cfg/<set>.cfg`.
- [ ] Optional phase 6: touch light guns (Cube screen only), turbo in the owned `.rmp`, mahjong touch panel, MAME4droid and Flycast NAOMI, an upstream libretro/mame descriptors change.
- [ ] Device checks: the plan's §10.1 rows, by the phase that enables each (incl. D1 migration, owned-remap lifecycle, external pad / ABXY Swap, monitor and Dual Screen).
- Effort: about 20-24 days for phases 0-5, plus 6-10 for phase 6.
- **(decide)** The plan's §13 D1-D10 and open questions 1-11. First, before phase 1: D2 (Neo Geo layout: NEOGEO mini or FBNeo Classic), D1 (MAME profiles on, overwriting `disabled` once), D4 (ship FBNeo-derived data), D5 (MAME 2003-Plus names from MAME 0.289, not copied from its `controls.c`).

## Displays

- **Monitor resolutions other than 1080p60:** implemented on the 1.4.4 base and run on an RG Cube in test 42 (B5, B7, B9, MOD-Q, K1, T3; dp_dock_findings.md §12); see "USB-C docks and display modes" below. external_display_modes_plan.md §R ("Resolution") still describes the old 1080p60-only rule.
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

### Dual screen for build 43 (dual_screen_43_plan.md): not started (user request, 2026-11-27)
- **Goal:** in Dual Screen, swap which screen is the main screen (the monitor by default, or the Cube's screen); move an app to the other screen; the controller always on the main screen, the second screen for touch. Fixes the installed build's bug: controller input lost or late in a game while a browser on the other screen is used by touch (ranked causes and read-only checks in plan §3).
- Supersedes dual_screen_plan.md's "the controller follows the open UI", Controller Screen, the focus pin and the Switch Screen chord (note at the top of dual_screen_plan.md).
- [ ] Phase 0: owner's read-only checks on the installed build (plan §3.3): Controller Screen, the focus pin, gammapad's `fg_pkg` and BrowserMode, the key-wait and queue-prune log lines, key display ids in `dumpsys input`, game frame times and thermal state. Respect the hazards listed there (boot without the monitor, unplug at the monitor end).
- [ ] Phase 1 (medium-large): controller always on main: `sys.gammaos.display.second` as the gate, display 0 always the top focused display in Dual (RootWindowContainer); InputDispatcher clears the display of controller keys at `notifyKey`, lets injected Back keep its display, makes keys wait only for their own display, prunes only for the display being waited on, and skips the task-focus path for taps on the second screen's focused window; an idc for the built-in pad; VirtualTouchscreen on display 0; gammapad's foreground app and PhoneWindowManager's lookups from display 0 only; Nano drops Controller Screen and the pin, Quick Menu "Second Screen App" (Back, Close, Move to Main Screen by today's restart path), the old chord shows a notice; IME re-test; `inputflinger_tests` cases. Can ship alone.
- [ ] Phase 2: Move App: a system_server task move (`moveRootTaskToDisplay` with pre-checks, exceptions caught, home tasks refused, result property), Quick Menu "Move to Other Screen", Anbernic + left button hotkey; a game is refused until Phases 3 and 4 make it Swap Screens.
- [ ] Phase 3: main-screen swap, framework: `persist.gammaos.display.main`, arrangement with main = panel, a role id for the second screen, flags by role and no `TYPE_INTERNAL` tests, old-mode cleanup only without a monitor, rotation lock for any monitor, ExternalDisplayPolicy, power controllers on SWAPPED, SurfaceFlinger pacesetter = the main screen (one rule with refresh_matching_plan.md), trial guard, one swap at a time, no swap while a Presentation is shown, `cmd display gammaos-main`; `GammaosScreenArrangementCheck` and a secondary-id host check.
- [ ] Phase 4: main-screen swap, Nano: `choose()` with the monitor as second (`NanoDisplayTarget_test`), the second surface at its own size, the trial prompt on the monitor, Main Screen setting, Swap Screens in the Quick Menu, Quick Settings and Anbernic + top button, home themes (ES-DE, DSi) and drastic-nano when main = panel, swap back before leaving Dual, rows hidden outside Dual.
- [ ] Phase 5: docs (README Dual Screen, HOTKEYS.md), SystemUI's `config_gammaos_secondary_display_id=2`, optional gesture and SystemUI tile.
- **(decide)** The plan's §9 decisions D1-D21. First: what a swap moves (D1), where Nano's menus draw (D2), moving a game (D3), moving onto an occupied screen (D4), what counts as a game (D19).

### USB-C docks and display modes (usb_c_dock_plan.md, display_modes_plan.md): test 42 ships them; most ran on a device (dp_dock_findings.md)
- **Goal:** a powered USB-C dock gives power in, picture and USB at once; a monitor can run at modes other than 1080p60 (1440p60, 1080p120, later 4K30), chosen in Nano with confirm-and-revert.
- **Decisions taken** (dock plan §12; display plan §7):
  - D1 dtbo entry 2 only; D2 the PPS APDO dropped from sink-pdos; D3 device-tree route, no module patches; D4 DP-B ships, DP-A, DP-C and DP-D don't; D5/D11 no charging step 2 (unidentified sources stay at 1.05 A; the dock plan's D11, not `scripts/sprd_cm.py`'s; superseded in test 42 by CM12, which charges an SDP behind a PD hub at 1.05 A and a 5 V 3 A contract, UNKNOWN type included, at 2.0 A, with sprd_cm.py's own decisions D11 and D12); D6 no F4; D7 source PDO v3 (5 V 900 mA); D8 no other devices' firmware; D9 back up first, recovery is a Full install; D10 the core c2c set (stage 1b) with `vbus-supply`, no `use-pdhub-c2c` on pd@e00, no 1d.
  - Test 40 ships the dtbo fix untested on a device (the owner's decision; no phase-1 hand test): `DOCK_FIX` defaults to 1 (`DOCK_FIX=0` keeps the stock dtbo), and `tested_sha256` in `scripts/patch_dtbo.py` holds `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536`, approved without a device test; `build.sh` still refuses any other image, and needs `fdtoverlay`. Test 42 replaced it with TH2 (`c87ece8c...`): DK1 (`1b727faa...`, with fix A), which ran on the RG Cube, with the thermal2 charge temperature steps, which the owner chose to ship. README tells testers to back up, be ready for a Full install, plug a dock's charger in first, and what to report.
  - Display modes: kernel route; link-bandwidth check with 1000 (no audio headroom); Auto stays at 1080p60 (stage 1); 3840x2160 only as TV 4K30 and TV 4K60 4:2:0 (test 42: the driver's UHD and Y420, the CT4K composer and the NV12 SurfaceFlinger), DCI 4096 wide never; 1440p60 offered; kept per monitor model (a hash of `DeviceProductInfo`); modes below 50 Hz hidden in Dual Screen; 15 s with the cursor on Revert; physical and render range pinned to the mode's rate; no "render at 1080p, output 4K".
  - Every mode user-defined (1B) only with `SYSTEM_IMG` (`THIMSY_DP_ALL_MODES`, set by `build.sh`, in place of the plan's `--all-modes` flag); recovery never gets it.
- **Done:**
  - Build: `scripts/avb_footer.py`, `scripts/patch_dtbo.py` (pure Python, with an `fdtoverlay` merge check that `build.sh` runs against the build's vendor_boot, `fdtoverlay` from `device-tree-compiler` in the Docker image), the `DOCK_FIX` step in `build.sh`, `tests/unit/test_dtbo.py`. The approved image was `out/.dock/dtbo-c2c.img`, sha256 `94c0f3d4bb1fbbf518a0461db39908d02fd67e320e52c45ced540076d09c5536`, checked offline against vendor_boot's base DT and `live.dtb`; test 42 ships TH2 (`c87ece8c...`), DK1 (`1b727faa...`, which ran on the RG Cube) with the thermal2 jeita tables (`patch --tested` applies them).
  - `scripts/sprd_drm.py`: the link-bandwidth check and the DCI reject (display plan 1A, 1C), DP-B as its own list (`HPD_PATCHES`), 1B behind `all_modes`. Checked statically against the original module only.
  - After test 41 (display plan §9; 1440p monitor without CEA 1080p60: started in 4K bypass, no picture after any mode change, panics in `dptx_irq` and `dptx_threaded_irq` after an unplug): in `scripts/sprd_drm.py`, A (nothing 3840 or more wide, the 0x8e2c word) and B (`IRQ_GUARD_PATCHES`: both handlers guarded, pd_event's flag cleared after its glb->disable) in both copies; C (`SAFE_START_PATCHES`, a monitor's own 1080p60 at or below 148.5 MHz preferred; replaced in test 42 by the fixed mode table) and D (`MODE_CHANGE_PATCHES`, DPU1 disable and enable on a DP mode change) only with all_modes. `tests/unit/test_vendor_patches.py` checks them, with expected disassembly (`tests/a64.py`) and the stock module rebuilt from test 41's. Run on the device in test 42 (B5 mode switches with fix D, B7 the fixed mode table, B9 20 unplugs without a panic or "nobody cared"; dp_dock_findings.md §12).
  - Framework (1.4.4): the monitor's mode filtered, held, tried, kept and reverted in system_server; the applying marker; `cmd display gammaos-monitor-mode`; `sys.gammaos.display.monitor_mode`.
  - Nano (1.4.4): Output Resolution (one list since test 42; Refresh Rate removed), Keep This Resolution? and Forget Monitor Resolutions (`NanoMenuMonitorMode.cpp`, `NanoMonitorMode`); NanoTypec 4e items 1-4 and 7 (docks left alone, the flip branch, no dwc3 debugfs, port found by what it can do); `tests/unit/test_nano_units.py` builds both host test suites with g++.
  - Installer (repo thimsy-os, branch universal-installer): DTBO and DTBO_B written by Update and no longer read back, the `update`/`update_check` overlap check, `MIN_INSTALLER` 0.1.0-test20, e2e and package tests.
  - README: External monitors (the driver, Output Resolution, USB-C docks, the port lookup), Building (`DOCK_FIX`, `patch_dtbo.py`, `avb_footer.py`).
  - Test 42: Step 5 and 4K60 over YCbCr 4:2:0 (display plan §8): the driver's UHD and Y420 (MOD-Y1-E), the composer's CT4K and the NV12 SurfaceFlinger; 4K60 4:2:0 showed correct colours on a Cube (T3, with MOD-Y1). Left: 4K30 and 4K60 with fix E, a long 4K60 session, and 4K30's dropped pop-ups (dp_dock_findings.md §12, K1).
  - Test 42: charging step 2 (phase 5) as CM12 (`scripts/sprd_cm.py`, `fe3b3de4...`, tested on the Cube; charge/cm2/NOTES.md), with the accepted limits D11 and D12 in sprd_cm.py.
- **Still to do without a device:**
  - Display plan Step 3's SurfaceFlinger part: restart a monitor's vsync window (`gammaCheckMonitorVsync`) when its mode changes. Not implemented (§12 says so); without it a slow modeset can trip the "no composer vsync from monitor" fallback.
  - The crash4 abort (display plan §9): `sprd_dpu_atomic_disable` -> `dpu_clk_disable` -> `clk_set_parent` after DP's glb->disable gated the IPA clocks. Its own track.
  - A full `SYSTEM_IMG` build: the framework code was compiled only against stubs, and Nano's `NanoMenu.cpp`, `NanoMenu.h`, `NanoMenuMtp.cpp`, `NanoMenuPS3Menu.cpp` and `NanoMenuSettings.cpp` (the `ro.thimsy.release` suffix in System Information) edits were never compiled. The text shader's `highp` (`NanoMenuShaders.cpp`) is GLSL compiled only on the device.
  - A native speaker's review of the 19 new strings in each translation.
  - Run the installer's `installer/tests/e2e.sh` with a real `WORKSPACE` and `PACK_DIR` before release: `run-all.sh` skips it without them. Meanwhile the engine test `rg_cube_update_writes_dtbo_and_reads_back_the_rest` checks the RG Cube's update plan (both dtbo slots written, no dtbo read back, the rest of the boot chain read back).
  - Set the executable bit on `scripts/patch_dtbo.py` and `scripts/avb_footer.py` when committing (`git update-index --chmod=+x`). Test 42's commit: after `git add`, `git update-index --chmod=+x scripts/build_144.sh scripts/aosp/setup_host.sh scripts/aosp/system.sh scripts/aosp/sync_patch.py scripts/patch_vendor.py` (core.filemode is false on Windows); `tests/unit/test_script_modes.py` fails on a commit without it.
  - Optional: `test_vendor_patches.py`'s footer tests onto `avb_footer`, then drop the `footer_info` wrapper in `patch_vendor_boot.py`.
  - Stale text: external_display_modes_plan.md §R.
  - Auto stage 2 and a "native" mark in Output Resolution: the framework can't tell the native mode once every mode is user-defined.
- **Needs the device:**
  - Phase 1a: back up, confirm download mode, `lpdump` and fastboot `--set-active` (Q6, Q11, Q12).
  - Done in test 42 (DK1, dp_dock_findings.md §12): **the dtbo fix on a device.** DK1 (`1b727faa…`, with fix A) ran on the RG Cube: it booted, and a multi-function dock with its charger plugged in first gave a picture, USB 3.0 and charging. Still open: a self-powered dual-role USB-C monitor without PD (VBUS from tcpm `set_vbus` with c2c, never observed), and an Update from a clean install and a Full install with the test 42 package (phase 2).
  - TH2's charge temperature steps at 45 C and 49 C: their tables ran on the RG Cube (`jeita_size:6`), but no step has been seen; only `test_dtbo` Jeita.test_steps simulates them. A warm charge on a 5 V 3 A charger should log `target current = 1500000` from 45 C, `1000000` from 49 C and `700000` from 52 C.
  - Before test 40 leaves draft (DP-B changes today's behaviour): matrix rows 1, 3 and 5, and a dongle whose first Attention is 0x18a.
  - The display driver after test 42 (test 41's fixes ran in test 42 as B5, B7 and B9; fix E's sleep and wake in irqfix run 1, out/dp-evidence/test42/irqfix/NOTES.md): with the shipping driver MOD-Y1-E (fix E) and the SLEEP+CT4K composer, 4K30, 4K60 4:2:0, 1440p60 and 1080p120 again (they ran only before fix E); a long 4K60 session; Dual Screen and Mirror sleep and wake; step 3b (unplug at the Cube's end while asleep); a real deep suspend (a 5-minute sleep with `PM: suspend entry` in the log); recovery's driver `c7e8c829` (it replaces test 41's `4c2b7eb7` and hasn't run): recovery with and without a monitor, and a plug and unplug in recovery.
  - Display modes: display plan §6 for 1440p60, 1080p120 and (after Step 5) 4K30; the revert paths; the applying marker; SELinux for the `sys.` property and `/data/system/gammaos_monitor_modes.txt`; that Nano's `cmd` call passes the uid check; the side panels and dialog in every theme.
  - Nano's text shader at `highp` (`NanoMenuShaders.cpp`): it compiles on the Mali-G57 (no shader error in logcat at Nano's start), and glyphs packed past texel 1024 of the atlas are sharp. The atlas is 2048 texels tall on every device (`mAtlasH`, `NanoMenuRender.cpp`), so this applies everywhere, not only where it is 2048 wide.
  - DP-A (with variant (a), or with DP-D), phase 3, after phase 1; Nano's DP retry (4e.5) needs it.
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

### DS layouts for the Cube screen and monitors (ds_layouts_plan.md): not started (user request, 2026-11-27)
- **Goal:** WatermelonDS and SeedlessDS layouts that suit the Cube's 720x720 screen and each monitor resolution, in Handheld, Docked, Dual Screen and Mirror. The plan's §1 has the owner's steps for today.
- **Today:** WatermelonDS custom layouts are pixel variants used only at exactly their own size (no scaling, even at the same aspect ratio); its Default is poor on the square screen, which Thimsy reports as landscape. SeedlessDS layouts scale with the window, but one setting covers every screen (Second layout + a swap hotkey), and its Vertical layout asks for portrait, so it must not be current on a monitor (Docked or Dual). Docked has no touch. Thimsy writes no layout or display setting for either app, and NanoEmuSync does not sync WatermelonDS `files/layouts.json`.
- [ ] Owner checks (plan §6): T1 variant keys from WatermelonDS "Backup layout" at 720x720 and 1080p; T2 editor with the controller on the monitor (hat D-pad, L2/R2 keys); T3 editor size equals game size; T4 Dual presets; T5 the pad hides touch buttons; T6 SeedlessDS Vertical on the Cube (letterbox?); T7 SeedlessDS second screen in Docked, Dual and Mirror; T8 dock and undock mid-game; T9 display names, dpi and presentation flags; T10 4K60 4:2:0 pacing; T11 SeedlessDS Vertical on a monitor.
- [ ] Option 1 (small, low-medium risk): add WatermelonDS `files/layouts.json` and `files/backgrounds.json` to NanoEmuSync (P:17381), with a safeguard for a stale card copy (keep the app's copy as .bak and merge by layout id) and emusync host tests, "card older than the app" included.
- [ ] Option 2 (small): provision with `ifUnset`: WatermelonDS `dual_screen_preset=internal_top_external_bottom`; SeedlessDS `_DsKeepRatio=true`, `_DsSameSize=false`, `_ExtDisplayDelay=0` and a layout pair (`_DefaultLayout`/`_ScreenLayout2` = 2/1 or 1/2, D2); Cycle layout / Swap layouts hotkeys (WatermelonDS `CYCLE_LAYOUT`, SeedlessDS `s_5`/`s_8`) and HOTKEYS.md.
- [ ] Option 3 (medium, after T1): generated Thimsy reference layouts for WatermelonDS (a variant per Thimsy size), merge-only writer through `appFileWrite`, versionCode gate, owner-edit detection, validator and host tests.
- [ ] Optional: option 4 (SeedlessDS layout per mode at launch, opt-in); option 5 (upstream requests: proportional variant fallback, "Built-in Screen" prefix match).
- **(decide)** The plan's §8 D1-D9. First: D1 (sync layouts.json), D2 (provisioned defaults), D4 (ship reference layouts).

## Android launcher

### Thimsy Home: Nano as an installable Android home app (android_launcher_plan.md): not started (user request, 2026-11-24)
- **Goal:** Nano's features, UI and navigation as a launcher APK for stock-Android handhelds (Anbernic, AYN, Retroid Pocket), installable as the home app. Its game lists and emulator profiles are interchangeable with Thimsy OS through the `ES-DE` folder, `ROMs` and `emulator_config`.
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
- **Card contract (proposed, plan §7; decision 8):** the drive mounts at `/storage/<VOLID>` on Linux too; no symlink ever lives on a drive. v1 shares the library, BIOS and same-core saves. States and settings are per platform; N64 and DS saves are not shared. `/storage/roms` is gone with ROCKNIX.
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
- Phase 3: card interop v1: `LIBRARY-FORMAT.md`, the drive model, the immutable mount point, links and re-pointing, the per-system core table, emulator definitions schema 2, `systems.linux.json`, `card.json`, and the Android-side changes (label rule, not overwriting unknown launch tuples, dialog wording).
- Phase 4: installer: the SD-card writer in the reserved `sdcard` crate (pulled forward from multi_device_plan.md Phase 5; decision 7), `pack.json` format 2, the card picker, the ARC-D's dual-boot card layout (no step writes the eMMC), the storage job (below), images with bmap and `legal-info` sources.
- **Storage job (owner requirement, 2026-11-25):** the universal installer writes microSD game cards and USB drives as a first-class job, with the same flow as the firmware install (pick device, pick card or drive with the same safety rules, pick contents, write, verify), for Android and Linux devices. All four parts: (1) format and lay out (exFAT, basic-data type, non-zero serial, the card contract's folders `ES-DE/`, `ROMs/<system>/`, `emulator_config/`, `bios/`); (2) pack-in games, art and gamelists for the chosen package profile; (3) ROMs and BIOS copied from the PC into system folders, BIOS checked by hash; (4) a non-destructive update or repair of an existing card, never a format. Replaces the plan's "Prepare a game card" step (plan §8.4; also a requirement in `universal_installer.md`).
- Open the ARC-D and wire its serial pads (tx/rx/gnd, 1.5 Mbaud, 3.3 V, CP2102N/FT232 adapter): owner, later. Phase 0 starts without it (plan §11.1, open question 13).
- Linux v1 release: RetroArch hotkeys (its own menu) and Close Current App, scraper, BIOS manager, File Explorer, setup wizard, update file, PPSSPP and ScummVM recipes, the ARC's HDMI in Docked mode (Nano and games on the TV, panel off; mirroring later), 3 s on the reference unit.
- Phase 5: Nano's Quick Menu through an in-process libretro host and padd's intercept mode; a DRM-lease spike for standalones; button map overlay. Phase 6: boot time and the 1.5 s stretch measured. Phase 7, coverage waves 1 to 4: 7a builds `thimsy-import` (generator, hash guards, release-image check, catalog with status levels, Device check, hardware-in-the-loop rig), done when it regenerates the RK3566 hand import and imports H700; 7b runs the waves, about 1 to 2 weeks per family plus testing, and per-device quirk hooks, pad sections and rotation and touch checks (wave 1's H700 needs the H700 kept in device_profiles_plan.md, its §11 question 6). Phase 8: bootloader writing with a verified backup once allowed (ARC-D route (c), wave 3), shared save states for cores that pass the state test, HDMI mirroring, heavy emulators on Qualcomm, media apps, Quick Resume, dual-screen layouts.
- Effort: about 22 to 36 person-weeks to the alpha and 38 to 62 to v1 on RK3566 *(guess)*; the importer 4 to 8 *(guess)*; the 14 further families 20 to 40 *(guess)* plus per-device work; **the whole program about 60 to 100+ person-weeks** *(guess)* (plan §12).
- Shares work with: android_launcher_plan.md (seam, library format), multi_device_plan.md (descriptors, SdCard backend, package format 2), device_profiles_plan.md (resolver, Small and Medium), user_emulators_plan.md (definitions), usb_storage_plan.md (drive model), controller_layout_plan.md Phase 1 (`face_layout`). Plan §4.5 names an owner for each piece.
- **Owner answers, 2026-11-25 (second batch; plan §17.5):** question 11, save states load across same-commit 64-bit cores, "yes, but we need to test it" (per core: save on the Cube, load on the ARC and back; shared later, after v1); 12, test both GPU drivers, ship the most stable first; 13, serial later (to-do above); 14, Docked HDMI in v1, mirror later; 20 and 21, Phase 0 items with a pass rule; 22, "I don't care who wrote it. Use the ROCKNIX license." (no author recovery; a provenance line per patch); 23, the recommended recipe approach (our own recipes, ROCKNIX's `package.mk` ported into `gpl/`, cores built from the commits of `retroarch-cores.lock`), not objected to; 24, "use the FlipBase TF-A" (decision 18 for RK3566); 26, don't weigh the BaseOS licence (most files replaced anyway; the four C files used as-is). Questions 7 and 17 are moot (no erase). Question 10 is still being researched.
- **(decide)** The plan's §14 decisions. Decided: 1 (base; amended by 21: ROCKNIX's hardware layer is a tracked upstream, its runtime unused), 3 (the Android build stays, maintained separately; the ARC-D will offer both), 5 (boot target), 18 for RK3566 (the unit's own vendor BL31 with FlipBase's pm-config patch, never redistributed; rkbin's as the fallback; other families as proposed) and 21 (device layer: Thimsy Base + importer). Reopened, each with a recommendation: 2 (first device: ARC-D dual boot by an SD-only route plus an ARC-S; erasing Android out by the SD-only principle), 4 (display stack: KMS/GBM hand-off first, no compositor; amendment proposed: the rotation contract confirmed for the Mali families only, a per-family display setting on Qualcomm, a per-game kiosk fallback), 6 (an MIT build tree with a `gpl/` area), 9 (real ES-DE: not in v1), 11 (GPU: Panfrost, GLES, no Vulkan; extension proposed: Mesa only on every family; amended by the owner: v1 ships the more stable driver from the Phase 0 A/B, both switchable until then), 12 (package profiles), 13 (image distribution; reopened again, proposed: the installer assembles each card from verified parts, which would supersede the earlier complete-images recommendation), 14 (H700 panel: variant images), 16 (public alpha: credit for imported patches, each with a provenance line), 17 (one clause moot). New: 19 (fork FlipBase at a pinned commit), 20 (card identity on the Cube), 22 (kernel per family: ROCKNIX's version at a Thimsy-chosen tag, skipping tags freely; about 6 version changes a month across families; RK3566's version chosen at Phase 2a), 23 (input: `thimsy-padd` everywhere, InputPlumber never in the image), 24 (firmware: ship only known-licence `linux-firmware`, fetch the rest from the user's device), 25 (image structure: tiers, GPU packs, hardware packs), 26 (coverage waves, wave 3 deferred until bootloader writing is allowed, and per-product status). Still open: 7 (the order of the plans), 8, 10, 15. New open questions 28 to 35 include ROCKNIX's tag cadence (33), stable updates on its .0 kernels (34) and Vulkan rotation on Qualcomm (35).

## Scraping

### ScummVM scraping (scummvm_scraping_plan.md): listing done, scraping phases open
- Phase 0: measure which `romnom` spelling ScreenScraper accepts, `jeuRecherche` on systems 123 and 135, folder queries.
- Phase 2: a second-spelling fallback; the ini description as the search title; cached ScreenScraper ids; a 30-day not-found back-off.
- Phase 3: a bundled `games.tsv`; TheGamesDB by title; the original-platform fallback (DOS, Amiga, Windows); Beneath a Steel Sky's pack-in media.
- Phase 4 (optional): detection fingerprints.
- **(decide)** §4 Q1-7 and R.8 Q8-10; some may be moot now that the listing comes from the ini.

### ScummVM games listed from the ROM folders (scummvm_autoadd_plan.md): not started (user report, 2026-11-27)
- **Goal:** every top folder in `ROMs/scummvm` (any case, so the owner's card `Roms/scummvm` too) on internal storage and microSD shows in the ScummVM list on its own, and keeps showing (Phase 1); Nano identifies each new folder and adds it to ScummVM's `scummvm.ini` itself, with Mass Add's detection, so it starts from Nano (Phases 2-3). Unrecognised folders stay listed with a "?" and a "ScummVM: Not Recognised" list. The owner's own ScummVM entries are kept.
- **Why today's list "doesn't persist":** with the ScummVM app as player, Nano lists only `scummvm.ini` (the owner's holds only `[sky]`) and scans no folder; with emulator files kept on the card, a card-wins sync replaces the whole ini and drops app-only games (B6); the Cube's card id is a mount index, which matters only with a second volume (plan §2.2). ScummVM can't be asked to add games from outside (one intent argument only, plan §2.3). Stop-gap today: Mass Add in ScummVM on the card's `Roms/scummvm` (plan §1).
- Affects the "ScummVM scraping" section above: its "listing done" and "listing comes from the ini" change once Phase 1 lands.
- [ ] Phase 0: owner's checks with the card (plan §16: backups, `mount` and the `emu_sd` settings, emusync log lines without a tag filter, Add Game and Mass Add on the card in ScummVM); B2 `emusync::isVolumeId` and `repoint()` accept the 36-character Cube id; B6 a card win on `scummvm.ini` merges game sections instead of dropping the app's; B1 folder grants with a 36-character volume id ("too long to send": logged 21 times for Azahar's Move Emulator Files to microSD in the dock-tv-run log; SeedlessDS and the hakuX, ScummVM and WatermelonDS card grants fail by the same arithmetic, not logged).
- [ ] Phase 1: folders listed without any ini write: sources and game rules, path matching, mirror line 3, sticky folder names and name collisions, card out and in, Add Folder for ScummVM, "?" mark and a launch dialog showing the folder path with Open ScummVM (launcher only). Stops the list disappearing; folders without a ScummVM entry show but don't start until Phase 3. Assumes the recommended D3, D6, D7.
- [ ] Phase 2: detector: a separate GPLv3 driver over ScummVM 2026.3.0's detection with Mass Add's arguments (not `--add`), built with the release's engines (recommended; size and NDK build path unknown) or a built-in table from its detection data; isolated uid; cache, time budget and back-off, Not Recognised list.
- [ ] Phase 3: registration: append-only `scummvm.ini` writes when no ScummVM game could be lost (force-stop a cached ScummVM with no activity, ask before closing a running game), under the emusync locks, with a backup and `thimsy_added=1`; minimal file on first boot at launch only; pre-launch registration at the three launch sites; dismissed list; repoint after a card index change or a renamed or moved folder; `browser_lastpath` seeding; rewrite ARCHITECTURE.md's "ScummVM is the exception" (it already misses the BIOS worker's `extrapath` write) and scummvm_scraping_plan.md Q3/Q9.
- [ ] Phase 4: B3 stable card identity for emusync; Clean Up Missing ScummVM Games; title suggestions; RetroArch's ScummVM core listing folders too (D9), if chosen.
- **(decide)** The plan's §18 D1-D9. First: may Nano add game entries to `scummvm.ini` (D1), and helper or table (D2).

## Refresh matching for games

### Per-game monitor refresh rate (refresh_matching_plan.md): not started (user request, 2026-11-27)
- **Goal:** a game started with a monitor attached switches the monitor to a rate that suits it, and back on return to Nano: PAL at 50 or 100 Hz first (phases 1-4); later, on monitors with a continuous range only (phase 5), MS-DOS VGA at 70, WonderSwan and Lynx at 75, Mortal Kombat / R-Type at 55, Toaplan / Cave at 57, Neo Geo / Taito F3 at 59. 1080p only: no matching at 1440p or 4K until optional phase 10. DOS, Amiga, C64, Spectrum, ST and the other computers need cores first (decision 8). The rate is found automatically (per-game override, learned rate, arcade table, region from file name or header, system table), and only rates the monitor's EDID allows and the user has confirmed once are used.
- **Not true FreeSync:** DPU1's fixed timing, the HIDL composer and the DP-to-HDMI converters rule it out (out/dp-evidence/test43/freesync/FREESYNC.md).
- [ ] Phase 0: owner checks on the current build, read-only: SurfaceFlinger's fps heuristic for a PAL game at 1080p120, RetroArch's "[Core] Geometry ... FPS" log line, black time and monitor audio after a `try`; content-detection props; RetroArch at a 120 Hz base (swap interval); reading the cached EDID from sysfs.
- [ ] Phase 1: the vsync watchdog fix (plan §5.5): a 10 s grace, re-arm and vsync toggle after every monitor mode change and connect, first-vsync timing in the log. The same item as "Display plan Step 3's SurfaceFlinger part" in Displays; one fix closes both.
- [ ] Phase 2: kernel 1080p50 (VIC 31) and 1080p100 (VIC 64) records, a highest-rate field in the EDID gate (LISTED), code space; `test_vendor_patches.py`. vendor_dlkm write needs the owner's approval.
- [ ] Phase 3: framework `match`, `hold`, `release`, `confirm` verbs with a system_server 15 s `--confirm` timer, owner watchdog with a launch grace, confirm results keyed without `monitorKey` (null for the H27S17), holding marker, all new modes hidden from Auto and Output Resolution; `GammaosScreenArrangementCheck`.
- [ ] Phase 4: Nano resolver v1 and tables, Game Options > Monitor Refresh, Settings > Match Game Refresh Rate and Test Game Refresh Rates, switch before launch, release on every return path, `video_refresh_rate` and `video_swap_interval` written per matched launch and the user's values restored, first-use confirm, Quick Menu > Display > Normal Refresh Rate (exempt from the game lock); dosbox_pure and puae cores with dos/amiga systems. Exit criterion: a ~200-switch soak with no crash4 `dpu_clk_disable` abort.
- [ ] Phase 5: game rates 55, 57, 59, 70, 75: RANGE gate, a test module (long blank and long line for 70/75), then the shipping records; EDID labels and monitor key from the cached sysfs EDID (Java parser in system_server).
- [ ] Phase 6: learning the real rate (RetroArch log; later a SurfaceFlinger content vote).
- [ ] Optional: phase 7 composer exact-rate patch (0x66004), phase 8 games started outside Nano, phase 9 built-in screen 50 Hz (dtbo), phase 10 matching at 1440p and 4K (1440p50, 4K50 4:2:0).
- **(decide)** The plan's §10 decisions 1-16. First: the rate set (1), 50 or 100 for PAL (2), on by default (3, waits on the soak and crash4), adding the DOS and Amiga cores (8).

## Nano mode and Android Settings

### Nano pop-ups for SystemUI dialogs, and Settings stability without SystemUI (nano_systemui_plan.md): not started (user report, 2026-11-27)
- **Goal:** the "Allow wireless debugging on this network?" and "Allow USB debugging?" prompts work in Nano mode, and Android Settings stops freezing and flickering there; Android mode stays as it is.
- **Owner decision (2026-11-27): "Convert it to Nano pop-ups."** In Nano mode every SystemUI pop-up a Thimsy user can hit is shown by Nano (its own XMB/DSi/Minima style, controller-driven, 640x480-safe) and the answer goes back to Android, or is refused with a passive Nano notice where a decision says so (D13, D15, D16, D20). SystemUI stays off in Nano mode. Nothing is granted without a press; timeout or no Nano = deny; only system_server completes a grant. This replaces the Settings-hosted `ThimsyWifiDebuggingActivity`/`ThimsyUsbDebuggingActivity` (not kept as a fallback, see D10) and the separate Nano debugging prompt.
- **Cause:** Nano mode never starts SystemUI but leaves it installed. Both prompts are SystemUI activities, so Android starts SystemUI, which crashes in its Dagger init (no `statusbar` service in minimal boot), the prompt never shows and the switch flips back off. The dead prompt stays as the top activity and SystemUI is restarted and crashes each time it comes to the front again (after a second toggle, a Settings page change, Settings closing, a game ending); once it hid the Settings page and the input dispatcher waited for a focused window (test 42 A0/A2/t3 logcats: 12 of 12 SystemUI starts were this record; 25 SystemUI crashes in test 43). This is very likely the main part of the Settings instability; two test 43 bursts have no log, so their trigger is unconfirmed (Phase 0). The USB prompt is very probably broken the same way if `ro.adb.secure=1` (likely on this LineageOS userdebug build). Text toasts from apps targeting API 30+ are dropped silently in Nano mode (no status bar), and the first "Turn on Battery Saver?" confirmation and the Internet panel silently do nothing (they go to receivers SystemUI registers at run time). Also: System > Backup can crash Settings (backup service off in Nano), Seedvault crashes on every USB attach (40 in test 43), gammapad logs up to 600 lines/s, `[joypad_spi_read]` uses half a core. RescueParty is off.
- **Workaround today:** trust each Wi-Fi network once from Android mode ("Always allow on this network"); trust is per access point and band. Reboot if Settings stops responding after a toggle in Nano mode (`adb shell am force-stop com.android.systemui` probably clears it too, untried). With USB adb already authorised, `adb tcpip 5555` plus `adb connect` until reboot.
- [ ] Phase 0: owner's read-only checks (plan §10): `ro.adb.secure`, `service.adb.tcp.port`, config overlays, `am_proc_start` causes, Settings exit-info, ANRs and `data_app_crash`, load, `[joypad_spi_read]` priority and state; safe volume, USB attach filters, USB drives and pads, dropped-toast count, Battery Saver acknowledged flags, Nano's `init.svc.*` names, microphone.
- [ ] Phase 1a (small, 1.5-2 days): `NanoProcessGuard` refusing SystemUI in ActivityStarter and ProcessList in Nano mode (SystemUI never started, no dead window); without the prompt service, a new USB key is denied at once (`denyDebugging()`) and a USB permission request answered granted=false, so nothing waits forever; host test.
- [ ] Phase 1b (medium, 6.5-7.5 days): first a half-day spike (prompt-only raise from parked over Settings, RetroArch and DraStic; pad reading under EVIOCGRAB; `overlayPoll` while parked; `drop_fence`; orientation clamp). Then the pop-up bridge: `ThimsyPromptService` LocalService started before AdbService (1b methods only), pure codec and policy; `sys.gammaos.prompt.req` (`<id>.<seq>`) / `reply` / `ready` (with `handoff`) with their own SELinux types (neverallows exclude init; request set only by system_server, reply only by bootanim, which keeps system-signed apps out; root/su and the permissive bootanim domain are not kept out); `/data/system/nano_prompt/current.json` with its own file type, id and nonce (not a secret); any reply within 3 s, `busy` republished until the timeout, `answer` implies `shown` and carries Nano's `shown_ms`/`press_ms` for the 500 ms rule; Nano-lost check (`init.svc.*`, `ready` pid) with republish while time is left; Nano's own expiry; `cancel` on service start. Nano's `NanoMenuPrompt.cpp` (XMB renderer in every theme, prompt-only overlay raise over apps, home hook, 600 ms arming and fresh-press rule, focus on Deny, checkbox label on 2 lines, ellipsized untrusted values). `adb_wifi` with its own Wi-Fi state receiver; `adb_usb` sends `NO` only for the current prompt, closes a replaced one silently, unplug cancel only when `service.adb.tcp.port` is 0. English text; `test_thimsy_prompt.py`, `NanoPrompt_test.cpp`; README.
- [ ] Phase 1c (about 2.5 days): DSi and Minima pop-up styles, 640x480 checks, touch, dual-screen "answer on the other screen" card, translations into the 21 i18n files (D9).
- [ ] Phase 2 (about 2 days): USB permission as a Nano pop-up (`UsbUserPermissionManager`, grants and `PendingIntent` answered by system_server; "Always" box per D21, recommended left out because `setDevicePackage` does nothing in Nano mode); no app opened and no default-app grant on USB plug-in in Nano mode (D13).
- [ ] Phase 3 (2.5-3 days): text toasts as Nano banners (`TextToastRecord.show()`/`hide()` hook, `hide()` NPE avoided, the app's toast callback called from the prompt service off the `mToastQueue` lock; passive overlay banner over apps per D14, never setting `show_overlay`); the passive `notice` type.
- [ ] Phase 4 (about 1.5 days): refused-start reports (ActivityStarter, ProcessList with hosting type and component, broadcasts addressed to SystemUI in AMS); screen capture refused with a Nano notice (D15); the `battery_saver` pop-up ("Turn on Battery Saver?", completed by system_server).
- [ ] Phase 5 (about 3.5 days): brightness dialog to Nano's brightness bar, low-battery warning, notices for media output, contrast, sensor privacy and log access (D20), Internet panel to Nano's Wi-Fi screen (D20), app volume changes to Nano's volume bar, screenshot chord per D16; optional heads-up toasts (D18) and Wireless Debugging row in Nano (D6).
- [ ] Phase S (small, about 1 day, independent): `BackupSettingsHelper` null-safe; Seedvault per D4; gammapad "Device maps set" logged only on change.
- [ ] Device steps: plan §6.3 (1a: 1-3; 1b: 4-11, with the spike at 4; 1c: 12; Android mode: 13; then 14-22 by phase).
- **(decide)** The plan's §8. Resolved by the owner's decision: D1, D2; superseded: D3, D8. Open: D4-D7, D9, and the new D10-D21. First: D10 (no Nano process to draw: deny Wi-Fi, hold USB), D11 (over games: no pause, if the spike shows it works; over the Quick Menu: on top), D12 (timeouts 60 s / 30 s), then D21 (USB "Always" box), D13 (USB open-with), D14 (toasts over apps), D15 (screen capture), D20 (notice instead of a pop-up: media output, contrast, log access; Internet panel to Nano's Wi-Fi screen), D4 (Seedvault).

## Small items from this session

- **Game list folders:** removing a default ROMs folder (e.g. ROMs/snes) removes it on every drive at once. **(decide)** Remove only the chosen location instead?
- **Dual-screen text diagnostics:** test 36 logs the panel's text drawing state every 10 s. Remove it once the block letters are confirmed fixed.
- **Mixed-up translations noticed in review:**
  - ru "Remove" and ja "Remove" read as "delete";
  - de "Open" is "Offen" where "Öffnen" is meant in places.
