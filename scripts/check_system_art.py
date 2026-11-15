#!/usr/bin/env python3
"""Checks that every game system Nano ships has its platform art in every ES-DE theme the image ships.

usage: check_system_art.py NANO_SYSTEMS_JSON THEMES_DIR

NANO_SYSTEMS_JSON is the systems file nano_systems.py writes. THEMES_DIR is the image's
etc/nano_esde_themes: theme sets as folders or zips. Nano's ES-DE home draws a system as the art
its theme names after the system's ES-DE name (${system.theme}); a missing file shows the theme's
default art or nothing. Each system's ROM folder is its ES-DE name, as in nano-systems.txt. The
XMB, DSi and Minima homes draw the system's icon, which nano_systems.py checks.
"""

import json
import os
import sys
import zipfile

# Where each shipped theme keeps a system's art in its system view.
ART = {
    "slate-es-de": "{system}/images/logo.svg",
    "colorful-simplified-es-de": "_inc/systems/images/{system}.webp",
}


def theme_files(path):
    """The files of a theme set (a folder or a zip), relative to the folder holding capabilities.xml."""
    if os.path.isdir(path):
        return {os.path.relpath(os.path.join(d, f), path) for d, _, files in os.walk(path) for f in files}
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
    tops = {n[: -len("capabilities.xml")] for n in names
            if n == "capabilities.xml" or (n.endswith("/capabilities.xml") and n.count("/") == 1)}
    top = min(tops, key=len) if tops else ""
    return {n[len(top):] for n in names if n.startswith(top)}


def missing(systems, themes):
    """Problems, one per system and theme without art. themes maps a theme name to its files."""
    problems = []
    for name in sorted(themes):
        if name not in ART:
            problems.append(f"{name}: no art layout known for this theme; add it to ART in {os.path.basename(__file__)}")
            continue
        for system in systems:
            art = ART[name].format(system=system)
            if art not in themes[name]:
                problems.append(f"{name}: no art for {system} ({art})")
    return problems


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])
    systems_json, themes_dir = sys.argv[1:]
    systems = [s["romDir"] for s in json.load(open(systems_json))["systems"]]
    themes = {}
    for entry in sorted(os.listdir(themes_dir)):
        path = os.path.join(themes_dir, entry)
        name = entry[: -len(".zip")] if entry.endswith(".zip") else entry
        if os.path.isdir(path) or entry.endswith(".zip"):
            themes[name] = theme_files(path)
    if not themes:
        sys.exit(f"check_system_art: no themes in {themes_dir}")
    problems = missing(systems, themes)
    if problems:
        sys.exit("check_system_art: " + "\ncheck_system_art: ".join(problems))
    print(f"Nano: {len(systems)} game systems have art in {', '.join(sorted(themes))}")


if __name__ == "__main__":
    main()
