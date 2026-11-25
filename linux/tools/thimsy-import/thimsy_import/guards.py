"""Hash guards per family block (generated/<family>/guards.json).

Each block of ROCKNIX's imperative text the importer translates, or whose behaviour it
reproduces, is hashed after a light normalisation (trailing spaces, blank lines and
comment-only lines dropped, CRLF already undone). The import stops when a hash differs
from the recorded one: the block changed upstream and its translation needs a human look.
`--accept-guards` records the new hashes after that review. Only hashes and locations are
stored, never ROCKNIX's text.
"""

from __future__ import annotations

import hashlib
import json
from collections import OrderedDict


def normalise(text: str | None) -> str | None:
    if text is None:
        return None
    keep = []
    for line in text.splitlines():
        s = line.rstrip()
        if not s.strip() or s.strip().startswith("#"):
            continue
        keep.append(s)
    return "\n".join(keep) + "\n"


def digest(text: str | None) -> str:
    t = normalise(text)
    if t is None:
        return "absent"
    return "sha256:" + hashlib.sha256(t.encode("utf-8")).hexdigest()


def compute(blocks: list) -> "OrderedDict[str, OrderedDict]":
    """name -> {path, line, kind, hash}; a block given as (name, path, None) is 'absent'."""
    out: "OrderedDict[str, OrderedDict]" = OrderedDict()
    for b in blocks:
        out[b.name] = OrderedDict([("path", b.path), ("kind", b.kind), ("line", b.line),
                                   ("hash", digest(b.text if b.lines is not None else None))])
    return out


def compare(recorded: dict | None, current: dict) -> list:
    """Changed, added and removed guard names; empty when nothing changed."""
    if recorded is None:
        return []
    old = recorded.get("blocks", {})
    diffs = []
    for name, cur in current.items():
        if name not in old:
            diffs.append("added: %s" % name)
        elif old[name].get("hash") != cur["hash"]:
            diffs.append("changed: %s (line %s)" % (name, cur.get("line")))
    for name in old:
        if name not in current:
            diffs.append("removed: %s" % name)
    return diffs


def document(family: str, tag: str, commit: str, blocks: dict) -> "OrderedDict":
    d = OrderedDict()
    d["family"] = family
    d["rocknix_tag"] = tag
    d["rocknix_commit"] = commit
    d["blocks"] = blocks
    return d


def load(path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
