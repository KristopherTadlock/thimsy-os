"""Tests of the package recipes, linux/tools/lint_packages.py, linux/tools/assets_allowlist.py
and thimsy-gpu-pack's gpu_pack.py (IMPLEMENTATION.md D.4, test_packages; step G2d).

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
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

LINUX = Path(__file__).resolve().parent.parent
REPO = LINUX.parent
sys.path.insert(0, str(LINUX / "tools"))

import assets_allowlist as aa  # noqa: E402
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
            "THIMSY_BAR_SITE = $(BR2_EXTERNAL_THIMSY_PATH)/src/x\nTHIMSY_BAR_SITE_METHOD = local")
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
                     "nano_esde_themes/colorful-simplified-es-de/_inc/systems/images/ps2.webp"):
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
        for opt in ("CONFIG_IP=y", "CONFIG_UDHCPC=y", "CONFIG_NTPD=y", "CONFIG_FEATURE_FLOAT_SLEEP=y"):
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
