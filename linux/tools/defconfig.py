#!/usr/bin/env python3
"""Merges configs/fragments into the committed configs/<target>_defconfig files.

Usage:
  defconfig.py [TARGET ...]          write the defconfigs (all targets by default)
  defconfig.py --check [--buildroot DIR]
                                     check that every committed defconfig equals
                                     its merge, plus the lint below; writes nothing
  defconfig.py --list [--publish]    print the targets (only publishable ones)
  defconfig.py --info TARGET         print kind, publish, sdk_from, fragments and
                                     the missing files, as KEY=VALUE lines for sh
  -v                                 also list the pending paths that do not exist yet

Merging follows Buildroot's support/kconfig/merge_config.sh: fragments are read
in configs/targets.json's order and a later value wins. Unlike merge_config.sh, a
later fragment may change a value only on a line preceded by a comment
"# override: <why>"; any other change is an error, so no option is set twice with
different values by accident (IMPLEMENTATION.md E.4, D.4).

The lint (--check, and after writing): every symbol of this tree's packages that
a fragment names exists in a Config.in under package/; every package directory is
sourced from package/Config.in; every $(BR2_EXTERNAL_THIMSY_PATH)/... path exists
or is listed in targets.json's pending_paths; every override marker takes effect
in some target; a hardware pack's external toolchain states the versions of its
SDK tier's toolchain; and no target marked "publish": true, nor any named
*_release, includes dev.fragment or lacks the release policy (decision I37:
root login locked, no adbd, no serial getty; SSH off until the user turns it
on follows (OA-3), since D50ssh decides by ro.thimsy.build.type, which those
make).
With --buildroot DIR (a Buildroot tree at the pinned version), every other
symbol must exist in that tree and not be a legacy one, and no package of this
tree may share a name with one of Buildroot's.

Exit status: 0 clean, 1 a merge error, a stale defconfig or a lint error, 2 usage.
Standard library only; runs on Python 3.8 and later.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

LINUX = Path(__file__).resolve().parent.parent
EXT = "$(BR2_EXTERNAL_THIMSY_PATH)"
DEV_FRAGMENT = "dev"
FORCED_PREFIXES = ("BR2_PACKAGE_THIMSY_", "BR2_PACKAGE_LIBRETRO_", "BR2_THIMSY_")

SET_RE = re.compile(r"^(BR2_[A-Za-z0-9_]+)=(.*)$")
UNSET_RE = re.compile(r"^# (BR2_[A-Za-z0-9_]+) is not set$")
OVERRIDE_RE = re.compile(r"^#\s*override:\s*(.*)$")
VALUE_RE = re.compile(r'^(y|m|"(?:[^"\\]|\\.)*"|-?[0-9]+|0x[0-9a-fA-F]+)$')
CONFIG_RE = re.compile(r"^\s*(?:menu)?config\s+(BR2_[A-Za-z0-9_]+)\s*$")
SOURCE_RE = re.compile(r'^\s*source\s+"\$BR2_EXTERNAL_THIMSY_PATH/([^"]+)"\s*$')


class Setting:
    """One line of a fragment: a value, or None for "is not set"."""

    def __init__(self, symbol: str, value: Optional[str], label: str, line: int,
                 override: Optional[str]) -> None:
        self.symbol = symbol
        self.value = value
        self.label = label
        self.line = line
        self.override = override

    def render(self) -> str:
        if self.value is None:
            return "# %s is not set" % self.symbol
        return "%s=%s" % (self.symbol, self.value)

    def where(self) -> str:
        return "%s:%d" % (self.label, self.line)


class Target:
    def __init__(self, name: str, spec: dict) -> None:
        self.name = name
        self.kind = spec.get("kind", "tier")
        self.publish = bool(spec.get("publish", False))
        self.sdk_from = spec.get("sdk_from")
        self.fragments = list(spec.get("fragments", []))

    def release_bound(self) -> bool:
        return self.publish or self.name.endswith("_release")


class Merge:
    def __init__(self, target: Target) -> None:
        self.target = target
        self.settings: Dict[str, Setting] = {}
        self.labels: List[str] = []
        self.skipped: List[str] = []
        self.errors: List[str] = []
        self.overrides_used: List[Tuple[str, str]] = []

    def value(self, symbol: str) -> Optional[str]:
        s = self.settings.get(symbol)
        return None if s is None else s.value

    def is_y(self, symbol: str) -> bool:
        return self.value(symbol) == "y"


class Tree:
    """The BR2_EXTERNAL tree: targets.json, fragments and package Config.in files."""

    def __init__(self, linux: Path = LINUX) -> None:
        self.linux = linux
        with open(linux / "configs" / "targets.json", encoding="utf-8") as f:
            data = json.load(f)
        self.targets: Dict[str, Target] = {
            name: Target(name, spec) for name, spec in data["targets"].items()}
        self.pending: Dict[str, str] = dict(data.get("pending_paths", {}))
        self._fragments: Dict[str, List[Setting]] = {}

    # -- fragments -----------------------------------------------------------

    def fragment_label(self, ref: str) -> str:
        return ref if "/" in ref else "fragments/%s.fragment" % ref

    def fragment_path(self, ref: str) -> Path:
        if "/" in ref:
            return self.linux / ref
        return self.linux / "configs" / "fragments" / ("%s.fragment" % ref)

    def all_fragment_refs(self) -> List[str]:
        refs = []
        for t in self.targets.values():
            for r in t.fragments:
                if r not in refs:
                    refs.append(r)
        for p in sorted((self.linux / "configs" / "fragments").glob("*.fragment")):
            if p.stem not in refs:
                refs.append(p.stem)
        return refs

    def parse(self, ref: str) -> List[Setting]:
        if ref not in self._fragments:
            self._fragments[ref] = parse_fragment(self.fragment_path(ref), self.fragment_label(ref))
        return self._fragments[ref]

    # -- merging -------------------------------------------------------------

    def merge(self, name: str) -> Merge:
        target = self.targets[name]
        m = Merge(target)
        for ref in target.fragments:
            label = self.fragment_label(ref)
            if not self.fragment_path(ref).is_file():
                if ref in self.pending:
                    m.skipped.append(ref)
                else:
                    m.errors.append("%s: fragment %s does not exist" % (name, label))
                continue
            try:
                settings = self.parse(ref)
            except FragmentError as e:
                m.errors.append(str(e))
                continue
            m.labels.append(label)
            for s in settings:
                prev = m.settings.get(s.symbol)
                if prev is None or prev.value == s.value:
                    if prev is None:
                        m.settings[s.symbol] = s
                    continue
                if s.override is None:
                    m.errors.append(
                        "%s: %s is %s at %s and %s at %s; mark the later line "
                        "'# override: <why>' if the change is meant"
                        % (name, s.symbol, show(prev.value), prev.where(),
                           show(s.value), s.where()))
                    continue
                del m.settings[s.symbol]
                m.settings[s.symbol] = s
                m.overrides_used.append((label, s.symbol))
        return m

    def render(self, m: Merge) -> str:
        t = m.target
        out = [
            "# Generated by linux/tools/defconfig.py from configs/targets.json; do not edit.",
            "# Edit configs/fragments/ and run: python3 linux/tools/defconfig.py",
            "# %s (%s%s): %s" % (t.name, t.kind, ", publish" if t.publish else "",
                                 " + ".join(t.fragments)),
        ]
        if m.skipped:
            out.append("# Not merged until they exist (pending_paths): %s"
                       % ", ".join(m.skipped))
        for label in m.labels:
            lines = [s.render() for s in m.settings.values() if s.label == label]
            if lines:
                out.append("")
                out.append("# --- %s" % label)
                out.extend(lines)
        return "\n".join(out) + "\n"

    def defconfig_path(self, name: str) -> Path:
        return self.linux / "configs" / ("%s_defconfig" % name)

    # -- the package tree ----------------------------------------------------

    def package_dirs(self) -> List[Path]:
        dirs = []
        for base in (self.linux / "package", self.linux / "gpl" / "package"):
            if base.is_dir():
                dirs.extend(sorted(p for p in base.iterdir() if (p / "Config.in").is_file()))
        return dirs

    def external_symbols(self) -> Dict[str, str]:
        files = [self.linux / "Config.in"]
        for base in (self.linux / "package", self.linux / "gpl" / "package"):
            if base.is_dir():
                files.extend(sorted(base.rglob("Config.in")))
        symbols: Dict[str, str] = {}
        for f in files:
            if not f.is_file():
                continue
            for line in read_lines(f):
                mo = CONFIG_RE.match(line)
                if mo:
                    symbols[mo.group(1)] = rel(self.linux, f)
        return symbols

    def external_prefixes(self) -> List[str]:
        prefixes = list(FORCED_PREFIXES)
        for d in self.package_dirs():
            prefixes.append("BR2_PACKAGE_" + d.name.upper().replace("-", "_"))
        return prefixes


class FragmentError(Exception):
    pass


def show(value: Optional[str]) -> str:
    return "not set" if value is None else value


def read_lines(path: Path) -> List[str]:
    with open(path, encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n").split("\n")


def rel(base: Path, path: Path) -> str:
    return path.relative_to(base).as_posix()


def parse_fragment(path: Path, label: str) -> List[Setting]:
    """Reads one fragment; raises FragmentError on any line it does not understand."""
    settings: List[Setting] = []
    seen: Dict[str, int] = {}
    pending: Optional[Tuple[str, int]] = None
    lines = read_lines(path)
    if lines and lines[-1] == "":
        lines.pop()
    for n, raw in enumerate(lines, 1):
        line = raw.rstrip()
        if line != raw:
            raise FragmentError("%s:%d: trailing whitespace" % (label, n))
        if not line:
            if pending:
                raise FragmentError("%s:%d: '# override:' is not followed by a setting"
                                    % (label, pending[1]))
            continue
        mo = OVERRIDE_RE.match(line)
        if mo:
            if not mo.group(1).strip():
                raise FragmentError("%s:%d: '# override:' needs a reason" % (label, n))
            if pending:
                raise FragmentError("%s:%d: two '# override:' markers for one setting"
                                    % (label, n))
            pending = (mo.group(1).strip(), n)
            continue
        symbol: Optional[str] = None
        value: Optional[str] = None
        mo = SET_RE.match(line)
        if mo:
            symbol, value = mo.group(1), mo.group(2)
            if not VALUE_RE.match(value):
                raise FragmentError("%s:%d: %s has a value Kconfig does not read: %s"
                                    % (label, n, symbol, value))
        else:
            mo = UNSET_RE.match(line)
            if mo:
                symbol = mo.group(1)
            elif line.startswith("#"):
                if re.match(r"^#\s*BR2_", line):
                    raise FragmentError("%s:%d: write '# SYMBOL is not set' exactly, "
                                        "or a comment that does not start with a symbol"
                                        % (label, n))
                continue
            else:
                raise FragmentError("%s:%d: not a setting or a comment: %s" % (label, n, line))
        if symbol in seen:
            raise FragmentError("%s:%d: %s already set at line %d" % (label, n, symbol, seen[symbol]))
        seen[symbol] = n
        settings.append(Setting(symbol, value, label, n, pending[0] if pending else None))
        pending = None
    if pending:
        raise FragmentError("%s:%d: '# override:' is not followed by a setting" % (label, pending[1]))
    return settings


def ext_paths(value: Optional[str]) -> List[str]:
    """The $(BR2_EXTERNAL_THIMSY_PATH)/... paths in a value, relative to linux/."""
    if not value or EXT not in value:
        return []
    paths = []
    for token in value.strip('"').split():
        if token.startswith(EXT + "/"):
            paths.append(token[len(EXT) + 1:].rstrip("/"))
        elif EXT in token:
            paths.append(token)  # reported as malformed below
    return paths


def y_symbol(m: Merge, pattern: str) -> Optional[str]:
    rx = re.compile(pattern)
    for s in m.settings.values():
        if s.value == "y":
            mo = rx.match(s.symbol)
            if mo:
                return mo.group(1)
    return None


def missing_paths(tree: Tree, m: Merge) -> List[str]:
    """Files a target needs before it can be built: pending fragments and paths."""
    missing = list(m.skipped)
    for s in m.settings.values():
        for p in ext_paths(s.value):
            if not (tree.linux / p).exists() and p not in missing:
                missing.append(p)
    return missing


def release_problems(tree: Tree, m: Merge) -> List[str]:
    """Decision I37's policy for anything that may be published."""
    t = m.target
    why = "marked publish" if t.publish else "named *_release"
    bad: List[str] = []

    def no(text: str) -> None:
        bad.append("%s (%s): %s" % (t.name, why, text))

    if DEV_FRAGMENT in t.fragments or any(
            tree.fragment_path(r).name == "dev.fragment" for r in t.fragments):
        no("includes dev.fragment")
    if m.is_y("BR2_THIMSY_FLAVOUR_DEV"):
        no("BR2_THIMSY_FLAVOUR_DEV=y")
    if t.kind == "tier" and not m.is_y("BR2_THIMSY_FLAVOUR_RELEASE"):
        no("a published tier must set BR2_THIMSY_FLAVOUR_RELEASE=y (release.fragment)")
    s = m.settings.get("BR2_TARGET_ENABLE_ROOT_LOGIN")
    if s is None or s.value is not None:
        no("root password not locked: needs '# BR2_TARGET_ENABLE_ROOT_LOGIN is not set' "
           "(Buildroot's default is root login with an empty password)")
    for sym, what in (("BR2_PACKAGE_ANDROID_TOOLS_ADBD", "adbd"),
                      ("BR2_PACKAGE_ANDROID_TOOLS_ADB", "adb"),
                      ("BR2_TARGET_GENERIC_GETTY", "a serial getty")):
        if m.is_y(sym):
            no("%s is enabled (%s=y)" % (what, sym))
    for p in ext_paths(m.value("BR2_ROOTFS_OVERLAY")):
        if p.rstrip("/").endswith("-dev"):
            no("uses the dev overlay %s" % p)
            continue
        inittab = tree.linux / p / "etc" / "inittab"
        if inittab.is_file():
            for n, line in enumerate(read_lines(inittab), 1):
                if line.strip() and not line.lstrip().startswith("#") and "getty" in line:
                    no("%s/etc/inittab:%d starts a getty: %s" % (p, n, line.strip()))
    return bad


def lint(tree: Tree, merges: Dict[str, Merge]) -> Tuple[List[str], List[str]]:
    errors: List[str] = []
    notes: List[str] = []
    linux = tree.linux

    # Package Config.in files: sourced, and defining the symbols fragments name.
    sourced = set()
    pkg_config = linux / "package" / "Config.in"
    if pkg_config.is_file():
        for n, line in enumerate(read_lines(pkg_config), 1):
            mo = SOURCE_RE.match(line)
            if mo:
                if not (linux / mo.group(1)).is_file():
                    errors.append("package/Config.in:%d sources a missing file: %s" % (n, mo.group(1)))
                sourced.add(mo.group(1))
    else:
        errors.append("package/Config.in is missing")
    for d in tree.package_dirs():
        r = rel(linux, d / "Config.in")
        if r.startswith("package/") and r not in sourced:
            errors.append("%s is not sourced from package/Config.in" % r)
    symbols = tree.external_symbols()
    prefixes = tree.external_prefixes()

    refs = tree.all_fragment_refs()
    used_paths = set()
    overrides: List[Tuple[str, str, int]] = []
    for ref in refs:
        path = tree.fragment_path(ref)
        if not path.is_file():
            used_paths.add(ref)
            if ref not in tree.pending:
                errors.append("fragment %s does not exist and is not pending" % ref)
            continue
        try:
            settings = tree.parse(ref)
        except FragmentError as e:
            errors.append(str(e))
            continue
        label = tree.fragment_label(ref)
        for s in settings:
            if s.override:
                overrides.append((label, s.symbol, s.line))
            ours =any(s.symbol == p or s.symbol.startswith(p if p.endswith("_") else p + "_")
                       for p in prefixes)
            if ours and s.symbol not in symbols:
                errors.append("%s: %s is not defined in any Config.in of this tree"
                              % (s.where(), s.symbol))
            for p in ext_paths(s.value):
                if p.startswith("$") or ".." in p.split("/"):
                    errors.append("%s: malformed path in %s: %s" % (s.where(), s.symbol, p))
                    continue
                used_paths.add(p)
                if (linux / p).exists():
                    continue
                if p in tree.pending:
                    notes.append("%s: %s does not exist yet (pending: %s)"
                                 % (s.where(), p, tree.pending[p]))
                else:
                    errors.append("%s: %s names %s, which does not exist"
                                  % (s.where(), s.symbol, p))
    for p, owner in sorted(tree.pending.items()):
        if p not in used_paths:
            errors.append("pending_paths lists %s, which no fragment or target names" % p)
        elif (linux / p).exists():
            notes.append("%s exists now; drop it from pending_paths" % p)

    used = set()
    for m in merges.values():
        errors.extend(m.errors)
        used.update(m.overrides_used)
        if m.target.release_bound():
            errors.extend(release_problems(tree, m))
    for label, symbol, line in overrides:
        if (label, symbol) not in used:
            errors.append("%s:%d: '# override:' on %s changes nothing in any target"
                          % (label, line, symbol))

    for name, t in tree.targets.items():
        if t.kind not in ("tier", "hw"):
            errors.append("%s: kind must be tier or hw, not %r" % (name, t.kind))
        if t.kind != "hw":
            continue
        src = tree.targets.get(t.sdk_from or "")
        if src is None or src.kind != "tier":
            errors.append("%s: sdk_from must name a tier target, not %r" % (name, t.sdk_from))
            continue
        hw, tier = merges.get(name), merges.get(src.name)
        if hw is None or tier is None:
            continue
        if t.publish and not src.publish:
            errors.append("%s is published but its SDK comes from %s, which is not" % (name, src.name))
        for ours, theirs, what in (
                (r"^BR2_TOOLCHAIN_EXTERNAL_GCC_(\d+)$", r"^BR2_GCC_VERSION_(\d+)_X$", "GCC"),
                (r"^BR2_TOOLCHAIN_EXTERNAL_HEADERS_(\d+_\d+)$", r"^BR2_KERNEL_HEADERS_(\d+_\d+)$",
                 "kernel headers")):
            a, b = y_symbol(hw, ours), y_symbol(tier, theirs)
            if a != b:
                errors.append("%s states %s %s for its SDK, but %s builds %s"
                              % (name, what, a, src.name, b))
    return errors, notes


def buildroot_problems(tree: Tree, buildroot: Path) -> List[str]:
    """Checks the fragments' upstream symbols against a Buildroot tree."""
    known, legacy = set(), set()
    for root, dirs, files in os.walk(buildroot):
        dirs[:] = [d for d in dirs if d not in (".git", "output", "dl", "support")]
        for fn in files:
            if not (fn.startswith("Config") and ".in" in fn):
                continue
            target = legacy if fn == "Config.in.legacy" else known
            for line in read_lines(Path(root) / fn):
                mo = CONFIG_RE.match(line)
                if mo:
                    target.add(mo.group(1))
    if not known:
        return ["%s does not look like a Buildroot tree (no Config.in symbols)" % buildroot]
    errors = []
    ours = tree.external_symbols()
    for d in tree.package_dirs():
        if (buildroot / "package" / d.name).is_dir():
            errors.append("package %s clashes with Buildroot's package/%s" % (d.name, d.name))
    for ref in tree.all_fragment_refs():
        if not tree.fragment_path(ref).is_file():
            continue
        for s in tree.parse(ref):
            if s.symbol in ours:
                continue
            if s.symbol in legacy and s.symbol not in known:
                errors.append("%s: %s is a legacy symbol in this Buildroot" % (s.where(), s.symbol))
            elif s.symbol not in known:
                errors.append("%s: %s is not a symbol of this Buildroot" % (s.where(), s.symbol))
    return errors


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("targets", nargs="*")
    ap.add_argument("--linux", type=Path, default=LINUX, help="the linux/ tree (default: this one)")
    ap.add_argument("--check", action="store_true", help="check, write nothing")
    ap.add_argument("--buildroot", type=Path, help="also check symbols against this Buildroot tree")
    ap.add_argument("--list", action="store_true", help="print the targets")
    ap.add_argument("--publish", action="store_true", help="with --list: only publishable targets")
    ap.add_argument("--info", metavar="TARGET", help="print one target's facts for build.sh")
    ap.add_argument("-v", "--verbose", action="store_true", help="list each pending path")
    args = ap.parse_args(argv)

    try:
        tree = Tree(args.linux)
    except (OSError, ValueError, KeyError) as e:
        print("defconfig: cannot read configs/targets.json: %s" % e, file=sys.stderr)
        return 1

    if args.list:
        for name, t in tree.targets.items():
            if t.publish or not args.publish:
                print(name)
        return 0

    if args.info:
        t = tree.targets.get(args.info)
        if t is None:
            print("defconfig: no target %s in configs/targets.json" % args.info, file=sys.stderr)
            return 2
        m = tree.merge(t.name)
        missing = missing_paths(tree, m)
        print("kind=%s" % t.kind)
        print("publish=%s" % ("true" if t.publish else "false"))
        print("sdk_from=%s" % (t.sdk_from or ""))
        print("fragments=%s" % " ".join(t.fragments))
        print("errors=%d" % len(m.errors))
        print("missing=%s" % " ".join(missing))
        for e in m.errors:
            print("defconfig: %s" % e, file=sys.stderr)
        return 0

    names = args.targets or list(tree.targets)
    unknown = [n for n in names if n not in tree.targets]
    if unknown:
        print("defconfig: unknown target(s): %s" % " ".join(unknown), file=sys.stderr)
        return 2

    merges = {name: tree.merge(name) for name in tree.targets}
    status = 0
    if args.check:
        for name in names:
            m = merges[name]
            if m.errors:
                continue  # reported by lint
            path = tree.defconfig_path(name)
            want = tree.render(m)
            have = path.read_text(encoding="utf-8").replace("\r\n", "\n") if path.is_file() else None
            if have != want:
                print("defconfig: %s is %s; run linux/tools/defconfig.py"
                      % (rel(tree.linux, path), "missing" if have is None else "stale"),
                      file=sys.stderr)
                status = 1
        for path in sorted((tree.linux / "configs").glob("thimsy_*_defconfig")):
            if path.name[:-len("_defconfig")] not in tree.targets:
                print("defconfig: %s has no target in targets.json" % rel(tree.linux, path),
                      file=sys.stderr)
                status = 1
    else:
        for name in names:
            m = merges[name]
            if m.errors:
                status = 1
                continue
            path = tree.defconfig_path(name)
            text = tree.render(m)
            old = path.read_text(encoding="utf-8").replace("\r\n", "\n") if path.is_file() else None
            if old != text:
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                print("wrote %s" % rel(tree.linux, path))

    errors, notes = lint(tree, merges)
    if args.buildroot:
        errors.extend(buildroot_problems(tree, args.buildroot))
    if args.verbose:
        for n in notes:
            print("defconfig: note: %s" % n, file=sys.stderr)
    elif notes:
        print("defconfig: %d notes on pending paths (-v lists them)" % len(notes), file=sys.stderr)
    for e in dict.fromkeys(errors):
        print("defconfig: error: %s" % e, file=sys.stderr)
    if errors:
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
