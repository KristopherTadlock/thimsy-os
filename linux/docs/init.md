# Init: from BusyBox init to Nano, and the first boot

The Linux image's init: BusyBox init, a short synchronous `rcS`, Nano, then
everything else in the background. Design: `docs/linux/IMPLEMENTATION.md`
sections E.5 and E.6 and B.5 (Paths, Raw block devices); decisions I39, I40,
I44. This page describes what was built (step G2e) and where it refines the
design.

## Files

```
linux/board/common/rootfs-overlay/
  etc/inittab                      BusyBox init's table
  etc/init.d/rcS                   sysinit: runs S[0-9][0-9]* in order
  etc/init.d/S00base               mounts, the boot disk, /storage, the first-boot tree
  etc/init.d/S01props              thimsy-prop boot, then the device family
  etc/init.d/S02padd               thimsy-padd in the background
  etc/init.d/S03modules            the family's modules and its boot hook
  etc/init.d/rcD                   the deferred runner: D[0-9][0-9]* start, or stop in reverse
  etc/init.d/deferred/D05family    the family's hook after the first frame
  etc/init.d/deferred/D10cards     coldplug: re-send "add" for block devices
  etc/init.d/deferred/D20sound     alsactl restore (and store on stop)
  etc/init.d/deferred/D30net       loopback, host name, Wi-Fi if it was on
  etc/init.d/deferred/D35bluetooth D-Bus and bluetoothd, if the device has Bluetooth and it was on
  etc/init.d/deferred/D40time      BusyBox ntpd
  etc/init.d/deferred/D50ssh       dropbear (dev images; release: opt-in, keys only)
  etc/init.d/deferred/D60firstboot the games partition on the boot disk
  etc/init.d/rcK                   shutdown
  etc/thimsy/init.conf             tunables (shell assignments)
  usr/share/thimsy/init/lib.sh     the library every script sources
linux/board/rk3566/common/
  rootfs-overlay/usr/share/thimsy/family/rk3566/hook
  README.md                        governors, sleep, ALSA notes
linux/board/common/post-build.sh   runner: post-build.d/* by name (common and --family=)
linux/board/common/post-image.sh   runner: post-image.d/* likewise (parts are G8's)
linux/board/common/post-build.d/20-build-props.sh  /usr/share/thimsy/build.props
linux/board/common/post-build.d/30-permissions.sh  modes, mount points, links, dev getty
linux/tools/check_rcs.py           static checks
linux/tests/test_rcs.py            static and fake-root tests (rcs_fake_tool.py: the fakes)
```

## Boot sequence

```
kernel -> /init (FlipBase's: proc, sys, devtmpfs, splash) -> busybox init
  ::sysinit:   rcS -> S00base, S01props, S02padd, S03modules     (synchronous)
  ::respawn:   /usr/libexec/thimsy/session  (thimsy-session: starts Nano at once)
               ... Nano sets sys.thimsy.nano.first_frame=1 ...
               session runs "rcD start" in the background:
               D05family, D10cards, D20sound, D30net, D35bluetooth, D40time, D50ssh,
               D60firstboot
  ::shutdown:  rcK
```

`rcS` writes `thimsy: rcS done` to `/dev/kmsg` when it ends (the QEMU test's
first marker, E.9). Each stage leaves a breadcrumb in `/run/thimsy/boot/`
(`s00-mounts`, `s00-done`, `s01-done`, `s03-done`, `rcs-done`,
`deferred-start`, `deferred-done`) holding `/proc/uptime` at that moment,
read with shell builtins (no fork). The log is `/run/thimsy/log/boot.log`;
warnings also go to `/dev/kmsg`.

### The budget

`rcS` is on the path to the first frame; plan 9.3 keeps it near 0.1 s
(FlipBase's is 0.06 s). So:

- the library uses builtins only (`read`, `printf`, `case`, `[`), and is
  sourced; family hooks are sourced too;
- the forks are: one process per S script, the mounts (`/run`, `/tmp`,
  `/dev/shm`, `/dev/pts`, `/storage`), one `mkdir -p`, `thimsy-prop boot`,
  `thimsy-padd &`; the first boot adds a few `mkdir`, `ln` and `chmod`;
- `check_rcs.py` refuses, on the synchronous path (rcS, S*, lib.sh, family
  hooks), every program in its `FORBIDDEN_SYNC` list: network and time
  daemons, SSH, D-Bus, Bluetooth, `alsactl`, `sleep`, `mdev`/`udevadm`,
  `fsck`/`mkfs`/partitioning tools, `find`, `ldconfig`, `depmod`, `sync`,
  `wait`. One exception is marked in place with `# rcs-allow:`: `e2fsck -p`
  on the data partition, run only when its mount has failed.

## The boot disk (decision I40)

`S00base` resolves the disk the root came from, in this order, and records
it in `/run/thimsy/bootdisk` (`BOOT_DISK`, `ROOT_PART`, `DATA_PART`,
`BOOT_HOW`, `BOOT_EMMC`) for `D60firstboot` and `thimsy-session`:

1. **The mounted root** (`/proc/self/mountinfo`, the `major:minor` of `/`),
   matched against `/sys/class/block/*/dev`. This is what the kernel actually
   mounted.
2. `root=PARTUUID=` from `/proc/cmdline`, matched case-insensitively against
   `PARTUUID=` in `/sys/class/block/*/uevent`. **If two partitions carry the
   UUID, nothing is guessed.**
3. `root=/dev/NAME`.

The disk is the `/sys/class/block/<disk>` directory that holds the
partition's directory. The design put `root=PARTUUID=` first; the mounted
root is first here because two cards written from the same image have the
same PARTUUIDs (the ARC has two slots), and the kernel's choice is the
truth. The tests cover both cards being clones.

Then, **on that disk only**: the partition whose GPT name is `data`
(`PARTNAME=` in its uevent) is mounted at `/storage` (ext4,
`noatime,nosuid,nodev`). A second card's `data` partition and the eMMC's are
never looked at. If the boot disk is an eMMC (`device/type` is `MMC`), none
of its partitions is used. If there is no data partition, or it won't mount
even after `e2fsck -p`, `/storage` is a tmpfs for this boot and a warning
goes to the kernel log.

`/dev/block/by-name` is never created (IMPLEMENTATION B.5): no script
names it, `check_rcs.py` fails on a script that does, and
`30-permissions.sh` fails the build if the root has it.

## The first-boot tree (B.5)

Made by `S00base` when `/storage/.config/thimsy/.tree-v1` is missing; the
stamp is written last, and every step can be repeated, so a power cut just
repeats it. Existing files are never removed.

```
/storage/.config/thimsy/
  data/                 /data links here (post-build.d/10-android-links.sh, G9)
    system/ local/tmp/ (1777) data/ misc/ gammaos_ota/
    user/0 -> ../data   (as on Android: /data/user/0 is /data/data)
    media/0 -> /storage/emulated/0
  ssh/ (0700)           dropbear's host key
  home/root/.ssh/       root's home: bind-mounted on /root by S00base (below)
  localtime -> /usr/share/zoneinfo/Etc/UTC   (/etc/localtime links here; Nano rewrites it)
/storage/.config/retroarch/
/storage/thimsy/        the card's thimsy/ folder (install.json)
/storage/emulated/0/
/storage/self/primary -> /storage/emulated/0
```

A later release that needs more bumps the stamp to `.tree-v2`.

### Root's home

`/root` in the read-only root is an empty 0700 directory. `S00base` binds
`/storage/.config/thimsy/home/root` over it only where something can log in
as root: dev builds (`ro.thimsy.build.type=dev`), and release builds once
the user has turned SSH on (`persist.thimsy.ssh.enabled=1`; dropbear then
runs key-only). A release build with SSH off has no root login at all and
keeps the empty `/root`. The home is on the data partition (ext4, so Unix
modes hold), never on the games exFAT. At every boot, before binding, S00base
refuses a `home`, `home/root`, `.ssh` or `authorized_keys` that is a
symbolic link (it warns and leaves `/root` unbound), and sets owner 0:0 and
modes 0700 on the home and `.ssh` and 0600 on `authorized_keys`: dropbear
refuses keys that others can write, and the card can be edited on a PC.

## Properties and the family

`S01props` runs `thimsy-prop boot`, which writes `/run/thimsy/props/ro/<key>`
from `/usr/share/thimsy/device.json`, `/storage/thimsy/install.json` and
`/usr/share/thimsy/build.props` (B.3; the CLI is libthimsy's, G1a). The
device family is `ro.thimsy.family`, else the only directory under
`/usr/share/thimsy/family/`; it is written to `/run/thimsy/family`.
Then each `persist.*` line of `/usr/share/thimsy/persist-defaults.props`
(thimsy-data's copy of `mods/nano-theme.props`: the theme settings Android's
build.prop sets, OA-1) whose key `persist.props` does not have is set with
`thimsy-prop set`, so Nano starts with Android's defaults and a later choice
is kept. Once every key is there, this runs no program.

`build.props` comes from `post-build.d/20-build-props.sh`:
`ro.thimsy.build.{version,commit,target,type,date,date.utc,arch}`. `type` is
`release` when the defconfig has no root login and no serial getty (the
release fragment, I37), else `dev`. The date is `SOURCE_DATE_EPOCH`'s.

### Family hooks

`/usr/share/thimsy/family/<family>/hook` is a sh file with one entry point,
`ACTION` from `THIMSY_HOOK` (when sourced) or `$1` (when run):

| Action | Caller | RK3566 |
|---|---|---|
| `boot` | `S03modules` (sourced) | CPU and devfreq governors `performance` |
| `deferred` | `D05family` (sourced) | `schedutil`, `simple_ondemand` |
| `sound-defaults` | `D20sound`, first boot only | `Playback Path` = `SPK_HP` |
| `sleep-pre`, `sleep-post` | `thimsy-session` (run) | `deep` into `mem_sleep` |
| `fixedperf-on`, `fixedperf-off` | Nano's control keys (run; `sys.gammaos.nano.fixedperf=1`/`0`, G9-13) | as `boot`, then as `deferred` |

A hook must not call `exit`, and skips a governor file that is missing or
refuses the value. Each family's files sit under `family/<family>/`, so a
tier root can carry several families. The family's overlay is in its
hardware pack's `BR2_ROOTFS_OVERLAY` (`hw-<family>.fragment`), and reaches
each card's root when the card is put together (`post-image.d/lib/card.sh`).

`ro.thimsy.family` and `ro.thimsy.modules` are written by `thimsy-prop boot`
from `device.json`'s `family` and `modules` (the latter space-separated;
`S03modules` loads them).

## Deferred services

`thimsy-session` runs `rcD start` in the background after the first frame;
`rcK` runs `rcD stop` (reverse order). Persistent switches are read from
`/storage/.config/thimsy/persist.props` (libthimsy's file) with builtins:

| Script | Does | Switch |
|---|---|---|
| `D05family` | family hook `deferred` | |
| `D10cards` | writes `add` to every non-eMMC block device's `uevent`, so `thimsy-session`, already listening, sees the drives that were there at boot and applies its own policy (fsck, `/storage/<VOLID>`, `volumes.json`). One policy, in the session (F.1); not a second one in shell | |
| `D20sound` | waits up to 5 s (`THIMSY_SOUND_WAIT_S`) for `card<N>`, N = device.json's `audio.card_index` (`ro.thimsy.device.audio.card_index`, else 0, the session's `ALSA_CARD`); warns when that card's id is not `audio.card`'s (letters and digits kept) or that card has another index; `alsactl -f /storage/.config/thimsy/asound.state restore`, or the family's `sound-defaults`; stop: `store` | |
| `D30net` | `lo` up; host name; if Wi-Fi was on: `wpa_supplicant -B` on the first `wlan*` (waited for up to 5 s; control socket `/run/wpa_supplicant`, config `/storage/.config/thimsy/wpa_supplicant.conf`, seeded 0600), `udhcpc -f -T 1 -A 2` in a background job. Stop (rcK, and Nano's Wi-Fi off): TERM `udhcpc`, then `wpa_supplicant`. Nano's Wi-Fi switch sets the key and runs this script (start or stop) on its actions worker, through the same runner as `D35bluetooth` (a session of its own, none of Nano's descriptors); its Wi-Fi screens then talk to `wpa_supplicant`'s control socket | `persist.thimsy.wifi.enabled=1` |
| `D35bluetooth` | only when device.json says `"bluetooth": true` (`ro.thimsy.device.bluetooth`) and Bluetooth was on: waits up to 5 s (`THIMSY_BT_WAIT_S`) for a controller (`/sys/class/bluetooth/hci*`); `/storage/.config/thimsy/bluetooth` (0700, where `/var/lib/bluetooth` links: pairings survive); `dbus-uuidgen --ensure`, `dbus-daemon --system --nofork` in a background job (waits up to 3 s for `/run/dbus/system_bus_socket`), then `bluetoothd -n` in a background job, pids in `/run/dbus-daemon.pid` and `/run/bluetoothd.pid`. Without `/sys/class/bluetooth` (no Bluetooth in the kernel; S03modules has loaded every module by then) it does not wait. Exit 3 when it was on and the device has no Bluetooth (device.json, the kernel, or no controller): Nano then writes the key back to 0, so later boots neither wait nor warn; exit 1 when a daemon could not start. Stop (rcK, and Nano's Bluetooth off): TERM `bluetoothd`, then the bus, each waited for up to 2 s, then KILL. A pidfile is trusted only while its pid runs and is that daemon (`/proc/PID/comm` is `bluetoothd` or `dbus-daemon`; start waits up to 1 s for `bluetoothd` to exec): one left by a daemon that died is removed, and the process that has its pid now is neither signalled nor taken for the daemon. One start or stop at a time: each holds `/run/thimsy/bluetooth.lock` (BusyBox `flock` on fd 9; the daemons start without fd 9), and start reads the key again once it holds it, so rcD's boot start and Nano's switch cannot interleave. Nano's Bluetooth switch (the legacy screen's `setBluetoothEnabled` on the actions worker, and the PS3 wizard's Turn On/Off through `Network::setBtRadio` on its own thread) sets the key and runs this script (start, then `bluetoothctl power on`, tried up to 5 times 500 ms apart while it fails at once, as bluetoothd may not have registered the controller yet; `power off`, then stop). Nano runs no `bluetoothctl` for its state unless the key is 1, the device has Bluetooth and the bus socket is there. Nano's runner starts it in a session of its own with none of Nano's descriptors above stderr, so the daemons are not in Nano's process group and hold none of its evdev fds | `persist.thimsy.bluetooth.enabled=1` (off by default) |
| `D40time` | `ntpd -n -p ...` in the background (it retries until the network is up) | `persist.thimsy.time.ntp=0` turns it off |
| `D50ssh` | `dropbear -r <host key on /storage>`; dev images by default; release images only when turned on, and then `-s` (no passwords) | `persist.thimsy.ssh.enabled` |
| `D60firstboot` | the games partition, below | |

## The games partition (E.6, decisions I39 and I44)

The written image has GPT entries 1 to 4 (`uboot`, `boot`, `root`, `data`)
and leaves the rest of the card unallocated. `D60firstboot`, on the boot
disk only and never on an eMMC:

0. if the backup GPT sits at the image's end rather than the card's (more
   than 1 MiB short), `sfdisk --relocate gpt-bak-std`;
1. if there is no entry 5: creates it with `sfdisk --force -N 5`, starting
   2 MiB (`GAP_MIB`) after the first 1 MiB boundary after the end of the last
   entry (so a spare area between entries is never used; 2410 MiB on the
   cards), and running to the end of the card, typed **Linux reserved**,
   named `games`; not if less than 64 MiB is free. Unless the old library is
   there (re-adoption, below);
   `--force` because the disk is in use (root and `/storage` are mounted):
   sfdisk refuses otherwise, and with `--force` it still adds the entry to
   the kernel's table (BLKPG);
2. `mkfs.exfat -L THIMSY` on it, then `sync`;
3. `sfdisk --part-type <disk> 5 EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` (basic
   data), `sync`, and `change` into the partition's `uevent` so the session
   looks at it again.

The state is read from the GPT (`sfdisk --dump`) on every run:

| Entry 5 | Action |
|---|---|
| missing, a THIMSY exFAT at the start | step 0, then re-adoption (below) |
| missing | steps 0 to 3 |
| Linux reserved, named `games` | ours, not finished: steps 2 and 3 (formatting again is safe: nothing can be on it) |
| basic data | done |
| anything else | the user's: left alone |

So at no moment is there a basic-data entry without a complete filesystem,
and a power cut at any step leaves a state the next boot finishes. The
tests cut the power before and after each of the four steps, and in the
middle of the format, and check both. A cut inside one sfdisk write can
tear the primary GPT (libfdisk writes the backup first, then the primary
entries, then the primary header); U-Boot then reads the backup, and the
kernel does too because the command line has `gpt` (tools/fit.py,
run-qemu.sh). D60 ignores TERM while it holds `/run/thimsy/firstboot.lock`,
and rcK waits up to 10 s for the lock before it signals everyone, so a
shutdown never cuts the write (review finding device-safety-1).

### Re-adoption (review finding device-safety-2)

An entry 5 that is `missing` is also what a card looks like when an image is
written again over a used Thimsy card (a repair, or an update before there
is an updater). The written image ends 1 MiB after `data` (genimage.cfg's
`size = 2409M`; its backup GPT is its last 33 sectors) and games starts
1 MiB after that, so a re-write leaves the old library's exFAT whole, boot
region (sectors 0 to 23: main and backup boot regions) and all.
`tools/gpt_check.py` holds both sides: a written image must end exactly
1 MiB after its last entry, and a booted card's games entry may not start
before 2 MiB after it.

Before step 1, D60 reads the boot sector at the computed start (`dd`, `od`).
If it says `EXFAT   ` at byte 3, ends in 55 AA, has sane shifts and lengths,
and the first cluster of the root directory has the allocation-bitmap (0x81)
and up-case-table (0x82) entries (a complete exFAT), D60 creates entry 5
over the filesystem's extent (`VolumeLength` shifted by
`BytesPerSectorShift`, clamped to the card's last usable sector), typed
**basic data** at once and named `games`, logs it with the volume's label,
and runs no `mkfs`. The label does not matter (review finding
linux-host-impls-4): a library the user renamed or cleared on a PC, or one
under 64 MiB, is adopted as it is. That is one sfdisk write: a power cut
leaves either no entry 5 (adopted at the next boot) or the adopted entry,
and the filesystem was complete before the entry existed, so a basic-data
entry still never lacks one. When `dd` or `od` fails, D60 creates nothing,
warns (`could not read ...: trying again at the next boot`) and exits with
an error: a read error never formats a library. Anything else at the start
(a boot sector without its root directory, NTFS, zeros) is formatted as at
a first boot, so a second-hand card still gets a fresh library. Not
covered: a format that was cut after the root directory's entries were
written and then re-written over (its half-made exFAT is adopted; the
session's fsck then sees it).

The partition is the alpha's library drive by default (I44): the
session's volume policy accepts the boot disk's `games` partition and no
other boot-disk partition (F.1); Nano chooses it.

## Shutdown

`rcK`: the game's process group (`sys.thimsy.game.pid`) gets TERM, 3 s, KILL;
`rcD stop`; every other process TERM, up to 5 s, KILL (FlipBase's lesson:
a frontend needs time to exit cleanly, and zombies keep files busy); `sync`;
drives under `/storage` then `/storage` unmounted, a mount that will not go
remounted read-only. BusyBox init then signals what is left, syncs and
reboots.

## Test seams

Every path the scripts touch is `$R/...`, with `R=$THIMSY_ROOT` (empty on a
device), and every program they run is `"$TOOL"` from the library, replaced
by `$THIMSY_TOOLDIR/<name>` when that file exists. `rcK` sends no signal
when `THIMSY_ROOT` is set. Redirections to `/dev/null` stay real.

`linux/tests/test_rcs.py` builds a fake root: `/proc/cmdline`,
`/proc/self/mountinfo`, `/proc/mounts`, `/sys/class/block` with an eMMC
(`device/type` `MMC`, Android's table with an entry named `data`), the boot
card in slot 1 and a second card in slot 2 that also has a `data`
partition (optionally a clone with the same PARTUUIDs). Disks are JSON
files under `$R/dev/` that the fake `sfdisk` and `mkfs.exfat`
(`rcs_fake_tool.py`) read and write; the fake kernel view is regenerated
from them as the kernel would at boot or on BLKPG. A disk may also have
bytes, a sparse file `$R/dev/<disk>.img` that the fake `dd` reads for
`if=$R/dev/<disk>` and the fake `mkfs.exfat` writes a small exFAT into (the
re-adoption tests). Other tools are small sh fakes that log their
arguments. A power cut makes every later tool call
fail without effect; the test then "reboots" (empties `/run`, re-reads the
tables) and runs the script again.

Run:

```
py -3.12 -I -m unittest discover -s linux/tests -t linux -p test_rcs.py      (Windows)
python3 -I -m unittest discover -s tests -t . -p test_rcs.py                 (WSL, in linux/)
py -3.12 -I linux/tools/check_rcs.py
```

The scripts run under BusyBox sh when it is installed, else `/bin/sh` (dash
on Ubuntu), or `THIMSY_SH`. On Windows they run under Git for Windows'
`sh.exe`, which cannot make symbolic links here, so the S00base, rcS and
30-permissions tests skip there; everything else runs.

## Not verified

Nothing here has run on a device or in QEMU. In particular: that `sfdisk
--force -N 5` on the mounted boot card adds the entry through BLKPG on the
pinned util-linux; that `PARTUUID=` appears in partition uevents on the
pinned kernel (only the fallback after the mounted root needs it); the
RK817 control names; the time `rcS` takes. The QEMU boot test (E.9) is the
first check.
