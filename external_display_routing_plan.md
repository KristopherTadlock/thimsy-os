# External Display Routing Plan

Status: proposed design for review; implementation has not started.

Date: 2026-11-09

Project: `/home/michaeltadlock/thimsy`

Android source: `/home/michaeltadlock/gammaos-src`

## Intended behavior

When External as Primary is enabled and an HDMI/DisplayPort monitor is attached, the monitor is the actual destination for the game, Nano's ES-DE menu, Quick Menu, and keyboard. Games receive the monitor's resolution and display context directly. The Cube is the fallback when the monitor disconnects or external routing fails.

This design keeps the Cube as Android logical display 0 and places game activities on the monitor's current logical display. It does not depend on mirroring a game that remains on the Cube.

## Correction to the earlier diagnosis

The physical-display adapter inherits a no-op `setWindowManagerMirroringLocked()` implementation. Therefore, setting that flag alone does not establish why the existing game mirror failed. The reported symptom is consistent with Nano's menu mirror working while the game's output does not reach the monitor, but the exact failure of the current mirror needs device evidence.

The direct-routing design below implements the requested behavior without depending on that mirror.

## 1. Create one shared display-target policy

Add a small framework policy that resolves the active destination:

```java
// Proposed policy pseudocode, not an existing API.
if (externalAsPrimary && monitorIsConnectedAndEnabled()) {
    return monitorLogicalDisplayId;
}
return handheldDisplayId;
```

Track:

- The detected HDMI/DisplayPort monitor's physical identity.
- Its current Android logical display ID.
- Whether it is connected, enabled, and ready.
- A connection generation, so delayed work from an earlier connection cannot affect a new one.

Logical IDs can change after reconnecting. Resolve them from the display registry each time; never assume the monitor is display 1 or 2. Distinguish the actual monitor from a second built-in panel that Android might label external.

Framework and native launch paths must consume the same policy. Native code needs a supported way to obtain the current logical target and connection generation, rather than maintaining a separate display-selection rule.

## 2. Route every game-launch path to that target

The project already has `getNanoTargetDisplayId()` in:

`frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java`

Extend that routing to select the monitor when External as Primary is active. Apply the same target to:

- Initial game launch from ES-DE.
- Recent games and Android Games.
- Game switching from the Quick Menu.
- Quick Resume.
- Emulator restarts and helper activities.

Framework activity launches should use the existing Android API:

```java
ActivityOptions options = ActivityOptions.makeBasic();
options.setLaunchDisplayId(targetDisplayId);
```

Native launch commands should use the equivalent resolved target:

```bash
am start --display <resolved-logical-display-id> ...
```

The existing overlay launch routing in `frameworks/base/cmds/gammaos-nano/NanoMenuOverlay.cpp` currently handles the handheld's multi-screen configuration. It needs to consume the external-display policy too.

Changing only the first launch is insufficient: subsequent launches and reused tasks can otherwise return to the Cube. Define how explicit per-app display choices and dual-screen settings interact with external-primary mode, preserving their behavior when the monitor is absent.

## 3. Move an already-running game when the monitor connects

A launch option affects new launches. Connecting a display during gameplay also requires moving the existing game task.

Use the framework's existing task reparenting support, including `RootWindowContainer.moveRootTaskToDisplay()` where appropriate:

1. Identify the tracked game's task and its current display.
2. Reparent the appropriate task to the monitor's task display area.
3. Update its configuration and focus.
4. Let Android deliver the display/resolution change to the emulator.

Some emulators recreate their graphics surface or activity during this transition. This needs testing; preserving the Android task does not guarantee every emulator preserves its running session.

On unplug, move the task back to the Cube through the normal display-removal handling. Avoid retaining a logical target that no longer exists.

## 4. Put Nano directly on the monitor

Nano currently mirrors its menu surface through `extMirrorUpdate()` in:

`frameworks/base/cmds/gammaos-nano/NanoMenuExtDisplay.cpp`

For external-primary mode, attach Nano's own surface to the monitor's layer stack instead. Update together:

- Display token and layer stack.
- Surface dimensions.
- Viewport and rotation.
- Theme layout and media sizing.
- Quick Menu and controller keyboard placement.

The ES-DE interface can then render at the monitor's actual resolution instead of enlarging a panel-sized menu.

Android dialogs, file pickers, and IME windows must follow the game's display context. Nano's custom keyboard overlay must follow the same active target. Surface recreation and display changes must invalidate any cached dimensions or placement associated with the old target.

## 5. Replace the current backlight sequence

The current external-primary implementation in:

`frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java`

resizes display 0 to the monitor's dimensions and suppresses its backlight. Replace that behavior for direct routing with:

1. Enable and configure the monitor.
2. Route Nano and the game onto it.
3. Confirm the relevant surface has presented a frame there.
4. Suppress the Cube's backlight.

Remove the display-0 resize from this mode: the game already receives the monitor's dimensions. Keep backlight suppression separate from making the Cube's logical display unavailable.

If routing or presentation fails, keep the Cube visible. On unplug, restore its backlight immediately and return the active content there. Revalidate the connection generation before any delayed backlight operation.

The readiness check must not rely solely on an `am start` success result or the existence of a display object. The external output needs to be usable and the requested content needs to have reached it. Use a bounded failure path that restores visible output.

## 6. Handle native DraStic separately

`drastic-nano` can render directly through DRM and stop SurfaceFlinger. Android activity routing cannot move that output.

The project already has a SurfaceFlinger-backed launch path through `com.gammaos.drasticsf/.DrasticSfActivity`, selected by the `sf` backend in `NanoMenuOverlay.cpp`.

For external-primary sessions, select that backend and launch the host activity on the monitor. Apply the same selection to initial launch, game switching, and Quick Resume.

Moving an existing DRM session requires a separate backend transition with state preservation. Until that transition is implemented, keep the DRM session's Cube screen visible. Do not suppress its backlight on the assumption that moving an Android task also moves direct DRM scanout.

## 7. Performance expectations

Direct routing adds no screen-capture buffer or additional game-frame queue. Hardware composition remains available; selected global shaders can still require GPU composition.

Rendering at 1080p may increase the emulator's own GPU workload. This is separate from routing overhead. Emulator render-resolution settings may need to remain independent of the monitor's output resolution.

No additional latency or performance numbers have been measured for this design. Measure presentation behavior on the RG Cube before making those claims.

## 8. Verification and completion criteria

Verify on the device:

- Launch RetroArch and standalone Android emulators with the monitor already attached.
- Attach and unplug the monitor during gameplay.
- Exercise short disconnects and repeated reconnects, including changed logical display IDs.
- Switch games and open the Quick Menu.
- Type through Nano Keyboard and open emulator file pickers or dialogs.
- Exercise Quick Resume and both DraStic backends.
- Change display modes and check viewport, rotation, and global crop shaders.
- Test routing failures and confirm neither screen becomes permanently dark.

For each game launch or move, confirm both the game's task display ID and visible game output on the monitor. A visible ES-DE menu alone is insufficient.

Completion requires:

1. Game, menu, Quick Menu, and keyboard appear on the attached monitor in external-primary mode.
2. The Cube's backlight is suppressed only after usable external output is established.
3. Unplugging restores visible output and control on the Cube.
4. Reconnects do not use stale display IDs or stale backlight operations.
5. Emulator session behavior during display changes is documented and tested.

This requires Thimsy framework/Nano changes. Ordinary Android emulators do not need recompiling. The native DraStic backend needs the separate handling described above.

## Implementation scope

Primary areas:

- `frameworks/base/services/core/java/com/android/server/display/DisplayManagerService.java`: connection state and backlight ordering.
- `frameworks/base/services/core/java/com/android/server/wm/RootWindowContainer.java`: Nano target selection and task movement.
- `frameworks/base/services/core/java/com/android/server/wm/TaskLaunchParamsModifier.java`: launch-display policy and inherited task placement, if required.
- `frameworks/base/cmds/gammaos-nano/NanoMenuOverlay.cpp`: resident launch commands, surface target, Quick Menu, and keyboard.
- `frameworks/base/cmds/gammaos-nano/NanoMenuExtDisplay.cpp`: monitor identification and direct Nano placement.
- Initial-launch and Quick Resume paths: use the same resolved target and backend choice.

This document is a proposal. No emulator, framework, firmware, or device state has been changed by writing it.
