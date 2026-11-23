# Dual screen plan

Status: approved design (2026-11-17); questions answered below. Builds on test 31's one-screen-at-a-time work (`external_display_modes_plan.md` §R).

## Requirement (user, 2026-11-17)

An **External Display** choice with two options, one selected at a time:
- **Docked** (default): test 31's behaviour. The monitor is the only screen; the Cube's screen is off.
- **Dual screen**: the Cube's screen and the monitor run independently, each at its own resolution.
  - The **monitor is the primary screen** (Android display 0). **Games always run on the primary.**
  - The **Cube's screen is the secondary** and can run apps.
  - **ES-DE home:** the primary shows the platform (system) view; the secondary shows the selected system's game list.
  - **Media** (video, music, photos, PDF) opens on the primary.
  - On the **secondary**: the Quick Menu, the button map overlays, the app launcher (Applications), ES-DE's menu/settings, Nano's settings and Android's full Settings.

Decisions (user, 2026-11-17):
- The choice is in Nano's settings and a Quick Settings tile.
- The controller follows the open UI: the game list on the Cube's screen in the ES-DE home, the Quick Menu or settings while open, the game while one runs. An app on the Cube's screen gets the controller while it is focused.
- The touchscreen acts on the Cube's screen.
- Switching takes effect immediately while a monitor is plugged in.
- Apps from the app launcher (and Android Settings) open on the Cube's screen; games, including the Android Games system's, on the monitor.
- Other home themes: the DSi theme uses its own second screen (its bottom-screen content) on the Cube's screen; the XMB shows its wallpaper and wave there (Minima likewise its wallpaper). Menus still open on the Cube's screen.
- ES-DE: one combined screen (Left/Right or L1/R1 change system, Up/Down move in the game list, A starts).
- Dual-screen games: the app manages both screens itself when it supports it; Thimsy only makes the Cube's screen available to it. Otherwise a game is like any other: on the monitor.

Without a monitor nothing changes: the Cube's screen is the only screen, in both modes.

## Design

### Mode
- `persist.gammaos.display.mode` = `docked` (default) | `dual`. A new name; `persist.gammaos.ext.*` is cleared at boot by test 31's cleanup.
- Nano: Settings > Display > **External Display**: Docked / Dual screen (radio rows). A **Quick Settings tile** "External display" cycles the two (Android shade and Nano's Quick Settings).
- system_server watches the property and re-arranges at once (as for a hotplug).

### Framework (system_server, SurfaceFlinger)
1. **Arrangement** (`GammaosScreenArrangement.plan`): with a monitor and mode `dual`, the monitor is display 0 and the panels are in `on` (enabled) instead of `off`. Pure; host-checked as today.
2. **A fixed id for the panel** while it is secondary (say 2, which SystemUI's `config_gammaos_secondary_display_id` and Nano's `cc.topdisplay` already assume), instead of a fresh id at every plug. Reuses test 31's remembered-id producer.
3. **Flags** for the panel while secondary: `FLAG_PRESENTATION` (apps with a dual-screen feature find it through `DisplayManager.getDisplays(DISPLAY_CATEGORY_PRESENTATION)` and put their second screen there: Azahar, WatermelonDS and the like), `FLAG_OWN_CONTENT_ONLY` (never auto-mirror the monitor), `FLAG_STEAL_TOP_FOCUS_DISABLED` (a tap on the Cube's screen must not take the controller away from the game; focus moves are Nano's, through `sys.gammaos.nano.focus.display`), no secondary home, no system decorations.
4. **Power:** both screens in the default display group, so the power key, screen timeout and wake act on both. Brightness: the Lights HAL backlight belongs to the panel's DisplayPowerController (it is SurfaceFlinger's first display); the brightness slider and Nano's backlight control drive it; the monitor has none.
5. **Square panel as landscape** (`gammaSquareIsLandscape`) applied to the panel as a secondary display too, or apps on it take portrait layouts.
6. **Keyboard:** apps on the panel get their keyboard on the panel (`ime.localdisplay`, set by Nano in dual mode, not only on two-panel devices).
7. **Touch:** the internal touchscreen already maps to the INTERNAL viewport, which is the panel's display in dual mode (no change); verify on device.
8. **SurfaceFlinger pacesetter:** the monitor (display 0) paces vsync whenever it is on, whichever display came on first, so games on the monitor don't judder (extend the existing pacesetter override; today the panel would stay the pacesetter).
9. **Launch routing:** games (every Nano game launch, drastic-nano, RetroArch, standalone emulators, the Android Games system) go to display 0, explicitly. Apps from the app launcher and Android Settings go to the panel, without stopping the game on the monitor.

### Nano
1. **Display target** (`nanodisplay::choose`): in dual mode with a monitor, `target` = the monitor, `second` = the panel (today `second` is -1 while a monitor is in). Unit-tested.
2. **Secondary surface at its own size:** the second-panel render pass draws at the panel's 720x720 with its own projection (today it assumes both screens share a size, true only on the RG DS). Touch maps to the panel's size with its own touch-zone list.
3. **ES-DE home, split:** the primary draws the system view, the panel draws the selected system's game list. This needs a second parsed theme document for the panel's 1:1 aspect (the theme layout depends on aspect), the game-list view drawn from the shared selection, one video decoder given to one screen, and no view-swap transition. **Navigation** (proposal, see question 3): Left/Right (or L1/R1) change system on the primary and the game list follows; Up/Down move in the game list; A starts the game on the primary; Start/Select open ES-DE's menu on the panel.
4. **Menus on the panel:** the Quick Menu, button map, Applications view, ES-DE's menu, Nano's settings (the Minima settings theme) draw on the panel's surface; the primary keeps the home (or the game). Needs the per-frame drawing routed per surface, the XMB's per-frame state ticks split from drawing (drawn once, ticked once), and the settings theme switch made per-screen.
5. **Over a game:** the overlay process keeps its layer on the monitor hidden (no scrim over the game) and draws the Quick Menu / button map on the panel; closing it returns the controller to the game.
6. **Media on the primary:** video, music, photos and PDF draw on the primary even when opened from a menu on the panel; the panel shows the game list (ES-DE) or the menu that opened it.
7. **Apps on the panel:** generalise the Control Center's "bottom app" launch (`am start --display <panel>`, no force-stop of the game, exit detection, focus hand-back). Foreground tracking becomes per screen.
8. **Dual-screen games:** drastic-nano (Thimsy's DS host) already puts the bottom DS screen on a second screen when there is one (screen mode "auto"; "single" keeps both on the monitor), so in dual mode it uses the Cube's screen, with touch there. Other emulators use it through Android's presentation displays (framework 3). While a game uses the Cube's screen, Nano's menus on it (the Quick Menu over the game) draw over the game's second screen and hide again on close.
9. **Other themes:** the DSi theme draws its bottom screen on the Cube's screen; the XMB its wallpaper and wave; Minima its wallpaper.

## Phases (one build, in order; each phase is usable on its own)
1. **Mode + framework:** the property, settings row and tile, the arrangement, fixed panel id, flags, power/brightness, pacesetter, keyboard, launch routing. Result: dual mode works with the panel showing Nano's wallpaper; games on the monitor; apps can be put on the panel.
2. **Nano surfaces:** choose(), the panel pass at its own size, touch on the panel, menus (Quick Menu, button map, settings, Applications, Android Settings) on the panel, the overlay over a game.
3. **ES-DE split home** and media routing.

Rough size: phase 1 medium, phase 2 medium-large, phase 3 medium-large. Phase 1 and 2 touch the same framework files as test 31 and are the riskiest on the device (power, pacesetter, focus).

## Implementation (2026-11-17, not yet run on a device)
- **Mode:** `persist.gammaos.display.mode`. Nano: Display Settings > External Display > Docked Mode / Dual Screen Mode (two rows on the one property; the old inert Secondary Display Apps/Packages rows are gone); Quick Menu > Display shows the same submenu. Android Quick Settings tile `externaldisplaymode` (ExternalDisplayModeTile). LogicalDisplayMapper polls the property each second while a monitor is in and re-arranges.
- **Framework:** the dual plan keeps the panels enabled (GammaosScreenArrangement, host-checked); the panel gets a stable non-default id (not necessarily 2; Nano uses the panel's layer stack, which is its display id); `FLAG_PRESENTATION | FLAG_STEAL_TOP_FOCUS_DISABLED`, no system decorations, never auto-mirrored; square-as-landscape applies to it; its keyboard is local (DisplayContent.getImePolicy); SurfaceFlinger makes a powered-on monitor the active display and pacesetter in dual mode, whichever came on first.
- **Nano:** `nanodisplay::choose(..., dualMode)` gives the panel as `second` beside a monitor. `setDualPanel` / `uiToPanel` / `dualPanelTick` (NanoMenuOverlay.cpp): the panel's surface is made at its own size; the second-screen pass draws at that size (XMB: the home's wave or wallpaper, centred and cropped; DSi: its bottom screen; Minima: its second screen; ES-DE: the game list). While a menu is open (`nanodisplay::uiOnPanel`) Nano's UI moves onto the panel (mSurface and its size swapped with the panel's) and the monitor keeps its last frame; over a game the overlay is always on the panel (wallpaper behind it) and the monitor layer stays hidden. Touch works while the UI is on the panel.
- **Apps on the panel:** `panelLaunch` (Applications, the ES-DE app list, an app's option menu, Android Settings): `am start --display <panel>`, the game or home stays; the controller goes to the app (`sys.gammaos.nano.focus.display`, drop_input 0) and Nano reads only the Anbernic button, whose release takes it back and opens the Quick Menu. Quick Menu rows Use / Close Built-in Screen App. Exit noticed by a dumpsys poll (1 s, 3 s beside a game), shared with the other Nano process through `sys.gammaos.nano.panel_app`.
- **ES-DE:** in dual mode the home stays in the game list for input (Left/Right or L1/R1 system, Up/Down game, A start, Back does nothing); the monitor draws the system view (`mEsdeForceView=1`), the panel the game list (`=2`), and the game list pass owns the video. `esdeDocForAspect` keeps the theme for each aspect ratio and swaps (also fixes a docked monitor drawing the theme parsed for the panel's 1:1).
- **drastic-nano:** each DS screen at 4:3 in its window (`DsScreenLayout::fitOne`), touch mapped through it.
- **Reviews (3 rounds, 2026-11-17):** fixed among others: a ds-cover matrix squashing the panel UI, sleep turning dual off (no display shows layer stack 0 while off), the monitor layer staying hidden after a game or when dual ends, panel UI rotated by the monitor, overlay-home pacing, panel layer-stack drift, translucent panel surface, stale-buffer reveals, ES-DE views sharing carousel/grid/marquee state, ES-DE per-aspect theme staleness and failure back-off, idle sleep while a panel app is used, exit detection while the home idles, stale focus pin after a restart, panel apps landing on display 0 (REMOVE_MODE_DESTROY_CONTENT), the panel app never getting WindowManager focus beside a game (RootWindowContainer pin now sets the panel's focused app), stale Docked/Dual row values, chords in the input gate, drop_input cleared by InputDispatcher.
- **Known gaps:** touch does nothing while the UI is on the monitor (the ES-DE game list on the panel included), except that a tap on the panel's app gives it the controller; a panel app's keyboard shows only while it has the controller (Android shows the IME only on the focused display); the monitor shows its last frame (not a live home) while a menu is on the panel, and a menu opened right after a game or a plug-in stays on the monitor until it closes; brightness stays Nano's panel backlight.

## Risks
- Two screens on from one Unisoc display controller: bandwidth and whether the vendor composer drives the panel and the DP output together at their own modes (test 31 never ran both). Must be checked on the device first; a quick check build (phase 1 alone) is worthwhile.
- Wake order of the two screens after sleep (pacesetter, black screen).
- Controller focus between the game and an app on the panel.

## Answered questions (2026-11-17)
1. Apps: the Cube's screen. 2. Other themes: DSi its second screen, XMB the wallpaper and wave. 3. ES-DE: one combined screen. 4. Dual-screen games: the app manages both screens when it supports it; Thimsy makes the Cube's screen available (presentation display), otherwise games run on the monitor. 5. Brightness: the slider drives the Cube's screen (assumed; the monitor has its own controls).
