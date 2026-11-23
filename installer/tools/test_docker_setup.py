#!/usr/bin/env python3
"""The Docker setup (compose.yaml, .devcontainer/) agrees with the installer's CI
(.github/workflows/installer.yml) and with itself, and MaliSX2 is pre-installed in
ARMSX2 under the id ARMSX2 gives it. Static checks, standard library only:
python3 -I installer/tools/test_docker_setup.py (installer/tests/run-all.sh runs it)."""
import json
import os
import re
import subprocess
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
        self.assertIn('entrypoint: ["bash", ".devcontainer/as-owner.sh"]', read("compose.yaml"))
        subprocess.run(["bash", "-n", os.path.join(DEV, "as-owner.sh")], check=True)
        text = read(".devcontainer", "as-owner.sh")
        self.assertIn("trap 'kill -TERM \"$child\" 2>/dev/null' TERM", text)
        self.assertIn('env --default-signal=INT,QUIT "$@" <&0 &', text)

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


if __name__ == "__main__":
    unittest.main()
