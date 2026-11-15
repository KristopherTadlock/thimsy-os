"""mods/console-pictures: the pictures this project adds to Colorful, and their credits."""
import json
import os
import re
import struct
import xml.etree.ElementTree as ET

from tests import util

PICTURES = os.path.join(util.MODS, "console-pictures")
CATALOG = os.path.join(util.AOSP, "frameworks/base/cmds/gammaos-nano/ps3xmb/daijishou")
# Pictures made ahead of their catalog platform.
PLANNED = {"switch2"}
# The licences a source photo may have: free to redistribute and adapt, commercially too. A CC 2.0
# or 3.0 licence may be a national port, such as "CC BY-SA 2.0 de".
PHOTO_LICENCE = re.compile(r"^(Public domain|CC0|CC BY(-SA)? [1-4]\.\d( [a-z]{2})?)$")
COMMONS = "https://commons.wikimedia.org/wiki/File:"

csa = util.load_script("check_system_art")


def webp_info(path):
    """(width, height, has alpha) of an extended-format WebP."""
    with open(path, "rb") as f:
        head = f.read(30)
    assert head[:4] == b"RIFF" and head[8:12] == b"WEBP", path
    assert head[12:16] == b"VP8X", f"{path}: not an extended WebP"
    w = struct.unpack("<I", head[24:27] + b"\0")[0] + 1
    h = struct.unpack("<I", head[27:30] + b"\0")[0] + 1
    return w, h, bool(head[20] & 0x10)


def images():
    return sorted(f[:-5] for f in os.listdir(f"{PICTURES}/images"))


def sources():
    with open(f"{PICTURES}/sources.json", encoding="utf-8") as f:
        return json.load(f)


class Pictures(util.TestCase):
    def test_images(self):
        for name in images():
            path = f"{PICTURES}/images/{name}.webp"
            self.assertEqual((1080, 1080, True), webp_info(path), name)
            self.assertLess(os.path.getsize(path), 300 * 1024, name)

    def test_metadata(self):
        names = set(images())
        for kind in ("metadata-global", "metadata-custom"):
            self.assertEqual([], sorted({f[:-4] for f in os.listdir(f"{PICTURES}/{kind}")} - names), kind)
        for f in sorted(os.listdir(f"{PICTURES}/metadata-global")):
            variables = ET.parse(f"{PICTURES}/metadata-global/{f}").getroot().find("variables")
            self.assertRegex(variables.findtext("systemColor"), r"^[0-9A-F]{6}$", f)
            self.assertTrue(variables.findtext("systemName"), f)
        for f in os.listdir(f"{PICTURES}/metadata-custom"):
            self.assertIn("${systemManufacturer}", util.slurp(f"{PICTURES}/metadata-custom/{f}", text=True), f)

    def test_each_picture_is_for_a_catalog_platform(self):
        if not os.path.isdir(CATALOG):
            self.skipTest(f"{CATALOG} isn't on this host")
        platforms = {csa.theme_name(uid) for uid in csa.catalog_platforms(CATALOG)} | PLANNED
        self.assertEqual([], sorted(set(images()) - platforms))

    def test_no_picture_replaces_a_theme_file(self):
        """50-nano.sh refuses to; this catches it before a build. A picture without its own
        metadata-global uses the theme's."""
        from tests.unit.test_check_system_art import colorful_zip
        zip_path = colorful_zip()
        if not os.path.isfile(zip_path):
            self.skipTest(f"{zip_path} isn't on this host")
        files = csa.theme_files(zip_path) - {"_inc/systems/images/quake.webp"}
        for kind in ("images", "metadata-global", "metadata-custom"):
            for f in os.listdir(f"{PICTURES}/{kind}"):
                self.assertNotIn(f"_inc/systems/{kind}/{f}", files)
        own = set(os.listdir(f"{PICTURES}/metadata-global"))
        for name in images():
            if f"{name}.xml" not in own:
                self.assertIn(f"_inc/systems/metadata-global/{name}.xml", files, name)


class Credits(util.TestCase):
    """Every picture's source photos are freely licensed and credited wherever the pictures are."""

    def test_every_picture_has_sources(self):
        self.assertEqual(images(), sorted(sources()))

    def test_photos(self):
        for name, entry in sources().items():
            for photo in entry["photos"]:
                self.assertRegex(photo["licence"], PHOTO_LICENCE, name)
                self.assertTrue(photo["url"].startswith(COMMONS), name)
                self.assertTrue(photo["author"].strip(), name)
                self.assertTrue(photo["title"].strip(), name)

    def test_picture_licence(self):
        """A picture is under its CC BY-SA photo's licence (ShareAlike), otherwise CC BY-NC 4.0."""
        for name, entry in sources().items():
            share_alike = {p["licence"] for p in entry["photos"] if p["licence"].startswith("CC BY-SA")}
            self.assertLessEqual(len(share_alike), 1, name)
            self.assertEqual(share_alike.pop() if share_alike else "CC BY-NC 4.0", entry["licence"], name)

    def test_drawings(self):
        """A picture with no photo is a drawing whose SVG source is kept."""
        drawn = {e["drawn"] for e in sources().values() if not e["photos"]}
        svg = f"{PICTURES}/svg"
        kept = os.listdir(svg) if os.path.isdir(svg) else []
        self.assertEqual(sorted(drawn), sorted(f"svg/{f}" for f in kept))
        for name, entry in sources().items():
            if not entry["photos"]:
                self.assertEqual(f"svg/{name}.svg", entry["drawn"])
                self.assertEqual("CC BY-NC 4.0", entry["licence"], name)

    def test_license_file(self):
        """LICENSE, which the theme gets as THIMSY-PICTURES-LICENSE.txt, credits each picture's photos."""
        text = util.slurp(f"{PICTURES}/LICENSE", text=True)
        blocks = dict(re.findall(r"^(\S+)\.webp: (.*(?:\n  .*)*)", text, re.M))
        self.assertEqual(images(), sorted(blocks))
        for name, entry in sources().items():
            block = blocks[name]
            self.assertTrue(block.startswith(entry["licence"] + ","), name)
            for photo in entry["photos"]:
                for key in ("title", "author", "licence", "url"):
                    self.assertIn(photo[key], block, f"{name}: {key}")
        mod = util.slurp(f"{util.MODS}/50-nano.sh", text=True)
        self.assertIn('cp "$pictures/LICENSE" "$src/_inc/systems/THIMSY-PICTURES-LICENSE.txt"', mod)

    def test_attribution(self):
        rows = util.slurp(os.path.join(util.ROOT, "ATTRIBUTION.md"), text=True).splitlines()
        for name, entry in sources().items():
            for photo in entry["photos"]:
                cells = [name, entry["licence"], photo["title"].replace("|", "\\|"),
                         photo["author"].replace("|", "\\|"), photo["licence"], photo["url"]]
                self.assertIn("| " + " | ".join(cells) + " |", rows, f"{name}: {photo['title']}")

    def test_notice(self):
        self.assertIn("mods/console-pictures/LICENSE", util.slurp(os.path.join(util.ROOT, "NOTICE"), text=True))
