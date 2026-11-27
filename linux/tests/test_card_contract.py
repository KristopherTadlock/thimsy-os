# SPDX-License-Identifier: MIT
"""The card contract (LIBRARY-FORMAT.md) against its schemas and test vectors.

linux/schema/*.schema.json describe the JSON files on a game card that
Android and Linux Thimsy share. They are checked with the draft-07 subset
validator thimsy-import already has (thimsy_import/devicejson.py), so a
keyword it does not know fails here instead of being skipped.

linux/tests/fixtures/card/ holds two copies of one card:
  android-cube/    as the Thimsy Android build on the RG Cube leaves it
                   (Cube mount-order paths, Android-only labels);
  linux-written/   the same card after a Linux visit that follows the
                   contract (favourite, play, collection add, emulator
                   choices, a PPSSPP setting, settings.ini, a playlist).
The vector tests check the contract's rules between the two: Android's
files Linux must not rewrite stay byte-identical, labels and unknown tags
survive, and nothing Linux owns holds an absolute card path.

Standard library only. Nothing here builds or runs a program.

    py -3.12 -B -I -m unittest discover -s tests -t . -p test_card_contract.py   (from linux/)
"""
from __future__ import annotations

import json
import os
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
REPO = LINUX.parent
SCHEMA = LINUX / "schema"
CARDS = HERE / "fixtures" / "card"
ANDROID = CARDS / "android-cube"
LINUXW = CARDS / "linux-written"
DOC = REPO / "LIBRARY-FORMAT.md"

sys.path.insert(0, str(LINUX / "tools" / "thimsy-import"))
from thimsy_import import devicejson  # noqa: E402

# Where each schema's file lives on the card.
SCHEMA_FOR = {
    "ES-DE/nano/systems.json": "systems.schema.json",
    "ES-DE/nano/systems.linux.json": "systems.linux.schema.json",
    "ES-DE/nano/emulators.json": "emulators.schema.json",
    "ES-DE/nano/names.json": "names.schema.json",
    "ES-DE/nano/index.json": "index.schema.json",
    "ES-DE/nano/format.json": "format.schema.json",
    "emulator_config/card.json": "card.schema.json",
}
CARD_SCHEMAS = sorted(set(SCHEMA_FOR.values()))

LAUNCH_FIELDS = ("launchType", "coreSo", "package", "launchArgs", "intentTemplate")
ABSOLUTE_CARD = re.compile(r"/storage/|/mnt/media_rw/|/sdcard/|/data/media/0/")
CUBE_PREFIX = re.compile(r"/storage/(00000000-0000-0000-0000-[0-9A-Fa-f]{12})/")


def load(name):
    return json.loads((SCHEMA / name).read_text(encoding="utf-8"))


def card_json(card, rel):
    return json.loads((card / rel).read_text(encoding="utf-8"))


def schema_nodes(node, where="#"):
    """Every subschema position: the keywords at each must all be ones the validator knows."""
    yield where, node
    for key in ("properties", "patternProperties", "definitions"):
        for name, sub in node.get(key, {}).items():
            yield from schema_nodes(sub, "%s/%s/%s" % (where, key, name))
    for key in ("items", "propertyNames", "additionalProperties"):
        if isinstance(node.get(key), dict):
            yield from schema_nodes(node[key], "%s/%s" % (where, key))
    for i, sub in enumerate(node.get("anyOf", [])):
        yield from schema_nodes(sub, "%s/anyOf/%d" % (where, i))


def esde_xml(path):
    """ES-DE files have several top-level elements; parse them under one root."""
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"^<\?xml[^>]*\?>", "", text.lstrip("﻿"))
    return ET.fromstring("<root>%s</root>" % text)


def ini(path):
    """{(section, key): value} for an INI file with 'key = value' lines (';' and '#' comments)."""
    out, section = {}, ""
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s[0] in "#;":
            continue
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
            continue
        k, _, v = s.partition("=")
        out[(section, k.strip())] = v.strip()
    return out


def files_under(root):
    for dirpath, _dirs, names in os.walk(root):
        for n in names:
            p = Path(dirpath) / n
            yield p.relative_to(root).as_posix(), p


class Schemas(unittest.TestCase):
    def test_every_card_schema_exists_and_parses(self):
        for name in CARD_SCHEMAS:
            with self.subTest(schema=name):
                s = load(name)
                self.assertEqual(s["$schema"], "http://json-schema.org/draft-07/schema#")
                self.assertTrue(s["$id"].endswith("/" + name), s["$id"])

    def test_only_keywords_the_validator_knows(self):
        known = devicejson._KNOWN
        for name in CARD_SCHEMAS:
            for where, node in schema_nodes(load(name)):
                with self.subTest(schema=name, at=where):
                    self.assertEqual(set(node) - known, set())

    def test_shared_definitions_agree(self):
        seen = {}
        for name in CARD_SCHEMAS:
            for d, body in load(name).get("definitions", {}).items():
                body = {k: v for k, v in body.items() if k != "description"}
                if d in seen:
                    self.assertEqual(body, seen[d][1], "%s differs between %s and %s" % (d, seen[d][0], name))
                else:
                    seen[d] = (name, body)

    def test_doc_names_every_schema(self):
        text = DOC.read_text(encoding="utf-8")
        for name in CARD_SCHEMAS:
            self.assertIn("linux/schema/" + name, text)
        for rel in SCHEMA_FOR:
            self.assertIn(rel.split("/")[-1], text)


class Fixtures(unittest.TestCase):
    def test_every_json_file_on_a_fixture_card_has_a_schema_and_passes(self):
        for card in (ANDROID, LINUXW):
            for rel, path in files_under(card):
                if not rel.endswith(".json"):
                    continue
                with self.subTest(card=card.name, file=rel):
                    self.assertIn(rel, SCHEMA_FOR, "a JSON file the contract does not define")
                    doc = json.loads(path.read_text(encoding="utf-8"))
                    self.assertEqual(devicejson.validate(doc, load(SCHEMA_FOR[rel])), [])

    def test_esde_xml_files_parse(self):
        for card in (ANDROID, LINUXW):
            for rel, path in files_under(card):
                if rel.endswith(".xml"):
                    with self.subTest(card=card.name, file=rel):
                        esde_xml(path)

    def test_android_card_is_a_cube_card(self):
        """The vectors carry the Cube's mount-order id, the case Linux must resolve (D20, OA-19)."""
        names = (ANDROID / "ES-DE/nano/names.json").read_text(encoding="utf-8")
        self.assertRegex(names, CUBE_PREFIX)
        self.assertFalse((ANDROID / "emulator_config/card.json").exists(), "no shipped Android build writes card.json")


class Rejections(unittest.TestCase):
    """Each schema refuses what the contract forbids."""

    def bad(self, schema, doc, why):
        with self.subTest(schema=schema, why=why):
            self.assertTrue(devicejson.validate(doc, load(schema)), "accepted: " + why)

    def good(self, schema, doc, why):
        with self.subTest(schema=schema, why=why):
            self.assertEqual(devicejson.validate(doc, load(schema)), [], why)

    def test_card_json(self):
        base = card_json(LINUXW, "emulator_config/card.json")
        for why, change in (
                ("a path as an alias id", lambda d: d["aliases"][0].update(id="/storage/9602-C755")),
                ("a lower-case serial", lambda d: d["aliases"][0].update(id="9602-c755")),
                ("an unknown alias kind", lambda d: d["aliases"][0].update(kind="label")),
                ("an absolute copy path", lambda d: d["copies"][0].update(to="/storage/9602-C755/x.bin")),
                ("a '..' copy path", lambda d: d["copies"][0].update(**{"from": "flycast/../../x"})),
                ("a token in a copy path", lambda d: d["copies"][0].update(to="%CARDID%/x")),
                ("version 2", lambda d: d.update(version=2)),
                ("no aliases", lambda d: d.pop("aliases")),
                ("a field this major does not define", lambda d: d.update(extra=1))):
            doc = json.loads(json.dumps(base))
            change(doc)
            self.bad("card.schema.json", doc, why)

    def test_systems_linux_json(self):
        base = card_json(LINUXW, "ES-DE/nano/systems.linux.json")
        for why, change in (
                ("a core path", lambda d: d["systems"]["psx"].update(key="/usr/lib/libretro/pcsx_rearmed_libretro.so")),
                ("an Android package", lambda d: d["systems"]["psx"].update(key="pkg:org.ppsspp.ppsspp")),
                ("upper case", lambda d: d["systems"]["psx"].update(key="core:PCSX_ReARMed")),
                ("a launch command", lambda d: d["systems"]["psx"].update(argv=["retroarch"])),
                ("a './' game key", lambda d: d["games"].update({"./n64/x.z64": {"key": "core:x"}})),
                ("a game key with './' inside", lambda d: d["games"].update({"n64/./x.z64": {"key": "core:x"}})),
                ("a game key with no folder", lambda d: d["games"].update({"x.z64": {"key": "core:x"}}))):
            doc = json.loads(json.dumps(base))
            change(doc)
            self.bad("systems.linux.schema.json", doc, why)
        for key in ("core:mupen64plus_next", "emu:ppsspp", "emu:m64plus-fz", "family:melonds", "family:psp.ppsspp"):
            self.good("systems.linux.schema.json", {"version": 1, "systems": {"x": {"key": key}}}, key)

    def test_format_json(self):
        self.good("format.schema.json", {"version": 1}, "major 1")
        self.bad("format.schema.json", {"version": 2}, "a major nobody implements")
        self.bad("format.schema.json", {}, "no version")

    def test_systems_json_never_holds_scraper_accounts(self):
        base = card_json(ANDROID, "ES-DE/nano/systems.json")
        for field in ("user", "pass", "key"):
            doc = json.loads(json.dumps(base))
            doc["systems"][1]["scraper"][field] = "secret"
            self.bad("systems.schema.json", doc, "scraper " + field)

    def test_names_and_index_keep_the_forms_shipped_builds_read(self):
        """A relative key never stats, so every shipped build would prune it (check.md B1, B2)."""
        names = card_json(ANDROID, "ES-DE/nano/names.json")
        names["items"][0]["rom"] = "psx/Crash Bandicoot (USA).chd"
        self.bad("names.schema.json", names, "a relative rom key")
        index = card_json(ANDROID, "ES-DE/nano/index.json")
        index["items"][0]["box"] = "psx/covers/Crash Bandicoot (USA).png"
        self.bad("index.schema.json", index, "a relative box path")

    def test_android_owned_files_keep_unknown_fields(self):
        emus = card_json(ANDROID, "ES-DE/nano/emulators.json")
        emus["fromANewerBuild"] = {"x": 1}
        emus["items"][0]["key"] = "emu:m64plus-fz"
        emus["items"][0]["nested"] = {"unknown": [1, 2]}
        self.good("emulators.schema.json", emus, "unknown fields and a key")
        emus["items"][0]["key"] = "M64Plus FZ"
        self.bad("emulators.schema.json", emus, "a label as a key")
        systems = card_json(ANDROID, "ES-DE/nano/systems.json")
        systems["systems"][0]["fromANewerBuild"] = True
        self.good("systems.schema.json", systems, "an unknown system field")


class Vectors(unittest.TestCase):
    """The rules between an Android-written card and the same card after a Linux visit."""

    def test_files_linux_only_reads_stay_byte_identical(self):
        # names.json and index.json: compatibility mode until the floor (interop Q12).
        # emulators.json: Android's; Linux keeps its own per-game choices in systems.linux.json.
        # es_settings.xml: ROMDirectory is written only when empty.
        for rel in ("ES-DE/nano/names.json", "ES-DE/nano/index.json", "ES-DE/nano/emulators.json",
                    "ES-DE/settings/es_settings.xml"):
            with self.subTest(file=rel):
                self.assertEqual((ANDROID / rel).read_bytes(), (LINUXW / rel).read_bytes())

    def test_launch_fields_are_androids(self):
        a = {s["id"]: s for s in card_json(ANDROID, "ES-DE/nano/systems.json")["systems"]}
        l = {s["id"]: s for s in card_json(LINUXW, "ES-DE/nano/systems.json")["systems"]}
        self.assertEqual(set(a), set(l), "Linux keeps every system, used or not")
        for sid in a:
            for f in LAUNCH_FIELDS:
                with self.subTest(system=sid, field=f):
                    self.assertEqual(a[sid].get(f), l[sid].get(f))

    def test_labels_and_unknown_tags_survive(self):
        a, l = (esde_xml(c / "ES-DE/gamelists/psx/gamelist.xml") for c in (ANDROID, LINUXW))
        self.assertEqual([e.findtext("label") for e in a.iter("alternativeEmulator")],
                         [e.findtext("label") for e in l.iter("alternativeEmulator")])
        lgames = {g.findtext("path"): g for g in l.iter("game")}
        for g in a.iter("game"):
            path = g.findtext("path")
            with self.subTest(game=path):
                self.assertIn(path, lgames)
                self.assertTrue(path.startswith("./"), "gamelist paths are relative")
                for child in g:
                    if child.tag in ("altemulator", "mainlineOnlyTag", "hidden", "favorite", "name"):
                        self.assertEqual(lgames[path].findtext(child.tag), child.text, child.tag)

    def test_play_stats_only_move_forward(self):
        a, l = (esde_xml(c / "ES-DE/gamelists/psx/gamelist.xml") for c in (ANDROID, LINUXW))
        lgames = {g.findtext("path"): g for g in l.iter("game")}
        for g in a.iter("game"):
            lg = lgames[g.findtext("path")]
            if g.findtext("playcount"):
                self.assertGreaterEqual(int(lg.findtext("playcount")), int(g.findtext("playcount")))
            if g.findtext("lastplayed"):
                self.assertGreaterEqual(lg.findtext("lastplayed"), g.findtext("lastplayed"))

    def test_collections_keep_every_line(self):
        rel = "ES-DE/collections/custom-road trip.cfg"
        a = (ANDROID / rel).read_text(encoding="utf-8").splitlines()
        l = (LINUXW / rel).read_text(encoding="utf-8").splitlines()
        self.assertEqual(l[:len(a)], a, "unresolved and absolute lines are kept verbatim")
        for line in l[len(a):]:
            self.assertTrue(line.startswith("%ROMPATH%/"), "a new line under ROMDirectory uses %ROMPATH%: " + line)

    def test_nothing_linux_owns_holds_an_absolute_card_path(self):
        owned = ["ES-DE/nano/systems.linux.json", "ES-DE/nano/format.json", "emulator_config/card.json"]
        for rel, _p in files_under(LINUXW):
            if rel.startswith("Thimsy/") or re.match(r"emulator_config/[^/]+/linux/", rel):
                owned.append(rel)
        self.assertTrue(any("/linux/" in r for r in owned))
        for rel in owned:
            with self.subTest(file=rel):
                self.assertIsNone(ABSOLUTE_CARD.search((LINUXW / rel).read_text(encoding="utf-8")))

    def test_card_json_rules(self):
        doc = card_json(LINUXW, "emulator_config/card.json")
        ids = [a["id"] for a in doc["aliases"]]
        self.assertEqual(len(ids), len(set(ids)), "one entry per id")
        writers = [(w["device"], w["platform"]) for w in doc.get("writers", [])]
        self.assertEqual(len(writers), len(set(writers)), "one entry per device model and platform")
        # Every Cube prefix in the Android-written files is listed (D20(b)).
        found = set()
        for _rel, p in files_under(ANDROID):
            found |= set(CUBE_PREFIX.findall(p.read_text(encoding="utf-8")))
        self.assertTrue(found)
        self.assertLessEqual(found, set(ids))

    def test_systems_linux_game_keys_name_real_games(self):
        doc = card_json(LINUXW, "ES-DE/nano/systems.linux.json")
        emus = {i["game"] for i in card_json(LINUXW, "ES-DE/nano/emulators.json")["items"]}
        for game in doc.get("games", {}):
            self.assertIn(game, emus, "fixture: the Linux choice sits beside Android's for the same game")

    # Example key classes for the PPSSPP fixture only; the full list is later work (OA-26).
    PPSSPP_PLATFORM_KEYS = {("General", "CurrentDirectory"), ("Graphics", "GraphicsBackend")}

    def test_ppsspp_layers(self):
        rel = "emulator_config/ppsspp/PSP/SYSTEM/ppsspp.ini"
        a, l = ini(ANDROID / rel), ini(LINUXW / rel)
        self.assertEqual(set(a), set(l), "the shared file keeps every key, known or not")
        for k in self.PPSSPP_PLATFORM_KEYS:
            self.assertEqual(a[k], l[k], "Android's per-platform value stays in the shared file: %s" % (k,))
        override = ini(LINUXW / "emulator_config/ppsspp/linux/PSP/SYSTEM/ppsspp.ini")
        self.assertTrue(override)
        self.assertLessEqual(set(override), self.PPSSPP_PLATFORM_KEYS, "the override holds per-platform keys only")

    # OA-32 Q5 and Q6 and its autonomous answers: never on the card.
    DEVICE_ONLY = re.compile(r"(^|\.)(ssh|password|passwd|api_key|kiosk|timezone|auto_time|system_name|rgb|led|slide)"
                             r"(\.|_|$)|^scraper\.(user|pass|key)$|^hotkeys\..*button")

    def test_settings_ini_dialect(self):
        path = LINUXW / "Thimsy/settings.ini"
        raw = path.read_bytes()
        raw.decode("utf-8")
        self.assertLessEqual(len(raw), 1 << 20)
        section, keys = None, {}
        for n, line in enumerate(raw.decode("utf-8").splitlines(), 1):
            self.assertLessEqual(len(line.encode("utf-8")), 4096)
            s = line.strip()
            if not s or s[0] in "#;":
                continue
            m = re.fullmatch(r"\[([a-z0-9_.-]+)\]", s)
            if m:
                section = m.group(1)
                continue
            m = re.fullmatch(r"([a-z0-9_.]+)\s*=\s*(.*)", s)
            self.assertIsNotNone(m, "line %d is not 'key = value': %r" % (n, line))
            self.assertIsNotNone(section, "line %d is outside a section" % n)
            keys[(section, m.group(1))] = m.group(2)
            full = m.group(1) if section in ("meta", "android", "linux") or section.startswith("device.") \
                else section + "." + m.group(1)
            self.assertIsNone(self.DEVICE_ONLY.search(full), "a device-only key on the card: " + full)
        self.assertEqual(keys[("meta", "format")], "1")
        self.assertTrue(raw.decode("utf-8").rstrip().splitlines()[-1].startswith("#"), "Nano's trailer comment")

    def test_playlists_are_relative_m3u8(self):
        pl = LINUXW / "Thimsy/playlists"
        lists = sorted(pl.glob("*.m3u8"))
        self.assertTrue(lists)
        for p in lists:
            lines = p.read_text(encoding="utf-8").splitlines()
            with self.subTest(playlist=p.name):
                self.assertEqual(lines[0], "#EXTM3U")
                entries = [x for x in lines if x and not x.startswith("#")]
                self.assertTrue(entries)
                for e in entries:
                    self.assertNotRegex(e, r"^(/|[A-Za-z]:|\\|%|[a-z]+://)", "relative to the playlist file")
                    self.assertNotIn("\\", e)
                    target = os.path.normpath(os.path.join(str(p.parent), e))
                    self.assertTrue(target.startswith(str(LINUXW)), "stays on the card: " + e)


if __name__ == "__main__":
    unittest.main()
