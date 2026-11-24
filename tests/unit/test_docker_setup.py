"""The Docker setup (compose.yaml, .devcontainer/, tools/hakux/Dockerfile) agrees with itself, the
README and what scripts/build.sh needs. Static checks: no Docker needed."""
import json
import os
import re

from tests import util

DEV = os.path.join(util.ROOT, ".devcontainer")


def compose():
    return util.slurp(os.path.join(util.ROOT, "compose.yaml"), text=True)


def devcontainer():
    """devcontainer.json, which allows // comments."""
    text = util.slurp(os.path.join(DEV, "devcontainer.json"), text=True)
    return json.loads(re.sub(r"^\s*//.*$", "", text, flags=re.M))


def work_volume():
    return re.search(r"^  work:\n    name: (\S+)$", compose(), re.M).group(1)


class DockerSetup(util.TestCase):
    def test_one_work_volume(self):
        """compose and the Dev Container share work/, so their downloads are the same."""
        name = work_volume()
        self.assertIn(f"source={name},target=/workspaces/thimsy-os/work,type=volume", devcontainer()["mounts"])
        self.assertIn(f"`{name}`", util.slurp(os.path.join(util.ROOT, "README.md"), text=True))

    def test_services(self):
        text = compose()
        self.assertEqual(re.findall(r"^    build: (\S+)$", text, re.M), [".devcontainer", "tools/hakux"])
        self.assertEqual(text.count('entrypoint: ["bash", ".devcontainer/as-owner.sh"]'), 2)
        self.assertEqual(devcontainer()["build"]["dockerfile"], "Dockerfile")
        self.assertIn("--privileged", devcontainer()["runArgs"])
        # The build loop-mounts images; hakux, unprivileged, needs SELinux labels off to read the checkout.
        build, hakux = text.split("\n  hakux:\n")
        self.assertIn("\n    privileged: true\n", build)
        self.assertIn('\n    security_opt: ["label=disable"]\n', hakux)
        # docker stop gives the build time to clean up.
        self.assertRegex(build, r"\n    stop_grace_period: \d+m\n")
        self.assertRegex(hakux, r"\n    stop_grace_period: \d+m\n")

    def test_images(self):
        for path in (os.path.join(DEV, "Dockerfile"), os.path.join(util.ROOT, "tools", "hakux", "Dockerfile")):
            text = util.slurp(path, text=True)
            # Google's Android tools are x86-64 only.
            self.assertRegex(text, r"(?m)^FROM --platform=linux/amd64 ubuntu:\d+\.\d+$", path)
            # A failed download in a pipe fails the build.
            self.assertIn('SHELL ["/bin/bash", "-o", "pipefail", "-c"]', text, path)

    def test_image_has_the_tools_build_sh_checks(self):
        """Every tool scripts/build.sh stops without (but the 1.0a-only nano-bootlogo) comes with the image:
        from an apt package line of its own, or a link the image makes in /usr/local/bin."""
        packages = {"fsck.erofs": "erofs-utils", "mkfs.erofs": "erofs-utils", "lz4": "lz4", "debugfs": "e2fsprogs",
                    "secilc": "secilc", "7z": "7zip", "7zz": "7zip", "aapt": "google-android-build-tools-34.0.0-installer",
                    "fdtoverlay": "device-tree-compiler"}
        links = {"avbtool", "unpack_bootimg"}
        image = util.slurp(os.path.join(DEV, "Dockerfile"), text=True)
        build = util.slurp(f"{util.SCRIPTS}/build.sh", text=True)
        tools = set()
        for group in re.findall(r"^for tool in ([^;]+); do\n  command -v", build, re.M):
            tools.update(group.split())
        tools.update(re.findall(r"command -v ([a-z0-9._-]+)(?: >/dev/null)? \|\|", build))
        tools.update(re.findall(r"! command -v ([a-z0-9._-]+)(?: >/dev/null)?; then", build))
        tools.discard("nano-bootlogo")
        self.assertTrue({"fsck.erofs", "secilc", "7z", "aapt", "fdtoverlay"} <= tools, tools)
        for tool in sorted(tools):
            if tool in links:
                self.assertRegex(image, rf"ln -s \S+ /usr/local/bin/{re.escape(tool)}\b", tool)
            else:
                self.assertIn(tool, packages, f"build.sh needs {tool}: say which package provides it")
                self.assertRegex(image, rf"(?m)^\s+{re.escape(packages[tool])} \\$", tool)
        # aapt is found through PATH, in build tools 34.0.0.
        self.assertRegex(image, r"(?m)^ENV PATH=\$ANDROID_HOME/build-tools/34\.0\.0:\$PATH$")
        self.assertRegex(image, r"(?m)^ENV MKBOOTIMG=/\S+mkbootimg\.py$")
        self.assertRegex(image, r"(?m)^ENV AVB_KEY=/\S+\.pem$")

    def test_scripts(self):
        owner = os.path.join(DEV, "as-owner.sh")
        self.assertOk(util.run(["bash", "-n", owner]))
        text = util.slurp(owner, text=True)
        # As PID 1, it passes docker stop's SIGTERM on to its process group, and gives the command
        # SIGINT back.
        self.assertIn("trap 'trap \"\" TERM; kill -TERM 0 2>/dev/null' TERM", text)
        self.assertIn('env --default-signal=INT,QUIT "$@" <&0 &', text)

    def test_line_endings(self):
        attrs = util.slurp(os.path.join(util.ROOT, ".gitattributes"), text=True).splitlines()
        self.assertIn("* text=auto eol=lf", attrs)
        for ext in ("png", "webp", "zip"):
            self.assertIn(f"*.{ext} binary", attrs)
