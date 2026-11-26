"""Control keys (docs/linux/IMPLEMENTATION.md B.2, H.4): every gammaos-nano.rc trigger, every rc trigger
elsewhere in the patch that names one of Nano's keys, every framework listener of Nano's keys and init's
control keys has a row in tools/nano-tree/control-keys.json, and the rows Nano's Linux process acts on
are exactly kControlRows in nano/platform/linux/ControlKeys.cpp. Fork-only: it never gates an Android
build. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_control_keys.py

`python3 tools/nano-tree/tests/test_control_keys.py --list` prints the derived triggers and keys with
their rows.
"""
from __future__ import annotations

import os
import re
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.config import PATCH_REL, Upstream  # noqa: E402
from nanotree.controlkeys import (check_table, covering_row, cpp_rows, derive,  # noqa: E402
                                  expected_cpp_rows, load_props, load_table, rc_triggers)

NANO = os.path.join(ROOT, "nano")
TABLE = os.path.join(TOOL_DIR, "control-keys.json")
PROPS = os.path.join(TOOL_DIR, "props.txt")
CPP = os.path.join(NANO, "platform", "linux", "ControlKeys.cpp")
RC = os.path.join(NANO, "gammaos-nano.rc")
SESSION_DOC = os.path.join(ROOT, "linux", "docs", "session.md")


def _derived():
    with open(os.path.join(ROOT, *PATCH_REL.split("/")), "rb") as f:
        patch = f.read()
    return derive(NANO, patch, Upstream.load(ROOT).prefix, load_props(PROPS))


@unittest.skipUnless(os.path.isfile(RC) and os.path.isfile(TABLE), "no nano/ or control-keys.json here")
class ControlKeys(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = load_table(TABLE)
        cls.derived = _derived()

    def test_every_trigger_and_listener_has_a_row(self):
        problems = check_table(self.table, self.derived)
        self.assertFalse(problems, "tools/nano-tree/control-keys.json:\n  " + "\n  ".join(problems))

    def test_every_rc_trigger_line_is_covered(self):
        # Independent of derive(): every `on ... property:` line of gammaos-nano.rc, read directly.
        with open(RC, "r", encoding="utf-8") as f:
            text = f.read()
        lines = [ln for ln in text.splitlines() if re.match(r"\s*on\s+.*property:", ln)]
        triggers = rc_triggers(text)
        self.assertGreaterEqual(len(lines), 18)
        self.assertGreaterEqual(len(triggers), 15)
        rows = self.table["rows"]
        missing = [t for t in triggers if covering_row(t, rows) is None]
        self.assertFalse(missing, "gammaos-nano.rc triggers without a row: %s" % missing)

    def test_cpp_table_matches(self):
        with open(CPP, "r", encoding="utf-8") as f:
            have = cpp_rows(f.read())
        want = expected_cpp_rows(self.table)
        self.assertEqual(len(have), len(set(have)), "duplicate rows in ControlKeys.cpp")
        only_cpp = sorted(set(have) - set(want))
        only_json = sorted(set(want) - set(have))
        self.assertFalse(only_cpp or only_json,
                         "ControlKeys.cpp and control-keys.json disagree:\n  only in ControlKeys.cpp: %s\n"
                         "  only in control-keys.json: %s" % (only_cpp, only_json))

    def test_session_verbs_are_documented(self):
        with open(SESSION_DOC, "r", encoding="utf-8") as f:
            doc = f.read()
        for r in self.table["rows"]:
            route = r.get("route", "")
            if route.startswith("session:"):
                verb = route[len("session:"):]
                self.assertIn("| `%s`" % verb, doc, "%s: verb %s is not in linux/docs/session.md" % (r["match"], verb))

    def test_routed_triggers_name_nanos_keys(self):
        props = load_props(PROPS)
        for r in self.table["rows"]:
            if r["disposition"] != "routed":
                continue
            key = r["match"].split("=", 1)[0]
            self.assertIn(key, props, "%s is routed but Nano's files never name %s" % (r["match"], key))


class Unit(unittest.TestCase):
    def test_rc_triggers(self):
        text = ("on property:a.b=1 && property:c.d=\n    setprop x 1\non early-init && property:ro.x=y\n"
                "on late-init\non property:a.b=1\n")
        self.assertEqual(rc_triggers(text), ["a.b=1 && c.d=", "early-init && ro.x=y", "a.b=1"])

    def test_check_table(self):
        derived = {"a.b=1": ["rc"], "sys.k": ["X.java"], "persist.p": ["Y.java"]}
        rows = [
            {"match": "a.b=1", "disposition": "routed", "route": "session:sleep", "note": "n"},
            {"match": "persist.*", "disposition": "store-only", "note": "n"},
            {"match": "old.key", "disposition": "refused", "note": "n"},
            {"match": "man.key", "disposition": "refused", "note": "n", "source": "manual"},
        ]
        problems = check_table({"rows": rows}, derived)
        self.assertTrue(any("no row for sys.k" in p for p in problems), problems)
        self.assertTrue(any("stale row old.key" in p for p in problems), problems)
        self.assertFalse(any("man.key" in p for p in problems), problems)
        self.assertFalse(any("persist" in p for p in problems), problems)

    def test_expected_cpp_rows(self):
        rows = [
            {"match": "a.b=1", "disposition": "routed", "route": "session:sleep", "reset": "0", "note": "n"},
            {"match": "k", "value": "1", "disposition": "routed", "route": "first-frame", "note": "n"},
            {"match": "s", "disposition": "routed", "route": "session-key", "note": "n"},
            {"match": "ctl.start", "disposition": "refused", "note": "n"},
            {"match": "x=1 && y=2", "disposition": "refused", "note": "n"},
            {"match": "st", "disposition": "store-only", "note": "n"},
        ]
        self.assertEqual(expected_cpp_rows({"rows": rows}), [
            ("a.b", "1", "Session", "sleep", "0"),
            ("k", "1", "FirstFrame", "", ""),
            ("ctl.start", "", "Refuse", "", ""),
        ])

    def test_cpp_rows(self):
        text = ('x\n// BEGIN control-keys\nconst Row r[] = {\n    {"a.b", "1", Action::Session, "sleep", "0"},\n'
                '};\n// END control-keys\n')
        self.assertEqual(cpp_rows(text), [("a.b", "1", "Session", "sleep", "0")])
        with self.assertRaises(ValueError):
            cpp_rows('// BEGIN control-keys\n    {"a", nullptr, Action::Refuse},\n// END control-keys\n')


def _list() -> int:
    table = load_table(TABLE)
    for item, where in _derived().items():
        r = covering_row(item, table["rows"])
        print("%-60s %-12s %-24s %s" % (item[:60], r["disposition"] if r else "MISSING",
                                        (r or {}).get("route", ""), ", ".join(where[:2])))
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--list"]:
        sys.exit(_list())
    unittest.main()
