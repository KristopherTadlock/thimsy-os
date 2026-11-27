"""Ratchets on nano/'s shared files (docs/linux/IMPLEMENTATION.md H.4). Fork-only checks: they live
outside tests/, so they never gate an Android release build. Run them with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests

`python3 tools/nano-tree/tests/test_nano_ratchets.py --update-props` rewrites props.txt after a
deliberate change of property keys (or after the merge driver imported new ones from nano-144).
"""
from __future__ import annotations

import os
import re
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
# __ANDROID__, __linux__, __BIONIC__, __GLIBC__ and NANO_PLATFORM_* in shared files (platform/** and
# compat/ are not shared): none today, and none may be added (the seam is cut with nanoplat, shims,
# stubs and link-time substitution, not #ifdefs). The Linux ratchets are in test_linux_ratchets.py.
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
        # compat/ is the Linux and host shim layer: its nanocompat/bionic.h tests __BIONIC__ and
        # __GLIBC__ by design. Everything else shared has none.
        files = [f for f in self.files if not f.startswith("compat/")]
        hits = list(platform_conditionals(NANO, files))
        self.assertLessEqual(len(hits), PLATFORM_CONDITIONAL_BUDGET,
                             "__ANDROID__ / __linux__ / __BIONIC__ / __GLIBC__ / NANO_PLATFORM_* in shared "
                             "files (budget %d):\n  %s" % (
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


# --- The settings dispatcher's stores are the platform's (W6-caps) ----------------------------------
# readSettingValue and writeSettingValue (NanoMenuSettingsTree.cpp) ran Android's `settings` tool for
# every kGlobal, kSecure and kSystem row. That code moved behind nanoplat::Settings, into
# platform/firmware/Settings.cpp (docs/linux/ARCHITECTURE-AUDIT.md step 3). No "settings get" or
# "settings put" string literal may come back to the dispatcher's files, and the other files Linux
# compiles from Nano's shared code may not gain one: SETTINGS_TOOL_CEILINGS are their counts, which
# only shrink as later steps (SystemSettings, Session) move the rest. Lower a ceiling when a count
# drops; never raise one.
SETTINGS_TOOL = re.compile(r"\bsettings (?:get|put)\b")
SETTINGS_DISPATCHER_FILES = ("NanoMenuSettingsTree.cpp", "NanoMenuSettingsTree.h")
SETTINGS_TOOL_CEILINGS = {
    "NanoControlCenter.cpp": 4,
    "NanoMenu.cpp": 1,
    "NanoMenuOverlay.cpp": 6,
    "NanoMenuPS3Menu.cpp": 6,
    "NanoMenuRender.cpp": 1,
    "NanoMenuSetupWizard.cpp": 4,
}


def settings_tool_literals(nano_dir, files):
    """{file: [(line, literal)]} for the string literals in FILES that run `settings get` or `settings put`."""
    from nanotree.ratchets import lex
    out = {}
    for rel in files:
        path = os.path.join(nano_dir, *rel.split("/"))
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            _, strings = lex(fh.read().decode("utf-8", "replace"))
        for line, text in strings:
            if SETTINGS_TOOL.search(text):
                out.setdefault(rel, []).append((line, text))
    return out


@unittest.skipUnless(os.path.isfile(os.path.join(NANO, "build", "sources.txt")), "no nano/build/sources.txt")
class SettingsDispatcherInFirmware(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from nanotree.ratchets import linux_shared_sources
        cls.files = linux_shared_sources(NANO)
        cls.hits = settings_tool_literals(NANO, cls.files)

    def test_the_dispatcher_runs_no_settings_tool(self):
        for f in SETTINGS_DISPATCHER_FILES:
            self.assertIn(f, self.files)
            self.assertNotIn(f, self.hits, "%s runs Android's settings tool again; it belongs in "
                             "platform/firmware/Settings.cpp behind nanoplat::Settings (W6-caps): %s" %
                             (f, self.hits.get(f)))

    def test_shared_code_gains_no_settings_tool_literal(self):
        problems = []
        for f in sorted(set(self.hits) | set(SETTINGS_TOOL_CEILINGS)):
            n, cap = len(self.hits.get(f, [])), SETTINGS_TOOL_CEILINGS.get(f, 0)
            if n > cap:
                problems.append("%s: %d, ceiling %d (%s); OA-4: reach the platform's settings through "
                                "nanoplat::Settings (readSettingValue/writeSettingValue) instead" %
                                (f, n, cap, "; ".join("%d %s" % h for h in self.hits[f][:5])))
            elif n < cap:
                problems.append("%s: %d, ceiling %d: lower SETTINGS_TOOL_CEILINGS in this file" % (f, n, cap))
        self.assertFalse(problems, "settings get/put literals in Linux-compiled shared code:\n  " +
                         "\n  ".join(problems))

    def test_the_firmware_keeps_them(self):
        hits = settings_tool_literals(NANO, ["platform/firmware/Settings.cpp"])
        texts = [t for _l, t in hits.get("platform/firmware/Settings.cpp", [])]
        self.assertEqual(texts, ["settings get ", "settings put "])

    def test_checker(self):
        import shutil
        import tempfile
        tmp = tempfile.mkdtemp(prefix="nano-settings-")
        self.addCleanup(shutil.rmtree, tmp)
        with open(os.path.join(tmp, "A.cpp"), "w", encoding="utf-8", newline="\n") as f:
            f.write('// settings get in a comment is fine\n'
                    'void a() { system("settings put global x 1"); popen("settings get system y", "r");\n'
                    '  const char* s = "Settings"; const char* t = "mysettings get"; }\n')
        self.assertEqual(settings_tool_literals(tmp, ["A.cpp", "Missing.cpp"]),
                         {"A.cpp": [(2, "settings put global x 1"), (2, "settings get system y")]})


# The Linux settings table (platform/linux/Settings.cpp's kNanoPreferences, W6-caps) holds only Nano's
# own preferences that the dispatcher reaches: each key is a literal of the settings rows or of a
# dispatcher call, and none is a key another Android process acts on (inv_props.md class 1b).
LINUX_SETTINGS = os.path.join(NANO, "platform", "linux", "Settings.cpp")
SETTINGS_ROW_FILES = ("NanoMenuSettingsTree.cpp", "NanoMenuPS3Menu.cpp")
ANDROID_CONSUMED_PREFIXES = ("persist.gammaos.gamepad.", "persist.gammaos.hotkeys.", "persist.gammaos.rotate.",
                             "persist.gammaos.shader.", "persist.gammaos.rgb.", "persist.gammaos.bfi.",
                             "persist.gammaos.dualstack.", "persist.gammaos.nano.screencal.", "persist.sys.spk.",
                             "ro.", "sys.")
ANDROID_CONSUMED_KEYS = ("persist.gammaos.nano.orientation", "persist.gammaos.nano.orient_mode",
                         "persist.gammaos.nano.ctrl_screen", "persist.gammaos.nano.mode_quick_settings",
                         "persist.gammaos.nano.display.reading", "persist.gammaos.nano.display.saturation",
                         "persist.gammaos.nano.display.cal_r", "persist.gammaos.nano.display.cal_g",
                         "persist.gammaos.nano.display.cal_b", "persist.gammaos.nano.browser_pkg",
                         "persist.gammaos.nano.emu_sd", "persist.gammaos.nano.pspclock.freezeapp",
                         "persist.gammaos.nano.pspclock.liveapp")


def linux_nano_preferences(path):
    """The keys of kNanoPreferences in PATH, in file order."""
    from nanotree.ratchets import lex
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")
    m = re.search(r"kNanoPreferences\[\] = \{\n(.*?)\n\};", text, re.S)
    if not m:
        return []
    _, strings = lex(m.group(1))
    return [s for _l, s in strings]


@unittest.skipUnless(os.path.isfile(LINUX_SETTINGS), "no nano/platform/linux/Settings.cpp")
class LinuxSettingsTable(unittest.TestCase):
    def test_the_table_is_nanos_own_preferences(self):
        from nanotree.ratchets import lex
        keys = linux_nano_preferences(LINUX_SETTINGS)
        self.assertGreater(len(keys), 40)
        self.assertEqual(keys, sorted(set(keys)), "kNanoPreferences must be sorted (strcmp) and unique")
        literals = set()
        for rel in SETTINGS_ROW_FILES:
            with open(os.path.join(NANO, rel), "rb") as fh:
                literals.update(s for _l, s in lex(fh.read().decode("utf-8", "replace"))[1])
        unused = [k for k in keys if k not in literals]
        self.assertEqual(unused, [], "keys no settings row or dispatcher call names (stale; remove them)")
        android = [k for k in keys if k.startswith(ANDROID_CONSUMED_PREFIXES) or k in ANDROID_CONSUMED_KEYS]
        self.assertEqual(android, [], "keys another Android process acts on are unsupported on Linux (OA-4)")


if __name__ == "__main__":
    if sys.argv[1:] == ["--update-props"]:
        sys.exit(update_props())
    unittest.main()
