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
                   picks, a PPSSPP setting, settings.ini, a playlist).
The vector tests check the contract's rules between the two: Android's
files Linux must not rewrite stay byte-identical, emulator picks follow
OA-38 (the last pick wins, in the shared files, with ES-DE's labels; what
Linux did not pick is kept), unknown tags survive, and nothing Linux owns
holds an absolute card path.

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
# ES-DE's commands per system (its android table), as Nano has them: the labels both platforms write.
ESDE_COMMANDS = REPO / "nano" / "NanoEsdeEmulators.inc"

sys.path.insert(0, str(LINUX / "tools" / "thimsy-import"))
from thimsy_import import devicejson  # noqa: E402

# Where each schema's file lives on the card.
SCHEMA_FOR = {
    "ES-DE/nano/systems.json": "systems.schema.json",
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

# What the Linux visit in linux-written/ picked (OA-38): Spyro and Mario Kart 64 per game, PlayStation
# per system. Everything else on the card is what Android picked, and stays.
LINUX_GAME_PICKS = {("psx", "./Spyro the Dragon (USA).chd"), ("n64", "./Mario Kart 64 (USA).z64")}
LINUX_SYSTEM_PICKS = {"psx"}


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


def esde_commands():
    """{system: {label: [keys]}} from kEsdeCommands in NanoEsdeEmulators.inc (generated from ES-DE)."""
    text = ESDE_COMMANDS.read_text(encoding="utf-8")
    body = text[text.index("kEsdeCommands[] = {"):]
    body = body[:body.index("\n};")]
    cstr = r'"((?:[^"\\]|\\.)*)"'
    row = re.compile(r"^\s*\{%s, %s, %s, %s\},\s*$" % (cstr, cstr, cstr, cstr), re.M)
    out = {}
    for m in row.finditer(body):
        out.setdefault(m.group(1), {})[m.group(2)] = m.group(4).split()
    return out


def launch_key(system):
    """The emulator a systems.json entry's launch fields start, as ES-DE's table keys it."""
    if system.get("launchType") == "libretro-core":
        so = system.get("coreSo", "")
        suffix = "_libretro_android.so"
        return "core:" + so[:-len(suffix)] if so.endswith(suffix) else ""
    return "pkg:" + system["package"] if system.get("package") else ""


def gamelists(card):
    """{folder: parsed gamelist} for every gamelist on the card."""
    root = card / "ES-DE" / "gamelists"
    return {p.parent.name: esde_xml(p) for p in sorted(root.glob("*/gamelist.xml"))}


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

    def test_emulators_json_keys_and_games(self):
        base = card_json(ANDROID, "ES-DE/nano/emulators.json")
        for why, change in (
                ("a core path as a key", lambda d: d["items"][0].update(key="/usr/lib/libretro/pcsx_rearmed_libretro.so")),
                ("an Android package as a key", lambda d: d["items"][0].update(key="pkg:org.ppsspp.ppsspp")),
                ("upper case", lambda d: d["items"][0].update(key="core:PCSX_ReARMed")),
                ("a './' game key", lambda d: d["items"][0].update(game="./n64/x.z64")),
                ("a game key with './' inside", lambda d: d["items"][0].update(game="n64/./x.z64")),
                ("a game key with no folder", lambda d: d["items"][0].update(game="x.z64")),
                ("an empty am", lambda d: d["items"][0].update(am="")),
                ("no items", lambda d: d.pop("items"))):
            doc = json.loads(json.dumps(base))
            change(doc)
            self.bad("emulators.schema.json", doc, why)
        for key in ("core:mupen64plus_next", "emu:ppsspp", "emu:m64plus-fz", "family:melonds", "family:psp.ppsspp"):
            doc = json.loads(json.dumps(base))
            doc["items"][0]["key"] = key
            self.good("emulators.schema.json", doc, key)

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


class Retired(unittest.TestCase):
    """OA-38 retired systems.linux.json: Linux's picks go in the shared files."""

    def test_no_card_and_no_schema_has_it(self):
        self.assertFalse((SCHEMA / "systems.linux.schema.json").exists())
        for card in (ANDROID, LINUXW):
            self.assertFalse((card / "ES-DE/nano/systems.linux.json").exists(), card.name)

    def test_the_doc_says_so(self):
        text = DOC.read_text(encoding="utf-8")
        self.assertIn("OA-38", text)
        self.assertRegex(text, r"`systems\.linux\.json`[^\n]*retired")


class Vectors(unittest.TestCase):
    """The rules between an Android-written card and the same card after a Linux visit."""

    def test_files_linux_only_reads_stay_byte_identical(self):
        # names.json and index.json: compatibility mode until the floor (interop Q12).
        # es_settings.xml: ROMDirectory is written only when empty.
        for rel in ("ES-DE/nano/names.json", "ES-DE/nano/index.json", "ES-DE/settings/es_settings.xml"):
            with self.subTest(file=rel):
                self.assertEqual((ANDROID / rel).read_bytes(), (LINUXW / rel).read_bytes())

    def test_launch_fields_change_only_where_linux_picked(self):
        """OA-38: a system's launch fields are its last pick. One Linux could not run (n64's app) is kept."""
        a = {s["id"]: s for s in card_json(ANDROID, "ES-DE/nano/systems.json")["systems"]}
        l = {s["id"]: s for s in card_json(LINUXW, "ES-DE/nano/systems.json")["systems"]}
        self.assertEqual(set(a), set(l), "Linux keeps every system, used or not")
        self.assertEqual(a["n64"]["launchType"], "custom-package", "fixture: a pick Linux cannot run")
        for sid in a:
            changed = [f for f in LAUNCH_FIELDS if a[sid].get(f) != l[sid].get(f)]
            with self.subTest(system=sid):
                if sid in LINUX_SYSTEM_PICKS:
                    self.assertTrue(changed, "fixture: Linux picked this system's emulator")
                else:
                    self.assertEqual(changed, [], "kept as the last pick left it (a fallback is never written)")

    def test_a_system_pick_carries_its_es_de_label(self):
        cmds = esde_commands()
        l = {s["id"]: s for s in card_json(LINUXW, "ES-DE/nano/systems.json")["systems"]}
        lists = gamelists(LINUXW)
        for sid in LINUX_SYSTEM_PICKS:
            folder = l[sid]["romDir"]
            labels = [e.findtext("label") for e in lists[folder].iter("alternativeEmulator")]
            with self.subTest(system=sid):
                self.assertEqual(len(labels), 1)
                self.assertIn(launch_key(l[sid]), cmds[folder][labels[0]],
                              "the gamelist's label names the emulator systems.json now starts")

    def test_every_label_is_one_es_de_knows(self):
        """Both platforms write ES-DE's own labels, so the ES-DE app reads either side's picks."""
        cmds = esde_commands()
        self.assertIn("PCSX ReARMed", cmds["psx"])
        for card in (ANDROID, LINUXW):
            for folder, gl in gamelists(card).items():
                labels = [e.findtext("label") for e in gl.iter("alternativeEmulator")]
                labels += [g.findtext("altemulator") for g in gl.iter("game") if g.findtext("altemulator")]
                for label in labels:
                    with self.subTest(card=card.name, folder=folder, label=label):
                        self.assertIn(label, cmds.get(folder, {}))

    def test_labels_linux_did_not_pick_and_unknown_tags_survive(self):
        al, ll = gamelists(ANDROID), gamelists(LINUXW)
        self.assertLessEqual(set(al), set(ll), "no gamelist is lost")
        for folder, a in al.items():
            l = ll[folder]
            if folder not in LINUX_SYSTEM_PICKS:  # system ids are their folders here
                self.assertEqual([e.findtext("label") for e in a.iter("alternativeEmulator")],
                                 [e.findtext("label") for e in l.iter("alternativeEmulator")])
            lgames = {g.findtext("path"): g for g in l.iter("game")}
            for g in a.iter("game"):
                path = g.findtext("path")
                with self.subTest(folder=folder, game=path):
                    self.assertIn(path, lgames)
                    self.assertTrue(path.startswith("./"), "gamelist paths are relative")
                    for child in g:
                        if child.tag == "altemulator" and (folder, path) in LINUX_GAME_PICKS:
                            continue
                        if child.tag in ("altemulator", "mainlineOnlyTag", "hidden", "favorite", "name"):
                            self.assertEqual(lgames[path].findtext(child.tag), child.text, child.tag)

    def test_linux_game_picks_are_in_the_shared_files(self):
        """A Linux pick ES-DE has a label for is the game's <altemulator>, and its emulators.json item goes."""
        cmds = esde_commands()
        ll = gamelists(LINUXW)
        items = {i["game"] for i in card_json(LINUXW, "ES-DE/nano/emulators.json")["items"]}
        for folder, path in sorted(LINUX_GAME_PICKS):
            with self.subTest(folder=folder, game=path):
                game = {g.findtext("path"): g for g in ll[folder].iter("game")}[path]
                self.assertIn(game.findtext("altemulator"), cmds[folder])
                self.assertNotIn(folder + "/" + path[2:], items, "last pick wins: the older pick's item goes")

    def test_emulators_json_keeps_what_linux_did_not_pick(self):
        a = card_json(ANDROID, "ES-DE/nano/emulators.json")
        l = card_json(LINUXW, "ES-DE/nano/emulators.json")
        picked = {f + "/" + p[2:] for f, p in LINUX_GAME_PICKS}
        kept = [i for i in a["items"] if i["game"] not in picked]
        self.assertTrue(kept, "fixture: an Android pick Linux leaves alone")
        self.assertEqual([i for i in l["items"] if i["game"] not in picked], kept,
                         "other games' items survive whole, in order")
        for k in a:
            if k != "items":
                self.assertEqual(l.get(k), a[k], "top-level keys survive: " + k)

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
        owned = ["ES-DE/nano/format.json", "emulator_config/card.json"]
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
