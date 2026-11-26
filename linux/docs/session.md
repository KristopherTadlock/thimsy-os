# thimsy-session

`thimsy-session` is the Linux image's session manager (IMPLEMENTATION F.1). BusyBox init starts it
from `/etc/inittab` and starts it again if it dies:

```
::respawn:/usr/libexec/thimsy/session
```

It starts and supervises Nano, owns drives and volumes, reads the power, volume and lid keys and the
quit hotkey, puts the device to sleep, starts the deferred services after Nano's first frame, and
carries out Nano's requests. It is C++17 in `linux/src/thimsy-session/`: pure cores under `src/core/`
(tested on the host with a fake clock) and a thin I/O layer under `src/io/`, wired by one epoll loop
in `src/Session.cpp`. MIT.

Nano talks to it through the config store (libthimsy, IMPLEMENTATION B.3) and one file,
`/run/thimsy/volumes.json`. **This document is the contract for both**: Nano's Linux `Volumes` and
`Power` implementations (G7) read and write exactly what is described here.

## The contract with Nano

### `/run/thimsy/volumes.json`

Written atomically (temp file and rename; readers never see half a file) whenever anything in it
changes, and once at start (with an empty list), so it always exists while the session runs. After
each write the session sets `sys.thimsy.volumes.serial` to a new, larger number: watch that key
(or the file) and re-read the file when it changes.

```json
{
  "version": 1,
  "volumes": [
    {
      "id": "9602-C755",
      "state": "mounted",
      "path": "/storage/9602-C755",
      "media_rw": "/mnt/media_rw/9602-C755",
      "dev": "/dev/mmcblk1p5",
      "disk": "mmcblk1",
      "partition": 5,
      "fs": "exfat",
      "label": "THIMSY",
      "kind": "sd",
      "boot_disk": true,
      "read_only": false,
      "size_bytes": 60129542144
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `version` | `1`. A reader refuses a larger version it does not know. |
| `volumes` | One entry per partition (or partitionless disk) that is a drive by the policy below, sorted by kernel name. Partitions that are not drives (the eMMC, the boot card's system partitions, a half-made games partition) never appear. |
| `id` | The drive's id, `<VOLID>` of plan 7.3: the FAT or exFAT serial as `XXXX-XXXX` (upper case, as vold and blkid print it), or an ext4 UUID (lower case, dashed). Empty when the state is `unsupported` or `no_id`. |
| `state` | `checking` (the filesystem check runs), `mounted`, `ejected` (unmounted by an `eject` request; it stays listed until the card is removed), `check_failed`, `mount_failed`, `unsupported` (no FAT, exFAT or ext4 filesystem), `no_id` (an all-zero or missing serial: plan 7.3 forbids using it), `duplicate` (another drive has the same id: neither is used until one is removed, plan 7.3 and decision 17). Only `mounted` drives may be used. |
| `path` | The mount point, `/storage/<id>`. It is a permanent directory on the data partition, made immutable (`chattr +i`) so nothing can be created in it while the drive is out: check `state`, and before writing compare the `st_dev` of `path` with `/storage`'s (plan 7.3). |
| `media_rw` | The root-side alias, `/mnt/media_rw/<id>`, a symbolic link to `path` while mounted (B.5). |
| `dev`, `disk`, `partition` | The block device, its disk, and its partition table entry (0 for a partitionless disk). |
| `fs` | `vfat`, `exfat` or `ext4` (ext2 and ext3 show as `ext4`). |
| `label` | The filesystem label (exFAT's volume label entry; FAT's root-directory label, else the boot sector's; ext's). |
| `kind` | `sd`, `usb`, `virtio` or `other`. |
| `boot_disk` | On the card Thimsy booted from. The boot card's `games` partition is the default library drive (decision I44). |
| `read_only` | The device is read-only (a card's lock switch): mounted `ro`, never checked. |
| `size_bytes` | The partition's size. |
| `error` | Present when there is something to say: why a check or mount failed, why a drive is a duplicate, that an eject failed because the drive is busy. |

### Requests: `sys.thimsy.session.request` and `sys.thimsy.session.done`

Nano writes a request into `sys.thimsy.session.request`; the session answers in
`sys.thimsy.session.done`.

```
request   [<seq>:]<verb>[:<arg>]
done      <seq>:ok
          <seq>:error:<why>
```

`<seq>` is a decimal number that Nano increments per request. The session acts on every write of the
key (it watches the file, so writing the same text twice is two requests), except that a numbered
request whose `<seq>` equals the last one handled is ignored. Without a `<seq>` the answer starts with
the verb instead (`quit-game:error:no game`). Values follow the store's limit of 91 bytes.

| Verb | Does | Answer |
|---|---|---|
| `sleep` | Sleep now (below). | `ok`, written before the device sleeps |
| `poweroff`, `reboot` | Stops restarting anything and runs BusyBox `poweroff` or `reboot` (init then runs `rcK`). | `ok`, written first |
| `quit-game` | The running game's process group (`sys.thimsy.game.pid`): SIGCONT and SIGTERM, then SIGKILL after 3 s. | `error:no game` when none runs |
| `eject:<id>` | Unmounts a mounted drive fully (`umount`, never a remount: plan 7.3); `state` becomes `ejected`. | `error:...` when there is no such mounted drive or it is busy |
| `scrape` | Starts `thimsy-nano-scrape` unless it runs (as `sys.gammaos.nano.scrape_run` does). | `ok` |
| `restart-nano` | Stops Nano and starts it again at once (the Android `park_restart` trigger's job). | `ok` |
| `brightness:<0-100>` | Sets the first backlight to that share of `max_brightness` (0 is off; anything above 0 is at least 1). | `ok`, or `error:...` without a backlight |

A malformed numbered request is answered `<seq>:error:<why>`; a malformed unnumbered one only logged.

### Other keys

| Key | Who writes it | Meaning |
|---|---|---|
| `sys.thimsy.nano.first_frame` | Nano | `1` once its first frame is on screen: the session starts the deferred services (once per boot) and notes when that Nano was first on screen (the launch-loop hold-back). The session sets it to `0` before each Nano start. |
| `sys.boot_completed` | Nano | `1` at the same moment (its control keys, G9-11): Nano gates sleep and its idle work on it. The session never writes it. |
| `sys.thimsy.game.pid` | Nano | The pid of the game it launched (the game is its process group leader), written when the game starts. The session stops, continues and quits that group; when Nano has exited and left the game running, the session clears the key once that game ends. |
| `sys.gammaos.nano.app_launched`, `sys.gammaos.nano.app_drawn` | Nano; the session | Android's framework sets them when an app starts and clears them when it returns to the home. On Linux Nano's launcher sets `app_launched=1` when it takes a launch (so its menu's exit path does not wait for it); the session sets both back to `0` when the game Nano left running ends, and before it starts Nano with no game running (a stale `1` would read as a game in front of Nano). |
| `sys.powerctl` | Nano (Android's key) | `shutdown[,reason]` or `reboot[,reason]`: as the `poweroff` and `reboot` requests. |
| `sys.gammaos.nano.scrape_run` | Nano | Any new value starts the scraper (decision I25), as `gammaos-nano.rc`'s trigger does. |
| `sys.thimsy.volumes.serial` | session | Bumped after each write of `volumes.json`. |
| `sys.thimsy.session.done` | session | Answers to requests, above. |
| `sys.thimsy.hud.volume` | session | The new volume level (0 to 100) after a volume key, for Nano's slider. |
| `sys.thimsy.key.power_long` | session | A counter, bumped on each long power-key press while Nano runs: Nano shows its power menu. |
| `sys.thimsy.power.state` | session | `sleeping` just before the device sleeps, `awake` after it wakes. |
| `persist.thimsy.audio.volume` | session | The volume level, saved 1.5 s after the last volume key. |
| `ro.thimsy.audio.volume_control` | `device.json` props | The ALSA control the volume keys set (default `Master`). |

The control table of Nano's Linux platform (G9's `ControlKeys.cpp`) turns Android's own triggers into
requests: `sys.gammaos.nano.dosleep=1` into `sleep`, `sys.gammaos.nano.park_restart=1` into
`restart-nano`, and `service.bootanim.nano_action=reboot` or `=shutdown` into `reboot` or `poweroff`.
Nano's own Reboot and Power Off (the last step of its shutdown, `Power::powerAction`) write the
`reboot` or `poweroff` request directly; the `nano_action` rows are for a write from elsewhere.

## What the session does

### At start

1. Opens the store, reads `device.json` (`power.mem_sleep`, `pad.hotkey.quit_combo`,
   `audio.card_index`), the family (`/run/thimsy/family`, else `ro.thimsy.family`, else the only
   family directory) and the boot disk from `/run/thimsy/bootdisk`, which `S00base` wrote. Reads the
   GPU pack's `/usr/share/thimsy/gpu.json` (`thimsy-gpu-pack`): its `"env"` object (for the swrast
   pack `GALLIUM_DRIVER=softpipe`) is added to Nano's environment, and so to every game Nano starts.
   Entries whose name is not a shell variable name or whose value is not a string are left out and
   logged; no file, or a bad one, means no GPU environment. Nano's environment also gets
   `ALSA_CARD=<audio.card_index>` (0 without one): the card `/etc/asound.conf`'s `default` (plug over
   a dmix) and its control use, so Nano's streams and its games' share the card.
2. Makes `/` a shared mount (so drives mounted later reach Nano's namespace), stops a Nano left over
   by an earlier session (`/run/thimsy/nano.pid`), and **starts Nano at once** (`thimsy-session: nano
   started` in the kernel log). Nano's output goes to `/run/thimsy/log/nano.log`.
3. Starts `thimsy-padd` if `S02padd`'s one (`/run/thimsy/padd.pid`) is not running; watches it either
   way (a pidfd) and starts it again if it dies.
4. Listens for uevents and opens the input nodes; writes the first `volumes.json`.
5. If an earlier session of this boot already ran the deferred services
   (`/run/thimsy/session/deferred-started`), it scans the block devices itself and adopts drives that
   are still mounted (`/proc/mounts`) instead of mounting them again.

### Nano

Restarted when it exits: at once if it ran 30 s or more, else after 1, 2, 4 ... up to 30 s. If it
exits while the game it launched still runs (as Android's home exits when it starts a game), it is
started again when that game ends, subject to the launch-loop hold-back below. Nano exits after
every launch: it starts the game, leaves it running and exits 0. An exit 0 with no game running (the
game ended at once, or the launch could not start) is not a crash either: Nano is started again at
once and the back-off is reset, subject to the same hold-back. The hold-back: a launch cycle is
quick when it is shorter than 10 s (from Nano's start until it is to start again, the game's run
included) and Nano's first frame was on screen less than 2 s before it exited for the launch, or
never; no one had time to choose, as in an auto-launch loop. After three quick cycles in a row the
next ones wait the crash back-off (1, 2, 4 ... up to 30 s); a cycle that is not quick, or a
`restart-nano` request, ends the run. A user trying games that fail at once (a missing core or BIOS)
has the menu on screen for longer than that before each launch and is not held back. The session
sets `sys.thimsy.nano.first_frame` to `0` before each Nano start, so each Nano's first frame is
seen. A Nano whose menu ends with no launch exits 1, which counts as a crash, as does exit 2 (its platform
did not start). After `poweroff` or
`reboot` nothing is started again.

Nano runs in its own mount namespace (B.5, decision I40): every node of an internal-storage disk
(`mmcblkN` whose `device/type` is `MMC`, its partitions, `bootN` and `rpmb`) has `/dev/null` bound
read-only over it, and `CAP_MKNOD` is out of its bounding set, so neither Nano nor a game it starts
can open the eMMC or make a node for it. If the namespace can't be set up, Nano is not started
(fail closed). The namespace is a slave of the session's, so drives mounted later appear in it.

### Deferred services

`/etc/init.d/rcD start` runs once per boot, in the background, when `sys.thimsy.nano.first_frame`
becomes `1`, or 30 s after Nano first started if no first frame came (so a broken Nano still gets SSH
and the drives). Its output goes to `/run/thimsy/log/deferred.log`.

### Keys

The session reads every input node whose capabilities include `KEY_POWER`, a volume key or `SW_LID`,
except gamepads (padd grabs their sources), and the `Thimsy Gamepad` itself (without a grab) for the
quit hotkey. Nodes appearing later (padd's pad, a USB keyboard) are picked up from their uevents.

| Input | Does |
|---|---|
| Power, released before 3 s | Sleep. |
| Power, held 3 s | Nano running: `sys.thimsy.key.power_long`. Nano not running: power off (FlipBase's pwrkeyd idea). It fires at 3 s, while the key is still held. |
| Power, within 1 s of waking | Ignored (the press that woke the device). |
| Volume up or down (and autorepeat) | The level moves by 5 (0 to 100): `amixer -q -c <audio.card_index> sset <control> <level>%`, `sys.thimsy.hud.volume`, and later `persist.thimsy.audio.volume`. |
| Lid closed (`SW_LID` 1) | Sleep. Opening it does nothing: the kernel wakes the device. |
| Quit combo on the Thimsy Gamepad (`BTN_MODE` + `BTN_START` by default) while a game runs | Quit the game, as the `quit-game` request. Once per hold. |

### Sleep

In order: `sys.thimsy.power.state=sleeping`; SIGSTOP to the game's process group; Wi-Fi stopped
(`/etc/init.d/deferred/D30net stop`, when `/run/wpa_supplicant.pid` names a running process);
backlight to 0; `sync`; the family hook's `sleep-pre`; `/sys/power/mem_sleep` (`device.json`'s
`power.mem_sleep`, default `deep`, if the kernel offers it, else `s2idle`); `mem` (else `freeze`)
into `/sys/power/state`, which returns when the device wakes. Then the reverse: the hook's
`sleep-post`; the backlight back; SIGCONT to the game; `D30net start`; `sys.thimsy.power.state=awake`.
The family hook is run as `sh /usr/share/thimsy/family/<family>/hook sleep-pre`.

### Drives

The session hears of block devices from kernel uevents. `D10cards` (a deferred service) re-sends
`add` for the drives that were there at boot, so the first mounts wait for Nano's first frame. Per
partition:

- never virtual devices, an eMMC's hardware partitions, or anything on an eMMC (`device/type` MMC);
- never a whole disk that has partitions, nor anything under 1 MiB;
- on a GPT disk only the types Android's vold mounts, Microsoft basic data and Linux filesystem data,
  read from the disk's own GPT (both CRCs checked). So the `games` entry `D60firstboot` is still
  making (typed Linux reserved) and every card's `data` partition (Linux home) are never mounted;
- on the boot disk only the partition named `games` typed basic data. `D60firstboot` retypes it and
  writes `change` to its uevent; the session reads the GPT again on that event and mounts it;
- with a table it can't read, partitions named `uboot`, `boot`, `root` or `data` are not drives.

A drive is checked, then mounted at `/storage/<id>` with plan 7.3's options:

| Filesystem | Check | Mount options |
|---|---|---|
| vfat | `fsck.fat -a`, only when the boot-sector state bit or FAT[1]'s clean bit says dirty | `utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush` |
| exfat | `fsck.exfat -p`, always (the plan's choice until Phase 0 times it) | `dirsync,noatime,nodev,nosuid,noexec` |
| ext4 | `e2fsck -p`, always | `noatime,nodev,nosuid,noexec` |

A check that exits above 1 (above 3 for `e2fsck`) leaves the drive `check_failed`. A card pulled while
being checked stops the check and is never mounted; one pulled while mounted is detached
(`umount -l`). Filesystem serials and labels are read from the superblocks by the session itself
(`src/io/Blkid.cpp`): there is no libblkid.

## Files

| Path | What |
|---|---|
| `/run/thimsy/volumes.json` | the drives (above) |
| `/usr/share/thimsy/gpu.json` | read: the GPU pack's environment for Nano (At start) |
| `/run/thimsy/nano.pid`, `/run/thimsy/padd.pid` | the processes it supervises |
| `/run/thimsy/media_rw/<id>` | links to the mount points; the root's `/mnt/media_rw` must link here (G9's `10-android-links.sh`), since the root is read-only |
| `/run/thimsy/session/deferred-started` | written when it starts `rcD`: a respawned session does not start it again |
| `/run/thimsy/log/session.log` | its own log; `nano.log`, `padd.log`, `scrape.log`, `fsck.log`, `deferred.log`, `session-tools.log` for the programs it starts |
| `/dev/kmsg` | markers: `thimsy-session: started`, `nano started`, `nano exited`, `deferred services starting`, `sleep`, `awake`, `poweroff requested` |

## Testing

```
nice -n 19 make -C linux/src/thimsy-session -j1 check
```

builds in `~/.cache/thimsy/thimsy-session` (with libthimsy's sources), behind the same memory guard
as libthimsy's Makefile, and runs:

- the core and I/O tests (`tests/*_test.cpp`, the GoogleTest stand-in, ASan and UBSan): the key
  machine with a fake clock (2.9 s and 3.1 s presses, the wake press, the quit combo), the sleep plan
  (a lid closing during a game), the volume policy (a card removed mid-check, the root disk's
  partitions with and without the games name, a second card with a `data` partition, the eMMC, two
  cards with one serial), the supervisor's back-off, the request parser, the jail plan, and Blkid on
  exFAT, FAT16, FAT32, ext4, GPT and MBR images built in the test;
- the E2E (`tests/e2e/test_e2e.py`): the real binary, sanitized, as
  `thimsy-session --root R --fake-hw --tooldir T`.

`--root R` puts every path under `R`. `--fake-hw` reads uevents from the FIFO
`R/run/fake-hw/uevent` (and once from `R/run/fake-hw/uevent.replay`) as text records
(`add@/devices/...`, then `KEY=VALUE` lines, then a blank line), reads the input nodes
`R/dev/input/eventN` as FIFOs of text lines (`KEY_POWER 1 [<ms>]`), mounts through the `mount` and
`umount` programs, and writes Nano's namespace plan to `R/run/thimsy/session/jail.plan` instead of
applying it. `--tooldir T` makes programs in `T` replace the system's by base name (Nano, padd, the
scraper, `fsck.*`, `amixer`, `poweroff`), and the session passes `THIMSY_ROOT` and `THIMSY_TOOLDIR`
to everything it starts, as the init scripts expect (`docs/init.md`).
