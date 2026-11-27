"""check_system_art.py: every shipped game system has its platform art in every shipped ES-DE theme, and
every catalog platform its own picture in Colorful."""
import json
import os
import re
import zipfile

from tests import util

csa = util.load_script("check_system_art")
NANO = os.path.join(util.AOSP, "frameworks/base/cmds/gammaos-nano")
SLATE = os.path.join(NANO, "nano_esde_themes/slate-es-de.zip")
CATALOG = os.path.join(NANO, "ps3xmb/daijishou")
PICTURES = os.path.join(util.MODS, "console-pictures")


def colorful_zip():
    """The Colorful zip 50-nano.sh bundles (scripts/colorful_theme.sh's pins), in the build cache."""
    mod = util.slurp(f"{util.SCRIPTS}/colorful_theme.sh", text=True)
    theme = re.search(r'^THEME="([^"]+)"', mod, re.M).group(1)
    commit = re.search(r'^THEME_COMMIT="([^"]+)"', mod, re.M).group(1)
    return os.path.join(util.TEST_CACHE or os.path.join(util.ROOT, "work/cache"), "nano", f"{theme}-{commit}.zip")


def shipped_colorful(zip_path):
    """Colorful's files as 50-nano.sh ships them: without quake.webp, with this project's pictures."""
    files = csa.theme_files(zip_path) - {"_inc/systems/images/quake.webp"}
    for kind in ("images", "metadata-global", "metadata-custom"):
        files |= {f"_inc/systems/{kind}/{f}" for f in os.listdir(os.path.join(PICTURES, kind))}
    return files


def cpp_table(src, name):
    body = re.search(name + r"\[\] = \{(.*?)\};", src, re.S).group(1)
    return dict(re.findall(r'\{"([^"]+)", "([^"]+)"\}', body))


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
        self.assertEqual(["colorful-simplified-es-de: no art for pet (_inc/systems/images/pet.webp)",
                          "slate-es-de: no art for pet (pet/images/logo.svg)"],
                         csa.missing(["nes", "pet"], self.THEMES))

    def test_a_catalog_id_draws_as_its_esde_system(self):
        self.assertEqual([], csa.missing(["nes", "coleco"], self.THEMES))

    def test_unknown_theme(self):
        problems = csa.missing(["nes"], {"art-book-next-es-de": {"capabilities.xml"}})
        self.assertEqual(1, len(problems))
        self.assertIn("art-book-next-es-de: no art layout known", problems[0])


class ThemeName(util.TestCase):
    def test_names(self):
        for rom, want in (("nes", "nes"), ("PS", "psx"), ("coleco", "colecovision"), ("3ds", "n3ds"),
                          ("wiiware", "wii"), ("g7400", "videopac"), ("pico", "pico"), ("pico8", "pico8"),
                          ("astrocade", "astrocade"), ("astrocde", "astrocade"), ("lynx", "atarilynx"),
                          ("switch2", "switch2"), ("my_games", "my_games"), ("", "")):
            self.assertEqual(want, csa.theme_name(rom), rom)
        self.assertEqual("megadrive", csa.theme_name("", "MD"))


class MatchesNano(util.TestCase):
    """theme_name()'s tables are NanoEsdeSystemName.cpp's, so the check sees what Nano draws."""

    def setUp(self):
        super().setUp()
        cpp = os.path.join(NANO, "NanoEsdeSystemName.cpp")
        if not os.path.isfile(cpp):
            self.skipTest(f"{cpp} isn't on this host")
        self.src = util.slurp(cpp, text=True)

    def test_esde_systems(self):
        inc = util.slurp(os.path.join(NANO, "NanoEsdeSystemNames.inc"), text=True)
        self.assertEqual(set(re.findall(r'\{"([^"]+)", "', inc)), set(csa.ESDE_SYSTEMS))

    def test_tables(self):
        self.assertEqual(cpp_table(self.src, "kAlias"), csa.FOLDER_ALIASES)
        self.assertEqual(cpp_table(self.src, "kCatalog"), csa.CATALOG_IDS)
        self.assertEqual(cpp_table(self.src, "kGamesFor"), csa.GAMES_FOR)

    def test_renames(self):
        renames = dict(re.findall(r'if \(sys == "([^"]+)"\) return "([^"]+)";', self.src))
        self.assertEqual(renames, csa.THEME_RENAMES)


class MissingCatalogArt(util.TestCase):
    FILES = {"_inc/systems/images/colecovision.webp", "_inc/systems/metadata-global/colecovision.xml",
             "_inc/systems/images/chip8.webp"}

    def test_complete(self):
        self.assertEqual([], csa.missing_catalog_art(["coleco"], {"colorful-simplified-es-de": self.FILES}))

    def test_missing_picture_and_metadata(self):
        self.assertEqual(
            ["colorful-simplified-es-de: catalog platform chip8 has no _inc/systems/metadata-global/chip8.xml "
             "(it would show the default)",
             "colorful-simplified-es-de: catalog platform pet has no _inc/systems/images/pet.webp "
             "(it would show the default)",
             "colorful-simplified-es-de: catalog platform pet has no _inc/systems/metadata-global/pet.xml "
             "(it would show the default)"],
            csa.missing_catalog_art(["chip8", "pet"], {"colorful-simplified-es-de": self.FILES}))

    def test_only_colorful_is_checked(self):
        self.assertEqual([], csa.missing_catalog_art(["pet"], {"slate-es-de": {"capabilities.xml"}}))


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

    def catalog(self, *uids):
        for uid in uids:
            self.write(f"catalog/{uid}.json", json.dumps({"platform": {"uniqueId": uid}}))
        return self.path("catalog")

    def test_catalog_ok(self):
        themes = self.themes()
        self.write("themes/colorful-simplified-es-de/_inc/systems/metadata-global/colecovision.xml", "<theme/>")
        proc = util.run_script("check_system_art", self.systems("nes"), themes, self.catalog("coleco"))
        self.assertOk(proc)
        self.assertIn("1 catalog platforms have pictures", proc.stdout)

    def test_catalog_platform_without_a_picture_fails(self):
        self.assertFails(util.run_script("check_system_art", self.systems("nes"), self.themes(),
                                         self.catalog("coleco", "pet")),
                         "catalog platform pet has no _inc/systems/images/pet.webp")

    def test_empty_catalog_fails(self):
        os.makedirs(self.path("catalog"))
        self.assertFails(util.run_script("check_system_art", self.systems("nes"), self.themes(), self.path("catalog")),
                         "no catalog platforms")

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
        self.assertEqual([], csa.missing(shipped_systems(), {"colorful-simplified-es-de": shipped_colorful(zip_path)}))

    def test_colorful_has_every_catalog_platform(self):
        """Every catalog platform resolves to a real picture, the theme's or this project's."""
        zip_path = colorful_zip()
        if not os.path.isfile(zip_path) or not os.path.isdir(CATALOG):
            self.skipTest(f"needs {zip_path} and {CATALOG}")
        self.assertEqual([], csa.missing_catalog_art(csa.catalog_platforms(CATALOG),
                                                     {"colorful-simplified-es-de": shipped_colorful(zip_path)}))

    def test_folders_are_esde_names(self):
        """The check looks art up by ROM folder, which is what Nano draws for an ES-DE system name."""
        for rom in shipped_systems():
            self.assertRegex(rom, re.compile(r"^[a-z0-9-]+$"), rom)
