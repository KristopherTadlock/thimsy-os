"""tools/hakux (hakuX from source in Docker) against the build mods/added-apps.txt installs.

tools/hakux/build.sh pins the hakuX commit and names the build (THIMSY_BUILD); hakuX's line
in mods/added-apps.txt must be that build. These check the two agree, check the patch series,
and run tools/hakux's helpers on fixtures."""
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import unittest

from tests import util

HAKUX = os.path.join(util.ROOT, "tools", "hakux")
PATCHES = os.path.join(HAKUX, "patches")


def load(name):
    """tools/hakux/NAME.py as a module, leaving no __pycache__ beside it (the source tarball packs tools/hakux)."""
    spec = importlib.util.spec_from_file_location("hakux_" + name, os.path.join(HAKUX, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    dont_write, sys.dont_write_bytecode = sys.dont_write_bytecode, True
    try:
        spec.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = dont_write
    return module


def build_sh():
    return util.slurp(os.path.join(HAKUX, "build.sh"), text=True)


def build_pins():
    """(hakuX commit, {NC_DEPS path: (repository, commit)}) from tools/hakux/build.sh."""
    text = build_sh()
    commit = re.search(r"^HAKUX_COMMIT=([0-9a-f]{40})$", text, re.M).group(1)
    block = re.search(r"^DEPS=\(\n(.*?)^\)", text, re.M | re.S).group(1)
    deps = {}
    for line in block.splitlines():
        path, url, rev, _name = line.strip().strip('"').split()
        deps[path] = (url, rev)
    return commit, deps


def thimsy_build():
    """THIMSY_BUILD from tools/hakux/build.sh."""
    return int(re.search(r"^THIMSY_BUILD=(\d+)$", build_sh(), re.M).group(1))


def added_entry():
    """(file, fields) of hakuX's line in mods/added-apps.txt, split the way 40-added-apps.sh does."""
    found = []
    for line in util.slurp(f"{util.MODS}/added-apps.txt", text=True).splitlines():
        if line.startswith("apk ") and "package=com.rfandango.haku_x" in line:
            _kind, file, *pairs = re.split(r" {2,}", line)
            found.append((file, dict(p.split("=", 1) for p in pairs)))
    assert len(found) == 1, found
    return found[0]


def series():
    """[(patch file, source)] from tools/hakux/patches/series."""
    out = []
    for line in util.slurp(os.path.join(PATCHES, "series"), text=True).splitlines():
        if line.strip() and not line.startswith("#"):
            name, source = line.split(None, 1)
            out.append((name, source))
    return out


class HakuxPins(util.TestCase):
    def test_hakux_commit(self):
        commit, _ = build_pins()
        _file, fields = added_entry()
        self.assertEqual(fields["source"], f"https://github.com/rfandango/hakuX/tree/{commit}")
        self.assertIn(f"to rfandango/hakuX {commit}", util.slurp(os.path.join(PATCHES, "series"), text=True))

    def test_added_apps_entry_is_this_build(self):
        """The file build.sh names (hakuX-<version>-<commit>-thimsy<N>.apk, signed), from a mirror release."""
        commit, _ = build_pins()
        file, fields = added_entry()
        self.assertRegex(file, rf"^hakuX-\d+(\.\d+)*-{commit[:7]}-thimsy{thimsy_build()}\.apk$")
        self.assertRegex(fields["url"], r"^https://github\.com/KristopherTadlock/thimsy-os/releases/download/[^/]+/")
        self.assertTrue(fields["url"].endswith("/" + file), fields["url"])
        self.assertRegex(fields["sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(fields["name"], "hakuX Xbox emulator")
        # 47-hakux-folder.sh's tests find hakuX in the app list by its file in /system/etc.
        e2e = util.slurp(os.path.join(util.TESTS, "e2e", "test_offline_mods.py"), text=True)
        self.assertIn(f"|com.rfandango.haku_x|/system/etc/{file}|", e2e)

    def test_native_deps(self):
        """Every native dependency at a full commit of an https repository, each in its own folder."""
        _, deps = build_pins()
        self.assertTrue(deps)
        for path, (url, rev) in deps.items():
            self.assertRegex(url, r"^https://\S+\.git$", path)
            self.assertRegex(rev, r"^[0-9a-f]{40}$", path)
        self.assertEqual(len({url for url, _ in deps.values()}), len(deps))

    def test_toolchain(self):
        """The build-tools build.sh signs the test-key copy with are the ones the image installs."""
        dockerfile = util.slurp(os.path.join(HAKUX, "Dockerfile"), text=True)
        tools = re.search(r'^  bt="\$ANDROID_HOME/build-tools/([^"]+)"$', build_sh(), re.M).group(1)
        self.assertIn(f'"build-tools;{tools}"', dockerfile)


class HakuxPatches(util.TestCase):
    def test_series_lists_every_patch_once(self):
        names = [name for name, _ in series()]
        self.assertEqual(names, sorted(f for f in os.listdir(PATCHES) if f.endswith(".patch")))
        self.assertEqual(len(names), len(set(names)))
        for i, name in enumerate(names, 1):
            self.assertTrue(name.startswith(f"{i:04d}-"), name)

    def test_each_patch_is_the_upstream_commit_it_names(self):
        for name, source in series():
            first = util.slurp(os.path.join(PATCHES, name), text=True).split("\n", 1)[0]
            m = re.match(r"From ([0-9a-f]{40}) ", first)
            self.assertTrue(m, f"{name}: not a git format-patch file")
            self.assertIn(m.group(1), source, name)
            self.assertRegex(source, r"^https://github\.com/", name)


class HakuxKeyProperties(util.TestCase):
    def test_relative_store_file_is_next_to_the_properties(self):
        rewrite = load("keyprops").rewrite
        text = "storePassword=a\nstoreFile=keys/release.jks\nkeyAlias=k\nkeyPassword=b\n"
        self.assertEqual(rewrite(text, "/w/co"),
                         "storePassword=a\nstoreFile=" + os.path.join("/w/co", "keys/release.jks") + "\nkeyAlias=k\nkeyPassword=b\n")
        self.assertEqual(rewrite("storeFile = r.jks", "/w"), "storeFile=" + os.path.join("/w", "r.jks"))

    def test_absolute_or_empty_store_file_is_kept(self):
        rewrite = load("keyprops").rewrite
        for text in ("storeFile=/keys/r.jks\n", "storeFile=\n", "# storeFile=r.jks\n", "keyAlias=storeFile\n"):
            self.assertEqual(rewrite(text, "/w"), text)

    def test_copies_the_file(self):
        props = self.write("keys/key.properties", "storeFile=release.jks\nkeyAlias=k\n")
        dst = self.path("out.properties")
        self.assertOk(util.run([sys.executable, "-I", os.path.join(HAKUX, "keyprops.py"), props, dst]))
        self.assertEqual(util.slurp(dst, text=True),
                         "storeFile=" + os.path.join(self.path("keys"), "release.jks") + "\nkeyAlias=k\n")


@unittest.skipUnless(shutil.which("git"), "needs git")
class HakuxManifest(util.TestCase):
    def test_manifest(self):
        src, deps_dir, out = self.path("src"), self.path("deps"), self.path("out")
        self.write("src/android/app/build.gradle.kts",
                   'compileSdk = 36\nbuildToolsVersion = "36.1.0"\nndkVersion = "29.0.14206865"\n'
                   'versionCode = 9\nversionName = "1.3.0"\n')
        self.write("src/android/gradle/wrapper/gradle-wrapper.properties",
                   "distributionUrl=https\\://services.gradle.org/distributions/gradle-9.1.0-bin.zip\n")
        os.makedirs(f"{src}/external/fex")
        env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
        for cmd in (["git", "init", "-q"], ["git", "commit", "-q", "--allow-empty", "-m", "fex"]):
            self.assertOk(util.run(cmd, cwd=f"{src}/external/fex", env=env))
        fex = util.run(["git", "rev-parse", "HEAD"], cwd=f"{src}/external/fex").stdout.strip()
        here = self.path("hakux")
        self.write("hakux/patches/0001-x.patch", "From " + "a" * 40 + " Mon Sep 17 00:00:00 2001\n")
        self.write("hakux/patches/series", "# comment\n0001-x.patch  https://example.org/x\n")
        self.write("out/hakuX-a-unsigned.apk", b"apk")
        self.write("out/hakuX-a-source.tar.xz", b"src")
        self.write("out/notes.txt", "not an output")
        os.makedirs(deps_dir)
        dest = self.path("out/hakuX-build-manifest.json")
        self.assertOk(util.run([sys.executable, "-I", os.path.join(HAKUX, "manifest.py"), dest, src, deps_dir, here, out,
                                "https://github.com/rfandango/hakuX.git", "c" * 40, "2",
                                "volk https://github.com/zeux/volk.git " + "d" * 40 + " 1.4.304",
                                "adrenotools https://github.com/bylaws/libadrenotools.git " + "e" * 40 + " 8fae8ce"]))
        m = json.loads(util.slurp(dest, text=True))
        self.assertEqual(m["hakux"], {"repository": "https://github.com/rfandango/hakuX.git", "commit": "c" * 40,
                                      "versionName": "1.3.0", "versionCode": 9, "thimsy_build": 2})
        self.assertEqual(m["patches"], [{"file": "0001-x.patch", "source": "https://example.org/x",
                                         "sha256": hashlib.sha256(util.slurp(f"{here}/patches/0001-x.patch")).hexdigest()}])
        self.assertEqual(m["fex"], {"commit": fex, "submodules": {}})
        self.assertEqual([d["path"] for d in m["native_deps"]], ["volk", "adrenotools"])
        self.assertEqual(m["native_deps"][1]["patched"], "tools/native/adrenotools-kgsl-context-priority.patch")
        self.assertIsNone(m["native_deps"][0]["patched"])
        self.assertEqual((m["toolchain"]["compileSdk"], m["toolchain"]["buildTools"], m["toolchain"]["ndk"],
                          m["toolchain"]["gradle"]), (36, "36.1.0", "29.0.14206865", "9.1.0"))
        self.assertEqual(m["outputs"], {"hakuX-a-source.tar.xz": hashlib.sha256(b"src").hexdigest(),
                                        "hakuX-a-unsigned.apk": hashlib.sha256(b"apk").hexdigest()})


class HakuxScripts(util.TestCase):
    def test_build_script(self):
        path = os.path.join(HAKUX, "build.sh")
        self.assertOk(util.run(["bash", "-n", path]))
        text = build_sh()
        self.assertIn("\nset -euo pipefail\n", text)
        # The copied key.properties (with the key's passwords) goes even when the build fails.
        self.assertRegex(text, r"\ntrap 'rm -f \"\$SRC/android/key\.properties\"' EXIT\n")
        for name in ("manifest", "keyprops"):
            # In memory: no __pycache__ beside them, which the source tarball would not want.
            path = os.path.join(HAKUX, name + ".py")
            compile(util.slurp(path, text=True), path, "exec")

    def test_keys_stay_out_of_git(self):
        ignored = util.slurp(os.path.join(util.ROOT, ".gitignore"), text=True).splitlines()
        for pattern in ("key.properties", "*.jks", "*.keystore"):
            self.assertIn(pattern, ignored)


@unittest.skipUnless(shutil.which("git") and shutil.which("xz") and shutil.which("tar"), "needs git, tar and xz")
class HakuxSourceTarball(util.TestCase):
    """build.sh's source tarball step, run on a fixture: what git tracks in tools/hakux and the patched
    sources go in; a key, key.properties, .git and bytecode don't."""

    def tarball_step(self):
        text = build_sh()
        start = text.index('if [[ -z "${SOURCE_DIR:-}" ]]; then\n  rm -f "$OUT"/hakuX-*-source.tar.xz')
        return text[start:text.index("\nfi\n", start) + 4]

    def test_packs_only_what_git_tracks(self):
        root, work, out = self.path("co"), self.path("co/work/hakux"), self.path("co/out/hakux")
        env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
               "GIT_COMMITTER_EMAIL": "t@t"}
        self.write("co/tools/hakux/build.sh", "#!/bin/sh\n")
        self.write("co/tools/hakux/patches/series", "0001-x.patch  https://example.org\n")
        self.assertOk(util.run(["git", "init", "-q"], cwd=root, env=env))
        self.assertOk(util.run(["git", "add", "tools"], cwd=root, env=env))
        self.assertOk(util.run(["git", "commit", "-q", "-m", "t"], cwd=root, env=env))
        # Left in the folder, not in git: none of it may be published.
        self.write("co/tools/hakux/key.properties", "storePassword=secret\n")
        self.write("co/tools/hakux/release.jks", b"key")
        self.write("co/tools/hakux/__pycache__/m.cpython-312.pyc", b"pyc")
        self.write("co/work/hakux/hakuX/README.md", "hakuX\n")
        self.write("co/work/hakux/hakuX/.git/HEAD", "ref\n")
        self.write("co/work/hakux/hakuX/external/fex/.git", "gitdir: x\n")
        self.write("co/work/hakux/deps/volk/volk.c", "int x;\n")
        os.makedirs(out)
        step = "set -euo pipefail\n" + self.tarball_step()
        proc = util.run(["bash", "-c", step], cwd=root, env={"ROOT": root, "WORK": work, "OUT": out, "name": "hx",
                                                              "FEX_COMMIT": "f" * 40, "SOURCE_DIR": ""})
        self.assertOk(proc)
        listing = util.run(["tar", "-tJf", f"{out}/hx-source.tar.xz"]).stdout.split()
        files = sorted(n for n in listing if not n.endswith("/"))
        self.assertEqual(files, ["hx-source/FEX_COMMIT", "hx-source/deps/volk/volk.c", "hx-source/hakuX/README.md",
                                 "hx-source/tools/hakux/build.sh", "hx-source/tools/hakux/patches/series"])
        commit = util.run(["tar", "-xJOf", f"{out}/hx-source.tar.xz", "hx-source/FEX_COMMIT"]).stdout
        self.assertEqual(commit, "FEX_COMMIT=" + "f" * 40 + "\n")
        self.assertFalse(os.path.exists(f"{work}/source-tools"))

    def test_refuses_a_key_in_tools_hakux(self):
        text = build_sh()
        self.assertIn('"$HERE"/*) echo "KEY_PROPERTIES: keep the key outside tools/hakux', text)
