"""Kernel configs: the family's base config, and the conformance check of Thimsy's fragment.

The base config is ROCKNIX's file as kernel_config_path() (config/functions) finds it:

    devices/$DEVICE/linux/$version/linux.$ARCH.conf
    devices/$DEVICE/linux/$LINUX/linux.$ARCH.conf
    devices/$DEVICE/linux/linux.$ARCH.conf
    linux/$version/...  linux/$LINUX/...  linux/...          (under projects/ROCKNIX)
    <linux package>/config/$version/... /$LINUX/... /...

The conformance check merges the base config and Thimsy's fragment the way
merge_config.sh does (a later value wins) and compares the result with the Thimsy-critical
options of kernel-critical.conf. It is a text merge: Kconfig dependencies resolved later by
olddefconfig are not modelled, so it catches a flipped option, not an unmet dependency.
"""

from __future__ import annotations

import re
from collections import OrderedDict
from pathlib import Path

from .rocknix import ImportError_

_SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
_UNSET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def parse(text: str) -> "OrderedDict[str, str]":
    """CONFIG_X -> value; '# CONFIG_X is not set' -> 'n'; string values keep their quotes."""
    out: "OrderedDict[str, str]" = OrderedDict()
    for line in text.splitlines():
        s = line.strip()
        m = _SET.match(s)
        if m:
            out[m.group(1)] = m.group(2)
            continue
        m = _UNSET.match(s)
        if m:
            out[m.group(1)] = "n"
    return out


def config_path(tree, family: str, version: str, linux_var: str, arch: str):
    name = "linux.%s.conf" % arch
    dev = tree.device_dir(family) / "linux"
    prj = tree.project / "linux"
    pkg = tree.linux_pkg_dir() / "config"
    cands = []
    for base in (dev, prj, pkg):
        cands += [base / version / name, base / linux_var / name if linux_var else None, base / name]
    for c in cands:
        if c is not None and tree.resolve(c) is not None:
            return c
    raise ImportError_("no kernel config %s for %s" % (name, family))


def read_critical(path: Path, family: str) -> "OrderedDict[str, tuple]":
    """kernel-critical.conf: CONFIG_X=value lines under [all] or [FAMILY]; '=n' means off.

    Returns CONFIG_X -> (value, reason) for the family (a family line overrides [all])."""
    out: "OrderedDict[str, tuple]" = OrderedDict()
    section = None
    reason = ""
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        s = line.strip()
        if not s:
            reason = ""
            continue
        if s.startswith("#"):
            reason = s.lstrip("#").strip()
            continue
        m = re.match(r"^\[([A-Za-z0-9_-]+)\]$", s)
        if m:
            section = m.group(1)
            continue
        m = _SET.match(s)
        if not m or section is None:
            raise ImportError_("%s:%d: not a critical-option line: %s" % (path, n, s))
        if section in ("all", family):
            out[m.group(1)] = (m.group(2), reason)
    return out


def _norm(v: str | None) -> str:
    return "n" if v is None else v


def conformance(base: dict, fragment: "dict | None", critical: dict) -> dict:
    """Effective value of each critical option after the fragment; violations listed.

    A violation is 'flipped' when the fragment itself sets the wrong value, and
    'not-covered' when the base config is wrong and the fragment does not set it."""
    rows = []
    violations = []
    for opt, (want, reason) in critical.items():
        b = base.get(opt)
        f = fragment.get(opt) if fragment is not None else None
        eff = f if f is not None else b
        ok = _norm(eff) == want
        row = OrderedDict([("option", opt), ("want", want), ("base", _norm(b))])
        if f is not None:
            row["fragment"] = f
        row["ok"] = ok
        if not ok:
            row["problem"] = "flipped" if f is not None else "not-covered"
            violations.append(row)
        rows.append(row)
    return {"rows": rows, "violations": violations}
