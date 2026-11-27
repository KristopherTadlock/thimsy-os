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

- The ARCs' codec is the RK817's (`rk817-codec`), on the `simple-audio-card`
  named `rk817_ext` (ALSA id `rk817ext`). It is most likely **card 1**, not
  card 0: the ARC's device tree includes `rk3566-anbernic-rgxx3.dtsi`, which
  turns on HDMI and `hdmi_sound` (the ARC does not turn them off), so the HDMI
  audio card is there too and usually registers first, and ROCKNIX sets
  `ALSA_PRIMARY_CARD=1` for every RK3566 device (`050-audio_path`). Both ARC
  `device.json` files say `audio.card_index` 1 and `audio.card` `rk817_ext`;
  `D20sound` waits for that index and warns in the kernel log when the card
  there is not `rk817ext` or when `rk817ext` has another index. **Phase 0
  check:** `cat /proc/asound/cards` on each unit (expected: `0 [hdmisound]`,
  `1 [rk817ext]`); if the order differs, fix `audio.card_index` (the session's
  `ALSA_CARD`, `/etc/asound.conf` and `D20sound` all follow it). Mainline names
  the output selector `Playback Path` with values including `OFF`, `SPK`,
  `HP`, `HP_NO_MIC`, `SPK_HP` *(from ROCKNIX's RK3566 quirks and the driver;
  check on the unit)*.
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

## Idmapped mounts

- thimsy-session mounts an ext4 drive made on a PC with an idmapped mount, so
  its files appear owned by `nano` (decision OA-29). The kernel needs
  `CONFIG_USER_NS=y` (under `CONFIG_NAMESPACES=y`); ext4 takes idmapped
  mounts from 5.12 on, with no option of its own. `kernel-critical.conf`
  asks every family for both, so the conformance check fails an import
  without them.
- ROCKNIX's RK3566 `linux.aarch64.conf` (branch `next`, 7.3.5, read
  2026-11-27) has `CONFIG_NAMESPACES=y` and `# CONFIG_USER_NS is not set`.
  So `board/rk3566/linux.fragment` (Phase 2a, not written yet) must set
  `CONFIG_USER_NS=y`.

## Bluetooth notes

- Userland: BlueZ, the D-Bus system bus and `D35bluetooth` are only in the
  roots of devices whose `device.json` says `"bluetooth": true` (decision
  OA-15): the tier builds them (`configs/fragments/bluetooth.fragment`),
  `post-build.d/40-bt-pack.sh` moves them into `bluetooth.erofs`, and card
  assembly unpacks it for those devices only. So the ARC-D's card has them
  and the ARC-S's has not. `D35bluetooth` starts them only when the user
  turned Bluetooth on. It exits 3 at once when `/sys/class/bluetooth` is missing (no
  Bluetooth in the kernel) and after `THIMSY_BT_WAIT_S` when no `hci*`
  appears; Nano then turns its switch back off.
- Kernel: the kernel config is ROCKNIX's RK3566 `linux.aarch64.conf`, imported
  (`hw-rk3566.fragment`'s `gpl/generated/rk3566/linux/config`), not a Thimsy
  fragment. At the ROCKNIX revision read for this step it has `CONFIG_BT=m`,
  `CONFIG_BT_HCIUART=m` with `CONFIG_BT_HCIUART_SERDEV=y` and
  `CONFIG_BT_HCIUART_RTL=y`, `CONFIG_BT_RTL=m`, `CONFIG_BT_HIDP=m` and
  `CONFIG_UHID=y` (HoG pads need uhid). The image has no uevent-driven module
  loading (no mdev or udev), so a module-built UART driver is loaded only if
  `device.json`'s `modules` names it; the ARC-D's lists none yet.
  `bluetooth` and `btrtl` come with `hci_uart` (module dependencies), and
  `hidp` is loaded on demand by the kernel's socket module request.
- **Phase 0 (b) check** (with plan open question 25): after the import, the
  generated config still has the options above (`grep -E
  '^CONFIG_(BT|BT_HCIUART|BT_HCIUART_SERDEV|BT_HCIUART_RTL|BT_RTL|BT_HIDP|UHID)='`);
  the ARC-D's device tree gets a `bluetooth` node under its UART (mainline's
  dtsi has none), `device.json`'s `modules` gains `hci_uart`, the RTL
  firmware and config (`rtl_bt/`) pass the firmware policy, and
  `/sys/class/bluetooth/hci0` appears before `D35bluetooth` runs. Else the
  ARC-D's v1 ships without Bluetooth (`"bluetooth": false`).
