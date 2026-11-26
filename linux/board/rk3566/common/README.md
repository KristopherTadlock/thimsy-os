# RK3566 family: per-family init files

Owned by step G2e (docs/linux/IMPLEMENTATION.md, section G). The general
init design is in `linux/docs/init.md`.

## What is here

| Path | Installed as | What |
|---|---|---|
| `rootfs-overlay/usr/share/thimsy/family/rk3566/hook` | `/usr/share/thimsy/family/rk3566/hook` | the family's hook: `boot`, `deferred`, `sound-defaults`, `sleep-pre`, `sleep-post`, `fixedperf-on`, `fixedperf-off` |

`rootfs-overlay/` is part of the RK3566 hardware pack: `hw-rk3566.fragment`
lists it in `BR2_ROOTFS_OVERLAY` after `board/common/rootfs-overlay`, so the
`thimsy_rk3566_hw` tree carries it, not the tier roots. When a card is put
together (`board/common/post-image.d/lib/card.sh`, G8), each device's root
is the tier root, then the family's GPU pack unpacked over it (G8-2, gfix-8),
then the hardware pack's modules, firmware and this overlay, then the
device's `device.json`. Each family's files sit under `family/<family>/`, so
a root could carry several families without collisions; `S01props` picks
one at boot (`ro.thimsy.family`, which `thimsy-prop boot` writes from
`device.json`'s `family`, or the only family present) and writes it to
`/run/thimsy/family`.

## Governors

`boot` sets every cpufreq policy and every devfreq device to `performance`
until Nano's first frame (plan 9.3, "performance governor until the menu is
up"); `deferred` (run by `D05family` after the first frame) sets
`schedutil` and `simple_ondemand`. A governor file that is missing or
refuses the value is skipped silently. A game's fixed performance mode
(`sys.gammaos.nano.fixedperf=1`/`0`, IMPLEMENTATION B.2) is Nano's control
keys running this hook as `sh hook fixedperf-on` / `fixedperf-off` (G9-13):
the same governors as `boot`, then as `deferred`.

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
  `default` and the path is set once. `default` is the common overlay's
  `/etc/asound.conf`: plug over a dmix on card `ALSA_CARD` (the session sets
  it from device.json's `audio.card_index`), so Nano's streams and the game's
  mix; its rate (48 kHz), period (512) and buffer (2048) are unchecked on a
  unit. ROCKNIX's UCM files are GPL-2.0 and
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
