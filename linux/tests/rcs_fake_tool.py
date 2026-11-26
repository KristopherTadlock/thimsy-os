#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fake system tools for test_rcs.py, and the fake kernel's view of disks.

    rcs_fake_tool.py TOOL ARGS...

The init scripts run tools through THIMSY_TOOLDIR; test_rcs.py puts a small
sh wrapper per tool there that execs this file. Each call is appended to
THIMSY_FAKE_LOG as one tab-separated line. Only sfdisk and mkfs.exfat
are here; test_rcs.py writes the trivial tools as sh scripts (a Python start
per call is slow on Windows).

Disks. ``$THIMSY_ROOT/dev/<disk>`` is a JSON file holding a GPT:
    {"kind": "disk", "name", "sector_size", "size_sectors", "first_lba",
     "last_lba", "removable_type": "SD"|"MMC"|null, "major", "minor",
     "parts": {"<n>": {"start", "size", "type", "uuid", "name", "fs"}}}
and ``$THIMSY_ROOT/dev/<partition>`` is {"kind": "part", "disk", "n"}.
``sync_kernel_view`` writes the matching /sys/class/block tree, as the
kernel does at boot and, through BLKPG, when sfdisk adds an entry.

A disk may also have bytes: ``$THIMSY_ROOT/dev/<disk>.img``, a sparse file
as large as the disk. Then the sh fake ``dd`` reads it for ``if=<disk>``,
the fake mkfs.exfat writes a small exFAT (boot regions and a root
directory with the volume label) at the partition's start, and the fake
sfdisk, when it creates an entry, records the exFAT it finds at the start
in the entry's "fs" (what the kernel would see there). D60firstboot's
re-adoption of an old library is tested this way (device-safety-2).

Power cuts. THIMSY_FAKE_POWERCUT="STEP:MODE" cuts the power at the first
call of STEP (relocate, create, mkfs, retype): MODE "before" drops the
call's effect, "after" keeps it, "torn" (mkfs only) leaves a filesystem
that was started and not finished. The cut writes $THIMSY_ROOT/.powercut;
while it exists every fake tool fails without an effect, so nothing after
the cut reaches the disk, as on a device. A test "reboots" by removing it,
emptying /run and calling sync_kernel_view.

"""
from __future__ import annotations

import json
import os
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

RESERVED = "8DA63339-0007-60C0-C436-083AC8230908"
BASICDATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"


def write_lf(path: Path, text: str) -> None:
    """Write text with LF line endings (Path.write_text has no newline= before 3.10)."""
    with open(str(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# --- the fake kernel -----------------------------------------------------------

def part_name(disk: str, n: int) -> str:
    return f"{disk}p{n}" if disk[-1:].isdigit() else f"{disk}{n}"


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    write_lf(path, text)


def _rmtree(path: Path) -> None:
    if not path.exists():
        return
    for p in sorted(path.rglob("*"), key=lambda q: len(q.parts), reverse=True):
        if p.is_dir():
            p.rmdir()
        else:
            p.unlink()
    path.rmdir()


def load_disk(root: Path, disk: str) -> Dict[str, Any]:
    return json.loads((root / "dev" / disk).read_text(encoding="utf-8"))


def save_disk(root: Path, d: Dict[str, Any]) -> None:
    path = root / "dev" / d["name"]
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(d, indent=1, sort_keys=True), encoding="utf-8")
    os.replace(tmp, path)


def sync_kernel_view(root: Path, disk: str) -> None:
    """Make /sys/class/block and /dev match the disk's GPT."""
    d = load_disk(root, disk)
    cls = root / "sys" / "class" / "block"
    ddir = cls / disk
    ssz = d["sector_size"]
    for child in list(ddir.iterdir()) if ddir.exists() else []:
        if child.is_dir() and child.name.startswith(disk) and child.name != disk:
            _rmtree(child)
            _rmtree(cls / child.name)
            node = root / "dev" / child.name
            if node.exists():
                node.unlink()
    _write(ddir / "dev", f"{d['major']}:{d['minor']}\n")
    _write(ddir / "size", f"{d['size_sectors'] * ssz // 512}\n")
    _write(ddir / "uevent", f"MAJOR={d['major']}\nMINOR={d['minor']}\n"
                            f"DEVNAME={disk}\nDEVTYPE=disk\n")
    if d.get("removable_type"):
        _write(ddir / "device" / "type", d["removable_type"] + "\n")
    for key in sorted(d["parts"], key=int):
        n = int(key)
        p = d["parts"][key]
        name = part_name(disk, n)
        minor = d["minor"] + n
        ue = (f"MAJOR={d['major']}\nMINOR={minor}\nDEVNAME={name}\nDEVTYPE=partition\n"
              f"DISKSEQ=1\nPARTN={n}\n")
        if p.get("name"):
            ue += f"PARTNAME={p['name']}\n"
        ue += f"PARTUUID={p['uuid'].lower()}\n"
        for base in (ddir / name, cls / name):
            _write(base / "partition", f"{n}\n")
            _write(base / "dev", f"{d['major']}:{minor}\n")
            _write(base / "start", f"{p['start'] * ssz // 512}\n")
            _write(base / "size", f"{p['size'] * ssz // 512}\n")
            _write(base / "uevent", ue)
        _write(root / "dev" / name, json.dumps({"kind": "part", "disk": disk, "n": n}))


# --- bytes: sparse disk images and a small exFAT ---------------------------------

def image_path(root: Path, disk: str) -> Path:
    return root / "dev" / (disk + ".img")


def make_sparse(path: Path, size: int) -> bool:
    """Create PATH as a sparse file of SIZE bytes. False when it can't be sparse
    (writing far into it would then fill it with zeros)."""
    with open(str(path), "wb") as f:
        if os.name == "nt":
            import ctypes
            import msvcrt
            from ctypes import wintypes
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            k32.DeviceIoControl.argtypes = [
                wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                wintypes.LPVOID]
            done = wintypes.DWORD()
            fsctl_set_sparse = 0x000900C4
            if not k32.DeviceIoControl(msvcrt.get_osfhandle(f.fileno()), fsctl_set_sparse,
                                       None, 0, None, 0, ctypes.byref(done), None):
                return False
            # Python's truncate() on Windows (_chsize) writes the zeros out; a
            # write at the end of a sparse file does not.
            f.seek(size - 1)
            f.write(bytes(1))
        else:
            f.truncate(size)
    return True


def patch_bytes(path: Path, offset: int, data: bytes) -> None:
    with open(str(path), "r+b") as f:
        f.seek(offset)
        f.write(data)


def read_bytes(path: Path, offset: int, length: int) -> bytes:
    with open(str(path), "rb") as f:
        f.seek(offset)
        return f.read(length)


EXFAT_HEAP = 256          # ClusterHeapOffset, in volume sectors
EXFAT_ROOT_CLUSTER = 4


def exfat_boot_sector(volume_bytes: int, bpss: int = 9, spcs: int = 3,
                      root_cluster: int = EXFAT_ROOT_CLUSTER) -> bytes:
    """An exFAT main boot sector (Microsoft's exFAT specification, 3.1)."""
    b = bytearray(1 << bpss)
    b[0:3] = b"\xeb\x76\x90"
    b[3:11] = b"EXFAT   "
    vol = volume_bytes >> bpss
    heap = EXFAT_HEAP
    clusters = max(0, (vol - heap) >> spcs)
    struct.pack_into("<QQIIIIIIHHBBBB", b, 64, 0, vol, 128, 128, heap, clusters,
                     root_cluster, 0x1234ABCD, 0x0100, 0, bpss, spcs, 1, 0x80)
    b[510:512] = b"\x55\xaa"
    return bytes(b)


def exfat_label_entry(label: str) -> bytes:
    e = bytearray(32)
    e[0] = 0x83
    e[1] = len(label)
    enc = label.encode("utf-16-le")
    e[2:2 + len(enc)] = enc
    return bytes(e)


def write_exfat(path: Path, offset: int, volume_bytes: int, label: str, *,
                bpss: int = 9, spcs: int = 3, complete: bool = True) -> None:
    """A small exFAT at byte OFFSET of PATH: the main and backup boot sectors
    (sectors 0 and 12) and, when complete, the root directory's first entries
    (volume label, allocation bitmap, up-case table)."""
    ss = 1 << bpss
    boot = exfat_boot_sector(volume_bytes, bpss, spcs)
    patch_bytes(path, offset, boot)
    patch_bytes(path, offset + 12 * ss, boot)
    if not complete:
        return
    root = EXFAT_HEAP + ((EXFAT_ROOT_CLUSTER - 2) << spcs)
    entries = (exfat_label_entry(label) if label else bytes([0x03]) + bytes(31))
    entries += bytes([0x81]) + bytes(31) + bytes([0x82]) + bytes(31)
    patch_bytes(path, offset + root * ss, entries.ljust(1 << (bpss + spcs), b"\0"))


def sniff_exfat(path: Path, offset: int) -> Optional[Dict[str, Any]]:
    """The exFAT at byte OFFSET as the fake kernel's "fs" record, or None."""
    boot = read_bytes(path, offset, 512)
    if len(boot) < 512 or boot[3:11] != b"EXFAT   " or boot[510:512] != b"\x55\xaa":
        return None
    heap, = struct.unpack_from("<I", boot, 88)
    root_cluster, = struct.unpack_from("<I", boot, 96)
    bpss, spcs = boot[108], boot[109]
    root = heap + ((root_cluster - 2) << spcs)
    d = read_bytes(path, offset + (root << bpss), 4096)
    label = ""
    seen = set()
    for i in range(0, len(d) - 31, 32):
        if d[i] == 0:
            break
        seen.add(d[i])
        if d[i] == 0x83:
            label = d[i + 2:i + 2 + 2 * d[i + 1]].decode("utf-16-le")
    # Complete: the root directory has its allocation bitmap and up-case table.
    return {"type": "exfat", "label": label, "complete": {0x81, 0x82} <= seen}


# --- tools ------------------------------------------------------------------------

class PowerCut(SystemExit):
    pass


def root_dir() -> Path:
    return Path(os.environ.get("THIMSY_ROOT") or "/")


def log_call(tool: str, args: List[str], note: str = "") -> None:
    """One line per call: TOOL, NOTE, ARGS... separated by tabs (the sh fakes
    in test_rcs.py write the same)."""
    path = os.environ.get("THIMSY_FAKE_LOG")
    if not path:
        return
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write("\t".join([tool, note] + args) + "\n")


def cut_point(step: str) -> Optional[str]:
    spec = os.environ.get("THIMSY_FAKE_POWERCUT", "")
    if ":" not in spec:
        return None
    want, mode = spec.split(":", 1)
    return mode if want == step else None


def power_cut() -> None:
    (root_dir() / ".powercut").write_text("cut\n", encoding="utf-8")
    raise PowerCut(1)


def dev_name(path: str) -> str:
    return Path(path).name


def node(path: str) -> Dict[str, Any]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def sfdisk(args: List[str]) -> int:
    root = root_dir()
    force = "--force" in args
    args = [a for a in args if a != "--force"]
    if args[:1] == ["--dump"] and len(args) == 2:
        d = load_disk(root, dev_name(args[1]))
        out = ["label: gpt", "label-id: 00000000-1111-2222-3333-444444444444",
               f"device: {args[1]}", "unit: sectors", f"first-lba: {d['first_lba']}",
               f"last-lba: {d['last_lba']}", f"sector-size: {d['sector_size']}", ""]
        for key in sorted(d["parts"], key=int):
            p = d["parts"][key]
            dev = args[1][: -len(d["name"])] + part_name(d["name"], int(key))
            line = (f"{dev} : start={p['start']:>12}, size={p['size']:>12}, "
                    f"type={p['type']}, uuid={p['uuid'].upper()}")
            if p.get("name"):
                line += f', name="{p["name"]}"'
            out.append(line)
        # Bytes: a text stream would write CRLF on Windows.
        sys.stdout.buffer.write(("\n".join(out) + "\n").encode("utf-8"))
        return 0
    if args[:2] == ["--relocate", "gpt-bak-std"] and len(args) == 3:
        disk = dev_name(args[2])
        mode = cut_point("relocate")
        d = load_disk(root, disk)
        if mode != "before":
            d["last_lba"] = d["size_sectors"] - 34
            save_disk(root, d)
        if mode:
            power_cut()
        return 0
    if args[:1] == ["--part-type"] and len(args) == 4:
        disk = dev_name(args[1])
        d = load_disk(root, disk)
        if args[2] not in d["parts"]:
            sys.stderr.write("sfdisk: no such partition\n")
            return 1
        mode = cut_point("retype")
        if mode != "before":
            d["parts"][args[2]]["type"] = args[3].upper()
            save_disk(root, d)
        if mode:
            power_cut()
        return 0
    if args[:1] == ["-N"] and len(args) == 3:
        n = args[1]
        disk = dev_name(args[2])
        if not force:
            sys.stderr.write("This disk is currently in use - use --force\n")
            return 1
        line = sys.stdin.read().strip()
        fields: Dict[str, str] = {}
        for f in line.split(","):
            k, _, v = f.strip().partition("=")
            fields[k.strip()] = v.strip().strip('"')
        d = load_disk(root, disk)
        start = int(fields["start"])
        if start < d["first_lba"] or start > d["last_lba"]:
            sys.stderr.write("sfdisk: start out of range\n")
            return 1
        for key, p in d["parts"].items():
            if key != n and p["start"] <= start < p["start"] + p["size"]:
                sys.stderr.write("sfdisk: overlaps partition %s\n" % key)
                return 1
        grain = 1048576 // d["sector_size"]
        end = (d["last_lba"] + 1) // grain * grain
        size = int(fields["size"]) if fields.get("size") else end - start
        mode = cut_point("create")
        if mode != "before":
            img = image_path(root, disk)
            fs = sniff_exfat(img, start * d["sector_size"]) if img.exists() else None
            d["parts"][n] = {"start": start, "size": size,
                             "type": fields.get("type", "").upper(),
                             "uuid": "c0ffee00-0000-4000-8000-%012d" % int(n),
                             "name": fields.get("name", ""), "fs": fs}
            save_disk(root, d)
            sync_kernel_view(root, disk)  # BLKPG: the kernel learns the entry
        if mode:
            power_cut()
        return 0
    sys.stderr.write("fake sfdisk: unsupported arguments %r\n" % (args,))
    return 2


def mkfs_exfat(args: List[str]) -> int:
    root = root_dir()
    label = ""
    if "-L" in args:
        label = args[args.index("-L") + 1]
    target = args[-1]
    info = node(target)
    d = load_disk(root, info["disk"])
    mode = cut_point("mkfs")
    p = d["parts"][str(info["n"])]
    img = image_path(root, info["disk"])
    if mode in ("torn", None, "after") and img.exists():
        ss = d["sector_size"]
        write_exfat(img, p["start"] * ss, p["size"] * ss, label,
                    bpss=ss.bit_length() - 1, spcs=3 if ss == 512 else 0,
                    complete=mode != "torn")
    if mode == "torn":
        p["fs"] = {"type": "exfat", "label": label, "complete": False}
        save_disk(root, d)
    elif mode != "before":
        p["fs"] = {"type": "exfat", "label": label, "complete": True}
        save_disk(root, d)
    if mode:
        power_cut()
    return 0


TOOLS = {
    "sfdisk": sfdisk,
    "mkfs.exfat": mkfs_exfat,
}


def main(argv: List[str]) -> int:
    tool, args = argv[0], argv[1:]
    if (root_dir() / ".powercut").exists():
        log_call(tool, args, "no power")
        return 1
    log_call(tool, args)
    fn = TOOLS[tool]
    try:
        return fn(args)
    except PowerCut:
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
