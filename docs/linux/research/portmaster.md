# PortMaster on Thimsy Linux (OA-22, research, 2026-11-26)

OA-22 replaces Android's "Android Games" system with PortMaster on Linux. This note covers how PortMaster installs and runs ports, what Thimsy Base has to provide for it, and a staged plan. It is research only. Nothing was built, run or downloaded, and no code was changed.

Every claim about PortMaster, ROCKNIX or ES-DE cites a source from the list at the end ([S1] and so on). The sources were read on 2026-11-26 from `main` (PortMaster) and `next` (ROCKNIX). Anything without a source is marked *(guess)* or is a Thimsy proposal. Facts about Thimsy come from this repository, with the file named.

## 1. Short answers

- **What PortMaster is.** A Python GUI (`pugwash`) and library (`harbourmaster`) that download port zips and shared runtimes from GitHub releases. It also ships shell glue that each port's launch script sources (`control.txt`, `device_info.txt`, `funcs.txt`, `mod_<CFW>.txt`) and an input mapper, `gptokeyb`. The GUI is MIT [S1][S2]. gptokeyb and gptokeyb2 are GPL-2.0 [S12][S13].
- **What a host firmware provides.** Four things:
  - its own `control.txt` (ROCKNIX writes one; [S16]);
  - a writable PortMaster folder at one of the paths that ports search;
  - a ports folder at `/$directory/ports/`;
  - bash, Python 3 with SDL2, root (or a sudo stand-in) for `mount` of squashfs runtimes and for `/dev/uinput`, and a gamelist hook for the frontend.
- **What blocks Thimsy today.** Five gaps:
  - Drives are mounted `noexec` (`linux/src/thimsy-session/src/core/VolumePolicy.cpp:117-127`), and exFAT has no symlinks.
  - The image has no bash and no Python (`linux/configs/fragments/`).
  - Nano and games will run as `nano` with `NO_NEW_PRIVS`, and `/dev/uinput` stays root-only (OA-27, OA-28, `nonroot_nano_plan.md` 4.2). Ports, though, call `$ESUDO mount` [S10] and gptokeyb needs uinput [S5].
  - The ARC-D's panel is portrait, 480x640, rotated 90 degrees (`linux/board/rk3566/devices/anbernic-rg-arc-d.json`), and Thimsy has no compositor.
  - Nothing in Nano knows a Linux `ports` system yet.
- **Recommended shape.** These are proposals, and the owner decides in section 9:
  - PortMaster and its ports live on the ES-DE drive under `Thimsy/PortMaster/`. thimsy-session bind-mounts that folder at `/roms/ports` without `noexec`.
  - Launch scripts go to `ROMs/ports/` so that Nano lists them as the `ports` system.
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
| `/roms/ports` | `PortMaster/` (tools, `libs/` runtimes) and the port folders | bind mount of `<ES-DE drive>/Thimsy/PortMaster/` with `nosuid,nodev` and **exec** (the drive's own mount stays `noexec`); an empty root-owned mount point in the read-only root |
| `<ES-DE drive>/ROMs/ports/*.sh` | launch scripts (`HM_SCRIPTS_DIR`) | the card, as plain files; scripts run as `bash <script>`, so `noexec` does not matter |
| `$HOME` of `nano` | runtime mount points (`$HOME/godot` and others), symlinks that `bind_directories` makes, `.config` | ext4 `/storage` *(the home path is to be fixed by the non-root work)* |
| `/usr/share/thimsy/portmaster/` | Thimsy's `control.txt`, `mod_Thimsy.txt`, the launcher, a gamecontrollerdb line for the Thimsy pad, a pinned `PortMaster.zip` hash | image |

- **Why `/roms/ports`.** It is the fallback that `PortMaster.sh`, every port script and harbourmaster share [S6][S10][S22]. The controlfolder becomes `/roms/ports/PortMaster` with no extra hook, as on ROCKNIX and AmberELEC [S3][S9].
- **`$XDG_DATA_HOME/PortMaster` must not exist**, or it would win the search [S22].
- **Why a bind mount can drop `noexec`.** Each mount point has its own `noexec`, `nosuid` and `nodev`, so a bind mount of one subfolder can allow exec while the drive's own mount stays `noexec`. This is standard Linux behaviour (`mount -o bind`, then `remount,bind,exec`; [S30]). It has not been tried on Thimsy.
- **Environment set by the launcher:**
  - `HM_TOOLS_DIR=/roms/ports`, `HM_PORTS_DIR=/roms/ports` and `HM_SCRIPTS_DIR=<drive>/ROMs/ports` [S6];
  - `PM_CAN_MOUNT=N`, so `bind_directories` makes symlinks in `$HOME` instead of bind mounts [S5] *(guess that a value set before `funcs.txt` is kept: read the line before relying on it)*;
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
- **uinput.** gptokeyb needs `/dev/uinput`. `nonroot_nano_plan.md` 4.2 keeps it root-only for padd. Options for the owner (section 9, Q2):
  - (a) a `uinput` group that `nano` is in, with the node at 0660. This amends OA-27, which fixes `nano`'s groups as `video`, `render`, `input` and `audio`, and the plan's rule that `/dev/uinput` stays root-only (`nonroot_nano_plan.md` 4.2). The risk: any `nano` process can make a virtual keyboard, and Nano already runs as that user.
  - (b) ports launched by the session with an extra `uinput` group that Nano itself does not hold. Nano's groups stay as OA-27 lists them, but games run as `nano` (OA-28) would gain a group for ports only, so the owner should note it there too.
  - (c) a "keyboard mode" in padd that reads `.gptk` files. This is large, and gptokeyb2's state configs make it larger [S13].

### 3.5 Root actions without root (OA-27, OA-28)

- **What fails as `nano`.** Ports run `$ESUDO mount` and `$ESUDO umount` of a squashfs [S10]. Upstream glue also runs `$ESUDO chmod 666 /dev/tty*` and `/dev/uinput`, and `$ESUDO systemctl` [S5]. As `nano`, with `PR_SET_NO_NEW_PRIVS`, sudo cannot work, and a FUSE squashfs mount through a setuid `fusermount` cannot either. squashfs cannot be mounted inside a user namespace *(guess: it is not marked userns-mountable)*.
- **Proposal: `ESUDO` names a Thimsy client, `/usr/libexec/thimsy/pm-esudo`.**
  - `mount <file> <dir>`: allowed only when `<file>` is a regular file directly in `/roms/ports/PortMaster/libs/` with a `.squashfs` name, and `<dir>` is a directory that `nano` owns under `nano`'s `$HOME`. The client sends a new session verb, `pm-mount:<file name>:<dir>`. The session mounts it read-only with `nosuid,nodev`, through loop, and records it against the game's process group.
  - `umount <dir or file>`: the matching verb, only for mounts the session recorded.
  - Anything else is run unprivileged, as `nano`. `chmod` of a device node, `systemctl` and the like then fail as they would without sudo, which upstream glue tolerates *(guess per command; test)*.
- **The GUI goes through `ESUDO` too.** `pm_launch.txt` starts the GUI with `$ESUDO ./pugwash $PORTMASTER_CMDS`, after exporting `PYSDL2_DLL_PATH=/usr/lib` and `TERM=linux` [S18]. Upstream sets `ESUDO` to `sudo --preserve-env=...` with a fixed list of variables when sudo works, and to an empty string otherwise; ROCKNIX's `control.txt` sets `ESUDO=""`, because it runs as root [S5][S16]. On Thimsy, `pm-esudo` gets `./pugwash` and its arguments. It must not send that to the session: it runs it in place, as `nano`, in the same working directory and with the whole environment kept (`PYSDL2_DLL_PATH`, `HM_*_DIR`, `HOME`, `XDG_*`, the SDL variables). A kept list like upstream's would drop `PYSDL2_DLL_PATH` and the `HM_*` paths.
- **Cleanup.** When the game's process group ends, the session unmounts that group's runtime mounts, so a crash or a quit hotkey leaves nothing mounted.
- **Validation.** The new verbs follow the plan's rules for requests from an unprivileged writer: a fixed verb list and strictly parsed arguments (`nonroot_nano_plan.md` 4.2).
- **Optional extension.** `bind_directories` with `PM_CAN_MOUNT=Y` would need a `pm-bind` verb as well. Starting with `PM_CAN_MOUNT=N` avoids it (section 3.2).

### 3.6 Launch hand-off from Nano

- **Ports.** A port is a game. Nano's Linux launcher already releases DRM, forks, calls `setsid` and `execve`s the game, and leaves it to thimsy-session (`IMPLEMENTATION.md` B.6, the Launch row). For the `ports` system the launch template is `/bin/bash <drive>/ROMs/ports/<Name>.sh`, run with the working directory `/roms/ports` and the environment of section 3.2, as `nano`. The quit hotkey stops the process group, which includes gptokeyb because it runs in the background of the same script (`&`) *(guess at the group: check that no port calls `setsid`)*.
- **The PortMaster GUI** starts the same way, through a fixed launch of `/usr/share/thimsy/portmaster/start`. That script:
  - installs or updates PortMaster from the pinned zip when it is missing;
  - copies Thimsy's glue (section 3.3);
  - runs `PortMaster.sh`, which sources `pm_launch.txt` and runs `pugwash` [S18].
- **Where the GUI entry lives.** Nano's Linux build shows it as an "app" entry rather than a game in the list. ROCKNIX hides the PortMaster folder in the ports gamelist and puts PortMaster under Tools [S3][S15].
- **Keyboard for the GUI.** gptokeyb is not needed: the GUI reads the pad through SDL [S28].
- **Boot time.** Nothing starts at boot. The session adds one bind mount when the ES-DE drive is adopted, well under 1 ms *(guess)*, so OA-22's boot budget item is unaffected.

## 4. How ports appear in Nano and on the card

- **ES-DE's system.** ES-DE's Linux `ports` system reads `%ROMPATH%/ports` with `.sh` (and `.AppImage`, `.desktop` and others), using the command `%ENABLESHORTCUTS% %EMULATOR_OS-SHELL% %ROM%` [S31].
- **Android ES-DE.** Its `ports` system lists `.app`, `.port`, `.game` and others, but not `.sh` [S32]. So `.sh` scripts in `ROMs/ports/` are Linux-only, and the Android ES-DE app ignores them.
- **Android Games stays separate.** Android's own "Android Games" system is `ROMs/androidgames` with `.app` [S32] (and `mods/nano-systems.txt`). It is untouched, and on Linux it is hidden by OA-22's visibility rule.
- **Nano's Linux system.** Nano gets a `ports` system: folder `ports`, extensions `.sh`, launch type "port" with the template of section 3.6. It appears when the image ships the port launcher and the folder has scripts. `NanoSystemsHeal.cpp:89` already knows the folder name.
- **Gamelists.** ES-DE ignores `gamelist.xml` files in the ROM folders by default; they belong under `ES-DE/gamelists/<system>/gamelist.xml` [S33]. So the ROCKNIX-style `ports_dir/gamelist.xml` does nothing for Nano or ES-DE. Proposal: after the GUI exits, Thimsy's launcher (or Nano) merges each installed port's `gameinfo.xml` into `ES-DE/gamelists/ports/gamelist.xml`:
  - with relative `./<Name>.sh` paths (OA-8: relative paths);
  - with the cover or screenshot copied to `ES-DE/downloaded_media/ports/covers/<Name>.png`, which keeps it off absolute `/roms/ports/...` image paths;
  - with unknown tags and Android's entries kept.

  With `PlatformBase` harbourmaster writes no gamelist [S21], so nothing fights the merge. pugwash's "refresh" markers [S18] can trigger Nano's rescan *(guess at the trigger: Nano rescans after a game exits anyway)*.
- **Folders under `ROMs/ports`.** None are needed, because the port folders live in `Thimsy/PortMaster/`. No `noload.txt` is needed either [S33].

## 5. Card or image

| Item | Where | Why |
|---|---|---|
| bash, Python 3, SDL2 add-on libraries, kernel options | image | system software; small *(guess: 20 to 40 MB on the root, measured at the first build)* |
| Thimsy glue: `control.txt`, `mod_Thimsy.txt`, launcher, pad db line, pinned zip hash, `pm-esudo` | image | Thimsy's code (MIT), versioned with the release |
| `PortMaster/` (GUI, gptokeyb, tools) | card, `Thimsy/PortMaster/PortMaster/` | it updates itself and must be writable [S25]; GPL binaries are not redistributed by Thimsy (section 6) |
| runtimes (`libs/*.squashfs`) | card | hundreds of MB to about 2 GB [S9] |
| ports and their saves (`conf/`) | card | large, and moves with the user's library between Linux devices |
| launch scripts | card, `ROMs/ports/` | Nano lists them; ES-DE on Linux runs them [S31] |
| gamelist, covers | card, `ES-DE/` | the card contract (OA-8, OA-20) |
| runtime mount points, symlinks, `.config` | `/storage` (ext4) | links never live on a drive (`linux_handheld_plan.md` 7.4 rule 1) |

- **Effect on Android.** Android sees `Thimsy/PortMaster/` as plain files and does nothing with it.
- **Two cards.** With two libraries (OA-25), PortMaster follows the chosen ES-DE drive.
- **A change to an earlier rule.** `linux_handheld_plan.md` 7.4 said that content needing POSIX features (PortMaster ports among it) stays off FAT and exFAT drives, "not in v1". This proposal relaxes that rule for one folder, on the evidence that ports are now made to run on exFAT [S20]. The owner decides (Q1).

## 6. Licences

- **PortMaster-GUI** is MIT ("Copyright (c) 2023-2025 Jacob") [S1].
- **gptokeyb and gptokeyb2** are GPL-2.0 [S12][S13].
- **The zip's third-party parts.** The zip bundles others, each with a file in `PortMaster/licenses/`: 7zzs, innoextract, xdelta3, LÖVE, LuaJIT, libmodplug, libogg, libtheora, Noto and DejaVu fonts, and the Python libraries [S26]. Shipping the zip in Thimsy's image would make Thimsy a distributor of the GPL binaries, which needs a source offer for them *(not legal advice)*. ROCKNIX does ship the zip [S14].
  - Stage 1 therefore downloads it on the device from PortMaster's own release, pinned by version and SHA-256, or installs it from a zip the user drops into `<drive>/Thimsy/PortMaster/autoinstall/` (PortMaster's own autoinstall folder, [S9][S18]).
  - Shipping it in the image later is an owner decision (Q3).
- **ROCKNIX's glue** (`control.txt`, `start_portmaster.sh`) is GPL-2.0-or-later [S15][S16], and the recipe is GPL-2.0 [S14]. Thimsy writes its own (section 3.3).
- **Ports.** Each port carries its licences in `<port>/licenses/` [S8]. Many are engines that need the user's own game files (`rtr: false`, `inst` says what to copy) [S23]. Thimsy ships no ports and no runtimes, either in the image or in the repository. This matches the repository's rule on assets that may not be redistributed.

## 7. Risks

1. **exFAT limits.** Ports that still use symlinks inside their own folders fail on exFAT, about 25 of 1009 per Knulli [S20]. Case-insensitive names may break a few more *(guess)*. Mitigation, later: an ext4 image file on the card for those ports, loop-mounted by the session (Q1 (b)).
2. **Panel rotation on the ARC-D.** The panel is 480x640 portrait, rotated 90 degrees (`anbernic-rg-arc-d.json`). device_info reports a portrait panel as 270 [S4]. ROCKNIX runs ports under sway and calls `portmaster_sway_fullscreen.sh` [S29]. Thimsy has no compositor. Thimsy's SDL rotation patch (`gpl/patches/`, SDL3 through sdl2-compat after I43) cannot reach ports that bundle their own SDL. Westonpack exists for such cases [S9]. Ports may draw sideways until this is solved: the main device risk *(guess at how many)*.
3. **Libraries.** Thimsy differs from ROCKNIX in four ways:
   - libudev-zero, not systemd's udev;
   - ALSA with dmix, not PipeWire, which ROCKNIX's mod file sets up [S29];
   - no `/usr/lib/compat` set [S14];
   - a newer or older SDL2 than ports expect.

   Each needs a test port. Missing libraries are added to the image one by one, never by copying ROCKNIX's compat archive, whose contents and licences were not read.
4. **The privilege broker** is new root code that takes input from scripts downloaded off the internet. It must stay as narrow as section 3.5 says, and be tested like the session's other verbs. Its pass-through path runs the PortMaster GUI itself, so a mistake there either breaks the GUI (a lost variable) or runs it with more rights than `nano` has.
5. **uinput access** (Q2) weakens the non-root plan's "uinput belongs to padd" rule, and option (a) changes OA-27's group list.
6. **Self-updates.** PortMaster updates itself and its ports from the network [S25][S6]. A new release can change the glue's contract, for example `funcs.txt`'s version or the device_info cache format [S4][S5]. Thimsy pins the version it installs and tests, and the launcher re-copies Thimsy's glue on every start. Whether to block self-update is open (Q3).
7. **Arbitrary code as `nano`.** Ports are third-party code with exec on one card folder. `nano` cannot reach the eMMC or root files (OA-27), and that is the fence.
8. **RAM.** The ARC-D has 2 GB (`ram_mb: 2048`), so it is not `ultra` [S4]. Ports marked `reqs: Ultra` [S23] may not be offered or may run badly. Thimsy's own `profile_default` names (`small` to `ultra`, `device.schema.json`) are a different scale and must not be mixed with PortMaster's.
9. **Unclean card removal** during an install leaves a half-written port. PortMaster's own install is not atomic *(guess)*. D10cards' fsck on a dirty exFAT (OA-13) only repairs the filesystem, not the port.
10. **Android ES-DE and `.sh` entries.** If the Android ES-DE app or Android Nano rewrites `ES-DE/gamelists/ports/gamelist.xml`, it may drop entries for `.sh` files it does not list *(guess; the interop audit found that unknown tags are kept, not unknown entries)*. A round-trip test is needed (OA-8).

## 8. Staged plan

1. **Decide** (owner): Q1 to Q6 below. Nothing in the image changes before Q1 to Q3 are answered.
2. **Glue, host-tested.** A `thimsy-portmaster` package in `linux/package/` (MIT) with:
   - `control.txt`, `mod_Thimsy.txt` and the start script;
   - `pm-esudo`, with host tests: `mount` and `umount` only for the allowed paths; `$ESUDO ./pugwash --debug`, run as `pm_launch.txt` runs it against a fake `pugwash` that prints its uid, working directory and environment, runs as `nano` with every variable kept; anything else runs unprivileged;
   - the `/etc/os-release` fields;
   - the gameinfo-to-gamelist merge, in Python, tested on the host with fixture ports;
   - a test that runs `device_info.txt` against a fake root with Thimsy's `os-release` and the ARC-D's DRM and input files, and expects `CFW_NAME=Thimsy`, 480x640, two sticks and aarch64. It is read from a pinned PortMaster release and is not committed if its licence does not allow it (MIT, [S1]).
3. **Image.** Add bash, Python 3 (ctypes, ssl, zlib, lzma), SDL2_ttf, SDL2_image and SDL2_mixer. Add `SQUASHFS` with its compressors, `BLK_DEV_LOOP` and `INPUT_UINPUT` to `kernel-critical.conf`. Measure root size and boot time, which should not change.
4. **Session (C++, deferred: nothing can be compiled now):**
   - `VolumePolicy` and `Mounter`: bind `<ES-DE drive>/Thimsy/PortMaster` at `/roms/ports` with `nosuid,nodev,exec` after the drive mounts, and release it before an eject;
   - `Requests`: the `pm-mount` and `pm-umount` verbs, with records per process group and cleanup at game exit;
   - the node policy: `/dev/uinput` as Q2 decides.
5. **Nano (C++, deferred):**
   - the Linux platform's `ports` system and launch type (folder `ports`, `.sh`, `bash` template, environment) through OA-4's Emulators and capability interfaces;
   - the PortMaster app entry;
   - the gamelist merge trigger after the GUI exits;
   - Android Games hidden on Linux.

   No Android-surface change.
6. **Device test on the ARC-D** (OA-18):
   - install PortMaster offline and online, with the GUI started through `pm_launch.txt` and `pm-esudo`;
   - one native SDL2 port, one Godot/FRT runtime port, one LÖVE port, one gl4es port, one Mono port;
   - rotation, audio, the quit hotkey, sleep during a port, card removal, runtime cleanup;
   - a card round trip with Android (the `ports` gamelist and `Thimsy/PortMaster/` untouched).
7. **Upstream (optional).** Offer PortMaster a `PlatformThimsy` that writes ES-DE's gamelist location, plus a Thimsy marker in device_info. This needs the owner's yes before any contact.

## 9. Open questions for the owner

- **Q1, where ports live:**
  - (a) `<drive>/Thimsy/PortMaster/` on the exFAT drive, bind-mounted with exec at `/roms/ports` (recommended: plain files, carried with the card, as on Knulli and muOS);
  - (b) an ext4 image file on the drive, loop-mounted. Symlinks work, but the file is opaque to a PC and needs preallocation and its own fsck;
  - (c) internal `/storage`, which is too small at 256 MiB *(guess at the size)*.
- **Q2, `/dev/uinput` for gptokeyb:** (a) a `uinput` group for `nano`, which amends OA-27's fixed group list (`video`, `render`, `input`, `audio`); (b) only for ports the session launches; (c) a padd keyboard mode.
- **Q3, shipping PortMaster:** download at first use, pinned (recommended), or ship the zip in the image with a source offer for its GPL parts. Separately: allow PortMaster's self-update, or hold it at Thimsy's tested pin.
- **Q4, the GUI entry:** a Nano app entry (recommended), or a hidden item in the ports list as on ROCKNIX.
- **Q5, upstream:** whether to offer `PlatformThimsy` and Thimsy detection to PortMaster.
- **Q6, filtering by device:** whether Nano hides or marks ports whose `reqs` exceed the device (for example `Ultra` on a 2 GB unit), or leaves that to PortMaster.

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
- [S30] mount(8), bind mounts and per-mount flags: https://man7.org/linux/man-pages/man8/mount.8.html
- [S31] ES-DE Linux `es_systems.xml` (`ports`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/resources/systems/linux/es_systems.xml
- [S32] ES-DE Android `es_systems.xml` (`ports`, `androidgames`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/resources/systems/android/es_systems.xml
- [S33] ES-DE user guide (gamelist location, `noload.txt`): https://gitlab.com/es-de/emulationstation-de/-/raw/master/USERGUIDE.md
- [S34] ROCKNIX RK3566 kernel config, `next` ("Linux/arm64 7.3.5"), read 2026-11-27: https://raw.githubusercontent.com/ROCKNIX/distribution/next/projects/ROCKNIX/devices/RK3566/linux/linux.aarch64.conf
