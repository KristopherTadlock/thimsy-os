"""linux/tools/fit.py: the golden .its per device, the header, the read-back (D.4).

When mkimage and dtc are on PATH, a FIT is also built and read back; here
(Windows, and WSL without u-boot-tools) that test skips. The read-back is
still covered by FITs this file assembles itself with a small FDT writer.
"""
from __future__ import annotations

import glob
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))

import fit  # noqa: E402

FIX = os.path.join(HERE, "fixtures", "fit")
DEVICES = sorted(glob.glob(os.path.join(FIX, "devices", "*.json")))
ZSTD = bytes((0x28, 0xB5, 0x2F, 0xFD))


# -- a tiny FDT writer, independent of fit.py's reader

def s(text):
    return text.encode() + b"\0"


def u32(*vals):
    return b"".join(struct.pack(">I", v) for v in vals)


def fdt(root):
    """root: (name, [(prop, bytes)], [children])."""
    strings = bytearray()
    offs = {}
    body = bytearray()

    def stroff(name):
        if name not in offs:
            offs[name] = len(strings)
            strings.extend(s(name))
        return offs[name]

    def pad():
        while len(body) % 4:
            body.append(0)

    def emit(node):
        name, props, children = node
        body.extend(u32(1) + s(name))
        pad()
        for k, v in props:
            body.extend(u32(3, len(v), stroff(k)) + v)
            pad()
        for c in children:
            emit(c)
        body.extend(u32(2))

    emit(root)
    body.extend(u32(9))
    off_struct = 40 + 16
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    hdr = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, 40, 17, 16, 0,
                      len(strings), len(body))
    return hdr + bytes(16) + bytes(body) + bytes(strings)


DTB = fdt(("", [("compatible", s("anbernic,rg-arc-d"))], []))
KERNEL = ZSTD + bytes((0xA0,)) + struct.pack("<I", 40 * 1024 * 1024) + b"kernel" * 50


def fake_fit(*, compression="zstd", hash_node=False, bootargs="init=/init ro",
             kernel=KERNEL, dtb=DTB, external=False):
    kprops = [("description", s("Linux kernel")), ("type", s("kernel")),
              ("arch", s("arm64")), ("os", s("linux")), ("compression", s(compression)),
              ("load", u32(0x02000000)), ("entry", u32(0x02000000))]
    fprops = [("type", s("flat_dt")), ("arch", s("arm64")), ("compression", s("none"))]
    if external:
        kprops.append(("data-offset", u32(0)))
        kprops.append(("data-size", u32(len(kernel))))
        pad = (-len(kernel)) % 4
        fprops.append(("data-offset", u32(len(kernel) + pad)))
        fprops.append(("data-size", u32(len(dtb))))
    else:
        kprops.insert(1, ("data", kernel))
        fprops.insert(0, ("data", dtb))
    kchildren = [("hash-1", [("algo", s("sha256"))], [])] if hash_node else []
    conf = [("kernel", s("kernel")), ("fdt", s("fdt"))]
    if bootargs is not None:
        conf.append(("bootargs", s(bootargs)))
    tree = ("", [("description", s("test")), ("#address-cells", u32(1))], [
        ("images", [], [("kernel", kprops, kchildren), ("fdt", fprops, [])]),
        ("configurations", [("default", s("conf"))], [("conf", conf, [])]),
    ])
    blob = fdt(tree)
    if external:
        blob += b"\0" * ((-len(blob)) % 4) + kernel + b"\0" * ((-len(kernel)) % 4) + dtb
    return blob


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fit.main(argv)
    return rc, out.getvalue(), err.getvalue()


class GoldenTest(unittest.TestCase):
    def test_devices_present(self):
        names = [os.path.basename(p) for p in DEVICES]
        self.assertEqual(names, ["anbernic-rg-arc-d.json", "anbernic-rg-arc-s.json"])

    def test_its_matches_golden(self):
        for path in DEVICES:
            with open(path, encoding="utf-8") as f:
                dev = json.load(f)
            golden = os.path.join(FIX, "%s-%s.its" % (dev["family"], dev["id"]))
            with self.subTest(device=dev["id"]):
                cfg = fit.resolve(dev["family"], None, None, path)
                with open(golden, encoding="utf-8", newline="") as f:
                    want = f.read()
                self.assertEqual(fit.write_its(cfg), want)
                rc, out, _ = run(["its", "--family", dev["family"], "--device-json", path])
                self.assertEqual(rc, 0)
                self.assertEqual(out, want)

    def test_golden_content(self):
        for path in glob.glob(os.path.join(FIX, "*.its")):
            with open(path, encoding="utf-8") as f:
                text = f.read()
            with self.subTest(its=os.path.basename(path)):
                self.assertNotIn("hash", text)
                self.assertNotIn("signature", text)
                self.assertIn('compression = "zstd";', text)
                self.assertIn('data = /incbin/("Image.zst");', text)
                self.assertIn("load = <0x02000000>;", text)
                self.assertRegex(text, r'bootargs = "[^"]*init=/init[^"]*";')
                self.assertNotIn("root=", text)
                dtb = os.path.basename(path)[:-4] + ".dtb"
                self.assertIn('data = /incbin/("%s");' % dtb, text)

    def test_bootargs_rules(self):
        with self.assertRaisesRegex(fit.FitError, "root="):
            fit.resolve("rk3566", "x", "x.dtb", None, "root=/dev/mmcblk1p3")
        old = fit.FAMILIES["rk3566"]["bootargs"]
        try:
            fit.FAMILIES["rk3566"]["bootargs"] = "ro rootwait"
            with self.assertRaisesRegex(fit.FitError, "init=/init"):
                fit.resolve("rk3566", "x", "x.dtb", None)
        finally:
            fit.FAMILIES["rk3566"]["bootargs"] = old
        cfg = fit.resolve("rk3566", "x", "x.dtb", None, "console=ttyS2,1500000")
        self.assertTrue(cfg["bootargs"].endswith(" console=ttyS2,1500000"))
        self.assertIn('bootargs = "init=/init', fit.write_its(cfg))

    def test_board_device_jsons(self):
        """Every board device.json with a DTB resolves; the FIT names the DTB without its
        directory, and the command line still overrides the JSON."""
        board = os.path.join(HERE, os.pardir, "board")
        seen = 0
        for path in sorted(glob.glob(os.path.join(board, "*", "devices", "*.json"))):
            with open(path, encoding="utf-8") as f:
                dev = json.load(f)
            if dev.get("family") not in fit.FAMILIES or not dev.get("dtb"):
                continue
            with self.subTest(device=dev["id"]):
                cfg = fit.resolve(dev["family"], None, None, path)
                self.assertEqual(cfg["device"], dev["id"])
                self.assertEqual(cfg["dtb"], os.path.basename(dev["dtb"]))
                self.assertIn('data = /incbin/("%s");' % cfg["dtb"], fit.write_its(cfg))
                seen += 1
        self.assertGreaterEqual(seen, 2)
        cfg = fit.resolve("rk3566", "other", "o.dtb", DEVICES[0])
        self.assertEqual((cfg["device"], cfg["dtb"]), ("other", "o.dtb"))

    def test_resolve_refusals(self):
        with self.assertRaisesRegex(fit.FitError, "-kernel"):
            fit.resolve("qemu-aarch64-virt", "x", "x.dtb", None)
        with self.assertRaisesRegex(fit.FitError, "unknown family"):
            fit.resolve("h700", "x", "x.dtb", None)
        with self.assertRaisesRegex(fit.FitError, "file name ending .dtb"):
            fit.resolve("rk3566", "x", "dts/x.dtb", None)
        with self.assertRaisesRegex(fit.FitError, "need a device"):
            fit.resolve("rk3566", None, None, None)
        d = tempfile.mkdtemp(prefix="fit")
        try:
            p = os.path.join(d, "dev.json")
            with open(p, "w", encoding="utf-8") as f:
                json.dump({"id": "x", "family": "h700", "dtb": "rockchip/x.dtb"}, f)
            with self.assertRaisesRegex(fit.FitError, "family"):
                fit.resolve("rk3566", None, None, p)
            for bad, text in (({"family": "rk3566", "dtb": "rockchip/x.dtb"}, '"id"'),
                              ({"device": "x", "family": "rk3566", "dtb": "rockchip/x.dtb"}, '"id"'),
                              ({"id": "x", "family": "rk3566", "dtb": None}, "no FIT"),
                              ({"id": "x", "family": "rk3566", "dtb": "x.dtb"}, "<vendor>/<name>.dtb"),
                              ({"id": "x", "family": "rk3566", "dtb": "a/b/x.dtb"}, "<vendor>/<name>.dtb")):
                with open(p, "w", encoding="utf-8") as f:
                    json.dump(bad, f)
                with self.subTest(json=bad), self.assertRaisesRegex(fit.FitError, text):
                    fit.resolve("rk3566", None, None, p)
            rc, _, err = run(["its", "--family", "qemu-aarch64-virt", "--device", "x",
                              "--dtb", "x.dtb"])
            self.assertEqual(rc, 2)
            self.assertIn("no FIT", err)
        finally:
            shutil.rmtree(d)


class HeaderTest(unittest.TestCase):
    def test_layout(self):
        h = fit.make_header(1000, b"THIMSFIT")
        self.assertEqual(len(h), 512)
        self.assertEqual(h[:8], b"THIMSFIT")
        self.assertEqual(struct.unpack_from("<I", h, 8)[0], 2)  # 1000 bytes -> 2 sectors
        self.assertEqual(h[12:], bytes(500))
        self.assertEqual(fit.parse_header(h), (b"THIMSFIT", 2))
        self.assertEqual(struct.unpack_from("<I", fit.make_header(512, b"THIMSFIT"), 8)[0], 1)

    def test_flipbase_magic_words(self):
        # FlipBase's script compares *0xc000000 with 3533594d and *0xc000004
        # with 54494635: the bytes "MY355FIT", read as little-endian u32s.
        self.assertEqual(fit.magic_words(b"MY355FIT"), ["3533594d", "54494635"])
        self.assertEqual(fit.magic_words(b"THIMSFIT"), ["4d494854", "54494653"])

    def test_bad_magic_length(self):
        with self.assertRaises(fit.FitError):
            fit.make_header(10, b"SHORT")

    def test_wrap_pads_to_sectors(self):
        blob = fit.wrap(b"x" * 700, b"THIMSFIT")
        self.assertEqual(len(blob), 512 + 1024)
        self.assertEqual(blob[512:1212], b"x" * 700)


class ReadBackTest(unittest.TestCase):
    def test_good_fit(self):
        res = fit.verify_fit(fake_fit(), expect_kernel=KERNEL, expect_dtb=DTB)
        self.assertEqual(res["problems"], [])
        self.assertEqual(res["kernel"]["compression"], "zstd")
        self.assertEqual(res["kernel"]["load"], 0x02000000)
        self.assertEqual(res["bootargs"], "init=/init ro")

    def test_external_data(self):
        res = fit.verify_fit(fake_fit(external=True), expect_kernel=KERNEL, expect_dtb=DTB)
        self.assertEqual(res["problems"], [])

    def test_hash_node_refused(self):
        res = fit.verify_fit(fake_fit(hash_node=True))
        self.assertTrue(any("hash" in p for p in res["problems"]))

    def test_gzip_refused(self):
        res = fit.verify_fit(fake_fit(compression="gzip"))
        self.assertTrue(any("not zstd" in p for p in res["problems"]))

    def test_missing_bootargs(self):
        res = fit.verify_fit(fake_fit(bootargs=None))
        self.assertTrue(any("bootargs" in p for p in res["problems"]))

    def test_wrong_kernel(self):
        res = fit.verify_fit(fake_fit(), expect_kernel=KERNEL + b"!")
        self.assertTrue(any("kernel data differs" in p for p in res["problems"]))

    def test_not_fdt(self):
        with self.assertRaises(fit.FitError):
            fit.verify_fit(b"\0" * 64)

    def test_check_boot_img(self):
        d = tempfile.mkdtemp(prefix="fit")
        try:
            img = os.path.join(d, "boot.img")
            with open(img, "wb") as f:
                f.write(fit.wrap(fake_fit(), b"THIMSFIT"))
            rc, out, _ = run(["check", "--family", "rk3566", "--json", img])
            self.assertEqual(rc, 0, out)
            data = json.loads(out)
            self.assertEqual(data["header"]["magic"], "THIMSFIT")
            self.assertEqual(data["fdt"]["size"], len(DTB))

            with open(img, "wb") as f:
                f.write(fit.wrap(fake_fit(), b"MY355FIT"))
            rc, out, _ = run(["check", "--family", "rk3566", img])
            self.assertEqual(rc, 1)
            self.assertIn("header magic", out)

            blob = bytearray(fit.wrap(fake_fit(), b"THIMSFIT"))
            blob[8:12] = struct.pack("<I", 1)   # too few sectors
            with open(img, "wb") as f:
                f.write(blob)
            rc, _, _ = run(["check", "--family", "rk3566", img])
            self.assertEqual(rc, 1)

            raw = os.path.join(d, "boot.itb")
            with open(raw, "wb") as f:
                f.write(fake_fit())
            rc, _, _ = run(["wrap", "--family", "rk3566", raw, img])
            self.assertEqual(rc, 0)
            rc, _, _ = run(["check", "--family", "rk3566", img])
            self.assertEqual(rc, 0)
        finally:
            shutil.rmtree(d)


class MapTest(unittest.TestCase):
    cfg = fit.FAMILIES["rk3566"]

    def test_fits(self):
        self.assertEqual(fit.check_map(self.cfg, fit_len=20 * 1024 * 1024,
                                       kernel_size=60 * 1024 * 1024), [])

    def test_fit_over_header(self):
        p = fit.check_map(self.cfg, fit_len=33 * 1024 * 1024, kernel_size=None)
        self.assertTrue(any("before the header" in x for x in p), p)

    def test_kernel_over_fit(self):
        p = fit.check_map(self.cfg, fit_len=1024, kernel_size=129 * 1024 * 1024)
        self.assertTrue(any("decompresses" in x for x in p), p)

    def test_sizes_from_headers(self):
        self.assertEqual(fit.zstd_content_size(KERNEL), 40 * 1024 * 1024)
        two = ZSTD + bytes((0x60,)) + struct.pack("<H", 100)   # 2-byte FCS: +256
        self.assertEqual(fit.zstd_content_size(two), 356)
        self.assertIsNone(fit.zstd_content_size(ZSTD + bytes((0x00, 0x58))))
        image = bytearray(64)
        image[16:24] = struct.pack("<Q", 30 * 1024 * 1024)
        image[56:60] = b"ARMd"
        self.assertEqual(fit.arm64_image_size(bytes(image)), 30 * 1024 * 1024)
        self.assertIsNone(fit.arm64_image_size(bytes(64)))


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fitbuild")
        self.kernel = os.path.join(self.tmp, "Image.zst.in")
        self.dtb = os.path.join(self.tmp, "in.dtb")
        with open(self.kernel, "wb") as f:
            f.write(KERNEL)
        with open(self.dtb, "wb") as f:
            f.write(DTB)
        self.out = os.path.join(self.tmp, "out")
        self.dev = DEVICES[0]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def build(self, *extra, kernel=None):
        return run(["build", "--family", "rk3566", "--device-json", self.dev,
                    "--kernel", kernel or self.kernel, "--dtb-file", self.dtb,
                    "--out-dir", self.out] + list(extra))

    def test_its_only_without_mkimage(self):
        rc, _, err = self.build("--mkimage", "none", "--zstd", "none", "--its-only")
        self.assertEqual(rc, 0, err)
        with open(os.path.join(self.out, "boot.its"), encoding="utf-8") as f:
            its = f.read()
        with open(os.path.join(FIX, "rk3566-anbernic-rg-arc-d.its"), encoding="utf-8") as f:
            self.assertEqual(its, f.read())
        with open(os.path.join(self.out, "Image.zst"), "rb") as f:
            self.assertEqual(f.read(), KERNEL)
        self.assertTrue(os.path.isfile(os.path.join(self.out, "rk3566-anbernic-rg-arc-d.dtb")))

    def test_missing_mkimage_is_exit_3(self):
        rc, _, err = self.build("--mkimage", "none", "--zstd", "none")
        self.assertEqual(rc, 3)
        self.assertIn("mkimage", err)

    def test_raw_kernel_without_zstd_is_exit_3(self):
        raw = os.path.join(self.tmp, "Image")
        with open(raw, "wb") as f:
            f.write(b"\0" * 128)
        rc, _, err = self.build("--mkimage", "none", "--zstd", "none", "--its-only", kernel=raw)
        self.assertEqual(rc, 3)
        self.assertIn("zstd", err)

    @unittest.skipUnless(shutil.which("mkimage") and shutil.which("dtc"),
                         "mkimage and dtc are not on PATH")
    def test_mkimage_build_reads_back(self):
        rc, out, err = self.build("--zstd", "none")
        self.assertEqual(rc, 0, out + err)
        with open(os.path.join(self.out, "boot.img"), "rb") as f:
            res = fit.check_boot_img(f.read(), "rk3566")
        self.assertEqual(res["problems"], [])
        self.assertEqual(res["fdt"]["size"], len(DTB))
        self.assertEqual(res["kernel"]["size"], len(KERNEL))


if __name__ == "__main__":
    unittest.main()
