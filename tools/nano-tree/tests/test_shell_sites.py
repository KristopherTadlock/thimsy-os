"""The shell gate's reach (docs/linux/IMPLEMENTATION.md B.5, H.4; OA-4 migration steps 0 and 1).

- Every process-starting function Nano's Linux-compiled shared files call is wrapped by
  nano/platform/linux/linker-wraps.txt, so none bypasses the gate.
- Every fork-and-exec site has a reviewed row with its Linux behaviour, so a new site arriving from
  nano-144 fails until it has one.
- The program starts may not grow: their ceilings are the "spawners" of tools/nano-tree/linux-ratchets.json
  (shared with test_linux_ratchets.py), and they only shrink as the migration moves the commands behind
  nanoplat.
- The gate refuses by default (ShellGate.cpp). Its temporary allow-list (kAllow) has exactly the reviewed
  entries below, each naming its call site and marked "TODO(OA-4 step 4): remove".

Fork-only. Run with

  py -3.12 -B -m unittest discover -s tools/nano-tree/tests -p test_shell_sites.py
"""
from __future__ import annotations

import json
import os
import re
import sys
import unittest
from typing import Dict, List, Tuple

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.ratchets import SPAWNERS, _read, lex, linux_shared_sources  # noqa: E402

NANO = os.path.join(ROOT, "nano")
WRAPS = os.path.join(NANO, "platform", "linux", "linker-wraps.txt")
GATE = os.path.join(NANO, "platform", "linux", "ShellGate.cpp")
BASELINE = os.path.join(TOOL_DIR, "linux-ratchets.json")
LOWER = "py -3.12 tools/nano-tree/tests/test_linux_ratchets.py --lower"

# Functions that start a program (and pclose, which the gate must see to answer its own streams).
WRAPPED_FUNCS = SPAWNERS + ("pclose",)
CALL = re.compile(r"(?<![A-Za-z0-9_.>:])(?:(?:std)?::)?(%s)\s*\(" % "|".join(WRAPPED_FUNCS))
EXEC_FUNCS = ("execv", "execvp", "execve", "execl", "execlp", "execle", "execvpe", "fexecve")
# fork and vfork start nothing by themselves (the exec after them does): they need no wrap.
UNWRAPPED_OK = ("fork", "vfork")

# Fork-and-exec sites in Linux-compiled shared code (files calling an exec function), with what the gate
# does to them on Linux. NanoOtaCheck.cpp's are compiled out (NANO_WITH_ARCHIVE=OFF).
EXEC_SITES: Dict[str, str] = {
    "NanoMenuEsdeMenu.cpp": "esdeRunProc: every step refused (ENOENT, the child _exit(127)s): /system/bin "
                            "mkdir, rm, unzip, mv, chown system:system, restorecon and gammaos-net download; the "
                            "image has no curl, so the download could not work anyway (step 4: Http, FileOps, "
                            "Archive)",
    "NanoSdMove.cpp": "runArgv (and the BIOS RunFn callbacks): /system/bin/am, restorecon, logcat and pidof "
                      "<package> refused (ENOENT, the child _exit(127)s); every caller ignores or only logs "
                      "restorecon's result",
}

# The gate's temporary allow-list (ShellGate.cpp kAllow): call site -> why it may still run on Linux.
# An entry is added only after review against OA-4, and removed by migration step 4.
ALLOW_LIST: Dict[str, str] = {
    "NanoMenuSetupWizard.cpp:318 tzStandardOffset": "TZ=<zone> date -d @<epoch> +%z: busybox date and the "
                                                     "image's zoneinfo give the setup wizard a zone's offset",
}


def _wrapped():
    with open(WRAPS, "r", encoding="utf-8") as f:
        return {ln.split("#", 1)[0].strip() for ln in f if ln.split("#", 1)[0].strip()}


def _calls():
    """{function: {file: count}} over the Linux-compiled shared files, in code (not comments or strings)."""
    out: Dict[str, Dict[str, int]] = {}
    for rel in linux_shared_sources(NANO):
        code, _ = lex(_read(os.path.join(NANO, *rel.split("/"))))
        for m in CALL.finditer(code):
            per = out.setdefault(m.group(1), {})
            per[rel] = per.get(rel, 0) + 1
    return out


def allow_entries(text: str) -> List[Tuple[str, str]]:
    """(site, the comment block above it) for each entry of ShellGate.cpp's kAllow."""
    m = re.search(r"constexpr AllowRule kAllow\[\] = \{\n(.*?)\n\};", text, re.S)
    if not m:
        return []
    out = []
    comment: List[str] = []
    for line in m.group(1).split("\n"):
        s = line.strip()
        if s.startswith("//"):
            comment.append(s[2:].strip())
            continue
        e = re.match(r'\{"([^"]+)",\s*kVia', s)
        if e:
            out.append((e.group(1), " ".join(comment)))
            comment = []
    return out


@unittest.skipUnless(os.path.isfile(WRAPS), "no nano/platform/linux here")
class ShellSites(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calls = _calls()

    def test_every_spawner_is_wrapped(self):
        missing = sorted(f for f in self.calls if f not in _wrapped() and f not in UNWRAPPED_OK)
        self.assertFalse(missing, "Nano calls %s, which linker-wraps.txt does not wrap: the shell gate would be "
                         "bypassed (%s)" % (missing, {f: sorted(self.calls[f])[:3] for f in missing}))

    def test_exec_sites_have_rows(self):
        sites = set()
        for fn in EXEC_FUNCS:
            sites.update(self.calls.get(fn, {}))
        self.assertEqual(sorted(sites), sorted(EXEC_SITES),
                         "fork-and-exec sites changed: give each new one a row in EXEC_SITES with its Linux "
                         "behaviour (the gate refuses it unless it is on ShellGate.cpp's allow-list)")

    def test_program_starts_do_not_grow(self):
        # A ceiling, not a floor (H.4): the sites only shrink as the migration moves them behind nanoplat.
        with open(BASELINE, "r", encoding="utf-8") as f:
            ceilings = json.load(f)["spawners"]
        problems = []
        for fn in sorted(set(ceilings) | {f for f in self.calls if f in SPAWNERS}):
            n = sum(self.calls.get(fn, {}).values())
            cap = ceilings.get(fn, 0)
            if n > cap:
                problems.append("%s: %d calls, ceiling %d (in %s); OA-4: start programs from "
                                "nano/platform/<platform>/ behind a nanoplat interface instead" % (
                                    fn, n, cap, ", ".join(sorted(self.calls.get(fn, {}))[:5])))
            elif n < cap:
                problems.append("%s: %d calls, ceiling %d: lower the ceiling with %s" % (fn, n, cap, LOWER))
        self.assertFalse(problems, "program starts in Linux-compiled shared code:\n  " + "\n  ".join(problems))


@unittest.skipUnless(os.path.isfile(GATE), "no nano/platform/linux here")
class AllowList(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(GATE, "r", encoding="utf-8") as f:
            cls.text = f.read()
        cls.entries = allow_entries(cls.text)

    def test_entries_are_the_reviewed_ones(self):
        self.assertEqual(sorted(s for s, _c in self.entries), sorted(ALLOW_LIST),
                         "ShellGate.cpp's allow-list changed: OA-4 allows only short-lived, Linux-meaningful "
                         "shapes until migration step 4; review the entry and add it to ALLOW_LIST here")

    def test_each_entry_names_its_site_and_its_removal(self):
        for site, comment in self.entries:
            self.assertIn("TODO(OA-4 step 4): remove", comment, site)
            path = site.split(":", 1)[0]
            self.assertIn(path, comment, "%s: the comment names its call site" % site)
            self.assertTrue(os.path.isfile(os.path.join(NANO, path)), "%s: no such file in nano/" % site)

    def test_the_gate_refuses_by_default(self):
        # The pre-OA-4 tables (pass by default, /system/bin rewrites, restorecon no-ops) are gone.
        for gone in ("kPosixTools", "kAndroidRoots", "Kind::Noop", "ExecAction::PathSearch", "imageHas"):
            self.assertNotIn(gone, self.text)
        self.assertIn("ExecAction::Refuse", self.text)

    def test_parser(self):
        text = ('constexpr AllowRule kAllow[] = {\n    // a.cpp:1 (f): why.\n    // TODO(OA-4 step 4): remove\n'
                '    {"a.cpp:1 f", kViaShell,\n     {"x", nullptr}},\n    // b.cpp:2\n    {"b.cpp:2 g", kViaExec, '
                '{"y", nullptr}},\n};\n')
        self.assertEqual(allow_entries(text), [("a.cpp:1 f", "a.cpp:1 (f): why. TODO(OA-4 step 4): remove"),
                                               ("b.cpp:2 g", "b.cpp:2")])


if __name__ == "__main__":
    unittest.main()
