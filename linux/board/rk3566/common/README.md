# RK3566 family: per-family init files

Owned by step G2e (docs/linux/IMPLEMENTATION.md, section G). The general
init design is in `linux/docs/init.md`.

## What is here

| Path | Installed as | What |
|---|---|---|
| `rootfs-overlay/usr/share/thimsy/family/rk3566/hook` | `/usr/share/thimsy/family/rk3566/hook` | the family's hook: `boot`, `deferred`, `sound-defaults`, `sleep-pre`, `sleep-post` |

`rootfs-overlay/` must be listed in `BR2_ROOTFS_OVERLAY` after
`board/common/rootfs-overlay` by every tier defconfig that can run on an
RK3566 (today `thimsy_v8_2` and `thimsy_v8_0`; the fragments are G2a's).
Each family's files sit under `family/<family>/`, so a tier root can carry
several families without collisions; `S01props` picks one at boot
(`ro.thimsy.family`, or the only family present).

## Governors

`boot` sets every cpufreq policy and every devfreq device to `performance`
until Nano's first frame (plan 9.3, "performance governor until the menu is
up"); `deferred` (run by `D05family` after the first frame) sets
`schedutil` and `simple_ondemand`. A governor the kernel lacks is skipped
silently. Per-game performance modes (`fixedperf`, IMPLEMENTATION B.2) are
`thimsy-session`'s, not the hook's.

## Sleep

`thimsy-session` writes `mem_sleep` and `state` itself (IMPLEMENTATION F.1);
the hook only prepares the family: `sleep-pre` asks for `deep`. Deep sleep on
RK3566 needs the vendor BL31's suspend mode and FlipBase's pm-config driver
(decision 18; Phase 0 measures it), so on a kernel without them the write is
refused and `s2idle` stays. Nothing is needed after resume yet (`sleep-post`).

## ALSA notes

- The ARCs' codec is the RK817's (`rk817-codec`), one card (`card0`,
  `rockchip,rk817-codec` in the device tree). Mainline names the output
  selector `Playback Path` with values including `OFF`, `SPK`, `HP`,
  `HP_NO_MIC`, `SPK_HP` *(from ROCKNIX's RK3566 quirks and the driver; check
  on the unit)*.
- No ALSA UCM profile is shipped for the alpha: Nano and RetroArch open
  `default` and the path is set once. ROCKNIX's UCM files are GPL-2.0 and
  would be imported, not copied by hand, if a later step needs them (plan
  3.2).
- The mixer state is saved by `D20sound stop` (from `rcK`) to
  `/storage/.config/thimsy/asound.state` and restored after the first frame;
  `sound-defaults` runs only when no state exists.
- Headphone detection is the codec driver's (jack GPIO in the device tree);
  `SPK_HP` lets it switch. If a unit plays through both, the fix is the
  device tree's jack description, not a script.
- Volume keys are `thimsy-session`'s (`sys.thimsy.hud.volume`), through
  `amixer` on the `Master` control *(guess: check the control name on the
  unit)*.
