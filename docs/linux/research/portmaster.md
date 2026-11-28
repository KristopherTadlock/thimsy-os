# PortMaster on Thimsy Linux (OA-22, research, 2026-11-26)

OA-22 replaces Android's "Android Games" system with PortMaster on Linux. This note covers how PortMaster installs and runs ports, what Thimsy Base has to provide for it, and a staged plan. It is research only. Nothing was built, run or downloaded, and no code was changed.

**Updated 2026-11-27** for two owner decisions in `docs/linux/DECISIONS.md`: OA-50 (PortMaster, its runtimes, the ports, their saves and their launch scripts are installed on the system card's ext4 data partition, `/storage`, never on the game library) and OA-45 (a `uinput` group used only for ports). Sections 1, 3.2, 3.4, 3.6, 4, 5, 7, 8 and 9 follow them; section 5 replaces the earlier "card" layout.

Every claim about PortMaster, ROCKNIX or ES-DE cites a source from the list at the end ([S1] and so on). The sources were read on 2026-11-26 from `main` (PortMaster) and `next` (ROCKNIX). Anything without a source is marked *(guess)* or is a Thimsy proposal. Facts about Thimsy come from this repository, with the file named.

## 1. Short answers

- **What PortMaster is.** A Python GUI (`pugwash`) and library (`harbourmaster`) that download port zips and shared runtimes from GitHub releases. It also ships shell glue that each port's launch script sources (`control.txt`, `device_info.txt`, `funcs.txt`, `mod_<CFW>.txt`) and an input mapper, `gptokeyb`. The GUI is MIT [S1][S2]. gptokeyb and gptokeyb2 are GPL-2.0 [S12][S13].
- **What a host firmware provides.** Four things:
  - its own `control.txt` (ROCKNIX writes one; [S16]);
  - a writable PortMaster folder at one of the paths that ports search;
  - a ports folder at `/$directory/ports/`;
  - bash, Python 3 with SDL2, root (or a sudo stand-in) for `mount` of squashfs runtimes and for `/dev/uinput`, and a gamelist hook for the frontend.
- **What blocks Thimsy today.** Five gaps:
  - Space: the system card's data partition, where OA-50 puts PortMaster, is 256 MiB (`linux/board/rk3566/genimage.cfg`, entry 4), too small for most runtimes (section 5). The earlier blocker, game drives mounted `noexec` with no symlinks on exFAT, no longer applies: `/storage` is ext4 and not `noexec` (`linux/board/common/rootfs-overlay/etc/thimsy/init.conf`).
  - The image has no bash and no Python (`linux/configs/fragments/`).
  - Nano and games will run as `nano` with `NO_NEW_PRIVS`, and `/dev/uinput` stays root-only (OA-27, OA-28, `nonroot_nano_plan.md` 4.2). Ports, though, call `$ESUDO mount` [S10] and gptokeyb needs uinput [S5]. OA-45 settles uinput (section 3.4).
  - The ARC-D's panel is portrait, 480x640, rotated 90 degrees (`linux/board/rk3566/devices/anbernic-rg-arc-d.json`), and Thimsy has no compositor.
  - Nothing in Nano knows a Linux `ports` system yet.
- **Recommended shape.** Proposals, except where OA-45 and OA-50 decided (section 9):
  - PortMaster, its runtimes, the ports with their saves, and the launch scripts live on the system card's ext4 data partition, in `/storage/ports/`, never on the game library (OA-50). The read-only root has `/roms/ports` as a link to it.
  - Nano lists the launch scripts there as a local `ports` system. Nothing goes in the library's `ROMs/ports/`.
  - Ports get `/dev/uinput` through a `uinput` group that thimsy-session adds only when it starts a port (OA-45).
  - A small root broker in thimsy-session stands in for `$ESUDO`. It only mounts squashfs runtimes.
  - `CFW_NAME=Thimsy` comes from `/etc/os-release`.
  - Thimsy writes its own MIT `control.txt` and `mod_Thimsy.txt`.
  - PortMaster itself is downloaded at first use, pinned by version and SHA-256, with an offline zip as the alternative. It is not shipped in the image at first.

## 2. How PortMaster works

### 2.1 Install and layout

- **Release contents.** The release is one `PortMaster.zip`. ROCKNIX's recipe fetches `https://github.com/PortsMaster/PortMaster-GUI/releases/download/<version>/PortMaster.zip`, pinned by version with a SHA-256 [S14][S15]. Manual installs instead copy `Install.PortMaster.sh` into the CFW's ports folder, for example `/roms/ports/` on ROCKNIX and `/userdata/roms/ports/` on Knulli [S3].
- **The `PortMaster/` folder holds:**
  - `PortMaster.sh` and `utils/pm_launch.txt`, which start the GUI;
  - `pugwash` and `harbourmaster` (Python);
  - `control.txt`, `device_info.txt`, `funcs.txt`, and `mod_<CFW>.txt` and `libgl_<CFW>.txt` per CFW (ROCKNIX, muOS, knulli, ArkOS, Batocera and others);
  - `gamecontrollerdb.txt`;
  - `gptokeyb` and `gptokeyb2` in aarch64, armhf and x86_64 builds, and `libinterpose.<arch>.so`;
  - `7zzs`, `innoextract`, `xdelta3`, `portmsg`, `sdl2imgshow`, `sdl_resolution` and `oga_controls`;
  - `pylibs/`, `exlibs/`, `libs/`, `runtimes/`, `licenses/`, `autoinstall/`;
  - one folder per CFW with launch hooks (`muos/`, `knulli/`, `trimui/`, `spruce/`, `retrodeck/` and others).

  Source: [S2].
- **Three directories.** harbourmaster works with a tools dir (which holds `PortMaster/`), a ports dir (port folders) and a scripts dir (launch scripts). It takes them from the environment variables `HM_TOOLS_DIR`, `HM_PORTS_DIR` and `HM_SCRIPTS_DIR`. Without them it probes fixed paths:
  - spruce, TrimUI, Batocera's `/userdata/roms/ports`, muOS's `/opt/muos`, `/opt/system/Tools`, `/opt/tools/PortMaster`;
  - `/storage/roms/ports_scripts` (a split layout);
  - `/storage/roms/ports`;
  - RetroDECK's config;
  - otherwise `/roms/ports` [S6].

  Ports and scripts may be separate directories. muOS uses `/mnt/mmc/ports` and `/mnt/mmc/ROMS/Ports` [S6].
- **Runtimes.** Runtimes are `.squashfs` files in `PortMaster/libs/` (`harbour.py` sets `libs_dir = tools_dir / "PortMaster" / "libs"`) [S7][S9]. The aarch64 set includes:
  - Godot and FRT (2 to 4.7, about 7 to 26 MB each);
  - Mono 6.12 (250 MB), DotNet 3.1 to 9 (about 30 MB each), Java (Zulu, 45 to 190 MB);
  - Python 3.11 (32 MB), Ren'Py, Solarus, AGS;
  - Westonpack (53 MB), Mesapack (55 MB), Qt6 EGLFS and others.

  The "popular" bundle is 614.5 MB and "all" is 1971.8 MB [S9]. Offline installs drop the zip or the squashfs into `autoinstall/` or `libs/` [S9].
- **Port catalogue.** Port zips come from `PortMaster-New` and the multiverse repository. Their indexes are `.../releases/latest/download/ports.json` [S6][S11].
- **A port's files.** Each port has `port.json`, `README.md`, `screenshot` (640x480), `gameinfo.xml` (ES metadata), an optional `cover`, a launch script `<Name>.sh`, and a port folder that holds its own `licenses/` [S8][S11].
- **port.json.** It carries:
  - `rtr` (ready to run);
  - `inst` (what the user must add);
  - `runtime`;
  - `reqs` (for example `Ultra`);
  - `arch` and `min_glibc`.

  Brotato, for example, is not ready to run: the user buys the game and copies `brotato.pck` in. It needs `frt_3.7.squashfs` and `reqs: Ultra` [S23].

### 2.2 How a port runs

The packaging template [S8], with a real port's lines [S10][S22]:

```bash
#!/bin/bash
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
# controlfolder: first that exists of /opt/system/Tools/PortMaster, /opt/tools/PortMaster,
#                $XDG_DATA_HOME/PortMaster, else /roms/ports/PortMaster
source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls
GAMEDIR=/$directory/ports/<portfolder>
cd $GAMEDIR
export XDG_DATA_HOME="$CONFDIR"; export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
# runtime ports: $ESUDO mount "$controlfolder/libs/<runtime>.squashfs" "$HOME/godot"
$GPTOKEYB "<exe>" -c "./<port>.gptk" &
pm_platform_helper "$GAMEDIR/<exe>"
./<exe>
pm_finish          # kills gptokeyb, closes messages; runtime ports also $ESUDO umount
```

- **`$directory`.** The host's `control.txt` sets it: ROCKNIX uses `roms`, so `GAMEDIR` is `/roms/ports/<port>` [S16]. Ports therefore expect the ports dir at `/$directory/ports`.
- **Saves and settings.** Ports send them to `$GAMEDIR/conf/` through `XDG_*`, or through `bind_directories`. That function bind-mounts when `PM_CAN_MOUNT=Y` and falls back to a symlink otherwise [S5][S8].
- **exFAT.** PortMaster moved most ports from symlinks to bind mounts so that they run on exFAT. Knulli reports about 25 of 1009 ports still affected [S20].
- **Script language.** Scripts are bash (`#!/bin/bash`) [S22]. The glue calls `mount`, `umount`, `pkill`, `chmod`, `tar`, `systemctl` (ArkOS only) and `glxinfo` (`libgl_ROCKNIX.txt`) [S5][S18][S19].

### 2.3 Platform detection

- **`device_info.txt`** finds the CFW and the hardware, and caches the result as `device_info_<cfw>_<device>.env` in the control folder [S4].
  - **CFW_NAME** comes from marker files, checked in order: RetroDECK, `/etc/rocknix-release`, JELOS, `/opt/muos`, spruce, ArkOS's Plymouth title, Knulli's version files, AmberELEC, Batocera, plumOS, dArkOS. Failing all of those, it takes the first of `OS_NAME`, `NAME` or `ID` from `/etc/os-release` [S4].
  - **DEVICE_NAME** comes from CFW board files, `$HOME/.config/.DEVICE`, `/etc/board` and the like, then DMI, then `/proc/device-tree/model` [S4].
  - **Hardware.** It also exports:
    - `DEVICE_ARCH` and `DEVICE_HAS_ARMHF`/`AARCH64`;
    - `CFW_GLIBC`;
    - `DEVICE_RAM`;
    - `GPU_DRIVER`, by probing `panfrost_dri.so`, the renderD128 driver and so on;
    - `DISPLAY_WIDTH`/`HEIGHT`, from the first connected DRM connector's `modes`;
    - `DISPLAY_ORIENTATION`, from `fbcon=rotate:` or `panel_orientation`, with a portrait panel taken as 270;
    - `ANALOG_STICKS`, `DEVICE_HAS_RUMBLE`, `DEVICE_TOUCH`;
    - `DEVICE_CAPABILITIES`, with tags such as `ultra`, which needs 4 GB or more on most CPUs, and `analog_N` [S4].
- **The Python side** takes `cfw_name` lowercased. It picks a class from `HM_PLATFORMS`:
  - `rocknix`, `jelos`, `muos`, `knulli`, `batocera`, `arkos`, `amberelec`, `trimui`, `spruce`, `retrodeck` and others;
  - `default` (`PlatformBase`) for any other name [S17][S7][S21].
- **What the class does.**
  - `PlatformBase.gamelist_file()` returns `None`, so an unknown CFW gets no frontend gamelist updates [S21].
  - ROCKNIX's class (via JELOS) writes `ports_dir/gamelist.xml`. On first run it copies `control.txt`, `gamecontrollerdb.txt`, `mapper.txt` and `oga_controls*` from `/storage/.config/PortMaster` [S24].
  - `gamelist_add` copies `path`, `name`, `image`, `desc`, `releasedate`, `developer`, `publisher`, `players` and `genre` from each port's `gameinfo.xml`. It makes `image` absolute against `ports_dir` when ports and scripts differ [S21].
- **What Thimsy gets.** `/etc/os-release` with `NAME=Thimsy` gives `CFW_NAME=Thimsy`. Ports then source `mod_Thimsy.txt` if it exists, and harbourmaster uses `PlatformBase`. An upstream `PlatformThimsy` is optional (section 7).

### 2.4 Controls: gptokeyb

- **What it does.** gptokeyb maps an SDL game controller to keyboard and mouse events (or an Xbox 360 pad) for ports that have no pad support. It reads a `.gptk` file of `button = key` lines with mouse, deadzone and hotkey (`*_hk`) settings [S12].
- **Quitting a port.** Start and Select together kill the program named by `-1`/`-k`, through `killall` or `sudo kill -9` (`-sudokill`). `HOTKEY` changes the hotkey, which defaults to Back [S12].
- **gptokeyb2** is a backward-compatible rewrite with an INI config and states. It needs SDL2 and libevdev and runs under `LD_PRELOAD=libinterpose` [S13][S5].
- **Device access.** The SDL controller mapping comes from `SDL_GAMECONTROLLERCONFIG_FILE`, which `get_controls` builds in `/tmp/gamecontrollerdb.txt` [S5][S16]. Upstream `control.txt` runs `chmod 666 /dev/uinput` (and `/dev/tty0`, `/dev/tty1`) when sudo is available [S5]. From that, gptokeyb writes uinput *(inferred, not read in its source)*.

### 2.5 Python, SDL and libraries

- **Python.** `pugwash` is `#!/usr/bin/env python3` and makes no version check. On first start it unpacks `pylibs.zip` into `pylibs/` and `exlibs/` and puts them first on `sys.path` [S25]. The developer guide asks for Python 3.9 or newer [S27].
- **Bundled Python libraries.** `PortMaster/licenses/` lists requests, urllib3, certifi, idna, loguru, ansimarkup, colorama, fastjsonschema, pypng, qrcode and typing_extensions [S26]. The system needs only an interpreter with the standard library: `ctypes`, `ssl`, `zlib`, and `lzma` for the `.tar.xz` fonts *(guess from the imports and the files)*.
- **SDL.** The GUI uses PySDL2 (`import sdl2`, `sdl2.ext`) and also touches `sdl2.sdlttf`, `sdl2.sdlimage` and `sdl2.sdlmixer` [S28]. `pm_launch.txt` sets `PYSDL2_DLL_PATH=/usr/lib` [S18], so the system needs libSDL2, SDL2_ttf, SDL2_image and probably SDL2_mixer in `/usr/lib` *(mixer: guess)*.
- **System libraries.** PortMaster's aim is not to change the OS's libraries: ports bundle what they need [S11]. They still expect glibc (`CFW_GLIBC`, `min_glibc`) [S4][S23], a system SDL2, and GLES/EGL/GBM/DRM, with gl4es for desktop GL (`libgl_default.txt` sets `LIBGL_ES=2`, `LIBGL_GL=21`, `LIBGL_FB=4` (GBM)) [S19].
- **ROCKNIX's extras.** ROCKNIX also installs a "compat" library set in `/usr/lib/compat` for ports. Its `control.txt` puts that set on `LD_LIBRARY_PATH` [S14][S16]. The set's contents were not read.
- **32-bit ports** (`PORT_32BIT=Y`) need an armhf userland in `/usr/lib32` [S8][S29].

## 3. What Thimsy Base needs

### 3.1 Packages and kernel

| Need | Why | Status in `linux/` |
|---|---|---|
| `bash` | port scripts and glue are bash [S22][S5] | absent (BusyBox only) |
| `python3` with ctypes (libffi), ssl, zlib, xz | pugwash and harbourmaster [S25][S27] | absent |
| SDL2, SDL2_ttf, SDL2_image, SDL2_mixer in `/usr/lib` | PySDL2 GUI [S18][S28]; many ports use the system SDL2 | SDL2 with KMSDRM and GLES is in `apps-v1.fragment`; the add-on libraries are absent |
| `unzip`, `tar`, `xz`, `pkill`, `killall`, `mount` with loop | glue and launchers [S5][S15] | BusyBox applets *(guess at which are enabled: check `busybox.fragment`)* |
| Mesa GLES, EGL, GBM, libdrm | ports and gl4es [S19] | present (Panfrost) |
| kernel: `SQUASHFS` with its decompressors; `BLK_DEV_LOOP`; `INPUT_UINPUT` | runtime mounts [S10], gptokeyb | uinput is in the QEMU fragment. The RK3566 config is ROCKNIX `next`'s 7.3.5 `linux.aarch64.conf`, which has `CONFIG_SQUASHFS=y` with `SQUASHFS_ZLIB`, `_LZ4`, `_LZO`, `_XZ` and `_ZSTD` all `=y`, `CONFIG_BLK_DEV_LOOP=y` and `CONFIG_INPUT_UINPUT=y` [S34]. Still add all three to `kernel-critical.conf`, as a guard against a later import dropping them. Which compressor each runtime uses was not read |
| CA certificates, a working clock | PortMaster downloads over HTTPS from GitHub [S6]; certifi is bundled [S26] | D40time `ntpd` exists |

None of this may enter `kernel-critical.conf` or a fragment until the owner agrees on the shape. No armhf userland: Thimsy is aarch64 only, so `DEVICE_HAS_ARMHF=N` and 32-bit ports stay unavailable.

### 3.2 Paths and environment (proposal)

| Path | Holds | Where it really is |
|---|---|---|
| `/storage/ports/` | `PortMaster/` (tools, `libs/` runtimes), the port folders with their saves (`conf/`), and the launch scripts `*.sh` | the system card's ext4 data partition (OA-50), mounted `noatime,nosuid,nodev` with exec allowed (`etc/thimsy/init.conf`); a name thimsy-session must allow under `/storage` (G12), owned by `nano` |
| `/roms/ports` | the path ports expect (`/$directory/ports`) | a symbolic link in the read-only root to `/storage/ports` |
| `$HOME` of `nano` | runtime mount points (`$HOME/godot` and others), symlinks that `bind_directories` makes, `.config` | ext4 `/storage` *(the home path is to be fixed by the non-root work)* |
| `/usr/share/thimsy/portmaster/` | Thimsy's `control.txt`, `mod_Thimsy.txt`, the launcher, a gamecontrollerdb line for the Thimsy pad, a pinned `PortMaster.zip` hash | image |

- **Why `/roms/ports`.** It is the fallback that `PortMaster.sh`, every port script and harbourmaster share [S6][S10][S22]. The controlfolder becomes `/roms/ports/PortMaster` with no extra hook, as on ROCKNIX and AmberELEC [S3][S9].
- **`$XDG_DATA_HOME/PortMaster` must not exist**, or it would win the search [S22].
- **No exec bind mount.** The earlier proposal bind-mounted a folder of the exFAT game drive with exec, because the drive is mounted `noexec`. `/storage` is not `noexec`, so nothing is mounted for PortMaster, and symlinks inside ports work (OA-50).
- **Environment set by the launcher:**
  - `HM_TOOLS_DIR=/roms/ports`, `HM_PORTS_DIR=/roms/ports` and `HM_SCRIPTS_DIR=/roms/ports` [S6], ROCKNIX's one-folder layout;
  - `PM_CAN_MOUNT=N`, so `bind_directories` makes symlinks in `$HOME` instead of bind mounts [S5], which keeps the `pm-bind` verb out (section 3.5) *(guess that a value set before `funcs.txt` is kept: read the line before relying on it)*;
  - `HOME`, `XDG_DATA_HOME` and `SDL_GAMECONTROLLERCONFIG_FILE`.

### 3.3 Platform id

- `/etc/os-release` gets `NAME="Thimsy"`, `ID=thimsy` and a `VERSION_ID`, so `CFW_NAME=Thimsy` and `CFW_VERSION` come from the generic fallbacks [S4].
- `DEVICE_NAME` comes from `/proc/device-tree/model` [S4], which matches Thimsy's `device.json` `model`.
- `mod_Thimsy.txt` (Thimsy's, MIT) defines `pm_platform_helper` and sets Thimsy's overrides, for example `GODOT_OPTS` with the panel size, as `mod_ROCKNIX.txt` does [S29].
- Thimsy's `control.txt` is written from PortMaster's MIT files, not from ROCKNIX's `control.txt`, which is GPL-2.0-or-later [S16]. If it does derive from ROCKNIX's, it goes in `linux/gpl/` under that licence.
- Thimsy's launcher copies its `control.txt` and `mod_Thimsy.txt` into the controlfolder on every start, because a PortMaster self-update may replace them. ROCKNIX's `start_portmaster.sh` does the same copy each time [S15].

### 3.4 Input through thimsy-padd

- **The pad.** padd exposes one pad, `Thimsy Gamepad` (bus virtual, vendor 0x5448, product 0x0001), with the kernel's gamepad layout. Thimsy ships one `gamecontrollerdb` line for it (`IMPLEMENTATION.md` F.2). `get_controls` in Thimsy's `control.txt` writes that line, plus PortMaster's database for external pads, to `/tmp/gamecontrollerdb.txt` and exports `SDL_GAMECONTROLLERCONFIG_FILE` [S16]. Ports then see a known controller with no per-port work.
- **`ANALOG_STICKS`** (2 on the ARC-D) picks each port's `.gptk.<n>` file [S8]. device_info finds it from input ABS capabilities [S4], which padd's pad reports.
- **`HOTKEY`.** gptokeyb's hotkey defaults to Back [S12]. Thimsy keeps that. The session's own quit hotkey is checked against it so that one press does not trigger both *(guess at the interaction; test)*.
- **uinput (OA-45, decided).** gptokeyb needs `/dev/uinput`. A `uinput` group is used only for ports: Nano asks thimsy-session (root) to start a port as `nano` with the extra `uinput` group, through the same request path as OA-30's Wi-Fi and Bluetooth requests. Nano, emulators and other games never get `/dev/uinput`, and OA-27's group list (`video`, `render`, `input`, `audio`) is unchanged.
  - The node is `root:uinput`, mode 0660, set by the session's node policy *(proposal for the mode)*; padd, which runs as root, keeps its own access (`nonroot_nano_plan.md` 4.2).
  - The PortMaster GUI starts without the group: it reads the pad through SDL [S28] and needs no uinput.
  - Not chosen: `nano` itself in a `uinput` group (any `nano` process could make a virtual keyboard), and a padd keyboard mode that reads `.gptk` files (large, and larger for gptokeyb2's state configs [S13]).

### 3.5 Root actions without root (OA-27, OA-28)

- **What fails as `nano`.** Ports run `$ESUDO mount` and `$ESUDO umount` of a squashfs [S10]. Upstream glue also runs `$ESUDO chmod 666 /dev/tty*` and `/dev/uinput`, and `$ESUDO systemctl` [S5]. As `nano`, with `PR_SET_NO_NEW_PRIVS`, sudo cannot work, and a FUSE squashfs mount through a setuid `fusermount` cannot either. squashfs cannot be mounted inside a user namespace *(guess: it is not marked userns-mountable)*.
- **Proposal: `ESUDO` names a Thimsy client, `/usr/libexec/thimsy/pm-esudo`.**
  - `mount <file> <dir>`: allowed only when `<file>` is a regular file directly in `/storage/ports/PortMaster/libs/` (reached as `/roms/ports/PortMaster/libs/`) with a `.squashfs` name, and `<dir>` is a directory that `nano` owns under `nano`'s `$HOME`. Symlinks work on ext4, so the session resolves both paths and refuses a link anywhere in them, rather than trusting the names. The client sends a new session verb, `pm-mount:<file name>:<dir>`. The session mounts it read-only with `nosuid,nodev`, through loop, and records it against the game's process group.
  - `umount <dir or file>`: the matching verb, only for mounts the session recorded.
  - Anything else is run unprivileged, as `nano`. `chmod` of a device node, `systemctl` and the like then fail as they would without sudo, which upstream glue tolerates *(guess per command; test)*.
- **The GUI goes through `ESUDO` too.** `pm_launch.txt` starts the GUI with `$ESUDO ./pugwash $PORTMASTER_CMDS`, after exporting `PYSDL2_DLL_PATH=/usr/lib` and `TERM=linux` [S18]. Upstream sets `ESUDO` to `sudo --preserve-env=...` with a fixed list of variables when sudo works, and to an empty string otherwise; ROCKNIX's `control.txt` sets `ESUDO=""`, because it runs as root [S5][S16]. On Thimsy, `pm-esudo` gets `./pugwash` and its arguments. It must not send that to the session: it runs it in place, as `nano`, in the same working directory and with the whole environment kept (`PYSDL2_DLL_PATH`, `HM_*_DIR`, `HOME`, `XDG_*`, the SDL variables). A kept list like upstream's would drop `PYSDL2_DLL_PATH` and the `HM_*` paths.
- **Cleanup.** When the game's process group ends, the session unmounts that group's runtime mounts, so a crash or a quit hotkey leaves nothing mounted.
- **Validation.** The new verbs follow the plan's rules for requests from an unprivileged writer: a fixed verb list and strictly parsed arguments (`nonroot_nano_plan.md` 4.2).
- **Optional extension.** `bind_directories` with `PM_CAN_MOUNT=Y` would need a `pm-bind` verb as well. Starting with `PM_CAN_MOUNT=N` avoids it (section 3.2).

### 3.6 Launch hand-off from Nano

- **Ports.** A port is a game. Nano's Linux launcher already releases DRM, forks, calls `setsid` and `execve`s the game, and leaves it to thimsy-session (`IMPLEMENTATION.md` B.6, the Launch row). For the `ports` system the launch template is `/bin/bash /roms/ports/<Name>.sh`, run with the working directory `/roms/ports` and the environment of section 3.2, as `nano` with the extra `uinput` group (OA-45), started by thimsy-session at Nano's request. The quit hotkey stops the process group, which includes gptokeyb because it runs in the background of the same script (`&`) *(guess at the group: check that no port calls `setsid`)*.
- **The PortMaster GUI** starts the same way, through a fixed launch of `/usr/share/thimsy/portmaster/start`. That script:
  - installs or updates PortMaster from the pinned zip when it is missing;
  - copies Thimsy's glue (section 3.3);
  - runs `PortMaster.sh`, which sources `pm_launch.txt` and runs `pugwash` [S18].
- **Where the GUI entry lives.** Nano's Linux build shows it as an "app" entry rather than a game in the list. ROCKNIX hides the PortMaster folder in the ports gamelist and puts PortMaster under Tools [S3][S15].
- **Keyboard for the GUI.** gptokeyb is not needed: the GUI reads the pad through SDL [S28].
- **Boot time.** Nothing starts at boot and nothing is mounted for PortMaster: `/roms/ports` is a link in the image. OA-22's boot budget item is unaffected.

## 4. How ports appear in Nano and on the card

- **ES-DE's system.** ES-DE's Linux `ports` system reads `%ROMPATH%/ports` with `.sh` (and `.AppImage`, `.desktop` and others), using the command `%ENABLESHORTCUTS% %EMULATOR_OS-SHELL% %ROM%` [S31].
- **Android ES-DE.** Its `ports` system lists `.app`, `.port`, `.game` and others, but not `.sh` [S32]. Under OA-50 no `.sh` script is on the game card, so the card's `ROMs/ports/` stays Android ES-DE's, and Linux does not read it.
- **Android Games stays separate.** Android's own "Android Games" system is `ROMs/androidgames` with `.app` [S32] (and `mods/nano-systems.txt`). It is untouched, and on Linux it is hidden by OA-22's visibility rule.
- **Nano's Linux system.** Nano gets a local `ports` system: its folder is `/roms/ports` on the system card, not the library's `ROMs/ports`; extensions `.sh`; launch type "port" with the template of section 3.6. It appears when the image ships the port launcher and the folder has scripts, whichever library is in use. `NanoSystemsHeal.cpp:89` already knows the folder name; the local folder is new, and goes through OA-4's Emulators and capability interfaces in `platform/linux`.
- **Gamelists (proposal).** The ports' gamelist and covers stay local too, because they describe local files: a gamelist on the card would list ports the next device does not have. After the GUI exits, Thimsy's launcher (or Nano) merges each installed port's `gameinfo.xml` into `/storage/ports/gamelist.xml`, ROCKNIX's `ports_dir/gamelist.xml` place [S24]:
  - with relative `./<Name>.sh` paths;
  - with image paths relative to the ports folder, so no cover is copied;
  - with unknown tags and the user's own edits kept.

  Nano reads that file for the `ports` system instead of `ES-DE/gamelists/ports/gamelist.xml`, so favourites and play counts of ports are local as well. With `PlatformBase` harbourmaster writes no gamelist [S21], so nothing fights the merge. pugwash's "refresh" markers [S18] can trigger Nano's rescan *(guess at the trigger: Nano rescans after a game exits anyway)*. Where this gamelist lives is Q8.
- **Nothing on the card.** No folder under `ROMs/ports`, no `ES-DE/gamelists/ports` entries and no media are written for ports. No `noload.txt` is needed either [S33].

## 5. Card or image

| Item | Where | Why |
|---|---|---|
| bash, Python 3, SDL2 add-on libraries, kernel options | image | system software; small *(guess: 20 to 40 MB on the root, measured at the first build)* |
| Thimsy glue: `control.txt`, `mod_Thimsy.txt`, launcher, pad db line, pinned zip hash, `pm-esudo` | image | Thimsy's code (MIT), versioned with the release |
OA-50 decides this section: PortMaster is local. Everything it installs is on the system card's ext4 data partition, `/storage`, and nothing of it is on the game library.

| Item | Where | Why |
|---|---|---|
| `PortMaster/` (GUI, gptokeyb, tools) | system card, `/storage/ports/PortMaster/` | it updates itself and must be writable [S25]; GPL binaries are not redistributed by Thimsy (section 6) |
| runtimes (`libs/*.squashfs`) | system card, `/storage/ports/PortMaster/libs/` | hundreds of MB to about 2 GB [S9]; see the space note below |
| ports and their saves (`conf/`) | system card, `/storage/ports/<port>/` | OA-50: they do not follow the library between devices; symlinks inside ports work on ext4 |
| launch scripts | system card, `/storage/ports/*.sh` | Nano lists them as its local `ports` system (section 4) |
| gamelist, covers | system card, `/storage/ports/` (proposal, Q8) | they describe local files (section 4) |
| runtime mount points, symlinks, `.config` | `/storage` (ext4) | `nano`'s home |
| anything | game library | never (OA-50) |

- **Effect on Android.** Nothing of PortMaster is on the game card, so Android and the ES-DE app never see it, and a card round trip cannot touch it.
- **Two cards.** With two libraries (OA-25), PortMaster stays on the boot card's `/storage`, whichever library is chosen.
- **Single-card devices.** The system card is also the game card, but `/storage` is its own ext4 partition, so moving the card to another unit takes the ports along with the rest of `/storage` (as with the local emulator sets, `per_device_config_plan.md` §5.4).
- **Space.** The data partition is 256 MiB on RK3566 (`linux/board/rk3566/genimage.cfg`, entry 4), a fixed size, with the image ending at 2410 MiB and the games partition made at 2411 MiB (W7-CARD-1). PortMaster itself and a few small ports fit *(guess at PortMaster's installed size)*; the "popular" runtime bundle (614.5 MB [S9]) does not. Growing the data partition moves the games partition's start, a layout change for the owner (Q7). No Linux card has shipped, so it needs no migration (OA-33's reasoning).
- **The earlier rule stands.** `linux_handheld_plan.md` 7.4 keeps content that needs POSIX features off FAT and exFAT drives. The earlier proposal relaxed it for one card folder; OA-50 makes that unnecessary.

## 6. Licences

- **PortMaster-GUI** is MIT ("Copyright (c) 2023-2025 Jacob") [S1].
- **gptokeyb and gptokeyb2** are GPL-2.0 [S12][S13].
- **The zip's third-party parts.** The zip bundles others, each with a file in `PortMaster/licenses/`: 7zzs, innoextract, xdelta3, LÖVE, LuaJIT, libmodplug, libogg, libtheora, Noto and DejaVu fonts, and the Python libraries [S26]. Shipping the zip in Thimsy's image would make Thimsy a distributor of the GPL binaries, which needs a source offer for them *(not legal advice)*. ROCKNIX does ship the zip [S14].
  - Stage 1 therefore downloads it on the device from PortMaster's own release, pinned by version and SHA-256, or installs it from a zip in `/storage/ports/PortMaster/autoinstall/` (PortMaster's own autoinstall folder, [S9][S18]). A user without a network puts the zip there over SSH, or *(proposal)* on the card's root as `PortMaster.zip`, which Nano's PortMaster entry offers to copy there; only the zip is read from the card.
  - Shipping it in the image later is an owner decision (Q3).
- **ROCKNIX's glue** (`control.txt`, `start_portmaster.sh`) is GPL-2.0-or-later [S15][S16], and the recipe is GPL-2.0 [S14]. Thimsy writes its own (section 3.3).
- **Ports.** Each port carries its licences in `<port>/licenses/` [S8]. Many are engines that need the user's own game files (`rtr: false`, `inst` says what to copy) [S23]. Thimsy ships no ports and no runtimes, either in the image or in the repository. This matches the repository's rule on assets that may not be redistributed.

## 7. Risks

1. **Space on `/storage`.** 256 MiB holds PortMaster and a few ports, not the runtimes most ports need (section 5, Q7). The exFAT risk this item used to name (ports that use symlinks, about 25 of 1009 per Knulli [S20], and case-insensitive names) no longer applies: OA-50 puts ports on ext4.
2. **Panel rotation on the ARC-D.** The panel is 480x640 portrait, rotated 90 degrees (`anbernic-rg-arc-d.json`). device_info reports a portrait panel as 270 [S4]. ROCKNIX runs ports under sway and calls `portmaster_sway_fullscreen.sh` [S29]. Thimsy has no compositor. Thimsy's SDL rotation patch (`gpl/patches/`, SDL3 through sdl2-compat after I43) cannot reach ports that bundle their own SDL. Westonpack exists for such cases [S9]. Ports may draw sideways until this is solved: the main device risk *(guess at how many)*.
3. **Libraries.** Thimsy differs from ROCKNIX in four ways:
   - libudev-zero, not systemd's udev;
   - ALSA with dmix, not PipeWire, which ROCKNIX's mod file sets up [S29];
   - no `/usr/lib/compat` set [S14];
   - a newer or older SDL2 than ports expect.

   Each needs a test port. Missing libraries are added to the image one by one, never by copying ROCKNIX's compat archive, whose contents and licences were not read.
4. **The privilege broker** is new root code that takes input from scripts downloaded off the internet. It must stay as narrow as section 3.5 says, and be tested like the session's other verbs. Its pass-through path runs the PortMaster GUI itself, so a mistake there either breaks the GUI (a lost variable) or runs it with more rights than `nano` has.
5. **uinput access** (OA-45): a port can make a virtual keyboard and mouse, which can type into whatever reads input while it runs. Only processes the session starts as ports hold the group; Nano, emulators and other games do not, and OA-27's group list is unchanged. The session must drop the group from everything else it starts, and a test checks it.
6. **Self-updates.** PortMaster updates itself and its ports from the network [S25][S6]. A new release can change the glue's contract, for example `funcs.txt`'s version or the device_info cache format [S4][S5]. Thimsy pins the version it installs and tests, and the launcher re-copies Thimsy's glue on every start. Whether to block self-update is open (Q3).
7. **Arbitrary code as `nano`.** Ports are third-party code, run as `nano` from `/storage/ports`. `nano` cannot reach the eMMC or root files (OA-27), and that is the fence. Ports now share the ext4 partition with Nano's own data under `/storage/.config`, which `nano` can write, so a port could change Nano's settings *(guess at the ownership: check against `nonroot_nano_plan.md`'s group-owned config store)*.
8. **RAM.** The ARC-D has 2 GB (`ram_mb: 2048`), so it is not `ultra` [S4]. Ports marked `reqs: Ultra` [S23] may not be offered or may run badly. Thimsy's own `profile_default` names (`small` to `ultra`, `device.schema.json`) are a different scale and must not be mixed with PortMaster's.
9. **A power cut** during an install leaves a half-written port. PortMaster's own install is not atomic *(guess)*. ext4's journal and `e2fsck -p` on a dirty data partition (OA-13) only repair the filesystem, not the port.
10. **Android ES-DE and `.sh` entries.** No longer applies while the ports' gamelist stays local (section 4): Android never sees a ports entry. It comes back if Q8 puts the gamelist on the card.

## 8. Staged plan

1. **Decide** (owner): Q3 to Q8 below; Q1 and Q2 are answered (OA-50, OA-45). Nothing in the image changes before Q3 and Q7 are answered.
2. **Glue, host-tested.** A `thimsy-portmaster` package in `linux/package/` (MIT) with:
   - `control.txt`, `mod_Thimsy.txt` and the start script;
   - `pm-esudo`, with host tests: `mount` and `umount` only for the allowed paths; `$ESUDO ./pugwash --debug`, run as `pm_launch.txt` runs it against a fake `pugwash` that prints its uid, working directory and environment, runs as `nano` with every variable kept; anything else runs unprivileged;
   - the `/etc/os-release` fields;
   - the gameinfo-to-gamelist merge, in Python, tested on the host with fixture ports;
   - a test that runs `device_info.txt` against a fake root with Thimsy's `os-release` and the ARC-D's DRM and input files, and expects `CFW_NAME=Thimsy`, 480x640, two sticks and aarch64. It is read from a pinned PortMaster release and is not committed if its licence does not allow it (MIT, [S1]).
3. **Image.** Add bash, Python 3 (ctypes, ssl, zlib, lzma), SDL2_ttf, SDL2_image and SDL2_mixer. Add `SQUASHFS` with its compressors, `BLK_DEV_LOOP` and `INPUT_UINPUT` to `kernel-critical.conf`. Measure root size and boot time, which should not change.
4. **Session (C++, deferred: nothing can be compiled now):**
   - `/storage/ports` allowed under `/storage` (G12), created at first use and owned by `nano`; the `/roms/ports` link in the image;
   - `Requests`: the `pm-mount` and `pm-umount` verbs, with records per process group and cleanup at game exit; the port launch request that adds the `uinput` group (OA-45, OA-30's request path);
   - the node policy: `/dev/uinput` as `root:uinput` 0660 (OA-45).
5. **Nano (C++, deferred):**
   - the Linux platform's local `ports` system and launch type (folder `/roms/ports`, `.sh`, `bash` template, environment, the launch through the session) through OA-4's Emulators and capability interfaces;
   - the PortMaster app entry;
   - the gamelist merge trigger after the GUI exits, and reading the local ports gamelist (section 4);
   - Android Games hidden on Linux.

   No Android-surface change.
6. **Device test on the ARC-D** (OA-18):
   - install PortMaster offline and online, with the GUI started through `pm_launch.txt` and `pm-esudo`;
   - one native SDL2 port, one Godot/FRT runtime port, one LÖVE port, one gl4es port, one Mono port, and one port that uses symlinks;
   - rotation, audio, the quit hotkey, sleep during a port, a power cut during an install, runtime cleanup, the `uinput` group present in a port and absent in an emulator;
   - a card round trip with Android: nothing of PortMaster on the game card.
7. **Upstream (optional).** Offer PortMaster a `PlatformThimsy` that writes ES-DE's gamelist location, plus a Thimsy marker in device_info. This needs the owner's yes before any contact.

## 9. Open questions for the owner

- **Q1, where ports live:** answered by OA-50: on the system card's ext4 `/storage`, never the game library (option (c) of the earlier list; the exFAT folder and the ext4 image file on the drive were not chosen). The size it raises is Q7.
- **Q2, `/dev/uinput` for gptokeyb:** answered by OA-45: a `uinput` group only for ports the session launches (option (b)).
- **Q3, shipping PortMaster:** download at first use, pinned (recommended), or ship the zip in the image with a source offer for its GPL parts. Separately: allow PortMaster's self-update, or hold it at Thimsy's tested pin.
- **Q4, the GUI entry:** a Nano app entry (recommended), or a hidden item in the ports list as on ROCKNIX.
- **Q5, upstream:** whether to offer `PlatformThimsy` and Thimsy detection to PortMaster.
- **Q6, filtering by device:** whether Nano hides or marks ports whose `reqs` exceed the device (for example `Ultra` on a 2 GB unit), or leaves that to PortMaster.
- **Q7, space for PortMaster (new, from OA-50):** the data partition is 256 MiB. (a) Grow it in the card layout, for example to 4 GiB *(guess at the size)*, which moves the games partition's start (no Linux card has shipped, so no migration); (b) keep 256 MiB and offer only ports without large runtimes.
- **Q8, the ports' gamelist (new):** local, in `/storage/ports/gamelist.xml` (recommended: the ports are local, section 4), or on the card in `ES-DE/gamelists/ports/`, where another device would list ports it does not have.

## Sources

- [S1] PortMaster-GUI licence (MIT): https://github.com/PortsMaster/PortMaster-GUI/blob/main/LICENSE, and the repository front page: https://github.com/PortsMaster/PortMaster-GUI
- [S2] `PortMaster/` folder listing: https://github.com/PortsMaster/PortMaster-GUI/tree/main/PortMaster
- [S3] Installing PortMaster: https://portmaster.games/installation/installing-portmaster/
- [S4] `device_info.txt`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/device_info.txt
- [S5] Upstream `control.txt` and `funcs.txt`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/control.txt, https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/funcs.txt
- [S6] harbourmaster `config.py` (path detection, `HM_*_DIR`, sources): https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/harbourmaster/config.py
- [S7] harbourmaster `harbour.py` (platform choice, `libs_dir`): https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/harbourmaster/harbour.py
- [S8] Port packaging: https://portmaster.games/contribute/porting/packaging/
- [S9] Runtimes: https://portmaster.games/installation/runtimes/
- [S10] A runtime port's script (Brotato): https://raw.githubusercontent.com/PortsMaster/PortMaster-New/main/ports/brotato/Brotato.sh
- [S11] PortMaster-New: https://github.com/PortsMaster/PortMaster-New
- [S12] gptokeyb: https://github.com/PortsMaster/gptokeyb, licence: https://raw.githubusercontent.com/PortsMaster/gptokeyb/master/LICENSE.md
- [S13] gptokeyb2: https://github.com/PortsMaster/gptokeyb2
- [S14] ROCKNIX recipe: https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/apps/portmaster/package.mk
- [S15] ROCKNIX `start_portmaster.sh`: https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/apps/portmaster/scripts/start_portmaster.sh
- [S16] ROCKNIX `control.txt`: https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/packages/apps/portmaster/sources/control.txt
- [S17] harbourmaster platform table: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/harbourmaster/platform/__init__.py
- [S18] `PortMaster.sh` and `utils/pm_launch.txt`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/PortMaster.sh, https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/utils/pm_launch.txt
- [S19] `libgl_default.txt` and `libgl_ROCKNIX.txt`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/libgl_default.txt, https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/libgl_ROCKNIX.txt
- [S20] Knulli, PortMaster and exFAT: https://knulli.org/guides/portmaster-and-exfat/
- [S21] harbourmaster `platform/base.py`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/harbourmaster/platform/base.py
- [S22] A simple port's script (2048): https://raw.githubusercontent.com/PortsMaster/PortMaster-New/main/ports/2048/2048.sh
- [S23] Brotato `port.json`: https://raw.githubusercontent.com/PortsMaster/PortMaster-New/main/ports/brotato/port.json
- [S24] harbourmaster `platform/jelos.py` (JELOS, ROCKNIX): https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/harbourmaster/platform/jelos.py
- [S25] `pugwash` launcher: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pugwash
- [S26] Bundled licences: https://github.com/PortsMaster/PortMaster-GUI/tree/main/PortMaster/licenses
- [S27] Developing PortMaster: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/DEVELOPING.md
- [S28] `pugwash/app.py` imports: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/pylibs/pugwash/app.py
- [S29] `mod_ROCKNIX.txt`: https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/PortMaster/mod_ROCKNIX.txt
- [S30] mount(8), bind mounts and per-mount flags: https://man7.org/linux/man-pages/man8/mount.8.html (cited by the exec bind mount that the 2026-11-27 update dropped)
- [S31] ES-DE Linux `es_systems.xml` (`ports`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/resources/systems/linux/es_systems.xml
- [S32] ES-DE Android `es_systems.xml` (`ports`, `androidgames`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/resources/systems/android/es_systems.xml
- [S33] ES-DE user guide (gamelist location, `noload.txt`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/USERGUIDE.md
- [S34] ROCKNIX RK3566 kernel config, `next` ("Linux/arm64 7.3.5"), read 2026-11-27: https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/devices/RK3566/linux/linux.aarch64.conf
