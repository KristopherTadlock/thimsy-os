# Dual screen for build 43: main screen, moving apps, controller on the main screen

Status: proposed 2026-11-27, revised the same day after a technical and an owner review. Planning only: no code was changed, nothing was built, and no device was touched. It supersedes parts of `dual_screen_plan.md` (listed in section 10). Every technical claim is marked **[C]** (confirmed from source, the Thimsy patch, a device log or the AOSP docs), **[I]** (inferred) or **[U]** (unverified).

Request (owner, 2026-11-27): "For build 43 - the dual screen setup has some issues. We need a way to swap between the external display as the main screen (default) and internal display. We also need a way to push an app from one screen to another. The controller should always have focus on the main screen, so that the secondary screen can be used with touch for apps. This doesn't work properly in the build currently installed on the cube. Controller inputs are not registered properly when say navigating a browser while playing a game"

Background: `dual_screen_plan.md` (the approved design and its implementation notes), `external_display_modes_plan.md`, `display_mirror_plan.md`, `external_display_routing_plan.md` (superseded), README "External monitors" (Dual Screen part), HOTKEYS.md, TODO.md "Displays". Three research passes (current behaviour and the bug; Android input routing; swap and move) and two reviews were done for this plan. They are not kept as files; their evidence is folded in below with its own citations.

Evidence references:
- **P:N** is a line in `patches/thimsy/0001-thimsy.patch` (commit a13c8ee, "Test 42").
- **T:** is the WSL AOSP tree `/aosp/gammaos` (upstream code and the GammaOS base). Shortened paths: `wm/` and `display/` are under `frameworks/base/services/core/java/com/android/server/`; `inputflinger/` is `frameworks/native/services/inputflinger/`.
- **Logs** are under `out/dp-evidence/`.
- The installed build is test 41 plus test 42 device patches. The dual-screen focus and input code is the same in test 41 and test 42: `git diff aa9b131 a13c8ee` on the patch touches only the 4K30 mode lines [C]. So the patch is what runs on the Cube.

## 1. Summary for the owner

### What you get

- **Swap Screens.** In Dual Screen you choose which screen is the **main screen**: the monitor (the default) or the Cube's screen. Games always run on the main screen and the controller always works there. Swapping moves what is on each screen to the other one: the game goes to the new main screen without restarting, and the app on the second screen goes to the other screen.
- **Move to Other Screen.** Any app can be pushed from one screen to the other from the Quick Menu or with a hotkey. It keeps running (Android moves the task; it does not restart from scratch). Moving a game is the same as Swap Screens, so a game never ends up where the controller can't reach it. Until Swap Screens ships, Move App refuses a game.
- **The controller never leaves the main screen.** The second screen is for touch. You can scroll a browser or tap through an app on the Cube's screen while the game on the monitor keeps every button press. Nano's own menus (the Anbernic button, the Quick Menu, settings) still work with the controller as today.
- **The bug gets a fix in the framework**, not another setting. Section 3 lists eight things in the current build that can take, delay or drop the game's input while you use an app on the other screen. The design closes all of them. Which one you actually hit is not confirmed yet; the read-only checks of Phase 0 settle that.

### What changes for you

- The **Controller Screen** setting (Monitor / Built-in Screen / Last Touched) goes away. The controller is always on the main screen.
- **Anbernic + the top button** (today "Switch Screen", which moves the controller) becomes **Swap Screens**. **Anbernic + the left button** becomes **Move App** (proposed; it looks free by default, still to be checked). Both are Anbernic-only chords, like Switch Screen today: Start + Select does not run them.
- An app on the second screen gets no controller input at all. You use it by touch. To use it with the controller, move it to the main screen.
- **Requirement 3 holds fully only while the monitor is main.** When the Cube's screen is the main screen, the monitor is a second screen with no touch, and Android 14 keeps a mouse pointer on display 0 as well, so a mouse can't reach it either. The monitor is then view-only: good for a video or a web page you read, not for operating an app. Making a mouse work there is extra work (D10).
- Until the boot crash with a monitor attached is fixed (a test 42 known issue), boot the Cube without the monitor and plug it in once Nano is up.

### What it costs

- Phase 1 (controller always on main, the bug fix) is medium to large work and low to medium risk. It can ship on its own in build 43.
- Moving apps is small to medium.
- Swapping the main screen is the big part: large, and the riskiest on the device (the same display, power and pacing code that test 31 and test 42 fought with).
- Section 8 has the phases.

### Decisions needed first

Section 9. The ones that shape the design: what a swap moves (D1), where Nano's menus draw (D2), what moving a game does (D3), moving onto a screen that already has an app (D4), and what counts as a game (D19).

## 2. The owner's four requirements

1. **Swap the main screen.** The external display is the main screen by default. The Cube's built-in screen can be made the main screen instead.
2. **Push an app.** Move an app from one screen to the other.
3. **The controller always has focus on the main screen**, so the second screen can be used with touch for apps.
4. **Bug on the installed build:** controller input is not registered properly when you, for example, navigate a browser on the second screen (by touch) while playing a game on the main screen.

Terms used below:
- **Main screen**: the screen with the game and the controller. In the design it is always Android display 0.
- **Second screen**: the other screen in Dual Screen. It has its own focus for touch and its own keyboard, and never takes the controller.
- **Panel**: the Cube's built-in 720x720 screen. **Monitor**: the USB-C DP display.

## 3. The bug: why the game loses or delays controller input

### 3.1 How dual screen works today (the parts that matter)

- **Arrangement** [C]. In Dual with a monitor, `LogicalDisplayMapper.gammaosArrangeLocked` (P:169420-169500) makes the monitor display 0 and enables the panel as a secondary. The panel gets `FLAG_PRESENTATION | FLAG_OWN_FOCUS | FLAG_STEAL_TOP_FOCUS_DISABLED`, no system decorations, `REMOVE_MODE_DESTROY_CONTENT` (P:169349-169360). `FLAG_OWN_CONTENT_ONLY` is not set (the old plan listed it); "never mirror" is done in DisplayManagerService instead (P:168273).
- **The panel's id is not fixed at 2** [C]. It is remembered per address for the boot (`gammaosAddToLayoutLocked`, P:~169510). In `logcat-640.txt` (around line 21400) it is display 3 ("Layerstack set to 3 for local:0"). SystemUI's `config_gammaos_secondary_display_id=2` is therefore wrong. Nano uses the panel's layer stack, which is right.
- **Focus** [C]. `RootWindowContainer.updateFocusedWindowLocked` (P:173171-173233) gives the panel its own focused app and never lets it become the top focused display by itself (`mayTakeTop`, P:173211). Nano's pin `sys.gammaos.nano.focus.display` overrides that and forces the panel to be the top focused display (P:173193). The top focused display is what InputDispatcher calls `mFocusedDisplayId`, the target of every event that names no display.
- **Nano's controller policy** [C]. Controller Screen `persist.gammaos.nano.ctrl_screen` = monitor (default) | panel | touched (P:45128, `NanoScreenChoice.h`). "panel" pins at app launch; "touched" pins when InputDispatcher reports a finger-down on the panel through `sys.gammaos.nano.panel_touch` (P:175774-175800, `screenSwapPoll`). The Switch Screen chord pins at any time (P:171056-171080, HOTKEYS.md line 70).

### 3.2 Root causes, ranked

None is confirmed on the device yet; the checks in 3.3 settle it in one session. The numbers are labels; the Rank column is the order of likelihood [I].

| # | Rank | Cause | Effect on the game | Applies when | Evidence |
|---|---|---|---|---|---|
| 1 | 1 | Nano's focus pin moves `mFocusedDisplayId` to the panel | Every button and stick goes to the browser; the game gets nothing | Controller Screen = Built-in Screen or Last Touched, or the Switch Screen chord was pressed | [C] code; [U] the owner's setting |
| 3 | 2 | Keys wait for the windows' unfinished events, the browser's included | Presses are late while you scroll: usually about one browser frame, up to 500 ms when the browser's UI thread stalls; stick motion queued behind the key waits too | Any setting, while the panel app is being touched | [C] code; [I] size of the delay |
| 7 | 3 | The no-focused-window wait, then the queue prune on a tap in the browser | Every key and stick event queued before the tap is **dropped** | Any setting, when the key's target display has a focused app but no focused window (the pin flipping focus, a game window in transition) | [C] code; [I] how often |
| 4 | 4 | Built-in pad keys (and gammapad's virtual touchscreen) carry the panel's display | Face buttons go to the browser (B = Back), sticks and D-pad hat to the game: a split controller. ScreenMapMode taps land on the panel | gammapad off or not grabbing the pad; ScreenMapMode in use | [C] code and log; [U] gammapad state; [I] ScreenMapMode |
| 8 | 5 | Load: a browser and a game together on the UMS9620 | Dropped game frames feel like lost presses | Heavy pages, thermal throttling | [U] |
| 2 | 6 | gammapad's foreground app briefly becomes the browser, turning on BrowserMode | The pad turns into a mouse and keyboard for up to about a second; held buttons are released at each switch | gammapad on, a Brave or Fulguris task comes to the front on the panel (once per front, not per tap) | [C] code; [I] gammapad on (test 42 logs) |
| 5 | 7 | Every tap on the panel runs the task-focus path | Small hitches; it does not re-trigger cause 2 | Every finger-down on the panel | [C] code; [I] effect |
| 6 | 8 | `sys.gammaos.nano.drop_input` left at 1 | All keys dropped | No concrete path found | [I] unlikely |

**Cause 1: the pin.**
- Path: a finger-down on the panel app writes `panel_touch` (InputDispatcher, P:175790-175800). Nano's `screenSwapPoll` calls `panelAppFocus(true)`, which sets `focus.display` to the panel. Within 200 ms WindowManager forces the top focus there and calls `InputManager.setFocusedDisplay(panel)` [C].
- The game keeps its window focus and stays resumed, so it simply receives nothing. Held keys are cancelled on the display change (`InputDispatcher.cpp:5577-5612`) [C].
- This is the old decision "the controller follows the open UI" working as designed. It contradicts requirement 3.

**Cause 2: gammapad's foreground app.**
- `GammapadVibrationBridge` writes `sys.gammaos.gamepad.fg_pkg` from `onTaskMovedToFront` of any task on any display, and from `getTasks(1, ..., -1 /* displayId */)` (T: `services/core/java/com/android/server/gamepad/GammapadVibrationBridge.java:114-125, 147-155`) [C]. Neither filters by display.
- gammapad applies per-app profiles, ScreenMapMode and BrowserMode from `fg_pkg` [C]. BrowserMode turns on for `com.brave.browser` and `net.slions.fulguris.*` (`kDefaultPackages`, P:174600; "BrowserMode: on", P:174704) [C]. Brave is Thimsy's default browser (README:72).
- `getTasks(1, ..., -1)` lists display 0's task first: `RunningTasks` puts the displays' focused visible tasks first, in display z-order (T: `wm/RunningTasks.java:75-110`; `forAllDisplays` goes top to bottom, `wm/RootWindowContainer.java:1478-1482`). New displays are added at the bottom (RWC 1534, 1627) and the panel can't be raised (`DisplayContent.mDontMoveToTop`, `wm/DisplayContent.java:817`) [C].
- So opening or re-fronting a browser task on the panel flips `fg_pkg` to the browser only until the next `updateForegroundPackage` (300 ms after the task-stack change, or the 1 s poll; `GammapadVibrationBridge.java:105-125`) [C]. Repeated taps on the same task don't notify again (`onLeafTaskMoved` skips `mLastLeafTaskToFrontId`, `wm/TaskDisplayArea.java:~456-472`) [C]. The effect is a short mouse-mode blip once per browser front, with held buttons released as BrowserMode turns on and off [I].
- PhoneWindowManager's foreground lookups for hotkey profiles and hold-to-quit (`getRunningTasks(1)`, T: `policy/PhoneWindowManager.java:3340-3347`) get the same order, so hold-to-quit closing the browser is unlikely [I]. The fix still reads display 0 explicitly, so the order never matters.

**Cause 3: cross-display key wait.**
- Before sending a key, InputDispatcher waits while **any** connection on **any** display has unacknowledged events (`shouldWaitToSendKeyLocked`, T: `inputflinger/dispatcher/InputDispatcher.cpp:2269-2300`, called at :2383-2389) [C]. The wait is up to `KEY_WAITING_FOR_EVENTS_TIMEOUT` = 500 ms (`dispatcher/include/InputDispatcherPolicyInterface.h:168-172`) [C]. Upstream does this so a key pressed after a tap reaches a window the tap opened.
- The waiting key is the dispatcher's pending event, so the whole inbound queue waits behind it, the browser's own new touches included (IF:1063-1080, 1301-1307) [C]. The wait therefore ends when the browser acknowledges what it already has: about one browser frame normally, up to 500 ms only when its UI thread stalls [I]. Any new pointer DOWN ends the wait at once (IF:1410-1418) [C]. Joystick motion queued behind the key waits too [C, structural]. A janky browser on a busy Cube makes the delay long enough that a press and its release arrive together and an emulator that polls once per frame misses it [I].
- The game's own unacknowledged joystick motion also delays its keys; that is upstream behaviour and stays.
- This one applies whatever the settings are, so it is a likely explanation if the owner uses the default Controller Screen = Monitor [I]. The log line "Dispatching key to ... even though there are other unprocessed events" is printed only after the full 500 ms (IF:2293-2297) [C], so its absence does not rule this cause out.

**Cause 4: keys tagged for the panel.**
- `KeyboardInputMapper` stamps keys with the INTERNAL viewport's display when `orientationAware`, which defaults to `!isExternal()` (T: `inputflinger/reader/mapper/KeyboardInputMapper.cpp:171-183, 238-241`) [C]. The INTERNAL viewport is the panel in dual mode (T: `display/DisplayManagerService.java:3665-3707`; `dumpsys-display-run2.txt:4`: "INTERNAL displayId=3") [C].
- `retrogame_joypad` (484b:1101) has no idc and is not EXTERNAL (`test42/B2/logcat.txt:73286`) [C]. So without gammapad, its keys go to the panel's focused window. Joystick motion carries no display (T: `JoystickInputMapper.cpp:344-347`) and goes to the game [C].
- With gammapad on, the pad is grabbed and re-emitted as "Thimsy Virtual Gamepad" on `BUS_BLUETOOTH` (P:175535ff), which Android treats as external: no display id, so this cause does not apply [C]. gammapad's other virtual devices (the BrowserMode keyboard, VirtualKeyboard, VirtualMouse, VirtualTouchscreen) are `BUS_VIRTUAL`, so they count as internal and land on the panel [I].
- That includes ScreenMapMode: its taps come from VirtualTouchscreen, which (internal, no association) maps to the INTERNAL viewport, the panel. A game that relies on ScreenMapMode taps would see them land in the browser instead [I].
- Nano's own default for gammapad is off (`persist.gammaos.gamepad.enable` "0", NanoMenuSettingsTree.cpp:537) [C]; test 42 logs show it running in most sessions [C]. The installed state is [U].

**Cause 5: taps run the task-focus path.**
- InputDispatcher compares a tapped window with the focused window of `mFocusedDisplayId`, not of the tapped display (T: `InputDispatcher.cpp:3742-3760`) [C]. So every finger-down on the panel calls `onPointerDownOutsideFocus`, which runs `handleTapOutsideFocusInsideSelf` → `setFocusedTask` → `moveToFront`, `updateTopResumedActivityIfNeeded`, `resumeFocusedTasksTopActivities` (T: `wm/WindowState.java:5979-5982`, `wm/ActivityTaskManagerService.java:2349-2392`, `wm/ActivityRecord.java:3540-3566`) [C]. The display is not raised (`DisplayContent.mDontMoveToTop`, T: `wm/DisplayContent.java:817`, used at `wm/TaskDisplayArea.java:399-405`) [C].
- On its own it is low impact: small hitches from the work on every tap [I]. It does not re-trigger cause 2, since a tap on the task already in front does not notify `onTaskMovedToFront` again (see cause 2) [C].

**Cause 6: `drop_input` stuck.** InputDispatcher drops keys while `sys.gammaos.nano.drop_input` is 1 (P:~175760). The home process skips its tick while behind a game (NanoMenu.cpp:4910-4935) and `overlayHide` clears it before `panelLaunch` [C]. No stuck path found [I]; checked anyway.

**Cause 7: the no-focused-window wait and the queue prune.**
- When a key's target display has a focused app but no focused window, InputDispatcher holds that key, and every event behind it, for up to the app's dispatching timeout (5 s): "Waiting because no window has focus but ... may eventually add a window" (IF:2324-2340, which sets `mAwaitedFocusedApplication` and `mAwaitedApplicationDisplayId`) [C].
- A finger-down on a window of a **different** app (the browser) while it waits prunes the queue: `shouldPruneInboundQueueLocked` (IF:1320-1361) sets `mNextUnblockedEvent` (IF:1405-1406), and every key and non-pointer motion queued before it is dropped as BLOCKED (IF:1227-1228, 1271-1274) [C]. Log: "Pruning input queue because user touched a different application while waiting for ...".
- That is literally "controller input not registered while touching the browser". Likely triggers: the pin moving focus to the panel and back (a display whose focused window is not settled yet), or the game's window in a transition (a relaunch, a configuration change) [I].

**Cause 8: load.** A browser and a game together can drop game frames on the UMS9620, which feels like lost input. Phase 0 rules it out with `dumpsys gfxinfo` for the game and the thermal and CPU state during the repro [U].

### 3.3 Read-only checks on the Cube

For whoever runs adb; this plan ran nothing. Everything here only reads state.

**Hazards (from the device history):**
- Never read `/sys/class/display/dispc1/*`.
- No `stop; start`, no reboot and no display-stack restart with a monitor attached. Booting with a monitor attached crash-loops system_server (AudioService NPE in `onAccessoryPlugMediaUnmute`, 158 restarts; not fixed in test 42: `dp_dock_findings.md:875`, `out/dp-evidence/test42/AUDIT.md` R4). Boot without the monitor; plug it in once Nano is up.
- Unplug at the **monitor end**: pulling the adapter at the Cube's end has panicked it (`out/dp-evidence/test42/dk3/NOTES.md:90-93`, `dp_dock_findings.md`).
- Nothing under dwc3 debugfs.
- No tool that grabs evdev nodes (gammapad grabs and re-hides event3). `dumpsys input` lists the devices; `getevent` is not needed.
- The optional `wm logging` line changes logging until the next reboot; skip it if in doubt.

**Repro:** Dual Screen, a game on the monitor, Brave on the Cube's screen through Quick Menu > Open on Built-in Screen (or Applications).

**Capture three times:** before touching the panel; after one tap in the browser; while scrolling the browser and pressing A and B in the game.

```
adb shell 'getprop | grep -E "gammaos.display.mode|nano.ctrl_screen|nano.focus.display|nano.touch_display|nano.panel_app|nano.drop_input|nano.show_overlay|gamepad.enable|gamepad.hide_source|gamepad.fg_pkg|bindings.enabled|init.svc.gammapad"'
adb shell dumpsys input > t43_input.txt
adb shell 'dumpsys window | grep -E "mTopFocusedDisplayId|mCurrentFocus|mFocusedApp"'
adb shell 'dumpsys window displays | grep -E "Display: mDisplayId|mCurrentFocus|mFocusedApp|mDontMoveToTop"'
adb shell 'dumpsys activity activities | grep -E "Display #|topResumedActivity|ResumedActivity|\* Task\{"'
adb shell 'dumpsys display | grep -E "FLAG_OWN_FOCUS|FLAG_STEAL_TOP_FOCUS_DISABLED|mDisplayId=|Viewport"'
adb logcat -b main,system,events -v threadtime > t43_log.txt      # during the whole repro; Ctrl+C after
adb shell dumpsys gfxinfo <game package> > t43_gfx.txt             # after the scroll-and-press capture (cause 8)
adb shell 'dumpsys thermalservice | head -40; cat /proc/loadavg'    # during the repro (cause 8)
# optional, resets at reboot:
adb shell wm logging enable-text WM_DEBUG_FOCUS_LIGHT WM_DEBUG_FOCUS
```

**Reading the results:**

| What you see | Cause |
|---|---|
| `ctrl_screen` is `panel` or `touched`, or `focus.display` ≥ 0, and `dumpsys input` FocusedDisplayId ≠ 0 | 1 |
| `fg_pkg` = `com.brave.browser`, "BrowserMode: on" in the log | 2 |
| "Dispatching key to ... even though there are other unprocessed events" in the log (only after a full 500 ms), or KeyEvents in RecentQueue with long delivery times, or "Received a new pointer down event, stop waiting for events to process" | 3 (its absence does not rule it out) |
| "Waiting because no window has focus but ... may eventually add a window", then "Pruning input queue because user touched a different application" or "Dropping because the current application is not responding and the user has started interacting with a different application" | 7 |
| RecentQueue `KeyEvent(... displayId=<panel id>)` after pressing B; retrogame_joypad listed with "IsExternal: false", "OrientationAware: true"; `init.svc.gammapad` not running | 4 |
| gammapad's ScreenMapMode on for the game, and its taps reach the browser | 4 (virtual touchscreen) |
| Janky frames in `gfxinfo` for the game during the scroll, a thermal status above 0, a high load | 8 |
| "onPointerDownOutsideFocusLocked", "Not moving display (displayId=N) to top", "setFocusedTask" on every tap | 5 (confirms the extra work) |
| `drop_input` = 1 while no Nano menu is open | 6 |

### 3.4 How the design fixes each cause

| Cause | Fix (section 4) |
|---|---|
| 1 | WindowManager keeps display 0 as the top focused display in Dual; Nano's pin is ignored there and Nano stops writing it (4.2.1, 4.3). |
| 2 | gammapad's foreground app and PhoneWindowManager's foreground lookups read display 0's focused task only (4.2.3). BrowserMode then only follows a browser on the main screen. |
| 3 | A key waits only for unfinished events on its own target display (4.2.2, step 3). |
| 4 | In Dual, `notifyKey` clears the display of every non-injected key before the policy sees it, so the dispatcher, PhoneWindowManager and the app all see the main screen (4.2.2, step 1); an idc for the built-in pad too. VirtualTouchscreen is associated with display 0 (4.2.3). |
| 5 | A tap on the second screen's already-focused window skips the task-focus path (4.2.4). |
| 6 | Unchanged logic, and fewer writers: Nano sets `drop_input` only while its own menu has the controller. |
| 7 | Keys always target display 0, whose focused window is the game; the prune only runs for a touch on the display being waited on (4.2.2, step 4). |
| 8 | Not a framework fix. If Phase 0 shows it, it is a performance item (game priority, browser limits), outside this plan. |

## 4. Controller always on the main screen; touch and keyboard on the second

### 4.1 The rule

- In Dual Screen, Android's top focused display is always display 0, the main screen. Nothing (touch, Nano, an app) can move it.
- The second screen keeps its own focused app and window (per-display focus), its own touch mode and its own on-screen keyboard. Both screens have a resumed app.
- Every controller event goes to display 0's focused window: built-in controls, Bluetooth and USB pads, gammapad's virtual devices.
- Nano's own menus are the one exception, as today: Nano reads the controller straight from evdev, and `drop_input` keeps Android from also delivering it while a Nano menu is open [C, `panelAppInputGate` and pollInput, NanoMenuOverlay.cpp:2839-2862].

This is a framework rule, not a Nano policy, so nothing that forgets it can break it.

### 4.2 Framework mechanism

**4.2.1 WindowManager: display 0 is always the top focused display (RootWindowContainer)**
- In `updateFocusedWindowLocked` (P:173171-173233): while a dual-screen secondary exists, the top focused display is `DEFAULT_DISPLAY`, like the RG DS path's `forceDefaultFocus`. Keep `mayTakeTop = false` for the secondary.
- Ignore `sys.gammaos.nano.focus.display` in Dual (P:173193). Keep it for the two-panel devices' Control Center (RG DS), which are not in Dual.
- Keep the panel's focused app set from its top running activity (P:173184), so its window gets focus for touch and the keyboard.
- The 200 ms pin poll and the `focus.notice` glow in WindowManagerService (P:~173380) become unused in Dual; keep the glow for swap feedback (section 5).
- Result: `InputManager.setFocusedDisplay` is never called with the secondary; the game never gets `onWindowFocusChanged(false)` or `onTopResumedActivityChanged(false)` from a tap [I, strong].

**4.2.2 InputDispatcher: physical controller events never choose a display (inputflinger)**
**The gate.** InputDispatcher doesn't see display flags [C], so "a dual-screen secondary exists" needs a source it can read. Phase 1 adds `sys.gammaos.display.second` (the arranged secondary's logical id, or -1), written by LogicalDisplayMapper whenever it arranges. The native gates already read `sys.gammaos.nano.*` properties the same way. Phase 3 keeps the same property for the second-screen role (5.1). Every step below applies only while it is ≥ 0, so other setups (Docked, Mirror, RG DS) keep today's behaviour.

1. **Clear the key's display at the source.** In `notifyKey`, before `interceptKeyBeforeQueueing` (T: `InputDispatcher.cpp:~4576`), set `args.displayId = ADISPLAY_ID_NONE` for every key that is not injected (`!(policyFlags & POLICY_FLAG_INJECTED)`). Do the same in `notifyMotion` for non-pointer motion (`!isFromSource(source, AINPUT_SOURCE_CLASS_POINTER)`); joystick motion already has no display unless the device has an association (T: `JoystickInputMapper.cpp:344-347`) [C].
   - Why at the source and not only in `getTargetDisplayId`: the KeyEntry's display is also what PhoneWindowManager sees before dispatch (`interceptKeyBeforeQueueing`; `interceptKeyBeforeDispatching`, IF:6591), what Home uses (`getKeyEventDisplayId` and the per-display `mDisplayHomeButtonHandlers`, T: `policy/PhoneWindowManager.java:5909-5920, 5946-5956`; `launchHomeFromHotKey(event.getDisplayId())`, :2485), what the app gets in `KeyEvent.getDisplayId()`, and what display-filtered cancellations match (IF:1560, 6302) [C]. With the display cleared, `getKeyEventDisplayId` falls back to the top focused display, which is 0 (4.2.1) [C], and they all agree.
   - Covers the built-in pad with or without gammapad, gpio-keys, goodix-ts keys, gammapad's BUS_VIRTUAL virtual devices (keyboard, BrowserMode keyboard) and any device given a port association later.
   - `getTargetDisplayId` (IF:2235-2266) then returns `mFocusedDisplayId` (0) for these events. Global monitors follow, since they use the same function (IF:1972, 2105) [C].
2. **Injected nav keys keep their display.** The GammaOS nav-key rule (IF:83-95, used at :2240-2246) turns HOME, BACK and APP_SWITCH into "no display" for every key, injected ones included [C]. Exempt injected keys that name a display (`POLICY_FLAG_INJECTED` and `displayId != ADISPLAY_ID_NONE`), so Nano's "Back" for the second screen (4.4) reaches the second screen and not the game. Audit Nano's and the RG DS Control Center's injected HOME/BACK first: any that pass a display while relying on the rule must pass none instead [U].
3. **Per-display key wait.** `shouldWaitToSendKeyLocked` (IF:2269-2300, called at :2383-2389): wait only while unacknowledged events belong to connections on the key's target display. `AnrTracker` has no way to list its tokens (`AnrTracker.h:31-42`) [C], so either add an iterator to it or walk each connection's `waitQueue`. Each connection's display: a window's `WindowInfo::displayId` (spy windows are windows, so they have one); a global monitor by the display it monitors (`mGlobalMonitorsByDisplay`); anything unknown counts as "on the target display" (it waits, as today). Safe because a tap on the second screen can never move key focus to it (4.2.1) [I]. What it removes: the up-to-500 ms stalls when the browser's UI thread is slow; the usual delay was about one browser frame (cause 3).
4. **Prune only for the display being waited on.** In `shouldPruneInboundQueueLocked` (IF:1320-1361), in Dual, prune only when the touch's `displayId == mAwaitedApplicationDisplayId`. A tap on the second screen then never drops the main screen's queued keys and stick events (cause 7). The no-focused-window wait itself stays upstream: after step 1 it only happens on display 0, while the game's window is really missing.
5. Thimsy's Nano gates in the same file (P:175700-175811):
   - remove the `panel_touch` hook in `notifyMotion` (P:175774-175800);
   - keep the `touch_display` exemption: touches must still reach the second screen while a Nano menu holds the controller and `drop_input` is 1;
   - the FOCUS gate's "keys display" (P:175700-175730) becomes display 0 in Dual.
6. **idc for the built-in pad:** `/product/usr/idc/Vendor_484b_Product_1101.idc` with `keyboard.orientationAware = 0`, so the reader stops stamping its keys with the INTERNAL viewport (T: `KeyboardInputMapper.cpp:171-183, 238-241`) [C]. Its D-pad is a hat, so rotation never applied to it [I]. Step 1 is the fix; the idc keeps the reader's output clean for anything that reads the device's display (and for a build without step 1).

**4.2.3 One foreground app: display 0's**
- `GammapadVibrationBridge` (T: `.../gamepad/GammapadVibrationBridge.java:114-125, 147-155`): ignore `onTaskMovedToFront` unless `taskInfo.displayId == DEFAULT_DISPLAY`; call `getTasks(1, ..., DEFAULT_DISPLAY)` instead of `-1`.
- PhoneWindowManager's foreground-app lookups (`getRunningTasks(1)`, T: `policy/PhoneWindowManager.java:3340-3347`): hotkey profiles, hold-to-quit and save/load use display 0's focused task.
- gammapad's ScreenMapMode touchscreen (VirtualTouchscreen) must target display 0. An idc's `touch.displayId` takes a display **unique id** (T: `inputflinger/reader/mapper/TouchInputMapper.cpp:438`), not "display 0" [C], and the monitor's unique id changes with the monitor. So:
  - main = monitor: associate it at runtime with `addUniqueIdAssociation` (T: `input/InputManagerService.java:1846-1905`) to the current display 0's unique id, re-applied on every plug, unplug and swap (system_server, where the arrangement is known) [U, to be tried];
  - main = panel: no association needed; an internal touch device maps to the INTERNAL viewport, which is then display 0 [C].
- VirtualMouse (BrowserMode): Android 14 shows the pointer on `DEFAULT_DISPLAY` unless desktop mode is forced on external displays (T: `wm/InputManagerCallback.java:212-217`) [C]. Whether the `enable_pointer_choreographer` flag, which changes this, is on in this build is [U]. So the pointer is on display 0, the main screen, in both arrangements, and a mouse can't reach the second screen (D10).

**4.2.4 Taps on the second screen don't run the task-focus path**
- Recommended: fix the comparison in InputDispatcher. `dispatchPointerDownOutsideFocus` compares the touched window with the focused window of `mFocusedDisplayId` (IF:3741-3760) [C]. In Dual, compare it with the focused window of the **touched** display (pass the motion's display in). A tap on the second screen's focused window then posts no policy command at all, which also saves a round trip on every tap.
- Guard in `WindowManagerService.onPointerDownOutsideFocusLocked` (T: `wm/WindowManagerService.java:9440-9474`): when the touched window is another task on the second screen (a second app there), only set that display's focused app and call `updateFocusedWindowLocked`, without a TRANSIT_TO_FRONT on the main screen's stack [I].

**4.2.5 Keyboard (IME) on the second screen**
- Already in the patch: the panel's IME policy is LOCAL (P:~172950-172957), and `isInputMethodClientFocus` accepts a window on an own-focus display while a dual secondary exists (P:173455) [C].
- Known gap (dual_screen_plan.md): the keyboard showed only while the panel app had the controller [U]. P:173455 was meant to fix it; re-test with the controller on main. If it still fails, trace InputMethodManagerService's focused-display check for the client [U].
- The IME serves one client at a time: InputMethodManagerService keeps one focused client, so the game regaining window focus on display 0 takes the keyboard back. The game on main rarely asks for text, and under 4.2.1 nothing on a tap changes the game's window focus, so the browser should keep the keyboard [U until tested on the Cube].

### 4.3 Nano changes

- Remove Controller Screen (`persist.gammaos.nano.ctrl_screen`, the row at NanoMenuPS3Menu.cpp:9841, `NanoScreenChoice.h` `focusOnLaunch` / `focusOnTouch`, P:65162-65190). Clear the property once at boot.
- Remove every `panelAppFocus(true)` caller: `panelLaunch` on launch, `screenSwap`, `screenSwapPoll`, the Control Center in Dual, Quick Menu "Use Built-in Screen App" (P:42718). Nano never writes `sys.gammaos.nano.focus.display` in Dual.
- Keep `panelTouchSync` / `sys.gammaos.nano.touch_display` (NanoMenuOverlay.cpp:2624-2633).
- `panelAppInputGate` (swallowing everything but the Anbernic button while a panel app "has the controller") goes away; a panel app never has it.
- `drop_input` is set only while Nano's own UI owns the controller.
- The Switch Screen chord becomes Swap Screens (section 5.4).

**Until the swap and the task move ship (if Phase 1 ships alone):**
- Anbernic + the top button does nothing in Dual except a short notice ("Swap Screens comes with the Main Screen setting"); it must not move the controller any more.
- Swap Screens rows and tiles are hidden.
- Quick Menu > Second Screen App > Move to Main Screen uses today's `am start --display` path (the app may restart, as today); Phase 2 replaces it with the task move.
- Move App on a game is refused with a notice until Phases 3 and 4 make it Swap Screens.

### 4.4 Edge cases

- **Quick Menu.** The Anbernic button opens it over the game, as today. It draws on the Cube's screen (decision D2) and Nano reads the controller itself. It acts on the main screen's app by default ("Close Current App", Game Options); a "Second Screen App" submenu holds Move to Main Screen and Close. The game gets no input while it is open (`drop_input`), and gets it back on close.
- **Overlays.** The button map overlay is about the game on main; it draws where the Quick Menu draws (D2). Notices and the focus glow: the glow now marks the new main screen after a swap.
- **Nano's menus on the second screen.** When the monitor is main, Nano's menus draw on the Cube's screen (today's `uiOnPanel`), which is the second screen. They still take the controller because Nano reads evdev, not Android focus. Touch works there too (`touch_display`). The app on the second screen is hidden under the menu while it is open.
- **Built-in controls.** Through gammapad: external, no display id, goes to main [C]. Without gammapad: 4.2.2 step 1 and the idc send them to main. The Anbernic button, Volume and Power stay with PhoneWindowManager and Nano.
- **Bluetooth or USB controller.** External, no display id: goes to main today and after [C]. gammapad's clone of it is BUS_BLUETOOTH too [C].
- **Bluetooth or USB keyboard.** External, so it types into the main screen [C]. Text on the second screen comes from the on-screen keyboard there. Decision D9 if a keyboard should ever type into the second screen (it would need a port association; not proposed).
- **Mouse.** The pointer is on `DEFAULT_DISPLAY` (T: `wm/InputManagerCallback.java:212-217`) [C], so it is on main in both arrangements and can't reach the second screen. With the Cube's screen main, that leaves the monitor view-only. Supporting a mouse on the second screen is new work (moving the pointer display, and pointer events with that display), not a check (D10).
- **Back on the second screen.** The controller's Back goes to main (the nav-key rule plus 4.2.2). The second screen has no navigation bar. Proposal: Quick Menu > Second Screen App > Back, which injects KEYCODE_BACK with the second screen's display id. That only works with 4.2.2 step 2: today the nav-key rule sends even an injected BACK to the top focused display, that is to the game [C, IF:83-95, 2240-2246]. A touch gesture later (D7).
- **Emulators with a presentation screen (Azahar, WatermelonDS) while the monitor is main.** Their second (touch) screen is a Presentation window on the Cube's screen. The controller drives the game on the monitor while touch drives the Presentation, which is what requirement 3 asks for; Phase 1 tests it. A Presentation is a window, not a task, so for Move App the Cube's screen looks free (6.5).
- **System dialogs, ANR dialogs, toasts:** go to main. Android Settings opened on the second screen is touch-only.
- **drastic-nano.** Its bottom DS screen on the Cube's screen with touch is unchanged while the monitor is main; the controller stays with the game. With the Cube's screen main, see 5.6.
- **Two-panel devices (RG DS).** Not in Dual: their Control Center pin and `forceDefaultFocus` path stay. The dispatcher changes are gated on `sys.gammaos.display.second` (4.2.2).
- **Unplugging the monitor** (at the monitor end). Unchanged: the game comes back to the Cube's screen; the second screen's app closes (`REMOVE_MODE_DESTROY_CONTENT`). There is an open TODO decision about keeping it.

### 4.5 Files

- `frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java`, `WindowManagerService.java` (pin poll, `onPointerDownOutsideFocusLocked`).
- `frameworks/native/services/inputflinger/dispatcher/InputDispatcher.cpp`, possibly `AnrTracker.h/.cpp` (+ `inputflinger/tests/InputDispatcher_test.cpp`).
- `frameworks/base/services/core/java/com/android/server/display/LogicalDisplayMapper.java` (publishes `sys.gammaos.display.second`).
- `frameworks/base/services/core/java/com/android/server/gamepad/GammapadVibrationBridge.java`.
- `frameworks/base/services/core/java/com/android/server/policy/PhoneWindowManager.java`.
- `gammapad` (VirtualTouchscreen display), an idc under `device/.../idc` or the product's `usr/idc`.
- Nano: `NanoMenuOverlay.cpp`, `NanoScreenChoice.h`, `NanoMenuPS3Menu.cpp`, `NanoMenuPS3Data.h`, `NanoControlCenter.cpp`, `ps3xmb/i18n/*.json`.
- SystemUI: drop `config_gammaos_secondary_display_id=2` or read the id from the new property (5.2).

## 5. Swapping the main screen

### 5.1 Design: main screen = display 0

- New property `persist.gammaos.display.main` = `monitor` (default) | `panel`. It matters only in Dual with a monitor.
- With `panel`, the arrangement makes the panel display 0 and the monitor the secondary. A swap exchanges the physical devices behind logical display 0 and the secondary's logical display.
- Why display 0: everything that targets display 0 then follows the main screen without a change: game launches (Nano's `--display 0`, NanoMenuOverlay.cpp:1393-1397), the cold-boot home's games (in display 0's Android home task, which must not be moved: display 0 would be left without its home [I]), system dialogs, the IME fallback, the brightness slider, InputDispatcher's default display, and focus rule 4.1 [C for each target; I that nothing else is needed].
- The machinery exists [C]:
  - `GammaosScreenArrangement.plan()` (P:168477) already gives display 0 to different devices (the monitor in Docked and Dual, the panel in Mirror).
  - `applyLayoutLocked` moves devices between logical displays with `swapDisplaysLocked` (T: `display/LogicalDisplayMapper.java:1264-1300`, `LogicalDisplay.java:917`) and raises `LOGICAL_DISPLAY_EVENT_SWAPPED` (:919-920).
  - Games survive a display-0 device change: `mThimsyScreenSwapPending` (P:172919) and `ensureActivityConfiguration` (P:172740-172763) hand the size, density and input change to the game without a relaunch.
  - The mode watch polls the property each second (P:169402).

**A fixed id for the second-screen role (essential).**
- Whichever device is secondary must get the same logical id N (the panel's remembered id for the boot). Then a swap is a two-way swap of LD0 and LDN: both DisplayContents survive, and each logical display's apps go with it.
- Without it, the monitor in `plan.on` would get a fresh id, LDN would end with no device and be removed, and `REMOVE_MODE_DESTROY_CONTENT` would close the second screen's apps [I, from T: `LogicalDisplayMapper.java:1285-1300` and P:169511].
- Publish N as `sys.gammaos.display.second`. Phase 1 already adds the property for the panel (4.2.2); from Phase 3 it names the second-screen role, whichever device holds it. Nano, SystemUI and the Quick Menu read it instead of assuming 2.

**Option B (rejected): keep the monitor at display 0 and move only the "main" role.** Every launch that names no display (system dialogs, notifications, sub-activities) would land on the monitor; the cold-boot home's games can't be put on the panel; the brightness slider would drive the monitor; SurfaceFlinger, InputDispatcher, ExternalDisplayPolicy and DisplayWindowSettings would each need a "main id"; and plugging in would still move display 0 under the running game. `external_display_modes_plan.md` §2.1/§2.6 rejected this kind of design before [C, the doc].

### 5.2 Framework changes

1. **Arrangement.** `GammaosScreenArrangement.plan(..., mode, main)`: Dual with main = panel gives display0 = the kept panel, `on` = the first monitor plus other panels, `off` = other monitors. Pure and host-checked.
2. **LogicalDisplayMapper.** Today there is no single secondary id: the patch keeps a per-address map `mGammaosDisplayIds`, and both `gammaosRememberIdLocked` and `gammaosAddToLayoutLocked` skip monitors (P:169392-169394, 169500-169518) [C]. Add a role id (`mGammaosSecondRoleId`) kept for the boot: taken from the panel's remembered id when it has one, otherwise allocated from `mIdProducer` the first time any device takes the second-screen role. That covers main = panel from boot, where the monitor arrives as the secondary with no remembered id. `gammaosAddToLayoutLocked` gives the role id to the secondary-role device, monitor or panel; rename `mGammaosSecondaryPanels` to secondaries and include the monitor when main = panel. The mode watch compares the main property too, and runs while a monitor is connected (today it stops when `mGammaosPanelAddress == null`, P:169402-169418).
3. **Flags by role.** `LogicalDisplay.setGammaosSecondaryPanelLocked` (P:169352) applies to the secondary whichever device it is. `FLAG_OWN_FOCUS | FLAG_STEAL_TOP_FOCUS_DISABLED` always; `FLAG_PRESENTATION` on the monitor too (D10).
4. **Drop the `TYPE_INTERNAL` tests.** `DisplayContent.isGammaosDualScreenPanel()` (P:172989) becomes `isGammaosDualScreenSecondary()`: non-default, physical, own focus, can't steal top focus. Used by `hasOwnFocus()` for display 0 (P:~172981), `getImePolicy` (P:~172955), `gammaosHasDualScreenPanel` (P:173239), the RootWindowContainer loop (P:173184-173211) and PhoneWindowManager's `nanoDualScreenPanelOn()` (P:170553). A missed one breaks focus, the keyboard or the tap rule [C, for the list].
5. **Rotation lock for any monitor.** `DisplayWindowSettings.gammaosIsMonitor` (P:173075) requires display 0. Split it: "any physical monitor" for the rotation lock and `ignoreOrientationRequest`; the display-0 form for `setForcedDensity`. Square-as-landscape (`gammaSquareIsLandscapeHere`) already covers the panel at 0 or N [C].
6. **ExternalDisplayPolicy.** `isExternalDisplayLocked` (P:168384) treats every TYPE_EXTERNAL display except 0 as external, so it may hold the secondary monitor disabled at boot, switch it off for heat, or wait for SystemUI's connect dialog (SystemUI doesn't run in Nano mode). Exclude the arranged secondary [I].
7. **Power.** On SWAPPED, `handleLogicalDisplaySwappedLocked` (T: `display/DisplayManagerService.java:2857-2873`) runs `applyDisplayChangedLocked`, which tells the power controller only when the display has high brightness mode data (T: `display/HighBrightnessModeMetadataMapper.java:44-46`) [C]. The Cube has none, which is why the patch's `gammaosTellPowerControllerLocked` exists and is called on ADDED and REMOVED (DMS 2711-2715, 2803-2827), but not on SWAPPED [C]. Call it for both displays on SWAPPED, so the panel's backlight follows its logical display [I].
8. **SurfaceFlinger pacesetter.** In Dual the monitor is the pacesetter (P:178265). Make it the physical display showing layer stack 0, re-evaluated on a swap [I]. Expect one vsync re-sample on each swap.
9. **Mode list.** Keep `MIN_DUAL_HZ` = 50 (P:168564) whichever screen paces (D13). `refresh_matching_plan.md` assumes the monitor paces in Dual (its Dual Screen notes and decision 6, `kDualMinHz` 50); the pacesetter rule here (8) and that plan's rule must be one rule, cross-referenced in both plans (D21).
10. **Resolution trial.** Refuse a swap during the 15 s Keep/Revert trial, or revert first. When main = panel, Nano must draw the trial prompt on the monitor's surface, or it can be kept while the monitor is black [I].
11. **Shell entry for tests.** `cmd display gammaos-main monitor|panel` next to `gammaos-monitor-mode` (DisplayManagerShellCommand).
12. **Old-mode cleanup on SWAPPED.** `handleLogicalDisplaySwappedLocked` posts `gammaosClearOldExternalModes` whenever display 0 becomes `TYPE_INTERNAL`, assuming "a monitor unplugged" (P:168155-168166, 167808-167830) [C]. A swap to main = panel triggers it with the monitor still connected: it would clear display 0's forced size and the old mode properties (once, guarded by `CLEARED_PROP`) [C for the code; I for the harm]. Run it only when no monitor is connected.
13. **One swap at a time.** A swap requested while one is being applied, or during the 1-2 s re-apply blanking (5.6), waits for it to finish (one pending request, the latest wins). A monitor unplugged mid-swap: the arrangement is recomputed from the devices still connected; with no monitor, main = panel is the ordinary handheld layout and only display N goes, as with any unplug [I].

### 5.3 What follows the swap

| Item | With main = panel |
|---|---|
| Game | Moves to the Cube's screen without a relaunch (`mThimsyScreenSwapPending`) [C]. |
| Second screen's app | Moves to the monitor; relaunches with saved state unless it handles size and density changes [I]. |
| Game launch target | Display 0, now the Cube's screen [C]. |
| Controller | Display 0 (rule 4.1). |
| Touch | The Cube's touchscreen maps to the INTERNAL viewport, which is the panel's logical display wherever it is (T: `DisplayManagerService.java:3665-3707`) [C]. So touch is on main; the monitor has none. |
| Nano's menus | On the Cube's screen in both settings (D2): with main = panel that is Nano's normal handheld layout. |
| Home | Display 0 → the Cube's screen. Per theme: the table below. |
| Media (Nano's players) | Follow display 0 by default (D12). |
| Sound | Today's Audio Output setting (`persist.gammaos.display.audio` = monitor or speakers, `monitor_audio_option.md`) is about the monitor, not the main screen. Recommended: keep it as is, so sound stays on the monitor (if it has speakers) after a swap unless the owner picks Built-in Speakers (D20) [I]. |
| Refresh matching | Only the main screen is matched. With main = panel the monitor is not switched for a game; a 50 Hz panel is `refresh_matching_plan.md` phase 9. A swap, or a move of a game, releases a held rate and re-applies it for the new main screen (D21) [I]. |
| Brightness | The panel's backlight with the stock slider, once its power controller follows it [I]. |
| Power, sleep | Both screens in the default group, unchanged [C, P:169560]. |
| Monitor mode, resolution | Unchanged; decided per monitor model [C]. |
| Rotation, density | Per device (5.2.5); apps see a density change. |

**Home themes on each screen** (main = monitor is `dual_screen_plan.md`'s design, lines 28 and 61, unchanged; main = panel is proposed, D11):

| Theme | Main = monitor: monitor / Cube's screen | Main = panel: Cube's screen / monitor |
|---|---|---|
| ES-DE | the home / the combined game list (one screen) | single-screen home / wallpaper (D11) |
| DSi | top screen / its bottom (touch) screen | both screens on the Cube's screen, as handheld, since the monitor has no touch / wallpaper [I] |
| XMB | the XMB / wallpaper and wave | the XMB / wallpaper and wave |
| Minima | the home / wallpaper | the home / wallpaper |
| Nano's menus | drawn on the Cube's screen (D2) | drawn on the Cube's screen (D2) |

### 5.4 UI

- Nano Settings > Display > External Display > **Main Screen**: Monitor / Built-in Screen (shown in Dual Screen).
- Quick Menu > Display > **Swap Screens**.
- Nano's Quick Settings: a **Swap Screens** tile. SystemUI tile `MainScreenTile` only if wanted (SystemUI isn't used in Nano mode; D14).
- Hotkey: **Anbernic + the top button** (`persist.gammaos.hotkeys.screen`, `FACE_NORTH`) becomes Swap Screens. PhoneWindowManager keeps writing `sys.gammaos.nano.screen_swap` (P:171056-171080); Nano flips `persist.gammaos.display.main` instead of the pin. Rename the setting "Swap Screens Button".
- Feedback: the glow on the new main screen for a moment.
- **In Docked or Mirror, or with no monitor:** the Quick Menu's Swap Screens and Move to Other Screen rows and the Quick Settings tile are hidden; the two chords show a short notice ("Works in Dual Screen with a monitor") and do nothing else. The Main Screen setting row shows only in Dual Screen. Alternative: the chord offers to switch to Dual (D17).

### 5.5 Persistence, plug and boot

- `persist.gammaos.display.main` survives reboots. In Docked and Mirror it is ignored.
- Plug in with main = panel: the game stays on display 0 (the panel); the monitor appears as display N. Nothing moves.
- Unplug with main = panel: only display N goes; its apps close (as today's panel app does). The game is untouched.
- Plug in with main = monitor: today's behaviour (the game moves to the monitor).
- Boot with a monitor and main = panel: the panel is display 0 from the start; the monitor comes up as N with the role id allocated then (5.2.2). This can't be tested until the boot crash loop with a monitor attached is fixed (3.3 hazards).
- **Leaving Dual while the Cube's screen is main.** Dual → Docked: the monitor becomes display 0 and LDN is removed, so whatever was on the monitor (LDN) closes (`REMOVE_MODE_DESTROY_CONTENT`), and the game on the panel moves to the monitor. Dual → Mirror likewise closes LDN's app. Recommended: Nano swaps back to main = monitor first (the monitor's app comes to the panel, then closes as today's panel app does when leaving Dual; the game stays on display 0 and lands on the monitor in Docked). The `persist` value stays `panel` for the next time Dual is chosen (D16) [I].

### 5.6 Running apps and risks of the swap

- Every swap is a configuration change (1080p-4K ↔ 720x720). The game guard covers games by manifest only: `ActivityRecord.thimsyIsGame()` is `CATEGORY_GAME` or `FLAG_IS_GAME` (P:172792-172797) [C]. Nano's idea of a game is wider ("launched from a game system", the Android Games system included). An Android game without the manifest flag would be relaunched on a swap or move (progress lost); a game with it is told the change was handled when it was not (P:172763ff), which may break the layout of some engines (Unity) [I]. Decision D19 sets the rule (manifest flag, Nano's game list, or both); the game guard and Move App use the same rule.
- Java emulators that misbehave on resize (PPSSPP, Dolphin, Azahar, Flycast) and a couple of Android Games titles need a device check [U].
- drastic-nano follows display 0 by relaunching and resuming slot 9 (display_mirror_plan.md, review round 3) [C, doc]. With main = panel its bottom (touch) screen would land on the monitor, which has no touch: use "single" (both on the Cube's screen) or top-on-monitor when main = panel [I].
- WatermelonDS and Azahar put their second screen on a Presentation on the second display. In Android 14 a Presentation is not dismissed when its display changes: `handleDisplayChanged` only calls `onDisplayChanged` (T: `frameworks/base/core/java/android/app/Presentation.java:336-338`) [C]. With the fixed role id, a swap therefore leaves the emulator's bottom (touch) screen on LDN, which moves to the monitor with the old 720x720 metrics, while the game goes to the panel [I]. Options: re-create the Presentation (the emulator must handle `onDisplayChanged`, most don't), or refuse a swap while a Presentation is shown on the second screen and tell the owner to quit the game first. Recommended: refuse, until a device test shows an emulator coping [I].
- Each swap may blank both screens for 1-2 s while modes are re-applied (`external_display_modes_plan.md` §4.4) [I].
- Two-way swap correctness depends on the fixed id; a break closes the second screen's apps [I].

### 5.7 Nano changes for the swap

- `nanodisplay::choose` (P:17087): in Dual with the panel showing layer stack 0 and a monitor on another non-blank stack, `second` = the monitor, plus a "second is a monitor" flag. Unit-tested.
- `setDualPanel` / the second-screen pass: from "the panel at 720x720" to "the secondary at its own size" (up to 4K when it is the monitor).
- Touch belongs to the target when it is the panel (already true when `c.monitor` is false).
- `panelLaunch` and the "Open on" choices use the right display id (0 or N) instead of `mSecondaryAppliedLayerStacks[0]`. The per-app choice is keyed to the physical screen today. With the Cube's screen main and a game on it, an app set to "Open on Built-in Screen" would open over the game. Recommended: key it to the role instead ("Main Screen" / "Second Screen"), which keeps today's meaning while the monitor is main (D18).
- The trial prompt on the monitor's surface when main = panel.

## 6. Moving an app to the other screen

### 6.1 Mechanism

- Use `ActivityTaskManagerService.moveRootTaskToDisplay(taskId, displayId)` (T: `wm/ActivityTaskManagerService.java:3387-3398`) → `RootWindowContainer.moveRootTaskToDisplay` → `Task.reparent(TaskDisplayArea, onTop)` (T: `wm/RootWindowContainer.java:3368-3420`, `wm/Task.java:6220-6236`) [C]. The task moves with its state; there is no cold start.
- Shell form for testing: `am display move-stack <rootTaskId> <displayId>` (T: `am/ActivityManagerShellCommand.java:3206, 3260`); task ids from `am stack list` [C]. It works from adb because Shell holds `INTERNAL_SYSTEM_WINDOW` (T: `frameworks/base/packages/Shell/AndroidManifest.xml:173`) [C].
- Today Nano moves apps with `am start --display` (P:38898, 39017, 39034), which reparents an existing task (T: `wm/ActivityStarter.java:2845-2898`) [C], but "from the home Nano starts at boot, an app moving to the monitor starts again" (README) and `monitorAppToPanel` quits to home first.
- **Recommended:** a small system_server handler, not shell parsing. `thimsyMoveTopTask(fromDisplayId, toDisplayId)` in ActivityTaskManagerService, triggered by `sys.gammaos.nano.move_task` = `<seq>:<from>:<to>` in WindowManagerService's existing Nano poll (P:~173353), plus a `WindowManagerShellCommand` entry. It:
  - takes `mGlobalLock` and calls `RootWindowContainer.moveRootTaskToDisplay` directly. (`ATMS.moveRootTaskToDisplay` enforces `INTERNAL_SYSTEM_WINDOW` against the caller, ATMS 3387-3388; that passes on a system_server thread, but the direct call is simpler) [C];
  - picks the top standard-type, fullscreen, non-pinned task on `from`, and **refuses home-type tasks** explicitly (6.2);
  - **checks first**, because the failures are silent or throw: the target display exists and is the arranged second screen or 0; the task is not already on the target display area; `canBeLaunchedOnDisplay(to)` is true (T: `wm/Task.java:1832`, via `ActivityTaskSupervisor.canPlaceEntityOnDisplay`, `wm/ActivityTaskSupervisor.java:395-435`). `Task.reparent` with that false only logs a warning and returns, and `resumeNextFocusAfterReparent` (Task.java:4852-4858) still runs, so no reason would come back otherwise [C];
  - **catches every exception**: `moveRootTaskToDisplay` throws `IllegalArgumentException` for an unknown display (RWC 3410-3415), an unknown root task, a task already on that display area, or a second root home task (`wm/TaskDisplayArea.java:262-268`) [C]. Uncaught on WMS's `mH` thread it would kill system_server [I, strong];
  - sets `mThimsyScreenSwapPending` on its attached game activities (the D19 rule) before reparenting (today only a physical display change sets it, P:172919) [I];
  - reparents the task on top of `to`;
  - keeps the top focused display at 0 (rule 4.1);
  - writes `sys.gammaos.nano.move_result` = `<seq>:ok|<reason>` for Nano.

### 6.2 Rules

- **An app moved to the main screen** becomes main's focused app and gets the controller. **An app moved to the second screen** becomes touch-only.
- **Moving a game** (the D19 rule), or any home-type task such as a game the cold-boot home started, runs Swap Screens instead: the main screen follows the game (D3). One mechanism; a game is never left without the controller.
  - Home-type tasks **must not** be reparented [I]. The cited TaskDisplayArea code (`wm/TaskDisplayArea.java:262-268`) only throws when the target already has a root home task, and the second screen (no system decorations) has none [C]. So the move would go through and leave display 0 without its home. The handler refuses them (6.1).
  - Until Swap Screens ships (Phases 3 and 4), Move App on a game is refused with a notice (4.3).
- **The target screen already has an app** (D4). Recommended: swap the two apps (both move). Alternatives: refuse (today's rule, P:42716 "offered only where that screen is free") or stack the moved app on top (the other is stopped, not closed).
- **Main screen at the home** and the second screen's app moved to main: it opens over the home and takes the controller.

### 6.3 UI

- **Quick Menu:** **Move to Other Screen** for the app in front (on a game it is labelled **Swap Screens**). A **Second Screen App** submenu: Move to Main Screen, Back, Close. These replace "Use Built-in Screen App", "Move Built-in Screen App to Monitor" and "Move Current App to Built-in Screen" (P:42714-42724).
- **An app's options in Applications:** keep Open on Monitor / Open on Built-in Screen, remembered per app.
- **Hotkey:** **Move App Button**, `persist.gammaos.hotkeys.moveapp`, default `FACE_WEST` (the left button, printed Y), which looks free by default [I, from HOTKEYS.md and `kButtonMapButtons` P:45059; to be checked against every default]. It is an Anbernic-only chord, like the Switch Screen chord (HOTKEYS.md line 70): it is not added to the Start + Select set (where Start + Select + any hotkey button runs that hotkey, HOTKEYS.md line 78). Anbernic + it moves the main screen's app to the second screen (a game: Swap Screens). Use `bmapMigrateSetting` so it moves off a button the user has given to another hotkey. Anbernic + B, R1, L1, R2, L2, Start, Select, Volume and Back are taken [C, HOTKEYS.md].
- **Recents:** Nano mode has no Recents screen and no WM Shell, so no drag between screens [C]. Not planned.
- **Touch gesture (later, optional):** a three-finger swipe on the Cube's screen, detected in `InputDispatcher::notifyMotion` like `panel_touch` was. Risk with multi-touch apps [U].
- New strings go into every `ps3xmb/i18n/*.json` (the i18n check fails otherwise) [C].

### 6.4 Behaviour across densities and sizes

- A move is a configuration change: size, smallest width, screen layout, density, maybe orientation, and the touchscreen and navigation fields (the patch's `THIMSY_SCREEN_SWAP_INPUT_CHANGES`, P:172740-172760) [C]. An app that doesn't declare them relaunches with saved state [C, Android behaviour].
- Non-resizable or fixed-aspect apps keep their configuration in size-compat mode and are letterboxed; a later display change offers a restart, and apps that don't save state can lose it (source.android.com/docs/core/display/multi_display/displays, "size compatibility mode") [C, doc].
- Portrait-only apps on the monitor are pillarboxed, since the monitor ignores orientation requests [C, P:173075ff]. On the square panel landscape apps use the square-as-landscape rule [C].
- Density: the monitor's density comes from `monitorDensity` per mode; apps re-layout. Expect small text on a 4K monitor for apps that pin a dp size [I].

### 6.5 Failure cases

| Case | Result |
|---|---|
| No movable task (home only, a pinned task) | `move_result` reason; Nano shows "Nothing to move". |
| Home-type task (cold-boot home's game or app) | Game: Swap Screens. App: fall back to today's `am start --display` restart, with a note "will restart". |
| Target display gone (monitor unplugged mid-move) | The pre-check fails, or `moveRootTaskToDisplay` throws `IllegalArgumentException` (RWC 3410-3415), which the handler catches; the task stays; reason reported [C for the throw]. |
| App refuses the display (`canBeLaunchedOnDisplay` false) | The pre-check refuses it; the task stays; reason reported. Without the pre-check `Task.reparent` would only log a warning [C, T: `wm/Task.java:1832`; `wm/ActivityTaskSupervisor.java:395-435`]. |
| An emulator's Presentation on the Cube's screen (Azahar, WatermelonDS with the monitor main) | No task there, so the screen looks free. A moved app would sit under the Presentation, which is a `TYPE_PRESENTATION` window layered above apps (T: `policy/WindowManagerPolicy.java:540`, layer 3 against `APPLICATION_LAYER` = 2 in `WindowManagerPolicyConstants.java:193`) [C], and touch would keep going to the Presentation. The handler counts a visible Presentation as "occupied" and refuses, with a notice [I]. |
| App crashes on the configuration change | It restarts on the new screen from its launcher activity [I]. |
| FLAG_SECURE / DRM video to the monitor | Shown in Dual today (display_mirror_plan.md known gaps); DRM may refuse a monitor without HDCP [U]. |
| During the resolution trial | Refused until Keep or Revert. |
| Two moves quickly | The sequence number serialises them; the second waits for the first's result. |

## 7. Changes to tests

Host tests (no device; `atest --host`, see Nano's `test/README.md`):
- **`GammaosScreenArrangementCheck.java`** (Nano `test/java/`): Dual with main = panel (display 0 = the panel; the monitor in `on`); main = monitor unchanged; Docked and Mirror ignore `main`; two monitors (second off); no monitor ignores `main`; a two-panel device unchanged.
- **A new host check for the secondary id** (pull the id choice out of LogicalDisplayMapper into a pure helper, `GammaosSecondaryIds`): the same id N across monitor-main → panel-main → monitor-main; plug and unplug in each setting; boot with main = panel, where the monitor is the first device to take the role and no remembered id exists. This is where the "fixed id" invariant is guarded.
- **`NanoDisplayTarget_test.cpp`**: `choose()` with the panel at layer stack 0 and the monitor as `second`; the "second is a monitor" flag; Docked and Mirror unchanged.
- **`NanoScreenChoice_test.cpp`**: Controller Screen removed; the old property ignored; the "Open on" per-app choice keyed to the physical screen with the panel at 0 or N.
- **`NanoTouch_test.cpp`**: touch zones for Nano's UI on the panel when the panel is the target.
- **Hotkeys**: the Move App Button default and `bmapMigrateSetting` (wherever the hotkey migration is tested today; `NanoDeviceKeys_test.cpp` or a new case).
- **i18n check**: new strings in every language.

Built tests (need a build; run on the build host or the device):
- **`inputflinger_tests`** (`InputDispatcher_test.cpp`), with the Dual gate set:
  - a gamepad key stamped with display N reaches the policy with no display and goes to the focused display (0);
  - a non-injected BACK goes to display 0; an injected BACK with display N goes to N; an injected BACK with no display goes to the top focused display;
  - an injected key with display N stays on N;
  - a key on display 0 is not held by unacknowledged motion on display N; it still waits for unacknowledged motion on display 0; a global monitor's connection counts for its own display;
  - with display 0 waiting for a focused window, a finger-down on display N does not prune the queue; a finger-down on display 0 on another app still does;
  - a tap on display N's focused window posts no `onPointerDownOutsideFocus`;
  - `touch_display` touches pass while `drop_input` is 1;
  - with the gate off, all of the above behave as upstream.
- **wmtests** (`RootWindowContainerTests`, optional): the top focused display stays 0 with a dual secondary and a pin set; a tap on the secondary's focused window doesn't call `setFocusedTask`; the Move App handler refuses a home task, an unknown display and a task already there without throwing.

## 8. Phases

Each phase is usable on its own. Phase 1 can ship in build 43 alone if the swap slips; 4.3 says what the UI does meanwhile (the chord shows a notice, Swap rows hidden, Move to Main Screen uses today's restart path, Move App refuses a game).

| Phase | Content | Effort | Risk |
|---|---|---|---|
| 0 | Owner's read-only checks (3.3) on the installed build, load check included | Small | None (read-only, hazards respected) |
| 1 | Controller always on main: `sys.gammaos.display.second` from LogicalDisplayMapper (the gate), RootWindowContainer focus, InputDispatcher (display cleared at `notifyKey`, injected nav keys keep their display, per-display key wait, prune limited to the awaited display, the tap comparison), the idc, VirtualTouchscreen association, gammapad bridge and PhoneWindowManager foreground, Nano removes Controller Screen and the pin, Quick Menu Second Screen App submenu (Back, Close, Move to Main Screen by today's restart path), the interim chord notice, IME re-test, tests | Medium-large | Low-medium (input path; gated on Dual) |
| 2 | Move App: the system_server handler (pre-checks, exceptions caught, home tasks refused), Quick Menu rows, Move App hotkey, Move to Main Screen switched to the task move, Applications "Open on" kept; a game refused until Phases 3 and 4 | Small-medium | Low-medium (configuration changes of apps) |
| 3 | Main-screen swap, framework: arrangement, the second-screen role id, flags by role, `TYPE_INTERNAL` tests dropped, old-mode cleanup guarded, rotation lock split, ExternalDisplayPolicy, power controllers on SWAPPED, SurfaceFlinger pacesetter (agreed with the refresh-matching plan), trial guard, one swap at a time, swap refused while a Presentation is shown, game rule (D19) in the guard, `cmd display gammaos-main`, host checks | Large | High (display, power and pacing on the Cube) |
| 4 | Main-screen swap, Nano: `choose()`, the secondary surface at its own size, the trial prompt on the monitor, Settings row, Quick Menu Swap Screens, Quick Settings tile, the chord, home themes when main = panel (ES-DE, DSi), drastic-nano mode, swap back before leaving Dual (D16), "Open on" by role (D18), rows hidden outside Dual (D17), refresh matching on swap (D21) | Medium-large | Medium |
| 5 | Docs (README Dual Screen, HOTKEYS.md, dual_screen_plan.md), SystemUI id fix, optional gesture and SystemUI tile | Small | Low |

### On-device test steps

All steps: boot without the monitor and plug it in once Nano is up; unplug at the monitor end (3.3 hazards).

Phase 1 (with a monitor, Dual Screen):
1. Game on the monitor, Brave on the Cube's screen. Scroll and tap the browser while pressing A, B and the sticks in the game: every press reaches the game at once; the browser never goes back on B; no "BrowserMode: on" and no "Pruning input queue" in the log.
2. `dumpsys input` during step 1: FocusedDisplayId 0; FocusedWindows has the game on 0 and Brave on N; RecentQueue keys `displayId=0` or -1.
3. Repeat with gammapad off (`persist.gammaos.gamepad.enable=0`, set through Nano's setting, not `stop`/`start`): same result.
4. A Bluetooth pad: same result.
5. Tap a text field in Brave: the keyboard shows on the Cube's screen and types there while the game keeps the controller.
6. Anbernic button: Quick Menu opens on the Cube's screen, works with the controller, and the game gets nothing until it closes. Second Screen App > Back goes back a page in Brave.
7. Hold-to-quit closes the game, never the browser. Home (the Anbernic button's hold, where it is `HOME`) acts on the main screen.
8. Azahar in a dual-screen layout (top screen on the monitor, bottom on the Cube's screen): touch on the bottom screen works while the controller drives the game.
9. A game that uses gammapad's ScreenMapMode: its mapped taps land in the game on the monitor, not in the browser.
10. Anbernic + the top button: only the notice; the controller stays on the game.
11. Unplug the monitor at the monitor end: the game comes back to the Cube's screen with the controller; Brave closes.

Phase 2:
1. Move Brave from the Cube's screen to the monitor (no game): it keeps its page; it gets the controller.
2. Move it back: touch-only again; the page is kept.
3. Move onto an occupied screen: the chosen D4 behaviour.
4. Move App on a game: refused with the notice (until Phases 3 and 4; then it becomes Swap Screens).
5. An app started from the cold-boot home: the "will restart" path.
6. Move App onto the Cube's screen while Azahar shows its bottom screen there: refused.

Phase 3 and 4 (start with `cmd display gammaos-main` before any UI):
1. Swap with Nano's home only: the home on the Cube's screen, wallpaper on the monitor, touch on the Cube's screen.
2. Swap with a RetroArch game running: no relaunch; picture, sound and controller on the Cube's screen; swap back.
3. Swap with a game and Brave: both move; Brave keeps its page.
4. Repeat 2 with PPSSPP, Dolphin, Flycast, drastic-nano and two Android Games titles (one with the manifest game flag, one without). Azahar in a dual-screen layout: the swap is refused while its Presentation is shown.
5. Sleep and wake with main = panel; brightness slider and Nano's backlight drive the Cube's screen.
6. Unplug at the monitor end and replug with main = panel: nothing moves on the Cube's screen.
7. **Deferred until the boot crash loop with a monitor attached is fixed:** reboot with main = panel and a monitor attached. Until then, reboot with main = panel and no monitor, then plug it in: the monitor comes up as the second screen.
8. The resolution trial on the monitor with main = panel: the prompt shows on the monitor; a swap is refused during it.
9. Watch the pacing: no judder on the main screen's game after each swap (SurfaceFlinger pacesetter in `dumpsys SurfaceFlinger`).
10. Switch Dual → Docked and Dual → Mirror with main = panel and a game running: the D16 behaviour, no crash.
11. Press the swap chord twice quickly: one swap at a time, no closed apps.
12. In Docked and with no monitor: Swap Screens and Move App rows hidden, the chords show the notice.

All with the hazards in 3.3.

## 9. Owner decisions and open questions

Recommendations first in each.

- **D1. What a swap moves.** Recommended: the screens' contents swap (the game goes to the new main screen, the second screen's app to the other screen). Alternative: content stays on its physical screen and only the controller and launch target move (option B, rejected in 5.1).
- **D2. Where Nano's menus draw.** Recommended: always on the Cube's screen (touch works; with main = panel that is the normal handheld layout). Alternative: always on the main screen.
- **D3. Moving a game.** Recommended: the main screen follows the game (Swap Screens). Alternatives: refuse, or move it to the second screen without the controller.
- **D4. Moving onto a screen that has an app.** Recommended: swap the two apps. Alternatives: refuse (today), or stack.
- **D5. Hotkeys.** Anbernic + top = Swap Screens; Anbernic + left = Move App. OK?
- **D6. Controller Screen.** Remove the setting entirely (recommended), or keep a temporary hand-over to the second screen's app?
- **D7. Back on the second screen.** Quick Menu > Second Screen App > Back (recommended for build 43; needs the nav-key exemption, 4.2.2 step 2); a touch gesture later?
- **D8. BrowserMode** only for a browser on the main screen (recommended; a browser on the second screen never gets the controller).
- **D9. A physical keyboard** always types on the main screen (recommended).
- **D10. The monitor as second screen (main = panel).** A presentation display, so dual-screen emulators can use it? Recommended: view-only for build 43. A mouse there is new work (the pointer stays on display 0 in Android 14, 4.2.3), not a setting.
- **D11. ES-DE home with main = panel.** Single screen on the Cube's screen with wallpaper on the monitor (recommended), or the split home with the game list on the monitor?
- **D12. Nano's media players with main = panel:** the Cube's screen or the monitor?
- **D13. `MIN_DUAL_HZ`** kept at 50 when the panel paces (recommended)?
- **D14. A SystemUI Quick Settings tile** as well as Nano's?
- **D15. Unplugging** still closes the second screen's app (existing TODO decision)?
- **D16. Leaving Dual while the Cube's screen is main.** Recommended: swap back to main = monitor first, so the game lands on the monitor in Docked and only the second screen's app closes, as today (5.5). Alternative: keep the game on the Cube's screen and treat it as Mirror/handheld.
- **D17. Swap and Move outside Dual (Docked, Mirror, no monitor).** Recommended: rows and tile hidden, the chords show a notice. Alternative: the chord offers to switch to Dual.
- **D18. "Open on" per app with the Cube's screen main.** Recommended: keyed to the role (Main Screen / Second Screen). Alternative: keep the physical screen and block "Open on Built-in Screen" while a game runs there.
- **D19. What counts as a game** for Move App, the swap guard and "moving a game = Swap Screens". Recommended: the manifest flag (`CATEGORY_GAME` / `FLAG_IS_GAME`) **or** launched by Nano from a game system (the Android Games system included). Alternatives: the manifest flag only (today's guard), or Nano's list only.
- **D20. Sound with the Cube's screen main.** Recommended: keep the Audio Output setting as is (monitor or built-in speakers), independent of the main screen. Alternative: sound follows the main screen.
- **D21. Refresh matching in Dual.** Recommended: only the main screen is matched; with main = panel the monitor is not switched; a swap or a game move releases and re-applies the held rate; one pacesetter rule shared with `refresh_matching_plan.md` (its decision 6).

Open questions (technical, settled by phase 0 or the first device run):
- Which of causes 1-8 the owner hits (3.3).
- Whether the second screen's keyboard shows with the controller on main (4.2.5).
- Whether the swap's power-controller handover moves the backlight correctly (5.2.7).
- Whether `enable_pointer_choreographer` is on in this build (it would change where the mouse pointer goes, 4.2.3).
- Whether any Nano or Control Center injection of HOME/BACK passes a display id and relies on the nav-key rule (4.2.2 step 2).
- Whether `addUniqueIdAssociation` for VirtualTouchscreen works when re-applied at runtime (4.2.3).
- Whether `/vendor/etc/input-port-associations.xml` exists on the Cube (4.2.2 overrides it for controller events anyway).

## 10. Old decisions this plan supersedes

From `dual_screen_plan.md` (2026-11-17) and README "External monitors":
- "The monitor is the primary screen (Android display 0)": the main screen is a choice (monitor by default). It is always display 0, whichever device that is.
- "The controller follows the open UI ... An app on the Cube's screen gets the controller while it is focused": Android's controller focus is always on the main screen. Only Nano's own menus read the controller while open.
- Framework 2, "a fixed id for the panel (say 2)": a fixed id for the second-screen role, whichever device, published in `sys.gammaos.display.second`. The panel was display 3 in the logs.
- The old plan's `FLAG_OWN_CONTENT_ONLY` for the panel was never set (3.1); this plan doesn't add it.
- Framework 3, "focus moves are Nano's, through `sys.gammaos.nano.focus.display`": not used in Dual.
- Implementation, "the controller goes to the app (`sys.gammaos.nano.focus.display`, drop_input 0)", Quick Menu "Use Built-in Screen App": removed; replaced by Move to Main Screen.
- README "Who gets the controller" (Controller Screen: Monitor / Built-in Screen / Last Touched) and HOTKEYS.md "Switch Screen Button": removed and repurposed as Swap Screens.
