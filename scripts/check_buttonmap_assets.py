#!/usr/bin/env python3
"""Checks that a system image carries the files Nano's button map reads.

usage: check_buttonmap_assets.py SYSTEM_IMG

Nano reads the pad specs, the RetroPad tables and the browser controls from
/system/etc/nano_xmb/buttonmap, and the controller drawings from /system/etc/nano_xmb/controllers
(controllers.json and the SVGs it names). Without pads.json the RG Cube is taken for a standard pad
and the button map's chord moves to the wrong button; without the rest the map has nothing to draw.

SYSTEM_IMG is an ext4 image with the system as root. Needs debugfs on PATH.
"""

import json
import posixpath
import re
import subprocess
import sys

BUTTONMAP = "/system/etc/nano_xmb/buttonmap"
CONTROLLERS = "/system/etc/nano_xmb/controllers"
BUTTONMAP_FILES = ("pads.json", "retropad.json", "browser.json")
CATALOG = "controllers.json"


def debugfs(image, request):
    """debugfs's output for one request; exits with debugfs's reason when it cannot read the image."""
    proc = subprocess.run(["debugfs", "-R", request, image], capture_output=True)
    if proc.returncode:
        sys.exit("check_buttonmap_assets: debugfs exited %d on %s" % (proc.returncode, image))
    err = proc.stderr.decode("utf-8", "replace")
    why = [line for line in err.splitlines() if line.strip() and not re.match(r"debugfs [0-9]", line)]
    # debugfs exits 0 for a missing file, and for an image it cannot open, and says so on stderr only.
    if why and not any("File not found" in line for line in why):
        sys.exit("check_buttonmap_assets: debugfs could not read %s: %s" % (image, "; ".join(why)))
    return proc.stdout


def listing(image, folder):
    """{name: size} of the non-empty regular files in FOLDER, None when FOLDER is missing."""
    out = debugfs(image, "ls -p " + folder).decode("utf-8", "replace")
    files, seen = {}, False
    for line in out.splitlines():
        # /inode/mode/uid/gid/name/size/
        parts = line.split("/")
        if len(parts) != 8 or parts[0] or parts[7]:
            continue
        seen = True
        mode, name, size = parts[2], parts[5], parts[6]
        if mode.startswith("100") and size.isdigit() and int(size) > 0:
            files[name] = int(size)
    return files if seen else None


def drawings(catalog_text):
    """(paths, problems): the drawings controllers.json names, as absolute paths in the image."""
    try:
        files = [d["file"] for d in json.loads(catalog_text)["drawings"].values()]
    except (ValueError, KeyError, TypeError, AttributeError):
        files = []
    if not files:
        return [], ["%s/%s: no drawings list" % (CONTROLLERS, CATALOG)]
    paths, problems = [], []
    for f in files:
        path = posixpath.normpath(posixpath.join(CONTROLLERS, f)) if isinstance(f, str) else ""
        if path.startswith(CONTROLLERS + "/"):
            paths.append(path)
        else:
            problems.append("%s/%s: drawing %r is outside %s" % (CONTROLLERS, CATALOG, f, CONTROLLERS))
    return sorted(set(paths)), problems


def missing(listings, catalog_text):
    """What is missing, given LISTINGS ({folder: listing(), None when missing}) and controllers.json's
    text (None when it is missing)."""
    paths = [posixpath.join(BUTTONMAP, name) for name in BUTTONMAP_FILES]
    paths.append(posixpath.join(CONTROLLERS, CATALOG))
    problems = []
    if catalog_text is not None:
        more, problems = drawings(catalog_text)
        paths += more
    out = []
    for path in paths:
        folder, name = posixpath.split(path)
        if name not in (listings.get(folder) or {}):
            out.append(path)
    return out + problems


def check(image):
    """What SYSTEM_IMG lacks of the button map's files, [] when nothing."""
    listings = {BUTTONMAP: listing(image, BUTTONMAP), CONTROLLERS: listing(image, CONTROLLERS)}
    catalog = None
    if CATALOG in (listings[CONTROLLERS] or {}):
        catalog = debugfs(image, "cat " + posixpath.join(CONTROLLERS, CATALOG)).decode("utf-8", "replace")
        for path in drawings(catalog)[0]:
            folder = posixpath.dirname(path)
            if folder not in listings:
                listings[folder] = listing(image, folder)
    return missing(listings, catalog)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    gone = check(sys.argv[1])
    if gone:
        sys.exit("check_buttonmap_assets: %s lacks the button map's files:\n  %s"
                 % (sys.argv[1], "\n  ".join(gone)))
    print("the system image has the button map's files")


if __name__ == "__main__":
    main()
