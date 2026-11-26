"""Host scenarios for nano-headless (docs/linux/IMPLEMENTATION.md D.3): the format's parser and the
runner that plays one in a jail (nano_jail.py). The format is documented in
nano/test/host/scenarios/README.md; this module is its reference.

A scenario is JSON lines: one step per line, `#` and `//` lines and blank lines ignored. The first
step is "boot". The runner writes the jail, the boot properties and nano-headless's host config,
starts it, plays the steps against the event log and the jail's files, and at the end asks Nano to
quit and checks that no shell command went unanswered (the host's shell gate is record-only).
Every nano-headless process must end with status 0 (a signal, such as ASan's abort, or any other
status fails the scenario), and its output must carry no sanitizer report (SANITIZER_MARKERS).

Standard library only; Python 3.8 or later.
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

from tests.e2e.nano_jail import (HEADLESS_DIR, LOG_FILE, PERSIST_FILE, RUN_DIR, Fifos, Jail, JailLayout,
                                 read_persist)

# Linux input codes (linux/input-event-codes.h) under the names scenarios use. The pad buttons are
# the ones Nano's tables read (NanoMenuInput.cpp: a = BTN_SOUTH, b = BTN_EAST, x = BTN_WEST,
# y = BTN_NORTH); the D-pad is reported as the arrow keys, which Nano navigates by.
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
KEYS = {
    "DPAD_UP": 103, "DPAD_DOWN": 108, "DPAD_LEFT": 105, "DPAD_RIGHT": 106,
    "A": 0x130, "B": 0x131, "Y": 0x133, "X": 0x134,
    "L": 0x136, "R": 0x137, "L2": 0x138, "R2": 0x139,
    "SELECT": 0x13A, "START": 0x13B, "MODE": 0x13C, "L3": 0x13D, "R3": 0x13E,
    "ENTER": 28, "ESC": 1, "BACK": 158, "HOME": 172, "POWER": 116, "VOLUME_UP": 115, "VOLUME_DOWN": 114,
}
ABS = {"X": 0x00, "Y": 0x01, "RX": 0x03, "RY": 0x04, "HAT0X": 0x10, "HAT0Y": 0x11}
BUS_VIRTUAL = 6
DEFAULT_PAD = {
    "node": "event0", "name": "Thimsy Gamepad", "bus": BUS_VIRTUAL, "vendor": 21576, "product": 1, "version": 1,
    "keys": ["DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "A", "B", "X", "Y", "L", "R", "L2", "R2",
             "SELECT", "START", "MODE", "L3", "R3"],
    "abs": [{"code": "HAT0X", "min": -1, "max": 1}, {"code": "HAT0Y", "min": -1, "max": 1},
            {"code": "X", "min": -32768, "max": 32767, "flat": 128}, {"code": "Y", "min": -32768, "max": 32767, "flat": 128},
            {"code": "RX", "min": -32768, "max": 32767, "flat": 128}, {"code": "RY", "min": -32768, "max": 32767, "flat": 128}],
}
# Properties every scenario boots with, under its own (D.3): no mlockall (ASan's shadow memory in a
# shared VM), and nothing marks the boot as finished, so readyToRun takes the DRM splash path.
BOOT_PROPS = {"persist.gammaos.nano.mlockall": "0"}
LAUNCH_PREFIX = "sys.gammaos.nano."
DEFAULT_WITHIN_MS = 10000
# Lines of a sanitizer report in nano-headless's output (ASan, LSan, UBSan, TSan).
SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "WARNING: ThreadSanitizer",
                     "runtime error:", "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer")

STEPS = ("boot", "expect", "expect_prop", "expect_persist", "expect_file", "expect_launch", "expect_no_shell",
         "press", "frames", "barrier", "capture", "wait_ms", "set_prop", "child_exit", "quit")
MODIFIERS = ("within_ms", "hold_ms", "device", "note")
BOOT_FIELDS = ("props", "devices", "display", "shell", "barrier", "files", "record_props", "volumes")
EXPECT_FIELDS = ("present_count_at_least", "prop", "event", "exited")


class ScenarioError(ValueError):
    """A malformed scenario file."""


class ScenarioFailure(AssertionError):
    """A scenario that ran and did not hold."""


def key_code(name: Any) -> int:
    if isinstance(name, int) and not isinstance(name, bool) and 0 <= name <= 0x2FF:
        return name
    if isinstance(name, str) and name.upper() in KEYS:
        return KEYS[name.upper()]
    raise ScenarioError("unknown key %r (one of %s, or a code)" % (name, ", ".join(sorted(KEYS))))


def abs_code(name: Any) -> int:
    if isinstance(name, int) and not isinstance(name, bool) and 0 <= name <= 0x3F:
        return name
    if isinstance(name, str) and name.upper() in ABS:
        return ABS[name.upper()]
    raise ScenarioError("unknown axis %r" % (name,))


def _check_prop_cond(c: Any, where: str) -> None:
    if not isinstance(c, dict) or not isinstance(c.get("key"), str):
        raise ScenarioError("%s: needs {\"key\": ..., and one of equals, contains, endswith, nonempty}" % where)
    ops = [k for k in ("equals", "contains", "endswith", "nonempty") if k in c]
    extra = set(c) - {"key", "equals", "contains", "endswith", "nonempty"}
    if len(ops) != 1 or extra:
        raise ScenarioError("%s: exactly one of equals, contains, endswith, nonempty" % where)


def match_value(value: str, cond: Dict[str, Any]) -> bool:
    if "equals" in cond:
        return value == cond["equals"]
    if "contains" in cond:
        return cond["contains"] in value
    if "endswith" in cond:
        return value.endswith(cond["endswith"])
    return bool(value) == bool(cond.get("nonempty"))


def parse(text: str, name: str = "scenario") -> List[Dict[str, Any]]:
    """The steps of a scenario, validated. Raises ScenarioError with the line number."""
    steps: List[Dict[str, Any]] = []
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        where = "%s:%d" % (name, n)
        try:
            step = json.loads(line)
        except ValueError as e:
            raise ScenarioError("%s: not JSON: %s" % (where, e))
        if not isinstance(step, dict):
            raise ScenarioError("%s: a step is a JSON object" % where)
        kinds = [k for k in step if k in STEPS]
        unknown = [k for k in step if k not in STEPS and k not in MODIFIERS]
        if len(kinds) != 1 or unknown:
            raise ScenarioError("%s: one step per line (got %s; unknown %s)" % (where, kinds, unknown))
        kind = kinds[0]
        try:
            _validate(kind, step, where, first=not steps)
        except ScenarioError as e:
            if not str(e).startswith(where + ":"):
                raise ScenarioError("%s: %s" % (where, e))
            raise
        step["_kind"] = kind
        step["_where"] = where
        steps.append(step)
    if not steps:
        raise ScenarioError("%s: empty" % name)
    return steps


def _validate(kind: str, step: Dict[str, Any], where: str, first: bool) -> None:
    v = step[kind]
    if first != (kind == "boot"):
        raise ScenarioError("%s: the first step, and only it, is \"boot\"" % where)
    if "within_ms" in step and (not isinstance(step["within_ms"], int) or step["within_ms"] <= 0):
        raise ScenarioError("%s: within_ms is a positive integer" % where)
    if kind == "boot":
        if not isinstance(v, dict) or set(v) - set(BOOT_FIELDS):
            raise ScenarioError("%s: boot takes %s" % (where, ", ".join(BOOT_FIELDS)))
        props = v.get("props", {})
        if not isinstance(props, dict) or not all(isinstance(k, str) and isinstance(x, str) for k, x in props.items()):
            raise ScenarioError("%s: boot.props maps keys to string values" % where)
        for d in v.get("devices", [DEFAULT_PAD]):
            if not isinstance(d, dict) or not isinstance(d.get("node"), str) or "/" in d["node"]:
                raise ScenarioError("%s: each device has a \"node\" (eventN)" % where)
            for k in d.get("keys", []):
                key_code(k)
            for a in d.get("abs", []):
                abs_code(a.get("code") if isinstance(a, dict) else None)
        shell = v.get("shell", {})
        if not isinstance(shell, dict) or set(shell) - {"rules", "unexpected", "default_status"} or \
                shell.get("unexpected", "fail") not in ("fail", "allow"):
            raise ScenarioError("%s: boot.shell takes rules, default_status and unexpected (fail|allow)" % where)
        for r in shell.get("rules", []):
            if not isinstance(r, dict) or not isinstance(r.get("match"), str):
                raise ScenarioError("%s: each shell rule has a \"match\"" % where)
        files = v.get("files", {})
        if not isinstance(files, dict) or not all(k.startswith("/") and isinstance(x, str) for k, x in files.items()):
            raise ScenarioError("%s: boot.files maps absolute jail paths to their text" % where)
        vols = v.get("volumes", [])
        if not isinstance(vols, list) or not all(isinstance(x, dict) and isinstance(x.get("id"), str) and x["id"]
                                                 for x in vols):
            raise ScenarioError("%s: boot.volumes lists drives, each with an \"id\" (FakeVolumes.h)" % where)
    elif kind == "expect":
        if not isinstance(v, dict) or not v or set(v) - set(EXPECT_FIELDS):
            raise ScenarioError("%s: expect takes %s" % (where, ", ".join(EXPECT_FIELDS)))
        if "prop" in v:
            _check_prop_cond(v["prop"], where)
        if "event" in v and (not isinstance(v["event"], dict) or "ev" not in v["event"]):
            raise ScenarioError("%s: expect.event needs \"ev\"" % where)
    elif kind in ("expect_prop", "expect_persist"):
        _check_prop_cond(v, where)
        if kind == "expect_persist" and not v["key"].startswith("persist."):
            raise ScenarioError("%s: expect_persist is for persist.* keys" % where)
    elif kind == "expect_file":
        if not isinstance(v, dict) or not str(v.get("path", "")).startswith("/") or \
                set(v) - {"path", "contains", "equals", "exists"}:
            raise ScenarioError("%s: expect_file takes path and contains, equals or exists" % where)
    elif kind == "expect_launch":
        if not isinstance(v, dict) or set(v) - {"kind", "core", "rom_endswith", "package"} or \
                v.get("kind", "Core") not in ("Core", "Package"):
            raise ScenarioError("%s: expect_launch takes kind (Core|Package), core, rom_endswith, package" % where)
    elif kind == "expect_no_shell":
        if not isinstance(v, list) or not all(isinstance(x, str) and x for x in v):
            raise ScenarioError("%s: expect_no_shell lists command prefixes" % where)
    elif kind == "press":
        key_code(v)
        if "hold_ms" in step and (not isinstance(step["hold_ms"], int) or step["hold_ms"] < 0):
            raise ScenarioError("%s: hold_ms is a non-negative integer" % where)
    elif kind in ("frames", "wait_ms"):
        if not isinstance(v, int) or isinstance(v, bool) or v < 0:
            raise ScenarioError("%s: %s is a non-negative integer" % (where, kind))
    elif kind == "barrier":
        if not isinstance(v, bool):
            raise ScenarioError("%s: barrier is true or false" % where)
    elif kind == "capture":
        if not isinstance(v, str) or not v or "/" in v or v.startswith("."):
            raise ScenarioError("%s: capture names a file (no folders)" % where)
    elif kind == "set_prop":
        if not isinstance(v, dict) or not all(isinstance(k, str) and isinstance(x, str) for k, x in v.items()):
            raise ScenarioError("%s: set_prop maps keys to string values" % where)
    elif kind == "child_exit":
        if not isinstance(v, dict) or set(v) - {"status", "after_ms"}:
            raise ScenarioError("%s: child_exit takes status and after_ms" % where)
    elif kind == "quit":
        if v not in (True, {}):
            raise ScenarioError("%s: quit is true" % where)


def load(path: str) -> List[Dict[str, Any]]:
    with open(path, "r", encoding="utf-8") as f:
        return parse(f.read(), os.path.basename(path))


def host_config(boot: Dict[str, Any]) -> Dict[str, Any]:
    """nano-headless's host config (nano/platform/host/HostConfig.h) for a boot step."""
    devices = []
    for d in boot.get("devices", [DEFAULT_PAD]):
        dev = {"path": "/dev/input/" + d["node"], "name": d.get("name", ""), "bus": d.get("bus", BUS_VIRTUAL),
               "vendor": d.get("vendor", 0), "product": d.get("product", 0), "version": d.get("version", 1),
               "keys": [key_code(k) for k in d.get("keys", [])], "abs": []}
        for a in d.get("abs", []):
            axis = {"code": abs_code(a["code"])}
            for k in ("value", "min", "max", "fuzz", "flat", "resolution"):
                if k in a:
                    axis[k] = a[k]
            dev["abs"].append(axis)
        devices.append(dev)
    shell = boot.get("shell", {})
    cfg = {
        "events": HEADLESS_DIR + "/events.jsonl",
        "control": HEADLESS_DIR + "/ctl",
        "persist_file": PERSIST_FILE,
        "run_dir": RUN_DIR,
        "log_file": LOG_FILE,
        "display": boot.get("display", {"width": 1280, "height": 720, "refresh_hz": 60}),
        "barrier": bool(boot.get("barrier", False)),
        "devices": devices,
        "shell": {"rules": [{"match": r["match"], "exact": bool(r.get("exact", False)), "status": r.get("status", 0),
                             "stdout": r.get("stdout", "")} for r in shell.get("rules", [])],
                  "default_status": shell.get("default_status", 127)},
        "record_props": boot.get("record_props", [""]),
        "volumes": boot.get("volumes", []),
    }
    return cfg


def input_events(code: int, value: int, now: Optional[float] = None) -> bytes:
    """A key event and its SYN_REPORT as struct input_event records (native layout)."""
    t = time.time() if now is None else now
    sec = int(t)
    usec = int((t - sec) * 1e6)
    fmt = "llHHi"
    return struct.pack(fmt, sec, usec, EV_KEY, code, value) + struct.pack(fmt, sec, usec, EV_SYN, 0, 0)


class Events:
    """Tails nano-headless's event log."""

    def __init__(self, path: str):
        self.path = path
        self.offset = 0
        self.partial = b""
        self.all: List[Dict[str, Any]] = []

    def poll(self) -> List[Dict[str, Any]]:
        try:
            with open(self.path, "rb") as f:
                f.seek(self.offset)
                data = f.read()
        except OSError:
            return []
        self.offset += len(data)
        data = self.partial + data
        lines = data.split(b"\n")
        self.partial = lines.pop()
        new = []
        for raw in lines:
            if not raw.strip():
                continue
            try:
                new.append(json.loads(raw.decode("utf-8", "replace")))
            except ValueError:
                new.append({"ev": "unparsed", "line": raw.decode("utf-8", "replace")})
        self.all.extend(new)
        return new

    def since_start(self) -> List[Dict[str, Any]]:
        """The events of the current nano-headless process (after its last "start")."""
        for i in range(len(self.all) - 1, -1, -1):
            if self.all[i].get("ev") == "start":
                return self.all[i:]
        return list(self.all)


class HeadlessRun:
    """Plays one scenario. `binary` is nano-headless; `work` an empty folder for the jail and logs."""

    def __init__(self, binary: str, steps: List[Dict[str, Any]], work: str, assets: Optional[str] = None,
                 fonts: Optional[str] = None, time_scale: float = 1.0, env: Optional[Dict[str, str]] = None):
        self.binary = os.path.abspath(binary)
        self.steps = steps
        self.work = os.path.abspath(work)
        self.boot = steps[0]["boot"]
        self.time_scale = time_scale
        self.env = dict(env or {})
        devices = [d["node"] for d in self.boot.get("devices", [DEFAULT_PAD])]
        self.layout = JailLayout(os.path.join(self.work, "root"), devices=devices, assets=assets, fonts=fonts,
                                 binary_dir=os.path.dirname(self.binary))
        self.jail = Jail(self.layout, os.path.join(self.work, "jail"))
        self.events = Events(self.layout.path(HEADLESS_DIR + "/events.jsonl"))
        self.fifos: Optional[Fifos] = None
        self.proc: Optional[subprocess.Popen] = None
        self.log_path = os.path.join(self.work, "nano-headless.log")
        self.starts = 0
        # (start number, status) of every process that has ended: Popen's returncode, so -N for
        # a signal. check_exits() fails the scenario on any but 0.
        self.exits: List[Tuple[int, int]] = []
        self.checked_exits = 0
        # Launch commits count only from this index of events.all on: the current process's start,
        # or just past the commit the last expect_launch matched (test-quality-6).
        self.launch_from = 0
        self.start_index = 0

    # -- life cycle
    def prepare(self) -> None:
        self.layout.build()
        props = dict(BOOT_PROPS)
        props.update(self.boot.get("props", {}))
        self.layout.write_props(props)
        for inside, text in self.boot.get("files", {}).items():
            p = self.layout.resolve(inside)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(text)
        with open(self.layout.path(HEADLESS_DIR + "/host.json"), "w", encoding="utf-8") as f:
            json.dump(host_config(self.boot), f, indent=1)
        self.fifos = Fifos(self.layout, control=self.layout.path(HEADLESS_DIR + "/ctl"))

    def start(self) -> None:
        env = {"ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "print_stacktrace=1",
               "GLSTUB_QUIET": "0"}
        env.update(self.env)
        self.starts += 1
        log = open(self.log_path, "ab")
        try:
            self.proc = self.jail.spawn(["/opt/nano/" + os.path.basename(self.binary), "--host-config",
                                         HEADLESS_DIR + "/host.json"], env, aslr_off=True,
                                        name="nano-%d" % self.starts, stdout=log, stderr=subprocess.STDOUT)
        finally:
            log.close()
        self.wait_for(lambda: self._started(), 10000 * self.time_scale, "nano-headless's start event")
        starts = [i for i, e in enumerate(self.events.all) if e.get("ev") == "start"]
        self.start_index = starts[-1] if starts else len(self.events.all)
        self.launch_from = max(self.launch_from, self.start_index)

    def _started(self) -> bool:
        self.events.poll()
        return sum(1 for e in self.events.all if e.get("ev") == "start") >= self.starts

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def _reap(self) -> None:
        """Records the current process's status once it has ended."""
        if self.proc is None or self.proc.poll() is None:
            return
        if not any(n == self.starts for n, _ in self.exits):
            self.exits.append((self.starts, self.proc.returncode))

    def check_exits(self) -> None:
        """Fails on a process that ended other than with status 0, and on a sanitizer report in
        the output (the scenarios run nano-headless with ASan and abort_on_error=1)."""
        self._reap()
        problems = []
        for n, status in self.exits[self.checked_exits:]:
            if status < 0:
                problems.append("nano-headless #%d died of signal %d" % (n, -status))
            elif status != 0:
                problems.append("nano-headless #%d exited with status %d" % (n, status))
        self.checked_exits = len(self.exits)
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                reports = [ln.rstrip() for ln in f if any(m in ln for m in SANITIZER_MARKERS)]
        except OSError:
            reports = []
        if reports:
            problems.append("sanitizer reports in %s:\n    %s" % (self.log_path, "\n    ".join(reports[:10])))
        if problems:
            raise ScenarioFailure("; ".join(problems))

    def stop(self) -> None:
        if self.running() and self.fifos is not None:
            try:
                self.fifos.control("quit")
                self.proc.wait(timeout=10 * self.time_scale)
            except (OSError, subprocess.TimeoutExpired):
                pass
        self._reap()
        if self.running():
            self.proc.kill()
            self.proc.wait()
            self._reap()
        if self.fifos is not None:
            self.fifos.close()

    def play(self) -> None:
        """Prepares the jail, runs every step, stops Nano, and checks the shell commands."""
        self.prepare()
        try:
            self.start()
            for step in self.steps[1:]:
                self.do(step)
            self.check_shell()
            self.stop()   # a quit; a process that does not end on it is killed, and that fails below
            self.check_exits()
        except ScenarioFailure as e:
            raise ScenarioFailure("%s\n%s" % (e, self.context()))
        finally:
            self.stop()

    # -- steps
    def within(self, step: Dict[str, Any]) -> float:
        return step.get("within_ms", DEFAULT_WITHIN_MS) * self.time_scale

    def wait_for(self, cond: Callable[[], bool], ms: float, what: str) -> None:
        end = time.monotonic() + ms / 1000.0
        while True:
            self.events.poll()
            if cond():
                return
            if time.monotonic() >= end:
                raise ScenarioFailure("timed out after %d ms waiting for %s" % (ms, what))
            if self.proc is not None and self.proc.poll() is not None and not cond():
                # One last look: the process may have written its last events just before it ended.
                self.events.poll()
                if cond():
                    return
            time.sleep(0.02)

    def presents(self) -> int:
        return sum(1 for e in self.events.since_start() if e.get("ev") == "present")

    def prop_now(self, key: str) -> str:
        return self.layout.prop(key)

    def do(self, step: Dict[str, Any]) -> None:
        kind = step["_kind"]
        v = step[kind]
        where = step["_where"]
        if kind == "expect":
            self.wait_for(lambda: self._expect(v), self.within(step), "%s %s" % (where, json.dumps(v)))
        elif kind == "expect_prop":
            self.wait_for(lambda: match_value(self.prop_now(v["key"]), v), self.within(step),
                          "%s %s (now %r)" % (where, json.dumps(v), self.prop_now(v["key"])))
        elif kind == "expect_persist":
            p = self.layout.path(PERSIST_FILE)
            self.wait_for(lambda: os.path.exists(p) and match_value(read_persist(p).get(v["key"], ""), v),
                          self.within(step), "%s %s in persist.props" % (where, json.dumps(v)))
        elif kind == "expect_file":
            self.wait_for(lambda: self._file_holds(v), self.within(step), "%s %s" % (where, json.dumps(v)))
        elif kind == "expect_launch":
            self.wait_for(lambda: self._launched(v), self.within(step),
                          "%s launch %s (handshake now %s)" % (where, json.dumps(v), json.dumps(self.launch_state()[0])))
            self.launch_from = len(self.events.all)   # this launch is used up
        elif kind == "expect_no_shell":
            self.events.poll()
            bad = [e["cmd"] for e in self.events.all if e.get("ev") == "shell"
                   and any(str(e.get("cmd", "")).startswith(p) for p in v)]
            if bad:
                raise ScenarioFailure("%s: shell commands that must not run: %s" % (where, bad))
        elif kind == "press":
            self.press(key_code(v), step.get("hold_ms", 80), step.get("device"))
        elif kind == "frames":
            target = self.presents() + v
            self.fifos.control("grant %d" % v)
            self.wait_for(lambda: self.presents() >= target, self.within(step), "%s %d frames" % (where, v))
        elif kind == "barrier":
            self.fifos.control("barrier %s" % ("on" if v else "off"))
        elif kind == "capture":
            self.fifos.control("capture %s" % (HEADLESS_DIR + "/" + v))
        elif kind == "wait_ms":
            time.sleep(v * self.time_scale / 1000.0)
        elif kind == "set_prop":
            self.layout.write_props(v)
        elif kind == "child_exit":
            self.child_exit(v, step)
        elif kind == "quit":
            self.fifos.control("quit")
            self.wait_for(lambda: not self.running(), self.within(step), "%s nano-headless to exit" % where)
            self.check_exits()

    def _expect(self, v: Dict[str, Any]) -> bool:
        if "present_count_at_least" in v and self.presents() < v["present_count_at_least"]:
            return False
        if "prop" in v and not match_value(self.prop_now(v["prop"]["key"]), v["prop"]):
            return False
        if "event" in v:
            want = v["event"]
            if not any(all(e.get(k) == x for k, x in want.items()) for e in self.events.since_start()):
                return False
        if "exited" in v and (not self.running()) != bool(v["exited"]):
            return False
        return True

    def _file_holds(self, v: Dict[str, Any]) -> bool:
        p = self.layout.resolve(v["path"])
        if not os.path.isfile(p):
            return v.get("exists") is False
        if v.get("exists") is False:
            return False
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "equals" in v:
            return text == v["equals"]
        if "contains" in v:
            return v["contains"] in text
        return True

    def launch_state(self) -> Tuple[Dict[str, str], bool]:
        """The launch handshake's keys as Nano last wrote them (the store keeps them across a
        restart), and whether a launch was committed since launch_from: do_launch set to 1, or the
        process ending cleanly (its "exit" event, or status 0) with launch_app set, as the
        firmware's handshake ends. A crash is never a commit (test-quality-3), and an earlier
        process's commit, or one an earlier expect_launch matched, does not count again."""
        keys: Dict[str, str] = {}
        committed = False
        for i, e in enumerate(self.events.all):
            fresh = i >= self.launch_from
            if e.get("ev") == "prop" and str(e.get("key", "")).startswith(LAUNCH_PREFIX):
                k = e["key"][len(LAUNCH_PREFIX):]
                keys[k] = e.get("value", "")
                if fresh and k in ("launch_app", "launch_core", "launch_intent", "launch_rom"):
                    committed = False
                if fresh and k == "do_launch" and e.get("value") == "1":
                    committed = True
            elif fresh and e.get("ev") == "exit" and keys.get("launch_app"):
                committed = True
        if self.proc is not None and self.proc.poll() == 0 and keys.get("launch_app") and \
                self.start_index >= self.launch_from:
            committed = True
        return keys, committed

    def _launched(self, v: Dict[str, Any]) -> bool:
        keys, committed = self.launch_state()
        if not committed:
            return False
        core = keys.get("launch_core", "")
        if v.get("kind", "Core") == "Core" and not core:
            return False
        if v.get("kind") == "Package" and core:
            return False
        if "core" in v and v["core"] not in os.path.basename(core):
            return False
        if "rom_endswith" in v and not keys.get("launch_rom", "").endswith(v["rom_endswith"]):
            return False
        if "package" in v and keys.get("launch_app", "") != v["package"]:
            return False
        return True

    def press(self, code: int, hold_ms: int, device: Optional[str]) -> None:
        node = device or self.layout.devices[0]
        self.fifos.write(node, input_events(code, 1))
        time.sleep(hold_ms / 1000.0)
        self.fifos.write(node, input_events(code, 0))

    def child_exit(self, v: Dict[str, Any], step: Dict[str, Any]) -> None:
        """The launched game ends. Until G7-launch, Nano exits on a launch (the firmware's
        behaviour) and the session restarts it, as init does on Android: so wait for Nano to end,
        wait after_ms, and start it again in the same jail. (After G7-launch the fake launcher's
        pollChild reports the exit instead; this step then writes a control line.)"""
        self.wait_for(lambda: not self.running(), self.within(step), "%s Nano to exit after the launch"
                      % step["_where"])
        self.check_exits()   # the hand-off is a clean exit, never a crash
        time.sleep(v.get("after_ms", 500) * self.time_scale / 1000.0)
        self.start()

    def check_shell(self) -> None:
        self.events.poll()
        if self.boot.get("shell", {}).get("unexpected", "fail") == "allow":
            return
        bad = sorted({str(e.get("cmd")) for e in self.events.all if e.get("ev") == "shell" and e.get("rule") == -1})
        if bad:
            raise ScenarioFailure("shell commands no rule answers (add a boot.shell rule, or "
                                  "\"unexpected\": \"allow\"):\n  " + "\n  ".join(bad[:40]))

    def context(self) -> str:
        self.events.poll()
        tail = self.events.all[-30:]
        try:
            with open(self.log_path, "rb") as f:
                f.seek(0, 2)
                size = f.tell()
                f.seek(max(0, size - 6000))
                log = f.read().decode("utf-8", "replace")
        except OSError:
            log = ""
        return "last events:\n  %s\nnano-headless output (tail):\n%s" % (
            "\n  ".join(json.dumps(e) for e in tail), log)
