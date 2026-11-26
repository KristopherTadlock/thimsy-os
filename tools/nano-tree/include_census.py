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
  include_census.py split                 the [core]/[drm]/[firmware]/[dropped-linux] split, one
                                          line per file with its reason
  include_census.py write                 writes nano/build/unit-closure.txt,
                                          tools/nano-tree/include-census.json and the four split
                                          sections of nano/build/sources.txt (G4)
  include_census.py check                 exit 1 when any of the three is stale

Each include is put in one class:

  nano       a file in nano/ (outside compat/include and compat/stubs)
  shim       a header of nano/compat/include (layer 1)
  stub       a header of nano/compat/stubs/include (layer 2: nano_core's include path only)
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

A file's needs are its lib, android, stub, sibling, missing and unknown includes, and those of every
nano/ file it reaches, counting only includes outside any #if (a header's include guard does not
count). A stub is a need for a unit test program (layer 2 is not on its include path) and is
satisfied for [core] and [drm].

The split (IMPLEMENTATION.md C.1, B.4), over nano/Android.bp's gammaos-nano srcs, in its order:
  - SPLIT_DECISIONS places a file by hand, with the reason (the KMS file is [drm]; features v1 leaves
    out on Linux are [firmware]);
  - any other file is [firmware] when it reaches an Android-only header with neither shim nor stub
    (each such header needs a NOT_STUBBED reason, or the census fails), a sibling's header, or a
    header that resolves nowhere; else [core];
  - [dropped-linux] is every [firmware] file that defines something a [core] or [drm] file (or a
    header they reach) names: a NanoMenu:: method, a free function or a variable. The Linux and host
    links get stubs generated for exactly those (nano/test/host/stubgen/gen.py).
`write` rewrites only the path lines right below those four section headers (with a comment line
per [firmware] and [dropped-linux] file giving its reason); the rest of sources.txt is kept.

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
STUBS_INCLUDE = "compat/stubs/include"
STUBGEN = "nano/test/host/stubgen/gen.py"
# Folders whose headers the host build puts on the include path after compat/include and nano/
# itself (with Android.bp's local_include_dirs): the tests' helpers.
EXTRA_INCLUDE_DIRS = ("test/unit",)
# Left out of the census: the shims and the stubs themselves (layers 1 and 2) and their own tests,
# which build with compat.mk and stubs.mk.
SKIP_DIRS = (COMPAT_INCLUDE + "/", "compat/stubs/", "test/host/compat/")
NEED_CLASSES = ("lib", "android", "stub", "sibling", "missing", "unknown")

# The split's hand decisions (G4): {gammaos-nano src: (section, reason)}. Everything else follows the
# census (module docstring).
SPLIT_DECISIONS: "OrderedDict[str, Tuple[str, str]]" = OrderedDict([
    ("NanoMenuDrm.cpp", ("drm", "the KMS display code, firmware and Linux each with its own buffer back end; "
                                "the host replaces it (platform/host/DrmHost.cpp)")),
    ("NanoMenuDrmBuffers.cpp", ("firmware", "the AHardwareBuffer render-target back end (G9a); Linux links "
                                            "platform/linux/display/DrmBuffersGbm.cpp instead")),
    ("NanoBootChime.cpp", ("firmware", "the boot chime plays through tinyalsa on the Android audio HAL's "
                                       "card; Linux boot sound is not v1's")),
    ("NanoBtStable.cpp", ("firmware", "a workaround for Android's Bluetooth stack (its services and settings)")),
    ("NanoMenuDrasticPark.cpp", ("firmware", "parks drastic-nano, the DS player (not on Linux v1)")),
    ("LibretroRunner.cpp", ("firmware", "the in-process libretro runner, until plan Phase 5")),
    ("DrasticGpu3d.cpp", ("firmware", "drastic-nano's GPU renderer (the DS player, not on Linux v1)")),
    ("DrasticSettings.cpp", ("firmware", "drastic-nano's settings (the DS player, not on Linux v1)")),
])
# Android-only headers G4 decided not to stub: {header: reason}. A [core] or [drm] file may not reach
# one (the census then puts it in [firmware]); a header that is in neither table nor stubbed fails.
NOT_STUBBED: Dict[str, str] = {
    "jni.h": "the JNI shim of drastic-nano's runner (the DS player, not on Linux v1) and main.cpp's",
    "binder/ProcessState.h": "Android's entry point (main.cpp) starts the binder thread pool; Linux has "
                             "its own main",
}
SIBLING_REASON = "a header outside Nano's folder (upstream.json siblings): v1 has neither Shares nor RGB follow on Linux"

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
        if self.cls in ("nano", "shim", "stub"):
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
        dirs = [COMPAT_INCLUDE, STUBS_INCLUDE]
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
                if p.startswith(COMPAT_INCLUDE + "/"):
                    inc.cls = "shim"
                elif p.startswith(STUBS_INCLUDE + "/"):
                    inc.cls = "stub"
                else:
                    inc.cls = "nano"
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


def census_json(c: Census, split: "Optional[Split]" = None) -> str:
    split = split if split is not None else Split(c)
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
    split_doc = OrderedDict()
    for sec, paths in split.sections.items():
        split_doc[sec] = OrderedDict((p, split.reasons.get("%s:%s" % (sec, p), "")) for p in paths)
    doc = OrderedDict([
        ("comment", "Generated by tools/nano-tree/include_census.py (IMPLEMENTATION.md B.4, C.1); do not "
                    "edit; G4 owns it. 'files': every C-family file's includes that are not the system's, "
                    "unconditional and (under 'conditional') inside an #if; 'sources': every file a module "
                    "of nano/Android.bp compiles, with the headers outside nano/ and the shims that it "
                    "reaches ('stub': layer 2's); 'split': nano/build/sources.txt's four sections over "
                    "gammaos-nano's srcs, with each placement's reason; 'not_stubbed': the Android-only "
                    "headers left without a stub, and why."),
        ("include_dirs", c.include_dirs),
        ("files", files),
        ("sources", sources),
        ("split", split_doc),
        ("not_stubbed", OrderedDict(sorted(NOT_STUBBED.items()))),
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


def _stubgen(root: str):
    """nano/test/host/stubgen/gen.py as a module (its dropped_definitions and blank)."""
    import importlib.util
    path = os.path.join(root, *STUBGEN.split("/"))
    spec = importlib.util.spec_from_file_location("nano_stubgen", path)
    if spec is None or spec.loader is None:
        raise NanoTreeError("can't load %s" % STUBGEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


class Split:
    """The [core]/[drm]/[firmware]/[dropped-linux] split of gammaos-nano's srcs (module docstring).
    .sections maps each of the four sections to its files in Android.bp order; .reasons maps a
    [drm], [firmware] or [dropped-linux] file (keyed "<section>:<path>") to its reason."""

    SECTIONS = ("core", "drm", "firmware", "dropped-linux")

    def __init__(self, c: Census):
        m = c.bp.module("gammaos-nano")
        if m is None:
            raise NanoTreeError("nano/Android.bp has no gammaos-nano module")
        srcs = [s for s in c.bp.srcs(m) if not s.startswith(":") and s.endswith(C_FAMILY)]
        unknown = sorted(set(SPLIT_DECISIONS) - set(srcs))
        if unknown:
            raise NanoTreeError("SPLIT_DECISIONS names files gammaos-nano does not compile: %s" % ", ".join(unknown))
        self.sections: "OrderedDict[str, List[str]]" = OrderedDict((s, []) for s in self.SECTIONS)
        self.reasons: Dict[str, str] = {}
        undecided: List[str] = []
        for path in srcs:
            if path in SPLIT_DECISIONS:
                sec, why = SPLIT_DECISIONS[path]
                self.sections[sec].append(path)
                if sec != "core":
                    self.reasons["%s:%s" % (sec, path)] = why
                continue
            needs = c.needs(path)
            why = []
            for h in sorted(needs.get("android", ())):
                if h in NOT_STUBBED:
                    why.append("%s (not stubbed: %s)" % (h, NOT_STUBBED[h]))
                else:
                    undecided.append("%s reaches %s" % (path, h))
            for h in sorted(needs.get("sibling", ())):
                why.append("%s (%s)" % (h, SIBLING_REASON))
            for cls in ("missing", "unknown"):
                for h in sorted(needs.get(cls, ())):
                    why.append("%s (%s: resolves nowhere)" % (h, cls))
            if why:
                self.sections["firmware"].append(path)
                self.reasons["firmware:" + path] = "reaches " + "; ".join(why)
            else:
                self.sections["core"].append(path)
        if undecided:
            raise NanoTreeError("Android-only headers with neither shim nor stub: stub them (nano/compat/stubs) or "
                                "add a NOT_STUBBED reason:\n  " + "\n  ".join(undecided))
        self._dropped(c)

    def _dropped(self, c: Census) -> None:
        gen = _stubgen(c.root)
        shared = self.sections["core"] + self.sections["drm"]
        code_parts = []
        for p in c.reach(shared):
            with open(os.path.join(c.nano, *p.split("/")), "rb") as f:
                code_parts.append(gen.blank(f.read().decode("utf-8", "replace")))
        code = "\n".join(code_parts)
        words = set(re.findall(r"[A-Za-z_]\w*", code))
        calls = set(re.findall(r"([A-Za-z_]\w*)\s*\(", code))
        for path in self.sections["firmware"]:
            with open(os.path.join(c.nano, *path.split("/")), "rb") as f:
                defs = gen.dropped_definitions(f.read().decode("utf-8", "replace"))
            used = []
            for q in sorted(defs):
                # android::NanoMenu::nsTick, android::LibretroRunner::init, android::gEarlyDrmFd: each
                # qualifier other than android must be named by the shared code, and so must the name
                # (called, for a function of a class or namespace: "init" alone is everywhere).
                parts = q.split("::")
                name = parts[-1].lstrip("~")
                quals = [p for p in parts[:-1] if p != "android"]
                if name == "main" and not quals:
                    continue
                if not all(p in words for p in quals):
                    continue
                if name in calls or (name in words and (not quals or name == quals[-1])):
                    used.append(q)
            if used:
                self.sections["dropped-linux"].append(path)
                shown = ", ".join(used[:4]) + (" and %d more" % (len(used) - 4) if len(used) > 4 else "")
                self.reasons["dropped-linux:" + path] = "[core]/[drm] name what it defines: " + shown

    def text(self) -> str:
        out = []
        for sec, paths in self.sections.items():
            for p in paths:
                why = self.reasons.get("%s:%s" % (sec, p))
                out.append("%-14s %s%s" % ("[%s]" % sec, p, ("  -- " + why) if why else ""))
        return "\n".join(out) + "\n"

    def section_lines(self, sec: str) -> List[str]:
        lines = []
        for p in self.sections[sec]:
            why = self.reasons.get("%s:%s" % (sec, p))
            if why and sec in ("firmware", "dropped-linux"):
                lines.append("# %s: %s" % (p, why))
            lines.append(p)
        return lines


def rewrite_sources(text: str, split: Split) -> str:
    """TEXT (sources.txt) with the path lines right below the four split section headers replaced:
    the run of non-blank lines after each header is the generated part."""
    lines = text.split("\n")
    out: List[str] = []
    i = 0
    seen = set()
    while i < len(lines):
        line = lines[i]
        out.append(line)
        m = re.match(r"^\[([a-z0-9-]+)\]\s*$", line.strip())
        i += 1
        if m and m.group(1) in Split.SECTIONS:
            seen.add(m.group(1))
            while i < len(lines) and lines[i].strip() and not re.match(r"^\[[a-z0-9-]+\]\s*$", lines[i].strip()):
                i += 1
            out.extend(split.section_lines(m.group(1)))
    missing = [s for s in Split.SECTIONS if s not in seen]
    if missing:
        raise NanoTreeError("sources.txt lacks the section(s) %s" % ", ".join(missing))
    return "\n".join(out)


def _outputs(root: str) -> Dict[str, str]:
    c = Census(root)
    split = Split(c)
    src_path = os.path.join(root, *SOURCES_REL.split("/"))
    with open(src_path, "r", encoding="utf-8", newline="") as f:
        src_text = rewrite_sources(f.read(), split)
    sources = SourceList.parse(src_text, src_path)
    return {SOURCES_REL: src_text, CLOSURE_REL: unit_closure_text(c, sources), CENSUS_REL: census_json(c, split)}


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
    sub.add_parser("split")
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
        if args.cmd == "split":
            sys.stdout.write(Split(Census(root)).text())
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
