"""A small reader for Soong's Blueprint files (Android.bp): modules, variables, strings, lists,
maps, booleans, integers, `+` concatenation and comments. Enough for Nano's Android.bp: the
source lists, filegroups, include dirs and the prebuilt_etc assets."""
from __future__ import annotations

import os
import re
from typing import Dict, List, Optional, Tuple

from .config import NanoTreeError, glob_regex


class Module(dict):
    """One module's properties, with its type in .type (and as the key "_type")."""

    @property
    def type(self) -> str:
        return self["_type"]

    @property
    def name(self) -> Optional[str]:
        return self.get("name")

    def strings(self, key: str) -> List[str]:
        v = self.get(key)
        if v is None:
            return []
        if isinstance(v, str):
            return [v]
        return [x for x in v if isinstance(x, str)]


_TOKEN = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>"(?:[^"\\\n]|\\.)*")
  | (?P<rawstr>`[^`]*`)
  | (?P<int>-?\d+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>\+=|[{}\[\]():,=+])
""", re.S | re.X)


def _tokens(text: str) -> List[Tuple[str, str, int]]:
    out = []
    pos = 0
    line = 1
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m:
            raise NanoTreeError("Android.bp: unexpected %r at line %d" % (text[pos:pos + 20], line))
        kind = m.lastgroup
        val = m.group(kind)
        if kind not in ("ws", "lc", "bc"):
            out.append((kind, val, line))
        line += val.count("\n")
        pos = m.end()
    out.append(("eof", "", line))
    return out


def _unescape(s: str) -> str:
    body = s[1:-1]
    return re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), body)


class Blueprint:
    def __init__(self, text: str):
        self.toks = _tokens(text)
        self.i = 0
        self.variables: Dict[str, object] = {}
        self.modules: List[Module] = []
        self._parse_file()

    @classmethod
    def load(cls, path: str) -> "Blueprint":
        with open(path, "r", encoding="utf-8") as f:
            return cls(f.read())

    # -- parser
    def _peek(self) -> Tuple[str, str, int]:
        return self.toks[self.i]

    def _next(self) -> Tuple[str, str, int]:
        t = self.toks[self.i]
        self.i += 1
        return t

    def _expect(self, val: str) -> None:
        t = self._next()
        if t[1] != val:
            raise NanoTreeError("Android.bp: expected %r at line %d, found %r" % (val, t[2], t[1]))

    def _parse_file(self) -> None:
        while self._peek()[0] != "eof":
            kind, val, line = self._next()
            if kind != "ident":
                raise NanoTreeError("Android.bp: unexpected %r at line %d" % (val, line))
            nxt = self._peek()[1]
            if nxt in ("=", "+="):
                self._next()
                value = self._expr()
                if nxt == "+=" and val in self.variables:
                    value = _add(self.variables[val], value, line)
                self.variables[val] = value
            elif nxt in ("{", "("):
                close = "}" if nxt == "{" else ")"
                self._next()
                props = self._props(close)
                mod = Module(props)
                mod["_type"] = val
                mod["_line"] = line
                self.modules.append(mod)
            else:
                raise NanoTreeError("Android.bp: unexpected %r after %s at line %d" % (nxt, val, line))

    def _props(self, close: str) -> Dict[str, object]:
        props: Dict[str, object] = {}
        while True:
            t = self._peek()
            if t[1] == close:
                self._next()
                return props
            kind, key, line = self._next()
            if kind != "ident":
                raise NanoTreeError("Android.bp: expected a property name at line %d, found %r" % (line, key))
            sep = self._next()
            if sep[1] not in (":", "="):
                raise NanoTreeError("Android.bp: expected ':' at line %d" % sep[2])
            props[key] = self._expr()
            if self._peek()[1] == ",":
                self._next()

    def _expr(self) -> object:
        value = self._value()
        while self._peek()[1] == "+":
            line = self._next()[2]
            value = _add(value, self._value(), line)
        return value

    def _value(self) -> object:
        kind, val, line = self._next()
        if kind == "str":
            return _unescape(val)
        if kind == "rawstr":
            return val[1:-1]
        if kind == "int":
            return int(val)
        if kind == "ident":
            if val == "true":
                return True
            if val == "false":
                return False
            if val in self.variables:
                return self.variables[val]
            raise NanoTreeError("Android.bp: unknown variable %s at line %d" % (val, line))
        if val == "[":
            items = []
            while self._peek()[1] != "]":
                items.append(self._expr())
                if self._peek()[1] == ",":
                    self._next()
            self._next()
            return items
        if val == "{":
            return self._props("}")
        raise NanoTreeError("Android.bp: unexpected %r at line %d" % (val, line))

    # -- queries
    def module(self, name: str) -> Optional[Module]:
        for m in self.modules:
            if m.name == name:
                return m
        return None

    def of_type(self, *types: str) -> List[Module]:
        return [m for m in self.modules if m.type in types]

    def srcs(self, module: Module, key: str = "srcs") -> List[str]:
        """The module's source entries with `:filegroup` references expanded (when the filegroup is
        in this file; other references are kept as they are)."""
        out: List[str] = []
        for s in module.strings(key):
            if s.startswith(":"):
                fg = self.module(s[1:])
                if fg is not None and fg.type == "filegroup":
                    out.extend(self.srcs(fg))
                    continue
            out.append(s)
        return out


def _add(a: object, b: object, line: int) -> object:
    if isinstance(a, list) and isinstance(b, list):
        return a + b
    if isinstance(a, str) and isinstance(b, str):
        return a + b
    if isinstance(a, int) and isinstance(b, int):
        return a + b
    if isinstance(a, dict) and isinstance(b, dict):
        r = dict(a)
        r.update(b)
        return r
    raise NanoTreeError("Android.bp: can't add %s and %s at line %d" % (type(a).__name__, type(b).__name__, line))


def expand_globs(patterns: List[str], files: List[str], excludes: Optional[List[str]] = None) -> List[str]:
    """Soong's glob semantics over FILES (paths relative to the module's directory)."""
    out: List[str] = []
    seen = set()
    ex = [glob_regex(e) for e in (excludes or [])]
    for p in patterns:
        if any(c in p for c in "*?["):
            rx = glob_regex(p)
            hits = sorted(f for f in files if rx.match(f))
        else:
            hits = [p] if p in files else []
        for h in hits:
            if h not in seen and not any(e.match(h) for e in ex):
                seen.add(h)
                out.append(h)
    return out


def etc_installs(bp: Blueprint, files: List[str]) -> List[Tuple[str, str, str]]:
    """(module, source path, installed path under /system/etc) for every prebuilt_etc module."""
    result = []
    for m in bp.of_type("prebuilt_etc"):
        sub = m.get("sub_dir") or m.get("relative_install_path") or ""
        if isinstance(m.get("src"), str):
            src = m["src"]
            name = m.get("filename") or os.path.basename(src)
            result.append((m.name, src, "/".join(x for x in (sub, name) if x)))
        srcs = m.strings("srcs")
        if srcs:
            for src in expand_globs(srcs, files, m.strings("exclude_srcs")):
                result.append((m.name, src, "/".join(x for x in (sub, os.path.basename(src)) if x)))
    return result
