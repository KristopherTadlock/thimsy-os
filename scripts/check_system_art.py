#!/usr/bin/env python3
"""Checks that every game system Nano ships has its platform art in every ES-DE theme the image ships,
and that every platform in Nano's emulator catalog has a real picture in Colorful.

usage: check_system_art.py NANO_SYSTEMS_JSON THEMES_DIR [CATALOG_DIR]

NANO_SYSTEMS_JSON is the systems file nano_systems.py writes. THEMES_DIR is the image's
etc/nano_esde_themes: theme sets as folders or zips. Nano's ES-DE home draws a system as the art
its theme names after the system's theme name (${system.theme}: esdeThemeSystemName() in Nano's
NanoEsdeSystemName.cpp, mirrored by theme_name() here); a missing file shows the theme's default
art or nothing. Each system's ROM folder is its ES-DE name, as in nano-systems.txt. The XMB, DSi
and Minima homes draw the system's icon, which nano_systems.py checks.

CATALOG_DIR is the image's etc/nano_xmb/daijishou. A system the user adds from it is named after
the platform's uniqueId, so each platform must have its own picture and metadata in the themes
CATALOG_ART lists, not the theme's default.
"""

import glob
import json
import os
import sys
import zipfile

# Where each shipped theme keeps a system's art in its system view.
ART = {
    "slate-es-de": "{system}/images/logo.svg",
    "colorful-simplified-es-de": "_inc/systems/images/{system}.webp",
}

# The themes that must draw every catalog platform with its own picture, and what that takes.
CATALOG_ART = {
    "colorful-simplified-es-de": ("_inc/systems/images/{system}.webp", "_inc/systems/metadata-global/{system}.xml"),
}

# esdeThemeSystemName()'s tables, from Nano's NanoEsdeSystemName.cpp and NanoEsdeSystemNames.inc;
# tests/unit/test_check_system_art.py checks them against those files.
ESDE_SYSTEMS = frozenset("""
    3do adam ags amiga amiga1200 amiga600 amigacd32 amstradcpc android androidapps androidgames
    apple2 apple2gs arcade arcadia archimedes arduboy astrocde atari2600 atari5200 atari7800
    atari800 atarijaguar atarijaguarcd atarilynx atarist atarixe atomiswave bbcmicro c64 cdimono1
    cdtv chailove channelf coco colecovision consolearcade cps cps1 cps2 cps3 crvision daphne
    desktop doom dos dragon32 dreamcast easyrpg electron emulators epic famicom fba fbneo fds flash
    fm7 fmtowns fpinball gamate gameandwatch gamecom gamegear gb gba gbc gc genesis gmaster gx4000
    intellivision j2me kodi laserdisc lcdgames lowresnx lutris lutro macintosh mame mame-advmame
    mark3 mastersystem megacd megacdjp megadrive megadrivejp megaduck mess model2 model3 moto msx
    msx1 msx2 msxturbor mugen multivision n3ds n64 n64dd naomi naomi2 naomigd nds neogeo neogeocd
    neogeocdjp nes ngage ngp ngpc odyssey2 openbor oric palm pc pc88 pc98 pcarcade pcengine
    pcenginecd pcfx pico8 plus4 pokemini ports ps2 ps3 ps4 psp psvita psx pv1000 quake samcoupe
    satellaview saturn saturnjp scummvm scv sega32x sega32xjp sega32xna segacd sfc sg-1000 sgb snes
    snesna solarus spectravideo steam stv sufami supergrafx supervision supracan switch symbian
    tanodragon tg16 tg-cd ti99 tic80 to8 triforce trs-80 type-x uzebox vectrex vic20 videopac
    vircon32 virtualboy vpinball vsmile wasm4 wii wiiu windows windows3x windows9x wonderswan
    wonderswancolor x1 x68000 xbox xbox360 xboxone zmachine zx81 zxnext zxspectrum
""".split())
FOLDER_ALIASES = {
    "ps": "psx", "ps1": "psx", "psone": "psx", "playstation": "psx", "fc": "nes", "sfc": "snes",
    "md": "megadrive", "gen": "genesis", "sms": "mastersystem", "ms": "mastersystem", "gg": "gamegear",
    "pce": "pcengine", "pcecd": "pcenginecd", "gw": "gameandwatch", "ws": "wonderswan",
    "wsc": "wonderswancolor", "vb": "virtualboy", "a2600": "atari2600", "a5200": "atari5200",
    "a7800": "atari7800", "a800": "atari800", "lynx": "atarilynx", "poke": "pokemini", "pm": "pokemini",
    "mdcd": "segacd", "scd": "segacd", "32x": "sega32x", "sat": "saturn", "dc": "dreamcast",
    "neocd": "neogeocd", "java": "j2me", "sg1000": "sg-1000", "ngpc": "ngpc", "ngp": "ngp",
}
CATALOG_IDS = {
    "coleco": "colecovision", "3ds": "n3ds", "tgcd": "tg-cd", "master": "mastersystem", "vita": "psvita",
    "cpc": "amstradcpc", "appleii": "apple2", "jaguar": "atarijaguar", "jaguarcd": "atarijaguarcd",
    "cdi": "cdimono1", "rpgmaker": "easyrpg", "astrocade": "astrocde",
}
THEME_RENAMES = {"astrocde": "astrocade"}
GAMES_FOR = {
    "genesismsu": "genesis", "snesmsu1": "snes", "pspminis": "psp", "wiiware": "wii", "ndsi": "nds",
    "segacd32x": "sega32x", "g7400": "videopac", "c128": "c64",
}


def _mapped(name, table):
    target = table.get(name.lower())
    return target if target in ESDE_SYSTEMS else name


def theme_name(rom_dir, shortname=""):
    """The name a theme draws a Nano system's art under, as esdeThemeSystemName() gives it."""
    raw = rom_dir or shortname
    canon = raw
    if raw and raw not in ESDE_SYSTEMS:
        low = raw.lower()
        canon = low if low in ESDE_SYSTEMS else _mapped(low, FOLDER_ALIASES)
        if canon not in ESDE_SYSTEMS:
            canon = raw
    name = canon if not canon or canon in ESDE_SYSTEMS else _mapped(canon, CATALOG_IDS)
    if name in THEME_RENAMES:
        return THEME_RENAMES[name]
    return name if not name or name in ESDE_SYSTEMS else _mapped(name, GAMES_FOR)


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
            art = ART[name].format(system=theme_name(system))
            if art not in themes[name]:
                problems.append(f"{name}: no art for {system} ({art})")
    return problems


def catalog_platforms(catalog_dir):
    """The uniqueId of every platform in the emulator catalog."""
    ids = set()
    for path in glob.glob(os.path.join(catalog_dir, "*.json")):
        with open(path, encoding="utf-8-sig") as f:
            uid = (json.load(f).get("platform") or {}).get("uniqueId")
        if uid:
            ids.add(uid)
    return sorted(ids)


def missing_catalog_art(platforms, themes):
    """Problems, one per catalog platform without its own picture or metadata in a CATALOG_ART theme."""
    problems = []
    for name in sorted(set(themes) & set(CATALOG_ART)):
        for uid in platforms:
            for pattern in CATALOG_ART[name]:
                path = pattern.format(system=theme_name(uid))
                if path not in themes[name]:
                    problems.append(f"{name}: catalog platform {uid} has no {path} (it would show the default)")
    return problems


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.strip().splitlines()[3])
    systems_json, themes_dir = sys.argv[1:3]
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
    platforms = []
    if len(sys.argv) == 4:
        platforms = catalog_platforms(sys.argv[3])
        if not platforms:
            sys.exit(f"check_system_art: no catalog platforms in {sys.argv[3]}")
        problems += missing_catalog_art(platforms, themes)
    if problems:
        sys.exit("check_system_art: " + "\ncheck_system_art: ".join(problems))
    print(f"Nano: {len(systems)} game systems have art in {', '.join(sorted(themes))}")
    if platforms:
        print(f"Nano: {len(platforms)} catalog platforms have pictures in "
              f"{', '.join(sorted(set(themes) & set(CATALOG_ART)))}")


if __name__ == "__main__":
    main()
