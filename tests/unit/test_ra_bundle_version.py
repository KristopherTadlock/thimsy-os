"""ra_bundle_version.py: RetroArch's retroarch.cfg marked as set up for the shipped APK's assets, so
its first start from the icon doesn't extract them over the payload (the pinned core info files)."""
import os
import struct
import zipfile

from tests import util

VERSION_CODE_ATTR = 0x0101021B


def string_pool(strings):
    offsets, body = [], b""
    for s in strings:
        offsets.append(len(body))
        enc = s.encode("utf-16-le")
        body += struct.pack("<H", len(s)) + enc + b"\0\0"
    body += b"\0" * (-len(body) % 4)
    header = 28 + 4 * len(strings)
    return struct.pack("<HHIIIIII", 0x0001, 28, header + len(body), len(strings), 0, 0, header, 0) + \
        struct.pack(f"<{len(strings)}I", *offsets) + body


def manifest(attrs, strings, resmap=()):
    """A binary AndroidManifest.xml: a <manifest> element with attrs [(name index, type, value)]."""
    chunks = string_pool(strings)
    if resmap:
        chunks += struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(resmap)) + struct.pack(f"<{len(resmap)}I", *resmap)
    attr = b"".join(struct.pack("<IIIHBBI", 0xFFFFFFFF, n, 0xFFFFFFFF, 8, 0, t, v) for n, t, v in attrs)
    element = struct.pack("<IIHHHHHH", 0xFFFFFFFF, strings.index("manifest"), 20, 20, len(attrs), 0, 0, 0) + attr
    chunks += struct.pack("<HHII", 0x0102, 16, 16 + len(element), 1) + struct.pack("<I", 0xFFFFFFFF) + element
    return struct.pack("<HHI", 0x0003, 8, 8 + len(chunks)) + chunks


CFG = ('audio_out_rate = "48000"\n'
       'bundle_assets_extract_enable = "true"\n'
       'bundle_assets_extract_last_version = "1763226224"\n'
       'bundle_assets_extract_version_current = "1763226224"\n'
       'savefile_directory = "/storage/emulated/0/RetroArch/saves"\n')


class RaBundleVersion(util.TestCase):
    def apk(self, xml):
        path = self.path("RetroArch_aarch64.apk")
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("AndroidManifest.xml", xml)
            z.writestr("assets/info/snes9x_libretro.info", "x")
        return path

    def run_it(self, xml, cfg=CFG):
        c = self.write("retroarch.cfg", cfg)
        return util.run_script("ra_bundle_version", self.apk(xml), c), util.slurp(c, text=True)

    def test_sets_both_keys_to_the_apks_version(self):
        xml = manifest([(1, 0x10, 1763607214), (2, 0x03, 0)], ["manifest", "versionCode", "versionName"])
        proc, cfg = self.run_it(xml)
        self.assertOk(proc)
        self.assertEqual(cfg, CFG.replace("1763226224", "1763607214"))
        proc, again = self.run_it(xml, cfg)   # a second run changes nothing
        self.assertOk(proc)
        self.assertEqual(again, cfg)

    def test_attribute_known_by_resource_id(self):
        xml = manifest([(0, 0x10, 7), (1, 0x11, 1234)], ["minSdkVersion", "", "manifest"],
                       resmap=(0x0101020C, VERSION_CODE_ATTR))
        proc, cfg = self.run_it(xml, 'savefile_directory = "default"')
        self.assertOk(proc)
        self.assertEqual(cfg, 'savefile_directory = "default"\n'
                              'bundle_assets_extract_last_version = "1234"\n'
                              'bundle_assets_extract_version_current = "1234"\n')

    def test_no_version_code_fails(self):
        xml = manifest([(1, 0x03, 0)], ["manifest", "versionName"])
        proc, cfg = self.run_it(xml)
        self.assertFails(proc, "no versionCode")
        self.assertEqual(cfg, CFG)

    def test_mod_marks_the_payload_config(self):
        text = util.slurp(os.path.join(util.MODS, "10-update-emulators.sh"), text=True)
        self.assertIn('ra_bundle_version.py" "$CACHE/RetroArch_aarch64.apk"', text)
        self.assertIn('"$work/tree/sdcard/Android/data/com.retroarch.aarch64/files/retroarch.cfg"', text)
        self.assertIn('"RetroArch 1.22.2|', text)
        self.assertIn("|RetroArch_aarch64.apk|", text)

    def test_shipped_apk(self):
        cache = os.environ.get("THIMSY_TEST_CACHE")
        apk = cache and os.path.join(cache, "emulators", "RetroArch_aarch64.apk")
        if not apk or not os.path.exists(apk):
            self.skipTest("no THIMSY_TEST_CACHE RetroArch APK")
        c = self.write("retroarch.cfg", CFG)
        self.assertOk(util.run_script("ra_bundle_version", apk, c))
        self.assertRegex(util.slurp(c, text=True), r'bundle_assets_extract_last_version = "\d{6,}"')
