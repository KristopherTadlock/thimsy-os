# User emulators plan: an emulator the user installs works like a pack-in

Status: draft for review (2026-11-23). Not started. Builds on usb_storage_plan.md (drives), controller_layout_plan.md (phase 5, pad provisioning and Merge Controllers) and button_map_overlay_plan.md (readers). Worked example throughout: DuckStation for Android (PS1), which Thimsy may not ship. Revised 2026-11-27 for `docs/linux/DECISIONS.md` OA-54: the definition also drives per-device settings, shared storage, launch entries per platform and card or local storage, on Android and Linux, and the shipped emulators use the same files (§1a). Still a plan: nothing here is implemented.

## Requirement (user, 2026-11-23)

> I would like the users of thimsy to be able to install emulators that are not part of the official package, make that emulator an option for launching games, and create all of the features supported for packaged emulators - universal hotkey support, controller mapping, and exporting files to external storage. Research how this could be supported. Write a planning document in markdown and add it to the to-do list. here is one example - the duckstation standalone emulator.

Read as six goals, with a seventh added by OA-54:
1. A user installs an emulator APK that Thimsy doesn't ship (Play, an app store, or a sideloaded APK).
2. Thimsy offers it as a launch option per system, and per game, in Nano and in ES-DE.
3. Universal hotkeys work for it.
4. Controller mapping works for it: positional default binds, player assignment (Merge Controllers) and the button map overlay.
5. Its files can go to external storage like a pack-in's (Move Emulator Files, microSD, later USB), and the BIOS installer puts BIOS files where it looks.
6. All of this is driven by a data file (an **emulator definition**), so known-but-not-shipped emulators such as DuckStation get it without a rebuild, and a completely unknown emulator still gets a defined minimum.
7. (OA-54, owner, 2026-11-27) The same file drives everything an integration needs, on both builds: the per-device settings (OA-46), the shared storage (OA-52, OA-53), the hotkeys, the controller map, the launch entries per platform and card or local storage. The shipped emulators are described by the same files, so a hand-written definition gets the same integration as a pack-in. per_device_config_plan.md holds the sync rules; this plan defines the fields (§1a).

## Conventions

- `P:<n>` is a line in `patches/thimsy/0001-thimsy.patch`. Code inside the patch is cited by its in-tree path and identifier as well.
- Several Nano files are modified GammaOS files, so the patch shows only hunks. Logic that lives only in the unpatched base is marked UNVERIFIED.
- "Pack-in" means an emulator the build installs (`/system/etc/gammaos/apps.list`, `mods/added-apps.txt`).
- A definition's **origin** is `system` (shipped in the image), `index` (from a Thimsy-signed index, phase 7) or `imported` (a file the user imported). Several rules below depend on it.

## Where things stand

### Two bases, two different stories

| | 1.4.4 base (`SYSTEM_IMG`, branch nano-144) | 1.2 base (Docker default, "Thimsy 1.0") |
|---|---|---|
| Home | GammaOS Nano | Trebuchet, or the ES-DE app when built with `ESDE_APK` (`README.md:208-209`) |
| Thimsy's patch | Applied to the source tree (`README.md:211-227`) | Not applied: the release's prebuilt system (`README.md:993-996`) |
| Universal hotkeys | Yes. `mods/55-hotkeys.sh:15-18` skips a non-Nano base, and `ThimsyHotkeyController` is patch code | No |
| Button map | Yes (Nano) | No |
| Move Emulator Files, BIOS installer | Yes (Nano, `NanoSdMove.cpp`, `NanoBios*.cpp`) | No |
| ES-DE | Nano's own ES-DE theme engine and store mode. The ES-DE app is refused (`mods/37-esde.sh:31-34`) | The ES-DE app, if `ESDE_APK` is set (`mods/37-esde.sh:27-30`) |

**Consequence:** on 1.2, the Docker default, this plan can only document what ES-DE already does by itself (it already lists DuckStation, below). Goals 3 to 6 need the hotkey controller, Nano and the patch, so they are 1.4.4-only as drafted. Porting them to 1.2 is a scope question for the user (D8). Everything below is about 1.4.4 unless it says 1.2.

### Launch: most of it already works for any installed app

**Systems and the catalog**
- `mods/nano-systems.txt` names each system's emulator as a catalog `player=` or a raw `package=` + `intent=` + `exts=` (`mods/nano-systems.txt:6-7`). Placeholders `{file.uri}` and `{file.path}` (`:11-12`). PS1 is `player=psx.ra64.mednafen_psx_hw` (`:30`).
- `scripts/nano_systems.py` turns each line into a system JSON with `"builtin": True` (`scripts/nano_systems.py:72-75`). The build fails when a package isn't in `apps.list` (`:98-99`). That gate is build-time only.
- `mods/50-nano.sh:222-227` installs the result as `/system/etc/gammaos/nano_systems.json`. Setup copies it to `/data/system/nano_systems.json` only on a fresh setup (`:233-239`).
- The emulator catalog is Daijisho's platform JSON, read-only at `/system/etc/nano_xmb/daijishou` (Android.bp `sub_dir: "nano_xmb/daijishou"`, P:2807). `NanoMenu::loadEmuCatalog()` is base code; only its log line is in the patch (P:40207). Nothing in the patch reads a user catalog directory (grep for `daijishou`). Whether the base body reads more than one directory is UNVERIFIED.
- **The catalog already has DuckStation.** `ps3xmb/daijishou/SonyPlayStation.json`: Thimsy renamed `"psx - Duckstation (Legacy)"` to uniqueId `psx.com.github.stenzek.duckstation.legacy`, with `-n com.github.stenzek.duckstation/.EmulationActivity -e bootPath {file.path} --ez resumeState 0 --activity-clear-task --activity-clear-top` (P:88931-88936). The rename exists because upstream Daijisho also has a non-legacy `psx.com.github.stenzek.duckstation` entry ([SonyPlayStation.json](https://raw.githubusercontent.com/TapiocaFox/Daijishou/main/platforms/SonyPlayStation.json)). That entry sits outside the hunk, so its presence in Nano's copy is UNVERIFIED (likely).

**Detection**
- `installedPackages()` parses `/data/system/packages.list` (`NanoMenuPS3EmuCatalog.cpp`, P:40241-40253). No PackageManager query, so Android 11+ package visibility doesn't apply. `packages.list` has no labels, no launcher activities, no system flag and no signer.
- `NanoMenu::emuEntryUsable`: a libretro player needs its core, any other player needs its package installed (P:40263-40267). **A sideloaded DuckStation makes the catalog's DuckStation rows usable at once.**

**Choosing it**
- Game Systems > Emulator: `buildEmulatorPicker` lists the platform's runnable players, then "Search All Emulators..." and "Custom (type core / package)..." (P:40372-40379).
- Custom pops the picker and opens the on-screen keyboard on the package or core field (`PS3_GS_EMU_CUSTOM`, P:43777-43783). There is no list of installed apps and no template help.
- Per game: Game Options > Emulator, `openGameEmulatorPicker` (P:42763-42796): "System Default" plus the catalog's players for the platform. **No Custom row**, so a non-catalog app can't be chosen per game.
- Per-game choices are stored in `/data/system/nano_game_emulators.json` (`kGameEmusPath`, P:40600), or as gamelist `<altemulator>` in ES-DE store mode (`NanoMenu::loadGameEmulators` P:40610, `esdeStoreLoadGameEmus` branch P:40611). `applyGameEmulator` clears the system's Launch Args (P:40699-40706).
- `nanosys::playerRepair` re-points only a catalog-made, **non-builtin** system whose player can't run (`NanoSystemsHeal.h`, P:68073-68080). A seeded system the user pointed at DuckStation stays broken after DuckStation is uninstalled.

**Building the intent** (`NanoLaunch.cpp`, new file at P:23973)
- `{file.uri}` is `safContentUri()`: `content://com.android.externalstorage.documents/tree/<vol>%3A<dir>/document/...` (P:24061-24129). Same form as ES-DE's `%ROMSAF%`.
- `{file.path}` is the app-visible path (`appVisiblePath`, P:24059). `buildLaunchIntent` tokenizes the template plus Launch Args, substitutes placeholders and rejects tab or newline tokens (P:24247-24300). It does not check which `am` options a template uses.
- Home launch: tokens are written to `/data/system/nano_launch_intent.txt`, then `sys.gammaos.nano.launch_app` and `launch_intent=file` are set (`NanoMenuXmb.cpp` `launchXmbGame`, P:60030-60034). The framework side that reads them is base code (UNVERIFIED how it handles `-e`/`--ez` and grant flags).
- Over a game (the overlay): Nano, running as root, runs `am start <tokens> --grant-read-uri-permission` (`NanoMenuOverlay.cpp` `overlayLaunchGame`, P:38120-38121). Root can start non-exported activities.
- Quick Resume: RetroArch cores and DraStic only (P:60041, P:60050-60060). A user emulator gets none.
- Exit: long-press Back stops any foreground standalone app (`PhoneWindowManager.nanoStopAppToHome`, P:166600-166660). Per-app keep-alive is `persist.gammaos.nano.background_pkgs` (P:166602).

**Why a content URI opens for any app**
- `ContentProvider` grants everything except `THIMSY_STRICT_AUTHORITIES` (P:162597-162636). `UriGrantsManagerService.checkHoldingPermissionsUnlocked` always allows non-strict URIs (P:168795-168809).
- `ROMs/...` document URIs are non-strict (`isThimsyStrictUri`, P:168808-168840). The storage denylist still applies (`isThimsyStorageDenied`, P:162915-162945).

**Android Games**
- `tools/android-games` lists every app flagged `CATEGORY_GAME`/`FLAG_IS_GAME` with a launcher, minus `/system/etc/gammaos/android-games-exclude.txt` (`Games.java:47`, `:68-78`). That file is built from `apps.list` only (`mods/50-nano.sh:214-220`). A user emulator flagged as a game would be listed as an Android game. Whether DuckStation sets the flag is UNVERIFIED.

### Launch from ES-DE

- **1.4.4 (Nano's ES-DE store):** `kEsdeCommands` (`NanoEsdeEmulators.inc`, generated from ES-DE at commit 50e4b600a by `tools/gen_esde_emulators.py`) already has `{"psx", "DuckStation (Standalone)", "... %EXTRABOOL_resumeState%=false %EXTRA_bootPath%=%ROMSAF%", "pkg:com.github.stenzek.duckstation"}` (P:18188). `esdeEmulatorLabel` / `esdeCatalogForLabel` map labels to catalog players by `pkg:` key (P:40711-40733). So an ES-DE label round-trips to a catalog DuckStation player when the app is installed. Emulators ES-DE has no label for are stored in `nano/` (`NanoEsdeStore.cpp` header, P:18622-18623). Nano never reads a user's ES-DE `custom_systems` commands or find rules.
- **1.2 (ES-DE app):** ES-DE's own bundled `es_systems.xml` lists "DuckStation (Standalone)" as an alternative emulator for psx, and its `es_find_rules.xml` maps `DUCKSTATION` to `com.github.stenzek.duckstation/.EmulationActivity` ([es_find_rules.xml @50e4b600a](https://gitlab.com/es-de/emulationstation-de/-/raw/50e4b600ae533d772bae3ff880d11a09b05dbe84/resources/systems/android/es_find_rules.xml)). The user picks it in ES-DE's Alternative Emulators.
  - `mods/esde-emulators.txt` sets build-time defaults. The build fails on a label ES-DE doesn't have or on a label that needs a RetroArch core that isn't shipped (`mods/esde-emulators.txt:3-5`; `scripts/esde_config.py` `check_emulators`, `:55-72`). A standalone app isn't checked against what is installed: its command has no core, so `core_of()` returns None and it passes (`:50-52`, `:61`), and 1.2 has no `apps.list` to check against. A line `psx DuckStation (Standalone)` would build and default PS1 to an app that may not be installed. Don't add one.
  - For an emulator ES-DE doesn't know, the user adds `ES-DE/custom_systems` files ([INSTALL.md](https://gitlab.com/es-de/emulationstation-de/-/raw/master/INSTALL.md)).

### Permissions and file access

- `NanoPermGrantBridge` pre-grants the storage and microphone runtime permissions an app declares to every non-system user app, gated by `persist.gammaos.perm.autogrant` (P:166305-166317). DuckStation gets these.
- All files access goes only to `PACK_IN_EMULATORS` (12 packages, P:166326-166339) plus the chunked prop `persist.gammaos.perm.mestorage` (`readMesAllowlist`, P:166352-166366), and only to apps whose manifest declares it (comment, P:166320-166322). The allowlist is by package name only. Nothing in the repo sets the prop today (grep).
- The prop falls under the `persist.gammaos.` prefix context `persist_gammaos_prop` (`system/sepolicy/private/property_contexts`, P:180619), which `system_app` may set (`system_app.te`, P:180631) as well as root. It has no provenance.
- Three readers share the "self-granted" set, each with its own copy: `NanoPermGrantBridge.readMesAllowlist`; StorageManagerService through `getSelfGrantedManageStoragePackages()` (comment, P:166320-166325); and `ThimsyStorageDenylist` in the MediaProvider process (also used by DownloadProvider), with its own `PACK_IN_EMULATORS` and `SELF_GRANTED_PACKAGES_PROP` with 5 chunks (P:178790-178815).
- **The exemption rule:** self-granted packages are never exempt from the denylist; apps holding All files access that are not in the set (granted by hand) are exempt (`README.md:625-635`). So any MES grant whose package MediaProvider doesn't see in the set makes the app exempt from the denylist.
- SAF folder grants on an app's behalf: `NanoNetBridge.uriGrant` via `sys.gammaos.nano.uri_grant` (P:166246-166262), restricted to `NanoUriGrant.APPS` = hakuX, Azahar, SeedlessDS, WatermelonDS, ScummVM (P:166407-166410). A user app can't get one.
- How `NanoPermGrantBridge` learns about a new install ("at boot and when it is installed", `README.md:707-709`) is base code: UNVERIFIED which hook it uses.

### Hotkeys: data-driven already, with gaps

- Chord path: `PhoneWindowManager.nanoOverApp()` arms the chord only when Nano launched the app (`sys.gammaos.nano.app_launched=1`, P:167023-167027; set in e.g. `NanoControlCenter.cpp` `ccLaunchBottomApp`, P:16557). An app opened from Android's own launcher gets no chords. `nanoHotkey()` maps the button to an action (P:167257).
- `ThimsyHotkeyController` (`frameworks/base/services/core/java/com/android/server/policy/ThimsyHotkeyController.java`):
  - reads `DIRS = {"/system/etc/gammaos/hotkeys", "/data/system/gammaos/hotkeys"}` (P:168346);
  - `onAction` looks up the foreground package by exact match, then the `"*"` fallback, then an implicit `exit` for quit (P:168404-168425). It learns the foreground package only when an action fires (P:168405);
  - `loadProfiles` is called from `onAction` (P:168411) and re-parses on a path:mtime:length stamp change (P:168492-168530). It reads each file whole with `Files.readAllBytes`, no size cap, no `schema` check, in system_server on the key path. Profiles are keyed by `id`; `/data` wins for the same id (P:168512-168521; documented as "User or override", `HOTKEYS.md:130`);
  - **Bug for this plan:** two profiles with different ids that list one package resolve by `HashMap` iteration order (`byId.values()`, P:168518-168528). Not deterministic. Nano's own loader is deterministic (map by id, then sorted by `name`, P:23391-23398);
  - `injectKey` sends `SOURCE_KEYBOARD` events on `KeyCharacterMap.VIRTUAL_KEYBOARD` (device -1) only (P:168579-168585). There is no gamepad-source driver;
  - the `intent` driver runs as system_server (P:168601-168628).
- Nano's provisioning (`NanoHotkeys.cpp`, P:22903): same two dirs (`kDirs`, P:22937). `provision()` runs at Nano start and each time Nano comes to the front (`HOTKEYS.md:158`), holds a flock (P:23403-23413), and skips running packages (P:23414-23423). It edits only files that already exist, unless a rule has a `template` (`HOTKEYS.md:162`). `provisionFile` handles `ini`, `retroarch_cfg`, `flycast_mapping`, `xml_prefs`, `melonds_mapper` (P:23512-23527). `openTarget` refuses links at every step (P:22970-22990) but accepts **any absolute path**.
- `writeLines` (P:23095) keeps the folder's owner and the mode but doesn't run `restorecon` or clear a SharedPreferences `.bak`. `appFileWrite` in `NanoBiosUtil.cpp` (P:8871-8929) does both.
- `fallback.json` gives every other app quit only (`mods/hotkeys/fallback.json:1-13`). DuckStation today: save, load, fast forward and rewind log "not available"; quit stops it.
- User profiles: `HOTKEYS.md:128-132` and `:421-429` say to copy a JSON into `/data/system/gammaos/hotkeys/`. Nothing creates that directory (only readers at P:22937 and P:168346), and only root can write `/data/system`. No UI imports one. Settings > Hotkeys > Emulator Support lists profiles read-only, by `name` (`buildGpHotkeySupport`, P:43084-43105).
- Shared keys: F2 save, F4 load, F6 fast forward, F7 rewind, F8 quit (`HOTKEYS.md:156`). The virtual keyboard reports F1-F12 in `hasKeys` (`HOTKEYS.md:115`); "virtual keyboard (device id -1)" is the term the emulator-adding guide uses (`HOTKEYS.md:423`).

### Controller mapping

- **Positional at the Android level, with gammapad off.** `mods/27-controller-layout.sh:19-26` installs the Cube's keylayout so the bottom button is `BUTTON_A` (`controller_layout_plan.md:89`, `:98`). An emulator whose own defaults follow the Android/Xbox convention (Cross on `BUTTON_A`) is positional on the Cube with no Thimsy work.
- **gammapad (Controller Enable) changes that.** While it runs it grabs every pad with `EVIOCGRAB` and apps see one virtual 045e:0b13 pad that uses `Generic.kl` (`controller_layout_plan.md:94`, `:100-104`). Since test 37 the built-in pad comes through by position, other pads by label, and ABXY Swap (off by default) swaps every pad (`TODO.md:104-105`; `controller_layout_plan.md:107`; `button_map_overlay_plan.md:585-606`). No player 2 is possible while it runs (`controller_layout_plan.md:104`).
- **Writing pad binds** into an emulator's config is not implemented: every shipped provision rule writes hotkeys only (`controller_layout_plan.md:204`). Per-emulator positional provisioning and Merge Controllers player assignment are controller_layout_plan.md phase 5 (`TODO.md:116`), with `mods/pads/*.json`, `ifUnset` plus an owner sidecar `/data/system/nano_pads_owned.json` (`controller_layout_plan.md:439-441`, `:575`), and per-emulator player writers (`controller_layout_plan.md:507-535`, `:589`).
- **Button map** (`NanoButtonMap.cpp`, `NanoButtonMapReaders.cpp`):
  - the emulator is chosen by hotkey profile `packages` (`emulatorForPackage`, P:11321);
  - its default console is a hard-coded table, `defaultSystemFor` (P:11309-11317), used by `resolveContext` (P:11348ff);
  - readers are one C++ function per emulator, dispatched in `readEmulator` (P:16184-16205). An unknown id gives `Source::kUnknown` and "No reader for <package>" (P:16201-16204);
  - an app with no profile still gets the console drawing when Nano launched it for a system the catalogue draws (P:11372-11374, P:11394-11400);
  - the hotkey rows come from the profile whose `id` equals `ctx.emulator` (P:28551-28553). With no profile, **Quit isn't shown** although the fallback serves it;
  - `sys.gammaos.nano.bmap_dev` carries the descriptor of the device that pressed the chord (`button_map_overlay_plan.md:141`).
- `tests/unit/test_data_lint.py:199-216` requires a profile for every reader id; `:218-247` requires a profile for every Nano system's app.

### Storage export and BIOS

All of it is per-emulator C++ or Java today:
- package constants, `enum kGroup*` and `kGroupPkg[]` (`NanoSdMove.cpp`, P:64857-64897);
- one `if (packageInstalled(...))` block per emulator in `emuMoveStart` and one `case` per group in `fileJobRelocate` (P:65869-66124, P:67016ff);
- the two-way settings sync table `emusync::items()` (`NanoEmuSync.cpp`, P:17365-17380; `struct Item` in `NanoEmuSync.h`, P:17573);
- the Dolphin and hakuX link lists, repeated in `emuRepointAbsent` (P:66074-66130, P:66703-66713);
- `sdGrantsFrom` kApps (P:66351-66354) and `NanoUriGrant.APPS` (P:166407);
- `PACK_IN_EMULATORS` twice (P:166326, P:178802);
- the BIOS installer's targets: one `SlotKind` per emulator (`NanoBiosCatalog.h`, `SLOT_RA` ... `SLOT_MAME4DROID`, P:6794-6810). PS1 BIOS rows install to RetroArch's system folder only (`NanoBiosCatalog.cpp`, P:5776-5782). Installed BIOS files follow Move Emulator Files (`README.md:686-704`; `BIOS.md`).

The mechanisms themselves are generic: settings rewrite (`sdPrefsSet`, `sdIniKey`), a SAF grant, symlink replacement (`addLinked`, `FileCopyPair::linkTo` P:21991, `linkFolder` P:21154), and two-way sync. The safety rails are generic by package: `sdSourceRoot` canonicalises and limits a source to the app's own `Android/data` or `/data/data` (`NanoAppFiles.cpp`, P:3450-3485), `sdTooBroad` (P:3426), `sdAppPathSafe`, `appFileWrite`. Symlinks work because "apps reach `Android/data` and their own folder directly here, not through FUSE" (`README.md:575-579`).

Two rules matter for definitions:
- **Card out:** today an emulator with files on the card needs the card to start (`README.md:586-587`). usb_storage_plan.md replaces this: a removed drive falls back to internal storage within 10 s, the emulators are re-pointed, and a banner shows (`usb_storage_plan.md:64-69`, `:81-86`). Its mirror option covers `NanoEmuSync` files only, not linked saves (`usb_storage_plan.md:72-79`).
- **Sync goes both ways and the card wins:** editing a file on the card changes the emulator (`README.md:588-599`). `emulator_config` is not owner-protected: the denylist's refused top-level folders are only `"Nano Backup"` (P:178785).

USB is never adopted today (`README.md:619`; usb_storage_plan.md).

### Summary: what a user emulator gets today on 1.4.4

| Feature | Pack-in | DuckStation, installed by the user | Hard-coded in |
|---|---|---|---|
| Listed in Game Systems > Emulator | Yes | Yes (catalog row) | — |
| Non-catalog app as system emulator | — | Only by typing package and intent (Custom) | P:43777 |
| Per-game emulator | Yes | Yes (catalog rows); a non-catalog app no | P:42763 |
| ES-DE label round-trip | Yes | Yes (ES-DE knows DuckStation) | `NanoEsdeEmulators.inc` |
| Repair after uninstall | Non-builtin systems only | Same | P:68073 |
| Quick Resume | RetroArch, DraStic | No | P:60041 |
| All files access | Yes | No | P:166326, P:178802 |
| SAF folder grant | 5 apps | No | P:166407, P:66351 |
| Hotkeys | Profile | Quit only | `mods/hotkeys/` |
| Button map | Reader | Drawing, no rows, no Quit row | P:16184, P:11309 |
| Pad binds, player assignment | Not yet for any | No | controller_layout_plan.md phase 5 |
| Move to microSD / sync | Yes | No | `NanoSdMove.cpp`, `NanoEmuSync.cpp` |
| BIOS installer | Yes | No | P:6794-6810 |
| Hidden from Android Games | Yes | No | `mods/50-nano.sh:214-220` |

## Design

### 1. The emulator definition

One JSON file per emulator. It **is the hotkey profile, grown**: the framework, Nano's provisioning and the button map already load `*.json` from `/system/etc/gammaos/hotkeys` and `/data/system/gammaos/hotkeys`, key by `id`, let `/data` override, and match `packages`. Today's readers ignore unknown top-level keys (they read `id`, `name`, `packages`, `actions`, `provision`, `note`; `name` is Nano-only, P:23391, P:43102). So new blocks need no new directory, and the button map's "which emulator" already works. **(decide)** D1: grow the profile in place (recommended), or a separate `emulators/` directory with a generator that writes the hotkey part.

Blocks (all optional except `id`, `name`, `packages`):

| Block | Purpose | Read by |
|---|---|---|
| `id`, `name`, `packages`, `note`, `actions`, `provision` | Today's hotkey profile (`HOTKEYS.md:126-187`) | PWM, Nano |
| `schema` | Integer, 1 for this plan. Unknown higher versions are refused, by both readers | Nano, PWM, registry |
| `signers` | SHA-256 of signing certificates the definition trusts | Registry (system_server) |
| `install` | `{url, store_id, note}`: where to get the APK. Thimsy never hosts it | Nano UI |
| `tested` | Versions known to work: `[{versionName, versionCode}]` | Nano UI |
| `systems` | ROM-folder ids it plays (`["psx"]`); extends `defaultSystemFor` | Button map, Nano |
| `players` | Daijisho-schema player entries per platform `uniqueId`, merged into `mEmuCatalog`; the Android list of `launch` (§1a) | Nano |
| `launch` | Launch entries per platform: `android` (the `players` list), `linux` (argv entries with the core and its save family) (§1a) | Nano (both builds) |
| `files` | Per-device settings, shared storage and left-out files, per platform, and how config reaches the emulator and comes back (§1a) | Nano (both builds) |
| `controls` | Button map reader, pad provisioning, player assignment (section 5) | Nano |
| `storage` | Card folder, move, link, sync and grant rules (section 6); its `link` and `sync` are shorthand for `files` entries (§1a) | Nano, registry |
| `bios` | Where the BIOS installer puts a system's files for this app (section 6) | Nano |
| `android_game` | `false` by default: listed packages are hidden from Android Games | Android Games helper |

Where definitions come from:
- **Shipped definitions** (origin `system`) in `/system/etc/gammaos/hotkeys/` from `mods/hotkeys/` (installed by `mods/55-hotkeys.sh:20-25`, no change). Thimsy ships a definition for every emulator it bundles (OA-54, §1a) and for known emulators it doesn't bundle: DuckStation first. A definition is not the app, so this doesn't redistribute anything.
- **User definitions** in `/data/system/gammaos/hotkeys/`, written only by Nano after validation (sections 4 and 8), or by root by hand (the developer path in `HOTKEYS.md:421-429`, treated as `imported`). Nano records each file's origin in `/data/system/gammaos/emulators_state.json` (section 2). A file whose origin isn't recorded there is `imported`.
- **(decide)** D2: a community index (a signed JSON in the Thimsy repo or releases) that Nano can fetch (origin `index`). Recommended later (phase 7), never automatic.
- **Linux** reads the same format (OA-54): shipped definitions in `/usr/share/thimsy/emulators/`, user ones in `/storage/.config/thimsy/emulators/` (*proposed paths*, per_device_config_plan.md §2.8), imported through the same confirm screen (`linux_handheld_plan.md` §7.2, decision 17). `packages`, `signers` and `install` are Android fields; a Linux definition launches through `launch.linux`. Emulators the user installs on Linux are later work.

**One validator, two languages.** Nano (`NanoEmuDefs`, C++) and PWM (`ThimsyHotkeyController`, Java) apply the same checks at load: 1 MB size cap (as `openTarget`), `schema`, known keys, and the user-tier rules (section 4). A file either reader refuses is ignored by both. One shared corpus of good and bad definitions drives `tests/unit/test_hotkey_controller.py` and `NanoEmuDefs_test.cpp`. PWM reads off the key path: it reloads when the directories change (a `FileObserver` or the stamp check on a handler thread), not on each hotkey.

### 1a. Fields for settings, shared storage and launch (OA-54)

OA-54 (`docs/linux/DECISIONS.md`) makes the definition the only source of an emulator's per-device configuration (OA-46), its shared storage (OA-52, OA-53), its hotkeys, its controller map, its launch entries and its card or local storage, on Android and Linux. The rules that use these fields are in per_device_config_plan.md: §2.2 the per-device set, §2.7 shared storage and the pointing methods, §5 the sync, §5.7 the launch order, §2.8 the mapping from each rule to a field. This section only defines the fields. They are part of `schema` 1: no definition has shipped.

| Field | Meaning | per_device_config_plan.md |
|---|---|---|
| `files.roots` | Per platform, the base folders the emulator reads. Android: `media` (`/data/media/0/Android/data/<pkg>/files`), `data` (`/data/data/<pkg>`), `card` (the shared root: `emulator_config/<card_folder>` on the drive, or `internal` while the emulator files are on internal storage, `emu_sd` off), `internal` (the folder Move Emulator Files moves into the card folder, for example RetroArch's `/storage/emulated/0/RetroArch`, ScummVM's `/data/data/org.scummvm.scummvm/files`). Linux: `config` (`/storage/.config/<dir>`), `share` (`/storage/.local/share/<dir>`), `card`. `<dir>` matches `^[A-Za-z0-9._-]{1,32}$` | §3 |
| `files.settings[]` | Per-device settings: `{base, rel, dir, many, skip, platforms}`. Each goes into the device's set, `devices/<id>/<card_folder>/[app/]<rel>` | §2.2 |
| `files.shared[]` | Shared storage: `{base, rel, class, card, point, place, platforms, legacy, names}`. `class` is `drive`, `saves`, `states`, `bios` or `content`; `card` is its folder under the `card` root, which may use the placeholders `{save_family}`, `{core}`, `{system}` and `{subdir}`, filled at each launch from the launch entry and the game (such an entry must be pointed by `config_key` or `append`): `{system}` is the game's system folder under `ROMs/`, `{subdir}` the path of the game's own folder below it, empty for a game directly in `ROMs/<system>/` (an empty `{subdir}` leaves no empty path part), so games of one name in two subfolders get two folders. In `legacy` and `names`, `{game}` is the ROM's file name without its last extension, the name RetroArch gives its saves **(verify for `.m3u`, `.cue`, `.zip`, `.7z` and names with several dots, per_device_config_plan.md §8.5)**, matched literally (only a `*` in the pattern is a wildcard), and `{rom_dir}` is the ROM's own folder; `place` is `card` (the default) or `local`; `legacy` lists the places saves made outside the folder may be (`{rom_dir}/{game}.srm` beside the ROM, `saves/{game}.srm` loose, `saves/{core_name}/{game}.srm` sorted by core, `saves/{content_dir}/{game}.srm` and `saves/{core_name}/{content_dir}/{game}.srm` sorted by content directory, `{content_dir}` being the name of the ROM's own folder; a path without `{rom_dir}` is under the `card` root; `{names}` at the end of a `legacy` path stands for each of the entry's `names` patterns, so every per-game file a core writes, not only its `.srm`, is looked for in each place). A file in a `{core_name}` folder belongs to that core's family; any other to the game's legacy pick, else the system's. One is copied in once per game for the family it belongs to when the folder has none, and every other one is listed in Older saves. `names` lists every per-game file name the emulator or any of its cores writes in a placeholder folder (`{game}.srm`, `{game}.rtc`, a core's extra memory card or VMU files, `{game}.state*`); after each run a file of another game's name written there is moved to `thimsy-older/` and listed in Older saves | §2.7, §5.6 |
| `files.shared[].point` | How the emulator reaches it: `slot`, `link`, `bind`, `append` (`{key}`: RetroArch's append config on Linux) or `config_key` (`{file, format, section, key}`, where `file` is one of this definition's `files.settings`). With `config_key` or `append`, `set` holds other keys fixed for the launch, and with `config_key` on a file with no launch copy, `rest` holds the values the stored file keeps: for a key whose `card` path has no launch placeholder, by default the shared-root path itself, `{card}/<card path>`, so the emulator opened outside Nano uses the same copy; only launch-placeholder and `set` keys rest at an emulator default; launch values go only into the per-launch copy where the definition has one (`files.launch_copy`), and then no stored file is reset: each keeps the user's own value of the key, and `rest` is not used; else they are put back to `rest` after each run. With `append`, each key's value in the live settings file, or its absence, is recorded before the run and put back after it (the value restored, the key removed only when it was absent), in case the emulator saved the launch's values there | §2.7, §5.6 |
| `files.exclude[]` | `{base, glob}`: never synced, never moved (caches, shader caches, logs) | §2.2, §2.7 |
| `files.live` | Per platform, where the emulator reads its settings: `local` (its own folder, which is the local copy) or `slot` (its folder on the card, filled from the set before each launch) | §5.6 |
| `files.launch_copy` | Per platform, a settings file each launch passes as a per-launch copy: `{file, base, rel}`, where `file` is one of `files.settings` and `base` and `rel` a path under the definition's own roots that no other launcher names (RetroArch's on Android: `retroarch.cfg`, `media`, `thimsy-launch/retroarch.cfg`). Before a launch the copy is the live file plus the launch's `config_key` and `point.set` values, and the launch template's `{launch_config}` names it; after the run each other key the emulator changed in it goes back into the live file. A `config_key` entry with a launch placeholder in its `card` path, or with a `set`, must name a file that has a `launch_copy` on that platform | §5.6 |
| `files.after` | When the after-run copy runs: `exit` (the emulator's process has ended: on Linux the game's process group, on Android every process of its packages) or `foreground` (Android: Nano is in front again). A definition with a `files.launch_copy`, or with any `config_key` or `append` point, uses `exit`, so no pointing key is put back while the emulator is alive; the validator refuses `foreground` there | §5.6 |
| `launch.android[]` | Today's `players` entries (section 3). A definition may still write `players`; it is read as this list. An entry that runs a libretro core also carries `core`, `core_file` (RetroArch's: `{core}_libretro_android.so`), `save_family`, `core_name` and, when needed, `state_core`, as on Linux. The entry marked `frontend: libretro` (origin `system` only) names the `package`, `activity`, `extras` and `cores` folder of Nano's own `libretro-core` launches (the overlay's, the home's and Quick Resume's), with `{file.path}`, `{core.path}` and `{launch_config}` (the per-launch copy, `files.launch_copy`) as placeholders, and `config_prop`, the system property through which Nano tells the framework's home-screen launch where the per-launch copy is (RetroArch's: `sys.gammaos.retroarch.cfg_dir`); that framework launch stays code for now (per_device_config_plan.md §2.8) | §5.7, §2.7 |
| `launch.linux[]` | `{id, name, systems, exts, argv, core, core_file, save_family, core_name, state_core, frontend}`: `argv` is a list of tokens with `{file.path}`, `{core.path}`, `{append}` and the `files.roots` names as placeholders, never run through a shell; `core` names a libretro core for a RetroArch entry; `core_file` is the core's file name as a template with `{core}` (RetroArch's: `{core}_libretro.so`), which fills `{core.path}` and through which `systems.json`'s `coreSo` is read (the `launch.android` entry's template) and written; `save_family` joins cores whose in-game saves are compatible (OA-21), and a core with none is its own family; `core_name` is the folder name RetroArch's own per-core sorting gives the core (its `library_name` **(verify per core)**), used only to know whose a legacy save is; `state_core`, default `core`, fills `{core}` in a states path, so a core whose two platform builds cannot read each other's states gets its own folder on one platform (per_device_config_plan.md §2.7); `frontend: libretro` (origin `system` only) marks the entry that takes `systems.json`'s `libretro-core` launches | §2.7, §5.7, §8.3 |
| `actions`, `provision` | Hotkeys, as today (section 4). An entry may carry `platforms`. On Linux the drivers are the ones `linux_handheld_plan.md` §5.3 names: RetroArch's network commands and a signal to the game's process group | — |
| `controls` | The controller map (section 5) | — |
| `storage.needs_card_to_start` | Kept (section 6). With shared storage on the card and the card out, the emulator does not start | §2.7 |

**Shorthand kept.** Section 6's `storage.link` entries are read as `files.shared` entries with `point: link`, `storage.sync` items as `files.settings` entries, and `storage.setting` entries as `files.shared` entries with `point.config_key` (the `file`, `format`, `section` and `key` given; `subdir` as the `card` path): the shared root's path is written per launch, and `point.rest` is the entry's `rest` value, else the shared-root path itself, `{card}/<subdir>` (the internal root while `emu_sd` is off). A `storage.setting` path has no launch placeholder, so the stored settings file keeps naming the one copy and the emulator opened outside Nano uses it too; only launch-placeholder and `set` keys rest at an emulator default (per_device_config_plan.md §2.7, §5.6). A path appears in one form only; the validator refuses a path written in both.

**Shipped emulators use the same files.** Thimsy writes a definition for every emulator it ships (RetroArch, PPSSPP and ScummVM on both builds; on Android also Dolphin, Azahar, Vita3K, Flycast, DraStic and the other pack-ins). Today's per-emulator tables become data in those files, moved with no change of behaviour: `NanoEmuSync`'s `items()`, RetroArch's `raDirKeys` and slot list, `NanoSdMove.cpp`'s groups and link lists, the BIOS installer's slots. per_device_config_plan.md §2.7 gives the first cut for each and §2.8 says which of them that plan moves. From then on a pack-in and a user emulator differ only in their definition's origin, and a definition written by hand gets the same integration.

**How the new fields fall under the existing rules.**

- **Validator (section 1).** The same two readers, the same 1 MB cap, the `schema` check and the known-key check; the new keys join the known list. Every path is relative, with no `..` and no leading `/`, under one of the definition's own `files.roots`; a `card` path is under the definition's `card_folder`. A path listed as both settings and shared storage, or as shared and excluded, is refused. Linux uses the same C++ validator (`NanoEmuDefs`) and the same corpus.
- **Origin (sections 4, 6 and 8).**
  - `system` and `index`: every field.
  - `imported`: `card_folder` is forced to `user-<id>`, as today. `files.settings` entries with `base: data` stay in the device's local set and never go to the card set (section 6's rule that an imported definition cannot sync private data to the card). `point: bind` is refused, since it needs a root mount. `point: config_key` may name only the definition's own settings files, like `provision[].file`. `launch.android` passes the player validation (section 3). `launch.linux`'s `argv[0]` must be an executable in the image or under the definition's own `share` root, and runs as `nano`.
  - Overriding a built-in id keeps today's rule (`packages` a subset of the built-in's). An override's `files.shared` may move or narrow the built-in's shared storage within the same card folder, never add a folder outside it.
- **Signing (sections 2 and 8).** The new fields grant no access. They name only files inside the definition's own folders and its card folder, which Nano already writes as root for `provision`. All files access and SAF grants still need a `system` or `index` origin, a signer pin match and consent. So a `slot` that the emulator reads through a SAF grant (Azahar's folder, for example) works for a user emulator only with those. Linux has no APK signer: a Linux definition's trust is its origin alone.
- **User-editable.** Definitions and the per-device sets are plain, documented files (OA-54), with the schema `linux/schema/emulator-def.schema.json` (new). A user edits a copy and imports it (section 8); Nano never acts on a file it has not validated and recorded. Whether the device's set on the card keeps a copy of its user definitions is D12.

### 2. Registry: detection, signer check, grants

A new system_server class, `ThimsyEmulatorRegistry` (`frameworks/base/services/core/java/com/android/server/gammaos/`), next to `NanoPermGrantBridge` and `NanoNetBridge`:
- Registers a context receiver for `ACTION_PACKAGE_ADDED`, `REPLACED`, `REMOVED` and `FULLY_REMOVED` (manifest receivers can't get ADDED/REPLACED on 14: [broadcast exceptions](https://developer.android.com/develop/background-work/background-tasks/broadcasts/broadcast-exceptions)). Also runs once at boot.
- For each installed package that some definition lists, records:
  - whether it is a plain user app: not `FLAG_SYSTEM` / `FLAG_UPDATED_SYSTEM_APP`, not platform-signed, not Nano, the Android Games helper or another Thimsy app. Nano can't tell this from `packages.list`, so it reads it here;
  - its current signer's SHA-256, and whether a definition pin matches.
- Signer check: `PackageManager.hasSigningCertificate(pkg, sha256, CERT_INPUT_SHA256)` "searches the set of signing certificates by which the given package has proven to have been signed" ([reference](https://developer.android.com/reference/android/content/pm/PackageManager#hasSigningCertificate(java.lang.String,%20byte[],%20int))). That suggests a past certificate in a v3 rotation lineage also matches, so a rotated-away key would still pass (inference, UNVERIFIED). The alternative is to compare against the current signer only (`PackageInfo.signingInfo.getApkContentsSigners()`) and treat a rotation as "signer changed: ask again". **(decide)** D9.
- Writes `/data/system/gammaos/emulators_state.json` (system:system 0644): package, definition id, definition origin, versionName, versionCode, `user_app`, signer SHA-256, `verified`, `consent` (copied from Nano's consent file, below), and what was granted. Nano reads this file; it doesn't need PackageManager.
- **Consent is keyed on (package, signer SHA-256)**, not the package alone. A reinstall with another signer has no consent.
- Applies grants only when `verified`, consented and the definition's origin is `system` or `index` (section 8, D10 for `imported`):
  - **All files access, D3.** The recommended form: the registry publishes its set as its own chunked property, `persist.gammaos.perm.mestorage_reg` (base plus `_0`..`_4`, like the existing prop), with a new `property_contexts` entry and a type only system_server may set and MediaProvider and system_server may read. All three readers union it in: `NanoPermGrantBridge.readMesAllowlist`, StorageManagerService through `getSelfGrantedManageStoragePackages()`, and `ThimsyStorageDenylist`. It must be a property: MediaProvider runs as another uid and can't read `/data/system` (UNVERIFIED for its SELinux domain, but the uid alone rules it out). A registry-only set that MediaProvider can't see would make the app look hand-granted, and so **exempt** from the denylist (`README.md:625-635`). The cheaper form writes the existing `persist.gammaos.perm.mestorage`, which works with no reader change but can be set by root and `system_app` (P:180619, P:180631).
  - **No install race.** `NanoPermGrantBridge` grants All files access by package name when a package is installed (`README.md:707-709`; hook UNVERIFIED). For any package not in `PACK_IN_EMULATORS`, the bridge calls the registry synchronously (same process) before it flips the MES app-op: `isGrantAllowed(pkg)` checks the current signer and the consent for that signer. On `PACKAGE_REMOVED` the registry drops the package from the property before anything else. So a same-name APK signed by someone else, installed after an uninstall, never gets MES in the gap.
  - SAF grants: `NanoUriGrant.parse` accepts the registry's consented packages in addition to `APPS`, for folders under that package's `emulator_config/<card_folder>` only. `PROTECTED` folders stay refused.
- On removal or a signer change: revoke what it granted (`uri_revoke`, remove from the property, reset the MES app-op), and mark the entry unverified.
- Nano's consent file `/data/system/gammaos/emulators_consent.json` is written only by Nano's UI. The registry ignores consent whose signer doesn't match the current one.

Why not do the signer check in Nano: Nano is native and reads `packages.list`, which has no signer. Parsing APK signature blocks in C++ or the binary `packages.xml` is far more work than one PackageManager call in system_server.

### 3. Launch in Nano

**Catalog merge (code, Nano).** `loadEmuCatalog()` appends each definition's `players` to `mEmuCatalog`, under the player's `platform` uniqueId, tagged with the definition id. Everything downstream already works on `mEmuCatalog`: `buildEmulatorPicker`, `emuPlatformChoices`, `openGameEmulatorPicker`, `esdeCatalogForLabel`, `playerRepair`.

**Player validation (code, Nano, at load).** `buildLaunchIntent` only splits and substitutes (P:24247-24300), and the overlay runs the result as root (P:38120-38121). So `NanoEmuDefs` checks each player's `amStartArguments`:
- every `-n` names a package in the definition's `packages`, and the template has one;
- options come from an allowlist: `-n -a -d -t -c -f -e --es --ez --ei --el --ef --eu --activity-clear-task --activity-clear-top --activity-new-task --activity-single-top`. Everything else is refused, `--user`, `-D`, `-N`, `-P`, `-S`, `-W`, `--start-profiler`, `--attach-agent` and `--windowingMode` included. The exact list is a draft: settle it against `am`'s help on 14 when building;
- a player whose `uniqueId` equals a catalog player's replaces it only if the definition's origin is `system` and the replaced player already names one of its packages (so a shipped DuckStation definition can fix the catalog's template). Otherwise the definition's player is renamed `<uniqueId>.<definition id>` and added beside it. An imported file can't re-point `psx.ra64.mednafen_psx_hw` or any other player.

**Custom, improved (code, Nano).**
- Game Systems > Emulator > Custom gets a first step **Installed App...**: the user apps with a launcher, minus pack-ins and the apps the registry marks as not `user_app`.
  - The list comes from one call, `cmd package query-activities --brief --components -a android.intent.action.MAIN -c android.intent.category.LAUNCHER`. GammaOS's own setup script used `cmd package query-activities --components -a ... -c ...` (`gammaos/setup.sh`, removed lines at P:173606). `--brief` with `--components` on 14 is UNVERIFIED.
  - Labels need PackageManager. Get them from a small system_server bridge (as `NanoNetBridge` does) or from the Android Games helper; otherwise show package names.
- Picking one fills Package and offers templates. The launcher activity is usually not the one that takes a ROM (DuckStation's is `.EmulationActivity`, Appendix A), so Nano looks for activities of that package that take a file: `cmd package query-activities --components -a android.intent.action.VIEW -d content://com.android.externalstorage.documents/... -t application/octet-stream`, filtered to the package (flags on 14 UNVERIFIED). The templates:
  - "Open the game file" (`-n <pkg>/<activity> -a android.intent.action.VIEW -d {file.uri}`), the PPSSPP/Flycast/ARMSX2 shape (`mods/nano-systems.txt:31-32`, `:40`);
  - "Pass the file path" (`... -d {file.path}`);
  - "Type an intent..." (today's OSK).
- Game Options > Emulator (per game) gets the same Custom row, storing `{rom, label, am}` as today (P:40600-40653).
- A Custom intent passes the same option allowlist as definition players.

**Repair after uninstall (code, Nano).** `playerRepair` today skips builtin systems. Add: a builtin system whose package isn't installed and differs from the seeded one in `/system/etc/gammaos/nano_systems.json` falls back to its seeded emulator, with a banner "DuckStation was removed: PlayStation uses Beetle PSX HW again". The per-game entry keeps its choice and shows `showEmuMissingMsg` (P:59970) as today. **(decide)** D4: fall back automatically (recommended) or leave the system pointing at the missing app.

**Android Games (code, Nano and helper).** Every package listed in a definition (both dirs) is hidden from Android Games unless the definition says `android_game: true`. The helper is an ordinary app and can't read `/data/system`, so Nano writes the list for it: `/data/system/gammaos/android-games-exclude-user.txt` 0644, read next to `EXCLUDE` (`Games.java:47`, `:68`). UNVERIFIED that the helper's SELinux domain may read `system_data_file`; if not, Nano writes the list into the helper's own `Android/data` folder instead.

**What a user emulator still won't get:** Quick Resume (needs per-emulator code: P:60041, P:60157). Say so in its Emulator Support note.

**Seeding (data, optional).** A user who wants DuckStation as the PS1 default picks it in Game Systems. Shipping it as the default is impossible (`scripts/nano_systems.py:98-99` and licensing). No build change.

### 4. Hotkeys

**Precedence fix (code, PWM and Nano).** When two definitions list one package, the `/data` one wins, then the higher `schema`, then the id in sort order. Same rule in `ThimsyHotkeyController.loadProfiles`, `nanohotkeys::loadProfiles` and `emulatorForPackage`. Fixes the `HashMap` nondeterminism (P:168518-168528).

**User tier limits (code, PWM and Nano).** Nano and system_server act on a user definition as root and as system, so for a definition loaded from `/data`:
- `packages`: real package names, no `"*"`. Each must be a plain user app in `emulators_state.json` (`user_app`; PWM asks PackageManager directly). System apps, updated system apps, platform-signed apps, Nano, the Android Games helper and Thimsy's apps are refused. The "Installed App..." and "Add Emulator..." lists apply the same filter.
- `id`: `fallback` is reserved. Reusing another built-in id (`retroarch`, `dolphin`, ...) is an **override**, as `HOTKEYS.md:130` documents today: its `packages` must be a subset of the built-in's, and the import screen says "replaces Thimsy's RetroArch definition". A new id may not list a package a built-in definition claims; it has to override that definition instead.
- `intent`: broadcasts only, with `package` set to one of the definition's packages. No activity starts as system.
- `then`: `exit` or `force_stop` only.
- `provision[].file`, after token expansion, must be under one of the definition's packages' own folders: `/data/data/<pkg>/`, `/data/user/0/<pkg>/`, `/storage/emulated/0/Android/data/<pkg>/`, or that definition's `emulator_config/<card_folder>/` on a drive. Anything else is skipped and logged.
- Built-in definitions keep today's rules.

**Provisioning fixes (code, Nano).**
- Route `/data/data/<pkg>` writes through `appFileWrite` (`NanoBiosUtil.cpp`, P:8871-8929), which removes `<file>.bak` and runs `restorecon`. A file root creates inside `/data/data/<pkg>` may get the wrong MLS categories otherwise (inference, UNVERIFIED on device).
- Add ini `multi: "repeat"` for emulators that store several binds as repeated keys (DuckStation's `GetStringList`, [ini_settings_interface.cpp](https://github.com/stenzek/duckstation/blob/master/src/util/ini_settings_interface.cpp)). `append` joins on one line (`HOTKEYS.md:173`), which DuckStation would read as one bad binding.
- Add tokens: `${appdata}` (`/data/data/<pkg>`), `${appmedia}` (`/storage/emulated/0/Android/data/<pkg>`), so definitions don't spell paths twice.
- **Provision before launch.** Provisioning runs at Nano start and when Nano comes to the front, and only edits files that exist (`HOTKEYS.md:158`, `:162`). Add: `launchXmbGame` and `overlayLaunchGame` run `provision(pkg)` for the package they are about to start when `emulators_state.json`'s stamp changed since the last run (an install or update). The first session after an install still has no hotkeys unless a rule has a `template`, because the emulator creates its settings file on first run. Checklist and README say "hotkeys from the second launch".

**Add Emulator... for unknown emulators (code, PWM and Nano).** Settings > Hotkeys > Emulator Support > **Add Emulator...**:
1. Pick an installed app no definition covers (the user-tier filter applies).
2. Nano writes a generic user definition: save `key F2`, load `key F4`, fast forward `key_toggle F6`, rewind `key_hold F7`, quit `exit`. No provisioning.
3. Nano sets **Bind Mode** for that package (`sys.gammaos.hotkeys.bind_pkg`, not persistent) and launches the app itself, so `sys.gammaos.nano.app_launched=1` is set and the chords are armed (P:167023). It tells the user to open the emulator's own hotkey screen and press Anbernic+R1, L1, R2, L2 at each "press a key" prompt.
4. While Bind Mode is on, every action except quit is sent as a plain tap (`key`), so a toggle or hold driver doesn't leave a key held in the app's dialog. Quit still exits, so the user can leave. Quit is not bound (it stays `exit`).
5. Bind Mode ends when `onAction` sees another foreground package (it already compares, P:168405-168409), and when Nano comes to the front (Nano clears the property).

**Will the virtual keyboard reach the emulator's bind dialog?** The 2021 DuckStation app accepted any device with `SOURCE_CLASS_BUTTON` or `SOURCE_CLASS_JOYSTICK` ("Accept all devices with an axis or buttons, filter in events", `EmulationSurfaceView.isBindableDevice`, `EmulationSurfaceView.java:28-36` at [81da9be](https://github.com/stenzek/duckstation/tree/81da9be2d1040665ebfaf2db6d7fdb710a48a383/android/app/src/main/java/com/github/stenzek/duckstation)). `SOURCE_KEYBOARD` (0x101) includes `SOURCE_CLASS_BUTTON` (0x1), so keyboards pass. `isBindableKeyEvent` refuses only BACK, HOME and POWER from a non-gamepad (`:48-58`). The dialog stores `<descriptor>/Button<keycode>` from `event.getDevice()` (`ControllerBindingDialog.java:76-82`). Open questions, settled in Phase 0 step 6:
- whether `InputDevice.getDevice(-1)` returns the virtual keyboard on this build and gives it a descriptor the app later matches during play (UNVERIFIED; `HOTKEYS.md:115` saying it reports F1-F12 in `hasKeys` suggests the device exists);
- what the current Play build and the 2026 beta do (both closed source).

**Gamepad driver (code, PWM, only if needed).** If Phase 0 step 6 fails, add a `pad_key` driver that injects `SOURCE_GAMEPAD` keys (`BUTTON_1`..`BUTTON_16`). Whether an app that binds by device descriptor accepts an injected event's device is UNVERIFIED. Only build it after Phase 0 shows it is needed.

**Fallback shown in the button map (code, Nano).** When `ctx.emulator` is empty, pass the `fallback` profile's actions to `chordRows` (P:28551-28553) so Quit is listed.

### 5. Controller mapping

**Default binds.** Most Android emulators bind Android keycodes by the Xbox convention. The definition says so with `controls.defaults_convention: "android_xbox"`. What the user then gets depends on gammapad:

| State | Built-in pad | Other pads | Device a device-keyed app binds |
|---|---|---|---|
| gammapad off (Cube default) | Positional (keylayout) | By their own keylayout: positional except Switch Pro (`controller_layout_plan.md:88-98`) | The real pad |
| gammapad on, ABXY Swap off | Positional (test 37) | By label | gammapad's virtual pad (045e:0b13) |
| gammapad on, ABXY Swap on | Swapped | Swapped | gammapad's virtual pad |

The button map pushes `defaults_convention` through the existing `PadModel` inversion with the live gammapad and swap state (`button_map_overlay_plan.md:585-606`), so labels are right in all three rows. Thimsy writes pad binds only through controller_layout_plan.md phase 5 (owner sidecar, `ifUnset`), using `controls.provision` rules with `${bind:<phys>}` tokens filled from the pad model. Until that phase lands, user emulators get the emulator's own defaults.

**Player assignment (Merge Controllers).** Pack-ins get per-emulator player rules in controller_layout_plan.md §9 (`:507-535`). A definition declares `controls.players`:
- `"single"`: one player; every pad drives it. Merge On and Off need nothing.
- `"global"`: binds are global; the emulator takes any pad as player 1 (OR'd). Merge Off can't give player 2 its own pad.
- `"per_device"`: the emulator assigns ports by device; optional `controls.players_provision` rules with `${pad:primary}` / `${pad:builtin}` descriptor tokens, written by the phase 5 writers.
- `"unsupported"` (default when absent): Thimsy doesn't touch it; the Merge Controllers help says so for this emulator.

While gammapad runs there is one virtual pad and no player 2 for any emulator (`controller_layout_plan.md:104`).

**Declarative reader (code, Nano).** `readEmulator` falls through to `readDeclarative(env, ctx, def.controls)` when no C++ reader matches (P:16201). It:
- reads `controls.read.file` with the existing bounded `readAppFile` / `readMovedAppFile`, `iniValue`, `cfgValue` helpers;
- parses each value with a small pattern list (`{dev}/Button{key}`, `{dev}/{sign}Axis{axis}`, `SDL-{n}/{sdl}`, `Keyboard/{name}`), no `std::regex` on untrusted text;
- maps SDL names to Android inputs with the table the Vita3K reader already embeds (`button_map_overlay_plan.md:420`; P:15533-15577);
- maps console ids (`btn-cross`, `dpad-up`, `stick-l:left`, ...) through `controls.binds` onto the drawing's anchors;
- returns `Source::kNotMapped` with `controls.read.not_mapped_where` when the section is absent, or `Source::kDefaults` with the convention table and a "Default mapping (assumed)" note when `read` is missing or the file can't be read.

The 14 C++ readers stay. `defaultSystemFor` (P:11309-11317) consults the definition's `systems[0]` after its table (Phase 1, so the drawing shows as soon as a definition ships).

**Device-keyed emulators.** An emulator that keys binds by Android device descriptor (DuckStation 2021, Dolphin's `Device=`, `button_map_overlay_plan.md:355`) needs the pad's descriptor, which Nano can't compute (`controller_layout_plan.md:633`). The reader filters on `sys.gammaos.nano.bmap_dev` (`button_map_overlay_plan.md:141`) and shows "Bound to another controller" on a mismatch. While `init.svc.gammapad=running`, a bind to gammapad's virtual pad descriptor counts as "any held pad", since every grabbed pad reaches the app through it.

### 6. Storage export and BIOS

**Definition `storage` block (data):**
```json
"storage": {
  "card_folder": "duckstation",
  "link":  [{"base": "media", "dirs": ["memcards", "savestates", "bios", "covers", "screenshots", "cheats", "gamesettings", "inputprofiles"]}],
  "sync":  [{"base": "data", "rel": "shared_prefs"}, {"base": "media", "rel": "settings.ini"}],
  "setting": [{"file": "${appmedia}/files/settings.ini", "format": "ini", "section": "MemoryCards", "key": "Directory", "subdir": "memcards"}],
  "saf":   {"pref_file": "", "key": ""},
  "all_files": false,
  "needs_card_to_start": true
}
```
- `card_folder`: must match `^[a-z0-9_-]{1,32}$`. Built-in group folder names (`NanoSdMove.cpp` `kGroupPkg[]`, P:64857-64897: `dolphin`, `retroarch`, ...) and a name another definition already uses are refused, and the import fails on a clash. For `imported` definitions Nano forces the prefix `user-` (`user-<id>`), so a user file can't write into a shipped definition's folder.
- `base`: `media` is `/data/media/0/Android/data/<pkg>/files`, `data` is `/data/data/<pkg>`. Paths are relative and may not contain `..` or start with `/`.
- `link`: Dolphin's method (strategy C). Each listed subfolder moves to `<drive>/emulator_config/<card_folder>/<dir>` and becomes a link. These folders are shared storage (OA-52, OA-53): one copy on the card for every device, no device mirror. From OA-54 on they are written as `files.shared` entries with `point: link` (§1a).
- `sync`: per-device settings (strategy D), written as `files.settings` entries from OA-54 on (§1a). They go into this device's set on the card, `devices/<id>/<card_folder>/` (per_device_config_plan.md §2.2), not into one copy every device shares. The card copy is where any app with storage access can read and change the files, and card edits flow back into the app with the card winning (`README.md:588-599`, per_device_config_plan.md §5.1). So:
  - `base: "media"` is allowed for every origin (`Android/data` is already outside the app's private data);
  - `base: "data"` only for `system` and `index` origins, only for `shared_prefs` or named files, with a size cap per item (1 MB). An `imported` definition can't sync private data;
  - the confirm screen says "copies <app>'s private settings to the card, where other apps can read and change them" when a `data` item is present.
- `setting`: point one key in an `ini` or `xml_prefs` file at the card path (strategy A, generic formats only). From OA-54 on it is a `files.shared` entry with `point.config_key` (§1a): the path is written before each launch and the `rest` value (by default the shared-root path itself, so the emulator opened outside Nano uses the same copy) put back after it.
- `saf`: give the app `emulator_config/<card_folder>` as a picked folder and write the tree URI into a prefs key (strategy B). Needs registry consent.
- `all_files`: request All files access through the registry (verified + consent, origin `system` or `index`).

**Definition `bios` block (data).** `"bios": [{"system": "psx", "dir": "${appmedia}/files/bios"}]`. The BIOS installer gets one more `SlotKind`, `SLOT_DEFINITION` (next to `SLOT_RA` ... `SLOT_MAME4DROID`, P:6794-6810): for each installed definition package, every catalog row of that `system` (for psx, P:5776-5782) is also copied to `dir`, under the row's name. The accepted files and SHA-1s come only from Nano's catalog, never from the definition. `dir` passes the same per-package path rules as `provision[].file`. Installed files follow Move Emulator Files through the `link` of that folder, as BIOS files do for pack-ins (`README.md:704`). Whether DuckStation's Android builds look in `files/bios` is UNVERIFIED (Phase 0 step 9).

**Code changes (Nano):**
- Groups become dynamic: built-in `kGroup*` stay; each definition with `storage` gets a group id above them. `groupPkg()`, `fileJobRelocate`, `emuRepointAbsent`'s link loop and `sdGrantsFrom` look definitions up.
- `emusync::items()` comes from the definitions only, shipped ones included (`kItems` becomes their data, §1a), rebuilt when the definition stamp changes.
- Every rule passes through `sdSourceRoot`, `sdTooBroad`, `sdAppPathSafe` and the "link only into this drive's `emulator_config/<card_folder>`" check. These cover the app's own files; the card copy is covered by the `sync` and `card_folder` rules above.

**Card out.** Which rule applies depends on usb_storage_plan.md:
- **Before it lands:** today's rule. `needs_card_to_start` makes a definition group block the launch with the usual message while its drive is out (`README.md:586-587`).
- **After it lands:** under OA-52 and OA-53 shared storage has no device mirror, so per_device_config_plan.md §2.7 recommends that an emulator with shared storage on the card waits for the card, and the fallback below covers settings only (its owner question 20). If the owner chooses the fallback for shared storage too: `needs_card_to_start` maps to its fallback. Sync items fall back to internal copies like pack-ins (its option A or B). For `link` groups, the links point at the missing drive, so on removal each link is swapped for an internal folder (empty, or the mirror if option A keeps one); when the drive returns, the internal contents are copied back without overwriting (the drive wins) and the links are restored. The same swap is needed for Dolphin and hakuX, so it belongs in usb_storage_plan.md and definitions reuse it.
- Order: Phase 5 ships either before usb_storage_plan.md with the blocking rule, or after it with the swap. It doesn't define a third behaviour.
- `emuSdSlotCard()` becomes `emuSettingsDrive()` there, and definition groups get it with no extra work.

**Unknown emulator:** nothing moves automatically. **(decide)** D5: offer an experimental "Keep this app's files on the card" that links every top-level folder of `Android/data/<pkg>/files` except `cache`/`shaders`, or nothing (recommended for the first version).

### 7. What a completely unknown emulator gets (no definition)

| Feature | Works? | How |
|---|---|---|
| Launch from a system | Yes | Custom > Installed App... with a template |
| Launch per game | Yes | The new per-game Custom row |
| ROM access by content URI | Yes | Provider and grant relaxations (P:162597, P:168795) |
| Storage runtime permissions | Yes | `NanoPermGrantBridge` (P:166305) |
| Quit hotkey | Yes | `fallback.json` |
| Other hotkeys | After Add Emulator... and binding in the app | Section 4 |
| Button map | Drawing plus Quit row; no binds | Section 4 fix |
| Pad binds | The emulator's own defaults (positional if Xbox convention and gammapad off or swap-free) | Keylayout, section 5 |
| Merge Controllers | Not handled: whatever the app does; no player-2 assignment with Merge Off | Section 5 |
| Move to card | No (D5) | — |
| BIOS installer | No | Needs a definition `bios` block |
| All files access / SAF grants | No | Needs a `system` or `index` definition with a signer pin |
| Hidden from Android Games | Only if Add Emulator... was used | Nano's user exclude list |
| Quick Resume | No | — |

### 8. Security and privacy

- **Root acts on definition data.** Nano is root and writes files a definition names, and runs `am start` with its players; system_server starts intents a definition names. User definitions get the user-tier limits (section 4), the player validation (section 3) and the storage rules (section 6). Validation runs at import and again at load, in both Nano and PWM.
- **Import, not live read.** Definitions are never read from shared storage. Import is Settings > Hotkeys > Emulator Support > **Import Definition...** from `/sdcard/Thimsy/emulators/*.json` (or Download). Nano shows what the file asks for: packages, definitions it replaces, files it writes, folders it links or syncs, grants. The user confirms; Nano copies it to `/data/system/gammaos/hotkeys/<id>.json`, system:system 0644, `system_data_file`, and records origin `imported`. Any app with storage access can drop a file in `/sdcard`, so a confirm screen is mandatory. Where the import folder `Thimsy/emulators/` lives is not settled across the plans (D11): this plan imports from internal shared storage, `android_launcher_plan.md` §3 item 8 says the definitions travel with the card, and `linux_handheld_plan.md` §7.2 lists the folder on the drive and recommends importing from both.
- **Directory.** Create `/data/system/gammaos/hotkeys` from Nano's rc (`mkdir ... 0775 system system`) so no SELinux question arises over who creates it. Files must be readable by system_server (uid 1000): 0644 or owned by system. UNVERIFIED whether Nano's domain (`bootanim`, P:180563ff) may write there; the rc entry plus a sepolicy line settles it. Both come in Phase 1, before anything writes a user definition.
- **What a signer pin proves depends on origin.** For a `system` or `index` definition, Thimsy chose the pin, so a match means the APK comes from the expected developer. For an `imported` definition, whoever wrote the file chose the pin, so a match only means the APK is the one the file names: "pin plus consent" is consent alone. So:
  - All files access and SAF grants go only to `system` and `index` definitions, after a pin match and the user's yes on a screen that names the app, the access and the definition's origin;
  - **(decide)** D10: an `imported` definition gets no grants (recommended), or gets them after a second confirm screen that says the source is unverified;
  - a package whose signer doesn't match gets launch and hotkeys (which only touch its own files) and no grants.
- All files access granted by Thimsy exempts nothing from the denylist (`README.md:631-632`), provided MediaProvider sees the package in the self-granted set (section 2, D3). It still lets the app read all shared storage.
- **Writing another app's config** is the same thing a user does in that app's UI. It only happens while the app isn't running (P:23414-23423), keeps owner and mode, and stays inside the package's own folders.
- **Untrusted JSON.** Definitions go through `njson` (Nano) and `JSONObject` (PWM) with the shared validator (section 1): size limit 1 MB, `schema`, known keys, user-tier rules. The existing JSON fuzzer (`test/fuzz/json_fuzzer.cpp`, P:150896) gets a definition corpus, and the player validator is fuzzed too.
- **Privacy.** A community index (D2) is fetched whole, never queried per installed package, so the server doesn't learn which apps a user has. No telemetry.
- **Licensing.** Thimsy never ships, mirrors or links a download of an APK whose terms forbid it. DuckStation's beta page forbids APK redistribution and asks for links to the page ([downloads.duckstation.org/android-testing](https://downloads.duckstation.org/android-testing/)); the project's licence is CC BY-NC-ND 4.0 ([LICENSE](https://raw.githubusercontent.com/stenzek/duckstation/master/LICENSE)) and its README treats pre-configured packages as modifications ([README](https://raw.githubusercontent.com/stenzek/duckstation/master/README.md)). A definition carries the official URL only, and nothing is pre-seeded into the image. Whether writing hotkey binds at runtime into the user's own install counts as "modification" is a legal reading, not settled here (UNVERIFIED); it distributes nothing.

### 9. Emulator updates

- The registry records `versionCode` per package. On a change Nano:
  - re-runs provisioning for that package before its next launch (section 4), so binds come back after an update that resets settings;
  - shows "Version <x> not tested with this definition" in Emulator Support when `tested` doesn't list it. Nothing is blocked.
- A provision rule whose file no longer exists is a no-op (`HOTKEYS.md:163`). Rules for both of an emulator's known layouts can sit in one definition; only the one present applies. DuckStation needs this (Appendix A).
- A signer change revokes grants and consent (section 2).
- Links survive an update. An update that deletes and recreates a folder puts files back on internal storage; the after-game quiet move (`README.md:606-610`) covers definition groups once they are dynamic.
- Uninstall deletes `Android/data/<pkg>` and `/data/data/<pkg>`, links included; card data under `emulator_config` stays (Dolphin relies on this; UNVERIFIED for installd's recursive delete). On reinstall the links are made again after the app first creates its folder.

## Phases

Each phase says what is code and what is data.

**Phase 0: DuckStation facts on a Cube (no code).** Needs a Cube with DuckStation installed from Play and, separately, the 2026 beta. Read-only checks:
1. `dumpsys package com.github.stenzek.duckstation`: versionName, targetSdk, declared permissions (MANAGE_EXTERNAL_STORAGE?), signer, signing lineage.
2. `apksigner verify --print-certs` on the installed `base.apk` (pulled with root): the pin.
3. Launch both templates (`bootPath {file.uri}` and `{file.path}`) with a `.cue`/`.bin` pair and a `.chd`, from Nano's home and from the overlay.
4. Settings store: `ls` `/data/data/<pkg>/shared_prefs` and `/data/media/0/Android/data/<pkg>/files`; find `settings.ini`.
5. Bind one hotkey and one pad button in DuckStation's UI and read the stored value: prefix, descriptor or name. Repeat with gammapad on (swap off and on).
6. With chords armed (launched from Nano), does DuckStation's binding prompt take an injected F2 from the virtual keyboard (device -1), and does the bind then fire during play? Also `input keyevent KEYCODE_F2` for comparison.
7. Does a fresh install have default pad binds, and which convention? Which pad does it treat as player 1 and 2 with two pads and gammapad off?
8. Is it flagged as a game (`dumpsys package | grep -i game`)?
9. Where does it look for the PS1 BIOS (`files/bios`?), and does it create its settings file on first run?

**Phase 1: foundation and launch (code: Nano, PWM, rc, sepolicy; data: definition `players`, `systems`).**
- `NanoEmuDefs` validator and the matching Java checks in `ThimsyHotkeyController` (size, `schema`, known keys, origin, user-tier rules, player validation, `card_folder`), with the shared corpus. PWM loads off the key path.
- §1a's fields in the validator and in `linux/schema/emulator-def.schema.json`, and shipped definitions for the bundled emulators that carry today's tables (`items()`, `raDirKeys`, the link lists). per_device_config_plan.md's phase 0b needs them.
- Precedence fix in PWM, Nano and `emulatorForPackage`.
- rc `mkdir /data/system/gammaos/hotkeys` and the sepolicy line.
- `loadEmuCatalog` merges definition players. Custom > Installed App... with templates. Per-game Custom row. `playerRepair` seed fallback (D4). Android Games user exclude list.
- `defaultSystemFor` consults `systems[0]`.
- `mods/hotkeys/duckstation.json` with `players` and `systems` only (quit from the fallback).
- 1.2: README text on picking DuckStation (Standalone) in ES-DE. No code (or D8).

**Phase 2: hotkeys (code: PWM, Nano; data: definitions).**
- `appFileWrite` routing, ini `multi: repeat`, `${appdata}`/`${appmedia}`, provision before launch.
- Add Emulator... and Bind Mode (`sys.` property, cleared on foreground change and by Nano). Fallback Quit row in the button map.
- DuckStation `actions` and `provision` from Phase 0 findings.
- `pad_key` driver only if Phase 0 step 6 fails.

**Phase 3: registry, import and grants (code: system_server, Nano, MediaProvider, sepolicy; data: `signers`).**
- `ThimsyEmulatorRegistry`, `emulators_state.json` (with origin and `user_app`), consent file keyed on signer, consent screen.
- D3: `persist.gammaos.perm.mestorage_reg` with its `property_contexts` entry, unioned in `NanoPermGrantBridge`, StorageManagerService and `ThimsyStorageDenylist`. The bridge's synchronous registry check before any non-pack-in MES grant. Removal drops the package first.
- `NanoUriGrant` accepts consented packages for their own `emulator_config/<card_folder>`.
- Import Definition... with the confirm screen (origin, replaced definitions, writes, links, syncs, grants). D10.

**Phase 4: button map (code: Nano; data: `controls.read`, `controls.binds`).**
- `readDeclarative`, the gammapad virtual-descriptor rule, `defaults_convention` through `PadModel`, DuckStation fixtures captured in Phase 0.

**Phase 5: storage and BIOS (code: Nano; data: `storage`, `bios`).**
- Dynamic groups, definition sync items (§1a's `files` entries) with the origin rules, `card_folder` checks, card-out behaviour per usb_storage_plan.md's state (section 6), the D5 choice.
- `SLOT_DEFINITION` in the BIOS installer.
- DuckStation `storage` and `bios` blocks per Phase 0 (link for the Play line; SAF if the beta's data-location setting can take a granted folder, UNVERIFIED).

**Phase 6: pad provisioning and players (code: with controller_layout_plan.md phase 5; data: `controls.provision`, `controls.players`).**
- `${bind:<phys>}` and `${pad:*}` tokens, the owner sidecar for definitions, Merge Controllers rows per `controls.players`.

**Phase 7: community definitions (code: Nano; data: index) (D2).**
- Signed index, fetched whole, offered as "Get definition for <app>" when an installed app matches. Signature with a Thimsy key (Ed25519 via BoringSSL, already in AOSP). Its definitions get origin `index`.
- Further shipped definitions, each after its own Phase-0-style check: AetherSX2/NetherSX2, Lime3DS, Citra MMJ, Eden and other Switch forks (network_storage_plan.md:9 already names Eden).

## Decisions for the user

- **(decide) D1:** grow the hotkey profile into the definition (recommended), or a separate `emulators/` directory?
- **(decide) D2:** a community definition index (phase 7), or shipped and hand-imported definitions only?
- **(decide) D3:** grant All files access through the existing `persist.gammaos.perm.mestorage` prop (no reader changes, but root and `system_app` can set it), or a registry-owned `persist.gammaos.perm.mestorage_reg` that only system_server can set, read by all three readers (recommended)?
- **(decide) D4:** when a user emulator is uninstalled, the system falls back to its seeded emulator automatically (recommended), or stays on the missing app?
- **(decide) D5:** an experimental generic "Keep this app's files on the card" for unknown emulators, or none in the first version (recommended)?
- **(decide) D6:** DuckStation's load key: F4 (Thimsy's shared key, provisioned) or F1 (DuckStation's desktop default, no provisioning needed if the Android app applies it). Desktop F4 is "select next slot" (`src/core/hotkeys.cpp`, [source](https://github.com/stenzek/duckstation/blob/master/src/core/hotkeys.cpp)), so an unprovisioned F4 would change slot, not load.
- **(decide) D7:** DuckStation quit: `exit` only (fast, no resume state), or `PowerOff` key then `exit` (saves a resume state when its Save State On Exit is on)?
- **(decide) D8:** user emulators on 1.4.4 only (as drafted; 1.2 gets README text for ES-DE), or port the hotkey controller, Move Emulator Files and the button map to the 1.2 base? The port means carrying framework and Nano code onto a prebuilt system that today takes no patch (`README.md:993-996`): a large port, not estimated here and not part of this plan's phases.
- **(decide) D9:** signer check: accept any certificate in the app's rotation lineage (`hasSigningCertificate`, simpler), or only the current signer, with a rotation treated as "signer changed: ask again" (stricter, recommended)?
- **(decide) D10:** an imported definition gets no All files access or SAF grants (recommended), or gets them after a second confirm screen that says the source is unverified?
- **(decide) D11:** where the import folder `Thimsy/emulators/` lives: internal shared storage only (as section 8 is drafted), the ES-DE Drive only (`usb_storage_plan.md`), or both? Recommended: both, always through **Import Definition...** and its confirm screen, never read live from a drive; this is what `linux_handheld_plan.md` decision 17 recommends for Linux. That decision and this one settle it together, and `android_launcher_plan.md` §3 item 8 follows.
- **(decide) D12:** OA-54 keeps definitions locally or on the card. Recommended: with the SD card settings switch on, the device's set on the card carries a copy of its user definitions (`devices/<id>/emulators/<def id>.json`), and a copy changed on the card is used only after this device's Import Definition... screen accepts it. Or local only, imported again by hand after a reinstall. Settled with per_device_config_plan.md owner question 21.

## Risks

- **DuckStation's Android app is closed source and changing.** Two app lines share the package and key: the Play build (`0.1-89xx`) and the 2026 rewrite beta, which doesn't migrate settings ([beta page](https://downloads.duckstation.org/android-testing/)). Any provisioning format can break with a beta. Mitigation: rules for both layouts, no-ops when absent, `tested` versions shown.
- **Keyboard hotkeys may not reach it.** The 2021 code accepts keyboard devices, but whether the virtual keyboard (-1) is enumerated and keeps a matching descriptor, and what current builds do, is open. If not, the `key` drivers do nothing; the `pad_key` driver is unproven.
- **`{file.path}` vs `{file.uri}`.** Frontends have hit "permission denied" with plain paths ([LaunchBox forum](https://forums.launchbox-app.com/topic/72598-duckstation-android-failed-to-load-cd-image-permission-denied/)). The catalog entry uses a path (P:88936); ES-DE uses SAF (P:18188). Phase 0 decides.
- **Framework launch parser is base code** (UNVERIFIED for arbitrary `-e`/`--ez`). Mitigation: Phase 0 step 3 from both launch paths.
- **Root acts on untrusted definitions.** Writes into app folders, `am start` as root, system-uid broadcasts. Mitigation: user-tier package filter, reserved and override-only built-in ids, player option allowlist, path limits per package, no links followed, `appFileWrite`, skip while running.
- **Granting All files access widens access to all shared storage.** Mitigation: `system`/`index` origin, signer pin, consent keyed on signer, MediaProvider seeing the set (D3), the bridge's synchronous check.
- **Private data on the card.** `base: "data"` sync puts an app's private settings where other apps can read and change them. Mitigation: origin rule, `shared_prefs` or named files only, size cap, a line on the confirm screen.
- **Duplicated lists drift.** `PACK_IN_EMULATORS` exists twice already. The registry must feed `NanoPermGrantBridge`, `ThimsyStorageDenylist` and `NanoUriGrant` from one place.
- **HashMap order** in profile loading picks a random winner today; fixed in phase 1.
- **Card or drive out with linked folders:** the app may crash or recreate empty folders. Mitigation: launch gating as for Dolphin, or the link swap once usb_storage_plan.md lands.
- **gammapad changes what "positional" means** and which device a device-keyed app binds. Mitigation: the three-state table in section 5, Phase 0 step 5.
- **SELinux** for root-created files inside `/data/data/<pkg>`: wrong MLS level can block the app from rewriting them (inference). Mitigation: `restorecon` via `appFileWrite`; check `ls -Z` on a device.

## Tests

**Host unit tests (`tests/unit/`)**
- `test_data_lint.py`:
  - `test_hotkeys` (`:187-197`, today only `id` uniqueness) validates shipped definitions with the shared validator: known keys, `schema` 1, `signers` 64 hex digits, player option allowlist, each `-n` naming a package from `packages`, `systems` exist in `controllers.json`, storage paths relative with no `..`, `card_folder` pattern and uniqueness.
  - `test_button_map_emulators_have_profiles` (`:199-216`) is unchanged; a new test requires `controls.read` or `defaults_convention` for definitions with no C++ reader.
- `test_hotkey_controller.py`, on the shared corpus: `/data` beats `/system` on a package clash; deterministic winner for two ids on one package; user `intent` limited to own-package broadcasts; a user `"*"` and a user `fallback` id are ignored; an override of a built-in id with extra packages is refused; a system package is refused; `then` limited; an oversize or `schema` 2 file is ignored.
- `test_nano_uri_grant.py`: consented registry packages get only their own `emulator_config/<card_folder>`; everyone else is still refused; protected folders refused.
- MediaProvider `ThimsyStorageDenylistTest.java` (P:179386): a package in `persist.gammaos.perm.mestorage_reg` that holds MES is not exempt; one holding MES and in neither set is exempt (today's hand-grant rule).
- A framework test for `NanoPermGrantBridge`: a non-pack-in package without verified consent for its current signer gets no MES.

**Nano C++ tests (`frameworks/base/cmds/gammaos-nano/test/unit/`)** (Nano's test suite is here; the patch has no `ps3xmb/test` directory)
- New `NanoEmuDefs_test.cpp`: the shared corpus; path limits per package, size, unknown drivers and formats, no `*`, player option allowlist, `uniqueId` replacement only for `system` origin, `card_folder` rules, `base: "data"` refused for `imported`, `${appdata}`/`${appmedia}` expansion. §1a: paths under `files.roots`, a path in both settings and shared refused, `bind` refused for `imported`, `config_key` only into the definition's own settings files, `launch.linux` argv rules, `imported` `data` settings kept out of the card set; the shipped definitions reproduce today's `items()`, `raDirKeys` and link lists.
- `NanoSystemsHeal_test.cpp` (P:160383): seed fallback for a builtin system on a missing package (D4).
- `NanoLaunch_test.cpp` (P:157974): DuckStation templates with `{file.uri}` and `{file.path}` for internal, card and subfolder ROMs.
- `NanoButtonMapReaders_test.cpp` (P:153622): `readDeclarative` against fixture trees under `test/fixtures/buttonmap/duckstation/{bound,unbound}` captured in Phase 0; the gammapad virtual-descriptor rule.
- `NanoEmuSync_test.cpp` (P:156421): the items come from the definitions; the shipped ones equal today's `kItems`.
- `NanoBiosCatalog_test.cpp` (P:152303): `SLOT_DEFINITION` installs a psx row to a definition's `dir`.
- `test/e2e/NanoFileJob_e2e_test.cpp` (P:147331): a definition-driven link move and repoint.
- `test/fuzz/json_fuzzer.cpp`: definition corpus and the player validator.

## Docs

- README: a new section "Emulators you install": what works with no definition, with a definition, the DuckStation example, the consent screen, "hotkeys from the second launch", and the 1.2 ES-DE route. Update the storage-access bullet (`README.md:707-709`), the Move Emulator Files list (`README.md:560-587`) and the BIOS dialog text (`README.md:686-704`).
- HOTKEYS.md: Profiles (`:126-132`) become definitions; the new blocks, §1a's included; user-tier limits and reserved ids; Add Emulator... and Bind Mode; Import Definition...; rewrite "Adding an emulator" (`:421-429`).
- BIOS.md: definitions' `bios` targets.
- TODO.md: add this plan under a new "Emulators" heading.

## Device checklist (acceptance)

On a Cube with the 1.4.4 build:
1. Install DuckStation from Play. Game Systems > PlayStation > Emulator lists it; pick it; a `.cue`/`.bin` game and a `.chd` start from the home and from the overlay.
2. Game Options > Emulator on one PS1 game picks DuckStation while the system stays on Beetle; Recently Played reopens it in DuckStation.
3. Custom > Installed App... lists user apps only (no system apps, no Nano) and sets an emulator with no definition; a game starts.
4. Uninstall DuckStation: the system falls back (D4) with a banner; reinstall restores the choice only if the user picks it again.
5. DuckStation doesn't appear under Android Games.
6. Hotkeys, from the second launch after install: Anbernic+R1 saves, +L1 loads, +R2 toggles fast forward, +L2 rewinds (with rewind on), quit closes (D7). Emulator Support shows DuckStation and its note.
7. Unknown app: Add Emulator... launches it, bind in the app, then hotkeys work. Bind Mode ends when the app leaves and after a reboot.
8. Button map over DuckStation shows the dualshock drawing with its binds (or "assumed defaults"), and the Quit row, with gammapad off, on with swap off, and on with swap on. An app with no definition shows the Quit row.
9. Import Definition... from `/sdcard/Thimsy/emulators` shows the confirm screen with the origin "imported"; a definition that writes outside its package's folders, lists a system app, uses `fallback` as its id, or has a player with `--user` is refused with a reason.
10. Consent screen for DuckStation names the app, the access and the origin; with a non-matching signer (a re-signed APK, installed after an uninstall), no grants at any point and a "signature doesn't match" note. An imported definition gets no grants (D10).
11. Move Emulator Files to microSD moves DuckStation's memcards and states; DuckStation still finds them; card out blocks its launch with the usual message, also after usb_storage_plan.md, since memcards and states are shared storage with no internal copy (per_device_config_plan.md §2.7), unless the owner picks that plan's fallback for shared storage (its owner question 20); card back, games start.
12. With usb_storage_plan.md in: Emulator Settings Drive on a USB drive; pull it during the home screen and during a DuckStation game; DuckStation's links are swapped to internal and restored when the drive returns, with nothing lost.
13. Install Your Own BIOS Files with a PS1 BIOS puts it where DuckStation looks; DuckStation boots with it.
14. Update DuckStation (Play to beta): binds come back after its settings reset; "not tested" note if the version isn't listed; grants kept (same key).
15. On the 1.2 build with ES-DE: DuckStation (Standalone) can be chosen in ES-DE and starts a game. No other feature is expected there (unless D8 says port).

## Appendix A: DuckStation definition

Identity and distribution:
- Package `com.github.stenzek.duckstation`; launch activity `.EmulationActivity` (ES-DE find rules, P:18188 key; [es_find_rules.xml](https://gitlab.com/es-de/emulationstation-de/-/raw/50e4b600ae533d772bae3ff880d11a09b05dbe84/resources/systems/android/es_find_rules.xml)).
- Sources: Google Play ([listing](https://play.google.com/store/apps/details?id=com.github.stenzek.duckstation)) and the official beta page ([downloads.duckstation.org/android-testing](https://downloads.duckstation.org/android-testing/)), which states it uses the Play build's signing key and needs Android 7+. Android code left the public repo with [commit 6e49adb](https://github.com/stenzek/duckstation/commit/6e49adb508c009c5f7998a38d47e1d0d174990d0) (parent 81da9be; the year, 2021, is UNVERIFIED from the commit page).
- Signer pin: SHA-256 `a6357957d296b3b03edeb62da317cb347ecde63b6577977ffcbfe0d9a6975450` from APKMirror's listing ([APKMirror](https://www.apkmirror.com/apk/stenzek/duckstation/)). UNVERIFIED until checked with `apksigner` on a device (Phase 0 step 2).
- Origin `system` (shipped in `mods/hotkeys/`), so its pin is Thimsy's choice and grants are possible after consent.
- Not bundled, not mirrored, not pre-configured in the image (section 8).

Draft file `mods/hotkeys/duckstation.json`. Everything marked `UNVERIFIED` in the notes below is filled or removed after Phase 0.

```json
{
  "schema": 1,
  "id": "duckstation",
  "name": "DuckStation",
  "packages": ["com.github.stenzek.duckstation"],
  "signers": ["a6357957d296b3b03edeb62da317cb347ecde63b6577977ffcbfe0d9a6975450"],
  "install": {"url": "https://www.duckstation.org/", "store_id": "com.github.stenzek.duckstation",
              "note": "Install DuckStation from Google Play or duckstation.org. It needs a PS1 BIOS you dumped."},
  "tested": [],
  "systems": ["psx"],
  "note": "Save and load use DuckStation's selected slot. Rewind needs Rewind turned on in DuckStation. No Quick Resume.",
  "players": [
    {"platform": "psx", "name": "psx - DuckStation", "uniqueId": "psx.com.github.stenzek.duckstation",
     "acceptedFilenameRegex": "^(.*).(?:bin|chd|cue|ecm|exe|img|iso|m3u|mds|pbp|psexe|psf)$",
     "amStartArguments": "-n com.github.stenzek.duckstation/.EmulationActivity\n-e bootPath {file.uri}\n--ez resumeState false\n--activity-clear-task\n--activity-clear-top"}
  ],
  "actions": {
    "save_state":   {"driver": "key", "key": "F2"},
    "load_state":   {"driver": "key", "key": "F4"},
    "fast_forward": {"driver": "key_toggle", "key": "F6"},
    "rewind":       {"driver": "key_hold", "key": "F7"},
    "quit":         {"driver": "exit"}
  },
  "provision": [
    {"file": "${appmedia}/files/settings.ini", "format": "ini", "section": "Hotkeys", "mode": "add", "multi": "repeat",
     "values": {"SaveSelectedSaveState": "Keyboard/F2", "LoadSelectedSaveState": "Keyboard/F4",
                "ToggleFastForward": "Keyboard/F6", "Rewind": "Keyboard/F7"}}
  ],
  "controls": {
    "console": "psx",
    "defaults_convention": "android_xbox",
    "players": "unsupported",
    "read": {"file": "${appmedia}/files/settings.ini", "format": "ini", "section": "Pad1", "multi": "repeat",
             "absent": "not_mapped", "not_mapped_where": "DuckStation > Settings > Controllers",
             "codecs": [{"pattern": "SDL-{n}/{sdl}", "input": "sdl"},
                        {"pattern": "{dev}/Button{key}", "input": "key"},
                        {"pattern": "{dev}/{sign}Axis{axis}", "input": "axis"},
                        {"pattern": "Keyboard/{name}", "input": "offpad"}],
             "chord": " & "},
    "binds": {"btn-cross": "Cross", "btn-circle": "Circle", "btn-square": "Square", "btn-triangle": "Triangle",
              "dpad-up": "Up", "dpad-down": "Down", "dpad-left": "Left", "dpad-right": "Right",
              "btn-l1": "L1", "btn-r1": "R1", "btn-l2": "L2", "btn-r2": "R2", "btn-l3": "L3", "btn-r3": "R3",
              "btn-start": "Start", "btn-select": "Select",
              "stick-l:left": "LLeft", "stick-l:right": "LRight", "stick-l:up": "LUp", "stick-l:down": "LDown",
              "stick-r:left": "RLeft", "stick-r:right": "RRight", "stick-r:up": "RUp", "stick-r:down": "RDown"}
  },
  "files": {
    "live": {"android": "local"},
    "after": {"android": "foreground"},
    "settings": [{"base": "data", "rel": "shared_prefs", "dir": true},
                 {"base": "media", "rel": "settings.ini"}],
    "shared": [{"base": "media", "rel": "memcards",    "class": "saves",   "card": "memcards",    "point": "link"},
               {"base": "media", "rel": "savestates",  "class": "states",  "card": "savestates",  "point": "link"},
               {"base": "media", "rel": "bios",        "class": "bios",    "card": "bios",        "point": "link"},
               {"base": "media", "rel": "covers",      "class": "content", "card": "covers",      "point": "link"},
               {"base": "media", "rel": "screenshots", "class": "content", "card": "screenshots", "point": "link"},
               {"base": "media", "rel": "cheats",      "class": "content", "card": "cheats",      "point": "link"}],
    "exclude": [{"base": "media", "glob": "cache/**"}, {"base": "media", "glob": "shaders/**"}]
  },
  "storage": {
    "card_folder": "duckstation",
    "all_files": false,
    "needs_card_to_start": true
  },
  "bios": [{"system": "psx", "dir": "${appmedia}/files/bios"}]
}
```

What each part rests on:

| Part | Basis | Status |
|---|---|---|
| Launch `bootPath`, `resumeState` | 2021 `EmulationActivity.java:250-254` ([81da9be](https://github.com/stenzek/duckstation/blob/81da9be2d1040665ebfaf2db6d7fdb710a48a383/android/app/src/main/java/com/github/stenzek/duckstation/EmulationActivity.java)); ES-DE and Daijisho still pass them (P:18188, P:88936) | Likely for Play; UNVERIFIED for the beta |
| `{file.uri}` over `{file.path}` | ES-DE parity (`%ROMSAF%`); LaunchBox moved to SAF for this app | UNVERIFIED; keep the catalog's `{file.path}` legacy player as an alternate |
| Replacing the catalog's `psx.com.github.stenzek.duckstation` | Origin `system`, and the replaced player names this package (section 3) | Allowed by the rule |
| Extensions | Daijisho regex; DuckStation README formats | `.7z`/`.zip` left out (UNVERIFIED that the app reads archives) |
| Hotkey names | `src/core/hotkeys.cpp` (`SaveSelectedSaveState` :700, `LoadSelectedSaveState` :693, `ToggleFastForward` :193, `Rewind` :240, `PowerOff` :208), section `Hotkeys` (`settings.cpp:179`) ([source](https://github.com/stenzek/duckstation/tree/master/src/core)) | Desktop core verified; Android use of `settings.ini` UNVERIFIED |
| Repeated keys for several binds | `ini_settings_interface.cpp:575-590` | Verified in desktop source; needs `multi: repeat` (new) |
| Play-line settings | 2021 app used default SharedPreferences, keys `Hotkeys/<Name>`, values `<descriptor>/Button<keycode>` (`HotkeyInfo.java:27`, `ControllerBindingDialog.java:76-82`) | UNVERIFIED for 0.1-89xx; would need an `xml_prefs` rule with the virtual keyboard's descriptor |
| Keyboard events accepted for binding | 2021 `EmulationSurfaceView.isBindableDevice` accepts `SOURCE_CLASS_BUTTON` devices, keyboards included (`:28-36`) | UNVERIFIED for current builds and for device -1 (Phase 0 step 6) |
| Pad section `Pad1`, bind names | `settings.cpp` ~212-219, `analog_controller.cpp:766-791` | Desktop verified; Android prefix (SDL or device) UNVERIFIED |
| Default pad convention | 2021 `ControllerAutoMapper.java:209-219` (Cross on `BUTTON_A`) | UNVERIFIED for current builds; the beta uses SDL's controller database (beta page) |
| `players: unsupported` | No evidence either way for multitap or port assignment on Android | UNVERIFIED (Phase 0 step 7) |
| Storage folders | `Android/data/<pkg>/files/{memcards,savestates}` ([guide](https://joeysretrohandhelds.com/guides/duckstation-android-setup-guide/)) | Single hobbyist source; others UNVERIFIED |
| `files.settings` with `shared_prefs` | Origin `system`, so `base: "data"` is allowed for `shared_prefs`; it goes into each device's own set (per_device_config_plan.md §2.2) | Needs the confirm-screen line |
| `files.shared` folders, `files.exclude` | OA-52, OA-53: memory cards, states and BIOS shared on the card, one copy; caches and shaders never synced | Folder names UNVERIFIED (Phase 0 step 4) |
| BIOS folder `files/bios` | Desktop DuckStation's `bios` folder in its data dir | UNVERIFIED on Android (Phase 0 step 9) |
| Beta data relocation | Beta page: data locations per category, document-provider folders allowed | UNVERIFIED whether Nano can set it without the app's UI |
| Quit | No documented exit intent; `exit` driver | D7 |
| Load key F4 vs F1 | Desktop defaults F1 load, F2 save, F4 next slot (`hotkeys.cpp:48-51`) | D6 |

1.4.4 vs 1.2 for DuckStation:
- **1.4.4:** everything above.
- **1.2:** pick "DuckStation (Standalone)" in ES-DE's alternative emulators for PlayStation. No hotkeys beyond the app's own, no button map, no card move, no BIOS install. README says so. Don't set it as a build default in `mods/esde-emulators.txt`: the build wouldn't catch that it isn't installed.
