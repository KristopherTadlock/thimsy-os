"""The shell gate's reach (docs/linux/IMPLEMENTATION.md B.5, H.4): every process-starting function Nano's
shared runtime files call is wrapped by nano/platform/linux/linker-wraps.txt, and every fork-and-exec
site has a reviewed row with its Linux behaviour, so a new site arriving from nano-144 fails until it
has one. Fork-only. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_shell_sites.py
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from typing import Dict

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.config import Upstream  # noqa: E402
from nanotree.pathroots import runtime_files  # noqa: E402
from nanotree.ratchets import _read, lex, shared_sources  # noqa: E402

NANO = os.path.join(ROOT, "nano")
WRAPS = os.path.join(NANO, "platform", "linux", "linker-wraps.txt")

# Functions that start a program. Each one Nano calls must be wrapped (or the gate is bypassed).
SPAWNERS = ("system", "popen", "pclose", "execv", "execvp", "execve", "execl", "execlp", "execle", "execvpe",
            "fexecve", "posix_spawn", "posix_spawnp")
CALL = re.compile(r"(?<![A-Za-z0-9_.>:])(?:::)?(%s)\s*\(" % "|".join(SPAWNERS))

# Fork-and-exec sites (files calling an exec function), with what the gate does to them on Linux.
EXEC_SITES: Dict[str, str] = {
    "NanoMenuEsdeMenu.cpp": "esdeRunProc: nsenter -t 1 -m -- dropped; /system/bin/{rm,mkdir,unzip} run the Linux "
                            "tools; gammaos-net download becomes curl -fsSL -o FILE URL",
    "NanoOtaCheck.cpp": "curl passes; /system/bin/{rm,mkdir} run the Linux tools (the OTA hand-off itself is "
                        "refused in control-keys.json)",
    "NanoSdMove.cpp": "runArgv: /system/bin/am refused (ENOENT, the child _exit(127)s); restorecon succeeds as a "
                      "no-op; other /system/bin POSIX tools run the Linux tools",
}


def _wrapped():
    with open(WRAPS, "r", encoding="utf-8") as f:
        return {ln.split("#", 1)[0].strip() for ln in f if ln.split("#", 1)[0].strip()}


def _calls():
    """{function: {file: count}} over the shared runtime files, in code (not comments or strings)."""
    out: Dict[str, Dict[str, int]] = {}
    for rel in runtime_files(shared_sources(NANO, Upstream.load(ROOT))):
        code, _ = lex(_read(os.path.join(NANO, *rel.split("/"))))
        for m in CALL.finditer(code):
            per = out.setdefault(m.group(1), {})
            per[rel] = per.get(rel, 0) + 1
    return out


@unittest.skipUnless(os.path.isfile(WRAPS), "no nano/platform/linux here")
class ShellSites(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calls = _calls()

    def test_every_spawner_is_wrapped(self):
        missing = sorted(f for f in self.calls if f not in _wrapped())
        self.assertFalse(missing, "Nano calls %s, which linker-wraps.txt does not wrap: the shell gate would be "
                         "bypassed (%s)" % (missing, {f: sorted(self.calls[f])[:3] for f in missing}))

    def test_exec_sites_have_rows(self):
        sites = set()
        for fn in ("execv", "execvp", "execve", "execl", "execlp", "execle", "execvpe", "fexecve"):
            sites.update(self.calls.get(fn, {}))
        self.assertEqual(sorted(sites), sorted(EXEC_SITES),
                         "fork-and-exec sites changed: give each new one a row in EXEC_SITES with its Linux "
                         "behaviour (and teach ShellGate.cpp what it needs)")

    def test_system_and_popen_are_used(self):
        # A sanity check of the scan itself: Nano runs many shell commands.
        self.assertGreater(sum(self.calls.get("system", {}).values()), 50)
        self.assertGreater(sum(self.calls.get("popen", {}).values()), 10)


if __name__ == "__main__":
    unittest.main()
