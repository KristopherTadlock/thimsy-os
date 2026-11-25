"""device.json: the writer, the schema check (linux/schema/device.schema.json), and merge.

The validator implements the draft-07 keywords the schema uses (listed in its description);
a keyword it does not know is an error, so the schema can't silently grow past it.
"""

from __future__ import annotations

import json
import re
from collections import OrderedDict
from pathlib import Path

from .rocknix import ImportError_

SCHEMA_PATH = Path(__file__).resolve().parents[3] / "schema" / "device.schema.json"

_ANNOTATIONS = {"$schema", "$id", "title", "description", "$comment", "default", "examples"}
_KNOWN = _ANNOTATIONS | {"type", "enum", "const", "pattern", "minLength", "minimum", "maximum",
                         "minItems", "maxItems", "uniqueItems", "properties", "required",
                         "additionalProperties", "patternProperties", "propertyNames", "items",
                         "$ref", "anyOf", "definitions"}


def load_schema(path: Path = SCHEMA_PATH) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _type_ok(v, t: str) -> bool:
    if t == "object":
        return isinstance(v, dict)
    if t == "array":
        return isinstance(v, list)
    if t == "string":
        return isinstance(v, str)
    if t == "integer":
        return isinstance(v, int) and not isinstance(v, bool)
    if t == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "boolean":
        return isinstance(v, bool)
    if t == "null":
        return v is None
    raise ImportError_("schema type %r not supported" % t)


def _eq(a, b) -> bool:
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    return a == b


def validate(doc, schema: dict, root: dict | None = None, where: str = "") -> list:
    """All errors as 'pointer: message' strings; empty when the document is valid."""
    root = root if root is not None else schema
    errs: list = []
    unknown = set(schema) - _KNOWN
    if unknown:
        raise ImportError_("schema keyword(s) not supported: %s" % ", ".join(sorted(unknown)))
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/"):
            raise ImportError_("only local $ref supported: %s" % ref)
        node = root
        for part in ref[2:].split("/"):
            node = node[part]
        errs += validate(doc, node, root, where)
    if "type" in schema:
        ts = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_type_ok(doc, t) for t in ts):
            return errs + ["%s: expected %s" % (where or "/", "/".join(ts))]
    if "const" in schema and not _eq(doc, schema["const"]):
        errs.append("%s: must be %r" % (where or "/", schema["const"]))
    if "enum" in schema and not any(_eq(doc, e) for e in schema["enum"]):
        errs.append("%s: %r not in %r" % (where or "/", doc, schema["enum"]))
    if "anyOf" in schema:
        if not any(not validate(doc, s, root, where) for s in schema["anyOf"]):
            errs.append("%s: %r matches none of the allowed forms" % (where or "/", doc))
    if isinstance(doc, str):
        if "pattern" in schema and not re.search(schema["pattern"], doc):
            errs.append("%s: %r does not match %s" % (where or "/", doc, schema["pattern"]))
        if "minLength" in schema and len(doc) < schema["minLength"]:
            errs.append("%s: too short" % (where or "/"))
    if isinstance(doc, (int, float)) and not isinstance(doc, bool):
        if "minimum" in schema and doc < schema["minimum"]:
            errs.append("%s: below %s" % (where or "/", schema["minimum"]))
        if "maximum" in schema and doc > schema["maximum"]:
            errs.append("%s: above %s" % (where or "/", schema["maximum"]))
    if isinstance(doc, list):
        if "minItems" in schema and len(doc) < schema["minItems"]:
            errs.append("%s: fewer than %d items" % (where or "/", schema["minItems"]))
        if "maxItems" in schema and len(doc) > schema["maxItems"]:
            errs.append("%s: more than %d items" % (where or "/", schema["maxItems"]))
        if schema.get("uniqueItems"):
            seen = [json.dumps(x, sort_keys=True) for x in doc]
            if len(seen) != len(set(seen)):
                errs.append("%s: items not unique" % (where or "/"))
        if "items" in schema:
            for i, item in enumerate(doc):
                errs += validate(item, schema["items"], root, "%s/%d" % (where, i))
    if isinstance(doc, dict):
        for k in schema.get("required", []):
            if k not in doc:
                errs.append("%s: missing %r" % (where or "/", k))
        props = schema.get("properties", {})
        pats = schema.get("patternProperties", {})
        for k, v in doc.items():
            p = "%s/%s" % (where, k)
            if "propertyNames" in schema:
                errs += validate(k, schema["propertyNames"], root, p + " (name)")
            matched = False
            if k in props:
                matched = True
                errs += validate(v, props[k], root, p)
            for pat, sub in pats.items():
                if re.search(pat, k):
                    matched = True
                    errs += validate(v, sub, root, p)
            if not matched:
                ap = schema.get("additionalProperties", True)
                if ap is False:
                    errs.append("%s: property not allowed" % p)
                elif isinstance(ap, dict):
                    errs += validate(v, ap, root, p)
    return errs


def check_refs(doc: dict) -> list:
    """Cross-field rules the schema can't say: pad sources referenced by id must exist."""
    errs = []
    pad = doc.get("pad")
    if not isinstance(pad, dict):
        return errs
    ids = [s.get("id") for s in pad.get("sources", [])]
    if len(ids) != len(set(ids)):
        errs.append("/pad/sources: duplicate id")
    refs = [("/pad/buttons/%d" % i, b.get("source")) for i, b in enumerate(pad.get("buttons", []))]
    refs += [("/pad/sticks/%d" % i, s.get("source")) for i, s in enumerate(pad.get("sticks", []))]
    refs += [("/pad/triggers/%d" % i, s.get("source")) for i, s in enumerate(pad.get("triggers", []))]
    if "rumble" in pad:
        refs.append(("/pad/rumble", pad["rumble"].get("source")))
    for where, sid in refs:
        if sid not in ids:
            errs.append("%s: unknown source %r" % (where, sid))
    seen = set()
    for i, b in enumerate(pad.get("buttons", [])):
        k = (b.get("source"), b.get("code"))
        if k in seen:
            errs.append("/pad/buttons/%d: %s %s mapped twice" % (i, k[0], k[1]))
        seen.add(k)
    return errs


def check(doc: dict, schema: dict | None = None) -> list:
    return validate(doc, schema or load_schema()) + check_refs(doc)


def merge(base, override):
    """Deep merge: objects merge key by key, anything else is replaced by the override."""
    if isinstance(base, dict) and isinstance(override, dict):
        out = OrderedDict(base)
        for k, v in override.items():
            out[k] = merge(base[k], v) if k in base else v
        return out
    return override


def dumps(doc) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
