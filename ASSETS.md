# Thimsy OS: non-code assets, their origin and licence

Audit of 2026-11-12 (research only; nothing was changed). It covers the images, fonts and sounds the
1.0a build (GammaOS Next 1.4.4 built from source, `SYSTEM_IMG=`) puts in the image: Nano's
`prebuilt_etc` modules, the Thimsy mods, and the GammaOS base. APKs, emulator data and the bundled
ROMs are covered by `plan_remove_unlicensed_files.md` and are not repeated here. The 1.0 build reuses
the official 1.2 release's system image as is, and this audit does not cover that image.

"Shipped" means the file is in `out/target/product/tdgsi_arm64_ab/installed-files.txt` (build of
2026-11-11), or a mod installs it. Upstream GammaOS (TheGammaSqueeze) reaches us as one squashed
commit (`0bc531e2d64`). So git history can only tell upstream files (in that commit) from this
project's files (in `patches/thimsy/0001-thimsy.patch`). The patch adds no binary asset; its only new
asset is `bootscreen/thimsy.svg`.

Evidence grades: **CONFIRMED** means the file's metadata, the history or the content proves it.
**PLAUSIBLE** means it is inferred from names, context or style.

Path prefixes: `ps3xmb/` = `frameworks/base/cmds/gammaos-nano/ps3xmb/` and `nano/` =
`frameworks/base/cmds/gammaos-nano/`, both in `~/gammaos-src`. `colorful/` = the Colorful
(Simplified) theme at commit `669e7f6`, which `mods/50-nano.sh` downloads to
`/system/etc/nano_esde_themes/colorful-simplified-es-de/`.

## Changes since the audit

Made on 2026-11-12, after the audit; the tables below still show the state they audited.

- **Akrobat is not OFL.** ES-DE ships it under Fontfabric's Free Font EULA 2.0
  (`licenses/Akrobat_Fontfabric.pdf` in ES-DE's repository), which forbids redistributing the
  font itself without Fontfabric's written consent. Plan 1.5 was wrong; Akrobat counts as REPLACE.
- `colorful/_inc/fonts/Gilroy-*.ttf` are replaced by Urbanist 1.330 and `Akrobat-Bold.ttf` by
  Barlow Semi Condensed Bold 1.422 (both OFL, with their OFL.txt), and `quake.webp` is removed
  (`mods/50-nano.sh`). Nano's `akrobat-regular.ttf` and `akrobat-semibold.ttf` get Barlow Semi
  Condensed Medium and SemiBold under the same names, because Nano loads those paths.
- Font Awesome 4.7.0 (OFL 1.1, per its README at `v4.7.0`) and Exo 2 (OFL 1.1, per Google
  Fonts' `ofl/exo2`) are confirmed upstream.
- The boot animation is the Thimsy logo (`mods/58-bootanimation.sh`,
  `tools/bootanimation/make_bootanimation.py`), and the AOSP tree's
  `vendor/lineage/bootanimation/bootanimation.tar` and `desc.txt` are regenerated to match.
- The Colorful ASK row for the nine artist-credited pictures is decided: keep them and credit
  their artists. [NOTICE](NOTICE) and [ATTRIBUTION.md](ATTRIBUTION.md) credit them and the items this
  audit found uncredited.

## Summary

Rows that ship in the 1.0a image: **KEEP 22, REPLACE 22, ASK 8**. The ASK count includes Anbernic's
charging pictures, already plan item M. On top of these:

- 1 KEEP row is for the 1.2 base only (the ES-DE Magisk fix).
- 7 rows cover files that are in the source tree but no longer installed (the Thimsy patch already
  dropped Nano's boot intro).

REPLACE (all of these ship today; the 22 rows are grouped into 17 items here):

1. `ps3xmb/fonts/ps3-rodin-{regular,bold,light}.ttf`: "SCE-PS3 Rodin LATIN", (c) 2006 Fontworks Japan (CONFIRMED).
2. `ps3xmb/icons/xmb_icon_000..070.png`, `xmb_icon_psn.png`: the PS3 XMB icon set, including the PSN/PlayStation and Walkman logos (CONFIRMED).
3. `ps3xmb/normalmaps/nmap_000..069.png`: normal maps of those same icons (CONFIRMED).
4. `ps3xmb/audioplayer/*`, `ps3xmb/videoplayer/*`, `ps3xmb/photoviewer/*`: PS3 control-panel icons (PLAUSIBLE).
5. `ps3xmb/assets/icon_fw_track.png`: the PS3 default music cover (PLAUSIBLE).
6. `ps3xmb/wave/wave_geo.bin`, `wave_seq2.bin`: the PS3 SPU cloth wave, captured in RPCS3 (CONFIRMED).
7. `ps3xmb/backgrounds/cinfo-bg-photogallery.jpg`: the PS3 Photo Gallery background (CONFIRMED not ours; Sony PLAUSIBLE).
8. `ps3xmb/audio/SE02,03,04,05,08,09*.wav`: the PS3 menu sounds (by name, as in the plan).
9. Shipped but never loaded, so they can be dropped rather than replaced: `ps3xmb/gradients/*`, `ps3xmb/textures/noise.png`, `ps3xmb/assets/icon_fw_folder.png`, `ps3xmb/wave/wave_boot.bin`.
10. `ps3xmb/nds/dsvec.ttf`: "Mirsany" by Pikgura, 2023, with no licence (CONFIRMED by its name table).
11. `ps3xmb/nds/nds_frame.png`, `nds_tile.png`: the DSi launcher's `cell_00` and `tile_white` sprites (CONFIRMED by the code).
12. `ps3xmb/nds/nds_ring_00..35.png`: the DSi launcher's `launcher_d` cells 53..88 (CONFIRMED by the code).
13. `ps3xmb/nds/nds_photo.png`, `dialog_box_blue.png`, `nds_batt.png`: the DSi sprites `photo_U`, `msk_dialog_BG` and `spr_batt_full` (CONFIRMED by the code).
14. `ps3xmb/audio/menu_ambiance.wav`: the DSi menu music (PLAUSIBLE).
15. `ps3xmb/audio/{nav_blip,app_launch,settings_nav,settings_back,settings_enter}.wav`: the DSi menu sounds (PLAUSIBLE).
16. `colorful/_inc/fonts/Gilroy-{Bold,Light,Regular,SemiBold}.ttf`: (c) 2016 Radomir Tinkov, all rights reserved (CONFIRMED).
17. `colorful/_inc/systems/images/quake.webp`: a Quake Champions render taken from imgbin.com (CONFIRMED by the theme's credits).

Corrections to `plan_remove_unlicensed_files.md`:

- The PS3 boot plates (item E), `coldboot_stereo.wav` (item G) and the other boot-intro files no
  longer ship. The Thimsy patch removed the `nano_xmb_boot` module and lists `nano_xmb_audio` by
  file, and `loadPs3BootPlate` is gone from the code.
- Only the six SE sounds of item G still ship.
- The `*_white` boot plates are GammaOS's own logo, not Sony's.

## 1. Nano PS3 (XMB) theme

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `ps3xmb/fonts/ps3-rodin-regular.ttf`, `-bold.ttf`, `-light.ttf` | XMB Latin UI font. Nano and drastic-nano load only Regular; Bold and Light ship through the `*.ttf` glob but nothing loads them | third party: Fontworks Japan, made for SCE | CONFIRMED: the name table says "Copyright (c)2006 Fontworks Japan, Inc. All Rights Reserved.", family "SCE-PS3 Rodin LATIN", vendor FWKS, fsType 4 | none (commercial) | REPLACE |
| `ps3xmb/fonts/akrobat-regular.ttf`, `akrobat-semibold.ttf` | ES-DE's default face, used for theme text that has no `fontPath` | third party: Fontfabric (Plamen Motev), by way of ES-DE | the name table says "Copyright (c) 2016 by Fontfabric LLC. All rights reserved." with no licence field; plan 1.5 records it as OFL, which this audit could not verify offline | OFL per plan 1.5 (not in the file) | KEEP (verify, then ship the licence text) |
| `ps3xmb/fonts/fontawesome-webfont.ttf` | icon glyph fallback (ES-DE's PUA glyphs) | third party: Font Awesome 4.7.0 (Dave Gandy) | CONFIRMED: the name table gives version 4.7.0, licence URL fontawesome.io/license | SIL OFL 1.1 (the font) | KEEP |
| `ps3xmb/icons/xmb_icon_000..070.png`, `xmb_icon_psn.png` | XMB category and item icons, 512x512 RGBA white silhouettes (`xmb_icon_psn` is never loaded) | third party: Sony (PS3 firmware), ported by upstream | CONFIRMED by content: the PS3 XMB set (user smiley, PSP, Remote Play, trophy, PS3 tools), the **PSN logo with the PlayStation symbol** and the **Sony Walkman "W"** | none | REPLACE |
| `ps3xmb/normalmaps/nmap_000..069.png` | 128x128 tangent-space normal maps for the glass relight of those icons | third party: derived from the Sony icons | CONFIRMED: `nmap_000`/`005`/`069` are the house-smiley, controller and finger icons, matching `xmb_icon_000`/`005`/`069` by index | none | REPLACE |
| `ps3xmb/assets/icon_amb.png`, `texenv.png` | glass material's ambient ramp (16x12) and silver matcap (64x64) | upstream port of the web app's `ICON_MATERIAL_DAY` | PLAUSIBLE firmware capture: the code says it is "1:1 port of the web app's FS_ICON_GLASS ... ICON_MATERIAL_DAY", and the same web app lifted data from RPCS3. No metadata | none found | ASK (upstream: how were they made? If unknown, regenerate, which is trivial) |
| `ps3xmb/assets/icon_fw_track.png` | music cover placeholder, 128x128 | third party: PS3 firmware (PLAUSIBLE) | the code calls it "the web's default cover / plane_default_music_cover", a firmware resource name; the art is in the XMB icon style | none | REPLACE |
| `ps3xmb/assets/icon_fw_folder.png` | music folder icon, 128x128 | as above | no code loads it (grep); ships through the `assets/*.png` glob | none | REPLACE (drop: unused) |
| `ps3xmb/textures/noise.png` | 128x128 colour swirl | unknown, upstream | no code loads it (grep); ships through `nano_xmb_textures` | none found | REPLACE (drop: unused) |
| `ps3xmb/gradients/month_bg_00..23.png` | 64x32 per-month background gradients | upstream port of the PS3 month colours | no code loads them; the gradient is now computed (`NanoMenuPS3Bg.cpp:298`, "firmware month-base path") | none found | REPLACE (drop: unused) |
| `ps3xmb/wave/wave_geo.bin`, `wave_seq2.bin` | the default wallpaper's cloth-wave mesh: 128x128 vertices (pos, normal, uv as float32) and 85 float16 keyframes | third party: PS3 firmware output, captured by upstream | CONFIRMED: `wave_seq2.json` says "The SPU cloth is noise-driven ...", "loopSourceEmuFrames [8515, 8933]", "12s capture"; `wave_boot.json` says "PS3 native 60fps"; `NanoMenuPS3ParticleCloud.h` shows the capture method ("Firmware-extracted ... RPCS3 draw 36") | none | REPLACE |
| `ps3xmb/wave/wave_boot.bin` | 42-keyframe boot-wave capture (5.5 MB) | as above | no code loads it (the boot intro is removed); ships through the `wave/*.bin` glob | none | REPLACE (drop: unused) |
| `ps3xmb/backgrounds/cinfo-bg-photogallery.jpg` | 1920x1080 "Photo Gallery" item background | third party: Sony (PS3 Photo Gallery app, 2010), PLAUSIBLE | CONFIRMED not ours: the XMP says Photoshop CS4 Macintosh, created 2010-07-16T21:08:33+09:00 (Japan), saved 2011-12-12. The content is stock photos, including identifiable children | none | REPLACE |
| `ps3xmb/canyon/canyon_normalmap.png` | 128x128 normal and height map for the Canyon music visualizer | upstream port of `canyon_port.js` | PLAUSIBLE firmware-derived: the same capture-based web app; no metadata | none found | ASK (upstream; if unknown, regenerate with noise) |
| `ps3xmb/audioplayer/icon_000..071.png` | music-player control panel: transport, digits 0-9, the MP3/PCM/CD/WMA/ATRAC/AAC badges, SHUF, (i+) | third party: PS3 firmware (PLAUSIBLE) | the art matches the PS3 control-panel style, and the **ATRAC** badge (Sony's codec) and the (i+) glyph are PS3-specific; the code calls them "the web app's images/audioplayer set"; no metadata | none | REPLACE |
| `ps3xmb/videoplayer/icon_000..024.png`, `icon_114.png` | video-player control panel | as above (PLAUSIBLE) | same style and set as the audio player (scene search, (i+), angle) | none | REPLACE |
| `ps3xmb/photoviewer/icon_000..027.png` | photo-viewer control panel | as above (PLAUSIBLE) | same style and set | none | REPLACE |
| `ps3xmb/icons_ui/*.png` (22) | generic settings icons, 256x256 white | AOSP framework and SystemUI vectors, rasterised by upstream | PLAUSIBLE: `Android.bp` says "Framework/SystemUI vector icons rasterised"; the art is Material-style | Apache-2.0 | KEEP |
| `ps3xmb/icons_retroarch/*.png` (849) | RetroArch "Monochrome" system icons | third party: libretro `retroarch-assets` | CONFIRMED by content and names (`Nintendo_-_...`, `-content` variants); plan 1.5 | CC BY 4.0 (needs credit; NOTICE has none yet) | KEEP |
| `ps3xmb/systemstatus/*.svg` (8) | battery, Wi-Fi, BT and cellular fallbacks | third party: ES-DE `resources/graphics/systemstatus` | per `Android.bp`; Inkscape SVGs | ES-DE's licence (MIT, PLAUSIBLE; not verified offline) | KEEP |
| `ps3xmb/globe/earth_day.png`, `earth_night.png`, `earth_clouds.png` | 1024x512 equirectangular Earth maps (Time Zone globe, music globe) | third party: NASA (Blue Marble 2002, Earth's City Lights) | PLAUSIBLE: the content matches NASA's flat-ocean Blue Marble and the city-lights map; `earth_clouds` EXIF says "Adobe Photoshop 7.0, 2002:09:04", the Blue Marble release period | public domain (NASA; credit requested) | KEEP |
| `ps3xmb/audio/SE02_Cursor.wav`, `SE03_Normal_OK.wav`, `SE04_Back.wav`, `SE05_Category_OK.wav`, `SE08_Option.wav`, `SE09_Error.wav` | XMB navigation sounds | third party: PS3 (by name) | names as in plan G; ffmpeg-converted (`ISFT=Lavf58.76.100`). SE03/05/08 are identical; SE09 is 112 zero samples | none | REPLACE |
| `ps3xmb/audio/preview.wav` | GammaEQ preview loop, 134.9 s | third party: "Retro Arcade Game Music" by original_soundtrack (Viacheslav Starostin), Pixabay | CONFIRMED: md5 identical to `packages/apps/GammaEQ/res/raw/preview_fast.wav`, whose `credits.txt` names the track and its Pixabay URL | Pixabay Content License (use in products allowed, no standalone redistribution) | KEEP |
| `ps3xmb/fonts`, `icons`, ... (dev overrides) | `/data/system/nano_xmb/...` is checked first | users | runtime only | n/a | n/a |

Not installed (in the tree only):

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `ps3xmb/boot/{logo,footer,blur}_{gold,sd_gold}.png` | PS3 cold-boot plates | Sony | CONFIRMED by content: the PlayStation symbol, "PS3", "PlayStation 3" | none | REPLACE (done: not installed since the Thimsy patch) |
| `ps3xmb/boot/{logo,footer,blur}_white.png` | boot plates with the GammaOS cube logo and the "GammaOS" wordmark | upstream GammaOS | CONFIRMED by content | Apache-2.0 repo; GammaOS mark | not installed |
| `ps3xmb/audio/coldboot_stereo.wav` | PS3 cold-boot sound (10.0 s, 48 kHz stereo) | Sony (plan G) | by name | none | REPLACE (done: not installed) |
| `ps3xmb/audio/boot_chime.wav`, `menu_enter.wav`, `touch_continue.wav` | old boot-intro sounds | upstream; PLAUSIBLE DSi rips ("touch to continue") | `Android.bp` says "upstream boot-intro sounds Nano no longer plays" | none found | not installed |

## 2. DSi System Menu theme

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `ps3xmb/nds/dsvec.ttf` | DSi theme letters ("DSVec") | third party: "Mirsany" by Pikgura (2023) | CONFIRMED: the name table gives copyright "Pikgura", family "Mirsany", unique ID "Version 1.000;Pikgura;Mirsany-Regular;2023;FL830", vendor Pikg, fsType 4 (restricted embedding), no licence fields. Pikgura is a commercial foundry. The code comment says "DSVec = letters/Mirsany" | none | REPLACE |
| `ps3xmb/nds/dsvecnum.ttf` | DSi theme digits ("DSVecNum") | third party: Rounded M+ 1c Medium | CONFIRMED: the name table says "Copyright 2016 The Rounded M+ Project Authors", with an OFL 1.1 licence and URL | SIL OFL 1.1 (ship the licence) | KEEP |
| `ps3xmb/nds/nds_frame.png`, `nds_tile.png` | the selection frame (64x80) and app tile (64x64), drawn at 8x | third party: Nintendo DSi launcher | CONFIRMED: the code calls them "the REAL cell_00 selection-frame sprite", "tile_white", "Firmware geometry: 64x80". The PNGs are exact 8x nearest-neighbour upscales (11 and 6 colours) | none | REPLACE |
| `ps3xmb/nds/nds_ring_00..35.png` | 36-frame launch sparkle ring, 128x144, 4-bit palette | third party: DSi launcher | CONFIRMED: `NanoMenuRender.cpp:986` says "The 36 sparkle-ring frames (launcher_d cell_53..88)", firmware cell names; 4-bpp palette like DS graphics; re-saved 2026-07-02 (PNG `date:create`) | none | REPLACE |
| `ps3xmb/nds/nds_photo.png`, `dialog_box_blue.png`, `nds_batt.png` | top-screen photo panel (222x170), dialog body (224x156), battery sprite (17x11) | third party: DSi launcher | CONFIRMED by the code's names: "photo_U top-screen panel", "the real msk_dialog_BG sprite", "spr_batt_full sprite". Simple shapes | none | REPLACE |
| `ps3xmb/nds/nds_sb_{bt,note,speaker,speaker_mute,wifi}.png` | status-bar glyphs, 96x96 | AOSP SystemUI vectors, rasterised | PLAUSIBLE per the code ("framework SystemUI vectors"). These files ship through the `nds_*` glob, but the code uses embedded copies (`NanoMenuSbIcons.h`) | Apache-2.0 | KEEP (unused files; could be dropped) |
| `ps3xmb/audio/menu_ambiance.wav` | DSi home background music, a 30.37 s loop (48 kHz stereo) | third party: Nintendo DSi menu music (PLAUSIBLE) | used as "the DSi home BGM"; it belongs to a theme whose sprites are confirmed firmware rips. Audio not compared; no metadata | none | REPLACE |
| `ps3xmb/audio/nav_blip.wav`, `app_launch.wav`, `settings_nav.wav`, `settings_back.wav`, `settings_enter.wav` | DSi navigation sounds | third party: DSi menu (PLAUSIBLE) | as above ("The web app (audio.js) plays a distinct sound for each menu event"); no metadata | none | REPLACE |
| `ps3xmb/nds/hs_triangle.png` | Health & Safety warning triangle | DSi (PLAUSIBLE) | not installed ("Not the boot notice's hs_triangle.png") | none | not installed |

## 3. Minima theme

Minima imitates NextUI's look. It has no image or font files of its own: its text uses Nano's
primary face (Rodin today, so it changes when Rodin does).

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `ps3xmb/audio/minima_{cursor,ok,back,drill,error}.wav` | Minima navigation sounds (mono, 48 kHz, 0.055 to 0.29 s) | upstream GammaOS | PLAUSIBLE original: the code says "The clips are composed for Minima (NextUI itself is silent)". All five peak at exactly 18021 and have almost no energy above 4 kHz (-64 to -95 dB), which looks like scripted synthesis | Apache-2.0 (upstream repo) | KEEP |
| `ps3xmb/audio/minima_boot.wav` | old Minima boot sound | upstream | not installed | Apache-2.0 | not installed |

## 4. ES-DE themes and assets

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `nano/nano_esde_themes/slate-es-de.zip`: `core/fonts/Exo2-*Condensed.otf` | Slate's fonts | third party: Exo 2 (Natanael Gama) | CONFIRMED: the name table gives the OFL URL | SIL OFL 1.1 | KEEP |
| `slate-es-de.zip`: theme XML, `core/images`, `core/sounds/*.wav`, per-system `consolegame.svg`/`controller.svg` | ES-DE's default theme Slate (upstream bundles it) | third party: ES-DE, based on recalbox-multi, Carbon (Rookervik), Bezza191, Dan Patrick | the zip's `LICENSE` and `CREDITS.md` | CC BY-NC-SA 2.0 | KEEP |
| `slate-es-de.zip`: per-system `*/images/logo.svg` | console and brand logos | the trademark owners | the zip's `LICENSE` says "The used logos and trademarks are copyright of their respective owners" | none (excluded from the theme licence) | ASK (policy: nominative trademark use, which every ES-DE build ships) |
| `nano/nano_esde_assets/{help,controllers,badges}/*.svg` | ES-DE help glyphs (including PS/Xbox/Switch face buttons), controller and badge icons | third party: ES-DE `resources/graphics` | per `Android.bp` ("Sourced from ES-DE resources/graphics"); Inkscape SVGs | ES-DE's licence (MIT, PLAUSIBLE) | KEEP |
| `colorful/_inc/fonts/Gilroy-{Bold,Light,Regular,SemiBold}.ttf` | the theme's main font | third party: Radomir Tinkov | CONFIRMED: the name table says "Copyright (c) 2016 by Radomir Tinkov. All rights reserved.", vendor UKWN | none (commercial) | REPLACE |
| `colorful/_inc/fonts/Akrobat-Bold.ttf` | secondary font | Fontfabric | the same notice as Nano's Akrobat | as Akrobat above | KEEP (verify) |
| `colorful/_inc/sounds/*.wav` (7) | theme navigation sounds, 44.1 kHz stereo | the theme author | `ISFT=FL Studio 20` / `Fission`; no third-party credit | CC BY-NC-SA 2.0 | KEEP |
| `colorful/_inc/images/*` (SVG icons, `game-art-default.webp`, `space.png`) | theme UI art | the theme author | README | CC BY-NC-SA 2.0 | KEEP |
| `colorful/_inc/systems/images/*.webp` (about 195) | per-system art: composites of console product photos and screens | viking & faeran (Colorful for LaunchBox), g-spawn (ckau-book) | README credits. The content uses manufacturers' product photography and marks: e.g. `ps2.webp` shows a Sony WEGA TV, a PS2 and the "Sony Computer Entertainment" boot screen | CC BY-NC-SA 2.0 from the theme authors; the photos and marks underneath are not theirs to license | ASK (the same policy question as the Slate logos) |
| `colorful/_inc/systems/images/quake.webp` | Quake system art: a Quake Champions character render | third party: id Software / Bethesda; "noorkabir (imgbin.com)" per the README | CONFIRMED: the README credit links an imgbin "quake-champions" PNG, and the content is an official 3D render | none | REPLACE |
| `colorful/_inc/systems/images/{doom,openbor,mugen,easyrpg,lowresnx}.webp`, `auto-{allgames,favorites,lastplayed}.webp`, `custom-collections.webp` | art credited to other artists | DeviantArt artists azakachi-rd-17 (a Cacodemon, id's character), DOA687, mpadillathespriter; SeraphCircle; Duimon; Rick Patrick's folder art (softicons) plus Font Awesome | the README credits name them; no licence is stated | none found | ASK (the artists; or drop or redraw these 9 files) |
| `MAGISK_ES-DE_fix.zip` (`mods/37-esde.sh`) | GammaOS's Magisk fix | upstream GammaOS | NOTICE; only on the 1.2 base with `ESDE_APK` | Apache-2.0 | KEEP (not in 1.0a) |

## 5. Boot

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `nano/bootscreen/thimsy.svg` → `/system/etc/nano_xmb/boot/logo.svg`; also drawn into the bootloader `logo.img` (`scripts/bootlogo.py`) | the Thimsy logo: a boy with a thimble hat and a raised sword | this project | CONFIRMED by history: added in `0001-thimsy.patch` (thimsy commit `1ac8fb5`). It is one auto-traced path (0.1 px coordinates), so it was traced from a raster drawing whose source is not recorded | none recorded | ASK (owner: confirm you made or own the source drawing, and record its licence) |
| `external/roboto-fonts/Roboto-Regular.ttf` (AOSP) | text of the boot screen and boot logo | AOSP | `scripts/build.sh` `BOOT_LOGO_FONT`; `NanoBootArt` | Apache-2.0 | KEEP |
| `/system/etc/nano_xmb/audio/esde_boot.mp3` (`mods/57-boot-sound.sh`) | ES-DE home startup sound (Nano plays the first 2 s); MPEG-1 L3, 44.1 kHz, joint stereo, 256 kbps, about 8 s, no tags | third party: "Computer Startup Sound Effect" by Dragon Studio, Pixabay | the mod's header and URL; sha256 pinned; fetched at build time, not in the repo | Pixabay Content License | KEEP |
| `vendor/lineage/bootanimation/bootanimation.tar` → `/system/product/media/bootanimation.zip` | boot animation: one 2880x2880 frame of the GammaOS cube "G" logo (shown when booting full Android; Nano mode replaces it) | upstream GammaOS (stock LineageOS ships a multi-frame animation) | CONFIRMED by content; PNG `date:modify` 2024-04-07 | Apache-2.0 repo, but it is GammaOS's mark | ASK (GammaOS; better, replace it with Thimsy art, since the project is renamed and NOTICE says it is not endorsed by GammaOS) |
| `device/phh/treble/bootanimation.zip` | 133-frame animation (2021) | phh / upstream | no makefile references it; not installed | n/a | not installed |
| bootloader charging pictures, fastboot logo (`logo.img`) | Anbernic's images | Anbernic vendor firmware | README "The charging pictures and the fastboot logo are Anbernic's" | none (plan item M) | ASK (covered by plan M) |

## 6. GammaOS base

`device/phh/treble/base.mk` copies only APKs, emulator and app data, `roms.tar.zst` (all covered by the
plan) and scripts from `gammaos/`. GammaOS's overlays (`device/gammaos/overlay-common`,
`device/phh/treble/overlay*`) have no drawables and no wallpaper, and `treble_app` is not installed.

| path | what it is | origin | evidence | licence | verdict |
|---|---|---|---|---|---|
| `packages/apps/GammaEQ/res/raw/preview_fast.wav` (inside GammaEQ.apk) | the same Pixabay track as `preview.wav` | upstream GammaOS; Pixabay | CONFIRMED: `res/raw/credits.txt` | Pixabay Content License | KEEP |
| `/system/etc/res/images/charger/battery_{fail,scale}.png` (`gammaos/utils/battery_*.png`) | charger images | AOSP healthd | CONFIRMED: md5 identical to `system/core/healthd/images/` | Apache-2.0 | KEEP |
| `/system/fonts/*` (208 files: Roboto, Noto CJK/Naskh/Thai/Hebrew/Emoji, ...), `/system/product/fonts/{Lato,Rubik}-*` | system fonts, also Nano's script fallbacks | AOSP / LineageOS | standard AOSP and Lineage modules | Apache-2.0 / OFL 1.1 | KEEP |
| `/system/product/media/audio/**` | ringtones, alarms, notifications, UI sounds | AOSP / LineageOS sound sets | standard modules | the projects' licences (Apache-2.0, PLAUSIBLE) | KEEP |

## Code-embedded data (outside this inventory, same question)

These are not separate files, but they carry the same provenance problem:

- `nano/NanoMenuPS3ParticleCloud.h`: "Firmware-extracted eye-space particle cloud (RPCS3 draw 36)".
- `nano/NanoMenuMusicGlobeScenes.h`: camera paths "decoded from the captured view-projection regs".
- `nano/NanoMenuPS3ClockGlyphs.h`: numeral outlines from `psp_clock.js NUM_GLYPHS`.

These ship inside the `gammaos-nano` binary. `nano/xmb_icons.h` (RetroArch icons, CC BY 4.0) and
`nano/NanoMenuSbIcons.h` (AOSP icons) are fine.
