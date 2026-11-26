# Host scenarios for nano-headless

`docs/linux/IMPLEMENTATION.md` D.3. Each `*.jsonl` file here is one scenario: Nano's real code
(`nano_core` over the host platform and the GL stub) booted in a jail and driven like a user would,
checked through what Nano writes and does. `tests/e2e/test_nano_headless.py` plays every file
(`slow` mode; it skips while `nano-headless` is not built, which needs the libraries of
`nano/test/host/DEPS.md`: step G10). The parser and runner are `tests/e2e/nano_scenario.py`; the
jail is `tests/e2e/nano_jail.py`.

The same scenarios run before and after each G7 step (the platform interfaces), so a move behind
`NanoPlatform.h` keeps Nano's observable behaviour (H.5).

## How a scenario runs

1. The runner builds a jail: a temporary root with the Linux image's layout (`/data`,
   `/system/etc`, `/sdcard`, the property store under `/run/thimsy/props` and
   `/storage/.config/thimsy/persist.props`, a fixture drive `/storage/1234-ABCD` with `ROMs/` and
   `ES-DE/`), and `/dev/input/eventN` FIFOs for the fake input devices. Assets come from
   `THIMSY_NANO_ASSETS` (the output of `nano_tree.py export-assets`), fonts from `THIMSY_NANO_FONTS`.
2. It writes the boot properties (the scenario's, plus `persist.gammaos.nano.mlockall=0`), the
   boot files, and nano-headless's host config (`nano/platform/host/HostConfig.h`).
3. It starts `nano-headless` in the jail (user, mount and pid namespaces, chroot; ASan with
   address-space randomisation off), and plays the steps.
4. At the end it sends `quit`, waits for Nano to exit, and fails the scenario if Nano ran a shell
   command no rule answers (the host's shell gate never executes anything: it records the command
   and answers from the rules).

Nano runs on real time; there is no fake clock. Deadlines are generous (`within_ms`, default
10 000 ms; `THIMSY_HEADLESS_TIME_SCALE` multiplies them), so a slow `-O0` ASan build does not flake.
A scenario that needs exact frames turns the frame barrier on and releases frames one by one.

## The format

JSON lines, one step per line. Blank lines and lines starting with `#` or `//` are comments. Each
step is an object with exactly one step key, plus optional modifiers: `within_ms` (the step's
deadline), `hold_ms` (`press`), `device` (`press`: the node, default the first device) and `note`
(ignored; says why). The first step is `boot`, and only the first.

| Step | Value | Does |
|---|---|---|
| `boot` | object, below | sets the jail up and starts Nano |
| `expect` | object: `present_count_at_least` N, `prop` {cond}, `event` {fields}, `exited` bool | waits until all of them hold |
| `expect_prop` | {cond} | waits until the property's value (as the store keeps it) matches |
| `expect_persist` | {cond} on a `persist.*` key | waits until `persist.props` on disk holds it |
| `expect_file` | `{"path": "/data/...", "contains"\|"equals": text}` or `"exists": bool` | waits for a file in the jail (links followed as Nano sees them) |
| `expect_launch` | `{"kind": "Core"\|"Package", "core", "rom_endswith", "package"}` | waits until the fake launcher records a launch with those values (below) |
| `expect_no_shell` | list of command prefixes | fails if any shell command so far starts with one |
| `press` | key name or code | key down, `hold_ms` (default 80), key up, each followed by `SYN_REPORT` |
| `barrier` | bool | turns the frame barrier on (each present then waits for a grant) or off |
| `frames` | N | grants N presents and waits until they happened (barrier on) |
| `capture` | file name | the next present is written to `<jail>/run/thimsy/headless/<name>` (binary PPM, top row first) |
| `wait_ms` | N | sleeps |
| `set_prop` | {key: value} | writes properties as another process would (the store watches) |
| `child_exit` | `{"status": 0, "after_ms": 500}` | the launched game ends: waits for Nano to exit, then starts it again in the same jail, as the session would |
| `quit` | `true` | asks Nano to exit and waits for it |

A condition `{cond}` is `{"key": K}` with exactly one of `"equals": text`, `"contains": text`,
`"endswith": text` or `"nonempty": bool`. An `event` condition matches an event of the current
Nano process whose fields equal the given ones, e.g. `{"ev": "grab", "on": true}`.

`boot` takes:

- `props`: {key: value}. `persist.*` keys go to `persist.props`, `ro.*` to the run dir's `ro/`, the
  rest one file per key, as `thimsy-prop boot` and the store write them.
- `devices`: the fake input devices, default one pad: `{"node": "event0", "name": "Thimsy Gamepad",
  "bus": 6, "vendor": 21576, "product": 1, "version": 1, "keys": [names or codes], "abs": [{"code":
  "HAT0X", "min": -1, "max": 1}, ...]}`. FakeEvdev answers Nano's `EVIOCG*` ioctls from it.
- `display`: `{"width", "height", "refresh_hz", "count", "rotation", "pace"}` (default 1280x720 at
  60 Hz, one display).
- `shell`: `{"rules": [{"match": prefix, "exact": bool, "status": 0..255, "stdout": text}],
  "default_status": 127, "unexpected": "fail"|"allow"}`. A command matches the first rule whose
  `match` begins it (equals it, when `exact`).
- `barrier`: start with the frame barrier on.
- `files`: {absolute jail path: text}, written before Nano starts (ROMs, ES-DE files, state files).
- `record_props`: key prefixes logged as `prop` events (default every key).

Key names: `DPAD_UP`, `DPAD_DOWN`, `DPAD_LEFT`, `DPAD_RIGHT` (the arrow keys, which Nano navigates
by), `A` (`BTN_SOUTH`), `B` (`BTN_EAST`), `X` (`BTN_WEST`), `Y` (`BTN_NORTH`), `L`, `R`, `L2`, `R2`,
`SELECT`, `START`, `MODE`, `L3`, `R3`, `ENTER`, `ESC`, `BACK`, `HOME`, `POWER`, `VOLUME_UP`,
`VOLUME_DOWN`; or a number.

**Launches.** Nano writes the firmware's handshake (`sys.gammaos.nano.launch_app`, `launch_core`,
`launch_rom`, `launch_intent` and the rest) through `commitLaunch`, which since G7-launch-b goes to
the platform's launcher. The host's fake launcher (`nano/platform/host/FakeLauncher.h`) writes the
keys as the firmware does and, at a write of the trigger (`do_launch=1` or
`service.bootanim.nano_retroarch=1`), reads the request from them and records it with a `launch`
event, as the Linux launcher starts a game there; `do_launch` and then `nano_retroarch` for the same
request is one launch. `expect_launch` waits for a `launch` event since the last one it matched:
`kind` is the event's, `core` is found in its core path's file name, `rom_endswith` ends its `rom`
(`launch_rom`, else `launch_game`), `package` equals its `package` (`launch_app`). The handshake's
keys alone, or Nano exiting after writing them, are no launch, and a crash never is. Nano's launch
sites still exit after a launch, so `child_exit` still restarts it.

## nano-headless's events and control lines

The runner talks to `nano-headless` through `/run/thimsy/headless/` (`nano/platform/host/HostControl.h`):

- `events.jsonl`: one JSON object per line, `{"ev": kind, "t_ms": monotonic ms, ...}`. Kinds:
  `start` (pid, config), `splash` (displays, w, h, rotation), `present` (n, slot, w, h, draws),
  `capture` (n, path, ok, w, h), `prop` (key, value: every property write Nano makes), `shell`
  (fn, cmd, rule, status), `grab` (path, on), `drm_stop`, `drm_release`, `pong`, `quit_requested`,
  `control_error`, `exit` (code), `display` (op, ...: the fake display), `launch` (kind, package,
  core, rom, system, intent, trigger: the fake launcher), `launch_quit`.
- `ctl`, a FIFO: `barrier on|off`, `grant N`, `capture PATH`, `ping [TOKEN]`, `quit`.

## Scenarios here

| File | Checks |
|---|---|
| `boot_home.jsonl` | boot to the home: the DRM path, presents, the pad opened and grabbed, a captured frame, no launch |
| `navigate.jsonl` | presses reach Nano; it keeps presenting; nothing launches |
| `launch_game.jsonl` | a system opened and a game launched with a core; the handshake and the ROM file; the game ends and Nano comes back with it played |
| `setting_persisted.jsonl` | Font Size changed: `settings put system font_scale` through the gate and `persist.gammaos.nano.fontscale` in `persist.props`, still there after exit |

The key sequences of `launch_game` and `setting_persisted` are first guesses at the XMB's layout,
marked `TODO(G10)`: they are fixed from the first run's captures and events.
