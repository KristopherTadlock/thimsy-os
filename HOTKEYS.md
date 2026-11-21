# Universal hotkeys

Applies to Thimsy OS built from GammaOS Next 1.4.4 source for the RG Cube (branch `nano-144`).

Status: implemented. The per-emulator bindings come from each emulator's source code and are not yet tested on a device (see [Testing](#testing)).

## What it does

Hold the **Anbernic button**, or **Start + Select**, in a game and press another button:

| Action | Default chord | Kind |
|---|---|---|
| Save state | Anbernic + R1, or Start + Select + R1 | one shot |
| Load state | Anbernic + L1, or Start + Select + L1 | one shot |
| Fast forward | Anbernic + R2, or Start + Select + R2 | toggle |
| Rewind | Anbernic + L2, or Start + Select + L2 | toggle |
| Quit game | Anbernic, or Start + Select, held on its own for 3 s | one shot |

- R is "forward" (save, fast forward) and L is "back" (load, rewind).
- **A chord acts the moment the button that completes it goes down**, everywhere: over a game and in Nano's own menus. Anbernic + R1 saves on R1's press, and Anbernic + the Button Map Button shows the map on its press. That button never reaches the game, and its release is swallowed.
- **A button that starts chords acts on its own only when it is let go**, since until then it may still become a chord: the Anbernic button (the Quick Menu), Back, and Start + Select (the Quick Menu, in Nano's menus). Holds stay holds: 3 s to quit (or go Home), the 1 s *Quit Game Button* hold, and the 10 s recovery.
- Quit needs a 3 s hold with nothing else pressed, so it can't happen by accident. Any other key during the hold cancels it until every button is let go and the hold starts again.
- Where the function button is Android's `HOME` key, its 3 s hold goes Home instead of quitting, with Universal Hotkeys on or off ([The function button is `HOME`](#the-function-button-is-home)). Start + Select's hold still quits.
- The buttons can be changed under **Settings → Gamepad Settings → Hotkeys**. *Quit Game Button* adds a button chord for quit; it is off by default.
- The Anbernic button on its own opens the Quick Menu when it is **released**, over a game and in Nano's own menus alike. Start + Select opens the Quick Menu only in Nano's own menus, never in a game.
- Anbernic + Volume Up / Down changes the screen brightness everywhere. It is not a hotkey, and the Quick Menu does not open on that release.
- Anbernic + Select turns the display shader on or off everywhere: Nano's menus, apps and games. The selected shader is kept (CRT Simple if none was ever chosen), so a shader that makes the screen unreadable can be turned off without seeing it. It replaces Power + Select, which is plain Power again.
- Anbernic + Start opens the Applications view, and closes it again. Over a game the overlay rises on it, and closing it (Anbernic + Start, Start or B) returns to the game.
- Anbernic + the **Button Map Button** (Hotkeys settings; by default the bottom face button, printed B on the RG Cube) shows the **button map**: how the controller in hand maps onto the console being emulated, with the hotkeys ([README, Button map](README.md#button-map)). It works over a game or app and in Nano's own menus, in every theme. Over a game the overlay rises on the map alone; the game keeps running under it with its input held back, as under the Quick Menu, and B, Back, Start, the chord again or a tap outside it closes it. In Nano's menus Start + Select + that button opens it too (never over a game, where Start + Select + B is a common combination). That raise doesn't reload Nano's lists or rebuild the XMB's graphics, which took most of a second on the RG Cube (a raise on the Quick Menu or Applications still does), so the map shows while the button is still down. The XMB is prepared only if something other than the map comes to be shown. Over Android Games (the system and its games) and most apps it does nothing; over a browser, or on a browser's entry in Applications, it shows the browser controls.
- None of these is a hotkey: they work with Universal Hotkeys off, the Quick Menu does not open on that release, and the game never sees that Start, Select or Button Map Button. Start and Select can't be hotkey buttons.
- With Start + Select, the game still sees Start and Select: they can't be held back without delaying them. Start + Select is not a chord when another pad button is already held (A + B + Start + Select resets many games).
- On the touchscreen, a two-finger tap opens or closes the Quick Menu, and a three-finger tap opens or closes the Applications view, in Nano's own menus (not over a game or app).

Each emulator has its own hotkeys, or none. A per-emulator **profile** turns the five actions into whatever that emulator understands. Adding an emulator means adding a profile file, with no code change.

## How it works

```
 pad --> PhoneWindowManager.interceptKeyBeforeQueueing
            |  chord detector (Anbernic held + button)
            v
         ThimsyHotkeyController (system_server)
            |  foreground package --> profile --> driver
            +--> inject a keyboard key (InputManager)
            +--> RetroArch network command (UDP 127.0.0.1:55355)
            +--> intent
            +--> the system exit (long-press-Back path) / force-stop

 Nano (startup and every return to the front)
            +--> profile "provision" rules: bind the keys in each emulator's settings files
```

### Chord detector (PhoneWindowManager)

It runs while a game is in front (`app_launched`, overlay down, screen on):

1. **Anbernic down:** swallowed and armed. Nothing opens yet.
2. **A gamepad button or D-pad press while armed:**
   - it is a chord: the button runs its hotkey;
   - the button's repeats and key-up are swallowed, even if the Anbernic button is released first.

   Buttons with no hotkey are swallowed too, so the game never sees half a chord.

   A Volume key while armed is not swallowed: it changes the brightness (the volume key handler reads the Anbernic button's physical state), and the Quick Menu does not open on release.

   Select or Start pressed while the Anbernic button is physically held comes before the hotkeys: Select flips `persist.gammaos.shader.enable`, and Start sets `sys.gammaos.nano.qs_summon=2` with `show_overlay=1`, so Nano rises on the Applications view. The button's repeats and key-up are swallowed. Select works even with the Anbernic button given back to apps (Toolbox); Start needs it to be Nano's.

   The Button Map Button pressed while the Anbernic button is physically held comes before the hotkeys too: it sets `sys.gammaos.nano.qs_summon=3` with `show_overlay=1`, and writes the pressing device to `sys.gammaos.nano.bmap_dev` (`vendor:product:descriptor:name`), so Nano rises on the button map for that pad. The button is matched by position for the device that sent it (`ThimsyDeviceKeys.buttonMapKeycode`): the bottom button is `BUTTON_A` from the built-in pad and from other pads, and through gammapad's virtual pad it is what ABXY Swap and `remap_btn` make of it. Its repeats and key-up are swallowed, the hold to quit is cancelled, and the Anbernic button's release opens nothing. Like Start it needs the Anbernic button to be Nano's, and it works with Universal Hotkeys off.

   The Switch Screen Button (`persist.gammaos.hotkeys.screen`, `FACE_NORTH`, the top button) pressed while the Anbernic button is physically held, in Dual Screen mode, writes the uptime to `sys.gammaos.nano.screen_swap`, and Nano moves the controller between the monitor and the built-in screen. It is matched by position like the Button Map Button, swallowed the same way, needs the Anbernic button to be Nano's, and works with Universal Hotkeys off. It also acts, without an app on the monitor, while the built-in screen's app has the controller (`sys.gammaos.nano.focus.display` set). In Nano's own menus Nano reads the chord itself; over the Quick Menu on an app, in a drastic-nano session and in Nano's Quick Resume players it does nothing.

   Back pressed while armed is a chord too: the Anbernic button's release opens no Quick Menu, and with Universal Hotkeys on the app gets no Back. With them off the app gets its Back when Back is let go, as usual ([Back acts when it is let go](#back-acts-when-it-is-let-go)).
3. **Anbernic up:**
   - after a chord: nothing more;
   - alone, within 3 s: the overlay rises on the Quick Menu.
4. **Hold to quit:** the Anbernic button held on its own for 3 s runs quit, and its release opens nothing. Any other key pressed during the hold cancels it. With Universal Hotkeys off there is no hold to quit, and a release within 10 s opens the Quick Menu as before. Where the button is Android's `HOME` key the hold goes Home instead, with hotkeys on or off, and a release opens the Quick Menu only within 3 s ([The function button is `HOME`](#the-function-button-is-home)). The 10 s emergency recovery hold (BACK, `BUTTON_MODE`, or the function button on a device with no Back) is unchanged, and checks the key is still down when it fires. It also works while Nano's overlay holds input back (`sys.gammaos.nano.drop_input`, which a crash can leave set): `BUTTON_MODE`, F10 and `HOME` get through to it then, as Back does, so it works where the recovery key is the function button. They get through only for the hold: while Nano holds input they don't reach the app, raise the Quick Menu or go Home. Injected keys (a hotkey's own, `input keyevent`) neither start nor cancel it. The D-pad reaches Android as a hat axis, so over an Android app it doesn't cancel a hold.
5. **Start + Select:** the second of the two pressed while the other is held, with no other pad button down, arms the chord. Both keys still reach the app. Their down state is tracked from the key events, and a chord whose key-ups went missing (the overlay holds them back) is dropped at the next key.
   - A hotkey button pressed while both are held runs its hotkey and is swallowed like an Anbernic chord. Any other key reaches the app. A game's own combination that starts with Start + Select and then adds a hotkey button (L + R + Start + Select resets DS games) runs the hotkeys instead; *Start + Select Hotkeys* off avoids that.
   - Held for 3 s with nothing else pressed, it runs quit. Any other key, or Start or Select pressed again, cancels the hold until both are let go.
   - It never opens the Quick Menu over a game.
   - *Start + Select Hotkeys* (`persist.gammaos.hotkeys.startselect`) turns it off.
6. **Quit button:** if *Quit Game Button* is set, that button must stay down for 1 s (*Hold to Quit*). Releasing it earlier cancels.
7. **Order of presses:** only Anbernic first, then the button, is a chord. A button already held when Anbernic goes down passes through untouched.
8. **A lost key-up:** if the Anbernic button's key-up goes missing, a later chord checks that it is physically down before acting. Start + Select reads both buttons' physical state.
9. **A key-up left over:** when the app stops while the Anbernic button or Back is held (a hold to quit, the 10 s recovery), the chords are forgotten, and that button's key-up, if it still comes, is dropped.

Nano's own menus follow the same rule. The Quick Menu toggles on release, and a button pressed while the Anbernic button is held cancels it and is ignored. Back pressed while the Anbernic button is held is a chord too: neither the Quick Menu nor Back acts. Start and Select act when they are released, so pressing both together toggles the Quick Menu once both are up. A key pressed while either is held cancels its action. Anbernic + Volume is brightness, and cancels the Quick Menu like any other chord. Anbernic + Select (shader) and Anbernic + Start (Applications) act on the press and are not a Start + Select chord; Select's search and Start's own action don't run. Applications closes back to where it was opened from, and Start on its own closes it too. Anbernic + the Button Map Button opens the button map on the press (or closes it); so does Start + Select + that button, which marks the Start + Select chord used so its release opens nothing. While the map is up it takes every key but Power, Volume and the mouse's left and middle buttons (a click acts as a tap; the wheel scrolls the map), and a key that closed it has its release dropped. Where the Anbernic button is Android's `HOME` key, holding it on its own for 3 s goes Home there too, and only a shorter press toggles the Quick Menu.

### ThimsyHotkeyController (system_server)

`frameworks/base/services/core/java/com/android/server/policy/ThimsyHotkeyController.java`. It:

- finds the foreground package and its profile, falling back to the `"*"` profile;
- runs the action's driver;
- re-reads profiles when a file in either folder changes;
- keeps the toggle state for the app in front.

**Held keys never get stuck.** A `key_hold` key is released when:
- it is toggled off;
- another hold starts;
- the Quick Menu opens;
- quit runs;
- another app comes to the front;
- the game exits;
- the screen goes off.

**A tap is held for 100 ms.** The emulator acts on the key's press, which is sent the moment the chord is made. The release follows 100 ms later, not at once: an emulator that reads its keys once a frame misses a press whose down and up arrive in the same read (RetroArch 1.22.2's `android_input_poll` applies its whole input queue, setting and clearing the key, before the frame's hotkey check). 100 ms is a few frames even at 20 fps, and well short of Android's key repeat. Tapping the same key again within that time releases it first. Quit and a hotkey in another app release a tap at once; otherwise the release always comes after the 100 ms, even with the Quick Menu up (the game then gets it as a cancelled release, as Nano's overlay holds keys back). RetroArch keeps the `key` driver rather than `retroarch_cmd`: network commands need `network_cmd_enable`, which listens on UDP 55355 on every interface.

There are no on-screen messages from the hotkeys. This build skips SystemUI, so system toasts can't show over a game. The emulators show their own messages: RetroArch, PPSSPP and Flycast report saves and loads.

### Drivers

| Driver | What it does |
|---|---|
| `key` | Taps a keyboard key (virtual keyboard, `SOURCE_KEYBOARD`): down at once, up 100 ms later (`TAP_HOLD_MS`). `"mods": ["CTRL", "SHIFT", "ALT"]` puts the modifiers on the event, and also presses and releases the modifier keys around it. The virtual keyboard reports F1–F12 in `InputDevice.hasKeys`, for emulators that only map keys a device says it has |
| `key_toggle` | Taps a key the emulator treats as a toggle, as `key` does |
| `key_hold` | Key down when toggled on, key up when toggled off |
| `retroarch_cmd` | Sends `"command"` (e.g. `SAVE_STATE`) as a UDP datagram to `"port"` (default 55355). RetroArch needs `network_cmd_enable = "true"` |
| `intent` | `"action"`, `"package"`, `"class"`, `"extras"`. A broadcast unless `"broadcast": false`, which starts an activity instead |
| `exit` | The system exit used by a long press of Back. RetroArch and DraStic get Esc and save before closing; standalone apps are stopped (unless *Keep Running in Background* is set) and Nano returns |
| `force_stop` | Stops the app and returns to Nano |
| `none` | Not available; logged |

Any action can add `"then": "<driver>"` with `"timeout_ms"`. If the app is still in front after the timeout, that driver runs. Graceful quits use it: the emulator's own quit key, then `exit`.

### Profiles

Profiles are JSON files, one per emulator:

- **Built-in:** `/system/etc/gammaos/hotkeys/<id>.json`, from `mods/hotkeys/` (installed by `mods/55-hotkeys.sh`).
- **User or override:** `/data/system/gammaos/hotkeys/<id>.json`. A file with the same `id` replaces the built-in one; a new id adds an emulator.
- **Fallback:** the profile with `"packages": ["*"]` (`fallback.json`) covers every other app. It supports quit only.

```json
{
  "id": "ppsspp",
  "name": "PPSSPP",
  "packages": ["org.ppsspp.ppsspp", "org.ppsspp.ppssppgold"],
  "note": "Shown in Settings > Hotkeys > Emulator Support.",
  "actions": {
    "save_state":   {"driver": "key", "key": "F2"},
    "load_state":   {"driver": "key", "key": "F4"},
    "fast_forward": {"driver": "key_hold", "key": "F6"},
    "rewind":       {"driver": "key", "key": "F7"},
    "quit":         {"driver": "exit"}
  },
  "provision": [
    {"file": "${sdcard}/PSP/SYSTEM/controls.ini", "format": "ini", "section": "ControlMapping",
     "mode": "append", "separator": ",",
     "values": {"Save State": "1-132,0-132", "Load State": "1-134,0-134"}}
  ]
}
```

- **Key names:** Android `KeyEvent` names without `KEYCODE_` (`F2`, `SPACE`, `R`).
- **Shared keys:** the profiles use F2 save, F4 load, F6 fast forward, F7 rewind and F8 quit, except RetroArch, which keeps its own defaults.

### Provisioning

Nano applies every profile's `provision` rules on a background thread when it starts and each time it comes back to the front, one run at a time. It skips emulators with a running process, which may write their settings back when they exit (so an emulator kept running in the background is bound once it has been closed), and leaves a file the emulator changed while Nano was editing it for the next run. Every launch starts from Nano, so the bindings are in place before the emulator reads them, and they come back if an emulator rewrites its file. With Universal Hotkeys off, nothing is provisioned.

**Rules:**
- Only files that already exist are edited (the emulator has run once). A rule with `"template"` creates the file from it when the emulator's folder exists.
- `${sdcard}` is `/storage/emulated/0`, which keeps file ownership right on shared storage. An edited file keeps its owner and mode; a file made from a template takes its folder's owner, mode 0660.
- A value can be a string or a number. Files are replaced through a temporary file that is synced first.
- After **Move Emulator Files to microSD** a rule's file is looked for where the emulator now keeps it: PPSSPP's `PSP` folder on its memory stick, Flycast's files in its new home, and a folder replaced by a link to the card (Dolphin's) at the link's target in `emulator_config` (a link anywhere else is left alone). A rule for RetroArch's default `retroarch.cfg` is applied to the config Nano starts RetroArch with too, the card's after a move.

**Modes, for each value:**

| Mode | Behaviour |
|---|---|
| `add` (default) | Sets a missing or empty setting |
| `append` | Like `add`, and also adds the value to a setting bound to something else, joined with `"separator"`. Use it where the emulator takes several bindings for one action |
| `ifUnset` | Only sets a missing setting |
| `set` | Always sets it |

**Formats:**

| Format | File shape | Behaviour |
|---|---|---|
| `ini` | `[section]` then `key = value` (`"assign"` changes the ` = `) | — |
| `retroarch_cfg` | `key = "value"`, no sections | — |
| `flycast_mapping` | Flycast's `[digital]` `bindN = keycode:action` lines | Adds each `"binds"` entry as the next number, unless its keycode is already bound |
| `xml_prefs` | Android SharedPreferences | `"type"`: `int`, `long`, `boolean`, `string`, or `set` (a one-string set, only ever added). An `int` of 0 counts as unbound |
| `melonds_mapper` | melonDS for Android `controller_config.json` | `"binds"`: input name → keycode. Set only where the input has no assignment and the keycode is free |

The user's own bindings are never replaced in `add` mode: a hotkey whose setting the user bound to something else stays theirs.

### Settings in Nano

**Settings → Gamepad Settings → Hotkeys:**

| Row | Property | Default |
|---|---|---|
| Universal Hotkeys | `persist.gammaos.hotkeys.enable` | On |
| Save State Button | `persist.gammaos.hotkeys.save` | R1 (`BUTTON_R1`) |
| Load State Button | `persist.gammaos.hotkeys.load` | L1 |
| Fast Forward Button | `persist.gammaos.hotkeys.fastforward` | R2 |
| Rewind Button | `persist.gammaos.hotkeys.rewind` | L2 |
| Start + Select Hotkeys | `persist.gammaos.hotkeys.startselect` | On |
| Quit Game Button | `persist.gammaos.hotkeys.quit` | Off |
| Hold to Quit | `persist.gammaos.hotkeys.quithold` | On |
| Button Map Button | `persist.gammaos.hotkeys.buttonmap` | Bottom (B) (`FACE_SOUTH`) |
| Emulator Support | read-only: each profile, what works and its note | — |

- A button can be A, B, X, Y, L1, R1, L2, R2, L3, R3, or Off. Start and Select are reserved for Anbernic + Start and Anbernic + Select, and a D-pad direction does nothing over apps (the D-pad reaches Android as a hat axis, which `PhoneWindowManager` never sees); a hotkey set to one of these earlier is turned Off.
- Two hotkeys can't share a button: choosing a button another hotkey has swaps the two. Button Map Button joins the rule. Its options are by position, labelled with the RG Cube's print (Bottom (B), Right (A), Top (X), Left (Y)), plus L1, R1, L2, R2, L3, R3 and Off; the hotkeys' A/B/X/Y are Android keycodes, so the rows are compared as the buttons they press: with gammapad off the hotkeys' A is the bottom button. When one row takes another's button, that row gets the first one's old button in its own words, and Button Map, which can't hold a D-pad direction, takes the first face button nothing holds.
- At start Nano moves Button Map Button off a button a universal hotkey already has (an earlier Save State on A): the hotkey keeps it, and the map takes the first free face button (bottom, right, top, left), or Off when all four are hotkeys. The log says which.
- **Controller Bindings** in the Quick Menu (`sys.gammaos.bindings.enabled`, On at every boot) set to Off turns off Start + Select's hotkeys and its hold to quit, in apps, along with the other controller bindings (the browser controls, mouse mode and its combo). The Anbernic button's hotkeys stay.

Nano's libretro player only runs during the Quick Resume preview, so it has no hotkeys.

## Emulator support

Bindings come from each emulator's source code at the shipped version.

| Emulator | Save | Load | Fast forward | Rewind | Quit | How |
|---|---|---|---|---|---|---|
| RetroArch | F2 | F4 | Space (toggle) | R held | `exit` (Esc, auto-save) | `retroarch.cfg` keyboard binds. Keyboard hotkeys work without the Select hotkey-enable button. Rewind needs a core that can rewind, with Rewind on in RetroArch |
| PPSSPP | F2 | F4 | F6 held | F7, one snapshot per press | `exit` | `PSP/SYSTEM/controls.ini` `[ControlMapping]`, keyboard device `1-` and default device `0-` appended. Rewind needs `RewindSnapshotInterval` set in PPSSPP |
| Flycast | F2 | F4 | F6 (toggle) | — | `exit` | `mappings/Android_Virtual.cfg`: the injected keys come from the "Virtual" device, which Flycast maps like a pad |
| ARMSX2 | F2 | F4 | F6 (toggle) | — | F8 (save and exit), then `exit` | `shared_prefs/ARMSX2.xml` `pad.*.keycode`. Not if "Emulate USB Keyboard" is on |
| Azahar | F2 (quick slot) | F4 | F6 (turbo toggle) | — | F8 (close game), then `exit` | `shared_prefs` `InputMapping_HostAxis_<keycode>` sets. Azahar's own hotkey Enable button must be unset |
| WatermelonDS | F2 | F4 | F6 (toggle) | F7 opens its rewind screen | F8 (exit game), then `exit` | `files/controller_config.json`. Rewind needs Rewind on in its settings |
| SeedlessDS 1.0.1 | F2 (quick slot) | F4 | F6 (toggle) | — | `exit` | `shared_prefs/keymap.xml` `s_1` (Fast Forward), `s_2` (Quick Save), `s_3` (Quick Load): its emulator shortcuts, which have no keys by default. The file exists once its first game has started. Only its first key map profile is bound. It has no rewind and no exit shortcut (Back opens its menu) |
| ScummVM | — | — | Ctrl+F (SCUMM games) | — | Ctrl+Q, then `exit` | Built-in ScummVM keys |
| DraStic (app, if installed) | — | — | — | — | `exit` (Esc) | Not shipped; its keymap is left alone |
| Dolphin 2609-197 | F2 (selected slot) | F4 | F6 (toggle) | — | `exit` | `Config/Hotkeys.ini` `[Hotkeys]`, bound to `` `Android/0/Virtual:F2` `` and so on. Dolphin only maps keys a device reports in `hasKeys`, so the virtual keyboard reports F1–F12 (`EventHub.cpp`) |
| M64Plus FZ | — | — | — | — | `exit` | Hotkeys live in private controller profiles and need long holds |
| hakuX | — | — | — | — | `exit` | Save states only from its in-game menu; no fast forward |
| Vita3K | — | — | — | — | `exit` | No save states or fast forward |
| Any other app | — | — | — | — | `exit` | `fallback.json` |

## Rewind

Rewind works on every system whose emulator or core can rewind; performance is not a concern. The hotkey never turns rewind on in an emulator's settings. If the user enabled it there, the button works; if not, the emulator ignores the key. RetroArch's `rewind_enable` stays as the user set it (off in the shipped config).

## Power button

Holding the power button for 7 s shuts the console down, in a game or in Nano. `PhoneWindowManager` handles it. Shutdown closes the game first where it can, and Quick Resume is prepared.

## Devices

"The Anbernic button" in this document means the device's **function button**. On the RG Cube it is the Anbernic logo button. Other handhelds GammaOS supports have a Mode, Home or Menu button instead, or only a Back button. Every chord above, Anbernic + the Button Map Button included, works through the same lookup, so they work the same on every device.

Nano's prompts and settings text name the button as it is printed on the device (`devkeys::functionPrompt()`, `ThimsyDeviceKeys.functionPrompt()`; the *Prompt* column below): on the RG DS the Hotkeys page says *RG + this button saves the game's state.* and the Toolbox row is *RG Button Opens Quick Menu*. Where Back doubles as the function button they name Back's button instead (*Menu* on the TrimUI Brick and MagicX, *Back* on the RG405M, RG505 and Retroid Pocket Classic), since the chords are made with it there, and the Universal Hotkeys and Toolbox texts say that a tap is still Back. The texts are translatable formats with the name as their `%s` (`fnText` in `NanoMenuPS3Menu.cpp`), so each language keeps its own word order; `test/i18n_check.py` fails on "Anbernic" in a translation or in a Nano string outside log lines and the device table.

### How a device's buttons are found

One table, `/system/etc/gammaos/device_keys.json` (from `mods/device-keys.json`, installed by `mods/55-hotkeys.sh`), lists each device's function and Back buttons. Every reader picks the first entry whose `match` fits the device's system properties, or the table's `default` if none does:

| Reader | Code | Uses |
|---|---|---|
| PhoneWindowManager | `ThimsyDeviceKeys.java`, read once at `systemReady` | Android keycodes: the chord detector, the Anbernic + Volume / Select / Start checks, the 10 s recovery hold |
| Nano | `NanoDeviceKeys.cpp`, read on first use | evdev codes: its menus' Anbernic handling, Anbernic + Volume / Select / Start |

`ThimsyHotkeyController` doesn't change. It only injects keyboard keys, and never reads the pad.

The two resolvers are held to the same test cases (`gammaos-nano/test/fixtures/devkeys/cases.json`), so they always pick the same keys and the same prompt name.

- **No table, or a broken one:** the RG Cube's keys, the ones Thimsy always used. Those are F10 from a gamepad, `BUTTON_MODE` and Back.
- **`match`:** a list of alternatives. All the properties an alternative names must match, and any one alternative is enough. A value matches case-insensitively, or as a prefix when it ends in `*`.
- **The properties used:**
  - `ro.gammaos.device`: only newer GammaOS vendor images set it (`anbernicrgds`, `anbernicrgrotate`, `trimuibrick`, `magicxxu20v32`, `pocketairmini`, `retroidpocketduolite`, ...). The T820, T618 RG405M/V/RG505, MediaTek, Retroid 4 Pro/Classic, KT-R1, ZPG, Pocket Micro and E6 Max images don't, so those match by model or platform.
  - `ro.product.vendor.model`, `ro.product.odm.model`, `ro.product.odm_dlkm.model`: the real strings, spaces included (`RGCUBE`, `RG 406H`, `RG 477V`, `RG VITA`, `Retroid Pocket Classic`). Anbernic shares vendor images, so the RG406V reports `RG 406H` and the RG Slide reports `RG 476H`; their keys are the same.
  - `ro.product.vendor.manufacturer`, `ro.product.odm.manufacturer` (the MediaTek Anbernics);
  - `ro.board.platform` (`ums9620` for T820, `ums512` for T618, `ceres` for A133P, `mt6897`, `mt6893`).
- **A key:**
  - `linux`, the evdev code Nano reads;
  - `android`, the keycode name PhoneWindowManager sees after the key layout. A key without one (the AYANEO codes 753 and 754, which no layout maps) is for Nano only;
  - `gamepad_only`, so a keyboard's F10 still reaches the app;
  - `prompt`, the name printed on the button, where `label` describes the key instead (`Anbernic / Home`, `Function (side)`, `Home / Back`). Prompts use the first function key's `prompt`, else its `label`, or the first Back key's where Back doubles as the function button; with no name at all, Anbernic (Back where Back doubles).
- **A function key without `gamepad_only`** counts over apps only from a built-in device or a gamepad, so a plugged-in keyboard's Home stays Home.
- **An entry without `function` or `back`:** takes the default's. `"function": []` means the device has no function button.
- **`"fallback": "back"`:** Back doubles as the function button (below).

The default function buttons are F10 (gamepad only), `BUTTON_MODE` and `HOME`, and the default Back is `BACK`. The Cube's own entry leaves out `HOME`, so nothing on the Cube changes.

### The function button is `HOME`

Android can't hand `HOME` to an app, so a function key whose `android` is `HOME` stays the system's. Today that is the default entry's `HOME` (evdev 172), so it applies to the devices on the default list: the KT Pocket KT-R1, the ZPG A1 Unicorn, the AYANEO Pocket Micro and any device the table doesn't know. The Retroid Pocket 4 Pro and Duo, the E6 Max and the Mangmi Air X list their Home button as `BUTTON_MODE`, so they work like the RG Cube; if one of their key layouts turns out to map it to `HOME`, changing the entry's `android` to `HOME` gives them this behaviour. The decisions are per key: on a default-list device, F10 and `BUTTON_MODE` keep the rules above.

| | The function button is `HOME` | Every other function button (RG Cube included) |
|---|---|---|
| Released on its own within 3 s | the Quick Menu | the Quick Menu (within 10 s with Universal Hotkeys off) |
| Held on its own for 3 s, over a game | **Home**, hotkeys on or off | quit with hotkeys on; nothing with them off |
| Held on its own for 3 s, in Nano's menus | **Home**: the top of the home screen | nothing (the release opens the Quick Menu) |
| + a button | that button's hotkey (with hotkeys on) | the same |
| Toolbox → *Anbernic Button Opens Quick Menu* off | no effect: the row shows a fixed **On** with the reason | the button goes to apps |

- **Home over a game** (`PhoneWindowManager`): what a Home press over an app does in Nano mode (`nanoKillAppAndRestart`), which leaves an app with *Keep Running in Background*, or one running a foreground service, running. RetroArch and DraStic are the exception: they leave through the long-press-Back exit (Esc), so they save before closing instead of being force-stopped. Before, the hold ran quit, which also closed the game; a Home press with the button given back to apps force-stopped it with no save.
- **Home in Nano's menus:** over a running app (the Quick Menu up over a game) the app closes to the home, as *Close Current App* does. In Nano's own menus every level, dialog, keyboard and player closes (music keeps playing, minimised), and the home shows its top: the ES-DE home, the DSi or Minima root, or on the XMB the first row of the category the home starts on (*Startup Menu*, else Game).
- **The decisions** are `devkeys::isHomeFunction`, `functionRelease` and `functionHold` (C++) and `ThimsyDeviceKeys.isHome`, `functionRelease` and `functionHold` (Java), held to the same cases (`function_release`, `function_hold` in `gammaos-nano/test/fixtures/devkeys/cases.json`).

Nano maps the function button's code to the Cube's F10 as it reads it, after the button pickers (which record the raw code). The rest of its menu code then needs no change. F10 and `BUTTON_MODE` keep working there on every device, as before.

**GammaPad remaps.** On many newer images (`persist.gammaos.gamepad.enable=1` in the vendor `build.prop`), GammaPad grabs the pad and re-emits it as "Xbox Wireless Controller", after its default remaps:
- RG DS and RG DS Plus: Mode 316 → 204.
- RG Vita and RG Vita Pro: F10 68 → 204.
- Pocket Air Mini: AYANEO 754 → 158, ≡ 753 → 204.

Such entries list both the raw and the remapped code, so the key matches whichever arrives. On the virtual pad 204 reaches Android as `NOTIFICATION` (`Generic.kl`, product `0b13`) or as `ALL_APPS` (the `02fd` layout), so both are listed.

On the RG DS, 204 is also the key Nano's Control Center toggles on. With the Mode button as the function button, it raises the Quick Menu and toggles the Control Center at once. That needs a product decision there: turn the remap off, or give the Control Center another key.

**The `045e:02fd` layout.** `Vendor_045e_Product_02fd.kl` (in AOSP, `frameworks/base/data/keyboards`) now maps `key 68 F10`. Without it, a GammaPad that presents as `045e:02fd` (the old vendor `gammapad`, or the `0x02fd` preset) would hand the Anbernic button to PhoneWindowManager as `KEYCODE_UNKNOWN`. The RG Cube's own pad (`484b:1101`) doesn't use it.

### Device map

Sources:
- **level 1:** the device tree or key layout inside a GammaOS release image or stock dump;
- **level 2:** driver or daemon source or binary;
- **level 3:** GammaOS vendor configuration (`GammaOSCoreVendor`);
- **level 4:** reviews and spec pages, for labels.

The research notes are `work/device-buttons-research.md`. "GP" is the code after GammaOS's default GammaPad remap.

| Device | SoC | Detected by | Function button → evdev / Android | Back button → evdev / Android | Prompt | Confidence (source) |
|---|---|---|---|---|---|---|
| RG Cube | T820 | vendor/odm model `RGCUBE` | Anbernic logo → 68 / `F10`; 316 / `BUTTON_MODE` kept | Back → 158 / `BACK` | Anbernic | verified (1: v1.2 dtbo, vendor key layout, `build.prop`) |
| RG556, RG406H, RG406V, RG476H, RG Slide | T820 | `RG556`, `RG 406H`, `RG 476H`, or `ums9620` + Anbernic | Anbernic / Home → 68 / `F10` | Back → 158 / `BACK` (Slide: right of the screen) | Anbernic | verified (1: each model's dtbo) |
| RG477M, RG477V, RG557 | Dimensity 8300 | odm model `RG477M`, `RG 477V`, `RG557` | Home → 68 / `F10` | Back → 158 / `BACK` | Home | verified (1: stock dtbo from the Unbricker dumps) |
| RG Rotate | T618 | `anbernicrgrotate` | side Function → 68 / `F10`; a second key sends 67 / `F9` | centre home/return → 158 / `BACK` | Function | codes verified (1), labels likely (4); which side key is 67 or 68 unconfirmed |
| RG Vita | **T618** | `anbernicrgvita`, `RG VITA` | Anbernic → 68 / `F10` (GP 204 / `NOTIFICATION`) | Back → 158 / `BACK` | Anbernic | verified (1, 3) |
| RG405V | T618 | odm/vendor model `RG405V` (assumed string) | side Function → 68 / `F10` | front Home → 158 / `BACK` | Function | likely (1 codes, 4 labels) |
| RG405M, RG505 | T618 | `RG405M` (verified), `RG505` (assumed), or `ums512` + Anbernic | **None** (68 is wired to no button) | Home / Back → 158 / `BACK` | Back | codes verified (1), labels likely (4). **Back doubles as function** |
| RG Vita Pro | RK3576 | `anbernicrgvitapro`, `RG VITA Pro` | Anbernic → 68 / `F10` (GP 204) | Back → 158 / `BACK` (on `adc-keys`) | Anbernic | verified (1, 3) |
| RG DS, RG DS Plus | RK3568 | `anbernicrgds`, `anbernicrgdsplus` | Mode → 316 / `BUTTON_MODE` (GP 204) | adc-keys button → 158 / `BACK` | RG | codes verified (1: v1.4.4 OTA dtb); reviews call it the RG button |
| TrimUI Brick, MagicX Zero 40, Mini Zero 28, XU20 V32 | A133P | `trimuibrick`, `magicxzero40`, `magicxminizero28`, `magicxxu20v32`, or `ceres` + `QUAD-CORE A133 aw4` | **None** | **Menu** → 158 / `BACK` | Menu | verified (1, 2). **Menu doubles as function** |
| GKD 350H Ultra | RK3576 | `gkd350hultra` | "F1" → 316 / `BUTTON_MODE` | "F" → 158 / `BACK` | F1 | codes verified (1), labels from the device tree |
| AYANEO Pocket Air Mini | MT6785 | `pocketairmini` | ≡ → 753 / none (GP 204 / `NOTIFICATION`) | AYANEO → 754 / none (GP 158 / `BACK`) | ≡ | codes verified (1, 3); the role split is GammaOS's |
| Retroid Pocket 4 Pro | Dimensity 1100 | `mt6893` + `Moorechip` | Home → 316 / `BUTTON_MODE` | Back → 158 / `BACK` | Home | verified (2: `retroidpad` binary) |
| Retroid Pocket Classic | Qualcomm `parrot` | `Retroid Pocket Classic` | **None distinct** (Home also sends 158) | Back and Home → 158 / `BACK` | Back | verified (2). **Back doubles as function** |
| Retroid Pocket Duo Lite (beta), Duo | Qualcomm | `retroidpocketduolite`, `retroidpocketduo` | Home → 316 / `BUTTON_MODE` | Back → 158 / `BACK` | Home | verified (2: `RetroidPad.c`) |
| GameMT E6 Max | A527 `saturn` | `A527 PRO` + `saturn` | Home → 316 / `BUTTON_MODE` (likely) | default (unverified) | Home | partial (1: stock dtb) |
| Mangmi Air X | SD680 | `mangmiairxmq65`, `mangmiairxmq66` | Home → 316 / `BUTTON_MODE` (likely) | default (unverified) | Home | likely (3); ROCKNIX wiki and setup guides call it Home |
| KT Pocket KT-R1 | `mt6789` | odm model `KTPocket-R1` | default list | default | Function (default) | unverified: detection only |
| ZPG A1 Unicorn | `mt6789` | odm model `ZPG A1` | default list | default | Function (default) | unverified: detection only |
| AYANEO Pocket Micro | `mt6789` | `Pocket Micro` + AYANEO | default list | default | Function (default) | unverified: detection only |

Not supported, so not in the table: AYN Thor, the GammaOS Core (Android TV) devices, and the planned AYANEO Pocket S/DMG and Air, Retroid Pocket Mini, Flip 2 and 5, and Powkiddy X28. They get the default.

**Still to check with `adb shell getevent -lp`, then `getevent -l` while pressing each system button:**
- **KT-R1:** which buttons send `KEY_MENU`, `KEY_HOMEPAGE`, `BTN_START` and `BTN_SELECT`.
- **ZPG A1 and Pocket Micro:** the Home and Back codes.
- **Mangmi Air X:** Back's code, and confirm Home is 316.
- **E6 Max (both revisions):** Back's code, and the USB "Lenovo GamePad" codes.
- **RG Rotate:** which side key sends 67 and which sends 68.
- **RG Vita:** what the 67 "RESET" key is.
- **RG405M and RG505:** that nothing is wired to 68, and the RG505's model string.
- **RG405V:** that the side button sends 68, and its model string.
- **Air Mini:** the printed label.
- **TrimUI and MagicX:** when Menu also sends 256.
- **Any GammaPad device on a Thimsy build:** `dumpsys input`, to see which virtual product ID (`0b13` or `02fd`) and key layout are in use.

### Back acts when it is let go

On every device, the RG Cube included, the Back button acts on its release, never on its press. A Back press that turns into a chord or a hold therefore never also goes back.

- **Over a running app (PhoneWindowManager):**
  - The press is held back.
  - Let go on its own, the app gets a normal Back press and release (injected), without the virtual-key vibration.
  - Held to the long press, it runs the long-press-Back exit as before (where a long-press action is set), and the release sends nothing. A release that comes as the long press fires does exactly one of the two, never both.
  - Pressed during the Anbernic or Start + Select 3 s hold to quit, it cancels the hold, like any other key.
  - Pressed while the Anbernic button is held, it makes a chord: the Anbernic button's release opens no Quick Menu, and with Universal Hotkeys on the release sends nothing. With them off the app gets Back as usual.
  - If the app stops while Back is held, Back's key-up, when it comes, is dropped.
  - Any other key pressed while it is held (Volume, a RetroArch Back + button combination) means nothing is sent. A key-up cancelled by the system (a device reset mid-press) sends nothing either.
  - Held into the 10 s recovery hold, nothing is sent; the recovery hold itself still starts on the press.
  - The device's Back key is used even where its raw code isn't `KEY_BACK` (the AYANEO button, 754, on the Pocket Air Mini): Nano reads it as Back. PhoneWindowManager only sees Android keycodes, and 754 has none, so over apps it gets that button as `BACK` only through GammaPad's default remap (754 → 158).
- **Nano's menus:**
  - Back is held back like Start and Select, and acts (goes back, closes the keyboard or dialog) when let go.
  - Another key pressed meanwhile cancels it.
  - Pressed while the Anbernic button is held, neither acts: no Back, and no Quick Menu on the Anbernic button's release.
  - Holding it no longer repeats.
- **Quick Resume preview:** the same, already on release.

The decision on release is `devkeys::backRelease` (C++) and `ThimsyDeviceKeys.backRelease` (Java), held to the same test cases. With nothing else pressed:
- Back alone, released short of the 10 s recovery hold: Back.
- Where Back doubles as the function button:
  - under 1 s: Back;
  - 1 to 3 s: the Quick Menu;
  - 3 s or more: nothing (that was the hold to quit).

### Devices without a function or Back button

- **No function button, only Back** (RG405M, RG505, and the Retroid Pocket Classic, whose Home and Back both send 158), `"fallback": "back"`. Back + a button replaces the Anbernic chord. Back works like the Anbernic button over a game, except on its own:
  - Back + a button: that button's hotkey. Back + Volume is brightness, Back + Select toggles the shader, and Back + Start opens Applications.
  - Back held on its own for 3 s quits the game.
  - Back tapped and let go within 1 s: the game gets a Back press and release, sent on the release.
  - Back held between 1 s and 3 s: the Quick Menu, as the Anbernic button's release does.
  - In Nano's menus Back goes back when let go within 1 s, and held 1 to 3 s it opens the Quick Menu. Back + Volume, Select and Start work there too.
  - Toolbox → *Anbernic Button Opens Quick Menu* off makes Back plain Back again over apps, and in Nano's menus Back's 1 to 3 s hold no longer opens the Quick Menu (Start + Select still does).
- **Only a Menu button** (TrimUI Brick and the MagicX handhelds):
  - Their Menu button sends `KEY_BACK` (158), so it is their Back button (`"function": []`, `"fallback": "back"`).
  - It doubles as the function button exactly as Back does above: Menu + a button runs the hotkeys, holding it opens the Quick Menu or quits, and a tap is Back on release.
  - Being `BACK`, it is the 10 s recovery key too.
  - B is not Back on these devices.
- **A device with a function button but no Back button** (none known): the function button becomes the 10 s recovery key. PhoneWindowManager adds it when a device's entry has no Back.
- **On every device:** Start + Select still does every hotkey and the 3 s quit, and Power held raises the Quick Menu over a game.

### The Quick Menu stays reachable

Kiosk Mode (Quick Menu, `persist.gammaos.nano.kiosk`) changes only the ES-DE home: Start opens Applications instead of the ES-DE menu, and Select does nothing in a game list. That leaves the Quick Menu, which the Anbernic button opens, as the way to every setting and out of Kiosk Mode. So:

- Turning Toolbox → *Anbernic Button Opens Quick Menu* off also turns Kiosk Mode off.
- Turning Kiosk Mode on also turns *Anbernic Button Opens Quick Menu* back on, rather than refusing: Kiosk Mode is turned on from the Quick Menu, and its design relies on the button to get back there.
- Nano treats Kiosk Mode as off while the button doesn't open the Quick Menu (`devkeys::kioskInEffect`), whatever the properties say, so an `adb setprop` or an old saved state can't lock the menus away either. Where the function button is `HOME`, the button always opens the Quick Menu, so Kiosk Mode is unaffected there.

Other routes to the Quick Menu in Nano's menus, which no setting turns off: Start + Select (Universal Hotkeys, *Start + Select Hotkeys* and Controller Bindings only affect games), and the two-finger tap on the touchscreen (unless *Touch Input* is off). Over a game, Power held raises it and a long press of Back closes the game. Universal Hotkeys off only removes the chords and the hold to quit; the Anbernic button still opens the Quick Menu.

### Adding a device

1. Find the function and Back buttons' codes:
   - `adb shell getevent -lp` gives the input devices and the codes each sends;
   - `adb shell getevent -l` shows a press;
   - the key layout (`/vendor/usr/keylayout/Vendor_XXXX_Product_YYYY.kl`, or `Generic.kl`) gives the Android keycode.
2. Find a property that identifies the device: `adb shell getprop ro.gammaos.device`, `ro.product.vendor.model`.
3. Add an entry to `mods/device-keys.json`, before any broader entry that would also match it. Say in `verified` where the codes come from.
4. To try it without a rebuild, push it to `/system/etc/gammaos/device_keys.json` (root, remounted system) and reboot. `logcat -s NanoDeviceKeys` and `logcat | grep "device keys"` show what Nano and PhoneWindowManager picked.

## Adding an emulator

1. Find its hotkey settings in the source: which actions exist, where they are stored, the format, and whether key events from the virtual keyboard (device id -1) are accepted.
2. Write a profile:
   - `actions` sends unused keys;
   - `provision` binds those keys in the emulator's file;
   - `note` says anything a user should know.
3. Copy it to `/data/system/gammaos/hotkeys/`. The framework picks it up on the next hotkey; Nano binds the keys the next time it comes to the front. Then test it.
4. Ship it in `mods/hotkeys/`.

## Testing

On the device, for each emulator:

1. **Check the bindings:** start a game, return to Nano, then check that the emulator's file gained the bindings (`adb shell cat <file>`).
2. **Check each action:** try each chord. To tell a binding problem from a chord problem, inject the key directly:
   - a tap: `adb shell input keyevent KEYCODE_F2`;
   - a hold: `adb shell input keyevent --longpress KEYCODE_R`.
3. **Watch the log:** `adb logcat -s GammaHotkeys` shows each action, the profile and the driver, and Nano logs `hotkeys: bound keys in <file>`.
4. **Check the edge cases:**
   - Anbernic alone opens the Quick Menu on release;
   - Anbernic, or Start + Select, held 3 s quits; a key pressed during the hold cancels it;
   - on a device whose function button is `HOME` (KT-R1, ZPG A1, Pocket Micro): a tap opens the Quick Menu, a 3 s hold goes Home over a game (RetroArch saves first) and in Nano's menus, with Universal Hotkeys on and off, and with Toolbox's switch showing a fixed On;
   - Toolbox's switch off turns Kiosk Mode off, and Kiosk Mode on turns the switch back on;
   - Start + Select + R1 saves, and Start + Select released opens nothing in a game;
   - Anbernic + an unmapped button does nothing;
   - the 10 s emergency hold still recovers, with Nano's overlay up too;
   - Anbernic + Back over an app opens nothing and, with Universal Hotkeys on, doesn't go back;
   - Anbernic + Select turns the shader off and on again, in Nano and in a game, and Power on its own sleeps;
   - Anbernic + Start opens Applications over a game, and Start, B or Anbernic + Start returns to the game;
   - Anbernic + the bottom button opens the button map over a game (the game keeps running, gets no input, and never sees the button) and in Nano's menus; Start + Select + the bottom button opens it in Nano's menus only; B, Start, the chord again or a tap outside closes it; with Universal Hotkeys off it still works; on Android Games (the system and a game) and Settings it does nothing (`logcat | grep buttonmap:` shows what Nano resolved);
   - rewind released when the Quick Menu opens.
   - chords act on the press: hold Anbernic, press R1 and keep it down; the save happens (and RetroArch shows its message) before R1 is let go. The same for Anbernic + the bottom button (the map shows while it is held) and Start + Select + R1. To time it: `adb shell getevent -lt` beside `adb logcat -v monotonic -s GammaHotkeys WindowManager GammaOSNano` (both in seconds since boot); the R1 `DOWN` and the `save_state -> ...` line should be a few ms apart, and Nano's `overlay: button map ready N ms after the raise began` within about 200 ms of the button map button's `DOWN` (`GammaOS Nano: Anbernic+South -> overlay button map`).
   - Back acts on release: in an app (Settings, a browser) a tap goes back once when let go, a long press runs the exit, and Back held while Volume is pressed never goes back. In Nano's menus Back goes back on release and never repeats while held.
