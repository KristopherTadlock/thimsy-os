"""devices/<FAMILY>/options (and projects/ROCKNIX/options): KEY="value" lines read as data.

Never executed. The shapes ROCKNIX's options files use:

    PARTITION_TABLE="gpt"                        a plain assignment
    UBOOT_FIRMWARE+=" atf"                       an append
    KERNEL_MAKE_EXTRACMD=" $(get_kernel...)"     dynamic: kept as text, flagged, never expanded
    case ${TARGET_ARCH} in aarch64) ... ;; esac  only the target architecture's branch is read

Any other shell line is ignored and listed in `ignored` so a reviewer sees what was skipped.
"""

from __future__ import annotations

import re
from collections import OrderedDict

_ASSIGN = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)(\+?=)(.*)$")
_CASE = re.compile(r"^\s*case\s+\"?\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?\"?\s+in\s*$")
_LABEL = re.compile(r"^\s*([^()\s]+)\)\s*(.*)$")


def _value(raw: str) -> tuple[str, bool]:
    """Parse one shell word on the right of '='; return (value, dynamic)."""
    raw = raw.strip()
    # drop a trailing comment after a closed word
    if raw.startswith('"'):
        end = raw.find('"', 1)
        while end > 0 and raw[end - 1] == "\\":
            end = raw.find('"', end + 1)
        if end < 0:
            return raw[1:], True
        val = raw[1:end].replace('\\"', '"')
        return val, ("$" in val or "`" in val)
    if raw.startswith("'"):
        end = raw.find("'", 1)
        if end < 0:
            return raw[1:], True
        return raw[1:end], False
    word = raw.split("#", 1)[0].strip()
    if " " in word:
        return word, True
    return word, ("$" in word or "`" in word)


class Options:
    """Ordered KEY -> value facts, with the operations that produced them."""

    def __init__(self):
        self.values: "OrderedDict[str, str]" = OrderedDict()
        self.dynamic: set = set()
        self.ignored: list = []
        self.sources: list = []

    def get(self, key: str, default: str | None = None) -> str | None:
        if key in self.dynamic:
            return default
        return self.values.get(key, default)

    def raw(self, key: str) -> str | None:
        return self.values.get(key)

    def to_json(self) -> dict:
        return {
            "values": OrderedDict((k, self.values[k]) for k in sorted(self.values)),
            "dynamic": sorted(self.dynamic),
            "sources": list(self.sources),
        }

    def read(self, text: str, source: str, arch: str = "aarch64") -> "Options":
        self.sources.append(source)
        case_var = None     # inside `case ${X} in`
        branch_on = False   # inside a branch that matches
        depth_if = 0
        for n, line in enumerate(text.splitlines(), 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            m = _CASE.match(line)
            if m and case_var is None:
                case_var = m.group(1)
                branch_on = False
                continue
            if case_var is not None:
                if s == "esac":
                    case_var = None
                    branch_on = False
                    continue
                if s == ";;":
                    branch_on = False
                    continue
                lm = _LABEL.match(line)
                if lm and not _ASSIGN.match(line):
                    labels = lm.group(1).split("|")
                    want = arch if case_var in ("TARGET_ARCH", "ARCH") else None
                    branch_on = want is not None and (want in labels or "*" in labels)
                    if case_var not in ("TARGET_ARCH", "ARCH"):
                        self.ignored.append("%s:%d case on %s" % (source, n, case_var))
                    continue
                if not branch_on:
                    continue
            if s.startswith(("if ", "if[")):
                depth_if += 1
            if depth_if:
                self.ignored.append("%s:%d %s" % (source, n, s))
                if s == "fi":
                    depth_if -= 1
                continue
            m = _ASSIGN.match(line)
            if not m:
                self.ignored.append("%s:%d %s" % (source, n, s))
                continue
            key, op, rest = m.group(1), m.group(2), m.group(3)
            val, dyn = _value(rest)
            if op == "+=":
                prev = self.values.get(key, "")
                self.values[key] = prev + val
                if key in self.dynamic or dyn:
                    self.dynamic.add(key)
            else:
                self.values[key] = val
                if dyn:
                    self.dynamic.add(key)
                else:
                    self.dynamic.discard(key)
        return self


def words(value: str | None) -> list:
    return [w for w in (value or "").split() if w]
