"""Device-tree facts: model strings and rocknix-joypad settings.

Read from ROCKNIX's out-of-tree device trees (devices/<FAMILY>/linux/dts/<vendor>/), from
device trees that ROCKNIX's patches add as new files, and optionally from a kernel tree
(--kernel-src). Text is scanned, not compiled: `#include "x.dts"` is followed inside the
same directory only, and the last value seen wins (a board file overrides what it
includes). That is enough for `model`, `joypad-name` and the joypad's invert-abs* flags.
"""

from __future__ import annotations

import re
from collections import OrderedDict

_INCLUDE = re.compile(r'^\s*#include\s+"([^"]+)"', re.M)
_MODEL = re.compile(r'^\s*model\s*=\s*"([^"]+)"\s*;', re.M)
_JOYNAME = re.compile(r'^\s*joypad-name\s*=\s*"([^"]+)"\s*;', re.M)
_INVERT = re.compile(r"^\s*invert-abs(x|y|rx|ry)\s*;", re.M)
_AMUX = re.compile(r"^\s*amux-count\s*=\s*<(\d+)>\s*;", re.M)
_COMPAT = re.compile(r'^\s*compatible\s*=\s*"(rocknix-[a-z0-9-]*joypad)"', re.M)
_REF = re.compile(r"^\s*&joypad\s*\{", re.M)


class DtsSource:
    """Device-tree text by file name, from one directory or from patches."""

    def __init__(self):
        self.files: "OrderedDict[str, tuple]" = OrderedDict()   # name -> (text, origin)

    def add(self, name: str, text: str, origin: str):
        self.files.setdefault(name, (text, origin))

    def expanded(self, name: str, depth: int = 0) -> str | None:
        if name not in self.files or depth > 8:
            return None
        text = self.files[name][0]
        parts = []
        for inc in _INCLUDE.findall(text):
            sub = self.expanded(inc.rsplit("/", 1)[-1], depth + 1)
            if sub:
                parts.append(sub)
        parts.append(text)
        return "\n".join(parts)


def facts(text: str) -> "OrderedDict[str, object]":
    out: "OrderedDict[str, object]" = OrderedDict()
    models = _MODEL.findall(text)
    if models:
        out["model"] = models[-1]
    joy = _COMPAT.findall(text)
    names = _JOYNAME.findall(text)
    if joy or names or _REF.search(text):
        jp = OrderedDict()
        if joy:
            jp["driver"] = joy[-1]
        if names:
            jp["name"] = names[-1]
        inv = sorted(set(_INVERT.findall(text)), key=["x", "y", "rx", "ry"].index)
        jp["invert"] = inv
        amux = _AMUX.findall(text)
        if amux:
            jp["amux_count"] = int(amux[-1])
        out["joypad"] = jp
    return out


def joypad_names_in_patch(data: bytes) -> list:
    """joypad-name values a patch adds (on its + lines), new file or not."""
    return re.findall(r'^\+\s*joypad-name\s*=\s*"([^"]+)"\s*;', data.decode("utf-8", errors="replace"), re.M)


def from_patch(data: bytes) -> list:
    """[(dts file name, text)] for device trees a patch creates (--- /dev/null)."""
    out = []
    text = data.decode("utf-8", errors="replace").splitlines()
    i = 0
    while i < len(text):
        if text[i] == "--- /dev/null" and i + 1 < len(text) and text[i + 1].startswith("+++ b/"):
            path = text[i + 1][6:].strip()
            i += 2
            body = []
            while i < len(text) and not text[i].startswith("diff ") and not text[i].startswith("--- "):
                line = text[i]
                if line.startswith("+"):
                    body.append(line[1:])
                i += 1
            if re.search(r"/boot/dts/.+\.dtsi?$", path):
                out.append((path.rsplit("/", 1)[-1], "\n".join(body) + "\n"))
            continue
        i += 1
    return out
