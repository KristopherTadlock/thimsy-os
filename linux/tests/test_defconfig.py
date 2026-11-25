"""Tests of linux/tools/defconfig.py and the committed defconfigs (IMPLEMENTATION.md D.4).

Nothing here runs Buildroot. THIMSY_BUILDROOT=<a Buildroot tree at the pinned
version> adds the check of every upstream symbol the fragments name.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

LINUX = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(LINUX / "tools"))

import defconfig  # noqa: E402

# IMPLEMENTATION.md E.3: every package, and the cores of the alpha and v1 sets.
E3_PACKAGES = ["libthimsy", "thimsy-nano", "thimsy-data", "thimsy-session", "thimsy-padd",
               "thimsy-fonts", "thimsy-gpu-pack", "retroarch", "ppsspp", "scummvm"]
ALPHA_CORES = ["snes9x", "genesis_plus_gx", "picodrive", "mgba", "sameboy", "mesen", "fceumm",
               "pcsx_rearmed"]
V1_CORES = ["fbneo", "mame2003_plus", "mamearcade", "stella", "prosystem", "a5200", "gearcoleco",
            "bluemsx", "mednafen_vb", "mednafen_ngp", "mednafen_pce_fast", "mednafen_wswan",
            "mednafen_lynx", "fake08", "opera", "yabasanshiro", "mupen64plus_next", "melondsds",
            "flycast"]


def run_main(*args: str):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = defconfig.main(list(args))
    return code, out.getvalue(), err.getvalue()


class CommittedTree(unittest.TestCase):
    """The real tree: fragments, targets.json, Config.in files and defconfigs."""

    @classmethod
    def setUpClass(cls):
        cls.tree = defconfig.Tree(LINUX)
        cls.merges = {n: cls.tree.merge(n) for n in cls.tree.targets}

    def test_check_passes(self):
        code, _, err = run_main("--check")
        self.assertEqual(code, 0, err)

    def test_each_defconfig_equals_its_merge(self):
        for name, m in self.merges.items():
            with self.subTest(target=name):
                self.assertEqual(m.errors, [])
                path = self.tree.defconfig_path(name)
                self.assertTrue(path.is_file(), "%s is not committed" % path.name)
                have = path.read_text(encoding="utf-8").replace("\r\n", "\n")
                self.assertEqual(have, self.tree.render(m), "run linux/tools/defconfig.py")

    def test_no_defconfig_without_a_target(self):
        for path in (LINUX / "configs").glob("*_defconfig"):
            self.assertIn(path.name[:-len("_defconfig")], self.tree.targets, path.name)

    def test_lint_is_clean(self):
        errors, _ = defconfig.lint(self.tree, self.merges)
        self.assertEqual(errors, [])

    def test_targets_of_section_e4(self):
        t = self.tree.targets
        want = {
            "thimsy_v8_0": ["common", "tier-v8_0", "userland", "gpu-panfrost", "gpu-swrast", "apps-alpha", "dev"],
            "thimsy_v8_2": ["common", "tier-v8_2", "userland", "gpu-panfrost", "apps-alpha", "dev"],
            "thimsy_qemu_aarch64_hw": ["common", "hw-common", "hw-qemu-aarch64"],
        }
        for name, frags in want.items():
            self.assertEqual(t[name].fragments, frags, name)
        self.assertEqual(t["thimsy_rk3566_hw"].fragments[:3], ["common", "hw-common", "hw-rk3566"])
        self.assertIn("generated/rk3566/defconfig.fragment", t["thimsy_rk3566_hw"].fragments)

    def test_release_targets_are_their_dev_twins_with_release(self):
        for tier in ("v8_0", "v8_2"):
            dev, rel = self.tree.targets["thimsy_" + tier], self.tree.targets["thimsy_%s_release" % tier]
            self.assertTrue(rel.publish)
            self.assertFalse(dev.publish)
            self.assertEqual([("release" if f == "dev" else f) for f in dev.fragments], rel.fragments)

    def test_no_publish_or_release_target_includes_dev(self):
        bound = [t for t in self.tree.targets.values() if t.release_bound()]
        self.assertGreaterEqual(len(bound), 2)
        for t in bound:
            with self.subTest(target=t.name):
                self.assertNotIn("dev", t.fragments)
                self.assertEqual(defconfig.release_problems(self.tree, self.merges[t.name]), [])

    def test_release_policy_values(self):
        for t in self.tree.targets.values():
            if not t.release_bound():
                continue
            m = self.merges[t.name]
            with self.subTest(target=t.name):
                login = m.settings.get("BR2_TARGET_ENABLE_ROOT_LOGIN")
                self.assertIsNotNone(login, "the root password must be locked explicitly")
                self.assertIsNone(login.value)
                self.assertFalse(m.is_y("BR2_PACKAGE_ANDROID_TOOLS_ADBD"))
                self.assertFalse(m.is_y("BR2_TARGET_GENERIC_GETTY"))
                self.assertFalse(m.is_y("BR2_THIMSY_FLAVOUR_DEV"))
                self.assertNotIn("rootfs-overlay-dev", m.value("BR2_ROOTFS_OVERLAY") or "")
                if m.is_y("BR2_PACKAGE_DROPBEAR"):
                    self.assertFalse(m.is_y("BR2_PACKAGE_THIMSY_SESSION_SSH_AT_BOOT"))
                    self.assertFalse(m.is_y("BR2_PACKAGE_THIMSY_SESSION_SSH_PASSWORD"))
                if t.kind == "tier":
                    self.assertTrue(m.is_y("BR2_THIMSY_FLAVOUR_RELEASE"))

    def test_dev_targets_are_never_published(self):
        for t in self.tree.targets.values():
            if "dev" in t.fragments:
                self.assertFalse(t.publish, t.name)
                self.assertFalse(t.name.endswith("_release"), t.name)
                self.assertTrue(self.merges[t.name].is_y("BR2_THIMSY_FLAVOUR_DEV"), t.name)

    def test_every_e3_package_has_a_config_in_and_its_symbol(self):
        symbols = self.tree.external_symbols()
        dirs = list(E3_PACKAGES) + ["libretro-" + c.replace("_", "-") for c in ALPHA_CORES + V1_CORES]
        self.assertEqual(len(ALPHA_CORES) + len(V1_CORES), 27)
        for d in dirs:
            with self.subTest(package=d):
                self.assertTrue((LINUX / "package" / d / "Config.in").is_file())
                self.assertIn("BR2_PACKAGE_" + d.upper().replace("-", "_"), symbols)
        for c in ALPHA_CORES + V1_CORES:
            self.assertIn("BR2_PACKAGE_LIBRETRO_" + c.upper(), symbols)

    def test_cores_in_the_app_fragments(self):
        alpha = {s.symbol for s in self.tree.parse("apps-alpha")}
        v1 = {s.symbol for s in self.tree.parse("apps-v1")}
        self.assertEqual({"BR2_PACKAGE_LIBRETRO_" + c.upper() for c in ALPHA_CORES},
                         {s for s in alpha if s.startswith("BR2_PACKAGE_LIBRETRO_")})
        self.assertEqual({"BR2_PACKAGE_LIBRETRO_" + c.upper() for c in V1_CORES},
                         {s for s in v1 if s.startswith("BR2_PACKAGE_LIBRETRO_")})

    def test_external_tree_files(self):
        desc = (LINUX / "external.desc").read_text(encoding="utf-8")
        self.assertRegex(desc, r"(?m)^name: THIMSY$")
        self.assertIn('source "$BR2_EXTERNAL_THIMSY_PATH/package/Config.in"',
                      (LINUX / "Config.in").read_text(encoding="utf-8"))
        mk = (LINUX / "external.mk").read_text(encoding="utf-8")
        macro = mk.index("package/libretro/libretro.mk")
        self.assertLess(macro, mk.index("package/*/*.mk"), "the core macro must be read first")

    def test_paths_exist_or_are_pending(self):
        for ref in self.tree.all_fragment_refs():
            if not self.tree.fragment_path(ref).is_file():
                self.assertIn(ref, self.tree.pending)
                continue
            for s in self.tree.parse(ref):
                for p in defconfig.ext_paths(s.value):
                    with self.subTest(path=p):
                        self.assertTrue((LINUX / p).exists() or p in self.tree.pending)

    def test_hw_toolchain_matches_its_sdk(self):
        for t in self.tree.targets.values():
            if t.kind != "hw":
                continue
            hw, tier = self.merges[t.name], self.merges[t.sdk_from]
            self.assertEqual(defconfig.y_symbol(hw, r"^BR2_TOOLCHAIN_EXTERNAL_GCC_(\d+)$"),
                             defconfig.y_symbol(tier, r"^BR2_GCC_VERSION_(\d+)_X$"))
            self.assertEqual(defconfig.y_symbol(hw, r"^BR2_TOOLCHAIN_EXTERNAL_HEADERS_(\d+_\d+)$"),
                             defconfig.y_symbol(tier, r"^BR2_KERNEL_HEADERS_(\d+_\d+)$"))
            self.assertFalse(hw.is_y("BR2_TOOLCHAIN_BUILDROOT_GLIBC"))

    def test_info_and_list(self):
        code, out, _ = run_main("--info", "thimsy_rk3566_hw")
        self.assertEqual(code, 0)
        self.assertIn("kind=hw\n", out)
        self.assertIn("publish=true\n", out)
        self.assertIn("sdk_from=thimsy_v8_0_release\n", out)
        code, out, _ = run_main("--list", "--publish")
        self.assertEqual(set(out.split()), {n for n, t in self.tree.targets.items() if t.publish})
        self.assertEqual(run_main("--info", "no_such_target")[0], 2)

    @unittest.skipUnless(os.environ.get("THIMSY_BUILDROOT"), "THIMSY_BUILDROOT not set")
    def test_upstream_symbols(self):
        code, _, err = run_main("--check", "--buildroot", os.environ["THIMSY_BUILDROOT"])
        self.assertEqual(code, 0, err)


class Scratch(unittest.TestCase):
    """Merge and lint rules on a small tree written for each test."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-defconfig-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.write("Config.in", 'config BR2_THIMSY_FLAVOUR_DEV\n\tbool "dev"\n'
                                'config BR2_THIMSY_FLAVOUR_RELEASE\n\tbool "release"\n'
                                'source "$BR2_EXTERNAL_THIMSY_PATH/package/Config.in"\n')
        self.write("package/Config.in", 'source "$BR2_EXTERNAL_THIMSY_PATH/package/thimsy-foo/Config.in"\n')
        self.write("package/thimsy-foo/Config.in", 'config BR2_PACKAGE_THIMSY_FOO\n\tbool "foo"\n')
        self.write("board/overlay/etc/inittab", "::sysinit:/etc/init.d/rcS\n")
        self.fragments = {
            "base": """
                BR2_aarch64=y
                BR2_GCC_VERSION_14_X=y
                BR2_KERNEL_HEADERS_6_12=y
                # BR2_TARGET_ENABLE_ROOT_LOGIN is not set
                BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL_THIMSY_PATH)/board/overlay"
                """,
            "pkgs": "BR2_PACKAGE_THIMSY_FOO=y\n",
            "release": "BR2_THIMSY_FLAVOUR_RELEASE=y\n",
            "dev": """
                BR2_THIMSY_FLAVOUR_DEV=y
                # override: dev images allow root login
                BR2_TARGET_ENABLE_ROOT_LOGIN=y
                """,
        }
        self.targets = {
            "t_dev": {"kind": "tier", "fragments": ["base", "pkgs", "dev"]},
            "t_release": {"kind": "tier", "publish": True, "fragments": ["base", "pkgs", "release"]},
        }
        self.pending = {}

    def write(self, rel, text):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def build(self, generate=True):
        for name, text in self.fragments.items():
            self.write("configs/fragments/%s.fragment" % name, textwrap.dedent(text).lstrip("\n"))
        self.write("configs/targets.json", json.dumps(
            {"targets": self.targets, "pending_paths": self.pending}, indent=2))
        if generate:
            code, _, err = run_main("--linux", str(self.dir))
            return code, err
        return None, None

    def check(self, *extra):
        return run_main("--linux", str(self.dir), "--check", *extra)

    def test_clean_tree_generates_and_checks(self):
        code, err = self.build()
        self.assertEqual(code, 0, err)
        self.assertEqual(self.check()[0], 0)
        text = (self.dir / "configs" / "t_dev_defconfig").read_text(encoding="utf-8")
        self.assertIn("# t_dev (tier): base + pkgs + dev\n", text)
        # The override moves the value into the later fragment's section.
        self.assertNotIn("# BR2_TARGET_ENABLE_ROOT_LOGIN is not set", text)
        self.assertRegex(text, r"# --- fragments/dev.fragment\n(.*\n)*BR2_TARGET_ENABLE_ROOT_LOGIN=y")
        rel = (self.dir / "configs" / "t_release_defconfig").read_text(encoding="utf-8")
        self.assertIn("# BR2_TARGET_ENABLE_ROOT_LOGIN is not set", rel)

    def test_change_without_override_is_an_error(self):
        self.fragments["pkgs"] += "# BR2_aarch64 is not set\n"
        code, err = self.build()
        self.assertEqual(code, 1)
        self.assertIn("BR2_aarch64 is y at fragments/base.fragment:1 and not set at "
                      "fragments/pkgs.fragment:2", err)
        self.assertFalse((self.dir / "configs" / "t_dev_defconfig").exists())

    def test_same_value_twice_is_allowed(self):
        self.fragments["pkgs"] += "BR2_aarch64=y\n"
        code, err = self.build()
        self.assertEqual(code, 0, err)
        text = (self.dir / "configs" / "t_dev_defconfig").read_text(encoding="utf-8")
        self.assertEqual(text.count("BR2_aarch64=y"), 1)

    def test_override_with_no_effect_is_an_error(self):
        self.fragments["pkgs"] = "# override: nothing to override\nBR2_PACKAGE_THIMSY_FOO=y\n"
        code, err = self.build()
        self.assertEqual(code, 1)
        self.assertIn("changes nothing in any target", err)

    def test_bad_fragment_lines(self):
        cases = {
            "BR2_FOO=bar\n": "value Kconfig does not read",
            "# override: why\n\nBR2_FOO=y\n": "not followed by a setting",
            "# override: why\n": "not followed by a setting",
            "# override:\nBR2_FOO=y\n": "needs a reason",
            "BR2_FOO=y\nBR2_FOO=y\n": "already set at line 1",
            "#BR2_FOO is not set\n": "exactly",
            "FOO=y\n": "not a setting or a comment",
            "BR2_FOO=y \n": "trailing whitespace",
        }
        for text, message in cases.items():
            with self.subTest(text=text):
                self.write("x.fragment", text)
                with self.assertRaises(defconfig.FragmentError) as cm:
                    defconfig.parse_fragment(self.dir / "x.fragment", "x")
                self.assertIn(message, str(cm.exception))

    def test_publish_target_with_dev_is_refused(self):
        self.targets["t_release"]["fragments"] = ["base", "pkgs", "dev"]
        self.build()
        code, _, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("t_release (marked publish): includes dev.fragment", err)
        self.assertIn("BR2_THIMSY_FLAVOUR_DEV=y", err)

    def test_release_name_without_publish_is_held_to_the_policy(self):
        self.targets["t_release"]["publish"] = False
        self.targets["t_release"]["fragments"] = ["base", "pkgs", "dev"]
        self.build()
        code, _, err = self.check()
        self.assertIn("t_release (named *_release): includes dev.fragment", err)

    def test_release_needs_locked_root_and_no_dev_services(self):
        self.fragments["base"] = self.fragments["base"].replace(
            "# BR2_TARGET_ENABLE_ROOT_LOGIN is not set", "BR2_PACKAGE_ANDROID_TOOLS_ADBD=y")
        self.fragments["dev"] = "BR2_THIMSY_FLAVOUR_DEV=y\n"
        self.write("board/overlay/etc/inittab", "::sysinit:/etc/init.d/rcS\nttyS2::respawn:/sbin/getty -L ttyS2 0 vt100\n")
        self.build()
        code, _, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("root password not locked", err)
        self.assertIn("adbd is enabled", err)
        self.assertIn("etc/inittab:2 starts a getty", err)

    def test_release_with_ssh_at_boot_is_refused(self):
        self.write("package/thimsy-foo/Config.in",
                   'config BR2_PACKAGE_THIMSY_FOO\n\tbool "foo"\n'
                   'config BR2_PACKAGE_THIMSY_SESSION_SSH_AT_BOOT\n\tbool "ssh"\n')
        self.fragments["pkgs"] += "BR2_PACKAGE_DROPBEAR=y\nBR2_PACKAGE_THIMSY_SESSION_SSH_AT_BOOT=y\n"
        self.build()
        code, _, err = self.check()
        self.assertIn("dropbear must be off at boot and key-only", err)

    def test_release_tier_needs_release_flavour(self):
        self.fragments["release"] = "# nothing\n"
        self.build()
        code, _, err = self.check()
        self.assertIn("must set BR2_THIMSY_FLAVOUR_RELEASE=y", err)

    def test_unknown_symbol_of_this_tree(self):
        self.fragments["pkgs"] += "BR2_PACKAGE_THIMSY_FOOO=y\nBR2_PACKAGE_THIMSY_FOO_EXTRA=y\n"
        self.build()
        code, _, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("BR2_PACKAGE_THIMSY_FOOO is not defined", err)
        self.assertIn("BR2_PACKAGE_THIMSY_FOO_EXTRA is not defined", err)

    def test_unsourced_package(self):
        self.write("package/thimsy-bar/Config.in", 'config BR2_PACKAGE_THIMSY_BAR\n\tbool "bar"\n')
        self.build()
        code, _, err = self.check()
        self.assertIn("package/thimsy-bar/Config.in is not sourced", err)

    def test_paths_missing_pending_and_stale(self):
        self.fragments["pkgs"] += 'BR2_ROOTFS_POST_BUILD_SCRIPT="$(BR2_EXTERNAL_THIMSY_PATH)/board/post-build.sh"\n'
        self.build()
        code, _, err = self.check()
        self.assertIn("names board/post-build.sh, which does not exist", err)
        self.pending = {"board/post-build.sh": "G2e", "board/gone.sh": "nobody"}
        self.build()
        code, _, err = self.check("-v")
        self.assertNotIn("names board/post-build.sh", err)
        self.assertIn("board/post-build.sh does not exist yet (pending: G2e)", err)
        self.assertIn("pending_paths lists board/gone.sh, which no fragment or target names", err)
        self.assertEqual(code, 1)

    def test_pending_fragment_is_skipped_and_reported(self):
        self.targets["t_dev"]["fragments"].append("generated/x/defconfig.fragment")
        self.pending = {"generated/x/defconfig.fragment": "Phase 2a"}
        code, err = self.build()
        self.assertEqual(code, 0, err)
        text = (self.dir / "configs" / "t_dev_defconfig").read_text(encoding="utf-8")
        self.assertIn("# Not merged until they exist (pending_paths): generated/x/defconfig.fragment", text)
        code, out, _ = run_main("--linux", str(self.dir), "--info", "t_dev")
        self.assertIn("missing=generated/x/defconfig.fragment\n", out)
        del self.pending["generated/x/defconfig.fragment"]
        self.build(generate=False)
        self.assertEqual(self.check()[0], 1)

    def test_check_finds_stale_and_orphan_defconfigs(self):
        self.build()
        path = self.dir / "configs" / "t_dev_defconfig"
        path.write_text(path.read_text(encoding="utf-8") + "BR2_EXTRA=y\n", encoding="utf-8")
        code, _, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("configs/t_dev_defconfig is stale", err)
        self.build()
        self.write("configs/thimsy_old_defconfig", "BR2_aarch64=y\n")
        code, _, err = self.check()
        self.assertIn("thimsy_old_defconfig has no target", err)

    def test_hw_toolchain_must_match_its_sdk(self):
        self.fragments["hw"] = """
            # override: no toolchain of its own
            # BR2_GCC_VERSION_14_X is not set
            # override: no toolchain of its own
            # BR2_KERNEL_HEADERS_6_12 is not set
            BR2_TOOLCHAIN_EXTERNAL=y
            BR2_TOOLCHAIN_EXTERNAL_GCC_13=y
            BR2_TOOLCHAIN_EXTERNAL_HEADERS_6_12=y
            """
        self.targets["t_hw"] = {"kind": "hw", "publish": True, "sdk_from": "t_release",
                                "fragments": ["base", "hw"]}
        code, err = self.build()
        self.assertEqual(code, 1)
        self.assertIn("t_hw states GCC 13 for its SDK, but t_release builds 14", err)
        self.assertNotIn("kernel headers", err)
        self.targets["t_hw"]["sdk_from"] = "t_dev"
        self.build()
        code, _, err = self.check()
        self.assertIn("t_hw is published but its SDK comes from t_dev", err)

    def test_buildroot_symbols(self):
        self.build()
        br = self.dir / "buildroot"
        upstream = ["BR2_aarch64", "BR2_GCC_VERSION_14_X", "BR2_TARGET_ENABLE_ROOT_LOGIN",
                    "BR2_ROOTFS_OVERLAY"]
        (br / "package" / "thimsy-foo").mkdir(parents=True)
        (br / "Config.in").write_text("".join("config %s\n\tbool\n" % s for s in upstream))
        (br / "Config.in.legacy").write_text("config BR2_KERNEL_HEADERS_6_12\n\tbool\n")
        code, _, err = self.check("--buildroot", str(br))
        self.assertEqual(code, 1)
        self.assertIn("BR2_KERNEL_HEADERS_6_12 is a legacy symbol", err)
        self.assertIn("package thimsy-foo clashes with Buildroot's package/thimsy-foo", err)
        self.assertNotIn("BR2_aarch64", err)
        self.assertNotIn("BR2_PACKAGE_THIMSY_FOO is not", err)
        empty = self.dir / "empty"
        empty.mkdir()
        self.assertIn("does not look like a Buildroot tree", self.check("--buildroot", str(empty))[2])


if __name__ == "__main__":
    unittest.main()
