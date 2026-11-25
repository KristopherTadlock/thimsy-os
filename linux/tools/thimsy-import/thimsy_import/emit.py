"""Deterministic writes to generated/<family>/ and gpl/generated/<family>/.

The importer builds the complete set of files in memory (path -> bytes) and this module
makes the two directories equal to it: new and changed files written, files the import no
longer produces removed. Nothing outside the two managed directories is touched. A second
run over the same input writes nothing. With check=True nothing is written at all; the
differences are only returned.
"""

from __future__ import annotations

import os
from pathlib import Path


class Plan:
    def __init__(self):
        self.added: list = []
        self.changed: list = []
        self.removed: list = []

    @property
    def clean(self) -> bool:
        return not (self.added or self.changed or self.removed)

    def lines(self) -> list:
        return (["A %s" % p for p in self.added] + ["M %s" % p for p in self.changed]
                + ["D %s" % p for p in self.removed])


def _existing(root: Path, managed: list) -> set:
    out = set()
    for m in managed:
        d = root / m
        if not d.is_dir():
            continue
        for dirpath, _dirs, files in os.walk(str(d)):
            for f in files:
                out.add(Path(os.path.relpath(os.path.join(dirpath, f), str(root))).as_posix())
    return out


def sync(root: Path, managed: list, files: dict, check: bool = False) -> Plan:
    """Make root/<managed dirs> hold exactly `files` (posix relpath -> bytes)."""
    for rel in files:
        if not any(rel == m or rel.startswith(m.rstrip("/") + "/") for m in managed):
            raise ValueError("%s is outside the managed directories" % rel)
    plan = Plan()
    have = _existing(root, managed)
    for rel in sorted(files):
        p = root / rel
        if rel not in have:
            plan.added.append(rel)
        elif p.read_bytes() != files[rel]:
            plan.changed.append(rel)
    plan.removed = sorted(have - set(files))
    if check:
        return plan
    for rel in plan.added + plan.changed:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_name(p.name + ".thimsy-import.tmp")
        tmp.write_bytes(files[rel])
        os.replace(str(tmp), str(p))
    for rel in plan.removed:
        (root / rel).unlink()
    for m in managed:   # drop directories left empty
        d = root / m
        if not d.is_dir():
            continue
        for dirpath, dirs, fs in sorted(os.walk(str(d), topdown=False), reverse=False):
            if not os.listdir(dirpath):
                os.rmdir(dirpath)
    return plan
