"""The settings schema inventory (portable_settings_plan.md §3.3 and §7.1 phase 0): every setting key
nano/ reads or writes has a row in tools/nano-tree/prefs.json, classified as the plan and OA-32 say.
Fork-only: it never gates an Android build. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_prefs.py

`python3 tools/nano-tree/tests/test_prefs.py --list` prints every scanned key with its class, its
settings.ini key, the defaults the code passes and where it is first used.
`python3 tools/nano-tree/tests/test_prefs.py --update-globs` rewrites prefs-globs.txt after a key
under a persist.* glob row was added or dropped and its class was checked.
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
from nanotree.prefs import (Context, Found, Use, check_table, covering_row, expand_runtime,  # noqa: E402
                            glob_lines, linux_only, load_table, report, scan, scan_script, scan_text)
from nanotree.ratchets import read_list, write_list  # noqa: E402

NANO = os.path.join(ROOT, "nano")
TABLE = os.path.join(TOOL_DIR, "prefs.json")
GLOBS = os.path.join(TOOL_DIR, "prefs-globs.txt")
GLOBS_HEADER = """The keys each persist.* glob row in prefs.json covers ("<glob> <key>"; tools/nano-tree/nanotree/prefs.py).
A glob takes in any new key under it, so test_prefs.py fails when this set changes: check the new key's
class (give it its own row if the glob's class is wrong for it), then
  python3 tools/nano-tree/tests/test_prefs.py --update-globs"""
# Keys with class "?" (the owner has not decided). The list in prefs.json may only shrink: classify a
# key, drop it from "unclassified" and lower this number. 9 at first; 14 since the scan lists the
# global button-action rules (persist.gammaos.gamepad.act*), which share the per-app rules' question.
UNCLASSIFIED_BUDGET = 14


@unittest.skipUnless(os.path.isdir(NANO) and os.path.isfile(TABLE), "no nano/ or prefs.json here")
class Prefs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = load_table(TABLE)
        cls.rows = cls.table["rows"]
        cls.found = scan(NANO)

    def cls_of(self, key):
        r = covering_row(key, self.rows)
        self.assertIsNotNone(r, "no row for %s" % key)
        return r["class"]

    def row(self, key):
        for r in self.rows:
            if r["match"] == key:
                return r
        self.fail("no exact row for %s" % key)

    def test_every_key_has_a_row(self):
        problems = check_table(self.table, self.found)
        self.assertFalse(problems, "tools/nano-tree/prefs.json:\n  " + "\n  ".join(problems[:60]))

    def test_unclassified_only_shrinks(self):
        uncl = self.table["unclassified"]
        self.assertEqual(len(uncl), len(set(uncl)))
        self.assertLessEqual(len(uncl), UNCLASSIFIED_BUDGET,
                             "new unclassified keys: classify them (they may not be parked in the list)")
        self.assertEqual(sorted(uncl), sorted(r["match"] for r in self.rows if r["class"] == "?"))

    def test_scan_finds_the_known_shapes(self):
        f = self.found
        self.assertGreater(len(f), 900)
        self.assertIn("60000", f["system:screen_off_timeout"].defaults)          # a kSystem row
        self.assertIn("row", f["persist.gammaos.nano.iptv"].ops)                 # a kProp row
        self.assertIn("persist.gammaos.nano.screencal.", f)                       # a key prefix
        self.assertIn("persist.gammaos.gamepad.pa%d_pkg", f)                      # a printf template
        self.assertIn("persist.thimsy.wifi.enabled", f)                           # Linux-only code
        self.assertIn("en-US", f["persist.sys.locale"].defaults)
        self.assertNotIn("sys.x", f)                                              # tests are left out
        self.assertIn("gammaos-nano.rc", f["persist.gammaos.nano.overlay"].files)  # an init script
        self.assertIn("persist.thimsy.dexopt_done", f)                            # a shell script

    def test_runtime_families(self):
        f = self.found
        self.assertEqual(f.problems, [])
        built = {k for k, u in f.items() if "built" in u.ops}
        for k in ("persist.gammaos.shader.blurfill.blur_scale", "persist.gammaos.shader.blurfill.luma_threshold",
                  "persist.gammaos.shader.crt-simple.curv",                       # GsParamDef tables
                  "persist.gammaos.hotkeys.save",                                 # kHot
                  "persist.gammaos.scraper.media.manual",                         # kExtraMedia
                  "persist.vendor.hue.aux",                                       # kField x main/aux
                  "persist.gammaos.gamepad.act_count", "persist.gammaos.gamepad.act%d_code",
                  "persist.gammaos.gamepad.pa%d_act%d_l",                         # "%sact..." x prefixes
                  "persist.gammaos.nano.pinned_pkgs_%d", "persist.gammaos.qs.blacklist_%d"):  # "%s_%d" x bases
            self.assertIn(k, built)
        self.assertEqual(f["persist.gammaos.hotkeys.save"].defaults, {"BUTTON_R1"})
        self.assertEqual(self.row("persist.gammaos.shader.blurfill.blur_scale")["class"], "A")
        self.assertEqual(self.cls_of("persist.gammaos.gamepad.act_count"), "?")

    def test_glob_keys_match_baseline(self):
        have = glob_lines(self.found, self.rows)
        want = read_list(GLOBS)
        msg = ["new: " + x for x in sorted(set(have) - set(want))] + ["gone: " + x for x in sorted(set(want) - set(have))]
        self.assertFalse(msg, "keys under persist.* glob rows differ from tools/nano-tree/prefs-globs.txt; check each "
                         "new key's class, then run tools/nano-tree/tests/test_prefs.py --update-globs:\n  "
                         + "\n  ".join(msg[:40]))

    def test_oa32_rules(self):
        # Theme travels and mirrors to ES-DE; aspect ratio per model (Q4).
        for k, es in (("themeset", "ThemeSet"), ("variant", "ThemeVariant"), ("colorscheme", "ThemeColorScheme"),
                      ("fontsize", "ThemeFontSize")):
            r = self.row("persist.gammaos.nano.esde." + k)
            self.assertEqual((r["class"], r.get("es_de")), ("P", es))
            self.assertTrue(r["ini"].startswith("esde_theme."))
        self.assertEqual(self.row("persist.gammaos.nano.esde.aspectratio")["class"], "M")
        # Hotkey buttons, RGB light settings and slide actions: device only (Q5).
        for k in ("save", "load", "fastforward", "rewind", "quit", "buttonmap", "screen"):
            self.assertEqual(self.cls_of("persist.gammaos.hotkeys." + k), "D")
        for k in ("persist.gammaos.rgb.effect", "persist.gammaos.rgb.enable", "persist.gammaos.primary.rgb_hex_custom",
                  "persist.gammargb.control", "persist.gammaos.rotate.down_action", "persist.gammaos.rotate.up_action",
                  "persist.gammaos.rotate.launch_target"):
            self.assertEqual(self.cls_of(k), "D", k)
        # Scraper account and API keys: never on the card (Q6).
        for k in ("ssuser", "sspass", "tgdbkey", "ssdevid", "ssdevpw"):
            self.assertEqual(self.cls_of("persist.gammaos.scraper." + k), "S")
        # Language travels; time zone and automatic time stay (Q9).
        self.assertEqual(self.cls_of("persist.sys.locale"), "P")
        for k in ("persist.sys.timezone", "persist.gammaos.nano.tz_olson", "global:auto_time", "global:auto_time_zone"):
            self.assertEqual(self.cls_of(k), "D", k)
        # Kiosk Mode and the system name stay on the device (Q13).
        self.assertEqual(self.cls_of("persist.gammaos.nano.kiosk"), "D")
        self.assertEqual(self.cls_of("persist.gammaos.nano.system_name"), "D")
        # [device.<model>] holds only screen-dependent keys.
        for r in self.rows:
            if r["class"] == "M":
                self.assertTrue(r["ini"].startswith("device.*."), r["match"])

    def test_linux_only_keys_are_not_android_section(self):
        cfg = Upstream.load(ROOT)
        for key, u in self.found.items():
            if linux_only(u, cfg):
                self.assertNotEqual(self.cls_of(key), "A", "%s is named only by Linux code" % key)

    def test_scraper_keys_es_de_owns_stay_in_es_settings(self):
        for r in self.rows:
            if r["match"].startswith("persist.gammaos.scraper.") and r["class"] == "E" and not r["match"].endswith("."):
                self.assertTrue(r.get("es_de"), r["match"])

    def test_no_secret_key_is_portable(self):
        # Belt and braces for plan §7.2's safety tests: a key that looks like a credential is S.
        for key in self.found:
            last = re.split(r"[._:]", key.lower())[-1]
            if key.startswith("persist.") and re.search(r"(pass|password|pw|user|token|key|devid)\Z", last):
                c = self.cls_of(key)
                self.assertIn(c, ("S", "D", "R"), "%s looks like a credential but is class %s" % (key, c))


class Unit(unittest.TestCase):
    def scan(self, text):
        out = {}
        scan_text(text, "x.cpp", out)
        return out

    def test_property_calls(self):
        f = self.scan('''
            char b[92]; property_get("persist.gammaos.nano.a", b, "7");   // "persist.gammaos.nano.c"
            bool x = property_get_bool("persist.gammaos.nano.b", true);
            int y = android::base::GetIntProperty("persist.gammaos.nano.d", -1);
            property_set("persist.gammaos.nano.e", "1");
            /* property_set("persist.gammaos.nano.f", "1"); */
            const char* k = "persist.gammaos.nano." "g";
        ''')
        self.assertEqual(f["persist.gammaos.nano.a"].defaults, {"7"})
        self.assertEqual(f["persist.gammaos.nano.b"].defaults, {"true"})
        self.assertEqual(f["persist.gammaos.nano.d"].defaults, {"-1"})
        self.assertEqual(f["persist.gammaos.nano.e"].ops, {"set"})
        self.assertNotIn("persist.gammaos.nano.c", f)
        self.assertNotIn("persist.gammaos.nano.f", f)
        self.assertIn("persist.gammaos.nano.g", f)
        self.assertEqual(f["persist.gammaos.nano.a"].ops, {"get"})

    def test_settings_rows_and_shell(self):
        f = self.scan('''
            {"Screen Timeout", SettingSource::kSystem, "screen_off_timeout", "60000", "15000:15 s,-1:Never"},
            {"IPTV", SettingSource::kProp, "persist.gammaos.nano.iptv", "true", "false:Off,true:On"},
            b.toggle("air", "Airplane", SettingSource::kGlobal, "airplane_mode_on", "0");
            system("setprop persist.sys.locale en-US; settings put secure ui_night_mode 2");
            snprintf(k, sizeof k, "persist.gammaos.gamepad.pa%d_pkg", i);
        ''')
        self.assertEqual(f["system:screen_off_timeout"].options, ["15000:15 s,-1:Never"])
        self.assertEqual(f["persist.gammaos.nano.iptv"].ops, {"row"})
        self.assertEqual(f["persist.gammaos.nano.iptv"].defaults, {"true"})
        self.assertEqual(f["global:airplane_mode_on"].defaults, {"0"})
        self.assertEqual(f["persist.sys.locale"].ops, {"set"})
        self.assertEqual(f["secure:ui_night_mode"].ops, {"set"})
        self.assertIn("persist.gammaos.gamepad.pa%d_pkg", f)

    def test_check_table(self):
        found = {}
        for k, d in (("persist.a.x", "1"), ("persist.a.y", ""), ("sys.z", ""), ("persist.b", "on")):
            u = Use()
            u.ops.add("get")
            if d:
                u.defaults.add(d)
            found[k] = u
        rows = [
            {"match": "persist.a.x", "class": "P", "ini": "home.x", "type": "bool", "default": "0", "encoding": "1/0",
             "note": "n"},
            {"match": "persist.e", "class": "P", "ini": "home.e", "type": "bool", "default": "1", "note": "n",
             "source": "manual"},
            {"match": "system:g", "class": "P", "ini": "home.g", "type": "int", "default": "1", "note": "n",
             "source": "manual"},
            {"match": "persist.f", "class": "P", "ini": "home.f", "type": "bool", "default": "1",
             "encoding": "true/false", "note": "n", "source": "manual"},
            {"match": "persist.a.*", "class": "P", "note": "n"},
            {"match": "persist.b", "class": "M", "ini": "home.b", "type": "string", "default": "on", "note": "n"},
            {"match": "persist.old", "class": "D", "note": "n"},
            {"match": "persist.c", "class": "?", "note": "n", "source": "manual"},
            {"match": "persist.d", "class": "S", "ini": "x.d", "note": "n", "source": "manual"},
        ]
        problems = check_table({"rows": rows, "unclassified": []}, found)
        text = "\n".join(problems)
        self.assertIn("persist.a.x: default '0' is not one the code uses", text)
        self.assertIn("persist.a.*: a class P row must name one key", text)
        self.assertIn("persist.b: class M, but its ini key home.b is not in [device.*]", text)
        self.assertIn("no row for sys.z", text)
        self.assertIn("stale row persist.old", text)
        self.assertIn("persist.c: class ?, but not in the unclassified list", text)
        self.assertIn("persist.d: class S never goes on the card", text)
        self.assertIn("persist.e: a bool, but its device-store encoding is None", text)
        self.assertIn("persist.f: default '1' is not in its encoding true/false", text)
        self.assertIn("system:g: a settings-provider key on the card for Linux too, but no \"linux\"", text)
        self.assertNotIn("persist.a.y", text)        # covered by the glob

    def test_scripts(self):
        out = {}
        scan_script("# property:persist.gammaos.nano.c=1\n"
                    "on property:sys.a=1 && property:persist.gammaos.nano.a=\n"
                    "    setprop persist.gammaos.nano.a 1\n"
                    '[ "$(getprop persist.thimsy.b)" = 1 ] && exit 0\n'
                    "    setprop bluetooth.profile.x false\n", "x.rc", out)
        self.assertEqual(sorted(out), ["persist.gammaos.nano.a", "persist.thimsy.b", "sys.a"])
        self.assertEqual(out["persist.gammaos.nano.a"].ops, {"get", "set"})

    def test_runtime_families(self):
        ctx = Context()
        out = Found()
        scan_text('''
            struct Def { const char* suffix; float def; };
            const Def kA[] = { {"one.x", 1.0f}, {"one.y", 2.0f}, };
            static const char* kList = "persist.q.list";
            void f() { std::string k = std::string("persist.q.") + d.suffix; snprintf(k, 9, "%sact%d_code", p);
                       readList(kList); readList("persist.q.other"); readList(base); readList(kNope);
                       snprintf(k, 9, "%s_%d", base, i); snprintf(k, 9, "%sstray", p); }
            std::string pre() { return "persist.q.pa"; }
        ''', "a.cpp", out, ctx)
        fams = (
            {"name": "table", "file": "a.cpp", "prefix": "persist.q.", "array_type": "Def", "field": 0},
            {"name": "templ", "file": "a.cpp", "templates": "%sact", "prefixes": ("persist.q.", "persist.q.pa%d_")},
            {"name": "chunks", "file": "a.cpp", "templates": "%s_%d", "bases_from": ("readList",), "params": ("base",)},
            {"name": "gone", "file": "b.cpp", "templates": "%sz", "prefixes": ("persist.z.",)},
        )
        expand_runtime(out, ctx, fams)
        for k in ("persist.q.one.x", "persist.q.one.y", "persist.q.act%d_code", "persist.q.pa%d_act%d_code",
                  "persist.q.list_%d", "persist.q.other_%d"):
            self.assertIn("built", out[k].ops, k)
        text = "\n".join(out.problems)
        self.assertIn("cannot resolve kNope", text)
        self.assertIn("'%sstray' with a prefix chosen at run time", text)
        self.assertIn("'gone': b.cpp is not scanned", text)
        self.assertNotIn("persist.q.base_%d", out)
        # A key prefix with an exact row needs a family (or a glob).
        rows = [{"match": "persist.q.", "class": "D", "note": "n"}, {"match": "persist.r.", "class": "D", "note": "n"},
                {"match": "persist.s.*", "class": "D", "note": "n"}]
        found = Found()
        for k in ("persist.q.", "persist.r.", "persist.s."):
            found[k] = Use()
        found.family_prefixes.add("persist.q.")
        text = "\n".join(check_table({"rows": rows, "unclassified": []}, found))
        self.assertIn("persist.r. is a key prefix", text)
        self.assertNotIn("persist.q. is a key prefix", text)
        self.assertNotIn("persist.s. is a key prefix", text)

    def test_part_of(self):
        found = {}
        for k in ("persist.l", "persist.l_%d", "persist.m_%d", "persist.n_%d"):
            found[k] = Use()
        rows = [
            {"match": "persist.l", "class": "A", "ini": "android.l", "type": "list", "default": "", "note": "n"},
            {"match": "persist.l_%d", "class": "A", "part_of": "persist.l", "note": "n"},
            {"match": "persist.m_%d", "class": "D", "part_of": "persist.l", "note": "n"},
            {"match": "persist.n_%d", "class": "A", "part_of": "persist.gone", "ini": "android.n", "note": "n"},
        ]
        text = "\n".join(check_table({"rows": rows, "unclassified": []}, found))
        self.assertNotIn("persist.l_%d", text)
        self.assertIn("persist.m_%d: class D, but it is part of persist.l, class A", text)
        self.assertIn("persist.n_%d: part_of persist.gone, which has no row", text)
        self.assertIn("persist.n_%d: part of persist.gone, whose ini key covers it", text)

    def test_covering_row(self):
        rows = [{"match": "a.*"}, {"match": "a.b.*"}, {"match": "a.b.c"}]
        self.assertIs(covering_row("a.b.c", rows), rows[2])
        self.assertIs(covering_row("a.b.d", rows), rows[1])
        self.assertIs(covering_row("a.x", rows), rows[0])
        self.assertIsNone(covering_row("b.x", rows))


def _list() -> int:
    for line in report(scan(NANO), load_table(TABLE)["rows"]):
        print(line)
    return 0


def _update_globs() -> int:
    lines = glob_lines(scan(NANO), load_table(TABLE)["rows"])
    write_list(GLOBS, GLOBS_HEADER, lines)
    print("prefs-globs.txt: %d keys" % len(lines))
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--list"]:
        sys.exit(_list())
    if sys.argv[1:] == ["--update-globs"]:
        sys.exit(_update_globs())
    unittest.main()
