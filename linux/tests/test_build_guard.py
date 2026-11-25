"""Tests of linux/tools/build_guard.sh and linux/build.sh (IMPLEMENTATION.md E.8, D.4).

The guard runs against a fake /proc/meminfo and /proc/loadavg, fake wsl.exe and
docker first on PATH, and a lock held by this process. It needs flock(1), so it
runs in WSL and is skipped where flock is missing (Git Bash). Nothing here
touches the real lock, the real distros or Docker.

build.sh is run only with --help, --dry-run and refusals, never a build.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None

LINUX = Path(__file__).resolve().parent.parent
GUARD = LINUX / "tools" / "build_guard.sh"
BUILD = LINUX / "build.sh"
SH = shutil.which("sh")
FORCE = "--force=I-checked-no-android-build-runs"

WSL_HEADER = "Windows Subsystem for Linux Distributions:"


def meminfo(avail_kb):
    return ("MemTotal:       13000000 kB\nMemFree:          500000 kB\n"
            "MemAvailable:   %8d kB\nBuffers:          100000 kB\n" % avail_kb)


@unittest.skipIf(SH is None or shutil.which("flock") is None or fcntl is None,
                 "needs sh, flock(1) and fcntl (run in WSL)")
class Guard(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-guard-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.lock = self.dir / "thimsy-build.lock"
        self.set_mem(12000000)
        self.set_load("1.50 1.20 1.00 2/300 1234")
        self.set_wsl(["Ubuntu (Default)", "docker-desktop"])
        self.set_docker(["buildroot-cache", "registry"])
        self.fake("wsl.exe", 'cat "%s"\nexit "$(cat "%s")"\n'
                  % (self.dir / "wsl.out", self.dir / "wsl.rc"))
        self.fake("docker", '[ "$1" = ps ] || exit 64\ncat "%s"\nexit "$(cat "%s")"\n'
                  % (self.dir / "docker.out", self.dir / "docker.rc"))
        self.write("wsl.rc", "0")
        self.write("docker.rc", "0")

    # -- fixtures ------------------------------------------------------------

    def write(self, name, data):
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(self.dir / name, mode) as f:
            f.write(data)

    def fake(self, name, body):
        p = self.bin / name
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)

    def set_mem(self, kb):
        self.write("meminfo", meminfo(kb))

    def set_load(self, text):
        self.write("loadavg", text + "\n")

    def set_wsl(self, names, utf16=True):
        text = "\r\n".join([WSL_HEADER] + names) + "\r\n"
        self.write("wsl.out", text.encode("utf-16-le") if utf16 else text.encode("utf-8"))

    def set_docker(self, names):
        self.write("docker.out", "".join(n + "\n" for n in names))

    def env(self, **extra):
        env = {
            "PATH": "%s:%s" % (self.bin, os.environ.get("PATH", "/usr/bin:/bin")),
            "HOME": str(self.dir),
            "TMPDIR": str(self.dir),
            "THIMSY_GUARD_LOCK": str(self.lock),
            "THIMSY_GUARD_MEMINFO": str(self.dir / "meminfo"),
            "THIMSY_GUARD_LOADAVG": str(self.dir / "loadavg"),
        }
        env.update(extra)
        return env

    def guard(self, *args, **env):
        p = subprocess.run([SH, str(GUARD)] + list(args), env=self.env(**env),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, timeout=30)
        return p.returncode, p.stdout, p.stderr

    def hold_lock(self):
        f = open(self.lock, "a")
        fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.addCleanup(f.close)
        return f

    def assertRefused(self, result, text):
        code, _, err = result
        self.assertEqual(code, 3, err)
        self.assertIn(text, err)

    def assertPassed(self, result):
        code, _, err = result
        self.assertEqual(code, 0, err)

    # -- full mode (linux/build.sh) ------------------------------------------

    def test_passes_when_clear(self):
        self.assertPassed(self.guard())
        self.assertEqual(sorted(p.name for p in self.dir.glob("build_guard.*")), [])

    def test_memory_under_the_floor(self):
        self.set_mem(8999999)
        self.assertRefused(self.guard(), "MemAvailable is 8999999 kB, under the floor of 9000000 kB")
        self.set_mem(9000000)
        self.assertPassed(self.guard())

    def test_unreadable_meminfo(self):
        self.write("meminfo", "MemTotal: 1 kB\n")
        self.assertRefused(self.guard(), "cannot read MemAvailable")

    def test_lock_held(self):
        self.hold_lock()
        self.assertRefused(self.guard(), "is held by another build")
        self.assertRefused(self.guard("--", "true"), "is held by another build")

    def test_unopenable_lock(self):
        self.assertRefused(self.guard(THIMSY_GUARD_LOCK=str(self.dir / "no" / "such" / "dir" / "x")),
                           "cannot open the build lock")

    def test_aosp_distro_running(self):
        self.set_wsl(["Ubuntu (Default)", "thimsy-aosp", "docker-desktop"])
        self.assertRefused(self.guard(), "WSL distro thimsy-aosp is running")
        self.set_wsl(["thimsy-aosp (Default)", "Ubuntu"], utf16=False)
        self.assertRefused(self.guard(), "WSL distro thimsy-aosp is running")

    def test_similar_distro_name_is_not_the_aosp_one(self):
        self.set_wsl(["Ubuntu (Default)", "thimsy-aosp-old", "my-thimsy-aosp"])
        self.assertPassed(self.guard())

    def test_wsl_failing_or_missing_refuses(self):
        self.write("wsl.rc", "1")
        self.assertRefused(self.guard(), "wsl.exe -l --running failed")
        self.assertRefused(self.guard(THIMSY_GUARD_WSL=str(self.dir / "absent" / "wsl.exe")),
                           "not found (is WSL interop on?)")

    def test_android_build_container(self):
        self.set_docker(["buildroot-cache", "thimsy-aosp-build-1"])
        self.assertRefused(self.guard(), "docker container thimsy-aosp-build-1 is running")

    def test_docker_that_does_not_answer_is_only_noted(self):
        self.write("docker.rc", "1")
        code, _, err = self.guard()
        self.assertEqual(code, 0, err)
        self.assertIn("note: docker ps failed", err)

    def test_load_above_eight(self):
        self.set_load("9.50 7.00 6.00 3/400 99")
        self.assertRefused(self.guard(), "the one-minute load is 9.50, above 8")
        self.set_load("8.00 9.00 9.00 3/400 99")
        self.assertPassed(self.guard())

    def test_every_reason_is_listed(self):
        self.set_mem(1000)
        self.set_load("12.0 1 1 1/1 1")
        self.set_wsl(["thimsy-aosp"])
        code, _, err = self.guard()
        self.assertEqual(code, 3)
        for text in ("MemAvailable", "thimsy-aosp", "load"):
            self.assertIn(text, err)

    # -- --force ----------------------------------------------------------------

    def test_force_skips_memory_and_load_only(self):
        self.set_mem(1000)
        self.set_load("15.0 1 1 1/1 1")
        self.assertPassed(self.guard(FORCE))
        self.set_wsl(["thimsy-aosp"])
        self.assertRefused(self.guard(FORCE), "WSL distro thimsy-aosp is running")
        self.set_wsl(["Ubuntu"])
        self.set_docker(["thimsy-build"])
        self.assertRefused(self.guard(FORCE), "docker container thimsy-build is running")
        self.set_docker([])
        self.hold_lock()
        self.assertRefused(self.guard(FORCE), "is held by another build")

    def test_force_needs_its_typed_confirmation(self):
        for arg in ("--force", "--force=yes", "--force=I-checked"):
            with self.subTest(arg=arg):
                self.assertRefused(self.guard(arg), "needs its typed confirmation")

    # -- --light (the C++ test Makefiles) -----------------------------------------

    def test_light_lets_only_small_targets_through_an_android_build(self):
        self.set_wsl(["thimsy-aosp"])
        for t in ("check-compat", "check-unit-0", "libthimsy", "thimsy-session", "thimsy-padd"):
            with self.subTest(target=t):
                code, _, err = self.guard("--light", "--target", t)
                self.assertEqual(code, 0, err)
                self.assertIn("allowed small target %s despite" % t, err)
        for t in ("check-unit", "syntax-core", "headless"):
            with self.subTest(target=t):
                code, _, err = self.guard("--light", "--target=" + t)
                self.assertEqual(code, 3, err)
                self.assertIn("only these targets run", err)
        self.assertRefused(self.guard("--light"), "thimsy-aosp is running")

    def test_light_treats_lock_and_load_as_android_signs(self):
        self.hold_lock()
        self.set_load("11.0 1 1 1/1 1")
        self.assertPassed(self.guard("--light", "--target", "check-unit-0"))
        self.assertRefused(self.guard("--light", "--target", "headless"), "held by another build")

    def test_light_memory_floors(self):
        self.set_mem(7000000)
        self.assertPassed(self.guard("--light", "--target", "check-unit-0"))
        self.assertRefused(self.guard("--light", "--target", "syntax-core"), "under the floor of 9000000 kB")
        self.set_mem(5999999)
        self.assertRefused(self.guard("--light", "--target", "check-unit-0"),
                           "under the floor of 6000000 kB")

    # -- holding the lock around a command ---------------------------------------

    PROBE = 'if flock -n "$1" true; then echo free; else echo held; fi'

    def test_command_runs_holding_the_lock(self):
        code, out, err = self.guard("--", "sh", "-c", self.PROBE, "sh", str(self.lock))
        self.assertEqual(code, 0, err)
        self.assertEqual(out.strip(), "held")
        with open(self.lock, "a") as f:  # released when the command ended
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_light_command_does_not_take_the_lock(self):
        code, out, err = self.guard("--light", "--target", "libthimsy", "--",
                                    "sh", "-c", self.PROBE, "sh", str(self.lock))
        self.assertEqual(code, 0, err)
        self.assertEqual(out.strip(), "free")

    def test_command_status_passes_through_and_refusal_skips_it(self):
        self.assertEqual(self.guard("--", "sh", "-c", "exit 7")[0], 7)
        self.set_mem(1)
        marker = self.dir / "ran"
        self.assertRefused(self.guard("--", "touch", str(marker)), "MemAvailable")
        self.assertFalse(marker.exists())

    def test_usage(self):
        self.assertEqual(self.guard("--bogus")[0], 2)
        self.assertEqual(self.guard("--")[0], 2)
        code, out, _ = self.guard("--help")
        self.assertEqual(code, 0)
        self.assertIn("--light", out)


def posix(path):
    return str(path).replace("\\", "/")


@unittest.skipIf(SH is None, "needs sh")
class BuildSh(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-build-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.work = self.dir / "work"

    def build(self, *args):
        env = dict(os.environ)
        env.update({"THIMSY_PYTHON": posix(sys.executable), "THIMSY_WORK": posix(self.work),
                    "THIMSY_BUILD_DIR": posix(self.dir / "build")})
        p = subprocess.run([SH, posix(BUILD)] + list(args), env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, timeout=120)
        return p.returncode, p.stdout, p.stderr

    def test_publish_refused_for_a_target_not_marked_publish(self):
        for target in ("thimsy_v8_0", "thimsy_v8_2", "thimsy_qemu_aarch64_hw"):
            with self.subTest(target=target):
                code, out, err = self.build("--publish", target)
                self.assertEqual(code, 4, err)
                self.assertIn('is not marked "publish": true', err)
                self.assertEqual(out, "")
        self.assertFalse(self.work.exists())

    def test_dry_run_puts_the_guard_before_any_work(self):
        code, out, err = self.build("--dry-run", "--publish", "thimsy_v8_0_release")
        self.assertEqual(code, 0, err)
        steps = [line.split(":", 1)[0] for line in out.splitlines()]
        self.assertIn("guard", steps)
        first_work = min(steps.index(s) for s in ("buildroot", "archive", "docker"))
        self.assertLess(steps.index("guard"), first_work)
        self.assertTrue(all(s == "check" for s in steps[:steps.index("guard")]))
        self.assertIn("make all sdk legal-info", out)
        self.assertIn("(the whole repository, I41)", out)
        self.assertFalse(self.work.exists(), "a dry run must not write")

    def test_dry_run_of_a_hardware_pack(self):
        code, out, err = self.build("--dry-run", "thimsy_qemu_aarch64_hw")
        self.assertEqual(code, 0, err)
        self.assertIn("then make all (log:", out)
        self.assertNotIn("legal-info", out)
        code, out, err = self.build("--dry-run", "thimsy_v8_2", "linux-rebuild", "all")
        self.assertIn("then make linux-rebuild all (log:", out)

    def test_dry_run_passes_force_to_the_guard(self):
        code, out, err = self.build("--dry-run", FORCE, "thimsy_v8_2")
        self.assertEqual(code, 0, err)
        self.assertIn("build_guard.sh %s --" % FORCE, out)

    def test_usage_errors(self):
        self.assertEqual(self.build()[0], 2)
        self.assertEqual(self.build("--bogus", "thimsy_v8_0")[0], 2)
        code, _, err = self.build("--dry-run", "no_such_target")
        self.assertEqual(code, 2)
        self.assertIn("known: thimsy_v8_0", err)
        code, _, err = self.build("card", "rg-arc-d")
        self.assertEqual(code, 2)
        self.assertIn("G8", err)

    def test_help(self):
        code, out, _ = self.build("--help")
        self.assertEqual(code, 0)
        self.assertIn("--publish", out)
        self.assertIn("Exit status", out)


if __name__ == "__main__":
    unittest.main()
