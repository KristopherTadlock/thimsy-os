"""InputPlumber device and capability YAML, read for pad facts; no YAML is copied.

The files are devices/<FAMILY>/filesystem/usr/share/inputplumber/{devices,capability_maps}/
*.yaml. A device file matches a device-tree model and lists source nodes (evdev name and
phys path) with a capability map id; a capability map lists source event -> target pairs.
The facts written from them are Thimsy's own rows: source match rules, and each source code
mapped to the kernel gamepad code (Documentation/input/gamepad.rst) of the target.

The YAML reader handles the block subset these files use: nested mappings, block sequences
("- key: value" and "- scalar"), plain and quoted scalars, comments, and [] / {}. Anchors,
tags, flow collections with content and multi-line scalars are refused.
"""

from __future__ import annotations

import json
import re
from collections import OrderedDict

from .rocknix import ImportError_

IP_DIR = "filesystem/usr/share/inputplumber"

# InputPlumber gamepad targets -> kernel gamepad codes
GAMEPAD = {
    "South": "BTN_SOUTH", "East": "BTN_EAST", "North": "BTN_NORTH", "West": "BTN_WEST",
    "Start": "BTN_START", "Select": "BTN_SELECT", "Guide": "BTN_MODE",
    "LeftBumper": "BTN_TL", "RightBumper": "BTN_TR",
    "LeftTrigger": "BTN_TL2", "RightTrigger": "BTN_TR2",
    "LeftStick": "BTN_THUMBL", "RightStick": "BTN_THUMBR",
    "DPadUp": "BTN_DPAD_UP", "DPadDown": "BTN_DPAD_DOWN",
    "DPadLeft": "BTN_DPAD_LEFT", "DPadRight": "BTN_DPAD_RIGHT",
}
# InputPlumber keyboard targets look like KeyF1, KeyVolumeUp
_KEY = re.compile(r"^Key([A-Za-z0-9]+)$")

# Kernel aliases (include/uapi/linux/input-event-codes.h): one canonical name each
ALIASES = {"BTN_A": "BTN_SOUTH", "BTN_B": "BTN_EAST", "BTN_X": "BTN_NORTH", "BTN_Y": "BTN_WEST",
           "BTN_GAMEPAD": "BTN_SOUTH"}


def canonical(code: str) -> str:
    return ALIASES.get(code, code)


# ----------------------------------------------------------------------------- YAML subset
def _scalar(s: str):
    s = s.strip()
    if s == "" or s == "~" or s == "null":
        return None
    if s == "[]":
        return []
    if s == "{}":
        return OrderedDict()
    if s[0] == "[" and s.endswith("]") and not any(c in s[1:-1] for c in "[]{}"):
        return [_scalar(x) for x in s[1:-1].split(",")] if s[1:-1].strip() else []
    if s[0] in "[{&*!|>":
        raise ImportError_("YAML feature not supported: %r" % s)
    if s[0] in "\"'":
        q = s[0]
        if not s.endswith(q) or len(s) < 2:
            raise ImportError_("unterminated quoted scalar %r" % s)
        body = s[1:-1]
        return body.replace("''", "'") if q == "'" else body.replace('\\"', '"')
    if s in ("true", "True"):
        return True
    if s in ("false", "False"):
        return False
    if re.match(r"^-?\d+$", s):
        return int(s)
    if re.match(r"^0x[0-9A-Fa-f]+$", s):
        return int(s, 16)
    return s


def _strip_comment(line: str) -> str:
    out, q = [], None
    for i, ch in enumerate(line):
        if q:
            if ch == q:
                q = None
        elif ch in "\"'":
            q = ch
        elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
            break
        out.append(ch)
    return "".join(out).rstrip()


def load_yaml(text: str):
    lines = []
    for raw in text.splitlines():
        if raw.strip() in ("---", "..."):
            continue
        s = _strip_comment(raw)
        if s.strip():
            if "\t" in s[:len(s) - len(s.lstrip())]:
                raise ImportError_("tab indentation in YAML")
            lines.append((len(s) - len(s.lstrip()), s.strip()))
    pos = [0]

    def parse_block(indent):
        if pos[0] >= len(lines):
            return None
        ind, s = lines[pos[0]]
        if s.startswith("- ") or s == "-":
            return parse_seq(ind)
        return parse_map(ind)

    def parse_map(indent, first=None):
        out = OrderedDict()
        if first is not None:
            k, v = first
            out[k] = v
        while pos[0] < len(lines):
            ind, s = lines[pos[0]]
            if ind < indent:
                break
            if ind > indent:
                raise ImportError_("bad YAML indentation at %r" % s)
            if s.startswith("- "):
                break
            m = re.match(r"^([^:]+?):(?:\s+(.*))?$", s)
            if not m:
                raise ImportError_("YAML line not understood: %r" % s)
            key, rest = m.group(1).strip(), m.group(2)
            pos[0] += 1
            if rest is None or rest == "":
                if pos[0] < len(lines) and (lines[pos[0]][0] > ind or
                                            (lines[pos[0]][0] == ind and lines[pos[0]][1].startswith("- "))):
                    out[key] = parse_block(lines[pos[0]][0])
                else:
                    out[key] = None
            else:
                out[key] = _scalar(rest)
        return out

    def parse_seq(indent):
        out = []
        while pos[0] < len(lines):
            ind, s = lines[pos[0]]
            if ind != indent or not (s.startswith("- ") or s == "-"):
                break
            item = s[2:].strip() if s != "-" else ""
            pos[0] += 1
            if item == "":
                out.append(parse_block(lines[pos[0]][0]) if pos[0] < len(lines) and lines[pos[0]][0] > ind else None)
                continue
            m = re.match(r"^([^:\"']+?):(?:\s+(.*))?$", item)
            if m:
                key, rest = m.group(1).strip(), m.group(2)
                child = ind + 2
                if rest is None or rest == "":
                    val = parse_block(lines[pos[0]][0]) if pos[0] < len(lines) and lines[pos[0]][0] > child - 1 else None
                else:
                    val = _scalar(rest)
                if pos[0] < len(lines) and lines[pos[0]][0] == child and not lines[pos[0]][1].startswith("- "):
                    out.append(parse_map(child, (key, val)))
                else:
                    d = OrderedDict()
                    d[key] = val
                    out.append(d)
            else:
                out.append(_scalar(item))
        return out

    if not lines:
        return None
    result = parse_block(lines[0][0])
    if pos[0] != len(lines):
        raise ImportError_("YAML not fully read near %r" % (lines[pos[0]][1],))
    return result


# ----------------------------------------------------------------------------- facts
class Composite:
    def __init__(self, path, models, sources):
        self.path = path
        self.models = models      # device-tree models it matches
        self.sources = sources    # [{name, phys_path, map_id}]


def read_family(tree, family: str):
    """(composites, maps) from the family's InputPlumber files."""
    base = tree.device_dir(family) / IP_DIR
    composites, maps = [], {}
    cdir = base / "capability_maps"
    if cdir.is_dir():
        for p in sorted(cdir.glob("*.yaml"), key=lambda q: q.name):
            doc = load_yaml(tree.read_text(p))
            if isinstance(doc, dict) and doc.get("kind") == "CapabilityMap" and doc.get("id"):
                maps[doc["id"]] = (tree.rel(p), doc)
    ddir = base / "devices"
    if ddir.is_dir():
        for p in sorted(ddir.glob("*.yaml"), key=lambda q: q.name):
            doc = load_yaml(tree.read_text(p))
            if not isinstance(doc, dict) or doc.get("kind") != "CompositeDevice":
                continue
            models = []
            for m in doc.get("matches") or []:
                udev = (m or {}).get("udev") or {}
                if udev.get("sys_path") == "/sys/firmware/devicetree/base":
                    for a in udev.get("attributes") or []:
                        if a.get("name") == "model" and a.get("value"):
                            models.append(str(a["value"]))
            sources = []
            for s in doc.get("source_devices") or []:
                ev = (s or {}).get("evdev")
                if not ev or not ev.get("name"):
                    continue
                sources.append(OrderedDict([("name", str(ev["name"])),
                                            ("phys_path", ev.get("phys_path")),
                                            ("map", s.get("capability_map_id"))]))
            composites.append(Composite(tree.rel(p), models, sources))
    return composites, maps


def _source_id(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def pad_facts(composite: Composite, maps: dict):
    """(pad dict, notes, map paths) for one composite device."""
    notes = []
    sources = []
    buttons = []
    used_maps = []
    rumble_source = None
    abs_seen = False
    for s in composite.sources:
        sid = _source_id(s["name"])
        match = OrderedDict([("name", s["name"])])
        if s.get("phys_path"):
            match["phys"] = str(s["phys_path"])
        if s.get("map") is None:
            if "vibrator" in s["name"]:
                role = "rumble"
                rumble_source = sid
            else:
                role = "buttons"
                notes.append("source %s has no capability map" % s["name"])
        else:
            role = "buttons"
            if s["map"] not in maps:
                raise ImportError_("%s: capability map %s not found" % (composite.path, s["map"]))
        sources.append(OrderedDict([("id", sid), ("role", role), ("match", match),
                                    ("grab", role != "rumble"), ("hide", True)]))
        if s.get("map") is None:
            continue
        mpath, mdoc = maps[s["map"]]
        if mpath not in used_maps:
            used_maps.append(mpath)
        for ent in mdoc.get("mapping") or []:
            srcs = ent.get("source_events") or []
            tgt = ent.get("target_event") or {}
            if len(srcs) != 1 or "evdev" not in srcs[0]:
                abs_seen = True   # what it feeds (often a stick) is unknown: derive nothing
                notes.append("mapping %r: chorded or non-evdev source skipped" % ent.get("name"))
                continue
            ev = srcs[0]["evdev"]
            if ev.get("event_type") != "KEY":
                abs_seen = abs_seen or ev.get("event_type") == "ABS"
                notes.append("mapping %r: %s source skipped" % (ent.get("name"), ev.get("event_type")))
                continue
            code = canonical(str(ev.get("event_code")))
            to = None
            gp = tgt.get("gamepad")
            if isinstance(gp, dict):
                if "button" in gp:
                    to = GAMEPAD.get(str(gp["button"]))
                    km = _KEY.match(str(gp["button"]))
                    if to is None and km:
                        to = "KEY_" + km.group(1).upper()
                elif isinstance(gp.get("trigger"), dict):
                    to = GAMEPAD.get(str(gp["trigger"].get("name")))
            elif "keyboard" in tgt:
                km = _KEY.match(str(tgt["keyboard"]))
                if km:
                    to = "KEY_" + km.group(1).upper()
            if to is None:
                notes.append("mapping %r: target %s not mapped"
                             % (ent.get("name"), json.dumps(tgt, sort_keys=True)))
                continue
            row = OrderedDict([("source", sid), ("code", code), ("to", to)])
            if any(b["source"] == sid and b["code"] == code for b in buttons):
                notes.append("%s %s also maps to %s in InputPlumber; Thimsy keeps the first target"
                             % (sid, code, to))
                continue
            buttons.append(row)
    pad = OrderedDict()
    pad["producer"] = "padd"
    pad["sources"] = sources
    pad["buttons"] = buttons
    tos = set(b["to"] for b in buttons)
    face = len(tos & {"BTN_SOUTH", "BTN_EAST", "BTN_NORTH", "BTN_WEST"})
    clicks = tos & {"BTN_THUMBL", "BTN_THUMBR"}
    if abs_seen:
        pad["face_buttons"] = 4
        notes.append("sticks and face_buttons not derived: the map has axis or non-evdev mappings; "
                     "fill them by hand")
    else:
        pad["face_buttons"] = 6 if face == 4 and len(clicks) == 2 else 4
        if pad["face_buttons"] == 6:
            notes.append("face_buttons 6: stick clicks are mapped and the map has no sticks (derived)")
        pad["sticks"] = []
    if rumble_source:
        pad["rumble"] = OrderedDict([("source", rumble_source)])
    seen = set()
    notes = [n for n in notes if not (n in seen or seen.add(n))]
    return pad, notes, used_maps
