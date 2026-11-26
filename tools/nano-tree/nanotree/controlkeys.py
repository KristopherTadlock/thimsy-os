"""Control keys (docs/linux/IMPLEMENTATION.md B.2): the property writes that are messages to Android's
init or framework, derived from the tree, and their Linux dispositions in
tools/nano-tree/control-keys.json.

Derived, not hand-listed:
- every `on ...` trigger with a `property:` term in nano/'s .rc files (gammaos-nano.rc);
- every `on ...` trigger in the patch's other .rc sections (drastic-nano.rc, the device rc files)
  with a term naming one of Nano's property keys (tools/nano-tree/props.txt);
- every framework listener: one of Nano's property keys that the patch's other sections (Java
  services, SurfaceFlinger, inputflinger, drastic-nano, gammapad) name as a string literal, in added
  or context lines. GammaOS base files the patch does not touch can't be read here, so a listener
  only there is missed; such rows are added by hand with "source": "manual";
- init's own control keys among Nano's keys: ctl.*, sys.powerctl and service.bootanim.*.

A trigger is written as its conditions joined by " && ", each `property:K=V` as `K=V` and an event
as its name (`early-init && ro.bootmode=normal`). A listener is its key.
"""
from __future__ import annotations

import json
import os
import re
from typing import Dict, List, Optional, Set, Tuple

from .patchfile import Patch
from .ratchets import PROP_KEY, read_list

DISPOSITIONS = ("routed", "store-only", "refused", "not-reached")
INIT_KEY = re.compile(r"(ctl\..*|sys\.powerctl|service\.bootanim\..*)\Z")
STRING = re.compile(r'"([^"\\\r\n]*)"')
ON_LINE = re.compile(r"^\s*on\s+(.*?)\s*$")
CPP_ROW = re.compile(r'^\s*\{\s*"([^"]*)",\s*"([^"]*)",\s*Action::(\w+),\s*"([^"]*)",\s*"([^"]*)"\s*\},\s*$')
CPP_ACTIONS = {"Session", "Launch", "Hook", "FirstFrame", "Reset", "Refuse"}


def normalise_trigger(conditions: str) -> Optional[str]:
    """The trigger of an `on` line's conditions, or None when it has no property term."""
    terms = [t.strip() for t in conditions.split("&&")]
    out = []
    has_prop = False
    for t in terms:
        if t.startswith("property:"):
            has_prop = True
            t = t[len("property:"):]
        out.append(t)
    return " && ".join(out) if has_prop else None


def trigger_keys(trigger: str) -> List[str]:
    """The property keys a trigger's terms name."""
    return [t.split("=", 1)[0] for t in trigger.split(" && ") if "=" in t]


def rc_triggers(text: str) -> List[str]:
    out = []
    for line in text.splitlines():
        m = ON_LINE.match(line)
        if m:
            t = normalise_trigger(m.group(1))
            if t is not None and t not in out:
                out.append(t)
    return out


def _post_lines(section_data: bytes) -> List[str]:
    """The lines of a patch section as they read after the patch (added and context lines)."""
    out = []
    for raw in section_data.split(b"\n"):
        if raw.startswith(b"+++") or raw[:1] not in (b"+", b" "):
            continue
        out.append(raw[1:].decode("utf-8", "replace"))
    return out


def derive(nano_dir: str, patch_bytes: bytes, prefix: str, props: Set[str]) -> Dict[str, List[str]]:
    """{trigger or key: [where it was found]} over the tree and the patch."""
    found: Dict[str, Set[str]] = {}

    def add(item: str, where: str) -> None:
        found.setdefault(item, set()).add(where)

    for name in sorted(os.listdir(nano_dir)):
        if name.endswith(".rc"):
            with open(os.path.join(nano_dir, name), "r", encoding="utf-8", errors="replace") as f:
                for t in rc_triggers(f.read()):
                    add(t, "nano/" + name)
    patch = Patch.parse(patch_bytes)
    start, end = patch.block_range(prefix)
    for i, sec in enumerate(patch.sections):
        if start <= i < end:
            continue
        lines = _post_lines(sec.data)
        short = sec.path.rsplit("/", 1)[-1]
        if sec.path.endswith(".rc"):
            for t in rc_triggers("\n".join(lines)):
                if any(k in props for k in trigger_keys(t)):
                    add(t, short)
        for line in lines:
            for m in STRING.finditer(line):
                k = m.group(1)
                if k in props and PROP_KEY.match(k):
                    add(k, short)
    for k in props:
        if INIT_KEY.match(k):
            add(k, "init")
    return {k: sorted(v) for k, v in sorted(found.items())}


def load_table(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def is_glob(match: str) -> bool:
    return match.endswith(".*") and "=" not in match and " " not in match


def covering_row(item: str, rows: List[dict]) -> Optional[dict]:
    """The row for ITEM: its exact row, else (for a key) the longest glob row that matches."""
    for r in rows:
        if r["match"] == item:
            return r
    if "=" in item or " " in item:
        return None
    best = None
    for r in rows:
        m = r["match"]
        if is_glob(m) and item.startswith(m[:-1]) and (best is None or len(m) > len(best["match"])):
            best = r
    return best


def check_table(table: dict, derived: Dict[str, List[str]]) -> List[str]:
    """Problems: rows that are malformed, derived items with no row, rows nothing derives."""
    problems = []
    rows = table.get("rows", [])
    seen = set()
    for r in rows:
        m = r.get("match", "")
        if not m:
            problems.append("a row without a match: %r" % r)
            continue
        if m in seen:
            problems.append("two rows for %s" % m)
        seen.add(m)
        d = r.get("disposition")
        if d not in DISPOSITIONS:
            problems.append("%s: disposition %r is not one of %s" % (m, d, ", ".join(DISPOSITIONS)))
        if d == "routed" and not r.get("route"):
            problems.append("%s: routed, but no route" % m)
        if d != "routed" and r.get("route"):
            problems.append("%s: a route on a %s row" % (m, d))
        if not r.get("note"):
            problems.append("%s: no note" % m)
        if "value" in r and ("=" in m or is_glob(m)):
            problems.append("%s: a value on a trigger or glob row" % m)
        if r.get("source") not in (None, "manual"):
            problems.append("%s: source %r (only \"manual\")" % (m, r.get("source")))
        if r.get("source") == "manual" and m in derived:
            problems.append("%s: marked manual, but it is derived (drop \"source\")" % m)
    for item in derived:
        if covering_row(item, rows) is None:
            problems.append("no row for %s (found in %s)" % (item, ", ".join(derived[item][:3])))
    for r in rows:
        m = r.get("match", "")
        if not m or r.get("source") == "manual":
            continue
        if is_glob(m):
            if not any(covering_row(i, rows) is r for i in derived):
                problems.append("glob row %s covers nothing" % m)
        elif m not in derived:
            problems.append("stale row %s: nothing derives it (mark it \"source\": \"manual\" with a reason, "
                            "or drop it)" % m)
    return problems


def cpp_rows(cpp_text: str) -> List[Tuple[str, str, str, str, str]]:
    """The (key, value, action, arg, reset) rows between ControlKeys.cpp's BEGIN and END markers."""
    try:
        body = cpp_text.split("// BEGIN control-keys", 1)[1].split("// END control-keys", 1)[0]
    except IndexError:
        raise ValueError("ControlKeys.cpp has no BEGIN/END control-keys markers")
    out = []
    for line in body.splitlines():
        s = line.strip()
        if not s.startswith("{"):
            continue
        m = CPP_ROW.match(line)
        if not m:
            raise ValueError("can't read this ControlKeys.cpp row: %s" % s)
        out.append(m.groups())
    return out


def expected_cpp_rows(table: dict) -> List[Tuple[str, str, str, str, str]]:
    """The rows ControlKeys.cpp must have for TABLE: every single-key row that is refused, or
    routed to something Nano's process does (not "session-key", which the session reads itself)."""
    out = []
    for r in table.get("rows", []):
        m = r["match"]
        if " && " in m or is_glob(m):
            continue
        key, _, value = m.partition("=")
        value = r.get("value", value)
        reset = r.get("reset", "")
        d = r["disposition"]
        if d == "refused":
            out.append((key, value, "Refuse", "", reset))
            continue
        if d != "routed":
            continue
        route = r["route"]
        if route == "session-key":
            continue
        if route.startswith("session:"):
            out.append((key, value, "Session", route[len("session:"):], reset))
        elif route.startswith("hook:"):
            out.append((key, value, "Hook", route[len("hook:"):], reset))
        elif route == "launcher":
            out.append((key, value, "Launch", "", reset))
        elif route == "first-frame":
            out.append((key, value, "FirstFrame", "", reset))
        elif route == "rc-reset":
            out.append((key, value, "Reset", "", reset))
        else:
            raise ValueError("%s: unknown route %r" % (m, route))
    return out


def load_props(path: str) -> Set[str]:
    return set(read_list(path))
