# SPDX-License-Identifier: MIT
"""The default ES-DE theme and its settings, shared with Android (decision OA-1).

scripts/colorful_theme.sh prepares the Colorful (Simplified) theme for both
builds: mods/50-nano.sh sources it, linux/build.sh runs it into the asset
export before assets.list filters it. Its "build" step runs here on a small
fixture (a stand-in theme zip and fonts; no download), with the repository's
console pictures, and the result goes through the committed assets.list.
mods/nano-theme.props holds the theme settings: Android writes its lines into
build.prop, Linux installs it as persist-defaults.props, which S01props hands
to thimsy-prop boot --defaults (OA-35).

The script needs bash and unzip: on Windows Git for Windows' bash.exe (never
the bash.exe of System32, which is WSL's), elsewhere bash from PATH. Without
them the script tests are skipped. THIMSY_PYTHON points the script at this
interpreter.

    py -3.12 -I -m unittest discover -s linux/tests -t linux -p test_theme.py
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from typing import Dict, List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
REPO = LINUX.parent
sys.path.insert(0, str(LINUX / "tools"))

import assets_allowlist as aa  # noqa: E402

SCRIPT = REPO / "scripts" / "colorful_theme.sh"
PICTURES = REPO / "mods" / "console-pictures"
PROPS = REPO / "mods" / "nano-theme.props"
MOD = REPO / "mods" / "50-nano.sh"
LIST = LINUX / "package" / "thimsy-data" / "assets.list"
HAVE_REPO = SCRIPT.is_file() and PICTURES.is_dir() and MOD.is_file()


def find_bash() -> Optional[str]:
    if os.name == "nt":
        for cand in (r"C:\Program Files\Git\bin\bash.exe", r"C:\Program Files\Git\usr\bin\bash.exe"):
            if os.path.exists(cand):
                return cand
        return None
    return shutil.which("bash")


BASH = find_bash()


def _has_unzip() -> bool:
    if BASH is None:
        return False
    try:
        r = subprocess.run([BASH, "-c", "command -v unzip"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0


CAN_RUN = HAVE_REPO and _has_unzip()


def script_var(name: str) -> str:
    m = re.search(r'^%s="([^"]+)"' % name, SCRIPT.read_text(encoding="utf-8"), re.M)
    assert m, name
    return m.group(1)


THEME_XML = """<theme>
    <fontPath>./_inc/fonts/Gilroy-Light.ttf</fontPath>
    <fontPath>./_inc/fonts/Gilroy-Regular.ttf</fontPath>
    <fontPath>./_inc/fonts/Gilroy-SemiBold.ttf</fontPath>
    <fontPath>./_inc/fonts/Gilroy-Bold.ttf</fontPath>
    <fontPath>./_inc/fonts/Akrobat-Bold.ttf</fontPath>
</theme>
"""


def meta_xml(colour: str) -> str:
    return ("<theme><variables><systemColor>%s</systemColor><systemColorPalette1>%s</systemColorPalette1>"
            "<systemColorPalette2>%s</systemColorPalette2><systemColorPalette3>%s</systemColorPalette3>"
            "<systemColorPalette4>%s</systemColorPalette4></variables></theme>\n" % ((colour,) * 5))


def theme_files() -> Dict[str, str]:
    """A stand-in for the theme at its pinned commit: the files the script changes, and a few it keeps."""
    return {
        ".gitignore": "*.tmp\n",
        "README.md": "# Colorful (Simplified)\n\nCC BY-NC-SA 2.0.\n",
        "capabilities.xml": "<themeCapabilities/>\n",
        "theme.xml": THEME_XML,
        "_inc/fonts/Gilroy-Light.ttf": "gilroy", "_inc/fonts/Gilroy-Regular.ttf": "gilroy",
        "_inc/fonts/Gilroy-SemiBold.ttf": "gilroy", "_inc/fonts/Gilroy-Bold.ttf": "gilroy",
        "_inc/fonts/Akrobat-Bold.ttf": "akrobat",
        "_inc/images/space.png": "png", "_inc/sounds/nav.wav": "wav",
        "_inc/systems/images/quake.webp": "the theme's quake", "_inc/systems/images/nes.webp": "nes",
        "_inc/systems/metadata-global/quake.xml": meta_xml("FF0000"),
        "_inc/systems/metadata-global/moonlight.xml": meta_xml("00FF00"),
        "_inc/systems/metadata-global/nes.xml": meta_xml("123456"),
        "_inc/systems/metadata-custom/nes.xml": "<theme/>\n",
    }


def write_cache(cache: Path, files: Dict[str, str]) -> None:
    """What colorful_theme.sh fetch leaves in CACHE_DIR, with stand-in contents."""
    theme, commit = script_var("THEME"), script_var("THEME_COMMIT")
    (cache / "fonts").mkdir(parents=True)
    top = "%s-%s/" % (theme, commit)
    with zipfile.ZipFile(str(cache / ("%s-%s.zip" % (theme, commit))), "w") as z:
        for name, body in files.items():
            z.writestr(top + name, body)
    with zipfile.ZipFile(str(cache / "fonts" / "Urbanist-fonts-1.330.zip"), "w") as z:
        for w in ("Light", "Regular", "SemiBold", "Bold", "Black"):
            z.writestr("Urbanist-fonts/fonts/ttf/Urbanist-%s.ttf" % w, "urbanist " + w)
    for name in ("Urbanist-OFL.txt", "Barlow-OFL.txt", "BarlowSemiCondensed-Medium.ttf",
                 "BarlowSemiCondensed-SemiBold.ttf", "BarlowSemiCondensed-Bold.ttf"):
        (cache / "fonts" / name).write_bytes(name.encode())


def tree(root: Path) -> Dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}


def text(b: bytes) -> str:
    return b.decode("utf-8").replace("\r\n", "\n")


@unittest.skipUnless(CAN_RUN, "needs the repository's scripts/ and mods/, and bash with unzip")
class ColorfulThemeScript(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="thimsy-theme-"))
        self.addCleanup(shutil.rmtree, str(self.tmp), True)
        self.cache = self.tmp / "cache"
        self.out = self.tmp / "export" / "nano_esde_themes" / script_var("THEME")
        self.out.parent.mkdir(parents=True)

    def run_script(self, *args: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, THIMSY_PYTHON=Path(sys.executable).as_posix())
        return subprocess.run([BASH, SCRIPT.as_posix()] + list(args), env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, universal_newlines=True, timeout=300)

    def build(self, files: Optional[Dict[str, str]] = None, pictures: Optional[Path] = None
              ) -> subprocess.CompletedProcess:
        write_cache(self.cache, theme_files() if files is None else files)
        args = ["build", self.cache.as_posix(), self.out.as_posix()]
        if pictures is not None:
            args.append(pictures.as_posix())
        return self.run_script(*args)

    def test_build_makes_the_theme_android_ships(self) -> None:
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        got = tree(self.out)
        fonts = "_inc/fonts/"
        for name in ("Gilroy-Light.ttf", "Gilroy-Regular.ttf", "Gilroy-SemiBold.ttf", "Gilroy-Bold.ttf",
                     "Akrobat-Bold.ttf"):
            self.assertNotIn(fonts + name, got)
        for w in ("Light", "Regular", "SemiBold", "Bold"):
            self.assertEqual(got[fonts + "Urbanist-%s.ttf" % w], ("urbanist " + w).encode())
        self.assertNotIn(fonts + "Urbanist-Black.ttf", got)
        for name in ("BarlowSemiCondensed-Bold.ttf", "Urbanist-OFL.txt", "Barlow-OFL.txt"):
            self.assertEqual(got[fonts + name], name.encode())
        self.assertNotIn(fonts + "BarlowSemiCondensed-Medium.ttf", got)
        xml = text(got["theme.xml"])
        for name in ("Urbanist-Light", "Urbanist-Regular", "Urbanist-SemiBold", "Urbanist-Bold",
                     "BarlowSemiCondensed-Bold"):
            self.assertEqual(xml.count("./_inc/fonts/%s.ttf" % name), 1, name)
        self.assertNotIn("Gilroy", xml)
        self.assertNotIn("Akrobat", xml)
        self.assertNotIn(".gitignore", got)
        # This project's pictures and metadata, the Quake picture in place of the theme's.
        for kind in ("images", "metadata-global", "metadata-custom"):
            for p in sorted((PICTURES / kind).iterdir()):
                self.assertEqual(got["_inc/systems/%s/%s" % (kind, p.name)], p.read_bytes(), p.name)
        self.assertEqual(got["_inc/systems/images/nes.webp"], b"nes")
        self.assertEqual(got["_inc/systems/THIMSY-PICTURES-LICENSE.txt"], (PICTURES / "LICENSE").read_bytes())
        import json
        colours = json.loads((PICTURES / "theme-colours.json").read_text(encoding="utf-8"))
        for system, values in colours.items():
            meta = text(got["_inc/systems/metadata-global/%s.xml" % system])
            for tag, value in values.items():
                if tag != "why":
                    self.assertIn("<%s>%s</%s>" % (tag, value, tag), meta)
        self.assertIn("<systemColor>123456</systemColor>", text(got["_inc/systems/metadata-global/nes.xml"]))
        readme = text(got["README.md"])
        self.assertTrue(readme.startswith("# Colorful (Simplified)\n\nCC BY-NC-SA 2.0.\n\n## Changes in Thimsy OS\n"))
        self.assertIn("`_inc/systems/THIMSY-PICTURES-LICENSE.txt`", readme)
        # Only the theme folder is left beside it (the unpack folder is gone).
        self.assertEqual(sorted(p.name for p in self.out.parent.iterdir()), [self.out.name])

    def test_the_prepared_theme_passes_assets_list(self) -> None:
        """thimsy-data installs every file of the prepared theme; the theme's own Quake
        picture, Gilroy and Akrobat never (assets.list, ASSETS.md)."""
        r = self.build()
        self.assertEqual(r.returncode, 0, r.stderr)
        export = self.out.parent.parent
        dest, report = self.tmp / "dest", self.tmp / "report.txt"
        code = aa.main(["install", "--list", str(LIST), "--report", str(report), str(export), str(dest)])
        self.assertEqual(code, 0)
        self.assertEqual(sorted(tree(dest)), sorted("nano_esde_themes/%s/%s" % (self.out.name, f)
                                                    for f in tree(self.out)))
        self.assertNotIn("left out", report.read_text(encoding="utf-8"))
        # The theme's own files under the same names are left out.
        theme = "nano_esde_themes/%s/" % self.out.name
        (self.out / "_inc/systems/images/quake.webp").write_bytes(b"the theme's quake")
        (self.out / "_inc/fonts/Gilroy-Bold.ttf").write_bytes(b"gilroy")
        (self.out / "_inc/fonts/Akrobat-Bold.ttf").write_bytes(b"akrobat")
        (export / "nano_esde_themes/slate-es-de.zip").write_bytes(b"PK")
        dest2, report2 = self.tmp / "dest2", self.tmp / "report2.txt"
        code = aa.main(["install", "--list", str(LIST), "--report", str(report2), str(export), str(dest2)])
        self.assertEqual(code, 0)
        left = report2.read_text(encoding="utf-8")
        for rel in ("_inc/systems/images/quake.webp", "_inc/fonts/Gilroy-Bold.ttf", "_inc/fonts/Akrobat-Bold.ttf"):
            self.assertIn("left out\t%s%s\tdenied" % (theme, rel), left)
        self.assertIn("left out\tnano_esde_themes/slate-es-de.zip\tdenied", left)
        self.assertFalse((dest2 / theme / "_inc/systems/images/quake.webp").exists())

    def test_refusals(self) -> None:
        # OUT already there.
        self.out.mkdir()
        r = self.build()
        self.assertEqual(r.returncode, 1)
        self.assertIn("already exists", r.stderr)
        self.out.rmdir()
        shutil.rmtree(str(self.cache))
        # A theme whose theme.xml does not name a replaced font once.
        files = theme_files()
        files["theme.xml"] = THEME_XML.replace("./_inc/fonts/Akrobat-Bold.ttf", "./_inc/fonts/Other.ttf")
        r = self.build(files)
        self.assertEqual(r.returncode, 1)
        self.assertIn("theme.xml: ./_inc/fonts/Akrobat-Bold.ttf not found once", r.stderr)
        shutil.rmtree(str(self.cache))
        shutil.rmtree(str(self.out))
        # A console picture that would replace a file of the theme's.
        pictures = self.tmp / "pictures"
        shutil.copytree(str(PICTURES), str(pictures))
        (pictures / "images" / "nes.webp").write_bytes(b"ours")
        r = self.build(pictures=pictures)
        self.assertEqual(r.returncode, 1)
        self.assertIn("the theme already has _inc/systems/images/nes.webp", r.stderr)

    def test_usage_and_name(self) -> None:
        r = self.run_script("name")
        self.assertEqual((r.returncode, r.stdout), (0, "colorful-simplified-es-de\n"))
        for args in ((), ("build", "x"), ("fetch",), ("bogus",)):
            self.assertEqual(self.run_script(*args).returncode, 2, args)


@unittest.skipUnless(HAVE_REPO, "needs the repository's scripts/ and mods/")
class SharedWithAndroid(unittest.TestCase):
    """One script and one settings list for both builds (OA-1)."""

    WANT = {
        "persist.gammaos.nano.ndstheme": "3",
        "persist.gammaos.nano.esde.themeset": "colorful-simplified-es-de",
        "persist.gammaos.nano.esde.colorscheme": "colorful-dark",
        "persist.gammaos.nano.esde.settingstheme": "minima",
        "persist.gammaos.nano.minima.bg": "1a1a2e",
        "persist.gammaos.nano.nav_sounds": "0",
    }

    def props(self) -> List[str]:
        return [line for line in PROPS.read_text(encoding="utf-8").splitlines()
                if line and not line.startswith("#")]

    def test_theme_settings(self) -> None:
        lines = self.props()
        self.assertEqual(dict(line.split("=", 1) for line in lines), self.WANT)
        for line in lines:
            self.assertRegex(line, r"^persist\.[A-Za-z0-9._-]+=[^\s]+$")
            # thimsy-prop boot --defaults ignores a value the store refuses (over 91 bytes).
            self.assertLessEqual(len(line.split("=", 1)[1].encode("utf-8")), 91, line)
        self.assertEqual(self.WANT["persist.gammaos.nano.esde.themeset"], script_var("THEME"))

    def test_android_writes_the_same_lines(self) -> None:
        mod = MOD.read_text(encoding="utf-8")
        self.assertIn('source "$MODS/../scripts/colorful_theme.sh"', mod)
        self.assertIn('colorful_theme_fetch "$cache"', mod)
        self.assertIn('colorful_theme_build "$cache" "$src" "$MODS/console-pictures"', mod)
        self.assertIn("grep '^persist\\.' \"$MODS/nano-theme.props\"", mod)
        for key in self.WANT:
            self.assertNotRegex(mod, r"(?m)^%s=" % re.escape(key))
        # The mod's second-run guard still covers every key of the list.
        guard = re.search(r"grep -qE '(\^\(persist.*?)' \"\$SYSTEM/build\.prop\"", mod).group(1)
        for key in self.WANT:
            self.assertRegex(key + "=", guard)
        # The theme preparation lives only in the shared script.
        for old in ('unzip -q "$zip"', "Gilroy-Light", "THIMSY-PICTURES-LICENSE", "## Changes in Thimsy OS"):
            self.assertNotIn(old, mod)
        self.assertNotIn("sudo", SCRIPT.read_text(encoding="utf-8"))

    def test_linux_build_prepares_the_theme_before_the_filter(self) -> None:
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        export = build.index('nano_tree.py" export-assets')
        head = build.index('archive -o "$assets/theme-src.tar" HEAD scripts/colorful_theme.sh mods/console-pictures')
        slate = build.index('rm -f "$themes/slate-es-de.zip"')
        prepare = build.index('bash "$theme_sh" prepare "$DL/colorful-theme" "$themes/$(bash "$theme_sh" name)"')
        filt = build.index('assets_allowlist.py" filter')
        defaults = build.index('show HEAD:mods/nano-theme.props')
        pack = build.index('gzip -9n >"$SRC/nano-assets-$SHA.tar.gz"')
        self.assertLess(export, head)
        self.assertLess(head, prepare)
        self.assertLess(slate, filt)
        self.assertLess(prepare, filt)
        self.assertLess(filt, defaults)
        self.assertLess(defaults, pack)
        self.assertIn('>"$assets/nano-assets-$SHA/.thimsy-defaults/nano-theme.props"', build)

    def test_linux_installs_the_settings_for_s01props(self) -> None:
        mk = (LINUX / "package/thimsy-data/thimsy-data.mk").read_text(encoding="utf-8")
        self.assertRegex(mk, r"\$\(INSTALL\) -D -m 0644 \$\(@D\)/\.thimsy-defaults/nano-theme\.props \\\n"
                             r"\t\t\$\(TARGET_DIR\)/usr/share/thimsy/persist-defaults\.props")
        # S01props hands the file to thimsy-prop boot, which seeds the store (OA-35).
        s01 = (LINUX / "board/common/rootfs-overlay/etc/init.d/S01props").read_text(encoding="utf-8")
        self.assertIn('--defaults "$R/usr/share/thimsy/persist-defaults.props"', s01)
        self.assertNotIn("persist_defaults", s01)

    def test_assets_list_keeps_quake_only_with_thimsy_bytes(self) -> None:
        entries = aa.read_list(LIST)
        exact = {e.pattern: e.sha256 for e in entries if e.action == "keep-sha256"}
        quake = "nano_esde_themes/colorful-simplified-es-de/_inc/systems/images/quake.webp"
        self.assertEqual(exact, {quake: aa.file_sha256(PICTURES / "images" / "quake.webp")})
        al = aa.Allowlist(entries)
        self.assertFalse(al.decide(quake)[0])    # without the file: denied
        self.assertTrue(al.decide(quake, PICTURES / "images" / "quake.webp")[0])
        other = Path(tempfile.mkdtemp(prefix="thimsy-theme-"))
        self.addCleanup(shutil.rmtree, str(other), True)
        (other / "quake.webp").write_bytes(b"the theme's quake")
        self.assertFalse(al.decide(quake, other / "quake.webp")[0])

    def test_glob_with_any_folders(self) -> None:
        r = aa.glob_regex("t/**/*.xml")
        for path, ok in (("t/a.xml", True), ("t/x/y/a.xml", True), ("t/x/a.png", False), ("u/a.xml", False),
                         ("t/x/a.xml/b", False)):
            self.assertEqual(bool(r.match(path)), ok, path)


if __name__ == "__main__":
    unittest.main()
