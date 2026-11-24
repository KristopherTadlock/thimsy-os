# Android launcher plan: Nano as an installable home app, sharing its library with Thimsy OS

Status: draft for review (2026-11-24). Not started. Research only: nothing here has been built or run on a device. Facts about stock Android's behaviour that this plan leans on and that were not checked on a device are marked **(verify)** and collected in Phase 0.

Working name in this document: **Thimsy Home** (the app), against **Thimsy OS** (the firmware). The name and package id are decision D2.

## Requirement (user, 2026-11-24)

- Port the Nano build's features to an Android launcher for Android gaming handhelds (Anbernic, AYN, Retroid Pocket), keeping as many features as possible.
- The exported game lists and emulator profiles (through `ES-DE` and `emulator_config`) are interoperable between Thimsy OS as custom firmware and the Android launcher.
- The launcher can be installed as Android's main launcher (the home app).
- Thimsy's UI and navigation are kept where possible.

## Conventions

- **Firmware** = Thimsy OS on the 1.4.4 base, where Nano is `/system/bin/gammaos-nano`. **App** = Thimsy Home, an APK on any Android.
- **Store mode** = Nano keeping its library in an `ES-DE` folder in ES-DE's own files (`NanoEsdeStore.cpp`), as it does today after **Move ES-DE to microSD**. **Internal mode** = Nano keeping it in `/data/system/nano_*`.
- **Access levels** used in the feature tables:

  | Level | What the app has | How the user gets it |
  |---|---|---|
  | **P** plain | Home role, All files access, a folder grant on the ROM folders | Two or three taps in the app's setup |
  | **G** grants | P, plus Accessibility service, Display over other apps, Usage access, Notification access, Modify system settings | Android's own Settings screens, or once over USB by the installer (`pm grant`, `appops set`, `WRITE_SECURE_SETTINGS`); these survive a restart |
  | **S** shell | G, plus commands run as Android's shell user | Shizuku (or its equivalent); without root it must be started again after every restart |
  | **R** root | Everything a root shell can do | Magisk or KernelSU, or a vendor root switch where the stock firmware has one **(verify per vendor)** |
  | **F** firmware only | Needs Thimsy's patched framework, SurfaceFlinger, init or kernel pieces | Install Thimsy OS |

## Where things stand

### Nano is not an app, and cannot become one by repackaging

- **One native C++ process, running as root** in the `bootanim` SELinux domain (`system/sepolicy/private/file_contexts`, `bootanim.te` in the patch). There is no Java in Nano.
- **It draws with EGL and OpenGL ES 2.0**, straight to DRM/KMS or on a raw SurfaceFlinger layer through private `libgui` (`NanoMenuDrm.cpp`, `NanoMenu.cpp`, `NanoDisplayTargetSf.h`). Text is FreeType into a glyph atlas; images are AImageDecoder, stb_image, libpng and nanosvg; video and music are the media NDK; sound is AAudio.
- **It reads raw evdev** (`/dev/input/*`, `EVIOCGRAB`, `EVIOCGKEY`) for buttons and touch (`NanoMenuInput.cpp`), never Android's input events.
- **It launches by handing off to the framework.** The home writes `sys.gammaos.nano.launch_app` and the intent to `/data/system/nano_launch_intent.txt`, then exits; a patched `RootWindowContainer` starts the app. A second instance (`gammaos-nano-overlay`) sits on a top SurfaceFlinger layer over the game for the Quick Menu, and runs `am start` and `am force-stop` as root.
- **About 250 `sys.gammaos.*` and `persist.gammaos.*` properties** are its settings store and its message bus to system_server, gammapad, SurfaceFlinger and init.
- **Its state is in `/data/system/nano_*`**, which no app can read.
- **Size:** the patch alone holds 69 new source files (27,600 lines) and changes 71 more (+19,800 lines); upstream's own `docs/ARCHITECTURE.md` says about 120 source files. Most screens are methods of one class, `NanoMenu` (its header's diff alone is 2,500 lines), and every theme shares the XMB's model and input chains, so no theme or screen lifts out on its own.

### What does carry over

- **The rendering approach.** GLES 2.0, FreeType, libpng, stb, nanosvg, tinyxml2, AImageDecoder, AMediaCodec and AAudio are all in the NDK or can be built into an APK. Nano already has a second display backend (SurfaceFlinger beside DRM) and sizes everything from the active mode, from 640x480 to 1080p and the Cube's 1:1.
- **The self-contained pieces**, most with host tests that already build with `g++` (`tests/unit/test_nano_units.py`): `NanoLaunch` (the launch template builder), `NanoTouch`, `NanoBidi`, `NanoUtf8Case`, `NanoJson`, `NanoArchive`, `NanoFileJob`, `NanoEsdeSystemName`, `NanoSystemsHeal`, `NanoScummvmList`, `NanoRaConfig`, `NanoBiosCatalog`, `NanoBiosUtil`, `NanoBiosWorker`, `NanoButtonMap*`, `NanoScrapeQueue`, `NanoScrapeWorker`, `NanoDeviceKeys`, `NanoHotkeys`, `NanoEmuSync`.
- **The Java hotkey logic.** `ThimsyHotkeyController` and `ThimsyDeviceKeys` are plain Java with host tests against stubs (`tests/fixtures/hotkeys`, `tests/fixtures/device_keys`), so they can compile into an APK too.
- **The data files:** the emulator catalog (`ps3xmb/daijishou/*.json`, MIT), the ES-DE emulator table (`NanoEsdeEmulators.inc`), the hotkey profiles (`mods/hotkeys/*.json`), `mods/device-keys.json`, the 21 translations, the BIOS catalogue and the ES-DE theme with its console pictures.
- **The licence.** GammaOS Next's source is Apache-2.0 (its repository's LICENSE), and Thimsy's own Nano files carry Apache-2.0 headers. Phase 0 confirms there is no separate statement for the `gammaos-nano` directory and audits the assets (see Risks).

### The library is already nearly portable, in store mode only

After **Move ES-DE to microSD**, Nano's library is a folder mainline ES-DE can use (README, "Move ES-DE to microSD"; `NanoEsdeStore.cpp`):

| File | Holds | Path form inside |
|---|---|---|
| `ES-DE/gamelists/<system>/gamelist.xml` | favourites, hidden, titles, metadata, `playcount`, `lastplayed`, a game's emulator (`<altemulator>`), a system's (`<alternativeEmulator>`) | relative, `./sub/Game.zip` |
| `ES-DE/collections/custom-<name>.cfg` | collections (those `CollectionSystemsCustom` lists) | `%ROMPATH%/<system>/<rel>`, else absolute |
| `ES-DE/settings/es_settings.xml` | `ROMDirectory`, the scraper keys | absolute; Nano re-roots a missing volume |
| `ES-DE/custom_systems/es_systems.xml` | systems in folders ES-DE has no system for (write-only, append-only) | `%ROMPATH%/<folder>` |
| `ES-DE/downloaded_media/<system>/<type>/<name>.<ext>` | covers, fan art, screenshots, manuals, videos | by name |
| `ES-DE/nano/systems.json` | Nano's systems: folders, extensions, `launchType`, `coreSo`, `intentTemplate`, icon | scan sources with the card id as `%CARDID%` |
| `ES-DE/nano/emulators.json` | per-game emulators ES-DE has no label for | `"<folder>/<rel>"` |
| `ES-DE/nano/names.json`, `nano/index.json` | title overrides, the scrape index | **absolute ROM and picture paths, tied to the volume id** |

- **In internal mode none of this is reachable.** Favourites, hidden games, collections, per-game emulators and Recently Played are in `/data/system`. Nano still reads ES-DE gamelists and media in internal mode, so a library made elsewhere shows up, but nothing Nano changes goes back.
- **Store mode is only entered by a move to a card.** `persist.gammaos.nano.esde.dir` is set in `fileJobRelocate` (`NanoSdMove.cpp`) and nowhere else.
- **Nano never reads `custom_systems`** or `es_find_rules.xml` (also noted in `user_emulators_plan.md`).

### Launch templates are already Android intents

- A system or game stores an `am start` argument string with `{file.uri}`, `{file.path}`, `{file.mime}`, `{file.content}`, `{file.storagepath}` and `{file.storagedir}` (`NanoLaunch.h`). `{file.uri}` is a Storage Access Framework document URI, `content://com.android.externalstorage.documents/tree/primary%3AROMs%2Fpsx/document/...`, the same form ES-DE's `%ROMSAF%` gives.
- RetroArch systems store only a core name (`launchType: libretro-core`, `coreSo`), and the framework adds RetroArch's `ROM`, `LIBRETRO`, `CONFIGFILE`, `DATADIR`, `SDCARD` and `EXTERNAL` extras.
- **What the firmware adds that stock Android will not:** a root `am` can start activities that are not exported, the patched framework hands out content URIs without the sender holding a grant, and `NanoPermGrantBridge` and `uri_grant` give emulators their storage permissions and folder grants. On stock Android the app must hold a folder grant itself and pass `FLAG_GRANT_READ_URI_PERMISSION`, as ES-DE does, or pass a file path to an emulator the user gave All files access.

### `emulator_config` is portable as files, not as behaviour

- **Move Emulator Files to microSD** puts each emulator's settings, saves and states in `<card>/emulator_config/<emulator>/` and points the emulator there (`NanoSdMove.cpp`; README; BIOS.md). The same card keeps its volume id in any device, so paths such as `/storage/1234-ABCD/emulator_config/retroarch` stay valid on stock Android.
- **How each emulator is pointed there decides what the app can do:**

  | Emulator | Pointed by | On stock Android |
  |---|---|---|
  | RetroArch | paths in `retroarch.cfg` (in `Android/data`), `CONFIGFILE` extra at launch | The app can pass the card's `retroarch.cfg` as `CONFIGFILE` at launch (P), if RetroArch has All files access |
  | PPSSPP | `memstick_dir.txt` in its private data | user sets the memory stick folder in PPSSPP; R to write it |
  | Flycast, ARMSX2, M64Plus FZ, DraStic | Android preferences in private data, plus ini files | user sets the folder in the emulator; R to write it |
  | ScummVM | `savepath` in `scummvm.ini` (private data) | user sets it; R to write it |
  | Vita3K | `config.yml` (in `Android/data`) | user sets it; S to write it |
  | Azahar, SeedlessDS | a folder grant made on the app's behalf through system_server | user picks the folder in the emulator's own picker |
  | Dolphin, hakuX | the folder replaced by a symlink to the card | **no folder setting**: R only |
  | WatermelonDS | nothing moves | nothing to do |

- **Keep Emulator Files on microSD** also mirrors each emulator's Android preferences and controller files to `emulator_config/<emulator>/app/` (`NanoEmuSync.cpp`). Putting those back into an emulator is writing its private data: R only.

### Emulator profiles today

- A **profile** is one JSON file per emulator (`mods/hotkeys/*.json`, installed to `/system/etc/gammaos/hotkeys`, user overrides in `/data/system/gammaos/hotkeys`): the packages it covers, how each universal hotkey is carried out (`key`, `key_toggle`, `key_hold`, `retroarch_cmd`, `intent`, `exit`, `force_stop`, `none`), and `provision` rules that bind the keys in the emulator's own settings files (HOTKEYS.md).
- `user_emulators_plan.md` (not started) grows that file into an **emulator definition**: signers, players (launch templates), controls, storage (the `emulator_config` rules above, as data), and BIOS slots, importable from `/sdcard/Thimsy/emulators/*.json`.
- Chords are detected in system_server (`PhoneWindowManager`) and keys are injected from there. Neither exists on stock Android.

### The installer has no path to a running Android

- The installer (branch `universal-installer`) is a Flutter app over a Rust engine. It flashes only through Unisoc download mode (`spd_dump`); it speaks no ADB. `detect.rs` recognises a running RG Cube by its USB product string only.
- `pack.json` format 1 can only describe a PAC and super-image rebuild (`system`, `source_sha256` and `reference_sha256` are required; unknown fields are refused).
- `universal_installer.md` already designs an `adb:` action namespace and an `adb/` crate (§4.2, §5.1) and an Android host (§9). None of it is built.
- The catalog has no AYN device, and from Retroid only the RP4 Pro and RP Classic.

## Design

### 1. One Nano source, two platforms

**Decided (D1, user, 2026-11-24): keep Nano's C++ as the single UI code base, and put a platform layer under it.** The firmware build links the layer that does what Nano does today; the app build links one that calls Android through JNI. A rewrite in Kotlin would have to redo the ES-DE theme engine, the XMB, the on-screen keyboard, the settings tree, the scraper and 21 languages of screens, and the two would drift apart from the first release. The requirement to keep Thimsy's UI and navigation is met by running the same drawing and input code.

```
                    NanoMenu (themes, screens, lists, OSK, scraper, store)   <- unchanged logic
                                        |
                                 NanoPlatform.h                              <- new seam
              +-------------------------+--------------------------+
   platform/firmware (today's code)                  platform/android (new)
   DRM / SurfaceFlinger layer                        ANativeWindow of the home activity / overlay window
   raw evdev                                         KeyEvent / MotionEvent from Kotlin
   property_get / property_set                       a settings store in the app's files
   launch properties, root `am`                      startActivity / PackageManager
   system(), /data/system                            capability providers (P, G, S, R)
```

The seam, by area (each is a small interface; the firmware side is a move of existing code, with no change in behaviour):

| Area | Firmware implementation | App implementation |
|---|---|---|
| Display | DRM, `SurfaceComposerClient` layer, `followDisplay0` | EGL on the activity's `ANativeWindow`; a second surface for the overlay window |
| Input | evdev readers, `NanoAbxy`, `NanoDeviceKeys` by evdev code | Android key codes and axes mapped onto the same internal buttons; touch already in screen orientation |
| Settings | `persist.gammaos.nano.*` properties | key/value file in the app's data, same key names |
| State files | `/data/system/nano_*` | the app's files directory; the library always in store mode (§3) |
| Launch | launch properties and the intent file; root `am start` | `Intent` built from the same tokens (§4) |
| App list, foreground app | `packages.list`, `dumpsys activity` | `PackageManager`, `LauncherApps`; Usage access or the Accessibility service |
| Other apps' files | direct, as root | a provider chain: shared storage (P), shell (S), root (R); a feature asks "can I read/write this path" and degrades by the answer |
| System actions | `am`, `cmd`, `settings`, `svc`, `reboot`, sysfs | Android APIs where one exists; shell or root where not; otherwise the row is hidden |
| Network | libcurl, `/system/bin/curl` | libcurl built into the APK |
| Assets | `/system/etc/nano_xmb`, `/system/etc/gammaos-nano/i18n` | APK assets, unpacked on first start |

- **Where the code lives (D3).** Nano's source exists only as GammaOS's tree plus `patches/thimsy/0001-thimsy.patch`. The app build fetches `frameworks/base/cmds/gammaos-nano` at the pinned base commit, applies the patch's hunks for that directory, and builds it with the NDK and CMake outside AOSP. The seam itself lands in the patch, so the firmware and the app always build from the same source.
- **The Kotlin shell** holds everything Android wants in Java form: the home activity, the Accessibility service, the overlay window, the folder picker, permission screens, the notification listener, a foreground service for scraping, and (later) the keyboard.
- **Rows that cannot work are hidden, not greyed,** as Slide Behaviour is on the Cube (`ro.gammaos.nano.no_slide`). Each settings row declares the capability it needs.

### 2. The home app

- **Manifest:** one activity with `MAIN`, `HOME` and `DEFAULT`, `singleTask`, excluded from recents, handling its own configuration changes, immersive. The app asks for the home role with `RoleManager.ROLE_HOME`; the installer can set it over USB (`cmd role add-role-holder android.app.role.HOME`).
- **Home key.** As the home app, a press of Home while a game runs brings Thimsy Home forward with no extra permission. That is the plain-level Quick Menu: it opens with **Resume <game>** first. This matches the firmware's rule for devices whose function button is `HOME` (HOTKEYS.md, "The function button is `HOME`").
- **Never locked out.** ES-DE's own Android notes warn about this for a home app. Settings always has **Android Settings** and **Change Home App**, reachable by touch and by controller, and they need no permission.
- **Boot.** Android starts the home app itself; Nano's boot screen, chime and Quick Resume preview do not apply. Quick Resume becomes "start the last game again when the home first shows" (P), with the resume itself left to the emulator's own auto-load setting.
- **System bars and notifications.** The status bar stays hidden in the home; the Quick Menu's Notifications row opens Android's shade (P) or lists them itself with Notification access (G).
- **While not in front** the app stops drawing and trims its texture caches (`onTrimMemory`), so it costs a game nothing.
- **Also Thimsy OS's Android-mode home (D11).** The same APK can replace Trebuchet under **Boot Android**, which also gives a test device today: the RG Cube in Android mode.

### 3. The shared library: one format, on both sides

**The contract is the `ES-DE` folder in store mode, the `ROMs` layout beside it, and `emulator_config`.** The app has no other store: it always runs in store mode, on internal storage (`/storage/emulated/0/ES-DE`) or on a card.

Changes that make it a real interchange format:

1. **Write it down.** A new `LIBRARY-FORMAT.md` specifies every file in the table above, with a `nano/format.json` (`{"version": 2}`) so each side can refuse or convert what it does not know. The spec, not either implementation, is what tests are written against.
2. **Store mode on internal storage, in the firmware (D7, recommended as the default for new installs).** Today only a card gets store mode, so a Thimsy device without a card has nothing to export. `persist.gammaos.nano.esde.dir=/storage/emulated/0/ES-DE` makes the internal folder the store. This is the same switch `usb_storage_plan.md` plans as **ES-DE Drive = Internal Storage**; the two must land as one design. **(verify)** that `esdeStoreFollowCard`, the sweep and the move code accept the internal volume.
3. **No volume-bound paths in `nano/`.** `names.json` and `index.json` move to the forms the other files already use: `"<folder>/<rel>"` for a game under the ROM folder and `%CARDID%` for anything else on the store's own volume. Version 1 files are read and converted on the first save.
4. **`nano/systems.json` stays as it is.** The app reads `launchType`, `coreSo` and `intentTemplate` unchanged (§4). A system the reader cannot launch on this device (an app that is not installed) is shown with **Install** or **Choose Emulator**, and its entry is kept as written, as the firmware keeps a card's systems it cannot validate (`nano_esde_kept_systems.json`).
5. **Path spellings.** The app only ever sees `/storage/emulated/0` and `/storage/<id>`; the firmware also sees `/sdcard`, `/data/media/0` and `/mnt/media_rw/<id>`. `storagePath()` (`NanoLaunch.h`) and `sameFile` already normalise these; the spec fixes the stored spelling to `/storage/...`.
6. **Android Games.** `ROMs/androidgames/<name>.app` holds one line, the package name, on both sides (ES-DE uses the same `.app` idea). The app does the helper APK's job itself (list the installed games, keep the files and their icons in `downloaded_media/androidgames`) and starts a game with `getLaunchIntentForPackage`.
7. **ScummVM.** The firmware lists games from ScummVM's private `scummvm.ini`; the app cannot read it below R. It falls back to `ROMs/scummvm/*.scummvm` files (first line = the game's ID, launched through `{file.content}`), which is ES-DE's own convention and what the pack-in already ships.
8. **Emulator profiles.** The emulator definition of `user_emulators_plan.md` becomes the shared profile format. The built-in set ships in both the image and the APK; the user's own live in `Thimsy/emulators/*.json` on shared storage (the import folder that plan already names) so they travel with the card. The app reads the same `actions`, `provision`, `players`, `storage` and `bios` blocks and applies each rule only at the access level it needs.
9. **A library without a card (D12, optional).** **Export Library...** and **Import Library...** on both sides write and read one zip of `ES-DE` (without media unless asked), `nano/` and `Thimsy/emulators`, for moving between two devices' internal storage. No ROMs, no BIOS files, no scraper accounts.

What stays outside the contract, and why: `/data/system` state (internal mode), system properties, `emulator_config/<emulator>/app/` mirrors below R, the Quick Resume state, and Nano's built-in DS player data.

**Direction of travel, once the above is in:**

| From | To | What the user does | What arrives |
|---|---|---|---|
| Thimsy OS, card | Thimsy Home | move the card | games, favourites, hidden, titles, collections, per-game and per-system emulators, play counts, media, systems; emulator saves and settings for emulators that take a folder |
| Thimsy Home, card | Thimsy OS | move the card | the same; Thimsy OS adopts the card as it does today (`emuSdTick`, the foreign-card dialog) and also restores `app/` mirrors if any exist |
| Either, internal storage | the other | Export Library / Import Library, or copy the `ES-DE` folder over USB | the library; emulator files only where the user copies `emulator_config` too |
| Either | mainline ES-DE | point ES-DE at the folder | everything ES-DE has a field for; `nano/` is ignored |

### 4. Launching games on stock Android

- **Same templates, same builder.** `buildLaunchIntent` (`NanoLaunch.cpp`, host-tested) turns a template into tokens on both sides. The app then turns the tokens into an `Intent` in Kotlin, with the grammar the framework's `parseAmIntent` accepts (`-n`, `-a`, `-c`, `-d`, `-t`, `-e`/`--es`, `--ez`, `--ei`, `--esa`, `--activity-*`, quoted argument strings). One table of cases tests both parsers.
- **RetroArch.** `libretro-core` systems start `RetroActivityFuture` with `ROM`, `LIBRETRO` and `CONFIGFILE`, as ES-DE and Daijisho do. The package is whichever RetroArch is installed (`com.retroarch.aarch64`, `com.retroarch`, `com.retroarch.ra32`), and the core path follows it. `CONFIGFILE` is the card's `emulator_config/retroarch/retroarch.cfg` when it exists (`raLaunchConfig`'s rule), else RetroArch's default. A core RetroArch does not have yet is reported before the launch ("Download Mesen in RetroArch: Online Updater").
- **`{file.uri}`.** Setup asks for a folder grant on each ROM root (`ACTION_OPEN_DOCUMENT_TREE`, persisted). The URI is built under the tree the app actually holds and passed with `FLAG_GRANT_READ_URI_PERMISSION`. Some emulators only open a file inside a folder they were given themselves; those get a one-time "Give <emulator> this folder" step **(verify per emulator: Phase 0 launch matrix)**.
- **Activities that are not exported** cannot be started below S. The launch matrix lists any pack-in template that depends on one and gives it an exported alternative from the ES-DE table.
- **ES-DE's commands as a second source.** `NanoEsdeEmulators.inc` already holds every ES-DE Android command with its package keys. The app adds an interpreter for that grammar (`%EMULATOR_x%`, `%ACTION%`, `%DATA%`, `%EXTRA_*%`, `%ROM%`, `%ROMSAF%`, `%ACTIVITY_*%`, `%ANDROIDAPP%`), so an `<altemulator>` label with no Nano player still launches, and so the app reads `custom_systems/es_systems.xml` and `es_find_rules.xml` that the user or ES-DE wrote. The firmware gains the same reader (it closes the gap `user_emulators_plan.md` records under "Launch from ES-DE").
- **Closing a game.** Home (P); the Accessibility service's Back or Home action (G); `am force-stop` (S).

### 5. Over a game: Quick Menu, chords and hotkeys

| Piece | Firmware | App |
|---|---|---|
| Seeing the function button and chords over a game | `PhoneWindowManager` | the Accessibility service's key filter (G). Gamepad buttons arrive as key events **(verify per device)**; the D-pad and analogue triggers arrive as axes on many pads and are not seen, so on those pads L2/R2 chords need the pad's key form or are remapped |
| The chord rules | `ThimsyHotkeyController`, `ThimsyDeviceKeys` | the same two classes, compiled into the APK, fed from the key filter |
| Quick Menu, Applications and the button map drawn over the running game | the overlay process on a top SurfaceFlinger layer | a `TYPE_APPLICATION_OVERLAY` window drawn by the same code (G). Being focusable, it takes the controller away from the game, as `drop_input` does. Without the grant: Home brings the launcher forward instead (P) |
| Which app is in front | properties, `dumpsys` | the Accessibility service's window events, or Usage access (G) |
| Save, load, fast forward, rewind | key injection from system_server, `retroarch_cmd`, intents | `retroarch_cmd` (UDP to 127.0.0.1:55355) and `intent` drivers work at P once the emulator's side is set; `key` drivers need injection: S or R. An input-method route for `key` drivers is a Phase 0 spike **(verify)** |
| Binding the keys in the emulators' files (`provision`) | Nano, as root | by where the file is: shared storage (P), `Android/data` (S), private data (R). Where it cannot write, **Hotkeys** shows the binding for the user to set by hand, from the same profile |
| Quit | the long-press Back path, force-stop | Back or Home through the Accessibility service (G); force-stop (S) |
| Brightness chord | backlight | `WRITE_SETTINGS` (G) |
| Shader on/off chord | SurfaceFlinger | F |

On Android 13 and later an app installed from a downloaded file is refused Accessibility and Notification access until the user allows "restricted settings" for it; an install over ADB or from an app store's session is not. The installer route (§7) avoids the detour, and setup explains it when it applies.

### 6. Feature map

The level is the least access at which the feature works as on the firmware; "part" means a reduced form, described in the note.

| Feature | Level | Note |
|---|---|---|
| ES-DE theme home, system and game views, clock and status icons | P | same engine; themes from `ES-DE/themes` |
| Minima settings look, Background Colour, Navigation Sounds | P | |
| XMB and DSi themes | P | after `plan_remove_unlicensed_files.md` replaces the PS3 font, sounds and icons and the DSi font and sprites (D8) |
| Controller navigation, touch (taps, drags, flings, swipes, two- and three-finger taps in Nano's own screens) | P | `NanoTouch` fed from `MotionEvent` |
| Game systems, Game Systems editor, emulator picker, Scan Folders, Rescan | P | reads ROM folders with All files access |
| Favourites, hidden games, collections, Recently Played, titles, Information, Search Games | P | store mode |
| Emulator for This Game / for a system | P | |
| Set Boxart, Delete Game, Rename | P | |
| Boxart Scraper, background scraping, queue | P | a foreground service in place of `gammaos-nano-scrape` |
| Applications (list and Minima grid), app options, Android Games | P | |
| On-screen keyboard inside Nano (controller and touch forms), 21 languages | P | |
| Nano Keyboard for other apps | P, later | an input method shipped in the APK that draws Nano's keyboard itself; the user selects it |
| Setup wizard | part | language, folders, grants, emulators found, BIOS; no Wi-Fi, time zone or first-boot installs (Android's own setup did those) |
| Quick Menu in the home | P | rows by capability |
| Quick Menu, Applications, button map over a game | G | §5 |
| Universal hotkeys | G to R | §5, per emulator and driver |
| Button map | part | the drawing, labels and defaults at P; live bindings by where each emulator keeps them: shared storage P, `Android/data` S, private data R (`NanoButtonMapReaders`) |
| Close Current App | G | force-stop at S |
| Quick Resume | part | last game restarted at home start; no preview |
| Install Your Own BIOS Files | part | recognises files at P; installs to shared-storage targets at P, `Android/data` at S, private data at R; otherwise says where to put each file |
| Move ES-DE to a card / choose the library's location | P | |
| Move Emulator Files to microSD | part | guided at P ("open PPSSPP, set this folder"), automatic where the setting's file is reachable (S, R); Dolphin and hakuX links R only |
| Keep Emulator Files on microSD (settings sync) | R | the `app/` mirrors are left untouched below R |
| File Explorer, archives | P | shared storage and cards; not other apps' `Android/data` below S |
| Music, video, photos, PDF, Internet Radio, IPTV | P, later | pdfium built in, or Android's `PdfRenderer` |
| Network Shares | part, later | an in-app SMB client for the players and File Explorer; no system-wide mount |
| Wi-Fi and Bluetooth rows | part | open Android's panels; On/Off at S |
| Screen Brightness | G | |
| Date and time, language, IME choice, USB mode | part | open the matching Android Settings screen |
| Kiosk Mode | part | Start opens Applications and the menus are locked; Android's own gestures and shade are still there |
| Power menu | part | Android's power dialog through the Accessibility service (G); no Boot Android / Boot Nano |
| Performance Mode, Fan Speed, RGB, EQ | F | device vendors' own apps keep these; a row can open the vendor's settings where an intent exists **(verify per vendor)** |
| Global Shaders, 4:3 and 8:7 Cropped | F | SurfaceFlinger. The RetroArch copies of the two presets can still be offered to RetroArch |
| External monitor modes, Dual Screen, Mirror, USB-C dock fix | F | Android's own mirroring applies on stock |
| A second built-in screen (AYN Thor, RG DS) | later | an activity on the second display (`setLaunchDisplayId`); the firmware's controller-focus pin is F |
| Controller Enable (gammapad), ABXY Swap for every app, browser controls | F | inside the app, face buttons follow the device table (§8) |
| Storage privacy rules, pre-granted emulator permissions | F | the installer can grant the emulators' permissions once over USB (§7) |
| System Update | replaced | an in-app check of the app's own releases |
| Power button 7 s shutdown, charging screen, boot logo and animation | F | |
| drastic-nano, in-process libretro runner | F | they load another app's private libraries |

### 7. Getting it onto a device

- **The APK alone** (downloaded and sideloaded, or through Obtainium or F-Droid, D5) gives level P, and G through Android's Settings screens.
- **The Thimsy installer gains a third journey: "Keep my Android: add Thimsy Home"** beside first-time and returning installs. It needs USB debugging, no unlock, and erases nothing:
  - **Engine:** a new `adb` module (the `adb/` crate `universal_installer.md` §5.1 already plans), ADB-interface detection in `detect.rs` confirmed by `getprop`, `adb` in the tool allow-list, a Windows driver case, a `Mode::Launcher` with a journaled fetch-and-verify job.
  - **Package:** `pack.json` `format: 2` with `kind: "launcher"`: members are APKs and data tars with sizes and hashes; typed operations `apk_install`, `push`, `grant`, `appop`, `set_home`. Installers older than this refuse format 2 cleanly today ("newer installer needed"). `multi_device_plan.md` also plans a format 2 (a shared system artifact): the launcher kind must be designed into that same format 2, not a second one.
  - **Catalog:** an optional `launcher:` block per device (minimum Android API, `getprop` identification, per-host support) and an `installable_launcher()` test beside `buildable()`; devices with no `source` at all (AYN Odin 2, Thor, Retroid Pocket 5, Mini and Flip 2) become catalog entries for this journey only.
  - **What it does on the device:** installs Thimsy Home and the profile's emulators (the redistributable ones in `mods/added-apps.txt`, by the same pinned URLs and hashes), gives each its storage permissions (`pm grant`, `appops set ... MANAGE_EXTERNAL_STORAGE allow`), gives Thimsy Home its G-level grants, sets the home role, and pushes the pack-in games with their gamelists and media into `ROMs` and `ES-DE` (`scripts/bundled_roms.py`'s output, which is already ES-DE files).
  - **What it cannot do** without root: RetroArch's cores and config payload in private data (the app points the user at RetroArch's Online Updater instead, or installs a config through RetroArch's own `CONFIGFILE` launch extra), the MaliSX2 driver preinstall, the hotkey binds in private data, keylayouts.
- **On-device setup without a computer** does the same from inside the app with Android's package installer: one confirmation per emulator.
- **Profiles.** `device_profiles_plan.md`'s Small, Medium, Large and Ultra decide the emulator set here too; the launcher journey reads the same profile files, and the chipset's Recommended mark applies unchanged.

### 8. Devices

- This widens Thimsy's reach beyond what GammaOS supports (no AYN device is on GammaOS's list), which fits the multi-device goal (`multi_device_plan.md`): nothing in the app may assume the RG Cube or a Unisoc chip. Device facts the app needs (buttons, face layout, screen shape, second screen) come from that plan's device descriptor (§4.2), extended with an app-side match, so a device is described once for the firmware, the installer and the app.
- **`mods/device-keys.json` gains an app-side match.** Today it matches by `ro.product.*` properties and gives the function and Back buttons as evdev and Android key codes. The app matches on `Build.MANUFACTURER`, `Build.MODEL`, `Build.DEVICE` and `Build.BOARD` and uses the `android` codes. Stock firmwares often keep the function button for the vendor's own panel; where it never reaches apps, the function button is Home (the default entry already lists it) or a chord the user picks.
- **Face buttons.** Inside the app, A/B/X/Y are read by position from Android's key codes, then labelled by the device's print (`face_layout`, the table `controller_layout_plan.md` Phase 1 introduces). The same table serves both sides.
- **Minimum Android (D4).** Recommended: Android 10 (API 29) so the first AYN Odin and Retroid Pocket 3+ are in; All files access begins at 11, so 10 uses the legacy storage flag. Test on 10, 11, 13 and 14 at least.

## Phases

**Phase 0: facts and spikes (no product code).**
1. Build Nano outside AOSP: fetch `gammaos-nano` at the base commit, apply the patch, compile for arm64 with the NDK against stubs. Output: a list of every call that is not NDK (`property_*`, `system()`, `libgui`, `libbinder`, evdev, tinyalsa, light HAL), by file. **The fetch, the patch and the list are done (2026-11-24, "Phase 0 results" below); a build that produces object files is still open.**
2. Render spike: the ES-DE theme's system view drawn in a plain activity, on the RG Cube under Boot Android and on an emulator image.
3. Launch matrix on stock Android 13: every pack-in emulator started from an ordinary app with its Nano template and with its ES-DE command. Record: exported or not, file path or folder grant, whether the emulator needs its own grant.
4. Input matrix per device family: which buttons the Accessibility key filter sees, what the function button sends on the stock firmware, Home's behaviour.
5. Hotkey drivers: `retroarch_cmd` from an app, key injection through Shizuku, the input-method route.
6. Firmware: `persist.gammaos.nano.esde.dir` set to the internal `ES-DE` folder on a 1.4.4 build; what breaks.
7. Licences: the `gammaos-nano` directory's terms, every asset the APK would carry (ASSETS.md, ATTRIBUTION.md), ES-DE's terms for the data `NanoEsdeEmulators.inc` is generated from, and the Thimsy logo's non-commercial licence against the chosen distribution.
8. Vendor root: whether AYN's, Retroid's and Anbernic's stock firmwares offer a root shell an app can use.
- Go/no-go after items 1-3: if the god-class cannot be cut along the seam in reasonable time, fall back to D1's alternative with the self-contained pieces as a library.

### Phase 0 results

**Item 1, the source and the list (2026-11-24).** Run on a PC in Docker; nothing was linked and nothing ran on a device.

- **How:** `tools/android-launcher/fetch_nano.sh` fetches only `frameworks/base/cmds/gammaos-nano` (about 100 MB) from GammaOSNextDistribution-14 at `0bc531e2d64698316f28cedc8992ffa0e44f1a76`, the 1.4.4 base (BACKPORTS.md), and applies the Nano part of the patch; `git apply` checks every file's base, and it applied without a reject. `tools/android-launcher/ndk_probe.py` then checks each source file of the `gammaos-nano` binary with the NDK's clang (r29, arm64, API 29, `-fsyntax-only`, the NDK's headers only) and writes `work/android-launcher/probe/REPORT.md` and `report.json` (per file: outside headers, missing names, privileged uses).
- **Size:** upstream's folder has 141 source and header files at its top level (162,854 lines); with the patch, 208 (202,699 lines). The binary itself is 114 source files, 156,985 lines.
- **Compiles unchanged:** 26 files, 9,304 lines (`NanoLaunch`, `NanoTouch`, `NanoJson`, `NanoI18n`, `NanoBidi`, `NanoEsdeSystemName`, `NanoSystemsHeal`, `NanoScummvmList`, `NanoRaConfig`, `NanoEmuSync`, `NanoAppFiles`, `NanoButtonMap`, `NanoMenuPS3Layout`, the demuxers and decoders, and others).
- **Blocked only by headers that are easy to supply:** 17 more files, 14,930 lines, need nothing but log macros, a few libutils types, FreeType, libpng, tinyxml2 or libcurl.
- **72 headers come from outside the NDK:**

  | What | Headers | Files that reach one |
  |---|---:|---:|
  | Log macros (`log/log.h`, `utils/Log.h`) | 2 | 71 |
  | System properties (`cutils/properties.h`, `android-base/properties.h`) | 2 | 59 |
  | libutils (`Thread`, `SystemClock`, `StrongPointer`, `Timers`, `String8`, `String16`) | 6 | 57 |
  | FreeType | 1 | 53 |
  | binder, AIDL and HIDL (`binder/IBinder.h`, light and health HALs) | 13 | 51 |
  | DRM/KMS | 3 | 18 |
  | SurfaceFlinger client (`gui/*`, `ui/*`) | 25 | 12 |
  | libpng, tinyxml2, OpenSSL, libziparchive, LZMA SDK, libcurl, pdfium | 11 | 8 or fewer each |
  | Other: libbase, libcutils, tinyalsa, GammaRGB, the share config, filesystem ids | 8 | 2 or fewer each |
  | `NanoScraperDevCreds.gen.h`, made by the build | 1 | 1 |

- **The platform headers spread through three of Nano's own headers, not through the code.** 59 files (122,049 lines) reach a display or binder header, but only 13 include one themselves. `NanoMenu.h` carries `binder/IBinder.h` into 51 files, `NanoMenuDrm.h` the DRM headers into 15, and `NanoVideo.h` the `gui` headers into 14. Cutting those three headers frees most of the tree.
- **2,739 uses of things an app may not do,** 63% of them in ten files (`NanoMenu.cpp` 352, `NanoMenuPS3Menu.cpp` 264, `NanoMenuOverlay.cpp` 201, `DrasticRunner.cpp` 194, `NanoMenuDrm.cpp` 163, `NanoMenuInput.cpp` 143, `NanoMenuXmb.cpp` 126, `NanoControlCenter.cpp` 104, `NanoSdMove.cpp` 97, `NanoMenuRender.cpp` 91). 30 files (13,045 lines) have none.

  | Kind | Uses | Files |
  |---|---:|---:|
  | Property read | 985 | 58 |
  | Property write | 654 | 39 |
  | `/data/system` path | 175 | 46 |
  | DRM ioctl | 149 | 5 |
  | Shell command (`system`, `popen`, `exec`) | 114 | 27 |
  | `/data/media` or `/mnt/media_rw` path | 114 | 23 |
  | `/system` or `/vendor` file | 109 | 30 |
  | SurfaceFlinger | 89 | 4 |
  | evdev | 71 | 7 |
  | `/sys` | 57 | 10 |
  | `/proc` | 51 | 22 |
  | Other `/data` path | 51 | 13 |
  | Another app's `/data/data` | 32 | 11 |
  | binder | 30 | 8 |
  | `/dev` | 27 | 10 |
  | `chown`, `chmod`, `mount` | 23 | 15 |
  | `fork` | 5 | 3 |
  | `symlink` | 3 | 2 |

**What this changes in the design:**

- **Properties become a link-time shim, not 1,639 edits.** The app supplies `property_get`, `property_set` and their typed forms with the same signatures over its settings store. Names Nano uses as messages to the framework (`sys.gammaos.nano.launch_app`, `show_overlay`, `net_cmd` and the like) are routed to the Kotlin shell as events by the same shim. This is 60% of the privileged uses with no call site touched.
- **The seam's first step is three header edits:** forward-declare `IBinder` in `NanoMenu.h`, and move the DRM and `gui` types out of `NanoMenuDrm.h` and `NanoVideo.h` behind the display interface.
- **Fixed paths get one root each.** The `/data/system` and `/system/etc` literals go through two functions (state root, asset root) that the firmware answers as today.
- **The hand work is the 114 shell commands in 27 files,** plus the display (DRM and SurfaceFlinger, 9 files) and evdev (7 files). Those are the platform layer's real surface.
- **Left out of the app build:** `DrasticRunner.cpp` and `DrasticGpu3d.cpp` (15,986 lines, 282 of the uses), `LibretroRunner.cpp`, `FakeJNI.cpp`, the boot chime and `NanoMenuDrm.cpp`: all firmware only (§6).
- **A new question, D13:** Nano compiles ScreenScraper developer credentials in (`gen_ss_creds.py` makes `NanoScraperDevCreds.gen.h`, obfuscated, never committed). An APK is far easier to take apart than a system image.
- **Go/no-go so far: go.** Nothing found says the class cannot be cut along the seam. The stronger test is still open: shim headers for the log macros, libutils and properties, FreeType, libpng and tinyxml2 from their sources, and a count of files that then compile to objects.

**Phase 1: the platform seam in Nano.** `NanoPlatform.h` and `platform/firmware`, area by area, with the firmware's behaviour unchanged (existing host tests, plus a new fake platform that runs screens headless). Lands in the patch.

**Phase 2: launcher MVP (level P).** The home activity and role; ES-DE theme with Minima settings; store-mode library on internal storage or a card; scan, list, search, favourites, collections, Recently Played, game options; launches for RetroArch and the standalone pack-ins; Applications and Android Games; on-screen keyboard; translations; setup (folders, grants, emulators found); Android Settings and Change Home App rows; the scraper as a foreground service.

**Phase 3: the library contract.** `LIBRARY-FORMAT.md`; `nano/format.json` and the portable `names.json` and `index.json`; store mode on internal storage in the firmware (with `usb_storage_plan.md`); the ES-DE command interpreter and `custom_systems` reader on both sides; `Thimsy/emulators` definitions (with `user_emulators_plan.md` Phase 1); Export Library and Import Library if D12 says so; round-trip tests.

**Phase 4: over a game (level G).** The Accessibility service and key filter with `ThimsyHotkeyController`; the overlay window (Quick Menu, Applications, button map); foreground tracking; Close Current App; brightness; the hotkey drivers that need no injection.

**Phase 5: emulator files.** The guided Move Emulator Files; the capability provider chain with Shizuku and root; hotkey provisioning and button-map readers by level; the BIOS installer by level; settings sync at R.

**Phase 6: the installer's launcher journey.** Engine `adb` module, package format 2, catalog entries, the app screens, protocol samples and tests; the on-device setup path.

**Phase 7: the rest.** XMB and DSi themes (after the asset replacements); the players; File Explorer; the in-app share client; Nano Keyboard as an input method; Kiosk Mode; second-screen devices; the in-app update check.

**Phase 8: Thimsy OS uses it too (if D11).** Thimsy Home as the Boot Android home in the image, in place of Trebuchet.

## Decisions for the user

- **D1** Approach. **Decided (user, 2026-11-24): one Nano source with a platform layer.** The alternative, a separate Kotlin launcher that only shares the data formats, stays only as the fallback of Phase 0's go/no-go.
- **D2** Name and package id. Working name Thimsy Home; `org.thimsy.home` would sit beside `org.thimsy.androidgames`.
- **D3** Where the app's code lives: a new `android-launcher` branch holding the Gradle project and the build that fetches and patches Nano (recommended), or inside `nano-144`.
- **D4** Lowest Android version: 10 (recommended), or 11.
- **D5** Distribution: GitHub releases with Obtainium and later F-Droid (recommended). Google Play would need All files access and Accessibility declarations approved, and the non-commercial assets keep it free either way.
- **D6** Access levels in scope: is Shizuku and root support wanted, or only P and G?
- **D7** Store mode on internal storage as the firmware's default for new installs (recommended), or opt-in.
- **D8** First themes: ES-DE with Minima settings only (recommended), with the XMB and DSi following the asset clean-up.
- **D9** Emulators: detect what is installed and offer the pack-in set as downloads (recommended), or detect only.
- **D10** Test devices: which stock AYN and Retroid devices are available? Until then: the RG Cube under Boot Android, and emulator images.
- **D11** Should Thimsy Home also become the Android-mode home on Thimsy OS?
- **D12** Is the card-less Export Library / Import Library wanted?
- **D13** The scraper's built-in ScreenScraper developer credentials in an APK: ship them as the image does, ask ScreenScraper for credentials registered to the app, or ship none and have each user sign in with their own account.

## Risks

- **The `NanoMenu` class.** Privileged calls are scattered through tens of thousands of lines that the patch only shows as diffs. Phase 0 item 1 sizes this before anything is promised.
- **Patch growth.** The seam touches most Nano files, which makes every GammaOS rebase harder. Keep the seam thin and mechanical, and offer it upstream.
- **Accessibility as a dependency.** Stock firmwares differ in what the key filter sees; Android keeps tightening sideloaded apps' access to it; some stores reject it. Level P must be a complete launcher on its own.
- **Folder grants and exported activities.** A template that works through the firmware's relaxed grants may not work on stock Android. The launch matrix is the gate for Phase 2's emulator list.
- **Two writers on one card.** The app and the firmware both edit gamelists in place and both merge. The sweep, the merge rule (the card's entries win) and atomic renames exist already; the contract must also say which side may delete.
- **Emulator settings that name a device.** A card's `retroarch.cfg` tuned for the Cube's square screen and Mali driver is wrong on a 16:9 Adreno device. The definition's `storage` block needs a way to mark device-bound settings; until then the app offers "Use the card's settings" per emulator, as the foreign-card dialog does.
- **Assets.** The PS3 font and sounds, the DSi font and sprites and the ES-DE controller badges are open items in `plan_remove_unlicensed_files.md`; an APK makes redistribution more visible than an image does. The theme and the Thimsy pictures are non-commercial licences.
- **Vendor launchers and key mappers** that refuse to give up Home or the function button on some stock firmwares.
- **RetroArch variants.** Store builds have no Online Updater and a different package; cores and paths differ.
- **Memory.** A home app stays resident; texture and atlas caches must be released when hidden, on the 3 GB and 4 GB devices especially.

## Tests

- **Host:** the existing Nano unit suites against the fake platform; `buildLaunchIntent` tokens to `Intent` against one shared case table (the firmware's `parseAmIntent` and the app's parser); the ES-DE command interpreter against `NanoEsdeEmulators.inc`.
- **Library contract:** a fixture library under `tests/fixtures/library` (gamelists, collections, settings, `nano/` in versions 1 and 2, `custom_systems`, media names). Both builds read it to the same model; each build's writes are read back by the other; a version 1 library converts without loss; a mainline ES-DE gamelist with unknown tags survives a write.
- **Instrumented (emulator images, API 29, 30, 33, 34 and the newest):** the home role; a stub "emulator" APK that records the intent it was started with, for every template; folder grants; the overlay window taking and returning controller focus; the key filter with a virtual gamepad.
- **Installer:** engine tests for format 2 packages and the launcher plan against a fake `adb` (as `tests/fake-tools/spd_dump` does for flashing); protocol samples.
- **Devices:** the launch matrix and input matrix from Phase 0 become the acceptance checklist per supported device.

## Docs

- `LIBRARY-FORMAT.md` (new): the contract.
- README: a "Thimsy Home" section with the feature map by access level; the Building section for the APK.
- HOTKEYS.md: the drivers and provisioning by access level; the app-side device match.
- `universal_installer.md`: the launcher journey, format 2, the catalog's `launcher:` block.
- `user_emulators_plan.md`, `usb_storage_plan.md`, `device_profiles_plan.md`, `controller_layout_plan.md`, `multi_device_plan.md`: a note in each on the part this plan shares with it (definitions as the shared profile, store mode on internal storage, profiles for the launcher journey, `face_layout`, the device descriptor and package format 2).
