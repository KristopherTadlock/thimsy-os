#!/usr/bin/env python3
"""Write a family's boot FIT: the .its, the FIT (through mkimage), the header.

Design: E.6 of docs/linux/IMPLEMENTATION.md and plan 8.2. The `boot`
partition holds a 512-byte header followed by a FIT of the zstd kernel and
the device's DTB, with no hash nodes (a hash over the kernel is the work this
path exists to delete; bootm treats hashes as optional). U-Boot's boot script
finds `boot` by GPT name, reads the header, checks the magic, reads exactly
the FIT's sectors and runs bootm (FlipBase's docs/uboot.md, "How this build
works", and board/my355/uboot.env).

Subcommands:
  its    write the .its for one device of a family (golden-tested)
  build  compress the kernel if needed, write the .its, run mkimage when it
         is on PATH, read the FIT back, and write boot.img (header + FIT)
  wrap   put the header in front of a FIT made elsewhere
  check  read a boot.img (or a bare FIT) back and check its nodes

Bootargs: the FIT's configuration node carries a `bootargs` property, the
family's static command line (init=/init and the EROFS root options; no
root=). bootm does not read it: the family's boot script reads it from the
loaded FIT with `fdt get value` and appends root=PARTUUID= of the boot
disk's entry 3 (from `part uuid`), which is what S00base resolves the boot
disk from (E.5). The DTB is not edited.

Exit codes: 0 ok; 1 a check failed; 2 usage or input error; 3 a tool
(mkimage, zstd) is missing.

Python 3.8+, standard library only.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from typing import Any, Dict, List, Optional, Tuple

SECTOR = 512
MIB = 1024 * 1024
ZSTD_MAGIC = bytes((0x28, 0xB5, 0x2F, 0xFD))
FDT_MAGIC = 0xD00DFEED
ARM64_IMAGE_MAGIC = 0x644D5241  # "ARM\x64" at offset 56 of an arm64 Image
KERNEL_NAME = "Image.zst"
# FlipBase's encoder settings, tuned for U-Boot's decoder (docs/uboot.md, "zstd").
ZSTD_ARGS = ["--ultra", "-22", "--zstd=wlog=18,mml=6", "--no-check"]

# Family facts. Device facts (the DTB) come from the device's JSON.
FAMILIES: Dict[str, Dict[str, Any]] = {
    "rk3566": {
        "arch": "arm64",
        # DRAM map (FlipBase's, without OP-TEE): the kernel decompressed at
        # 32 MiB (2 MiB-aligned), the FIT read to 160 MiB, its header at 192 MiB.
        "kernel_load": 0x02000000,
        "fit_load": 0x0A000000,
        "header_load": 0x0C000000,
        "boot_slot": 40 * MIB,
        "bootargs": "init=/init rootfstype=erofs ro rootwait quiet loglevel=3",
        "magic": b"THIMSFIT",
    },
}
NO_FIT = {"qemu-aarch64-virt": "QEMU boots the kernel with -kernel (E.2); it has no FIT"}


class FitError(Exception):
    pass


class ToolMissing(Exception):
    pass


# --------------------------------------------------------------------------
# The 512-byte header


def make_header(fit_len: int, magic: bytes) -> bytes:
    """The sector in front of the FIT.

    Read from FlipBase's boot script (board/my355/uboot.env): the script reads
    the partition's first sector, compares the u32 words at +0 and +4 with the
    magic (`MY355FIT` there) and reads `*(u32 *)(hdr + 8)` sectors of FIT from
    the next sector.

    TODO(verify against FlipBase tools/mkfit.py at subtree merge): that the
    rest of the sector is zero (no length in bytes, checksum or version), and
    that the count is rounded up to whole sectors. Thimsy's magic differs
    from FlipBase's (its boot script is the family's own).
    """
    if len(magic) != 8:
        raise FitError("the header magic must be 8 bytes")
    sectors = (fit_len + SECTOR - 1) // SECTOR
    if sectors > 0xFFFFFFFF:
        raise FitError("FIT too large for the header")
    return (magic + struct.pack("<I", sectors)).ljust(SECTOR, b"\0")


def parse_header(blob: bytes) -> Tuple[bytes, int]:
    if len(blob) < SECTOR:
        raise FitError("shorter than the 512-byte header")
    magic = blob[:8]
    (sectors,) = struct.unpack_from("<I", blob, 8)
    if any(blob[12:SECTOR]):
        # TODO(verify against FlipBase tools/mkfit.py): may carry more fields.
        raise FitError("header bytes 12-511 are not zero")
    return magic, sectors


def magic_words(magic: bytes) -> List[str]:
    """The two u32 values a U-Boot script compares (setexpr.l reads LE)."""
    return ["%08x" % struct.unpack_from("<I", magic, i)[0] for i in (0, 4)]


# --------------------------------------------------------------------------
# A small flattened-device-tree reader (a FIT is an FDT)


class Node:
    def __init__(self, name: str) -> None:
        self.name = name
        self.props: Dict[str, bytes] = {}
        self.children: Dict[str, "Node"] = {}

    def walk(self, path: str = ""):
        here = path + "/" + self.name if self.name else (path or "/")
        yield here, self
        for c in self.children.values():
            yield from c.walk(here if here != "/" else "")

    def str(self, prop: str) -> Optional[str]:
        v = self.props.get(prop)
        if v is None:
            return None
        return v.split(b"\0", 1)[0].decode("utf-8", "replace")

    def u32(self, prop: str) -> Optional[int]:
        v = self.props.get(prop)
        if v is None or len(v) < 4:
            return None
        if len(v) >= 8:
            return struct.unpack(">Q", v[:8])[0]
        return struct.unpack(">I", v[:4])[0]


def read_fdt(blob: bytes) -> Tuple[Node, int]:
    """Parse an FDT; returns the root node and the blob's total size."""
    if len(blob) < 40:
        raise FitError("too short for an FDT header")
    (magic, total, off_struct, off_strings, _rsv, version, _last, _cpu,
     size_strings, size_struct) = struct.unpack_from(">10I", blob)
    if magic != FDT_MAGIC:
        raise FitError("no FDT magic")
    if total > len(blob) or off_struct + size_struct > total or \
            off_strings + size_strings > total or version < 16:
        raise FitError("FDT header out of range (version %d)" % version)
    strings = blob[off_strings:off_strings + size_strings]
    pos = off_struct
    end = off_struct + size_struct
    stack: List[Node] = []
    root: Optional[Node] = None

    def name_at(off: int) -> str:
        z = strings.find(b"\0", off)
        return strings[off:z].decode("ascii", "replace")

    while pos < end:
        (tok,) = struct.unpack_from(">I", blob, pos)
        pos += 4
        if tok == 1:  # FDT_BEGIN_NODE
            z = blob.index(b"\0", pos)
            node = Node(blob[pos:z].decode("ascii", "replace"))
            pos = (z + 4) & ~3
            if stack:
                stack[-1].children[node.name] = node
            elif root is None:
                root = node
            else:
                raise FitError("FDT has two roots")
            stack.append(node)
        elif tok == 2:  # FDT_END_NODE
            if not stack:
                raise FitError("FDT: unbalanced END_NODE")
            stack.pop()
        elif tok == 3:  # FDT_PROP
            length, nameoff = struct.unpack_from(">II", blob, pos)
            pos += 8
            if not stack:
                raise FitError("FDT: property outside a node")
            stack[-1].props[name_at(nameoff)] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif tok == 4:  # FDT_NOP
            continue
        elif tok == 9:  # FDT_END
            break
        else:
            raise FitError("FDT: bad token 0x%x at %d" % (tok, pos - 4))
    if root is None or stack:
        raise FitError("FDT: structure block incomplete")
    return root, total


def image_data(blob: bytes, fit_size: int, node: Node) -> bytes:
    """An image's data: embedded, or external (mkimage -E / -p)."""
    if "data" in node.props:
        return node.props["data"]
    size = node.u32("data-size")
    if size is None:
        raise FitError("image %r has no data" % node.name)
    if "data-position" in node.props:
        off = node.u32("data-position") or 0
    elif "data-offset" in node.props:
        off = ((fit_size + 3) & ~3) + (node.u32("data-offset") or 0)
    else:
        raise FitError("image %r has data-size but no offset" % node.name)
    data = blob[off:off + size]
    if len(data) != size:
        raise FitError("image %r's external data runs past the file" % node.name)
    return data


def verify_fit(blob: bytes, *, expect_kernel: Optional[bytes] = None,
               expect_dtb: Optional[bytes] = None) -> Dict[str, Any]:
    """Check a FIT's nodes: zstd kernel, one DTB, bootargs, no hash nodes."""
    root, total = read_fdt(blob)
    problems: List[str] = []
    for path, node in root.walk():
        if node.name.startswith("hash") or node.name.startswith("signature"):
            problems.append("%s: a hash or signature node (the boot FIT has none)" % path)
    images = root.children.get("images")
    confs = root.children.get("configurations")
    if images is None or confs is None:
        raise FitError("not a FIT: no /images or /configurations")
    default = confs.str("default")
    conf = confs.children.get(default or "")
    if conf is None:
        raise FitError("configurations/default %r names no configuration" % default)
    kname, fname = conf.str("kernel"), conf.str("fdt")
    kernel = images.children.get(kname or "")
    fdt = images.children.get(fname or "")
    if kernel is None or fdt is None:
        raise FitError("configuration %r: kernel %r or fdt %r missing" % (default, kname, fname))
    kdata = image_data(blob, total, kernel)
    fdata = image_data(blob, total, fdt)
    if kernel.str("type") not in ("kernel", "kernel_noload"):
        problems.append("kernel image type is %r" % kernel.str("type"))
    if kernel.str("compression") != "zstd":
        problems.append("kernel compression is %r, not zstd" % kernel.str("compression"))
    if not kdata.startswith(ZSTD_MAGIC):
        problems.append("kernel data is not a zstd frame")
    if kernel.str("os") != "linux":
        problems.append("kernel os is %r" % kernel.str("os"))
    if kernel.u32("load") is None or kernel.u32("entry") is None:
        problems.append("kernel has no load or entry address")
    if fdt.str("type") != "flat_dt":
        problems.append("fdt image type is %r" % fdt.str("type"))
    if len(fdata) < 4 or struct.unpack(">I", fdata[:4])[0] != FDT_MAGIC:
        problems.append("fdt data is not a device tree blob")
    bootargs = conf.str("bootargs")
    if not bootargs:
        problems.append("configuration %r has no bootargs" % default)
    if expect_kernel is not None and kdata != expect_kernel:
        problems.append("kernel data differs from the input kernel")
    if expect_dtb is not None and fdata != expect_dtb:
        problems.append("fdt data differs from the input DTB")
    return {
        "ok": not problems, "problems": problems, "fdt_size": total,
        "default": default, "kernel": {"name": kname, "size": len(kdata),
                                       "load": kernel.u32("load"),
                                       "compression": kernel.str("compression")},
        "fdt": {"name": fname, "size": len(fdata), "description": fdt.str("description")},
        "bootargs": bootargs,
    }


# --------------------------------------------------------------------------
# Inputs


def load_device(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        dev = json.load(f)
    if not isinstance(dev, dict):
        raise FitError("%s: not a JSON object" % path)
    return dev


# device.json's "dtb" (linux/schema/device.schema.json): <vendor>/<name>.dtb.
SCHEMA_DTB = re.compile(r"^[a-z0-9-]+/[A-Za-z0-9._,+-]+\.dtb$")


def device_from_json(path: str, family: str) -> Tuple[str, str]:
    """(id, DTB file name) of a device.json (linux/schema/device.schema.json): "id", "family",
    and "dtb" as <vendor>/<name>.dtb, of which the FIT carries the file name (Buildroot copies
    DTBs to BINARIES_DIR without their directory)."""
    dev = load_device(path)
    dev_id = dev.get("id")
    if not isinstance(dev_id, str) or not dev_id:
        raise FitError("%s has no \"id\" (linux/schema/device.schema.json)" % path)
    if dev.get("family") != family:
        raise FitError("%s is for family %r, not %r" % (path, dev.get("family"), family))
    dtb = dev.get("dtb")
    if dtb is None:
        raise FitError("%s has \"dtb\": null (the firmware provides the device tree), so it "
                       "has no FIT" % path)
    if not isinstance(dtb, str) or not SCHEMA_DTB.match(dtb):
        raise FitError("%s: \"dtb\" must be <vendor>/<name>.dtb, got %r" % (path, dtb))
    return dev_id, dtb.rsplit("/", 1)[1]


def resolve(family: str, device: Optional[str], dtb: Optional[str],
            device_json: Optional[str], append: str = "") -> Dict[str, Any]:
    if family in NO_FIT:
        raise FitError("%s: %s" % (family, NO_FIT[family]))
    if family not in FAMILIES:
        raise FitError("unknown family %r (known: %s)" % (family, ", ".join(sorted(FAMILIES))))
    fam = FAMILIES[family]
    if device_json:
        # The schema has no bootargs: the family's come from FAMILIES, a device's own from
        # --bootargs-append.
        dev_id, dev_dtb = device_from_json(device_json, family)
        device = device or dev_id
        dtb = dtb or dev_dtb
    if not device or not dtb:
        raise FitError("need a device and its DTB (--device-json, or --device and --dtb)")
    if os.path.basename(dtb) != dtb or not dtb.endswith(".dtb"):
        raise FitError("dtb must be a file name ending .dtb, got %r" % dtb)
    bootargs = " ".join(x for x in (fam["bootargs"], append) if x)
    for word in bootargs.split():
        if word.startswith("root="):
            raise FitError("bootargs must not carry root=: the boot script appends "
                           "root=PARTUUID= of the boot disk")
    if "init=/init" not in bootargs.split():
        raise FitError("bootargs must carry init=/init (a disk root never tries /init)")
    cfg = dict(fam)
    cfg.update({"family": family, "device": device, "dtb": dtb, "bootargs": bootargs})
    return cfg


def _dts_str(s: str) -> str:
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def write_its(cfg: Dict[str, Any]) -> str:
    """The .its text. Paths in /incbin/ are basenames next to the .its."""
    load = "<0x%08x>" % cfg["kernel_load"]
    lines = [
        "/dts-v1/;",
        "",
        "/ {",
        "\tdescription = %s;" % _dts_str("Thimsy boot FIT: %s %s" % (cfg["family"], cfg["device"])),
        "\t#address-cells = <1>;",
        "",
        "\timages {",
        "\t\tkernel {",
        "\t\t\tdescription = \"Linux kernel\";",
        "\t\t\tdata = /incbin/(%s);" % _dts_str(KERNEL_NAME),
        "\t\t\ttype = \"kernel\";",
        "\t\t\tarch = %s;" % _dts_str(cfg["arch"]),
        "\t\t\tos = \"linux\";",
        "\t\t\tcompression = \"zstd\";",
        "\t\t\tload = %s;" % load,
        "\t\t\tentry = %s;" % load,
        "\t\t};",
        "",
        "\t\tfdt {",
        "\t\t\tdescription = %s;" % _dts_str(cfg["dtb"]),
        "\t\t\tdata = /incbin/(%s);" % _dts_str(cfg["dtb"]),
        "\t\t\ttype = \"flat_dt\";",
        "\t\t\tarch = %s;" % _dts_str(cfg["arch"]),
        "\t\t\tcompression = \"none\";",
        "\t\t};",
        "\t};",
        "",
        "\tconfigurations {",
        "\t\tdefault = \"conf\";",
        "",
        "\t\tconf {",
        "\t\t\tdescription = %s;" % _dts_str(cfg["device"]),
        "\t\t\tkernel = \"kernel\";",
        "\t\t\tfdt = \"fdt\";",
        "\t\t\tbootargs = %s;" % _dts_str(cfg["bootargs"]),
        "\t\t};",
        "\t};",
        "};",
        "",
    ]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Size checks against the family's DRAM map and slot


def arm64_image_size(data: bytes) -> Optional[int]:
    """image_size (with BSS) from an uncompressed arm64 Image header."""
    if len(data) >= 64 and struct.unpack_from("<I", data, 56)[0] == ARM64_IMAGE_MAGIC:
        size = struct.unpack_from("<Q", data, 16)[0]
        return size or None
    return None


def zstd_content_size(data: bytes) -> Optional[int]:
    """Frame_Content_Size from a zstd frame header, if the frame records it."""
    if len(data) < 6 or not data.startswith(ZSTD_MAGIC):
        return None
    fhd = data[4]
    fcs_flag, single, did_flag = fhd >> 6, (fhd >> 5) & 1, fhd & 3
    pos = 5 + (0 if single else 1) + (0, 1, 2, 4)[did_flag]
    n = (1 if single else 0, 2, 4, 8)[fcs_flag]
    if n == 0 or len(data) < pos + n:
        return None
    val = int.from_bytes(data[pos:pos + n], "little")
    return val + 256 if n == 2 else val


def check_map(cfg: Dict[str, Any], *, fit_len: int,
              kernel_size: Optional[int]) -> List[str]:
    problems = []
    if cfg["kernel_load"] % (2 * MIB):
        problems.append("kernel load address is not 2 MiB-aligned")
    fit_room = cfg["header_load"] - cfg["fit_load"]
    if fit_len > fit_room:
        problems.append("FIT is %d bytes; it is read to 0x%x and must end before the "
                        "header at 0x%x (%d bytes)" % (fit_len, cfg["fit_load"],
                                                       cfg["header_load"], fit_room))
    img_len = SECTOR + ((fit_len + SECTOR - 1) // SECTOR) * SECTOR
    if img_len > cfg["boot_slot"]:
        problems.append("boot.img is %d bytes; the boot partition holds %d"
                        % (img_len, cfg["boot_slot"]))
    if kernel_size is not None and cfg["kernel_load"] + kernel_size > cfg["fit_load"]:
        problems.append("the kernel decompresses to %d bytes at 0x%x, over the FIT at 0x%x"
                        % (kernel_size, cfg["kernel_load"], cfg["fit_load"]))
    return problems


# --------------------------------------------------------------------------
# Commands


def find_tool(name: str, override: Optional[str]) -> Optional[str]:
    if override is not None:
        return None if override in ("", "none") else override
    return shutil.which(name)


def prepare_kernel(kernel: str, out: str, zstd: Optional[str]) -> Optional[int]:
    """Write out/Image.zst; returns the decompressed size when known."""
    with open(kernel, "rb") as f:
        data = f.read()
    if data.startswith(ZSTD_MAGIC):
        shutil.copyfile(kernel, out)
        return zstd_content_size(data)
    size = arm64_image_size(data) or len(data)
    if not zstd:
        raise ToolMissing("zstd is not on PATH and %s is not zstd-compressed" % kernel)
    subprocess.run([zstd] + ZSTD_ARGS + ["-q", "-f", "-o", out, kernel], check=True)
    return size


def wrap(fit: bytes, magic: bytes) -> bytes:
    pad = (-len(fit)) % SECTOR
    return make_header(len(fit), magic) + fit + b"\0" * pad


def cmd_its(args: argparse.Namespace) -> int:
    cfg = resolve(args.family, args.device, args.dtb, args.device_json, args.bootargs_append)
    text = write_its(cfg)
    if args.output and args.output != "-":
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    cfg = resolve(args.family, args.device, args.dtb, args.device_json, args.bootargs_append)
    out = args.out_dir
    os.makedirs(out, exist_ok=True)
    kz = os.path.join(out, KERNEL_NAME)
    ksize = prepare_kernel(args.kernel, kz, find_tool("zstd", args.zstd))
    dtb_out = os.path.join(out, cfg["dtb"])
    if os.path.abspath(args.dtb_file) != os.path.abspath(dtb_out):
        shutil.copyfile(args.dtb_file, dtb_out)
    its = os.path.join(out, "boot.its")
    with open(its, "w", encoding="utf-8", newline="\n") as f:
        f.write(write_its(cfg))
    mkimage = find_tool("mkimage", args.mkimage)
    if mkimage is None:
        if args.its_only:
            print("wrote %s (mkimage not used)" % its)
            return 0
        raise ToolMissing("mkimage is not on PATH; wrote %s only" % its)
    itb = os.path.join(out, "boot.itb")
    env = dict(os.environ, SOURCE_DATE_EPOCH="0")   # reproducible: FIT timestamp 0
    subprocess.run([mkimage, "-f", "boot.its", "boot.itb"], cwd=out, env=env, check=True,
                   stdout=subprocess.DEVNULL)
    with open(itb, "rb") as f:
        fit = f.read()
    with open(kz, "rb") as f:
        kdata = f.read()
    with open(dtb_out, "rb") as f:
        ddata = f.read()
    res = verify_fit(fit, expect_kernel=kdata, expect_dtb=ddata)
    problems = res["problems"] + check_map(cfg, fit_len=len(fit), kernel_size=ksize)
    if res.get("bootargs") != cfg["bootargs"]:
        problems.append("bootargs read back differ from those written")
    if problems:
        for p in problems:
            print("FAIL: %s" % p)
        return 1
    with open(os.path.join(out, "boot.img"), "wb") as f:
        f.write(wrap(fit, cfg["magic"]))
    print("wrote %s (%d-byte FIT)" % (os.path.join(out, "boot.img"), len(fit)))
    return 0


def cmd_wrap(args: argparse.Namespace) -> int:
    fam = FAMILIES[args.family]
    with open(args.fit, "rb") as f:
        fit = f.read()
    read_fdt(fit)
    problems = check_map(fam, fit_len=len(fit), kernel_size=None)
    if problems:
        for p in problems:
            print("FAIL: %s" % p)
        return 1
    with open(args.output, "wb") as f:
        f.write(wrap(fit, fam["magic"]))
    return 0


def check_boot_img(blob: bytes, family: str) -> Dict[str, Any]:
    fam = FAMILIES[family]
    res: Dict[str, Any] = {"family": family}
    problems: List[str] = []
    if blob[:4] == struct.pack(">I", FDT_MAGIC):
        fit = blob
        res["header"] = None
    else:
        magic, sectors = parse_header(blob)
        res["header"] = {"magic": magic.decode("latin-1"), "sectors": sectors,
                         "magic_words": magic_words(magic)}
        if magic != fam["magic"]:
            problems.append("header magic %r, expected %r" % (magic, fam["magic"]))
        fit = blob[SECTOR:SECTOR + sectors * SECTOR]
        if len(fit) < sectors * SECTOR:
            problems.append("header says %d sectors; the file has fewer" % sectors)
    v = verify_fit(fit)
    res.update({k: v[k] for k in ("default", "kernel", "fdt", "bootargs", "fdt_size")})
    problems += v["problems"]
    if res["header"] is not None and \
            (v["fdt_size"] + SECTOR - 1) // SECTOR > res["header"]["sectors"]:
        problems.append("the FIT is longer than the header's sector count")
    problems += check_map(fam, fit_len=len(fit), kernel_size=None)
    res["problems"] = problems
    res["ok"] = not problems
    return res


def cmd_check(args: argparse.Namespace) -> int:
    with open(args.image, "rb") as f:
        blob = f.read()
    res = check_boot_img(blob, args.family)
    if args.json:
        print(json.dumps(res, indent=2, sort_keys=True))
    else:
        for p in res["problems"]:
            print("FAIL: %s" % p)
        print("%s: %s" % (args.image, "ok" if res["ok"] else "%d problem(s)" % len(res["problems"])))
    return 0 if res["ok"] else 1


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Thimsy's boot FIT (E.6).")
    sub = ap.add_subparsers(dest="cmd")
    sub.required = True

    def device_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--family", required=True)
        p.add_argument("--device-json", help="the device's device.json (its id, family and dtb)")
        p.add_argument("--device", help="the device id (overrides the JSON)")
        p.add_argument("--dtb", help="the DTB's file name (overrides the JSON)")
        p.add_argument("--bootargs-append", default="", help="appended to the bootargs")

    p = sub.add_parser("its", help="write the .its")
    device_args(p)
    p.add_argument("-o", "--output", help="the .its to write (default: stdout)")
    p.set_defaults(func=cmd_its)

    p = sub.add_parser("build", help="kernel + DTB -> boot.its, boot.itb, boot.img")
    device_args(p)
    p.add_argument("--kernel", required=True, help="arm64 Image, or Image.zst")
    p.add_argument("--dtb-file", required=True, help="the compiled DTB")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--its-only", action="store_true",
                   help="succeed after writing the .its when mkimage is missing")
    p.add_argument("--mkimage", help="mkimage to use ('none': do not run it)")
    p.add_argument("--zstd", help="zstd to use ('none': only accept Image.zst)")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("wrap", help="put the 512-byte header in front of a FIT")
    p.add_argument("--family", required=True, choices=sorted(FAMILIES))
    p.add_argument("fit")
    p.add_argument("output")
    p.set_defaults(func=cmd_wrap)

    p = sub.add_parser("check", help="read a boot.img or FIT back")
    p.add_argument("--family", required=True, choices=sorted(FAMILIES))
    p.add_argument("--json", action="store_true")
    p.add_argument("image")
    p.set_defaults(func=cmd_check)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except ToolMissing as exc:
        print("fit: %s" % exc, file=sys.stderr)
        return 3
    except FitError as exc:
        print("fit: %s" % exc, file=sys.stderr)
        return 1 if args.cmd in ("check", "wrap") else 2
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print("fit: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
