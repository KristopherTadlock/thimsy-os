# External Display Modes Plan

Status: being implemented (2026-11-16) as §R below. §R overrides everything after it: Mirror, Dual Display, mode choosing, `persist.gammaos.ext.mode` and the TV density are dropped. The rest stays as background. This plan replaces `external_display_routing_plan.md` (the "direct routing" proposal).

Date: 2026-11-13, revised 2026-11-16

## R. One screen at a time (user decision, 2026-11-16)

"All attached screens are independent." Each screen draws at its own native resolution and refresh rate, with no mirroring and no scaling. For now only one screen is on at a time:
- **No external display:** the internal panel is the screen.
- **An external display is plugged in:** it becomes Android's main display (display 0) and drives the whole system; the panel is switched off (its logical display disabled, so it is really powered off and apps don't see it).
- **Unplugged:** the panel is display 0 again and turns back on.
- **Boot:** the same rule when the displays come up. A monitor connected at boot is display 0 from then on; otherwise the panel turns on.

This is the only display mode. There is nothing to choose and nothing recorded: no forced size, no mirror, no saved mode.

**Resolution (user decision, 2026-11-16):** this build always runs a monitor at 1080p60; other resolutions come in a later build. Only two places hold 1080p60: the kernel DisplayPort driver (`scripts/sprd_drm.py`), which makes CEA 1080p60 the mode the composer sets on connect, and `GammaosScreenArrangement.monitorMode`, which holds the monitor at that mode (base mode and refresh range sent to SurfaceFlinger) so frame-rate votes can't move it; a monitor without a 1080p60 mode keeps the composer's. Everything else follows whatever mode the monitor has: Nano and drastic-nano size their surfaces from the active mode, and the monitor's density is 213 dpi at 1080 lines, scaled by the mode's shorter side (`monitorDensity`).

| Area | Change |
|---|---|
| `display/LogicalDisplayMapper` | When a physical external display device is added, rebuild the default layout with it as display 0 and the panel disabled (§2.3 item 1, `internalEnabled=false`). When the device at display 0 is removed, restore the panel (§2.3 items 2-3). Display 0 is always in the default display group (§2.3 item 4). |
| `display/LocalDisplayAdapter` | External displays don't get the secondary panel's own display group, and aren't "followers" that wait for the panel to turn on. A monitor is held at 1080p60 when it has that mode, and its density follows its resolution (see Resolution above). |
| `display/DisplayManagerService` | The `persist.gammaos.ext.*` blocks (primary, force_mirror, mirror_resize, half_4k), the forced size and brightness restore, and the forced-size marker are deleted. A physical external display ignores saved user-preferred modes. At boot, once: if any old `persist.gammaos.ext.*` property is set, clear display 0's forced size (a build before this one may have left it, R4) and clear the properties. |
| `wm/DisplayWindowSettings` | A physical external display never takes a forced size, and is fixed at rotation 0, ignoring apps' orientation requests (§2.4 Rotation). |
| `wm/DisplayContent` and the force_mirror checks in `ActivityTaskManagerService`, `DisplayAreaPolicy`, `DisplayContent`, `RootWindowContainer` | No auto-mirroring in Nano mode at all (`sys.gammaos.nano.ext_mirror` is gone); the force_mirror checks are deleted. |
| `frameworks/native/.../SurfaceFlinger.cpp` | The active display (vsync pacesetter) may be an external display: a powered-on external display is activatable when no other internal one is on, and a display powered on while the active one is off becomes active (§2.4). |
| SystemUI, Settings | The External Docking/Mirror/Resize Quick Settings tiles and the toolbox's external display switches are removed. |
| Nano | Nano draws on the physical display that shows layer stack 0 (display 0), re-creating its surface at that display's size when it changes. It ignores the touchscreen while that display is external. The mirror (`extMirrorUpdate`), the "keep the monitor switched on" thread, the mode chooser and 1080p auto mode, and the External as Primary / Force Mirror / Resize rows are removed. drastic-nano picks the same display. |

Trees: AOSP `~/gammaos-src` (GammaOS Next 1.4.4, LineageOS 21, ap2a release config). Build repo `~/thimsy`.

All paths below are relative to `~/gammaos-src/frameworks/base/` unless they start with `~/` or name another project (`frameworks/native/...`). Line numbers are for the current tree.

---

## 0. Summary

| Mode | What the user gets | Mechanism |
|---|---|---|
| **External Only** (default) | TV at 1080p60 is the only screen. The handheld panel is powered off. Apps, the IME, dialogs, Nano and games all render natively at 1920x1080. | The HDMI device backs **logical display 0**, done by editing the default `Layout` the way foldables swap panels. The internal panel's logical display is **disabled**, so its DisplayPowerController turns it OFF. |
| **Mirror** | TV at 1080p60 shows the 720x720 handheld screen aspect-fit (1080x1080, pillarboxed). Both screens are on. | The panel stays display 0. DMS puts the HDMI device on **layer stack 0**, with a letterboxed projection (legacy layer-stack mirroring through `LogicalDisplay.configureDisplayLocked`). The TV's own logical display is marked `FLAG_PRIVATE`, so apps don't treat it as a second screen. |
| **Dual Display** | TV is display 0 (native 1080p). The handheld is a secondary presentation display at 720x720. Games run on the TV. WatermelonDS and Azahar put their second screen on the handheld. | Same layout swap as External Only, but the internal panel's logical display stays **enabled** and gets `FLAG_PRESENTATION`. Screens are swapped with the emulator's own swap action, through a new universal hotkey and a Quick Menu row. |

- The TV mode (1920x1080 at the rate nearest 60 Hz, or the nearest mode) is chosen in DMS **before** the display is enabled or swapped. It no longer comes from Nano's shell call.
- Unplugging always restores the default layout (panel = display 0, enabled) before the stock removal code runs.
- One property selects the mode: `persist.gammaos.ext.mode`. It replaces `persist.gammaos.ext.{primary,force_mirror,mirror_resize,half_4k}`, which are migrated once.

Phases:
1. External Only + Mirror, with mode selection, migration and diagnostics.
2. Dual Display and swap screens.
3. Optional: swap the primary screen at the Nano home, and early-boot swap.

---

## 1. Why the current "External as Primary" fails

### 1.1 How the code path actually runs on this build

1. **Connected-display management (CDM) is ON in this build.**
   - `build/release/aconfig/ap2a/com.android.server.display.feature.flags/enable_connected_display_management_flag_values.textproto` sets it `ENABLED` (READ_ONLY).
   - ap2a is the only release config (`build/release/release_config_map.mk`).
   - So hotplug goes CONNECTED → `ExternalDisplayPolicy`. With `persist.sys.display.enable_on_connect.external` unset, the policy **disables** the new display (`services/core/java/com/android/server/display/ExternalDisplayPolicy.java:218-225`) and waits for user consent.
   - The consent is SystemUI's `MirroringConfirmationDialog`, which Nano covers.
2. **Nano works around the consent.**
   - `cmds/gammaos-nano/NanoMenuExtDisplay.cpp:119-147` (`keepDisplayEnabled`) polls `dumpsys display` and runs `cmd display enable-display N`.
   - That reaches `DisplayManagerService.enableConnectedDisplay` (`DisplayManagerService.java:3953`) → `setDisplayEnabledLocked` → LOGICAL_DISPLAY_EVENT_ADDED → `handleLogicalDisplayAddedLocked` (`DisplayManagerService.java:2512`).
   - **Only then** does the GammaOS block (`:2526-2602`) run. So the "flag path skips the code" hypothesis is **mostly false**: the code runs, but only after Nano enables the display, one to several seconds after plug-in.
3. **Display recognition is OK.**
   - The HWC doesn't report a connection type, so SurfaceFlinger types every display but the first External (the comment at `NanoMenuExtDisplay.cpp:150-151`).
   - `LocalDisplayAdapter` then sets `TYPE_EXTERNAL` with a `DisplayAddress.Physical` (`LocalDisplayAdapter.java:936-944`, `861-862`).
   - So `gammaosIsPhysicalExternalDisplayLocked` (`DisplayManagerService.java:667`) should match. Confirm with diagnostics D2.
4. **Property parsing is OK.**
   - `SystemProperties.getBoolean` accepts `1`/`true`.
   - `mods/50-nano.sh:220-221` only appends **defaults** to `build.prop`. A value already persisted in `/data/property` wins, for example `0` from an earlier toggle. Confirm with D1.

### 1.2 Root causes (ranked)

**R1. "Primary" never moves anything to the TV. It only resizes the panel's display.**
- `setWindowManagerMirroringLocked(true)` is a **no-op for physical displays**. The base `DisplayDevice.setWindowManagerMirroringLocked` is empty (`DisplayDevice.java:133`); only `VirtualDisplayAdapter` overrides it (`:356`).
- Its meaning is also inverted. "WM mirroring" means DMS should *not* choose a mirror source (`DisplayManagerService.java:5612`).
- What the TV shows is Android 14's default mirroring: WM `ContentRecorder` of display 0. Nano explicitly allows it in minimal-boot mode by writing `sys.gammaos.nano.ext_mirror` (`wm/DisplayContent.java:7016-7028`, `NanoMenuExtDisplay.cpp:205-209`).
- Display 0 stays backed by the 720x720 panel. Primary mode only calls `setForcedDisplaySize(0, extW, extH)` (`DisplayManagerService.java:2568-2573`, `478-532`). Apps then render 1920x1080 into a 720x405 letterbox **on the panel**, and the TV shows a mirror of that.

**R2. Nano is not a WM window, so the TV always shows Nano "small and boxed".**
- Nano's surface is a raw SurfaceFlinger layer, sized to the panel's mode (720x720), on the panel's layer stack (`cmds/gammaos-nano/NanoMenu.cpp:239-313`, `1113-1161`).
- `ContentRecorder` mirrors only the WM hierarchy, so Nano reaches the TV only through `extMirrorUpdate`. That function scales Nano's 720x720 surface to fit the TV, centred: 1080x1080 in a 1920x1080 frame (`NanoMenuExtDisplay.cpp:236-305`).
- This **exactly matches** what the user sees in Nano, whether or not the forced size was applied.
- On top of that, the home instance calls `setDisplayProjection` on the panel token (`NanoMenu.cpp:286`, `1157`). That fights WM's projection once display 0 is force-resized.

**R3. Backlight suppression is a one-shot write that the DisplayPowerController overrides.**
- `gammaosPostApplyForcedSize(..., suppressInternalBacklight=true)` calls `requestDisplayStateInternal(0, ON, BRIGHTNESS_OFF)` once (`DisplayManagerService.java:522-530`).
- The display-0 DPC still owns brightness. Any later DPC update puts its own value back: user activity dim/undim, auto-brightness, a brightness change, screen on, or Nano's Anbernic+Vol HUD.
- Also, it is not "panel off": the panel stays powered (STATE_ON) and only the backlight is dimmed. This explains "the Cube's screen stays on".

**R4. The forced size is captured from the TV's mode at ADDED time and never updated.**
- `di.logicalWidth/Height` is read when the display is enabled (`:2570`). Nano's `extAutoMode` then switches the TV to 1920x1080 some time later (`NanoMenuExtDisplay.cpp:307-344`, polling `dumpsys` up to 10 s).
- `handleLogicalDisplayChangedLocked` has no GammaOS code. If the TV came up in another mode (4K, 720p), display 0 keeps that size.
- `DisplayContent.setForcedSize` clamps to 3x the panel (2160), so a 4K TV gives a 2160x2160 square.
- The size is also persisted per uniqueId in `display_settings.xml`. If the device reboots or crashes while connected, the panel can stay force-resized until the restore runs.

**R5. Smaller issues that also hurt.**
- **Boot with the TV connected:** `ExternalDisplayPolicy` keeps the display enabled until boot completes, then disables it again (`ExternalDisplayPolicy.java:205-226`, `onBootCompleted`). That produces ADDED → REMOVED → (Nano) ADDED. The restore is skipped while an external is connected (`DisplayManagerService.java:633-639`), so the state depends on timing.
- **The HDMI device is in its own display group.** `LocalDisplayAdapter` treats every non-first local display as "secondary" (`LocalDisplayAdapter.java:960-967`), which gives the HDMI device its own display group (`FLAG_OWN_DISPLAY_GROUP`). That's harmless today, but it **blocks** using HDMI as display 0 (see §2.3).
- **drastic-nano's SF backend uses any second physical display as the DS bottom screen** (`cmds/drastic-nano/SfDisplayBackend.cpp:306-336`). With a TV connected it powers the TV on and draws the bottom screen there, regardless of the mode.

**Conclusion.** Even when it works as written, the current design can't give native 1080p on the TV:
- display 0 is still the panel;
- Nano isn't covered;
- the "mirroring" call is a no-op;
- the panel never really powers off;
- SurfaceFlinger keeps the panel as its active display and pacesetter.

It needs replacing, not tuning.

### 1.3 Device diagnostics to request (exact commands)

Ask the user to connect the TV, wait for the picture, stay in Nano, then run these and send back the output. Run D7 and D8 again after unplugging.

```bash
# D1 – effective properties (persisted values override build.prop)
adb shell 'getprop | grep -E "gammaos\.ext\.|gammaos\.nano\.ext|enable_on_connect|sysui.disable_mirroring|minimal_boot|nano.primary_display|ro.board.platform|ro.build.type|ro.sf.lcd_density|display.tweaks"'

# D2 – DMS view: CDM flag, logical displays, types, enabled state, layer stacks, groups, layout
adb shell dumpsys display > display.txt
adb shell 'dumpsys display | grep -iE "connected_display|mIsEnabled|mBaseDisplayInfo|type EXTERNAL|type INTERNAL|layerStack|mDisplayGroupId|Applying layout|DeviceStateToLayoutMap|mUserPreferredModeId|userPreferredModeId"'

# D3 – WM view: forced size/density on display 0, display contents and mirroring sessions
adb shell wm size -d 0; adb shell wm density -d 0
adb shell 'dumpsys window displays | grep -E "Display: mDisplayId|init=|cur=|app=|mIsSizeForced|ContentRecord|mCurrentUniqueDisplayId"'
adb shell 'dumpsys window | grep -iE "contentrecord|recording"'

# D4 – SurfaceFlinger: physical displays, active display, power modes, layer stacks, projections
adb shell 'dumpsys SurfaceFlinger --displays'
adb shell 'dumpsys SurfaceFlinger | grep -iE "active display|pacesetter|powerMode|layerStack|GammaOSNano|Display [0-9]+ \(|HWC display"'

# D5 – connector state and EDID
adb shell 'for c in /sys/class/drm/card*-*; do echo "$c $(cat $c/status 2>/dev/null) $(cat $c/modes 2>/dev/null | head -3 | tr "\n" " ")"; done'

# D6 – the logs of the plug-in sequence (clear first, then plug in the TV, wait 15 s)
adb logcat -c
#   ...plug the TV in, wait 15 s...
adb logcat -d -b main,system,events | grep -iE "GammaOS:|ExternalDisplayPolicy|LogicalDisplayMapper|DisplayManagerService|external display|GammaOSNano|SetEnabled on display|Applying layout|ContentRecorder|Content Recording|setForcedDisplaySize|BRIGHTNESS_OFF" > plug.txt

# D7 – backlight state while connected (and again after unplug)
adb shell 'cat /sys/class/backlight/*/brightness /sys/class/backlight/*/bl_power 2>/dev/null; dumpsys power | grep -iE "mScreenBrightness|Display Power: state"'

# D8 – after unplugging: confirm display 0 is restored
adb shell wm size -d 0; adb shell 'dumpsys display | grep -E "mIsEnabled|uniqueId"' 
```

What each result tells us:
- **R1/R4:** D3 shows `mIsSizeForced=true` on display 0 with the TV's size, and an active ContentRecorder session on the TV's display.
- **R3:** D6 contains "Setting internal display ON with BRIGHTNESS_OFF", but D7 shows a non-zero backlight.
- **Root cause is the property:** D1 shows `persist.gammaos.ext.primary=0` or `false`.
- **Recognition:** D2 shows `type EXTERNAL` and a physical address for the TV.
- **Group:** D2 shows the TV with `mDisplayGroupId` ≠ 0.
- **Build type:** `ro.build.type` tells whether the userdebug-only `enable_on_connect` property would even work. The plan below doesn't rely on it.
- **Layouts:** D2's `DeviceStateToLayoutMap` dump confirms there is only the default layout (no vendor `display_layout_configuration.xml`). The plan relies on that.

---

## 2. External Only: the TV backs display 0

### 2.1 Why the layout swap and not "direct routing"

The old plan kept the panel as display 0 and routed every launch path to the TV's display id: initial launch, recents, Quick Resume, restarts, task moves, IME, dialogs, file pickers, Nano and the keyboard. Every path that forgets lands on an invisible display.

Swapping which device backs display 0 makes all of these correct **by construction**:
- Every app, IME, dialog, toast and SystemUI window targets display 0, which is now the TV at its real resolution.
- `getNanoTargetDisplayId` (`wm/RootWindowContainer.java:2970`) already returns `DEFAULT_DISPLAY` on the Cube (`persist.gammaos.nano.primary_display=0`).
- Nano's layer stack 0 is shown on the TV.

The framework already supports this:
- `LogicalDisplayMapper.applyLayoutLocked` (`display/LogicalDisplayMapper.java:1116`) swaps devices between logical displays (`LogicalDisplay.swapDisplaysLocked`).
- `handleDisplayDeviceRemovedLocked` (`:650-680`) **already** moves display 0 to another local display when the default device disappears.
- Every local device has `FLAG_ALLOWED_TO_BE_DEFAULT_DISPLAY` (`LocalDisplayAdapter.java:886`).
- WM handles the swap through `DisplayContent.updateBaseDisplayMetricsIfNeeded` (`wm/DisplayContent.java:3123-3196`). It detects `physicalDisplayChanged` by uniqueId and re-applies the per-uniqueId window settings (forced size/density, rotation, ignore-orientation). TV and panel therefore keep separate settings automatically.
- DMS sends `notifyDefaultDisplayDeviceUpdated` on SWAPPED (`DisplayManagerService.java`, `handleLogicalDisplaySwappedLocked`).

A static `display_layout_configuration.xml` can't be used, for two reasons:
- Layouts are keyed by physical display id, which for HDMI includes an EDID hash, so it differs per TV.
- `enable_port_in_display_layout` is not enabled in ap2a.

So the layout is edited at runtime.

### 2.2 New class: `GammaExternalDisplayController`

New file: `services/core/java/com/android/server/display/GammaExternalDisplayController.java`. It is owned by DMS and runs under `mSyncRoot`, with delayed work on `mHandler`.

State:
- `mMode` (applied mode);
- `mExtDevice` (managed HDMI `DisplayDevice`);
- `mExtUniqueId`;
- `mGeneration` (incremented on every connect/disconnect/mode change; delayed runnables check it);
- `mLayoutSwapped`;
- `mPendingModeId`.

Pure logic goes in `GammaExtModePolicy.java` (no Android dependencies, host-testable, see §6):
- `parseMode(String)`;
- `migrate(primary, forceMirror, mirrorResize)`;
- `chooseMode(List<ModeSpec>)`, which returns 1920x1080 at |hz−60| minimum; otherwise the largest mode ≤1920x1080 with hz in 59–61; otherwise the smallest mode above 1080p at ~60 Hz; otherwise the active mode.

Hooks (all in `DisplayManagerService.java`):

| Hook | Change |
|---|---|
| `handleLogicalDisplayConnectedLocked` (`:2494`) | Before `mExternalDisplayPolicy.handleExternalDisplayConnectedLocked`: if `controller.managesLocked(display)` (physical TYPE_EXTERNAL; mode not `off`), call `controller.onConnectedLocked(display)` and skip the consent path, which would disable the display. |
| `ExternalDisplayPolicy.handleExternalDisplayConnectedLocked` (`ExternalDisplayPolicy.java:205-245`) | Add an injector check `isGammaManaged(display)` → send `EVENT_DISPLAY_CONNECTED` and return without `setEnabledLocked(false)`. This also stops SystemUI's pending-display dialog (enabled displays aren't pending). Keep the before-boot deferral list. |
| `handleLogicalDisplayAddedLocked` (`:2520-2602`) | **Delete** the GammaOS primary/force_mirror/mirror_resize/half_4k block. |
| `handleLogicalDisplayDisconnectedLocked` (`:2391-2434`), `handleLogicalDisplayRemovedLocked` (`:2676-2770`) | **Delete** the matching restore blocks. Keep only `controller.onLogicalDisplayGoneLocked(display)` for bookkeeping. Delete `gammaosPostApplyForcedSize`, `gammaosPostApplyExternalForcedSizeWithRetry`, `gammaosSnapshotInternalBrightnessLocked`, `gammaosPostRestoreInternalDisplay` and the half_4k fields (`:355-680`). Keep `gammaosIsPhysicalExternalDisplayLocked` and move it into the controller. |
| `configureDisplayLocked` (`:3599`) | Mirror mode, §3. |
| `DisplayManagerShellCommand` | New `gammaos-ext status`, `gammaos-ext mode <external_only\|mirror\|dual>`, `gammaos-ext swap-primary` (phase 3). Nano and adb use it. `enableConnectedDisplay` has no permission check today, so this is the same trust level. |
| `onBootCompleted` / `systemReady` | Run the property migration (§5.2). Apply a mode to a display that connected before boot completed. |

`onConnectedLocked(display)` sequence:
1. Increment `mGeneration` and record the device and uniqueId. Publish `sys.gammaos.ext.connected=1`.
2. **Mode first.** Unless `persist.gammaos.nano.ext_ask=1` (the user picks the resolution in Nano), choose a mode from `DisplayDeviceInfo.supportedModes` with `GammaExtModePolicy.chooseMode`. Apply it with `setUserPreferredDisplayModeInternal(displayId, mode)`, which persists per uniqueId in `PersistentDataStore` and votes through DisplayModeDirector.
   - Do **not** use the global `mUserPreferredMode`: `setupLogicalDisplay` (`:2457`) would apply it to the panel too.
   - Nano's `extAutoMode`/`setExtMode` auto path is removed.
3. Enable the display through `mLogicalDisplayMapper.setDisplayEnabledLocked(display, true)`.
4. Wait for the device's active mode to equal the chosen mode: poll on `mHandler` every 50 ms, up to 1500 ms, generation-checked. Then apply the mode's layout. This way WM sees a single 1920x1080 configuration instead of boot-mode then 1080p.
5. `applyModeLocked(mode)`:
   - `external_only`: `mLogicalDisplayMapper.gammaosSetDefaultDeviceLocked(extDevice, /*internalEnabled*/ false)`.
   - `dual`: the same with `internalEnabled = true` (phase 2).
   - `mirror`: no layout change. Set the mirror flags (§3) and `scheduleTraversalLocked`.
6. Publish `sys.gammaos.ext.state=<mode>`, `sys.gammaos.ext.default_phys=<physical id backing display 0>` and `sys.gammaos.ext.gen=<mGeneration>`. Nano and drastic-nano retarget from these (§2.5).
7. While connected, poll `persist.gammaos.ext.mode` every 500 ms. `SystemProperties.addChangeCallback` doesn't fire for properties set by other processes, as the comments at `wm/WindowManagerService.java:5633-5636` and `policy/PhoneWindowManager.java:3594-3600` already note. A change re-runs `applyModeLocked`. This lets the SystemUI tile and `adb setprop` work too; the shell command applies at once.

`onDisconnectedLocked` is reached from the device-removed path (§2.3) before the stock code:
- restore the default layout;
- clear `sys.gammaos.ext.*`;
- increment the generation.

There is no forced size to clear and no brightness to restore: the panel's DPC simply turns it back ON when its logical display is enabled again.

### 2.3 `LogicalDisplayMapper` changes

File: `services/core/java/com/android/server/display/LogicalDisplayMapper.java`

1. New `void gammaosSetDefaultDeviceLocked(DisplayDevice newDefault, boolean otherInternalEnabled)`:
   - `Layout layout = mDeviceStateToLayoutMap.get(DeviceStateToLayoutMap.STATE_DEFAULT)`. If `mDeviceStateToLayoutMap.size() > 1`, log and refuse: the device has real layouts and this is unsupported.
   - Remember the internal panel's address the first time (`mGammaInternalDefaultAddress`, the address currently at `DEFAULT_DISPLAY`).
   - Rebuild: `layout.removeDisplayLocked(...)` for every entry, then `layout.createDefaultDisplayLocked(newDefault address, mIdProducer)`, then `layout.createDisplayLocked(internalAddress, false, otherInternalEnabled, DEFAULT_DISPLAY_GROUP_NAME, mIdProducer, POSITION_UNKNOWN, null, null, null, null, null)`.
   - Mark the affected displays for transition (`resetLayoutLocked`-style `setIsInTransitionLocked(true)`). This blanks both screens during the swap, as foldables do. Otherwise call `applyLayoutLocked(); updateLogicalDisplaysLocked();` directly.
   - Result: display 0 gets SWAPPED (uniqueId changes to the HDMI's). The panel moves to a new logical id M, disabled or enabled. The HDMI's old logical display loses its device and is DISCONNECTED.
2. New `void gammaosRestoreDefaultLayoutLocked()`: rebuild the layout with only `createDefaultDisplayLocked(mGammaInternalDefaultAddress)`, then apply and update.
3. In `handleDisplayDeviceRemovedLocked` (`:650`), **first**: if the removed device is the one at `DEFAULT_DISPLAY` and `mGammaInternalDefaultAddress != null`, call `gammaosRestoreDefaultLayoutLocked()` and return.
   - Without this, the stock fallback calls `createDefaultDisplayLocked(internal)`. The layout still contains the panel's disabled entry, so `Layout.createDisplayLocked` rejects the duplicate address (`layout/Layout.java:111-114`) and display 0 would have **no device**.
4. `assignDisplayGroupLocked` (`:938-1033`): force `DEFAULT_DISPLAY` into `Display.DEFAULT_DISPLAY_GROUP`, whatever the device flags say.
   - Today the HDMI device carries `FLAG_OWN_DISPLAY_GROUP` (`LocalDisplayAdapter.java:960-962`), so display 0 would get its own group. PowerManager expects display 0 in the default group, and `Layout` throws for a default display outside it (`Layout.java:124-126`).
   - Also stop adding `FLAG_OWN_DISPLAY_GROUP` (and the secondary-panel treatment) to `TYPE_EXTERNAL` devices in `LocalDisplayAdapter.java:960`. Restrict that block to `mStaticDisplayInfo.isInternal && !mIsFirstDisplay` (a real second panel, as on the RG DS or Thor).

### 2.4 What happens around the swap

**SurfaceFlinger / HWC / hotplug order**
- SF hotplug creates the HDMI DisplayDevice. `LocalDisplayAdapter` adds the device. LogicalDisplayMapper creates logical display N → CONNECTED. The controller sets the mode, enables, waits, then swaps → display 0 SWAPPED.
- SF learns the new layer stack and projection per device in the next DMS traversal (`LogicalDisplay.configureDisplayLocked`, `setLayerStackLocked`).

**SF active display / pacesetter. This needs a native patch (`frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp`).**
- When the panel powers off, `getActivatableDisplay` (`:10375-10387`) only considers **internal** displays. SF then falls back to synthetic vsync (`:7466-7473`) and keeps the panel's schedule as pacesetter, which means judder on the TV.
- Patch `getActivatableDisplay`: if no other internal display is powered on, return a powered-on **external** display.
- Patch `processDisplayRemoved` (`:4981`): if the removed display is `mActiveDisplayId`, switch to `getActivatableDisplay()`, or leave it to the next power-on path, which already handles `!activeDisplay` (`:7413-7416`).
- The existing GammaOS pacesetter override in `onActiveDisplayChangedLocked` (`:10429-10446`) is a precedent.
- For Dual (both on), optionally add `persist.gammaos.ext.pacesetter=external`: on the next active-display change, the pacesetter becomes the HDMI display, since games run there.

**Power and backlight**
- The panel's logical display is disabled. Its DPC drives STATE_OFF (`display/state/DisplayStateController.java:82-84`). `LocalDisplayDevice` sets SF power mode OFF and the Lights HAL backlight to 0. The panel is really off, not just dimmed.
- Display 0's DPC now controls the HDMI device. It has no backlight (`BacklightAdapter.mBacklight` exists only for the first display, `LocalDisplayAdapter.java:2475-2480`), so brightness controls do nothing on the TV. That is expected.
- Sleep and wake work through the default group: the power key turns the TV signal off and on.
- GammaOS's DEFAULT_DISPLAY→all-internal propagation (`DisplayManagerService.java:742-756`) is skipped automatically, because display 0 is no longer `TYPE_INTERNAL`.
- **Risk:** with `persist.gammaos.display.tweaks=true`, HDMI is a "follower" that defers power-on until the first display reports ON (`LocalDisplayAdapter.java:1071-1081`). The panel never turns on, so the TV waits for the timeout. Exempt devices that back display 0 from the follower rule.
- **Risk:** Nano and drastic-nano call `SurfaceComposerClient::setDisplayPowerMode(token, ON)` directly on "secondary" panels (`cmds/gammaos-nano/NanoMenu.cpp:5719`, `NanoMenuRender.cpp:5607`, `cmds/drastic-nano/SfDisplayBackend.cpp:330`, `:542`). In External Only these must never target the panel. They are gated by the new display selection in §2.5.

**Input**
- DMS viewports follow the devices: the panel keeps the INTERNAL viewport, now with displayId M, and the HDMI device gets the EXTERNAL viewport with displayId 0.
- Internal touchscreens bind to the INTERNAL viewport. In External Only that viewport is inactive (panel OFF), so InputReader disables the touchscreen. A touch on the dark panel does nothing. That's right.
- In Dual, touch reaches display M, the handheld, which is what DS/3DS bottom screens need.
- Nano reads raw evdev touch in panel coordinates. Nano must ignore touch in External Only (§2.5).
- Gamepad and keys follow the focused display, which is 0.

**Density** (`ro.sf.lcd_density` applies only to internal displays)
- SF gives external displays `FALLBACK_DENSITY` = 213 dpi (`SurfaceFlinger.cpp:266`, `:1271-1273`). At 1920x1080 that is 1442x811 dp.
- Add `persist.gammaos.ext.density`, default **320**. That is the Android TV standard for 1080p: a 960x540 dp, 10-foot UI. 0 means Android's 213.
- Apply it in `LocalDisplayAdapter.getLogicalDensity()` (`:686`) for `!mStaticDisplayInfo.isInternal`. It becomes the TV's *base* density, so `wm density reset -d 0` returns to it. A user `wm density N -d 0` while connected persists per TV uniqueId.

**Rotation**
- When display 0 is the TV, the Cube's accelerometer must not rotate the TV picture.
- In `wm/DisplayWindowSettings.applySettingsToDisplayLocked`, add defaults for physical TYPE_EXTERNAL displays without stored settings:
  - `fixedToUserRotation = FIXED_TO_USER_ROTATION_ENABLED`;
  - `userRotation = ROTATION_0`;
  - `ignoreOrientationRequest = true` (portrait-only apps are pillarboxed instead of rotating the TV).
- WM re-applies rotation settings after a physical swap (`DisplayContent.java:3172-3175`).
- GammaOS's landscape mapping in `WindowManagerService.mapOrientationRequest` (`:4511-4545`) stays compatible: landscape is ROTATION_0 on a 16:9 display.
- The square-panel tie-break at `DisplayContent.java:2493-2510` doesn't trigger at 1920x1080.

**Unplug**
- SF hotplug disconnect → `LocalDisplayAdapter` removes the device → `handleDisplayDeviceRemovedLocked` restores the default layout (§2.3 item 3) → display 0 SWAPPED back to the panel. WM re-applies the panel's per-uniqueId settings and reconfigures to 720x720.
- The panel's DPC turns it ON as the default group is awake. Nano retargets on `sys.gammaos.ext.gen`. SF makes the panel active on power-on.
- Nothing is persisted against the panel's uniqueId, so a crash or reboot while connected can't leave the panel resized. This fixes R4's side effect.

**Boot with the TV connected**
- Phase 1 swaps after `onBootCompleted`, so boot and Nano start on the panel and move to the TV a moment later.
- Phase 3 can swap at CONNECTED during early boot, as foldables do. The risk is in WM's first-boot ordering and Nano's boot path.

### 2.5 Nano and drastic-nano

**Display selection, shared.** Add a helper `nanoDefaultPhysicalDisplay()`, in a new small header shared by gammaos-nano and drastic-nano. It returns the physical id named by `sys.gammaos.ext.default_phys`, or the physical display whose `getDisplayState().layerStack == 0`, or `ids.front()`. When `persist.gammaos.nano.primary_display > 0` (dual-panel devices), the current port rule wins.

Use the helper in:
- `NanoMenu::initSurfaceFlingerPath` (`NanoMenu.cpp:245-256`) and the `readyToRun` SF block (`:1113-1127`);
- `SfDisplayBackend` primary selection (`cmds/drastic-nano/SfDisplayBackend.cpp:302-316`).

**Follow layer stack 0, not the panel.**
- The "layerStack changed → reapplied" block (`NanoMenu.cpp:5675-5693`) moves Nano's layer to the panel's *new* stack. After a swap that would hide Nano on a dark panel.
- Change it to keep the layer on the stack of the display returned by `nanoDefaultPhysicalDisplay()`.

**Live retarget.**
- Both the home and the overlay instance poll `sys.gammaos.ext.gen` from the existing 1 s `pollExternalDisplay` cadence (`NanoMenuExtDisplay.cpp:346`). On a change they run `nanoRetargetDisplay()`.
- That generalises `overlayUpdateSurfaceSize` (`NanoMenuOverlay.cpp:2091-2215`) to the home instance:
  - switch `mDisplayToken`;
  - `setLayerStack`;
  - unbind EGL and destroy the surface;
  - `updateDefaultBufferSize` and `native_window_set_buffers_user_dimensions`;
  - recreate the EGL surface;
  - set `mWidth`/`mHeight`, `mDisplayDirty`, and invalidate size-dependent caches (blur FBOs, ES-DE layout caches such as `mEsdeGamelistGrid`, and the PS3 clock face buffers).
- Nano's render code already lays out from `mWidth`/`mHeight` every frame; it swaps them per pass for secondary panels (`NanoMenuRender.cpp:6457-6485`). Nano also runs at 1920x1080 on other GammaOS devices.
- Only call `setDisplayProjection` (`NanoMenu.cpp:286`, `1157`) when the target is an internal panel. For the TV, WM owns the projection.
- Fallback if a live retarget proves fragile: restart Nano's home process on a generation change. That costs about a second at the home and is acceptable for a hotplug.

**Remove or gate the old Nano external-display code.**
- `keepDisplayEnabled` and the `sys.gammaos.nano.ext_mirror` writes (`NanoMenuExtDisplay.cpp:119-147`, `199-210`): skip them when `sys.gammaos.ext.state` is set (DMS manages the display). Keep them as a fallback otherwise.
- `extAutoMode` (`:324-344`): removed from the auto path. The chooser stays behind `persist.gammaos.nano.ext_ask=1` and calls `setExtMode`; DMS then skips its own mode choice.
- `extMirrorUpdate` (`:236-305`): not needed in any mode once Mirror uses layer-stack mirroring (§3). Keep it only for the ContentRecorder fallback.
- Touch: ignore panel touches while `sys.gammaos.ext.state=external_only`.

**drastic-nano**
- The Cube already uses the SF backend: `nanoForceSfPath()` is true for `ums*` platforms (`NanoMenu.cpp:432-440`), and `NanoMenu.cpp:1740` forces `dnBackend = "sf"`.
- **Fix the overlay path too.** `NanoMenuOverlay.cpp:1797-1824` reads the property without the platform force. When `sys.gammaos.ext.state` is set, force `sf` there as well. The DRM backend drives the panel's CRTC directly and can't reach the TV.
- `SfDisplayBackend`:
  - primary = `nanoDefaultPhysicalDisplay()`;
  - secondary = the internal panel **only** when `sys.gammaos.ext.state=dual`;
  - External Only and Mirror → single screen, and never power the panel or the TV on directly.
- The DrasticSf host activity (`com.gammaos.drasticsf`) launches on display 0, the TV, with no change.

### 2.6 Comparison with the old direct-routing proposal

| | Layout swap (recommended) | Direct routing |
|---|---|---|
| Apps, IME, dialogs, toasts, pickers on the TV | Automatic (display 0) | Every launch path must be routed and kept in sync |
| Nano on the TV | Follows layer stack 0; one retarget | Separate token/stack/size code path |
| Panel off | Real power-off via a disabled logical display | Backlight sequencing |
| Unplug | One restore in the mapper | Move tasks back, invalidate the target |
| Per-display settings | WM's per-uniqueId store, for free | Manual |
| Risk | Config change for running apps on plug/unplug (same as rotating or folding); SF active-display patch | Missed paths land on an invisible display |

---

## 3. Mirror

### 3.1 What Android does today

The TV is enabled with no content. WM's `ContentRecorder` mirrors display 0's WM hierarchy:
- `DisplayContent.setDisplayMirroring` (`:7015`);
- in Nano mode only for the uniqueId in `sys.gammaos.nano.ext_mirror`;
- scaled uniformly, aspect-fit and centred (`ContentRecorder.java:561-584`, anisotropy correction disabled at `:135-139`).

So 720x720 becomes 1080x1080, pillarboxed, at the TV's mode. It doesn't include Nano's layer or drastic-nano's SF surfaces, which is why Nano had to add `extMirrorUpdate`.

The TV is also listed to apps as a `FLAG_PRESENTATION` display. WatermelonDS and Azahar therefore move their second screen onto it, and the TV stops being a mirror.

### 3.2 Recommended mechanism: layer-stack mirroring in DMS

- **Projection.** In `DisplayManagerService.configureDisplayLocked` (`:3599`), when the controller says Mirror and `device == mExtDevice`:
  - first call the TV's own `display.configureDisplayLocked(t, device, blank)`, so its DisplayModeDirector mode specs (1080p60) still reach SF;
  - then call `mLogicalDisplayMapper.getDisplayLocked(DEFAULT_DISPLAY).configureDisplayLocked(t, device, blank)`.
  - The second call puts the TV on **layer stack 0**, with the letterboxed projection computed in `LogicalDisplay.configureDisplayLocked` (`LogicalDisplay.java:~640-720`, the `device != mPrimaryDisplayDevice` mirror branch). Its empty mode spec is ignored (`LocalDisplayAdapter.java:1338-1342`).
  - This mirrors **everything** on layer stack 0: apps, Nano, the Nano overlay and drastic-nano. One mechanism, no `extMirrorUpdate`.
- **Hide the TV from apps.** Mark the TV's logical display `FLAG_PRIVATE` and clear `FLAG_PRESENTATION` while in Mirror. Add a controller-owned override in `LogicalDisplay.updateLocked` next to the flag mapping (`LogicalDisplay.java:425-441`).
  - Private displays are filtered from third-party `getDisplays()` (`Display.hasAccess`, `LogicalDisplayMapper.getDisplayIdsLocked:302-310`), and apps can't launch on them.
  - Emulators then keep their single-screen handheld layouts.
- **No WM recording.** `getDisplayIdToMirror` (`DisplayManagerService.java:5598`) returns `INVALID_DISPLAY` for the managed TV, so WM doesn't record a mirror nobody sees.
- **No home on the TV.** Replace the three `persist.gammaos.ext.force_mirror` gates with `controller mode == mirror`, published as `sys.gammaos.ext.state=mirror`:
  - `wm/DisplayContent.isHomeSupported` (`:5978-5985`);
  - `wm/RootWindowContainer` (`:3055-3062`);
  - `wm/DisplayAreaPolicy` (`:128-137`) and `wm/ActivityTaskManagerService.getSecondaryHomeIntent` (`:5744-5750`).
- **Unchanged:** no forced size and no backlight change; the panel stays display 0 and both screens stay on.
- **Mode:** 1080p60 from §2.2 step 2.

**Fallback (if layer-stack mirroring misbehaves on the Unisoc HWC):**
- keep stock `ContentRecorder` mirroring;
- move the `sys.gammaos.nano.ext_mirror` write from Nano into DMS (rename it `sys.gammaos.ext.mirror_uid`);
- keep Nano's `extMirrorUpdate`;
- keep `FLAG_PRIVATE`. With the private flag, check that `ContentRecorder` still records the display: it doesn't check access, but verify on the device.

### 3.3 Mirror risks
- The global shader (ThimsySurfaceFilter) may apply per display output, so the TV may or may not get the shader. Test.
- Frame pacing: the panel stays the pacesetter and the TV runs on its own clock, so expect an occasional dropped or repeated frame on the TV. That's acceptable for a mirror. Optionally use `persist.gammaos.ext.pacesetter=external`.

---

## 4. Dual Display (phase 2)

### 4.1 Mechanism

- **Layout:** `gammaosSetDefaultDeviceLocked(extDevice, /*internalEnabled*/ true)`. The TV is display 0 (1080p, TV density, rotation locked). The panel is logical display M: `TYPE_INTERNAL`, 720x720, its own density, default display group (so it sleeps with display 0), panel touch bound to M.
- **Presentation flag:** emulators use `android.app.Presentation`, and WM refuses presentation windows on displays without `FLAG_PRESENTATION`. In `LogicalDisplay.updateLocked`, add `Display.FLAG_PRESENTATION` for a `TYPE_INTERNAL` device on a non-default display.
  - Dual-panel GammaOS devices already set this for their second panel (`LocalDisplayAdapter.java:960-962`).
- **No home or mirroring on M:**
  - `isHomeSupported` false for the managed internal secondary;
  - `getDisplayIdToMirror(M)` returns INVALID, so full-Android mode doesn't mirror the TV onto the handheld. In Nano mode the minimal-boot gate at `DisplayContent.java:7024` already stops it.
- **Focus:** a touch on the handheld must not steal key focus from the TV game.
  - `WindowState.handleTapOutsideFocusInsideSelf` (`wm/WindowState.java:5979`) moves the touched display to the top. Presentation windows are focusable, so gamepad keys would go to the presentation.
  - Patch `receiveFocusFromTapOutside()` to return false for windows on the managed internal secondary while `sys.gammaos.ext.state=dual`.
  - Alternative: `persist.gammaos.multidisplay.dual_focus` (`WindowManagerService.java:724-734`). Per-display focus alone doesn't stop the display moving to the top, so the narrow patch is preferred.
- **Pacesetter:** optional TV pacesetter (§2.4).
- **Launches:** games go to display 0 through the existing paths. `getNanoTargetDisplayId` returns 0. The secondary-display package forcing is off in minimal_boot (`wm/ActivityStarter.java:3405-3412`).

### 4.2 Existing GammaOS dual-screen support

- `persist.gammaos.secondary_display.{enabled,packages}` (ActivityStarter) is inert in Nano mode. Leave it unchanged.
- `persist.gammaos.secondary_home`: don't set it. M should be presentation-only.
- `persist.gammaos.multidisplay.split_brightness`: not needed. The TV has no backlight and the panel keeps the normal pipeline. Make sure it stays off on the Cube.
- `persist.gammaos.dualstack.*` (tall-canvas mirror/crop) is a different feature that assumes display 0 is the internal panel. Disable DualStack while `sys.gammaos.ext.state` is `external_only` or `dual` (`DualStackController` checks).
- **Nano on the handheld.** Nano's secondary-surface machinery (`setupSecondaryEglSurfaces`, `NanoMenuRender.cpp:5554`; `renderEsdeSecondary`, `NanoThemeEngine.cpp:5928`; `renderMinimaSecondary`; the PSP clock secondary) only treats *internal* displays as second panels (`nanoInternalDisplay`, `NanoMenuRender.cpp:5525-5538`).
  - In Dual, define the second panel as "the internal panel when `sys.gammaos.ext.state=dual`". Nano then shows its backdrop or clock on the handheld at the home.
  - Over a game, the existing overlay rule leaves the secondary transparent (`NanoThemeEngine.cpp:5938`), so the emulator's presentation shows.
  - Hide the secondary layer entirely while an app with a presentation is in front: it costs composition and could cover the presentation.

### 4.3 How the emulators pick the second screen (from source, cloned read-only to `/tmp`)

**WatermelonDS 0.8.0.rc3 (`me.magnum.melondualds`)**
- **Screen choice.** `impl/layout/SecondaryDisplaySelector.kt` picks the first display that:
  - is not the activity's display;
  - is non-default or in `DISPLAY_CATEGORY_PRESENTATION`;
  - is not named "HiddenDisplay";
  - is not `STATE_OFF` and is valid.
- **Mode.** It's used when `external_display_mode` is `melon_dual_ds`, the default (`SharedPreferencesSettingsRepository.kt:1375`; `res/xml/pref_video.xml:104-110`). It shows an `ExternalPresentation` there (`EmulatorActivity.kt:1549-1610`).
- **Layout.** `dual_screen_preset`: `off` (default), `internal_top_external_bottom` or `internal_bottom_external_top` (`DualScreenPreset.kt`). Recommend provisioning `internal_top_external_bottom`: top screen on the TV, touch screen on the handheld.
- **Swap.** `Input.SWAP_SCREENS` is a mappable input (`Input.kt:32`). It swaps both the main layout and the presentation (`EmulatorActivity.kt:1815-1818`), so the existing `melonds_mapper` provisioning can bind it.

**Azahar 2126.2 (`org.azahar_emu.azahar`)**
- **Screen choice.** `display/SecondaryDisplay.kt`:
  - creates a hidden 1920x1080 virtual display, used when there's no real one;
  - lists displays that are non-default or presentation, not current, not "HiddenDisplay", not OFF;
  - prefers names that don't contain "Built". The panel's name is "Built-in Screen (port N)", so it is still picked when it's the only candidate.
- **Settings.** `ENABLE_SECONDARY_DISPLAY` defaults to true. `SECONDARY_DISPLAY_LAYOUT` defaults to 4 (`REVERSE_PRIMARY`), so the handheld shows the screen the TV doesn't.
- **Swap.** `Hotkey.SWAP_SCREEN = 10001` can be bound as `InputMapping_HostAxis_<keycode>`, the same pattern `azahar.json` already uses for 10003, 10005, 10006 and 10007.
- **Catalog.** The package is `org.azahar_emu.azahar` (`mods/added-apps.txt:16`), not `io.github.lime3ds.android`.

### 4.4 Swap screens: recommendation

**Recommended (phase 2): the emulator's own swap, through a new universal hotkey action `swap_screens`.**
- It's instant, needs no configuration change and keeps the session. Touch stays on the handheld.
- `ThimsyHotkeyController.java`: add `SWAP_SCREENS = "swap_screens"`.
- `PhoneWindowManager` chord dispatch (`~2860-2870`): add the button `persist.gammaos.hotkeys.swapscreens`, default **Y**. Add it to Nano's Hotkeys settings and to HOTKEYS.md.
- Profiles:
  - `mods/hotkeys/watermelonds.json`: `"swap_screens": {"driver":"key","key":"F9"}`, plus the provision bind `"SWAP_SCREENS": "139"`;
  - `mods/hotkeys/azahar.json`: the same key, plus `"InputMapping_HostAxis_139": "10001"`;
  - `drastic-nano.json`: `native` (its own `DsScreenLayout` swap);
  - the others: `none`.
- Optional WatermelonDS provisioning (`xml_prefs`, `ifUnset`): `dual_screen_preset=internal_top_external_bottom` in `me.magnum.melondualds_preferences.xml`.
- Quick Menu **"Swap Screens"** row (Dual mode, over a game):
  - Nano closes the menu and sets `sys.gammaos.hotkeys.request=swap_screens`;
  - PWM's existing 100 ms poll thread (`PhoneWindowManager.java:3594-3612`, which handles `overlay_esc`) runs `nanoHotkeys().onAction("swap_screens")` on its handler, after the overlay is down;
  - the key is injected only after the overlay is gone, so it isn't swallowed.

**Not recommended for in-game use: swapping the display layout.** Making the handheld display 0 and the TV secondary:
- changes display 0 from 1920x1080 to 720x720, with a full configuration change for every activity;
- emulators may recreate the activity or surface and lose the session;
- Nano retargets, the density changes, input viewports swap and the SF active display moves;
- 1–2 s of black.

It's offered in phase 3 only as **"Swap Primary Screen"** in the Quick Menu, **at the Nano home** (no game running), where it's cheap. It uses the same `gammaosSetDefaultDeviceLocked(internal, otherEnabled=true)` with the HDMI as the enabled secondary.

---

## 5. Mode selection UX and migration

### 5.1 Properties

| Property | Values | Default | Owner |
|---|---|---|---|
| `persist.gammaos.ext.mode` | `external_only` \| `mirror` \| `dual` \| `off` (`off` = stock Android behaviour, unmanaged) | `external_only` | user / Nano / tile; read by DMS |
| `persist.gammaos.ext.density` | dpi, 0 = Android default (213) | 320 | user |
| `persist.gammaos.ext.ask_mode` | 0/1: Nano asks which mode on connect | 0 | user |
| `persist.gammaos.nano.ext_ask` | 0/1: Nano asks the resolution (existing; DMS then skips auto 1080p60) | 0 | user |
| `persist.gammaos.ext.pacesetter` | `auto` \| `external` | `auto` | advanced |
| `persist.gammaos.hotkeys.swapscreens` | button name or Off | `BUTTON_Y` | user (phase 2) |
| `sys.gammaos.ext.connected` / `.state` / `.default_phys` / `.gen` | runtime, written by DMS | — | DMS |
| `sys.gammaos.hotkeys.request` | one-shot request to PWM | — | Nano |

Sepolicy needs no change: `persist.gammaos.` and `sys.gammaos.` are `persist_gammaos_prop` (`system/sepolicy/private/property_contexts:1688`, `:1693`). system_server, bootanim, platform_app and system_app already have set and get access (`system_server.te:1630-1631`, `bootanim.te:44`, `:58`, `platform_app.te:141`, `:144`).

### 5.2 Migration (DMS `systemReady`, once)

If `persist.gammaos.ext.mode` is empty:
- `persist.gammaos.ext.force_mirror` true → `mirror`;
- else `persist.gammaos.ext.primary` true → `external_only`;
- else `primary` explicitly false (`0`/`false`) → `mirror`, since the user turned primary off and had Android's mirroring;
- else → `external_only`.

Then reset `ext.primary`, `ext.force_mirror`, `ext.mirror_resize` and `ext.half_4k` to empty. The logic lives in `GammaExtModePolicy.migrate` (host-tested).

`~/thimsy/mods/50-nano.sh:218-221`: replace the two lines with `persist.gammaos.ext.mode=external_only` (and `persist.gammaos.ext.density=320`). Update the comment.

### 5.3 Nano settings and Quick Menu

- `cmds/gammaos-nano/NanoMenuSettingsTree.cpp:479-488`: replace External as Primary, Force Mirror, Mirror Resize and Half 4K with:
  - a chooser `"External Display Mode"` (`persist.gammaos.ext.mode`: External Only / Mirror / Dual Display);
  - `"Ask on Connect"` (`ext.ask_mode`);
  - `"Ask for Resolution"` (`nano.ext_ask`);
  - `"TV Density"` (`ext.density`).
- `NanoMenuPS3Menu.cpp:9783-9786` (settings table) and `NanoMenuPS3Data.h:274-281` (`kExternalDisplayCh`, with its descriptions): the same rows. Description for External Display Mode: "External Only: the TV is the only screen. Mirror: the TV shows this screen. Dual Display: the TV is the main screen and this screen is the second screen."
- Applying from Nano: write the property, then run `cmd display gammaos-ext mode <m>` off-thread for an immediate apply. The DMS poll catches it otherwise.
- Quick Menu > Display (`buildQuickDisplaySubmenu`, `NanoMenuPS3Menu.cpp:1859-1866`):
  - **"External Display Mode"** chooser, only while `sys.gammaos.ext.connected=1`;
  - **"Swap Screens"** (Dual, app behind, phase 2);
  - **"Swap Primary Screen"** (Dual, at the home, phase 3).
- Replace `QA_SECONDARY_DISPLAY` (`NanoMenuPS3Menu.cpp:1162`, `7083-7087`, `12170-12179`). It hard-codes `cmd display enable-display 2`, which is wrong once display ids move.
- On connect: silent External Only by default. With `ext.ask_mode=1`, reuse the chooser dialog infrastructure (`openExtModeChooser`, `NanoMenuExtDisplay.cpp:400`) with three mode options, with the same gating as `pollExternalDisplay` (`:346-376`) and `raiseForExternalDisplay`.

### 5.4 SystemUI tiles

- `ExternalDockingTile` (`packages/SystemUI/src/com/android/systemui/qs/tiles/ExternalDockingTile.java:57`, `:153-160`) becomes "External Display Mode". It cycles `persist.gammaos.ext.mode`: External Only → Mirror → Dual.
- `ExternalMirrorTile` and `ExternalResizeTile` are removed from the default list. Keep the classes, inert, for one release, or delete them.
- `~/thimsy/mods/50-nano.sh:236`: change `externaldocking,externalmirror,externalresize` to `externaldocking`.

---

## 6. Phases, files, effort

### Phase 1: External Only + Mirror (≈ 6–9 dev-days + device time)

| # | Change | Files |
|---|---|---|
| 1 | `GammaExtModePolicy` (pure) + host JUnit | `services/core/java/com/android/server/display/GammaExtModePolicy.java`, `~/thimsy/tests/java/...` |
| 2 | `GammaExternalDisplayController` + shell command | `display/GammaExternalDisplayController.java`, `display/DisplayManagerShellCommand.java` |
| 3 | DMS hooks; delete the old ext.* code (`:355-680`, `:2391-2434`, `:2520-2602`, `:2676-2770`); `configureDisplayLocked` mirror; `getDisplayIdToMirror` | `display/DisplayManagerService.java` |
| 4 | Skip consent for managed displays | `display/ExternalDisplayPolicy.java` |
| 5 | Layout swap/restore, removal hook, display 0 forced into the default group | `display/LogicalDisplayMapper.java` |
| 6 | No own-group or secondary treatment for externals; TV density; follower exemption | `display/LocalDisplayAdapter.java` |
| 7 | Private flag override (Mirror) | `display/LogicalDisplay.java` |
| 8 | Rotation and orientation defaults for physical externals; force_mirror gates → mode | `wm/DisplayWindowSettings.java`, `wm/DisplayContent.java`, `wm/RootWindowContainer.java`, `wm/DisplayAreaPolicy.java`, `wm/ActivityTaskManagerService.java`; `wm/DualStackController.java` (off while external is default) |
| 9 | Activatable external display; removal re-activation | `frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp` |
| 10 | Nano: display selection, follow stack 0, live retarget, gate the old ext code, ignore touch, settings rows, Quick Menu mode row | `cmds/gammaos-nano/NanoMenu.cpp`, `NanoMenuOverlay.cpp`, `NanoMenuExtDisplay.cpp`, `NanoMenuRender.cpp`, `NanoMenuSettingsTree.cpp`, `NanoMenuPS3Menu.cpp`, `NanoMenuPS3Data.h`, new `NanoDisplayTarget.h` |
| 11 | drastic-nano primary/secondary selection; force SF in the overlay launch | `cmds/drastic-nano/SfDisplayBackend.cpp`, `cmds/gammaos-nano/NanoMenuOverlay.cpp:1797` |
| 12 | Tile + build props | `ExternalDockingTile.java`, `~/thimsy/mods/50-nano.sh` |
| 13 | Device-side tests: extend `LogicalDisplayMapperTest`, `ExternalDisplayPolicyTest`, `LocalDisplayAdapterTest` | `services/tests/displayservicetests/src/com/android/server/display/` |
| 14 | Regenerate `~/thimsy/patches/thimsy/0001-thimsy.patch` and `work/patch-paths.txt`; docs | `~/thimsy` |

### Phase 2: Dual Display + swap screens (≈ 5–7 dev-days + device time)

| # | Change | Files |
|---|---|---|
| 1 | `dual` in the controller (internal enabled) | controller, `LogicalDisplayMapper.java` |
| 2 | `FLAG_PRESENTATION` for an internal panel off display 0; no home or mirror on M | `LogicalDisplay.java`, `DisplayManagerService.java`, `wm/DisplayContent.java` |
| 3 | Tap-outside focus patch | `wm/WindowState.java` |
| 4 | Optional external pacesetter | `SurfaceFlinger.cpp` |
| 5 | `swap_screens` action, chord property, PWM request poll | `policy/ThimsyHotkeyController.java`, `policy/PhoneWindowManager.java`, Nano hotkey settings, `~/thimsy/HOTKEYS.md` |
| 6 | Profiles + provisioning | `~/thimsy/mods/hotkeys/watermelonds.json`, `azahar.json`, `drastic-nano.json` |
| 7 | Nano second-panel = handheld in Dual; hide over presentations; Swap Screens row | `NanoMenuRender.cpp`, `NanoThemeEngine.cpp`, `NanoMenuPS3Menu.cpp` |
| 8 | drastic-nano dual on the handheld | `SfDisplayBackend.cpp` |

### Phase 3 (optional, ≈ 3–4 days)
- Swap Primary Screen at the home.
- Swap during early boot.
- Ask-on-connect polish.

---

## 7. Risks

1. **Configuration change on plug and unplug.** Every running activity sees display 0 change size and density. Emulators that don't handle `screenSize|smallestScreenSize|density|screenLayout` restart their activity and may lose unsaved progress. The same thing happens on foldables, but it's new on the Cube.
   - Mitigation: test each bundled emulator mid-game.
   - Optional: when a game is running at plug-in, Nano's overlay asks first (phase 3), or the swap is deferred until the game exits.
2. **The SF native patch** (`getActivatableDisplay`) touches scheduling. Without it External Only works but paces on synthetic vsync. Measure frame pacing with `dumpsys SurfaceFlinger --latency` or Perfetto.
3. **Runtime layout editing** assumes there are no vendor display layouts. The code refuses when `mDeviceStateToLayoutMap.size() > 1`. Confirm with D2.
4. **Nano live retarget**: EGL surface recreation on the Mali-G57 driver; stale size-dependent caches. Fallback: restart Nano on a generation change.
5. **Unisoc HWC**: power mode OFF on the panel while HDMI is on; mode switches; layer-stack mirroring (two outputs, one stack). All of these need a device.
6. **Removing the DMS ext.* code** also removes half_4k. 4K TVs are covered by choosing 1080p instead.
7. **GammaOS code that assumes display 0 is the internal panel**: DualStack, split brightness, the RG Rotate rotation key, the Nano DRM boot path, and IME pinning to the first non-default display (`WindowManagerService.java:759-790`). Audit by grepping `DEFAULT_DISPLAY` together with `TYPE_INTERNAL` in `wm/` and `display/`. Gate each with `sys.gammaos.ext.state`.
8. **Focus stealing in Dual**: the patch is narrow, but Presentation key handling differs per emulator. Test.
9. **SystemUI's connected-display dialog** in full-Android mode: avoided because managed displays are enabled at connect. Verify the dialog doesn't appear.

---

## 8. Testability

### On the host (no device)
- **`GammaExtModePolicy`**, plain JUnit with the host `javac`/`java` (`/usr/local/bin`). Run from a new `~/thimsy/tests/java` harness, invoked by `tests/run.sh slow`:
  - mode parsing and defaults;
  - migration truth table (all 2×2×2 combinations of primary/force_mirror/mirror_resize, plus empty values);
  - `chooseMode`: 1080p at 60, 59.94, 50 and 120; no 1080p (720p60 vs 4K60); only 4K30; only one mode.
- **`NanoDisplayTarget.h`**, a header-only function taking `(ids, layerStacks, prop values)` → chosen primary/secondary. Test it with host `g++` from `tests/unit` (the build already needs `cc`).
- **Python lint** (`tests/unit/test_data_lint.py`):
  - `swap_screens` uses a known driver in each profile;
  - provisioning keycodes don't clash with the existing F2/F4/F6/F7/F8 binds;
  - `mods/50-nano.sh` sets `persist.gammaos.ext.mode` to a valid value and no longer sets the old properties.
- **Instrumentation tests** (build on the host, run on the device or any A14 emulator with `atest DisplayServiceTests`):
  - `LogicalDisplayMapperTest`: swap HDMI to display 0, internal disabled or enabled; device removal restores; a duplicate-address restore doesn't leave display 0 without a device; display 0 always in the default group.
  - `ExternalDisplayPolicyTest`: a managed display isn't disabled.
  - `LocalDisplayAdapterTest`: TV density; no own group for externals.

### On the device only
- HWC power, mode set and layer-stack mirroring.
- Panel truly off: backlight sysfs 0, power mode OFF.
- SF pacing.
- Nano retarget.
- Emulator behaviour.
- Touch, focus, sleep and wake.

### Device test checklist (each in External Only, Mirror and Dual unless noted)

1. **Plug in at the Nano home.**
   - The TV comes up at 1920x1080 at about 60 Hz (`dumpsys display | grep -i userPreferredModeId`; the TV's info OSD).
   - **External Only:** Nano fills the TV at 1080p; the panel is dark, with backlight 0 and power mode OFF (D4, D7); panel touch does nothing.
   - **Mirror:** a 1080x1080 pillarboxed mirror that includes Nano; both screens on.
   - **Dual:** Nano on the TV, backdrop on the handheld.
2. Plug in during a game (RetroArch, PPSSPP, Dolphin, WatermelonDS, Azahar, drastic-nano SF). Check what happens to the session, and that the game ends up on the TV.
3. Unplug in each state: the panel comes back at 720x720 with the right density and brightness and touch works; `wm size -d 0` shows no override; Nano is back on the panel.
4. Replug quickly five times, then use a different TV: no stale ids, per-TV density kept, no hang.
5. Boot with the TV connected, and Quick Resume with the TV connected.
6. Switch modes from the Quick Menu while connected (all six transitions), and with `adb shell cmd display gammaos-ext mode ...`.
7. Sleep and wake with the power key while connected: the TV signal goes off and on, and the panel stays off (External Only).
8. Use the IME / Nano keyboard, an emulator file picker and an Android Settings dialog: all on the TV (External Only, Dual).
9. A portrait-only app in External Only is pillarboxed, not rotated. Tilting the Cube doesn't rotate the TV.
10. Frame pacing: a 60 fps scroll test in RetroArch on the TV (External Only) shows no regular stutter (Perfetto or `dumpsys SurfaceFlinger --latency`).
11. **Dual:**
    - WatermelonDS shows the top screen on the TV and the touch screen on the handheld; touch works; touching the handheld doesn't take gamepad input away from the game.
    - The Swap Screens hotkey and Quick Menu row swap in WatermelonDS and Azahar.
    - drastic-nano draws the bottom screen on the handheld.
12. **Mirror:** WatermelonDS and Azahar keep their single-screen handheld layout (the TV is hidden from apps); a game shows on both screens.
13. Full Android mode (outside Nano): the same plug and unplug behaviour, and no "Mirror to external display?" dialog.
14. Migration: flash over an install with `persist.gammaos.ext.primary=0` → Mirror; with the defaults → External Only.
