"""Ratchets on nano/'s shared files (docs/linux/IMPLEMENTATION.md H.4). Fork-only checks: they live
outside tests/, so they never gate an Android release build. Run them with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests

`python3 tools/nano-tree/tests/test_nano_ratchets.py --update-props` rewrites props.txt after a
deliberate change of property keys (or after the merge driver imported new ones from nano-144).
"""
from __future__ import annotations

import os
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.config import Upstream  # noqa: E402
from nanotree.ratchets import (platform_conditionals, property_keys, read_list,  # noqa: E402
                               shared_sources, write_list)

NANO = os.path.join(ROOT, "nano")
PROPS = os.path.join(TOOL_DIR, "props.txt")
# __ANDROID__ and NANO_PLATFORM_* in shared files: none today, and none may be added in passes
# P0 to P3 (the seam is cut with shims, stubs and link-time substitution, not #ifdefs).
PLATFORM_CONDITIONAL_BUDGET = 0
PROPS_HEADER = """Property key literals in nano/'s shared C and C++ files (tools/nano-tree/nanotree/ratchets.py).
The Linux config store serves these names (IMPLEMENTATION.md B.3); test_nano_ratchets.py fails
when the set changes. After a deliberate change, or a merge that brought new keys from nano-144:
  python3 tools/nano-tree/tests/test_nano_ratchets.py --update-props"""


def _sources():
    cfg = Upstream.load(ROOT)
    return shared_sources(NANO, cfg)


def update_props() -> int:
    keys = property_keys(NANO, _sources())
    write_list(PROPS, PROPS_HEADER, list(keys))
    print("props.txt: %d keys" % len(keys))
    return 0


@unittest.skipUnless(os.path.isdir(NANO), "no nano/ in this checkout")
class Ratchets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = _sources()
        cls.keys = property_keys(NANO, cls.files)

    def test_property_keys_match_baseline(self):
        keys = self.keys
        baseline = set(read_list(PROPS))
        added = sorted(set(keys) - baseline)
        removed = sorted(baseline - set(keys))
        msg = []
        for k in added:
            msg.append("new key %s (in %s)" % (k, ", ".join(keys[k][:3])))
        for k in removed:
            msg.append("key gone: %s" % k)
        self.assertFalse(msg, "property keys differ from tools/nano-tree/props.txt; if deliberate, run "
                         "tools/nano-tree/tests/test_nano_ratchets.py --update-props:\n  " + "\n  ".join(msg[:40]))

    def test_no_new_platform_conditionals(self):
        hits = list(platform_conditionals(NANO, self.files))
        self.assertLessEqual(len(hits), PLATFORM_CONDITIONAL_BUDGET,
                             "__ANDROID__ / NANO_PLATFORM_* in shared files (budget %d):\n  %s" % (
                                 PLATFORM_CONDITIONAL_BUDGET,
                                 "\n  ".join("%s:%d %s" % h for h in hits[:40])))

    def test_sources_found(self):
        self.assertIn("NanoMenu.cpp", self.files)
        self.assertGreater(len(self.files), 200)


# --- The seam's frozen-API rule (B.6) ----------------------------------------------------------------
# drastic-nano, gammaos-nano-scrape, the host tests and the other binaries built from Nano's files
# never call nanoplat::install(), so nothing they compile may include NanoPlatform.h or name
# nanoplat::, directly or through a header. The set is read from nano/Android.bp: the filegroup
# drastic_nano_shared_srcs, and every C or C++ module but gammaos-nano itself (the design names
# gammaos-nano-scrape and the cc_test_host modules; the fuzzers, the other binaries and the static
# libraries do not install a platform either).
FROZEN_FILEGROUPS = ("drastic_nano_shared_srcs",)
FROZEN_TYPES = ("cc_binary", "cc_binary_host", "cc_test", "cc_test_host", "cc_fuzz", "cc_library",
                "cc_library_static", "cc_library_shared", "cc_benchmark")
SEAM_MODULE = "gammaos-nano"


def _nano_files(nano_dir):
    out = []
    for dirpath, dirnames, names in os.walk(nano_dir):
        dirnames.sort()
        for n in names:
            out.append(os.path.relpath(os.path.join(dirpath, n), nano_dir).replace(os.sep, "/"))
    return sorted(out)


def frozen_sources(bp, files):
    """The nano/ files the frozen-API rule covers, from a parsed Android.bp."""
    from nanotree.androidbp import expand_globs
    out = set()
    for m in bp.modules:
        frozen = (m.type == "filegroup" and m.name in FROZEN_FILEGROUPS) or \
                 (m.type in FROZEN_TYPES and m.name != SEAM_MODULE)
        if frozen:
            out.update(expand_globs([s for s in bp.srcs(m) if not s.startswith(":")], files))
    return sorted(out)


def seam_users(nano_dir, bp, roots):
    """[(file, why)] for every file reached from ROOTS by #include that includes NanoPlatform.h or
    names nanoplat:: in its code."""
    from nanotree.ratchets import lex
    from nanotree.surface import includes_of, resolve_include
    files = _nano_files(nano_dir)
    fileset = set(files)
    inc_dirs = [""]
    for m in bp.modules:
        for d in m.strings("local_include_dirs") + m.strings("export_include_dirs"):
            if d not in inc_dirs:
                inc_dirs.append(d)
    seen = set()
    todo = [r for r in roots if r in fileset]
    hits = []
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        with open(os.path.join(nano_dir, *f.split("/")), "rb") as fh:
            data = fh.read()
        code, _ = lex(data.decode("utf-8", "replace"))
        for quote, name in includes_of(data):
            if name.split("/")[-1] == "NanoPlatform.h":
                hits.append((f, "includes " + name))
            target = resolve_include(f, quote, name, inc_dirs, fileset)
            if target is not None:
                todo.append(target)
        if "nanoplat::" in "".join(code.split()):
            hits.append((f, "names nanoplat::"))
    return sorted(hits)


@unittest.skipUnless(os.path.isfile(os.path.join(NANO, "Android.bp")), "no nano/Android.bp in this checkout")
class FrozenApi(unittest.TestCase):
    def test_frozen_files_do_not_reach_the_seam(self):
        from nanotree.androidbp import Blueprint
        bp = Blueprint.load(os.path.join(NANO, "Android.bp"))
        roots = frozen_sources(bp, _nano_files(NANO))
        # The set is what the design names, at least.
        for f in ("NanoMenuDrm.cpp", "NanoScraper.cpp", "NanoJson.cpp", "test/unit/NanoJson_test.cpp"):
            self.assertIn(f, roots)
        hits = seam_users(NANO, bp, roots)
        self.assertEqual(hits, [], "files built into binaries that never call nanoplat::install() reach "
                                   "NanoPlatform.h (IMPLEMENTATION.md B.6); pass what they need in from a "
                                   "caller outside the set instead")

    def test_checker_finds_direct_and_indirect_users(self):
        import shutil
        import tempfile
        from nanotree.androidbp import Blueprint
        tmp = tempfile.mkdtemp(prefix="nano-seam-")
        self.addCleanup(shutil.rmtree, tmp)

        def put(rel, text):
            p = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)

        put("Android.bp", 'filegroup { name: "drastic_nano_shared_srcs", srcs: ["Shared.cpp"] }\n'
                          'cc_binary { name: "gammaos-nano", srcs: ["main.cpp", "Shared.cpp"] }\n'
                          'cc_test_host { name: "t", srcs: ["test/unit/*_test.cpp"] }\n'
                          'cc_binary { name: "gammaos-nano-scrape", srcs: ["Scrape.cpp"] }\n')
        put("main.cpp", '#include "NanoPlatform.h"\nint main() { nanoplat::get(); }\n')
        put("Shared.cpp", '#include "Shared.h"\n')
        put("Shared.h", '#pragma once\n#include "sub/Deep.h"\n')
        put("sub/Deep.h", '#include "../NanoPlatform.h"\n')
        put("Scrape.cpp", '// nanoplat:: in a comment is fine\nconst char* s = "nanoplat::";\n'
                          'void f() { nanoplat :: get(); }\n')
        put("test/unit/A_test.cpp", '#include "Clean.h"\n')
        put("Clean.h", "int clean();\n")
        put("NanoPlatform.h", "namespace nanoplat {}\n")
        bp = Blueprint.load(os.path.join(tmp, "Android.bp"))
        roots = frozen_sources(bp, _nano_files(tmp))
        self.assertEqual(roots, ["Scrape.cpp", "Shared.cpp", "test/unit/A_test.cpp"])
        self.assertEqual(seam_users(tmp, bp, roots),
                         [("Scrape.cpp", "names nanoplat::"), ("sub/Deep.h", "includes ../NanoPlatform.h")])


# --- Android's boot-owner decision stays in the firmware (W6-boot) ----------------------------------
# Android's "run Nano or hand the screen to Android's boot animation" decision, its waits for init's
# persistent properties and its ctl.start of the boot animation ended every Linux boot
# (docs/linux/ARCHITECTURE-AUDIT.md defect 1). They now live behind nanoplat::Boot, in
# platform/firmware/Boot.cpp. No file Linux compiles from Nano's shared code (sources.txt's [core]
# and [drm]) may name them again: a string literal holding nano_skip, skip_nano or
# ro.persistent_properties.ready, or a "ctl.start" literal followed by "bootanim" on its line.
BOOT_OWNER_WORDS = ("nano_skip", "skip_nano", "ro.persistent_properties.ready")
LINUX_SHARED_SECTIONS = ("core", "drm")


def boot_owner_hits(nano_dir, files):
    """[(file, line, why)] for the Android boot-owner literals in FILES (paths relative to NANO_DIR)."""
    from nanotree.ratchets import lex
    hits = []
    for rel in files:
        path = os.path.join(nano_dir, *rel.split("/"))
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            _, strings = lex(fh.read().decode("utf-8", "replace"))
        for i, (line, text) in enumerate(strings):
            for word in BOOT_OWNER_WORDS:
                if word in text:
                    hits.append((rel, line, '"%s" names %s' % (text, word)))
            if text == "ctl.start" and i + 1 < len(strings) and strings[i + 1] == (line, "bootanim"):
                hits.append((rel, line, '"ctl.start", "bootanim"'))
    return hits


@unittest.skipUnless(os.path.isfile(os.path.join(NANO, "build", "sources.txt")), "no nano/build/sources.txt")
class BootOwnerInFirmware(unittest.TestCase):
    def test_linux_compiled_shared_code_has_no_android_boot_decision(self):
        from nanotree.sources import SourceList
        src = SourceList.load(os.path.join(NANO, "build", "sources.txt"))
        files = [f for s in LINUX_SHARED_SECTIONS for f in src[s]]
        self.assertIn("NanoMenu.cpp", files)
        self.assertNotIn("platform/firmware/Boot.cpp", files)
        hits = boot_owner_hits(NANO, files)
        self.assertEqual(hits, [], "Android's boot-owner decision in Linux-compiled shared code; it belongs in "
                                   "platform/firmware/Boot.cpp behind nanoplat::Boot (W6-boot):\n  " +
                         "\n  ".join("%s:%d %s" % h for h in hits))

    def test_the_firmware_keeps_it(self):
        hits = boot_owner_hits(NANO, ["platform/firmware/Boot.cpp"])
        whys = " ".join(h[2] for h in hits)
        for want in ("nano_skip", "skip_nano", "ro.persistent_properties.ready", '"ctl.start", "bootanim"'):
            self.assertIn(want, whys)

    def test_checker_finds_each_form(self):
        import shutil
        import tempfile
        tmp = tempfile.mkdtemp(prefix="nano-boot-")
        self.addCleanup(shutil.rmtree, tmp)
        with open(os.path.join(tmp, "A.cpp"), "w", encoding="utf-8", newline="\n") as f:
            f.write('// skip_nano and nano_skip in a comment are fine\n'
                    'int a = open("/data/misc/bootanim/nano_skip", 0);\n'
                    'void b() { property_set("ctl.start", "bootanim"); property_set("ctl.stop", "bootanim"); }\n'
                    'const char* c = "ctl.start";\nconst char* d = "bootanim";\n')
        self.assertEqual([(h[0], h[1]) for h in boot_owner_hits(tmp, ["A.cpp", "Missing.cpp"])],
                         [("A.cpp", 2), ("A.cpp", 3)])


if __name__ == "__main__":
    if sys.argv[1:] == ["--update-props"]:
        sys.exit(update_props())
    unittest.main()
