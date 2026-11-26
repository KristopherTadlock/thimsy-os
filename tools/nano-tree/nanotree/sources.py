"""nano/build/sources.txt (docs/linux/IMPLEMENTATION.md C.1): every file the builds outside Soong
compile, listed once, in sections. The format is what nano/test/host/Makefile's awk reads too:

    # a comment (a whole line; a '#' inside a line is part of nothing, so paths never hold one)
    [section]
    path/relative/to/nano.cpp

Blank lines are ignored; a path is one token per line, relative to nano/, with '/' separators.
"""
from __future__ import annotations

import re
from collections import OrderedDict
from typing import Dict, List, Optional

from .config import NanoTreeError

SECTIONS = ("core", "drm", "firmware", "dropped-linux", "linux", "host", "scrape", "test-unit")
_HEADER = re.compile(r"^\[([a-z][a-z0-9-]*)\]$")
_PATH = re.compile(r"^[A-Za-z0-9_.+@/-]+$")


class SourceList:
    """The parsed list: .sections maps each section name to its paths, in file order."""

    def __init__(self, sections: "OrderedDict[str, List[str]]", path: Optional[str] = None):
        self.sections = sections
        self.path = path

    def __getitem__(self, name: str) -> List[str]:
        return self.sections.get(name, [])

    @classmethod
    def parse(cls, text: str, path: Optional[str] = None) -> "SourceList":
        where = path or "sources.txt"
        sections: "OrderedDict[str, List[str]]" = OrderedDict()
        current: Optional[str] = None
        seen: Dict[str, int] = {}
        for n, raw in enumerate(text.split("\n"), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = _HEADER.match(line)
            if m:
                name = m.group(1)
                if name not in SECTIONS:
                    raise NanoTreeError("%s:%d: unknown section [%s]" % (where, n, name))
                if name in sections:
                    raise NanoTreeError("%s:%d: section [%s] appears twice" % (where, n, name))
                sections[name] = []
                current = name
                seen = {}
                continue
            if current is None:
                raise NanoTreeError("%s:%d: %r is outside any section" % (where, n, line))
            if not _PATH.match(line) or line.startswith("/") or ".." in line.split("/"):
                raise NanoTreeError("%s:%d: not a relative path: %r" % (where, n, line))
            if line in seen:
                raise NanoTreeError("%s:%d: %s is listed twice in [%s] (first at line %d)"
                                    % (where, n, line, current, seen[line]))
            seen[line] = n
            sections[current].append(line)
        return cls(sections, path)

    @classmethod
    def load(cls, path: str) -> "SourceList":
        with open(path, "r", encoding="utf-8") as f:
            return cls.parse(f.read(), path)

    def test_programs(self) -> List[str]:
        """[test-unit]'s test sources (test/unit/*_test.cpp)."""
        return [p for p in self["test-unit"] if p.startswith("test/unit/") and p.endswith("_test.cpp")]

    def test_production(self) -> List[str]:
        """[test-unit]'s production files (everything that is not a test source)."""
        tests = set(self.test_programs())
        return [p for p in self["test-unit"] if p not in tests]
