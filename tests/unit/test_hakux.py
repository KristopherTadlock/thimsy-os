"""tools/hakux (hakuX from source in Docker) against the mirrored build's own record.

The same hakuX source is pinned in tools/hakux/build.sh, scripts/hakux_source.sh,
scripts/hakux-build-manifest.json and mods/added-apps.txt; these keep them the same,
check the patch series, and run tools/hakux's helpers on fixtures."""
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


def source_pins():
    """The same from scripts/hakux_source.sh's fetch lines."""
    deps, commit = {}, None
    for url, rev, dest in re.findall(r"^fetch (\S+) ([0-9a-f]{40}) \"([^\"]+)\"$",
                                     util.slurp(f"{util.SCRIPTS}/hakux_source.sh", text=True), re.M):
        if dest == "$DIR/hakuX":
            commit = rev
        else:
            deps[dest.replace("$D/", "")] = (url, rev)
    return commit, deps


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
        self.assertEqual(source_pins()[0], commit)
        manifest = json.loads(util.slurp(f"{util.SCRIPTS}/hakux-build-manifest.json", text=True))
        self.assertEqual(manifest["source"]["hakuX"]["revision"], commit)
        added = util.slurp(f"{util.MODS}/added-apps.txt", text=True)
        self.assertIn(f"source=https://github.com/rfandango/hakuX/tree/{commit}", added)
        self.assertIn(f"to rfandango/hakuX {commit}", util.slurp(os.path.join(PATCHES, "series"), text=True))

    def test_native_deps(self):
        """Every native dependency at the revision the mirrored APK was built with."""
        _, deps = build_pins()
        _, source = source_pins()
        self.assertEqual(sorted(deps), sorted(source))
        manifest = json.loads(util.slurp(f"{util.SCRIPTS}/hakux-build-manifest.json", text=True))["source"]
        for path, (url, rev) in deps.items():
            self.assertEqual(rev, source[path][1], path)
            self.assertEqual(url.removesuffix(".git"), source[path][0].removesuffix(".git"), path)
            self.assertEqual(manifest[f"deps/{path}"]["revision"], rev, path)

    def test_toolchain(self):
        ndk = re.search(r"^ARG NDK_VERSION=(\S+)$", util.slurp(os.path.join(HAKUX, "Dockerfile"), text=True), re.M).group(1)
        manifest = json.loads(util.slurp(f"{util.SCRIPTS}/hakux-build-manifest.json", text=True))
        self.assertEqual(manifest["toolchain"]["ndk"], ndk)
        self.assertIn(f"ndk/{ndk}", util.slurp(f"{util.SCRIPTS}/build_hakux.sh", text=True))
        dockerfile = util.slurp(os.path.join(HAKUX, "Dockerfile"), text=True)
        self.assertIn(f'"build-tools;{manifest["toolchain"]["buildTools"]}"', dockerfile)
        self.assertIn(f'"platforms;android-{manifest["toolchain"]["compileSdk"]}"', dockerfile)
        self.assertIn(f"openjdk-{manifest['toolchain']['jdk']}-jdk-headless", dockerfile)


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
            subprocess.run([sys.executable, "-I", "-m", "py_compile", os.path.join(HAKUX, name + ".py")], check=True,
                           env={**os.environ, "PYTHONPYCACHEPREFIX": self.path("pycache")})

    def test_keys_stay_out_of_git(self):
        ignored = util.slurp(os.path.join(util.ROOT, ".gitignore"), text=True).splitlines()
        for pattern in ("key.properties", "*.jks", "*.keystore"):
            self.assertIn(pattern, ignored)
