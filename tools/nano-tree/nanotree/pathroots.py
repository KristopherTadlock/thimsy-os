"""Absolute-path roots in nano/'s shared files (docs/linux/IMPLEMENTATION.md B.5, H.4).

Nano names Android paths as string literals ("/data/system/...", "/storage/emulated/0", "/proc/self/...").
On Linux they work unedited because the image maps them (post-build.d/10-android-links.sh,
thimsy-session's /mnt/media_rw links), the shell gate catches the programs, or the feature is not
reached. tools/nano-tree/path-roots.json gives every root a disposition; test_path_roots.py fails on a
root without one, so a new root arriving from nano-144 fails loudly.

A literal counts when its first component is a top-level directory of an Android or Linux root (TOPS).
Its root is its first two components under the Android tops that hold unrelated trees
(/data/system and /data/data differ), else its first component; a component that is a format
specifier, empty or the end of a prefix literal ("/storage/") is written "*". Only literals that look
like paths count: they start with "/" and a letter, and hold no blank.
"""
from __future__ import annotations

import os
import re
from typing import Dict, List, Set

from .ratchets import _read, lex

# The top-level directories of an Android or Linux root. A literal under any other top ("/PSP",
# "/Config.ini") is a suffix Nano appends to a folder, not an absolute path.
TOPS = {"acct", "apex", "bin", "boot", "bootstrap-apex", "cache", "config", "data", "data_mirror", "debug_ramdisk",
        "dev", "efs", "etc", "home", "lib", "lib64", "linkerconfig", "media", "metadata", "mnt", "odm", "oem",
        "opt", "persist", "postinstall", "proc", "product", "root", "run", "sbin", "sdcard", "storage", "sys",
        "system", "system_ext", "tmp", "usr", "var", "vendor"}
TWO_LEVEL = {"data", "system", "mnt", "vendor", "storage", "product", "odm", "apex", "system_ext",
             "sys", "dev", "proc", "run", "usr", "etc", "var"}
# Under /storage only these names are fixed; any other is a volume id ("/storage/1234-ABCD"): "*".
STORAGE_FIXED = {"emulated", "self"}
PATH = re.compile(r"/[A-Za-z][A-Za-z0-9_.@%+:-]*(?:/[A-Za-z0-9_.@%+:*{}-]*)*\Z")
DISPOSITIONS = ("link", "same", "gate", "kernel", "dropped", "not-reached", "not-a-path")


def root_of(literal: str) -> str:
    parts = literal.split("/")[1:]
    first = parts[0]
    if first not in TWO_LEVEL:
        return "/" + first
    second = parts[1] if len(parts) > 1 else None
    if second is None:
        return "/" + first
    if not second or "%" in second or "{" in second or "*" in second:
        second = "*"
    elif first == "storage" and second not in STORAGE_FIXED:
        second = "*"
    return "/%s/%s" % (first, second)


def runtime_files(files: List[str]) -> List[str]:
    """The files of FILES that run on a device: not the tests (they run on a build host, in scratch
    folders) and not compat/ (the Linux and host shim layer, which names its own Linux paths)."""
    return [f for f in files if not f.startswith(("test/", "compat/"))]


def path_literals(nano_dir: str, files: List[str]) -> Dict[str, Set[str]]:
    """{root: files naming it} over FILES (relative to nano/)."""
    out: Dict[str, Set[str]] = {}
    for rel in files:
        _code, strings = lex(_read(os.path.join(nano_dir, *rel.split("/"))))
        for _line, s in strings:
            if PATH.match(s) and s.split("/")[1] in TOPS:
                out.setdefault(root_of(s), set()).add(rel)
    return out


def check_table(table: dict, roots: Dict[str, Set[str]]) -> List[str]:
    problems = []
    rows = {}
    for r in table.get("rows", []):
        root = r.get("root", "")
        if not root.startswith("/"):
            problems.append("a row without a root: %r" % r)
            continue
        if root in rows:
            problems.append("two rows for %s" % root)
        rows[root] = r
        d = r.get("disposition")
        if d not in DISPOSITIONS:
            problems.append("%s: disposition %r is not one of %s" % (root, d, ", ".join(DISPOSITIONS)))
        if d == "link" and not r.get("linux"):
            problems.append("%s: a link without its Linux target" % root)
        if not r.get("note"):
            problems.append("%s: no note" % root)
    for root in sorted(roots):
        if root not in rows and ("/%s/*" % root.split("/")[1]) not in rows:
            problems.append("no row for %s (in %s)" % (root, ", ".join(sorted(roots[root])[:3])))
    for root in rows:
        if root.endswith("/*") and any(k.startswith(root[:-1]) for k in roots):
            continue
        if root not in roots:
            problems.append("stale row %s: no shared file names it" % root)
    return problems
