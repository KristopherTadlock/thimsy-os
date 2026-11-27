#!/usr/bin/env python3
"""The per-device parts of a Thimsy card that genimage does not make itself.

    card_image.py cfg CFG --release VERSION --device ID -o OUT
    card_image.py info --cfg CFG --release VERSION --device ID [--model TEXT]
                       [--epoch SECONDS] [--readme TEMPLATE] -o OUT

cfg (OA-9; post-image.d/30-genimage.sh): the board's genimage.cfg with the
device's pinned GPT GUIDs added, disk-uuid in the hdimage section and
partition-uuid in every partition that is in the table. The GUIDs are
gpt_check.pinned_guid()'s, name-based from the release and the device, so a
rebuild is byte-identical. A board cfg that already sets one is refused: the
values belong to the release and the device, not to the board. The output
is read back and checked by gpt_check.check_genimage().

info (OA-2; post-image.d/28-info.sh): entry 5's filesystem, a FAT12 as large
as the cfg's info partition (1 MiB), labelled THIMSY-INFO, holding
README.TXT (board/common/card-info/README.txt with @MODEL@, @VERSION@ and
@DEVICE@ filled in, CRLF line ends). Android's vold mounts it, so a freshly
written card is not offered for set-up (formatting) before Linux's first
boot. Every value comes from the arguments: the volume serial is name-based
like the GUIDs, the times are the epoch's (SOURCE_DATE_EPOCH, clamped to
FAT's 1980 to 2107), so the same arguments give the same bytes.

Exit codes: 0 written; 1 refused (the cfg or the README does not fit); 2 usage.
Python 3.8+, standard library only.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import time
import uuid
from typing import List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gpt_check  # noqa: E402

DEFAULT_README = os.path.join(os.path.dirname(HERE), "board", "common", "card-info",
                              "README.txt")
INFO_LABEL = "THIMSY-INFO"
SECTOR = 512
ROOT_ENTRIES = 512          # 32 sectors, as mkfs.fat makes on a disk
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
NUM_FATS = 2
MEDIA = 0xF8


class Refused(Exception):
    pass


# --------------------------------------------------------------------------
# cfg: the pinned GUIDs


def pin_cfg(text: str, release: str, device: str) -> str:
    images, problems = gpt_check.genimage_layout(text)
    if problems:
        raise Refused("; ".join(problems))
    if len(images) != 1:
        raise Refused("the cfg defines %d hdimage images; a card has one" % len(images))
    im = images[0]
    if "disk-uuid" in im["hdimage"]:
        raise Refused("the cfg sets disk-uuid by hand; it is pinned per device")
    names = []
    for p in im["partitions"]:
        if not p["in_table"]:
            continue
        if "partition-uuid" in p["raw"]:
            raise Refused("partition %r sets partition-uuid by hand; it is pinned per device"
                          % p["name"])
        names.append(p["name"])
    want = gpt_check.pinned_guids(release, device, names)
    lines = text.split("\n")
    out: List[str] = []
    hd = re.compile(r"^(\s*)hdimage\s*\{\s*$")
    part = re.compile(r"^(\s*)partition\s+([^\s{]+)\s*\{\s*$")
    done_disk = False
    done_parts = set()
    for line in lines:
        out.append(line)
        m = hd.match(line)
        if m:
            if done_disk:
                raise Refused("two hdimage sections")
            out.append('%s\tdisk-uuid = "%s"' % (m.group(1), want["disk"]))
            done_disk = True
            continue
        m = part.match(line)
        if m and m.group(2) in want and m.group(2) != "disk":
            out.append('%s\tpartition-uuid = "%s"' % (m.group(1), want[m.group(2)]))
            done_parts.add(m.group(2))
    if not done_disk:
        raise Refused("no 'hdimage {' line to put disk-uuid in")
    missing = [n for n in names if n not in done_parts]
    if missing:
        raise Refused("no 'partition NAME {' line for %s" % ", ".join(missing))
    return "\n".join(out)


def cmd_cfg(args: argparse.Namespace) -> int:
    with open(args.cfg, encoding="utf-8") as f:
        text = f.read()
    pinned = pin_cfg(text, args.release, args.device)
    tmp = args.out + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(pinned)
    rep = gpt_check.check_genimage(tmp, family=args.family, release=args.release,
                                   device=args.device)
    if not rep.ok:
        os.unlink(tmp)
        raise Refused("the pinned cfg fails gpt_check: " + "; ".join(rep.errors))
    os.replace(tmp, args.out)
    return 0


# --------------------------------------------------------------------------
# info: a small FAT12


FAT_FIRST = 315532800       # 1980-01-01T00:00:00Z
FAT_LAST = 4354819198       # 2107-12-31T23:59:58Z


def fat_datetime(epoch: int) -> "tuple[int, int]":
    """FAT's (date, time) for an epoch, UTC, clamped to FAT's range."""
    t = time.gmtime(min(max(int(epoch), FAT_FIRST), FAT_LAST))
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tod = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tod


def volume_serial(release: str, device: str) -> int:
    g = uuid.UUID(gpt_check.pinned_guid(release, device, "fat:info"))
    serial = struct.unpack("<I", g.bytes[:4])[0]
    return serial or 1   # 0000-0000 is no id (thimsy-session, plan 7.3)


def fat12_geometry(total_sectors: int) -> "tuple[int, int, int]":
    """(sectors per FAT, root directory sectors, clusters)."""
    root_secs = ROOT_ENTRIES * 32 // SECTOR
    fat_secs = 1
    while True:
        data = total_sectors - RESERVED_SECTORS - NUM_FATS * fat_secs - root_secs
        clusters = data // SECTORS_PER_CLUSTER
        need = ((clusters + 2) * 3 + 1) // 2
        if (need + SECTOR - 1) // SECTOR <= fat_secs:
            return fat_secs, root_secs, clusters
        fat_secs += 1


def dir_entry(name11: bytes, attr: int, cluster: int, size: int, date: int, tod: int) -> bytes:
    assert len(name11) == 11
    return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, tod, date, date, 0,
                       tod, date, cluster, size)


def make_info_fat(size: int, *, hidden_sectors: int, serial: int, epoch: int,
                  readme: bytes) -> bytes:
    if size % SECTOR:
        raise Refused("the info partition is not a whole number of sectors")
    total = size // SECTOR
    if total >= 0x10000:
        raise Refused("the info partition is too large for this FAT12 writer")
    fat_secs, root_secs, clusters = fat12_geometry(total)
    if not 16 <= clusters < 4085 - 16:   # well inside FAT12, away from the FAT16 boundary
        raise Refused("%d clusters is not a clear FAT12" % clusters)
    cluster_bytes = SECTOR * SECTORS_PER_CLUSTER
    n = (len(readme) + cluster_bytes - 1) // cluster_bytes
    if n > clusters:
        raise Refused("README.TXT (%d bytes) does not fit in %d bytes" % (len(readme), size))
    date, tod = fat_datetime(epoch)
    img = bytearray(size)

    # Boot sector and BPB (FAT12/16 layout).
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x3c\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, SECTORS_PER_CLUSTER, RESERVED_SECTORS,
                     NUM_FATS, ROOT_ENTRIES, total, MEDIA, fat_secs, 32, 64,
                     hidden_sectors, 0)
    struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, serial,
                     INFO_LABEL.encode("ascii").ljust(11), b"FAT12   ")
    bs[62:65] = b"\xf4\xeb\xfd"   # not bootable: hlt; jmp $-1
    bs[510:512] = b"\x55\xaa"
    img[0:SECTOR] = bs

    # The FATs: media and end-of-chain marks, then README.TXT's chain.
    entries = [0xF00 | MEDIA, 0xFFF] + [0] * clusters
    for i in range(n):
        c = 2 + i
        entries[c] = c + 1 if i < n - 1 else 0xFFF
    fat = bytearray(fat_secs * SECTOR)
    for c, v in enumerate(entries):
        off = c * 3 // 2
        if c % 2 == 0:
            fat[off] = v & 0xFF
            fat[off + 1] = (fat[off + 1] & 0xF0) | (v >> 8)
        else:
            fat[off] = (fat[off] & 0x0F) | ((v & 0x0F) << 4)
            fat[off + 1] = v >> 4
    for k in range(NUM_FATS):
        at = (RESERVED_SECTORS + k * fat_secs) * SECTOR
        img[at:at + len(fat)] = fat

    # The root directory: the volume label, then README.TXT.
    root = (RESERVED_SECTORS + NUM_FATS * fat_secs) * SECTOR
    img[root:root + 32] = dir_entry(INFO_LABEL.encode("ascii").ljust(11), 0x08, 0, 0, date, tod)
    img[root + 32:root + 64] = dir_entry(b"README  TXT", 0x20, 2 if n else 0, len(readme),
                                         date, tod)

    data = root + root_secs * SECTOR
    img[data:data + len(readme)] = readme
    return bytes(img)


def readme_text(template: str, *, model: str, release: str, device: str) -> bytes:
    text = (template.replace("@MODEL@", model).replace("@VERSION@", release)
            .replace("@DEVICE@", device))
    left = re.search(r"@[A-Z_]+@", text)
    if left:
        raise Refused("the README template has an unknown placeholder: %s" % left.group(0))
    text = text.replace("\r\n", "\n").replace("\n", "\r\n")
    return text.encode("utf-8")


def cmd_info(args: argparse.Namespace) -> int:
    with open(args.cfg, encoding="utf-8") as f:
        images, problems = gpt_check.genimage_layout(f.read())
    if problems or len(images) != 1:
        raise Refused("%s: not a one-image genimage.cfg" % args.cfg)
    parts = [p for p in images[0]["partitions"] if p["name"] == gpt_check.THIMSY_INFO[0]]
    if len(parts) != 1:
        raise Refused("%s has no %r partition" % (args.cfg, gpt_check.THIMSY_INFO[0]))
    p = parts[0]
    with open(args.readme, encoding="utf-8") as f:
        readme = readme_text(f.read(), model=args.model or args.device, release=args.release,
                             device=args.device)
    blob = make_info_fat(p["size"], hidden_sectors=p["offset"] // SECTOR,
                         serial=volume_serial(args.release, args.device), epoch=args.epoch,
                         readme=readme)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, args.out)
    return 0


# --------------------------------------------------------------------------


def _device_id(text: str) -> str:
    if not re.match(r"^[a-z0-9][a-z0-9-]*$", text):
        raise argparse.ArgumentTypeError("bad device id %r" % text)
    return text


def _release(text: str) -> str:
    if not text or any(c.isspace() or c == ":" for c in text):
        raise argparse.ArgumentTypeError("bad release %r (no spaces or colons)" % text)
    return text


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Per-device card parts (OA-2, OA-9).")
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("cfg", help="the board's genimage.cfg with the pinned GPT GUIDs")
    c.add_argument("cfg")
    c.add_argument("--family", required=True, choices=sorted(gpt_check.FAMILIES),
                   help="the SoC family (gpt_check's idbloader rules)")
    i = sub.add_parser("info", help="entry 5's FAT image")
    i.add_argument("--cfg", required=True)
    i.add_argument("--model", default="")
    i.add_argument("--epoch", type=int, default=0)
    i.add_argument("--readme", default=DEFAULT_README)
    for s in (c, i):
        s.add_argument("--release", required=True, type=_release)
        s.add_argument("--device", required=True, type=_device_id)
        s.add_argument("-o", "--out", required=True)
    args = ap.parse_args(argv)
    if args.cmd is None:
        ap.print_usage(sys.stderr)
        return 2
    try:
        return cmd_cfg(args) if args.cmd == "cfg" else cmd_info(args)
    except Refused as exc:
        print("card_image: %s" % exc, file=sys.stderr)
        return 1
    except (OSError, ValueError) as exc:
        print("card_image: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
