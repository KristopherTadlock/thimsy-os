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
# Platform tests in the preprocessor: shared code has none (the seam is cut with nanoplat, shims,
# stubs and link-time substitution, not #ifdefs).
PLATFORM_TOKEN = re.compile(r"\b(__ANDROID__|__linux__|__BIONIC__|__GLIBC__|NANO_PLATFORM_[A-Za-z0-9_]*)\b")
# Each platform's own implementation (OA-4): platform/firmware is Android's, platform/linux and
# platform/host the fork's. None of it is shared code.
PLATFORM_DIR = "platform/"


def shared_sources(nano_dir: str, cfg: Upstream) -> List[str]:
    """Paths (relative to nano/) of the C and C++ files Nano shares between platforms: everything
    with a source extension outside the fork-only globs and outside platform/ (platform/firmware
    is Android's own implementation, compiled only there)."""
    out = []
    for dirpath, dirnames, names in os.walk(nano_dir):
        dirnames.sort()
        for n in sorted(names):
            if not n.endswith(SOURCE_EXT):
                continue
            rel = os.path.relpath(os.path.join(dirpath, n), nano_dir).replace(os.sep, "/")
            if not cfg.is_fork_only(rel) and not rel.startswith(PLATFORM_DIR):
                out.append(rel)
    return out


def feature_files(nano_dir: str) -> List[str]:
    """The files behind Nano's CMake feature switches (nano/cmake/features.cmake). The Linux build
    turns every switch off (linux/package/thimsy-nano/thimsy-nano.mk), so they are not compiled there."""
    path = os.path.join(nano_dir, "cmake", "features.cmake")
    if not os.path.isfile(path):
        return []
    out: List[str] = []
    for m in re.finditer(r"set\(NANO_FEATURE_FILES_\w+\s+([^)]*)\)", _read(path)):
        out.extend(x.strip('"') for x in m.group(1).split() if x.strip('"'))
    return out


def linux_shared_sources(nano_dir: str) -> List[str]:
    """The shared files the Linux build compiles: nano/build/sources.txt's [core], [drm] and [scrape]
    (less the feature-switch files, all off on Linux), then every top-level nano/*.h and *.inc. Never
    platform/** (each platform's own implementation) or compat/ (the shim layer)."""
    from .sources import SourceList
    sl = SourceList.load(os.path.join(nano_dir, "build", "sources.txt"))
    skip = set(feature_files(nano_dir))
    out: List[str] = []
    seen: Set[str] = set()
    for section in ("core", "drm", "scrape"):
        for f in sl[section]:
            if f in skip or f in seen or f.startswith((PLATFORM_DIR, "compat/")):
                continue
            seen.add(f)
            out.append(f)
    for n in sorted(os.listdir(nano_dir)):
        if n.endswith((".h", ".inc")) and os.path.isfile(os.path.join(nano_dir, n)) and n not in seen:
            seen.add(n)
            out.append(n)
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


# --- The Linux ratchets (OA-4 migration step 0; IMPLEMENTATION.md H.4) --------------------------------
# Counts over linux_shared_sources() of what OA-4 says shared code must not hold. Each has a ceiling in
# tools/nano-tree/linux-ratchets.json that may only shrink, towards 0, as the migration moves the code
# behind nanoplat (tools/nano-tree/tests/test_linux_ratchets.py).

# Functions that start a program, called bare, as ::f( or as std::f( (not as a member or as another
# namespace's function).
SPAWNERS = ("system", "popen", "fork", "vfork", "execv", "execvp", "execve", "execl", "execlp", "execle",
            "execvpe", "fexecve", "posix_spawn", "posix_spawnp")
SPAWN_CALL = re.compile(r"(?<![A-Za-z0-9_.>:])(?:(?:std)?::)?(%s)\s*\(" % "|".join(SPAWNERS))

# Android-only programs a string literal may start with (the shell gate refuses all of them on Linux).
ANDROID_TOOLS = frozenset("""
am appops app_process app_process64 bmgr bugreport bugreportz chcon cmd dalvikvm dumpstate dumpsys
gammaos-net getenforce getprop ime locksettings logcat lshal monkey ndc nsenter pm restorecon screencap
setenforce setprop settings svc toolbox toybox uiautomator vdc
""".split())
# Android tools with everyday names: they count only with an argument ("input keyevent", not "input").
ANDROID_TOOLS_WITH_ARG = frozenset(("content", "input", "service", "sm", "wm"))
# Words that run their arguments; skipped to find a literal's first program.
_WRAPPERS = ("exec", "nohup", "busybox", "toybox", "toolbox", "command")

# Android-only headers: the same prefixes as include_census.py's ANDROID_PREFIXES, and bionic's
# property API.
ANDROID_INCLUDE_PREFIXES = (
    "android/", "android-base/", "aaudio/", "aidl/", "binder/", "cutils/", "gui/", "hardware/", "hidl/",
    "log/", "media/", "nativebase/", "nativehelper/", "private/", "system/", "ui/", "utils/", "vndk/",
    "jni.h", "hwbinder/", "input/", "renderengine/", "sync/",
)
ANDROID_INCLUDE_EXTRA = ("sys/system_properties.h",)
# The portability allow-list: Android headers compat/ shims for good (logging, clocks, properties,
# RefBase, Thread, android-base file and strings). Everything else counts.
PORTABLE_INCLUDES = frozenset((
    "log/log.h", "log/log_main.h", "android/log.h", "utils/Log.h", "cutils/log.h",
    "utils/Timers.h", "utils/SystemClock.h",
    "cutils/properties.h", "sys/system_properties.h", "android-base/properties.h",
    "utils/RefBase.h", "utils/StrongPointer.h",
    "utils/Thread.h",
    "android-base/file.h", "android-base/strings.h",
))
_INCLUDE_LINE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\r\n]+)[>"]')

# Android path roots, by family (the first component of an absolute path literal). /sys, /proc and
# /dev are the kernel's; their device-specific nodes are the device-literal ratchet's.
ANDROID_PATH_FAMILIES = ("acct", "apex", "bootstrap-apex", "cache", "config", "data", "data_mirror",
                         "debug_ramdisk", "efs", "linkerconfig", "metadata", "mnt", "odm", "oem", "persist",
                         "postinstall", "product", "sdcard", "storage", "system", "system_ext", "vendor")
_PATH_LITERAL = re.compile(r"/[A-Za-z][A-Za-z0-9_.@%+:-]*(?:/[A-Za-z0-9_.@%+:*{}-]*)*\Z")

# Device and SoC knowledge in string literals (it belongs in each platform's device profile).
DEVICE_LITERALS = (
    ("fde60000", re.compile(r"fde60000")),
    ("sprd_", re.compile(r"sprd_")),
    ("soc-thermal", re.compile(r"soc-thermal")),
    ("gpu-thermal", re.compile(r"gpu-thermal")),
    ("vop@", re.compile(r"vop@")),
    ("route-dsi", re.compile(r"route-dsi")),
    ("baseparameter", re.compile(r"baseparameter")),
    ("rfkill0", re.compile(r"rfkill0")),
    ("panel0-backlight", re.compile(r"panel0-backlight")),
    ("lcd-backlight", re.compile(r"lcd-backlight")),
    ("ums", re.compile(r"(?<![A-Za-z0-9_])ums(?![a-z])")),
    ("sc98", re.compile(r"(?<![A-Za-z0-9_])sc98")),
    ("sc99", re.compile(r"(?<![A-Za-z0-9_])sc99")),
    ("sharkl", re.compile(r"(?<![A-Za-z0-9_])sharkl")),
    ("rk356x", re.compile(r"rk356x")),
)

# Android-named members of the nanoplat interface (R5: interfaces are named for intent).
PLATFORM_HEADER = "NanoPlatform.h"
ANDROID_MEMBER = re.compile(r"\b(\w*ViaHal\w*|mirror\w*|android\s*::\s*LaunchTuple|forceSfPath)\b")

Hit = Tuple[str, int, str]


def _lines(nano_dir: str, rel: str) -> Tuple[str, List[Tuple[int, str]], List[str]]:
    text = _read(os.path.join(nano_dir, *rel.split("/")))
    code, strings = lex(text)
    return code, strings, text.split("\n")


def spawner_calls(nano_dir: str, files: List[str]) -> List[Hit]:
    """(file, line, function) for each call of a SPAWNERS function in code."""
    out = []
    for rel in files:
        code, _s, _t = _lines(nano_dir, rel)
        for n, text in enumerate(code.split("\n"), 1):
            for m in SPAWN_CALL.finditer(text):
                out.append((rel, n, m.group(1)))
    return out


def first_program(literal: str) -> str:
    """The Android tool a command-like LITERAL starts with, or "": past leading assignments and
    wrappers (timeout with its options and duration), an Android path reduced to its last component."""
    w = literal.split()
    i = 0
    while i < len(w):
        x = w[i]
        if re.match(r"[A-Za-z_][A-Za-z0-9_]*=", x) or x in _WRAPPERS:
            i += 1
        elif x == "timeout":
            i += 1
            while i < len(w) and w[i].startswith("-"):
                i += 2 if w[i] in ("-s", "-k", "--signal", "--kill-after") else 1
            i += 1
        else:
            break
    if i >= len(w):
        return ""
    first = w[i]
    if first.startswith(("/system/", "/vendor/", "/product/", "/odm/", "/apex/", "/system_ext/")):
        first = first.rsplit("/", 1)[-1]
    if first in ANDROID_TOOLS:
        return first
    if first in ANDROID_TOOLS_WITH_ARG and i + 1 < len(w):
        return first
    return ""


def android_tool_literals(nano_dir: str, files: List[str]) -> List[Hit]:
    """(file, line, tool) for each string literal whose first program is an Android tool."""
    out = []
    for rel in files:
        _c, strings, _t = _lines(nano_dir, rel)
        for line, s in strings:
            tool = first_program(s)
            if tool:
                out.append((rel, line, tool))
    return out


def is_android_include(name: str) -> bool:
    return name.startswith(ANDROID_INCLUDE_PREFIXES) or name in ANDROID_INCLUDE_EXTRA


def android_includes(nano_dir: str, files: List[str]) -> List[Hit]:
    """(file, line, header) for each #include of an Android-only header outside PORTABLE_INCLUDES
    (in code, not in comments)."""
    out = []
    for rel in files:
        code, _s, text = _lines(nano_dir, rel)
        for n, c in enumerate(code.split("\n"), 1):
            if "#" not in c or "include" not in c or n - 1 >= len(text):
                continue
            m = _INCLUDE_LINE.match(text[n - 1])
            if not m:
                continue
            name = m.group(1).strip()
            if is_android_include(name) and name not in PORTABLE_INCLUDES:
                out.append((rel, n, name))
    return out


def android_paths(nano_dir: str, files: List[str]) -> List[Hit]:
    """(file, line, "/family") for each absolute path literal under an Android root."""
    out = []
    for rel in files:
        _c, strings, _t = _lines(nano_dir, rel)
        for line, s in strings:
            if _PATH_LITERAL.match(s):
                fam = s.split("/")[1]
                if fam in ANDROID_PATH_FAMILIES:
                    out.append((rel, line, "/" + fam))
    return out


def device_literals(nano_dir: str, files: List[str]) -> List[Hit]:
    """(file, line, name) for each device or SoC name found in a string literal."""
    out = []
    for rel in files:
        _c, strings, _t = _lines(nano_dir, rel)
        for line, s in strings:
            for name, rx in DEVICE_LITERALS:
                if rx.search(s):
                    out.append((rel, line, name))
    return out


def android_named_members(nano_dir: str) -> List[Hit]:
    """(file, line, name) for each Android-named identifier in NanoPlatform.h's code."""
    out: List[Hit] = []
    if not os.path.isfile(os.path.join(nano_dir, PLATFORM_HEADER)):
        return out
    code, _s, _t = _lines(nano_dir, PLATFORM_HEADER)
    for n, text in enumerate(code.split("\n"), 1):
        for m in ANDROID_MEMBER.finditer(text):
            out.append((PLATFORM_HEADER, n, re.sub(r"\s+", "", m.group(1))))
    return out


def _by_key(hits: List[Hit], total: bool) -> Dict[str, List[Hit]]:
    out: Dict[str, List[Hit]] = {}
    for h in hits:
        out.setdefault("total" if total else h[2], []).append(h)
    return out


def linux_ratchet_hits(nano_dir: str) -> Dict[str, Dict[str, List[Hit]]]:
    """{ratchet: {key: [hits]}} for every Linux ratchet. Keys are the function, path family or device
    name, or "total" for the ratchets with one ceiling."""
    files = linux_shared_sources(nano_dir)
    return {
        "spawners": _by_key(spawner_calls(nano_dir, files), False),
        "android_tool_literals": _by_key(android_tool_literals(nano_dir, files), True),
        "android_includes": _by_key(android_includes(nano_dir, files), True),
        "android_paths": _by_key(android_paths(nano_dir, files), False),
        "device_literals": _by_key(device_literals(nano_dir, files), False),
        "android_named_members": _by_key(android_named_members(nano_dir), True),
    }
