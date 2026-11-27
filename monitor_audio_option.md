# External Display > Audio Output (sound on the built-in speakers)

Date: 2026-11-26. Status: in `patches/thimsy/0001-thimsy.patch` (1.4.4 base). Not committed. Not yet
run on an RG Cube (see "Not verified" below).

## What the owner gets

Settings > Display Settings > External Display (and the Quick Menu's Display copy, which builds
the same list) has a new row, **Audio Output**, after Controller Screen:

- **Monitor** (the default): the sound plays on the monitor or TV, as before.
- **Built-in Speakers**: the sound stays on the Cube, for a monitor without speakers. If headphones
  or Bluetooth audio are connected, it goes there instead, as it would with no monitor.

The row is always listed, so it can be set before a monitor is plugged in. The choice is kept in
`persist.gammaos.display.audio` (`monitor` or `speakers`; unset or any other value counts as
Monitor), so it holds through reboots and every later connect. A change while a monitor is
connected moves the sound within about a second, with no reconnect.

## How the sound reaches the monitor today

On the RG Cube the stock path never connects the monitor's audio. WiredAccessoryManager logs
"does not have DP audio support" for every extcon it checks. GammaOS's own code does it instead:
`AudioDeviceBroker`'s vendor HDMI path, gated by `persist.gammaos.unisoc.hdmi.enable` (1 on the
Cube). On each display event it runs `MSG_TOGGLE_HDMI`. That reads the evidence (extcon `audio3`
says `HDMI=1` and a `DP-`/`HDMI-` DRM connector is `connected`). Then it either:

- registers `DEVICE_OUT_HDMI` and `DEVICE_IN_HDMI` (`handleDeviceConnection`) and makes HDMI the
  preferred device for every product strategy; or
- clears those preferences and unregisters both devices when the evidence goes.

The Cube's logs in `out/dp-evidence/` show this path at work. Across the test 41/42 runs:
"GammaHDMI(UNISOC) register OUT=true IN=true" 114 times, "unregistered OUT/IN" 18 times,
`setPreferred ... rc=0` for strategies 15 to 24, and `[persist.gammaos.unisoc.hdmi.enable]: [1]`.
Allwinner and Rockchip devices have sibling branches with their own evidence.

## The mechanism

The least invasive switch is to leave the monitor's audio device unregistered. The audio policy
then never learns of it, and routes exactly as it does with no monitor (speaker, or headphones and
Bluetooth by their usual priority). The other options were rejected:

- Registering it and preferring the speaker would leave HDMI as a candidate that every strategy
  and app could still pick.
- An audio policy config change would sit on the vendor side, so it could not be per-user.
- `setPreferredDeviceForStrategy` isn't persisted by AudioService, and the vendor path already
  overwrites it with HDMI.

Changes (all in the patch):

- `services/core/java/com/android/server/audio/GammaosMonitorAudio.java` (new, pure): `PROP`,
  `register(present, value)` (register only when the evidence is there and the value isn't
  `speakers`), and `changed(applied, now)`.
- `AudioDeviceBroker.java`: the UniSoc, Allwinner and Rockchip branches of `MSG_TOGGLE_HDMI` pass
  their evidence through `gammaMonitorAudio(present)`. Their existing connect, disconnect and
  preferred-role code is untouched. With Built-in Speakers the evidence reads false, so:
  - a monitor that connects is never registered;
  - a registered one is unregistered by the same code an unplug runs (preferred roles cleared,
    devices disconnected, roles re-applied).

  While a monitor's audio is present, a runnable on the broker thread reads the property each
  second (`WATCH_MS`), and sends `MSG_TOGGLE_HDMI` when its meaning changed. This is the same
  approach DisplayManager uses for `persist.gammaos.display.mode`. It stops when the monitor goes.
  The stock path (other devices, `onToggleHdmi`) is unchanged.
- Nano (`NanoMenuPS3Menu.cpp`):
  - a `kPs3Bindings` row, `{"Audio Output", kProp, "persist.gammaos.display.audio", "monitor",
    "monitor:Monitor,speakers:Built-in Speakers"}`;
  - its leaf in `buildExternalDisplaySubmenu` (sound icon, with a description);
  - `externalDisplayTick` now keeps the cursor on the row it was on by label. Before, it would
    jump to Controller Screen when the list rebuilt.
- Translations of "Audio Output", "Built-in Speakers" and the description in all 21
  `ps3xmb/i18n/*.json` files.
- Docs: `docs/ARCHITECTURE.md` and `test/README.md` in Nano, and this repo's README.

A side effect to confirm on the device: with Built-in Speakers set, a boot with a monitor attached
never connects the monitor's audio device. That connection is what triggers the AudioService
NullPointerExceptions in the boot crash loop (`dp_dock_findings.md`). That crash isn't fixed by
this change, and it still happens with the default, Monitor.

## How it was verified

- **Host check** `GammaosMonitorAudioCheck` (new, `cmds/gammaos-nano/test/java/`) covers:
  - unset, empty, unknown and `monitor` values register the monitor;
  - `speakers` doesn't;
  - nothing registers without the evidence;
  - which changes re-run the toggle.

  Passes with AOSP's JDK 17.
- **Thimsy unit tests** `tests/unit/test_monitor_audio.py` (new):
  - runs that check from the tree or from the patch;
  - checks that Nano's row writes the property and values system_server reads, defaulting to
    Monitor;
  - checks the row sits in the External Display list after Controller Screen, and the cursor
    keeps its row;
  - checks all three vendor branches go through the setting, and the old direct evidence calls
    are gone;
  - checks every language translates the new strings.

  `bash tests/run.sh quick` in WSL `thimsy-aosp`: 403 tests OK, 49 skipped (missing host tools,
  as before).
- **Nano's i18n check** (`test/i18n_check.py`, every key in every language, placeholders): OK.
- **Compile against the real build:**
  - `AudioDeviceBroker.java` and `GammaosMonitorAudio.java` compiled with javac 17 against the
    `services.core` classpath and system modules from the last build plan, with
    `-XDstringConcat=inline` as Soong uses: no errors.
  - `gammaos-nano` rebuilt with ninja from the same plan: see "Nano compile" below.
- **Patch:** the changed files' sections were regenerated from the tree (`git diff` against the
  1.4.4 base). The whole patch then applies to the base in a scratch index and gives exactly the
  tree's 27 changed files.

### Nano compile

ninja on `out/combined-lineage_arm64_bgN.ninja` (the plan from the last analysis), target
`gammaos-nano`, ran 49 steps with exit 0:

- clang++ compiled `NanoMenuPS3Menu.cpp` under Nano's `-Wall -Werror`;
- `gammaos-nano` was linked, stripped and installed to `system/bin`;
- the 21 i18n files were installed to `system/etc/gammaos-nano/i18n/`.

## Not verified

Nothing was flashed or run on the Cube (the main session holds it for test 42, and a system image
write needs the owner's OK). The full Soong analysis and system image build haven't been run for
this change either, so errorprone and the Soong glob of the new Java file are untested.

To test on the Cube, after flashing a system image built from this patch, with a monitor that has
speakers connected:

1. `adb shell dumpsys audio | grep -i -A3 "devices:"` and `logcat -d | grep GammaHDMI` should show
   HDMI registered, and the sound playing on the monitor.
2. Set External Display > Audio Output to Built-in Speakers while music plays. Within a second,
   logcat should show "GammaHDMI monitor audio left unregistered" and "unregistered OUT/IN", and
   the sound moves to the Cube's speakers. `dumpsys media.audio_policy` should show no
   `AUDIO_DEVICE_OUT_HDMI` among the available output devices.
3. Unplug and replug the monitor, and reboot (unplug the monitor at the monitor end first, as
   always). The sound should stay on the speakers, and `getprop persist.gammaos.display.audio`
   should read `speakers`.
4. Set it back to Monitor. HDMI is registered again within a second, and the sound returns to the
   monitor.
