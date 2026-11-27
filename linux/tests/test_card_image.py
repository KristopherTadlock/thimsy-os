# SPDX-License-Identifier: MIT
"""linux/tools/card_image.py: info's FAT (OA-2) and the pinned genimage.cfg (OA-9).

The FAT12 reader below is separate from card_image's writer: it follows the
FAT specification's boot sector, FAT and directory layout, so a field the
writer puts in the wrong place shows here. Nothing is built or run but
Python.

    py -3.12 -I -m unittest discover -s tests -t . -p test_card_image.py   (from linux/)
"""
from __future__ import annotations

import io
import os
import shutil
import struct
import sys
import tempfile
import time
import unittest
import uuid
from contextlib import redirect_stderr
from typing import Any, Dict

HERE = os.path.dirname(os.path.abspath(__file__))
LINUX = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(LINUX, "tools"))

import card_image  # noqa: E402
import gpt_check  # noqa: E402

BOARDS = {fam: os.path.join(LINUX, "board", fam, "genimage.cfg")
          for fam in ("rk3566", "qemu-aarch64-virt")}
RELEASE = "1.2.0"
DEVICE = "anbernic-rg-arc-d"
EPOCH = 1700000000            # 2023-11-14T22:13:20Z
MIB = 1024 * 1024


def read_fat12(blob: bytes) -> Dict[str, Any]:
    """The boot sector's fields, the volume label and the root directory's files."""
    (bps, spc, rsvd, nfats, root_ents, tot16, media, fatsz, _spt, _heads, hidden,
     tot32) = struct.unpack_from("<HBHBHHBHHHII", blob, 11)
    drv, _r, sig, serial, label, fstype = struct.unpack_from("<BBBI11s8s", blob, 36)
    assert blob[510:512] == b"\x55\xaa", "no boot signature"
    total = tot16 or tot32
    root_secs = (root_ents * 32 + bps - 1) // bps
    first_data = rsvd + nfats * fatsz + root_secs
    clusters = (total - first_data) // spc
    fat = blob[rsvd * bps:(rsvd + fatsz) * bps]
    fats = [blob[(rsvd + k * fatsz) * bps:(rsvd + (k + 1) * fatsz) * bps] for k in range(nfats)]

    def entry(n: int) -> int:
        v = struct.unpack_from("<H", fat, n * 3 // 2)[0]
        return v >> 4 if n & 1 else v & 0xFFF

    root = blob[(rsvd + nfats * fatsz) * bps:first_data * bps]
    files: Dict[str, bytes] = {}
    dir_label = None
    times = {}
    for off in range(0, len(root), 32):
        e = root[off:off + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        attr = e[11]
        if attr & 0x08:
            dir_label = e[:11].decode("ascii").rstrip()
            continue
        name = e[:8].decode("ascii").rstrip()
        ext = e[8:11].decode("ascii").rstrip()
        full = name + ("." + ext if ext else "")
        wtime, wdate, clus, size = struct.unpack_from("<HHHI", e, 22)
        times[full] = (wdate, wtime)
        data = bytearray()
        seen = set()
        c = clus
        while 2 <= c < 0xFF8:
            assert c not in seen and c < clusters + 2, "bad chain at %d" % c
            seen.add(c)
            at = (first_data + (c - 2) * spc) * bps
            data += blob[at:at + spc * bps]
            c = entry(c)
        assert len(data) >= size
        files[full] = bytes(data[:size])
    used = sum(1 for n in range(2, clusters + 2) if entry(n))
    return {"bps": bps, "spc": spc, "reserved": rsvd, "fats": nfats, "root_entries": root_ents,
            "total": total, "media": media, "fat_sectors": fatsz, "hidden": hidden,
            "drive": drv, "sig": sig, "serial": serial,
            "label": label.decode("ascii").rstrip(), "fstype": fstype.decode("ascii"),
            "dir_label": dir_label, "files": files, "times": times, "clusters": clusters,
            "fat0": entry(0), "fat1": entry(1), "used": used, "fats_equal": len(set(fats)) == 1,
            "jump": blob[:3]}


def fat_date(epoch: int):
    t = time.gmtime(epoch)
    return (((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday,
            (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2))


class InfoFatTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="cardimage")
        self.out = os.path.join(self.tmp, "info.vfat")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make(self, *extra: str, fam: str = "rk3566", device: str = DEVICE) -> int:
        return card_image.main(["info", "--cfg", BOARDS[fam], "--release", RELEASE,
                                "--device", device, "--model", "Anbernic RG ARC-D",
                                "--epoch", str(EPOCH), "-o", self.out] + list(extra))

    def read(self) -> bytes:
        with open(self.out, "rb") as f:
            return f.read()

    def test_fat12_with_the_readme(self) -> None:
        self.assertEqual(self.make(), 0)
        blob = self.read()
        self.assertEqual(len(blob), MIB)   # genimage.cfg's info partition
        fat = read_fat12(blob)
        self.assertEqual((fat["bps"], fat["spc"], fat["fats"], fat["media"]), (512, 1, 2, 0xF8))
        self.assertEqual(fat["total"], MIB // 512)
        self.assertLess(fat["clusters"], 4085)           # FAT12 by the specification's rule
        self.assertEqual(fat["fstype"], "FAT12   ")
        self.assertEqual((fat["label"], fat["dir_label"]), ("THIMSY-INFO", "THIMSY-INFO"))
        self.assertEqual(fat["sig"], 0x29)
        self.assertEqual(fat["hidden"], 2408 * MIB // 512)   # info's offset in sectors
        self.assertEqual((fat["fat0"], fat["fat1"]), (0xFF8, 0xFFF))
        self.assertTrue(fat["fats_equal"])
        self.assertIn(fat["jump"][0], (0xEB, 0xE9))
        self.assertEqual(sorted(fat["files"]), ["README.TXT"])
        readme = fat["files"]["README.TXT"]
        self.assertEqual(fat["used"], (len(readme) + 511) // 512)
        text = readme.decode("utf-8")
        self.assertTrue(text.startswith("Thimsy OS Linux 1.2.0 for the Anbernic RG ARC-D\r\n"))
        self.assertIn("Device id: anbernic-rg-arc-d", text)
        self.assertIn("THIMSY", text)
        self.assertNotIn("@", text)
        self.assertNotIn("\n", text.replace("\r\n", ""))   # CRLF only, for Notepad
        self.assertEqual(fat["times"]["README.TXT"], fat_date(EPOCH))

    def test_serial_is_name_based_and_nonzero(self) -> None:
        self.assertEqual(self.make(), 0)
        serial = read_fat12(self.read())["serial"]
        g = uuid.UUID(gpt_check.pinned_guid(RELEASE, DEVICE, "fat:info"))
        self.assertEqual(serial, struct.unpack("<I", g.bytes[:4])[0])
        self.assertNotEqual(serial, 0)
        self.assertEqual(card_image.volume_serial(RELEASE, DEVICE), serial)
        self.assertNotEqual(card_image.volume_serial(RELEASE, "anbernic-rg-arc-s"), serial)

    def test_same_arguments_same_bytes(self) -> None:
        self.assertEqual(self.make(), 0)
        first = self.read()
        self.assertEqual(self.make(), 0)
        self.assertEqual(self.read(), first)
        self.assertEqual(self.make(device="anbernic-rg-arc-s"), 0)
        self.assertNotEqual(self.read(), first)

    def test_qemu_board(self) -> None:
        self.assertEqual(self.make(fam="qemu-aarch64-virt", device="qemu-virt"), 0)
        fat = read_fat12(self.read())
        self.assertEqual(fat["label"], "THIMSY-INFO")
        self.assertIn("Device id: qemu-virt", fat["files"]["README.TXT"].decode())

    def test_gpt_check_sees_fat(self) -> None:
        self.assertEqual(self.make(), 0)
        self.assertEqual(gpt_check.sniff_filesystem(self.read()[:4096]), "vfat")

    def test_epoch_clamped_to_fat(self) -> None:
        self.assertEqual(card_image.fat_datetime(0), (1 << 5 | 1, 0))   # 1980-01-01 00:00
        date, _ = card_image.fat_datetime(2 ** 40)
        self.assertEqual(date >> 9, 2107 - 1980)

    def test_refusals(self) -> None:
        big = os.path.join(self.tmp, "big.txt")
        with open(big, "w", encoding="utf-8") as f:
            f.write("x" * MIB)
        odd = os.path.join(self.tmp, "odd.txt")
        with open(odd, "w", encoding="utf-8") as f:
            f.write("@SERIAL@\n")
        for readme, needle in ((big, "does not fit"), (odd, "unknown placeholder")):
            with self.subTest(readme=readme):
                err = io.StringIO()
                with redirect_stderr(err):
                    self.assertEqual(self.make("--readme", readme), 1)
                self.assertIn(needle, err.getvalue())
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
            self.make(device="Bad Id")
        self.assertEqual(cm.exception.code, 2)

    def test_readme_template(self) -> None:
        with open(card_image.DEFAULT_README, encoding="utf-8") as f:
            text = f.read()
        for key in ("@MODEL@", "@VERSION@", "@DEVICE@"):
            self.assertIn(key, text)
        text.encode("ascii")   # plain ASCII: every PC and phone shows it the same
        for line in text.splitlines():
            self.assertLessEqual(len(line), 76, line)


class PinCfgTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="cardimage")
        self.out = os.path.join(self.tmp, "genimage.cfg")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def board(self, fam: str = "rk3566") -> str:
        with open(BOARDS[fam], encoding="utf-8") as f:
            return f.read()

    def test_every_table_entry_and_the_disk_pinned(self) -> None:
        for fam in BOARDS:
            with self.subTest(family=fam):
                text = card_image.pin_cfg(self.board(fam), RELEASE, DEVICE)
                images, problems = gpt_check.genimage_layout(text)
                self.assertEqual(problems, [])
                im = images[0]
                self.assertEqual(im["hdimage"]["disk-uuid"],
                                 gpt_check.pinned_guid(RELEASE, DEVICE, "disk"))
                for e in im["partitions"]:
                    if e["in_table"]:
                        self.assertEqual(e["uuid"], gpt_check.pinned_guid(
                            RELEASE, DEVICE, "part:" + e["name"]))
                    else:
                        self.assertEqual(e["uuid"], "")   # the raw idbloader has none
                # Only the GUID lines are added.
                added = [ln for ln in text.split("\n") if ln not in self.board(fam).split("\n")]
                self.assertEqual(len(added), 1 + len([e for e in im["partitions"]
                                                      if e["in_table"]]))
                self.assertTrue(all("-uuid = " in ln for ln in added), added)
                with open(self.out, "w", encoding="utf-8") as f:
                    f.write(text)
                rep = gpt_check.check_genimage(self.out, family=fam, release=RELEASE,
                                               device=DEVICE)
                self.assertEqual(rep.errors, [])

    def test_cli_writes_a_checked_cfg(self) -> None:
        rc = card_image.main(["cfg", BOARDS["rk3566"], "--family", "rk3566", "--release",
                              RELEASE, "--device", DEVICE, "-o", self.out])
        self.assertEqual(rc, 0)
        rep = gpt_check.check_genimage(self.out, family="rk3566", release=RELEASE,
                                       device=DEVICE)
        self.assertEqual(rep.errors, [])
        other = gpt_check.check_genimage(self.out, family="rk3566", release="1.2.1",
                                         device=DEVICE)
        self.assertFalse(other.ok)
        self.assertTrue(any("(OA-9)" in e for e in other.errors), other.errors)
        board = gpt_check.check_genimage(self.out, family="rk3566")
        self.assertTrue(any("set by hand" in e for e in board.errors), board.errors)
        self.assertFalse(os.path.exists(self.out + ".tmp"))

    def test_same_arguments_same_text(self) -> None:
        a = card_image.pin_cfg(self.board(), RELEASE, DEVICE)
        self.assertEqual(a, card_image.pin_cfg(self.board(), RELEASE, DEVICE))
        self.assertNotEqual(a, card_image.pin_cfg(self.board(), "1.2.1", DEVICE))
        self.assertNotEqual(a, card_image.pin_cfg(self.board(), RELEASE, "anbernic-rg-arc-s"))

    def test_refuses_a_guid_already_set(self) -> None:
        g = "6C1B8C68-6E51-4F6B-8B4F-0C3D2A1E9F00"
        for old, new in (('partition-table-type = "gpt"',
                          'partition-table-type = "gpt"\n\t\tdisk-uuid = "%s"' % g),
                         ('image = "data.ext4"',
                          'image = "data.ext4"\n\t\tpartition-uuid = "%s"' % g)):
            with self.subTest(new=new):
                with self.assertRaises(card_image.Refused):
                    card_image.pin_cfg(self.board().replace(old, new, 1), RELEASE, DEVICE)
        path = os.path.join(self.tmp, "hand.cfg")
        with open(path, "w", encoding="utf-8") as f:
            f.write(self.board().replace('partition-table-type = "gpt"',
                                         'partition-table-type = "gpt"\n\t\tdisk-uuid = "%s"'
                                         % g, 1))
        err = io.StringIO()
        with redirect_stderr(err):
            rc = card_image.main(["cfg", path, "--family", "rk3566", "--release", RELEASE,
                                  "--device", DEVICE, "-o", self.out])
        self.assertEqual(rc, 1)
        self.assertIn("by hand", err.getvalue())
        self.assertFalse(os.path.exists(self.out))

    def test_refuses_a_bad_release(self) -> None:
        for bad in ("1.0 beta", "1:0", ""):
            with self.subTest(release=bad):
                with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
                    card_image.main(["cfg", BOARDS["rk3566"], "--family", "rk3566",
                                     "--release", bad, "--device", DEVICE, "-o", self.out])
                self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
