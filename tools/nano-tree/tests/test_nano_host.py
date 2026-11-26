"""Nano's host unit programs (docs/linux/IMPLEMENTATION.md C.3): runs `make -C nano/test/host -j1
check-unit-0` with g++, on Linux directly or from Windows in the WSL distro (THIMSY_WSL_DISTRO,
default Ubuntu), at nice 19 and under the C++ lock /tmp/thimsy-cxx.lock when flock exists. The
Makefile runs linux/tools/build_guard.sh --light first; a refusal (an Android build may be running
and memory is short, say) skips this test with the guard's reasons, never passes it.

Skips without nano/, without make and g++ (and without WSL on Windows), or with THIMSY_NANO_HOST=0.
The first run compiles for a few minutes; later runs relink and run in about a minute. Fork-only.
"""
from __future__ import annotations

import os
import re
import shlex
import shutil
import subprocess
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
MAKEFILE_DIR = os.path.join(ROOT, "nano", "test", "host")
DISTRO = os.environ.get("THIMSY_WSL_DISTRO", "Ubuntu")
TIMEOUT = 1800
SUMMARY = re.compile(r"^nano check-unit-0: (\d+) tests, (\d+) failed, (\d+) skipped, in (\d+) programs$", re.M)
REFUSED = re.compile(r"^(build_guard: refused: .*|nano compat: refusing .*|.*: refusing to build: .*)$", re.M)


def wsl_path(path: str) -> str:
    """C:\\x\\y -> /mnt/c/x/y."""
    path = os.path.abspath(path)
    drive, rest = os.path.splitdrive(path)
    return "/mnt/%s%s" % (drive.rstrip(":").lower(), rest.replace("\\", "/"))


def _wsl_has_distro() -> bool:
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        return False
    try:
        out = subprocess.run([wsl, "-l", "-q"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    names = out.decode("utf-16-le", "replace") if b"\0" in out else out.decode("utf-8", "replace")
    return DISTRO in [n.strip().strip("\ufeff") for n in names.splitlines()]


# The shell script that runs a target (ROOT and TARGET are set before it: values in the script, run
# with wsl.exe --exec, so no outer shell expands it). It holds the C++ lock from the copy through the
# make, so another run can't replace the copy under it (test-quality-2). A checkout on /mnt (a
# Windows drive seen from WSL, slow 9p) is copied to ~/.cache/thimsy/cxx-src-<key> first, as
# tests/run.sh cxx does, and every checkout builds under its own ~/.cache/thimsy/cxx-<key>/nano-host
# (<key>: cksum of the checkout's path; the same trees tests/run.sh cxx uses), so make never takes
# another worktree's objects as up to date (test-quality-1). The base's assets (work/nano-composed)
# are read where they are.
SCRIPT = r"""set -e
root=$ROOT; target=$TARGET
if command -v flock >/dev/null; then
  : >>/tmp/thimsy-cxx.lock 2>/dev/null || true
  exec 9</tmp/thimsy-cxx.lock
  flock 9
fi
key=$(printf '%s' "$root" | cksum | cut -d' ' -f1)
out=$HOME/.cache/thimsy/cxx-$key/nano-host
data=; [ -d "$root/work/nano-composed" ] && data="NANO_DATA=$root/work/nano-composed"
case "$root" in
  /mnt/*) copy=$HOME/.cache/thimsy/cxx-src-$key
          rm -rf "$copy" && mkdir -p "$copy"
          tar -C "$root" -cf - nano libthimsy linux/tools | tar -C "$copy" -xf -
          root=$copy ;;
esac
cd "$root/nano/test/host"
exec nice -n 19 make -j1 "$target" O="$out" $data
"""


def script(root: str, target: str) -> str:
    return "ROOT=%s TARGET=%s\n%s" % (shlex.quote(root), shlex.quote(target), SCRIPT)


def command(target: str):
    """(argv, how) to run TARGET of the host Makefile, or (None, why not)."""
    if sys.platform.startswith("linux"):
        if not (shutil.which("make") and shutil.which("g++")):
            return None, "make or g++ is not installed"
        return ["sh", "-c", script(ROOT, target)], "local"
    if os.name == "nt":
        if not _wsl_has_distro():
            return None, "no WSL distro %s" % DISTRO
        wsl = shutil.which("wsl.exe") or shutil.which("wsl")
        return [wsl, "-d", DISTRO, "--exec", "sh", "-c", script(wsl_path(ROOT), target)], "WSL " + DISTRO
    return None, "no way to run g++ here"


@unittest.skipUnless(os.path.isfile(os.path.join(MAKEFILE_DIR, "Makefile")), "no nano/test/host/Makefile")
@unittest.skipIf(os.environ.get("THIMSY_NANO_HOST") == "0", "THIMSY_NANO_HOST=0")
class CheckUnit0(unittest.TestCase):
    def test_check_unit_0(self):
        argv, how = command("check-unit-0")
        if argv is None:
            self.skipTest(how)
        try:
            proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.fail("check-unit-0 took more than %d s (%s)" % (TIMEOUT, how))
        out = proc.stdout.decode("utf-8", "replace")
        refused = REFUSED.findall(out)
        if proc.returncode != 0 and refused:
            self.skipTest("the memory guard refused (%s): %s" % (how, "; ".join(refused)))
        tail = "\n".join(out.splitlines()[-60:])
        self.assertEqual(proc.returncode, 0, "check-unit-0 failed (%s):\n%s" % (how, tail))
        m = SUMMARY.search(out)
        self.assertIsNotNone(m, "no summary line (%s):\n%s" % (how, tail))
        tests, failed, _skipped, programs = (int(x) for x in m.groups())
        self.assertEqual(failed, 0, tail)
        self.assertGreater(tests, 0)
        self.assertGreater(programs, 0)


class Helpers(unittest.TestCase):
    def test_wsl_path(self):
        if os.name != "nt":
            self.skipTest("Windows paths")
        self.assertEqual(wsl_path("C:\\Users\\x\\repo"), "/mnt/c/Users/x/repo")

    def test_patterns(self):
        self.assertTrue(SUMMARY.search("x\nnano check-unit-0: 120 tests, 0 failed, 1 skipped, in 19 programs\n"))
        self.assertTrue(REFUSED.search("build_guard: refused: MemAvailable is 1 kB, under the floor of 2 kB"))


if __name__ == "__main__":
    unittest.main()
