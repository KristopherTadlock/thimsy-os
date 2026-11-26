# SPDX-License-Identifier: MIT
"""The QEMU boot test (IMPLEMENTATION E.9; decision I8). Written, not run.

Boots a built qemu-aarch64-virt card with board/qemu-aarch64-virt/run-qemu.sh
and follows the serial console:
  1. waits (600 s under TCG) for "thimsy: rcS done", "thimsy-session: nano
     started" and "nano: first frame";
  2. sends Enter through QMP send-key, which thimsy-padd maps to BTN_SOUTH
     through qemu-virt.json's pad section, and waits for Nano's line for the
     opened menu;
  3. logs in on the serial getty (dev images) and powers off through
     thimsy-prop (sys.powerctl), and waits for QEMU to exit;
  4. prints the boot clock's timeline (/run/thimsy/boot/*, uptime seconds).

It runs only when THIMSY_QEMU=1, with the card and kernel of a built
thimsy_qemu_aarch64_hw (THIMSY_QEMU_IMAGES, default
<repo>/work/linux/images/thimsy_qemu_aarch64_hw), qemu-system-aarch64 on
PATH and Unix sockets (not Windows). Otherwise it is skipped. Markers that
Nano does not print yet are environment variables (guesses until Phase 2b's
instrumentation lands): THIMSY_QEMU_SESSION_MARKER, THIMSY_QEMU_FRAME_MARKER,
THIMSY_QEMU_MENU_MARKER. THIMSY_QEMU_TIMEOUT overrides the 600 s.

The run-qemu.sh checks below need only a POSIX shell and always run.

    THIMSY_QEMU=1 python3 -m unittest discover -s tests -t . -p test_qemu_boot.py
"""
from __future__ import annotations

import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from typing import List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent.parent
REPO = LINUX.parent
sys.path.insert(0, str(LINUX / "tools"))

import check_rcs  # noqa: E402

SHELLS = check_rcs.find_shells()
SH: Optional[List[str]] = SHELLS[0] if SHELLS else None
RUN_QEMU = LINUX / "board/qemu-aarch64-virt/run-qemu.sh"
IMAGES = Path(os.environ.get("THIMSY_QEMU_IMAGES",
                             str(REPO / "work/linux/images/thimsy_qemu_aarch64_hw")))
TIMEOUT = float(os.environ.get("THIMSY_QEMU_TIMEOUT", "600"))
MARKERS = [
    "thimsy: rcS done",
    os.environ.get("THIMSY_QEMU_SESSION_MARKER", "thimsy-session: nano started"),
    os.environ.get("THIMSY_QEMU_FRAME_MARKER", "nano: first frame"),
]
MENU_MARKER = os.environ.get("THIMSY_QEMU_MENU_MARKER", "nano: menu opened")  # (guess)
E9_APPEND = "root=/dev/vda3 rootfstype=erofs ro init=/init console=ttyAMA0"


def _skip_reason() -> Optional[str]:
    if os.environ.get("THIMSY_QEMU") != "1":
        return "THIMSY_QEMU=1 is not set (E.9: written, not run)"
    if SH is None:
        return "no POSIX shell"
    if not hasattr(socket, "AF_UNIX") or os.name == "nt":
        return "QMP over a Unix socket needs Linux"
    if shutil.which(os.environ.get("QEMU", "qemu-system-aarch64")) is None:
        return "qemu-system-aarch64 is not on PATH"
    for f in ("Image", "thimsy-qemu-virt.img"):
        if not (IMAGES / f).is_file():
            return "no built card: %s is missing" % (IMAGES / f)
    return None


class Serial:
    """QEMU's stdout (the serial console), read on a thread, line by line."""

    def __init__(self, proc: "subprocess.Popen[bytes]") -> None:
        self.proc = proc
        self.lines: "queue.Queue[str]" = queue.Queue()
        self.seen: List[str] = []
        self.t0 = time.monotonic()
        t = threading.Thread(target=self._pump, daemon=True)
        t.start()

    def _pump(self) -> None:
        assert self.proc.stdout is not None
        buf = b""
        while True:
            ch = self.proc.stdout.read(1)
            if not ch:
                break
            buf += ch
            if ch == b"\n" or buf.endswith(b"login: "):
                line = buf.decode("utf-8", "replace").rstrip("\r\n")
                sys.stdout.write("[%7.2f] %s\n" % (time.monotonic() - self.t0, line))
                self.lines.put(line)
                buf = b""

    def wait_for(self, needle: str, timeout: float) -> str:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                line = self.lines.get(timeout=1)
            except queue.Empty:
                if self.proc.poll() is not None:
                    raise AssertionError("QEMU exited (%s) before %r" % (self.proc.returncode, needle))
                continue
            self.seen.append(line)
            if needle in line:
                return line
        raise AssertionError("no %r within %.0f s" % (needle, timeout))

    def send(self, text: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()


class Qmp:
    def __init__(self, path: str, timeout: float = 30) -> None:
        end = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)  # type: ignore[attr-defined]
                self.sock.connect(path)
                break
            except OSError:
                if time.monotonic() > end:
                    raise
                time.sleep(0.2)
        self.f = self.sock.makefile("rwb")
        json.loads(self.f.readline())          # the greeting
        self.cmd("qmp_capabilities")

    def cmd(self, name: str, **args: object) -> dict:
        msg = {"execute": name}
        if args:
            msg["arguments"] = args  # type: ignore[assignment]
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply or "error" in reply:
                if "error" in reply:
                    raise AssertionError("QMP %s: %s" % (name, reply["error"]))
                return reply["return"]

    def close(self) -> None:
        self.sock.close()


@unittest.skipIf(_skip_reason() is not None, _skip_reason() or "")
class QemuBootTest(unittest.TestCase):
    def test_boot_menu_poweroff(self) -> None:
        assert SH is not None
        work = Path(tempfile.mkdtemp(prefix="thimsy-qemu-"))
        qmp_path = str(work / "qmp.sock")
        proc = subprocess.Popen(
            SH + [RUN_QEMU.as_posix(), "--images", str(IMAGES), "--work", str(work / "card.img"),
                  "--qmp", qmp_path],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        serial = Serial(proc)
        qmp: Optional[Qmp] = None
        try:
            for m in MARKERS:
                serial.wait_for(m, TIMEOUT)
            qmp = Qmp(qmp_path)
            qmp.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
            serial.wait_for(MENU_MARKER, 120)
            # The dev image's serial getty (dev.fragment): root, no password.
            serial.send("\n")
            serial.wait_for("login:", 60)
            serial.send("root\n")
            time.sleep(2)
            serial.send("cat /run/thimsy/boot/* >/dev/null 2>&1; "
                        "for f in /run/thimsy/boot/*; do echo \"bootclock ${f##*/} $(cat $f)\"; done\n")
            serial.wait_for("bootclock", 60)
            serial.send("thimsy-prop set sys.powerctl shutdown\n")
            proc.wait(timeout=120)
            self.assertEqual(proc.returncode, 0)
            print("boot clock:")
            for line in serial.seen:
                m = re.match(r"bootclock (\S+) (\S+)", line)
                if m:
                    print("  %-20s %s s" % m.groups())
        finally:
            if qmp is not None:
                qmp.close()
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            shutil.rmtree(str(work), ignore_errors=True)


@unittest.skipIf(SH is None, "no POSIX shell found")
class RunQemuScriptTest(unittest.TestCase):
    """run-qemu.sh's command line (E.9), without QEMU."""

    def dry(self, *args: str) -> str:
        assert SH is not None
        env = dict(os.environ)
        env.pop("QEMU", None)
        r = subprocess.run(SH + [RUN_QEMU.as_posix(), "--dry-run"] + list(args), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout)
        return r.stdout.strip()

    def test_parses(self) -> None:
        assert SH is not None
        r = subprocess.run(SH + ["-n", RUN_QEMU.as_posix()], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_e9_command_line(self) -> None:
        out = self.dry("--images", "/imgs", "--qmp", "/tmp/q.sock")
        self.assertTrue(out.startswith("qemu-system-aarch64 -M virt "), out)
        for part in ("-smp 4", "-m 1024", "-nographic", "-no-reboot", "-kernel /imgs/Image",
                     "-append '%s'" % E9_APPEND,
                     "-drive file=/imgs/thimsy-qemu-virt.run.img,if=virtio,format=raw",
                     "-device virtio-gpu-pci", "-device virtio-keyboard-pci",
                     "-qmp unix:/tmp/q.sock,server,nowait"):
            self.assertIn(part, out)
        self.assertRegex(out, r"-accel (tcg -cpu max|kvm -cpu host)")

    def test_options(self) -> None:
        out = self.dry("--kernel", "/k/Image", "--card", "/c/card.img", "--in-place",
                       "--append", "loglevel=7", "--smp", "2", "--mem", "2048", "--", "-s")
        self.assertIn("-kernel /k/Image", out)
        self.assertIn("-drive file=/c/card.img,if=virtio,format=raw", out)
        self.assertIn("-append '%s loglevel=7'" % E9_APPEND, out)
        self.assertIn("-smp 2 -m 2048", out)
        self.assertTrue(out.endswith(" -s"), out)
        self.assertNotIn("-qmp", out)

    def test_bad_arguments(self) -> None:
        assert SH is not None
        for args in (["--grow", "x"], ["--bogus"], ["--kernel"]):
            r = subprocess.run(SH + [RUN_QEMU.as_posix(), "--dry-run"] + args,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               universal_newlines=True, timeout=60)
            self.assertEqual(r.returncode, 2, (args, r.stdout))

    def test_device_json_maps_enter_to_south(self) -> None:
        dev = json.loads((LINUX / "board/qemu-aarch64-virt/devices/qemu-virt.json").read_text(
            encoding="utf-8"))
        self.assertEqual(dev["id"], "qemu-virt")
        self.assertIn({"source": "virtio-keyboard", "code": "KEY_ENTER", "to": "BTN_SOUTH"},
                      dev["pad"]["buttons"])


if __name__ == "__main__":
    unittest.main()
