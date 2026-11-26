"""nano/build/sources.txt against nano/Android.bp (docs/linux/IMPLEMENTATION.md C.1), and the files
generated from it (nano/build/unit-closure.txt, tools/nano-tree/include-census.json) against
nano/. Fork-only; run with

  py -3.12 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests
"""
from __future__ import annotations

import os
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

import include_census  # noqa: E402
from nanotree.androidbp import Blueprint, expand_globs  # noqa: E402
from nanotree.config import NanoTreeError, Upstream  # noqa: E402
from nanotree.sources import SECTIONS, SourceList  # noqa: E402

NANO = os.path.join(ROOT, "nano")
SOURCES = os.path.join(NANO, "build", "sources.txt")
HAVE = os.path.isfile(SOURCES) and os.path.isfile(os.path.join(NANO, "Android.bp"))
# drastic-nano's test: its production file is a sibling outside nano/ (C.1).
NOT_HOST_TESTS = {"test/unit/DsScreenLayout_test.cpp"}


def _nano_files():
    out = []
    for dirpath, dirnames, names in os.walk(NANO):
        dirnames.sort()
        for n in names:
            out.append(os.path.relpath(os.path.join(dirpath, n), NANO).replace(os.sep, "/"))
    return sorted(out)


class Format(unittest.TestCase):
    def test_parse(self):
        s = SourceList.parse("# c\n[core]\na.cpp\n\n  sub/b.cpp  \n[drm]\n# x\n[firmware]\nm.cpp\n")
        self.assertEqual(s["core"], ["a.cpp", "sub/b.cpp"])
        self.assertEqual(s["drm"], [])
        self.assertEqual(s["firmware"], ["m.cpp"])
        self.assertEqual(s["linux"], [])

    def test_rejects(self):
        for text in ("a.cpp\n", "[nope]\n", "[core]\n[core]\n", "[core]\na.cpp\na.cpp\n",
                     "[core]\n/abs.cpp\n", "[core]\n../up.cpp\n", "[core]\na b.cpp\n"):
            with self.assertRaises(NanoTreeError, msg=text):
                SourceList.parse(text)

    def test_tests_and_production(self):
        s = SourceList.parse("[test-unit]\nA.cpp\ntest/unit/A_test.cpp\n")
        self.assertEqual(s.test_programs(), ["test/unit/A_test.cpp"])
        self.assertEqual(s.test_production(), ["A.cpp"])


class Scan(unittest.TestCase):
    def test_conditionals_and_guard(self):
        text = ("#ifndef X_H\n#define X_H\n#include <zlib.h>\n#ifdef __aarch64__\n#include <arm_neon.h>\n"
                "#endif\n#if __has_include(\"gen.h\")\n#include \"gen.h\"\n#endif\n#endif\n")
        incs, optional = include_census.scan(text)
        self.assertEqual([(i.name, i.conditional) for i in incs],
                         [("zlib.h", False), ("arm_neon.h", True), ("gen.h", True)])
        self.assertEqual(optional, {"gen.h"})

    def test_classify(self):
        sib = {"DsScreenLayout.h"}
        cases = {("zlib.h", "<"): ("lib", "zlib"), ("openssl/md5.h", "<"): ("lib", "openssl"),
                 ("binder/IBinder.h", "<"): ("android", ""), ("vector", "<"): ("system", ""),
                 ("sys/stat.h", "<"): ("system", ""), ("gtest/gtest.h", "<"): ("gtest", ""),
                 ("DsScreenLayout.h", '"'): ("sibling", ""), ("Nope.h", '"'): ("missing", ""),
                 ("weird/thing.h", "<"): ("unknown", ""), ("arm_neon.h", "<"): ("arch", ""),
                 ("windows.h", "<"): ("foreign", ""), ("thimsy/config.h", "<"): ("libthimsy", "")}
        for (name, quote), want in cases.items():
            self.assertEqual(include_census.classify_outside(name, quote, sib), want, name)


@unittest.skipUnless(HAVE, "no nano/build/sources.txt in this checkout")
class AgainstAndroidBp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = SourceList.load(SOURCES)
        cls.bp = Blueprint.load(os.path.join(NANO, "Android.bp"))
        cls.files = _nano_files()

    def module_srcs(self, name):
        m = self.bp.module(name)
        self.assertIsNotNone(m, name)
        return [s for s in expand_globs([s for s in self.bp.srcs(m) if not s.startswith(":")], self.files)]

    def test_every_section_known_and_present(self):
        self.assertEqual(list(self.src.sections), list(SECTIONS))

    def test_nano_srcs_partitioned(self):
        """Every file in gammaos-nano's srcs is in exactly one of [core], [drm] and [firmware], and
        those three hold nothing else."""
        srcs = self.module_srcs("gammaos-nano")
        where = {}
        problems = []
        for sec in ("core", "drm", "firmware"):
            for p in self.src[sec]:
                if p in where:
                    problems.append("%s is in [%s] and [%s]" % (p, where[p], sec))
                where[p] = sec
        problems += ["%s (gammaos-nano) is in none of [core], [drm], [firmware]" % p for p in srcs if p not in where]
        problems += ["%s is in [%s] but not in gammaos-nano's srcs" % (p, s) for p, s in sorted(where.items())
                     if p not in srcs]
        self.assertEqual(problems, [])

    def test_platform_sections(self):
        """[linux] is every platform/linux .cpp outside platform/linux/tests/, and [host] every
        platform/host .cpp plus the headless main. NanoPlatform.cpp, which every platform links, is
        in [core] since G7-main added it to gammaos-nano, and in no platform section; the firmware
        platform (platform/firmware) is all [firmware]."""
        def cpps(prefix):
            return [p for p in self.files if p.startswith(prefix) and p.endswith(".cpp")]
        linux = [p for p in cpps("platform/linux/") if not p.startswith("platform/linux/tests/")]
        self.assertTrue(linux, "no platform/linux sources")
        self.assertEqual(sorted(self.src["linux"]), sorted(linux))
        self.assertEqual([p for p in self.src["linux"] if "/tests/" in p], [])
        self.assertEqual(sorted(self.src["host"]),
                         sorted(["test/host/headless/main_headless.cpp"] + cpps("platform/host/")))
        self.assertIn("NanoPlatform.cpp", self.src["core"])
        firmware = cpps("platform/firmware/")
        self.assertIn("platform/firmware/Platform.cpp", firmware)
        self.assertEqual(sorted(p for p in self.src["firmware"] if p.startswith("platform/")), sorted(firmware))
        self.assertEqual([p for p in self.src["dropped-linux"] if p.startswith("platform/")], [],
                         "a platform's createPlatform must never be stubbed")

    def test_dropped_linux_subset_of_firmware(self):
        self.assertEqual(sorted(set(self.src["dropped-linux"]) - set(self.src["firmware"])), [])

    def test_scrape(self):
        self.assertEqual(sorted(self.src["scrape"]), sorted(self.module_srcs("gammaos-nano-scrape")))

    def test_test_unit(self):
        """[test-unit]'s production files are gammaos_nano_unit_tests' (no more, no fewer), and its
        tests are test/unit/*_test.cpp without drastic-nano's."""
        m = self.bp.module("gammaos_nano_unit_tests")
        entries = [s for s in self.bp.srcs(m) if not s.startswith(":")]
        globbed = [s for s in entries if any(c in s for c in "*?[")]
        production = [s for s in entries if s not in globbed]
        tests = sorted(set(expand_globs(globbed, self.files)) - NOT_HOST_TESTS)
        self.assertEqual(sorted(self.src.test_production()), sorted(production))
        self.assertEqual(sorted(self.src.test_programs()), tests)

    def test_listed_files_exist(self):
        fileset = set(self.files)
        missing = ["[%s] %s" % (sec, p) for sec, paths in self.src.sections.items() for p in paths
                   if p not in fileset]
        self.assertEqual(missing, [])

    def test_fork_only(self):
        """sources.txt, the closure and the host build never reach the patch (upstream.json)."""
        cfg = Upstream.load(ROOT)
        for rel in ("build/sources.txt", "build/unit-closure.txt", "test/host/Makefile", "test/host/DEPS.md"):
            self.assertTrue(cfg.is_fork_only(rel), rel)

    def test_no_hand_written_stubs(self):
        """[dropped-linux] stubs are generated at build time (C.1); none are committed."""
        hits = [p for p in self.files if "/dropped/" in "/" + p]
        self.assertEqual(hits, [])


@unittest.skipUnless(HAVE, "no nano/build/sources.txt in this checkout")
class Generated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = include_census.Census(ROOT)
        cls.src = SourceList.load(SOURCES)

    def committed(self, rel):
        path = os.path.join(ROOT, *rel.split("/"))
        if not os.path.isfile(path):
            return None
        with open(path, "r", encoding="utf-8", newline="") as f:
            return f.read()

    def test_unit_closure_current(self):
        want = include_census.unit_closure_text(self.census, self.src)
        self.assertEqual(self.committed(include_census.CLOSURE_REL), want,
                         "nano/build/unit-closure.txt is stale: py -3.12 tools/nano-tree/include_census.py write")

    def test_census_current(self):
        have = self.committed(include_census.CENSUS_REL)
        if have is None:
            self.skipTest("no tools/nano-tree/include-census.json")
        self.assertEqual(have, include_census.census_json(self.census),
                         "include-census.json is stale: py -3.12 tools/nano-tree/include_census.py write")

    def test_closure_rows(self):
        rows = {p: (c, n) for p, c, n in include_census.unit_closure(self.census, self.src)}
        self.assertEqual(sorted(rows), sorted(os.path.basename(t)[:-4] for t in self.src.test_programs()))
        production = set(self.src.test_production())
        for prog, (closure, needs) in rows.items():
            self.assertLessEqual(set(closure), production, prog)
        # Programs that need nothing outside: the ones check-unit-0 builds.
        self.assertEqual(rows["NanoJson_test"], (["NanoJson.cpp"], []))
        self.assertIn("zlib.h", rows["NanoNdsBanner_test"][1])
        self.assertIn("tinyxml2.h", rows["NanoEsdeTheme_test"][1])

    def test_no_unknown_includes_in_tests(self):
        """Every include the unit programs reach is classified (no 'unknown' or 'missing')."""
        bad = []
        for t in self.src.test_programs() + self.src.test_production():
            needs = self.census.needs(t)
            for cls in ("unknown", "missing"):
                bad += ["%s: %s %s" % (t, cls, h) for h in sorted(needs.get(cls, ()))]
        self.assertEqual(bad, [])


if __name__ == "__main__":
    unittest.main()
