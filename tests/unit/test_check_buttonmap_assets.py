"""check_buttonmap_assets.py: the system image carries the files Nano's button map reads."""
import json
import os

from tests import util

cba = util.load_script("check_buttonmap_assets")

BM = "/system/etc/nano_xmb/buttonmap"
CT = "/system/etc/nano_xmb/controllers"
CATALOG = json.dumps({"drawings": {"nes": {"file": "svg/nes.svg"}, "snes": {"file": "svg/snes.svg"}}})


def full():
    return {
        BM: {"pads.json": 10, "retropad.json": 10, "browser.json": 10},
        CT: {"controllers.json": len(CATALOG), "LICENSE": 10},
        CT + "/svg": {"nes.svg": 10, "snes.svg": 10},
    }


class Missing(util.TestCase):
    def test_complete(self):
        self.assertEqual([], cba.missing(full(), CATALOG))

    def test_each_buttonmap_file(self):
        for name in ("pads.json", "retropad.json", "browser.json"):
            listings = full()
            del listings[BM][name]
            self.assertEqual([BM + "/" + name], cba.missing(listings, CATALOG), name)

    def test_no_buttonmap_folder(self):
        listings = full()
        listings[BM] = None
        self.assertEqual([BM + "/pads.json", BM + "/retropad.json", BM + "/browser.json"],
                         cba.missing(listings, CATALOG))

    def test_no_controllers(self):
        listings = full()
        listings[CT] = None
        del listings[CT + "/svg"]
        self.assertEqual([CT + "/controllers.json"], cba.missing(listings, None))

    def test_a_drawing(self):
        listings = full()
        del listings[CT + "/svg"]["snes.svg"]
        self.assertEqual([CT + "/svg/snes.svg"], cba.missing(listings, CATALOG))
        listings[CT + "/svg"] = None
        self.assertEqual([CT + "/svg/nes.svg", CT + "/svg/snes.svg"], cba.missing(listings, CATALOG))

    def test_bad_catalog(self):
        for text in ("", "{", "[]", "{}", '{"drawings": {}}', '{"drawings": {"a": {}}}', '{"drawings": []}'):
            self.assertEqual([CT + "/controllers.json: no drawings list"], cba.missing(full(), text), text)

    def test_drawing_outside_the_folder(self):
        for f in ("../buttonmap/pads.json", "/system/etc/x.svg", 7):
            text = json.dumps({"drawings": {"nes": {"file": "svg/nes.svg"}, "x": {"file": f}}})
            gone = cba.missing(full(), text)
            self.assertEqual(1, len(gone), f)
            self.assertIn("is outside " + CT, gone[0])

    def test_aosp_tree(self):
        """The files the gammaos-nano prebuilts install (AOSP=<tree>, default ~/gammaos-src)."""
        src = os.path.join(util.AOSP, "frameworks/base/cmds/gammaos-nano/ps3xmb")
        if not os.path.isdir(src):
            self.skipTest("no AOSP tree at %s" % util.AOSP)
        listings = {BM: {n: 1 for n in os.listdir(os.path.join(src, "buttonmap"))},
                    CT: {n: 1 for n in os.listdir(os.path.join(src, "controllers"))},
                    CT + "/svg": {n: 1 for n in os.listdir(os.path.join(src, "controllers/svg"))}}
        catalog = util.slurp(os.path.join(src, "controllers/controllers.json"), text=True)
        self.assertEqual([], cba.missing(listings, catalog))


class Image(util.TestCase):
    def setUp(self):
        super().setUp()
        self.bin = util.need(self, "mke2fs", "debugfs")
        self.env = {"PATH": os.pathsep.join(self.bin + [os.environ["PATH"]])}

    def image(self, skip=(), empty=(), shared_blocks=False):
        root = self.path("root")
        util.rmtree(root)
        os.makedirs(os.path.join(root, "system/bin"))
        files = {BM + "/pads.json": "{}", BM + "/retropad.json": "{}", BM + "/browser.json": "{}",
                 CT + "/controllers.json": CATALOG, CT + "/svg/nes.svg": "<svg/>", CT + "/svg/snes.svg": "<svg/>"}
        for path, text in files.items():
            if path not in skip:
                self.write("root" + path, "" if path in empty else text)
        img = self.path("system.img")
        self.assertOk(util.run(["mke2fs", "-q", "-F", "-t", "ext4", "-O", "^metadata_csum", "-d", root, img, "4M"],
                               env=self.env))
        if shared_blocks:
            # As in test_check_charger_rc: the source build's read-only ext4 with shared blocks.
            with open(img, "r+b") as f:
                f.seek(1024 + 0x64)
                ro_compat = int.from_bytes(f.read(4), "little") | 0x4000
                f.seek(1024 + 0x64)
                f.write(ro_compat.to_bytes(4, "little"))
        return img

    def check(self, *args):
        return util.run_script("check_buttonmap_assets", *args, env=self.env)

    def test_good(self):
        proc = self.assertOk(self.check(self.image()))
        self.assertIn("the system image has the button map's files", proc.stdout)

    def test_shared_blocks(self):
        self.assertOk(self.check(self.image(shared_blocks=True)))
        self.assertFails(self.check(self.image(skip={BM + "/browser.json"}, shared_blocks=True)),
                         BM + "/browser.json")

    def test_no_pads(self):
        """The RG Cube would be taken for a standard pad."""
        self.assertFails(self.check(self.image(skip={BM + "/pads.json"})), "lacks the button map's files",
                         BM + "/pads.json")

    def test_empty_file(self):
        self.assertFails(self.check(self.image(empty={BM + "/retropad.json"})), BM + "/retropad.json")

    def test_no_drawing(self):
        self.assertFails(self.check(self.image(skip={CT + "/svg/snes.svg"})), CT + "/svg/snes.svg")

    def test_no_controllers(self):
        proc = self.check(self.image(skip={CT + "/controllers.json", CT + "/svg/nes.svg", CT + "/svg/snes.svg"}))
        self.assertFails(proc, CT + "/controllers.json")

    def test_not_ext4(self):
        """A sparse or EROFS image: debugfs exits 0 here too, and the reason is passed on."""
        img = self.write("system.img", b"\0" * 65536)
        self.assertFails(self.check(img), "debugfs could not read", "Bad magic number")

    def test_usage(self):
        self.assertFails(self.check(), "check_buttonmap_assets.py SYSTEM_IMG")


class Build(util.TestCase):
    def test_build_checks_the_system_image_first(self):
        """build.sh runs the check on SYSTEM_IMG as given, before sudo and the long steps."""
        text = util.slurp(os.path.join(util.SCRIPTS, "build.sh"), text=True)
        call = 'python3 -I "$ROOT/scripts/check_buttonmap_assets.py" "$SYSTEM_IMG"\n'
        self.assertEqual(text.count(call), 1)
        at = text.index(call)
        self.assertLess(at, text.index("\nsudo -v\n"))
        self.assertLess(at, text.index('cp "$SYSTEM_IMG" "$system"'))
        self.assertGreater(at, text.index('SYSTEM_IMG="$(realpath "$SYSTEM_IMG")"'))
        # Only a source-built system image has Nano's button map.
        self.assertGreater(at, text.index('if [[ -n "${SYSTEM_IMG:-}" ]]; then\n  AOSP='))
