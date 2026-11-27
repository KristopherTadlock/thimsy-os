# Hotkey Combo Plan

Status: design plan with a read-only audit, revised after two technical reviews. Nothing is implemented.

Date: 2026-11-27

Owner request: "The Start + Select hotkey as the root needs to be rebindable. For example, L3 + R3."

Trees: AOSP `~/gammaos-src` (GammaOS Next 1.4.4 base, branch `nano-144`). Release worktree `thimsy-os-nano` (the framework and Nano code is in `patches/thimsy/0001-thimsy.patch`).

Paths:
- `nano/` means `frameworks/base/cmds/gammaos-nano/`.
- `drastic-nano/` means `frameworks/base/cmds/drastic-nano/`.
- `pwm/` means `frameworks/base/services/core/java/com/android/server/policy/`.
- `gammapad/` means `frameworks/native/services/gammapad/`.
- `mods/` means this repo's `mods/`.
- `P:N` is line N of `patches/thimsy/0001-thimsy.patch`. `file:N` is the line in the patched source, worked out from the hunk headers. Both are for the current patch.
- `bmap plan` means `button_map_overlay_plan.md`. `layout plan` means `controller_layout_plan.md`.

"Unverified" means it was read from source or inferred, not checked on a device. "Upstream" means GammaOS code the patch shows only as context or not at all.

---

## 0. Summary

Today the second way to make a hotkey chord is fixed: **Start + Select**. It is hardcoded in five places, each with its own copy of the logic. This plan makes it a setting, **Hotkey Combo**, in Settings → Gamepad Settings → Hotkeys. It replaces the *Start + Select Hotkeys* On/Off row.

What the owner gets:
- **Hotkey Combo: Start + Select** (default), **L3 + R3**, or **Off**. With the default, everything works as today. The one visible change: the button map hides the combo row while Controller Bindings is Off, since the combo does nothing then (D9).
- Whatever is chosen does everything Start + Select does today over a game: Combo + R1 saves, Combo + L1 loads, Combo + R2 / L2 fast forward and rewind, Combo + the Quit Game Button, and holding the combo for 3 s closes the game.
- **L3 + R3** is offered only where the built-in controller has stick clicks (the RG Cube does). It is hidden on handhelds without sticks (TrimUI Brick, MagicX; to be confirmed per device).
- L3 + R3 makes a chord only when both are pressed together (within 200 ms), so holding L3 to sprint and later clicking R3 doesn't save, load or quit (D10).
- Every text that names the combo (Settings, the Controller Bindings explanation and toast, the button map, the docs) names the one chosen, from the controller's own labels.
- In Nano's menus Start + Select still opens the Quick Menu whatever the setting, and the chosen combo opens it too.
- With L3 + R3 chosen, RetroArch's own menu moves off L3 + R3 (where RetroArch ships it) to holding Start, so a hotkey chord doesn't also open RetroArch's menu (D6).

| Decision | Recommended | Alternative |
|---|---|---|
| D1. Which combos to offer | **Start + Select, L3 + R3, Off.** Each pair has its own clashes (§3) and needs its own device checks. The stored value is two key names (`BUTTON_THUMBL+BUTTON_THUMBR`), and the code handles any pair from {Start, Select, L3, R3}, so more pairs later are menu entries plus checks. **Asked first (Q1)**, before phase 1. | (b) Any two of Start, Select, L3, R3 (six pairs plus Off). Same code; adds the §3.1 checks for the mixed pairs. (c) Press-to-capture any two buttons: not recommended (§3.2). |
| D2. Default | **Start + Select**, as today. | L3 + R3 on new installs with stick clicks: it leaks far less into games. |
| D3. The combo's buttons in a game | **Delivered**: the game still sees them, as Start + Select today. Holding them back would delay every L3 press (sprint in PS2 and PC-style games). | Held back: adds lag and breaks hold-to-sprint. |
| D4. Hold to quit | **Follows the combo.** With L3 + R3 chosen, holding Start + Select no longer quits a game. Freeing Start + Select for games is the point. | Keep Start + Select's hold as well. |
| D5. Start + Select in Nano's menus | **Always opens the Quick Menu**, plus the chosen combo. A route no setting removes (HOTKEYS.md:409). | Only the chosen combo. |
| D6. RetroArch's own menu combo, shipped as L3 + R3 | **Move it to Hold Start** (`input_menu_toggle_gamepad_combo` 2 → 7) while the Hotkey Combo is L3 + R3 and Universal Hotkeys is On. Only if it is still the shipped 2; put back when that stops. The button map gets a "Hold Start" row (today combo 7 has none). | (b) Start + Select (4): gives Start + Select, which D4 frees, to RetroArch's menu, which then also opens on game resets (A + B + Start + Select). (c) Leave it: every L3 + R3 chord also opens RetroArch's menu. |
| D7. A hotkey or the Button Map Button already on L3 or R3 when L3 + R3 is chosen | **Allowed.** Anbernic + L3 still runs it. Only the combo can't use its own buttons: no "combo + L3". The docs say shoulder buttons suit L3 + R3 (no room in the row's text). | Refuse, or swap the button off. |
| D8. Both combo buttons from one controller | **Not now: any controller, as today.** D10 makes a cross-player L3 + R3 chord unlikely. | Same controller, with all built-in input devices counting as one. Needs a check of every device's input nodes first (§9), since a handheld whose Start and Select come from different nodes would lose today's chord. |
| D9. The button map's combo row while Controller Bindings is Off | **Hidden**, since the combo does nothing then. Changes today's map for the default too. | Keep showing it. |
| D10. A time window for pairs with a stick click | **200 ms** between the two presses for L3 + R3 (and any pair with L3 or R3); later than that, no chord. Start + Select keeps no window, as today. Adds no delay: both buttons are delivered anyway. | No window for any pair (sprint + crouch held 3 s quits; sprint + later R3 + L1 loads a state). |

Phases (§10), effort each:
0. Owner decisions and facts. Half a day.
1. The setting, resolvers, migration, Settings and i18n. 2.5 days.
2. PhoneWindowManager, decision logic pulled into tested pure functions first. 2 days.
3. drastic-nano and Nano's menus (per-button hold). 2 days.
4. Button map and gammapad's browser controls. 1.5 days.
5. RetroArch menu combo sync. 1 day.
6. Translations (21 files), docs and device checks. 1.5 days plus device time.

Total: about 11 days, plus device time.

---

## 1. How it works today (audit)

Start + Select is matched by fixed codes everywhere: `KEYCODE_BUTTON_START`/`SELECT` in Java, `BTN_START`/`BTN_SELECT` (315/314) in C++. Unlike the Button Map Button it is never looked up by position or per device. There is no time window: the second key's press arms the chord while the first is still held.

### 1.1 The five places

| Place | Where | What it does |
|---|---|---|
| PhoneWindowManager, over an app | pwm/PhoneWindowManager.java: state `mNanoSsArmed`, `mNanoSsStartDown`/`SelectDown`, `mNanoSsDownMs` (PWM:2818-2824, P:170329-170335); `nanoStartSelectOverApp()` (PWM:2864-2871, P:170375-170382); `nanoOtherPadButtonHeld()` (PWM:2915-2927, P:170426-170438); `nanoHotkeyButton()` (PWM:3078-3086, P:170589-170597); `nanoChordReset()` (P:170599); reset (P:170921-170925); Back cancels the hold (P:170953-170958); **the detector** (PWM:7688-7737, P:171149-171198) | Combo + a hotkey button runs the hotkey and swallows that button. Held 3 s with nothing else: quit (`mNanoHoldQuit`, shared with the Anbernic hold). Never the Quick Menu. Both buttons reach the app. The Anbernic chords run earlier (P:171021-171135) and are unaffected. |
| drastic-nano (DS sessions) | drastic-nano/InputMap.cpp: `hotkeyButton()` (P:976-998), `hatChord` (P:1041-1064), rescan reset (P:1078), `heldKeys` (P:1106-1110), `endHold` exemption for Start/Select (P:1113-1116), main block (InputMap.cpp:814-874, P:1150-1211); InputMap.h `ssStart`, `ssSelect`, `ssArmed`, `holdQuitStartMs` (P:1402-1407) | Its own copy of the detector: hotkeys (D-pad hotkeys too) and the 3 s exit. "Other button held" is the DS button mask (`dsBtnMask & ~(Start|Select)`, P:1203). |
| Nano's menus | nano/NanoMenuInput.cpp: `startSelectKey()` (1485-1507, P:31905-31930), `ssReleaseTaken()` (P:31963), `inputEventsLost` (1573, P:31993), call site (3186-3198, P:33171-33185); NanoMenu.h `mSsHeld[2]`, `mSsFd`, `mSsChord`, `mSsUsed` (P:26696-26700, 26719); held-back keys replay through `mKeyReplay` (P:26716, 31852); resets in NanoMenuButtonMap.cpp:500 (P:28929), NanoMenuDrasticPark.cpp:272 (P:29187), NanoMenuOverlay.cpp:430, 907, 2609 (P:37696, 37970, 38961) | Start and Select are held back until release. Both together open or close the Quick Menu on release. Combo + the Button Map Button opens the map; that check leaves out `BTN_START`/`BTN_SELECT` by name (P:33172). Not gated by any setting. |
| Button map | nano/NanoButtonMap.h `Trigger::kStartSelectSouth` (P:12296-12305); `triggerOpens()` (P:11072); `bmap::chordRows` (P:11735) with the literal row `{"Start + Select", "Works in place of the function button for these"}` (NanoButtonMap.cpp:1179-1180, P:11768-11769), also used by drastic-nano's `DnButtonMap.cpp` (P:530); NanoMenuButtonMap.cpp:407-421 (P:28836-28850); RetroArch menu combos `kCombos` (P:15012, combo 7 has no row); browser rows `ps3xmb/buttonmap/browser.json:25` (P:74421, `BTN_START+BTN_SELECT`) | The map's combo row, and the combo + Button Map Button trigger (menus only). |
| gammapad browser controls | gammapad/BrowserMode.cpp `startChord()`/`endChord()` (253-276, P:174831-174852); `onButton` starts the chord whenever Start and Select are both down, whatever the settings (P:174874-174876); Select as modifier and Start as Ctrl+L, both on release (296-316, P:174874-174894); **L3 = Ctrl+W, R3 = Ctrl+T on the press** (391-396, P:174969-174974); BrowserMode.h comments (P:175154, 175186, 175230) | Passes Start + Select to the virtual pad so PhoneWindowManager sees the chord in Brave and Fulguris. |

### 1.2 Settings, gating and text

- *Start + Select Hotkeys*: `persist.gammaos.hotkeys.startselect`, default 1 (NanoMenuPS3Menu.cpp:9835, P:45122). Read in four places: PWM:2868, drastic-nano InputMap.cpp:864, NanoButtonMap.cpp:1179, and the binding.
- Every hotkey property is read with `SystemProperties.get` / `property_get` on each use (for example `nanoHotkeyButton`, P:170592). The patch has no `addChangeCallback`.
- *Universal Hotkeys* (`persist.gammaos.hotkeys.enable`) gates the hotkeys and the hold.
- *Controller Bindings* (Quick Menu, `sys.gammaos.bindings.enabled`, On at every boot) Off turns off the combo over apps and in drastic-nano (PWM:2869, InputMap.cpp:866), plus the browser controls and mouse mode. It does not hide the map row.
- Neither switch affects Nano's menus.
- The hotkey buttons and the Button Map Button can already be L3 or R3 (`kHotkeyButtons`, `kButtonMapButtons`, NanoMenuPS3Menu.cpp:9611-9628, P:45046-45062). Start and Select are refused as hotkey buttons (PWM `nanoHotkeyButton`, drastic-nano `hotkeyButton`, Nano's start-up clean-up P:24803-24812).
- Text that names "Start + Select" (4 Thimsy keys, in all 21 `ps3xmb/i18n/*.json` files):
  1. the row name "Start + Select Hotkeys";
  2. its description (P:43537);
  3. the Quit Game Button description (P:43547), `%1$s` for the function button;
  4. the Controller Bindings description (NanoMenuPS3Menu.cpp:1990-1992, P:42815-42817), `fnText` with one `%s`.
- The separate Controller Bindings rewording job (TODO.md, "Controller Bindings explanation") adds three more: a new description, a toast "Games get Start + Select", and a cross-reference appended to the *Start + Select Hotkeys* description (§2.4).
- The 7 newer language files (da, el, fi, he, nb, sv, th) also hold GammaOS's "Start+Select LED" strings. They are the LED feature, not the combo. Leave them.
- `fnText` (P:42378) fills exactly one name, the function button's.
- `test/i18n_check.py` holds every `fnText` key to one `%s` or `%1$s` throughout. Its `source_keys()` finds keys only in `trDyn`, `tfmt` and `fnText` calls (P:152558). It has no check for unused keys.
- A translation must have exactly its key's printf conversions **in the same order**: Nano's `i18nFormatSafe` (P:23676) drops one that differs at load (English shows), and `i18n_check`'s `format_safe` (P:152443) fails it. So today `%1$s`/`%2$s` can't be reordered by a translation.

### 1.3 Facts that shape the design

- **RetroArch already uses L3 + R3.** The shipped `retroarch.cfg` has `input_menu_toggle_gamepad_combo = "2"` (L3 + R3) and `input_enable_hotkey_btn = "109"` (Select), with Select + L2 / R2 / R1 / A for load, save, fast forward and rewind (test fixture `test/fixtures/buttonmap/retroarch/shipped/.../retroarch.cfg`, P:151446 on; bmap plan line 231). Start (108), L3 (106) and R3 (107) are not RetroArch hotkeys. The button map shows that combo (P:15009; test P:155673 expects "L3 + R3"). Combo 7 (Hold Start) has no row in the map (`kCombos[7]` is empty, P:15012).
- **Select is RetroArch's hotkey-enable button whatever Thimsy's combo is.** Select + L2 / R2 / R1 / A are RetroArch's own hotkeys with any Hotkey Combo. D4 frees Start + Select from *Thimsy's* hotkeys and hold; it can't free Select inside RetroArch.
- **The RG Cube has L3 and R3.** Its layout maps 317 to `BUTTON_THUMBL` and 318 to `BUTTON_THUMBR` (`mods/keylayout/Vendor_484b_Product_1101.kl`), and `pads.json` lists them for `rg_cube` (484b:1101, P:74428 on). L2 and R2 are keys too (312, 313).
- **The browser uses L3 and R3** for close tab and new tab, on the press.
- **Nano's ES-DE home uses L3 / R3** for a random game, on the press (NanoMenuInput.cpp:4075, P:33805).
- **Emulators' own L3 / R3 binds**, known so far: M64Plus FZ's default "Android Gamepad" profile has L3 = Z and R3 = Start (bmap plan line 489); ARMSX2 binds L3/R3 as the PS2's (bmap plan line 400); RetroArch's PS1 cores use L3/R3 with a DualShock or Analog pad (bmap plan lines 282, 308); ScummVM maps them to its stick clicks (bmap plan line 498). Azahar, Dolphin, Flycast, PPSSPP and Vita3K are not checked yet (Phase 0).
- **Mouse mode** is Select + R1 held 2 s, per bmap plan line 119 (`gammapad/MouseMode.cpp:23-26`, upstream, not in the patch; unverified). No clash with L3 + R3.
- `nanoOtherPadButtonHeld` lists A, B, X, Y, L1, R1, L2, R2, THUMBL, THUMBR and the D-pad, read from every device (`getKeyCodeState(-1, ...)`). With L3 + R3 as the combo it would refuse every chord. It must leave out the combo's own buttons.
- drastic-nano's `heldKeys` (P:1106-1110) is filled from EV_KEY only: the hat D-pad never enters it (ABS_HAT0X/Y set `dsBtnMask` and `hat0xPrev`/`hat0yPrev`). It also holds non-pad keys (the function key, Volume, KEY_BACK, keyboard keys).
- drastic-nano keeps upstream `btnL3`/`btnR3` state (P:1072, context only). What upstream drastic-nano does with them is not visible in the patch (§11, Q3).
- NanoMenu.h has an upstream `mStartHeld`, commented "START+SELECT PS3 boot-intro re-trigger" (P:26451, context only). Its use is not in the patch (Phase 0).
- gammapad's virtual pad declares `BUS_BLUETOOTH` (VirtualGamepad.cpp:71, P:175549).
- **Where the function button is Android's `HOME`,** its 3 s hold goes Home, so the combo's hold is the only hold to quit (HOTKEYS.md:23). Hotkey Combo Off removes that, as *Start + Select Hotkeys* Off does today.
- Nothing in `mods/hotkeys/*.json` or `mods/55-hotkeys.sh` mentions Start, Select, L3 or R3. PPSSPP and DraStic configs don't depend on them.

---

## 2. What the owner sees

### 2.1 The row

Settings → Gamepad Settings → Hotkeys, where *Start + Select Hotkeys* is today:

| Row | Property | Options | Default |
|---|---|---|---|
| Hotkey Combo | `persist.gammaos.hotkeys.combo` | Start + Select, L3 + R3, Off | Start + Select |

- The stored values are `BUTTON_START+BUTTON_SELECT`, `BUTTON_THUMBL+BUTTON_THUMBR` and `NONE`: keycode names, as the other hotkey properties use.
- The option names are the built-in controller's labels joined by " + ". They are not translated, as button names aren't today. "Off" is translated.
- **L3 + R3 is listed only where the built-in controller has both buttons** (§5.1). Elsewhere the row offers Start + Select and Off.
- Description: one `fnText` key, 123 characters (129 with "Function"), within the 4-line budget at 640x480 and 720x720 that TODO.md's fit model gives (about 125 characters, 131 with the button name):
  > In a game, the buttons shown work like %s for hotkeys; holding them 3 s quits. If a game needs them, choose another or Off.
- "The buttons shown" is the row's value beside it. The description doesn't say that Start + Select always opens the Quick Menu in Nano's menus (D5) or that shoulder buttons suit L3 + R3 (D7): there is no room. HOTKEYS.md and README say both.
- It replaces the rewording job's cross-reference ("If a game needs Start + Select, turn off Controller Bindings…", §2.4): with this row, choosing another combo or Off is the better advice, and it keeps the browser controls.

### 2.2 How each behaviour follows the combo

"Combo" below is the chosen pair. With Off, nothing in this table happens over a game.

| Behaviour | Today (Start + Select) | With the combo |
|---|---|---|
| Combo + a hotkey button, over a game or app | runs the hotkey, swallows that button | the same, for the chosen pair |
| Combo held 3 s with nothing else, over a game | quit | the same; Start + Select's hold does nothing when the combo is L3 + R3 (D4) |
| Any other key during the hold | cancels until both are let go | the same |
| Another pad button already held when the combo forms | no chord (A + B + Start + Select resets many games) | the same rule; "another button" means any pad button but the combo's two, so Start and Select count when they aren't the combo |
| The two presses far apart | still a chord | L3 + R3: no chord if the second comes more than 200 ms after the first (D10). Start + Select: no window, as today |
| Combo in a DS session (drastic-nano) | hotkeys, D-pad hotkeys, 3 s exit | the same |
| Combo in Nano's menus, including the Quick Menu over a game | opens or closes the Quick Menu on release | Start + Select always does; the combo does too (D5) |
| Combo over a game with Nano's overlay down | PhoneWindowManager's chord (rows above), never the Quick Menu | unchanged |
| Combo + the Button Map Button | opens the map in Nano's menus only | Start + Select and the combo both do; never over a game (unchanged). Not when the Button Map Button is one of the combo's own buttons (D7) |
| Combo in a browser (gammapad) | passed through to Android | the combo's pair is passed through; Start + Select keep their browser actions when they aren't the combo; L3 / R3 tab shortcuts move to the release when they are in the combo |
| Universal Hotkeys Off | no combo hotkeys or hold | unchanged |
| Controller Bindings Off | no combo hotkeys or hold over apps and in DS | unchanged, for whichever combo |
| Anbernic + Start / Anbernic + Select | shader / Applications, come first | unchanged; Start and Select stay reserved as hotkey buttons |
| The Anbernic button's own chords | unaffected | unaffected |

### 2.3 Held back or delivered

| Where | The combo's two buttons | The hotkey button pressed with the combo |
|---|---|---|
| Over a game or app (PhoneWindowManager) | **delivered**, both, the moment they're pressed (D3) | swallowed, with its repeats and key-up |
| DS session (drastic-nano) | delivered to the DS game if mapped | swallowed |
| Nano's menus | **held back** until release, then replayed if they weren't a chord. This already happens for Start and Select. With L3 + R3, L3 and R3 are held back too; the replay reaches ES-DE's random game, which then acts on the release with no extra code | swallowed (a chord) |
| Browser (gammapad) | Start and Select already act on release. With L3 + R3, Ctrl+W / Ctrl+T move to the release and fire only if the partner wasn't pressed meanwhile | as over an app |

Why delivered: holding L3 back would delay every press until release or until R3 arrives. That breaks hold-to-sprint in PS2 (ARMSX2) and PC-style games, and a timeout adds lag and still leaks.

What leaks with L3 + R3: the game sees L3 and R3 pressed together at each chord, and keeps them through a 3 s hold. PSP, GameCube, 3DS, Dreamcast and DS have no stick clicks, but their emulators may still bind L3/R3 to something (Phase 0 checks). Known leaks: RetroArch's menu combo (D6), M64Plus FZ's L3 = Z and R3 = Start (the chord presses Z and Start), and PS2 / PS1-DualShock games, which get both stick clicks at once.

### 2.4 Controller Bindings and its text

Controller Bindings Off turns off "the hotkey combo", whichever it is, and its hold to quit. The logic is unchanged. Only its text and the map row change.

The rewording job (TODO.md, "Controller Bindings explanation") has final English strings that name "Start + Select" literally. With a rebindable combo, each becomes:

| String | Combo chosen | Hotkey Combo Off |
|---|---|---|
| Description (Quick Menu > Controller > Controller Bindings) | `comboText`, `%2$s` the combo, `%1$s` the function button: "Off until restart: games get %2$s. Shortcuts, hold to quit, browser controls, mouse mode stop. %1$s opens this menu." (124 characters with Start + Select, as the rewording job measured) | `fnText`: "Off until restart: browser controls and mouse mode stop. %s opens this menu." (the rewording job's Off variant) |
| Toast when turned Off | `comboName`, one `%s` the combo: "Games get %s" (24 characters with Start + Select) | none: the generic "Setting changed" toast, as today. Games gain nothing then |
| Cross-reference on *Start + Select Hotkeys* | retired with that row; the Hotkey Combo description carries the advice (§2.1) | retired |
| Button Map hint "Controller Bindings is off (Quick Menu)" | unchanged | unchanged |

- Start + Select is the widest combo label, so the rewording job's fit numbers hold for the other labels.
- The literal "Start + Select" must not come back in any of these keys.
- If the rewording job lands first, phase 1 turns its description and toast into these keys and removes its cross-reference key with the row. If this lands first, the rewording job writes these keys.
- The text that follows the setting is picked when the item is built, so after changing Hotkey Combo the Quick Menu shows the new text the next time it opens.

The Quit Game Button description drops "or Start + Select" (one `fnText` key, `%1$s` only): "…as holding the %1$s button for 3 seconds does." The combo's hold is described in the Hotkey Combo row instead.

### 2.5 Button map

- The combo row's label is built from the pad in hand: `pad.labelOf(a) + " + " + pad.labelOf(b)` (as the Anbernic rows are built), so an Xbox pad shows its own names for L3 and R3. The action text stays "Works in place of the function button for these".
- The row shows only when the combo isn't Off, the pad has both buttons, and Controller Bindings is On (D9).
- drastic-nano's panel gets the same row through the shared `chordRows`.
- With D6 active, RetroArch's menu shows as "Hold Start — RetroArch menu" (a new row for combo 7).
- Browser controls: the `BTN_START+BTN_SELECT` row becomes a `HOTKEY_COMBO` row that the reader fills with the combo's labels, or hides when the combo is Off.

### 2.6 Docs

HOTKEYS.md, README and the bmap plan say "the hotkey combo (Start + Select by default)" where they now say "Start + Select" for the chord. Lines in §6.12.

---

## 3. Why these combos

### 3.1 Pairs left out

| Pair | Why not |
|---|---|
| L1 + R1, L2 + R2 | The default save/load and fast forward/rewind buttons. Pressed together in many games. Triggers arrive as axes from Xbox-style Bluetooth pads and gammapad's 0b13 profile (bmap plan line 240), so PhoneWindowManager may never see the keys. |
| Anything with the D-pad (Down + Select, Down + Y + L + R) | The D-pad reaches Android as a hat axis, which PhoneWindowManager never sees (HOTKEYS.md:206). |
| L3 + R1, L3 + R (RetroArch's 5) | R1 is a hotkey button. |
| L1 + R1 + Start + Select | Contains Start + Select and two hotkey buttons. |
| Hold Start, Hold Select | One button would have to be delayed or leak a long press. Anbernic + Start / Select are reserved. |
| Select + a face button | Select is RetroArch's hotkey-enable button. Face-button combos are common in games. |
| Mixed pairs (Select + R3, Start + L3, …) | Workable with the same code (§6.3 holds buttons back one by one, so pairs may share a button). They don't clash with RetroArch's L3 + R3 menu, so D6 isn't needed for them. They still leak Start or Select into the game, and each needs the §9 checks. Offered only under D1 (b). |

How other systems do it, for reference: RetroArch offers a fixed list of menu combos (NONE, L3 + R3, Start + Select, Hold Start, ...) and delivers combo buttons to the core. Batocera has one configurable hotkey button. ROCKNIX and ArkOS use Select (or a Function key) as hotkey enable (from memory, unverified). Steam Deck uses a dedicated button. None captures a free two-button root.

### 3.2 Why a list, not capture

- Capture would still need every exclusion above, and error messages for them in 21 languages.
- It can't be done by touch.
- It can store a pair a later controller can't press.
- The stored format keeps capture possible later (any `A+B` of keycode names; the readers accept only the supported set and fall back to the default otherwise).

---

## 4. The resolver

One rule set, in Java and C++, held to the same cases (as the device-key resolvers are).

**Input:** `persist.gammaos.hotkeys.combo`, the legacy `persist.gammaos.hotkeys.startselect`, and the device's `stick_clicks` field in `device_keys.json` (§5.1). Java (`ThimsyDeviceKeys`) and C++ (`NanoDeviceKeys`) already read that table, so rule 3 uses the same input in both.

**Rules:**
1. `combo` set and valid (two different names from {`BUTTON_START`, `BUTTON_SELECT`, `BUTTON_THUMBL`, `BUTTON_THUMBR`}, or `NONE`): use it. D1 (a) offers two of the pairs; the parser accepts the set so (b) needs only menu entries.
2. `combo` unset or invalid: `startselect` "0" means `NONE`, otherwise Start + Select. This covers the first boot after the update, before Nano migrates.
3. A pair with L3 or R3 where `stick_clicks` is false: Start + Select (Nano also rewrites the property, §6.7).

**Output:** two Android keycodes, two evdev codes (314/315/317/318), an enabled flag, whether the D10 window applies (the pair has L3 or R3), and a label ("Start + Select", "L3 + R3") from the built-in pad's `pads.json` labels, falling back to those English names.

**When it is read:** on each use, as every hotkey property is today (§1.2). No caching, no change callback. A chord that is armed when the pair changes is dropped.

| Language | Where |
|---|---|
| Java | `pwm/ThimsyDeviceKeys.java`: `hotkeyCombo()` returning `int[2]` or null, `comboWindow()`, `comboLabel()` |
| C++ | `nano/NanoDeviceKeys.cpp/.h`: `devkeys::hotkeyCombo()` (evdev codes and Android names), `devkeys::comboPrompt()`. drastic-nano already links Nano's shared code for the button map, so it uses the same function. |
| C++ (gammapad) | a small copy of rules 1-2 only (gammapad doesn't link Nano). On a device without stick clicks it relies on Nano's rewrite (§6.7); until Nano first runs it may chord on an `adb setprop`'d L3 + R3, which is harmless. |
| Cases | `nano/test/fixtures/devkeys/cases.json`: a new `hotkey_combo` group, with the host copy in `tests/fixtures/device_keys/cases.json` that `DeviceKeysCheck.java` reads. gammapad's copy runs the cases without `stick_clicks: false`. |

---

## 5. Availability and gating

### 5.1 Which devices get L3 + R3

- `device_keys.json` (`mods/device-keys.json`) gets an optional `"stick_clicks": false` per device. It defaults to true. Phase 0 sets it for each device without stick clicks.
- Nano cross-checks at start, log only: it finds the built-in pad by `pads.json` vendor:product (the Cube's is 484b:1101) and logs when `stick_clicks` and the pad's `EVIOCGBIT(EV_KEY)` (317 and 318) disagree. It doesn't decide from EVIOCGBIT: the Cube pad's bus type is unverified, and gammapad's virtual pad declares every button on `BUS_BLUETOOTH`.
- Likely without stick clicks, unverified: TrimUI Brick, MagicX Mini Zero 28, maybe Zero 40 and XU20 V32. They join the `getevent -lp` list (HOTKEYS.md:343).
- A Bluetooth pad without stick clicks just can't make L3 + R3. The Anbernic button, Power held (Quick Menu over a game) and Start + Select in Nano's menus are still there, so no one is locked out.

### 5.2 Switches

| Switch | Effect on the combo |
|---|---|
| Hotkey Combo Off | no combo hotkeys or hold over games; Start + Select still opens the Quick Menu in Nano's menus. Where the function button is `HOME`, no hold to quit is left (as with *Start + Select Hotkeys* Off today) |
| Universal Hotkeys Off | no combo hotkeys, no hold; RetroArch's menu combo goes back to 2 (§6.8) |
| Controller Bindings Off | no combo hotkeys or hold over apps and DS; map row hidden (D9) |
| Toolbox *Anbernic Button Opens Quick Menu* Off | no effect on the combo |

---

## 6. Changes, by part

### 6.1 PhoneWindowManager (`pwm/PhoneWindowManager.java`)

First pull the decision into pure static functions, as `ThimsyDeviceKeys.functionRelease` was, so it can be tested on the host (§8). Then:
- Rename `mNanoSs*` to `mNanoCombo*` (PWM:2818-2824, P:170329-170335), with `mNanoComboDown[2]` and `mNanoComboDownAt[2]` (event times of each down, for D10).
- Read the resolver on each combo-key event, as `nanoHotkeyButton` reads its property (P:170592). If the pair differs from the one an armed chord was made with, drop the chord.
- `nanoStartSelectOverApp()` → `nanoComboOverApp()` (PWM:2864-2871, P:170375-170382): the resolver's enabled flag replaces the `startselect` read; keep `sys.gammaos.bindings.enabled` and `nanoOverApp()`.
- `nanoOtherPadButtonHeld()` (PWM:2915-2927, P:170426-170438): leave out the combo's two keycodes; add START and SELECT when they aren't the combo. It reads every device (`getKeyCodeState(-1, ...)`), so player 2 holding a button blocks player 1's combo, as today.
- **D10:** arm only if the second down's event time is within 200 ms of the first's, when the resolver says the window applies.
- `nanoIsHotkeyButton()` (PWM:3098, P:170609): no change needed. Combo keys always take the combo branch before the hotkey branch (P:171155 vs 171183), so a hotkey on L3 never runs as "combo + L3". A guard that returns false for the combo's own buttons is optional.
- `nanoHotkeyButton()` (P:170589-170597) is unchanged (Start and Select stay reserved for the Anbernic chords).
- The detector (PWM:7688-7737, P:171149-171198): `ssKey` becomes "is one of the combo's keycodes". Arming, release, the 3 s `mNanoHoldQuit`, the "pressed again" cancel and the lost key-up drop keep their logic.
- Resets and the Back cancel (P:170599, 170921-170925, 170953-170958): rename only.
- Comments at PWM:2782, 2818, 2864, 2915, 3088, 7492 say "the hotkey combo".
- Leave alone: the Anbernic + Select / Start block (PWM:7562-7578) still runs first. The old RetroArch Back → `BTN_SELECT` injection (`mRetroarchSelectDown`, P:170262-170272) can still arm a Start + Select combo, as today (D8 not taken); it can't arm L3 + R3.

### 6.2 drastic-nano (`drastic-nano/InputMap.cpp`, `InputMap.h`)

- Rename `ssStart`, `ssSelect`, `ssArmed` to `comboDown[2]`, `comboArmed`, plus `comboDownMs[2]` for D10 (`android::elapsedRealtime()`) (InputMap.h, P:1402-1407).
- Main block (InputMap.cpp:814-874, P:1150-1211): match the resolver's evdev codes instead of `BTN_START`/`BTN_SELECT`. The `startselect` read becomes the resolver; keep the bindings read.
- "Other button held" (P:1203): the DS mask can't express L3/R3, and a user remap can put them on DS buttons. Use instead: any pad button in `heldKeys` (`BTN_A`..`BTN_THUMBR`, `BTN_DPAD_*`, `KEY_UP`..`KEY_RIGHT`) other than the combo's two codes, or a hat that isn't centred (`hat0xPrev`/`hat0yPrev`). Not `heldKeys` as a whole: it misses the hat and holds the function key, Volume, Back and keyboard keys.
- `hatChord` (P:1041-1064) and the rescan reset (P:1078): rename.
- `endHold` exemption (P:1113-1116): exempt the combo's codes, not Start and Select.
- `hotkeyButton()` (P:976-998): unchanged.
- Check what upstream drastic-nano does with `btnL3`/`btnR3` (P:1072) before L3 + R3 ships (§11, Q3).

### 6.3 Nano's menus (`nano/NanoMenuInput.cpp`, `NanoMenu.h`)

Two pairs are live: Start + Select (always, D5) and the combo. Under D1 (b) they can share a button (Start + Select and Start + L3), so the hold-back is per button, with pairs on top:
- Per button, for the four that can be in a pair (Start, Select, L3, R3): `held`, `fd`, and whether it is held back. A button is held back while it belongs to a live pair.
- Per pair: `chord` and `used`, as `mSsChord` / `mSsUsed` today.
- Replace `mSsHeld[2]`, `mSsFd`, `mSsChord`, `mSsUsed` (P:26696-26700, 26719) with this, and `startSelectKey()` (1485-1507, P:31905-31930) with a function that updates the button, then each pair it belongs to.
- A pair whose two buttons are both down is a chord; once both are up and it wasn't used, it toggles the Quick Menu. A held-back button is replayed once, on its release, if no pair it belongs to became a chord.
- `ssReleaseTaken()` (P:31963), `inputEventsLost` (1573, P:31993) and the resets (NanoMenuButtonMap.cpp:500, NanoMenuDrasticPark.cpp:272, NanoMenuOverlay.cpp:430, 907, 2609): over both pairs and the per-button state.
- The call site (3186-3198, P:33171-33185): combo + the Button Map Button checks the pair's own codes instead of `BTN_START`/`BTN_SELECT` by name. When the Button Map Button is one of the pair's buttons, that pair has no Button Map chord (D7). The same rule for the Switch Screen Button, if the combo can chord with it in the menus (check in phase 3).
- ES-DE's L3 / R3 random game (NanoMenuInput.cpp:4075, P:33805): no change. A held-back L3 is replayed on release through `mKeyReplay` and reaches the same case.

### 6.4 Button map (`nano/NanoButtonMap.*`, `nano/NanoMenuButtonMap.cpp`, `nano/NanoRetroarch*` reader, drastic-nano `DnButtonMap.cpp`)

- Rename `Trigger::kStartSelectSouth` to `kComboSouth` (NanoButtonMap.h P:12298, NanoButtonMap.cpp P:11072, NanoMenuButtonMap.cpp P:28850). The "Decided 2026-11-13" comment says "the hotkey combo".
- NanoMenuButtonMap.cpp:409 (P:28837): `startSelect` is `!(mAnbernicHeld && anbernicPhysicallyDown())`, meaning "not the Anbernic chord". Rename it (`notAnbernic`); its logic stays.
- Hotkey rows in `bmap::chordRows` (NanoButtonMap.cpp:1157-1191, P:11735, 11746-11780): the literal row becomes the pad-label row, gated as §2.5. `chordRows` is pure: it gets the combo from `env.prop` (the property values) and the pad model, not from a resolver call that touches devices.
- RetroArch reader (P:15005-15020): a row for combo 7, "Hold Start", so D6's value shows.
- `browser.json` (P:74421): `HOTKEY_COMBO` row, resolved by the browser reader; the `BrowserMode::onButton` coverage test (note at P:74399) accepts it for every pair.
- Browser state (NanoMenuButtonMap.cpp:156, P:28585): unchanged.

### 6.5 gammapad browser controls (`gammapad/BrowserMode.cpp`, `.h`)

- Read the combo from the property with a copy of resolver rules 1-2 (§4), on each button event.
- `startChord()`/`endChord()` (P:174831-174852), the chord tracking (P:174808-174811) and the `onButton` test (P:174874-174876): chord only on the chosen pair. Today Start + Select chords whatever the settings.
- With the combo Off or not Start + Select, Start and Select keep their own browser actions even when both are down (Ctrl+L on Start's release, Select as modifier and Ctrl+Shift+B).
- With L3 or R3 in the combo: Ctrl+W / Ctrl+T (P:174969-174974) fire on the release, and only if the partner wasn't pressed meanwhile, as Select is handled today.
- Header comments (P:175154, 175186, 175230).

### 6.6 Settings and text (`nano/NanoMenuPS3Menu.cpp`)

- Binding (9835, P:45122): `{"Hotkey Combo", kProp, "persist.gammaos.hotkeys.combo", "BUTTON_START+BUTTON_SELECT", "BUTTON_START+BUTTON_SELECT:Start + Select,BUTTON_THUMBL+BUTTON_THUMBR:L3 + R3,NONE:Off"}`. The chooser drops L3 + R3 where §5.1 says so. The option labels come from `devkeys::comboPrompt()` for each pair.
- `buildGpHotkeys` (2803-2832, P:43527-43558): the row and its description (§2.1); the Quit Game Button description (§2.4).
- Controller Bindings (1990-1992, P:42815-42817): the two variants (§2.4), picked by the resolver's enabled flag.
- The toast for `bindings` = 0 (§2.4; the rewording job adds the toast path): `comboName("Games get %s")`, or the generic toast when the combo is Off.
- New helpers next to `fnText` (P:42378):
  - `comboText(english)`: `%1$s` = `devkeys::functionPrompt()`, `%2$s` = `devkeys::comboPrompt()`. Every key uses both.
  - `comboName(english)`: one `%s` = `devkeys::comboPrompt()`.
- `hotkeyTakeButton` (2859-2877, P:43585-43603): unchanged under D7. Under the alternative it would refuse L3/R3 while the combo holds them.
- Comment at 9876-9877 (P:45157-45159): Controller Bindings turns off "the hotkey combo".

### 6.7 Migration (`nano/NanoMenu.cpp`, next to P:24803-24815)

At start, once:
- `combo` unset and `startselect` "0": write `combo=NONE`. Leave `startselect` in place, unused.
- `combo` names L3 or R3 where `stick_clicks` is false: write Start + Select, and log it.

PhoneWindowManager and drastic-nano apply rules 2-3 of §4 themselves, and gammapad rule 2, so behaviour is right before Nano first runs.

### 6.8 RetroArch menu combo (D6; `nano/NanoHotkeys.cpp`)

A small `retroarchComboSync()` on both the default `retroarch.cfg` and the config Nano starts RetroArch with. "Active" means the Hotkey Combo is L3 + R3 and Universal Hotkeys is On. The target is 7 (Hold Start) under D6, or 4 under its alternative (b).
- **Active** and `input_menu_toggle_gamepad_combo` is `"2"`: set it to the target and set `persist.gammaos.hotkeys.ra_combo_moved=1`. This also covers RetroArch writing its old value back on exit (`config_save_on_exit`): the next pass moves it again.
- **Not active** and the marker is set: if the value is still the target, put back `"2"`. Clear the marker either way.
- **Active**, marker set, and a value other than 2 or the target: the user's. Clear the marker and leave it.
- Any other value is the user's: never touched. A user who sets 2 by hand while the combo is L3 + R3 has it moved again; the log says so.

When it runs:
1. In `provision()` (P:23406), **before** its Universal Hotkeys check, so turning hotkeys off restores 2.
2. When the Settings dialog closes (`closePs3Dialog`) after Hotkey Combo or Universal Hotkeys changed, if RetroArch isn't running.
3. Right before Nano starts RetroArch, so the first launch after the change already has it.

Also:
- Log each change. Add a line to `mods/hotkeys/retroarch.json`'s `note`.
- RetroArch's Hold Start delay is about 2 s (`HOLD_BTN_DELAY_SEC` in RetroArch's source, from memory; check on device).

### 6.9 Profiles

- `mods/hotkeys/mupen64plus-fz.json` `note`: "With the L3 + R3 hotkey combo, M64Plus FZ's default profile also gets Z (L3) and Start (R3), so the combo pauses."
- No other profile changes, unless Phase 0 finds another emulator that binds L3 or R3 to an action of its own.

### 6.10 Strings (`nano/ps3xmb/i18n/*.json`, 21 files; `nano/test/i18n_check.py`; NanoI18n)

New keys (6): "Hotkey Combo"; its description (`fnText`); the Controller Bindings description, combo variant (`comboText`) and Off variant (`fnText`); the toast (`comboName`); the revised Quit Game Button description (`fnText`).

Retired keys, removed explicitly from all 21 files (i18n_check has no unused-key check): "Start + Select Hotkeys", its description, the old Controller Bindings description, the old Quit Game Button description, and the rewording job's description, toast and cross-reference keys if it landed first. That is 4 retired keys, or 7 if the rewording job landed first.

Checks:
- `source_keys()` in i18n_check: add `comboText` and `comboName` to the pattern (P:152558), so the new keys are required in every language.
- `comboText` keys use `%1$s` and `%2$s` only, each at least once. `comboName` keys use one `%s` or `%1$s`.
- Reordering: when every conversion in a key is numbered (`%N$`), `i18nFormatSafe` (P:23676) and `format_safe` (P:152443) compare the sorted conversions instead of their order, so a translation may put the combo before the function button. Both get cases for it (Nano's test at P:159551).
- No `fnText`, `comboText` or `comboName` key contains "Start + Select". Today only the keys retired here do.

### 6.11 Downstream

The Linux port carries a copy of Nano with the same code and strings (`thimsy-os-linux`, `nano/`). Port phases 1, 3 and 4 there after this ships.

### 6.12 Docs

- HOTKEYS.md: What it does (lines 9-31, including the `HOME` line at 23), Chord detector step 5 (77-81) with the D10 window, the Nano's menus paragraph (87), the settings table (200) and its notes (206, 209, with D7's advice to put the hotkeys on shoulder buttons with L3 + R3), Back (364, 371), devices (392, 399), Quick Menu routes (409), Testing (442-453).
- README.md: 486-495 (Start + Select and hold to quit), 538 (L3 / R3 tabs act on release with the L3 + R3 combo), 546, 751 (Controller Bindings), 870 (button map).
- bmap plan: 60, 115, 146, 461, 795, 843, 992, 1043 (notes that the row follows the combo).
- layout plan: none needed.

---

## 7. Risks

1. **RetroArch's menu opens on every L3 + R3 chord** if D6 is "leave it". The sync (§6.8) moves it without touching a user's own value. It edits a user config file, only the shipped value.
2. **The browser closes and opens a tab** on every L3 + R3 chord unless gammapad moves those shortcuts to the release (§6.5).
3. **Games that use L3 / R3** still get them, both at once at each chord. L3 + R3 + R1 saves instead of reaching the game: the same trade-off as Start + Select + R1 today. Without D10, a player holding L3 to sprint who clicks R3 and then presses L1 loads a state, and sprint + crouch held 3 s quits. D10's 200 ms window makes both need a near-simultaneous press. Moving the sticks is not a key, so it doesn't cancel a hold.
4. **Emulators' own L3/R3 binds** are only partly known (§1.3). Phase 0 checks the rest.
5. **A hotkey on a face button** is hard to reach with both thumbs on the sticks. The docs say so (D7).
6. **PhoneWindowManager has no host tests today.** Refactoring it without first extracting pure functions risks system_server regressions. Phase 2 does the extraction first.
7. **gammapad `remap_btn`** can make PhoneWindowManager (virtual pad) and Nano or drastic-nano (raw evdev) disagree on which button is L3. Already true for every button today.
8. **Start-up order:** PhoneWindowManager reads the properties before Nano migrates; rules 2-3 of §4 cover it.
9. **The parallel Controller Bindings rewording** will conflict unless it uses the keys of §2.4.
10. **21 translations.** The new keys need translating; machine drafts are reviewed like the other strings.

---

## 8. Tests

Host:
- **Resolver cases** (`nano/test/fixtures/devkeys/cases.json`, group `hotkey_combo`): each valid value, `NONE`, invalid values, the legacy fallback (`startselect` 0 and 1), L3 + R3 with `stick_clicks: false`, the window flag, the labels. Run by `NanoDeviceKeys_test` (C++), `tests/fixtures/device_keys/DeviceKeysCheck.java` (Java) and gammapad's copy (rules 1-2).
- **Detector decisions** (new group `combo_step`): arm on the second press; no arm with another pad button held (Start held counts when the combo is L3 + R3); a hotkey button runs and is swallowed; a combo button set as hotkey button doesn't run as "combo + itself"; hold reaches 3 s; a key during the hold cancels; pressed again cancels; lost key-up dropped; the pair changes while armed → dropped; L3 + R3 210 ms apart → no arm, 150 ms → arm; Start + Select 2 s apart → arm (no window). The same cases for the Java pure function and drastic-nano's C++. drastic-nano adds: a held hat D-pad blocks arming; a held Volume or function key doesn't.
- **Nano's held buttons**: release replays a lone button once; both open the Quick Menu once; another key cancels; combo + Button Map Button marks used; a Button Map Button that is one of the combo's buttons makes no chord; under D1 (b), Start + Select and Start + L3 live together and Start is replayed once.
- **Button map**: `ButtonMapTrigger.ComboOnlyInNanosMenus` and `ButtonMapContext.ComboDoesNothingOverAGame` (renamed from P:156874, P:157070, and P:155158); the `chordRows` tests (P:157398 on) get the label "L3 + R3" for L3 + R3, no row for Off, no row with bindings Off; RetroArch combo 7 shows "Hold Start"; browser.json coverage for every pair.
- **Scene goldens**: regenerate `test/buttonmap/scenes/gamecube.json`, `ps2_hotkeys.json`, `ps2_page2.json` (`"keys"` stays "Start + Select" at the default), and add `ps2_hotkeys_l3r3.json`.
- **RetroArch sync**: shipped 2 → 7 with the marker; back to 2 when the combo changes or Universal Hotkeys goes Off; RetroArch's write-back of 2 while active → 7 again; a user's 0 or 4 never touched; marker set but value changed by the user → cleared, value kept.
- **Migration**: unset + `startselect=0` → `NONE`; L3 + R3 with `stick_clicks: false` → Start + Select.
- **gammapad**: with the combo L3 + R3 or Off, Start + Select in browser mode keeps Ctrl+L and the Select modifier and starts no chord.
- **i18n_check and `i18nFormatSafe`**: `comboText` keys must have both placeholders; a reordered `%2$s … %1$s` translation passes; a dropped placeholder fails; `comboName` with one `%s`; the new keys are required in every file; no "Start + Select" in those keys.
- `tests/unit/test_device_keys.py`: the optional `stick_clicks` field parses and defaults to true.

No build runs during an AOSP compile (`aosp-build-resource-rule`).

---

## 9. On-device checks (RG Cube)

With `adb logcat -s GammaHotkeys WindowManager GammaOSNano DrasticNano` running:

1. Fresh update from a build with *Start + Select Hotkeys* On: the row shows Start + Select, and every Start + Select check in HOTKEYS.md Testing still passes.
2. Update from a build with it Off: the row shows Off; Start + Select + R1 does nothing in a game; Start + Select still opens the Quick Menu in Nano's menus.
3. Choose L3 + R3. In RetroArch (a SNES game):
   - L3 + R3 + R1 saves, L1 loads, R2 / L2 fast forward and rewind; R1 never reaches the game.
   - L3 + R3 held 3 s quits; a key during the hold cancels; release before 3 s does nothing.
   - L3 held, R3 clicked a second later, then R1: no save (D10).
   - Start + Select + R1 now does not save, and Start + Select held 3 s does not quit.
   - RetroArch's menu does not open on L3 + R3, and opens on holding Start (D6); `retroarch.cfg` shows `input_menu_toggle_gamepad_combo = "7"`. Back to Start + Select in Settings, or Universal Hotkeys Off, puts back `"2"`.
   - The first RetroArch launch right after choosing L3 + R3 already has `"7"`.
4. PPSSPP, Dolphin, ARMSX2, Flycast, Azahar, Vita3K and M64Plus FZ: L3 + R3 + R1 saves. In ARMSX2 a lone L3 still reaches the game with no delay (getevent beside the game). Note what each emulator does with L3 + R3 itself.
5. A DS game (drastic-nano): L3 + R3 + R1 saves, held 3 s exits, a D-pad hotkey works; D-pad held, then L3 + R3: no chord.
6. Holding A, then L3 + R3 + R1: no hotkey (another button held). Holding Start, then L3 + R3 + R1: no hotkey.
7. Nano's menus: L3 + R3 released opens the Quick Menu; Start + Select still does; L3 alone in ES-DE picks a random game on the release; L3 + R3 + the bottom button opens the button map.
8. Button map over a game: the combo row reads "L3 + R3"; with Controller Bindings Off the row is gone; with Hotkey Combo Off it is gone; RetroArch's row reads "Hold Start".
9. Controller Bindings text in the Quick Menu names "L3 + R3", and with Hotkey Combo Off it has no combo clause. Turning it Off shows "Games get L3 + R3". Check both fit at 640x480.
10. Brave with Controller Enable on: L3 alone closes a tab on release; L3 + R3 closes nothing and opens nothing; L3 + R3 held 3 s quits; Start + Select keeps the address bar and bookmarks.
11. Universal Hotkeys Off: L3 + R3 does nothing over a game.
12. A hotkey set to L3 with the L3 + R3 combo: Anbernic + L3 runs it; L3 + R3 doesn't run it.
13. Reboot with L3 + R3 chosen: the setting stays, and the first chord after boot works before Nano's first menu.

On a device without stick clicks (TrimUI Brick or MagicX, when one is available): the row offers only Start + Select and Off; an `adb setprop` of L3 + R3 is turned back into Start + Select at Nano's start.

Only if D8's alternative is taken: on every device, `getevent -lp` shows whether Start, Select, L3 and R3 come from one input node, and `dumpsys input` whether the built-in pad is "external".

---

## 10. Phases

| Phase | What | Effort | Risk |
|---|---|---|---|
| 0 | Owner decisions D1-D10, Q1 first. Read upstream drastic-nano's `btnL3`/`btnR3` use, NanoMenu.h `mStartHeld` and gammapad's MouseMode combo. Confirm RetroArch's shipped binds have nothing on Start, L3 or R3 beyond the menu combo (the fixture says so; check the payload) and its Hold Start delay. Check the emulators' own L3/R3 binds (Azahar, Dolphin, Flycast, PPSSPP, Vita3K, ScummVM, ARMSX2, the PS1 cores). Set `stick_clicks` per device from `getevent -lp` (and whether a driver declares 317/318 without wiring them). | 0.5 day | none |
| 1 | Resolvers (Java, C++, gammapad copy) and shared cases; `persist.gammaos.hotkeys.combo`; `stick_clicks`; the Hotkey Combo row with its filter; `comboText`, `comboName` and the i18n checks (both placeholders, positional reordering, source pattern); new and revised English strings; migration. Readers still use Start + Select until phases 2-4, so behaviour is unchanged. | 2.5 days | low |
| 2 | PhoneWindowManager: extract the combo decision into pure functions with host cases, then switch the detector, `nanoOtherPadButtonHeld`, gating and the D10 window to the resolver. | 2 days | medium-high (system_server, untested today) |
| 3 | drastic-nano detector on the resolver (pad-button and hat "other held", D10); Nano's menus with per-button hold and two pairs; the Button Map Button rule. | 2 days | medium |
| 4 | Button map row in `chordRows` and `kComboSouth`; browser.json `HOTKEY_COMBO`; gammapad chord on the chosen pair and L3/R3 tab shortcuts on release; scene goldens. | 1.5 days | medium |
| 5 | RetroArch menu combo sync (three run points, marker, write-back) and the "Hold Start" map row; M64Plus note. | 1 day | medium (edits a user config, but only the shipped value) |
| 6 | Translations of the new keys in 21 files, retired keys removed; HOTKEYS.md, README, bmap plan; the device checklist (§9). | 1.5 days plus device time | low |

Phases 2-4 must ship together: a combo that works over games but not in drastic-nano or Nano's menus would confuse. Phase 1 can ship alone only if the row is hidden until then. Phase 5 must ship with or before the L3 + R3 option becomes visible. Total: about 11 days, plus device time.

---

## 11. Open questions

1. D1, before phase 1: does "rebindable" mean the owner wants the mixed pairs too (Select + R3, Start + L3), or a capture screen? The code supports any pair from {Start, Select, L3, R3} (§6.3 holds buttons back one by one); only the menu list and the §9 checks change.
2. Should the combo hold quit with Universal Hotkeys Off? Today no hold exists with them off; the plan keeps that.
3. What does upstream drastic-nano do with `btnL3`/`btnR3` (P:1072)? If they drive a DS session action (screen swap, menu), L3 + R3 as the combo would fire it too.
4. Which GammaOS devices really lack stick clicks? `getevent -lp` on the TrimUI Brick and MagicX units, and whether any driver declares 317/318 without wiring them.
5. Is 200 ms right for D10? Tune on the Cube with deliberate L3 + R3 presses.
6. Should Nano's own text (Hotkeys page descriptions such as "%s + this button saves") mention the combo too, or only the function button as today? Recommended: function button only; the Hotkey Combo row explains the combo.
