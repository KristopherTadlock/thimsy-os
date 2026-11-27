# Plan: remove files we have no right to redistribute

Status: planned, not started (written 2026-11-10). Google apps are already removed in the
working tree (see 1.1); nothing else here has been changed yet.

## 1. The problem

The RG Cube image is published (private test drafts now, a public release later). Everything in
it must be ours, open source, or shipped with the owner's permission. An audit of the image and
the GammaOS Next base found files that are none of these. Most come from upstream GammaOS Next
(TheGammaSqueeze), not from this project, but we ship them all the same.

### 1.1 Google apps (done in the working tree)

Upstream's "Full" product (`lineage_arm64_bgN`) pulled in `vendor/gapps`: Play Store
(`Phonesky`), Play Services (`GmsCore`), Google Services Framework, Google SetupWizard,
GooglePartnerSetup, GoogleFeedback, the contacts/calendar sync adapters, PrebuiltExchange3Google,
AndroidAutoStub and `libjni_latinimegoogle`. Google does not license these for redistribution.

Fix made: `device/phh/treble/lineage_arm64_bgN.mk` no longer inherits
`vendor/gapps/arm64/arm64-vendor.mk`; README documents installing Google Play as a Magisk module.
Still to do: build, confirm the image (section 4), commit with the patch.

### 1.2 Almost certainly unlicensed

| # | Item | What it is | Where it ships | How it gets there |
|---|---|---|---|---|
| A | DraStic app `drastic_r2.6.0.4a.apk` | Exophase's closed, paid DS emulator | installed on first boot | `device/phh/treble/base.mk` (copies to `/system/etc`), `gammaos/apps.list` line `DraStic`, `gammaos/setup.sh` `post_drastic` |
| B | DraStic data `drastic.tar.zst` | DraStic's BIOS replacements, `game_database.xml`, `usrcheat.dat`, controller skins, demo saves | `/data/data/com.dsemu.drastic` | `base.mk`, `post_drastic` |
| C | `libdrastic_arm64.so`, `libdrastic_cpu.so` | DraStic's native core, lifted from the APK for drastic-nano | `/system/lib64` | `base.mk` lines ~169-178 (`gammaos/emulators/drastic-nano-lib`) |
| D | drastic-nano seed data | the same DraStic files (`system/drastic_bios_arm7.bin`, `drastic_bios_arm9.bin`, `nds_firmware_modified.bin`, `game_database.xml`, `usrcheat.dat`, `config`, `shaders`) | `/system/etc/drastic-nano` | `base.mk` (`find-copy-subdir-files ... gammaos/emulators/drastic-nano`) |
| E | PlayStation / PS3 logos | Sony's PlayStation symbol and "PS3" wordmark (checked: `logo_gold.png`) | `/system/etc/nano_xmb/boot` | `frameworks/base/cmds/gammaos-nano/ps3xmb/boot/{logo,footer,blur}_{gold,sd_gold,white}.png`; loaded in `NanoMenuPS3Boot.cpp` (`loadPs3BootPlate("logo_white.png")`, `"footer_white.png"`, around lines 813 and 1005) |
| F | "PS3 Rodin" fonts | FontWorks' commercial Rodin, the PS3 system font | `/system/etc/nano_xmb/fonts` | `ps3xmb/fonts/ps3-rodin-{regular,bold,light}.ttf`; `NanoMenuRender.cpp` `rodinPaths` (around line 4184) |
| G | PS3 system sounds (by name; audio not compared) | `coldboot_stereo.wav`, `SE02_Cursor.wav`, `SE03_Normal_OK.wav`, `SE04_Back.wav`, `SE05_Category_OK.wav`, `SE08_Option.wav`, `SE09_Error.wav` | `/system/etc/nano_xmb/audio` | `ps3xmb/audio`; `NanoMenuPS3Boot.cpp` (cold boot around line 728, the SE list around line 392) |
| H | Gilroy font | commercial typeface (Radomir Tinkov) inside the Colorful (Simplified) ES-DE theme: `_inc/fonts/Gilroy-{Bold,Light,Regular,SemiBold}.ttf` | the ES-DE themes folder | `mods/50-nano.sh` (`THEME="colorful-simplified-es-de"`); the theme itself is CC BY-NC-SA, the font inside it is not |

### 1.3 Unclear: needs the owner's terms or an audit

| # | Item | Question | Where |
|---|---|---|---|
| I | MiXplorer 6.64.3 | closed freeware from XDA (paid on Play); no redistribution grant found | `gammaos/apps.list` `MiXplorer`; `base.mk` copy of `MiXplorer_v6.64.3-API29_B23090720.apk` |
| J | Daijisho 1.5.0 | closed app, free on Play; its GitHub (TapiocaFox/Daijishou) licenses platform files and wallpapers (MIT), not the app | `apps.list` `Daijisho`, `setup.sh` `post_daijisho` (also sets it as home in full-Android mode), `mods/35-daijisho.sh` |
| K | The rest of `ps3xmb/` | XMB icons (`icons`, `icons_ui`, `audioplayer`, `videoplayer`, `photoviewer`, `systemstatus`), `wave/*.bin`, `textures`, `gradients`, `normalmaps`, `canyon`, `backgrounds/cinfo-bg-photogallery.jpg`, `globe/earth_*.png` (likely NASA Blue Marble, public domain), the other sounds (`app_launch`, `boot_chime`, `menu_enter`, `nav_blip`, `preview`, `settings_*`, `touch_continue`, `minima_*`) | `frameworks/base/cmds/gammaos-nano/ps3xmb/**`, `Android.bp` `prebuilt_etc` modules `nano_xmb_*` |
| L | DSi System Menu theme | `menu_ambiance.wav` is used as the DSi home BGM; `ps3xmb/nds/*` holds the "DSVec" fonts (`dsvec.ttf`, `dsvecnum.ttf`) and sprites. Recreation or rip? | `ps3xmb/audio/menu_ambiance.wav`, `ps3xmb/nds`, `NanoMenuPS3Boot.cpp` (`menu_ambiance` around lines 514-575) |
| M | Anbernic vendor, kernel, bootloader; Unisoc blobs | from Anbernic's firmware, no explicit licence; normal custom-ROM practice | the PAC (`scripts/build.sh`, `work/`) |
| N | Nano's GPL parts | `NanoAc3.h` wraps the vendored liba52 ("a52dec 0.7.4, GPL-2", in its header and the `Android.bp` comment) to decode AC-3 audio; `NanoOskKanji.h`, the on-screen keyboard's kanji table, is "derived from SKK-JISYO, which is GPL-licensed" (its header). Nano is Apache-2.0 (GammaOS Next's licence, `LICENSE`). What does shipping both in Nano require of the Android build, and may the Kanji table stay? It also matters for `linux_handheld_plan.md`, which keeps Nano Apache-2.0 (§3.3, decision 6) and can drop AC-3 there (risk 11) | `frameworks/base/cmds/gammaos-nano/NanoAc3.h`, the `liba52_nano` library in its `Android.bp`, `NanoOskKanji.h` |

### 1.4 Allowed, with conditions (document, no removal)

- M64Plus FZ 3.0.335 "free" (GPLv3) bundles closed ad SDKs, so complete corresponding source
  can't be offered for that build. Option: replace it with a FOSS build of mupen64plus-ae.
- RetroArch cores under non-commercial licences (`mame2003_plus`, `fbneo`, Genesis Plus GX and
  any others in `mods/retroarch-cores.txt`): fine for a free image, never for a sold one.

### 1.5 Checked and fine

RetroArch, PPSSPP (including its `flash0` fonts), Flycast, Aurora Store, Brave, Magisk, Dolphin,
ARMSX2, Azahar, WatermelonDS, Vita3K, hakuX (GPL-3.0, an xemu fork), ScummVM, Shattered Pixel
Dungeon, malisx2 (MIT, Mesa-based), the bundled ROMs and their art (`mods/rom-licenses`), the
Colorful (Simplified) theme apart from Gilroy, Akrobat and Font Awesome, the RetroArch XMB icons
Nano copies (CC BY). DraStic's `nds_firmware_modified.bin` is a generated settings stub (149
non-empty bytes, `MACP` header), not Nintendo code, but it goes with DraStic (D) anyway.

## 2. High-level solution

1. **Nothing closed or proprietary is built into the image** unless its owner allows it.
   Anything users need from a closed app, they install themselves from its official source.
2. **DraStic becomes optional.** drastic-nano already loads `libdrastic` from an installed
   DraStic app when there is no system copy (`resolveDrasticLibsDir`). Ship no DraStic files;
   seed drastic-nano from the installed app; fall back to WatermelonDS (GPL) for DS games when
   DraStic is not installed.
3. **Nano's PS3 theme gets original or openly licensed art, fonts and sounds:** a Thimsy
   boot plate instead of the PlayStation logos, an OFL font instead of Rodin, and original or
   CC0 sounds instead of the PS3 sounds. Same layout and behaviour, different assets.
4. **Gilroy is replaced inside the theme copy we ship** with an OFL font of similar shape, or
   the theme is swapped for one whose fonts are all open.
5. **MiXplorer and Daijisho:** ask their authors for written permission; failing that, replace
   MiXplorer with Material Files (GPLv3) and drop Daijisho.
6. **Every asset gets a recorded source and licence** (a new `ASSETS.md` next to `NOTICE`), and
   the build checks the finished image for a list of banned file names, so nothing slips back.

## 3. Implementation guide

Work in `~/gammaos-src` (source, the Nano patch) and `~/thimsy` (mods, notes,
the patch file). After each step: build the touched modules, regenerate the patch
(`git diff -- $(cat /tmp/paths.txt)`), run the credential check, commit.

### Step 0: inventory first (before deleting anything)

1. Mount or unpack the current system image (`scripts/build.sh` output, or
   `out/target/product/tdgsi_arm64_ab/system`) and list every file that is not built from
   source: `find system -name '*.apk' -o -name '*.so' -o -name '*.ttf' -o -name '*.otf' -o -name '*.wav' -o -name '*.png' -o -name '*.tar.*'`.
2. For each file under `frameworks/base/cmds/gammaos-nano/ps3xmb/`, record origin and licence
   in `ASSETS.md` (columns: path, what, origin, licence, keep/replace). Items K and L get decided
   here. `git log` on the GammaOS Next repos, file metadata and visual/audio comparison help.
3. Write `scripts/banned_files.txt` (one glob per line: `*drastic_r2*.apk`, `libdrastic*.so`,
   `drastic.tar.zst`, `ps3-rodin-*.ttf`, `logo_gold.png`, ..., `Gilroy-*.ttf`, `Phonesky*`,
   `GmsCore*`) and a check in `scripts/build.sh` that fails the build when the finished image
   contains a match (debugfs `ls -R`, as `check_image_files.py` already reads the image).

### Step 1: finish Google apps (1.1)

1. Build (`make-release.sh` style: `buildbot_unified.sh treble 64GN nosync`).
2. Confirm `out/target/product/tdgsi_arm64_ab/system/product/priv-app` has no `GmsCore` or
   `Phonesky`, and `system/system_ext/priv-app` no `GoogleServicesFramework` or `SetupWizard`
   (`LineageSetupWizard` must remain: `gammaos/setup.sh` logs to its data folder).
3. First boot on the device: setup completes, Aurora Store opens and installs an app anonymously.
4. Add `device/phh/treble/lineage_arm64_bgN.mk` to the patch path list (already in
   `/tmp/paths.txt` this session) and commit.

### Step 2: DraStic (A-D)

1. **Stop shipping it.**
   - `device/phh/treble/base.mk`: remove the copies of `drastic.tar.zst`,
     `drastic_r2.6.0.4a.apk`, `gammaos/emulators/drastic-nano` (seed data) and
     `drastic-nano-lib/*.so`.
   - `gammaos/apps.list`: remove the `DraStic` line. `gammaos/setup.sh`: remove `post_drastic`
     (its permission grants move to step 2.4).
   - Delete `gammaos/emulators/drastic*` from the tree (or leave them out of the patch).
2. **drastic-nano without system files** (`frameworks/base/cmds/drastic-nano`):
   - `DrasticAssets.cpp` `systemLibDir()`: no `/system/lib64` copy, so `main.cpp` already falls
     back to `resolveDrasticLibsDir(findDrasticApkDir())` (unzips `lib/arm64-v8a/*.so` from
     the installed APK). Make that the normal path; cache the extracted libs under
     `/data/system/drastic-nano/lib`, keyed by the APK's version, so it isn't unzipped each launch.
   - `systemDir()` / `seedRoot()`: seed BIOS, `game_database.xml`, `usrcheat.dat`, config and
     shaders from the installed app's data (`kLegacyRoot`,
     `/data/user/0/com.dsemu.drastic/files/DraStic`, which DraStic fills on its first start)
     or from the APK's `assets/`. Where DraStic hasn't run yet, start it once (or tell the user
     to) before the first drastic-nano launch.
   - Our own files stay: GammaOS shaders and `drastic-nano` user data in
     `/storage/emulated/0/drastic-nano`. Check what in `gammaos/emulators/drastic-nano/` is ours
     (shaders, config presets) and keep only that, in a new folder, with its licence in
     `ASSETS.md`.
   - Update the stale header comment at the top of `drastic-nano/main.cpp`.
3. **Nano's DS routing** (`frameworks/base/cmds/gammaos-nano`):
   - `NanoMenuXmb.cpp` `kXmbSystemDefs` NDS row launches `com.dsemu.drastic`; drastic-nano is
     the default via `persist.gammaos.nano.drastic_nano=1` (`base.mk` around line 255). When
     `packageInstalled("com.dsemu.drastic")` is false, launch DS games with WatermelonDS
     (`me.magnum.melondualds`, shipped in `mods/added-apps.txt`) and hide drastic-nano's
     options. Places to check: `NanoMenu.cpp` drastic launch paths (QR resume, the
     `sys.gammaos.drastic_nano.start` hand-off), `NanoMenuOverlay.cpp` (`launchPkg ==
     "com.dsemu.drastic"`), `NanoMenuDrasticPark.cpp`, the emulator catalog, the setup wizard.
   - A Settings or Game Settings row "DS emulator: drastic-nano (needs DraStic from Google Play)
     / WatermelonDS" with a short explanation, so users can opt in.
   - `NanoSdMove.cpp`: DraStic moves already skip when the app isn't installed; nothing to do.
4. **Permissions when the user installs DraStic:** `post_drastic` granted `RECORD_AUDIO` and
   `BLUETOOTH_CONNECT`. Grant them from Nano when it first sees the package (a
   `PACKAGE_ADDED` check in the existing package scan), or leave them to the app's own prompts.
5. **Docs:** README sections on DraStic and drastic-nano, `NOTICE`, test notes ("install DraStic
   from Google Play to use drastic-nano").
6. **Test:** a fresh flash without DraStic: DS games open in WatermelonDS. Install DraStic, start
   it once: drastic-nano becomes available and runs a game, saves land in
   `/storage/emulated/0/drastic-nano`. Uninstall DraStic: falls back cleanly.

### Step 3: Nano's PS3 theme assets (E-G, then K and L from the step 0 audit)

1. **Boot plates (E).** Make new `logo_*`, `footer_*`, `blur_*` PNGs with the same sizes and
   alpha layout (Thimsy / "Nano" wordmark, original design; or render text at runtime and
   delete the PNGs). Keep the file names so `loadPs3BootPlate` needs no change, or rename and
   update `NanoMenuPS3Boot.cpp` (around lines 813 and 1005). Check the other boot code paths
   for any other Sony artwork.
2. **Fonts (F).** Replace `ps3-rodin-*.ttf` with an OFL font of similar metrics (Noto Sans or
   Source Sans 3; Akrobat is already shipped, so it costs nothing). Rename the files (`xmb-*.ttf`)
   and update `NanoMenuRender.cpp` `rodinPaths` plus any other `rodin` reference (grep the
   source). Check text widths in the XMB, the option menu and the dialogs: Rodin's metrics are
   narrow, so long labels may need the existing truncation to kick in.
3. **Sounds (G, then the rest of K).** Produce replacements: original synthesis (a short
   script with `sox` or Python writing WAVs, kept in the repo so they can be rebuilt) or CC0
   sounds with recorded sources. Same file names, same format (PCM WAV, the sample rate and
   channels the current files use, `nanoDirectPlayOneShot` expects raw WAV) and similar lengths
   (`coldboot_stereo.wav` is about 10 s and the fade-in is timed to it).
4. **DSi theme (L).** If `menu_ambiance.wav` or `ps3xmb/nds/*` turn out to be Nintendo's,
   replace them the same way (original BGM loop; open vector font or original glyphs).
5. **Runtime overrides:** Nano checks `/data/system/nano_xmb/...` before `/system/etc/nano_xmb`.
   Users who want the original look can place their own files there; document that in the
   README without shipping or linking the originals.
6. **Android.bp:** update the `prebuilt_etc` `srcs` lists for renamed files.
7. **Test:** cold boot (logo, footer, chime timing), XMB navigation sounds, option menu, the DSi
   theme home and BGM, Minima theme sounds; text fits in the XMB with the new font.

### Step 4: Gilroy (H)

1. In `mods/50-nano.sh`, after the Colorful (Simplified) theme is unpacked, replace
   `_inc/fonts/Gilroy-*.ttf` with an OFL font (Outfit, Urbanist or Plus Jakarta Sans are close
   geometric sans faces) and update the theme's XML `fontPath` references (grep the theme for
   `Gilroy`). Keep the theme's licence file and note the change in its README and in `NOTICE`
   (CC BY-NC-SA requires marking changes).
2. Or switch the default theme to one with only open fonts (Nano also ships
   `nano_esde_themes/slate-es-de.zip`, whose fonts are Exo 2, OFL).
3. Remove the Gilroy hinting note from the README (its "small text in the theme's own fonts"
   bullet).
4. **Test:** the ES-DE home with the theme: the game list renders, no missing-font fallback.

### Step 5: MiXplorer and Daijisho (I, J)

1. Ask: MiXplorer's author (XDA thread) and TapiocaFox (Daijisho) for permission to include
   their app in a free firmware image. Keep the replies in the repo (`docs/permissions/`).
2. Without permission:
   - MiXplorer: replace with Material Files (`me.zhanghai.android.files`, GPLv3, F-Droid). Change
     `gammaos/apps.list`, `base.mk`, and `mods/15-update-apps.sh` (version/URL/sha256 like the
     other added apps), README app table.
   - Daijisho: drop it from `apps.list`, `post_daijisho` (which also sets it as home in full
     Android mode; Nano or Lineage's launcher takes that role), `mods/35-daijisho.sh` and the
     Daijisho wallpapers/platform steps; keep the MIT platform JSON Nano uses
     (`ps3xmb/daijishou/*.json`), already credited in `NOTICE`.

### Step 6: conditions and records (1.4, M, N)

1. `NOTICE`: list GPL apps with their source links (already in `mods/added-apps.txt`, add the
   ones from `apps.list`: RetroArch, PPSSPP, Flycast, M64Plus FZ, Aurora Store), state that
   some RetroArch cores are non-commercial and the image must not be sold.
2. M64Plus FZ: decide whether to keep the ad-supported build or ship a FOSS mupen64plus-ae
   build (would need the N64 launch intent and the `NanoSdMove.cpp` M64Plus paths checked
   against the new package name).
3. Vendor blobs (M): note their origin in `NOTICE`; nothing to change unless Anbernic objects.
4. Nano's GPL parts (N): answer the question in 1.3 for liba52 and for the SKK-JISYO table,
   then record the result in `NOTICE` or replace the part. The Linux build's choice follows
   (`linux_handheld_plan.md` §3.3).

## 4. Verification for the whole plan

1. The banned-file check (step 0.3) passes on the finished image.
2. `ASSETS.md` has a row for every non-source file in the image.
3. A fresh flash completes setup with no Google apps, no DraStic, no MiXplorer/Daijisho (if
   replaced); Nano's themes look and sound complete; DS, PSP, Dreamcast and N64 games start.
4. The release notes and README describe what users can add themselves (Google Play via
   Magisk, DraStic from Google Play) without linking to unlicensed copies.

## 5. Order and size

1. Step 1 (Google apps): small, already coded.
2. Step 0 (inventory + banned-file check): small, makes everything after it safe.
3. Step 2 (DraStic): medium; drastic-nano and Nano routing changes, needs device testing.
4. Step 3 (PS3 assets): medium; mostly asset work, plus font fitting.
5. Step 4 (Gilroy): small.
6. Step 5 (MiXplorer, Daijisho): small, but waits on replies.
7. Step 6 (records): small.
