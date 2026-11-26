# SPDX-License-Identifier: MIT
"""Fake build tools for test_post_image.py: fsck.erofs, mkfs.erofs,
mkfs.ext4, genimage, and stand-ins for tools/fit.py and tools/gpt_check.py.

    post_image_fake_tool.py NAME ARGS...

Each call appends one JSON line {"tool", "argv", "env"} to $FAKE_LOG and
writes small files shaped like the real outputs, so the scripts under test
can be followed end to end without Buildroot:

  * a fake EROFS image is 1024 zero bytes, the EROFS magic, then JSON
    {"files": {path: text}}; mkfs.erofs writes one from a directory and
    fsck.erofs --extract unpacks one (it refuses to overwrite without
    --overwrite, as the real one does);
  * mkfs.ext4 -d writes JSON {"files": ...} and checks the size argument;
  * genimage checks every file its --config names is in --inputpath and
    writes the image the cfg names: JSON of the inputs' sizes and digests,
    or, with FAKE_GENIMAGE_GPT=good|basic, a real GPT image in the E.6
    layout shrunk (test_gpt_check.write_image), the idbloader input at
    sector 64; "basic" adds a Microsoft basic data entry (a vold breach);
  * fit.py build writes cards/<id>/fit/boot.img with the THIMSFIT header;
  * gpt_check.py exits with $FAKE_GPT_RC (default 0).

FAKE_FAIL=NAME makes that tool exit 1; FAKE_EROFS_BYTES pads mkfs.erofs's
output to that size (sparse).
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import sys
from typing import Dict, List

EROFS_MAGIC = struct.pack("<I", 0xE0F5E1E2)
HERE = os.path.dirname(os.path.abspath(__file__))


def log(name: str, argv: List[str]) -> None:
    path = os.environ.get("FAKE_LOG")
    if not path:
        return
    env = {k: os.environ[k] for k in ("E2FSPROGS_FAKE_TIME", "SOURCE_DATE_EPOCH")
           if k in os.environ}
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps({"tool": name, "argv": argv, "env": env}) + "\n")


def native(p: str) -> str:
    """A path from Git Bash (/c/x) as Windows sees it (C:/x); others unchanged."""
    if os.name == "nt":
        m = re.match(r"^/([a-zA-Z])(/.*)?$", p)
        if m:
            return "%s:%s" % (m.group(1).upper(), m.group(2) or "/")
    return p


def tree(src: str) -> Dict[str, str]:
    files: Dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            with open(full, "rb") as f:
                files[rel] = f.read().decode("utf-8", "replace")
    return files


def write_fake_erofs(path: str, files: Dict[str, str]) -> None:
    with open(path, "wb") as f:
        f.write(b"\0" * 1024 + EROFS_MAGIC + json.dumps({"files": files}, sort_keys=True).encode())


def read_fake_erofs(path: str) -> Dict[str, str]:
    with open(path, "rb") as f:
        data = f.read()
    if data[1024:1028] != EROFS_MAGIC:
        raise SystemExit("fsck.erofs: %s: not an EROFS image" % path)
    return json.loads(data[1028:].decode())["files"]


def mkfs_erofs(args: List[str]) -> int:
    pos = [a for a in args if not a.startswith("-")]
    if len(pos) != 2:
        print("mkfs.erofs: want OUT SRC, got %r" % pos, file=sys.stderr)
        return 2
    out, src = native(pos[0]), native(pos[1])
    write_fake_erofs(out, tree(src))
    pad = os.environ.get("FAKE_EROFS_BYTES")
    if pad:
        with open(out, "r+b") as f:
            f.truncate(int(pad))
    return 0


def fsck_erofs(args: List[str]) -> int:
    dest = None
    overwrite = False
    image = None
    for a in args:
        if a.startswith("--extract="):
            dest = native(a.split("=", 1)[1])
        elif a == "--overwrite":
            overwrite = True
        elif not a.startswith("-"):
            image = native(a)
    if not dest or not image:
        print("fsck.erofs: want --extract=DIR IMAGE", file=sys.stderr)
        return 2
    for rel, text in read_fake_erofs(image).items():
        p = os.path.join(dest, rel)
        if os.path.exists(p) and not overwrite:
            print("fsck.erofs: %s exists" % p, file=sys.stderr)
            return 1
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return 0


def mkfs_ext4(args: List[str]) -> int:
    src = None
    pos: List[str] = []
    i = 0
    with_value = {"-L", "-U", "-m", "-E", "-d", "-T", "-O", "-N", "-b"}
    while i < len(args):
        a = args[i]
        if a in with_value:
            if a == "-d":
                src = native(args[i + 1])
            i += 2
            continue
        if not a.startswith("-"):
            pos.append(a)
        i += 1
    if src is None or len(pos) != 2 or not re.match(r"^\d+k$", pos[1]):
        print("mkfs.ext4: want -d DIR OUT SIZEk, got %r" % args, file=sys.stderr)
        return 2
    with open(native(pos[0]), "w", encoding="utf-8") as f:
        json.dump({"files": tree(src), "size": pos[1]}, f, sort_keys=True)
    return 0


def cfg_images(cfg: str) -> List[str]:
    with open(cfg, encoding="utf-8") as f:
        return re.findall(r'^\s*image\s*=\s*"([^"]+)"', f.read(), re.M)


def cfg_output(cfg: str) -> str:
    with open(cfg, encoding="utf-8") as f:
        m = re.search(r"^image\s+(\S+)\s*\{", f.read(), re.M)
    assert m
    return m.group(1)


def genimage(args: List[str]) -> int:
    opts: Dict[str, str] = {}
    i = 0
    while i < len(args):
        opts[args[i]] = native(args[i + 1])
        i += 2
    need = ["--rootpath", "--tmppath", "--inputpath", "--outputpath", "--config"]
    for k in need:
        if k not in opts:
            print("genimage: %s missing" % k, file=sys.stderr)
            return 2
    for k in need[:4]:
        if not os.path.isdir(opts[k]):
            print("genimage: %s %s is not a directory" % (k, opts[k]), file=sys.stderr)
            return 2
    inputs = {}
    for name in cfg_images(opts["--config"]):
        p = os.path.join(opts["--inputpath"], name)
        if not os.path.isfile(p):
            print("genimage: %s not in the input path" % name, file=sys.stderr)
            return 1
        with open(p, "rb") as f:
            data = f.read()
        inputs[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    out = os.path.join(opts["--outputpath"], cfg_output(opts["--config"]))
    mode = os.environ.get("FAKE_GENIMAGE_GPT")
    if mode:
        sys.path.insert(0, HERE)
        import test_gpt_check as g  # the test's own GPT writer
        parts = list(g.GOOD)
        if mode == "basic":
            parts.append(("games", g.BASIC, g.DATA_END, g.MIB_S))
        idb = b""
        p = os.path.join(opts["--inputpath"], "idbloader.img")
        if os.path.isfile(p):
            with open(p, "rb") as f:
                idb = f.read()
        g.write_image(out, parts, idbloader=idb)
    else:
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"inputs": inputs}, f, sort_keys=True)
    return 0


def fit_py(args: List[str]) -> int:
    if not args or args[0] != "build":
        return 2
    out = None
    for i, a in enumerate(args):
        if a == "--out-dir":
            out = native(args[i + 1])
    if out is None:
        return 2
    os.makedirs(out, exist_ok=True)
    body = (" ".join(args)).encode()
    sectors = (len(body) + 511) // 512
    with open(os.path.join(out, "boot.img"), "wb") as f:
        f.write((b"THIMSFIT" + struct.pack("<I", sectors)).ljust(512, b"\0") + body)
    return 0


def main(argv: List[str]) -> int:
    name, args = argv[0], argv[1:]
    log(name, args)
    if os.environ.get("FAKE_FAIL") == name:
        return 1
    if name == "mkfs.erofs":
        return mkfs_erofs(args)
    if name == "fsck.erofs":
        return fsck_erofs(args)
    if name == "mkfs.ext4":
        return mkfs_ext4(args)
    if name == "genimage":
        return genimage(args)
    if name == "fit.py":
        return fit_py(args)
    if name == "gpt_check.py":
        return int(os.environ.get("FAKE_GPT_RC", "0"))
    print("fake tool: unknown %s" % name, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
