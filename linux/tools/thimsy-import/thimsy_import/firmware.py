"""Firmware lists, filtered by the firmware policy (decision 24).

ROCKNIX names firmware in three places, all read here:

1. devices/<FAMILY>/config/kernel-firmware.dat: linux-firmware paths or globs, one per line,
   grouped under "# DRIVER" comments; installed into the root filesystem.
2. The family's CONFIG_EXTRA_FIRMWARE block in packages/linux/package.mk's pre_make_target:
   `cp -Lv $(get_build_dir <package>)/<dir>/<path> ${PKG_BUILD}/external-firmware/<dest>`
   lines that build files into the kernel. Plus regulatory.db when CONFIG_CFG80211=y.
3. The FIRMWARE="..." option: whole packages (`extra-firmware` is ROCKNIX's own proprietary
   collection, with no licence file).

Policy (D24): ship a file only when its licence is known. linux-firmware's licences come from
its WHENCE file (--whence); without it every linux-firmware entry is unknown. Everything else
is left out, marked as a candidate to fetch from the user's own device at install time, and
reported. The importer never decides that a blob is redistributable on its own.
"""

from __future__ import annotations

import fnmatch
import re
from collections import OrderedDict

from .rocknix import ImportError_

# Packages ROCKNIX's build copies firmware from, with what Thimsy knows about each.
PACKAGES = {
    "kernel-firmware": "linux-firmware",       # licences per file, from WHENCE
    "wireless-regdb": "ISC",                   # wireless-regdb's LICENSE (ISC), whole package
    "extra-firmware": None,                    # ROCKNIX's proprietary blobs: no licence
}

_UNKNOWN_LICENCE = re.compile(r"unknown|not redistributable|no licen[cs]e", re.I)


def parse_whence(text: str) -> "dict[str, str]":
    """linux-firmware WHENCE: File:/RawFile:/Link: entries take the block's Licence: line;
    an entry whose block has no usable licence maps to ""."""
    out: "dict[str, str]" = {}
    pending: list = []
    licence = None

    def flush():
        for f in pending:
            if licence and not _UNKNOWN_LICENCE.search(licence):
                out[f] = licence
            else:
                out.setdefault(f, "")   # listed, with no usable licence
    for line in text.splitlines():
        if line.startswith("----------"):
            flush()
            pending, licence = [], None
            continue
        m = re.match(r"^(File|RawFile):\s*(.+?)\s*$", line)
        if m:
            name = m.group(2)
            if len(name) >= 2 and name[0] == name[-1] == '"':
                name = name[1:-1]
            pending.append(name)
            continue
        m = re.match(r"^Link:\s*(.+?)\s*->\s*(.+?)\s*$", line)
        if m:
            pending.append(m.group(1))
            continue
        m = re.match(r"^Licen[cs]e:\s*(.*?)\s*$", line)
        if m and licence is None:
            licence = m.group(1)
    flush()
    return out


def parse_dat(text: str) -> list:
    """[(driver_comment, path_or_glob)] from kernel-firmware.dat."""
    out = []
    driver = ""
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith("#"):
            driver = s.lstrip("#").strip()
            continue
        out.append((driver, s))
    return out


_CP = re.compile(
    r"^cp\s+(?:-[A-Za-z]+\s+)*\$\(get_build_dir\s+([A-Za-z0-9_.-]+)\)/(\S+)\s+"
    r"\$\{PKG_BUILD\}/external-firmware(/\S*)?\s*$")


def parse_builtin_block(lines: list, block_name: str) -> list:
    """[(package, source_path, dest_path)] from a family's CONFIG_EXTRA_FIRMWARE block.

    Every line must be one the translation knows; anything else stops the import so a
    human ports it (the block is hash-guarded as well)."""
    out = []
    for raw in lines[1:]:   # lines[0] is the elif/if header
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("mkdir -p ${PKG_BUILD}/external-firmware"):
            continue
        if s.startswith("FW_LIST=") or s.startswith("${PKG_BUILD}/scripts/config --set-str CONFIG_EXTRA_FIRMWARE"):
            continue
        m = _CP.match(s)
        if not m:
            raise ImportError_("%s: firmware line not understood, port it by hand: %s" % (block_name, s))
        pkg, src, dest = m.group(1), m.group(2), (m.group(3) or "/").lstrip("/")
        if "*" in src:
            raise ImportError_("%s: glob copy not supported: %s" % (block_name, s))
        if pkg == "kernel-firmware":
            if not src.startswith(".copied-firmware/"):
                raise ImportError_("%s: unexpected kernel-firmware path: %s" % (block_name, s))
            src = src[len(".copied-firmware/"):]
        base = src.rsplit("/", 1)[-1]
        dest_path = (dest.rstrip("/") + "/" + base) if dest else base
        out.append((pkg, src, dest_path))
    return out


class Entry:
    def __init__(self, path, source, package, builtin, origin):
        self.path = path            # path under /lib/firmware (or the kernel's firmware dir)
        self.source = source        # path inside the package
        self.package = package
        self.builtin = builtin
        self.origin = origin        # ROCKNIX path (and block) that names it
        self.licence = None
        self.decision = None
        self.reason = None
        self.driver = ""

    def to_json(self) -> dict:
        d = OrderedDict()
        d["path"] = self.path
        d["package"] = self.package
        if self.source != self.path:
            d["source"] = self.source
        d["builtin"] = self.builtin
        if self.driver:
            d["driver"] = self.driver
        d["origin"] = self.origin
        d["licence"] = self.licence
        d["decision"] = self.decision
        d["reason"] = self.reason
        if self.decision == "exclude":
            d["fetch_candidate"] = True
        return d


def _licence_for(path: str, whence: "dict[str, str] | None"):
    if whence is None:
        return None
    if any(c in path for c in "*?["):
        hits = sorted(f for f in whence if fnmatch.fnmatchcase(f, path))
        lic = sorted(set(whence[f] for f in hits))
        return lic[0] if len(lic) == 1 and hits and lic[0] else None
    return whence.get(path) or None


def apply_policy(entries: list, whence: "dict[str, str] | None") -> None:
    for e in entries:
        kind = PACKAGES.get(e.package, None) if e.package in PACKAGES else "unknown-package"
        if kind == "linux-firmware":
            lic = _licence_for(e.source, whence)
            if lic:
                e.licence, e.decision = lic, "ship"
                e.reason = "linux-firmware WHENCE names a licence"
            else:
                e.decision = "exclude"
                e.reason = ("no WHENCE given: licence unknown" if whence is None
                            else "linux-firmware WHENCE has no licence for it")
        elif kind is None:
            e.decision = "exclude"
            e.reason = "package %s is proprietary, with no licence file" % e.package
        elif kind == "unknown-package":
            e.decision = "exclude"
            e.reason = "package %s: licence not known to the importer" % e.package
        else:
            e.licence, e.decision = kind, "ship"
            e.reason = "package %s has a known licence" % e.package


def collect(dat_text, dat_path, builtin, cfg80211, option_firmware, whence) -> list:
    """All firmware entries for a family, policy applied, in a stable order."""
    entries = []
    if dat_text is not None:
        for driver, path in parse_dat(dat_text):
            e = Entry(path, path, "kernel-firmware", False, dat_path)
            e.driver = driver
            entries.append(e)
    for pkg, src, dest, origin in builtin:
        entries.append(Entry(dest, src, pkg, True, origin))
    if cfg80211:
        for f in ("regulatory.db", "regulatory.db.p7s"):
            entries.append(Entry(f, f, "wireless-regdb", True, cfg80211))
    for pkg, origin in option_firmware:
        entries.append(Entry("*", "*", pkg, False, origin))
    apply_policy(entries, whence)
    # de-duplicate (a file both installed and built in keeps both roles)
    seen: "OrderedDict[tuple, Entry]" = OrderedDict()
    for e in entries:
        seen.setdefault((e.path, e.package, e.builtin), e)
    return sorted(seen.values(), key=lambda e: (not e.builtin, e.package, e.path))


def extra_firmware_value(entries: list) -> str:
    """CONFIG_EXTRA_FIRMWARE as ROCKNIX builds it (the family's files sorted, then the
    regulatory database appended), shipped files only."""
    ship = [e for e in entries if e.builtin and e.decision == "ship"]
    main = sorted(e.path for e in ship if e.package != "wireless-regdb")
    regdb = [e.path for e in ship if e.package == "wireless-regdb"]
    return " ".join(main + sorted(regdb))
