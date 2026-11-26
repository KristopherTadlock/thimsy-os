#!/usr/bin/env python3
"""Writes mods/retroarch-cores.src.lock: the source commit of each libretro core the
Linux image builds, read from the Android build's pinned binaries.

  core_sources.py [--lock FILE] [--cache DIR] [--out FILE] [--resolve] [--dry-run]

mods/retroarch-cores.lock pins the buildbot's Android binaries by sha256, not their
source commits, so the Linux recipes (linux/package/libretro-<core>/) have nothing to
build "at the commit the lock pins" (linux_handheld_plan.md 6.2) without this file
(docs/linux/IMPLEMENTATION.md E.3, decision I10). For each Linux core this reads the
core's zip from the build cache (<cache>/emulators/buildbot/<sha256>, where
scripts/refresh_cores.sh puts it), finds the git hash the core embeds in its version
string (retro_get_system_info's library_version usually ends in the short hash the
buildbot built), and writes one line per core:

  core  repository  commit  source

commit is a full 40-digit commit; "short:<hash>" when only the short hash is known
(--resolve looks it up on GitHub, which needs the network); or "pending:<reason>":
not-in-cache, bundled (the base image's own core ships, so no buildbot binary is
pinned), not-in-android-build (the core is Linux's alone and its commit is chosen
there), no-hash or ambiguous (the binary's strings gave no single hash). source is
the lock's file and sha256 the commit was read from ("-" for none). A full commit
already in the file is kept while its source and short hash still match, so running
this again offline loses nothing --resolve found.

linux/tools/lint_packages.py checks each recipe's _VERSION against this file and
reports the cores as pending until their commit is full. Never writes to the cache.
Standard library only; Python 3.8 and later.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.request
import zipfile
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "mods", "retroarch-cores.lock")
OUT = os.path.join(ROOT, "mods", "retroarch-cores.src.lock")
CACHE = os.path.join(ROOT, "work", "cache")

# The Linux image's cores (IMPLEMENTATION.md E.3): library name -> (repository, the
# Android lock's file for the same source, or None). The repositories are the ones
# linux/package/libretro-<core>/ builds from; linux/tests/test_packages.py keeps the
# two lists equal.
LINUX_CORES: Dict[str, Tuple[str, Optional[str]]] = {
    "snes9x": ("https://github.com/libretro/snes9x", "snes9x_libretro_android.so.zip"),
    "genesis_plus_gx": ("https://github.com/libretro/Genesis-Plus-GX", "genesis_plus_gx_libretro_android.so.zip"),
    "picodrive": ("https://github.com/libretro/picodrive", "picodrive_libretro_android.so.zip"),
    "mgba": ("https://github.com/libretro/mgba", "mgba_libretro_android.so.zip"),
    "sameboy": ("https://github.com/libretro/SameBoy", "sameboy_libretro_android.so.zip"),
    "mesen": ("https://github.com/libretro/Mesen", "mesen_libretro_android.so.zip"),
    "fceumm": ("https://github.com/libretro/libretro-fceumm", "fceumm_libretro_android.so.zip"),
    "pcsx_rearmed": ("https://github.com/libretro/pcsx_rearmed", "pcsx_rearmed_libretro_android.so.zip"),
    "fbneo": ("https://github.com/libretro/FBNeo", "fbneo_libretro_android.so.zip"),
    "mame2003_plus": ("https://github.com/libretro/mame2003-plus-libretro", "mame2003_plus_libretro_android.so.zip"),
    "mamearcade": ("https://github.com/libretro/mame", "mamearcade_libretro_android.so.zip"),
    "stella": ("https://github.com/stella-emu/stella", "stella_libretro_android.so.zip"),
    "prosystem": ("https://github.com/libretro/prosystem-libretro", "prosystem_libretro_android.so.zip"),
    "a5200": ("https://github.com/libretro/a5200", "a5200_libretro_android.so.zip"),
    "gearcoleco": ("https://github.com/drhelius/Gearcoleco", "gearcoleco_libretro_android.so.zip"),
    "bluemsx": ("https://github.com/libretro/blueMSX-libretro", "bluemsx_libretro_android.so.zip"),
    "mednafen_vb": ("https://github.com/libretro/beetle-vb-libretro", "mednafen_vb_libretro_android.so.zip"),
    "mednafen_ngp": ("https://github.com/libretro/beetle-ngp-libretro", "mednafen_ngp_libretro_android.so.zip"),
    "mednafen_pce_fast": ("https://github.com/libretro/beetle-pce-fast-libretro",
                          "mednafen_pce_fast_libretro_android.so.zip"),
    "mednafen_wswan": ("https://github.com/libretro/beetle-wswan-libretro", "mednafen_wswan_libretro_android.so.zip"),
    "mednafen_lynx": ("https://github.com/libretro/beetle-lynx-libretro", "mednafen_lynx_libretro_android.so.zip"),
    "fake08": ("https://github.com/jtothebell/fake-08", "fake08_libretro_android.so.zip"),
    "opera": ("https://github.com/libretro/opera-libretro", "opera_libretro_android.so.zip"),
    "yabasanshiro": ("https://github.com/libretro/yabause", "yabasanshiro_libretro_android.so.zip"),
    # The same source as Android's mupen64plus_next_gles3 (plan 6.2's alias).
    "mupen64plus_next": ("https://github.com/libretro/mupen64plus-libretro-nx",
                         "mupen64plus_next_gles3_libretro_android.so.zip"),
    "melondsds": ("https://github.com/JesseTG/melonds-ds", None),
    "flycast": ("https://github.com/flyinghead/flycast", "flycast_libretro_android.so.zip"),
}

HEADER = """\
# The source commit of each libretro core the Linux image builds (decision I10), read
# from the Android build's binaries that mods/retroarch-cores.lock pins. Written by
# scripts/core_sources.py; don't edit. Columns: core, repository, commit, source.
# commit: a full commit; short:<hash> until --resolve looks it up (network);
# pending:<reason> when none could be read. source: <lock file>@<sha256>, or "-".
"""

FULL_RE = re.compile(r"^[0-9a-f]{40}$")
# "1.62.3 a1b2c3d", "v2.0-rc1 a1b2c3d", "1.0.0-a1b2c3d", "a1b2c3d" (the hash at the end).
VERSIONED_RE = re.compile(r"^v?\d+(?:\.\w+)+[-\w.]*[\s\-_]+g?([0-9a-f]{7,12})$")
BARE_RE = re.compile(r"^\s*g?([0-9a-f]{7,12})\s*$")
STRING_RE = re.compile(rb"[\x20-\x7e]{4,200}")


def read_lock(path: str) -> Dict[str, Tuple[str, Optional[str]]]:
    """mods/retroarch-cores.lock: file -> (sha256 or "bundled", url)."""
    out: Dict[str, Tuple[str, Optional[str]]] = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            out[parts[0]] = (parts[1] if len(parts) > 1 else "", parts[2] if len(parts) > 2 else None)
    return out


def read_src_lock(path: str) -> Dict[str, Tuple[str, str, str]]:
    """An existing src.lock: core -> (repository, commit, source)."""
    out: Dict[str, Tuple[str, str, str]] = {}
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 4:
                out[parts[0]] = (parts[1], parts[2], parts[3])
    return out


def hash_candidates(data: bytes) -> Tuple[List[str], List[str]]:
    """Short hashes in a binary's strings: (version-prefixed ones, bare ones), each sorted.
    A hash needs a digit and a letter, which leaves out words and plain numbers; game
    databases' md5 sums (32 digits) are longer than any short hash."""
    versioned, bare = set(), set()
    for mo in STRING_RE.finditer(data):
        s = mo.group().decode("ascii").strip()
        for regex, bucket in ((VERSIONED_RE, versioned), (BARE_RE, bare)):
            m = regex.match(s)
            if m:
                h = m.group(1)
                if re.search(r"[0-9]", h) and re.search(r"[a-f]", h):
                    bucket.add(h)
                break
    return sorted(versioned), sorted(bare)


def commit_from_binary(data: bytes) -> str:
    versioned, bare = hash_candidates(data)
    for found in (versioned, bare):
        if len(found) == 1:
            return "short:" + found[0]
        if len(found) > 1:
            return "pending:ambiguous"
    return "pending:no-hash"


def core_binary(zip_path: str) -> bytes:
    """The .so inside a buildbot zip (the largest member, normally its only one)."""
    with zipfile.ZipFile(zip_path) as z:
        members = [i for i in z.infolist() if i.filename.endswith(".so")] or z.infolist()
        if not members:
            raise ValueError("%s is empty" % zip_path)
        best = max(members, key=lambda i: i.file_size)
        return z.read(best)


def resolve_short(repo: str, short: str, timeout: float = 20.0) -> Optional[str]:
    """The full commit of SHORT in REPO (a github.com URL), from GitHub's API."""
    mo = re.match(r"^https://github\.com/([^/]+)/([^/]+?)(?:\.git)?$", repo)
    if not mo:
        return None
    url = "https://api.github.com/repos/%s/%s/commits/%s" % (mo.group(1), mo.group(2), short)
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                               "User-Agent": "core-sources-script"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        sha = json.load(r).get("sha", "")
    return sha if FULL_RE.match(sha) and sha.startswith(short) else None


def compute(lock: Dict[str, Tuple[str, Optional[str]]], cache: str,
            old: Dict[str, Tuple[str, str, str]], resolve: bool = False,
            log=print) -> List[Tuple[str, str, str, str]]:
    rows = []
    buildbot = os.path.join(cache, "emulators", "buildbot")
    for core in sorted(LINUX_CORES):
        repo, zip_name = LINUX_CORES[core]
        if zip_name is None:
            commit, source = "pending:not-in-android-build", "-"
        elif zip_name not in lock:
            commit, source = "pending:not-in-lock", "-"
        else:
            sha = lock[zip_name][0]
            if sha == "bundled":
                commit, source = "pending:bundled", "%s@bundled" % zip_name
            else:
                source = "%s@%s" % (zip_name, sha)
                path = os.path.join(buildbot, sha)
                if not os.path.isfile(path):
                    commit = "pending:not-in-cache"
                else:
                    try:
                        commit = commit_from_binary(core_binary(path))
                    except (OSError, ValueError, zipfile.BadZipFile) as e:
                        log("%s: cannot read %s: %s" % (core, path, e))
                        commit = "pending:unreadable"
        prev = old.get(core)
        if commit.startswith("short:") and prev and prev[0] == repo and prev[2] == source \
                and FULL_RE.match(prev[1]) and prev[1].startswith(commit[len("short:"):]):
            commit = prev[1]
        if resolve and commit.startswith("short:"):
            try:
                full = resolve_short(repo, commit[len("short:"):])
            except (OSError, ValueError) as e:
                log("%s: cannot resolve %s: %s" % (core, commit, e))
                full = None
            if full:
                commit = full
        rows.append((core, repo, commit, source))
    return rows


def render(rows: List[Tuple[str, str, str, str]]) -> str:
    width = [max(len(r[i]) for r in rows) for i in range(3)]
    lines = [HEADER.rstrip("\n")]
    for r in rows:
        lines.append("  ".join([r[0].ljust(width[0]), r[1].ljust(width[1]), r[2].ljust(width[2]), r[3]]))
    return "\n".join(lines) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lock", default=LOCK, help="mods/retroarch-cores.lock")
    ap.add_argument("--cache", default=os.environ.get("THIMSY_CACHE", CACHE),
                    help="the build's cache (default work/cache)")
    ap.add_argument("--out", default=OUT, help="the file to write")
    ap.add_argument("--resolve", action="store_true", help="look short hashes up on GitHub (network)")
    ap.add_argument("--dry-run", action="store_true", help="print the file instead of writing it")
    args = ap.parse_args(argv)
    try:
        lock = read_lock(args.lock)
    except OSError as e:
        print("core_sources: %s" % e, file=sys.stderr)
        return 1
    rows = compute(lock, args.cache, read_src_lock(args.out), resolve=args.resolve,
                   log=lambda m: print("core_sources: " + m, file=sys.stderr))
    text = render(rows)
    if args.dry_run:
        sys.stdout.write(text)
    else:
        tmp = args.out + ".partial"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, args.out)
    done = sum(1 for r in rows if FULL_RE.match(r[2]))
    short = sum(1 for r in rows if r[2].startswith("short:"))
    print("core_sources: %d cores: %d full commits, %d short hashes, %d pending%s"
          % (len(rows), done, short, len(rows) - done - short,
             "" if args.dry_run else "; wrote " + args.out), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
