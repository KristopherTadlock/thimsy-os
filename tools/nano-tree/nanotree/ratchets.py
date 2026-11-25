"""Inventories of nano/'s shared source files for the ratchet tests (IMPLEMENTATION.md H.4)."""
from __future__ import annotations

import os
import re
from typing import Dict, Iterator, List, Set, Tuple

from .config import Upstream

SOURCE_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inc")
# Android property namespaces Nano uses; a literal "<ns>.<more>" (or a "<ns>.<more>." prefix) is a key.
PROP_NAMESPACES = ("persist", "sys", "ro", "service", "init", "ctl", "debug", "vendor", "dalvik")
PROP_KEY = re.compile(r"(?:%s)\.[A-Za-z0-9_\-]+(?:\.[A-Za-z0-9_\-]+)*\.?\Z" % "|".join(PROP_NAMESPACES))
PLATFORM_TOKEN = re.compile(r"\b(__ANDROID__|NANO_PLATFORM_[A-Za-z0-9_]*)\b")


def shared_sources(nano_dir: str, cfg: Upstream) -> List[str]:
    """Paths (relative to nano/) of the C and C++ files Android compiles from nano/: everything
    with a source extension outside the fork-only globs."""
    out = []
    for dirpath, dirnames, names in os.walk(nano_dir):
        dirnames.sort()
        for n in sorted(names):
            if not n.endswith(SOURCE_EXT):
                continue
            rel = os.path.relpath(os.path.join(dirpath, n), nano_dir).replace(os.sep, "/")
            if not cfg.is_fork_only(rel):
                out.append(rel)
    return out


def lex(text: str) -> Tuple[str, List[Tuple[int, str]]]:
    """Split C or C++ TEXT into its code (comments and literals blanked, newlines kept) and its
    string literals as (line, contents) pairs. Raw strings R"x(...)x" are handled."""
    code: List[str] = []
    strings: List[Tuple[int, str]] = []
    pos = 0
    line = 1
    for m in _LEX.finditer(text):
        code.append(text[pos:m.start()])
        line += text.count("\n", pos, m.start())
        tok = m.group(0)
        nl = tok.count("\n")
        if m.group("raw") is not None:
            strings.append((line, m.group("raw")))
            code.append('""')
        elif m.group("str") is not None:
            strings.append((line, m.group("str")))
            code.append('""')
        elif tok.startswith("'"):
            code.append("''")
        code.append("\n" * nl)
        line += nl
        pos = m.end()
    code.append(text[pos:])
    return "".join(code), strings


_LEX = re.compile(r"""
    //[^\n]*
  | /\*.*?(?:\*/|\Z)
  | (?<![A-Za-z0-9_])R"(?P<delim>[^()\\\s"]{0,16})\((?P<raw>.*?)\)(?P=delim)"
  | "(?P<str>(?:[^"\\\n]|\\.)*)"?
  | '(?:[^'\\\n]|\\.)*'?
""", re.S | re.X)


def _read(path: str) -> str:
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def property_keys(nano_dir: str, files: List[str]) -> Dict[str, List[str]]:
    """{key literal: [files using it]} over FILES."""
    keys: Dict[str, Set[str]] = {}
    for rel in files:
        _code, strings = lex(_read(os.path.join(nano_dir, *rel.split("/"))))
        for _line, s in strings:
            if PROP_KEY.match(s):
                keys.setdefault(s, set()).add(rel)
    return {k: sorted(v) for k, v in sorted(keys.items())}


def platform_conditionals(nano_dir: str, files: List[str]) -> Iterator[Tuple[str, int, str]]:
    """(file, line, token) for each __ANDROID__ or NANO_PLATFORM_* in code (not comments or strings)."""
    for rel in files:
        code, _ = lex(_read(os.path.join(nano_dir, *rel.split("/"))))
        for n, text in enumerate(code.split("\n"), 1):
            for m in PLATFORM_TOKEN.finditer(text):
                yield rel, n, m.group(1)


def read_list(path: str) -> List[str]:
    """A baseline file: one entry per line; blank lines and lines starting with # are skipped."""
    with open(path, "r", encoding="utf-8") as f:
        return [ln.rstrip("\n") for ln in f if ln.strip() and not ln.startswith("#")]


def write_list(path: str, header: str, items: List[str]) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for h in header.strip("\n").split("\n"):
            f.write("# " + h + "\n" if h else "#\n")
        for it in items:
            f.write(it + "\n")
