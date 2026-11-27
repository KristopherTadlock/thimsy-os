"""linux/tools/gpt_check.py against small GPT images written here (D.4).

The writer below is deliberately separate from gpt_check's reader: it builds
images byte by byte from the UEFI layout, good ones and broken ones.
"""
from __future__ import annotations

import binascii
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))

import gpt_check  # noqa: E402

SS = 512
RESERVED = "8DA63339-0007-60C0-C436-083AC8230908"
ROOT = "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"
HOME = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915"
BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
LINUXFS = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"

MIB_S = 2048  # sectors per MiB
# The E.6 layout, shrunk: 1 MiB regions tiled from LBA 16384 with equal spares,
# then info (OA-2), 1 MiB right behind data.
DATA_END = 16384 + 7 * MIB_S          # first LBA after data
INFO = ("info", BASIC, DATA_END, MIB_S)
GOOD = [
    ("uboot", RESERVED, 16384, MIB_S),
    ("boot", RESERVED, 16384 + 2 * MIB_S, MIB_S),
    ("root", ROOT, 16384 + 4 * MIB_S, MIB_S),
    ("data", HOME, 16384 + 6 * MIB_S, MIB_S),
    INFO,
]
IMAGE_END = DATA_END + MIB_S          # first LBA after info, the image's last entry
TOTAL = IMAGE_END + MIB_S             # a written image: a 1 MiB tail ending in the backup GPT
GAMES = IMAGE_END + 2 * MIB_S         # where D60firstboot puts games (device-safety-2)
CARD = GAMES + 4 * MIB_S              # a card that has booted
RETIRED_INFO = ("info", RESERVED, DATA_END, MIB_S)   # D60firstboot's step 4
DISK = "12345678-1234-4678-9abc-def012345678"        # version 4, as on a booted card


def crc(b: bytes) -> int:
    return binascii.crc32(b) & 0xFFFFFFFF


def v4(seed: str) -> str:
    """A fixed version 4 GUID: random-looking, as the kernel or the installer writes."""
    return str(uuid.UUID(bytes=hashlib.md5(seed.encode()).digest(), version=4))


def entry(name: str, tguid: str, first: int, nsect: int, attrs: int = 0,
          guid: str = "") -> bytes:
    e = uuid.UUID(tguid).bytes_le
    e += uuid.UUID(guid or v4(name + str(first))).bytes_le
    e += struct.pack("<QQQ", first, first + nsect - 1, attrs)
    e += name.encode("utf-16-le").ljust(72, b"\0")
    return e


def header(my_lba: int, alt_lba: int, entries_lba: int, first_usable: int,
           last_usable: int, arr_crc: int, n_entries: int = 128, disk: str = DISK) -> bytes:
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0,
                    my_lba, alt_lba, first_usable, last_usable, uuid.UUID(disk).bytes_le,
                    entries_lba, n_entries, 128, arr_crc)
    h = h[:16] + struct.pack("<I", crc(h)) + h[20:]
    return h.ljust(SS, b"\0")


def fat12_boot_sector() -> bytes:
    b = bytearray(SS)
    b[0:3] = b"\xeb\x3c\x90"
    b[3:11] = b"MSWIN4.1"
    b[54:62] = b"FAT12   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def exfat_boot_sector() -> bytes:
    b = bytearray(SS)
    b[0:3] = b"\xeb\x76\x90"
    b[3:11] = b"EXFAT   "
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def write_image(path: str, parts=None, total: int = TOTAL, *, slots=None,
                idbloader: bytes = b"RKNS" + b"\x01" * 2000,
                mbr_type: int = 0xEE, fs=None, guids=None, disk: str = DISK) -> None:
    """parts: (name, type, first_lba, sectors); slots: entry index per part; fs: {lba:
    bytes}, by default info's FAT; guids: {name: unique GUID} (else a version 4 one)."""
    parts = GOOD if parts is None else parts
    if fs is None:
        fs = {DATA_END: fat12_boot_sector()}
    guids = guids or {}
    arr = bytearray(128 * 128)
    for i, p in enumerate(parts):
        k = slots[i] if slots else i
        arr[k * 128:(k + 1) * 128] = entry(*p, guid=guids.get(p[0], ""))
    arr = bytes(arr)
    last = total - 1
    first_usable, last_usable = 34, last - 33
    with open(path, "wb") as f:
        f.truncate(total * SS)
        mbr = bytearray(SS)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\x02\0", mbr_type, b"\xff\xff\xff",
                                   1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
        f.write(header(1, last, 2, first_usable, last_usable, crc(arr), disk=disk))
        f.write(arr)
        f.seek((last - 32) * SS)
        f.write(arr)
        f.write(header(last, 1, last - 32, first_usable, last_usable, crc(arr), disk=disk))
        if idbloader:
            f.seek(64 * SS)
            f.write(idbloader)
        for lba, blob in fs.items():
            f.seek(lba * SS)
            f.write(blob)


def patch(path: str, offset: int, data: bytes) -> None:
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(data)


def booted(games_fs: bool = True):
    """A card after its first boot: info retired, games formatted over the rest."""
    last_usable = CARD - 34
    parts = GOOD[:4] + [RETIRED_INFO, ("games", BASIC, GAMES, last_usable - GAMES + 1)]
    fs = {DATA_END: fat12_boot_sector()}
    if games_fs:
        fs[GAMES] = exfat_boot_sector()
    return parts, fs


class GptCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="gptcheck")
        self.img = os.path.join(self.tmp, "card.img")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def check(self, **kw):
        kw.setdefault("family", "rk3566")
        return gpt_check.check_image(self.img, **kw)

    def assertFails(self, rep, *needles):
        self.assertFalse(rep.ok, "expected a failure")
        text = "\n".join(rep.errors)
        for n in needles:
            self.assertIn(n, text)

    # -- good images

    def test_good_rk3566_image(self):
        write_image(self.img)
        rep = self.check()
        self.assertEqual(rep.errors, [])
        self.assertEqual([e["name"] for e in rep.info["entries"]],
                         ["uboot", "boot", "root", "data", "info"])
        self.assertEqual(rep.info["entries"][2]["type"], ROOT)
        self.assertEqual(rep.info["entries"][4]["filesystem"], "vfat")
        self.assertEqual(rep.info["idbloader"]["first_lba"], 64)
        self.assertEqual(rep.info["idbloader"]["magic"], "RKNS")

    def test_good_qemu_image_without_idbloader(self):
        write_image(self.img, idbloader=b"")
        rep = self.check(family="qemu-aarch64-virt")
        self.assertEqual(rep.errors, [])

    def test_guid_is_mixed_endian(self):
        raw = bytes.fromhex("A2A0D0EBE5B9334487C068B6B72699C7")
        self.assertEqual(gpt_check.guid_from_disk(raw), BASIC)
        self.assertEqual(gpt_check.guid_to_disk(BASIC), raw)

    def test_names_are_utf16le(self):
        parts = [("uboot", RESERVED, 16384, MIB_S), ("boït", RESERVED, 16384 + 2 * MIB_S, MIB_S)]
        write_image(self.img, parts)
        rep = self.check(thimsy=False)
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.info["entries"][1]["name"], "boït")

    # -- headers and CRCs

    def test_primary_header_crc(self):
        write_image(self.img)
        patch(self.img, SS + 0x48, struct.pack("<Q", 3))   # entries LBA, CRC not updated
        self.assertFails(self.check(), "primary GPT header: CRC32")

    def test_backup_header_missing(self):
        write_image(self.img)
        patch(self.img, (TOTAL - 1) * SS, b"\0" * 8)
        self.assertFails(self.check(), "backup GPT header", "signature")

    def test_entry_array_crc(self):
        write_image(self.img)
        patch(self.img, 2 * SS + 56, "x".encode("utf-16-le"))   # rename uboot in place
        rep = self.check()
        self.assertFails(rep, "primary entry array: CRC32", "backup entry array differs")

    def test_backup_array_differs(self):
        write_image(self.img)
        patch(self.img, (TOTAL - 33) * SS + 56, "x".encode("utf-16-le"))
        self.assertFails(self.check(), "backup entry array")

    def test_image_truncated(self):
        write_image(self.img)
        with open(self.img, "r+b") as f:
            f.truncate((TOTAL - 1) * SS)
        self.assertFails(self.check(), "backup GPT header")

    def test_protective_mbr(self):
        write_image(self.img, mbr_type=0x83)
        self.assertFails(self.check(), "protective MBR: entry 1 has type 0x83")

    def test_hybrid_mbr(self):
        write_image(self.img)
        patch(self.img, 446 + 16, struct.pack("<B3sB3sII", 0x80, b"\0\0\0", 0x0C, b"\0\0\0",
                                              16384, 2048))
        self.assertFails(self.check(), "hybrid MBR")

    def test_no_mbr_signature(self):
        write_image(self.img)
        patch(self.img, 510, b"\0\0")
        self.assertFails(self.check(), "0x55AA")

    # -- layout

    def test_misaligned_entry(self):
        parts = list(GOOD)
        parts[3] = ("data", HOME, GOOD[3][2] + 8, MIB_S)
        write_image(self.img, parts)
        self.assertFails(self.check(), "not on a 1 MiB boundary")

    def test_overlap(self):
        parts = list(GOOD)
        parts[1] = ("boot", RESERVED, 16384 + MIB_S // 2, MIB_S)
        write_image(self.img, parts)
        self.assertFails(self.check(thimsy=False), "inside or before")

    def test_wrong_name_and_type(self):
        parts = list(GOOD)
        parts[2] = ("rootfs", LINUXFS, GOOD[2][2], MIB_S)
        write_image(self.img, parts)
        self.assertFails(self.check(), "named 'rootfs'", "Linux filesystem data")

    def test_wrong_order(self):
        parts = [GOOD[1], GOOD[0], GOOD[2], GOOD[3], GOOD[4]]
        write_image(self.img, parts)
        self.assertFails(self.check(), "entry 1 is named 'boot'")

    def test_spare_missing(self):
        parts = list(GOOD)
        parts[3] = ("data", HOME, GOOD[2][2] + MIB_S, MIB_S)   # right behind root
        write_image(self.img, parts)
        self.assertFails(self.check(), "equal spare")

    def test_uboot_not_at_16384(self):
        parts = [(n, t, f + MIB_S, s) for n, t, f, s in GOOD]
        write_image(self.img, parts, TOTAL + MIB_S, fs={DATA_END + MIB_S: fat12_boot_sector()})
        self.assertFails(self.check(), "the SPL needs LBA 16384")

    def test_idbloader_missing(self):
        write_image(self.img, idbloader=b"")
        self.assertFails(self.check(), "idbloader at sector 64")

    def test_idbloader_runs_into_uboot(self):
        write_image(self.img)
        rep = self.check(idbloader_size=(16384 - 64 + 1) * SS)
        self.assertFails(rep, "runs into entry 1 ('uboot')")

    def test_idbloader_fits_exactly(self):
        write_image(self.img)
        rep = self.check(idbloader_size=(16384 - 64) * SS)
        self.assertEqual(rep.errors, [])

    def test_idbloader_without_rkns_warns(self):
        write_image(self.img, idbloader=b"\x3b\x8c\xdc\xfc" + b"\x01" * 600)
        rep = self.check()
        self.assertEqual(rep.errors, [])
        self.assertTrue(any("RKNS" in w for w in rep.warnings))

    # -- info (OA-2)

    def test_image_without_info(self):
        """The old layout: no entry vold can mount, so Android offers to set up the card."""
        write_image(self.img, GOOD[:4], DATA_END + MIB_S, fs={})
        self.assertFails(self.check(), "has 4 entries", "no entry 5 'info'",
                         "set up (format)")

    def test_info_unformatted(self):
        write_image(self.img, fs={})
        self.assertFails(self.check(), "entry 5 ('info')", "carries no filesystem")

    def test_info_not_fat(self):
        write_image(self.img, fs={DATA_END: exfat_boot_sector()})
        self.assertFails(self.check(), "carries exfat; it must be FAT")

    def test_info_misplaced_or_sized(self):
        cases = [
            ("gap", [GOOD[i] for i in range(4)] + [("info", BASIC, DATA_END + MIB_S, MIB_S)],
             TOTAL + MIB_S, DATA_END + MIB_S, "right behind data"),
            ("size", GOOD[:4] + [("info", BASIC, DATA_END, 2 * MIB_S)], TOTAL + MIB_S,
             DATA_END, "must be 1 MiB"),
        ]
        for what, parts, total, at, needle in cases:
            with self.subTest(what=what):
                write_image(self.img, parts, total, fs={at: fat12_boot_sector()})
                self.assertFails(self.check(), needle)

    def test_info_in_another_slot(self):
        write_image(self.img, slots=[0, 1, 2, 3, 5])
        self.assertFails(self.check(), "in table slot 6", "entry 5 'info'")

    def test_info_retired_in_a_written_image(self):
        write_image(self.img, GOOD[:4] + [RETIRED_INFO])
        self.assertFails(self.check(), "Linux reserved", "no entry 5 'info'")

    # -- the vold rule

    def test_written_image_with_basic_data(self):
        write_image(self.img, GOOD + [("games", BASIC, GAMES, MIB_S)], CARD)
        self.assertFails(self.check(), "Microsoft basic data, which vold mounts",
                         "has 6 entries")

    def test_written_image_with_formatted_basic_data_still_fails(self):
        write_image(self.img, GOOD + [("games", BASIC, GAMES, MIB_S)], CARD,
                    fs={DATA_END: fat12_boot_sector(), GAMES: exfat_boot_sector()})
        self.assertFails(self.check(), "which vold mounts")

    def test_linux_filesystem_type_is_vold_visible(self):
        parts = list(GOOD)
        parts[3] = ("data", LINUXFS, GOOD[3][2], MIB_S)
        write_image(self.img, parts)
        self.assertFails(self.check(thimsy=False), "Linux filesystem data, which vold mounts")

    def test_android_expand_type(self):
        parts = GOOD + [("x", "193D1EA4-B3CA-11E4-B075-10604B889DCF", GAMES, MIB_S)]
        write_image(self.img, parts, CARD)
        self.assertFails(self.check(thimsy=False), "adopt")

    def test_card_with_formatted_games(self):
        parts, fs = booted()
        write_image(self.img, parts, CARD, fs=fs)
        rep = self.check(mode="card")
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.info["entries"][5]["filesystem"], "exfat")
        self.assertEqual(rep.info["entries"][5]["index"], 6)

    def test_card_before_games_is_made(self):
        """Booted, games not made yet (no room, or a read error): info is the one volume."""
        write_image(self.img, total=CARD)
        self.assertEqual(self.check(mode="card").errors, [])

    def test_card_with_info_and_games_both_mounted(self):
        """D60's step 4 not done: two volumes, which the card's end state never has."""
        parts, fs = booted()
        parts[4] = INFO
        write_image(self.img, parts, CARD, fs=fs)
        self.assertFails(self.check(mode="card"), "exactly one")

    def test_card_with_unformatted_games(self):
        parts, fs = booted(games_fs=False)
        write_image(self.img, parts, CARD, fs=fs)
        self.assertFails(self.check(mode="card"), "carries no filesystem")

    def test_card_with_games_past_entry_7(self):
        parts = GOOD[:4] + [("games", BASIC, GAMES, MIB_S)]
        write_image(self.img, parts, CARD, slots=[0, 1, 2, 3, 7], fs={GAMES: exfat_boot_sector()})
        self.assertFails(self.check(mode="card", thimsy=False), "entries 1 to 7")

    def test_card_with_two_public_volumes(self):
        parts = GOOD[:4] + [("games", BASIC, GAMES, MIB_S),
                            ("more", BASIC, GAMES + MIB_S, MIB_S)]
        write_image(self.img, parts, CARD, fs={GAMES: exfat_boot_sector(),
                                               GAMES + MIB_S: exfat_boot_sector()})
        self.assertFails(self.check(mode="card", thimsy=False), "exactly one")

    # -- the tail (device-safety-2)

    def test_image_without_tail(self):
        """The old layout: the image ended at its last entry's end, so its backup GPT landed
        on the first 33 sectors of games, the old library's boot region."""
        write_image(self.img, total=IMAGE_END + 33)
        self.assertFails(self.check(), "device-safety-2", "1 MiB before games")

    def test_image_tail_too_long(self):
        write_image(self.img, total=TOTAL + MIB_S)
        self.assertFails(self.check(), "device-safety-2")

    def test_backup_gpt_ends_a_mib_before_games(self):
        write_image(self.img)
        rep = self.check()
        self.assertEqual(rep.errors, [])
        backup_end = rep.info["image"]["sectors"]          # first LBA after the backup GPT
        self.assertLessEqual(backup_end + MIB_S, GAMES)
        self.assertGreaterEqual(backup_end - 33, IMAGE_END)  # the backup is the last 33 sectors

    def test_card_games_too_early(self):
        for start in (IMAGE_END, IMAGE_END + MIB_S):
            with self.subTest(start=start):
                last_usable = CARD - 34
                parts = GOOD[:4] + [RETIRED_INFO,
                                    ("games", BASIC, start, last_usable - start + 1)]
                write_image(self.img, parts, CARD,
                            fs={DATA_END: fat12_boot_sector(), start: exfat_boot_sector()})
                self.assertFails(self.check(mode="card"), "device-safety-2")

    def test_card_mode_ignores_the_image_size(self):
        write_image(self.img, total=CARD)
        self.assertEqual(self.check(mode="card").errors, [])

    def test_sniff_ext4(self):
        head = bytearray(4096)
        head[1024 + 56:1024 + 58] = b"\x53\xef"
        self.assertEqual(gpt_check.sniff_filesystem(bytes(head)), "ext4")
        self.assertIsNone(gpt_check.sniff_filesystem(bytes(4096)))

    # -- GUIDs (OA-9)

    def pinned(self, release="1.2.0", device="anbernic-rg-arc-d"):
        want = gpt_check.pinned_guids(release, device, [p[0] for p in GOOD])
        return {"guids": {n: g for n, g in want.items() if n != "disk"}, "disk": want["disk"]}

    def test_pinned_guid_values(self):
        g = gpt_check.pinned_guid("1.2.0", "anbernic-rg-arc-d", "part:root")
        self.assertEqual(g, str(uuid.uuid5(uuid.uuid5(uuid.NAMESPACE_URL, "thimsy:gpt"),
                                           "1.2.0:anbernic-rg-arc-d:part:root")).upper())
        self.assertEqual(uuid.UUID(g).version, 5)
        self.assertEqual(g, gpt_check.pinned_guid("1.2.0", "anbernic-rg-arc-d", "part:root"))
        others = {gpt_check.pinned_guid("1.2.1", "anbernic-rg-arc-d", "part:root"),
                  gpt_check.pinned_guid("1.2.0", "anbernic-rg-arc-s", "part:root"),
                  gpt_check.pinned_guid("1.2.0", "anbernic-rg-arc-d", "part:data"),
                  gpt_check.pinned_guid("1.2.0", "anbernic-rg-arc-d", "disk")}
        self.assertEqual(len(others), 4)
        self.assertNotIn(g, others)
        with self.assertRaises(ValueError):
            gpt_check.pinned_guid("", "x", "disk")

    def test_image_with_its_pinned_guids(self):
        write_image(self.img, **self.pinned())
        rep = self.check(release="1.2.0", device="anbernic-rg-arc-d")
        self.assertEqual(rep.errors, [])
        self.assertEqual(rep.info["gpt"]["disk_guid"], self.pinned()["disk"])

    def test_image_with_another_release_or_devices_guids(self):
        for release, device in (("1.1.0", "anbernic-rg-arc-d"), ("1.2.0", "anbernic-rg-arc-s")):
            with self.subTest(release=release, device=device):
                write_image(self.img, **self.pinned(release, device))
                self.assertFails(self.check(release="1.2.0", device="anbernic-rg-arc-d"),
                                 "the disk GUID is", "entry 3 ('root')'s unique GUID is",
                                 "(OA-9)")

    def test_image_guids_unchecked_without_a_release(self):
        write_image(self.img)
        self.assertEqual(self.check().errors, [])

    def test_repeated_or_zero_guids(self):
        write_image(self.img, guids={"boot": v4("uboot16384")})
        self.assertFails(self.check(), "entry 2 ('boot')'s unique GUID", "repeats")
        write_image(self.img, disk="00000000-0000-0000-0000-000000000000")
        self.assertFails(self.check(), "the disk GUID is zero")

    def test_booted_card_still_pinned(self):
        """A card written with Etcher or dd that has not run D60firstboot's step R."""
        parts, fs = booted()
        p = self.pinned()
        write_image(self.img, parts, CARD, fs=fs, **p)
        self.assertFails(self.check(mode="card"), "the disk GUID is still name-based",
                         "entry 3 ('root')'s unique GUID is still name-based")
        write_image(self.img, parts, CARD, fs=fs)
        self.assertEqual(self.check(mode="card").errors, [])

    # -- an independent writer: util-linux sfdisk, where present (WSL)

    def sfdisk_image(self, extra=""):
        sfdisk = shutil.which("sfdisk") or next(
            (p for p in ("/usr/sbin/sfdisk", "/sbin/sfdisk") if os.path.exists(p)), None)
        if not sfdisk:
            self.skipTest("sfdisk not found")
        with open(self.img, "wb") as f:
            f.truncate(TOTAL * SS)
        script = "label: gpt\n" + "".join(
            "start=%d, size=%d, type=%s, name=%s\n" % (first, n, t, name)
            for name, t, first, n in GOOD) + extra
        subprocess.run([sfdisk, "-q", self.img], input=script.encode(), check=True,
                       stdout=subprocess.DEVNULL)
        patch(self.img, DATA_END * SS, fat12_boot_sector())

    def test_sfdisk_written_image(self):
        self.sfdisk_image()
        rep = self.check(family="qemu-aarch64-virt")
        self.assertEqual(rep.errors, [])
        self.assertEqual([e["name"] for e in rep.info["entries"]],
                         ["uboot", "boot", "root", "data", "info"])

    def test_sfdisk_basic_data(self):
        self.sfdisk_image("start=%d, size=%d, type=%s, name=games\n" % (IMAGE_END, MIB_S // 2,
                                                                         BASIC))
        self.assertFails(self.check(family="qemu-aarch64-virt"), "which vold mounts")

    # -- the command line

    def run_cli(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = gpt_check.main(list(args))
        return rc, out.getvalue()

    def test_cli_exit_codes_and_json(self):
        write_image(self.img)
        rc, out = self.run_cli(self.img, "--family", "rk3566", "--json")
        self.assertEqual(rc, 0)
        data = json.loads(out)
        self.assertTrue(data["ok"])
        self.assertEqual(data["family"], "rk3566")
        self.assertEqual(len(data["entries"]), 5)
        self.assertEqual(data["gpt"]["entries"], 128)

        write_image(self.img, GOOD + [("games", BASIC, GAMES, MIB_S)], CARD)
        rc, out = self.run_cli(self.img, "--family", "rk3566")
        self.assertEqual(rc, 1)
        self.assertIn("FAIL:", out)

        write_image(self.img, **self.pinned())
        rc, _ = self.run_cli(self.img, "--family", "rk3566", "--release", "1.2.0",
                             "--device", "anbernic-rg-arc-d")
        self.assertEqual(rc, 0)
        rc, _ = self.run_cli(self.img, "--family", "rk3566", "--release", "1.2.1",
                             "--device", "anbernic-rg-arc-d")
        self.assertEqual(rc, 1)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
            self.run_cli(self.img, "--release", "1.2.0")
        self.assertEqual(cm.exception.code, 2)

        missing = os.path.join(self.tmp, "missing.img")
        with redirect_stdout(io.StringIO()), \
                open(os.devnull, "w") as null:
            old = sys.stderr
            sys.stderr = null
            try:
                rc = gpt_check.main([missing])
            finally:
                sys.stderr = old
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
