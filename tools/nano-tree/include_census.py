#!/usr/bin/env python3
"""The include census of nano/ (docs/linux/IMPLEMENTATION.md B.4, C.1, C.3): for every C and C++
file, what it #includes and what each include resolves to on the host and Linux builds; for every
compiled file, the headers outside nano/ that it reaches, transitively. Include resolution is
nanotree/surface.py's, with the compat shims (nano/compat/include) first on the path, as the host
build has them.

Commands (Python 3.8 or later, standard library only):

  include_census.py census [--out FILE]   the census as JSON (stdout, or FILE); G4 commits it as
                                          tools/nano-tree/include-census.json
  include_census.py unit-closure [--out FILE]
                                          nano/build/unit-closure.txt's text (stdout, or FILE)
  include_census.py write                 writes nano/build/unit-closure.txt and
                                          tools/nano-tree/include-census.json
  include_census.py check                 exit 1 when either committed file is stale

Each include is put in one class:

  nano       a file in nano/ (outside compat/include)
  shim       a header of nano/compat/include (layer 1)
  system     the C and C++ standard libraries, POSIX, glibc and the Linux UAPI
  gtest      <gtest/gtest.h>: GoogleTest, or libthimsy/testing's stand-in
  libthimsy  <thimsy/...>: the repository's libthimsy
  optional   a header named in a __has_include() of the same file (built without it)
  lib        an outside library with a Linux package (zlib, OpenSSL, tinyxml2, ...)
  android    an Android-only header with no shim (binder, gui, media, ... ; G4's stubs)
  sibling    a header of the Nano folder's siblings in the AOSP tree (upstream.json "siblings")
  arch       a CPU's intrinsics (arm_neon.h, emmintrin.h): always under a conditional
  foreign    another OS's header (windows.h, ...): vendored code's other ports
  missing    a quoted include that resolves nowhere
  unknown    an angle include none of the tables knows

A file's needs are its lib, android, sibling, missing and unknown includes, and those of every nano/
file it reaches, counting only includes outside any #if (a header's include guard does not count).

A test program also needs an asset folder (upstream.json "assets": ps3xmb, nano_esde_themes,
nano_esde_assets) when a string literal of its test source names one: nano/ holds only the files the
patch changes there, the rest is the base's (nano_tree.py compose). unit-closure.txt writes such a
need as "@<folder>".
"""
from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import sys
from collections import OrderedDict
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.androidbp import Blueprint  # noqa: E402
from nanotree.config import NanoTreeError, Upstream  # noqa: E402
from nanotree.ratchets import lex  # noqa: E402
from nanotree.sources import SourceList  # noqa: E402
from nanotree.surface import C_FAMILY, blueprint_surface, module_sources, resolve_include  # noqa: E402

CENSUS_REL = "tools/nano-tree/include-census.json"
CLOSURE_REL = "nano/build/unit-closure.txt"
SOURCES_REL = "nano/build/sources.txt"
COMPAT_INCLUDE = "compat/include"
# Folders whose headers the host build puts on the include path after compat/include and nano/
# itself (with Android.bp's local_include_dirs): the tests' helpers.
EXTRA_INCLUDE_DIRS = ("test/unit",)
# Left out of the census: the shims themselves (layer 1) and their own tests, which build with
# compat.mk and include compat/src's private headers.
SKIP_DIRS = (COMPAT_INCLUDE + "/", "test/host/compat/")
NEED_CLASSES = ("lib", "android", "sibling", "missing", "unknown")

# Where a header's definitions live when it is not the .cpp of the same name. Each entry was found
# by linking a test program on the host (G1c); only [test-unit] production files are added.
HEADER_SOURCES: Dict[str, Sequence[str]] = {}

# Outside libraries: (prefix or exact name, library). A prefix ends with '/'.
LIB_TABLE: Sequence[Tuple[str, str]] = (
    ("zlib.h", "zlib"), ("zconf.h", "zlib"),
    ("openssl/", "openssl"),
    ("tinyxml2.h", "tinyxml2"),
    ("ft2build.h", "freetype"), ("freetype/", "freetype"),
    ("png.h", "libpng"), ("pngconf.h", "libpng"),
    ("jpeglib.h", "libjpeg"), ("jerror.h", "libjpeg"),
    ("webp/", "libwebp"),
    ("gif_lib.h", "giflib"),
    ("curl/", "libcurl"),
    ("EGL/", "egl"), ("KHR/", "egl"),
    ("GLES/", "gles"), ("GLES2/", "gles"), ("GLES3/", "gles"),
    ("drm.h", "libdrm"), ("drm_fourcc.h", "libdrm"), ("drm_mode.h", "libdrm"), ("xf86drm.h", "libdrm"),
    ("xf86drmMode.h", "libdrm"), ("libdrm/", "libdrm"),
    ("gbm.h", "gbm"),
    ("alsa/", "alsa"),
    ("7z.h", "lzma-sdk"), ("7zCrc.h", "lzma-sdk"), ("7zFile.h", "lzma-sdk"), ("7zTypes.h", "lzma-sdk"),
    ("lzma.h", "xz"),
    ("ziparchive/", "libziparchive"),
    ("fpdfview.h", "pdfium"), ("fpdf_", "pdfium"),
    ("libretro.h", "libretro-api"),
    ("tinyalsa/", "tinyalsa"),
)
# Android-only headers (when compat/include has no shim for them).
ANDROID_PREFIXES: Sequence[str] = (
    "android/", "android-base/", "aaudio/", "aidl/", "binder/", "cutils/", "gui/", "hardware/", "hidl/",
    "log/", "media/", "nativebase/", "nativehelper/", "private/", "system/", "ui/", "utils/", "vndk/",
    "jni.h", "hwbinder/", "input/", "renderengine/", "sync/",
)
ARCH: Set[str] = {"arm_neon.h", "arm_acle.h", "emmintrin.h", "xmmintrin.h", "immintrin.h", "tmmintrin.h",
                  "smmintrin.h", "nmmintrin.h", "cpuid.h", "x86intrin.h"}
FOREIGN: Set[str] = {"windows.h", "wtypes.h", "intrin.h", "ogcsys.h", "AvailabilityMacros.h", "share.h",
                     "io.h", "direct.h", "malloc/malloc.h", "TargetConditionals.h"}
FOREIGN_PREFIXES: Sequence[str] = ("3ds/", "mach/", "mach-o/", "CoreFoundation/", "switch/")
CXX_STD: Set[str] = set("""
algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat chrono cinttypes climits clocale
cmath codecvt complex condition_variable csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib cstring
ctime cuchar cwchar cwctype deque exception execution filesystem forward_list fstream functional future
initializer_list iomanip ios iosfwd iostream istream iterator limits list locale map memory
memory_resource mutex new numeric optional ostream queue random ratio regex scoped_allocator
set shared_mutex span sstream stack stdexcept streambuf string string_view system_error thread tuple type_traits
typeindex typeinfo unordered_map unordered_set utility valarray variant vector
""".split())
C_SYSTEM: Set[str] = set("""
assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h
signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h
string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h
aio.h alloca.h byteswap.h cpio.h dirent.h dlfcn.h elf.h endian.h err.h error.h execinfo.h fcntl.h
features.h fnmatch.h ftw.h getopt.h glob.h grp.h iconv.h ifaddrs.h langinfo.h libgen.h link.h malloc.h
memory.h mntent.h monetary.h mqueue.h netdb.h nl_types.h paths.h poll.h pthread.h pty.h pwd.h regex.h
resolv.h sched.h search.h semaphore.h shadow.h spawn.h strings.h stropts.h syscall.h sysexits.h syslog.h
tar.h termios.h ucontext.h ulimit.h unistd.h utime.h utmp.h utmpx.h values.h wait.h wordexp.h
""".split())
SYSTEM_PREFIXES: Sequence[str] = ("sys/", "linux/", "asm/", "asm-generic/", "bits/", "arpa/", "netinet/",
                                  "net/", "netpacket/", "scsi/", "mtd/", "sound/", "rdma/", "gnu/")
INCLUDE_LINE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\r\n]+)[>"]')
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*([a-z]+)\b[ \t]*(.*)$")
HAS_INCLUDE = re.compile(r'__has_include\s*\(\s*[<"]([^>"]+)[>"]\s*\)')


class Include:
    __slots__ = ("quote", "name", "line", "conditional", "cls", "detail", "path")

    def __init__(self, quote: str, name: str, line: int, conditional: bool):
        self.quote = quote
        self.name = name
        self.line = line
        self.conditional = conditional
        self.cls = ""
        self.detail = ""          # the library name for "lib"
        self.path: Optional[str] = None   # the nano/ path for "nano" and "shim"

    def label(self) -> str:
        if self.cls in ("nano", "shim"):
            return "%s:%s" % (self.cls, self.path)
        if self.cls == "lib":
            return "lib:%s:%s" % (self.detail, self.name)
        return "%s:%s" % (self.cls, self.name)


def scan(text: str) -> Tuple[List[Include], Set[str]]:
    """The #include lines of a C or C++ TEXT, each marked conditional when it sits inside an #if,
    #ifdef or #ifndef other than the file's include guard, and the names its __has_include()s test."""
    lines = text.split("\n")
    directives = []
    for n, line in enumerate(lines, 1):
        m = DIRECTIVE.match(line)
        if m:
            directives.append((n, m.group(1), m.group(2).strip()))
    guard_line = None
    if len(directives) >= 2 and directives[0][1] == "ifndef" and directives[1][1] == "define":
        name = directives[0][2].split()[0] if directives[0][2] else ""
        if name and directives[1][2].split()[:1] == [name]:
            guard_line = directives[0][0]
    stack: List[bool] = []   # True for a conditional that counts
    out: List[Include] = []
    for n, kind, rest in directives:
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(n != guard_line)
        elif kind == "endif":
            if stack:
                stack.pop()
        elif kind == "include":
            m = INCLUDE_LINE.match(lines[n - 1])
            if m:
                out.append(Include(m.group(1), m.group(2).strip(), n, any(stack)))
    return out, set(HAS_INCLUDE.findall(text))


def _prefixed(name: str, table: Iterable[str]) -> bool:
    for p in table:
        if (p.endswith("/") or p.endswith("_")) and name.startswith(p):
            return True
        if name == p:
            return True
    return False


def classify_outside(name: str, quote: str, siblings: Set[str]) -> Tuple[str, str]:
    """The class of an include that resolves to no file in nano/."""
    if posixpath.basename(name) in siblings:
        return "sibling", ""
    if name.startswith("gtest/") or name.startswith("gmock/"):
        return "gtest", ""
    if name.startswith("thimsy/"):
        return "libthimsy", ""
    for p, lib in LIB_TABLE:
        if (p.endswith("/") or p.endswith("_")) and name.startswith(p) or name == p:
            return "lib", lib
    if _prefixed(name, ANDROID_PREFIXES):
        return "android", ""
    if name in ARCH:
        return "arch", ""
    if name in FOREIGN or _prefixed(name, FOREIGN_PREFIXES):
        return "foreign", ""
    if quote == '"':
        return "missing", ""
    if name in CXX_STD or name in C_SYSTEM or _prefixed(name, SYSTEM_PREFIXES):
        return "system", ""
    return "unknown", ""


class Census:
    """nano/'s includes, resolved. FILES maps every C-family path under nano/ (outside the asset
    folders and compat/include) to its includes."""

    def __init__(self, root: str = ROOT):
        self.root = os.path.abspath(root)
        self.nano = os.path.join(self.root, "nano")
        self.cfg = Upstream.load(self.root)
        self.all_files = self._walk()
        self.fileset = set(self.all_files)
        with open(os.path.join(self.nano, "Android.bp"), "r", encoding="utf-8") as f:
            self.bp = Blueprint(f.read())
        _named, bp_dirs = blueprint_surface(self.bp, self.all_files, self.cfg.prefix)
        dirs = [COMPAT_INCLUDE]
        for d in list(bp_dirs) + list(EXTRA_INCLUDE_DIRS):
            if d not in dirs:
                dirs.append(d)
        self.include_dirs = dirs
        self.siblings = {posixpath.basename(s) for s in self.cfg.siblings if s.endswith(C_FAMILY)}
        self.files: Dict[str, List[Include]] = {}
        for path in self.all_files:
            if path.startswith(SKIP_DIRS) or not path.endswith(C_FAMILY):
                continue
            self.files[path] = self._resolve(path)
        self._needs: Dict[str, Dict[str, Set[str]]] = {}

    def _walk(self) -> List[str]:
        out = []
        for dirpath, dirnames, names in os.walk(self.nano):
            dirnames.sort()
            rel_dir = os.path.relpath(dirpath, self.nano).replace(os.sep, "/")
            rel_dir = "" if rel_dir == "." else rel_dir
            for n in sorted(names):
                rel = posixpath.join(rel_dir, n) if rel_dir else n
                if self.cfg.is_asset(rel):
                    continue
                out.append(rel)
        return out

    def _resolve(self, path: str) -> List[Include]:
        with open(os.path.join(self.nano, *path.split("/")), "rb") as f:
            text = f.read().decode("utf-8", "replace")
        incs, optional = scan(text)
        for inc in incs:
            p = resolve_include(path, inc.quote, inc.name, self.include_dirs, self.fileset)
            if p is not None:
                inc.cls = "shim" if p.startswith(COMPAT_INCLUDE + "/") else "nano"
                inc.path = p
            elif inc.name in optional:
                inc.cls = "optional"
            else:
                inc.cls, inc.detail = classify_outside(inc.name, inc.quote, self.siblings)
        return incs

    def reach(self, start: Iterable[str]) -> List[str]:
        """The nano/ files START reaches through unconditional includes (START included)."""
        seen: List[str] = []
        todo = list(start)
        known: Set[str] = set()
        while todo:
            p = todo.pop(0)
            if p in known:
                continue
            known.add(p)
            seen.append(p)
            for inc in self.files.get(p, []):
                if inc.cls == "nano" and not inc.conditional and inc.path not in known:
                    todo.append(inc.path)
        return seen

    def needs(self, path: str) -> Dict[str, Set[str]]:
        """{class or "lib:<name>": headers} that PATH reaches outside nano/ and the shims."""
        if path in self._needs:
            return self._needs[path]
        out: Dict[str, Set[str]] = {}
        for p in self.reach([path]):
            for inc in self.files.get(p, []):
                if inc.cls in NEED_CLASSES and not inc.conditional:
                    key = "lib:" + inc.detail if inc.cls == "lib" else inc.cls
                    out.setdefault(key, set()).add(inc.name)
        self._needs[path] = out
        return out

    def compiled(self) -> Dict[str, List[str]]:
        """{compiled nano/ file: the Android.bp modules that compile it}."""
        out: Dict[str, List[str]] = {}
        for name, srcs in sorted(module_sources(self.bp, self.all_files).items()):
            for s in sorted(srcs):
                if s in self.fileset and s.endswith(C_FAMILY):
                    out.setdefault(s, []).append(name)
        return out


def census_json(c: Census) -> str:
    files = OrderedDict()
    for path in sorted(c.files):
        incs = c.files[path]
        entry = OrderedDict()
        direct = sorted({i.label() for i in incs if i.cls != "system" and not i.conditional})
        cond = sorted({i.label() for i in incs if i.cls != "system" and i.conditional})
        if direct:
            entry["includes"] = direct
        if cond:
            entry["conditional"] = cond
        if entry:
            files[path] = entry
    sources = OrderedDict()
    for path, modules in sorted(c.compiled().items()):
        needs = c.needs(path)
        sources[path] = OrderedDict([("modules", modules),
                                     ("needs", OrderedDict((k, sorted(v)) for k, v in sorted(needs.items())))])
    doc = OrderedDict([
        ("comment", "Generated by tools/nano-tree/include_census.py (IMPLEMENTATION.md B.4, C.1); do not "
                    "edit. First version by G1c; G4 refines and owns it. 'files': every C-family file's "
                    "includes that are not the system's, unconditional and (under 'conditional') inside "
                    "an #if; 'sources': every file a module of nano/Android.bp compiles, with the "
                    "headers outside nano/ and the shims that it reaches."),
        ("include_dirs", c.include_dirs),
        ("files", files),
        ("sources", sources),
    ])
    return json.dumps(doc, indent=1) + "\n"


def asset_needs(c: Census, test: str) -> List[str]:
    """The asset folders a test source names in a string literal, as "@<folder>"."""
    folders = [a.split("/")[0] for a in c.cfg.assets]
    with open(os.path.join(c.nano, *test.split("/")), "r", encoding="utf-8", errors="replace") as f:
        _code, strings = lex(f.read())
    out = set()
    for _line, s in strings:
        for d in folders:
            if re.search(r"(^|/)%s(/|$)" % re.escape(d), s):
                out.add("@" + d)
    return sorted(out)


def unit_closure(c: Census, sources: SourceList) -> List[Tuple[str, List[str], List[str]]]:
    """(program, production files, outside headers needed) per [test-unit] test, in file order. The
    closure: every [test-unit] production .cpp whose header (same name, or HEADER_SOURCES) the
    program or a file already in its closure reaches."""
    production = sources.test_production()
    prod = set(production)
    rows = []
    for test in sources.test_programs():
        closure: List[str] = []
        todo = [test]
        done: Set[str] = set()
        while todo:
            f = todo.pop(0)
            if f in done:
                continue
            done.add(f)
            for p in c.reach([f]):
                cands = []
                if p.endswith((".h", ".hpp")):
                    cands.append(posixpath.splitext(p)[0] + ".cpp")
                cands.extend(HEADER_SOURCES.get(p, ()))
                for cpp in cands:
                    if cpp in prod and cpp not in closure and cpp != test:
                        closure.append(cpp)
                        todo.append(cpp)
        closure.sort(key=production.index)
        needs: Set[str] = set(asset_needs(c, test))
        for f in [test] + closure:
            for _k, v in c.needs(f).items():
                needs.update(v)
        prog = posixpath.splitext(posixpath.basename(test))[0]
        rows.append((prog, closure, sorted(needs)))
    return rows


def unit_closure_text(c: Census, sources: SourceList) -> str:
    out = [
        "# Generated by tools/nano-tree/include_census.py from nano/build/sources.txt's [test-unit]; do not edit.",
        "# Regenerate: py -3.12 tools/nano-tree/include_census.py write   (test_nano_sources.py checks it.)",
        "# One line per test program (docs/linux/IMPLEMENTATION.md C.3), three tab-separated fields:",
        "#   the program (test/unit/<program>.cpp)",
        "#   the [test-unit] production files it links, space-separated, or -",
        "#   the headers outside nano/, the shims and the system that it reaches, and @<folder> for an asset",
        "#   folder its test names (the base's files: NANO_DATA), space-separated, or -",
        "# check-unit-0 builds the programs that need no header; check-unit probes the others' headers.",
    ]
    for prog, closure, needs in unit_closure(c, sources):
        out.append("%s\t%s\t%s" % (prog, " ".join(closure) or "-", " ".join(needs) or "-"))
    return "\n".join(out) + "\n"


def _outputs(root: str) -> Dict[str, str]:
    c = Census(root)
    sources = SourceList.load(os.path.join(root, *SOURCES_REL.split("/")))
    return {CLOSURE_REL: unit_closure_text(c, sources), CENSUS_REL: census_json(c)}


def _read(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8", newline="") as f:
            return f.read()
    except OSError:
        return None


def _write(path: str, text: str) -> None:
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT, help="the repository (default: this tool's)")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("census", "unit-closure"):
        p = sub.add_parser(name)
        p.add_argument("--out")
    sub.add_parser("write")
    sub.add_parser("check")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        if args.cmd in ("census", "unit-closure"):
            c = Census(root)
            if args.cmd == "census":
                text = census_json(c)
            else:
                text = unit_closure_text(c, SourceList.load(os.path.join(root, *SOURCES_REL.split("/"))))
            if args.out:
                _write(args.out, text)
            else:
                sys.stdout.write(text)
            return 0
        if args.cmd == "write":
            for rel, text in _outputs(root).items():
                _write(os.path.join(root, *rel.split("/")), text)
                print("wrote %s" % rel)
            return 0
        if args.cmd == "check":
            stale = [rel for rel, text in _outputs(root).items()
                     if _read(os.path.join(root, *rel.split("/"))) != text]
            for rel in stale:
                print("stale: %s (run: include_census.py write)" % rel, file=sys.stderr)
            return 1 if stale else 0
        ap.print_help()
        return 2
    except NanoTreeError as e:
        print("include_census: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
