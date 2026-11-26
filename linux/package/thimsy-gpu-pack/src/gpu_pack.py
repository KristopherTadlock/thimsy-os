#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
"""Moves Mesa's runtime files out of a tier root into one GPU pack per driver.

Usage:
  gpu_pack.py --target DIR --work DIR [--out DIR] --driver NAME [--driver NAME ...]
              [--mkfs PATH] [--epoch SECONDS] [--keep]

Decision D25: a tier's userland carries no GPU driver; each driver set is a
separate EROFS image, gpu-<driver>.erofs, mounted over the tier root at boot
(the mount is the image assembly's, step G8). thimsy-gpu-pack.mk runs this as a
Buildroot target-finalize hook, after every package has installed and before
the root image is made:

1. Mesa's files (PATTERNS below: the DRI and Gallium drivers, EGL, GLES, GBM
   and its backends, glapi, Vulkan drivers and ICDs, drirc) are found under
   --target. Symlinks are kept as symlinks.
2. Each --driver gets --work/gpu-<driver>/, a copy of those files plus
   usr/share/thimsy/gpu.json: the driver's name, the environment the session
   sets for it (DRIVER_ENV) and the file list.
3. With --mkfs (mkfs.erofs) and --out, each tree becomes --out/gpu-<driver>.erofs,
   reproducibly (--epoch, all files owned by root).
4. Unless --keep, the files are removed from --target, with the directories
   that only held them.

One Mesa build gives one libgallium with every enabled Gallium driver in it, so
two packs from the same tree hold the same files and differ only in gpu.json;
smaller per-driver packs need one Mesa build per driver (a later step).

If --target holds no Mesa file but --work holds a pack from an earlier run (a
rebuild without per-package directories, where the first run already moved
them), that pack is reused. Exit status: 0 done, 1 an error, 2 usage.
Standard library only; Python 3.8 and later.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from typing import Dict, List, Optional

# Paths relative to the root, '/'-separated; '*' stays inside one path element,
# '**' crosses them. The merged /usr layout (BR2_ROOTFS_MERGED_USR) puts every
# library in usr/lib.
PATTERNS = (
    "usr/lib/dri/**",
    "usr/lib/gbm/**",
    "usr/lib/libgallium*.so*",
    "usr/lib/libEGL*.so*",
    "usr/lib/libGLESv1_CM*.so*",
    "usr/lib/libGLESv2*.so*",
    "usr/lib/libgbm.so*",
    "usr/lib/libglapi.so*",
    "usr/lib/libvulkan_*.so*",
    "usr/share/vulkan/**",
    "usr/share/drirc.d/**",
    "usr/share/glvnd/**",
)

# The environment thimsy-session gives Nano and the emulators for a pack
# (read from gpu.json). swrast: softpipe, as Mesa is built without LLVM there
# (decision I8).
DRIVER_ENV: Dict[str, Dict[str, str]] = {
    "panfrost": {},
    "swrast": {"GALLIUM_DRIVER": "softpipe"},
}

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")


def pattern_regex(pattern: str) -> "re.Pattern[str]":
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


REGEXES = [pattern_regex(p) for p in PATTERNS]


def mesa_files(target: str) -> List[str]:
    """The files and symlinks under TARGET that belong to Mesa, sorted."""
    found = []
    for dirpath, dirs, files in os.walk(target):
        rel_dir = os.path.relpath(dirpath, target).replace(os.sep, "/")
        rel_dir = "" if rel_dir == "." else rel_dir + "/"
        names = list(files) + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))]
        for n in names:
            rel = rel_dir + n
            if any(r.match(rel) for r in REGEXES):
                found.append(rel)
    return sorted(found)


def copy_one(src: str, dst: str) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(src):
        if os.path.lexists(dst):
            os.remove(dst)
        os.symlink(os.readlink(src), dst)
    else:
        shutil.copy2(src, dst)


def build_tree(target: str, files: List[str], tree: str, driver: str) -> None:
    if os.path.lexists(tree):
        shutil.rmtree(tree)
    for rel in files:
        copy_one(os.path.join(target, *rel.split("/")), os.path.join(tree, *rel.split("/")))
    manifest = {
        "driver": driver,
        "env": DRIVER_ENV.get(driver, {}),
        "files": files,
    }
    path = os.path.join(tree, "usr", "share", "thimsy", "gpu.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")


def make_image(mkfs: str, tree: str, image: str, epoch: Optional[str]) -> None:
    cmd = [mkfs, "-zlz4hc", "-C16384", "--all-root"]
    if epoch:
        cmd.append("-T%s" % epoch)
    cmd += [image, tree]
    if os.path.exists(image):
        os.remove(image)
    subprocess.run(cmd, check=True)


def remove_from_target(target: str, files: List[str]) -> None:
    dirs = set()
    for rel in files:
        p = os.path.join(target, *rel.split("/"))
        if os.path.lexists(p):
            os.remove(p)
        d = os.path.dirname(rel)
        while d:
            dirs.add(d)
            d = os.path.dirname(d)
    # Deepest first; only directories this left empty.
    for d in sorted(dirs, key=lambda x: (-x.count("/"), x)):
        p = os.path.join(target, *d.split("/"))
        if d.count("/") >= 2 and os.path.isdir(p) and not os.path.islink(p) and not os.listdir(p):
            os.rmdir(p)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--target", required=True, help="the tier's target directory")
    ap.add_argument("--work", required=True, help="where the pack trees are made")
    ap.add_argument("--out", help="where gpu-<driver>.erofs is written (with --mkfs)")
    ap.add_argument("--driver", action="append", default=[], help="a driver set (repeatable)")
    ap.add_argument("--mkfs", help="mkfs.erofs to make the images with")
    ap.add_argument("--epoch", help="timestamp for the images (SOURCE_DATE_EPOCH)")
    ap.add_argument("--keep", action="store_true", help="leave the files in --target")
    args = ap.parse_args(argv)

    drivers = list(dict.fromkeys(args.driver))
    if not drivers:
        print("gpu_pack: no --driver given", file=sys.stderr)
        return 2
    for d in drivers:
        if not NAME_RE.match(d):
            print("gpu_pack: bad driver name %r" % d, file=sys.stderr)
            return 2
    if args.mkfs and not args.out:
        print("gpu_pack: --mkfs needs --out", file=sys.stderr)
        return 2
    if not os.path.isdir(args.target):
        print("gpu_pack: no target directory %s" % args.target, file=sys.stderr)
        return 1

    files = mesa_files(args.target)
    trees = {d: os.path.join(args.work, "gpu-%s" % d) for d in drivers}
    if files:
        for d in drivers:
            build_tree(args.target, files, trees[d], d)
        print("gpu_pack: %d Mesa files into %s" % (len(files), ", ".join("gpu-%s" % d for d in drivers)))
    else:
        stale = [d for d in drivers
                 if not os.path.isfile(os.path.join(trees[d], "usr", "share", "thimsy", "gpu.json"))]
        if stale:
            print("gpu_pack: no Mesa files under %s and no earlier pack for %s"
                  % (args.target, ", ".join(stale)), file=sys.stderr)
            return 1
        print("gpu_pack: no Mesa files in the target; reusing the packs made earlier")

    if args.mkfs:
        os.makedirs(args.out, exist_ok=True)
        for d in drivers:
            image = os.path.join(args.out, "gpu-%s.erofs" % d)
            try:
                make_image(args.mkfs, trees[d], image, args.epoch)
            except (OSError, subprocess.CalledProcessError) as e:
                print("gpu_pack: mkfs.erofs failed for %s: %s" % (image, e), file=sys.stderr)
                return 1
            print("gpu_pack: wrote %s" % image)

    if files and not args.keep:
        remove_from_target(args.target, files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
