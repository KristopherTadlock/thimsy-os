"""Tests of the package recipes, linux/tools/lint_packages.py, linux/tools/assets_allowlist.py,
thimsy-gpu-pack's gpu_pack.py and linux/tools/bt_pack.py with post-build.d/40-bt-pack.sh
(IMPLEMENTATION.md D.4, test_packages; step G2d; decision OA-15).

Nothing here runs Buildroot. THIMSY_BUILDROOT=<a Buildroot tree at the pinned version>
adds Buildroot's own utils/check-package over every recipe.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

LINUX = Path(__file__).resolve().parent.parent
REPO = LINUX.parent
sys.path.insert(0, str(LINUX / "tools"))

import assets_allowlist as aa  # noqa: E402
import bt_pack  # noqa: E402
import check_rcs  # noqa: E402
import lint_packages as lp  # noqa: E402

E3_PACKAGES = ["libthimsy", "thimsy-nano", "thimsy-data", "thimsy-session", "thimsy-padd",
               "thimsy-fonts", "thimsy-gpu-pack", "retroarch", "ppsspp", "scummvm"]
HAVE_ASSETS = (REPO / "ASSETS.md").is_file() and (REPO / "nano" / "Android.bp").is_file() \
    and (REPO / "tools" / "nano-tree" / "nanotree" / "androidbp.py").is_file()


def load(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def quiet(fn, *args, **kw):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = fn(*args, **kw)
    return code, out.getvalue(), err.getvalue()


class CommittedRecipes(unittest.TestCase):
    """The real tree."""

    @classmethod
    def setUpClass(cls):
        cls.report = lp.lint(LINUX, REPO)

    def test_lint_has_no_errors(self):
        self.assertEqual(self.report.errors, [])

    def test_every_package_has_a_recipe(self):
        dirs = [d.name for d in lp.package_dirs(LINUX)]
        for name in E3_PACKAGES:
            self.assertIn(name, dirs)
        for d in lp.package_dirs(LINUX):
            self.assertTrue((d / (d.name + ".mk")).is_file(), d.name)
        self.assertEqual(len([d for d in dirs if d.startswith("libretro-")]), 27)

    def test_cores_are_reported_pending_not_passing(self):
        # Decision I10: until mods/retroarch-cores.src.lock exists, every core is pending.
        if (REPO / "mods" / "retroarch-cores.src.lock").is_file():
            self.skipTest("the source lock exists; the cores are checked against it")
        joined = "\n".join(self.report.pending)
        self.assertIn("retroarch-cores.src.lock does not exist yet", joined)
        self.assertIn("27 cores", joined)
        code, _, _ = quiet(lp.main, ["-q", "--strict"])
        self.assertEqual(code, 1)
        code, _, _ = quiet(lp.main, ["-q"])
        self.assertEqual(code, 0)

    def test_core_versions_are_todo_or_full_commits(self):
        for d in lp.package_dirs(LINUX):
            if d.name.startswith("libretro-"):
                v = lp.Recipe(d / (d.name + ".mk")).vars[lp.prefix_of(d.name) + "_VERSION"]
                self.assertTrue(v == "TODO" or re.match(r"^[0-9a-f]{40}$", v), (d.name, v))

    def test_versions_follow_the_android_build(self):
        # RetroArch and PPSSPP from mods/10-update-emulators.sh, ScummVM from mods/added-apps.txt.
        upd = REPO / "mods" / "10-update-emulators.sh"
        apps = REPO / "mods" / "added-apps.txt"
        if not upd.is_file() or not apps.is_file():
            self.skipTest("mods/ is not in this tree")
        text = upd.read_text(encoding="utf-8")
        ra = re.search(r'"RetroArch ([0-9.]+)\|', text).group(1)
        pp = re.search(r'"PPSSPP ([0-9.]+)\|', text).group(1)
        sv = re.search(r"scummvm-([0-9.]+)\.apk", apps.read_text(encoding="utf-8")).group(1)

        def version(pkg):
            return lp.Recipe(LINUX / "package" / pkg / (pkg + ".mk")).vars[lp.prefix_of(pkg) + "_VERSION"]
        self.assertEqual(version("retroarch"), ra)
        self.assertEqual(version("ppsspp"), "v" + pp)
        self.assertEqual(version("scummvm"), sv)

    def test_core_repositories_match_core_sources(self):
        script = REPO / "scripts" / "core_sources.py"
        if not script.is_file():
            self.skipTest("scripts/core_sources.py is not in this tree")
        cs = load(script, "core_sources_for_test")
        recipes = {}
        for d in lp.package_dirs(LINUX):
            if d.name.startswith("libretro-"):
                r = lp.Recipe(d / (d.name + ".mk"))
                recipes[lp.core_name(d.name, r)] = lp.site_repository(r.vars[lp.prefix_of(d.name) + "_SITE"])
        self.assertEqual(recipes, {k: v[0] for k, v in cs.LINUX_CORES.items()})

    def test_core_recipes_use_the_shared_macro(self):
        for d in lp.package_dirs(LINUX):
            if d.name.startswith("libretro-"):
                r = lp.Recipe(d / (d.name + ".mk"))
                self.assertIn(r.evals[0], ("libretro-core-package", "libretro-cmake-core-package"), d.name)

    def test_yabasanshiro_and_mupen64plus_build_options(self):
        # Plan 6.2: platform=arm64_cortex_a53_gles3 and GLES3=1.
        y = lp.Recipe(LINUX / "package/libretro-yabasanshiro/libretro-yabasanshiro.mk").vars
        self.assertEqual(y["LIBRETRO_YABASANSHIRO_PLATFORM"], "arm64_cortex_a53_gles3")
        m = lp.Recipe(LINUX / "package/libretro-mupen64plus-next/libretro-mupen64plus-next.mk").vars
        self.assertIn("GLES3=1", m["LIBRETRO_MUPEN64PLUS_NEXT_MAKE_OPTS"])

    def test_repository_tarball_packages(self):
        # E.1 and I41: libthimsy, Nano and the assets come from build.sh's tarballs with a
        # file site, so legal-info saves them; their hashes come from build.sh.
        for pkg, src in (("libthimsy", "libthimsy"), ("thimsy-nano", "nano"), ("thimsy-data", "nano-assets")):
            v = lp.Recipe(LINUX / "package" / pkg / (pkg + ".mk")).vars
            p = lp.prefix_of(pkg)
            self.assertEqual(v[p + "_SITE_METHOD"], "file")
            self.assertEqual(v[p + "_VERSION"], "$(THIMSY_GIT_SHA)")
            self.assertEqual(v[p + "_SOURCE"], "%s-$(%s_VERSION).tar.gz" % (src, p))
            self.assertFalse((LINUX / "package" / pkg / (pkg + ".hash")).exists())
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        for pkg in ("libthimsy", "thimsy-nano", "thimsy-data"):
            self.assertIn(pkg, build)

    def test_nano_licence_names_the_kanji_table(self):
        v = lp.Recipe(LINUX / "package/thimsy-nano/thimsy-nano.mk").vars
        self.assertIn("GPL-2.0+ (NanoOskKanji.h", v["THIMSY_NANO_LICENSE"])
        self.assertIn("NanoOskKanji.h", v["THIMSY_NANO_LICENSE_FILES"].split())

    def test_nano_license_and_notice(self):
        """I41: nano/LICENSE (Apache-2.0) and nano/NOTICE exist, are listed, are fork-only (the
        Android patch does not carry them) and are no longer reported as pending (gfix)."""
        v = lp.Recipe(LINUX / "package/thimsy-nano/thimsy-nano.mk").vars
        files = v["THIMSY_NANO_LICENSE_FILES"].split()
        for f in ("LICENSE", "NOTICE"):
            self.assertIn(f, files)
            self.assertTrue((REPO / "nano" / f).is_file(), f)
        self.assertIn("Apache License", (REPO / "nano/LICENSE").read_text(encoding="utf-8")[:200])
        notice = (REPO / "nano/NOTICE").read_text(encoding="utf-8")
        for part in ("NanoOskKanji.h", "liba52/", "NanoAc3", "nanosvg/", "rcheevos/"):
            self.assertIn(part, notice)
        upstream = json.loads((REPO / "tools/nano-tree/upstream.json").read_text(encoding="utf-8"))
        self.assertIn("LICENSE", upstream["fork_only"])
        self.assertIn("NOTICE", upstream["fork_only"])
        self.assertFalse([w for w in self.report.pending if "NOTICE" in w or "nano/LICENSE" in w])

    def test_gpl_package_menu_is_sourced(self):
        top = (LINUX / "Config.in").read_text(encoding="utf-8")
        self.assertIn('source "$BR2_EXTERNAL_THIMSY_PATH/gpl/package/Config.in"', top)
        self.assertTrue((LINUX / "gpl" / "package" / "Config.in").is_file())

    def test_libretro_macro_is_included_first(self):
        mk = (LINUX / "external.mk").read_text(encoding="utf-8")
        self.assertLess(mk.index("package/libretro/libretro.mk"), mk.index("package/*/*.mk"))
        macro = (LINUX / "package/libretro/libretro.mk").read_text(encoding="utf-8")
        for name in ("libretro-core-package =", "libretro-cmake-core-package =", "LIBRETRO_CORE_NAMES +="):
            self.assertIn(name, macro)

    @unittest.skipUnless(os.environ.get("THIMSY_BUILDROOT"), "THIMSY_BUILDROOT is not set")
    def test_buildroot_check_package(self):
        rep = lp.Report()
        lp.check_package_tool(rep, LINUX, Path(os.environ["THIMSY_BUILDROOT"]))
        self.assertEqual(rep.errors, [])


class ScratchLint(unittest.TestCase):
    """Lint rules on a small tree written for each test."""

    MK = textwrap.dedent("""\
        ################################################################################
        #
        # {name}
        #
        ################################################################################

        {P}_VERSION = 1.0
        {P}_SITE = https://example.org
        {P}_LICENSE = MIT
        {P}_LICENSE_FILES = COPYING

        $(eval $(generic-package))
        """)

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-lint-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.linux = self.dir / "linux"
        self.write("linux/Config.in", 'source "$BR2_EXTERNAL_THIMSY_PATH/package/Config.in"\n'
                                      'source "$BR2_EXTERNAL_THIMSY_PATH/gpl/package/Config.in"\n')
        self.write("linux/gpl/package/Config.in", 'menu "x"\nendmenu\n')
        self.write("linux/package/libretro/libretro.mk", "# macro\n")
        self.pkgs = []
        self.add("thimsy-foo")

    def write(self, rel, text):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(str(p), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def add(self, name, mk=None, hash_text="sha256  %s  foo-1.0.tar.gz\n" % ("0" * 64), config=None):
        P = lp.prefix_of(name)
        self.pkgs.append(name)
        self.write("linux/package/Config.in", "".join(
            'source "$BR2_EXTERNAL_THIMSY_PATH/package/%s/Config.in"\n' % n for n in self.pkgs))
        self.write("linux/package/%s/Config.in" % name,
                   config if config is not None else 'config BR2_PACKAGE_%s\n\tbool "%s"\n' % (P, name))
        self.write("linux/package/%s/%s.mk" % (name, name), mk or self.MK.format(name=name, P=P))
        if hash_text is not None:
            self.write("linux/package/%s/%s.hash" % (name, name), hash_text)

    def lint(self):
        return lp.lint(self.linux, self.dir)

    def test_clean(self):
        rep = self.lint()
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.pending, [])

    def test_missing_fields_and_bad_prefix(self):
        self.add("thimsy-bar", mk=textwrap.dedent("""\
            ################################################################################
            #
            # thimsy-bar
            #
            ################################################################################

            THIMSY_BAR_VERSION = 1.0
            FOO_SITE = x

            $(eval $(generic-package))
            """))
        errors = "\n".join(self.lint().errors)
        self.assertIn("THIMSY_BAR_SITE is not set", errors)
        self.assertIn("THIMSY_BAR_LICENSE_FILES is not set", errors)
        self.assertIn("FOO_SITE is not THIMSY_BAR_...", errors)

    def test_header_eval_and_sourcing(self):
        P = "THIMSY_BAR"
        self.add("thimsy-bar", mk=self.MK.format(name="wrong", P=P).replace(
            "$(eval $(generic-package))", "$(eval $(made-up-package))"))
        self.write("linux/package/thimsy-baz/Config.in", 'config BR2_PACKAGE_THIMSY_BAZ\n\tbool "baz"\n')
        errors = "\n".join(self.lint().errors)
        self.assertIn("header block naming thimsy-bar", errors)
        self.assertIn("exactly one $(eval", errors)
        self.assertIn("package/thimsy-baz/Config.in is not sourced", errors)
        self.assertIn("thimsy-baz.mk is missing", errors)

    def test_symbol_must_exist(self):
        self.add("thimsy-bar", config='config BR2_PACKAGE_THIMSY_OTHER\n\tbool "x"\n')
        self.assertIn("does not define BR2_PACKAGE_THIMSY_BAR", "\n".join(self.lint().errors))

    def test_hash_files(self):
        self.add("thimsy-bar", hash_text=None)
        self.add("thimsy-baz", hash_text="sha256  abc  foo.tar.gz\n")
        self.add("thimsy-qux", hash_text="# nothing yet\n")
        rep = self.lint()
        errors = "\n".join(rep.errors)
        self.assertIn("thimsy-bar/thimsy-bar.hash is missing", errors)
        self.assertIn("sha256 hash must be 64", errors)
        self.assertIn("thimsy-qux: thimsy-qux.hash has no hash yet", "\n".join(rep.pending))

    def test_local_and_tarball_sites_take_no_hash(self):
        mk = self.MK.format(name="thimsy-bar", P="THIMSY_BAR").replace(
            "THIMSY_BAR_SITE = https://example.org",
            "THIMSY_BAR_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/src/x\nTHIMSY_BAR_SITE_METHOD = local").replace(
            "THIMSY_BAR_VERSION = 1.0", "THIMSY_BAR_VERSION = $(or $(THIMSY_GIT_SHA),dev)")
        self.add("thimsy-bar", mk=mk, hash_text=None)
        mk = self.MK.format(name="thimsy-baz", P="THIMSY_BAZ").replace(
            "THIMSY_BAZ_SITE = https://example.org", "THIMSY_BAZ_SITE = $(THIMSY_SRC_DIR)").replace(
            "THIMSY_BAZ_VERSION = 1.0", "THIMSY_BAZ_VERSION = 1.0\nTHIMSY_BAZ_SITE_METHOD = local")
        self.add("thimsy-baz", mk=mk, hash_text="# x\n")
        errors = "\n".join(self.lint().errors)
        self.assertNotIn("thimsy-bar", errors)
        self.assertIn("thimsy-baz.mk: a build.sh tarball package's _VERSION must be $(THIMSY_GIT_SHA)", errors)
        self.assertIn("needs _SITE_METHOD = file", errors)
        self.assertIn("takes no .hash file", errors)

    def test_local_site_is_versioned_by_the_commit(self):
        """build-integration-3: a fixed version would keep a local site's first rsync forever."""
        mk = self.MK.format(name="thimsy-bar", P="THIMSY_BAR").replace(
            "THIMSY_BAR_SITE = https://example.org",
            "THIMSY_BAR_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/src/x\nTHIMSY_BAR_SITE_METHOD = local")
        self.add("thimsy-bar", mk=mk, hash_text=None)
        errors = "\n".join(self.lint().errors)
        self.assertIn("thimsy-bar.mk: a local site in this tree needs a _VERSION of $(THIMSY_GIT_SHA)", errors)

    def core_mk(self, version, site="$(call github,libretro,snes9x,$(LIBRETRO_SNES9X_VERSION))"):
        return textwrap.dedent("""\
            ################################################################################
            #
            # libretro-snes9x
            #
            ################################################################################

            LIBRETRO_SNES9X_VERSION = {v}
            LIBRETRO_SNES9X_SITE = {s}
            LIBRETRO_SNES9X_LICENSE = Snes9x
            LIBRETRO_SNES9X_LICENSE_FILES = LICENSE

            $(eval $(libretro-core-package))
            """).format(v=version, s=site)

    def test_core_without_source_lock_is_pending(self):
        self.add("libretro-snes9x", mk=self.core_mk("TODO"), hash_text=None)
        rep = self.lint()
        self.assertEqual(rep.errors, [])
        self.assertIn("1 cores have no commit: snes9x", "\n".join(rep.pending))

    def test_core_against_source_lock(self):
        full = "a" * 40
        self.write("mods/retroarch-cores.src.lock",
                   "# x\nsnes9x  https://github.com/libretro/snes9x  %s  snes9x.zip@00\n" % full)
        self.add("libretro-snes9x", mk=self.core_mk("TODO"), hash_text=None)
        self.assertIn("_VERSION is TODO; the lock has", "\n".join(self.lint().pending))
        # --sync-cores writes the commit; the core then passes.
        code, _, _ = quiet(lp.sync_cores, self.linux, self.dir)
        self.assertEqual(code, 0)
        self.write("linux/package/libretro-snes9x/libretro-snes9x.hash",
                   "sha256  %s  libretro-snes9x-%s.tar.gz\n" % ("1" * 64, full))
        rep = self.lint()
        self.assertEqual((rep.errors, rep.pending), ([], []))
        # A different commit or repository is an error.
        self.add("libretro-snes9x", mk=self.core_mk("b" * 40, "https://github.com/someone/snes9x.git"))
        errors = "\n".join(self.lint().errors)
        self.assertIn("is not the source lock's", errors)
        self.assertIn("but the source lock says", errors)

    def test_short_commit_in_lock_is_pending(self):
        self.write("mods/retroarch-cores.src.lock",
                   "snes9x  https://github.com/libretro/snes9x  short:abc1234  s@0\n")
        self.add("libretro-snes9x", mk=self.core_mk("TODO"), hash_text=None)
        rep = self.lint()
        self.assertEqual(rep.errors, [])
        self.assertIn("short:abc1234, not a full commit", "\n".join(rep.pending))

    def test_core_missing_from_lock(self):
        self.write("mods/retroarch-cores.src.lock", "# empty\n")
        self.add("libretro-snes9x", mk=self.core_mk("TODO"), hash_text=None)
        self.assertIn("core snes9x has no line", "\n".join(self.lint().errors))


@unittest.skipUnless(HAVE_ASSETS, "ASSETS.md, nano/Android.bp or tools/nano-tree are not in this tree")
class AssetAllowlist(unittest.TestCase):
    """Decision I38: thimsy-data installs ASSETS.md's KEEP rows only."""

    @classmethod
    def setUpClass(cls):
        cls.list_path = LINUX / "package" / "thimsy-data" / "assets.list"
        cls.entries = aa.read_list(cls.list_path)
        cls.allow = aa.Allowlist(cls.entries)

    def test_committed_list_is_fresh(self):
        code, _, err = quiet(aa.main, ["--check"])
        self.assertEqual(code, 0, err)

    def test_replace_and_ask_assets_are_denied(self):
        # The design's examples: the Rodin fonts, the XMB icons with the PSN logo, the wave
        # captures, the DSi sprites and sounds, the Mirsany font; and Akrobat (changes section).
        for path in ("nano_xmb/fonts/ps3-rodin-regular.ttf", "nano_xmb/fonts/ps3-rodin-bold.ttf",
                     "nano_xmb/icons/xmb_icon_psn.png", "nano_xmb/icons/xmb_icon_000.png",
                     "nano_xmb/icons/xmb_icon_070.png", "nano_xmb/normalmaps/nmap_069.png",
                     "nano_xmb/wave/wave_geo.bin", "nano_xmb/wave/wave_seq2.bin",
                     "nano_xmb/nds/nds_frame.png", "nano_xmb/nds/nds_ring_35.png",
                     "nano_xmb/nds/dialog_box_blue.png", "nano_xmb/audio/nav_blip.wav",
                     "nano_xmb/audio/menu_ambiance.wav", "nano_xmb/audio/SE02_Cursor.wav",
                     "nano_xmb/nds/dsvec.ttf", "nano_xmb/fonts/akrobat-regular.ttf",
                     "nano_xmb/assets/icon_amb.png", "nano_xmb/boot/logo.svg",
                     "nano_esde_themes/slate-es-de.zip",
                     # OA-1: the Colorful files scripts/colorful_theme.sh removes.
                     "nano_esde_themes/colorful-simplified-es-de/_inc/systems/images/quake.webp",
                     "nano_esde_themes/colorful-simplified-es-de/_inc/fonts/Gilroy-Bold.ttf",
                     "nano_esde_themes/colorful-simplified-es-de/_inc/fonts/Akrobat-Bold.ttf"):
            ok, why = self.allow.decide(path)
            self.assertFalse(ok, path)
            self.assertTrue(why.startswith("denied"), (path, why))

    def test_keep_assets_and_data_are_allowed(self):
        for path in ("nano_xmb/fonts/fontawesome-webfont.ttf", "nano_xmb/icons_retroarch/Nintendo_-_SNES.png",
                     "nano_xmb/icons_ui/wifi.png", "nano_xmb/nds/dsvecnum.ttf", "nano_xmb/globe/earth_day.png",
                     "nano_xmb/audio/minima_ok.wav", "nano_xmb/audio/preview.wav",
                     "nano_esde_assets/help/button_a.svg", "nano_xmb/systemstatus/battery.svg",
                     "gammaos-nano/i18n/fr.json", "nano_xmb/controllers/LICENSE",
                     "nano_xmb/controllers/svg/x.svg", "nano_xmb/daijishou/apf.json"):
            ok, why = self.allow.decide(path)
            self.assertTrue(ok, (path, why))

    def test_anything_else_is_left_out(self):
        for path in ("nano_xmb/unknown/x.png", "nano_xmb/icons_retroarch/sub/x.png", "x.png"):
            self.assertFalse(self.allow.decide(path)[0], path)

    def test_every_replace_and_ask_path_is_denied(self):
        rep = lp.Report()
        lp.check_assets(rep, LINUX, REPO, self.list_path)
        self.assertEqual(rep.errors, [])

    def test_install_on_a_fake_export(self):
        tmp = Path(tempfile.mkdtemp(prefix="thimsy-assets-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        src, dest = tmp / "src", tmp / "dest"
        files = ["nano_xmb/fonts/ps3-rodin-regular.ttf", "nano_xmb/fonts/fontawesome-webfont.ttf",
                 "nano_xmb/fonts/akrobat-semibold.ttf", "nano_xmb/icons/xmb_icon_psn.png",
                 "nano_xmb/wave/wave_geo.bin", "gammaos-nano/i18n/de.json", "nano_xmb/nds/nds_tile.png",
                 "nano_xmb/nds/nds_sb_wifi.png", "nano_xmb/unlisted.bin", ".stamp_extracted"]
        for f in files:
            p = src / f
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"x")
        report = tmp / "report.txt"
        code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), "--report", str(report),
                                       str(src), str(dest)])
        self.assertEqual(code, 0, err)
        got = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
        self.assertEqual(got, ["gammaos-nano/i18n/de.json", "nano_xmb/fonts/fontawesome-webfont.ttf",
                               "nano_xmb/nds/nds_sb_wifi.png"])
        text = report.read_text(encoding="utf-8")
        self.assertIn("left out\tnano_xmb/fonts/ps3-rodin-regular.ttf\tdenied", text)
        self.assertIn("left out\tnano_xmb/unlisted.bin\tnot on the list", text)
        self.assertNotIn(".stamp_extracted", text)


    def fake_export(self, tmp: Path, files) -> Path:
        src = tmp / "src"
        for f in files:
            q = src / f
            q.parent.mkdir(parents=True, exist_ok=True)
            q.write_bytes(b"x")
        return src

    def test_install_replaces_akrobat_with_barlow(self):
        """As mods/50-nano.sh on Android: a denied Akrobat file of the export is installed
        under its name from Barlow, with Barlow's OFL text beside it (gfix)."""
        tmp = Path(tempfile.mkdtemp(prefix="thimsy-assets-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        src = self.fake_export(tmp, ["nano_xmb/fonts/akrobat-semibold.ttf",
                                     "nano_xmb/fonts/fontawesome-webfont.ttf"])
        rep = tmp / "barlow"
        rep.mkdir()
        for name in ("BarlowSemiCondensed-Medium.ttf", "BarlowSemiCondensed-SemiBold.ttf", "OFL.txt"):
            (rep / name).write_bytes(name.encode())
        dest, report = tmp / "dest", tmp / "report.txt"
        code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), "--report", str(report),
                                       "--replacements", str(rep), str(src), str(dest)])
        self.assertEqual(code, 0, err)
        got = {q.relative_to(dest).as_posix(): q.read_bytes() for q in dest.rglob("*") if q.is_file()}
        self.assertEqual(got, {"nano_xmb/fonts/fontawesome-webfont.ttf": b"x",
                               "nano_xmb/fonts/akrobat-semibold.ttf": b"BarlowSemiCondensed-SemiBold.ttf",
                               "nano_xmb/fonts/Barlow-OFL.txt": b"OFL.txt"})
        text = report.read_text(encoding="utf-8")
        self.assertIn("left out\tnano_xmb/fonts/akrobat-semibold.ttf\tdenied", text)
        self.assertIn("replaced\tnano_xmb/fonts/akrobat-semibold.ttf\tBarlowSemiCondensed-SemiBold.ttf", text)
        # Nothing to replace: no Barlow at all, not even its licence.
        src2 = self.fake_export(tmp / "b", ["nano_xmb/fonts/fontawesome-webfont.ttf"])
        code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), "--replacements", str(rep),
                                       str(src2), str(tmp / "dest2")])
        self.assertEqual(code, 0, err)
        self.assertFalse((tmp / "dest2/nano_xmb/fonts/Barlow-OFL.txt").exists())
        # A replacement that is missing fails the install.
        (rep / "OFL.txt").unlink()
        code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), "--replacements", str(rep),
                                       str(src), str(tmp / "dest3")])
        self.assertEqual(code, 1)
        self.assertIn("no replacement OFL.txt", err)

    def test_filter_leaves_no_denied_file_and_install_still_replaces(self):
        """security-1: build.sh filters the export before it packs thimsy-data's source tarball,
        which legal-info publishes; the filtered tree has no REPLACE or ASK file, only their
        names, and installing it gives what installing the full export gives."""
        tmp = Path(tempfile.mkdtemp(prefix="thimsy-assets-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        src = self.fake_export(tmp, ["nano_xmb/fonts/akrobat-semibold.ttf", "nano_xmb/fonts/ps3-rodin-regular.ttf",
                                     "nano_xmb/fonts/fontawesome-webfont.ttf", "nano_xmb/icons/xmb_icon_psn.png",
                                     "nano_xmb/controllers/LICENSE", "nano_xmb/unlisted.bin"])
        filtered = tmp / "filtered"
        code, _, err = quiet(aa.main, ["filter", "--list", str(self.list_path), str(src), str(filtered)])
        self.assertEqual(code, 0, err)
        got = sorted(p.relative_to(filtered).as_posix() for p in filtered.rglob("*") if p.is_file())
        self.assertEqual(got, [aa.LEFT_OUT, "nano_xmb/controllers/LICENSE", "nano_xmb/fonts/fontawesome-webfont.ttf"])
        for rel in got:
            if rel != aa.LEFT_OUT:
                self.assertTrue(self.allow.decide(rel)[0], rel)
        names = (filtered / aa.LEFT_OUT).read_text(encoding="utf-8")
        self.assertIn("nano_xmb/fonts/akrobat-semibold.ttf\tdenied", names)
        # A second filter into the same place is refused, so nothing stale is packed.
        code, _, err = quiet(aa.main, ["filter", "--list", str(self.list_path), str(src), str(filtered)])
        self.assertEqual(code, 1)
        rep = tmp / "barlow"
        rep.mkdir()
        for name in ("BarlowSemiCondensed-Medium.ttf", "BarlowSemiCondensed-SemiBold.ttf", "OFL.txt"):
            (rep / name).write_bytes(name.encode())
        want = {}
        for name, tree in (("full", src), ("filtered", filtered)):
            dest = tmp / ("dest-" + name)
            code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), "--replacements", str(rep),
                                           str(tree), str(dest)])
            self.assertEqual(code, 0, err)
            want[name] = {q.relative_to(dest).as_posix(): q.read_bytes() for q in dest.rglob("*") if q.is_file()}
        self.assertEqual(want["filtered"], want["full"])
        self.assertEqual(want["filtered"]["nano_xmb/fonts/akrobat-semibold.ttf"], b"BarlowSemiCondensed-SemiBold.ttf")

    def test_build_sh_packs_the_filtered_export(self):
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        export = build.index("export-assets")
        filt = build.index("assets_allowlist.py\" filter")
        pack = build.index('gzip -9n >"$SRC/nano-assets-$SHA.tar.gz"')
        self.assertLess(export, filt)
        self.assertLess(filt, pack)

    def test_source_tarball_carries_the_licence_texts(self):
        """gfix2: thimsy-data's source tarball, which legal-info saves, carries the repository's
        ATTRIBUTION.md and NOTICE (at HEAD), and LICENSE_FILES names them there."""
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        filt = build.index("assets_allowlist.py\" filter")
        pack = build.index('gzip -9n >"$SRC/nano-assets-$SHA.tar.gz"')
        self.assertIn('for f in ATTRIBUTION.md NOTICE; do', build)
        self.assertLess(build.index('git -C "$REPO" show "HEAD:$f"'), filt)
        call = build[filt:pack]
        for f in ("ATTRIBUTION.md", "NOTICE"):
            self.assertIn('--licence "$assets/licences/%s"' % f, call)
            self.assertTrue((REPO / f).is_file(), f)
        mk = lp.Recipe(LINUX / "package/thimsy-data/thimsy-data.mk").vars
        files = mk["THIMSY_DATA_LICENSE_FILES"].split()
        for f in ("ATTRIBUTION.md", "NOTICE"):
            self.assertIn(aa.LICENCES + "/" + f, files)

        tmp = Path(tempfile.mkdtemp(prefix="thimsy-assets-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        theme = [f for f in files if f.startswith("nano_esde_themes/")]   # the prepared theme's (OA-1)
        self.assertTrue(theme)
        src = self.fake_export(tmp, ["nano_xmb/controllers/LICENSE",
                                     "nano_xmb/fonts/fontawesome-webfont.ttf"] + theme)
        lic = tmp / "licences"
        lic.mkdir()
        for f in ("ATTRIBUTION.md", "NOTICE"):
            (lic / f).write_bytes((REPO / f).read_bytes())
        filtered = tmp / "filtered"
        code, _, err = quiet(aa.main, ["filter", "--list", str(self.list_path),
                                       "--licence", str(lic / "ATTRIBUTION.md"),
                                       "--licence", str(lic / "NOTICE"), str(src), str(filtered)])
        self.assertEqual(code, 0, err)
        for f in ("ATTRIBUTION.md", "NOTICE"):
            self.assertEqual((filtered / aa.LICENCES / f).read_bytes(), (REPO / f).read_bytes())
        # Buildroot's legal-info copies each LICENSE_FILES entry from the extracted source.
        for f in files:
            if not f.startswith(".thimsy-barlow/"):   # copied in at post-extract
                self.assertTrue((filtered / f).is_file(), f)
        # The install leaves them out of the image.
        dest = tmp / "dest"
        code, _, err = quiet(aa.main, ["install", "--list", str(self.list_path), str(filtered), str(dest)])
        self.assertEqual(code, 0, err)
        self.assertFalse(any(aa.LICENCES in p.as_posix() for p in dest.rglob("*")))
        # A missing licence file fails the filter.
        code, _, err = quiet(aa.main, ["filter", "--list", str(self.list_path), "--licence",
                                       str(lic / "MISSING"), str(src), str(tmp / "f2")])
        self.assertEqual(code, 1)
        self.assertIn("no licence file", err)

    def test_replacements_follow_android(self):
        """thimsy-data's Barlow is mods/50-nano.sh's: the same file for each Akrobat path, the
        same commit and the same hashes (scripts/colorful_theme.sh's pins, which 50-nano.sh
        sources); build.sh writes them into thimsy-data's hash file."""
        mod = (REPO / "mods/50-nano.sh").read_text(encoding="utf-8")
        pins = (REPO / "scripts/colorful_theme.sh").read_text(encoding="utf-8")
        self.assertIn('source "$MODS/../scripts/colorful_theme.sh"', mod)
        pairs = re.search(r'for pair in ((?:"[^"]+" ?)+); do', mod).group(1)
        android = {}
        for pair in re.findall(r'"([^"]+)"', pairs):
            old, new = pair.split()
            android["nano_xmb/fonts/" + old] = new
        self.assertEqual(dict(aa.REPLACEMENTS), android)
        self.assertIn('"$xmb_fonts/Barlow-OFL.txt"', mod)
        self.assertEqual(dict(aa.REPLACEMENT_EXTRAS), {"nano_xmb/fonts/Barlow-OFL.txt": "OFL.txt"})
        commit = re.search(r'BARLOW_URL="https://raw\.githubusercontent\.com/jpt/barlow/([0-9a-f]{40})"', pins).group(1)
        android_sha = {os.path.basename(m.group(1)): m.group(2)
                       for m in re.finditer(r'^  "(\S+) ([0-9a-f]{64})"$', pins, re.M)}
        mk = lp.Recipe(LINUX / "package/thimsy-data/thimsy-data.mk").vars
        self.assertEqual(mk["THIMSY_DATA_BARLOW_COMMIT"], commit)
        downloads = [os.path.basename(u) for u in mk["THIMSY_DATA_EXTRA_DOWNLOADS"].split()]
        wanted = sorted({f for _, f in aa.REPLACEMENTS + aa.REPLACEMENT_EXTRAS})
        self.assertEqual(sorted(downloads), wanted)
        hashes = {}
        for line in (LINUX / "package/thimsy-data/extra-downloads.hash").read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                algo, h, name = line.split()
                hashes[name] = h
        for name in wanted:
            self.assertEqual(hashes[name], android_sha[name], name)
        self.assertIn(".thimsy-barlow/OFL.txt", mk["THIMSY_DATA_LICENSE_FILES"].split())
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        self.assertIn('extra-downloads.hash" >>"$SRC/hash/$1/$1.hash"', build)


class AllowlistParsing(unittest.TestCase):
    """assets_allowlist.py's reading of ASSETS.md, on a small table."""

    TABLE = textwrap.dedent("""\
        # Assets

        ## Changes since the audit

        - **Foo is not OFL.** Foo counts as REPLACE.

        ## 1. Theme

        | path | what | verdict |
        |---|---|---|
        | `ps3xmb/fonts/rodin-regular.ttf`, `-bold.ttf` | font | REPLACE |
        | `ps3xmb/fonts/foo-regular.ttf`, `bar.ttf` | fonts | KEEP (verify) |
        | `ps3xmb/icons/i_00..02.png`, `logo.img` | icons | ASK (upstream) |
        | `nano/x/{a,b}/*.svg` | glyphs | KEEP |
        | `nano/t/theme.zip`: `core/fonts/*.otf` | fonts in a zip | KEEP |
        | `theme.zip`: `*/logo.svg` | logos in a zip | ASK |
        | `colorful/_inc/q.webp` | art | REPLACE |
        | `/system/etc/nano_xmb/audio/boot.mp3` | sound | KEEP |
        | `/system/etc/res/charger.png` | not Nano's | KEEP |
        | `ps3xmb/old/x.png` | gone | not installed |
        | `ps3xmb/fonts`, `icons` | overrides | n/a |
        """)

    def test_generate(self):
        mapper = aa.Mapper({"bootscreen/l.svg": "nano_xmb/boot/logo.svg"},
                           {"ps3xmb/fonts": "nano_xmb/fonts", "ps3xmb/i18n": "gammaos-nano/i18n"})
        entries, unmapped = aa.generate(self.TABLE, mapper)
        got = {(e.action, e.pattern) for e in entries}
        for want in (("deny", "nano_xmb/fonts/rodin-regular.ttf"), ("deny", "nano_xmb/fonts/rodin-bold.ttf"),
                     ("keep", "nano_xmb/fonts/foo-regular.ttf"), ("keep", "nano_xmb/fonts/bar.ttf"),
                     ("deny", "nano_xmb/fonts/foo-regular.ttf"),
                     ("deny", "nano_xmb/icons/i_00.png"), ("deny", "nano_xmb/icons/i_02.png"),
                     ("keep", "x/a/*.svg"), ("keep", "x/b/*.svg"), ("deny", "t/theme.zip"),
                     ("deny", "nano_esde_themes/colorful-simplified-es-de/_inc/q.webp"),
                     ("keep", "nano_xmb/audio/boot.mp3"), ("keep", "gammaos-nano/i18n/*.json")):
            self.assertIn(want, got)
        self.assertNotIn(("keep", "t/theme.zip"), got)
        self.assertFalse(any("logo.img" in p for _, p in got))
        self.assertFalse(any("old/x.png" in p for _, p in got))
        self.assertIn("/system/etc/res/charger.png (ASSETS.md:19 KEEP)", unmapped)
        al = aa.Allowlist(entries)
        self.assertFalse(al.decide("nano_xmb/fonts/foo-regular.ttf")[0])
        self.assertTrue(al.decide("nano_xmb/fonts/bar.ttf")[0])

    def test_expand(self):
        self.assertEqual(aa.expand("a_{x,y}_0..2.png"),
                         ["a_x_0.png", "a_x_1.png", "a_x_2.png", "a_y_0.png", "a_y_1.png", "a_y_2.png"])
        self.assertEqual(aa.expand("i_008..010"), ["i_008", "i_009", "i_010"])

    def test_install_refuses_symlinks(self):
        tmp = Path(tempfile.mkdtemp(prefix="thimsy-assets-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        (tmp / "src").mkdir()
        (tmp / "list").write_text("keep\t*\tx\n", encoding="utf-8")
        try:
            os.symlink(str(tmp / "list"), str(tmp / "src" / "link"))
        except (OSError, NotImplementedError):
            self.skipTest("no symlinks here")
        code, _, err = quiet(aa.main, ["install", "--list", str(tmp / "list"), str(tmp / "src"), str(tmp / "d")])
        self.assertEqual(code, 1)
        self.assertIn("refusing the symlink", err)


class GpuPack(unittest.TestCase):
    """thimsy-gpu-pack's gpu_pack.py on a fake tier root."""

    @classmethod
    def setUpClass(cls):
        cls.gp = load(LINUX / "package" / "thimsy-gpu-pack" / "src" / "gpu_pack.py", "gpu_pack_for_test")

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-gpu-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.target = self.dir / "target"
        for f in ("usr/lib/dri/panfrost_dri.so", "usr/lib/libgallium-25.1.so", "usr/lib/libEGL.so.1.0.0",
                  "usr/lib/libGLESv2.so.2.0.0", "usr/lib/libgbm.so.1.0.0", "usr/lib/gbm/dri_gbm.so",
                  "usr/share/drirc.d/00-mesa-defaults.conf", "usr/lib/libdrm.so.2", "usr/bin/thimsy-nano",
                  "usr/lib/libEGLfoo/keep.txt"):
            p = self.target / f
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"x")
        self.links = False
        try:
            os.symlink("libEGL.so.1.0.0", str(self.target / "usr/lib/libEGL.so.1"))
            self.links = True
        except (OSError, NotImplementedError):
            pass

    def run_pack(self, *args):
        return quiet(self.gp.main, ["--target", str(self.target), "--work", str(self.dir / "work")] + list(args))

    def test_moves_mesa_into_each_pack(self):
        code, out, err = self.run_pack("--driver", "panfrost", "--driver", "swrast")
        self.assertEqual(code, 0, err)
        for d in ("panfrost", "swrast"):
            tree = self.dir / "work" / ("gpu-" + d)
            m = json.loads((tree / "usr/share/thimsy/gpu.json").read_text(encoding="utf-8"))
            self.assertEqual(m["driver"], d)
            self.assertIn("usr/lib/libgallium-25.1.so", m["files"])
            self.assertTrue((tree / "usr/lib/gbm/dri_gbm.so").is_file())
            if self.links:
                self.assertTrue(os.path.islink(str(tree / "usr/lib/libEGL.so.1")))
        swrast = json.loads((self.dir / "work/gpu-swrast/usr/share/thimsy/gpu.json").read_text(encoding="utf-8"))
        self.assertEqual(swrast["env"], {"GALLIUM_DRIVER": "softpipe"})
        # Gone from the root, with the folders that only held them; the rest stays.
        self.assertFalse((self.target / "usr/lib/dri").exists())
        self.assertFalse((self.target / "usr/lib/libgbm.so.1.0.0").exists())
        self.assertFalse((self.target / "usr/share/drirc.d").exists())
        self.assertTrue((self.target / "usr/lib/libdrm.so.2").is_file())
        self.assertTrue((self.target / "usr/bin/thimsy-nano").is_file())
        self.assertTrue((self.target / "usr/lib/libEGLfoo/keep.txt").is_file())
        # A rebuild that finds the files gone reuses the packs.
        code, out, err = self.run_pack("--driver", "panfrost")
        self.assertEqual(code, 0, err)
        self.assertIn("reusing", out)

    def test_keep_and_errors(self):
        code, _, err = self.run_pack("--driver", "panfrost", "--keep")
        self.assertEqual(code, 0, err)
        self.assertTrue((self.target / "usr/lib/dri/panfrost_dri.so").is_file())
        self.assertEqual(self.run_pack()[0], 2)
        self.assertEqual(self.run_pack("--driver", "Bad/Name")[0], 2)
        self.assertEqual(self.run_pack("--driver", "x", "--mkfs", "mkfs.erofs")[0], 2)
        shutil.rmtree(str(self.target / "usr/lib"))
        shutil.rmtree(str(self.target / "usr/share"))
        code, _, err = self.run_pack("--driver", "other")
        self.assertEqual(code, 1)
        self.assertIn("no Mesa files", err)

    def test_mkfs_is_called_per_driver(self):
        # subprocess.run stands in for mkfs.erofs and records its arguments.
        calls = []

        def fake_run(cmd, check):
            calls.append(cmd)
            Path(cmd[-2]).write_text("img", encoding="utf-8")
        orig = self.gp.subprocess.run
        self.gp.subprocess.run = fake_run
        try:
            code, _, err = self.run_pack("--driver", "panfrost", "--driver", "swrast", "--out",
                                         str(self.dir / "images"), "--mkfs", "mkfs.erofs", "--epoch", "1700000000")
        finally:
            self.gp.subprocess.run = orig
        self.assertEqual(code, 0, err)
        self.assertEqual(len(calls), 2)
        self.assertIn("-T1700000000", calls[0])
        self.assertIn("--all-root", calls[0])
        self.assertTrue((self.dir / "images/gpu-swrast.erofs").is_file())


    def test_pack_is_unpacked_into_each_card_not_mounted(self):
        """Config.in, gpu_pack.py and card assembly agree: 10-erofs.sh unpacks the family's
        gpu-<driver>.erofs into each card's root; nothing mounts it at boot (gfix)."""
        texts = {p: (LINUX / p).read_text(encoding="utf-8") for p in (
            "package/thimsy-gpu-pack/Config.in", "package/thimsy-gpu-pack/src/gpu_pack.py")}
        for p, text in texts.items():
            flat = " ".join(text.split())
            self.assertNotIn("mounted over", flat, p)
            self.assertIn("10-erofs.sh", flat, p)
        erofs = (LINUX / "board/common/post-image.d/10-erofs.sh").read_text(encoding="utf-8")
        self.assertRegex(erofs, r'fsck\.erofs --extract="\$stage" --overwrite "\$pack"')
        initd = LINUX / "board/common/rootfs-overlay/etc/init.d"
        for f in sorted(initd.rglob("*")):
            if f.is_file():
                self.assertNotIn("gpu-", f.read_text(encoding="utf-8", errors="replace"), f)


def elf64(needed):
    """A minimal little-endian ELF64 shared object whose dynamic section lists NEEDED."""
    strtab = b"\0"
    offs = []
    for n in needed:
        offs.append(len(strtab))
        strtab += n.encode() + b"\0"
    str_off = 64 + 2 * 56
    dyn_off = (str_off + len(strtab) + 7) & ~7
    dyn = b"".join(struct.pack("<qQ", 1, o) for o in offs) + struct.pack("<qQ", 5, str_off) \
        + struct.pack("<qQ", 0, 0)
    total = dyn_off + len(dyn)
    ehdr = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + b"\0" * 9, 3, 183, 1, 0, 64, 0, 0,
                       64, 56, 2, 0, 0, 0)
    load = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, total, total, 0x1000)
    dynamic = struct.pack("<IIQQQQQQ", 2, 6, dyn_off, dyn_off, dyn_off, len(dyn), len(dyn), 8)
    body = ehdr + load + dynamic + strtab
    return body + b"\0" * (dyn_off - len(body)) + dyn


SHELLS = check_rcs.find_shells()
SH = SHELLS[0] if SHELLS else None

# A tier root as Buildroot leaves it, by package (packages-file-list.txt's "pkg,./path").
BT_TREE = {
    "dbus": {"usr/bin/dbus-daemon": None, "usr/bin/dbus-uuidgen": None, "usr/bin/dbus-send": None,
             "usr/lib/libdbus-1.so.3": None, "etc/dbus-1/system.conf": "conf",
             "usr/share/dbus-1/system.conf": "conf", "usr/libexec/dbus-daemon-launch-helper": None,
             "etc/init.d/S30dbus": "sh"},
    "bluez5_utils": {"usr/libexec/bluetooth/bluetoothd": None, "usr/bin/bluetoothctl": None,
                     "usr/lib/libbluetooth.so.3": None, "etc/dbus-1/system.d/bluetooth.conf": "conf",
                     "etc/bluetooth/main.conf": "conf", "etc/init.d/S40bluetoothd": "sh"},
    "libglib2": {"usr/lib/libglib-2.0.so.0": None},
    "thimsy-nano": {"usr/bin/thimsy-nano": None},
}
NEEDS = {"usr/bin/dbus-daemon": ["libdbus-1.so.3", "libc.so.6"],
         "usr/libexec/bluetooth/bluetoothd": ["libdbus-1.so.3", "libglib-2.0.so.0"],
         "usr/bin/bluetoothctl": ["libglib-2.0.so.0", "libreadline.so.8"],
         "usr/bin/thimsy-nano": ["libdrm.so.2", "libc.so.6"],
         "usr/libexec/dbus-daemon-launch-helper": ["libdbus-1.so.3", "libexpat.so.1"]}
# Listed but not packed: the helper Buildroot's permissions table names stays in the root (makedevs
# fails without it); the recipes' BusyBox boot scripts leave the root and the pack.
BT_KEPT = ("usr/libexec/dbus-daemon-launch-helper",)
BT_BOOT = ("etc/init.d/S30dbus", "etc/init.d/S40bluetoothd")


def bt_packed(pkg):
    return [r for r in BT_TREE[pkg] if r not in BT_KEPT and r not in BT_BOOT]


OTHER = ("etc/init.d/deferred/D35bluetooth", "etc/init.d/deferred/D30net", "var/lib/bluetooth")


class BtPack(unittest.TestCase):
    """tools/bt_pack.py on a fake tier root (decision OA-15)."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="thimsy-bt-"))
        self.addCleanup(shutil.rmtree, str(self.dir), True)
        self.target = self.dir / "target"
        self.work = self.dir / "work"
        lines = []
        for pkg, files in BT_TREE.items():
            for rel, text in files.items():
                self.put(rel, text)
                lines.append("%s,./%s" % (pkg, rel))
        for rel in OTHER:
            self.put(rel, "sh\n")
        self.list = self.dir / "packages-file-list.txt"
        self.list.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def put(self, rel, text=None):
        p = self.target / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if text is None:
            p.write_bytes(elf64(NEEDS.get(rel, ["libc.so.6"])))
        else:
            p.write_text(text, encoding="utf-8")

    def run_pack(self, *args):
        return quiet(bt_pack.main, ["--target", str(self.target), "--work", str(self.work),
                                    "--file-list", str(self.list)] + list(args))

    def test_elf_needed(self):
        self.assertEqual(bt_pack.elf_needed(str(self.target / "usr/libexec/bluetooth/bluetoothd")),
                         ["libdbus-1.so.3", "libglib-2.0.so.0"])
        self.assertEqual(bt_pack.elf_needed(str(self.target / "etc/bluetooth/main.conf")), [])

    def test_moves_bluez_dbus_and_d35(self):
        code, out, err = self.run_pack()
        self.assertEqual(code, 0, err)
        tree = self.work / "bluetooth"
        m = json.loads((tree / "usr/share/thimsy/bluetooth.json").read_text(encoding="utf-8"))
        self.assertEqual(m["packages"], ["dbus", "bluez5_utils"])
        moved = sorted(bt_packed("dbus") + bt_packed("bluez5_utils")
                       + ["etc/init.d/deferred/D35bluetooth", "var/lib/bluetooth"])
        self.assertEqual(m["files"], moved)
        for rel in moved:
            self.assertTrue((tree / rel).is_file(), rel)
            self.assertFalse(os.path.lexists(str(self.target / rel)), rel)
        # The folders that only held them go; the shared ones and the rest stay.
        for d in ("usr/libexec/bluetooth", "etc/dbus-1", "etc/bluetooth", "usr/share/dbus-1"):
            self.assertFalse((self.target / d).exists(), d)
        for d in ("usr/libexec", "usr/share", "var/lib", "etc/init.d/deferred"):
            self.assertTrue((self.target / d).is_dir(), d)
        for rel in ("usr/lib/libglib-2.0.so.0", "usr/bin/thimsy-nano", "etc/init.d/deferred/D30net"):
            self.assertTrue((self.target / rel).is_file(), rel)
        self.assertEqual(m["left_in_root"], list(BT_KEPT))
        self.assertEqual(m["boot_scripts_dropped"], list(BT_BOOT))

    def test_launch_helper_stays_for_makedevs(self):
        """dbus.mk's permissions line ("f" type) names the helper; makedevs fails if it is gone.
        It needs libdbus, which leaves: only dbus-daemon runs it, so that is no error."""
        code, out, err = self.run_pack()
        self.assertEqual(code, 0, err)
        rel = "usr/libexec/dbus-daemon-launch-helper"
        self.assertTrue((self.target / rel).is_file())
        self.assertFalse((self.work / "bluetooth" / rel).exists())
        self.assertIn("/%s stays in the root" % rel, out)

    def test_boot_scripts_leave_root_and_pack(self):
        """rcS runs every S??*: the recipes' scripts would start D-Bus and bluetoothd at every
        boot; D35bluetooth starts them only when Bluetooth is on."""
        code, out, err = self.run_pack()
        self.assertEqual(code, 0, err)
        for rel in BT_BOOT:
            self.assertFalse((self.target / rel).exists(), rel)
            self.assertFalse((self.work / "bluetooth" / rel).exists(), rel)
            self.assertIn("/%s removed" % rel, out)
        self.assertTrue((self.target / "etc/init.d").is_dir())
        # Only S??* directly in etc/init.d: the deferred scripts are not boot scripts.
        self.assertTrue(bt_pack.BOOT_SCRIPT.match("etc/init.d/S30dbus"))
        self.assertFalse(bt_pack.BOOT_SCRIPT.match("etc/init.d/deferred/D35bluetooth"))
        self.assertFalse(bt_pack.BOOT_SCRIPT.match("etc/init.d/rcS"))

    @unittest.skipIf(os.name == "nt", "directory modes are POSIX")
    def test_pack_directories_are_0755(self):
        old = os.umask(0o077)
        try:
            code, _, err = self.run_pack()
        finally:
            os.umask(old)
        self.assertEqual(code, 0, err)
        tree = self.work / "bluetooth"
        for d in [tree] + [p for p in tree.rglob("*") if p.is_dir() and not p.is_symlink()]:
            self.assertEqual(stat.S_IMODE(d.stat().st_mode), 0o755, str(d))

    def test_rebuild_takes_moved_files_from_the_earlier_pack(self):
        self.assertEqual(self.run_pack()[0], 0)
        # Without per-package directories only the rebuilt package installs again.
        for rel, text in BT_TREE["bluez5_utils"].items():
            self.put(rel, text)
        self.put("etc/init.d/deferred/D35bluetooth", "sh\n")
        code, _, err = self.run_pack()
        self.assertEqual(code, 0, err)
        for rel in bt_packed("dbus") + bt_packed("bluez5_utils"):
            self.assertTrue((self.work / "bluetooth" / rel).is_file(), rel)
            self.assertFalse((self.target / rel).exists(), rel)
        for rel in BT_BOOT:
            self.assertFalse((self.target / rel).exists(), rel)
        self.assertTrue((self.target / BT_KEPT[0]).is_file())

    def test_a_missing_program_fails(self):
        (self.target / "usr/bin/bluetoothctl").unlink()
        code, _, err = self.run_pack()
        self.assertEqual(code, 1)
        self.assertIn("would lack bluetoothctl", err)
        self.assertTrue((self.target / "usr/bin/dbus-daemon").is_file())  # nothing moved

    def test_a_program_left_needing_a_moved_library_fails(self):
        p = self.target / "usr/bin/retroarch"
        p.write_bytes(elf64(["libdbus-1.so.3", "libc.so.6"]))
        code, _, err = self.run_pack()
        self.assertEqual(code, 1)
        self.assertIn("/usr/bin/retroarch needs libdbus-1.so.3, which only the Bluetooth pack has", err)
        self.assertTrue((self.target / "usr/lib/libdbus-1.so.3").is_file())
        # Another copy of the library left in the root is enough.
        self.put("usr/lib/other/libdbus-1.so.3")
        self.assertEqual(self.run_pack()[0], 0)

    def test_none_removes_d35_and_an_old_pack(self):
        out = self.dir / "images"
        out.mkdir()
        (out / "bluetooth.erofs").write_bytes(b"old")
        code, _, err = quiet(bt_pack.main, ["--target", str(self.target), "--none", "--out", str(out)])
        self.assertEqual(code, 0, err)
        self.assertFalse((self.target / "etc/init.d/deferred/D35bluetooth").exists())
        self.assertFalse(os.path.lexists(str(self.target / "var/lib/bluetooth")))
        self.assertTrue((self.target / "etc/init.d/deferred/D30net").is_file())
        self.assertFalse((out / "bluetooth.erofs").exists())

    def test_usage(self):
        self.assertEqual(quiet(bt_pack.main, ["--target", str(self.target)])[0], 2)
        self.assertEqual(self.run_pack("--mkfs", "mkfs.erofs")[0], 2)
        self.assertEqual(quiet(bt_pack.main, ["--target", str(self.dir / "nope"), "--none"])[0], 1)

    def test_mkfs_is_called_reproducibly(self):
        calls = []

        def fake_run(cmd, check):
            calls.append(cmd)
            Path(cmd[-2]).write_text("img", encoding="utf-8")
        orig = bt_pack.subprocess.run
        bt_pack.subprocess.run = fake_run
        try:
            code, _, err = self.run_pack("--out", str(self.dir / "images"), "--mkfs", "mkfs.erofs",
                                         "--epoch", "1700000000")
        finally:
            bt_pack.subprocess.run = orig
        self.assertEqual(code, 0, err)
        self.assertEqual(len(calls), 1)
        self.assertIn("-T1700000000", calls[0])
        self.assertIn("--all-root", calls[0])
        self.assertTrue((self.dir / "images/bluetooth.erofs").is_file())

    @unittest.skipIf(SH is None, "no POSIX shell found")
    def test_post_build_part(self):
        """post-build.d/40-bt-pack.sh: in a tier tree with BlueZ, the pack; without, --none; in
        a hardware pack's tree, nothing."""
        script = (LINUX / "board/common/post-build.d/40-bt-pack.sh").as_posix()
        build = self.dir / "build"
        build.mkdir()
        shutil.copy(str(self.list), str(build / "packages-file-list.txt"))
        fakebin = self.dir / "fakebin"
        fakebin.mkdir()
        mkfs = fakebin / "mkfs.erofs"
        with open(str(mkfs), "w", encoding="utf-8", newline="\n") as f:
            f.write('#!/bin/sh\nfor a; do out=$prev; prev=$a; done\necho fake > "$out"\n')
        os.chmod(str(mkfs), 0o755)
        fake_mkfs = {}
        if os.name == "nt":
            # bt_pack.py runs mkfs.erofs itself, and Windows runs no sh script: a batch file.
            writer = self.dir / "fake_mkfs.py"
            writer.write_text("import sys\nopen(sys.argv[-2], 'w').write('fake')\n", encoding="utf-8")
            bat = fakebin / "mkfs-erofs.bat"
            bat.write_text('@"%s" "%s" %%*\r\n' % (sys.executable, writer), encoding="utf-8")
            fake_mkfs = {"THIMSY_MKFS_EROFS": str(bat)}
        config = self.dir / ".config"
        env = dict(os.environ, BR2_CONFIG=config.as_posix(), BUILD_DIR=build.as_posix(),
                   BINARIES_DIR=(self.dir / "images").as_posix(), THIMSY_PYTHON=Path(sys.executable).as_posix(),
                   BR2_EXTERNAL_THIMSY_PATH=LINUX.as_posix(),
                   PATH=str(fakebin) + os.pathsep + os.environ.get("PATH", ""), **fake_mkfs)
        env.pop("SOURCE_DATE_EPOCH", None)

        def run(*args):
            return subprocess.run(SH + [script, self.target.as_posix()] + list(args), env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  universal_newlines=True, timeout=120)
        config.write_text("BR2_PACKAGE_BLUEZ5_UTILS=y\n", encoding="utf-8")
        r = run("hw", "rk3566", "--family=rk3566")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue((self.target / "usr/bin/dbus-daemon").is_file())
        r = run("tier", "v8_0")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("wrote", r.stdout)
        self.assertTrue((self.dir / "images/bluetooth.erofs").is_file())
        self.assertFalse((self.target / "usr/bin/dbus-daemon").exists())
        self.assertFalse((self.target / "etc/init.d/deferred/D35bluetooth").exists())
        self.put("etc/init.d/deferred/D35bluetooth", "sh\n")
        config.write_text("# BR2_PACKAGE_BLUEZ5_UTILS is not set\n", encoding="utf-8")
        r = run("tier", "v8_2")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertFalse((self.target / "etc/init.d/deferred/D35bluetooth").exists())
        self.assertFalse((self.dir / "images/bluetooth.erofs").exists())


class Fragments(unittest.TestCase):
    """The userland and hardware-pack additions the init scripts need (G2e's open items)."""

    def read(self, name):
        return (LINUX / "configs" / "fragments" / (name + ".fragment")).read_text(encoding="utf-8")

    def test_userland_tools(self):
        text = self.read("userland")
        for sym in ("BR2_PACKAGE_UTIL_LINUX_BINARIES=y", "BR2_PACKAGE_EXFATPROGS=y", "BR2_PACKAGE_E2FSPROGS=y",
                    'BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="$(BR2_EXTERNAL_THIMSY_PATH)/board/common/'
                    'busybox.fragment"'):
            self.assertIn(sym, text)
        bb = (LINUX / "board" / "common" / "busybox.fragment").read_text(encoding="utf-8")
        for opt in ("CONFIG_IP=y", "CONFIG_UDHCPC=y", "CONFIG_NTPD=y", "CONFIG_FEATURE_FLOAT_SLEEP=y",
                    "CONFIG_DD=y", "CONFIG_OD=y", "CONFIG_DESKTOP=y", "CONFIG_CHOWN=y"):
            self.assertIn(opt, bb)

    def test_rk3566_overlay_and_family(self):
        text = (LINUX / "configs" / "thimsy_rk3566_hw_defconfig").read_text(encoding="utf-8")
        self.assertIn("board/rk3566/common/rootfs-overlay", text)
        self.assertIn('BR2_ROOTFS_POST_SCRIPT_ARGS="hw rk3566 --family=rk3566"', text)
        self.assertTrue((LINUX / "board" / "rk3566" / "common" / "rootfs-overlay").is_dir())
        qemu = (LINUX / "configs" / "thimsy_qemu_aarch64_hw_defconfig").read_text(encoding="utf-8")
        self.assertIn("--family=qemu-aarch64-virt", qemu)


if __name__ == "__main__":
    unittest.main()
