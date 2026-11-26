"""The host E2E of Nano (docs/linux/IMPLEMENTATION.md D.3): nano-headless in a jail, driven by the
scenarios in nano/test/host/scenarios/.

- GlstubGen, ScenarioFormat and JailPlan are pure Python and run everywhere (quick).
- JailLayout builds a jail's files (POSIX: links and FIFOs; quick).
- JailRun runs a probe in a jail without Nano: the read-only binds, the writable data tree, the
  FIFO input devices, /proc (slow; skips without unprivileged user namespaces).
- NanoHeadless plays every scenario (slow; skips unless nano-headless is built: THIMSY_NANO_HEADLESS
  or ~/.cache/thimsy/nano-host/headless/nano-headless, which `make -C nano/test/host headless`
  builds once the libraries of nano/test/host/DEPS.md are installed, step G10).

Everything skips where there is no nano/ (nano-144 before the merge back).
"""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import time
import unittest

from tests import util
from tests.e2e import nano_jail, nano_scenario
from tests.e2e.nano_jail import Fifos, Jail, JailLayout
from tests.e2e.nano_scenario import ScenarioError

NANO = os.path.join(util.ROOT, "nano")
HAVE_NANO = os.path.isdir(NANO)
SCENARIOS = os.path.join(NANO, "test", "host", "scenarios")
GEN_PY = os.path.join(NANO, "test", "host", "glstub", "gen.py")
POSIX = os.name == "posix"
BINARY = os.environ.get("THIMSY_NANO_HEADLESS") or os.path.expanduser(
    "~/.cache/thimsy/nano-host/headless/nano-headless")


def load_gen():
    spec = importlib.util.spec_from_file_location("glstub_gen", GEN_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@unittest.skipUnless(HAVE_NANO, "no nano/ in this checkout")
class GlstubGen(util.TestCase):
    def setUp(self):
        super().setUp()
        self.gen = load_gen()

    def test_parse_nm_formats(self):
        bsd = ("\nmain.o:\n                 U glClear\n0000000000000010 T glGenTextures\n"
               "                 U eglSwapBuffers\n                 U memcpy\n0000000000000000 W eglGetDisplay\n")
        u, d = self.gen.parse_nm(bsd)
        self.assertEqual(u, {"glClear", "eglSwapBuffers", "memcpy"})
        self.assertEqual(d, {"glGenTextures", "eglGetDisplay"})
        posix = "glClear U\nglGenTextures T 0000000000000010 0000000000000020\nfoo@GLIBC_2.2.5 U\n"
        u, d = self.gen.parse_nm(posix)
        self.assertEqual(u, {"glClear", "foo"})
        self.assertEqual(d, {"glGenTextures"})

    def test_generate(self):
        text = self.gen.generate({"glClear", "eglSwapBuffers", "glGenTextures", "glstub_draw_count", "memcpy",
                                  "glEGLImageTargetTexture2DOES", "global_thing"}, {"glGenTextures"})
        lines = [l for l in text.split("\n") if l.startswith("GLSTUB_GENERIC")]
        self.assertEqual(lines, ["GLSTUB_GENERIC_EGL(eglSwapBuffers)", "GLSTUB_GENERIC_GL(glClear)",
                                 "GLSTUB_GENERIC_GL(glEGLImageTargetTexture2DOES)"])
        self.assertIn('#include "glstub.h"', text)
        self.assertIn("const unsigned glstub_generic_count = 3u;", text)
        self.assertIn('    "glClear",', text)

    def test_cli_with_saved_nm_output(self):
        u = self.write("u.txt", "                 U glClear\n                 U glFlush\n")
        d = self.write("d.txt", "0000000000000000 T glFlush\n")
        out = self.path("gen.c")
        self.assertEqual(self.gen.main(["--out", out, "--undefined", u, "--defined", d]), 0)
        with open(out, "r", encoding="utf-8") as f:
            text = f.read()
        self.assertIn("GLSTUB_GENERIC_GL(glClear)", text)
        self.assertNotIn("GLSTUB_GENERIC_GL(glFlush)", text)

    def test_glue_names_are_not_generated(self):
        """Every function glue.c defines is one gen.py would otherwise stub (the regex matches it)."""
        import re
        with open(os.path.join(NANO, "test", "host", "glstub", "glue.c"), "r", encoding="utf-8") as f:
            glue = f.read()
        defined = set(re.findall(r"^[A-Za-z][\w \*]*?\b((?:gl|egl)[A-Z]\w*)\(", glue, re.M))
        self.assertIn("glReadPixels", defined)
        self.assertIn("eglChooseConfig", defined)
        for name in defined:
            self.assertRegex(name, self.gen.GL_NAME)


@unittest.skipUnless(HAVE_NANO, "no nano/ in this checkout")
class ScenarioFormat(util.TestCase):
    def test_every_scenario_parses(self):
        files = sorted(f for f in os.listdir(SCENARIOS) if f.endswith(".jsonl"))
        self.assertGreaterEqual(len(files), 4)
        for f in files:
            steps = nano_scenario.load(os.path.join(SCENARIOS, f))
            self.assertEqual(steps[0]["_kind"], "boot", f)
            cfg = nano_scenario.host_config(steps[0]["boot"])
            json.dumps(cfg)
            self.assertTrue(cfg["devices"], f)
            self.assertEqual(cfg["devices"][0]["path"], "/dev/input/event0")

    def test_the_design_scenarios_exist(self):
        """D.3's first scenarios: boot to the home, navigate, launch a game, a setting persisted."""
        kinds = {}
        for f in os.listdir(SCENARIOS):
            if f.endswith(".jsonl"):
                kinds[f] = {s["_kind"] for s in nano_scenario.load(os.path.join(SCENARIOS, f))}
        self.assertIn("expect_launch", kinds["launch_game.jsonl"])
        self.assertIn("child_exit", kinds["launch_game.jsonl"])
        self.assertIn("expect_persist", kinds["setting_persisted.jsonl"])
        self.assertIn("capture", kinds["boot_home.jsonl"])
        self.assertIn("press", kinds["navigate.jsonl"])

    def test_errors(self):
        bad = [
            "",                                                      # empty
            '{"press": "A"}',                                        # no boot first
            '{"boot": {}}\n{"boot": {}}',                            # boot twice
            '{"boot": {}}\n{"press": "NOPE"}',                       # unknown key
            '{"boot": {}}\n{"press": "A", "expect": {}}',            # two steps on a line
            '{"boot": {}}\n{"jump": 1}',                             # unknown step
            '{"boot": {"props": {"a": 1}}}',                         # non-string value
            '{"boot": {"shell": {"unexpected": "maybe"}}}',
            '{"boot": {"files": {"relative": "x"}}}',
            '{"boot": {}}\n{"expect_prop": {"key": "k", "equals": "a", "contains": "b"}}',
            '{"boot": {}}\n{"expect_persist": {"key": "sys.x", "equals": "1"}}',
            '{"boot": {}}\n{"frames": -1}',
            '{"boot": {}}\n{"capture": "../x.ppm"}',
            '{"boot": {}}\n{"expect": {"present_count_at_least": 1}, "within_ms": 0}',
            '{"boot": {}}\nnot json',
            '{"boot": {"devices": [{"node": "event0", "keys": ["WARP"]}]}}',
        ]
        for text in bad:
            with self.assertRaises(ScenarioError, msg=text):
                nano_scenario.parse(text)

    def test_comments_and_line_numbers(self):
        steps = nano_scenario.parse('# c\n\n// c\n{"boot": {}}\n{"wait_ms": 5}', "t")
        self.assertEqual([s["_kind"] for s in steps], ["boot", "wait_ms"])
        self.assertEqual(steps[1]["_where"], "t:5")
        with self.assertRaises(ScenarioError) as cm:
            nano_scenario.parse('{"boot": {}}\n\n{"press": 3.5}', "t")
        self.assertIn("t:3", str(cm.exception))

    def test_host_config(self):
        boot = nano_scenario.parse('{"boot": {"devices": [{"node": "event1", "name": "P", "keys": ["A", 28], '
                                   '"abs": [{"code": "HAT0X", "min": -1, "max": 1}]}], "shell": {"rules": '
                                   '[{"match": "am ", "status": 0}], "default_status": 1}, "barrier": true}}')[0]["boot"]
        cfg = nano_scenario.host_config(boot)
        self.assertEqual(cfg["devices"][0]["path"], "/dev/input/event1")
        self.assertEqual(cfg["devices"][0]["keys"], [0x130, 28])
        self.assertEqual(cfg["devices"][0]["abs"], [{"code": 0x10, "min": -1, "max": 1}])
        self.assertEqual(cfg["shell"]["rules"][0]["match"], "am ")
        self.assertEqual(cfg["shell"]["default_status"], 1)
        self.assertTrue(cfg["barrier"])
        self.assertEqual(cfg["persist_file"], "/storage/.config/thimsy/persist.props")
        # The defaults: one pad with Nano's buttons and the arrow keys.
        pad = nano_scenario.host_config({})["devices"][0]
        self.assertIn(0x130, pad["keys"])
        self.assertIn(106, pad["keys"])

    def test_match_value(self):
        self.assertTrue(nano_scenario.match_value("abc", {"equals": "abc"}))
        self.assertTrue(nano_scenario.match_value("abc", {"contains": "b"}))
        self.assertTrue(nano_scenario.match_value("abc", {"endswith": "bc"}))
        self.assertTrue(nano_scenario.match_value("abc", {"nonempty": True}))
        self.assertTrue(nano_scenario.match_value("", {"nonempty": False}))
        self.assertFalse(nano_scenario.match_value("", {"nonempty": True}))

    @unittest.skipUnless(POSIX and struct.calcsize("l") == 8, "struct input_event's 64-bit layout")
    def test_input_events(self):
        data = nano_scenario.input_events(0x130, 1, now=12.5)
        self.assertEqual(len(data), 48)
        sec, usec, typ, code, value = struct.unpack("llHHi", data[:24])
        self.assertEqual((sec, usec, typ, code, value), (12, 500000, 1, 0x130, 1))
        self.assertEqual(struct.unpack("llHHi", data[24:])[2:], (0, 0, 0))


def fake_host(test):
    """A host root with the folders the plan looks at: usr/{bin,lib}, a merged-/usr link where links
    work, and etc."""
    host = test.path("host")
    for d in ("usr/bin", "usr/lib", "usr/share/zoneinfo", "etc", "dev"):
        os.makedirs(os.path.join(host, d), exist_ok=True)
    try:
        os.symlink("usr/bin", os.path.join(host, "bin"))
    except (OSError, NotImplementedError):
        os.makedirs(os.path.join(host, "bin"), exist_ok=True)
    os.makedirs(os.path.join(host, "lib64"), exist_ok=True)  # a real top-level folder
    return host


@unittest.skipUnless(HAVE_NANO, "no nano/ in this checkout")
class JailPlan(util.TestCase):
    def test_mount_plan(self):
        host = fake_host(self)
        layout = JailLayout(self.path("root"), host=host, assets=self.path("assets"), binary_dir=self.path("bin"))
        plan = layout.mount_plan()
        self.assertEqual((plan[0].kind, plan[0].target), ("tmpfs", "/usr"))
        targets = [(m.kind, m.target) for m in plan]
        self.assertIn(("bind", "/usr/bin"), targets)
        self.assertIn(("bind", "/usr/lib"), targets)
        self.assertIn(("bind", "/usr/share/zoneinfo"), targets)
        self.assertNotIn(("bind", "/usr/lib32"), targets)          # absent on this host
        self.assertIn(("bind", "/lib64"), targets)                 # a real top-level folder is bound
        self.assertIn(("bind", "/etc"), targets)
        self.assertIn(("bind", "/dev/null"), targets)
        self.assertIn(("bind", "/opt/nano"), targets)
        for m in plan:
            if m.kind == "bind":
                self.assertTrue(m.readonly, m)                    # every bind is read-only
                self.assertNotIn(m.source.rstrip("/\\"), ("", "/", os.path.expanduser("~")))
        # proc after the binds; the asset folder made on the tmpfs only after them, then bound.
        i_proc = targets.index(("proc", "/proc"))
        i_mkdir = targets.index(("mkdir", "/usr/share/thimsy/system/etc"))
        i_assets = targets.index(("bind", "/usr/share/thimsy/system/etc"))
        self.assertLess(i_proc, i_mkdir)
        self.assertLess(i_mkdir, i_assets)
        self.assertTrue(all(k != "bind" or t.startswith("/usr/share") or i < i_proc
                            for i, (k, t) in enumerate(targets)))

    def test_setup_script(self):
        host = fake_host(self)
        with self.assertRaises(ValueError):
            JailLayout(self.path("ro ot"), host=host)       # mountinfo would escape the space
        layout = JailLayout(self.path("root$x"), host=host)
        script = layout.setup_script(["/opt/nano/nano-headless", "--host-config", "/run/x.json"],
                                     {"A": "1 2"}, aslr_off=False)
        self.assertTrue(script.startswith("set -eu\n"))
        self.assertIn("R='%s'" % layout.root, script)              # quoted: the path has a $
        self.assertIn("mount -t tmpfs -o mode=0755 tmpfs \"$R\"/usr", script)
        self.assertIn("mount --rbind ", script)
        self.assertIn("ro_tree \"$R\"/etc", script)
        self.assertIn("mount -o remount,bind,ro \"$R\"/dev/null 2>/dev/null || true", script)
        self.assertIn("mount -t proc", script)
        last = script.strip().split("\n")[-1]
        self.assertTrue(last.startswith('exec chroot "$R" /usr/bin/env -i'), last)
        self.assertIn("'A=1 2'", last)
        self.assertTrue(last.endswith("/opt/nano/nano-headless --host-config /run/x.json"), last)

    def test_props_round_trip(self):
        layout = JailLayout(self.path("root"))
        os.makedirs(layout.path("/run/thimsy/props/ro"))
        os.makedirs(layout.path("/storage/.config/thimsy"))
        layout.write_props({"persist.b": "two\nlines", "persist.a": "back\\slash", "sys.x": "1", "ro.y": "z"})
        layout.write_props({"persist.c": "3"})
        with open(layout.path("/storage/.config/thimsy/persist.props"), "r", encoding="utf-8") as f:
            text = f.read()
        self.assertEqual(text.split("\n")[1:], ["persist.a=back\\\\slash", "persist.b=two\\nlines", "persist.c=3", ""])
        self.assertEqual(layout.prop("persist.b"), "two\nlines")
        self.assertEqual(layout.prop("sys.x"), "1")
        self.assertEqual(layout.prop("ro.y"), "z")
        self.assertEqual(layout.prop("sys.unset"), "")


@unittest.skipUnless(HAVE_NANO and POSIX, "needs POSIX links and FIFOs")
class JailLayoutTest(util.TestCase):
    def test_build(self):
        host = fake_host(self)
        layout = JailLayout(self.path("root"), devices=["event0", "event1"], host=host).build()
        self.assertEqual(os.readlink(layout.path("/data")), "/storage/.config/thimsy/data")
        self.assertEqual(os.readlink(layout.path("/system/etc")), "/usr/share/thimsy/system/etc")
        self.assertEqual(os.readlink(layout.path("/bin")), "usr/bin")       # as the host's
        self.assertTrue(os.path.isdir(layout.path("/lib64")))
        for node in ("event0", "event1"):
            self.assertTrue(stat.S_ISFIFO(os.lstat(layout.fifo(node)).st_mode))
        for d in nano_jail.DATA_DIRS:
            self.assertTrue(os.path.isdir(layout.resolve("/data/" + d)), d)
        # Nano's paths resolve the way the jailed process sees them.
        self.assertEqual(layout.resolve("/data/system/x"), layout.path("/storage/.config/thimsy/data/system/x"))
        self.assertEqual(layout.resolve("/sdcard/a"), layout.path("/storage/emulated/0/a"))
        self.assertEqual(layout.resolve("/data/media/0/a"), layout.path("/storage/emulated/0/a"))
        self.assertEqual(layout.resolve("/mnt/media_rw/1234-ABCD/ROMs"), layout.path("/storage/1234-ABCD/ROMs"))
        self.assertTrue(os.path.isdir(layout.resolve("/storage/1234-ABCD/ES-DE")))
        self.assertEqual(stat.S_IMODE(os.stat(layout.path("/tmp")).st_mode), 0o1777)
        layout.build()  # again: idempotent

    def test_fifos_never_hit_end_of_file(self):
        layout = JailLayout(self.path("root"), host=fake_host(self)).build()
        fifos = Fifos(layout, control=layout.path("/run/thimsy/headless/ctl"))
        self.addCleanup(fifos.close)
        # A reader the way Nano opens a device: nothing to read is EAGAIN, not end-of-file.
        fd = os.open(layout.fifo("event0"), os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        with self.assertRaises(BlockingIOError):
            os.read(fd, 24)
        fifos.write("event0", b"x" * 24)
        self.assertEqual(os.read(fd, 48), b"x" * 24)
        self.assertTrue(stat.S_ISFIFO(os.stat(layout.path("/run/thimsy/headless/ctl")).st_mode))


PROBE = r"""
set -u
fail() { echo "FAIL: $*"; exit 1; }
[ "$(id -u)" = 0 ] || fail "uid $(id -u)"
for p in /usr/bin/probe-x /etc/probe-x /usr/lib/probe-x /opt/nano/probe-x; do
  ( : > "$p" ) 2>/dev/null && fail "wrote $p"
done
[ -e /usr/share/thimsy/system/etc/asset.txt ] || fail "no asset"
( : > /system/etc/probe-x ) 2>/dev/null && fail "wrote into the assets"
echo written > /data/system/probe.txt || fail "data not writable"
echo sd > /sdcard/probe.txt || fail "sdcard not writable"
echo t > /tmp/probe.txt || fail "tmp not writable"
echo hi > /dev/null || fail "/dev/null"
head -c 4 /dev/urandom | wc -c | grep -q 4 || fail "/dev/urandom"
[ -p /dev/input/event0 ] || fail "event0 is no FIFO"
[ -r /proc/self/maps ] || fail "no /proc/self/maps"
grep -q MemTotal /proc/meminfo || fail "no /proc/meminfo"
[ "$(cat /proc/1/comm)" != systemd ] || fail "the host's pid 1 is visible"
[ ! -e /home ] || fail "/home is visible"
[ -d /system/bin ] || fail "no /system/bin"
[ -z "$(ls -A /system/bin)" ] || fail "/system/bin is not empty"
[ "$(cat /run/thimsy/props/sys.probe)" = 1 ] || fail "run dir"
echo "env: HOME=$HOME PATH=$PATH"
head -c 24 /dev/input/event0 > /tmp/event.bin
echo "PROBE OK"
"""


@util.slow
@unittest.skipUnless(HAVE_NANO and POSIX, "needs POSIX")
class JailRun(util.TestCase):
    """A probe in a real jail, no Nano: the read-only binds, the writable trees, the devices."""

    def setUp(self):
        super().setUp()
        if not Jail.available():
            self.skipTest("unprivileged user namespaces are not available (unshare --user --map-root-user)")

    def test_probe(self):
        assets = self.write("assets/asset.txt", "a")
        os.makedirs(self.path("bindir"))
        layout = JailLayout(self.path("root"), assets=os.path.dirname(assets),
                            binary_dir=self.path("bindir")).build()
        layout.write_props({"sys.probe": "1", "persist.p": "x"})
        jail = Jail(layout, self.path("work"))
        fifos = Fifos(layout)
        self.addCleanup(fifos.close)
        with open(self.path("work", "probe.sh"), "w", encoding="utf-8") as f:
            f.write(PROBE)
        shutil.copy(self.path("work", "probe.sh"), layout.path("/tmp/probe.sh"))
        proc = jail.spawn(["/bin/sh", "/tmp/probe.sh"], {"PROBE": "1"}, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
        try:
            # The probe ends by reading one input_event from the FIFO: write it once the probe is in.
            time.sleep(0.3)
            fifos.write("event0", struct.pack("llHHi", 1, 2, 1, 0x130, 1) if struct.calcsize("l") == 8
                        else b"\0" * 24)
            out, _ = proc.communicate(timeout=60)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        text = out.decode("utf-8", "replace")
        self.assertEqual(proc.returncode, 0, text)
        self.assertIn("PROBE OK", text)
        self.assertIn("HOME=/data/local/tmp", text)
        # Writes landed in the jail's trees, outside the namespace.
        with open(layout.path("/storage/.config/thimsy/data/system/probe.txt")) as f:
            self.assertEqual(f.read(), "written\n")
        self.assertTrue(os.path.isfile(layout.path("/storage/emulated/0/probe.txt")))
        with open(layout.path("/tmp/event.bin"), "rb") as f:
            self.assertEqual(len(f.read()), 24)
        # Nothing was left mounted on the host's side of the root.
        self.assertEqual(os.listdir(layout.path("/usr")), [])
        self.assertEqual(os.path.getsize(layout.path("/dev/null")), 0)

    def test_exit_status_and_pid_namespace(self):
        layout = JailLayout(self.path("root")).build()
        jail = Jail(layout, self.path("work"))
        proc = jail.run(["/bin/sh", "-c", "echo $$; exit 7"])
        self.assertEqual(proc.returncode, 7, proc.stderr)
        self.assertEqual(proc.stdout.decode().strip(), "1")  # sh is the namespace's init: pid 1


# A stand-in for nano-headless that speaks its protocol (HostControl.h) from Python inside the jail,
# so the scenario runner is tested end to end without Nano: presents, the barrier, captures, the
# control FIFO, input from the device FIFO, property writes, a launch and the exit that follows it.
FAKE_NANO = r'''#!/usr/bin/python3
import json, os, struct, sys, time
cfg = json.load(open(sys.argv[sys.argv.index("--host-config") + 1]))
ev = open(cfg["events"], "a", buffering=1)
def event(kind, **f):
    f.update({"ev": kind, "t_ms": int(time.monotonic() * 1000)})
    ev.write(json.dumps(f) + "\n")
def setprop(k, v):
    open(os.path.join(cfg["run_dir"], k), "w").write(v)
    event("prop", key=k, value=v)
event("start", pid=os.getpid(), config="")
ctl = os.open(cfg["control"], os.O_RDWR | os.O_NONBLOCK)
pad = os.open(cfg["devices"][0]["path"], os.O_RDONLY | os.O_NONBLOCK)
rules = [i for i, r in enumerate(cfg["shell"]["rules"]) if "settings get global fake".startswith(r["match"])]
event("shell", fn="system", cmd="settings get global fake", rule=rules[0] if rules else -1, status=0)
barrier = cfg["barrier"]; grants = 0; capture = None; n = 0; nav = 0; buf = b""; pending = b""; launched = None
while True:
    try:
        buf += os.read(ctl, 4096)
    except BlockingIOError:
        pass
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        cmd = line.decode().split(" ", 1)
        if cmd[0] == "barrier": barrier = cmd[1] == "on"; grants = 0
        elif cmd[0] == "grant": grants += int(cmd[1])
        elif cmd[0] == "capture": capture = cmd[1]
        elif cmd[0] == "quit": event("exit", code=0); sys.exit(0)
    if barrier and grants == 0:
        time.sleep(0.005); continue
    if barrier: grants -= 1
    n += 1
    if capture:
        open(capture, "wb").write(b"P6\n2 1\n255\n" + b"\0" * 6); event("capture", n=n, path=capture, ok=True, w=2, h=1); capture = None
    event("present", n=n, slot=0, w=2, h=1)
    try:
        pending += os.read(pad, 240)
    except BlockingIOError:
        pass
    while len(pending) >= 24:
        sec, usec, typ, code, value = struct.unpack("llHHi", pending[:24]); pending = pending[24:]
        if typ == 1 and value == 1 and code == 106:
            nav += 1; setprop("sys.fake.nav", str(nav))
        if typ == 1 and value == 1 and code == 0x130:
            setprop("sys.gammaos.nano.launch_core", "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so")
            setprop("sys.gammaos.nano.launch_rom", "/storage/1234-ABCD/ROMs/snes/Test Game.sfc")
            setprop("sys.gammaos.nano.launch_app", "com.retroarch.aarch64")
            setprop("sys.gammaos.nano.do_launch", "1")
            launched = time.monotonic()
    if launched and time.monotonic() - launched > 0.2:
        open("/data/system/nano_esde_played.txt", "a").write("Test Game\n")
        event("exit", code=0); sys.exit(0)
    time.sleep(0.01)
'''

FAKE_SCENARIO = """
{"boot": {"props": {"persist.gammaos.nano.theme": "xmb"}, "shell": {"rules": [{"match": "settings get "}]}}}
{"expect": {"present_count_at_least": 3}}
{"expect_prop": {"key": "persist.gammaos.nano.mlockall", "equals": "0"}}
{"barrier": true}
{"frames": 2}
{"capture": "home.ppm"}
{"frames": 1}
{"expect": {"event": {"ev": "capture", "ok": true}}}
{"barrier": false}
{"press": "DPAD_RIGHT"}
{"expect_prop": {"key": "sys.fake.nav", "equals": "1"}}
{"set_prop": {"persist.gammaos.nano.fontscale": "1.15"}}
{"expect_persist": {"key": "persist.gammaos.nano.fontscale", "equals": "1.15"}}
{"press": "A"}
{"expect_launch": {"kind": "Core", "core": "snes9x", "rom_endswith": "/ROMs/snes/Test Game.sfc"}}
{"child_exit": {"status": 0, "after_ms": 100}}
{"expect": {"present_count_at_least": 2}}
{"expect_file": {"path": "/data/system/nano_esde_played.txt", "contains": "Test Game"}}
{"expect_no_shell": ["am start"]}
"""


@util.slow
@unittest.skipUnless(HAVE_NANO and POSIX, "needs POSIX")
class RunnerWithAFakeNano(util.TestCase):
    """nano_scenario.HeadlessRun end to end, against FAKE_NANO in a real jail."""

    def setUp(self):
        super().setUp()
        if not Jail.available():
            self.skipTest("unprivileged user namespaces are not available")
        if not os.path.exists("/usr/bin/python3"):
            self.skipTest("no /usr/bin/python3 to run the fake inside the jail")
        self.binary = self.write("bin/fake-nano-headless", FAKE_NANO, mode=0o755)

    def run_text(self, text, work):
        run = nano_scenario.HeadlessRun(self.binary, nano_scenario.parse(text, "fake"), self.path(work),
                                        env={"PYTHONDONTWRITEBYTECODE": "1"})
        run.play()
        return run

    def test_scenario_passes(self):
        run = self.run_text(FAKE_SCENARIO, "ok")
        self.assertEqual(run.starts, 2)  # restarted after the launch
        with open(run.layout.path("/run/thimsy/headless/home.ppm"), "rb") as f:
            self.assertTrue(f.read().startswith(b"P6\n"))
        self.assertFalse(run.running())

    def test_failures_are_reported(self):
        with self.assertRaises(nano_scenario.ScenarioFailure) as cm:
            self.run_text('{"boot": {}}\n{"expect_prop": {"key": "sys.never", "equals": "1"}, "within_ms": 300}', "f1")
        self.assertIn("timed out", str(cm.exception))
        self.assertIn("last events", str(cm.exception))
        # A shell command no rule answers fails the scenario at the end.
        with self.assertRaises(nano_scenario.ScenarioFailure) as cm:
            self.run_text('{"boot": {}}\n{"expect": {"present_count_at_least": 1}}', "f2")
        self.assertIn("settings get global fake", str(cm.exception))
        # Unless the scenario allows it.
        self.run_text('{"boot": {"shell": {"unexpected": "allow"}}}\n{"expect": {"present_count_at_least": 1}}', "f3")


@util.slow
@unittest.skipUnless(HAVE_NANO and POSIX, "needs POSIX")
class NanoHeadless(util.TestCase):
    """Every scenario against nano-headless (built in G10)."""

    def setUp(self):
        super().setUp()
        if not os.path.isfile(BINARY):
            self.skipTest("nano-headless is not built (%s; make -C nano/test/host headless, step G10)" % BINARY)
        if not Jail.available():
            self.skipTest("unprivileged user namespaces are not available")

    def test_scenarios(self):
        scale = float(os.environ.get("THIMSY_HEADLESS_TIME_SCALE", "1"))
        assets = os.environ.get("THIMSY_NANO_ASSETS") or None
        fonts = os.environ.get("THIMSY_NANO_FONTS") or None
        names = sorted(f for f in os.listdir(SCENARIOS) if f.endswith(".jsonl"))
        only = os.environ.get("THIMSY_SCENARIO")
        for name in names:
            if only and name != only:
                continue
            with self.subTest(scenario=name):
                steps = nano_scenario.load(os.path.join(SCENARIOS, name))
                run = nano_scenario.HeadlessRun(BINARY, steps, self.path(name[:-6]), assets=assets, fonts=fonts,
                                                time_scale=scale)
                run.play()


if __name__ == "__main__":
    unittest.main()
