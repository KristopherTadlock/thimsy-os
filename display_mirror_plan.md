# Display mirroring plan

Status: implemented 2026-11-17, not yet run on a device. Builds on `dual_screen_plan.md` (docked and dual screen).

## Requirement (user, 2026-11-17)

- A third External Display mode, **Mirror**, mutually exclusive with Docked and Dual Screen; the switch happens at once.
- The internal screen drives the system; the monitor shows the same picture.
- Low latency: take the fully rendered primary output and scale it to the monitor, sharp bilinear, aspect kept: 720x720 becomes 1080x1080 centred on 1080p, 480x480 on 640x480. One frame of latency or less.
- Apps, dual-screen ones included (drastic-nano, WatermelonDS), see a single logical display.

## Design

- **Mode:** `persist.gammaos.display.mode=mirror`. Nano: Display Settings > External Display > Mirror Mode (three rows on the one property, options `X:On,*:Off`, where Off goes back to docked); the full settings list; the Quick Settings tile cycles Docked, Dual screen, Mirror.
- **Arrangement** (`GammaosScreenArrangement.plan`, host-checked): the panel stays display 0, the monitors are in the layout disabled (apps and WindowManager don't see them) and listed in `plan.mirrored`. LogicalDisplayMapper marks their LogicalDisplays (`setGammaosMirrorLocked`). The mode watch runs while a monitor is in, as before.
- **DisplayManagerService** (`gammaosConfigureMirrorLocked`): the monitor's device gets display 0's layer stack and projection, which keeps the aspect ratio and centres it (old-style SurfaceFlinger mirroring, not WindowManager's ContentRecorder, which needs an app-visible display). It is held at its mode (1080p60 when it has it), takes no input, and is on and off with display 0 (`updateDisplayStateLocked`, `gammaosWithMirrorsLocked`). Auto low latency mode follows display 0's.
- **SurfaceFlinger:** `gammaIsMirror` (a monitor, mode mirror, an internal display on the same layer stack) marks the output each frame. A monitor never paces in mirror mode: a panel powering on takes the active display back, and a dual/mirror switch moves it after the display changes.
- **CompositionEngine** (`Output::composeThimsyMirror`): the mirror output is all GPU composition. Pass 1 draws the layers at the layer stack's size (720x720, the panel's own pixels, with any composition shader, as on the panel) into a cached buffer; pass 2 draws that frame into the monitor's projection with the sharp-bilinear kernel (`ThimsySurfaceScaler::makeSharp`, the 4:3 Cropped filter's kernel; plain bilinear when shrinking). Both passes run in the same SurfaceFlinger frame as the panel's composition, so the monitor shows the frame at its next vsync. Internal-only (skip-screenshot) layers are shown too, so the PSP clock is mirrored.
- **Nano / drastic-nano:** `nanodisplay::choose` prefers the panel when both show layer stack 0, so the panel is the target and the monitor is never a second screen. While a monitor is connected the home launches drastic-nano through the DrasticSf activity, and drastic-nano never takes DRM master while a monitor is connected (that would also stop the monitor): the activity's Surface when the activity started it (`sys.gammaos.drastic_nano.hosted`, set by init), else its own SurfaceFlinger layer on the target display.

## Reviews (2026-11-17)
- Fixed: confirming Off on a row that was already Off reset the mode to docked; drastic-nano's launches without the DrasticSf activity (Quick Resume, Restart Game, the park loop) waiting 20 s for it; the second-panel rotation watchdog off in mirror mode; the pacesetter not moving on a dual/mirror switch (no screen powers on; now moved after display changes); the panel taking the pacesetter from a monitor in docked mode; the mirroring monitor flagged for input; a posted power update racing display 0's; the composition cache reusing a buffer drawn the other way, and the mirror's layer set not refreshed on a switch; translations (pt-BR, ar, pl, ru). Round 2: drastic-nano uses the hosted Surface exactly when DrasticSf started it and never DRM with a monitor connected (also fixes Quick Resume and Restart Game waiting for a Surface with a monitor as the screen); the posted power update covers every mirroring monitor; the pacesetter goes to the panel the monitor mirrors. Round 3: a DS session follows display 0 when a mode switch or plug moves it (relaunches on the new screen, resuming slot 9; also fixes the picture cropped after a docked plug or unplug mid-game); Quick Resume with a monitor goes through SurfaceFlinger on DRM-direct devices; a monitor counts only when SurfaceFlinger lists it; the External Display rows read the mode fresh (the tile can change it while they are open); the mirror's re-composition no longer advances the composition shaders' frame count or previous frame (the panel's picture is as with no monitor); a failed mirror buffer allocation falls back to direct composition.

## Known gaps

- The panel and the monitor run on separate clocks: with both at 60 Hz the monitor occasionally shows a frame twice or skips one as they drift.
- Nano's boot splash before Android is up draws on the panel only; the monitor joins once system_server arranges the screens.
- Brightness controls the panel only (display 0's level is passed on to the monitor's device, which has no backlight on the RG Cube).
- A switch takes effect within a second (system_server checks the mode each second while a monitor is in).
- On a two-panel device, drastic-nano hosted by DrasticSf draws only the top screen while a monitor mirrors.
- Secure (FLAG_SECURE) and DRM-protected layers are black on the monitor: SurfaceFlinger treats every monitor as not secure, Android's rule for such a screen. In docked and dual screen modes the composer shows them.
- Shrinking (a monitor under 720 lines) is one bilinear sample per pixel, so fine detail can shimmer.
- Touch on display 0 relies on the panel coming before the monitor in SurfaceFlinger's display list (it is added first at boot); a re-created panel display could map touches through the monitor's projection.
- In dual screen mode, drastic-nano hosted by DrasticSf draws both DS screens on the monitor, while a relaunch without it puts the bottom screen on the Cube's screen.
- For a moment after the panel blanks, or after a switch away from mirror before system_server re-arranges, the monitor composes the layers directly (unscaled filter, no internal-only layers).

## Risks (device)

- The Unisoc composer driving the panel and the DisplayPort output at once (same as dual screen).
- GPU cost of the two passes at 1080p on the Mali-G57 (expected about a millisecond).
