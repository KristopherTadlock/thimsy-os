#!/usr/bin/env python3
"""The Docker setup (compose.yaml, .devcontainer/) agrees with the installer's CI
(.github/workflows/installer.yml) and with itself, and MaliSX2 is pre-installed in
ARMSX2 under the id ARMSX2 gives it. Static checks, standard library only:
python3 -I installer/tools/test_docker_setup.py (installer/tests/run-all.sh runs it)."""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEV = os.path.join(ROOT, ".devcontainer")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


class DockerSetup(unittest.TestCase):
    def test_flutter_version_is_the_ci_one(self):
        image = re.search(r"^ARG FLUTTER_VERSION=(\S+)$", read(".devcontainer", "Dockerfile"), re.M).group(1)
        ci = re.search(r"^\s*flutter-version: (\S+)$", read(".github", "workflows", "installer.yml"), re.M).group(1)
        self.assertEqual(image, ci)

    def test_image_has_the_ci_packages(self):
        """What CI installs for the Linux engine and app is in the image. libstdc++-12-dev is the one
        exception: CI's runner needs it for clang, but Ubuntu 24.04's clang pulls libstdc++-13-dev."""
        image = read(".devcontainer", "Dockerfile")
        ci = read(".github", "workflows", "installer.yml")
        packages = set()
        for line in re.findall(r"sudo apt-get install -y ([^\n]+)", ci):
            packages.update(line.split())
        self.assertTrue(packages)
        for package in sorted(packages - {"libstdc++-12-dev"}):
            self.assertRegex(image, rf"(?m)^\s+{re.escape(package)} \\$", package)

    def test_one_work_volume(self):
        name = re.search(r"^  work:\n    name: (\S+)$", read("compose.yaml"), re.M).group(1)
        dev = json.loads(re.sub(r"^\s*//.*$", "", read(".devcontainer", "devcontainer.json"), flags=re.M))
        self.assertIn(f"source={name},target=/workspaces/thimsy-os/work,type=volume", dev["mounts"])
        self.assertIn(f"`{name}`", read("README.md"))

    def test_entrypoint(self):
        compose = read("compose.yaml")
        self.assertIn('entrypoint: ["bash", ".devcontainer/as-owner.sh"]', compose)
        self.assertIn("\n    privileged: true\n", compose)
        # docker stop gives the build time to unmount and detach its images.
        self.assertRegex(compose, r"\n    stop_grace_period: \d+m\n")
        # bash -n where bash is Linux's (on Windows, System32's bash may be WSL's).
        if os.name == "posix" and shutil.which("bash"):
            subprocess.run(["bash", "-n", os.path.join(DEV, "as-owner.sh")], check=True)
        text = read(".devcontainer", "as-owner.sh")
        self.assertIn("trap 'trap \"\" TERM; kill -TERM 0 2>/dev/null' TERM", text)
        self.assertIn('env --default-signal=INT,QUIT "$@" <&0 &', text)

    def test_image_has_the_tools_build_sh_checks(self):
        """Every tool scripts/build.sh stops without (but nano-bootlogo, for SYSTEM_IMG builds only) comes
        with the image: from an apt package line of its own, or a link the image makes in /usr/local/bin."""
        packages = {"fsck.erofs": "erofs-utils", "mkfs.erofs": "erofs-utils", "lz4": "lz4", "debugfs": "e2fsprogs",
                    "secilc": "secilc", "7z": "7zip", "7zz": "7zip", "aapt": "google-android-build-tools-34.0.0-installer"}
        links = {"avbtool", "unpack_bootimg"}
        image = read(".devcontainer", "Dockerfile")
        build = read("scripts", "build.sh")
        tools = set()
        for group in re.findall(r"^for tool in ([^;]+); do\n  command -v", build, re.M):
            tools.update(group.split())
        tools.update(re.findall(r"command -v ([a-z0-9._-]+)(?: >/dev/null)? \|\|", build))
        tools.discard("nano-bootlogo")
        self.assertTrue({"fsck.erofs", "secilc", "7z", "aapt"} <= tools, tools)
        for tool in sorted(tools):
            if tool in links:
                self.assertRegex(image, rf"ln -s \S+ /usr/local/bin/{re.escape(tool)}\b", tool)
            else:
                self.assertIn(tool, packages, f"build.sh needs {tool}: say which package provides it")
                self.assertRegex(image, rf"(?m)^\s+{re.escape(packages[tool])} \\$", tool)
        self.assertRegex(image, r"(?m)^ENV PATH=\$ANDROID_HOME/build-tools/34\.0\.0:\$PATH$")
        self.assertRegex(image, r"(?m)^ENV MKBOOTIMG=/\S+mkbootimg\.py$")
        self.assertRegex(image, r"(?m)^ENV AVB_KEY=/\S+\.pem$")

    def test_malisx2_driver_id(self):
        """ARMSX2's CustomDriver.makeId: "armsx2libmali-" + tag + "-" + asset less ".zip",
        lower case, anything but [A-Za-z0-9._-] as "_"."""
        mod = read("mods", "45-armsx2-malisx2.sh")
        zip_name = re.search(r'^DRIVER_ZIP="([^"]+)"$', mod, re.M).group(1)
        driver_id = re.search(r'^DRIVER_ID="([^"]+)"$', mod, re.M).group(1)
        lines = [l for l in read("mods", "added-apps.txt").splitlines() if l.startswith(f"file  {zip_name}  ")]
        self.assertEqual(len(lines), 1)
        url = re.search(r"  url=(\S+)", lines[0]).group(1)
        m = re.fullmatch(r"https://github\.com/bmdhacks/malisx2/releases/download/([^/]+)/([^/]+)\.zip", url)
        self.assertTrue(m, url)
        tag, asset = m.groups()
        self.assertEqual(driver_id, re.sub(r"[^A-Za-z0-9._-]", "_", f"armsx2libmali-{tag}-{asset}").lower())


def default_signal_env():
    """Whether env has --default-signal (GNU coreutils 8.31 or later), which as-owner.sh uses."""
    try:
        return subprocess.run(["env", "--default-signal=INT", "true"], capture_output=True).returncode == 0
    except OSError:
        return False


@unittest.skipUnless(sys.platform.startswith("linux") and default_signal_env(), "needs Linux and GNU env")
class AsOwner(unittest.TestCase):
    """as-owner.sh, compose's entrypoint, hands exit status, stdin and signals through (SIGTERM to its
    whole process group, and SIGINT without a terminal). Its hand-back of files needs root: nano-144's
    tests/e2e/test_as_owner.py covers it."""

    def setUp(self):
        self.co = tempfile.mkdtemp(prefix="as-owner-")
        self.addCleanup(shutil.rmtree, self.co, True)

    def owner(self, *cmd, **kw):
        return subprocess.run(["bash", os.path.join(DEV, "as-owner.sh"), *cmd], cwd=self.co,
                              capture_output=True, text=True, timeout=60, **kw)

    def test_exit_status_and_stdin(self):
        self.assertEqual(self.owner("bash", "-c", "exit 7").returncode, 7)
        self.assertEqual(self.owner("cat", input="hi\n").stdout, "hi\n")

    def test_passes_signals_on(self):
        # Like build.sh: the command waits on a foreground program (sleep, as gradle or ninja) and has a
        # mod of its own running; both stop on docker stop's SIGTERM and on SIGINT without a terminal.
        mod = "trap 'echo mod >> mod_got; exit 3' TERM INT; sleep 30 & wait"
        child = f'env --default-signal=INT bash -c "{mod}" >/dev/null & echo ready; sleep 30; echo after'
        for sig, code in ((signal.SIGTERM, 143), (signal.SIGINT, 130)):
            # Its own session, as PID 1 in the container: kill 0 reaches its process group only.
            proc = subprocess.Popen(["bash", os.path.join(DEV, "as-owner.sh"), "bash", "-c", child], cwd=self.co,
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                    start_new_session=True)
            try:
                self.assertEqual(proc.stdout.readline(), "ready\n")
                time.sleep(0.3)
                proc.send_signal(sig)
                out, _ = proc.communicate(timeout=20)
                time.sleep(0.3)
            finally:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            # The command stopped (it never said "after"), with the signal's status.
            self.assertEqual((proc.returncode, out), (code, ""), sig.name)
            got = os.path.join(self.co, "mod_got")
            self.assertEqual(open(got, encoding="utf-8").read() if os.path.exists(got) else None, "mod\n", sig.name)
            os.remove(got)

if __name__ == "__main__":
    unittest.main()
