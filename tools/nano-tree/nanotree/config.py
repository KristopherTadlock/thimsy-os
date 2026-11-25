"""upstream.json, the repository layout and path globs."""
from __future__ import annotations

import json
import os
import re
from typing import Dict, List, Optional

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCH_REL = "patches/thimsy/0001-thimsy.patch"
NANO_DIR = "nano"


class NanoTreeError(Exception):
    """A failure the command line reports as one message and exit status 1."""


def find_root(start: Optional[str] = None) -> str:
    """The repository root: the directory above tools/nano-tree, unless NANO_TREE_ROOT says otherwise."""
    env = os.environ.get("NANO_TREE_ROOT")
    if env:
        return os.path.abspath(env)
    if start:
        return os.path.abspath(start)
    return os.path.dirname(os.path.dirname(TOOL_DIR))


_GLOB_CACHE: Dict[str, "re.Pattern[str]"] = {}


def glob_regex(pattern: str) -> "re.Pattern[str]":
    """A regex for a path glob relative to the Nano folder: `*` and `?` stay inside one path
    component, `**` spans components (`a/**` matches everything under a/, `**/x` any x)."""
    rx = _GLOB_CACHE.get(pattern)
    if rx is not None:
        return rx
    out = []
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif c == "*":
            out.append("[^/]*")
            i += 1
        elif c == "?":
            out.append("[^/]")
            i += 1
        elif c == "[":
            j = pattern.find("]", i + 1)
            if j < 0:
                out.append(re.escape(c))
                i += 1
            else:
                body = pattern[i + 1:j]
                if body.startswith("!"):
                    body = "^" + body[1:]
                out.append("[" + body.replace("\\", "\\\\") + "]")
                i = j + 1
        else:
            out.append(re.escape(c))
            i += 1
    rx = re.compile("".join(out) + r"\Z")
    _GLOB_CACHE[pattern] = rx
    return rx


def match_any(path: str, patterns: List[str]) -> bool:
    return any(glob_regex(p).match(path) for p in patterns)


class Upstream:
    """tools/nano-tree/upstream.json: where Nano comes from and how nano/ maps onto the patch."""

    def __init__(self, data: dict, path: Optional[str] = None):
        self.data = data
        self.file = path
        self.repo: str = data["repo"]
        self.base: str = data["base"]
        self.path: str = data["path"].strip("/")
        self.assets: List[str] = list(data.get("assets", []))
        self.abbrev: int = int(data.get("abbrev", 11))
        self.deleted: List[str] = list(data.get("deleted", []))
        self.fork_only: List[str] = list(data.get("fork_only", []))
        self.siblings: List[str] = list(data.get("siblings", []))

    @classmethod
    def load(cls, root: str) -> "Upstream":
        path = os.path.join(root, "tools", "nano-tree", "upstream.json")
        with open(path, "r", encoding="utf-8") as f:
            return cls(json.load(f), path)

    @classmethod
    def from_text(cls, text: str) -> "Upstream":
        return cls(json.loads(text))

    def save(self, path: Optional[str] = None) -> None:
        path = path or self.file
        assert path
        self.data["base"] = self.base
        self.data["deleted"] = sorted(self.deleted)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.data, f, indent=2)
            f.write("\n")

    def is_asset(self, rel: str) -> bool:
        return match_any(rel, self.assets)

    def is_fork_only(self, rel: str) -> bool:
        return match_any(rel, self.fork_only)

    @property
    def prefix(self) -> str:
        """The Nano folder's path in the AOSP tree, with a trailing slash."""
        return self.path + "/"


class Layout:
    """Where things are in a checkout of this repository."""

    def __init__(self, root: str):
        self.root = os.path.abspath(root)
        self.patch = os.path.join(self.root, *PATCH_REL.split("/"))
        self.nano = os.path.join(self.root, NANO_DIR)
        self.tool_dir = os.path.join(self.root, "tools", "nano-tree")
        self.block_sha = os.path.join(self.tool_dir, "block.sha256")
        self.cache = os.path.join(self.root, "work", "nano-base")

    def read_block_sha(self) -> Optional[str]:
        try:
            with open(self.block_sha, "r", encoding="ascii") as f:
                return f.read().split()[0].strip() or None
        except (OSError, IndexError):
            return None

    def write_block_sha(self, digest: str) -> None:
        with open(self.block_sha, "w", encoding="ascii", newline="\n") as f:
            f.write(digest + "\n")


BASE_LINE = re.compile(r"^BASE=([0-9a-f]{40})\s*$", re.M)


def base_from_fetch_script(text: str) -> Optional[str]:
    """The BASE= line of tools/android-launcher/fetch_nano.sh, as nano-144 records its base."""
    m = BASE_LINE.search(text)
    return m.group(1) if m else None
