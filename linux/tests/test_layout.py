"""Each board's genimage.cfg, read statically against E.6 (D.4 test_layout).

Names, type GUIDs, order, explicit 1 MiB-aligned offsets and sizes, the
regions tiled from LBA 16384 with their spares, the raw idbloader ending
before the first partition, and no entry of a type vold mounts (the games
entry is made at first boot, I39).
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
LINUX = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LINUX, "tools"))

import gpt_check  # noqa: E402

MIB = 1024 * 1024
BOARDS = {
    "rk3566": os.path.join(LINUX, "board", "rk3566", "genimage.cfg"),
    "qemu-aarch64-virt": os.path.join(LINUX, "board", "qemu-aarch64-virt", "genimage.cfg"),
}
# E.6's table: name, type GUID, offset, size.
E6 = [
    ("uboot", "8DA63339-0007-60C0-C436-083AC8230908", 8 * MIB, 8 * MIB),
    ("boot", "8DA63339-0007-60C0-C436-083AC8230908", 24 * MIB, 40 * MIB),
    ("root", "B921B045-1DF0-41C3-AF44-4C6F280D3FAE", 104 * MIB, 1024 * MIB),
    ("data", "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", 2152 * MIB, 256 * MIB),
]
VOLD_TYPES = {"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "0FC63DAF-8483-4772-8E79-3D69D8477DE4"}


def load(family):
    with open(BOARDS[family], encoding="utf-8") as f:
        text = f.read()
    images, problems = gpt_check.genimage_layout(text)
    return text, images, problems


class LayoutTest(unittest.TestCase):
    def test_boards_exist(self):
        for fam, path in BOARDS.items():
            self.assertTrue(os.path.isfile(path), path)
            self.assertIn(fam, gpt_check.FAMILIES)

    def test_e6_table(self):
        for fam in BOARDS:
            with self.subTest(family=fam):
                _, images, problems = load(fam)
                self.assertEqual(problems, [])
                self.assertEqual(len(images), 1)
                im = images[0]
                self.assertEqual(im["hdimage"].get("partition-table-type"), "gpt")
                table = [p for p in im["partitions"] if p["in_table"]]
                got = [(p["name"], p["type"], p["offset"], p["size"]) for p in table]
                self.assertEqual(got, E6)

    def test_alignment_and_spares(self):
        for fam in BOARDS:
            _, images, _ = load(fam)
            table = [p for p in images[0]["partitions"] if p["in_table"]]
            for p in table:
                self.assertEqual(p["offset"] % MIB, 0, p["name"])
                self.assertEqual(p["size"] % MIB, 0, p["name"])
            self.assertEqual(table[0]["offset"], 16384 * 512)
            for a, b in zip(table, table[1:]):
                self.assertEqual(b["offset"], a["offset"] + 2 * a["size"],
                                 "%s must follow %s and its spare" % (b["name"], a["name"]))

    def test_no_vold_type_in_written_image(self):
        for fam in BOARDS:
            text, images, _ = load(fam)
            for p in images[0]["partitions"]:
                self.assertNotIn(p["type"], VOLD_TYPES, p["name"])
            self.assertNotIn("games", [p["name"] for p in images[0]["partitions"]])
            body = " ".join(ln for ln in text.splitlines() if not ln.lstrip().startswith("#"))
            self.assertNotIn("EBD0A0A2", body.upper())

    def test_rk3566_idbloader(self):
        _, images, _ = load("rk3566")
        raw = [p for p in images[0]["partitions"] if not p["in_table"]]
        self.assertEqual([p["name"] for p in raw], ["idbloader"])
        idb = raw[0]
        self.assertEqual(idb["offset"], 64 * 512)
        self.assertEqual(idb["image"], "idbloader.img")
        first = min(p["offset"] for p in images[0]["partitions"] if p["in_table"])
        # genimage refuses an idbloader.img larger than its size, so the
        # declared size bounds it: it must end before the first partition.
        self.assertLessEqual(idb["offset"] + idb["size"], first)
        # and it must start after the primary GPT (128 entries end at LBA 33).
        self.assertGreater(idb["offset"], 33 * 512)

    def test_qemu_has_no_raw_region(self):
        _, images, _ = load("qemu-aarch64-virt")
        self.assertEqual([p for p in images[0]["partitions"] if not p["in_table"]], [])

    def test_images_named(self):
        _, images, _ = load("rk3566")
        table = {p["name"]: p["image"] for p in images[0]["partitions"]}
        self.assertEqual(table, {"idbloader": "idbloader.img", "uboot": "u-boot.itb",
                                 "boot": "boot.img", "root": "rootfs.erofs",
                                 "data": "data.ext4"})

    def test_gpt_check_passes_each_board(self):
        for fam, path in BOARDS.items():
            with self.subTest(family=fam):
                rep = gpt_check.check_genimage(path, family=fam)
                self.assertEqual(rep.errors, [])
                self.assertEqual(gpt_check.family_from_path(path), fam)

    # -- the checker refuses broken files

    def mutated(self, family, old, new):
        text, _, _ = load(family)
        self.assertIn(old, text)
        d = tempfile.mkdtemp(prefix="layout")
        path = os.path.join(d, "genimage.cfg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))
        try:
            return gpt_check.check_genimage(path, family=family)
        finally:
            os.unlink(path)
            os.rmdir(d)

    def test_refuses_games_entry(self):
        games = ('\tpartition games {\n'
                 '\t\tpartition-type-uuid = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"\n'
                 '\t\toffset = 2408M\n\t\tsize = 64M\n\t}\n}\n')
        text, _, _ = load("rk3566")
        end = text.rindex("}")
        rep = self.mutated("rk3566", text, text[:end] + games)
        self.assertFalse(rep.ok)
        self.assertTrue(any("vold mounts" in e for e in rep.errors), rep.errors)

    def test_refuses_linux_filesystem_type(self):
        rep = self.mutated("qemu-aarch64-virt", "933AC7E1-2EB4-4F13-B844-0E14E2AEF915",
                           "0FC63DAF-8483-4772-8E79-3D69D8477DE4")
        self.assertTrue(any("vold mounts" in e for e in rep.errors), rep.errors)

    def test_refuses_idbloader_overrun(self):
        rep = self.mutated("rk3566", "size = 8160K", "size = 8161K")
        self.assertTrue(any("runs into entry 1" in e for e in rep.errors), rep.errors)

    def test_refuses_implicit_offset(self):
        rep = self.mutated("rk3566", "offset = 104M\n", "")
        self.assertTrue(any("no explicit offset" in e for e in rep.errors), rep.errors)

    def test_refuses_misalignment(self):
        rep = self.mutated("qemu-aarch64-virt", "offset = 2152M", "offset = 2152516K")
        self.assertTrue(any("1 MiB boundary" in e for e in rep.errors), rep.errors)

    def test_refuses_reorder(self):
        rep = self.mutated("qemu-aarch64-virt", "partition uboot", "partition loader")
        self.assertTrue(any("named 'loader'" in e for e in rep.errors), rep.errors)

    def test_parser(self):
        items = gpt_check.parse_genimage(
            '# c\nimage a.img { /* b */ hdimage { x = "y z" } // d\n'
            ' partition p { size = 1M; offset = 0x100000 } }\n')
        self.assertEqual(items[0][0:2], ("image", "a.img"))
        body = items[0][2]
        self.assertEqual(body[0], ("hdimage", None, [("x", None, "y z")]))
        self.assertEqual(gpt_check.parse_size("0x100000"), 1024 * 1024)
        self.assertEqual(gpt_check.parse_size("8160K"), 8160 * 1024)
        self.assertEqual(gpt_check.parse_size("1G"), 1024 * MIB)
        with self.assertRaises(ValueError):
            gpt_check.parse_genimage("image a { x = }")


if __name__ == "__main__":
    unittest.main()
