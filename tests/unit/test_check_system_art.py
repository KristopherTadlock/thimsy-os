"""check_system_art.py: every shipped game system has its platform art in every shipped ES-DE theme."""
import json
import os
import re
import zipfile

from tests import util

csa = util.load_script("check_system_art")
SLATE = os.path.join(util.AOSP, "frameworks/base/cmds/gammaos-nano/nano_esde_themes/slate-es-de.zip")


def colorful_zip():
    """The Colorful zip 50-nano.sh bundles, in the build cache."""
    mod = util.slurp(f"{util.MODS}/50-nano.sh", text=True)
    theme = re.search(r'^THEME="([^"]+)"', mod, re.M).group(1)
    commit = re.search(r'^THEME_COMMIT="([^"]+)"', mod, re.M).group(1)
    return os.path.join(util.TEST_CACHE or os.path.join(util.ROOT, "work/cache"), "nano", f"{theme}-{commit}.zip")


def shipped_systems():
    return [rom for _, rom, _ in util.load_script("nano_systems").parse(f"{util.MODS}/nano-systems.txt")]


class Missing(util.TestCase):
    THEMES = {
        "slate-es-de": {"capabilities.xml", "nes/images/logo.svg", "colecovision/images/logo.svg"},
        "colorful-simplified-es-de": {"_inc/systems/images/nes.webp", "_inc/systems/images/colecovision.webp"},
    }

    def test_complete(self):
        self.assertEqual([], csa.missing(["nes", "colecovision"], self.THEMES))

    def test_a_catalog_id_has_no_art(self):
        self.assertEqual(["colorful-simplified-es-de: no art for coleco (_inc/systems/images/coleco.webp)",
                          "slate-es-de: no art for coleco (coleco/images/logo.svg)"],
                         csa.missing(["nes", "coleco"], self.THEMES))

    def test_unknown_theme(self):
        problems = csa.missing(["nes"], {"art-book-next-es-de": {"capabilities.xml"}})
        self.assertEqual(1, len(problems))
        self.assertIn("art-book-next-es-de: no art layout known", problems[0])


class Script(util.TestCase):
    def themes(self, colecovision=True):
        for system in ("nes", "colecovision") if colecovision else ("nes",):
            self.write(f"themes/colorful-simplified-es-de/_inc/systems/images/{system}.webp", "art")
        with zipfile.ZipFile(self.path("themes/slate-es-de.zip"), "w") as z:
            z.writestr("capabilities.xml", "<themeCapabilities/>")
            z.writestr("nes/images/logo.svg", "<svg/>")
            z.writestr("colecovision/images/logo.svg", "<svg/>")
        return self.path("themes")

    def systems(self, *roms):
        return self.write("nano_systems.json", json.dumps({"version": 1, "systems": [{"romDir": r} for r in roms]}))

    def test_ok(self):
        proc = util.run_script("check_system_art", self.systems("nes", "colecovision"), self.themes())
        self.assertOk(proc)
        self.assertIn("2 game systems have art", proc.stdout)

    def test_missing_art_fails(self):
        self.assertFails(util.run_script("check_system_art", self.systems("nes", "colecovision"),
                                         self.themes(colecovision=False)),
                         "colorful-simplified-es-de: no art for colecovision")

    def test_zip_under_a_folder(self):
        with zipfile.ZipFile(self.path("t.zip"), "w") as z:
            z.writestr("slate-es-de-main/capabilities.xml", "")
            z.writestr("slate-es-de-main/nes/images/logo.svg", "")
        self.assertEqual({"capabilities.xml", "nes/images/logo.svg"}, csa.theme_files(self.path("t.zip")))


class ShippedThemes(util.TestCase):
    """The real list against the real themes: Slate from the Nano tree, Colorful from the build cache."""

    def test_slate(self):
        if not os.path.isfile(SLATE):
            self.skipTest(f"{SLATE} isn't on this host")
        self.assertEqual([], csa.missing(shipped_systems(), {"slate-es-de": csa.theme_files(SLATE)}))

    def test_colorful(self):
        zip_path = colorful_zip()
        if not os.path.isfile(zip_path):
            self.skipTest(f"{zip_path} isn't on this host")
        files = csa.theme_files(zip_path) - {"_inc/systems/images/quake.webp"}   # 50-nano.sh removes it
        self.assertEqual([], csa.missing(shipped_systems(), {"colorful-simplified-es-de": files}))

    def test_folders_are_esde_names(self):
        """The check looks art up by ROM folder, which is what Nano draws for an ES-DE system name."""
        for rom in shipped_systems():
            self.assertRegex(rom, re.compile(r"^[a-z0-9-]+$"), rom)
