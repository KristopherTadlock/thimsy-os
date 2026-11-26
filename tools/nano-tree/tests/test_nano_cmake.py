"""Nano's CMake build, checked without CMake (docs/linux/IMPLEMENTATION.md C.2, step G6): the
machine has no CMake, so nano/CMakeLists.txt, nano/cmake/*.cmake, libthimsy/CMakeLists.txt and the
CMakeLists.txt of linux/src/thimsy-session and thimsy-padd are written, not configured. This test
reads them with a small CMake lexer and checks:

  - nano/cmake/sources.cmake's section logic, re-implemented here with the regular expressions read
    from that file, gives what tools/nano-tree/nanotree/sources.py gives on nano/build/sources.txt,
    and refuses what it refuses; the same for unit-closure.txt against the host Makefile's awk;
  - nano/cmake/androidbp_flags.cmake's reader, re-implemented the same way, gives what
    nanotree/androidbp.py's parser gives on nano/Android.bp for every list nano/CMakeLists.txt
    reads with it;
  - the CMake files use sources.txt's sections consistently ([firmware] never compiled), C.2's
    targets exist, the wrap flags match C.2 (and the host Makefile, and linker-wraps.txt), the GCC
    suppressions match the Makefile's, the compat and stub source globs match compat.mk and
    stubs.mk, the include order matches the include census, and every [core] file reaching a
    library the build has no package for is left out by a feature switch;
  - every -D option the Buildroot recipes pass (thimsy-nano, libthimsy, thimsy-session,
    thimsy-padd) is declared by that project's CMakeLists.txt; the local-site packages have
    their LICENSE file; libthimsy exports the package find_package(thimsy) needs.

Fork-only, standard library only, Python 3.8 or later; run with

  py -3.12 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests
"""
from __future__ import annotations

import json
import os
import re
import sys
import unittest
from typing import Dict, List, Optional, Sequence, Tuple

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.androidbp import Blueprint  # noqa: E402
from nanotree.config import NanoTreeError  # noqa: E402
from nanotree.sources import SourceList  # noqa: E402

NANO = os.path.join(ROOT, "nano")
CMAKELISTS = os.path.join(NANO, "CMakeLists.txt")
CMAKE_DIR = os.path.join(NANO, "cmake")
SOURCES = os.path.join(NANO, "build", "sources.txt")
CLOSURE = os.path.join(NANO, "build", "unit-closure.txt")
ANDROID_BP = os.path.join(NANO, "Android.bp")
HOST_MAKEFILE = os.path.join(NANO, "test", "host", "Makefile")
COMPAT_MK = os.path.join(NANO, "test", "host", "compat", "compat.mk")
STUBS_MK = os.path.join(NANO, "compat", "stubs", "stubs.mk")
CENSUS = os.path.join(TOOL_DIR, "include-census.json")
DESIGN = os.path.join(ROOT, "docs", "linux", "IMPLEMENTATION.md")
LINUX_WRAPS = os.path.join(NANO, "platform", "linux", "linker-wraps.txt")
LIBTHIMSY = os.path.join(ROOT, "libthimsy")
PACKAGES = os.path.join(ROOT, "linux", "package")
HAVE = os.path.isfile(CMAKELISTS) and os.path.isfile(SOURCES) and os.path.isfile(ANDROID_BP)

# Each Buildroot recipe built from this repository with CMake, and the CMakeLists.txt it configures.
CMAKE_PACKAGES = {
    "thimsy-nano": os.path.join(NANO, "CMakeLists.txt"),
    "libthimsy": os.path.join(LIBTHIMSY, "CMakeLists.txt"),
    "thimsy-session": os.path.join(ROOT, "linux", "src", "thimsy-session", "CMakeLists.txt"),
    "thimsy-padd": os.path.join(ROOT, "linux", "src", "thimsy-padd", "CMakeLists.txt"),
}


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# --- a small CMake lexer ---------------------------------------------------------------------------

def _unescape_quoted(body: str) -> str:
    """A CMake quoted argument's escapes (no variable references are expanded)."""
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "\n":
                i += 2
                continue
            out.append({"t": "\t", "n": "\n", "r": "\r", ";": "\\;"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


class Command:
    def __init__(self, name: str, args: List[str], line: int):
        self.name = name
        self.args = args
        self.line = line

    def __repr__(self) -> str:
        return "%s(%s) at line %d" % (self.name, " ".join(self.args), self.line)


def cmake_commands(text: str) -> List[Command]:
    """Every command invocation, in order, with its arguments (quoted ones unescaped, unquoted ones
    as written; parentheses inside the arguments are tokens of their own)."""
    out: List[Command] = []
    i, n = 0, len(text)
    ident = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    while i < n:
        c = text[i]
        if c == "#":
            m = re.match(r"#\[(=*)\[", text[i:])
            if m:
                end = text.find("]" + m.group(1) + "]", i)
                i = n if end < 0 else end + len(m.group(1)) + 2
            else:
                j = text.find("\n", i)
                i = n if j < 0 else j
            continue
        if c.isalpha() or c == "_":
            m = ident.match(text, i)
            name = m.group(0)
            j = m.end()
            while j < n and text[j] in " \t":
                j += 1
            if j < n and text[j] == "(":
                line = text.count("\n", 0, i) + 1
                args, i = _cmake_args(text, j + 1)
                out.append(Command(name.lower(), args, line))
                continue
            i = m.end()
            continue
        i += 1
    return out


def _cmake_args(text: str, i: int) -> Tuple[List[str], int]:
    args: List[str] = []
    depth = 1
    n = len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "#":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            args.append(_unescape_quoted(text[i + 1:j]))
            i = j + 1
        elif c == "(":
            depth += 1
            args.append("(")
            i += 1
        elif c == ")":
            depth -= 1
            i += 1
            if depth == 0:
                return args, i
            args.append(")")
        else:
            j = i
            while j < n and text[j] not in " \t\r\n()#":
                if text[j] == '"':
                    k = text.find('"', j + 1)
                    j = n if k < 0 else k + 1
                    continue
                j += 1
            args.append(text[i:j])
            i = j
    raise AssertionError("unterminated command arguments")


def cmake_set(cmds: Sequence[Command], var: str) -> List[str]:
    """The values of the first set(<var> ...), without CACHE/PARENT_SCOPE trailers."""
    for c in cmds:
        if c.name == "set" and c.args and c.args[0] == var:
            vals = c.args[1:]
            for stop in ("CACHE", "PARENT_SCOPE"):
                if stop in vals:
                    vals = vals[:vals.index(stop)]
            return vals
    raise AssertionError("no set(%s ...)" % var)


def cmake_options(cmds: Sequence[Command]) -> Dict[str, str]:
    """The project's options and cache variables: name -> default."""
    out: Dict[str, str] = {}
    for c in cmds:
        if c.name == "option" and c.args:
            out[c.args[0]] = c.args[2] if len(c.args) > 2 else "OFF"
        elif c.name == "set" and "CACHE" in c.args:
            out[c.args[0]] = c.args[1] if c.args.index("CACHE") > 1 else ""
    return out


def cmake_targets(cmds: Sequence[Command]) -> Dict[str, Command]:
    out: Dict[str, Command] = {}
    for c in cmds:
        if c.name in ("add_library", "add_executable", "add_custom_target") and c.args:
            out[c.args[0]] = c
    return out


def cmake_regex(pattern: str) -> "re.Pattern[str]":
    """A CMake regular expression as Python's. The patterns these files use are in the common
    subset (anchors, classes, groups, alternation, * + ?); anything else is refused here."""
    if re.search(r"\\[dswDSWbB]|\(\?|\{[0-9]", pattern):
        raise AssertionError("not a CMake regular expression: %r" % pattern)
    return re.compile(pattern)


def file_strings(text: str, regex: "re.Pattern[str]") -> List[str]:
    """CMake's file(STRINGS ... ENCODING UTF-8 REGEX ...): lines (any control character but a tab
    ends one), keeping the ones the regex matches."""
    cut = re.sub(r"[\x00-\x08\x0a-\x1f\x7f]", "\n", text)
    return [s for s in cut.split("\n") if regex.search(s)]


# --- the mirror of nano/cmake/sources.cmake ---------------------------------------------------------

class CMakeError(Exception):
    pass


class SourcesMirror:
    def __init__(self, cmds: Sequence[Command]):
        self.line = cmake_regex(cmake_set(cmds, "NANO_SOURCES_LINE_REGEX")[0])
        self.header = cmake_regex(cmake_set(cmds, "NANO_SOURCES_HEADER_REGEX")[0])
        self.path = cmake_regex(cmake_set(cmds, "NANO_SOURCES_PATH_REGEX")[0])
        self.up = cmake_regex(cmake_set(cmds, "NANO_SOURCES_UP_REGEX")[0])
        self.sections = cmake_set(cmds, "NANO_SOURCE_SECTIONS")
        self.closure_line = cmake_regex(cmake_set(cmds, "NANO_CLOSURE_LINE_REGEX")[0])

    def read_sources(self, text: str) -> Dict[str, List[str]]:
        """nano_read_sources(): section name -> paths; CMakeError where CMake stops with FATAL_ERROR."""
        secs: Dict[str, List[str]] = {s: [] for s in self.sections}
        seen = set()
        cur = None
        for raw in file_strings(text, self.line):
            line = raw.strip(" \t\r\n")
            m = self.header.search(line)
            if m:
                name = m.group(1)
                if name not in self.sections:
                    raise CMakeError("unknown section [%s]" % name)
                if name in seen:
                    raise CMakeError("section [%s] appears twice" % name)
                seen.add(name)
                cur = name
                continue
            if cur is None:
                raise CMakeError("%r is outside any section" % line)
            if not self.path.search(line) or line.startswith("/") or self.up.search(line):
                raise CMakeError("not a relative path: %r" % line)
            if line in secs[cur]:
                raise CMakeError("%s is listed twice" % line)
            secs[cur].append(line)
        return secs

    def read_closure(self, text: str) -> Dict[str, Tuple[List[str], List[str], List[str]]]:
        """nano_read_unit_closure(): program -> (sources, headers, asset folders)."""
        out: Dict[str, Tuple[List[str], List[str], List[str]]] = {}
        for line in file_strings(text, self.closure_line):
            fields = line.split("\t")
            if len(fields) < 3:
                continue
            prog, srcs, needs = fields[0], fields[1], fields[2]
            srcs = "" if srcs == "-" else srcs
            needs = "" if needs == "-" else needs
            headers, assets = [], []
            for w in needs.split(" "):
                if not w:
                    continue
                if w.startswith("@") and len(w) > 1:
                    assets.append(w[1:])
                else:
                    headers.append(w)
            out[prog] = ([s for s in srcs.split(" ") if s], headers, assets)
        return out


# --- the mirror of nano/cmake/androidbp_flags.cmake -------------------------------------------------

class BpMirror:
    NAMES = ("MODULE_START", "MODULE_END", "NAME", "MAP_START", "MAP_END", "LIST_START", "LIST_END",
             "LIST_INLINE_END", "CODE", "STRING")

    def __init__(self, cmds: Sequence[Command]):
        self.re = {k: cmake_regex(cmake_set(cmds, "NANO_BP_RE_" + k)[0]) for k in self.NAMES}

    @staticmethod
    def lines(text: str) -> List[str]:
        for a, b in ((";", "<SEMI>"), ("\\", "<BS>"), ("[", "<LB>"), ("]", "<RB>"), ("\r", "")):
            text = text.replace(a, b)
        return text.split("\n")

    def strings(self, line: str) -> List[str]:
        m = self.re["CODE"].search(line)
        if not m:
            return []
        out = []
        for q in self.re["STRING"].findall(m.group(1)):
            v = q[1:-1]
            if "<SEMI>" in v:
                raise CMakeError("a value holds ';'")
            out.append(v.replace("<BS>", "\\").replace("<LB>", "[").replace("<RB>", "]"))
        return out

    def bp_list(self, text: str, module: str, path: str) -> List[str]:
        R = self.re
        in_module = False
        module_lines: List[str] = []
        found = False
        name = ""
        for line in self.lines(text):
            if not in_module:
                if R["MODULE_START"].search(line):
                    in_module, module_lines, name = True, [], ""
                continue
            if R["MODULE_END"].search(line):
                in_module = False
                if name == module:
                    found = True
                    break
                continue
            if name == "":
                m = R["NAME"].search(line)
                if m:
                    name = m.group(1)
            module_lines.append(line)
        if not found:
            raise CMakeError("no module named %s" % module)
        stack: List[str] = []
        mode = ""
        list_indent = ""
        result: List[str] = []
        for line in module_lines:
            if mode:
                m = R["LIST_END"].search(line)
                if m and m.group(1) == list_indent:
                    if mode == "want":
                        break
                    mode = ""
                elif mode == "want":
                    result.extend(self.strings(line))
                continue
            m = R["MAP_START"].search(line)
            if m:
                stack.append(m.group(2))
                continue
            if R["MAP_END"].search(line):
                if stack:
                    stack.pop()
                continue
            m = R["LIST_START"].search(line)
            if m:
                indent, key, rest = m.group(1), m.group(2), m.group(3)
                inline = bool(R["LIST_INLINE_END"].search(rest))
                if ".".join(stack + [key]) == path:
                    result.extend(self.strings(rest))
                    if inline:
                        break
                    mode = "want"
                elif not inline:
                    mode = "skip"
                list_indent = indent
        return result


def bp_value(bp: Blueprint, module: str, path: str) -> List[str]:
    """The same list from androidbp.py's real parser."""
    v: object = bp.module(module)
    assert v is not None, module
    for k in path.split("."):
        if not isinstance(v, dict) or k not in v:
            return []
        v = v[k]
    assert isinstance(v, list), (module, path, v)
    return [x for x in v if isinstance(x, str)]


# --- make and recipe readers -----------------------------------------------------------------------

def make_var(path: str, var: str) -> List[str]:
    """A make variable's words (`:=`/`=`/`?=`, continuation lines joined; first assignment)."""
    text = _read(path).replace("\\\n", " ")
    m = re.search(r"^%s\s*[:?]?=(.*)$" % re.escape(var), text, re.M)
    assert m, "%s sets no %s" % (path, var)
    return m.group(1).split()


def recipe_conf_opts(pkg: str) -> Dict[str, str]:
    """-DNAME=VALUE of every <PKG>_CONF_OPTS assignment in the recipe (both arms of an ifeq)."""
    mk = os.path.join(PACKAGES, pkg, pkg + ".mk")
    text = _read(mk).replace("\\\n", " ")
    prefix = pkg.upper().replace("-", "_")
    out: Dict[str, str] = {}
    for m in re.finditer(r"^%s_CONF_OPTS\s*\+?=(.*)$" % prefix, text, re.M):
        for w in m.group(1).split():
            d = re.match(r"^-D([A-Za-z_][A-Za-z0-9_]*)(?::[A-Z]+)?=(.*)$", w)
            assert d, "%s: %r is not a -D option" % (mk, w)
            out.setdefault(d.group(1), d.group(2))
    return out


def design_c2() -> str:
    text = _read(DESIGN)
    start = text.index("### C.2 CMake")
    return text[start:text.index("### C.3", start)]


# --- the tests -------------------------------------------------------------------------------------

@unittest.skipUnless(HAVE, "nano/CMakeLists.txt, sources.txt or Android.bp missing")
class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _read(CMAKELISTS)
        cls.cmds = cmake_commands(cls.text)
        cls.sources_cmds = cmake_commands(_read(os.path.join(CMAKE_DIR, "sources.cmake")))
        cls.bp_cmds = cmake_commands(_read(os.path.join(CMAKE_DIR, "androidbp_flags.cmake")))
        cls.features_cmds = cmake_commands(_read(os.path.join(CMAKE_DIR, "features.cmake")))
        cls.mirror = SourcesMirror(cls.sources_cmds)
        cls.bpm = BpMirror(cls.bp_cmds)
        cls.src = SourceList.load(SOURCES)
        cls.bp = Blueprint.load(ANDROID_BP)
        cls.targets = cmake_targets(cls.cmds)
        cls.options = cmake_options(cls.cmds)


class Lexer(unittest.TestCase):
    def test_commands_and_quoting(self):
        cmds = cmake_commands('# c (x)\nset(A "a\\\\b\\"c\\t" b) # tail\nif(NOT (X OR Y))\nendif()\n'
                              'foo  ("x;y" $<A:b>)\n')
        self.assertEqual([c.name for c in cmds], ["set", "if", "endif", "foo"])
        self.assertEqual(cmds[0].args, ["A", 'a\\b"c\t', "b"])
        self.assertEqual(cmds[1].args, ["NOT", "(", "X", "OR", "Y", ")"])
        self.assertEqual(cmds[3].args, ["x;y", "$<A:b>"])
        self.assertEqual(cmds[3].line, 5)

    def test_file_strings(self):
        rx = cmake_regex("^[ \t]*[^# \t]")
        self.assertEqual(file_strings("a\n# b\n\n  # c\n\tx\r\n", rx), ["a", "\tx"])


class SourcesLogic(Base):
    def test_matches_sources_py_on_the_real_list(self):
        got = self.mirror.read_sources(_read(SOURCES))
        for name in self.mirror.sections:
            self.assertEqual(got[name], self.src[name], "[%s]" % name)

    def test_sections_match_sources_py(self):
        from nanotree.sources import SECTIONS
        self.assertEqual(tuple(self.mirror.sections), SECTIONS)

    def test_same_verdicts_on_crafted_lists(self):
        cases = ["# c\n[core]\na.cpp\n\n  sub/b.cpp  \n[drm]\n# x\n[firmware]\nm.cpp\n",
                 "  # indented comment [x]\n[core]\nx@y+z.cpp\n",
                 "a.cpp\n", "[nope]\n", "[core]\n[core]\n", "[core]\na.cpp\na.cpp\n", "[core]\n/abs.cpp\n",
                 "[core]\n../up.cpp\n", "[core]\na/../b.cpp\n", "[core]\na b.cpp\n", "[core]\na;b.cpp\n",
                 "[core]\na[1].cpp\n", "[Core]\n", "[core]\r\na.cpp\r\n"]
        for text in cases:
            try:
                want: Optional[Dict[str, List[str]]] = SourceList.parse(text).sections
            except NanoTreeError:
                want = None
            try:
                got: Optional[Dict[str, List[str]]] = self.mirror.read_sources(text)
            except CMakeError:
                got = None
            if want is None:
                self.assertIsNone(got, "sources.cmake accepts what sources.py refuses: %r" % text)
            else:
                self.assertIsNotNone(got, "sources.cmake refuses what sources.py accepts: %r" % text)
                for name in self.mirror.sections:
                    self.assertEqual(got[name], want.get(name, []), (text, name))

    def test_kept_lines_are_safe_cmake_list_items(self):
        # file(STRINGS) returns a list: a kept line with ';' or an unbalanced bracket would break it.
        for line in file_strings(_read(SOURCES), self.mirror.line):
            self.assertNotIn(";", line)
            if not self.mirror.header.search(line.strip()):
                self.assertNotRegex(line, r"[\[\]]")

    def test_closure_matches_the_makefile_reading(self):
        got = self.mirror.read_closure(_read(CLOSURE))
        want = {}
        for line in _read(CLOSURE).split("\n"):          # the Makefile's awk: -F '\t', skip # and NF < 3
            f = line.split("\t")
            if line.startswith("#") or len(f) < 3:
                continue
            needs = [] if f[2] == "-" else f[2].split()
            want[f[0]] = ([] if f[1] == "-" else f[1].split(), [w for w in needs if not w.startswith("@")],
                          [w[1:] for w in needs if w.startswith("@")])
        self.assertEqual(got, want)
        tests = {os.path.basename(p)[:-len(".cpp")] for p in self.src.test_programs()}
        self.assertEqual(set(got), tests)
        prod = set(self.src.test_production())
        for prog, (srcs, _h, _a) in got.items():
            self.assertLessEqual(set(srcs), prod, prog)


class Sections(Base):
    def test_every_section_variable_is_a_section(self):
        used = set(re.findall(r"NANO_SRC_([A-Za-z0-9_]+)", self.text))
        known = {s.replace("-", "_") for s in self.mirror.sections}
        self.assertLessEqual(used, known)
        for need in ("core", "drm", "dropped_linux", "linux", "host", "scrape"):
            self.assertIn(need, used, "nano/CMakeLists.txt never reads [%s]" % need)

    def test_firmware_is_never_compiled(self):
        self.assertNotIn("NANO_SRC_firmware", self.text)

    def test_reads_the_lists_with_sources_cmake(self):
        calls = [c for c in self.cmds if c.name == "nano_read_sources"]
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0].args, ["${NANO_DIR}/build/sources.txt", "NANO_SRC"])
        self.assertTrue(any(c.name == "nano_read_unit_closure" and c.args[0] == "${NANO_DIR}/build/unit-closure.txt"
                            for c in self.cmds))
        self.assertIn("include", [c.name for c in self.cmds])
        includes = {c.args[0] for c in self.cmds if c.name == "include"}
        for mod in ("sources", "androidbp_flags", "features", "deps", "stubgen"):
            self.assertIn(mod, includes)
            self.assertTrue(os.path.isfile(os.path.join(CMAKE_DIR, mod + ".cmake")), mod)

    def test_no_file_list_is_written_in_cmake(self):
        # One list per fact (Principle 5): no Nano .cpp is named in the CMake files, except the
        # platform sections' fixed entry points and the feature table.
        allowed = {"platform/linux/main_linux.cpp", "platform/linux/ShellGate.cpp",
                   "platform/linux/display/DrmBuffersGbm.cpp", "platform/linux/display/EglWraps.cpp",
                   "test/host/headless/main_headless.cpp", "test/host/glstub/glue.c"}
        args = " ".join(a for c in self.cmds for a in c.args)      # code only, comments left out
        named = set(re.findall(r"[\w/]+\.(?:cpp|c)\b", args))
        self.assertLessEqual(named, allowed)
        for p in allowed:
            self.assertIn(p, self.text)

    def test_platform_entry_points_match_the_design(self):
        # The fixed paths are the ones section G gives G9 and G5, and the [host] comment of sources.txt.
        self.assertEqual(cmake_set(self.cmds, "NANO_LINUX_MAIN"), ["platform/linux/main_linux.cpp"])
        self.assertEqual(cmake_set(self.cmds, "NANO_HOST_MAIN"), ["test/host/headless/main_headless.cpp"])
        design = _read(DESIGN)
        for p in ("nano/platform/linux/main_linux.cpp", "nano/platform/linux/ShellGate.cpp",
                  "DrmBuffersGbm.cpp", "main_headless.cpp"):
            self.assertIn(p, design)

    def test_platform_sections_name_existing_files_once_present(self):
        for sec in ("linux", "host"):
            for p in self.src[sec]:
                self.assertTrue(os.path.isfile(os.path.join(NANO, p)), "[%s] %s" % (sec, p))


class Targets(Base):
    def test_c2_targets_exist(self):
        rows = re.findall(r"^\| `([A-Za-z0-9_-]+)` \|", design_c2(), re.M)
        self.assertIn("nano_core", rows)
        self.assertGreaterEqual(len(rows), 9)
        for t in rows:
            self.assertIn(t, self.targets, "C.2's %s is not defined in nano/CMakeLists.txt" % t)

    def test_kinds(self):
        lib = {t for t, c in self.targets.items() if c.name == "add_library"}
        exe = {t for t, c in self.targets.items() if c.name == "add_executable"}
        for t in ("nano_compat", "nano_core", "nano_platform_linux", "nano_platform_host", "nano_glstub",
                  "nano_stubs", "nano_shellgate"):
            self.assertIn(t, lib)
            self.assertEqual(self.targets[t].args[1], "STATIC", t)
        for t in ("thimsy-nano", "thimsy-nano-scrape", "nano-headless"):
            self.assertIn(t, exe)
        self.assertEqual(self.targets["nano_unit_tests"].name, "add_custom_target")

    def test_installs(self):
        installed = [c.args[1] for c in self.cmds if c.name == "install" and c.args[:1] == ["TARGETS"]]
        self.assertEqual(sorted(installed), ["thimsy-nano", "thimsy-nano-scrape"])
        # thimsy-session starts them from /usr/bin (Session.h).
        session_h = _read(os.path.join(ROOT, "linux", "src", "thimsy-session", "src", "Session.h"))
        self.assertIn('"/usr/bin/thimsy-nano"', session_h)
        self.assertIn('"/usr/bin/thimsy-nano-scrape"', session_h)
        for c in self.cmds:
            if c.name == "install":
                self.assertIn("${CMAKE_INSTALL_BINDIR}", c.args)

    def test_guarded_targets_check_their_entry_points(self):
        # Targets whose sources other steps write fail at configure with a message, not at link.
        for var in ("NANO_LINUX_MAIN", "NANO_LINUX_DRM_BUFFERS", "NANO_LINUX_EGL_WRAPS", "NANO_HOST_MAIN"):
            self.assertRegex(self.text, r"if\(NOT %s IN_LIST NANO_SRC_(linux|host)\)" % var)
        self.assertIn('if(NANO_LINUX_SHELLGATE IN_LIST NANO_SRC_linux)', self.text)
        self.assertIn('NOT EXISTS "${NANO_DIR}/${NANO_GLSTUB_GLUE}"', self.text)

    def test_mutually_calling_archives_are_grouped(self):
        groups = [c.args for c in self.cmds if c.name == "nano_link_group"]
        self.assertIn(["thimsy-nano", "nano_platform_linux", "nano_core"], groups)
        self.assertIn(["nano-headless", "nano_platform_host", "nano_core"], groups)

    def test_stub_generation_covers_dropped_and_left_out_files(self):
        self.assertEqual(cmake_set(self.cmds, "NANO_STUBGEN_FROM"), ["${NANO_SRC_dropped_linux}", "${NANO_LEFT_OUT}"])
        calls = [c for c in self.cmds if c.name == "nano_dropped_stubs"]
        self.assertEqual(sorted(c.args[1] for c in calls), ["host", "linux"])
        for c in calls:
            self.assertEqual(c.args[-1], "${NANO_STUBGEN_FROM}")
            archives = c.args[c.args.index("ARCHIVES") + 1:c.args.index("FROM")]
            for a in ("nano_core", "nano_stubs", "nano_compat"):
                self.assertIn(a, archives)
        stubgen = _read(os.path.join(CMAKE_DIR, "stubgen.cmake"))
        self.assertIn("test/host/stubgen/gen.py", stubgen)
        self.assertTrue(os.path.isfile(os.path.join(NANO, "test", "host", "stubgen", "gen.py")))


class LinuxPlatform(Base):
    """thimsy-nano as G9's files make it (integ3): the default NANO_PLATFORM=linux configures with the
    real sources.txt, and each piece goes where C.2 and B.8 say."""

    def links(self, target):
        out = []
        for c in self.cmds:
            if c.name == "target_link_libraries" and c.args and c.args[0] == target:
                out += c.args[1:]
        return out

    def test_linux_entry_points_are_listed(self):
        for var in ("NANO_LINUX_MAIN", "NANO_LINUX_SHELLGATE", "NANO_LINUX_DRM_BUFFERS", "NANO_LINUX_EGL_WRAPS"):
            path = cmake_set(self.cmds, var)
            self.assertEqual(len(path), 1, var)
            self.assertIn(path[0], self.src["linux"], "%s (%s) is not in [linux]" % (var, path[0]))

    def test_platform_library_sources(self):
        # set(_plat ${NANO_SRC_linux} ${NANO_SRC_drm}) minus the main and the shell gate.
        self.assertIn('list(REMOVE_ITEM _plat "${NANO_LINUX_MAIN}" "${NANO_LINUX_SHELLGATE}")', self.text)
        plat = [p for p in self.src["linux"] + self.src["drm"]
                if p not in ("platform/linux/main_linux.cpp", "platform/linux/ShellGate.cpp")]
        for p in ("platform/linux/display/DrmBuffersGbm.cpp", "platform/linux/display/EglWraps.cpp",
                  "platform/linux/ControlKeys.cpp", "NanoPlatform.cpp", "NanoMenuDrm.cpp"):
            self.assertIn(p, plat)
        # Android's back end and entry point never: the GBM back end and main_linux stand in for them.
        for p in ("NanoMenuDrmBuffers.cpp", "main.cpp"):
            self.assertNotIn(p, plat)
            self.assertIn(p, self.src["firmware"])
        self.assertEqual([p for p in plat if "/tests/" in p], [])

    def test_links_gbm_egl_gles_and_the_stubs(self):
        plat = self.links("nano_platform_linux")
        for lib in ("nano_core", "nano_stubs", "nano_shellgate", "nano::gbm", "nano::egl", "nano::glesv2", "nano::drm"):
            self.assertIn(lib, plat)
        self.assertIn("nano_stubs", self.links("nano_platform_host"))
        self.assertIn("nano_stubs", self.links("nano_core"))
        # NANOSTUBS_CPPFLAGS reach nano_core through nano_stubs' PUBLIC usage requirements.
        self.assertIn("target_compile_definitions(nano_stubs PUBLIC EGL_NO_PLATFORM_SPECIFIC_TYPES)", self.text)
        self.assertIn('target_include_directories(nano_stubs PUBLIC "${NANO_DIR}/compat/stubs/include")', self.text)
        self.assertIn('"${NANO_DIR}/compat/stubs/include"', self.text)
        stubs_mk = _read(os.path.join(NANO, "compat", "stubs", "stubs.mk"))
        self.assertIn("-DEGL_NO_PLATFORM_SPECIFIC_TYPES", stubs_mk)

    def test_every_wrapped_symbol_has_its_wrapper_in_linux(self):
        names = [ln.split("#")[0].strip() for ln in _read(LINUX_WRAPS).split("\n")]
        names = [n for n in names if n]
        code = "\n".join(_read(os.path.join(NANO, p)) for p in self.src["linux"])
        for n in names:
            self.assertRegex(code, r'extern "C" [^;{]*\b__wrap_%s\s*\(' % n,
                             "linker-wraps.txt wraps %s; no [linux] file defines __wrap_%s" % (n, n))

    def test_stubgen_reads_every_linux_archive(self):
        call = [c for c in self.cmds if c.name == "nano_dropped_stubs" and c.args[1] == "linux"][0]
        archives = call.args[call.args.index("ARCHIVES") + 1:call.args.index("FROM")]
        self.assertEqual(archives, ["nano_linux_main", "nano_platform_linux", "nano_shellgate", "nano_core",
                                    "nano_stubs", "nano_compat"])
        self.assertIn('add_executable(thimsy-nano "${_linux_stubs}")', self.text)


class Flags(Base):
    def test_bp_reader_matches_androidbp_py_for_every_list_read(self):
        bp_text = _read(ANDROID_BP)
        calls = [c for c in self.cmds if c.name == "nano_bp_list"]
        self.assertGreaterEqual(len(calls), 5)
        for c in calls:
            module, path = c.args[1], c.args[2]
            got = self.bpm.bp_list(bp_text, module, path)
            self.assertEqual(got, bp_value(self.bp, module, path), "%s %s" % (module, path))
            self.assertTrue(got, "%s %s is empty" % (module, path))

    def test_bp_reader_on_other_lists(self):
        bp_text = _read(ANDROID_BP)
        for module, path in (("gammaos-nano", "srcs"), ("gammaos-nano", "arch.arm64.cflags"),
                             ("gammaos-nano", "static_libs"), ("gammaos-nano", "shared_libs"),
                             ("gammaos-nano-scrape", "srcs"), ("liba52_nano", "srcs"),
                             ("liba52_nano", "local_include_dirs"), ("gammaos-nano", "no_such_property")):
            self.assertEqual(self.bpm.bp_list(bp_text, module, path), bp_value(self.bp, module, path),
                             "%s %s" % (module, path))
        with self.assertRaises(CMakeError):
            self.bpm.bp_list(bp_text, "no-such-module", "srcs")

    def test_bp_reader_on_crafted_layouts(self):
        text = ('cc_defaults {\n    name: "d",\n    cflags: ["-Wnope"],\n}\n\n'
                'cc_binary {\n    name: "m",\n    srcs: [\n        "a.cpp", // the "b.cpp" comment\n'
                '        "c.cpp",\n    ],\n    target: {\n        host: {\n            cflags: [\n'
                '                "-DHOST",\n            ],\n        },\n    },\n'
                '    cflags: ["-Wall", "-DX=1"], // trailing\n    shared_libs: [],\n}\n')
        bp = Blueprint(text)
        for path in ("srcs", "cflags", "target.host.cflags", "shared_libs"):
            self.assertEqual(self.bpm.bp_list(text, "m", path), bp_value(bp, "m", path), path)
        self.assertEqual(self.bpm.bp_list(text, "m", "cflags"), ["-Wall", "-DX=1"])
        self.assertEqual(self.bpm.bp_list(text, "d", "cflags"), ["-Wnope"])

    def test_c2_flags(self):
        cflags = bp_value(self.bp, "gammaos-nano", "cflags")
        for f in re.findall(r"`(-[DOWf][A-Za-z0-9_=-]*)`", design_c2().split("Flags:", 1)[1].split("Feature switches")[0]):
            if f == "-Werror":
                continue
            self.assertIn(f, cflags, "C.2 names %s as a gammaos-nano cflag" % f)
        self.assertEqual(cmake_set(self.cmds, "CMAKE_CXX_STANDARD"), ["17"])
        self.assertEqual(cmake_set(self.cmds, "CMAKE_CXX_EXTENSIONS"), ["ON"])     # gnu++17
        self.assertIn("list(REMOVE_ITEM NANO_BP_CFLAGS -Werror)", self.text)
        self.assertEqual(self.options.get("NANO_WERROR"), "OFF")
        # The unit tests build with -Werror (C.2: on for the host tests).
        self.assertRegex(self.text, r"target_compile_options\(nano_unit_flags INTERFACE -Wall -Werror")

    def test_gcc_suppressions_match_the_makefile(self):
        self.assertEqual(cmake_set(self.cmds, "NANO_GCC_SUPPRESS"), make_var(HOST_MAKEFILE, "GCC_SUPPRESS"))

    def test_wraps_match_c2(self):
        c2 = design_c2()
        row = re.search(r"^\| `thimsy-nano` \|.*$", c2, re.M).group(0)
        shell = re.findall(r"--wrap=([a-z_]+)", re.search(r"`(-Wl,--wrap=[^`]+)`", row).group(1))
        self.assertEqual(cmake_set(self.cmds, "NANO_SHELL_WRAPS"), shell)
        headless_row = re.search(r"^\| `nano-headless` \|.*$", c2, re.M).group(0)
        extra = re.findall(r"`--wrap=([a-z_]+)`", headless_row)
        self.assertEqual(cmake_set(self.cmds, "NANO_HEADLESS_WRAPS"), ["${NANO_SHELL_WRAPS}"] + extra)
        make = [w for f in make_var(HOST_MAKEFILE, "HEADLESS_WRAP") for w in re.findall(r"--wrap=([a-z_]+)", f)]
        self.assertEqual(make, shell + extra)
        scrape_row = re.search(r"^\| `thimsy-nano-scrape` \|.*$", c2, re.M).group(0)
        self.assertIn("the same wrap", scrape_row)
        wraps = {c.args[0]: c.args[1:] for c in self.cmds if c.name == "nano_wrap"}
        self.assertEqual(wraps, {"thimsy-nano": ["${NANO_LINUX_WRAPS}"],
                                 "nano-headless": ["${NANO_HEADLESS_WRAPS}"],
                                 "thimsy-nano-scrape": ["${NANO_SHELL_WRAPS}"]})
        self.assertIn('"LINKER:--wrap=${_s}"', self.text)

    def test_linker_wraps_file_holds_the_shell_list(self):
        if not os.path.isfile(LINUX_WRAPS):
            self.skipTest("platform/linux/linker-wraps.txt is G9's, not in this tree yet")
        names = [ln.split("#")[0].strip() for ln in _read(LINUX_WRAPS).split("\n")]
        names = [n for n in names if n]
        rx = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*[ \t]*$")      # the REGEX nano/CMakeLists.txt reads it with
        self.assertIn('REGEX "^[A-Za-z_][A-Za-z0-9_]*[ \\t]*$"', self.text)
        self.assertEqual([ln.strip() for ln in _read(LINUX_WRAPS).split("\n") if rx.search(ln)], names)
        for w in cmake_set(self.cmds, "NANO_SHELL_WRAPS"):
            self.assertIn(w, names)

    def test_include_order_matches_the_census(self):
        census = json.loads(_read(CENSUS))["include_dirs"]
        local = bp_value(self.bp, "gammaos-nano", "local_include_dirs")
        self.assertEqual(census[:3 + len(local)], ["compat/include", "compat/stubs/include", ""] + local)
        self.assertIn('set(_core_includes "${NANO_DIR}/compat/include" "${NANO_DIR}/compat/stubs/include" "${NANO_DIR}")',
                      self.text)

    def test_compat_and_stub_globs_match_their_makefiles(self):
        def stems(d):
            return sorted(f[:-4] for f in os.listdir(d) if f.endswith(".cpp"))
        self.assertEqual(stems(os.path.join(NANO, "compat", "src")), sorted(make_var(COMPAT_MK, "COMPAT_SRCS")))
        self.assertEqual(stems(os.path.join(NANO, "compat", "stubs", "src")), sorted(make_var(STUBS_MK, "STUBS_SRCS")))
        self.assertIn('"${NANO_DIR}/compat/src/*.cpp"', self.text)
        self.assertIn('"${NANO_DIR}/compat/stubs/src/*.cpp"', self.text)
        self.assertIn('"SHELL:-include nanocompat/bionic.h"', self.text)
        self.assertIn("EGL_NO_PLATFORM_SPECIFIC_TYPES", self.text)

    def test_image_formats_match_compat_mk(self):
        # compat.mk links a library for each image header that exists; so does nano_compat.
        mk = _read(COMPAT_MK)
        mk_headers = re.findall(r"compat_has_header,([a-z_/]+\.h)\),-l([a-z]+)", mk)
        self.assertEqual(sorted(h for h, _ in mk_headers),
                         sorted(["png.h", "jpeglib.h", "webp/decode.h", "webp/demux.h", "gif_lib.h"]))
        calls = [c.args for c in self.cmds if c.name == "nano_image_format"]
        cmake_headers = {a[1] for a in calls} | {"webp/demux.h"}
        self.assertEqual(cmake_headers, {h for h, _ in mk_headers})
        src = _read(os.path.join(NANO, "compat", "src", "imagedecoder.cpp"))
        for fmt in {a[0] for a in calls}:
            self.assertIn("NANOCOMPAT_NO_%s" % fmt, src)


class Features(Base):
    LINKED = {"lib:egl": "egl_headers", "lib:gles": "gles_headers", "lib:libdrm": "drm_headers",
              "lib:freetype": "freetype", "lib:libcurl": "curl", "lib:libpng": "png", "lib:openssl": "crypto",
              "lib:tinyxml2": "tinyxml2", "lib:zlib": "zlib"}
    FEATURE = {"lib:libziparchive": "ARCHIVE", "lib:lzma-sdk": "ARCHIVE", "lib:pdfium": "PDF"}

    def feature_files(self, f: str) -> List[str]:
        return [p for p in cmake_set(self.features_cmds, "NANO_FEATURE_FILES_" + f) if p]

    def test_features_are_options_and_off(self):
        feats = cmake_set(self.features_cmds, "NANO_FEATURES")
        self.assertEqual(sorted(feats), ["AC3", "ARCHIVE", "MEDIA", "PDF"])
        for f in feats:
            self.assertEqual(self.options.get("NANO_WITH_" + f), "OFF", f)
        for name in re.findall(r"`(NANO_WITH_[A-Z0-9]+)=OFF`", design_c2()):
            self.assertIn(name, self.options)

    def test_feature_files_are_core(self):
        for f in cmake_set(self.features_cmds, "NANO_FEATURES"):
            for p in self.feature_files(f):
                self.assertIn(p, self.src["core"], "%s: %s" % (f, p))
        self.assertIn("NanoAc3.cpp", self.feature_files("AC3"))

    def test_every_unpackaged_library_is_behind_a_feature(self):
        census = json.loads(_read(CENSUS))
        core_links = [c for c in self.cmds if c.name == "target_link_libraries" and c.args[:1] == ["nano_core"]][0].args
        for lib, dep in self.LINKED.items():
            self.assertIn("nano::" + dep, core_links, lib)
        for sec in ("core", "drm"):
            for p in self.src[sec]:
                for key in census["sources"].get(p, {}).get("needs", {}):
                    if key in ("stub",):
                        continue
                    self.assertTrue(key.startswith("lib:"), "%s reaches %s" % (p, key))
                    if key in self.FEATURE:
                        self.assertIn(p, self.feature_files(self.FEATURE[key]),
                                      "%s needs %s: list it under NANO_FEATURE_FILES_%s" % (p, key, self.FEATURE[key]))
                    else:
                        self.assertIn(key, self.LINKED, "%s needs %s, which nano_core does not link" % (p, key))

    def test_liba52_users_are_behind_ac3(self):
        files = json.loads(_read(CENSUS))["files"]

        def reaches_a52(path, seen):
            for inc in files.get(path, {}).get("includes", []):
                if not inc.startswith("nano:"):
                    continue
                q = inc[len("nano:"):]
                if q.startswith("liba52/"):
                    return True
                if q not in seen:
                    seen.add(q)
                    if reaches_a52(q, seen):
                        return True
            return False
        users = [p for p in self.src["core"] + self.src["drm"] if reaches_a52(p, set())]
        self.assertEqual(sorted(users), sorted(self.feature_files("AC3")))


class Options(Base):
    def test_platform_values(self):
        self.assertEqual(cmake_set(self.cmds, "NANO_PLATFORMS"), ["linux", "host", "all", "none"])
        self.assertEqual(self.options["NANO_PLATFORM"], "linux")

    def test_recipe_options_exist(self):
        for pkg, cmakelists in CMAKE_PACKAGES.items():
            opts = cmake_options(cmake_commands(_read(cmakelists)))
            passed = recipe_conf_opts(pkg)
            self.assertTrue(passed, pkg)
            for name, value in passed.items():
                self.assertIn(name, opts, "%s's recipe passes -D%s, which %s does not declare"
                              % (pkg, name, os.path.relpath(cmakelists, ROOT)))
                if opts[name] in ("ON", "OFF"):
                    self.assertIn(value, ("ON", "OFF"), "%s -D%s" % (pkg, name))
        nano = recipe_conf_opts("thimsy-nano")
        self.assertEqual(nano["NANO_PLATFORM"], "linux")
        self.assertEqual(nano["NANO_WITH_AC3"], "OFF")          # no GPL liba52 in the image
        self.assertEqual(nano["NANO_BUILD_TESTS"], "OFF")
        self.assertIn("NANO_BUILD_SCRAPE", nano)
        self.assertIn(nano["NANO_PLATFORM"], cmake_set(self.cmds, "NANO_PLATFORMS"))

    def test_local_site_packages_have_their_licence(self):
        for pkg in ("thimsy-session", "thimsy-padd"):
            d = os.path.dirname(CMAKE_PACKAGES[pkg])
            mk = _read(os.path.join(PACKAGES, pkg, pkg + ".mk"))
            files = re.search(r"^%s_LICENSE_FILES\s*=\s*(.*)$" % pkg.upper().replace("-", "_"), mk, re.M).group(1)
            for f in files.split():
                self.assertTrue(os.path.isfile(os.path.join(d, f)), "%s: %s" % (pkg, f))
            self.assertIn("MIT License", _read(os.path.join(d, "LICENSE")))
            self.assertEqual(_read(os.path.join(d, "LICENSE")), _read(os.path.join(LIBTHIMSY, "LICENSE")))

    def test_session_and_padd(self):
        for pkg in ("thimsy-session", "thimsy-padd"):
            text = _read(CMAKE_PACKAGES[pkg])
            cmds = cmake_commands(text)
            self.assertIn("THIMSY_BUILD_TESTS", cmake_options(cmds), pkg)
            self.assertNotRegex(text, r"THIMSY_(SESSION|PADD)_BUILD_TESTS", pkg)
            self.assertIn("find_package(thimsy CONFIG QUIET)", text, pkg)
            self.assertIn("thimsy::thimsy", text, pkg)
            self.assertIn("thimsy::gtest_standin", text, pkg)
        session = cmake_commands(_read(CMAKE_PACKAGES["thimsy-session"]))
        inst = [c.args for c in session if c.name == "install"]
        self.assertIn(["PROGRAMS", "$<TARGET_FILE:thimsy-session>", "DESTINATION", "${CMAKE_INSTALL_LIBEXECDIR}/thimsy",
                       "RENAME", "session"], inst)
        inittab = _read(os.path.join(ROOT, "linux", "board", "common", "rootfs-overlay", "etc", "inittab"))
        self.assertIn("/usr/libexec/thimsy/session", inittab)


class CMakeVersions(Base):
    def test_minimum_and_no_newer_commands(self):
        for pkg, path in CMAKE_PACKAGES.items():
            text = _read(path)
            cmds = cmake_commands(text)
            mins = [c.args for c in cmds if c.name == "cmake_minimum_required"]
            self.assertEqual(mins, [["VERSION", "3.16"]], pkg)
        for path in list(CMAKE_PACKAGES.values()) + [os.path.join(CMAKE_DIR, f) for f in os.listdir(CMAKE_DIR)]:
            text = _read(path)
            for c in cmake_commands(text):
                # find_*(... REQUIRED) is CMake 3.18; LINK_GROUP 3.24; PROJECT_IS_TOP_LEVEL 3.21.
                if c.name in ("find_library", "find_path", "find_program", "find_file"):
                    self.assertNotIn("REQUIRED", c.args, "%s: %r" % (path, c))
            self.assertNotIn("LINK_GROUP", text, path)
            self.assertNotIn("PROJECT_IS_TOP_LEVEL", text, path)
            self.assertNotIn("\r", text, path)

    def test_no_identity_escapes_in_quoted_arguments(self):
        # In a CMake quoted argument "\[" is just "[" (an identity escape): a regex written "\[" loses
        # its backslash. Only \\ \" \t \n \r \; \$ \@ are allowed in these files.
        quoted = re.compile(r'"((?:[^"\\]|\\.)*)"', re.S)
        for path in list(CMAKE_PACKAGES.values()) + [os.path.join(CMAKE_DIR, f) for f in os.listdir(CMAKE_DIR)]:
            code = "\n".join(ln for ln in _read(path).split("\n") if not ln.lstrip().startswith("#"))
            for m in quoted.finditer(code):
                for e in re.findall(r"\\(.)", m.group(1), re.S):
                    self.assertIn(e, '\\"tnr;$@', "%s: %r" % (os.path.relpath(path, ROOT), m.group(0)))

    def test_balanced_blocks(self):
        pairs = {"if": "endif", "foreach": "endforeach", "function": "endfunction", "macro": "endmacro",
                 "while": "endwhile"}
        for path in list(CMAKE_PACKAGES.values()) + [os.path.join(CMAKE_DIR, f) for f in os.listdir(CMAKE_DIR)]:
            stack: List[Command] = []
            for c in cmake_commands(_read(path)):
                if c.name in pairs:
                    stack.append(c)
                elif c.name in pairs.values():
                    self.assertTrue(stack, "%s: %r without an opener" % (path, c))
                    self.assertEqual(pairs[stack.pop().name], c.name, "%s: %r" % (path, c))
            self.assertEqual(stack, [], path)

    def test_variables_used_in_functions_are_defined(self):
        # Every ${NANO_...} the CMake files read is set somewhere in them (or is a function's prefix).
        texts = {p: _read(p) for p in [CMAKELISTS] + [os.path.join(CMAKE_DIR, f) for f in os.listdir(CMAKE_DIR)]}
        defined = set()
        for t in texts.values():
            for c in cmake_commands(t):
                if c.name in ("set", "option", "list") and c.args:
                    defined.add(c.args[1] if c.name == "list" and len(c.args) > 1 else c.args[0])
                if c.name == "file" and len(c.args) > 1 and c.args[0] in ("GLOB", "GLOB_RECURSE", "READ"):
                    defined.add(c.args[1])
                if c.name == "file" and len(c.args) > 2 and c.args[0] == "STRINGS":
                    defined.add(c.args[2])
                if c.name in ("nano_read_sources",):
                    defined.update("NANO_SRC_" + s.replace("-", "_") for s in self.mirror.sections)
                if c.name == "nano_read_unit_closure":
                    defined.add(c.args[1] + "_PROGRAMS")
                if c.name in ("nano_bp_list",):
                    defined.add(c.args[3])
                if c.name in ("function", "macro"):
                    defined.update(c.args[1:])
        for p, t in texts.items():
            for v in re.findall(r"\$\{(NANO_[A-Z0-9_]+)\}", t):
                self.assertIn(v, defined, "%s reads ${%s}" % (os.path.relpath(p, ROOT), v))


class Libthimsy(unittest.TestCase):
    def test_package_export(self):
        text = _read(os.path.join(LIBTHIMSY, "CMakeLists.txt"))
        cmds = cmake_commands(text)
        exports = [c.args for c in cmds if c.name == "install" and c.args[:1] == ["EXPORT"]]
        self.assertEqual(len(exports), 1)
        self.assertIn("NAMESPACE", exports[0])
        self.assertEqual(exports[0][exports[0].index("NAMESPACE") + 1], "thimsy::")
        targets_file = exports[0][1]
        tmpl = os.path.join(LIBTHIMSY, "cmake", "thimsy-config.cmake.in")
        self.assertTrue(os.path.isfile(tmpl))
        t = _read(tmpl)
        self.assertIn("@PACKAGE_INIT@", t)
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/%s.cmake")' % targets_file, t)
        self.assertIn("find_dependency(Threads)", t)
        conf = [c.args for c in cmds if c.name == "configure_package_config_file"][0]
        self.assertEqual(os.path.basename(conf[1]), "thimsy-config.cmake")    # find_package(thimsy CONFIG)
        # The names consumers use exist as aliases in-tree and as exported names when installed.
        aliases = {c.args[0] for c in cmds if c.name in ("add_library", "add_executable") and "ALIAS" in c.args}
        for name in ("thimsy::thimsy", "thimsy::gtest_standin"):
            self.assertIn(name, aliases)
        export_names = {}
        for c in cmds:
            if c.name == "set_target_properties" and "EXPORT_NAME" in c.args:
                export_names[c.args[0]] = c.args[c.args.index("EXPORT_NAME") + 1]
        self.assertEqual(export_names, {"thimsy": "thimsy", "thimsy_gtest_standin": "gtest_standin"})
        installed = [a for c in cmds if c.name == "install" and c.args[:1] == ["TARGETS"] for a in c.args]
        for t in ("thimsy", "thimsy_gtest_standin", "thimsy-prop"):
            self.assertIn(t, installed)
        self.assertIn("THIMSY_BUILD_TESTS", cmake_options(cmds))
        self.assertIn("THIMSY_INSTALL", cmake_options(cmds))

    def test_sources_match_the_makefile(self):
        cmds = cmake_commands(_read(os.path.join(LIBTHIMSY, "CMakeLists.txt")))
        srcs = sorted(os.path.basename(s)[:-4] for s in cmake_set(cmds, "THIMSY_SOURCES"))
        self.assertEqual(srcs, sorted(make_var(os.path.join(LIBTHIMSY, "Makefile"), "LIB_NAMES")))


@unittest.skipUnless(HAVE, "nano/CMakeLists.txt missing")
class UnitTests(Base):
    def test_every_closure_header_has_a_rule(self):
        body = self.text[self.text.index("function(nano_unit_header_dep"):]
        body = body[:body.index("endfunction()")]
        exact = re.findall(r'header STREQUAL "([^"]+)"', body)
        prefixes = re.findall(r'header MATCHES "\^([^"]+)"', body)
        for prog, (_s, headers, _a) in self.mirror.read_closure(_read(CLOSURE)).items():
            for h in headers:
                self.assertTrue(h in exact or any(h.startswith(p) for p in prefixes),
                                "%s needs <%s>, which nano_unit_header_dep has no rule for" % (prog, h))
        # The same libraries as the host Makefile's unit_ldlibs.
        mk = _read(HOST_MAKEFILE)
        for lib in ("-lz", "-lcrypto", "-ltinyxml2", "-lziparchive"):
            self.assertIn(lib, mk)

    def test_asset_dirs_match_the_makefile(self):
        dirs = cmake_set(self.cmds, "NANO_UNIT_ASSET_DIRS")
        self.assertEqual(dirs, make_var(HOST_MAKEFILE, "ASSET_DIRS"))
        for prog, (_s, _h, assets) in self.mirror.read_closure(_read(CLOSURE)).items():
            self.assertLessEqual(set(assets), set(dirs), prog)

    def test_unit_test_environment_matches_the_makefile(self):
        mk = _read(HOST_MAKEFILE)
        for var in ("NANO_TEST_FIXTURES", "NANO_DAIJISHOU_DIR", "TMPDIR"):
            self.assertIn(var + "=", mk)
            self.assertIn(var + "=", self.text)
        self.assertTrue(os.path.isfile(os.path.join(CMAKE_DIR, "unit_data.cmake")))


if __name__ == "__main__":
    unittest.main()
