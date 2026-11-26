#!/usr/bin/env python3
# Copyright (C) 2026 The Thimsy OS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generates inert C++ stubs, at build time, for the functions a link still lacks because their
files are in nano/build/sources.txt's [dropped-linux] (docs/linux/IMPLEMENTATION.md C.1, decision
I29). Nothing it writes is committed: hand-written NanoMenu:: stubs would drift from NanoMenu.h with
every nano-144 change, so they are made from the link's own undefined symbols each time.

  gen.py --nm OBJS.nm [--nm ...] [--from DROPPED.cpp ...] [--scope PREFIX ...]
         --decls DIR_OR_FILE [--decls ...] [--include-root DIR] [--out FILE]

  --nm       the output of `nm -C` over the link's objects and archives (any mix of defined and
             undefined symbols; `nm -A` prefixes and archive member headers are fine). A symbol is
             missing when some object has it undefined (U) and none defines it.
  --from     a [dropped-linux] source: the missing symbols it defines are stubbed (matched by their
             qualified name as the file writes it: `NanoMenu::nsTick`, `LibretroRunner::init`,
             `nanoRgbFollowSample`).
  --scope    also stub every missing symbol whose demangled name starts with PREFIX
             (e.g. "android::NanoMenu::").
  --decls    headers (a directory: its *.h and *.hpp, recursively) and sources searched for each
             symbol's declaration, which gives the return type nm can't (a mangled name has none).
  --include-root  the generated file #includes, relative to this directory, every header a
             declaration came from (default: the first --decls directory).

Every stub logs once (NANOSTUB_ONCE, nano/compat/stubs/include/nanostubs/stub.h) and returns a zero
value: T() for a value, a static zero object for a reference, nothing for void. Constructors and
destructors get empty bodies; a static data member or extern variable gets a value-initialised
definition (decltype of its declaration). A symbol whose declaration can't be found, or is
ambiguous, is an error (exit 1): the build stops rather than guess a return type.

Python 3.8 or later, standard library only.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

SKIP_PREFIXES = ("vtable for ", "typeinfo for ", "typeinfo name for ", "VTT for ", "guard variable for ",
                 "non-virtual thunk to ", "virtual thunk to ", "construction vtable for ",
                 "covariant return thunk to ", "TLS init function for ", "TLS wrapper function for ",
                 "reference temporary ")
NM_LINE = re.compile(r"^(?:[^\s:]+:)*\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z?])\s+(\S.*?)\s*$")
ABI_TAG = re.compile(r"\[abi:[A-Za-z0-9_]+\]")
DECL_EXT = (".h", ".hh", ".hpp")
SPECIFIERS = re.compile(r"\b(virtual|static|inline|explicit|extern|constexpr|friend|override|final|"
                        r"__attribute__\s*\(\(.*?\)\))\s*|\[\[[^\]]*\]\]\s*")
NOT_TYPES = {"return", "else", "new", "delete", "case", "throw", "co_return", "co_await", "goto", "do",
             "if", "while", "for", "switch", "sizeof", "typedef", "using", "operator", "default", "public",
             "private", "protected", "namespace", "template"}
BUILTIN_TYPES = {"int", "char", "bool", "float", "double", "void", "unsigned", "signed", "long", "short", "auto",
                 "size_t", "const", "volatile"}


class GenError(Exception):
    pass


# --- nm ---------------------------------------------------------------------------------------------

def parse_nm(text: str) -> Tuple[Set[str], Set[str]]:
    """(undefined, defined) demangled names in an `nm -C` listing."""
    undefined: Set[str] = set()
    defined: Set[str] = set()
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line or line.endswith(":") and " " not in line.strip():
            continue                      # blank, or an archive member's header ("foo.o:")
        m = NM_LINE.match(line)
        if not m:
            continue
        kind, name = m.group(1), m.group(2)
        if kind == "U":
            undefined.add(name)
        elif kind not in ("w", "v", "?"):  # weak undefined symbols need no definition
            defined.add(name)
    return undefined, defined


def missing_symbols(nm_texts: Iterable[str]) -> List[str]:
    undefined: Set[str] = set()
    defined: Set[str] = set()
    for t in nm_texts:
        u, d = parse_nm(t)
        undefined |= u
        defined |= d
    return sorted(s for s in undefined - defined if not s.startswith(SKIP_PREFIXES))


# --- demangled names --------------------------------------------------------------------------------

def _split_top(text: str, sep: str) -> List[str]:
    """TEXT split at SEP where no (), <> or [] is open."""
    out, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c in "(<[":
            depth += 1
        elif c in ")>]":
            depth -= 1
        if depth == 0 and text.startswith(sep, i):
            out.append("".join(cur))
            cur = []
            i += len(sep)
            continue
        cur.append(c)
        i += 1
    out.append("".join(cur))
    return out


class Symbol:
    """A demangled name: scopes, name, parameters (None for data) and the trailing qualifiers."""

    def __init__(self, demangled: str):
        self.demangled = demangled
        text = ABI_TAG.sub("", demangled)
        self.params: Optional[List[str]] = None
        self.trailer = ""
        qual = text
        if text.endswith(")") or re.search(r"\)\s*(const|volatile|&|&&|\s)*$", text):
            close = text.rfind(")")
            depth = 0
            open_ = -1
            for i in range(close, -1, -1):
                if text[i] == ")":
                    depth += 1
                elif text[i] == "(":
                    depth -= 1
                    if depth == 0:
                        open_ = i
                        break
            if open_ < 0:
                raise GenError("can't parse %r" % demangled)
            qual = text[:open_]
            inner = text[open_ + 1:close].strip()
            self.params = [] if inner in ("", "void") else [p.strip() for p in _split_top(inner, ",")]
            self.trailer = text[close + 1:].strip()
        parts = _split_top(qual.strip(), "::")
        if any(not p or "<" in p or " " in p.strip() or "(" in p for p in parts) or parts[-1].startswith("operator"):
            raise GenError("unsupported symbol (template, operator or local entity): %s" % demangled)
        self.scopes = parts[:-1]
        self.name = parts[-1]

    @property
    def qualified(self) -> str:
        return "::".join(self.scopes + [self.name])

    @property
    def is_function(self) -> bool:
        return self.params is not None

    def ends_with(self, qualified_tail: str) -> bool:
        q = "::" + self.qualified
        return q.endswith("::" + qualified_tail)


# --- declarations -----------------------------------------------------------------------------------

_LEX = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', re.S)


def blank(text: str) -> str:
    """TEXT with comments and literals blanked (newlines kept) and preprocessor lines emptied."""
    def sub(m: "re.Match[str]") -> str:
        s = m.group(0)
        if s.startswith(("//", "/*")):
            return re.sub(r"[^\n]", " ", s)
        return s[0] + " " * (len(s) - 2) + s[-1] if len(s) >= 2 else s
    code = _LEX.sub(sub, text)
    lines = code.split("\n")
    out, cont = [], False
    for ln in lines:
        if cont or ln.lstrip().startswith("#"):
            cont = ln.rstrip().endswith("\\")
            out.append("")
        else:
            out.append(ln)
    return "\n".join(out)


def _match_close(text: str, open_idx: int, op: str, cl: str) -> int:
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == op:
            depth += 1
        elif text[i] == cl:
            depth -= 1
            if depth == 0:
                return i
    return -1


class DeclFile:
    def __init__(self, path: str, text: str):
        self.path = path
        self.code = blank(text)
        self.is_header = path.endswith(DECL_EXT)

    def class_bodies(self, name: str) -> List[Tuple[int, int]]:
        out = []
        for m in re.finditer(r"\b(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?%s\b[^;{()]*\{" % re.escape(name), self.code):
            start = m.end() - 1
            end = _match_close(self.code, start, "{", "}")
            if end > 0:
                out.append((start + 1, end))
        return out


class Decl:
    def __init__(self, path: str, ret: str, nparams: int, noexcept: bool, variadic: bool):
        self.path = path
        self.ret = ret
        self.nparams = nparams
        self.noexcept = noexcept
        self.variadic = variadic


def _norm(t: str) -> str:
    return re.sub(r"\s+", " ", t).strip()


def _count_params(inner: str) -> Tuple[int, bool]:
    inner = inner.strip()
    if inner in ("", "void"):
        return 0, False
    parts = _split_top(inner, ",")
    return len(parts), parts[-1].strip().endswith("...")


def find_decls(files: Sequence[DeclFile], sym: Symbol, class_scope: Optional[str]) -> List[Decl]:
    """Declarations (or definitions) of SYM's name with a plausible return type."""
    out: List[Decl] = []
    name = sym.name
    pat = re.compile(r"(?<![\w~.>])(~?)%s\s*\(" % re.escape(name.lstrip("~")))
    for f in files:
        regions = f.class_bodies(class_scope) if class_scope else [(0, len(f.code))]
        for lo, hi in regions:
            seg = f.code[lo:hi]
            for m in pat.finditer(seg):
                if (m.group(1) == "~") != name.startswith("~"):
                    continue
                start = m.start()
                # The declaration's prefix: back to the previous statement boundary.
                j = start
                while j > 0 and seg[j - 1] not in ";{}":
                    j -= 1
                prefix = seg[j:start]
                prefix = re.sub(r"^\s*(public|private|protected)\s*:", "", prefix)
                prefix = re.sub(r"\btemplate\s*<[^>]*>", "", prefix)
                prefix = SPECIFIERS.sub(" ", prefix)
                ret = _norm(prefix)
                # A qualified definition (Ret Class::name): drop the qualifier.
                ret = re.sub(r"(?:\b[A-Za-z_]\w*\s*::\s*)+$", "", ret).strip()
                if any(c in ret for c in "=()?!+-/%^|") or ret.split(" ")[0] in NOT_TYPES:
                    continue
                if ret.endswith((",", "<")):
                    continue
                ctor = class_scope is not None and (name == class_scope or name == "~" + class_scope)
                if ctor != (ret == ""):
                    continue
                open_ = m.end() - 1
                close = _match_close(seg, open_, "(", ")")
                if close < 0:
                    continue
                after = seg[close + 1:close + 200]
                # What follows a declaration's ')': qualifiers, then ';', '{', '=', ':' or '->'.
                tail = re.match(r"\s*((?:const|volatile|noexcept(?:\s*\([^)]*\))?|override|final|&&|&|"
                                r"__attribute__\s*\(\([^)]*\)\)|\s)*)\s*(;|\{|=|:|->)", after)
                if not tail:
                    continue
                n, variadic = _count_params(seg[open_ + 1:close])
                out.append(Decl(f.path, ret, n, "noexcept" in tail.group(1), variadic))
    return out


def load_decl_files(paths: Sequence[str]) -> List[DeclFile]:
    out: List[DeclFile] = []
    seen: Set[str] = set()
    for p in paths:
        if os.path.isdir(p):
            for dirpath, dirnames, names in os.walk(p):
                dirnames.sort()
                for n in sorted(names):
                    if n.endswith(DECL_EXT):
                        full = os.path.join(dirpath, n)
                        if full not in seen:
                            seen.add(full)
                            out.append(_read_decl(full))
        elif os.path.isfile(p):
            if p not in seen:
                seen.add(p)
                out.append(_read_decl(p))
        else:
            raise GenError("--decls %s: no such file or directory" % p)
    return out


def _read_decl(path: str) -> DeclFile:
    with open(path, "rb") as f:
        return DeclFile(path, f.read().decode("utf-8", "replace"))


# --- what the dropped files define ------------------------------------------------------------------

def dropped_definitions(text: str) -> Set[str]:
    """Qualified names of what a source file defines with external linkage at namespace level:
    functions (`void f(`, `bool C::m(`, `C::C(`) and variables (`int g = 0;`, `T C::s;`), each
    prefixed with its enclosing named namespaces (`android::NanoMenu::nsTick`). Anonymous
    namespaces, static and const definitions, and everything inside a function or class body are
    skipped."""
    code = blank(text)
    out: Set[str] = set()
    stack: List[str] = []     # "ns", "anon", "fn", "class", "block"
    names: List[str] = []     # the namespace name each stack entry adds ("" for none)
    last = 0

    def qualify(q: str) -> str:
        return "::".join([n for n in names if n] + [re.sub(r"\s+", "", q)])
    fn_head = re.compile(r"((?:[A-Za-z_]\w*\s*::\s*)*~?[A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*"
                         r"(?:const\s*|noexcept\s*|override\s*|final\s*)*(?::[^;{}]*)?$")
    var_stmt = re.compile(r"\b((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?$")
    for i, c in enumerate(code):
        if c == "{":
            top = stack[-1] if stack else "ns"
            head = code[last:i]
            ns_name = ""
            nsm = re.search(r"\bnamespace\s+([\w:]+)\s*$", head)
            if top not in ("ns", "anon"):
                kind = "block"
            elif re.search(r"\bnamespace\s*$", head):
                kind = "anon"
            elif nsm or re.search(r'\bextern\s*"\s*"\s*$', head):
                kind = top
                ns_name = nsm.group(1) if nsm else ""
            elif re.search(r"\b(class|struct|union|enum)\b[^;()]*$", head):
                kind = "class"
            else:
                m = fn_head.search(head)
                if m and m.group(1).split("::")[-1].strip().lstrip("~") not in NOT_TYPES:
                    if top == "ns" and not re.search(r"\bstatic\b", head):
                        out.add(qualify(m.group(1)))
                    kind = "fn"
                else:
                    kind = "block"
            stack.append(kind)
            names.append(ns_name)
            last = i + 1
        elif c == "}":
            if stack:
                stack.pop()
                names.pop()
            last = i + 1
        elif c == ";":
            top = stack[-1] if stack else "ns"
            stmt = code[last:i]
            if top == "ns" and "(" not in stmt and not re.search(r"\b(static|typedef|using|extern|class|struct|"
                                                                  r"enum|union|namespace|template|const|"
                                                                  r"constexpr)\b", stmt):
                m = var_stmt.search(stmt)
                if m and len(stmt.split()) >= 2 and m.group(1).split("::")[-1] not in BUILTIN_TYPES:
                    out.add(qualify(m.group(1)))
            last = i + 1
    return out


# --- generation -------------------------------------------------------------------------------------

HEADER = """\
// Generated by nano/test/host/stubgen/gen.py at build time; do not edit and do not commit
// (IMPLEMENTATION.md C.1). Inert stubs for symbols of nano/build/sources.txt's [dropped-linux]
// files: each logs once and returns a zero value.
#include <type_traits>
#include <utility>

#include <nanostubs/stub.h>
"""

ZERO = """
namespace nanostub_gen {
template <typename T>
struct Zero {
    static T get() { return T(); }
};
template <>
struct Zero<void> {
    static void get() {}
};
template <typename T>
struct Zero<T&> {
    static T& get() {
        static typename std::remove_cv<T>::type v{};
        return v;
    }
};
template <typename T>
struct Zero<T&&> {
    static T&& get() {
        static T v{};
        return std::move(v);
    }
};
}  // namespace nanostub_gen
"""


def generate(missing: Sequence[str], decl_files: Sequence[DeclFile], dropped: Set[str],
             scopes: Sequence[str], include_root: Optional[str]) -> Tuple[str, List[str], List[str]]:
    """(the .cpp text, the stubbed demangled names, the errors)."""
    classes: Set[str] = set()
    class_pat = re.compile(r"\b(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)\b[^;{()]*\{")
    for f in decl_files:
        classes.update(class_pat.findall(f.code))
    selected: List[Symbol] = []
    errors: List[str] = []
    for d in missing:
        name = ABI_TAG.sub("", d)
        wanted = any(name.startswith(p) for p in scopes)
        try:
            sym = Symbol(d)
        except GenError as e:
            if wanted or any(name.split("(")[0].endswith("::" + q) or name.split("(")[0] == q for q in dropped):
                errors.append(str(e))
            continue
        if not wanted and not any(sym.ends_with(q) for q in dropped):
            continue
        selected.append(sym)
    includes: Set[str] = set()
    bodies: List[str] = []
    done: List[str] = []
    for sym in sorted(selected, key=lambda s: s.demangled):
        owner = sym.scopes[-1] if sym.scopes and sym.scopes[-1] in classes else None
        if not sym.is_function:
            # Data: needs a visible declaration to take decltype of.
            hits = [f for f in decl_files if f.is_header and re.search(r"\b%s\b" % re.escape(sym.name), f.code)
                    and re.search(r"\b(extern|static)\b[^;]*\b%s\s*(\[[^\]]*\])?\s*;" % re.escape(sym.name), f.code)]
            if not hits:
                errors.append("%s: no extern or static declaration in a header" % sym.demangled)
                continue
            includes.add(hits[0].path)
            bodies.append("decltype(%s) %s{};" % (sym.qualified, sym.qualified))
            done.append(sym.demangled)
            continue
        decls = [d for d in find_decls(decl_files, sym, owner)
                 if d.nparams == len(sym.params or []) or (d.variadic and len(sym.params or []) >= d.nparams - 1)]
        rets = sorted({_norm(d.ret) for d in decls})
        if not decls:
            errors.append("%s: no declaration found" % sym.demangled)
            continue
        if len(rets) > 1:
            errors.append("%s: ambiguous return type (%s)" % (sym.demangled, " | ".join(rets)))
            continue
        ret = rets[0]
        headers = [d.path for d in decls if d.path.endswith(DECL_EXT)]
        if headers:
            includes.add(sorted(headers)[0])
        elif owner:
            errors.append("%s: its class is declared in no header" % sym.demangled)
            continue
        noexcept = " noexcept" if any(d.noexcept for d in decls) else ""
        params = ", ".join(sym.params or [])
        trailer = (" " + sym.trailer) if sym.trailer else ""
        if owner and sym.name in (owner, "~" + owner):
            bodies.append("%s(%s)%s {}" % (sym.qualified, params, noexcept))
        elif owner:
            bodies.append("auto %s(%s)%s%s -> %s {\n    NANOSTUB_ONCE();\n    return ::nanostub_gen::Zero<%s>::get();\n}"
                          % (sym.qualified, params, trailer, noexcept, ret, ret))
        else:
            ns_open = "".join("namespace %s {\n" % s for s in sym.scopes)
            ns_close = "".join("}  // namespace %s\n" % s for s in reversed(sym.scopes))
            bodies.append("%sauto %s(%s)%s -> %s {\n    NANOSTUB_ONCE();\n    return ::nanostub_gen::Zero<%s>::get();\n}\n%s"
                          % (ns_open, sym.name, params, noexcept, ret, ret, ns_close.rstrip("\n")))
        done.append(sym.demangled)
    root = include_root
    inc_lines = []
    for p in sorted(includes):
        rel = os.path.relpath(p, root).replace(os.sep, "/") if root else p.replace(os.sep, "/")
        inc_lines.append('#include "%s"' % rel)
    text = HEADER + ("\n" + "\n".join(inc_lines) + "\n" if inc_lines else "") + ZERO
    text += "".join("\n// %s\n%s\n" % (d, b) for d, b in zip(done, bodies))
    return text, done, errors


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--nm", action="append", default=[], required=True)
    ap.add_argument("--from", dest="dropped", action="append", default=[])
    ap.add_argument("--scope", action="append", default=[])
    ap.add_argument("--decls", action="append", default=[], required=True)
    ap.add_argument("--include-root")
    ap.add_argument("--out")
    args = ap.parse_args(argv)
    try:
        texts = []
        for p in args.nm:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                texts.append(f.read())
        dropped: Set[str] = set()
        for p in args.dropped:
            with open(p, "rb") as f:
                dropped |= dropped_definitions(f.read().decode("utf-8", "replace"))
        decl_files = load_decl_files(args.decls)
        root = args.include_root or next((d for d in args.decls if os.path.isdir(d)), None)
        text, done, errors = generate(missing_symbols(texts), decl_files, dropped, args.scope, root)
    except (GenError, OSError) as e:
        print("stubgen: %s" % e, file=sys.stderr)
        return 1
    for e in errors:
        print("stubgen: %s" % e, file=sys.stderr)
    if errors:
        return 1
    if args.out:
        tmp = args.out + ".new"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, args.out)
    else:
        sys.stdout.write(text)
    print("stubgen: %d stub(s)" % len(done), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
