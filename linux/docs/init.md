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
               D05family, D10cards, D20sound, D30net, D40time, D50ssh, D60firstboot
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
  home/root/.ssh/       bind-mounted on /root by S00base every boot
  localtime -> /usr/share/zoneinfo/Etc/UTC   (/etc/localtime links here; Nano rewrites it)
/storage/.config/retroarch/
/storage/thimsy/        the card's thimsy/ folder (install.json)
/storage/emulated/0/
/storage/self/primary -> /storage/emulated/0
```

A later release that needs more bumps the stamp to `.tree-v2`.

## Properties and the family

`S01props` runs `thimsy-prop boot`, which writes `/run/thimsy/props/ro/<key>`
from `/usr/share/thimsy/device.json`, `/storage/thimsy/install.json` and
`/usr/share/thimsy/build.props` (B.3; the CLI is libthimsy's, G1a). The
device family is `ro.thimsy.family`, else the only directory under
`/usr/share/thimsy/family/`; it is written to `/run/thimsy/family`.

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

A hook must not call `exit`. Each family's files sit under
`family/<family>/`, so a tier root can carry several families. The
family's overlay must be in `BR2_ROOTFS_OVERLAY` (G2a's fragments).

## Deferred services

`thimsy-session` runs `rcD start` in the background after the first frame;
`rcK` runs `rcD stop` (reverse order). Persistent switches are read from
`/storage/.config/thimsy/persist.props` (libthimsy's file) with builtins:

| Script | Does | Switch |
|---|---|---|
| `D05family` | family hook `deferred` | |
| `D10cards` | writes `add` to every non-eMMC block device's `uevent`, so `thimsy-session`, already listening, sees the drives that were there at boot and applies its own policy (fsck, `/storage/<VOLID>`, `volumes.json`). One policy, in the session (F.1); not a second one in shell | |
| `D20sound` | waits up to 5 s for `card0`; `alsactl -f /storage/.config/thimsy/asound.state restore`, or the family's `sound-defaults`; stop: `store` | |
| `D30net` | `lo` up; host name; if Wi-Fi was on: `wpa_supplicant -B` on the first `wlan*` (control socket `/run/wpa_supplicant`, config `/storage/.config/thimsy/wpa_supplicant.conf`, seeded 0600), `udhcpc -f -T 1 -A 2` in a background job | `persist.thimsy.wifi.enabled=1` |
| `D40time` | `ntpd -n -p ...` in the background (it retries until the network is up) | `persist.thimsy.time.ntp=0` turns it off |
| `D50ssh` | `dropbear -r <host key on /storage>`; dev images by default; release images only when turned on, and then `-s` (no passwords) | `persist.thimsy.ssh.enabled` |
| `D60firstboot` | the games partition, below | |

## The games partition (E.6, decisions I39 and I44)

The written image has GPT entries 1 to 4 (`uboot`, `boot`, `root`, `data`)
and leaves the rest of the card unallocated. `D60firstboot`, on the boot
disk only and never on an eMMC:

0. if the backup GPT sits at the image's end rather than the card's (more
   than 1 MiB short), `sfdisk --relocate gpt-bak-std`;
1. if there is no entry 5: creates it with `sfdisk --force -N 5`, starting at
   the first 1 MiB boundary after the end of the last entry (so a spare area
   between entries is never used) and running to the end of the card, typed
   **Linux reserved**, named `games`; not if less than 64 MiB is free.
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
| missing | steps 0 to 3 |
| Linux reserved, named `games` | ours, not finished: steps 2 and 3 (formatting again is safe: nothing can be on it) |
| basic data | done |
| anything else | the user's: left alone |

So at no moment is there a basic-data entry without a complete filesystem,
and a power cut at any step leaves a state the next boot finishes. The
tests cut the power before and after each of the four steps, and in the
middle of the format, and check both.

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
from them as the kernel would at boot or on BLKPG. Other tools are small sh
fakes that log their arguments. A power cut makes every later tool call
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
