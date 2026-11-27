#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
"""Moves BlueZ and D-Bus out of a tier root into bluetooth.erofs (decision OA-15).

Usage:
  bt_pack.py --target DIR --work DIR --file-list FILE [--out DIR] [--mkfs PATH]
             [--epoch SECONDS] [--keep]
  bt_pack.py --target DIR --none [--out DIR]

Only devices whose device.json says "bluetooth": true get BlueZ and D-Bus. A
tier's root serves every device of its cards, so the tier does not keep them:
board/common/post-build.d/40-bt-pack.sh runs this in a tier tree, after the
overlay and the other post-build parts and before Buildroot makes the root
image, and card assembly (board/common/post-image.d/10-erofs.sh) unpacks
bluetooth.erofs into the roots of the devices with Bluetooth only.

1. The files: every file and symlink Buildroot's packages-file-list.txt
   (--file-list) gives to PACKAGES, and EXTRA (D35bluetooth from the overlay,
   the /var/lib/bluetooth link from post-build.d/15-state-links.sh). A listed
   file that is no longer in --target (a rebuild without per-package
   directories, where an earlier run moved it) is taken from the earlier pack
   in --work. Two kinds of listed file are not packed:
   - KEEP stays in the root: Buildroot's permissions table names the D-Bus
     activation helper ("f" type), and makedevs, which runs on the root
     after the post-build parts, fails the build when such a file is gone.
     The helper is harmless without dbus-daemon: only the bus runs it.
   - boot scripts (etc/init.d/S??*, as the dbus and bluez5_utils recipes
     install for BusyBox init) leave the root and stay out of the pack:
     rcS would start D-Bus and bluetoothd at every boot, and D35bluetooth
     starts them only when Bluetooth is turned on (plan 5.2).
2. The pack must hold the programs D35bluetooth and Nano run (REQUIRED).
3. No ELF file left in the root may need a library the pack takes (its
   DT_NEEDED entries): such a program would fail to start on a device
   without Bluetooth. Libraries opened with dlopen are not seen.
4. --work/bluetooth/ gets the files plus usr/share/thimsy/bluetooth.json (the
   packages and the file list); with --mkfs and --out it becomes
   --out/bluetooth.erofs, reproducibly (--epoch, every file owned by root,
   every directory 0755, so unpacking it over a root changes no directory's
   mode).
5. Unless --keep, the files and the boot scripts leave --target, with the
   directories that only held them (never a top-level directory or one
   directly under usr/ or var/).

--none (a tier built without BlueZ: no device on its cards has Bluetooth)
removes EXTRA from --target and any old --out/bluetooth.erofs, and makes no
pack. Exit status: 0 done, 1 an error, 2 usage. Standard library only;
Python 3.8 and later.
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
from typing import Dict, List, Optional, Set

# Buildroot package names, as packages-file-list.txt writes them.
PACKAGES = ("dbus", "bluez5_utils")
# Thimsy's own Bluetooth files in the tier root, outside those packages.
EXTRA = ("etc/init.d/deferred/D35bluetooth", "var/lib/bluetooth")
# Programs the pack must hold, by file name (D35bluetooth runs the first
# three, Nano's system actions bluetoothctl).
REQUIRED = ("dbus-daemon", "dbus-uuidgen", "bluetoothd", "bluetoothctl")
# Listed files that stay in the tier root: dbus.mk's DBUS_PERMISSIONS line
# ("/usr/libexec/dbus-daemon-launch-helper f 4750 0 dbus"; guess, from
# Buildroot 2025.02) makes makedevs fail if the file is missing.
KEEP = ("usr/libexec/dbus-daemon-launch-helper",)
PACK = "bluetooth"
MANIFEST = "usr/share/thimsy/bluetooth.json"
BOOT_SCRIPT = re.compile(r"^etc/init\.d/S[0-9][0-9][^/]*$")


def read_file_list(path: str, packages=PACKAGES) -> List[str]:
    """The paths packages-file-list.txt gives to PACKAGES ("pkg,./path" lines), sorted."""
    want = set(packages)
    out = set()
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            line = line.rstrip("\n")
            if "," not in line:
                continue
            pkg, rel = line.split(",", 1)
            if pkg not in want:
                continue
            rel = rel[2:] if rel.startswith("./") else rel.lstrip("/")
            if rel and ".." not in rel.split("/"):
                out.add(rel)
    return sorted(out)


def native(base: str, rel: str) -> str:
    return os.path.join(base, *rel.split("/"))


def copy_one(src: str, dst: str) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    if os.path.islink(src):
        os.symlink(os.readlink(src), dst)
    else:
        shutil.copy2(src, dst)


# -- ELF: DT_NEEDED --------------------------------------------------------------------------

def elf_needed(path: str) -> List[str]:
    """The DT_NEEDED names of an ELF file; [] for anything else or a file it cannot read."""
    try:
        with open(path, "rb") as f:
            head = f.read(64)
            if len(head) < 52 or head[:4] != b"\x7fELF" or head[4] not in (1, 2) or head[5] not in (1, 2):
                return []
            is64 = head[4] == 2
            e = "<" if head[5] == 1 else ">"
            if is64:
                phoff, = struct.unpack_from(e + "Q", head, 0x20)
                phentsize, phnum = struct.unpack_from(e + "HH", head, 0x36)
                ph_fmt, dyn_fmt = e + "IIQQQQQQ", e + "qQ"
            else:
                phoff, = struct.unpack_from(e + "I", head, 0x1C)
                phentsize, phnum = struct.unpack_from(e + "HH", head, 0x2A)
                ph_fmt, dyn_fmt = e + "IIIIIIII", e + "iI"
            if phnum == 0 or phnum > 512 or phentsize < struct.calcsize(ph_fmt):
                return []
            f.seek(phoff)
            table = f.read(phentsize * phnum)
            loads = []
            dynamic = None
            for i in range(phnum):
                raw = table[i * phentsize:(i + 1) * phentsize]
                if len(raw) < struct.calcsize(ph_fmt):
                    return []
                v = struct.unpack_from(ph_fmt, raw)
                if is64:
                    p_type, p_offset, p_vaddr, p_filesz = v[0], v[2], v[3], v[5]
                else:
                    p_type, p_offset, p_vaddr, p_filesz = v[0], v[1], v[2], v[4]
                if p_type == 1:
                    loads.append((p_vaddr, p_offset, p_filesz))
                elif p_type == 2:
                    dynamic = (p_offset, p_filesz)
            if dynamic is None:
                return []
            f.seek(dynamic[0])
            dyn = f.read(dynamic[1])
            size = struct.calcsize(dyn_fmt)
            needed, strtab = [], None
            for i in range(len(dyn) // size):
                tag, val = struct.unpack_from(dyn_fmt, dyn, i * size)
                if tag == 0:
                    break
                if tag == 1:
                    needed.append(val)
                elif tag == 5:
                    strtab = val
            if strtab is None or not needed:
                return []
            base = None
            for vaddr, off, filesz in loads:
                if vaddr <= strtab < vaddr + filesz:
                    base = strtab - vaddr + off
                    break
            if base is None:
                return []
            names = []
            for n in needed:
                f.seek(base + n)
                s = f.read(256).split(b"\0", 1)[0]
                names.append(s.decode("utf-8", "replace"))
            return names
    except (OSError, struct.error):
        return []


def is_library(rel: str) -> bool:
    name = rel.rsplit("/", 1)[-1]
    return name.startswith("lib") and (name.endswith(".so") or ".so." in name)


def users_of(target: str, libs: Set[str], skip: Set[str]) -> Dict[str, List[str]]:
    """ELF files under TARGET, outside SKIP, that need one of LIBS no file left there provides."""
    remaining: Set[str] = set()
    needs: Dict[str, List[str]] = {}
    for dirpath, dirs, files in os.walk(target):
        rel_dir = os.path.relpath(dirpath, target).replace(os.sep, "/")
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        for n in list(files) + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))]:
            rel = rel_dir + n
            if rel in skip:
                continue
            remaining.add(n)
            p = os.path.join(dirpath, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            hit = [x for x in elf_needed(p) if x in libs]
            if hit:
                needs[rel] = hit
    out = {}
    for rel, hit in needs.items():
        lost = [x for x in hit if x not in remaining]
        if lost:
            out[rel] = lost
    return out


# -- the pack --------------------------------------------------------------------------------

def build_tree(sources: Dict[str, str], tree: str, packages, kept=(), dropped=()) -> None:
    new = tree + ".new"
    if os.path.lexists(new):
        shutil.rmtree(new)
    for rel, src in sorted(sources.items()):
        copy_one(src, native(new, rel))
    manifest = {"packages": list(packages), "files": sorted(sources),
                "left_in_root": sorted(kept), "boot_scripts_dropped": sorted(dropped)}
    path = native(new, MANIFEST)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    # Directories 0755 whatever the build host's umask: fsck.erofs --overwrite
    # gives a root's shared directories (/, /etc, /usr) the pack's modes.
    os.chmod(new, 0o755)
    for dirpath, dirs, _ in os.walk(new):
        for d in dirs:
            p = os.path.join(dirpath, d)
            if not os.path.islink(p):
                os.chmod(p, 0o755)
    if os.path.lexists(tree):
        shutil.rmtree(tree)
    os.rename(new, tree)


def make_image(mkfs: str, tree: str, image: str, epoch: Optional[str]) -> None:
    cmd = [mkfs, "-zlz4hc", "-C16384", "--all-root"]
    if epoch:
        cmd.append("-T%s" % epoch)
    cmd += [image, tree]
    if os.path.exists(image):
        os.remove(image)
    subprocess.run(cmd, check=True)


def kept_dir(d: str) -> bool:
    """Directories never removed: the top level, and those directly under usr/ and var/."""
    parts = d.split("/")
    return len(parts) == 1 or (len(parts) == 2 and parts[0] in ("usr", "var"))


def remove_from_target(target: str, files: List[str]) -> None:
    dirs = set()
    for rel in files:
        p = native(target, rel)
        if os.path.lexists(p):
            os.remove(p)
        d = os.path.dirname(rel)
        while d:
            dirs.add(d)
            d = os.path.dirname(d)
    for d in sorted(dirs, key=lambda x: (-x.count("/"), x)):
        p = native(target, d)
        if not kept_dir(d) and os.path.isdir(p) and not os.path.islink(p) and not os.listdir(p):
            os.rmdir(p)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--target", required=True, help="the tier's target directory")
    ap.add_argument("--work", help="where the pack tree is made")
    ap.add_argument("--file-list", help="Buildroot's packages-file-list.txt")
    ap.add_argument("--out", help="where bluetooth.erofs is written (with --mkfs)")
    ap.add_argument("--mkfs", help="mkfs.erofs to make the image with")
    ap.add_argument("--epoch", help="timestamp for the image (SOURCE_DATE_EPOCH)")
    ap.add_argument("--keep", action="store_true", help="leave the files in --target")
    ap.add_argument("--none", action="store_true", help="no BlueZ in this tier: remove EXTRA, make no pack")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.target):
        print("bt_pack: no target directory %s" % args.target, file=sys.stderr)
        return 1
    if args.none:
        remove_from_target(args.target, list(EXTRA))
        if args.out:
            old = os.path.join(args.out, PACK + ".erofs")
            if os.path.lexists(old):
                os.remove(old)
        print("bt_pack: no BlueZ in this tier; D35bluetooth and /var/lib/bluetooth removed, no pack")
        return 0
    if not args.work or not args.file_list:
        print("bt_pack: --work and --file-list are needed (or --none)", file=sys.stderr)
        return 2
    if args.mkfs and not args.out:
        print("bt_pack: --mkfs needs --out", file=sys.stderr)
        return 2
    try:
        listed = read_file_list(args.file_list)
    except OSError as e:
        print("bt_pack: cannot read %s: %s" % (args.file_list, e), file=sys.stderr)
        return 1

    tree = os.path.join(args.work, PACK)
    kept = [r for r in listed if r in KEEP]
    boot = [r for r in listed if BOOT_SCRIPT.match(r)]
    sources: Dict[str, str] = {}
    for rel in sorted((set(listed) | set(EXTRA)) - set(kept) - set(boot)):
        for base in (args.target, tree):
            p = native(base, rel)
            if os.path.lexists(p) and (os.path.islink(p) or os.path.isfile(p)):
                sources[rel] = p
                break
    names = {r.rsplit("/", 1)[-1] for r in sources}
    missing = [n for n in REQUIRED if n not in names]
    if missing:
        print("bt_pack: the pack would lack %s (packages %s in %s)"
              % (", ".join(missing), ", ".join(PACKAGES), args.file_list), file=sys.stderr)
        return 1
    libs = {r.rsplit("/", 1)[-1] for r in sources if is_library(r)}
    # The kept helper is only ever run by dbus-daemon, so what it needs may go.
    broken = users_of(args.target, libs, set(sources) | set(kept) | set(boot))
    if broken:
        for rel, lost in sorted(broken.items()):
            print("bt_pack: /%s needs %s, which only the Bluetooth pack has" % (rel, ", ".join(lost)),
                  file=sys.stderr)
        return 1

    build_tree(sources, tree, PACKAGES, kept, boot)
    print("bt_pack: %d files into %s" % (len(sources), PACK))
    for rel in kept:
        print("bt_pack: /%s stays in the root (the permissions table names it)" % rel)
    for rel in boot:
        print("bt_pack: /%s removed: D35bluetooth starts Bluetooth, not rcS" % rel)
    if args.mkfs:
        os.makedirs(args.out, exist_ok=True)
        image = os.path.join(args.out, PACK + ".erofs")
        try:
            make_image(args.mkfs, tree, image, args.epoch)
        except (OSError, subprocess.CalledProcessError) as e:
            print("bt_pack: mkfs.erofs failed for %s: %s" % (image, e), file=sys.stderr)
            return 1
        print("bt_pack: wrote %s" % image)
    if not args.keep:
        remove_from_target(args.target, sorted(set(sources) | set(boot)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
