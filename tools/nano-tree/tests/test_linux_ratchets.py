"""The Linux ratchets (OA-4 migration step 0; docs/linux/ARCHITECTURE-AUDIT.md, IMPLEMENTATION.md H.4).

OA-4: shared Nano code holds the model, views and controllers and reaches the platform only through
nanoplat. These ceilings count what it still holds of each platform's job in the files the Linux build
compiles (nanotree.ratchets.linux_shared_sources: never platform/**): program starts, Android tool
commands, Android-only headers, Android path roots, device and SoC names, and Android-named nanoplat
members. A ceiling may only shrink, towards 0. Fork-only (it lives outside tests/). Run with

  py -3.12 -B -m unittest discover -s tools/nano-tree/tests -p test_linux_ratchets.py

After moving code behind the platform, lower the ceilings with

  py -3.12 tools/nano-tree/tests/test_linux_ratchets.py --lower

which rewrites tools/nano-tree/linux-ratchets.json with the lower counts and never raises a ceiling.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from typing import Dict, List, Tuple

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree import ratchets  # noqa: E402
from nanotree.config import Upstream  # noqa: E402
from nanotree.ratchets import (first_program, linux_ratchet_hits, linux_shared_sources,  # noqa: E402
                               platform_conditionals, shared_sources)

NANO = os.path.join(ROOT, "nano")
BASELINE = os.path.join(TOOL_DIR, "linux-ratchets.json")
LOWER = "py -3.12 tools/nano-tree/tests/test_linux_ratchets.py --lower"
RATCHETS = ("spawners", "android_tool_literals", "android_includes", "android_paths", "device_literals",
            "android_named_members")
WHAT = {
    "spawners": "program starts (system, popen, fork, exec*, posix_spawn)",
    "android_tool_literals": "string literals that start with an Android tool (am, cmd, settings, ...)",
    "android_includes": "Android-only #includes beyond the portability allow-list",
    "android_paths": "Android path literals (by root)",
    "device_literals": "device and SoC names in string literals",
    "android_named_members": "Android-named members of NanoPlatform.h (ViaHal, mirror*, android::LaunchTuple, "
                             "forceSfPath)",
}


def load_baseline(path: str = BASELINE) -> Dict[str, Dict[str, int]]:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return {k: v for k, v in data.items() if not k.startswith("_")}


def compare(counts: Dict[str, Dict[str, int]], ceilings: Dict[str, Dict[str, int]]
            ) -> Tuple[List[str], List[str]]:
    """(over, under): the keys whose count is above their ceiling (a missing ceiling is 0), and the
    keys whose count fell below it."""
    over, under = [], []
    for r in RATCHETS:
        have = counts.get(r, {})
        cap = ceilings.get(r, {})
        for k in sorted(set(have) | set(cap)):
            n, c = have.get(k, 0), cap.get(k, 0)
            if n > c:
                over.append("%s[%s]: %d, ceiling %d" % (r, k, n, c))
            elif n < c:
                under.append("%s[%s]: %d, ceiling %d" % (r, k, n, c))
    return over, under


def lowered(counts: Dict[str, Dict[str, int]], ceilings: Dict[str, Dict[str, int]]) -> Dict[str, Dict[str, int]]:
    """The ceilings with every count below its ceiling taken as the new ceiling; 0s dropped."""
    out: Dict[str, Dict[str, int]] = {}
    for r in RATCHETS:
        cap = ceilings.get(r, {})
        have = counts.get(r, {})
        out[r] = {k: min(cap[k], have.get(k, 0)) for k in sorted(cap) if min(cap[k], have.get(k, 0)) > 0}
    return out


def current_counts(nano_dir: str = NANO) -> Tuple[Dict[str, Dict[str, int]], dict]:
    hits = linux_ratchet_hits(nano_dir)
    return {r: {k: len(v) for k, v in hits[r].items()} for r in RATCHETS}, hits


def lower_baseline() -> int:
    counts, _hits = current_counts()
    ceilings = load_baseline()
    over, _under = compare(counts, ceilings)
    if over:
        print("not lowering: these counts are over their ceilings (a ceiling is never raised here):\n  " +
              "\n  ".join(over))
        return 1
    with open(BASELINE, "r", encoding="utf-8") as f:
        data = json.load(f)
    new = lowered(counts, ceilings)
    for r in RATCHETS:
        data[r] = new[r]
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    print("linux-ratchets.json: " + ", ".join("%s %d" % (r, sum(new[r].values())) for r in RATCHETS))
    return 0


@unittest.skipUnless(os.path.isfile(os.path.join(NANO, "build", "sources.txt")), "no nano/ in this checkout")
class LinuxRatchets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = linux_shared_sources(NANO)
        cls.counts, cls.hits = current_counts()
        cls.ceilings = load_baseline()

    def test_scope_is_linux_compiled_shared_code(self):
        files = set(self.files)
        for f in ("NanoMenu.cpp", "NanoMenuDrm.cpp", "NanoScraper.cpp", "NanoMenu.h", "NanoPlatform.h"):
            self.assertIn(f, files)
        # Android-only, compiled out on Linux, or a platform's own implementation.
        for f in ("main.cpp", "NanoOtaCheck.cpp", "NanoArchive.cpp", "DrasticRunner.cpp", "NanoBtStable.cpp"):
            self.assertNotIn(f, files)
        self.assertFalse([f for f in files if f.startswith(("platform/", "compat/", "test/"))])

    def test_ceilings_only_shrink(self):
        over, under = compare(self.counts, self.ceilings)
        details = []
        for line in over:
            r, k = line.split("[", 1)[0], line.split("[", 1)[1].split("]", 1)[0]
            new = self.hits[r].get(k, [])
            details.append(line + " (" + WHAT[r] + ")\n      " +
                           "\n      ".join("%s:%d %s" % h for h in new[:15]))
        self.assertFalse(over, "Linux ratchets went up (OA-4: shared code reaches the platform only through "
                               "nanoplat; put the new platform work in nano/platform/<platform>/ behind an "
                               "interface instead; ceilings are never raised):\n  " + "\n  ".join(details))
        self.assertFalse(under, "Linux ratchets went down: well done. Lower the ceilings so they stay down:\n  " +
                         LOWER + "\n  " + "\n  ".join(under))

    def test_every_ratchet_has_a_ceiling(self):
        self.assertEqual(sorted(self.ceilings), sorted(RATCHETS))

    def test_no_platform_conditionals_in_linux_shared_code(self):
        hits = list(platform_conditionals(NANO, self.files))
        self.assertEqual(hits, [], "__ANDROID__, __linux__, __BIONIC__, __GLIBC__ or NANO_PLATFORM_* in shared "
                                   "code: put the platform's half in nano/platform/<platform>/ instead")


class Counters(unittest.TestCase):
    """The counters on a scratch tree."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="nano-linux-ratchets-")
        self.addCleanup(shutil.rmtree, self.tmp)

    def put(self, rel, text):
        p = os.path.join(self.tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def test_first_program(self):
        self.assertEqual(first_program("am force-stop "), "am")
        self.assertEqual(first_program("timeout -s KILL 10 cmd display gammaos-monitor-mode "), "cmd")
        self.assertEqual(first_program("timeout 5 pm list packages"), "pm")
        self.assertEqual(first_program("/system/bin/restorecon"), "restorecon")
        self.assertEqual(first_program("TZ=UTC settings get global x"), "settings")
        self.assertEqual(first_program("input keyevent 26"), "input")
        self.assertEqual(first_program("input"), "")         # an everyday word on its own
        self.assertEqual(first_program("Settings"), "")
        self.assertEqual(first_program("/system/bin/curl -s"), "")
        self.assertEqual(first_program("amount"), "")
        self.assertEqual(first_program(""), "")

    def test_counts_over_a_tree(self):
        self.put("build/sources.txt", "[core]\nA.cpp\nOff.cpp\n[drm]\nD.cpp\n[firmware]\nmain.cpp\n"
                                      "[scrape]\nA.cpp\nS.cpp\n")
        self.put("cmake/features.cmake", "set(NANO_FEATURE_FILES_X Off.cpp)\n")
        self.put("A.cpp", '#include <utils/Log.h>\n#include <binder/IBinder.h>\n// #include <gui/Surface.h>\n'
                          'void f() { system("am start x"); popen("timeout -s KILL 3 cmd package y", "r");\n'
                          '  obj.system(); x->fork(); open("/data/system/x"); open("/sys/class/x");\n'
                          '  const char* g = "/sys/class/devfreq/fde60000.gpu/load"; /* system("pm x") */ }\n')
        self.put("Off.cpp", 'void g() { system("am x"); }\n')
        self.put("D.cpp", 'static const char* p = "rk356x"; const char* q = "albums sums ums";\n')
        self.put("S.cpp", 'void s() { fork(); execv("/system/bin/rm", 0); std::system("x"); ::popen("y", "r");\n'
                          '  ::std::system("z"); my::system(); notstd::popen(); mystd::fork(); my::std::fork(); }\n')
        self.put("main.cpp", 'int main() { system("am x"); }\n')
        self.put("platform/firmware/P.cpp", 'void p() { system("am x"); }\n')
        self.put("NanoPlatform.h", "struct P { virtual void setBrightnessViaHal(int) = 0;\n"
                                   "  virtual void mirrorBrightness(int) = 0; // mirrorX in a comment\n"
                                   "  virtual void commit(const android :: LaunchTuple&) = 0; };\n")
        self.assertEqual(linux_shared_sources(self.tmp), ["A.cpp", "D.cpp", "S.cpp", "NanoPlatform.h"])
        hits = linux_ratchet_hits(self.tmp)
        counts = {r: {k: len(v) for k, v in hits[r].items()} for r in RATCHETS}
        self.assertEqual(counts["spawners"], {"system": 3, "popen": 2, "fork": 1, "execv": 1})
        self.assertEqual(counts["android_tool_literals"], {"total": 2})
        self.assertEqual([h[2] for h in hits["android_includes"]["total"]], ["binder/IBinder.h"])
        self.assertEqual(counts["android_paths"], {"/data": 1, "/system": 1})
        self.assertEqual(counts["device_literals"], {"fde60000": 1, "rk356x": 1, "ums": 1})
        self.assertEqual(sorted(h[2] for h in hits["android_named_members"]["total"]),
                         ["android::LaunchTuple", "mirrorBrightness", "setBrightnessViaHal"])

    def test_compare_and_lower(self):
        ceilings = {"spawners": {"system": 3, "popen": 2}, "device_literals": {"ums": 1}}
        counts = {"spawners": {"system": 2, "popen": 2, "fork": 1}, "device_literals": {}}
        over, under = compare(counts, ceilings)
        self.assertEqual(over, ["spawners[fork]: 1, ceiling 0"])
        self.assertEqual(under, ["spawners[system]: 2, ceiling 3", "device_literals[ums]: 0, ceiling 1"])
        new = lowered(counts, ceilings)
        self.assertEqual(new["spawners"], {"popen": 2, "system": 2})   # fork is never added
        self.assertEqual(new["device_literals"], {})


class SharedSources(unittest.TestCase):
    def test_platform_firmware_is_not_shared(self):
        files = shared_sources(NANO, Upstream.load(ROOT))
        self.assertIn("NanoMenu.cpp", files)
        self.assertFalse([f for f in files if f.startswith("platform/")])

    def test_include_prefixes_match_the_census(self):
        sys.path.insert(0, TOOL_DIR)
        import include_census
        self.assertEqual(tuple(ratchets.ANDROID_INCLUDE_PREFIXES), tuple(include_census.ANDROID_PREFIXES))


if __name__ == "__main__":
    if sys.argv[1:] == ["--lower"]:
        sys.exit(lower_baseline())
    unittest.main()
