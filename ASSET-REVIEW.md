# Sony and Nintendo assets to replace: review list

For review (written 2026-11-12, research only; nothing changed). It lists every Sony- or
Nintendo-derived asset the 1.0a image ships, how the user meets each one, and what a replacement must
match. Evidence details are in `ASSETS.md`. APKs and emulator data are in
`plan_remove_unlicensed_files.md`.

Path prefixes: `ps3xmb/` = `frameworks/base/cmds/gammaos-nano/ps3xmb/` (installed to
`/system/etc/nano_xmb/`), `nano/` = `frameworks/base/cmds/gammaos-nano/`, both in `~/gammaos-src`.
Every asset can be overridden at runtime by a file of the same name under
`/data/system/nano_xmb/<dir>/`; the loaders check that first.

Thimsy's defaults matter for how often a user meets these:
- The home is the ES-DE theme, with the Minima look for settings, so Nano's own text (Rodin) is seen
  daily.
- The XMB and DSi home themes are options in Theme Settings.
- Menu sounds are off (`persist.gammaos.nano.nav_sounds=0`). The DSi music plays whenever the DSi
  theme is on.

## Summary

- **To replace: 19 rows.**
  - Sony PS3: 12. That is the Rodin font, the XMB icons, their normal maps, the three player icon
    sets, the music cover, the wave data, the Photo Gallery background, the SE sounds, and two
    code-embedded captures (the particle cloud and the globe camera paths).
  - Sony PSP: 1. The clock numerals, which are code-embedded and never shown on the RG Cube.
  - Nintendo DSi: 6. That is the DSVec font, the frame and tile, the launch ring, three small
    sprites, the menu music, and the five menu sounds.
- **To remove (shipped, never loaded): 6 rows.**
- **Already out of the image: 4 rows.**
- **Unsure (ask upstream, or just regenerate): 2 rows.**

## 1. Sony: PS3 XMB theme files

| Asset(s) | What it is | How it's used | Owner and evidence | Planned replacement |
|---|---|---|---|---|
| `ps3xmb/fonts/ps3-rodin-regular.ttf` | "SCE-PS3 Rodin LATIN" Regular, the PS3 system font | Nano's main font: all text in the XMB, Minima and DSi themes (except the DSi labels), the settings screens, dialogs, the on-screen keyboard, the boot status line, and drastic-nano's in-game overlay menu. It is the text the user sees in Settings every day (`openFontFaces` in `nano/NanoMenuRender.cpp:4688-4701`; `drastic-nano/OverlayGfx.cpp:267-275`) | Fontworks Japan for SCE. CONFIRMED: the name table says "Copyright (c)2006 Fontworks Japan, Inc. All Rights Reserved.", vendor FWKS | An OFL sans with narrow metrics (Noto Sans, Source Sans 3, or the Akrobat Nano already ships), renamed (e.g. `xmb-regular.ttf`) and updated in both loaders. **Glyphs:** ASCII, all of Latin-1, Cyrillic (for `ru`) and `– — ‘ ’ ‚ “ ” „ † ‡ • … ‰ ‹ › € ™ № √ ∞ ≈ ≠ ≤ ≥ Œ œ Š š Ÿ ƒ`. Latin Ext-A (pl, tr) and Greek (el) are welcome; today they fall back to Roboto. CJK, Hangul, Arabic, Hebrew and Thai fall back to the system Noto fonts (`NotoSansCJK-Regular.ttc`, `NotoNaskhArabic`, `NotoSansHebrew`, `NotoSansThai`), so they are not needed. **Metrics:** Rodin is 1000 units/em, ascender 780, descender -193, gap 62, average width 501. Check long labels against the truncation |
| `ps3xmb/icons/xmb_icon_000..070.png` | PS3 XMB category and item icons, 512x512 RGBA white silhouettes on transparent | The icons of the categories and items on the XMB home, and of the XMB-style settings rows, drawn as relit glass. Index 0 is the user's smiley-house (`loadPs3IconTex`, `nano/NanoMenuPS3Menu.cpp:209-216`, names built at `:483` and `:645`) | Sony. CONFIRMED by content: the PS3 icon set itself, including the PSP, Remote Play and trophy icons and the Walkman "W" | Redraw an original set: 512x512 RGBA, white on transparent, the same index meanings (`NanoMenuPS3Data.h`), or map the items to the RetroArch/AOSP icons Nano already ships |
| `ps3xmb/normalmaps/nmap_000..069.png` | 128x128 tangent-space normal maps of those icons | Give the XMB icons their glass bevel and lighting (`loadPs3NmapTex`, `nano/NanoMenuPS3Icons.cpp:338-347`; `nmapForIcon` `:913`) | Sony (derived). CONFIRMED: shapes match the icons index-for-index | Regenerate from the new icons (Nano already builds a bevel from the alpha, `bevelFromRGBA`), or emit 128x128 RGBA normal maps (RGB normal, A mask) with the same indices |
| `ps3xmb/audioplayer/icon_000..071.png` | PS3 music player control panel: play, pause, skip, repeat, shuffle, the digits 0-9, the MP3/PCM/CD/WMA/ATRAC/AAC badges, (i+), volume | The Music player's Now Playing screen: the transport controls, the track-number digits, the codec badge and the control-panel menu (`nano/NanoMenuPS3Icons.cpp:471-478`) | Sony, PLAUSIBLE. PS3 control-panel art, with the ATRAC badge (Sony's codec) and the PS3 (i+) glyph; the code calls it "the web app's images/audioplayer set" | Redraw: RGBA, grey-white glossy, the same file numbers and sizes. The sizes are 74x74 (000-006), 98x74 (007, 009), 72x36 (008), 162x162 (010), 240x240 (011), 46x68 digits (012-021), 110x28 badges (022-027), 104x24 (028), 70x70 and 98x70 panel icons (029-070, with the dimmed and blurred variants), and 1x1 (071) |
| `ps3xmb/videoplayer/icon_000..024.png`, `icon_114.png` | PS3 video player control panel | The Video player's on-screen controls and options panel: info, scene search, audio and subtitle, transport (`nano/NanoMenuPS3Icons.cpp:502-507`) | Sony, PLAUSIBLE (the same set and style) | Redraw: RGBA 70x70 (017 is 98x70; 114 is 96x96), the same numbers |
| `ps3xmb/photoviewer/icon_000..027.png` | PS3 photo viewer control panel | The Photos viewer's controls: rotate, zoom, slideshow, display (`nano/NanoMenuPhotos.cpp:2123-2128`) | Sony, PLAUSIBLE (the same set and style) | Redraw: RGBA 70x70 (006 is 98x70), the same numbers |
| `ps3xmb/assets/icon_fw_track.png` | Grey music-note cover, 128x128 RGBA | The cover shown for a song without album art in the Music player (`mpJacket`, `nano/NanoMenuPS3Icons.cpp:526-531`) | Sony, PLAUSIBLE. The code names it `plane_default_music_cover`, a PS3 resource name; it is in the XMB icon style | Redraw: 128x128 RGBA in colour, or draw a note glyph at runtime |
| `ps3xmb/wave/wave_geo.bin`, `wave_seq2.bin` | The PS3 cloth "wave": a 128x128 mesh (pos, normal, uv) plus 85 animation keyframes, captured from PS3 firmware in RPCS3 | The animated silk wave over the month-coloured background: the XMB home's default wallpaper ("XMB Wave"), also offered as a DSi background effect, and the music player's Waves visualizer (`ps3bg` in `nano/NanoMenuPS3Bg.cpp:534` and `:591`; effect 22 in `nano/NanoMenuEffects.cpp:294`) | Sony. CONFIRMED: `wave_seq2.json` says "The SPU cloth is noise-driven", "loopSourceEmuFrames [8515, 8933]", "12s capture" | Generate procedurally. Either make Nano's existing procedural ribbon (effect 21) the default and remove these files, or write a generator (sum of noise-driven sines on a 128x128 grid) that emits the same formats. `wave_geo.bin`: 16384 vertices x 9 float32 (589,824 bytes; normal in columns 4-6, uv in 7-8). `wave_seq2.bin`: header `WSQ2`, u32 frameCount, u32 gridN=128, u32 vertStride=3, u32 format=1, f32 zwRatio (24 bytes), then frames of 128²x3 float16 (98,304 bytes each), looped at 12 keyframes/s |
| `ps3xmb/backgrounds/cinfo-bg-photogallery.jpg` | 1920x1080 "Photo Gallery" art: scattered photos with camera glyphs | The background behind the Photo Gallery (Photos) item on the XMB home while it is focused (`nano/NanoMenuPS3Menu.cpp:8418-8430`, stb_image) | Sony, PLAUSIBLE (the PS3 Photo Gallery app). CONFIRMED not ours: the XMP says Photoshop CS4 Mac, 2010-07-16 +09:00 (Japan); stock photos including children | New art: 1920x1080 JPEG (any stb-readable JPEG/PNG), dark on the left where the text sits, original or CC0 photos |
| `ps3xmb/audio/SE02_Cursor.wav`, `SE03_Normal_OK.wav`, `SE04_Back.wav`, `SE05_Category_OK.wav`, `SE08_Option.wav`, `SE09_Error.wav` | PS3 menu sound effects | XMB theme with Navigation Sounds on: moving the cursor, confirming, going back, changing category, opening the Triangle option menu, and an error (`ps3Sfx`, `nano/NanoMenuPS3Boot.cpp:145-160`; also `earlySfxOneShot` before boot completes) | Sony, by name. SE03, SE05 and SE08 are the same file; SE09 is silent (112 zero samples) | Synthesise original clips (a script kept in the repo) and rename them (e.g. `xmb_cursor.wav`, updating `kFiles`). **Format:** PCM WAV, 16-bit, mono, 48 kHz, about 40 ms (1904 frames) each, played at gain 0.8; distinct sounds for OK, category and option are fine |
| Code: `nano/NanoMenuPS3ParticleCloud.h` | 800 eye-space points (x, y, z) of the XMB glitter cloud | The sparkling particles that drift over the XMB wave (`ps3part`, `nano/NanoMenuPS3Particles.cpp`) | Sony. CONFIRMED: the header says "Firmware-extracted eye-space particle cloud (RPCS3 draw 36)" | Generate procedurally (a seeded random point cloud in the same volume) inside `NanoMenuPS3Particles.cpp` |
| Code: `nano/NanoMenuMusicGlobeScenes.h` | Camera paths for 5 globe scenes (32 keyframes each: eye, forward, up, fov), sun direction | The camera moves of the Music player's Globe visualizer (`ps3mpglobe`, `nano/NanoMenuMusicGlobe.cpp`) | Sony. CONFIRMED: "decoded from the captured view-projection regs" of the firmware's globe | Author new camera paths (hand-set orbits) in the same `[scene][kf][10]` layout |

## 2. Sony: PSP

| Asset(s) | What it is | How it's used | Owner and evidence | Planned replacement |
|---|---|---|---|---|
| Code: `nano/NanoMenuPS3ClockGlyphs.h` | Vector outlines of the 12, 3, 6 and 9 numerals | The PSP Go-style slide clock, shown while the screen is slid open (`KEY_F12`, `persist.gammaos.nano.pspclock`). The RG Cube does not slide (`ro.gammaos.nano.no_slide=1`), so Thimsy users never see it (`nano/NanoMenuPS3Clock.cpp`) | Sony, PLAUSIBLE. "AUTO-GENERATED ... from psp_clock.js NUM_GLYPHS", from a PSP Go clock recreation | Rasterise the numerals from the new UI font at runtime and delete the header, or leave the feature out of RG Cube builds |

## 3. Nintendo: DSi System Menu theme

| Asset(s) | What it is | How it's used | Owner and evidence | Planned replacement |
|---|---|---|---|---|
| `ps3xmb/nds/dsvec.ttf` | "DSVec" letters. The font itself is "Mirsany" by Pikgura (2023), not a Nintendo font | The rounded font of the DSi theme's labels | Pikgura (commercial foundry). CONFIRMED: name table, no licence | **Resolved (test 22):** no longer installed; the DSi theme uses M PLUS Rounded 1c (OFL, `dsvecnum.ttf`) for letters and digits |
| `ps3xmb/nds/nds_frame.png`, `nds_tile.png` | The DSi launcher's glossy blue selection frame (with its START platform) and the white "pillow" app tile, as 64x80 and 64x64 pixel art drawn at 8x | The frame around the centred app in the DSi home carousel (hue-shifted to the chosen colour), and the white tile behind every app icon in the carousel (`ensureNdsAssets`, `nano/NanoMenuRender.cpp:959-962`; `ndsLoadTex`, `nano/NanoMenuPS3Icons.cpp:355-360` and `:404-406`) | Nintendo. CONFIRMED by the code ("the REAL cell_00 selection-frame sprite", "tile_white", "Firmware geometry: 64x80") and the exact 8x upscale | Redraw (or draw procedurally; a fallback already exists, `NanoMenuRender.cpp:3010`). `nds_frame`: 512x640 RGBA, transparent centre window, a blue body that hue-rotates well. `nds_tile`: 512x512 RGBA |
| `ps3xmb/nds/nds_ring_00..35.png` | 36-frame sparkle ring of blue dots, 128x144, 4-bit palette | The ring that spins and fades when a game is launched from the DSi home (`ensureNdsRing`, `nano/NanoMenuRender.cpp:986-995`) | Nintendo. CONFIRMED: "The 36 sparkle-ring frames (launcher_d cell_53..88)" | Generate procedurally (a ring of dots that rotates and shrinks over 36 frames) or redraw: 36 frames, 128x144, RGBA or palette with alpha |
| `ps3xmb/nds/nds_photo.png`, `dialog_box_blue.png`, `nds_batt.png` | The top-screen photo panel (a mint field with a camera glyph, 222x170), the dialog box (white with a blue rim, 224x156), and the full-battery sprite (17x11) | The upper-screen panel when the DSi top screen is stacked over the carousel; the body of DSi-theme dialogs (default colour, light mode); the battery icon when the level is unknown (`nano/NanoMenuRender.cpp:963-964`, `:2096`) | Nintendo. CONFIRMED by the code's names ("photo_U", "the real msk_dialog_BG sprite", "spr_batt_full") | Redraw or draw procedurally (fallbacks exist for the dialog and the battery). The same sizes, RGBA |
| `ps3xmb/audio/menu_ambiance.wav` | DSi home background music, a 30.37 s loop | Plays continuously while the DSi home is on screen (on by default, `persist.gammaos.nano.nds.ambiance`), restarting at the end (`ndsAmbianceTick`, `nano/NanoMenuPS3Boot.cpp:331-380`) | Nintendo, PLAUSIBLE (the DSi menu music; the audio was not compared, no metadata; the theme's sprites are confirmed rips) | Compose an original loop or use a CC0 one. **Format:** PCM WAV, 16-bit, stereo, 48 kHz, about 30 s, seamless loop (it restarts at the end), played at about 0.56 gain |
| `ps3xmb/audio/nav_blip.wav`, `app_launch.wav`, `settings_nav.wav`, `settings_back.wav`, `settings_enter.wav` | DSi menu sound effects | DSi theme with Navigation Sounds on: moving along the carousel (0.215 s), launching an app (0.742 s), moving in, leaving and entering a settings list (0.014 s, 0.295 s, 0.265 s) (`ndsSfxPlay`, `nano/NanoMenuPS3Boot.cpp:100-125`) | Nintendo, PLAUSIBLE (as above) | Synthesise originals. **Format:** PCM WAV, 16-bit, stereo, 48 kHz, similar lengths |

## Unused: remove (shipped, never loaded)

| Asset(s) | Owner | Note |
|---|---|---|
| `ps3xmb/fonts/ps3-rodin-bold.ttf`, `ps3-rodin-light.ttf` | Sony (Fontworks) | Only Regular is opened; drop them from the `nano_xmb_fonts` glob |
| `ps3xmb/icons/xmb_icon_psn.png` | Sony (the PSN/PlayStation logo) | Never loaded; drop it with the icon set |
| `ps3xmb/wave/wave_boot.bin` (5.5 MB) | Sony (boot-wave capture) | The boot intro is gone; the `wave/*.bin` glob still installs it |
| `ps3xmb/gradients/month_bg_00..23.png` | PS3 month colours (upstream port) | The background is now computed in the shader; remove the `nano_xmb_gradients` module |
| `ps3xmb/assets/icon_fw_folder.png` | Sony, PLAUSIBLE (PS3 music folder icon) | Never loaded; installed by the `assets/*.png` glob |
| `ps3xmb/textures/noise.png` | Unknown | Never loaded; remove the `nano_xmb_textures` module |

## Already out of the image (the Thimsy patch dropped Nano's boot intro)

The tree still has these files, and the banned-file check should keep them out of the image.

| Asset(s) | Owner |
|---|---|
| `ps3xmb/boot/{logo,footer,blur}_{gold,sd_gold}.png` | Sony (PlayStation logo, "PS3", "PlayStation 3"; CONFIRMED) |
| `ps3xmb/audio/coldboot_stereo.wav` | Sony (PS3 cold-boot sound) |
| `ps3xmb/nds/hs_triangle.png` | Nintendo, PLAUSIBLE (DSi Health & Safety warning) |
| `ps3xmb/audio/boot_chime.wav`, `menu_enter.wav`, `touch_continue.wav` | PLAUSIBLE DSi boot sounds ("touch to continue") |

## Unsure: may be firmware captures

| Asset(s) | How it's used | Why unsure | Suggestion |
|---|---|---|---|
| `ps3xmb/assets/icon_amb.png` (16x12), `texenv.png` (64x64) | The colour ramp and silver reflection map that give XMB and system icons their glass look (`nano/NanoMenuPS3Icons.cpp:316-326`) | From the same capture-based port ("ICON_MATERIAL_DAY"); no metadata | Regenerate (a gradient and a soft matcap), so there is nothing to ask |
| `ps3xmb/canyon/canyon_normalmap.png` (128x128 RGBA) | The terrain of the Music player's Canyon visualizer (`nano/NanoMenuMusicCanyon.cpp:306-320`) | Same port; no metadata | Regenerate from seeded noise |

## Other assets we have no right to ship (not Sony or Nintendo)

| Asset(s) | What it is | How it's used | Owner | Status / planned action |
|---|---|---|---|---|
| `ps3xmb/nds/dsvec.ttf` | "Mirsany", a commercial font | DSi theme labels | Pikgura | **Resolved (test 22):** replaced by M PLUS Rounded 1c (OFL) |
| `colorful/_inc/systems/images/quake.webp` | Official Quake Champions render | The Quake system's picture in the Colorful ES-DE theme | id Software / Bethesda | **Removed (test 22).** A replacement is needed: an original or openly licensed Quake-system image |
| `colorful/_inc/fonts/Gilroy-*.ttf` | Commercial font | Text in the Colorful ES-DE theme | Radomir Tinkov | **Resolved (test 22):** replaced by an OFL font (see NOTICE) |
| `vendor/lineage/bootanimation/bootanimation.tar` | GammaOS cube logo | Boot animation when starting full Android | GammaOS (TheGammaSqueeze) | **Resolved (test 22):** replaced by the Thimsy logo on midnight blue |
| DraStic: `drastic_r2.6.0.4a.apk`, `drastic.tar.zst`, `libdrastic_arm64.so` / `libdrastic_cpu.so`, the drastic-nano seed data | Closed, paid DS emulator: the app, its data, its native core (which the built-in DS player runs) and its BIOS replacements, game database and cheats | Default DS emulator and the built-in DS player | Exophase | **To do:** ship none of it; make WatermelonDS (GPL) the default DS emulator; drastic-nano loads DraStic from the user's own install when present (see `plan_remove_unlicensed_files.md` step 2) |
| MiXplorer 6.64.3 | Closed freeware file manager | Android file manager | HootanParsa | **Removed** (dropped, Nano's File Explorer covers it; SMB/FTP server planned) |
| Daijisho 1.5.0 (the app) | Closed launcher app (its platform files are MIT; Nano's copy stays) | Launcher in full-Android mode | TapiocaFox | **Removed (test 22).** Trebuchet is the full-Android launcher |
| GammaToast (`gammaos/toast/Toast.apk`) and GammaDisplayLoading (`gammaos/toast/gammaos-displayloading.apk`) | GammaOS system apps shipped as prebuilt APKs; no source in the tree | Toast messages and the display-loading screen | GammaOS (TheGammaSqueeze) | **To check:** they come from GammaOS's Apache-licensed repo, so redistribution is likely fine, but the source isn't published. Ask TheGammaSqueeze for it, or replace them |
| RetroArch cores `snes9x`, `genesis_plus_gx`, `picodrive`, `fbneo`, `mame2003_plus`, `opera` | Emulator cores under non-commercial licences | Default cores for SNES, Genesis/Mega Drive, 32X, arcade and 3DO | their authors | **Keep:** Thimsy permits no commercial use of its own assets, so these licences fit. Anyone selling a device with Thimsy would still have to remove them (GPL alternatives: bsnes/mesen-s for SNES, clownmdemu for Genesis, mamearcade for arcade; none for 32X or 3DO) |
| RG Cube vendor, kernel and bootloader (Anbernic firmware 1.2) | Proprietary device firmware (GPU driver, modem, SoC support) | Required to boot | Anbernic / Unisoc | **Known:** every custom firmware for the device redistributes these without explicit permission from Anbernic; nothing to replace |

## Appendix: the "To be replaced" list (moved from ATTRIBUTION.md, kept out of the public repo)

These files are in the image but are not licensed to Thimsy OS. They are listed with their
owners so nobody mistakes them for licensed material; [ASSETS.md](ASSETS.md) and
`plan_remove_unlicensed_files.md` track their replacement.

| files | what they are | owner |
|---|---|---|
| `ps3xmb/fonts/ps3-rodin-{regular,bold,light}.ttf` | "SCE-PS3 Rodin LATIN", the XMB font | Fontworks Japan, for Sony Computer Entertainment |
| `ps3xmb/icons/xmb_icon_000..070.png`, `xmb_icon_psn.png` | the PS3 XMB icons, including the PSN/PlayStation and Walkman logos | Sony |
| `ps3xmb/normalmaps/nmap_000..069.png` | normal maps made from those icons | Sony (derived) |
| `ps3xmb/audioplayer/*`, `videoplayer/*`, `photoviewer/*` | the PS3 player control panels | Sony (inferred) |
| `ps3xmb/assets/icon_fw_track.png`, `icon_fw_folder.png` | the PS3 music cover and folder icons | Sony (inferred) |
| `ps3xmb/wave/wave_geo.bin`, `wave_seq2.bin`, `wave_boot.bin` | the PS3 wave, captured from the firmware | Sony |
| `ps3xmb/backgrounds/cinfo-bg-photogallery.jpg` | the PS3 Photo Gallery background | Sony (inferred) |
| `ps3xmb/audio/SE02_Cursor.wav`, `SE03_Normal_OK.wav`, `SE04_Back.wav`, `SE05_Category_OK.wav`, `SE08_Option.wav`, `SE09_Error.wav` | the PS3 menu sounds | Sony |
| `ps3xmb/gradients/*`, `ps3xmb/textures/noise.png` | unused month gradients and texture | unknown (upstream port of the PS3 colours) |
| `ps3xmb/assets/icon_amb.png`, `texenv.png`, `ps3xmb/canyon/canyon_normalmap.png` | glass material ramps and the Canyon visualizer map | unknown; thought to be captured from the PS3 |
| the particle cloud, globe camera paths and clock numerals built into Nano (`NanoMenuPS3ParticleCloud.h`, `NanoMenuMusicGlobeScenes.h`, `NanoMenuPS3ClockGlyphs.h`) | data captured from the PS3 firmware | Sony |
| `ps3xmb/nds/dsvec.ttf` | the "Mirsany" font | Pikgura |
| `ps3xmb/nds/nds_frame.png`, `nds_tile.png`, `nds_ring_00..35.png`, `nds_photo.png`, `dialog_box_blue.png`, `nds_batt.png` | sprites from the Nintendo DSi launcher | Nintendo |
| `ps3xmb/audio/menu_ambiance.wav`, `nav_blip.wav`, `app_launch.wav`, `settings_nav.wav`, `settings_back.wav`, `settings_enter.wav` | the DSi menu music and sounds | Nintendo (inferred) |
| the bootloader's charging pictures (`logo.img`, not the system image; the fastboot logo now shows Thimsy) | Anbernic's pictures | Anbernic |
| Colorful's `quake.webp` | a Quake Champions render | id Software / Bethesda; removed from the image, a replacement is needed |
