#!/usr/bin/env python3
"""Check a Thimsy card image's GPT, or a genimage.cfg, against the card layout.

An independent GPT reader (it shares no code with genimage or sfdisk), run by
post-image.d/40-gpt-check.sh on every image genimage writes, and by
linux/tests/test_layout.py on every board's genimage.cfg. Design: E.6 of
docs/linux/IMPLEMENTATION.md, decisions I7 and I39, plan 8.2 and risk 13.

What it checks in an image:
  - the protective MBR (signature, one 0xEE entry from LBA 1 over the disk);
  - the primary and backup GPT headers (signature, revision, size, CRC32,
    self and alternate LBAs, usable range) and that they agree;
  - the entry arrays' CRC32, and that the backup array equals the primary;
  - each entry: type GUID (mixed-endian on disk), UTF-16LE name, range inside
    the usable area, no overlaps, 1 MiB alignment;
  - the raw idbloader region (RK3566: sector 64) after the primary entry array
    and ending before the first partition;
  - the Thimsy layout (E.6): entries 1 to 5 named uboot, boot, root, data,
    info with their types, uboot to root tiled from LBA 16384 with a spare as
    large as each region, data right behind root's spare and info (1 MiB)
    right behind data; on a card that has booted, games is entry 6;
  - the tail (E.6, device-safety-2): a written image ends IMAGE_TAIL_MIB after
    the 1 MiB boundary that ends its last entry, and games starts
    GAMES_GAP_MIB after that boundary, so the image's backup GPT ends at least
    1 MiB before games and writing the image again over a used card leaves
    the old library's boot region (its first 24 sectors) intact;
  - the vold rule: Android's vold makes a public volume of every entry typed
    Microsoft basic data or Linux filesystem data. A card has exactly one, at
    entry 7 or lower, and it carries a filesystem. In a written image it is
    info, entry 5, a small FAT, so Android mounts something and never offers
    to set up (format) the card before its first boot (OA-2); on a card that
    has booted it is games, and info is retyped Linux reserved;
  - the GUIDs (OA-9): the disk GUID and every entry's unique GUID are set and
    unique. With --release and --device, a written image's GUIDs are the ones
    pinned for that release and device (pinned_guid(), name-based, so a
    rebuild is byte-identical); a card that has booted carries none of the
    image's name-based GUIDs any more (D60firstboot makes them random).

Exit codes: 0 every check passed; 1 a check failed; 2 usage error or the input
could not be read. --json prints the findings and the parsed table as JSON.

Python 3.8+, standard library only.
"""
from __future__ import annotations

import argparse
import binascii
import json
import os
import re
import struct
import sys
import uuid
from typing import Any, Dict, List, Optional, Tuple

MIB = 1024 * 1024

# Type GUIDs, canonical (upper-case) form.
GUID_BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
GUID_LINUX_FS = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
GUID_LINUX_RESERVED = "8DA63339-0007-60C0-C436-083AC8230908"
GUID_ROOT_ARM64 = "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"
GUID_LINUX_HOME = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915"
GUID_ANDROID_META = "19A710A2-B3CA-11E4-B026-10604B889DCF"
GUID_ANDROID_EXPAND = "193D1EA4-B3CA-11E4-B075-10604B889DCF"

# Types Android's vold makes a public volume of (Disk.cpp: basic data and
# Linux filesystem data, whatever the entry number; plan 8.2).
VOLD_PUBLIC_TYPES = {GUID_BASIC_DATA: "Microsoft basic data",
                     GUID_LINUX_FS: "Linux filesystem data"}
# Types that make vold offer adoptable (private) storage: never on a card.
VOLD_PRIVATE_TYPES = {GUID_ANDROID_META: "Android meta",
                      GUID_ANDROID_EXPAND: "Android expand"}
VOLD_MAX_ENTRY = 7  # vold's minor-number arithmetic: MMC has 8 minors a disk

# E.6: the written image's entries, in order. info (entry 5, OA-2) is a FAT of
# INFO_MIB with a README: the one entry vold mounts before the first boot.
# games (entry 6) is created on the device at first boot and is never in a
# written image (I39); once it is ready, D60firstboot retypes info Linux
# reserved, so a card always has exactly one vold-mounted entry.
THIMSY_INFO = ("info", GUID_BASIC_DATA)
THIMSY_LAYOUT: List[Tuple[str, str]] = [
    ("uboot", GUID_LINUX_RESERVED),
    ("boot", GUID_LINUX_RESERVED),
    ("root", GUID_ROOT_ARM64),
    ("data", GUID_LINUX_HOME),
    THIMSY_INFO,
]
INFO_INDEX = 5
INFO_MIB = 1
INFO_RETIRED_TYPE = GUID_LINUX_RESERVED
THIMSY_GAMES = ("games", GUID_BASIC_DATA)
GAMES_INDEX = 6
assert THIMSY_LAYOUT[INFO_INDEX - 1] == THIMSY_INFO and GAMES_INDEX == len(THIMSY_LAYOUT) + 1

# OA-9: a written image's GPT GUIDs, pinned per release and device. Name-based
# (version 5) under a fixed namespace, so a rebuild gives the same bytes; a
# random GUID (the kernel's, sfdisk's, the installer's) is version 4, so a
# card still carrying the image's GUIDs can be told by their version.
GPT_NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL, "thimsy:gpt")
PINNED_VERSION = 5
LAYOUT_BASE_LBA = 16384      # the SPL finds uboot by name; FlipBase pins 16384
# device-safety-2: the written image ends IMAGE_TAIL_MIB after its last entry
# (genimage.cfg's image size; the backup GPT is its last 33 sectors), and
# D60firstboot's games entry starts GAMES_GAP_MIB after it (its GAP_MIB).
IMAGE_TAIL_MIB = 1
GAMES_GAP_MIB = 2
assert (GAMES_GAP_MIB - IMAGE_TAIL_MIB) * MIB >= MIB
TILED_REGIONS = ("uboot", "boot", "root")   # each followed by an equal spare

# Per family: where the raw first-stage loader sits (in 512-byte sectors).
FAMILIES: Dict[str, Dict[str, Any]] = {
    "rk3566": {"idbloader_lba": 64},
    "qemu-aarch64-virt": {"idbloader_lba": None},
}

GPT_SIGNATURE = b"EFI PART"
GPT_REVISION = 0x00010000
GPT_HEADER_FMT = "<8sIIIIQQQQ16sQIII"
GPT_HEADER_LEN = struct.calcsize(GPT_HEADER_FMT)  # 92


class Report:
    def __init__(self) -> None:
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.info: Dict[str, Any] = {}

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warnings.append(msg)

    @property
    def ok(self) -> bool:
        return not self.errors

    def as_dict(self) -> Dict[str, Any]:
        d: Dict[str, Any] = {"ok": self.ok, "errors": self.errors,
                             "warnings": self.warnings}
        d.update(self.info)
        return d


class Part:
    """One region of the card: a GPT entry, or a raw region (index None)."""

    def __init__(self, index: Optional[int], name: str, type_guid: str,
                 first_lba: int, last_lba: int, attrs: int = 0,
                 part_guid: str = "") -> None:
        self.index = index
        self.name = name
        self.type_guid = type_guid.upper()
        self.first_lba = first_lba
        self.last_lba = last_lba
        self.attrs = attrs
        self.part_guid = part_guid.upper()
        self.filesystem: Optional[str] = None

    @property
    def sectors(self) -> int:
        return self.last_lba - self.first_lba + 1

    def label(self) -> str:
        if self.index is None:
            return "raw region %r" % self.name
        return "entry %d (%r)" % (self.index, self.name)

    def as_dict(self, ss: int) -> Dict[str, Any]:
        d: Dict[str, Any] = {
            "index": self.index, "name": self.name, "type": self.type_guid,
            "type_name": type_name(self.type_guid),
            "first_lba": self.first_lba, "last_lba": self.last_lba,
            "offset": self.first_lba * ss, "size": self.sectors * ss,
        }
        if self.index is not None:
            d["guid"] = self.part_guid
            d["attributes"] = "0x%016x" % self.attrs
            d["filesystem"] = self.filesystem
        return d


def type_name(guid: str) -> str:
    names = {GUID_BASIC_DATA: "Microsoft basic data", GUID_LINUX_FS: "Linux filesystem data",
             GUID_LINUX_RESERVED: "Linux reserved", GUID_ROOT_ARM64: "Linux root (arm64)",
             GUID_LINUX_HOME: "Linux home", GUID_ANDROID_META: "Android meta",
             GUID_ANDROID_EXPAND: "Android expand"}
    return names.get(guid.upper(), "unknown")


def guid_from_disk(raw: bytes) -> str:
    """GPT stores GUIDs mixed-endian: the first three fields little-endian."""
    return str(uuid.UUID(bytes_le=bytes(raw))).upper()


def guid_to_disk(text: str) -> bytes:
    return uuid.UUID(text).bytes_le


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def pinned_guid(release: str, device: str, what: str) -> str:
    """The GUID a written image carries (OA-9): what is "disk" for the disk
    GUID, else "part:<name>" for the entry of that name. Upper case."""
    if not release or not device or not what:
        raise ValueError("a pinned GUID needs a release, a device and a name")
    return str(uuid.uuid5(GPT_NAMESPACE, "%s:%s:%s" % (release, device, what))).upper()


def pinned_guids(release: str, device: str, names: List[str]) -> Dict[str, str]:
    """{"disk": GUID, name: GUID, ...} for a device's image of a release."""
    out = {"disk": pinned_guid(release, device, "disk")}
    for n in names:
        out[n] = pinned_guid(release, device, "part:" + n)
    return out


def guid_version(text: str) -> Optional[int]:
    try:
        return uuid.UUID(text).version
    except ValueError:
        return None


# --------------------------------------------------------------------------
# Rules shared by images and genimage.cfg files


def check_layout(parts: List[Part], ss: int, rep: Report, *, mode: str,
                 family: Optional[str], thimsy: bool,
                 last_usable: Optional[int] = None,
                 entries_end_lba: Optional[int] = None,
                 idbloader: Optional[Part] = None, sniffed: bool = True) -> None:
    """parts: the GPT entries in table order. idbloader: the raw region.
    sniffed: each vold-mounted entry's filesystem was read (an image), not
    unknown (a genimage.cfg)."""
    # Names: unique, non-empty.
    seen: Dict[str, int] = {}
    for p in parts:
        if not p.name:
            rep.error("%s has no name" % p.label())
        elif p.name in seen:
            rep.error("%s repeats the name of entry %d" % (p.label(), seen[p.name]))
        else:
            seen[p.name] = p.index or 0

    # Order and overlaps: table order is disk order (E.6), nothing overlaps.
    prev: Optional[Part] = None
    for p in parts:
        if p.last_lba < p.first_lba:
            rep.error("%s ends before it starts" % p.label())
        if prev is not None and p.first_lba <= prev.last_lba:
            rep.error("%s starts at LBA %d, inside or before %s (ends %d)"
                      % (p.label(), p.first_lba, prev.label(), prev.last_lba))
        prev = p

    # 1 MiB alignment of every start and size; an entry that runs to the end
    # of the usable area (games, grown at first boot) may end unaligned.
    align = MIB // ss if MIB % ss == 0 else None
    if align:
        for p in parts:
            if p.first_lba % align:
                rep.error("%s starts at LBA %d, not on a 1 MiB boundary"
                          % (p.label(), p.first_lba))
            if p.sectors % align and p.last_lba != last_usable:
                rep.error("%s is %d bytes, not a whole number of MiB"
                          % (p.label(), p.sectors * ss))

    # The raw idbloader: after the primary entry array, before every entry.
    fam = FAMILIES.get(family or "", {})
    want_idb = fam.get("idbloader_lba")
    if want_idb is not None:
        idb_lba = want_idb * 512 // ss
        if idbloader is None:
            rep.error("family %s needs the raw idbloader at sector %d; none found"
                      % (family, want_idb))
        else:
            if idbloader.first_lba != idb_lba:
                rep.error("the idbloader starts at LBA %d, not %d"
                          % (idbloader.first_lba, idb_lba))
            if entries_end_lba is not None and idbloader.first_lba <= entries_end_lba:
                rep.error("the idbloader (LBA %d) overlaps the primary GPT entry array "
                          "(ends LBA %d)" % (idbloader.first_lba, entries_end_lba))
            if parts:
                first = min(parts, key=lambda q: q.first_lba)
                if idbloader.last_lba >= first.first_lba:
                    rep.error("the idbloader region (LBA %d-%d) runs into %s at LBA %d"
                              % (idbloader.first_lba, idbloader.last_lba,
                                 first.label(), first.first_lba))
    elif idbloader is not None and family:
        rep.warn("family %s has no idbloader, but a raw region %r is present"
                 % (family, idbloader.name))

    if thimsy:
        check_thimsy_layout(parts, ss, rep, mode=mode)
    check_vold(parts, rep, mode=mode, thimsy=thimsy, sniffed=sniffed)


def align_up(lba: int, ss: int) -> int:
    a = MIB // ss
    return (lba + a - 1) // a * a


def check_tail(parts: List[Part], ss: int, rep: Report, *, mode: str,
               image_sectors: Optional[int]) -> None:
    """device-safety-2: the image's end, and in a card the games entry's start."""
    written = [p for p in parts if p.name in dict(THIMSY_LAYOUT)]
    if not written:
        return
    end = align_up(max(p.last_lba for p in written) + 1, ss)
    tail = IMAGE_TAIL_MIB * MIB // ss
    games_start = end + GAMES_GAP_MIB * MIB // ss
    if mode == "image" and image_sectors is not None and image_sectors != end + tail:
        rep.error("the image is %d sectors; it must end %d MiB after its last entry, at "
                  "%d sectors, so its backup GPT ends 1 MiB before games (LBA %d) and a "
                  "re-written image leaves the old library's boot region intact "
                  "(device-safety-2)" % (image_sectors, IMAGE_TAIL_MIB, end + tail, games_start))
    if mode == "card":
        for p in parts:
            if p.name == THIMSY_GAMES[0] and p.first_lba < games_start:
                rep.error("%s starts at LBA %d; games starts at LBA %d or later, %d MiB after "
                          "the last image entry, so a re-written image's backup GPT misses "
                          "its boot region (device-safety-2)"
                          % (p.label(), p.first_lba, games_start, GAMES_GAP_MIB))


def check_thimsy_layout(parts: List[Part], ss: int, rep: Report, *, mode: str) -> None:
    want = list(THIMSY_LAYOUT)
    if mode == "card" and len(parts) > len(want):
        want.append(THIMSY_GAMES)
    names = [p.name for p in parts]
    if len(parts) != len(want):
        rep.error("the table has %d entries %s; a %s has %d: %s"
                  % (len(parts), names, "written image" if mode == "image" else "card",
                     len(want), [n for n, _ in want]))
    for i, (p, (name, guid)) in enumerate(zip(parts, want), 1):
        if p.name != name:
            rep.error("entry %d is named %r, expected %r" % (p.index or 0, p.name, name))
        elif p.index is not None and p.index != i:
            rep.error("%s is in table slot %d; E.6 puts it at entry %d" % (p.label(), p.index, i))
        ok = [guid]
        if mode == "card" and name == THIMSY_INFO[0]:
            ok.append(INFO_RETIRED_TYPE)   # retired by D60firstboot once games is ready
        if p.type_guid not in ok:
            rep.error("%s is typed %s (%s), expected %s"
                      % (p.label(), p.type_guid, type_name(p.type_guid),
                         " or ".join("%s (%s)" % (g, type_name(g)) for g in ok)))
    # Tiling: uboot at LBA 16384, each updatable region followed by a spare of
    # its own size, data right behind root's spare (FlipBase's gptslot rule).
    by = {p.name: p for p in parts}
    base = LAYOUT_BASE_LBA * 512 // ss
    if "uboot" in by and by["uboot"].first_lba != base:
        rep.error("uboot starts at LBA %d; the SPL needs LBA %d"
                  % (by["uboot"].first_lba, base))
    order = list(TILED_REGIONS) + ["data"]
    for a, b in zip(order, order[1:]):
        if a in by and b in by:
            want_start = by[a].first_lba + 2 * by[a].sectors
            if by[b].first_lba != want_start:
                rep.error("%s starts at LBA %d; behind %s and its equal spare it must "
                          "start at LBA %d" % (b, by[b].first_lba, a, want_start))
    # info (OA-2): INFO_MIB right behind data, so the tail rules count from it.
    info = by.get(THIMSY_INFO[0])
    if info is not None and "data" in by:
        want_start = by["data"].last_lba + 1
        if info.first_lba != want_start:
            rep.error("info starts at LBA %d; it must start right behind data, at LBA %d "
                      "(OA-2)" % (info.first_lba, want_start))
        if info.sectors * ss != INFO_MIB * MIB:
            rep.error("info is %d bytes; it must be %d MiB (OA-2)"
                      % (info.sectors * ss, INFO_MIB))


def check_vold(parts: List[Part], rep: Report, *, mode: str, thimsy: bool = True,
               sniffed: bool = True) -> None:
    public = [p for p in parts if p.type_guid in VOLD_PUBLIC_TYPES]
    for p in parts:
        if p.type_guid in VOLD_PRIVATE_TYPES:
            rep.error("%s is typed %s: vold offers to adopt it as internal storage"
                      % (p.label(), VOLD_PRIVATE_TYPES[p.type_guid]))
    if mode == "image":
        info = None
        for p in public:
            if thimsy and p.name == THIMSY_INFO[0] and p.index == INFO_INDEX:
                info = p
                continue
            rep.error("%s is typed %s, which vold mounts; a written image has one such "
                      "entry, entry %d %r (OA-2; games is made at first boot, decision I39)"
                      % (p.label(), VOLD_PUBLIC_TYPES[p.type_guid], INFO_INDEX,
                         THIMSY_INFO[0]))
        if thimsy and info is None:
            rep.error("the written image has no entry %d %r typed Microsoft basic data: with "
                      "no volume vold can mount, Android offers to set up (format) the card "
                      "before its first boot (OA-2, device-safety-3)"
                      % (INFO_INDEX, THIMSY_INFO[0]))
        if info is not None and sniffed and info.filesystem not in (None, "vfat"):
            rep.error("%s carries %s; it must be FAT (OA-2)" % (info.label(), info.filesystem))
    elif len(public) > 1:
        rep.error("%d entries have a type vold mounts (%s); a card has exactly one"
                  % (len(public), ", ".join(p.label() for p in public)))
    for p in public:
        if p.index is not None and p.index > VOLD_MAX_ENTRY:
            rep.error("%s: vold only sees entries 1 to %d" % (p.label(), VOLD_MAX_ENTRY))
        if sniffed and p.filesystem is None:
            rep.error("%s is typed %s but carries no filesystem: vold reports it "
                      "unmountable and its format wizard repartitions the card (risk 13)"
                      % (p.label(), VOLD_PUBLIC_TYPES[p.type_guid]))


def check_guids(parts: List[Part], disk_guid: str, rep: Report, *, mode: str,
                thimsy: bool, release: Optional[str] = None,
                device: Optional[str] = None) -> None:
    """OA-9: set and unique; pinned in a written image, random on a booted card."""
    zero = "00000000-0000-0000-0000-000000000000"
    seen: Dict[str, str] = {}
    for label, g in [("the disk GUID", disk_guid)] + [
            ("%s's unique GUID" % p.label(), p.part_guid) for p in parts if p.index]:
        if not g or g == zero:
            rep.error("%s is zero" % label)
        elif g in seen:
            rep.error("%s %s repeats %s" % (label, g, seen[g]))
        else:
            seen[g] = label
    image_names = [n for n, _ in THIMSY_LAYOUT]
    if mode == "image" and release and device:
        want = pinned_guids(release, device, [p.name for p in parts if p.index])
        if disk_guid != want["disk"]:
            rep.error("the disk GUID is %s; the image of %r for %r pins %s (OA-9)"
                      % (disk_guid, release, device, want["disk"]))
        for p in parts:
            if p.index and p.part_guid != want[p.name]:
                rep.error("%s's unique GUID is %s; the image of %r for %r pins %s (OA-9)"
                          % (p.label(), p.part_guid, release, device, want[p.name]))
    if mode == "card" and thimsy:
        pinned = [label for label, g in [("the disk GUID", disk_guid)] + [
            ("%s's unique GUID" % p.label(), p.part_guid) for p in parts
            if p.index and p.name in image_names] if guid_version(g) == PINNED_VERSION]
        for label in pinned:
            rep.error("%s is still name-based, the image's pinned value: every card written "
                      "from the image shares it until D60firstboot (or the installer) makes "
                      "it random (OA-9)" % label)


# --------------------------------------------------------------------------
# Images


def sniff_filesystem(head: bytes) -> Optional[str]:
    """Name the filesystem whose signature the partition's first 4 KiB carry."""
    if head[3:11] == b"EXFAT   ":
        return "exfat"
    if head[3:11] == b"NTFS    ":
        return "ntfs"
    if head[510:512] == bytes((0x55, 0xAA)) and (head[82:87] == b"FAT32"
                                                 or head[54:59] in (b"FAT12", b"FAT16")):
        return "vfat"
    # ext2/3/4: superblock at byte 1024, magic 0xEF53 (LE) at +56.
    if head[1024 + 56:1024 + 58] == struct.pack("<H", 0xEF53):
        return "ext4"
    # EROFS: superblock at byte 1024, magic 0xE0F5E1E2 (LE).
    if head[1024:1028] == struct.pack("<I", 0xE0F5E1E2):
        return "erofs"
    return None


class Image:
    def __init__(self, path: str, ss: int) -> None:
        self.path = path
        self.ss = ss
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.size = self.f.tell()

    def close(self) -> None:
        self.f.close()

    def read(self, lba: int, count: int = 1) -> bytes:
        self.f.seek(lba * self.ss)
        return self.f.read(count * self.ss)

    def read_at(self, offset: int, length: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(length)


def parse_header(raw: bytes, ss: int, rep: Report, which: str,
                 expect_lba: int, last_lba: int) -> Optional[Dict[str, Any]]:
    if len(raw) < GPT_HEADER_LEN:
        rep.error("%s GPT header: short read" % which)
        return None
    (sig, rev, hsize, hcrc, reserved, my_lba, alt_lba, first_usable, last_usable,
     disk_guid, entries_lba, n_entries, entry_size, entries_crc) = struct.unpack_from(
        GPT_HEADER_FMT, raw)
    if sig != GPT_SIGNATURE:
        rep.error("%s GPT header at LBA %d: no 'EFI PART' signature" % (which, expect_lba))
        return None
    h = {"lba": my_lba, "alternate_lba": alt_lba, "first_usable_lba": first_usable,
         "last_usable_lba": last_usable, "disk_guid": guid_from_disk(disk_guid),
         "entries_lba": entries_lba, "entries": n_entries, "entry_size": entry_size,
         "entries_crc32": "0x%08x" % entries_crc, "header_size": hsize,
         "_entries_crc": entries_crc}
    if rev != GPT_REVISION:
        rep.error("%s GPT header: revision 0x%08x, expected 0x00010000" % (which, rev))
    if hsize < GPT_HEADER_LEN or hsize > ss:
        rep.error("%s GPT header: header size %d out of range" % (which, hsize))
        return None
    check = bytearray(raw[:hsize])
    check[16:20] = b"\0\0\0\0"
    if crc32(bytes(check)) != hcrc:
        rep.error("%s GPT header: CRC32 0x%08x does not match its contents (0x%08x)"
                  % (which, hcrc, crc32(bytes(check))))
    if reserved != 0:
        rep.error("%s GPT header: reserved field is not zero" % which)
    if any(raw[GPT_HEADER_LEN:hsize]):
        rep.error("%s GPT header: bytes after the defined fields are not zero" % which)
    if my_lba != expect_lba:
        rep.error("%s GPT header says it is at LBA %d, found at %d" % (which, my_lba, expect_lba))
    want_alt = last_lba if which == "primary" else 1
    if alt_lba != want_alt:
        rep.error("%s GPT header: alternate LBA %d, expected %d (the image has %d sectors)"
                  % (which, alt_lba, want_alt, last_lba + 1))
    if entry_size < 128 or entry_size % 128 or entry_size > 4096:
        rep.error("%s GPT header: entry size %d is not a multiple of 128" % (which, entry_size))
        return None
    if n_entries == 0 or n_entries > 4096:
        rep.error("%s GPT header: %d entries" % (which, n_entries))
        return None
    arr_sectors = (n_entries * entry_size + ss - 1) // ss
    h["_entries_sectors"] = arr_sectors
    if first_usable > last_usable or last_usable >= last_lba:
        rep.error("%s GPT header: usable range %d-%d is wrong for a disk of %d sectors"
                  % (which, first_usable, last_usable, last_lba + 1))
    if which == "primary":
        if entries_lba < 2 or entries_lba + arr_sectors > first_usable:
            rep.error("primary entry array (LBA %d, %d sectors) is not between the header "
                      "and the first usable LBA %d" % (entries_lba, arr_sectors, first_usable))
    else:
        if entries_lba <= last_usable or entries_lba + arr_sectors != last_lba:
            rep.error("backup entry array (LBA %d, %d sectors) is not between the last "
                      "usable LBA %d and the backup header" % (entries_lba, arr_sectors,
                                                             last_usable))
    return h


def parse_entries(raw: bytes, h: Dict[str, Any], rep: Report) -> List[Part]:
    parts: List[Part] = []
    size = h["entry_size"]
    for i in range(h["entries"]):
        e = raw[i * size:(i + 1) * size]
        if len(e) < 128:
            break
        tguid = e[0:16]
        if not any(tguid):
            if any(e):
                rep.error("entry %d has no type but is not empty" % (i + 1))
            continue
        first, last, attrs = struct.unpack_from("<QQQ", e, 32)
        name_raw = e[56:128]
        try:
            name = name_raw.decode("utf-16-le")
        except UnicodeDecodeError:
            rep.error("entry %d: name is not valid UTF-16LE" % (i + 1))
            name = name_raw.decode("utf-16-le", "replace")
        if "\0" in name:
            name, rest = name.split("\0", 1)
            if rest.strip("\0"):
                rep.error("entry %d: bytes after the name's terminator" % (i + 1))
        parts.append(Part(i + 1, name, guid_from_disk(tguid), first, last, attrs,
                          guid_from_disk(e[16:32])))
    return parts


def find_idbloader(img: Image, start_lba: int, limit_lba: int,
                   size_bytes: Optional[int]) -> Optional[Part]:
    """The raw loader's extent: its size if given, else its last non-zero sector."""
    if size_bytes is not None:
        n = max(1, (size_bytes + img.ss - 1) // img.ss)
        return Part(None, "idbloader", "", start_lba, start_lba + n - 1)
    end = None
    lba = start_lba
    chunk = 256
    while lba < limit_lba:
        n = min(chunk, limit_lba - lba)
        data = img.read(lba, n)
        if not data:
            break
        used = 0 if data == bytes(len(data)) else len(data.rstrip(b"\0"))
        if used:
            end = lba + (used - 1) // img.ss
        lba += n
    if end is None:
        return None
    return Part(None, "idbloader", "", start_lba, end)


def check_image(path: str, *, mode: str = "image", family: Optional[str] = None,
                thimsy: bool = True, ss: int = 512,
                idbloader_size: Optional[int] = None, release: Optional[str] = None,
                device: Optional[str] = None) -> Report:
    rep = Report()
    img = Image(path, ss)
    try:
        _check_image(img, rep, mode=mode, family=family, thimsy=thimsy,
                     idbloader_size=idbloader_size, release=release, device=device)
    finally:
        img.close()
    return rep


def _check_image(img: Image, rep: Report, *, mode: str, family: Optional[str],
                 thimsy: bool, idbloader_size: Optional[int], release: Optional[str],
                 device: Optional[str]) -> None:
    ss = img.ss
    if img.size % ss:
        rep.error("image size %d is not a multiple of the %d-byte sector" % (img.size, ss))
    n_sectors = img.size // ss
    rep.info["image"] = {"path": img.path, "bytes": img.size, "sector_size": ss,
                         "sectors": n_sectors}
    if n_sectors < 68:
        rep.error("image too small for a GPT (%d sectors)" % n_sectors)
        return
    last_lba = n_sectors - 1

    # Protective MBR.
    mbr = img.read(0)
    if mbr[510:512] != b"\x55\xaa":
        rep.error("protective MBR: no 0x55AA signature")
    want_len = min(n_sectors - 1, 0xFFFFFFFF)
    for i in range(4):
        e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
        status, ptype = e[0], e[4]
        lba_start, lba_len = struct.unpack_from("<II", e, 8)
        if i == 0:
            if ptype != 0xEE:
                rep.error("protective MBR: entry 1 has type 0x%02x, expected 0xEE" % ptype)
            if status != 0:
                rep.error("protective MBR: entry 1 is marked bootable")
            if lba_start != 1:
                rep.error("protective MBR: entry 1 starts at LBA %d, expected 1" % lba_start)
            if lba_len != want_len:
                rep.error("protective MBR: entry 1 covers %d sectors, expected %d"
                          % (lba_len, want_len))
        elif any(e):
            rep.error("protective MBR: entry %d is not empty (a hybrid MBR)" % (i + 1))

    prim = parse_header(img.read(1), ss, rep, "primary", 1, last_lba)
    back = parse_header(img.read(last_lba), ss, rep, "backup", last_lba, last_lba)
    if prim is None:
        return
    p_raw = img.read(prim["entries_lba"], prim["_entries_sectors"])
    p_arr = p_raw[:prim["entries"] * prim["entry_size"]]
    if crc32(p_arr) != prim["_entries_crc"]:
        rep.error("primary entry array: CRC32 0x%08x, header says %s"
                  % (crc32(p_arr), prim["entries_crc32"]))
    if back is not None:
        for k in ("first_usable_lba", "last_usable_lba", "disk_guid", "entries",
                  "entry_size", "_entries_crc"):
            if prim[k] != back[k]:
                rep.error("backup GPT header's %s differs from the primary's"
                          % k.strip("_"))
        b_raw = img.read(back["entries_lba"], back["_entries_sectors"])
        b_arr = b_raw[:back["entries"] * back["entry_size"]]
        if crc32(b_arr) != back["_entries_crc"]:
            rep.error("backup entry array: CRC32 0x%08x, header says %s"
                      % (crc32(b_arr), back["entries_crc32"]))
        if b_arr != p_arr:
            rep.error("backup entry array differs from the primary")

    parts = parse_entries(p_arr, prim, rep)
    entries_end = prim["entries_lba"] + prim["_entries_sectors"] - 1
    for p in parts:
        if p.first_lba < prim["first_usable_lba"] or p.last_lba > prim["last_usable_lba"]:
            rep.error("%s (LBA %d-%d) is outside the usable range %d-%d"
                      % (p.label(), p.first_lba, p.last_lba, prim["first_usable_lba"],
                         prim["last_usable_lba"]))
        if p.type_guid in VOLD_PUBLIC_TYPES and p.first_lba < n_sectors:
            p.filesystem = sniff_filesystem(img.read_at(p.first_lba * ss, 4096))

    idb = None
    fam = FAMILIES.get(family or "", {})
    if fam.get("idbloader_lba") is not None:
        start = fam["idbloader_lba"] * 512 // ss
        limit = min([p.first_lba for p in parts] + [n_sectors])
        idb = find_idbloader(img, start, limit, idbloader_size)
        if idb is not None:
            head = img.read(start, 1)
            magic = head[:4]
            rep.info["idbloader"] = {"first_lba": idb.first_lba, "last_lba": idb.last_lba,
                                     "magic": magic.decode("latin-1")}
            if magic != b"RKNS":
                rep.warn("idbloader at LBA %d does not start with Rockchip's 'RKNS' "
                         "header (an older RC4 header is possible)" % start)

    rep.info["gpt"] = {k: v for k, v in prim.items() if not k.startswith("_")}
    rep.info["entries"] = [p.as_dict(ss) for p in parts]
    check_layout(parts, ss, rep, mode=mode, family=family, thimsy=thimsy,
                 last_usable=prim["last_usable_lba"], entries_end_lba=entries_end,
                 idbloader=idb)
    check_guids(parts, prim["disk_guid"], rep, mode=mode, thimsy=thimsy, release=release,
                device=device)
    if thimsy:
        check_tail(parts, ss, rep, mode=mode, image_sectors=n_sectors)


# --------------------------------------------------------------------------
# genimage.cfg (libconfuse syntax), read statically

_TOKEN = re.compile(r'''
    (?P<ws>\s+)
  | (?P<comment>\#[^\n]*|//[^\n]*|/\*.*?\*/)
  | (?P<string>"(?:[^"\\]|\\.)*")
  | (?P<punct>[{}=,;()])
  | (?P<word>[^\s{}=,;()"\#]+)
''', re.S | re.X)


def _tokens(text: str) -> List[Tuple[str, str]]:
    out: List[Tuple[str, str]] = []
    pos = 0
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m:
            raise ValueError("genimage.cfg: cannot parse at offset %d: %r"
                             % (pos, text[pos:pos + 20]))
        pos = m.end()
        kind = m.lastgroup or ""
        if kind in ("ws", "comment"):
            continue
        val = m.group(kind)
        if kind == "string":
            val = re.sub(r"\\(.)", r"\1", val[1:-1])
        out.append((kind, val))
    return out


def parse_genimage(text: str) -> List[Tuple[str, Optional[str], Any]]:
    """Parse into a list of (key, title, value); a section's value is a list."""
    toks = _tokens(text)
    pos = 0

    def block(end: Optional[str]) -> List[Tuple[str, Optional[str], Any]]:
        nonlocal pos
        items: List[Tuple[str, Optional[str], Any]] = []
        while pos < len(toks):
            kind, val = toks[pos]
            if kind == "punct" and val == end:
                pos += 1
                return items
            if kind == "punct" and val in ",;":
                pos += 1
                continue
            if kind not in ("word", "string"):
                raise ValueError("genimage.cfg: unexpected %r" % val)
            key = val
            pos += 1
            if pos >= len(toks):
                raise ValueError("genimage.cfg: %r at end of file" % key)
            kind2, val2 = toks[pos]
            if kind2 == "punct" and val2 == "=":
                pos += 1
                if pos >= len(toks) or toks[pos][0] not in ("word", "string"):
                    raise ValueError("genimage.cfg: %r has no value" % key)
                items.append((key, None, toks[pos][1]))
                pos += 1
                continue
            title = None
            if kind2 in ("word", "string"):
                title = val2
                pos += 1
            if pos >= len(toks) or toks[pos] != ("punct", "{"):
                raise ValueError("genimage.cfg: %r: expected '{' or '='" % key)
            pos += 1
            items.append((key, title, block("}")))
        if end is not None:
            raise ValueError("genimage.cfg: missing '}'")
        return items

    return block(None)


_SIZE = re.compile(r"^(0x[0-9a-fA-F]+|\d+)\s*([kKMGT]?)$")


def parse_size(text: str) -> int:
    m = _SIZE.match(text.strip())
    if not m:
        raise ValueError("genimage.cfg: not a size: %r" % text)
    n = int(m.group(1), 0)
    return n * {"": 1, "k": 1024, "K": 1024, "M": MIB, "G": 1024 * MIB,
                "T": 1024 * 1024 * MIB}[m.group(2)]


def _truth(val: str) -> bool:
    return val.lower() in ("true", "yes", "on", "1")


def genimage_layout(text: str, ss: int = 512) -> Tuple[List[Dict[str, Any]], List[str]]:
    """Every hdimage image in the file, with its partitions, in file order.

    Returns (images, problems). A partition without an explicit offset and
    size is a problem: the layout is fixed by E.6, not left to genimage.
    """
    problems: List[str] = []
    images: List[Dict[str, Any]] = []
    for key, title, body in parse_genimage(text):
        if key != "image" or not isinstance(body, list):
            continue
        hd = [b for k, _, b in body if k == "hdimage"]
        if not hd:
            continue
        opts = {k: v for k, _, v in hd[0] if not isinstance(v, list)}
        image_opts = {k: v for k, _, v in body if not isinstance(v, list)}
        parts: List[Dict[str, Any]] = []
        for k, ptitle, pbody in body:
            if k != "partition":
                continue
            p = {kk: vv for kk, _, vv in pbody if not isinstance(vv, list)}
            entry: Dict[str, Any] = {"name": ptitle or "", "raw": p}
            for need in ("offset", "size"):
                if need not in p:
                    problems.append("%s: partition %r has no explicit %s" % (title, ptitle, need))
            entry["offset"] = parse_size(p["offset"]) if "offset" in p else None
            entry["size"] = parse_size(p["size"]) if "size" in p else None
            entry["in_table"] = _truth(p.get("in-partition-table", "true"))
            entry["type"] = p.get("partition-type-uuid", "").upper()
            entry["image"] = p.get("image")
            entry["uuid"] = p.get("partition-uuid", "").upper()
            parts.append(entry)
        images.append({"name": title, "hdimage": opts, "partitions": parts,
                       "size": parse_size(image_opts["size"]) if "size" in image_opts else None})
    return images, problems


def check_genimage(path: str, *, family: Optional[str] = None, thimsy: bool = True,
                   ss: int = 512, release: Optional[str] = None,
                   device: Optional[str] = None) -> Report:
    """A board's genimage.cfg, or with release and device the cfg
    tools/card_image.py wrote for that device: a board's has no GUIDs (they
    are pinned per device, OA-9), a device's has exactly the pinned ones."""
    rep = Report()
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    try:
        images, problems = genimage_layout(text, ss)
    except ValueError as exc:
        rep.error(str(exc))
        return rep
    for msg in problems:
        rep.error(msg)
    if not images:
        rep.error("%s defines no hdimage" % path)
    rep.info["images"] = []
    for im in images:
        opts = im["hdimage"]
        if opts.get("partition-table-type", "").lower() != "gpt":
            rep.error("image %s: partition-table-type is not \"gpt\"" % im["name"])
        if "gpt-location" in opts and parse_size(opts["gpt-location"]) != 2 * ss:
            rep.error("image %s: gpt-location moved from LBA 2" % im["name"])
        parts: List[Part] = []
        raw: List[Part] = []
        idx = 0
        for e in im["partitions"]:
            if e["offset"] is None or e["size"] is None:
                continue
            if e["offset"] % ss or e["size"] % ss:
                rep.error("image %s: partition %r is not sector-aligned" % (im["name"], e["name"]))
            first = e["offset"] // ss
            last = first + max(1, e["size"] // ss) - 1
            if e["in_table"]:
                idx += 1
                if not e["type"]:
                    rep.error("image %s: partition %r has no partition-type-uuid"
                              % (im["name"], e["name"]))
                elif not re.match(r"^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$",
                                  e["type"]):
                    rep.error("image %s: partition %r: %r is not a GUID"
                              % (im["name"], e["name"], e["type"]))
                parts.append(Part(idx, e["name"], e["type"], first, last,
                                  part_guid=e["uuid"]))
                if thimsy and e["name"] == THIMSY_INFO[0] and not e["image"]:
                    rep.error("image %s: partition %r names no image: a basic-data entry "
                              "without a filesystem (OA-2, risk 13)" % (im["name"], e["name"]))
            else:
                raw.append(Part(None, e["name"], "", first, last))
        idb = None
        fam = FAMILIES.get(family or "", {})
        if fam.get("idbloader_lba") is not None:
            want = fam["idbloader_lba"] * 512 // ss
            at = [r for r in raw if r.first_lba == want]
            idb = at[0] if at else (raw[0] if raw else None)
        for r in raw:
            if r is not idb:
                rep.error("image %s: unexpected raw region %r" % (im["name"], r.name))
        # The primary GPT of 128 entries ends at LBA 33 (genimage's layout).
        check_layout(parts, ss, rep, mode="image", family=family, thimsy=thimsy,
                     entries_end_lba=33, idbloader=idb, sniffed=False)
        disk_guid = opts.get("disk-uuid", "").upper()
        if release and device:
            check_guids(parts, disk_guid, rep, mode="image", thimsy=thimsy,
                        release=release, device=device)
        else:
            if disk_guid:
                rep.error("image %s: disk-uuid is set by hand; 30-genimage.sh pins it per "
                          "device (OA-9)" % im["name"])
            for p in parts:
                if p.part_guid:
                    rep.error("image %s: partition %r: partition-uuid is set by hand; "
                              "30-genimage.sh pins it per device (OA-9)" % (im["name"], p.name))
        if thimsy:
            if im["size"] is None:
                rep.error("image %s has no explicit size: it must end %d MiB after its last "
                          "entry (device-safety-2)" % (im["name"], IMAGE_TAIL_MIB))
            elif im["size"] % ss:
                rep.error("image %s: size is not sector-aligned" % im["name"])
            else:
                check_tail(parts, ss, rep, mode="image", image_sectors=im["size"] // ss)
        rep.info["images"].append({
            "name": im["name"],
            "entries": [p.as_dict(ss) for p in parts],
            "raw": [r.as_dict(ss) for r in raw],
        })
    return rep


# --------------------------------------------------------------------------


def family_from_path(path: str) -> Optional[str]:
    parent = os.path.basename(os.path.dirname(os.path.abspath(path)))
    return parent if parent in FAMILIES else None


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Check a Thimsy card image's GPT (or a genimage.cfg) against E.6.")
    ap.add_argument("path", help="the image, or with --genimage the genimage.cfg")
    ap.add_argument("--genimage", action="store_true",
                    help="read PATH as a genimage.cfg instead of an image")
    ap.add_argument("--mode", choices=("image", "card"), default="image",
                    help="image: a written image, info the only vold-mounted entry "
                         "(default); card: a card that has booted, games instead once "
                         "formatted")
    ap.add_argument("--family", choices=sorted(FAMILIES) + ["none"],
                    help="the SoC family (idbloader rules); from the path if omitted")
    ap.add_argument("--layout", choices=("thimsy", "none"), default="thimsy",
                    help="check E.6's names, types and tiling (default) or only GPT rules")
    ap.add_argument("--release", metavar="VERSION",
                    help="with --device: the image's GUIDs must be the ones pinned for them")
    ap.add_argument("--device", metavar="ID", help="the device id (with --release)")
    ap.add_argument("--idbloader-size", type=int, metavar="BYTES",
                    help="the idbloader's size; otherwise its last non-zero sector ends it")
    ap.add_argument("--sector-size", type=int, default=512, choices=(512, 4096))
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    args = ap.parse_args(argv)
    if bool(args.release) != bool(args.device):
        ap.error("--release and --device go together")

    family = args.family
    if family is None:
        family = family_from_path(args.path)
    if family == "none":
        family = None
    try:
        if args.genimage:
            rep = check_genimage(args.path, family=family, thimsy=args.layout == "thimsy",
                                 ss=args.sector_size, release=args.release,
                                 device=args.device)
        else:
            rep = check_image(args.path, mode=args.mode, family=family,
                              thimsy=args.layout == "thimsy", ss=args.sector_size,
                              idbloader_size=args.idbloader_size, release=args.release,
                              device=args.device)
    except OSError as exc:
        print("gpt_check: %s" % exc, file=sys.stderr)
        return 2
    rep.info["family"] = family
    if args.json:
        print(json.dumps(rep.as_dict(), indent=2, sort_keys=True))
    else:
        for w in rep.warnings:
            print("warning: %s" % w)
        for e in rep.errors:
            print("FAIL: %s" % e)
        print("%s: %s" % (args.path, "ok" if rep.ok else "%d problem(s)" % len(rep.errors)))
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
