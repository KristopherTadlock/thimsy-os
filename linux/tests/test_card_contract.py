# SPDX-License-Identifier: MIT
"""The card contract (LIBRARY-FORMAT.md) against its schemas and test vectors.

linux/schema/*.schema.json describe the JSON files on a game card that
Android and Linux Thimsy share. They are checked with the draft-07 subset
validator thimsy-import already has (thimsy_import/devicejson.py), so a
keyword it does not know fails here instead of being skipped.

linux/tests/fixtures/card/ holds three cards:
  android-cube/    as the Thimsy Android build on the RG Cube leaves it
                   (Cube mount-order paths, Android-only labels);
  linux-written/   the same card after a Linux visit that follows the
                   contract (favourite, play, collection add, Linux's own
                   emulator set and picks, the theme, a playlist, a save
                   in the shared per-family folder, a folder a core
                   keeps itself below one, and a save state in the
                   shared per-core folder);
  multi-device/    several devices' emulator sets side by side (OA-46):
                   two Android Cubes, both sides of a dual-boot ARC-D, a
                   partial set, and a theme.json with a newer writer's key.
The vector tests check the contract's rules between the first two: Android's
files Linux must not rewrite stay byte-identical, labels and unknown tags
survive, Linux adds only files the contract lets it write, and nothing
Thimsy owns holds an absolute card path.

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
MULTI = CARDS / "multi-device"
ALL_CARDS = (ANDROID, LINUXW, MULTI)
DOC = REPO / "LIBRARY-FORMAT.md"

sys.path.insert(0, str(LINUX / "tools" / "thimsy-import"))
from thimsy_import import devicejson  # noqa: E402

# Where each schema's file lives on the card: (pattern, name the doc uses, schema).
SCHEMA_FOR = (
    (r"ES-DE/nano/systems\.json", "systems.json", "systems.schema.json"),
    (r"ES-DE/nano/emulators\.json", "emulators.json", "emulators.schema.json"),
    (r"ES-DE/nano/names\.json", "names.json", "names.schema.json"),
    (r"ES-DE/nano/index\.json", "index.json", "index.schema.json"),
    (r"ES-DE/nano/format\.json", "format.json", "format.schema.json"),
    (r"emulator_config/card\.json", "card.json", "card.schema.json"),
    (r"emulator_config/devices/[^/]+/device\.json", "devices/<id>/device.json", "emu-set.schema.json"),
    (r"emulator_config/devices/[^/]+/picks\.json", "devices/<id>/picks.json", "picks.schema.json"),
    (r"Thimsy/theme\.json", "Thimsy/theme.json", "theme.schema.json"),
)
CARD_SCHEMAS = sorted({s for _p, _n, s in SCHEMA_FOR})

LAUNCH_FIELDS = ("launchType", "coreSo", "package", "launchArgs", "intentTemplate")
ABSOLUTE_CARD = re.compile(r"/storage/|/mnt/media_rw/|/sdcard/|/data/media/0/")
CUBE_PREFIX = re.compile(r"/storage/(00000000-0000-0000-0000-[0-9A-Fa-f]{12})/")
SET_DIR = re.compile(r"emulator_config/devices/([^/]+)/")

# LIBRARY-FORMAT.md 4.26 (per_device_config_plan.md 2.4): the theme keys, by prefs.json's ini names.
THEME_KEYS = frozenset("""
    home.theme home.xmb home.carousel home.minima
    home.font home.wallpaper_effect home.wallpaper_dim
    xmb.theme xmb.color xmb.color_custom xmb.background xmb.font xmb.day_night xmb.wave xmb.wave_explicit
    dsi.dark dsi.effect dsi.tile_icon dsi.rom_title
    minima.background minima.long_names
    esde_theme.set esde_theme.variant esde_theme.color_scheme esde_theme.font_size
    esde.settings_theme esde.menu_color_scheme esde.menu_blur esde.transition esde.clock esde.variant_triggers
""".split())

# Shared saves and states (OA-53, LIBRARY-FORMAT.md 4.17 to 4.19): RetroArch's saves in
# saves/<family>/<system>/[<subdir>/] and states in states/<core>/<system>/[<subdir>/] (a family or
# core id, a system id, then {subdir}, the game's own folder below the system folder, empty for a
# game directly in it, so two games of one name in two subfolders keep two files), with the files
# and folders a core keeps itself below them (opera/per_game/, mame/, ...), and the standalone
# emulators' own shared folders. `below` is {subdir} plus any core-kept folders.
SHARED_SAVES = re.compile(
    r"^emulator_config/(?:retroarch/(?:saves|states)/(?P<core>[a-z0-9][a-z0-9_.-]*)/(?P<system>[a-z0-9]+)/"
    r"(?P<below>(?:[^/]+/)*)[^/]+$"
    r"|ppsspp/PSP/(?:SAVEDATA|PPSSPP_STATE)/"
    r"|scummvm/saves/)")

# What a Linux visit may add to a card (LIBRARY-FORMAT.md 3, 4.17 to 4.26).
LINUX_MAY_ADD = re.compile(
    r"^(?:ES-DE/nano/format\.json$"
    r"|emulator_config/card\.json$"
    r"|emulator_config/devices/linux-[0-9a-f]{16}/"
    r"|emulator_config/retroarch/(?:saves|states)/[^/]+/[^/]+/(?:[^/]+/)*[^/]+$"
    r"|emulator_config/ppsspp/PSP/(?:SAVEDATA|PPSSPP_STATE)/"
    r"|emulator_config/scummvm/saves/"
    r"|Thimsy/theme\.json$"
    r"|Thimsy/playlists/[^/]+\.m3u8$"
    r"|ES-DE/gamelists/[^/]+/gamelist\.xml$"
    r"|ES-DE/collections/custom-[^/]+\.cfg$"
    r"|ES-DE/downloaded_media/)")


def load(name):
    return json.loads((SCHEMA / name).read_text(encoding="utf-8"))


def card_json(card, rel):
    return json.loads((card / rel).read_text(encoding="utf-8"))


def schema_for(rel):
    for pattern, _name, schema in SCHEMA_FOR:
        if re.fullmatch(pattern, rel):
            return schema
    return None


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


def files_under(root):
    for dirpath, _dirs, names in os.walk(root):
        for n in names:
            p = Path(dirpath) / n
            yield p.relative_to(root).as_posix(), p


def set_dirs(card):
    base = card / "emulator_config" / "devices"
    return sorted(p for p in base.iterdir() if p.is_dir()) if base.is_dir() else []


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
        for _pattern, name, _schema in SCHEMA_FOR:
            self.assertIn(name, text)

    def test_retired_schemas_are_gone(self):
        """systems.linux.json is retired (LIBRARY-FORMAT.md 4.9)."""
        self.assertFalse((SCHEMA / "systems.linux.schema.json").exists())


class Fixtures(unittest.TestCase):
    def test_every_json_file_on_a_fixture_card_has_a_schema_and_passes(self):
        for card in ALL_CARDS:
            for rel, path in files_under(card):
                if not rel.endswith(".json"):
                    continue
                with self.subTest(card=card.name, file=rel):
                    schema = schema_for(rel)
                    self.assertIsNotNone(schema, "a JSON file the contract does not define")
                    doc = json.loads(path.read_text(encoding="utf-8"))
                    self.assertEqual(devicejson.validate(doc, load(schema)), [])

    def test_esde_xml_files_parse(self):
        for card in ALL_CARDS:
            for rel, path in files_under(card):
                if rel.endswith(".xml") and rel.startswith("ES-DE/"):
                    with self.subTest(card=card.name, file=rel):
                        esde_xml(path)

    def test_android_card_is_a_cube_card(self):
        """The vectors carry the Cube's mount-order id, the case Linux must resolve (D20, OA-19)."""
        names = (ANDROID / "ES-DE/nano/names.json").read_text(encoding="utf-8")
        self.assertRegex(names, CUBE_PREFIX)
        self.assertFalse((ANDROID / "emulator_config/card.json").exists(), "no shipped Android build writes card.json")
        self.assertEqual(set_dirs(ANDROID), [], "no shipped Android build writes a per-device set")


class Rejections(unittest.TestCase):
    """Each schema refuses what the contract forbids."""

    def bad(self, schema, doc, why):
        with self.subTest(schema=schema, why=why):
            self.assertTrue(devicejson.validate(doc, load(schema)), "accepted: " + why)

    def good(self, schema, doc, why):
        with self.subTest(schema=schema, why=why):
            self.assertEqual(devicejson.validate(doc, load(schema)), [], why)

    def changed(self, base, change):
        doc = json.loads(json.dumps(base))
        change(doc)
        return doc

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
            self.bad("card.schema.json", self.changed(base, change), why)

    def test_set_device_json(self):
        base = card_json(LINUXW, "emulator_config/devices/linux-9c1d0e7a42b35f86/device.json")
        for why, change in (
                ("a raw serial as the id", lambda d: d.update(id="linux-ABC123XYZ")),
                ("no platform in the id", lambda d: d.update(id="9c1d0e7a42b35f86")),
                ("an unknown platform", lambda d: d.update(platform="windows")),
                ("an upper-case hash", lambda d: d.update(id="linux-9C1D0E7A42B35F86")),
                ("the local-only 'changed' field", lambda d: d.update(changed=True)),
                ("the local-only 'migration' field", lambda d: d.update(migration="slots-pending")),
                ("the user's device name", lambda d: d.update(deviceName="My ARC")),
                ("an unknown id source", lambda d: d.update(source="mac")),
                ("version 2", lambda d: d.update(version=2)),
                ("no id", lambda d: d.pop("id"))):
            self.bad("emu-set.schema.json", self.changed(base, change), why)
        self.good("emu-set.schema.json", {"version": 1, "id": "android-0123456789abcdef", "platform": "android"},
                  "the minimum")

    def test_picks_json(self):
        base = card_json(LINUXW, "emulator_config/devices/linux-9c1d0e7a42b35f86/picks.json")
        for why, change in (
                ("a core path as a key", lambda d: d["systems"]["psx"].update(key="/usr/lib/libretro/pcsx_rearmed_libretro.so")),
                ("an Android package as a key", lambda d: d["systems"]["psx"].update(key="pkg:org.ppsspp.ppsspp")),
                ("an upper-case key", lambda d: d["systems"]["psx"].update(key="core:PCSX_ReARMed")),
                ("an unknown launch type", lambda d: d["systems"]["psx"].update(launchType="shell")),
                ("a './' game key", lambda d: d["games"].update({"./n64/x.z64": {"key": "core:x"}})),
                ("a game key with './' inside", lambda d: d["games"].update({"n64/./x.z64": {"key": "core:x"}})),
                ("a game key with no folder", lambda d: d["games"].update({"x.z64": {"key": "core:x"}})),
                ("an absolute game key", lambda d: d["games"].update({"/storage/9602-C755/ROMs/n64/x.z64": {}})),
                ("a top-level field this major does not define", lambda d: d.update(settings={})),
                ("version 2", lambda d: d.update(version=2))):
            self.bad("picks.schema.json", self.changed(base, change), why)
        for key in ("core:mupen64plus_next", "emu:ppsspp", "emu:m64plus-fz", "family:melonds", "family:psp.ppsspp"):
            self.good("picks.schema.json", {"version": 1, "systems": {"x": {"key": key}}}, key)
        self.good("picks.schema.json", self.changed(base, lambda d: d["systems"]["psx"].update(fromANewerWriter=1)),
                  "an unknown field in an entry is kept")

    def test_theme_json(self):
        base = card_json(LINUXW, "Thimsy/theme.json")
        for why, change in (
                ("a number as a value", lambda d: d["theme"].update(**{"home.theme": 3})),
                ("a boolean as a value", lambda d: d["theme"].update(**{"esde.clock": True})),
                ("an upper-case key", lambda d: d["theme"].update(**{"Home.theme": "3"})),
                ("a key with no dot", lambda d: d["theme"].update(theme="3")),
                ("a top-level field this major does not define", lambda d: d.update(settings={})),
                ("no theme", lambda d: d.pop("theme")),
                ("version 2", lambda d: d.update(version=2))):
            self.bad("theme.schema.json", self.changed(base, change), why)
        self.good("theme.schema.json", card_json(MULTI, "Thimsy/theme.json"), "a newer writer's key is kept")

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


class Sets(unittest.TestCase):
    """Per-device emulator sets (OA-46, LIBRARY-FORMAT.md 4.24)."""

    def test_a_complete_set_is_named_by_its_id(self):
        for card in ALL_CARDS:
            for d in set_dirs(card):
                label = d / "device.json"
                if not label.exists():
                    continue
                doc = json.loads(label.read_text(encoding="utf-8"))
                with self.subTest(card=card.name, set=d.name):
                    self.assertEqual(doc["id"], d.name, "the folder is the set's id")
                    self.assertTrue(doc["id"].startswith(doc["platform"] + "-"))
                    if doc.get("source") == "random":
                        self.assertNotIn("fp", doc, "a random id has no serial to check")

    def test_the_multi_device_vector_covers_the_cases(self):
        complete = [d for d in set_dirs(MULTI) if (d / "device.json").exists()]
        partial = [d for d in set_dirs(MULTI) if not (d / "device.json").exists()]
        platforms = {d.name.split("-")[0] for d in complete}
        self.assertEqual(platforms, {"android", "linux"})
        self.assertGreaterEqual(sum(d.name.startswith("android-") for d in complete), 2)
        self.assertTrue(partial, "a set whose copy did not finish has no device.json")
        models = [card_json(MULTI, "emulator_config/devices/%s/device.json" % d.name)["model"] for d in complete]
        self.assertTrue(any(models.count(m) > 1 for m in models), "two units, or two platforms, of one model")

    def test_a_set_holds_no_shared_data(self):
        """Saves, states, BIOS and caches stay out of every set (4.24)."""
        forbidden = re.compile(r"/(saves|states|system|cheats|Cheats|SAVEDATA|PPSSPP_STATE|TEXTURES|shaders|cache)/"
                               r"|\.(srm|state\d*)$")
        for card in ALL_CARDS:
            for rel, _p in files_under(card):
                if SET_DIR.match(rel):
                    with self.subTest(card=card.name, file=rel):
                        self.assertIsNone(forbidden.search(rel))


class Vectors(unittest.TestCase):
    """The rules between an Android-written card and the same card after a Linux visit."""

    def test_files_linux_only_reads_stay_byte_identical(self):
        # names.json and index.json: compatibility mode until the floor (interop Q12).
        # emulators.json: legacy; no build with this contract writes it (4.10).
        # es_settings.xml: ROMDirectory is written only when empty, and the theme mirror
        #   only when the chosen set is in the store's themes (4.1); this card has none.
        # ppsspp.ini: Android's live slot; Linux never reads or writes it (4.18).
        for rel in ("ES-DE/nano/names.json", "ES-DE/nano/index.json", "ES-DE/nano/emulators.json",
                    "ES-DE/settings/es_settings.xml", "emulator_config/ppsspp/PSP/SYSTEM/ppsspp.ini"):
            with self.subTest(file=rel):
                self.assertEqual((ANDROID / rel).read_bytes(), (LINUXW / rel).read_bytes())

    def test_linux_adds_only_what_the_contract_lets_it_write(self):
        before = {rel for rel, _p in files_under(ANDROID)}
        added = sorted(rel for rel, _p in files_under(LINUXW) if rel not in before)
        self.assertTrue(added)
        for rel in added:
            with self.subTest(file=rel):
                self.assertRegex(rel, LINUX_MAY_ADD)
        self.assertTrue(any(SET_DIR.match(r) for r in added), "the visit made Linux's own set")

    def test_no_retired_file_is_written(self):
        """OA-46, OA-47, OA-53: no shared settings, no settings.ini, no systems.linux.json, no linux/ folder."""
        for rel in ("Thimsy/settings.ini", "ES-DE/nano/systems.linux.json", "emulator_config/scummvm/scummvm.ini"):
            with self.subTest(file=rel):
                self.assertFalse((LINUXW / rel).exists())
        for card in ALL_CARDS:
            for rel, _p in files_under(card):
                with self.subTest(card=card.name, file=rel):
                    self.assertNotRegex(rel, r"^emulator_config/(?:[^/]+/)?linux/",
                                        "no per-platform folder (W7-INTEROP-11, -12, -15 withdrawn)")

    def test_saves_and_states_are_shared_per_core(self):
        """OA-53: Linux's saves and states go to the shared folders: RetroArch's saves per family and
        system, its states per core and system, none beside a ROM."""
        systems = {s["id"] for s in card_json(LINUXW, "ES-DE/nano/systems.json")["systems"]}
        before = {rel for rel, _p in files_under(ANDROID)}
        added = [rel for rel, _p in files_under(LINUXW) if rel not in before]
        shared = [rel for rel in added if SHARED_SAVES.match(rel)]
        self.assertTrue(any("/states/" in r for r in shared), "the visit left a state in retroarch/states/<core>/<system>/")
        self.assertTrue(any("/saves/" in r for r in shared), "the visit left a save in retroarch/saves/<family>/<system>/")
        for rel in added:
            if re.search(r"\.(srm|sav|state\d*|state\.auto)$", rel):
                with self.subTest(file=rel):
                    m = SHARED_SAVES.match(rel)
                    self.assertIsNotNone(m, "a save or state outside the shared folders")
                    self.assertFalse(rel.startswith("ROMs/"), "nothing beside a ROM")
                    if rel.startswith("emulator_config/retroarch/"):
                        self.assertTrue(m.group("core"), "one folder per family or core")
                        self.assertIn(m.group("system"), systems, "then one folder per system")

    def test_core_kept_folders_are_shared_storage(self):
        """4.17: files and folders a core keeps itself land below its family's folder, at any depth;
        a loose file in retroarch/saves/ or one without a system level is not where Linux writes."""
        for rel in ("emulator_config/retroarch/saves/opera/3do/opera/per_game/x",
                    "emulator_config/retroarch/saves/mame/arcade/mame/cfg/default.cfg",
                    "emulator_config/retroarch/saves/mesen/nes/Zelda (USA).srm"):
            with self.subTest(file=rel):
                self.assertRegex(rel, SHARED_SAVES)
                self.assertRegex(rel, LINUX_MAY_ADD)
        for rel in ("emulator_config/retroarch/saves/Zelda (USA).srm",
                    "emulator_config/retroarch/saves/mesen/Zelda (USA).srm",
                    "emulator_config/retroarch/states/Zelda (USA).state"):
            with self.subTest(file=rel):
                self.assertNotRegex(rel, LINUX_MAY_ADD)
        self.assertTrue((LINUXW / "emulator_config/retroarch/saves/opera/3do/opera/per_game/x").is_file(),
                        "the Linux-written card has a core-kept folder")

    def test_same_name_games_in_two_subfolders_keep_two_files(self):
        """4.17: {subdir} below the system folder, so ROMs/snes/USA/Game.sfc and ROMs/snes/Europe/Game.sfc
        write two saves and two state folders; a game directly in ROMs/snes/ has an empty {subdir}."""
        cases = {
            "emulator_config/retroarch/saves/snes9x/snes/USA/Game.srm": "USA/",
            "emulator_config/retroarch/saves/snes9x/snes/Europe/Game.srm": "Europe/",
            "emulator_config/retroarch/saves/snes9x/snes/Game.srm": "",
            "emulator_config/retroarch/states/snes9x/snes/USA/Game.state1": "USA/",
            "emulator_config/retroarch/states/snes9x/snes/Europe/Game.state1": "Europe/",
            "emulator_config/retroarch/saves/mesen/nes/hacks/Super Mario Bros. (Hack).srm": "hacks/",
        }
        for rel, below in cases.items():
            with self.subTest(file=rel):
                m = SHARED_SAVES.match(rel)
                self.assertIsNotNone(m)
                self.assertRegex(rel, LINUX_MAY_ADD)
                self.assertIn(m.group("system"), ("snes", "nes"), "the system level comes first")
                self.assertEqual(m.group("below"), below)
        self.assertEqual(len(set(cases)), len(cases))
        self.assertIn("{subdir}", DOC.read_text(encoding="utf-8"), "LIBRARY-FORMAT.md 4.17 names the placeholder")

    def test_launch_fields_are_kept_as_read(self):
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

    def test_nothing_thimsy_owns_holds_an_absolute_card_path(self):
        # Emulator settings files in a set are the emulator's own bytes and may (4.24).
        for card in (LINUXW, MULTI):
            owned = []
            for rel, _p in files_under(card):
                if rel in ("ES-DE/nano/format.json", "emulator_config/card.json") or rel.startswith("Thimsy/") \
                        or re.fullmatch(r"emulator_config/devices/[^/]+/(device|picks)\.json", rel):
                    owned.append(rel)
            self.assertTrue(any(r.endswith("/picks.json") for r in owned))
            for rel in owned:
                with self.subTest(card=card.name, file=rel):
                    self.assertIsNone(ABSOLUTE_CARD.search((card / rel).read_text(encoding="utf-8")))

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

    def test_linux_picks_sit_beside_the_legacy_ones(self):
        """Linux's pick for a game Android picked too is in Linux's set; Android's item is untouched."""
        picks = card_json(LINUXW, "emulator_config/devices/linux-9c1d0e7a42b35f86/picks.json")
        emus = {i["game"] for i in card_json(LINUXW, "ES-DE/nano/emulators.json")["items"]}
        self.assertTrue(set(picks.get("games", {})) & emus)

    def test_theme_json_holds_only_theme_keys(self):
        """What this build writes to theme.json is the 31 theme keys, never aspect ratio (4.26)."""
        keys = set(card_json(LINUXW, "Thimsy/theme.json")["theme"])
        self.assertTrue(keys)
        self.assertLessEqual(keys, THEME_KEYS)
        self.assertEqual(len(THEME_KEYS), 31)
        text = DOC.read_text(encoding="utf-8")
        for key in sorted(THEME_KEYS):
            with self.subTest(key=key):
                self.assertIn("`%s`" % key, text, "LIBRARY-FORMAT.md 4.26 lists it")

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
