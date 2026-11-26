#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the external tree's package recipes (IMPLEMENTATION.md D.4, test_packages).

Usage:
  lint_packages.py [--strict] [--buildroot DIR] [-q]
  lint_packages.py --sync-cores      write each core's _VERSION from the source lock

For every package directory under package/ and gpl/package/ that has a Config.in:
- the Config.in is sourced from its Config.in menu (package/Config.in or
  gpl/package/Config.in) and defines BR2_PACKAGE_<NAME>;
- <pkg>.mk exists, starts with Buildroot's header block naming the package, sets
  only variables of its own prefix (<NAME>_...) apart from a few shared ones,
  sets _VERSION, _SITE, _LICENSE and _LICENSE_FILES, and ends with one
  $(eval $(<infrastructure>)) line of a known infrastructure;
- <pkg>.hash exists, except for a local site and for the tarballs linux/build.sh
  writes (sites under $(THIMSY_SRC_DIR), whose hash files build.sh writes too),
  and every line of it is a comment or "<algo> <hex> <file>" with a hex length
  that fits the algorithm.
For the libretro cores (package/libretro-*): the _VERSION equals the commit of
mods/retroarch-cores.src.lock for the core, and the _SITE names the lock's
repository (decision I10).
For thimsy-data: package/thimsy-data/assets.list equals what
tools/assets_allowlist.py generates from ASSETS.md, and every REPLACE or ASK path
is denied by it (decision I38).
For the tarball packages built from the repository (libthimsy, thimsy-nano), the
licence files exist in the repository's folder.

Some findings are "pending", not errors: work that waits on something outside
this step, reported every run until it is done. A core whose _VERSION is TODO or
whose lock entry is not a full commit, a source lock that does not exist yet, a
.hash file with no hash yet (they need a download), a licence file the recipe
names in a TODO. --strict makes pending findings fail too.

With --buildroot DIR (a Buildroot tree at the pinned version), Buildroot's own
utils/check-package runs on every recipe too.

Exit status: 0 no errors (pending allowed unless --strict), 1 errors, 2 usage.
Standard library only; Python 3.8 and later.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

LINUX = Path(__file__).resolve().parent.parent
REPO = LINUX.parent

INFRAS = {
    "generic-package", "cmake-package", "autotools-package", "meson-package",
    "python-package", "libretro-core-package", "libretro-cmake-core-package",
}
# Variables a recipe may set that are not its own: hooks lists and the cores'
# shared list (package/libretro/libretro.mk).
SHARED_VARS = {"TARGET_FINALIZE_HOOKS", "ROOTFS_PRE_CMD_HOOKS", "LIBRETRO_CORE_NAMES"}
HASH_LENGTHS = {"md5": 32, "sha1": 40, "sha224": 56, "sha256": 64, "sha384": 96, "sha512": 128}
# Packages built from the repository's own folders (E.1): package -> folder.
REPO_TARBALLS = {"libthimsy": "libthimsy", "thimsy-nano": "nano"}

HEADER_SEP = "#" * 80
ASSIGN_RE = re.compile(r"^([A-Za-z0-9_.-]+)\s*(\+=|\?=|:=|=)\s*(.*)$")
DEFINE_RE = re.compile(r"^define\s+([A-Za-z0-9_.-]+)\s*$")
EVAL_RE = re.compile(r"^\$\(eval \$\(([a-z0-9-]+)\)\)$")
CONFIG_RE = re.compile(r"^\s*(?:menu)?config\s+(BR2_[A-Za-z0-9_]+)\s*$")
SOURCE_RE = re.compile(r'^\s*source\s+"\$BR2_EXTERNAL_THIMSY_PATH/([^"]+)"\s*$')
GITHUB_RE = re.compile(r"^\$\(call github,([^,]+),([^,]+),")
FULL_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


class Report:
    def __init__(self) -> None:
        self.errors: List[str] = []
        self.pending: List[str] = []
        self.notes: List[str] = []

    def error(self, msg: str) -> None:
        self.errors.append(msg)

    def wait(self, msg: str) -> None:
        self.pending.append(msg)


class Recipe:
    """The variables a .mk file sets (first value of each), its defines and its eval line."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.lines = path.read_text(encoding="utf-8").split("\n")
        self.vars: Dict[str, str] = {}
        self.defines: List[str] = []
        self.evals: List[str] = []
        self.assigned: List[Tuple[int, str]] = []
        in_define = 0
        joined: List[Tuple[int, str]] = []
        buf, start = "", 0
        for n, raw in enumerate(self.lines, 1):
            if buf:
                buf += " " + raw.strip()
            else:
                buf, start = raw, n
            if buf.endswith("\\"):
                buf = buf[:-1].rstrip()
                continue
            joined.append((start, buf))
            buf = ""
        for n, line in joined:
            s = line.strip()
            if in_define:
                if s == "endef":
                    in_define -= 1
                elif DEFINE_RE.match(s):
                    in_define += 1
                continue
            mo = DEFINE_RE.match(s)
            if mo:
                self.defines.append(mo.group(1))
                in_define = 1
                continue
            if not s or s.startswith("#") or s.startswith(("ifeq", "ifneq", "ifdef", "ifndef", "else", "endif")):
                continue
            mo = EVAL_RE.match(s)
            if mo:
                self.evals.append(mo.group(1))
                continue
            mo = ASSIGN_RE.match(s)
            if mo:
                name, value = mo.group(1), re.sub(r"\s+", " ", mo.group(3)).strip()
                self.assigned.append((n, name))
                self.vars.setdefault(name, value)


def package_dirs(linux: Path) -> List[Path]:
    out = []
    for base in (linux / "package", linux / "gpl" / "package"):
        if base.is_dir():
            out.extend(sorted(p for p in base.iterdir() if (p / "Config.in").is_file()))
    return out


def prefix_of(name: str) -> str:
    return name.upper().replace("-", "_")


def sourced(linux: Path, menu: Path) -> List[str]:
    if not menu.is_file():
        return []
    out = []
    for line in menu.read_text(encoding="utf-8").splitlines():
        mo = SOURCE_RE.match(line)
        if mo:
            out.append(mo.group(1))
    return out


def check_header(rep: Report, rel: str, recipe: Recipe, name: str) -> None:
    want = [HEADER_SEP, "#", "# " + name, "#", HEADER_SEP, ""]
    if recipe.lines[:6] != want:
        rep.error("%s: must start with Buildroot's header block naming %s" % (rel, name))


def check_hash_file(rep: Report, rel: str, path: Path) -> int:
    """The number of hash lines; errors for malformed ones."""
    count = 0
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split()
        if len(parts) != 3 or parts[0] not in HASH_LENGTHS:
            rep.error("%s:%d: not '<algo> <hex> <file>' (algo one of %s)"
                      % (rel, n, ", ".join(sorted(HASH_LENGTHS))))
            continue
        if not re.match(r"^[0-9a-f]+$", parts[1]) or len(parts[1]) != HASH_LENGTHS[parts[0]]:
            rep.error("%s:%d: %s hash must be %d lowercase hex digits"
                      % (rel, n, parts[0], HASH_LENGTHS[parts[0]]))
            continue
        count += 1
    return count


def read_src_lock(path: Path) -> Dict[str, Tuple[str, str]]:
    out: Dict[str, Tuple[str, str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split()
        if len(parts) >= 3:
            out[parts[0]] = (parts[1], parts[2])
    return out


def site_repository(site: str) -> Optional[str]:
    """https://github.com/<owner>/<repo> for a github macro or a .git URL."""
    mo = GITHUB_RE.match(site)
    if mo:
        return "https://github.com/%s/%s" % (mo.group(1).strip(), mo.group(2).strip())
    mo = re.match(r"^(https://github\.com/[^/\s]+/[^/\s]+?)(?:\.git)?/?$", site)
    return mo.group(1) if mo else None


def core_name(pkg: str, recipe: Recipe) -> str:
    prefix = prefix_of(pkg)
    return recipe.vars.get(prefix + "_CORE") or pkg[len("libretro-"):].replace("-", "_")


def lint(linux: Path = LINUX, repo: Optional[Path] = REPO, rep: Optional[Report] = None) -> Report:
    rep = rep or Report()
    dirs = package_dirs(linux)
    srcs = set(sourced(linux, linux / "package" / "Config.in")) | \
        set(sourced(linux, linux / "gpl" / "package" / "Config.in"))
    top = sourced(linux, linux / "Config.in")
    for menu in ("package/Config.in", "gpl/package/Config.in"):
        if (linux / menu).is_file() and menu not in top:
            rep.error("Config.in does not source %s" % menu)
    src_lock_path = repo / "mods" / "retroarch-cores.src.lock" if repo else None
    src_lock: Optional[Dict[str, Tuple[str, str]]] = None
    if src_lock_path is not None and src_lock_path.is_file():
        src_lock = read_src_lock(src_lock_path)

    if not (linux / "package" / "libretro" / "libretro.mk").is_file():
        rep.error("package/libretro/libretro.mk (the cores' shared recipe) is missing")

    cores_pending = []
    for d in dirs:
        name = d.name
        base = d.parent.relative_to(linux).as_posix()
        rel_cfg = "%s/%s/Config.in" % (base, name)
        prefix = prefix_of(name)
        if rel_cfg not in srcs:
            rep.error("%s is not sourced from %s/Config.in" % (rel_cfg, base))
        symbols = {mo.group(1) for line in (d / "Config.in").read_text(encoding="utf-8").splitlines()
                   for mo in [CONFIG_RE.match(line)] if mo}
        if "BR2_PACKAGE_" + prefix not in symbols:
            rep.error("%s does not define BR2_PACKAGE_%s" % (rel_cfg, prefix))
        mk = d / (name + ".mk")
        rel_mk = "%s/%s/%s.mk" % (base, name, name)
        if not mk.is_file():
            rep.error("%s is missing" % rel_mk)
            continue
        recipe = Recipe(mk)
        check_header(rep, rel_mk, recipe, name)
        for n, var in recipe.assigned:
            if var not in SHARED_VARS and not var.startswith(prefix + "_"):
                rep.error("%s:%d: %s is not %s_..." % (rel_mk, n, var, prefix))
        for define in recipe.defines:
            if define not in SHARED_VARS and not define.startswith(prefix + "_"):
                rep.error("%s: define %s is not %s_..." % (rel_mk, define, prefix))
        if len(recipe.evals) != 1 or recipe.evals[0] not in INFRAS:
            rep.error("%s: needs exactly one $(eval $(<infrastructure>)) line of %s"
                      % (rel_mk, ", ".join(sorted(INFRAS))))
        else:
            last = [l.strip() for l in recipe.lines if l.strip()]
            if not last or not EVAL_RE.match(last[-1]):
                rep.error("%s: the $(eval ...) line must be the last one" % rel_mk)
        for field in ("VERSION", "SITE", "LICENSE", "LICENSE_FILES"):
            if not recipe.vars.get("%s_%s" % (prefix, field)):
                rep.error("%s: %s_%s is not set" % (rel_mk, prefix, field))

        version = recipe.vars.get(prefix + "_VERSION", "")
        site = recipe.vars.get(prefix + "_SITE", "")
        method = recipe.vars.get(prefix + "_SITE_METHOD", "")
        is_core = name.startswith("libretro-")
        is_repo_tarball = "$(THIMSY_SRC_DIR)" in site
        if is_repo_tarball and version != "$(THIMSY_GIT_SHA)":
            rep.error("%s: a build.sh tarball package's _VERSION must be $(THIMSY_GIT_SHA)" % rel_mk)
        if is_repo_tarball and method != "file":
            rep.error("%s: a build.sh tarball package needs _SITE_METHOD = file (legal-info "
                      "saves no source for a local site)" % rel_mk)

        hash_path = d / (name + ".hash")
        if method == "local" or is_repo_tarball:
            if hash_path.is_file():
                rep.error("%s: a %s package takes no .hash file here"
                          % (rel_mk, "local" if method == "local" else "build.sh tarball"))
        elif not hash_path.is_file():
            if is_core and "TODO" in version:
                pass  # reported with the core's commit below
            else:
                rep.error("%s/%s/%s.hash is missing" % (base, name, name))
        else:
            if check_hash_file(rep, "%s/%s/%s.hash" % (base, name, name), hash_path) == 0:
                rep.wait("%s: %s.hash has no hash yet (needs the first download)" % (name, name))
        # A build.sh tarball package's other downloads (build.sh appends these lines).
        extra = d / "extra-downloads.hash"
        if extra.is_file():
            if not is_repo_tarball:
                rep.error("%s/%s/extra-downloads.hash: only build.sh tarball packages take one"
                          % (base, name))
            elif check_hash_file(rep, "%s/%s/extra-downloads.hash" % (base, name), extra) == 0:
                rep.error("%s/%s/extra-downloads.hash has no hash" % (base, name))

        if is_core:
            core = core_name(name, recipe)
            repo_url = site_repository(site)
            if repo_url is None:
                rep.error("%s: _SITE must be a github site ($(call github,...) or a .git URL)" % rel_mk)
            if src_lock is None:
                cores_pending.append(core)
            elif core not in src_lock:
                rep.error("%s: core %s has no line in mods/retroarch-cores.src.lock" % (rel_mk, core))
            else:
                lock_repo, lock_commit = src_lock[core]
                if repo_url and lock_repo.rstrip("/") != repo_url:
                    rep.error("%s: _SITE is %s but the source lock says %s" % (rel_mk, repo_url, lock_repo))
                if not FULL_COMMIT_RE.match(lock_commit):
                    rep.wait("%s: the source lock has %s, not a full commit" % (name, lock_commit))
                elif version != lock_commit:
                    if "TODO" in version:
                        rep.wait("%s: _VERSION is TODO; the lock has %s (lint_packages.py --sync-cores)"
                                 % (name, lock_commit))
                    else:
                        rep.error("%s: _VERSION %s is not the source lock's %s" % (rel_mk, version, lock_commit))
        elif "TODO" in version:
            rep.wait("%s: _VERSION is TODO" % name)

        if repo is not None and name in REPO_TARBALLS:
            folder = repo / REPO_TARBALLS[name]
            if folder.is_dir():
                for f in recipe.vars.get(prefix + "_LICENSE_FILES", "").split():
                    if not (folder / f).is_file():
                        rep.error("%s: licence file %s/%s does not exist" % (rel_mk, REPO_TARBALLS[name], f))
            if name == "thimsy-nano":
                # Decision I41: nano/'s Apache-2.0 LICENSE and its NOTICE of the other licences.
                listed = recipe.vars.get(prefix + "_LICENSE_FILES", "").split()
                for f in ("LICENSE", "NOTICE"):
                    if f not in listed:
                        rep.error("%s: _LICENSE_FILES must list nano/%s (decision I41)" % (rel_mk, f))

    if cores_pending:
        rep.wait("mods/retroarch-cores.src.lock does not exist yet (decision I10): %d cores have no "
                 "commit: %s" % (len(cores_pending), " ".join(sorted(cores_pending))))

    data = linux / "package" / "thimsy-data" / "assets.list"
    if (linux / "package" / "thimsy-data").is_dir():
        check_assets(rep, linux, repo, data)
    return rep


def check_assets(rep: Report, linux: Path, repo: Optional[Path], data: Path) -> None:
    sys.path.insert(0, str(linux / "tools"))
    try:
        import assets_allowlist as aa  # type: ignore
    finally:
        sys.path.pop(0)
    if not data.is_file():
        rep.error("package/thimsy-data/assets.list is missing (tools/assets_allowlist.py)")
        return
    try:
        entries = aa.read_list(data)
    except ValueError as e:
        rep.error(str(e))
        return
    if not any(e.action == "keep" for e in entries):
        rep.error("package/thimsy-data/assets.list keeps nothing")
    assets = (repo / "ASSETS.md") if repo else None
    bp = (repo / "nano" / "Android.bp") if repo else None
    if assets is None or not assets.is_file() or bp is None or not bp.is_file():
        rep.notes.append("ASSETS.md or nano/Android.bp not found: assets.list not regenerated")
        return
    try:
        fresh = aa.build(assets, bp)
    except (OSError, ValueError, ImportError) as e:
        rep.error("assets_allowlist.py failed: %s" % e)
        return
    if data.read_text(encoding="utf-8").replace("\r\n", "\n") != fresh:
        rep.error("package/thimsy-data/assets.list is stale: run tools/assets_allowlist.py")
        return
    # Every REPLACE or ASK path of ASSETS.md is denied by the list (the deny lines
    # are the rows themselves, so check the decision for each).
    al = aa.Allowlist(entries)
    mapper = aa.Mapper.from_blueprint(bp)
    rows, _ = aa.read_assets(assets.read_text(encoding="utf-8"))
    zips: Dict[str, str] = {}
    for row in rows:
        for written in aa.row_paths(row, zips):
            inst = mapper.installed(written)
            if inst is None or row.kind != "deny" or aa.has_glob(inst):
                continue
            ok, _why = al.decide(inst)
            if ok:
                rep.error("assets.list would install %s, which ASSETS.md:%d marks %s"
                          % (inst, row.line_no, row.verdict))


def check_package_tool(rep: Report, linux: Path, buildroot: Path) -> None:
    tool = buildroot / "utils" / "check-package"
    if not tool.is_file():
        rep.error("%s does not exist" % tool)
        return
    files = [str(p) for d in package_dirs(linux) for p in sorted(d.iterdir())
             if p.suffix in (".mk", ".hash", ".in") or p.name == "Config.in"]
    files.append(str(linux / "package" / "libretro" / "libretro.mk"))
    proc = subprocess.run([sys.executable, str(tool), "--br2-external"] + files,
                          cwd=str(linux), capture_output=True, text=True)
    for line in (proc.stdout + proc.stderr).splitlines():
        if line.strip() and not line.startswith(("0 lines processed", "lines processed", "warnings generated")) \
                and re.search(r":\d+:|: ", line) and "warnings generated" not in line:
            rep.error("check-package: %s" % line)


def sync_cores(linux: Path, repo: Path) -> int:
    path = repo / "mods" / "retroarch-cores.src.lock"
    if not path.is_file():
        print("lint_packages: %s does not exist (scripts/core_sources.py writes it)" % path, file=sys.stderr)
        return 1
    lock = read_src_lock(path)
    changed = 0
    for d in package_dirs(linux):
        if not d.name.startswith("libretro-"):
            continue
        mk = d / (d.name + ".mk")
        recipe = Recipe(mk)
        core = core_name(d.name, recipe)
        commit = lock.get(core, ("", ""))[1]
        if not FULL_COMMIT_RE.match(commit):
            continue
        prefix = prefix_of(d.name)
        text = mk.read_text(encoding="utf-8")
        new = re.sub(r"(?m)^%s_VERSION = .*$" % re.escape(prefix), "%s_VERSION = %s" % (prefix, commit), text)
        if new != text:
            with open(str(mk), "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
            changed += 1
            print("%s: %s" % (d.name, commit))
    print("lint_packages: %d recipes updated" % changed)
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--linux", type=Path, default=LINUX, help="the linux/ tree (default: this one)")
    ap.add_argument("--repo", type=Path, default=REPO, help="the repository (ASSETS.md, mods/, nano/)")
    ap.add_argument("--strict", action="store_true", help="pending findings fail too")
    ap.add_argument("--buildroot", type=Path, help="also run that tree's utils/check-package")
    ap.add_argument("--sync-cores", action="store_true", help="write the cores' _VERSION from the lock")
    ap.add_argument("-q", "--quiet", action="store_true", help="print errors and the summary only")
    args = ap.parse_args(argv)
    if args.sync_cores:
        return sync_cores(args.linux, args.repo)
    rep = lint(args.linux, args.repo)
    if args.buildroot:
        check_package_tool(rep, args.linux, args.buildroot)
    if not args.quiet:
        for p in rep.pending:
            print("lint_packages: pending: %s" % p)
        for n in rep.notes:
            print("lint_packages: note: %s" % n)
    for e in rep.errors:
        print("lint_packages: error: %s" % e, file=sys.stderr)
    print("lint_packages: %d packages, %d errors, %d pending"
          % (len(package_dirs(args.linux)), len(rep.errors), len(rep.pending)))
    if rep.errors or (args.strict and rep.pending):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
