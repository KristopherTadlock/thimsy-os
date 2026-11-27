# Non-root Nano plan: running Nano below root on Linux and Android

Status: research and plan (2026-11-26). Not started. Nothing here was built or run on a device. It follows the owner's OA-14 (`docs/linux/DECISIONS.md:1113`), which accepted NanoJail's partial eMMC guard and asked for this separate look at running Nano below root.

Tags: **[V]** checked in this repository's code or in a cited upstream source; **[I]** inferred, to be proven on a device; *(guess)* for sizes and times. Paths are relative to the repository root. `nano/...` paths are in the shared tree, which both builds compile.

## Requirement (owner, via OA-14, 2026-11-26)

- Find out what it takes to run Nano (`nano/`) as a non-root user, on the Linux build (Thimsy Base) and on the Android build (GammaOS-based AOSP).
- The work must fit OA-4 (`docs/linux/DECISIONS.md:1096`). Shared code expresses intent through `nanoplat` interfaces, and each platform's implementation does the low-level work. So privileged work belongs behind the platform layer, and shared code stays platform-free.
- This document is the plan only. It changes no code.

## Summary

- **Linux: do it, and do it before the alpha.** It is cheap because Nano on Linux already asks `thimsy-session` for most root work: power, sleep, brightness, ejects and drive mounts.
  - What is left is mostly session and image work:
    - a `nano` user;
    - device-node groups set by the session, since there is no udev or mdev;
    - group-owned runtime files;
    - exFAT mount options;
    - a one-time chown of `/storage`;
    - three new session verbs (Wi-Fi, Bluetooth, fixed performance);
    - rlimits in place of capabilities.
  - Almost no shared, Android-compiled code changes.
  - It closes the hole OA-14 accepted: a non-root Nano cannot mount a new devtmpfs.
  - Size: about 2 to 3 weeks *(guess)*, plus device checks in Phase 0.
- **Android: not yet as a uid switch. Do the groundwork as part of the OA-4 seam moves.**
  - Nano runs as root in the reused `bootanim` SELinux domain with 11 capabilities, on a build that is permissive device-wide.
  - Nano makes about 150 shell calls (`am`, `cmd`, `settings`, `pm`, `appops` and others) that rely on root passing the framework's uid checks.
  - It also writes other apps' private data (`/data/data/<pkg>`), with `fchown` and SELinux relabels.
  - The realistic target is **uid `system` with a few capabilities**, as SurfaceFlinger runs, plus a small root helper behind an init socket for the file and sysfs work. It is not a fully unprivileged uid; that is the Android launcher plan's world.
  - Most of the work is moving shell sites behind `nanoplat`, which OA-4 asks for anyway.
  - Size: about 6 to 10 weeks *(guess)*, most of it Android-surface commits.
  - Value is lower while SELinux is permissive and uid `system` is nearly root to the framework.
  - A cheap first win: drop `SYS_ADMIN`, `SYS_PTRACE` and `SYS_CHROOT` by removing the one `nsenter -t 1 -m` use.
- **Upstream practice:** every retro-handheld distro looked at runs its frontend as root:
  - ROCKNIX's `emustation.service` has no `User=`.
  - Batocera and Knulli start EmulationStation from a root init script.
  - muOS starts its frontend from a root boot script.
  - ArkOS runs as user `ark`, but with `NOPASSWD: ALL` sudo.

  So Thimsy would be ahead of the field, not following it. The mechanisms are standard:
  - device groups and udev rules (ArkOS, Kodi-GBM, cage);
  - the Linux 5.8 DRM-master rule, which lets an unprivileged opener drop and retake master;
  - RLIMIT_RTPRIO and RLIMIT_MEMLOCK;
  - on Android, init's `user`, `capabilities` and `socket` options, and per-service `ctl.start$<svc>` property labels.

## 1. Where things stand

### 1.1 Linux [V]

- **The supervisor.** BusyBox init respawns `thimsy-session` as root (`linux/board/common/rootfs-overlay/etc/inittab:12`). The session starts Nano with `spawnChild` (`linux/src/thimsy-session/src/Session.cpp:531-561`, `io/Proc.cpp:44-123`).
- **What the session does not set for Nano:**
  - no `setuid`, `setgid`, `setgroups` or `capset` anywhere in `linux/src`;
  - no rlimits, cgroups, seccomp or `no_new_privs`.
  - Only the scraper child gets nice 10, `oom_score_adj` 800 and idle I/O priority (`Session.cpp:579-581`).
- **NanoJail** (`linux/src/thimsy-session/src/core/NanoJail.cpp:54-73`):
  - `unshare(CLONE_NEWNS)`;
  - `/` made a slave mount;
  - `/dev/null` bound read-only over each eMMC node;
  - `CAP_MKNOD` dropped from the bounding set;
  - it fails closed.
  - Every other capability stays, `CAP_SYS_ADMIN` among them.
  - With no eMMC the plan is empty and there is no jail at all (`Session.cpp:549-551`, `NanoJail.cpp:55`).
- **IPC is the file-backed config store, not a socket** (libthimsy):
  - Runtime keys are one file per key in `/run/thimsy/props/`, mode 0644, or 0600 for secret-looking keys (`libthimsy/src/config.cpp:482`).
  - `persist.*` keys live in `/storage/.config/thimsy/persist.props`: the file is 0600 in a 0700 directory, with a 0600 lock file (`config.cpp:329`).
  - Nano writes `sys.thimsy.session.request` = `[seq:]verb[:arg]`, and the session answers in `sys.thimsy.session.done` (`linux/docs/session.md:69-119`).
  - Verbs: `sleep`, `poweroff`, `reboot`, `quit-game`, `eject:<id>`, `scrape`, `restart-nano`, `brightness:<n>` (`core/Requests.cpp:15-24`).
  - The session also acts on `sys.powerctl` and on `sys.thimsy.game.pid`. It sends SIGTERM and SIGKILL to the pid and process group that Nano wrote there (`Session.cpp:323-344, 767-778`).
- **Users and the root:**
  - `linux/board/common/users.table` adds no user. Its header says a service that drops privileges adds its user there, and that `10-erofs.sh` must then stop passing `--all-root`.
  - `--all-root` is passed at `linux/board/common/post-image.d/10-erofs.sh:122`.
  - The root is read-only EROFS.
  - `/dev` is devtmpfs with no mdev or udev; libudev-zero stands in (`linux/configs/fragments/common.fragment:40-43`). Node modes are the kernel's defaults, root-owned with no group policy [I].
- **Mounts:**
  - `S00base` mounts the `data` partition as ext4 on `/storage` with `noatime,nosuid,nodev` (`etc/thimsy/init.conf:6`).
  - `data.ext4` is made root-owned (`post-image.d/20-data.sh:35-37`).
  - The session mounts removable drives at `/storage/<VOLID>` (`core/VolumePolicy.cpp:117-127`):
    - exFAT with `dirsync,noatime,nodev,nosuid,noexec`;
    - vfat with `utf8,shortname=mixed,...`;
    - ext4 with `noatime,nodev,nosuid,noexec`.
    - None sets `uid=`, `gid=` or a mask.
- **Display, input and launch:**
  - Nano opens `/dev/dri/card0` and tries `DRM_IOCTL_SET_MASTER`, which may fail (`nano/NanoMenuDrm.cpp:982-984`).
  - At a launch it drops master and closes the device. It then forks RetroArch as root, writes `sys.thimsy.game.pid` and exits. The session restarts Nano when the game ends (`nano/platform/linux/main_linux.cpp:130-139`, `Session.cpp:602-621, 776-781`).
  - Nano never grabs input on Linux (`nano/platform/linux/InputPolicy.h:17-21`). `thimsy-padd` grabs the pad's source nodes, unlinks them and exposes a uinput pad.
- **The shell gate filters Android-ness, not privilege.**
  - `nano/platform/linux/ShellGate.cpp` refuses Android tools and routes `setprop` and `reboot`.
  - Every other command passes, `mount`, `chmod`, `kill`, `dd` and `echo > /sys/...` included (`ShellGate.cpp:357, 548`).

### 1.2 Android [V]

- **The Nano services** (`nano/gammaos-nano.rc:40-53` and `72-81`):
  - `gammaos-nano` and `gammaos-nano-overlay` run as `user root`.
  - Groups: `root graphics input system media_rw sdcard_rw everybody external_storage package_info readproc inet`.
  - Capabilities: `SYS_NICE IPC_LOCK SYS_RESOURCE DAC_OVERRIDE DAC_READ_SEARCH CHOWN FOWNER SYS_ADMIN SYS_PTRACE SYS_CHROOT BLOCK_SUSPEND`. The overlay adds `KILL`.
  - Also `ioprio rt 0`, `rlimit rtprio 99`, `rlimit memlock 512 MiB` and `task_profiles MaxPerformance`.
- **Why it has these privileges.** The rc header (`gammaos-nano.rc:1-39`) gives the reasons:
  - root, because `cmd wifi` and `cmd bluetooth_manager` accept only ROOT_UID or SHELL_UID;
  - `DAC_OVERRIDE` and `CHOWN` for a cache migration;
  - `SYS_ADMIN`, `SYS_CHROOT`, `SYS_PTRACE` and `readproc`, so `nsenter -t 1 -m` works;
  - `BLOCK_SUSPEND` for `/sys/power/wake_lock`.
- **The scraper** (`gammaos-nano-scrape`, rc:348-360) runs as root with `DAC_OVERRIDE DAC_READ_SEARCH`.
- **Helper services.** About a dozen helper services run as root in `u:r:su:s0`, started by property triggers:
  - `nano-fixedperf-on` and `-off`, which run `cmd power`;
  - `nano-dosleep` and `-dowake`, which run `input keyevent`;
  - `nano-typec-guard`, `nano-pmdown`, `joypad_uevent_fixup`, `nano-seedrng`;
  - `nano-powerwatch`, with `SYS_BOOT`.

  Lines: rc:175-402.
- **The SELinux domain is `bootanim`:**
  - `system/sepolicy/private/file_contexts` labels `/system/bin/gammaos-nano` `bootanim_exec` (patch `patches/thimsy/0001-thimsy.patch`, lines 194638-194640).
  - The logs show `scontext=u:r:bootanim:s0 ... permissive=1` (`nano/sync.txt:9`).
  - The build is permissive device-wide (`androidboot.selinux=permissive`, recorded in the nano repository's `dp_dock_findings.md:721` and `plan.md:210`).
- **Display:**
  - Nano opens `card0` and takes `SET_MASTER` early (`nano/main.cpp:1180-1195`) unless the device is force-SurfaceFlinger, as the Cube's Unisoc SoC is (`main.cpp:1137-1158`).
  - At a launch, `drmStop()` drops master, closes the device and exits; SurfaceFlinger and the HWC take the CRTC back (`nano/NanoMenuDrm.cpp:3115-3204`).
  - After the first launch, the overlay is a SurfaceFlinger client (`nano/NanoMenu.cpp:242-284`).
- **Launches:**
  - The DRM home writes `sys.gammaos.nano.launch_*` properties and exits, and a patched framework starts the activity under the app's own uid (`nano/platform/firmware/Launcher.cpp:45-63`, `nano/NanoLaunchCommit.cpp:33-50`).
  - The overlay runs `am start` itself as root (`nano/NanoMenuOverlay.cpp:1384-1400`).

## 2. Inventory: every privileged thing Nano does

Columns: what Nano does, with evidence; how Linux does it today; how Android does it today; whether a non-root Nano needs a change. "Linux: not reached" means the code is `[firmware]` (`docs/linux/IMPLEMENTATION.md:497-511`), is refused by the gate, or finds no node.

### 2.1 Display (DRM/KMS)

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Open `card0`, become master | `NanoMenuDrm.cpp:982-984`; `main.cpp:1195` | Nano opens it after the session; whoever opens first with no master present becomes master implicitly [V: `drm_auth.c`] | early grab as root | Linux: needs only read-write on `card0` (group `video`). Android: `/dev/dri/*` is `0666 root graphics` in AOSP `ueventd.rc` |
| `DROP_MASTER` at a launch, `SET_MASTER` again later | `NanoMenuDrm.cpp:963,1014,3201` (drop); `:3243,3333` (re-take, park and resume) | the drop is followed by close, and Nano exits | the same; the parked home re-takes master in a loop (`NanoMenuDrasticPark.cpp:154,252`) | Since Linux 5.8, a process may set or drop master on a file it opened itself and was master on, with no `CAP_SYS_ADMIN` (kernel commit 45bc3d26c95a). Thimsy Base kernels are 6.18 to 7.3 (`linux_handheld_plan.md:81`): fine. Android kernels older than 5.8 (the Cube's is 5.4.233, `network_storage_plan.md:30`) need `CAP_SYS_ADMIN` for both ioctls. Fix there: release by `close()` and re-take by a fresh `open()`, neither of which needs a capability |
| Atomic commits, SETCRTC, page flips, colour properties | `NanoMenuDrm.cpp:606,844,2651`; `NanoScreenCal.cpp:142-285` | master only | master only | no change once Nano is master |
| SurfaceFlinger client: layers, display tokens, `captureDisplay` | `NanoMenu.cpp:242-284,1071-1104`; `NanoMenuRender.cpp:355` | not reached | root passes SurfaceFlinger's caller checks | uid `system` passes them too (SurfaceFlinger trusts `AID_SYSTEM` and `AID_GRAPHICS`) [I, verify] |

### 2.2 Input

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Open `/dev/input/event*`, inotify on `/dev/input` | `NanoMenuInput.cpp:76-105`; `NanoMenuPS3Menu.cpp:3745,4035` | reads padd's uinput pad and other nodes | reads the raw nodes | Linux: group `input` on the nodes, which the session sets. Android: `/dev/input/*` is `0660 root input`, and Nano is already in `input` |
| `EVIOCGRAB` | `platform/firmware/InputPolicy.cpp:51-76` (via nanoplat); `NanoMenu.cpp:6154` and `:402` (direct, DRM home before a hand-off) | never (`InputPolicy.h:17-21`) | optional, plus the hand-off grab | A grab needs only an open file, not a capability. The direct `NanoMenu.cpp:6154` site should move behind `InputPolicy` under OA-4 anyway |
| uinput, hiding source nodes | `linux/src/thimsy-padd/src/io/EvdevSource.cpp:148,189,193` | padd, as root | not used | stays in padd, which stays root. Its tap socket is `chmod 0660` root:root (`thimsy-padd/src/io/Control.cpp:82`) and needs group `nano` |
| Key injection (`input keyevent 223/224`) | rc:315-335; `NanoMenuInput.cpp:1049,1136-1139` | not reached | init's `su` service on a property trigger | already brokered |

### 2.3 Audio

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| PCM output | `platform/linux/AudioAlsa.cpp:40,53` (ALSA `default`, dmix); `platform/firmware/Audio.cpp:76-105` (AAudio) | dmix with `ipc_perm 0666` (`rootfs-overlay/etc/asound.conf:31-35`) | AAudio through audioserver | Linux: group `audio` on `/dev/snd/*`; dmix is already shared across users. Android: no change |
| Boot chime through tinyalsa on `/dev/snd` | `NanoBootChime.cpp:159-181,328` | not reached (`[firmware]`) | direct, as root | `/dev/snd/*` is `0660 system audio` in AOSP: uid `system` works |
| Volume keys | `Session.cpp:820-830` (`amixer`) | the session | framework | already brokered |

### 2.4 Scheduling and memory

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| `SCHED_FIFO` 80 and nice -20 on the render thread | `NanoMenu.cpp:1339-1341`; also `NanoMenuDrm.cpp:1882`, `NanoMenuOverlay.cpp:553`, `DrasticRunner.cpp:86,5178` | root | root plus `rlimit rtprio 99` | RLIMIT_RTPRIO lets an unprivileged thread take real-time priority up to the limit, and RLIMIT_NICE does the same for negative nice (`sched(7)`). Linux: the session sets both before it drops Nano's uid. Android: keep `SYS_NICE`, as SurfaceFlinger does |
| `setpriority(ANDROID_PRIORITY_DISPLAY)`, nice -4 | `main.cpp:1034`; `NanoBootSequence.cpp:37`; `NanoThemeEngine.cpp:423` | root | root | RLIMIT_NICE on Linux; `SYS_NICE` on Android |
| `mlockall(MCL_CURRENT|MCL_FUTURE)` | `NanoMenu.cpp:1421`; off when `persist.gammaos.nano.mlockall=0` | root | `IPC_LOCK` and `rlimit memlock 512 MiB` | RLIMIT_MEMLOCK covers it with no capability (`mlock(2)`). Linux: the session sets 512 MiB. Android: the rc limit already exists, so `IPC_LOCK` can go |
| `/proc/sys/vm/drop_caches` | `NanoMenuOverlay.cpp:851` | compiled (`[core]`); fails with EPERM, harmless [I] | root | moves to the helper (Android), or is dropped |
| `/proc/<pid>/oom_score_adj` on MediaProvider | `NanoMenuMtp.cpp:127-134,204` | not reached [I] | root | Android: helper |

### 2.5 Power, backlight, wakelocks, CPU, Type-C

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Backlight sysfs write | `NanoBacklight.h:99-112` (direct, from `NanoMenu.cpp:935` and `NanoMenuInput.cpp:1105,1325`); `NanoControlCenter.cpp:1758-1818` | Nano writes it directly; the session also has `brightness:<n>` (`Session.cpp:832-845`) | Lights HAL (`platform/firmware/Power.cpp:192-225`) plus a direct write | Linux: the session gives `<backlight>/brightness` to group `nano` with mode 0664 at boot, as ArkOS's udev rule does. The slider stays a direct write, with no IPC delay. Android: an init `chown system system` on the node, as the rc already does for rfkill (rc:159-173) |
| Wakelocks (`/sys/power/wake_lock`) | `NanoMenu.cpp:456,5141-5208`; `NanoMenuInput.cpp:1128-1376`; `NanoMenuMtp.cpp:350`; `NanoMenuOta.cpp:238` | compiled; whether `CONFIG_PM_WAKELOCKS` is set is unknown (nothing in `linux/` names it) | `BLOCK_SUSPEND` | the kernel checks `CAP_BLOCK_SUSPEND` whatever the file mode. Android: keep the capability, as system_server does. Linux: there is no autosleep, so the session's sleep plan decides; drop the writes from the Linux path or give `CAP_BLOCK_SUSPEND` as an ambient capability [I] |
| CPU clocks and governors | `NanoMenu.cpp:1241,1746,2042` (`/vendor/bin/setclock_*.sh`); `NanoBtStable.cpp:166-182` (cpufreq) | `/vendor` is refused by the gate. The family hook writes governors through `posix_spawn` (`platform/linux/ControlKeys.cpp:54-55,147-168`; `board/rk3566/.../family/rk3566/hook:23-39`) | root `system()`; `fixedperf` goes through init (rc:300-304) | Linux: a new session verb `perf:on|off` runs the family hook. Android: the helper, or rc triggers like `fixedperf` |
| Reboot, power-off, recovery | `platform/firmware/Power.cpp:266` (`service.bootanim.nano_action`); `platform/linux/Power.cpp:141-150,189-193` | session verbs | the bootanim side acts on a property [I: base tree] | already brokered on both |
| Type-C `port_type` | `NanoMenuMtp.cpp:352-355`; `nano-typec-guard.sh` | not reached [I] | root write; init's `su` guard | Android: init `chown system system` on the node, or the guard service |
| Bluetooth LPM (`/proc/bluetooth/sleep/lpm`) | `NanoBtStable.cpp:42,117`; `NanoMenuInput.cpp:941` | not reached (`[firmware]`) | root | Android: helper |

### 2.6 Wi-Fi and Bluetooth

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Radio on or off, starting daemons | `platform/linux/actions/SystemActions.cpp:83-133,243-250` (`sh D30net` and `D35bluetooth start|stop`); `platform/firmware/SystemActions.cpp:45-93` (`cmd wifi`, `cmd bluetooth_manager`) | **Nano itself starts wpa_supplicant, udhcpc, dbus-daemon and bluetoothd as root** | root `cmd` calls | Linux: new session verbs `wifi:on|off` and `bt:on|off`; the session runs the scripts (it already runs `D30net stop` for sleep, `Session.cpp:899-927`). This revises OA-5's "started by platform/linux code" to "requested by platform/linux code". Android: move to NanoNetBridge in system_server (`NanoMenuSettings.cpp:131-145`, patch 180273-180355), which already exists |
| Scan, join, status | `platform/linux/actions/WpaCtrl.cpp:48-80` (wpa_ctrl socket); `platform/firmware/Network.cpp:457-760` (`cmd wifi`, `dumpsys`) | wpa_ctrl socket created with `GROUP=0` (`D30net:38`) | root | Linux: `GROUP=nano`. Android: the bridge |
| Bluetooth control | `platform/linux/Network.cpp:573-574,738` (`bluetoothctl` over the system bus); `platform/firmware/Network.cpp:784-894` (`gammaos-net bt ...`, an app_process CLI) | root on D-Bus | root | Linux: a D-Bus policy that lets group `nano` talk to `org.bluez` [verify BlueZ's shipped `bluetooth.conf`]. Android: `gammaos-net` as uid `system` [I, verify], or the bridge |
| Wi-Fi config with static IP and proxy | `NanoMenuSettings.cpp:595-621` (`gammaos-net wifi configure`) | not reached | root | Android: the bridge |

### 2.7 Storage: mounts, cards, shares, moves

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Mount or unmount anything | none in shared code (no `mount(2)`, `losetup` or `mount -t`) | the session mounts drives (`io/Mounter.cpp:26-86`) with `fsck` first (`Session.cpp:643-698`) | vold; sharefs from init | already brokered on both |
| Network shares | `NanoMenuShares.cpp:85-268` writes share config only; the init-owned `gammaos-sharefs` mounts | not reached (`[firmware]`; no `/mnt/shares`) | init daemon | Linux later: a session verb or a FUSE helper owned by the session, never Nano (`network_storage_plan.md` §5 already proposes a non-root sharefs) |
| Card moves: links inside app data, `appops`, `am force-stop`, `restorecon` | `NanoSdMove.cpp:250,1355,1540,1965-1971,2268-2303` | `[core]`, but its `/data/data` and `am` paths are refused or empty on Linux [I] | root | Android: a helper verb ("link this app folder to the card") plus the bridge for `appops` |
| ES-DE store move | `NanoEsdeStore.cpp` (files only; `:631-634` reads `/mnt/shares`) | plain files on `/storage` and the card | root | needs only ownership of the files (section 4.2 for exFAT) |
| `nsenter -t 1 -m` (init's mount namespace) | `NanoMenuEsdeMenu.cpp:831-843` | unwrapped by the gate (`ShellGate.cpp:568-586`) | root with `SYS_ADMIN`, `SYS_PTRACE` and `SYS_CHROOT` | download in-process or through the bridge; then the three capabilities go |

### 2.8 Files Nano writes

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Its state in `/data/system/nano_*` (about 70 files and folders) | `NanoMenuUtils.h:128-213` and others | `/data` links to `/storage/.config/thimsy/data` (`post-build.d/10-android-links.sh:6-17`), root-owned | `/data/system` is `system:system`; Nano's files are root-owned | Linux: a one-time chown to `nano`. Android: a one-time chown to `system`, and the `chown(..., 0, 0)` calls change |
| `chown(..., 0, 0)` on its own caches | `NanoMenuMusic.cpp:237`, `NanoMenuXmb.cpp:313,325,978`, `NanoMenuVideo.cpp:274` and 7 more | root, no-op | root | Becomes "keep my own ownership": a no-op on both platforms once Nano owns its files. This is an Android-compiled change |
| "Keep the old owner and mode" on save | `NanoRaConfig.cpp:179`; `NanoHotkeys.cpp:204`; `NanoFileJob.cpp:187` | works as root | works as root | **A non-root save fails when the old file is owned by someone else** (`NanoRaConfig.cpp:179` fails the save). So the chown migration must run before the first non-root start |
| Other apps' private data (`/data/data/<pkg>`), `fchown` to the app's uid, `fsetxattr security.selinux`, `restorecon` | `NanoBiosUtil.cpp:173-198,430-486`; `NanoBiosWorker.cpp:59-66,755-946`; `NanoButtonMapReaders.cpp`; `NanoMenuPS3Folder.cpp:888` | not reached (no such apps) | root plus `DAC_OVERRIDE`, `CHOWN`, `FOWNER` | Android: a helper verb set (write an app file, link an app folder, relabel), with the package resolved from `packages.list` and paths checked against that package's own folder |
| `/data/media/0`, `/mnt/media_rw/<id>` | about 70 literals | links to `/storage/emulated/0` and `/storage/<id>` | groups `media_rw` and `sdcard_rw` | Android: keep the groups. Linux: ownership (below) |
| Settings on the game card (exFAT) | `ES-DE/`, `emulator_config/`, RetroArch saves | root on an exFAT mounted without `uid=` | vold mounts public volumes for `media_rw` | Linux: mount with `uid=nano,gid=nano` and masks (section 4.2) |
| SSH password (OA-3, not built yet) | `DECISIONS.md:1094`: a hash in a 0600 file under `/storage/.config/thimsy/ssh/` | not built | not on Android | Linux: a session verb `ssh-password:<hash>` and `ssh-password:default`. `ssh/` stays root-only |

### 2.9 Other processes

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| Launch a game | `platform/linux/Launcher.cpp:115-133,332-372` (fork and exec RetroArch); `platform/firmware/Launcher.cpp:45-63` (properties) | the game inherits root, Nano's mount namespace and bounding set | the framework starts the app under its own uid | Linux: the game inherits `nano` (section 4.2 on a separate uid). Android: no change |
| `am start`, `am force-stop`, `monkey` | `NanoMenuOverlay.cpp:1107-2790`; `NanoControlCenter.cpp:1119-1274`; `NanoSdMove.cpp:250,458`; `NanoBiosWorker.cpp:179,868,890` | refused by the gate | root shell | uid `system` holds `FORCE_STOP_PACKAGES` and starts activities [I, verify]; under OA-4 these move behind a `nanoplat` launcher or app-control interface anyway |
| SIGSTOP and SIGCONT of app pids | `NanoMenuOverlay.cpp:250,275,299` | not reached | overlay with `KILL` | Android: keep `KILL` on the overlay |
| Read other processes' `/proc` | `NanoMenuMtp.cpp:118`, `NanoSdMove.cpp:234`, `NanoHotkeys.cpp:235`, `NanoMenuSystem.cpp:671,699` | root | group `readproc` | Android: keep `readproc`. Linux: `/proc` without `hidepid` lets any user read it |
| Quit a game, signal a group | `Session.cpp:323-344,943-951` | session, by the pid Nano wrote | framework | **Linux: the session must check that the pid in `sys.thimsy.game.pid` belongs to `nano`** (its `/proc/<pid>/status` uid). Otherwise a non-root Nano could make the root session kill anything |
| Scraper | `Session.cpp:579-581`; rc:348-360 | started by the session as root | root with two capabilities | Linux: start it as `nano`. Android: uid `system`, or its own uid |

### 2.10 Properties and service control

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| About 557 `property_set` calls (281 `sys.*`, 175 `persist.*`) | `tools/nano-tree/props.txt` | libthimsy file store | SELinux `set_prop` rules (`bootanim.te` hunk, patch 194617-194630) | Android: unaffected by the uid; SELinux alone decides. Linux: the store's directory and files need group `nano` (section 4.2) |
| `ctl.start` and `ctl.stop` of `bootanim`; `ctl.restart nano-typec-guard` | `NanoMenu.cpp:558`; `NanoMenuRender.cpp:5832`; `NanoMenuMtp.cpp:351` | `ctl.*` mapped by `platform/linux/ControlKeys.cpp` | allowed in permissive mode | Android: label `ctl.start$<svc>` per service so a Nano domain may control only its own helpers (AOSP's `private/property_contexts` does this for `adbd` and `snapuserd`) |
| Property triggers into `su` services (`fixedperf`, `dosleep`, `park_restart`, `scrape_run`, `ota.prestage`) | rc:300-363 | session verbs or not reached | init | already brokered; the pattern to extend |
| `setprop persist.sys.locale`, `persist.sys.timezone` | `NanoMenuStrings.cpp:664`; `NanoMenuPS3Menu.cpp:18456` | routed to the store | shell as root | Android: `set_prop` rules; route through `SystemActions` under OA-4 |

### 2.11 Shell commands

- **Android:** about 150 `system()` and `popen()` sites and three fork-exec runners (`NanoMenuEsdeMenu.cpp:831`, `NanoOtaCheck.cpp:178`, `NanoSdMove.cpp:255`). The programs:
  - framework tools: `am`, `cmd` (12 services), `settings`, `svc`, `pm`, `dumpsys`, `appops`, `ime`, `locksettings`, `monkey`, `screencap`, `setprop`;
  - labels and logs: `restorecon`, `chown`, `logcat`;
  - process tools: `pidof`, `nsenter`;
  - `gammaos-net` and `/vendor/bin/setclock_*.sh`;
  - plain tools: `curl`, `unzip`, `rm`, `mkdir`, `mv`, `date`, `timeout`, `grep`, `sed`.
- **Three sites interpolate without quoting:**
  - `cmd alarm set-timezone` (`NanoMenuPS3Menu.cpp:12222,16349`);
  - `setprop persist.sys.timezone` (`:18454`);
  - `gammaos-net bt confirm` (`:16562`).

  As root, those are the sharpest edges. Fix them regardless of this plan.
- **Linux:** the gate refuses the Android tools. What passes runs with Nano's uid, so after the switch the kernel is the privilege gate.

### 2.12 Raw block and other device files

| What | Evidence | Linux today | Android today | Non-root |
|---|---|---|---|---|
| `baseparameter` partition write (screen calibration) | `NanoScreenCal.cpp:335,377,467` (`/dev/block/by-name/baseparameter`, `O_RDWR`, `pwrite`) | no node (`IMPLEMENTATION.md:257`); the jail masks eMMC nodes | root | Android: a helper verb "write calibration" with the block checked to be that partition |
| `/dev/dma_heap/system` | `DrasticRunner.cpp:7308,7359` | not reached | root | Android: `ueventd.rc` gives it `0444 system system` in AOSP [verify on the device] |
| `/proc/sysrq-trigger`, `reboot(2)` | `powerwatch/nano_powerwatch.cpp:127,138` | not used | a separate root service with `SYS_BOOT` | already separate |

## 3. Upstream practice

| Project | Frontend user | How privileged work is done | Source |
|---|---|---|---|
| ROCKNIX | root: `emustation.service` has no `User=`; nor do `essway.service` and `sway.service` | everything in-process as root | [emustation.service](https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/ui/emulationstation/system.d/emustation.service), [essway.service](https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/ui/emulationstation/system.d/essway.service) |
| Batocera, Knulli | root: `S31emulationstation` and `emulationstation-standalone` switch no user | flag files (`/tmp/reboot.please`) read by the wrapper after ES exits. The wiki: "Batocera is not a designed to be a secure OS" | [S31emulationstation](https://github.com/batocera-linux/batocera.linux/blob/master/package/batocera/emulationstation/batocera-emulationstation/S31emulationstation), [emulationstation-standalone](https://raw.githubusercontent.com/batocera-linux/batocera.linux/master/package/batocera/emulationstation/batocera-emulationstation/emulationstation-standalone), [wiki](https://wiki.batocera.org/security), [Knulli](https://github.com/knulli-cfw/knulli-linux) |
| muOS | root: `S99muos.sh` runs `FRONTEND start` with only `setsid` | in-process | [S99muos.sh](https://raw.githubusercontent.com/MustardOS/internal/main/script/init/S99muos.sh), [service.sh](https://raw.githubusercontent.com/MustardOS/internal/main/script/var/service.sh) |
| ArkOS (dArkOS) | `ark`: `User=ark` | groups `video,render,input,audio,netdev`. A udev rule makes the backlight group-writable and Mali `0660 video`. The ROM partition is mounted `uid=1000,gid=1000,umask=000`. Everything else goes through `ark ALL= NOPASSWD: ALL` | [emulationstation.service](https://raw.githubusercontent.com/christianhaitian/dArkOS/master/Emulationstation/emulationstation.service), [utils.sh](https://raw.githubusercontent.com/christianhaitian/dArkOS/master/utils.sh), [bootstrap_rootfs.sh](https://raw.githubusercontent.com/christianhaitian/dArkOS/master/bootstrap_rootfs.sh) |
| Kodi (GBM) | `kodi` | `SupplementaryGroups=audio video input ...`, `PAMName=login` | [kodi-gbm.service](https://raw.githubusercontent.com/graysky2/kodi-standalone-service/master/x86/init/kodi-gbm.service) |
| cage, Weston | any user | through logind or seatd (libseat). Weston removed its setuid `weston-launch` in version 11 | [cage wiki](https://github.com/cage-kiosk/cage/wiki/Starting-Cage-on-boot-with-systemd), [Weston commit](https://gitlab.freedesktop.org/wayland/weston/commit/a96dfc709854), [seatd](https://github.com/kennylevinsen/seatd) |
| Android SurfaceFlinger, composer | `system`, `capabilities SYS_NICE` | holds DRM master as a non-root first opener [I] | [surfaceflinger.rc](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/services/surfaceflinger/surfaceflinger.rc), [composer rc](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/graphics/composer/2.1/default/android.hardware.graphics.composer@2.1-service.rc) |
| Android vold, installd | root brokers: vold `user root`; installd root with 9 named capabilities | reached over binder from the unprivileged system_server | [vold.rc](https://android.googlesource.com/platform/system/vold/+/refs/heads/main/vold.rc), [installd.rc](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/cmds/installd/installd.rc) |

Kernel and libc facts the design relies on:
- **DRM master** ([commit 45bc3d26c95a](https://github.com/torvalds/linux/commit/45bc3d26c95a8fc63a7d8668ca9e57ef0883351c), current [drm_auth.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/drm_auth.c)):
  - The first opener with no master present becomes master.
  - `SET_MASTER` and `DROP_MASTER` need `CAP_SYS_ADMIN` only when the caller is not the process that opened the file and was master on it.
  - A forked child never passes that check with its parent's file.
  - It landed in v5.8 [U: from a backport header; the page itself was not readable].
  - Render nodes have no master and are gated by file mode ([drm-uapi](https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html)).
- **Real time and locking:**
  - RLIMIT_RTPRIO lets an unprivileged thread raise its real-time priority up to the limit, and `CAP_SYS_NICE` ignores the limit ([sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html)).
  - RLIMIT_MEMLOCK bounds an unprivileged `mlockall`, and `CAP_IPC_LOCK` ignores it ([mlock(2)](https://man7.org/linux/man-pages/man2/mlock.2.html)).
- **Default device groups** ([50-udev-default.rules](https://raw.githubusercontent.com/systemd/systemd/main/rules.d/50-udev-default.rules.in)):
  - udev's defaults: `card*` in `video`, `renderD*` in `render`, input in `input`, sound in `audio`.
  - AOSP's [ueventd.rc](https://android.googlesource.com/platform/system/core/+/refs/heads/main/rootdir/ueventd.rc): `/dev/dri/* 0666 root graphics`, `/dev/input/* 0660 root input`, `/dev/snd/* 0660 system audio`.
- **Mounting shares as a non-root user:**
  - only an fstab entry with `user` or a setuid `mount.cifs`, and both force `nosuid,nodev,noexec` ([mount(8)](https://man7.org/linux/man-pages/man8/mount.8.html), [mount.cifs(8)](https://manpages.debian.org/bookworm/cifs-utils/mount.cifs.8.en.html));
  - libnfs runs in user space ([libnfs](https://github.com/sahlberg/libnfs)).
- **Android init** ([init README](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/README.md)):
  - `user` defaults to root.
  - With `capabilities` unset, a non-root service has all capabilities dropped.
  - `socket <name> <type> <perm> [user [group [seclabel]]]` hands a service `/dev/socket/<name>`.
- **Android SELinux:**
  - `property_contexts` and `set_prop` decide property writes ([adding properties](https://source.android.com/docs/core/architecture/configuration/add-system-properties)).
  - `ctl.start$<svc>` can carry its own type ([property_contexts](https://android.googlesource.com/platform/system/sepolicy/+/main/private/property_contexts)).
  - Each init service needs its own domain ([implementing SELinux](https://source.android.com/docs/security/features/selinux/implement)).

## 4. Design

### 4.1 Principles

1. **The privileged half of the platform layer.**
   - Under OA-4 the platform implementation does the low-level work. On a non-root Nano, the part that needs root moves one step further out, to a process the platform layer talks to:
     - `thimsy-session` on Linux;
     - init services, system_server's NanoNetBridge and one small helper on Android.
   - Shared code does not change shape. It still calls `nanoplat::Power`, `SystemActions`, `Network` and the rest, and it never learns that a request crosses a process boundary.
   - There is no generic "run this as root" interface: every privileged request is a named intent with checked arguments.
2. **Access first by groups and limits, then by brokers.**
   - Device files and hot paths (DRM, evdev, the backlight slider, real-time priority) use groups, node modes and rlimits, so they stay in-process with no latency.
   - Rare, state-changing actions (radios, performance mode, reboot, writing another app's files) go to a broker.
3. **Acquire by open, release by close.**
   - For DRM master, opening `card0` when no master exists makes Nano master, and closing it releases master.
   - That needs no capability on any kernel. It is also what a crash does already.
4. **The kernel becomes the gate.**
   - After the switch, whatever the Linux shell gate passes runs as `nano`, so `mount`, `chmod` of root files, `dd` to a block device and `echo > /sys/...` fail with EPERM.
   - This is the protection OA-14 accepted as missing.

### 4.2 Linux

**User and groups.**
- Add `nano` (uid 1000, gid 1000) to `users.table`, with supplementary groups `video`, `render`, `input` and `audio`.
- `--all-root` can stay: nothing on the read-only root has to be owned by `nano`, because every file Nano writes is on `/storage` (ext4) or `/run` (tmpfs).
- The table's header comment changes to say so. The dbus helper caveat is unchanged.

**How the session starts Nano.** In `spawnChild`'s `preExec`, after the jail and before `execvp`, the session:
1. sets RLIMIT_RTPRIO to 80, RLIMIT_NICE to 40 (nice down to -20) and RLIMIT_MEMLOCK to 512 MiB (the Android figure);
2. calls `setgroups(video, render, input, audio, nano)`, `setresgid(nano)` and `setresuid(nano)`, which clears every capability because no ambient set is raised;
3. calls `prctl(PR_SET_NO_NEW_PRIVS, 1)`, so no setuid binary or file capability gives anything back;
4. keeps `PR_SET_DUMPABLE` at its default, so `/proc/<pid>` stays owned by Nano.

Further points:
- The jail stays, as a second fence. It now applies on every unit, not only those with an eMMC, because the uid drop runs in the same `preExec`.
- If `CAP_BLOCK_SUSPEND` turns out to be needed (section 2.5), it is the one ambient capability: `PR_SET_KEEPCAPS`, then `capset`, then `PR_CAP_AMBIENT_RAISE`, with the launcher clearing the ambient set for games.
- The scraper gets the same drop.

**Device nodes, without udev.**
- The session gains a small node policy, applied once before Nano starts and again on each netlink `add` for the subsystems below. It sets owner root, the group shown and mode 0660:
  - `drm` `card*` → `video`;
  - `renderD*` → `render`;
  - `input` `event*` → `input`, padd's uinput pad included;
  - `sound` → `audio`.
- It also gives `<backlight>/brightness` (and `/sys/class/leds/*/brightness`, if a family uses them) group `nano` with mode 0664.
- Cost: a walk of `/dev/dri`, `/dev/input` and `/dev/snd`, a few dozen `fchownat` calls, well under 1 ms [I].
- **Race:** the session's netlink handler must chmod a late node before Nano's open gives up. Nano's 600 ms poll for `card0` must treat `EACCES` like `ENOENT` and retry. That is a one-line platform change.
- **Not touched:** block devices and `/dev/uinput` stay root-only. The first is the eMMC guard by permission; the second belongs to padd.

**The config store.**
- `/run/thimsy/props` becomes `root:nano` with mode 2770 (setgid, so new files take the group). libthimsy writes runtime keys 0660, and 0640 for secret keys the session owns.
- `persist.props`, its lock and `/storage/.config/thimsy` become `nano`-owned, 0600 and 0700. Nano is the store's writer.
- The session only reads persist keys. Any key it must write goes through a runtime key, or through an `fchown` after the write.
- `/run/thimsy/volumes.json` stays 0644.

**Validating requests now that the writer is unprivileged.**
- `sys.thimsy.game.pid`: before signalling, check that the pid's uid is `nano` and that it is not the session or padd.
- Verb arguments are already parsed strictly (`Requests.cpp`). New verbs follow the same rule: a fixed verb list, numeric or allowlisted arguments.

**New session verbs.**
- `wifi:on|off` and `bt:on|off` run `D30net` and `D35bluetooth`.
- `perf:on|off` runs the family hook.
- `ssh-password:<hash>` and `ssh-password:default`, when OA-3's work lands.
- Later, `share:<n>:mount|unmount`.
- The Linux platform implementations (`platform/linux/actions/SystemActions.cpp`, `ControlKeys.cpp`) write these instead of spawning the scripts. No shared code changes.

**Daemons Nano talks to.**
- `D30net` writes `ctrl_interface=DIR=/run/wpa_supplicant GROUP=nano`.
- `D35bluetooth` installs a D-Bus policy allowing group `nano` to `send_destination="org.bluez"`, or relies on BlueZ's shipped default policy [verify].
- padd `chown`s its tap socket to `root:nano` with mode 0660.

**Storage ownership.**
- **`/storage` (ext4):** a one-time, versioned migration in `S00base`, which runs as root before the session:
  - if `/storage/.config/thimsy/.owner` is missing or older than the current version, `chown -R nano:nano` the Nano-writable trees: `.config/thimsy/data`, `.config/retroarch`, `persist.props`, `emulated/0`, `/storage/thimsy`;
  - `ssh/` and `home/root` stay root.
  - Cost: once, on the 256 MiB data partition, tens of milliseconds *(guess)*. Done before the alpha, it never runs on a user's unit.
- **exFAT and vfat drives:**
  - add `uid=1000,gid=1000,fmask=0022,dmask=0022` to `VolumePolicy::mountOptions`;
  - `noexec,nosuid,nodev` stay;
  - the files then look owned by `nano`, so Nano, RetroArch and its saves can write;
  - nothing on the card changes, so Android sees the same card (the card contract is untouched).
- **ext4 drives formatted on a PC:** they carry their own uids. Choices, in order:
  - an idmapped mount (`mount_setattr` with `MOUNT_ATTR_IDMAP`; ext4 supports it since 5.12) mapping the drive's owner to `nano`;
  - else mount it for Nano read-only with a notice.

  Open question 3.
- **Mount points:** stay root-owned and `chattr +i` (`Session.cpp:684`). The empty-mount-point hazard (`linux_handheld_plan.md:820`) gets better: a non-root Nano cannot create files under an empty, root-owned `/storage/<VOLID>`.

**Games.** RetroArch and PPSSPP inherit `nano`, its groups and its rlimits:
- DRM: after Nano closes `card0`, the game's own open makes it master. When it exits, Nano's fresh open does the same.
- Input: group `input` on padd's pad.
- Audio: dmix, plus group `audio`.
- Saves: `nano`-owned `/storage/.config/retroarch` and the uid-mapped card.
- No capability is needed. A separate `game` uid is possible (open question 2).

**What does not change.**
- The session itself, padd, `rcS`, `rcD`, dropbear and the sleep path stay root.
- Shared Nano code: the Linux-reachable root assumptions (real time, `mlockall`, the backlight write, `SET_MASTER` on its own file) are all covered by rlimits, groups and the 5.8 rule.
- The remaining Linux-compiled writes that will now fail with EPERM:
  - `drop_caches` (`NanoMenuOverlay.cpp:851`);
  - wakelocks;
  - `/vendor` scripts, already refused.

  An audit step (L3) runs the headless E2E as non-root and lists every EPERM and EACCES, to confirm each is harmless or to move it behind a `nanoplat` call.

### 4.3 Android

**Target:** `user system` with `group system graphics input audio media_rw sdcard_rw ext_data_rw everybody external_storage package_info readproc inet`.
- Capabilities `SYS_NICE BLOCK_SUSPEND`, plus `KILL` on the overlay.
- `IPC_LOCK` goes, because the rc's `rlimit memlock` covers `mlockall` without it.
- `DAC_OVERRIDE`, `DAC_READ_SEARCH`, `CHOWN`, `FOWNER`, `SYS_ADMIN`, `SYS_PTRACE`, `SYS_CHROOT` and `SYS_RESOURCE` go once their users have moved.
- This is how SurfaceFlinger runs (`user system`, `capabilities SYS_NICE`).
- uid `system` passes most framework permission checks: activity start and force-stop, settings, dumpsys, appops [I, verify].
- It does not pass the checks that accept only ROOT_UID or SHELL_UID, which the rc names for `cmd wifi` and `cmd bluetooth_manager`.

**Where each root use goes:**

| Root use | Goes to |
|---|---|
| `cmd wifi`, `cmd bluetooth_manager`, `svc wifi`, `cmd connectivity airplane-mode` | NanoNetBridge in system_server (exists; new verbs). The fallback `popen` paths in `platform/firmware/Network.cpp` and `SystemActions.cpp` go |
| `gammaos-net` (`bt`, `wifi configure`, `usb`) | runs as the caller's uid; verify each verb as `system`, else the bridge |
| `nsenter -t 1 -m` for theme downloads | an in-process download (curl or the bridge's `download`), then the unzip in Nano's own tree. Removes `SYS_ADMIN`, `SYS_PTRACE` and `SYS_CHROOT` |
| sysfs writes: backlight, `typec/port*/port_type`, `pmdown_time`, `wake_lock` group, rfkill (already done) | rc `on boot` or `on property:` lines: `chown system system <node>` and `chmod 0664`. Per-device paths come from the device's rc fragment |
| `setclock_*.sh`, cpufreq pinning, Bluetooth LPM, `drop_caches`, MediaProvider `oom_score_adj` | property triggers into `su` services, the pattern `fixedperf` already uses (one service per action), or helper verbs |
| `/data/data/<pkg>` writes, `fchown` to the app uid, `security.selinux` xattr, `restorecon`, `symlinkat` app folders to the card | **`thimsy-nanod`**, a new root native helper (below) |
| `baseparameter` write | `thimsy-nanod` verb `write-baseparameter` |
| `/data/system/nano_*` ownership | an rc `on post-fs-data` step that chowns once, behind a marker property: `chown -R system system /data/system/nano_xmb ...`. The `chown(..., 0, 0)` calls become a `nanoplat` "keep my ownership" call that does nothing |
| DRM master on kernels before 5.8 | release by `close()` (`drmStop` already closes) and re-take by `open()` (`drmReacquireForHome`, `drmResumeRecommit`), in `platform/firmware`'s Display |
| `ctl.start` and `ctl.stop` of `bootanim`, `ctl.restart nano-typec-guard` | per-service `ctl.*$<svc>` property labels, allowed to the Nano domain |

**`thimsy-nanod`.**
- A small C++ init service: `user root`, capabilities `CHOWN DAC_OVERRIDE FOWNER` (installd's shape, smaller).
- It runs in its own domain with `socket thimsy_nanod seqpacket 0660 root system`.
- Nano reaches it from `platform/firmware` only.
- Verbs, each with fixed arguments:
  - `app-write <pkg> <rel> <bytes>`;
  - `app-link <pkg> <rel> <target-on-card>`;
  - `app-relabel <pkg> <rel>`;
  - `write-baseparameter <bytes>`.
- It resolves `<pkg>` to a uid from `packages.list`, refuses `..` and absolute paths, and checks the peer with `SO_PEERCRED` (uid `system`, and the Nano domain once enforcing).
- It replaces the `DAC_OVERRIDE`, `CHOWN` and `FOWNER` the Nano services hold today. Size: about 400 to 700 lines *(guess)*.

**SELinux.**
- Give Nano its own domains, `thimsy_nano` and `thimsy_nanod`, in place of reusing `bootanim`: `file_contexts`, `init_daemon_domain`, `set_prop` for `persist_gammaos_prop` and `gammaos_private_prop`, and `unix_socket_connect(thimsy_nano, thimsy_nanod, thimsy_nanod)`.
- Mark both `permissive` while the device is permissive.
- Collect the denials (they already appear in logcat, as `nano/sync.txt` shows) and fold them in.
- Enforcing mode is a separate decision (open question 6).

**What stays root on Android:** init, vold, sharefs, the `su` helper services, `nano-powerwatch` and `thimsy-nanod`. Launched apps already run under their own uids.

### 4.4 The seam (OA-4)

**New or wider `nanoplat` surface.** Each item is an intent, named by what shared code wants:

| Interface change | Replaces in shared code | Linux implementation | Android implementation |
|---|---|---|---|
| `Power::setBacklight(level)` (fold in `nanoBacklightSet`) | `NanoBacklight.h:99-112` callers; `NanoControlCenter.cpp:1758-1818` | direct sysfs write (group-writable) | Lights HAL, then sysfs |
| `Power::holdAwake(reason, on)` | the 15 `/sys/power/wake_lock` sites | no-op, or ambient `BLOCK_SUSPEND` | the write, with `BLOCK_SUSPEND` |
| `Power::setPerformance(mode)` | `setclock_*.sh` and cpufreq sites; the `fixedperf` property | session `perf:` verb | the `fixedperf` trigger, plus a `setclock` trigger |
| `Files::adoptOwnership(path)` | the `chown(..., 0, 0)` calls | no-op | no-op once the migration ran |
| `AppFiles::{write, link, relabel}` | `NanoBiosUtil`, `NanoBiosWorker`, `NanoSdMove` app-data writes | unsupported (no such apps) | `thimsy-nanod` |
| `AppControl::{start, forceStop, setAppOp}` | about 40 `am`, `monkey` and `appops` sites | unsupported, or the launcher | `am` as uid `system`, or a bridge verb |
| Wider `SystemActions` (time, locale, settings, rotation, saturation, night mode, DND, location) | `settings put`, `cmd uimode`, `cmd notification` and similar sites | store keys or unsupported | the same commands as uid `system` |

- Most rows are moves that OA-4 already asks for (no Android command in shared code). The non-root work only fixes where the Android implementation sends them.
- The Linux column needs almost none of this for the switch itself: Linux works with today's shared code plus section 4.2.

## 5. Knock-on effects

- **Read-only EROFS root and `--all-root`:**
  - keep `--all-root`;
  - `/etc/passwd` and `/etc/group` gain `nano`;
  - nothing on the root needs a non-root owner.
  - File capabilities on EROFS (an xattr) are not needed, since the session raises the one possible ambient capability itself.
- **exFAT has no Unix permissions:**
  - the mount options decide everything, so `uid=`, `gid=` and masks go into `VolumePolicy`;
  - the files on the card are unchanged, so Android interop is unaffected;
  - vfat is the same; ext4 is open question 3.
- **DRM hand-off:**
  - unchanged in shape on Linux: Nano closes and exits, and the game's open makes the game master;
  - a game that inherited Nano's open file would need a capability to set master, but `Launcher.cpp` closes fds 3 and up before `execve` (`platform/linux/Launcher.cpp:115-133`), so it never does;
  - the per-game kiosk compositor (`linux_handheld_plan.md:514`) would run as `nano` through libseat's builtin backend, which needs only the node permissions.
- **Input grabs:**
  - padd keeps grabbing as root;
  - Nano never grabs on Linux;
  - on Android, grabs need only an open file.
- **Audio:**
  - dmix's `ipc_perm 0666` already lets root (`amixer` in the session) and `nano` share it;
  - mixer writes stay in the session.
- **Boot time (3 s cold target, D5):**
  - the uid drop is microseconds;
  - the node policy is under 1 ms *(guess)*;
  - the `/storage` migration is once, before the alpha.
  - One real risk: a race on `card0` permissions adding a poll interval to first frame. Measure it with the existing `nano started` and `nano: first frame` markers (`IMPLEMENTATION.md:860`).
- **Test harness** (`tests/e2e/nano_jail.py`):
  - It runs Nano as uid 0 inside an unprivileged user namespace (`unshare --user --map-root-user`, `:341`).
  - Add a non-root mode: after the jail's mounts, enter a nested user namespace mapping inner uid 1000 to the outer root, then exec Nano.
  - Inside, Nano has no capability over the jail's mounts, so `mount`, `SET_MASTER` on a file it did not open, and writes to read-only binds fail as they would on a device.
  - Fixture files that stand for root-owned files are made 0555 or 0444, since an unprivileged test cannot give them another owner.
  - Scenarios then assert:
    - no EPERM or EACCES in Nano's log outside a known list;
    - saves through `NanoRaConfig` succeed on a migrated tree;
    - requests reach the fake session.
  - `thimsy-session`'s own tests gain cases for the plan (`nano_jail_test.cpp`) and for the game-pid owner check. The real `setresuid` is a device test.

## 6. Phases

Sizes are *(guesses)* in person-days. "Android code" says whether a step changes files compiled into the Android build. Those changes are Android-surface commits, cherry-picked after the AOSP build (OA-11).

| Step | Content | Size | Android code | Depends on |
|---|---|---|---|---|
| **L0** | On the first booting Linux device (with linux_handheld_plan Phase 0): devtmpfs modes of `card0`, `event*` and `snd`; `CONFIG_PM_WAKELOCKS`; RetroArch and PPSSPP taking master after a non-root Nano closes; BlueZ's D-Bus default policy; dmix shared between root and `nano`; an idmapped mount of an ext4 USB drive | 1 to 2 | no | a booting device |
| **L1** | Session and image groundwork, Nano still root: `nano` in `users.table`; the node policy; the props directory group and modes in libthimsy; padd's socket group; `GROUP=nano` in `D30net`; exFAT and vfat `uid`/`gid` options; the `S00base` ownership migration with its version marker; the game-pid owner check | 4 to 6 | no (libthimsy and session are Linux-only) | — |
| **L2** | Seam: session verbs `wifi:`, `bt:` and `perf:`, with `platform/linux` writing them instead of spawning scripts; the scraper started as `nano`; the `EACCES` retry on `card0` | 2 to 3 | no (`platform/linux` only) | L1 |
| **L3** | The switch: rlimits, `setgroups`/`setresgid`/`setresuid`, `no_new_privs`, the jail on every unit; the E2E non-root mode; the EPERM audit, with each finding moved behind `nanoplat` or listed as harmless | 3 to 5 | only if the audit finds a shared site that must move (expected none or a few) | L1, L2 |
| **L4** | Hardening, optional: a separate `game` uid; seccomp on Nano; `hidepid` on `/proc`; OA-3's SSH verbs | 3 to 6 | no | L3 |
| **A0** | Facts on an Android unit, owner-run over adb: kernel version per DRM device; each framework tool as uid 1000 (`su 1000 -c 'am force-stop ...'`, and likewise `settings`, `dumpsys`, `appops`, `pm`, `ime`, `locksettings`, `monkey`, `screencap`, `gammaos-net bt list-bonded`); a denial log baseline for `bootanim` | 1 to 2 | no | a unit |
| **A1** | Quick wins: remove `nsenter`; drop `SYS_ADMIN`, `SYS_PTRACE` and `SYS_CHROOT` (and `IPC_LOCK`) from the rc; quote the three unquoted shell sites | 2 to 3 | yes | — |
| **A2** | Init groundwork: sysfs `chown` lines; the `/data/system/nano_*` migration; per-service `ctl.*` labels; Nano's own `thimsy_nano` domain (permissive) | 3 to 5 | yes (rc and sepolicy in the patch) | A0 |
| **A3** | Seam moves (OA-4 work that the switch needs): Wi-Fi and Bluetooth shell fallbacks to NanoNetBridge; `Power::setBacklight`, `holdAwake` and `setPerformance`; `Files::adoptOwnership`; DRM release by close and re-take by open; `AppControl` for `am` and `appops` | 15 to 25 | yes | A2 |
| **A4** | `thimsy-nanod` with its domain and socket; `AppFiles` and `write-baseparameter` through it | 6 to 10 | yes | A3 |
| **A5** | The switch: `user system`, the reduced capability list; overlay and scraper too; a full regression on a Cube and one DRM device | 4 to 6 | yes | A3, A4 |
| **A6** | Optional, its own decision: SELinux enforcing for Nano's domains | 10+ | yes | A5 |

Totals:
- **Linux, L0 to L3:** about 10 to 16 days.
- **Android, A0 to A5:** about 31 to 51 days. A3 overlaps work OA-4 schedules anyway; A3 is about half of it.

## 7. Risks

- **A missed root assumption on Linux.**
  - Some path that writes as root and fails quietly as `nano`, a save above all: `NanoRaConfig.cpp:179` fails the whole save when it cannot restore the old owner.
  - Mitigation: the migration runs before the first non-root start, and L3's EPERM audit.
- **Late device nodes.** `card0` or the pad appears after Nano starts and before the session sets its group. Mitigation: the netlink handler, plus Nano's retry on `EACCES`.
- **exFAT masks.**
  - `fmask=0022` makes card files non-executable for group and others, which matters only for scripts run from a card, and those are blocked by `noexec` anyway.
  - A user who copies files over SSH as root sees them owned by `nano`, which is fine.
- **ext4 drives from a PC:** idmapped mounts may be unsupported for a family's kernel config; then the drive is read-only to Nano.
- **The session trusts more.** A non-root Nano's requests become an attack surface into the root session. Every verb must stay narrowly parsed, and the game-pid check is mandatory.
- **Android framework checks.**
  - A tool may refuse uid `system` where it accepted root: `locksettings`, `monkey`, some `cmd` services.
  - Mitigation: A0 checks each tool on a unit before A3 decides where it goes.
- **Android merges.** A2 to A5 are Android-surface changes in files upstream also edits (OA-4 and OA-11 accepted that). They must each be cherry-picked after an AOSP build, which serialises them.
- **Little security gain on Android while permissive.** Uid `system` with a permissive device-wide policy stops a few file and capability abuses, not a determined attacker. The value is in the cleaner seam and in readiness for enforcing.
- **DRM on old Android kernels.** On kernels before 5.8, an unprivileged `DROP_MASTER` fails. Code that relies on it instead of `close()` would hold master across a launch and black-screen the app. A3's change and a test on the oldest kernel cover it.

## 8. Recommendation

- **Linux: go.** Do L0 inside linux_handheld_plan's Phase 0, and L1 to L3 before the Linux alpha:
  - before users have data partitions and cards, so the ownership migration never has to run on a user's unit;
  - it removes the accepted residual risk of OA-14 at a cost of about 2 to 3 weeks;
  - it needs no Android build.
  - Mark OA-14 as superseded by this once L3 lands.
- **Android: groundwork now, switch later.**
  - Do A1 (cheap, removes three capabilities) at the next Android-surface batch.
  - Fold A3's moves into the OA-4 seam migration as each area moves.
  - Decide on A4 and A5 once A3 is mostly done and A0's facts are in.
  - Do not plan a fully unprivileged uid for the firmware Nano: that is what android_launcher_plan.md's app build is for. The seam work here (`AppControl`, `AppFiles`, `Power`) is the same seam the launcher's access levels need.

## 9. Questions for the owner

1. **Linux user:** `nano` with uid and gid 1000, groups `video render input audio`? Is any other name or number preferred (ArkOS uses 1000 for `ark`)?
2. **Games on Linux:** run them as `nano` (simplest), or as a separate `game` uid that cannot write Nano's state or session requests?
3. **ext4 drives formatted on a PC:** idmapped mount (if the kernel allows), read-only for Nano, or chown the drive's top folder on first use?
4. **Linux timing:** do L1 to L3 before the alpha, as recommended, or after v1?
5. **OA-5:** may `D30net` and `D35bluetooth` move from "started by `platform/linux`" to "requested by `platform/linux` and run by `thimsy-session`"? That is the platform layer still, but in the session process.
6. **Android goal:** is uid `system` with a root helper the right end state, or should the firmware Nano stay root with the reduced capability list (A1 only)? And is SELinux enforcing a goal at all (A6)?
7. **Android interim:** may A5 keep `DAC_OVERRIDE` for a release, if A4 slips, so the uid switch is not blocked by the helper?

## Sources

Upstream sources are linked inline in section 3. Repository evidence is given as file:line throughout. Research inputs for this plan were four read-only surveys of this repository (shared Nano code, `platform/linux` with the session and image, `platform/firmware` with the rc and patch, and upstream projects), made on 2026-11-26. The base tree's `bootanim.te`, the rc that first starts `gammaos-nano`, and the handlers for `do_launch` and `nano_action` are not in this repository or in `thimsy-os-nano`. Statements that depend on them are marked [I].
