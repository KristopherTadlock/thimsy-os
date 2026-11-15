# Button Map Overlay Plan

Status: phases 1-5 implemented (see "As built" under §6.2 and §9.4); phase 6 (art and polish) open.

Date: 2026-11-13

Trees: AOSP `~/gammaos-src` (GammaOS Next 1.4.4 base, branch `nano-144`). Build repo `~/thimsy`.

Paths:
- `nano/` means `~/gammaos-src/frameworks/base/cmds/gammaos-nano/`.
- `pwm/` means `~/gammaos-src/frameworks/base/services/core/java/com/android/server/policy/`.
- `gammapad/` means `~/gammaos-src/frameworks/native/services/gammapad/`.
- `drastic-nano/` means `~/gammaos-src/frameworks/base/cmds/drastic-nano/`.
- Line numbers are for the current tree.
- Emulator source references point at the exact shipped tag or commit, cloned read-only under `/tmp/bmap-*`.

---

## 0. Summary

The user asked for a chord that shows how the controller in hand maps onto the console being emulated, in Nano's menus and over a running game. It reads the active emulator's own binding files, follows the ABXY Swap setting, does nothing on the Android apps playlist and in most apps, and shows the browser controls when a browser is in front.

| Decision | Choice |
|---|---|
| Trigger | **Anbernic + the bottom face button**, defined by position (south). On the Cube that is the button printed **B**. It stays the bottom button whatever ABXY Swap, Controller Enable or the pad's key layout make it send. It is configurable as **Button Map Button** in Settings → Gamepad Settings → Hotkeys (`persist.gammaos.hotkeys.buttonmap`, default `FACE_SOUTH`). |
| Over a game | PhoneWindowManager's chord detector sets `sys.gammaos.nano.qs_summon=3` plus `show_overlay=1`, the same path as Anbernic + Start (`qs_summon=2`). It also records which device pressed it (`sys.gammaos.nano.bmap_dev`). |
| In Nano's menus | A new branch in Nano's own Anbernic handling, next to Anbernic + Select / Start (`nano/NanoMenuInput.cpp:2990-3008`). Anbernic + a button is already swallowed there, so the bottom button never also acts as Back or confirm. |
| drastic-nano | Its own chord detector draws the same picture through its `OverlayGfx`. Nano's overlay can't show during a DS session (`HOTKEYS.md`, Anbernic + Start). |
| What it shows | A drawing of the console's controller, with each console button labelled by the Cube button that presses it (printed names: A right, B bottom, X top, Y left). With no drawing it shows a two-column table. A second section lists the emulator's hotkeys from its hotkey profile. In a browser it shows the browser controls list. |
| Where it lives | A pure C++ module in Nano, `NanoButtonMap` (context, pad model, per-emulator readers), built into the unit tests. A CPU renderer, `NanoButtonMapArt` (nanosvg + FreeType, like `bootscreen/NanoBootArt`), draws the panel into an RGBA texture. The same pixels go to the screen and to the golden tests. |
| Layout | The layout is a pure function of the screen size and the content. On 16:9 and wider the panel is centred. On every other aspect (4:3, 1:1, 5:3, 16:10, portrait) it uses the full width. Text is sized from `min(W,H)`, with a 14 px floor so it stays legible at 640x480. |
| Tests | Readers: fixture configs → expected tables, in `gammaos_nano_unit_tests`. Layout and goldens: a new `gammaos_nano_buttonmap_tests` (gtest layout assertions) and a new `nano_buttonmap_golden_test` (an `sh_test_host` hashing frames, cloned from `nano_bootlogo_golden_test`). |

**What the mapping readers can and can't do** (details in §3):

| Reliable from config (with built-in defaults where config is absent) | Partial | Config only once the user maps |
|---|---|---|
| RetroArch (binds, autoconfig, remaps, per-core RetroPad table) · PPSSPP · Flycast · WatermelonDS · ARMSX2 · drastic-nano · M64Plus FZ (with an embedded copy of its built-in profiles) · SeedlessDS · hakuX | Dolphin (single-input expressions only, device qualifier must match) · ScummVM (engine-specific keymaps unknowable) · Vita3K (config exact, the Android→SDL step replicated) · DraStic app (closed source, profile 0 assumed) | Azahar and Dolphin: **no default pad bindings at all**. The overlay says "Not mapped in <emulator>" with where to map |

> §3 has the source evidence for each emulator at its shipped version. When the emulator's file is missing and it has built-in defaults, the overlay shows them and marks the panel "Default mapping".

**Browser controls:** these exist, but only through gammapad's `BrowserMode` (`gammapad/BrowserMode.cpp:277-398`). That means **Controller Enable** (`persist.gammaos.gamepad.enable`, default Off) and **Controller Bindings** must both be on. The README doesn't say Controller Enable is needed (`README.md:450-482`). The overlay shows the list either way and says what to turn on when the controls are inactive (§8).

**Correction to the brief:** Nano's menus do **not** use the bottom button as confirm by default. Nano confirms on evdev `BTN_SOUTH` (304) and goes back on `BTN_EAST` (305) (`nano/NanoMenuInput.cpp:3565-3566`, `3601-3604`). The Cube's driver reports buttons by print, so printed A (right) sends 304 and printed B (bottom) sends 305. That makes the bottom button **Back** in Nano, unless gammapad is running with ABXY Swap on (`nano/NanoMenuPS3Menu.cpp:10873-10881`). Either way, a button pressed while the Anbernic button is held is swallowed (`nano/NanoMenuInput.cpp:3010-3021`), so the chord never also confirms or goes back.

Phases (§9):
1. The pure core: pad model, context, readers for RetroArch and the standalones, with fixtures (host only).
2. Layout and CPU renderer, with the golden and layout tests (host only).
3. Nano integration: the menus chord, the overlay over games, theme palettes, settings, i18n.
4. The PhoneWindowManager chord, the device identity hand-off, and browser mode.
5. drastic-nano.
6. The controller-art catalogue hook-up and polish.

---

## 1. Trigger

Decided on 2026-11-13 (the open questions in §10):

- **Two chords.** Anbernic + the bottom button opens the map everywhere: over a game or app, and in Nano's own menus. Start + Select + the bottom button also opens it, but **only in Nano's own menus** (any theme, and the overlay raised as the launcher), never over a game, where it is a common in-game combination. `bmap::Trigger {kFunctionSouth, kStartSelectSouth}`, `bmap::Where {kNanoMenus, kOverGame}` and `bmap::triggerOpens(t, w)` hold the rule; unit-tested (`ButtonMapTrigger.*`).
- **No pause.** Over a game the map is the Quick Menu's kind of overlay: it takes the game's input while it is up, and the game keeps running (`bmap::kPausesGame = false`, `kBlocksGameInput = true`). That includes drastic-nano.
- **Gammapad is never turned on** by the map (`bmap::kTurnsOnGammapad = false`). In a browser with gammapad off the controls list is still shown, greyed, with one hint line saying what to turn on (§8).

### 1.1 Which keycode the bottom button sends

The Cube's driver reports the face buttons **by print**. The raw evdev codes are: printed A (right) = 304, printed B (bottom) = 305, printed X (top) = 307, printed Y (left) = 308. Thimsy's key layout `mods/keylayout/Vendor_484b_Product_1101.kl:7-11` (installed by `mods/27-controller-layout.sh`) re-reports them **by position** to Android. The Anbernic button is `key 68 F10` (and 316 `BUTTON_MODE`) in the same file.

| Path | Who reads it | Bottom (printed B) | Right (printed A) | Top (printed X) | Left (printed Y) |
|---|---|---|---|---|---|
| Raw evdev, gammapad off | Nano, drastic-nano | 305 `BTN_EAST`/`BTN_B` | 304 `BTN_SOUTH`/`BTN_A` | 307 `BTN_NORTH`/`BTN_X` | 308 `BTN_WEST`/`BTN_Y` |
| Android keycode, gammapad off (default) | PhoneWindowManager, apps | `BUTTON_A` 96 | `BUTTON_B` 97 | `BUTTON_Y` 100 | `BUTTON_X` 99 |
| gammapad on, ABXY Swap off | apps (virtual "Xbox Wireless Controller", 045e:0b13, `Generic.kl`) | `BUTTON_B` 97 | `BUTTON_A` 96 | `BUTTON_X` 99 | `BUTTON_Y` 100 |
| gammapad on, ABXY Swap on | apps | `BUTTON_A` 96 | `BUTTON_B` 97 | `BUTTON_Y` 100 | `BUTTON_X` 99 |
| gammapad on, swap off | Nano (reads the virtual evdev) | 305 | 304 | 307 | 308 |
| gammapad on, swap on | Nano | 304 | 305 | 308 | 307 |

Evidence:
- gammapad reads only `/system/usr/keylayout` and `/vendor/usr/keylayout`, never `/product` (`gammapad/KeyLayoutParser.cpp:18-19`). So it gets the vendor's by-print layout, as `mods/27-controller-layout.sh:8-9` and `README.md:103-105` say.
- It turns keycode names back into Linux codes (`BUTTON_A`→`BTN_A`, `gammapad/KeyLayoutParser.cpp:132-135`).
- It swaps A↔B and X↔Y when `persist.gammaos.gamepad.abxy_swap` is set (`gammapad/InputTransformer.cpp:191`, `268-274`), **before** the user's `remap_btn` (`:276-280`).
- The virtual device is "Xbox Wireless Controller" 045e:0b13 by default (`gammapad/README.md`, Virtual Device Identity). AOSP has no `Vendor_045e_Product_0b13.kl`, so Android uses `Generic.kl` (`frameworks/base/data/keyboards/Generic.kl:289-293`: 304 A, 305 B, 307 X, 308 Y).
- External pads follow the Linux gamepad convention (`BTN_SOUTH` is the bottom button) and Android's layouts map 304 to `BUTTON_A`. So their bottom button is `BUTTON_A` with gammapad off, and also with gammapad on and swap off (their own `.kl` is by position).

**Rule:** the bottom button is a function of (device, gammapad running, swap, `remap_btn`). Both PhoneWindowManager and Nano call one resolver, specified once in §4.2 and unit-tested:

```
southKeycode(dev):            // Android keycode, for PhoneWindowManager
  built-in pad (484b:1101)            -> BUTTON_A
  gammapad virtual (device_vid/pid)   -> swapApplied ? BUTTON_A : BUTTON_B   // its main source is the built-in pad
  any other pad                       -> BUTTON_A
  then apply remap_btn (Linux codes) when the device is gammapad's
southEvdev(fd):               // raw code, for Nano and drastic-nano
  built-in pad -> 305; gammapad virtual -> swap ? 304 : 305; other pad -> 304 (BTN_SOUTH)
```

There is one known limit, listed with the risks. With gammapad merging an external **by-position** pad into the virtual device, that pad's bottom button reaches the virtual device as `BTN_A`. The rule above then expects `BTN_B`, so for that pad the chord is on its right button. The fix is in gammapad, which knows the source device: it can publish `sys.gammaos.gamepad.south` per event batch, or forward the Anbernic chord itself. That is phase 6, not needed for the Cube alone.

### 1.2 The setting

- New row **Button Map Button** in Settings → Gamepad Settings → Hotkeys, after Quit Game Button: `{"Button Map Button", kProp, "persist.gammaos.hotkeys.buttonmap", "FACE_SOUTH", kButtonMapButtons}` beside the hotkey rows (`nano/NanoMenuPS3Menu.cpp:9661-9668`). Description: "Anbernic + this button shows how the controller maps to the game's controller."
- Options are by position, labelled with the Cube's print: `FACE_SOUTH:Bottom (B), FACE_EAST:Right (A), FACE_NORTH:Top (X), FACE_WEST:Left (Y), BUTTON_L1:L1, BUTTON_R1:R1, BUTTON_L2:L2, BUTTON_R2:R2, BUTTON_THUMBL:L3, BUTTON_THUMBR:R3, NONE:Off`.
- No Start or Select (reserved, `nano/NanoMenuPS3Menu.cpp:9450-9457`). No D-pad: it reaches Android as a hat axis, which PhoneWindowManager never sees.
- Two hotkeys can't share a button (`hotkeyTakeButton`, `nano/NanoMenuPS3Menu.cpp:2738-2751`). Add `"Button Map Button"` to `kRows`, and compare **resolved physical buttons**, not strings. The existing rows store Android keycodes, and `BUTTON_A` *is* the bottom button with gammapad off, so `FACE_SOUTH` must collide with it. A helper `bmap::sameButton(a, b)` resolves both through the built-in pad's current model (§4.2).
- **Migration** at Nano start, next to the Start/Select clean-up (`nano/NanoMenu.cpp:909-917`). If a user already put a universal hotkey on the bottom button (`BUTTON_A` today), the hotkey keeps it. Button Map then takes the first free face button in the order south, east, north, west, and a log line says which.
- It is **not** a hotkey. Like Anbernic + Start it works with Universal Hotkeys off and with Controller Bindings off. It needs the Anbernic button to be Nano's (`persist.gammaos.nano.mode_quick_settings`, which Toolbox can turn off; `pwm/PhoneWindowManager.java:2686-2689`).
- `HOTKEYS.md` gets a row in "What it does", and a note in Chord detector step 2.

### 1.3 Collisions checked

| Existing behaviour | Interaction | Resolution |
|---|---|---|
| PWM Anbernic chord: any gamepad button while armed runs `nanoHotkey`, and unbound buttons are swallowed (`pwm/PhoneWindowManager.java:7193-7207`, `HOTKEYS.md` step 2) | Today Anbernic + `BUTTON_A` is swallowed and does nothing | The new branch goes before it, like Anbernic + Select / Start (`:7147-7164`). It is marked `mNanoChordUsed` with its key-up swallowed through `mNanoChordKeys`, so no Quick Menu opens on release and the game never sees the press |
| Universal hotkey bound to the same button | Would be ambiguous | Prevented by `hotkeyTakeButton` plus the migration rule above |
| Hold to quit (Anbernic held 3 s) | A chord press cancels it (`:7188`) | As intended |
| Start + Select hotkeys (`:7221-7266`) | Only `nanoIsHotkeyButton` buttons are taken there | Button Map is **not** added to PhoneWindowManager's Start + Select: Start + Select + B is a common in-game combination. In Nano's own menus Start + Select + bottom does open it (Nano's input code, §1.5) |
| Anbernic + Select / Start | Different buttons | None |
| Anbernic + Volume (brightness) | Different keys | None |
| gammapad BrowserMode | The Anbernic button and the buttons pressed while it is held pass through to the virtual gamepad (`gammapad/BrowserMode.cpp:191-214`, `220-236`) | PWM sees the chord in Brave and Fulguris |
| gammapad MouseMode combo (Select + R1 held 2 s, `gammapad/MouseMode.cpp:23-26`) | Different buttons | None |
| Nano menus: Anbernic + button is swallowed (`nano/NanoMenuInput.cpp:3010-3021`) | The bottom button never confirms or goes back during the chord | The new branch is placed before the swallow |
| drastic-nano's own chord detector | Reads the pad itself (`HOTKEYS.md`, drastic-nano) | Its own branch (§1.5, §3.2 drastic-nano) |
| RetroArch `input_enable_hotkey` / menu toggle on Mode | PWM swallows the Anbernic button over apps | None |
| Emergency 10 s hold (BACK / `BUTTON_MODE`, `pwm/PhoneWindowManager.java:7098-7101`) | A chord press doesn't cancel it. It checks the key is still down when it fires | Unchanged |

### 1.4 Over a running game or app (PhoneWindowManager)

Insert into `interceptKeyBeforeQueueing`, in the Anbernic + Select / Start block (`pwm/PhoneWindowManager.java:7147-7164`):

```java
} else if (nanoChordKey && freshDown && nanoAnbernicOverApp(policyFlags)
        && gammaAnbernicPhysicallyHeld() && nanoIsButtonMapButton(event)) {
    mNanoChordUsed = true;
    mNanoChordKeys.put(keyCode, true);
    mHandler.removeCallbacks(mNanoHoldQuit);
    nanoSummonButtonMap(event.getDeviceId());   // qs_summon=3, bmap_dev, show_overlay=1
    return 0;
}
```

- `nanoIsButtonMapButton` reads `persist.gammaos.hotkeys.buttonmap`. For `FACE_*` it resolves the event's device with the rule in §1.1 (`InputDevice.getVendorId/ProductId`, the gammapad props `persist.gammaos.gamepad.device_vid/pid`, `init.svc.gammapad`, `abxy_swap`). `BUTTON_*` values compare the keycode directly.
- `nanoSummonButtonMap` is `nanoSummonApplications` (`:2804-2808`) with `qs_summon=3`. It also writes `sys.gammaos.nano.bmap_dev=<vendor>:<product>:<descriptor>:<name>` for the device that pressed the chord (§4), so the overlay can label that pad.
- `nanoOverApp` (`:2699-2708`) already excludes drastic-nano sessions, the overlay being up, Nano killing an app, and boot. When the overlay is up, Nano reads the chord itself (§1.5).

### 1.5 Nano: menus, overlay up, and the raise

- **Start + Select + bottom, menus only.** Nano's input code opens the map when Start and Select are held and the bottom button (by position, through the pad model) goes down, only while a menu is Nano's own (`triggerOpens(kStartSelectSouth, kNanoMenus)`). Not while a game is in front, and not in drastic-nano.
- **Menus and overlay up.** Add a branch next to Anbernic + Select / Start (`nano/NanoMenuInput.cpp:2990-3008`): `mAnbernicHeld && keyHeld(F10|BTN_MODE) && ev.value==1 && bmap::isButtonMapEvdev(fd, ev.code)`. It clears `mAnbernicArmed`, toggles the button map (open, or close if open) and `continue`s. The fd gives the device (`mInputFdNames`, `EVIOCGID`) for §4.
- **Raise over a game.** `overlayShow` already consumes `qs_summon` (`nano/NanoMenuOverlay.cpp:412-414`) and opens Applications for 2 (`:626-629`). Add `summon == 3 && !mOverlayWallpaper → buttonMapOpen(ctxFromForeground())`. The "layer still up" path also needs it (`nano/NanoMenuOverlay.cpp:1686-1711`).
- **Dismiss.** The same chord, B (the button that is Back in Nano: evdev `BTN_EAST`, §1.1), Start, or a tap outside the panel. The game kept running under the map, and its input was blocked while the map was up. Closing returns input to the game the way Back on a summoned Applications view does (`quickMenuBackResumes`, `nano/NanoMenuPS3Menu.cpp:1990-1996`; `overlayResume`, `nano/NanoMenuOverlay.cpp:955`). From the menus it just closes. Moving around the panel (D-pad, left stick, L1 / R1, touch) is in §6.5.
- **drastic-nano.** It has its own chord detector and its own overlay (`drastic-nano/OverlayMenu.cpp`, `OverlayGfx.h`). It links Nano's shared sources (`nano/Android.bp:686-699`, `drastic_nano_shared_srcs`). `NanoButtonMap*.cpp` join that filegroup. drastic-nano then draws the same RGBA as a texture in its overlay pass. Unlike its own menu it does **not** pause emulation: it only stops forwarding the pad to the emulator while the map is up. Hook location: see §3.2 (drastic-nano).

---

## 2. Context resolution

The output is one `bmap::Context`:

```cpp
struct Context {
  enum Kind { kNone, kGame, kBrowser } kind = kNone;
  std::string systemId;      // Nano system id / romDir ("snes", "n64", "psp" ...); "" when unknown
  std::string emulator;      // "retroarch", "ppsspp", "dolphin", "drastic-nano", ... (hotkey profile id)
  std::string package;       // Android package
  std::string core;          // RetroArch core basename ("snes9x"), else ""
  std::string romPath;       // for per-game overrides; may be ""
  std::string browserPkg;    // kBrowser only
};
```

`kNone` means the chord does nothing: no panel, no sound. It is a pure function, `bmap::resolveContext(const ContextInputs&)`. Its inputs are plain strings gathered by the caller, so the host tests cover every branch (§9.2, T4).

### 2.1 In Nano's menus (home, or the overlay raised as the launcher)

| Focus | Result |
|---|---|
| A system (`PS3_SYSTEM`, `a = sysIdx`, `nano/NanoMenu.h:1500`), or the ES-DE system view (`mEsdeSysSel`, `nano/NanoMenu.h:1764`) | `kGame` for that system's player: `XmbSystem::coreSo` → RetroArch + core, or `launchPkg` → the standalone (`nano/NanoMenu.h:227-267`). DS with `persist.gammaos.nano.drastic_nano=1` and DraStic → `drastic-nano` |
| A game (`PS3_ROM`, `a,b`; ES-DE gamelist `mEsdeGameSel`; a collection or favourite row) | As above, after the game's own emulator choice: `gameEmulatorFor(romPath)` / `applyGameEmulator` (`nano/NanoMenu.h:3498-3500`). The ROM path is kept for per-game overrides |
| A Recently Played entry (`PS3_RECENT`) | `XmbRecentEntry::coreSo` / `launchPkg` (`nano/NanoMenu.h:275-284`), system from `recentSystemIndex` (`nano/NanoMenuPS3EmuCatalog.cpp:681`) |
| The **Android Games** system (`romDir == "androidgames"`, package `org.thimsy.androidgames`, `mods/nano-systems.txt:69`), or any of its games | `kNone` (the user's rule: the Android apps playlist does nothing) |
| Applications view, Settings, media players, anything else | `kNone`. The only exception is a browser row in Applications, which is `kBrowser` (the same list the browser itself would show) |

The focus helpers already exist: `focusedRomPath()` (`nano/NanoMenuPS3Menu.cpp:12891-12902`), and `ps3CurItems()` / `ps3CurSel()` for the system rows.

### 2.2 Over a running app (the overlay raised by `qs_summon=3`)

1. **Foreground package:** `mOverlayPausedPkg`, which `overlayShow` already resolves from `dumpsys` with `sys.gammaos.nano.launch_app` as the fallback (`nano/NanoMenuOverlay.cpp:300-331`, `464-470`).
2. **Package → emulator:** the hotkey profiles' `packages` lists (`nanohotkeys::loadProfiles()`, `nano/NanoHotkeys.h:20-22`; `mods/hotkeys/*.json`). The `"*"` fallback profile counts as "no emulator".
3. **Browser:** the package matches `persist.gammaos.gamepad.browser_pkgs` (default `com.brave.browser,net.slions.fulguris.*`, with trailing `*` as a prefix match; `gammapad/BrowserMode.cpp:22-41`) → `kBrowser`. The matcher is a copy of `packageMatches`, unit-tested.
4. **No emulator and no browser** (Android games, Settings, any other app) → `kNone`.
5. **System and core for an emulator:**
   - **RetroArch:** Nano's own launch record, `sys.gammaos.nano.launch_core` = `/data/data/com.retroarch.aarch64/cores/<core>_libretro_android.so`, set on every Nano RetroArch launch (`nano/NanoMenuXmb.cpp:3422-3426`, `nano/NanoMenuInput.cpp:429`), plus `sys.gammaos.nano.launch_rom`. If `content_history.lpl` has a newer entry (content loaded inside RetroArch), that entry's `core_path` and `path` win (§3.1). The core → system table is `retropad.json`. `GET_STATUS` isn't usable: network commands are off in the shipped config, and its reply names only the system (§3.1).
   - **Standalone emulators:** Nano clears `launch_rom` for standalone launches (`setLaunchRomPath("")`, `nano/NanoMenuXmb.cpp:3368`; the helper is at `nano/NanoMenuUtils.h:119-122`) and records no system. The ROM survives only as a URI in `/data/system/nano_launch_intent.txt`. **New:** every launch path writes `sys.gammaos.nano.launch_system=<system id>` and `sys.gammaos.nano.launch_game=<rom path>` beside `launch_app`, and clears both when the app exits, wherever `launch_app` is cleared (`nano/NanoMenuOverlay.cpp:1106-1108`, `1204`). Dolphin needs this to tell GameCube from Wii. Without it, Dolphin shows the GameCube pad with a note.
   - **drastic-nano:** `sys.gammaos.drastic_nano.session=1`. drastic-nano handles the chord itself, so the overlay never sees this case.
6. **App launched outside Nano** (from Applications or a recents switch): no ROM. For RetroArch the newest `content_history.lpl` entry gives the core. For standalones the emulator is known and the console defaults to the emulator's first system (PPSSPP → PSP and so on).

### 2.3 Device in hand

- **Over a game:** `sys.gammaos.nano.bmap_dev`, written by PhoneWindowManager (§1.4).
- **In Nano:** the fd that pressed the chord.

Both are resolved by §4.

---

## 3. Active mapping per emulator

### 3.0 Common recipe

Every reader returns `consoleId → Input` (an Android keycode, or an axis and a sign). `build` (§6.1) then labels each one with the physical button(s) through `PadModel` (§4.2).

Readers take `ReaderEnv.root`, so the host tests run them on fixture trees (§9.2 T1). "Defaults" means the emulator's built-in mapping, copied from its source at the shipped version into the reader as a table.

Key rule: a stored keycode means *what the app received*. With gammapad off, 96 is the Cube's bottom button. With gammapad on and swap off, 96 is the right button. In drastic-nano, which reads evdev, 96 means evdev 304, the right button. The table in §1.1 is the only translation.

### 3.1 RetroArch 1.22.2 (`com.retroarch.aarch64`): reliable

Sources:
- RetroArch tag v1.22.2 (`69a4f0e`);
- the shipped payload, extracted from `out/RG_Cube_Thimsy_OS_v1.0a.pac` (`/system/etc/retroarch.tar.zst`). `RA.cfg` below is its `sdcard/Android/data/com.retroarch.aarch64/files/retroarch.cfg`;
- libretro/docs `d83b34b`, libretro-core-info `eac3a27`, the cores' own sources.

**What Thimsy ships**

- `autoconfig/android/retrogame_joypad.cfg`: `input_device="retrogame_joypad"`, VID/PID 18507/4353 (484b:1101). b=96, a=97, y=99, x=100, l/r=102/103, l2/r2=104/105, l3/r3=106/107, start/select=108/109, D-pad `h0up…`, sticks `±0/±1` and `±2/±3`. These are by position, rewritten by `mods/10-update-emulators.sh:186-208`.
- **Player 1 binds are pinned in RA.cfg** (lines 1432-1580) to the same values, for whatever pad is at `input_player1_joypad_index`. `l2_axis`/`r2_axis` are `nul`.
- `input_player1_analog_dpad_mode="1"` (left stick = D-pad, RA.cfg:1437).
- `auto_overrides_enable` / `auto_remaps_enable` true (46-47), `input_remap_binds_enable` true (2792), `input_remap_sort_by_controller_enable` false (2793).
- Remaps in `/storage/emulated/0/RetroArch/config/remaps` (2794). Config dir `/storage/emulated/0/RetroArch/config` (3183).
- The shipped core overrides contain no `input_` keys, and `config/remaps/` is empty.
- Core options that matter: `PicoDrive.opt:8` `picodrive_input1="3 button pad"`; `SwanStation.opt:16`, `:60`.
- RetroArch's own hotkeys: enable = Select (`input_enable_hotkey_btn="109"`, 257); Select+L2 load (298), Select+R2 save (2822), Select+R1 fast-forward (2861); menu = L3+R3 (`input_menu_toggle_gamepad_combo=2`, 304). These go to the hotkeys section with physical labels.
- **`network_cmd_enable` is absent, so it is false** (`config.def.h:1466`). The hotkey profile provisions only keyboard keys (`mods/hotkeys/retroarch.json`).

**Bind resolution** (what the reader reproduces)

1. Button and axis are resolved separately for each RetroPad id. `input_player1_<id>_btn` wins unless it is `nul`. Then the autoconfig profile of the pad at `input_player1_joypad_index` applies. The same holds for `_axis` (`input/input_driver.c:806-835`; `input/drivers_joypad/android_joypad.c:113-131`; `"nul"` handling `configuration.c:6838-6844`, `6750-6755`). Profiles go into a separate table and never overwrite player binds (`tasks/task_autodetect.c:661-666`, `configuration.c:6649-6673`).
2. **Overrides** stack on top of retroarch.cfg, later keys winning: `<cfgdir>/<library_name>/<library_name>.cfg`, then `<contentdir>.cfg`, then `<rom basename>.cfg` (`configuration.c:4577-4760`, paths built `:4612-4634`).
3. **Autoconfig matching** (`task_autodetect.c:114-213`): +30 for a VID+PID match, +20 for an exact `input_device` name; the highest score wins. `autoconfig/android/` is scanned first; the fallback is the built-in "Android Gamepad".
   - The built-in pad matches `retrogame_joypad.cfg` (score 50).
   - The gammapad virtual pad (045e:0b13) matches `android/Xbox Wireless Controller 2020.cfg` (VID/PID 1118/2835, score 30): b=96, a=97, y=99, x=100, select=**4**, l2/r2 **axes +6/+7**.
   - With Thimsy's pinned player binds, only the `nul` fields come from it. So with gammapad on, the triggers also work as axes.
4. **Remaps:** the first that exists of `<remapdir>/<library_name>/<rom>.rmp`, then `<contentdir>.rmp`, then `<library_name>.rmp`. They don't stack (`configuration.c:4868-4990`).
   - Keys: `input_player1_btn_<b|y|select|start|up|down|left|right|a|x|l|r|l2|r2|l3|r3>` = the RetroPad id the core receives (0 B, 1 Y, 2 Select, 3 Start, 4-7 D-pad, 8 A, 9 X, 10 L, 11 R, 12 L2, 13 R2, 14 L3, 15 R3, 16-23 analog halves, −1 unmapped).
   - Analog keys: `input_player1_stk_<l_x+…>`.
   - Device: `input_libretro_device_p1` (`configuration.c:6140-6252`; meaning `input_driver.c:6310-6363`).
   - The device type lives **only** in `.rmp` in 1.22.2. Without one, it is `RETRO_DEVICE_JOYPAD` (`input_driver.c:6586-6599`). Subclass values: Beetle/PCSX DualShock 517, Analog 261; GPGX MD 3-button 257, 6-button 513, MS 769; FBNeo Classic 5 / Modern 261; MAME 2003+ Modern 257, 8-button 513, 6-button 769.
5. **Codes:**
   - Buttons are raw Android keycodes (`android_input.c:913-946`; `android_joypad.c:58-59`).
   - The D-pad is hat 0 from AXIS_HAT_X/Y (`configuration.c:6855-6866`; `android_joypad.c:35-56`).
   - Axes `±N` index `analog_state[]`: 0 X, 1 Y, 2 Z, 3 RZ, 6 LTRIGGER, 7 RTRIGGER, 8 BRAKE, 9 GAS (`android_input.c:543-550`).
6. **Left stick as D-pad** is true only for cores that never poll analog. `analog_dpad_mode` turns off for the port once the core reads a stick (`input_driver.c:1884-1891`). The reader marks it "(digital games only)" for cores whose row uses analog.
7. **Folder names use the core's `library_name`, not the .so name.** The table in `retropad.json` carries it. Three differ from the info `corename`: `mupen64plus_next_gles3` → `Mupen64Plus-Next`, `mamearcade` → `MAME`, `fake08` → `fake-08`.
8. **Unsaved binds:** retroarch.cfg is written on exit (`config_save_on_exit`, RA.cfg:102). A bind changed in RetroArch's Quick Menu and not saved isn't on disk, so the overlay shows the saved state. This is noted in the device checklist.

**Which core is running.** `GET_STATUS` (`command.c:1034-1072`) needs `network_cmd_enable`, which is off. Its reply names only the core info `systemid` (`super_nes`), so it can't tell Snes9x from bsnes. The plan therefore does **not** use it:
1. `sys.gammaos.nano.launch_core` (`.so` path), set by every Nano RetroArch launch. Nano's overlay launch passes `--es LIBRETRO …/cores/<core>.so` (`nano/NanoMenuOverlay.cpp:1942`).
2. If the content was changed inside RetroArch, or RetroArch was started outside Nano: the newest entry of `content_history.lpl` (`/storage/emulated/0/RetroArch/playlists/builtin/content_history.lpl`, RA.cfg:108), a JSON with `path`, `core_path`, `core_name` (`playlist.c:1959-1999`), when its mtime is newer than the launch. RetroArch writes it at content load (`tasks/task_content.c:1614`).
3. Otherwise the system's default core.

Turning on `network_cmd_enable` only for this isn't worth a listening UDP port, so it is listed as an option, not a step.

**RetroPad → console per core** (default device and options; the source of `retropad.json`). Columns are RetroPad ids. RetroPad B is bottom, A right, Y left, X top. "LS→D" means RetroArch's left stick as D-pad.

| Core | B | A | Y | X | L | R | L2 | R2 | L3 | R3 | Start | Select | Sticks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sameboy | B | A | — | — | — | — | — | — | — | — | Start | Select | LS→D |
| DoubleCherryGB | B | A | — | — | Prev audio mode | Next audio mode | — | — | — | — | Start | Select | LS→D |
| mgba | B | A | Turbo B | Turbo A | L | R | Turbo L | Turbo R | Solar − | Solar + | Start | Select | LS→D |
| gpsp | B | A | Turbo B | Turbo A | L | R | — | Fast forward | — | — | Start | Select | LS→D |
| mesen | B | A | Turbo B | Turbo A | (FDS) Next disk | (FDS) Switch side | (VS) Coin 1 | (VS) Coin 2 | (Famicom) Mic | — | Start | Select | LS→D |
| fceumm | B | A | Turbo B* | Turbo A* | (FDS) Disk side | (FDS) Insert/eject | — | (VS) Coin | A+B | Turbo A+B | Start | Select | LS→D |
| snes9x, bsnes | B | A | Y | X | L | R | — | — | — | — | Start | Select | LS→D |
| mupen64plus_next | A | C-Right (with R2 held) | B | C-Up (with R2) | L | R | Z | hold: C-button mode | — | — | Start | — | LS stick, RS C buttons |
| mednafen_vb | B | A | — | Low battery | L | R | Right D-pad Up | Right D-pad Left | Right D-pad Down | Right D-pad Right | Start | Select | RS right D-pad |
| genesis_plus_gx (MD) | B | C | A | Y (6B) | X (6B) | Z (6B) | — | — | — | — | Start | Mode (6B) | LS→D |
| genesis_plus_gx (SMS/GG) | 1 | 2 | — | — | — | — | — | — | — | — | Start/Pause | — | LS→D |
| picodrive (3-button shipped) | B | C | A | (Y) | (X) | (Z) | — | — | — | — | Start | (Mode) | LS→D |
| yabasanshiro | A | B | X | Y | C | Z | L | R | — | — | Start | — | LS analog |
| mednafen_saturn | A | B | X | Y | Z | C | L | R | (ST-V Test) | (ST-V Service) | Start | (Coin) | analog |
| flycast (DC) | A | B | X | Y | — | — | LT | RT | — | — | Start | — | LS analog |
| flycast (NAOMI/AW) | Btn 1 | Btn 2 | Btn 3 | Btn 4 | Btn 6 | Btn 5 | Btn 8 | Btn 7 | Test | Service | Start | Coin | per game |
| pcsx_rearmed, swanstation, mednafen_psx_hw | Cross | Circle | Square | Triangle | L1 | R1 | L2 | R2 | L3 (analog) | R3 (analog) | Start | Select | analog only with DualShock/Analog |
| fbneo (per game) | Fire 1 | Fire 2 | Fire 3 | Fire 4 | Fire 6 | Fire 5 | Fire 8 | Fire 7 | Fire 10 | Fire 9 | Start | Coin | per game |
| mamearcade (per game) | Btn 1 | Btn 2 | Btn 3 | Btn 4 | Btn 5 | Btn 6 | — | — | Btn 7 | Btn 8 | Start | Coin | analog |
| mame2003_plus (per game) | Btn 1 | Btn 2 | Btn 3 | Btn 4 | Btn 5 | Btn 6 | Btn 7 | Btn 8 | Btn 9 | Btn 10 | Start | Coin | analog |
| mednafen_ngp | A | B | — | — | — | — | — | — | — | — | Option | — | LS→D |
| mednafen_pce_fast | II | I | III | IV | V | VI | Mode 2/6 | — | — | — | Run | Select | LS→D |
| mednafen_wswan (landscape) | B | A | — | — | Y-Left | Y-Right | Y-Down | Y-Up | — | — | Start | Rotate | X cursors on D-pad |
| stella | Fire | Trigger | Booster | — | L Diff A | R Diff A | L Diff B | R Diff B | Color | B/W | Reset | Select | paddle |
| a5200 | Fire 2 | Fire 1 | * | # | Keypad | 0 | 3 | 1 | 7 | 5 | Start | Pause | analog, RS keypad |
| prosystem | 1 | 2 | — | Reset | L Diff | R Diff | — | — | — | — | Pause | Select | LS→D |
| mednafen_lynx | B | A | — | — | Option 2 | Option 1 | — | — | — | — | Pause | Rotate | LS→D |
| opera | B | C | A | P | L | R | — | — | — | — | P | X (Stop) | LS→D |
| gearcoleco | Yellow | Red | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | * | # | RS keypad |
| bluemsx | Button 2 | Button 1 | 3 | 4 | 5 | 6 | — | — | — | — | Coleco # | Coleco * | LS→D |
| fake08 | O | X | — | (mouse) | — | — | — | — | — | — | Pause | — | LS→D |

Evidence and variants. Paths are in each core's tree; docs are `docs/library/<core>.md`.
- **sameboy** `libretro/libretro.c:425-432`. **mgba** `src/platform/libretro/libretro.c:1384-1403`. **snes9x** `libretro/libretro.cpp:1276-1289`. **bsnes** `target-libretro/libretro.cpp:419-430`.
- **gpsp:** R2 = fast forward only with frontend override (`libretro/libretro.c:1149-1185`).
- **mesen:** `Libretro/libretro.cpp:859-887`. The `mesen_shift_buttons_clockwise` option moves A/B to B/Y (`:525-570`). The docs show only the shifted layout.
- **fceumm:** turbo needs Turbo Enable (`src/drivers/libretro/libretro.c:3846-3860`).
- **mupen64plus:** C buttons via R2 + face buttons (options `-r/-l/-d/-u-cbutton`, `libretro_core_options.h:1586-1661`). The `-alt-map` option changes it (`emulate_game_controller_via_libretro.c:76-139`).
- **genesis_plus_gx:** a 6-button pad when the ROM header asks, else 3-button (`libretro.c:3284-3292`, `core/input_hw/input.c:80-104`). The reader reads the header byte only when needed, otherwise it notes "(6-button games)".
- **picodrive:** 3-button by Thimsy's `.opt` (`platform/libretro/libretro.c:1792-1805`). The (Y)(X)(Z)(Mode) entries apply only when 6-button is chosen.
- **yabasanshiro** `yabause/src/libretro/libretro.c:273-316`. **saturn** `input.c:684-703`. The two Saturn cores put C/Z on opposite shoulders.
- **flycast** `shell/libretro/libretro.cpp:1758-1850`.
- **PS1:** the default device is a digital pad, so the sticks and L3/R3 are unused until the `.rmp` device is DualShock or Analog (`pcsx/frontend/libretro.c:1964-1983`, `psx/input.c:322-341`).
- **fbneo:** the descriptors are the game's own button names (`src/burner/libretro/retro_input.cpp:58-84`, `395-400` for CPS LP/MP/HP = Y/X/L and LK/MK/HK = B/A/R, Neo Geo `2455-2468`). The reader shows the generic row with the badge "Per-game layout". Reading the game's descriptors needs the running core, so it is out of scope.
- **mamearcade** `src/osd/modules/input/input_retro.cpp:206-231`. **mame2003_plus** `src/mame2003/mame2003.c:1100-1168`.
- **pce_fast:** 2-button default (`libretro_core_options.h:463-474`).
- **wswan:** rotation via Select; the portrait mapping is in `libretro.c:1315-1377`. `retropad.json` applies it when `wswan_rotate_display` is `portrait` (with `wswan_rotate_keymap` on `auto`) or `wswan_rotate_keymap` is `enabled`; Manual rotation by Select at run time can't be seen, so the map stays landscape then.
- **lynx:** the core's descriptor text is wrong. The code is A→A, B→B, L→Option 2, R→Option 1 (`libretro.cpp:322-376`), and the table follows the code.
- Others: `vb/libretro.cpp:1134-1175`, `ngp/libretro.c:534-542`, `stella/src/os/libretro/libretro.cxx:1769-1790`, `a5200/libretro/libretro.c:532-551`, `prosystem/core/libretro.c:484-494`, `opera/lr_input.c:85-96`, `gearcoleco/platforms/libretro/libretro.cpp:392-411`, `bluemsx/libretro.c:711-726`, `fake08/platform/libretro/libretro.cpp:192-198`.

**Core options** that change a row are read from `<cfgdir>/<library_name>/<library_name>.opt`, then the game's `.opt` (`game_specific_options` true, RA.cfg:178), then the global `retroarch-core-options.cfg`. The per-game `.opt` path is to be confirmed in source during phase 1. `retropad.json` lists, per core, the option keys that select a variant (mesen shift, mupen64 alt map, picodrive pad, pce pad type, wswan rotation).

**Default verdict:** reliable. The physical → RetroPad layer is exact. The core layer is exact for fixed cores, and generic ("Fire 1…", "Btn 1…") for fbneo and MAME, with a per-game badge.


### 3.2 Standalone emulators

Android axis ids used below: X=0, Y=1, Z=11, RZ=14, HAT_X=15, HAT_Y=16, LTRIGGER=17, RTRIGGER=18.

**Hat handling** decides how the D-pad row reads:
- Flycast (`BaseGLActivity.java:299-326`), ARMSX2 and Vita3K (through SDL) turn the hat into D-pad presses themselves, so their D-pad binds are keycodes 19-22. `PadModel` maps 19-22 onto the physical D-pad for them.
- PPSSPP, Dolphin and Azahar keep the hat as axes 15/16.

#### PPSSPP 1.20.4 (`org.ppsspp.ppsspp`): reliable

- **Files:**
  - `PSP/SYSTEM/controls.ini` `[ControlMapping]` (`Core/Config.cpp:1249-1250`), under the memstick dir (`ppssppMemstickAt(root)`, `nano/NanoAppFiles.h:99`);
  - per game, `PSP/SYSTEM/<GAMEID>_ppsspp.ini` `[ControlMapping]` (`Core/Util/PathUtil.cpp:144-149`, `Config.cpp:1885-1936`).
  - The game id needs the running game, so the reader reads the ISO/CSO/PBP header for the disc id. It is cached by path + mtime.
- **Keys** (`Core/KeyMap.cpp:460-551`): `Up Down Left Right Circle Cross Square Triangle Start Select L R`, `An.Up/Down/Left/Right`, `RightAn.*`. `Fast-forward`, `Save State`, `Rewind`, `Pause`, … go to the hotkeys section.
- **Value:**
  - a comma list of binds (`KeyMap.cpp:836-837`); each bind is `dev-code` pairs, with `:` joining a chord (`:1026-1034`, `Common/Input/InputState.cpp:92-107`);
  - devices: 0 default, 1 keyboard, 10 = any pad (`InputState.h:17-31`);
  - axis codes are `4000 + axis*2 + (dir<0)` (`InputState.h:56-72`): hat 4030 R / 4031 L / 4032 down / 4033 up; left stick 4000-4003; right stick Z 4022/4023, RZ 4028/4029; triggers 4034/4036.
- **Translate:** pad keycode N matches `10-N`. The overlay ignores `1-`/`0-` (keyboard, including the hotkey profile's provisioned `1-132`). A chord bind shows as "X + Y".
- **Per device:** global. Every gamepad is device 10 (`InputDeviceState.java:112-127`), the gammapad virtual pad included.
- **Defaults:** only when the section is **absent** (`KeyMap.cpp:779-815`, `KeyMapDefaults.cpp:232-261`): Cross/Circle/Square/Triangle = 96/97/99/100, D-pad = hat + 19-22, Start/Select = 108/109, L/R = 102/103, left stick = analog; fast-forward = RTRIGGER/105, pause = LTRIGGER/104.
- **Gotcha:** once the section exists, a key missing from it is **unbound** (`KeyMap.cpp:832-834`). A per-game ini without the section falls back to the built-in defaults, not to `controls.ini` (`:819-822`).
- **Shipped state:** Thimsy's payload `controls.ini` has the face binds by position (`mods/27-controller-layout.sh:28-70`): Cross on the bottom with gammapad off.

#### Dolphin 2609-197 @ `41799046` (`org.dolphinemu.dolphinemu`): partial, and **unbound by default**

- **Files:** under `Android/data/org.dolphinemu.dolphinemu/files/Config/` (or `/sdcard/dolphin-emu/Config`, the two paths the hotkey profile knows):
  - `GCPadNew.ini` `[GCPad1..4]` (`Core/HW/GCPad.cpp:14`);
  - `WiimoteNew.ini` `[Wiimote1..4]` (`Wiimote.cpp:100`);
  - `Dolphin.ini [Core] SIDevice0` (6 = GC pad) and `[Wiimote1] Source` (1 = emulated) decide which exist.
- **Keys:**
  - GC: `Buttons/A B X Y Z Start`, `Main Stick/Up…`, `C-Stick/…`, `Triggers/L R L-Analog R-Analog`, `D-Pad/…` (`GCPadEmu.cpp:38-80`);
  - Wii Remote: `Buttons/A B 1 2 - + Home`, `D-Pad/…`, with `Extension = Classic` adding `Classic/...` (`WiimoteEmu.cpp:213-271`, `Classic.h:221-244`).
- **Device:** each section has `Device = Android/<id>/<InputDevice name>` (`CoreDevice.cpp:190-199`). The id is the device's controller number when nonzero (`Android.cpp:658-661`). The gammapad virtual pad gets its own qualifier, so binds made for the Cube's pad **don't** apply to it. The reader compares `Device=` with `bmap_dev` and shows "Bound to <other device>" on a mismatch.
- **Expressions:**
  - names from `Android.cpp:95-385`: 96 `` `Button A` ``, 97 `` `Button B` ``, 99 X, 100 Y, 102-107 `Button L1…R3`, 108 `Start`, 109 `Select`, 19-22 `Up Down Left Right`;
  - axes as `` `Axis 15-` `` (`:392-426`).
  - The reader handles a bare or qualified single input, plus `|` alternatives. Anything else (`&`, functions) shows the expression text with "advanced".
- **Defaults:** **none**. On Android, `LoadDefaults` sets only rumble and the Triforce coin (`GCPadEmu.cpp:194-199`), so a fresh Dolphin has no pad buttons bound. **Finding:** Thimsy provisions no `GCPadNew.ini`. Recommendation, outside this plan's scope but worth noting: add a provision rule (template) for a by-position GC pad on the Cube.
- **Until then:** the overlay shows "Not mapped in Dolphin: open Dolphin → Settings → GameCube Input".
- **Per game:** `GameSettings/<ID>.ini [Controls] PadProfile1` / `WiimoteProfile1` → `Config/Profiles/GCPad/<name>.ini`, whose `[Profile]` replaces the section (`InputConfig.cpp:37-89`, `InputProfile.cpp:30-54`). The ID comes from the disc header.

#### Azahar 2126.2 (`org.azahar_emu.azahar`): reliable once bound, **no defaults**

- **File:** `shared_prefs/org.azahar_emu.azahar_preferences.xml` (`appdef::kAzaharPrefs`, `nano/NanoAppFiles.h:44`).
- **3DS codes** (`NativeLibrary.kt:1059-1090`): A 700, B 701, X 702, Y 703, Start 704, Select 705, Home 706, ZL 707, ZR 708, Up..Right 709-712, Circle Pad 713, C-Stick 718, L 773, R 774, D-pad axis 780. Hotkeys are 10001-10009.
- **Button binds:** `InputMapping_HostAxis_<keycode>` → a `<set>` of codes (manual bind) **or** a bare `<int>` (Auto-map) (`InputBindingSetting.kt:181-198`, `642-653`, `696-721`).
- **Axis binds:** `InputMapping_HostAxis_<axis>_GuestButton`, `_GuestOrientation` (0 horizontal, 1 vertical), `_Inverted` (`:203-215`, `726-742`):
  - 713/718 = stick X or Y;
  - 780 = D-pad (horizontal: −Left +Right; vertical: −Up +Down);
  - 773/774/707/708 = pressed when nonzero (`EmulationActivity.kt:424-615`).
- The `button_a`-style keys are only display text, not authoritative.
- **Per device:** global (`native.cpp:888-891` ignores the device).
- **Defaults:** none until the user binds or runs Auto-map (`SettingsAdapter.kt:665-676`). Auto-map uses the Xbox table when the east button reports 97 (3DS A←97, B←96, X←100, Y←99), else identity (`AutoMapDialogFragment.kt:89-112`). With no binds, the overlay says "Not mapped in Azahar: Settings → Controls → Auto-map".

#### Flycast 2.7 (`com.flycast.emulator`): reliable

- **File:** `mappings/<api>_<device name>[-<descriptor>][_<gameId>][_arcade].cfg`, with `/ \ : ? * | " < >` replaced by `-` (`core/input/gamepad_device.cpp:587-608`). The api is `Android` (`android_gamepad.h:63`).
  - The folder is `mappings/` under the Flycast home (`flycastHomeAt(root)`, `nano/NanoAppFiles.h:100`), unless `Dreamcast.MappingsPath` is set (`mapping.cpp:831-870`).
- **Lookup** (`find_mapping`, `gamepad_device.cpp:610-652`): instance + game, then name + game, then instance, then name, then (arcade) the Dreamcast file again, then the built-in default. The gammapad virtual pad has its own `Android_Xbox Wireless Controller.cfg`.
- **Format** (`mapping.cpp:445-588`): `[digital] bindN = keycode:action`, `[analog] bindN = axis±:action`, `[combo]`. A trailing 1-3 on the action is the port.
  - Actions (`:31-110`): `btn_a b c d x y z start`, `btn_dpad1_*`, `btn_dpad2_*`, `axis_left/right/up/down`, `axis2_*`, `axis_trigger_left/right`. `btn_menu`, `btn_fforward`, `btn_quick_save` go to the hotkeys section.
- **Hat:** becomes keycodes 19-22 in Java (`BaseGLActivity.java:299-326`).
- **Defaults** (`android_gamepad.h:249-366`):
  - A/B/X/Y = 96/97/99/100, Start 108;
  - D-pad 19-22 (the hat);
  - LT/RT = 17+/18+ analog;
  - btn_z = L1, btn_c = R1;
  - stick = axes 0/1, axis2 = 11/14;
  - **btn_menu = Select**.
  - The defaults can be active with no file written (`mapping.cpp:919`).
- **Per game:** the `_<gameId>` file. Game id from the disc header, or from Flycast's own `per-game` naming. Flycast caches mappings in process, so edits apply after a restart. The overlay shows the file on disk.

#### ARMSX2 2.9.2 (`com.armsx2`): reliable, with caveats

- **File:** `shared_prefs/ARMSX2.xml` (`appdef::kArmsx2Prefs`), `<int name="pad.map.<id>">` with the physical keycode, 0 = unbound (`ControllerMappings.kt:668-672`, `729-732`).
  - Ids: `dpad_up..right cross circle square triangle l1 r1 l2 r2 l3 r3 select start analog ls_* rs_*`.
- **Prefixes:** player 2 is `p2.` (`:95`). **Per game:** `game.<serial or file stem>.` takes precedence (`:113-133`). Stick modes: `pad.lstick.mode` / `pad.rstick.mode` (analog / face / dpad / custom, `:169-170`, `601-625`). Hotkeys are `pad.savestate.keycode` … (`:1112-1136`).
- **Defaults:** identity (`:20-42`): Cross/Circle/Square/Triangle = 96/97/99/100, L1/R1/L2/R2 = 102-105, L3/R3 = 106/107, Select/Start = 109/108, D-pad 19-22.
- **Rules to reproduce:**
  - the first action in list order wins (`:931-936`);
  - the hat drives the PS2 D-pad directly in a fixed direction when 19-22 are bound to anything (`MainActivityRuntime.kt:5875-5960`);
  - LTRIGGER/BRAKE behave as 104 and RTRIGGER/GAS as 105 (`:4894-4917`);
  - the left stick is axes 0/1; the right stick is Z/RZ (`:4856-4881`).
  - Macros and turbo aren't modelled; the overlay notes "Macros not shown" when any `pad.macro.*` exists.
- `PCSX2.ini [Pad1]` is **not** used on Android (`native-lib.cpp:1680-1749`).

#### Vita3K 0.2.1 @ `2f68c1bf` (`org.vita3k.emulator`): partial (SDL in the middle)

- **File:** `Android/data/org.vita3k.emulator/files/config.yml` (`appdef::kVita3kConfig`, `nano/NanoAppFiles.h:42`):
  - `controller-binds` (15 SDL button indices);
  - `controller-axis-binds` (6) (`config.h:227-228`, `ctrl.cpp:218-303`).
  - Global only.
- **Meaning:** entry *i* is the physical SDL button driving Vita button *i*:
  - 0 SOUTH→Cross, 1 EAST→Circle, 2 WEST→Square, 3 NORTH→Triangle;
  - 4 BACK→Select, 5 GUIDE→PS, 6 START→Start;
  - 9/10 shoulders→L/R, 11-14 D-pad.
  - The defaults are the identity (`app.cpp:86-113`).
- **Android → SDL** is SDL's job (`SDL_sysjoystick.c:69-141`, SDL 3.4.16 at `fa2c02bb`): 96→SOUTH, 97→EAST, 99→WEST, 100→NORTH, 102/103 shoulders, 108 Start, 109 Back, 110 Guide, and the hat → D-pad buttons. The reader embeds this table.
- **Result on the Cube with gammapad off:** Cross bottom, Circle right, Square left, Triangle top.
- All pads are OR'd into port 1 outside PSTV mode (`ctrl.cpp:340-356`).


#### WatermelonDS 0.8.0.rc3 (`me.magnum.melondualds`): reliable

- **File:** `/data/data/me.magnum.melondualds/files/controller_config.json`, global (`SharedPreferencesSettingsRepository.kt:103`, loaded `:152-163`). If the file is missing or doesn't parse, the app uses its built-in defaults (`:159-160`). An unknown `input` name fails the whole file, so the reader must do the same.
- **Format** (`impl/dtos/input/InputConfigDto.kt:24-171`): `{"inputMapper":[{"input":"A","assignment":{"type":"key","deviceId":null,"keyCode":97},"altAssignment":{...}}], "slot2AnalogMapping":{...}}`.
  - `type` is one of `none`, `key`, `chord` (`keyCode` + `modifierKeyCode`) or `axis` (`axisCode`, `direction` POSITIVE/NEGATIVE).
  - With no `type`, it is inferred from the fields present (`:112-123`).
  - When the file exists, an input missing from it is **unbound**. Defaults are not merged in (`ControllerConfiguration.kt:40-43`).
- **Translate:**
  - A key press triggers the first input, in list order, bound to that keycode with no modifier (`ControllerConfiguration.kt:55-63`). `deviceId` is ignored for keys.
  - An axis fires past ±0.5 (`InputProcessor.kt:155-187`). Axes honour `deviceId`: when it is non-null, show "bound to another controller" unless it matches.
- **DS buttons:** A B X Y Up Down Left Right L R Start Select Hinge. The app functions (Pause, Fast Forward, Quick Save/Load, Rewind, Swap Screens, Exit…) go to the hotkeys section.
- **Defaults** (`impl/input/DefaultControllerConfigurationFactory.kt:13-25`): A=97, B=96, X=100, Y=99, D-pad = HAT_X/Y with AXIS_X/Y as the alternate, L/R=102/103, Start/Select=108/109, Pause=110 (the Anbernic button, which PWM swallows over apps). With gammapad off that puts DS A on the right button and DS B on the bottom, the correct DS positions.
- **Per game:** a ROM config with `inputMode:"CUSTOM"` + `customControllerConfiguration` (`domain/model/rom/config/RomConfig.kt:15-34`). It is in `<save dir>/<rom>.opts` (Gson `{"version":1,"a":{…}}`, `FileSystemRomsRepository.kt:506-526`), mirrored in `files/rom_data.json`. The reader checks `rom_data.json` for the launched ROM.

#### drastic-nano and `com.gammaos.drasticsf`: reliable

`com.gammaos.drasticsf` is drastic-nano's SurfaceFlinger host activity, not DraStic, so it maps to drastic-nano. drastic-nano draws the panel itself (§1.5).

- **Keymap:** properties `persist.gammaos.drastic_nano.key.<N>`, N = 0..31, value = a code or −1 (`drastic-nano/DrasticPrefs.cpp:531`, `:593`, `:636`, `:683-687`). An unset key uses the seed default. DraStic's XML is imported **once** (`drastic-nano/main.cpp:4674-4688`, while `persist.gammaos.drastic_nano.cfg_seeded` is unset), and `writePrefs` has no callers. So the "keymap shared with DraStic" lines in `HOTKEYS.md` and `mods/hotkeys/drastic.json` are stale, and need a doc fix.
- **Index:** 0 X, 1 Y, 2 B, 3 A, 4 R, 5 L, 6 Start, 7 Select, 12 Up, 13 Right, 14 Down, 15 Left, 16 Screen Swap, 17 Fast Forward, 20 Menu, 28 Touch Cursor, 29 Save State, 30 Load State, 31 Close Lid (`drastic-nano/DrasticPrefs.h:32-50`).
  - Seed defaults (`:147-158`): X=99, Y=100, B=97, A=96, R=103, L=102, Start=108, Select=109, Up..Left=19/22/20/21, Swap=104, FF=105, Menu=4, Touch Cursor=107.
- **Code encoding:** evdev → Android-style code through `kKeyTable` (`drastic-nano/DrasticPrefs.cpp:831-885`): 304→96, 305→97, 307→99, 308→100, and so on. Anything else is 1000+evdev. So **96 here is evdev 304, the right button**, and the seed puts DS A on printed A, the correct position.
- **Rules the reader must reproduce** (`drastic-nano/InputMap.cpp`):
  - a code bound twice: the higher index wins (`:216-231`);
  - an unbound code falls back to a fixed table: SOUTH→A, EAST→B, NORTH→X, WEST→Y, TL→L, TR→R, Start, Select, D-pad (`:807-837`);
  - `ABS_HAT0X/Y` always drive the D-pad (`:923-966`), and the left stick is always an extra D-pad (`:289-299`);
  - portrait controls rotate the D-pad and ABXY (`persist.gammaos.drastic_nano.portrait_controls`, `:342-396`).
- **Per game:** `<userDir>/overrides/<rom base>.cfg` with `key.N=value` lines. userDir is `persist.gammaos.drastic.data_dir`, else `/storage/emulated/0/drastic-nano` (`main.cpp:4710-4724`, `DrasticPrefs.cpp:706-731`).
- **ABXY Swap:** drastic-nano never reads it. It reads evdev, so with gammapad running its codes come from the virtual pad, and `PadModel` covers that.
- **Existing chords** in `drastic-nano/InputMap.cpp` (Anbernic = `KEY_F10`/`BTN_MODE`, `:703-710`):
  - Volume = brightness `:661-672`;
  - Select = shader `:711-721`;
  - Start = swallowed;
  - R1/L1/R2 = save/load/fast-forward (`persist.gammaos.hotkeys.*`, `:510-528`);
  - hat hotkeys `:538-559`;
  - 3 s hold = exit `:983-987`;
  - Start + Select + hotkey `:730-746`;
  - anything else is swallowed (`:722-729`).
  - **New branch** between `:721` and `:722`: `ev.value==1 && st->anbernicHeld && ev.code == southEvdev`. It marks the chord used, pushes the key to `chordKeys`, and sets a new `InputActions::buttonMap` flag (beside `shaderToggle`, `drastic-nano/InputMap.h:205-207`). That flag is consumed in both render loops next to `shaderToggle` (`drastic-nano/main.cpp:2025`, `:3720`).
- **Bug found, fix in phase 5:** drastic-nano's `hotkeyButton` maps the name `BUTTON_A` to `BTN_SOUTH` (`drastic-nano/InputMap.cpp:479-486`). On the Cube that is the right button, while PWM's `BUTTON_A` is the bottom button. A face-button universal hotkey therefore sits on different buttons in apps and in drastic-nano. Resolving through the shared `bmap::PadModel` fixes both.

#### DraStic r2.6.0.4a (`com.dsemu.drastic`, closed source): partial

- **File:** `/data/user/0/com.dsemu.drastic/shared_prefs/_Dra$t1c_Pref$_.xml`, `<int name="_KeyMapConfigs_<p>_<a>" value="keycode|-1"/>`. The path and parsing are from Thimsy's own importer (`drastic-nano/main.cpp:4650-4652`, `DrasticPrefs.cpp:165-180`, `:296-300`). The action indexes are the same as above (reverse-engineered).
- **Unknown:** which profile `p` is active. The reader assumes 0 and shows the badge "Mapping read from DraStic profile 1 (assumed)".
- **Defaults** (Android keycodes): A=96, B=97, X=99, Y=100. With gammapad off that puts DS A on the bottom button. The overlay shows this as it is: it is DraStic's choice, not a reader bug.

#### M64Plus FZ 3.0.335 (`org.mupen64plusae.v3.fzurita`): reliable, with an embedded table

Source: branch `fz-master-v335`, versionCode 335 (`build_common/version_common.gradle:11-12`).

- **Active profile:**
  - per game first: `shared_prefs/<romMd5>_preferences.xml` key `controllerProfile1Game`, where an empty value means the controller is disabled (`GamePrefs.java:309`, `:347-348`, `:428-439`);
  - then global: `shared_prefs/org.mupen64plusae.v3.fzurita_preferences.xml` key `controllerProfile1`, default **"Android Gamepad"** (`GlobalPrefs.java:410`, `:437`, `:455`).
  - The ROM's MD5 is computed by the reader when the ROM is known (cached by path + mtime).
- **Profile files:**
  - user profiles: `/data/data/org.mupen64plusae.v3.fzurita/files/Profiles/controller.cfg` (`GlobalPrefs.java:485-486`);
  - built-in profiles: an APK asset (`mupen64plus_data/profiles/controller.cfg`, `AppData.java:421`), so it is **embedded** in `retropad.json`'s sibling `m64_profiles.json`;
  - lookup order: user, then built-in, then the default (`GlobalPrefs.java:991-1006`).
- **Format:** `[Name]` then `map=idx:code,…`, last wins (`SerializableMap.java:58-77`). Negative code = axis: `axis = (−code−1)/2`, positive iff `(−code)%2==1` (`AbstractProvider.java:296-325`).
- **Index** (`AbstractController.java:76-118`):
  - 0-3 D-pad R/L/D/U, 4 Start, 5 Z, 6 B, 7 A, 8-11 C R/L/D/U, 12 R, 13 L, 16-19 analog R/L/D/U;
  - 20-35 app functions (save, load, fast forward, menu …), which go to the hotkeys section.
- **Default "Android Gamepad":**
  - A=96 (bottom), B=99 (left), Z = L2/L3/trigger axes, Start = R3/Start, L/R = L1/R1, C buttons = right stick, analog = left stick;
  - **Menu = 97 and 100** (the right and top buttons open the app menu).
- **Per device:** a profile is per player. With no player map, every pad is player 1 (`PlayerMap.java:82-86`).

#### ScummVM v2026.3.0 (`org.scummvm.scummvm`): partial

- **File:** `/data/data/org.scummvm.scummvm/files/scummvm.ini` (`appdef::kScummvmIni`, `nano/NanoAppFiles.h:41`).
  - `[keymapper]` holds `keymap_<keymapId>_<actionId>=<hwId> …`. An empty value means unbound; a missing key means the default (`keymap.cpp:282-300`).
  - The game's own keymaps are in its `[<target>]` section. Thimsy's seeded ini has no `[keymapper]` (`mods/46-scummvm-game.sh:42-54`).
- **Android → hardware ids** (`events.cpp:1256-1395`): BUTTON_A/B/X/Y→JOY_A/B/X/Y, L1/R1→shoulders, THUMBL/R→stick clicks, Start→JOY_START, Select→JOY_BACK, Mode→JOY_GUIDE; HAT→JOY_HAT_*, X/Y→left stick, Z/RZ→right stick, triggers→JOY_*_TRIGGER. L2/R2 *keys* are dropped.
- **Defaults** (engine-default + Android backend):
  - JOY_A left click, JOY_B right click, JOY_X skip line, JOY_Y Esc/skip;
  - L1 game menu (F5), R1 slow mouse, Start main menu, Select virtual keyboard;
  - the left stick moves the mouse;
  - **the hat D-pad has no default action** (`metaengine.cpp:82-158`, `backends/platform/android/android.cpp:707-747`).
  - The "console" is shown as a **pointer-and-click table** (Left click, Right click, Skip, Menu, Keyboard), not a drawing.
- **Engine-specific keymaps** need each engine's source, so the reader shows engine-default plus the user's overrides and notes "This game may add its own keys".

#### SeedlessDS 1.0.1 (`com.seedlessds.app`, GPL-3, open source): reliable

`HOTKEYS.md` calls it not installed by default.

- **File:** `shared_prefs/keymap.xml` (`KeyMap.kt:9`).
  - `_KeyMapId` selects profile 0-2 (`:46`).
  - Keys are `k_<ds>` (profile 0) or `p<n>_k_<ds>`, int keycodes, −1 = cleared (`:50-51`).
  - `_KeyMapVer` < 2 erases all `k_` keys (`:59-64`).
- **DS index:** 0 Up, 1 Down, 2 Left, 3 Right, 4 A, 5 B, 6 X, 7 Y, 8 L, 9 R, 10 Start, 11 Select. The specials `s_1..9` go to the hotkeys section.
- **Duplicates:** the first in list order wins, and buttons beat specials (`:66-82`).
- **Defaults:** A=96, B=97, X=99, Y=100, L/R=102/103, Start/Select=108/109, D-pad 19-22 (`:34-41`).
- **Fixed in code:** AXIS_X/Y and HAT drive the D-pad. With `_AnalogTriggers` (default on, `seedless_config.xml`), L2/R2 press L/R (`EmulatorActivity.kt:415-440`).
- No per-game keymap.

#### hakuX `a7cdb5a` (`com.rfandango.haku_x`): reliable

- **File:** `/data/data/com.rfandango.haku_x/files/controllers.json` (`Controllers.kt:208`, `:353-378`): `{"slots":[…], "profiles":{"u:<descriptor>"|"m:<vid4>:<pid4>:<name>":{"bind":{"A":["k96"],"LS_Y":["a1-"],…}}}}`.
- **Profile choice:** the unit's own profile, else the model's profile, else defaults built from the device (`profileFor`, `:303-304`). The gammapad virtual pad looks up `m:045e:0b13:Xbox Wireless Controller`. `bmap_dev` carries the descriptor and VID:PID for exactly this.
- **Xbox inputs:** A B X Y BLACK WHITE START BACK LTHUMB RTHUMB DPAD_* LT RT LS_X/Y RS_X/Y (`:23-27`).
- **Defaults** (`:248-294`):
  - A/B/X/Y = 96/97/99/100 (bottom/right/left/top with gammapad off);
  - BLACK = R1, WHITE = L1, START 108, BACK = Select;
  - triggers = LTRIGGER/RTRIGGER (+ keys 104/105);
  - right stick = Z/RZ when Z is signed;
  - Mode opens hakuX's menu.
- No per-game controls. Device eligibility rules: `:215-223`.

---

## 4. Controller identity

### 4.1 What each layer sees

| Setup | Android `InputDevice` the emulator binds | Name / VID:PID | Notes |
|---|---|---|---|
| Built-in pad, gammapad off (default) | the Cube's joypad | vendor's name (the RetroArch profile `retrogame_joypad.cfg`, `mods/10-update-emulators.sh:186-206`), 484b:1101 | By position (Thimsy `.kl`). D-pad = `AXIS_HAT_X/Y`, sticks `X/Y` + `Z/RZ`, triggers `LTRIGGER/RTRIGGER` (`mods/keylayout/Vendor_484b_Product_1101.kl:19-31`). The Anbernic button is `F10`/`BUTTON_MODE` |
| Built-in pad, gammapad on | "Xbox Wireless Controller" 045e:0b13 (configurable: `persist.gammaos.gamepad.device_name/vid/pid`) | By print, unless ABXY Swap is on (§1.1). With `merge=1` (default) every pad is merged into it. With `hide_source=1` (default) the physical pad is hidden from apps |
| External pad, gammapad off | the pad itself | its own name and VID:PID | Its own `.kl` (Xbox, DualShock and Switch Pro layouts are by position in AOSP). Emulators bind it per device or globally (§3) |
| External pad, gammapad on | merged into the virtual pad | 045e:0b13 | Its bottom button is `BTN_A`, the built-in pad's is `BTN_B`: the limit noted in §1.1 |

Nano reads evdev fds directly (`mInputFdNames`, `nano/NanoMenu.h:1251`). It skips the vendor's `virtual_gamepad` touch device (`nano/NanoMenuInput.cpp:3038-3041`).

### 4.2 `bmap::PadModel`: physical button → what the app receives

```cpp
enum class Phys { South, East, North, West, L1, R1, L2, R2, L3, R3, Start, Select,
                  DUp, DDown, DLeft, DRight, LStick, RStick, Home /* Anbernic */ };
struct PadModel {
  std::string drawing;                 // "rg_cube", "xbox", "playstation", "switch_pro", "generic"
  std::map<Phys, std::string> label;   // as printed: South -> "B" on the Cube, "A" on Xbox, "Cross" on PS
  std::map<Phys, Input> sends;         // Android keycode or axis/direction the app sees
};
PadModel padModel(const DeviceId&, const GamepadState&);   // pure
```

- `DeviceId` is vendor, product, name and descriptor, from `bmap_dev` or `EVIOCGID` / `EVIOCGNAME`.
- `GamepadState` is `init.svc.gammapad`, `abxy_swap`, `remap_btn`, `device_vid/pid/name`, `merge` and `hide_source`, all read once per open.
- The table in §1.1 is `padModel` for the built-in pad. It is the core of T3 in §9.2.
- Drawing selection:
  - 484b:1101, or the gammapad virtual device with `merge=1` and the built-in pad present → `rg_cube`;
  - 045e → `xbox`; 054c → `playstation`; 057e → `switch_pro`;
  - anything else → `generic` (Xbox-style positions, labels "Bottom/Right/Top/Left").
- External pads keep their own printed labels: Xbox A on the bottom, PlayStation Cross on the bottom, Switch Pro B on the bottom.

### 4.3 How emulators tell devices apart (summary; details per emulator in §3)

| Keyed by | Emulators |
|---|---|
| Device name + VID:PID (autoconfig) | RetroArch |
| Device name (+ descriptor) in a file name | Flycast (`mappings/Android_<name>[-<descriptor>].cfg`) |
| Device qualifier in the binding | Dolphin (`Device = Android/<controller number>/<name>`) |
| Descriptor, then VID:PID:name | hakuX (`u:<descriptor>`, then `m:<vid>:<pid>:<name>`) |
| Global (any pad) | PPSSPP (every pad is device 10), Azahar, WatermelonDS (keys), ARMSX2 (per player), M64Plus FZ (per player), SeedlessDS, ScummVM, Vita3K (OR'd), drastic-nano |

The reader gets the `DeviceId` of the pad in hand. Where the emulator binds per device, it uses that device's section, file or profile. Where the emulator binds globally, the device doesn't matter.

---

## 5. ABXY Swap

- **Where:** Settings → Gamepad Settings → **ABXY Swap** (`nano/NanoMenuPS3Data.h:349`; tree row `nano/NanoMenuSettingsTree.cpp:555-556`; binding `nano/NanoMenuPS3Menu.cpp:9651`).
- **Property:** `persist.gammaos.gamepad.abxy_swap` (0/1). Writing it bumps gammapad's `config_version`, which makes gammapad reload (`gammapad/README.md`, Hot Reload).
- **What it changes:** only gammapad's event stream. It swaps `BTN_A`↔`BTN_B` and `BTN_X`↔`BTN_Y` before the user's `remap_btn` (`gammapad/InputTransformer.cpp:268-274`). It does **not** edit any emulator or RetroArch config, and it does **nothing** while gammapad is stopped. Nano checks `init.svc.gammapad == running` for exactly this reason (`abxySwapApplied`, `nano/NanoMenuPS3Menu.cpp:10873-10881`).
- **Nano's prompts:** with the swap applied, `abxyText` swaps the A/B/X/Y letters in hints, and `themeButtonText` / `drawFaceGlyph` swap Confirm/Cancel and Square/Triangle (`nano/NanoMenuPS3Menu.cpp:10906-10990`). The separate **OK Button** setting (`persist.gammaos.nano.face_swap`) relabels only Confirm/Cancel and doesn't change input (`:10949-10952`).
- **How the overlay reflects it:**
  1. The emulator reader gives console button → Android input (what the emulator binds).
  2. `PadModel.sends` inverted gives Android input → physical button, using the *current* gammapad state including the swap. With gammapad on, the swap moves which physical button sends `BUTTON_A`, so the labels move with it.
  3. Labels are always the **printed** names of the pad in hand. On the Cube: right = A, bottom = B, top = X, left = Y.
  4. Nano's `abxyText` is *not* applied to the overlay. That function rewrites prompts that were written in Nano-input terms, while the overlay's labels are already physical. The one shared piece is the header hint ("B Close"): its glyph is drawn with `drawFaceGlyph(1 /*Cancel*/)`, so it shows the button that closes, as every other Nano legend does.
- **Example** (SNES on Snes9x, RetroArch, default binds):

| Setting | Bottom (B) | Right (A) | Top (X) | Left (Y) |
|---|---|---|---|---|
| gammapad off (default) | SNES B | SNES A | SNES X | SNES Y |
| gammapad on, swap off | SNES A | SNES B | SNES Y | SNES X |
| gammapad on, swap on | SNES B | SNES A | SNES X | SNES Y |

  Thimsy's RetroArch player 1 binds pin `BUTTON_A`(96) to RetroPad B (the bottom position) for whatever pad is player 1 (RA.cfg:1432-1580, §3.1). With gammapad on and swap off, 96 comes from printed A (right). That is what `README.md:103-105` means by "apps see the buttons by their print again". The test T3 in §9.2 pins this table.

---

## 6. Data model and code layout

### 6.1 Types

```cpp
namespace bmap {
struct Input { enum Kind { kKey, kAxis, kNone } kind; int code; int dir; };  // Android keycode, or axis + sign
struct EmuBind { std::string consoleId; Input in; };               // what a reader returns
struct Row {                                                       // what the overlay shows
  std::string consoleId;          // == the SVG anchor id ("btn-a", "dpad-up", "stick-l", "c-up" ...)
  std::string consoleName;        // "A", "C-Up", "Cross" (from consoles.json; translatable where generic)
  std::vector<Phys> phys;         // physical buttons on the pad in hand; empty = unbound
  bool offPad = false;            // bound, but to an input this pad doesn't have (a keyboard key, BUTTON_C)
};
struct MapResult {
  std::string console;            // "snes", "n64", "ps2" ...
  std::string drawing;            // console art file, "" = table only
  std::vector<Row> rows;
  enum Source { kConfig, kDefaults, kUnknown } source;
  std::vector<std::string> notes; // "Per-game remap: Super Metroid.rmp", "Wii Remote: sideways"
  std::vector<HotkeyRow> hotkeys; // §7.3
};
struct ReaderEnv { std::string root; DeviceId dev; };             // root "" on device, a fixture dir in tests
using Reader = std::vector<EmuBind> (*)(const ReaderEnv&, const Context&, Source* src, std::vector<std::string>* notes);
MapResult build(const ReaderEnv&, const Context&, const PadModel&);   // reader + consoles.json + PadModel inversion
}
```

`build` does the following:
- picks the reader by `Context.emulator`;
- loads the console definition (button list, drawing, names);
- inverts `PadModel.sends` to label each `EmuBind` with physical buttons (one emulator input can come from two physical buttons, or from none);
- sorts rows in the console's canonical order.

### 6.2 Files

| File | What | Host-testable |
|---|---|---|
| `nano/NanoButtonMap.h/.cpp` | `Context`, `resolveContext`, `PadModel`, `padModel`, `southKeycode/southEvdev`, `sameButton`, `build`, browser list | yes, no Android deps (like `NanoLaunch`) |
| `nano/NanoButtonMapReaders.cpp` | one reader per emulator (§3), sharing small INI / cfg / XML-prefs / JSON parsers | yes. Reuses `NanoAppFiles` (`sdPrefsGet`, `sdPrefsGetInt`, `sdIniKey`, `ppssppMemstickAt(root)`, `flycastHomeAt(root)`, `drasticSystemDirAt(root)`; `nano/NanoAppFiles.h:60-104`), `NanoJson`, and `nanohotkeys::loadProfiles` (hotkey section) |
| `nano/NanoButtonMapArt.h/.cpp` | `layout(scene, W, H)`, and `render(scene, W, H, rgba)` with nanosvg + FreeType (the `bootscreen/NanoBootArt` pattern, `nano/bootscreen/NanoBootArt.h:27-58`) | yes |
| `nano/NanoMenuButtonMap.cpp` | `NanoMenu::buttonMapOpen/Close/Key/render`: gathers inputs, calls the above, uploads the RGBA as a GL texture, handles input | device |
| `nano/tools/nano_buttonmap.cpp` | host tool `nano-buttonmap SCENE.json FONTDIR ARTDIR W H OUT.rgba` | — |
| `nano/ps3xmb/buttonmap/consoles.json` | console → buttons (id, name, canonical order), drawing file, layout hints | data |
| `nano/ps3xmb/buttonmap/systems.json` | Nano system id → console (`nes`/`fds`→`nes`, `gb`/`gbc`→`gb`, `genesis`/`segacd`/`sega32x`→`md6`/`md3` per core option ...) | data |
| `nano/ps3xmb/buttonmap/retropad.json` | RetroArch core → console + RetroPad → console button, with core-option variants (§3.1) | data |
| `nano/ps3xmb/buttonmap/browser.json` | browser control list (§8) | data |
| `nano/ps3xmb/buttonmap/art/*.svg` | the controller-art catalogue (`~/thimsy/work/controller-art/`) plus the pad drawings (`rg_cube`, `xbox`, `playstation`, `switch_pro`, `generic`) | data |

**As built in phases 1-2** (differences from the table above):
- `consoles.json` and `systems.json` were not needed: the controller-art catalogue's `controllers.json` already names every console's buttons (anchors with ids, names, sides and label points), the Nano systems and platforms each console covers, and the aliases. `bmap::loadCatalog` reads it plus `retropad.json` and `browser.json`.
- The art is installed from `nano/ps3xmb/controllers/` (`svg/*.svg`, `controllers.json`, `LICENSE`) as `nano_xmb_controllers` and `nano_xmb_controllers_svg` → `/system/etc/nano_xmb/controllers/`, not `buttonmap/art/`.
- There are no pad drawings yet (`rg_cube`, `xbox` ...), so the browser list is table-only.
- `nano/NanoButtonMapNav.h/.cpp` holds the navigation state machine (§6.5).
- The readers do not go through `NanoAppFiles`' helpers: they take a test root, so they use their own bounded `readAppFile` (the `nanobios::safePath` / `appOwnerUid` / `openDir` checks, `O_NOFOLLOW`, a size cap) and small INI / cfg / XML-prefs parsers. The ones that read `NanoAppFiles` paths keep the same locations.
- Results report `Source::kNotMapped` (Dolphin, Azahar with no binds) as well as config / defaults / unknown.

Install as `prebuilt_etc` `nano_xmb_buttonmap` → `/system/etc/nano_xmb/buttonmap/`, checking `/data/system/nano_xmb/buttonmap/` first like the other `nano_xmb` assets (`nano/Android.bp:430-436` comment). Add it to `gammaos-nano`'s `required`.

### 6.3 The art contract (for the controller-art agent)

- One SVG per console (`nes.svg`, `snes.svg`, `n64.svg`, `gc.svg`, `ps1.svg`, `ps2.svg`, `psp.svg`, `md3.svg`, `md6.svg`, `saturn.svg`, `dc.svg`, `3ds.svg`, `nds.svg`, `gba.svg`, `gb.svg`, `vb.svg`, `pce.svg`, `ngp.svg`, `ws.svg`, `lynx.svg`, `a2600.svg`, `a5200.svg`, `a7800.svg`, `coleco.svg`, `msx.svg`, `3do.svg`, `xbox.svg`, `vita.svg`, `arcade6.svg` …). Plus the pad drawings.
- **Anchors** are elements whose `id` equals a console button id. The anchor point is the centre of the element's bounding box. Ids:
  - face: `btn-a btn-b btn-c btn-x btn-y btn-z btn-1 btn-2 btn-i btn-ii btn-cross btn-circle btn-square btn-triangle`
  - shoulders: `btn-l btn-r btn-zl btn-zr btn-l1 btn-l2 btn-r1 btn-r2 btn-l3 btn-r3 trigger-l trigger-r btn-z`
  - system: `btn-start btn-select btn-mode btn-home btn-power`
  - D-pad: `dpad-up dpad-down dpad-left dpad-right`, or a single `dpad`
  - N64: `c-up c-down c-left c-right`
  - Virtual Boy: `dpad2-up dpad2-down dpad2-left dpad2-right`
  - sticks: `stick-l stick-r stick-c`
  - arcade: `btn-1`…`btn-6`, `btn-coin`
- nanosvg keeps each shape's `id`, or its group's (`NSVGshape::id`, `nano/nanosvg/nanosvg.h:147`), and its `bounds`. So the anchor box for an id is the union of the bounds of the shapes carrying that id: one walk over `image->shapes`, no second parser. A group `<g id="btn-a">` works, because shapes inherit the group's id.
- The overlay draws labels **outside** the drawing with leader lines, never on top of it, so the art needs no free space. An anchor missing from the SVG means its row goes to the table strip under the drawing (counted in the layout tests).
- Recommended viewBox aspect is between 1.4:1 and 2.2:1, which most pads are. A taller drawing (DS, 3DS, Vita portrait) is fine: the layout gives it a column.
- Licence: only art under a free licence that allows redistribution and modification (CC0, CC-BY, CC-BY-SA, MIT). Credit goes in `ATTRIBUTION.md`, per the asset decisions (credit the artists).

### 6.4 Fallback table when there is no drawing

- If `MapResult.drawing` is empty, the file doesn't parse, or the user picked **Table** in the overlay (Y toggles the view), the panel shows a two-column table: **Console button | Your controller**. Rows are in canonical order. Unbound rows show "—", and off-pad rows show "Keyboard F2" and similar.
- Long tables use two column pairs side by side when the width allows (§7.2).
- The table is also what screen readers or the Information-style fallback would use. It exists in every case, so the layout tests always cover it.

### 6.5 Moving around the panel (navigation and touch)

`bmap::Nav` (`nano/NanoButtonMapNav.h`) is a pure state machine over the laid-out body: the items' tops and heights, the first item of each section (Controls, Hotkeys), the visible height, and the panel and body rectangles. `bmart::Layout::nav()` produces that model. All of it is unit-tested on the host (`ButtonMapNav*`, `ButtonMapNavInput*`, `ButtonMapTouch*` in `gammaos_nano_buttonmap_tests`).

**Buttons, by position.** `bmap::NavInput` takes physical buttons (`Phys`) from the pad model (§4.2), so it never assumes the Cube's keycodes:

| Input | Action |
|---|---|
| D-pad up / down, left stick up / down | one item: a table row, the drawing, a section title |
| D-pad left / right, left stick left / right | the previous / next section |
| L1 / R1 | one page up / down |
| B (the pad's Back: the button sending `BTN_EAST`), Start, the chord again | close |
| Y (the button sending `BTN_WEST`) | table / drawing |

- **Edge-triggered, with repeat.** A press acts once; held, it repeats after 350 ms every 90 ms. A second direction replaces the held one; releasing it stops the repeat.
- **Stick.** It counts past a 0.5 deadzone and stops below 0.35 (hysteresis, so a resting stick near the edge doesn't chatter). The larger axis wins. Moving to a new direction acts at once.
- **No movement past the ends.** Up at the top, down at the bottom, previous in the first section, next in the last do nothing (and report no movement, so the caller needn't redraw).
- **Pages.** Each page is as many whole items as fit, starting at the first item that didn't fit on the previous one. Down by item stops at item tops; the last stop is the last page start, so the last page is always shown whole. The footer shows the page indicator "2/3" and scroll hints (an up / down arrow when there is more above / below).

**Touch** (the NanoTouch conventions, scaled by `unit = min(W, H)`):
- **Drag.** Past the slop (`max(10, unit / 60)`), a vertical drag scrolls the body, with the content following the finger. A drag that starts across (more horizontal than vertical) becomes a swipe instead.
- **Fling.** On release, the finger's velocity (px / ms, over the last moves; stale after 80 ms) coasts on with friction `v *= exp(-dt / 325)`. It starts only above 0.3 px / ms, stops below 0.05, and stops at either end (clamped, no overscroll). A new touch stops a fling without counting as a tap.
- **Swipe.** A horizontal drag of at least `unit * 0.08` that is more across than down: left is the next section, right the previous one.
- **Tap.** A touch that never passes the slop: outside the panel it closes the map; inside it does nothing.
- **Coordinates.** Touch positions are mapped through the layout transform (`setTouchSpace(touchW, touchH, layoutW, layoutH)`), so a touch panel in another orientation or resolution than the rendered frame lands right.
- **More than one finger** cancels the gesture: the two- and three-finger taps stay Nano's.

The frame is rendered at the scroll position. While moving (drag, fling), items cut by the body edge are drawn clipped; at rest they are hidden, so a still frame never shows a half row.

---

## 7. Overlay UI and layout

### 7.1 Look and behaviour

- **A modal over any theme.** It is drawn in the same slot as the BIOS dialog, which is the existing "modal over any theme" (`renderBiosDialog`, `nano/NanoMenuRender.cpp:6947`). It uses the same per-theme palette:
  - XMB and ES-DE: dark panel 0.07/0.08/0.10 at 94 %, accent bar, square corners;
  - DSi: message-box colours from `ndsPal()`, rounded;
  - Minima: card colours, rounded.
  - See `nano/NanoBiosUi.cpp:804-858`. The palette is passed into `NanoButtonMapArt` as part of the scene, so the CPU-drawn panel matches.
- **Over a game.** The overlay layer is up with `drop_input=1`, so the game gets no input (`nano/NanoMenuOverlay.cpp:559-564`), the same as the Quick Menu.
- **No pause (decided).** The map does not pause the game: the game keeps running under the scrim with its input dropped, exactly as under the Quick Menu (`overlayPauseApp` is gated off, `persist.gammaos.nano.overlay.pause`, `nano/NanoMenuOverlay.cpp:333-344`). drastic-nano does the same. `NanoPauseActivity` is not used.
- **Picture.** The CPU renderer draws the whole panel once per open, and again on page / view / theme change, into an RGBA buffer. It is uploaded as one texture and drawn over a dim quad (`drawQuad(0,0,W,H,0,0,0,dimA)`). Nothing animates per frame except the fade-in, so the cost is one rasterisation, about 5-20 ms on the A55 at 720x720, done on open.
- **Header.** Console name, then "emulator · core" (for example "SNES · RetroArch Snes9x"), then the pad in hand ("RG Cube" / "Xbox Wireless Controller"). Under the header there is a one-line source badge: "From your RetroArch settings" / "Default mapping" / "Per-game remap: <name>".
- **Body.** The drawing with labels (§7.2), or the table.
- **Footer hints** use Nano's glyphs: "B Close · Y Table/Drawing · L1/R1 Page", with the page indicator "2/3" and up / down scroll hints (`drawFaceGlyph` roles, so swaps and the OK Button setting are respected).
- **Input.** As §6.5: D-pad and left stick move, L1 / R1 page, left / right change section, B / Start / the chord close, Y toggles drawing and table; touch drags, flings, swipes and closes on a tap outside. The hint glyphs are tappable (`touchHelpPrompt`, `nano/NanoMenuInput.cpp:1429-1440`). Everything else is swallowed.

### 7.2 Layout scaling (the new requirement)

`bmart::layout(scene, W, H, metrics)` is pure and deterministic. It returns integer rectangles for the panel, every text run, every leader line and the drawing box. Nothing is drawn outside these rectangles, which is what makes the layout assertions possible.

**Sizes**
- `unit = min(W, H)`, `aspect = W / H`.
- `body = clamp(round(unit / 30) * userFontScale, 14, 40)`. That gives 640x480 → 16, 960x544 → 18, 720x720 / 1280x720 → 24, 1024x768 → 26, 1920x1080 / 1080x1920 → 36.
- `title = round(body * 1.3)`, `small = max(13, round(body * 0.85))`.
- Padding `p = round(body * 0.75)`.
- Row height `rh = round(body * 1.55)`.
- 14 px is the floor. Fitting may shrink text in 8 % steps but never below 14 / 13 px. Once the floor is reached, rows move to the table strip and then to page 2. The text is never clipped and never ellipsised.

**Panel**

| Screen class | Rule | Examples |
|---|---|---|
| 16:9 and wider (`aspect >= 1.70`) | **Centred**: `ph = round(H * 0.90)`, `pw = min(round(W * 0.80), round(ph * 1.5))`, centred both ways | 1280x720 → 972x648 at (154,36). 1920x1080 → 1458x972. 960x544 → 735x490. 854x480 → 648x432 |
| Every other aspect (4:3, 5:3, 16:10, 1:1, 3:2, portrait) | **Full width**: `m = max(4, round(unit * 0.02))`, `pw = W - 2m`. Height is `H - 2m` in landscape and square. In portrait it is `min(H - 2m, content height)`, centred vertically | 640x480 → 620x460. 720x720 → 692x692. 1024x768 → 994x738. 800x480 → 780x460. 1080x1920 → 1036 x content (the margin is `round(unit * 0.02)`: 10, 14, 15 and 10 px) |

**Content inside the panel**
1. Header (title + subtitle + badge), then the body, then the footer.
2. **Drawing mode, landscape and square.**
   - Label gutters go left and right of the drawing. Gutter width = the widest label + glyph chip + `p`, clamped to 18-32 % of the body width each.
   - The drawing gets the middle, scaled to fit width and height, keeping its aspect.
   - An anchor left of the drawing's centre line is labelled in the left gutter, one right of it in the right gutter. Centre anchors (Start/Select/Home) go to the side with fewer labels.
   - Each side's labels are sorted by anchor y and placed at their anchor's y. A 1-D pass then pushes them apart to `rh` spacing, clamped to the body box.
   - Leader lines run from the label's inner edge to the anchor centre.
   - If one side overflows at the floor size, its lowest-priority rows (system buttons, then the stick clicks) move to a one-line strip under the drawing.
3. **Drawing mode, portrait** (`aspect < 0.8`): the drawing goes full body width at the top, with gutters at 22 % each. The table strip and the hotkeys go below.
4. **Table mode.**
   - `pairW` = the widest console name + the widest physical label + `3p`.
   - The number of column pairs is `k = min(floor(bodyW / pairW), ceil(rows * rh / bodyH))`, at least 1.
   - Rows fill column by column.
   - A D-pad whose four directions map to the four physical directions collapses to one row, "D-pad → D-pad", and likewise for sticks.
5. **Hotkeys section** (§7.3): placed under the body if it fits at the current size, otherwise on page 2. On 1920x1080 and 1024x768 most consoles fit on one page; at 640x480 most need two.

**Expected results, checked by tests (§9.2 T5/T6)**

| Size | Panel | body px | SNES drawing | N64 table | PS2 table + hotkeys | Browser |
|---|---|---|---|---|---|---|
| 640x480 | full width 620x460 | 16 | 1 page | 2 column pairs, 1 page | 2 pages | 1 page |
| 720x720 | full width 692x692 | 24 | 2 pages | 2 pairs, 2 pages | 2 pages | 2 pages |
| 800x480 | full width 780x460 | 16 | 1 page | 1 page | 2 pages | 1 page |
| 854x480 | centred 648x432 | 16 | 1 page | 1 page | 2 pages | 1 page |
| 960x544 | centred 735x490 | 18 | 1 page | 1 page | 2 pages | 1 page |
| 1024x768 | full width 994x738 | 26 | 1 page | 1 page | 2 pages | 1 page |
| 1280x720 | centred 972x648 | 24 | 1 page | 1 page | 2 pages | 1 page |
| 1280x800 | full width 1248x768 | 27 | 1 page | 1 page | 2 pages | 1 page |
| 1920x1080 | centred 1458x972 | 36 | 1 page | 1 page | 2 pages | 1 page |
| 1080x1920 | full width 1036x1333 (content) | 36 | 1 page | 1 page | 1 page | 1 page |
| 720x1280 | full width 692x885 (content) | 24 | 1 page | 1 page | 1 page | 1 page |

Measured in phase 2 (`nano-buttonmap --dump` on the golden scenes). The design targets were 1 page for the SNES drawing everywhere and 1 page for PS2 + hotkeys from 1024x768 up; with the font formula as written, 720x720 (24 px on a 692 px panel) needs a second page for the SNES hotkeys, and the PS2 scene's 11 hotkeys take a second page at every landscape size. Both page with L1 / R1 and the "2/2" indicator (§6.5). The tests assert the *rules* (centred vs full width, the floor, nothing clipped or overlapping, every row shown). The page counts are blessed with the goldens.

### 7.3 Hotkeys section

Built from the emulator's hotkey profile (`nanohotkeys::loadProfiles`, `nano/NanoHotkeys.h:20-22`; profile format in `HOTKEYS.md` "Profiles") and the current chord buttons (`persist.gammaos.hotkeys.*`, `nano/NanoMenuPS3Menu.cpp:9661-9668`):
- Universal: "Anbernic + R1 — Save state", "Anbernic + L1 — Load state", "Anbernic + R2 — Fast forward", "Anbernic + L2 — Rewind", "Hold Anbernic 3 s — Quit". This is only the actions whose driver isn't `none` for that profile, the same rule as Emulator Support (`nano/NanoMenuPS3Menu.cpp:2713-2735`). "Start + Select + …" is added when `persist.gammaos.hotkeys.startselect` is on.
- The fixed chords: "Anbernic — Quick Menu", "Anbernic + Start — Applications", "Anbernic + Select — Shader on/off", "Anbernic + Volume — Brightness", and the button map chord itself.
- The emulator's own in-game hotkeys, when the reader found them in the emulator's config:
  - RetroArch `input_menu_toggle_btn` / `input_enable_hotkey_btn` plus the hotkey binds;
  - PPSSPP `[ControlMapping]` Pause / Rewind;
  - Dolphin `Hotkeys.ini`;
  - drastic-nano's Menu binding.
  These are shown with physical labels through the same `PadModel` inversion. Keyboard-only binds the profile provisions (F2/F4…) are *not* shown: the user can't press them.

### 7.4 Translation

- New strings go through `trDyn` (`nano/NanoI18n.h:31`): the title words ("Button Map", "Controls", "Hotkeys", "Browser Controls"), the badge texts, the hint words, the generic button names ("D-pad", "Left stick", "Right stick", "Start", "Select", "Up/Down/Left/Right"), the action names (Save state …) and the browser actions (§8).
- Console button *letters and symbols* (A, B, C-Up, Cross, L2, Z) aren't translated. They are printed on the hardware.
- Every key goes into all 21 `nano/ps3xmb/i18n/*.json`. `gammaos_nano_i18n_check` enforces this (`nano/Android.bp:841-850`).
- RTL (ar, he): the text goes through `NanoBidi` (already in `drastic_nano_shared_srcs` and the unit tests). In table mode the columns mirror. In drawing mode the gutters keep their side, because they point at physical places.
- CJK, Thai, Arabic and Hebrew use the same face fallback chain Nano uses for `drawText` (`nano/NanoMenuRender.cpp:4700-4728`).
- One golden scene runs in Hebrew to pin the RTL path (§9.3).

---

## 8. Browser controls

**Source of truth:** gammapad's `BrowserMode` (`gammapad/BrowserMode.cpp`; description `gammapad/BrowserMode.h:18-26`; user docs `README.md:450-482`). It is active only while all of these hold:
- gammapad is running: **Controller Enable** on (`persist.gammaos.gamepad.enable`, default `0`, `nano/NanoMenuPS3Menu.cpp:9645`; `gammapad/gammapad.rc`);
- **Controller Bindings** is on (`sys.gammaos.bindings.enabled`, on at boot; `gammapad/GamepadManager.cpp:1143-1147`);
- the foreground package matches `persist.gammaos.gamepad.browser_pkgs`;
- Nano's overlay and keyboard are not up.

The buttons are gammapad's **transformed** codes: after the by-print vendor layout, ABXY Swap and `remap_btn`. So the physical labels come from the same `PadModel` (§4.2).

| Control (gammapad code) | Action | Source |
|---|---|---|
| `BTN_A` | Enter. In cursor mode: left click (hold to drag) | `BrowserMode.cpp:338-348` |
| `BTN_B` | Back (closes menus) | `:350-357` |
| `BTN_X` | Refresh (Ctrl+R). In cursor mode: right click | `:359-369` |
| `BTN_Y` | Bookmark the page (Ctrl+D; Ctrl+B in Fulguris) | `:371-372`, `:69` |
| L1 / R1 | Previous / next tab (Ctrl+Shift+Tab / Ctrl+Tab) | `:374-378` |
| R2 / L2 | Left / right click, hold to drag (the triggers' axis or button) | `:380-389`, `:461-480` |
| L3 / R3 | Close tab / new tab (Ctrl+W / Ctrl+T) | `:391-395` |
| Left stick | Scroll | `tick`, `:510-518` |
| Right stick | Mouse cursor | `:500-508` |
| D-pad | Arrow keys | `refreshArrows`, `:446-459` |
| Start (tap) | Address bar (Ctrl+L) | `:306-313` |
| Select (tap) | Bookmarks (Ctrl+Shift+B) | `:299-305` |
| Select + L1 / R1 | Close tab / new tab | `:318-326` |
| Select + D-pad | Scroll | `:512-517` |
| Select + A | Cursor mode on/off (toast) | `:327-333` |
| Cursor mode: D-pad | Move the cursor, faster the longer it's held | `:519-522` |
| Start + Select held | Passes through to the system (hotkeys, hold to quit) | `startChord`, `:255-265` |
| Anbernic + … | The Anbernic chords as everywhere | `:191-214` |

**Overlay behaviour**
- In a browser the chord shows this list. The title is "Browser Controls · <Brave|Fulguris>" and the pad drawing is labelled with the actions, or the table is shown.
- **If the controls aren't active**, the list is still shown, greyed, with one line saying why:
  - gammapad not running: "Browser controls need Controller Enable (Settings → Gamepad Settings)";
  - bindings off: "Controller Bindings is off (Quick Menu)".
  - Today the default install has Controller Enable off, so out of the box the browser gets the raw pad. **Finding for the README:** `README.md:450-452` reads as if the controls are always there.
  - **Decided:** the map never turns gammapad on, not even for browsers (`bmap::kTurnsOnGammapad = false`); the greyed list and the hint are the answer. `bmap::browserState` / `browserHint` hold it.
- In any other app (and Android games) the chord does nothing. Mouse mode in other apps (Select + R1 held, `gammapad/MouseMode.cpp:23-26`) is out of scope. It is a candidate for a later "Mouse controls" list using the same mechanism.
- `browser.json` holds the rows as `{gammapadCode | axis, actionKey}`, so a change to `BrowserMode` is a data change. A unit test (§9.2 T7) checks that `browser.json` and `BrowserMode::onButton`'s cases agree, by scanning `BrowserMode.cpp` for `case BTN_*` labels. It is a cheap drift guard.

---

## 9. Tests, phases and the device checklist

### 9.1 Test modules

| Module | Kind | New or extended | What goes in |
|---|---|---|---|
| `gammaos_nano_unit_tests` (`nano/Android.bp:723-758`) | `cc_test_host`, gtest | **extend** | `test/unit/NanoButtonMap_test.cpp` (T1-T4, T7, T8) and `test/unit/NanoButtonMapReaders_test.cpp` (T1, T2). Add `NanoButtonMap.cpp` and `NanoButtonMapReaders.cpp` to `srcs` (`NanoAppFiles.cpp` and `NanoJson.cpp` are already there). `NanoHotkeys.cpp` stays out, because it uses `cutils/properties`. The hotkey rows come from a pure `bmap::hotkeyRows(profileJson, props)`, which is tested with a profile JSON fixture, and the caller feeds it `nanohotkeys::loadProfiles()`. The fixtures go under `test/fixtures/buttonmap/`, which the existing `data: ["test/fixtures/**/*"]` already ships. Add `ps3xmb/buttonmap/*.json` to `data` (as `ps3xmb/daijishou/*.json` is) |
| `gammaos_nano_buttonmap_tests` | `cc_test_host`, gtest | **new** | `test/buttonmap/NanoButtonMapLayout_test.cpp` (T5). It links `NanoButtonMapArt.cpp`, `NanoButtonMap.cpp`, `NanoBidi.cpp`, `NanoJson.cpp` and `static_libs: ["libnanosvg_nano", "libft2.nodep"]`. These aren't in the unit-test module today, and the layout measures real text. `data`: `ps3xmb/buttonmap/**/*`, `:recovery_roboto-fonts_dep`, plus one Hebrew font for the RTL case. `defaults: ["gammaos_nano_test_defaults"]` (ASan + UBSan, `-Werror`) |
| `nano-buttonmap` | `cc_binary_host` | **new** | `tools/nano_buttonmap.cpp`, which renders one scene JSON at W×H to raw RGBA (the `nano-bootlogo` pattern, `nano/Android.bp:269-277`) |
| `nano_buttonmap_golden_test` | `sh_test_host` | **new** | `test/buttonmap/buttonmap_golden_test.sh` (T6), a copy of `test/bootlogo/bootlogo_golden_test.sh` (same `--bless` / `--keep DIR`, same installed-vs-source lookup). `data_bins: ["nano-buttonmap"]`, `data`: the scenes, the art, the fonts and `test/buttonmap/goldens.sha256` |
| `gammaos_nano_i18n_check` | `python_test_host` | unchanged | catches a missing key in any of the 21 languages |
| `gammaos_nano_buttonmap_fuzzer` | `cc_fuzz` (`gammaos_nano_fuzz_defaults`) | **new, optional** | one fuzzer over the readers. Each reader parses a file an app writes (untrusted), so they are attack surface for Nano, which runs as root-ish |

All new host modules go in `TEST_MAPPING` presubmit with the others, and in the `test/README.md` table.

### 9.2 Unit and layout tests

**T1. Per-emulator readers** (`NanoButtonMapReaders_test.cpp`, fixtures `test/fixtures/buttonmap/<emu>/<case>/`)
- Each case is a small root tree laid out like the device (`data/data/<pkg>/…`, `storage/emulated/0/…`), so the readers run unchanged with `ReaderEnv.root` = the fixture dir. This is the same convention as `ppssppMemstickAt(root)` in `nano/NanoAppFiles.h:99-103`.
- Next to each tree is `expected.txt`: one `consoleId=input` line per row, plus `source=config|defaults` and the notes.
- Cases, at least:
  - **RetroArch:** shipped `retroarch.cfg` + `retrogame_joypad.cfg` (copied from the payload) → the defaults; a `retroarch.cfg` with `input_player1_a_btn` overridden; a gammapad virtual device with the "Xbox Wireless Controller" autoconfig; a core override `config/Snes9x/Snes9x.cfg`; a remap `config/remaps/Snes9x/Snes9x.rmp`, a content-dir `.rmp` and a game `.rmp` (precedence: game > content dir > core); `input_libretro_device_p1` analog vs digital for Beetle PSX; unknown core → RetroPad names.
  - **PPSSPP:** shipped `controls.ini` (by position, from the payload after `27-controller-layout.sh`); the user's additions with several devices (`10-`, `1-`, `0-`); axis binds; missing file → defaults; per-game controls profile.
  - **Dolphin:** the shipped `GCPadNew.ini`; a profile named in `GameSettings/<ID>.ini`; Wii Remote; a device-name mismatch (bound to another pad) → "not this pad".
  - **Flycast:** `Android_<name>.cfg` for the Cube's name and for "Xbox Wireless Controller"; missing → defaults.
  - **Azahar, ARMSX2:** XML prefs present and partially missing.
  - **WatermelonDS:** `controller_config.json` with `key`, `axis` and `chord` entries; a missing file → defaults; an unknown input name → defaults (it fails to parse in the app too); a per-ROM `.opts` with `inputMode: CUSTOM`.
  - **drastic-nano:** properties fixture (a `props.txt` the test feeds through a property-reader seam) + an `overrides/<rom>.cfg`; a code bound twice (higher index wins); hat and stick always D-pad.
  - **DraStic app:** `_Dra$t1c_Pref$_.xml` (the `$` in the name is a real fixture file name).
  - **M64Plus FZ:** global and per-game (`<md5>_preferences.xml`) profile choice; the user `controller.cfg`; the built-in "Android Gamepad" (embedded table); negative axis codes.
  - **SeedlessDS:** `_KeyMapId` 0 and 2; `_KeyMapVer` < 2.
  - **hakuX:** a unit profile vs a model profile vs device-derived defaults.
  - **ScummVM:** no `[keymapper]` → defaults; a user `keymap_engine-default_LCLK`.
  - **Vita3K:** fixed defaults.
- Every fixture file is either an excerpt of a real shipped config (RetroArch, PPSSPP from the payloads) or written from the format in §3. None is generated by the code under test.

**T2. RetroArch resolution** (same file)
- autoconfig matching by name and VID:PID, and what happens with no match;
- `retroarch.cfg` player binds vs the autoconfig (§3.1 precedence);
- the three `.rmp` levels;
- RetroPad → console button for every core in `retropad.json` and every core in `mods/retroarch-cores.txt`, so a core added to the build without a table entry fails the test, as the daijishou player check does;
- core options that change the mapping (Genesis Plus GX 3/6 button, Beetle PSX analog, mupen64plus C-buttons).

**T3. Pad model and ABXY Swap** (`NanoButtonMap_test.cpp`)
- `padModel` for: the built-in pad with gammapad off; gammapad on with swap off; gammapad on with swap on; `remap_btn = "304:305,305:304"` on top of the swap; Xbox 045e, PlayStation 054c, Switch Pro 057e, and an unknown pad. Each asserts the §1.1 table and the printed labels.
- `southKeycode` / `southEvdev` / `sameButton` for the same matrix.
- The SNES example in §5 end to end: RetroArch fixture + each pad state → labels.
- Swap with gammapad **stopped** → no change (`abxySwapApplied` semantics).

**T4. Context resolution** (`NanoButtonMap_test.cpp`)
- Table-driven `ContextInputs → Context`:
  - SNES system → RetroArch/snes9x;
  - a SNES game with a per-game emulator (Game Options) → that emulator;
  - a Recently Played PSP entry → PPSSPP;
  - the Android Games system and one of its games → `kNone`;
  - Applications or Settings → `kNone`;
  - a browser row in Applications → `kBrowser`.
- Over an app:
  - `com.retroarch.aarch64` with `launch_core=…/mgba_libretro_android.so` → mgba / gba;
  - RetroArch with no `launch_core`, or with a newer `content_history.lpl` entry → that entry's core;
  - `org.ppsspp.ppsspp` → psp;
  - `org.dolphinemu.dolphinemu` with `launch_system=wii` → wii;
  - `com.brave.browser` and `net.slions.fulguris.debug` → `kBrowser`;
  - `com.shatteredpixel.shatteredpixeldungeon` → `kNone`;
  - an unknown package matching only the `"*"` profile → `kNone`;
  - `com.gammaos.drasticsf` → drastic-nano (its host activity).
- The `content_history.lpl` parser gets its own cases: newest entry, an empty list, a malformed JSON, and an entry older than the launch.

**T5. Layout scaling** (`NanoButtonMapLayout_test.cpp`, new module)
- Sizes: 640x480, 720x720, 800x480, 854x480, 960x544, 1024x768, 1280x720, 1280x800, 1920x1080, 1080x1920, 720x1280.
- Scenes: SNES drawing, N64 table, PS2 table + hotkeys, browser list, a DS (tall drawing), a 6-button arcade, the longest English console names, and one Hebrew table.
- Assertions per (scene, size):
  1. **Panel rule:** `aspect >= 1.70` → the panel is centred (|centre − screen centre| ≤ 1 px both ways) and `pw ≤ 0.80 W`. Otherwise `pw ≥ W − 2·ceil(0.02·unit)` (full width), and in portrait the panel height ≤ `H − 2m`.
  2. The panel is inside the screen. Every text box, chip, leader-line end and the drawing box is inside the panel's inner rectangle: nothing clipped.
  3. Every text box is at least as wide as its measured text: no truncation, no ellipsis.
  4. No two text boxes or chips intersect. No leader line crosses another label's box.
  5. Body text ≥ 14 px and small text ≥ 13 px at every size. Body px is non-decreasing as `unit` grows.
  6. Every `Row` and every hotkey row appears exactly once across all pages.
  7. Page count ≤ 2 for every shipped console at 640x480 in English.
  8. Determinism: two runs give identical layouts.
- The test prints a compact layout dump on failure, so a broken case can be read without images.

**T7. Browser list drift guard:** `browser.json` covers exactly the `case BTN_*` labels of `BrowserMode::onButton` (the test reads `frameworks/native/services/gammapad/BrowserMode.cpp` as data; it is skipped if the file isn't found).

**T8. Settings plumbing (pure helpers):** the `FACE_*` ↔ `BUTTON_*` collision rule; the migration (a hotkey on `BUTTON_A` pushes Button Map to `FACE_EAST`).

### 9.3 Golden rendering test (T6)

`test/buttonmap/buttonmap_golden_test.sh`:
- For each scene in `test/buttonmap/scenes/*.json` and each size in {640x480, 720x720, 960x544, 1024x768, 1280x720, 1920x1080, 1080x1920}, it runs `nano-buttonmap scene FONTDIR ARTDIR W H out.rgba` and compares `sha256` with the line in `goldens.sha256`.
- Scenes are the T5 set, plus three palettes (XMB, DSi light, Minima) for the SNES scene at 720x720 and 1280x720. That is about 60 frames.
- `--bless` rewrites `goldens.sha256`. `--keep DIR` keeps the frames, plus a PNG contact sheet made by a tiny `tools/rgba2png.py`, for review by eye. This mirrors `nano_bootlogo_golden_test` (`nano/test/bootlogo/bootlogo_golden_test.sh`; module `nano/Android.bp:853-867`).
- Fonts: Roboto from `:recovery_roboto-fonts_dep`, as the boot-logo test uses, plus a Hebrew face for the RTL scene. The device chain may use the theme font or PS3 Rodin. The goldens pin the layout and rendering code, not the user's font choice.
- Like the boot logo, the hashes depend on the FreeType and nanosvg revisions in the tree. A toolchain bump re-blesses them, with the contact sheet checked by eye.

### 9.4 Phases

| Phase | Scope | Effort | Host-testable |
|---|---|---|---|
| 1. Core model | `NanoButtonMap` (`Context`, `PadModel`, south rule, `sameButton`); readers for RetroArch (cfg, autoconfig, remaps, `retropad.json` for every shipped core), PPSSPP, Dolphin GC, Flycast, Azahar, ARMSX2, WatermelonDS, drastic-nano, M64Plus FZ, SeedlessDS, hakuX; ScummVM and Vita3K defaults; `consoles.json`, `systems.json`, `browser.json`; T1-T4, T7, T8 with fixtures | 6-8 days | all |
| 2. Layout and renderer | `NanoButtonMapArt` (layout, nanosvg anchors, FreeType text, bidi, palettes), the `nano-buttonmap` tool, T5 and T6 with goldens, interim placeholder art (the ES-DE controller badges `nano/nano_esde_assets/controllers/*.svg` have no anchors, so they serve only as table-mode headers until the catalogue lands) | 4-5 days | all |
| 3. Nano integration | `NanoMenuButtonMap.cpp` (open, close, keys, touch, texture upload, theme palette); the menus chord; the context gatherers per theme (XMB, DSi, ES-DE, Minima); the Settings row, `hotkeyTakeButton` and migration; `launch_system` / `launch_game` properties; i18n keys in 21 languages; `HOTKEYS.md` and README | 4-5 days | partly (helpers); UI on device |
| 4. Over games and browser | the PWM branch + `nanoSummonButtonMap` + `bmap_dev`; the `qs_summon=3` handling in `overlayShow` and the layer-still-up path; the RetroArch `content_history.lpl` check; browser context and inactive-reason line; README correction (§8) | 2-3 days | parser and context host-tested; the chord on device |
| 5. drastic-nano | `NanoButtonMap*.cpp` in `drastic_nano_shared_srcs`; its chord branch (`drastic-nano/InputMap.cpp:721`); draw the RGBA via `OverlayGfx`; block the pad but keep emulating (no pause, §1); fix the `BUTTON_A`→`BTN_SOUTH` hotkey mismatch noted in §3.2 | 2 days | the reader yes; the drawing on device |
| 6. Art and polish | Drop in the controller-art catalogue (anchor validation runs in T5: every console in `consoles.json` with a drawing has every anchor its buttons name, or the row is counted in the strip); pad drawings; the gammapad per-source south (§1.1 limit); optional mouse-mode list | 2-4 days, depends on art | mostly |

Total: about 20-27 working days. Phases 1-2 can start before any device time and before the art exists.

**As built in phases 3-5** (differences from the text above):
- The built-in pad's table is `ps3xmb/buttonmap/pads.json` (`bmap::loadPadSpecsFrom`, `padModelFor`): the first entry is the pad gammapad's virtual device stands for; any other device is a standard pad. gammapad's properties are read by `bmap::gamepadStateFrom`, shared by Nano and drastic-nano.
- PhoneWindowManager resolves the chord button with `ThimsyDeviceKeys.buttonMapKeycode` (no pad table in Java: 484b:1101 is the built-in pad). It and `bmap::buttonMapKeycode` run the same cases (`test/fixtures/devkeys/cases.json`, `button_map`).
- The panel's text is translated by `bmap::translate` / `bmart::translateScene`: only `uiStrings()` keys are looked up, formats by their `%s`, composed text (" / ", " + ", " · ") part by part. `test/i18n_check.py` treats every `uiStrings()` literal as a key.
- `takeButton` is the hotkey-row rule with the Button Map row; the migration is `migrateButtonMap` at Nano start (`bmapMigrateSetting`).
- In the menus, `MenuFocus` → `menuContextInputs` is the focus read off any theme (XMB, DSi, Minima, the ES-DE system and game views); over an app, `OverAppState` → `overAppContextInputs`, with `sys.gammaos.nano.launch_time` (written by every launch) deciding whether `content_history.lpl` is newer. With the Quick Menu up over a game, the chord shows the game's map.
- Over an app the overlay renders the map alone (`bmapOverAppOnly`, scrim 0, the panel's own dim). With nothing to map the layer goes down before its first frame.
- RetroArch's binds are read from the config the launches pass (`NanoMenu::retroarchConfigFile()` → `ReaderEnv::raConfig`): the card's `retroarch.cfg` once Move Emulator Files has put RetroArch's folder there and the card is in, else the default one. Its history is the one that config names (`content_history_path`, else `<playlist_directory>/builtin/content_history.lpl`), not always the path in §3.1.
- drastic-nano: `DnButtonMap.cpp` draws the panel through `OverlayGfx::drawImage`; the input is polled as for a menu (no pad to the game) without opening or pausing the menu. Its face-button hotkeys now resolve through the pad table, so A means the same button as in PhoneWindowManager (finding 3). There is no touch or Y (table) there yet.

### 9.5 What needs a device

- **Host:** every reader and context case, the pad model, the layout at every size, and the pixels of the panel.
- **Device only:**
  - the PWM chord (`interceptKeyBeforeQueueing`);
  - the overlay raise over real apps;
  - `drop_input`;
  - the `dumpsys` foreground resolve;
  - `content_history.lpl` against a live RetroArch;
  - private app files being readable by Nano under SELinux (`/data/data/<pkg>/shared_prefs`, which provisioning already writes, `HOTKEYS.md` Provisioning);
  - gammapad states;
  - drastic-nano's overlay pass;
  - external display sizes.

### 9.6 Device checklist

1. **Menus.** In each theme (XMB, DSi, ES-DE, Minima), focus a system, then a game, then a Recently Played entry, and press Anbernic + bottom, then Start + Select + bottom (both open it in the menus; Start + Select + bottom must do nothing over a game):
   - the panel opens for the right console and emulator;
   - the bottom button does **not** also go back;
   - the Quick Menu doesn't open on the Anbernic release.
2. Focus Android Games (the system and a game), Applications, Settings: nothing happens.
3. **Over each emulator:** RetroArch (Snes9x, mGBA, Genesis Plus GX 6-button, Beetle PSX analog, mupen64plus), PPSSPP, Dolphin GC and Wii, Azahar, WatermelonDS, Flycast, M64Plus FZ, ARMSX2, ScummVM, Vita3K, hakuX:
   - the right mapping and labels;
   - the game gets no input while the panel is up;
   - the game keeps running under the panel; B, Start, the chord or a tap outside closes it;
   - the game never saw the bottom button.
4. **RetroArch, three ways:**
   - change a bind in RetroArch's menu, quit, reopen: the overlay follows;
   - save a core remap and a game remap: the badge names the remap;
   - load other content from inside RetroArch: `content_history.lpl` picks the new core.
5. **ABXY Swap:**
   - gammapad off: swap has no effect on the labels;
   - Controller Enable on, swap off: labels move to the by-print positions;
   - swap on: back. Check in RetroArch and PPSSPP.
6. **External pads:** connect an Xbox pad and a DualShock with gammapad off. The chord from the external pad (if it has a Mode button) labels that pad with its own drawing.
7. **Browser:**
   - Brave with Controller Enable off: list shown, greyed, with the Controller Enable line;
   - on: active list, and Anbernic + bottom still reaches PWM through BrowserMode;
   - Fulguris shows Ctrl+B for bookmark;
   - Controller Bindings off: the "bindings off" line.
8. **drastic-nano:** the chord shows the DS map. Emulation keeps running with the pad blocked, and the pad returns on close. Anbernic + R1 still saves.
9. **Settings:** set Button Map Button to Right (A); set Save State to the same button: they swap. Off disables the chord. Hotkeys off: the chord still works.
10. **Sizes:** 720x720 panel; HDMI at 1080p in External Only and Mirror modes (`external_display_modes_plan.md`). The panel is centred on the TV and full width on the panel.
11. **Languages:** Japanese, Arabic and Hebrew: text renders, the table mirrors in RTL, nothing is clipped.
12. **Logs:** `logcat | grep buttonmap:` shows `buttonmap: ctx=<…> source=<config|defaults>`. PWM logs `Anbernic+South -> overlay button map`.

---

## 10. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| External by-position pad merged by gammapad: the chord lands on its right button (§1.1) | That user, with that setup, presses the wrong button | gammapad publishes the source device's south per batch (phase 6). Until then, document it. The chord from the built-in pad is always right |
| Closed or private formats change between emulator versions | The reader shows stale or default binds | Readers are pinned to the shipped versions. The badge says "Default mapping" when a key is missing. Fixtures make a format change visible when an emulator is bumped (`10-update-emulators.sh` / `added-apps.txt` bump → re-check §3) |
| RetroArch's running core when content is switched inside RetroArch | The wrong core table | `content_history.lpl`, written at content load. Network commands (`GET_STATUS`) are off in the shipped config and name only the system, so they aren't used |
| Binds changed in RetroArch's Quick Menu but not saved | The overlay shows the saved binds | Expected. RetroArch writes `retroarch.cfg` on exit. Mentioned in the device checklist |
| Dolphin and Azahar ship with no pad bindings | "Not mapped" panels out of the box | The overlay says where to map. Separately, a Dolphin `GCPadNew.ini` provision rule for the Cube is recommended |
| SELinux: the overlay instance reading `/data/data/<pkg>/…` | Readers fail on device | Nano already edits these files for provisioning (`nano/NanoHotkeys.cpp:492-534`), from the same domain. Verify the overlay process too |
| Reader cost on open (several small files, one UDP round trip) | Visible delay | Read on a worker thread when the chord is seen, render when done (50 ms typical). Show the panel frame with "Reading settings…" if it takes longer than 150 ms |
| Untrusted file contents (apps write them) | Parser crash in Nano | Readers are bounded (size caps as in `NanoEsdeTheme` limits), and fuzzed (`gammaos_nano_buttonmap_fuzzer`) |
| Art licence or anchor gaps | Missing drawing | The table fallback is always complete. T5 counts strip rows, so gaps are visible |
| Golden hashes depend on FreeType/nanosvg versions | Re-bless churn | Same policy as the boot logo; the contact sheet is reviewed by eye |
| No pause over a game | Something moves in the game while the map is up | Decided by the user: the game keeps running with its input blocked, as under the Quick Menu |

**Open questions: answered by the user on 2026-11-13** (recorded in §1, §6.5, §7.1, §8)
1. Pause? **No.** The map blocks the game's input and the game keeps running, also in drastic-nano.
2. Browser with gammapad off? **The greyed list with the hint line.** Thimsy never turns gammapad on by itself.
3. Start + Select + bottom? **Yes in Nano's own menus, not in a game.**

**Findings outside this feature** (found during the research; each is a separate fix)
1. **Browser controls need Controller Enable**, which is off by default (§8). `README.md:450-482` doesn't say so.
2. **drastic-nano no longer shares DraStic's keymap.** It imports the XML once into `persist.gammaos.drastic_nano.key.*` (`drastic-nano/main.cpp:4674-4688`), and `writePrefs` has no callers. The "shared" wording in `HOTKEYS.md` (Emulator support, DraStic row) and `mods/hotkeys/drastic.json` is stale.
3. **Face-button hotkeys mean different buttons in apps and in drastic-nano.** In drastic-nano `BUTTON_A` is `BTN_SOUTH`, the right button (`drastic-nano/InputMap.cpp:479-486`). In PhoneWindowManager it is the Android keycode, the bottom button (`pwm/PhoneWindowManager.java:2827-2833`).
4. **Dolphin has no pad bindings by default on Android** (`GCPadEmu.cpp:194-199` at `41799046`), and Thimsy provisions none. A fresh Dolphin game can't be played with the Cube's pad until the user maps it.
5. **Azahar has no default bindings** until Auto-map is run.
6. **SeedlessDS is open source** (GPL-3, SapphireRhodonite/SeedlessDS tag 1.0.1).
7. **The hotkey row labels "A/B/X/Y" are Android keycodes.** With gammapad off, "A" is the button printed B (`nano/NanoMenuPS3Menu.cpp:9454-9457`). The new Button Map row is labelled by position and print to avoid this. The existing rows could get the same labels.
