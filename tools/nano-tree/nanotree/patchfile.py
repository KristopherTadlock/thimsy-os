"""A git patch as a list of per-file sections, the Nano block among them, and splicing.

Everything works on bytes: the patch carries files in any encoding and binary sections.
"""
from __future__ import annotations

import hashlib
from typing import List, Optional, Tuple

from .config import NanoTreeError

DIFF = b"diff --git "


def _unquote(s: bytes) -> Tuple[bytes, bytes]:
    """Parse one C-quoted string at the start of S (which starts with a double quote); return the
    unquoted bytes and the rest of S after the closing quote."""
    assert s[:1] == b'"'
    out = bytearray()
    i = 1
    esc = {ord("n"): 10, ord("t"): 9, ord("r"): 13, ord('"'): 34, ord("\\"): 92,
           ord("a"): 7, ord("b"): 8, ord("f"): 12, ord("v"): 11}
    while i < len(s):
        c = s[i]
        if c == 34:
            return bytes(out), s[i + 1:]
        if c == 92 and i + 1 < len(s):
            n = s[i + 1]
            if 48 <= n <= 55 and i + 3 < len(s):
                out.append(int(s[i + 1:i + 4], 8))
                i += 4
                continue
            out.append(esc.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    raise NanoTreeError("unterminated quoted path in patch header: %r" % s[:80])


def header_path(line: bytes) -> str:
    """The path a `diff --git a/P b/P` line names (the b/ side), decoded as UTF-8."""
    rest = line[len(DIFF):].rstrip(b"\n")
    if rest.startswith(b'"'):
        a, rest2 = _unquote(rest)
        rest2 = rest2.lstrip(b" ")
        b = _unquote(rest2)[0] if rest2.startswith(b'"') else rest2
    else:
        n = len(rest)
        half = (n - 5) // 2
        cand = rest[2:2 + half]
        if (n - 5) % 2 == 0 and rest == b"a/" + cand + b" b/" + cand:
            b = b"b/" + cand
        else:
            k = rest.find(b' "b/')
            if k >= 0:
                b = _unquote(rest[k + 1:])[0]
            else:
                k = rest.find(b" b/")
                if k < 0:
                    raise NanoTreeError("can't read the path of %r" % line[:120])
                b = rest[k + 1:]
    if not b.startswith(b"b/"):
        raise NanoTreeError("unexpected path prefix in %r" % line[:120])
    return b[2:].decode("utf-8", "surrogateescape")


class Section:
    """One file's section of a patch: from its `diff --git` line to the next one."""

    __slots__ = ("path", "data", "header")

    def __init__(self, path: str, data: bytes):
        self.path = path
        self.data = data
        self.header = self._parse_header()

    def _parse_header(self) -> dict:
        h = {}
        for line in self.data.split(b"\n")[1:]:
            if line.startswith((b"@@", b"--- ", b"+++ ", b"GIT binary patch", b"Binary files ")):
                if line.startswith(b"GIT binary patch") or line.startswith(b"Binary files "):
                    h["binary"] = True
                break
            text = line.decode("utf-8", "surrogateescape")
            for key in ("old mode ", "new mode ", "deleted file mode ", "new file mode ",
                        "similarity index ", "rename from ", "rename to ", "copy from ", "copy to "):
                if text.startswith(key):
                    h[key.strip()] = text[len(key):]
                    break
            else:
                if text.startswith("index "):
                    parts = text[len("index "):].split(" ")
                    old, _, new = parts[0].partition("..")
                    h["index"] = (old, new, parts[1] if len(parts) > 1 else None)
        if b"\nGIT binary patch\n" in self.data:
            h["binary"] = True
        return h

    @property
    def binary(self) -> bool:
        return bool(self.header.get("binary"))

    @property
    def index(self) -> Optional[Tuple[str, str, Optional[str]]]:
        return self.header.get("index")

    @property
    def deleted(self) -> bool:
        return "deleted file mode" in self.header

    @property
    def added(self) -> bool:
        return "new file mode" in self.header

    def new_mode(self) -> Optional[str]:
        """The file's mode after the patch, when the section says it (None for a deletion)."""
        if self.deleted:
            return None
        for key in ("new file mode", "new mode"):
            if key in self.header:
                return self.header[key]
        idx = self.index
        return idx[2] if idx else None

    def post_id(self) -> Optional[str]:
        """The post-image blob id (abbreviated) of the `index` line; None for a deletion."""
        idx = self.index
        if not idx or self.deleted:
            return None
        return idx[1]

    def pre_id(self) -> Optional[str]:
        idx = self.index
        if not idx or self.added:
            return None
        return idx[0]


class Patch:
    """A patch split into its preamble (bytes before the first section) and sections."""

    def __init__(self, preamble: bytes, sections: List[Section]):
        self.preamble = preamble
        self.sections = sections

    @classmethod
    def parse(cls, data: bytes) -> "Patch":
        starts = []
        if data.startswith(DIFF):
            starts.append(0)
        pos = 0
        needle = b"\n" + DIFF
        while True:
            k = data.find(needle, pos)
            if k < 0:
                break
            starts.append(k + 1)
            pos = k + 1
        if not starts:
            return cls(data, [])
        sections = []
        for i, s in enumerate(starts):
            e = starts[i + 1] if i + 1 < len(starts) else len(data)
            chunk = data[s:e]
            line_end = chunk.find(b"\n")
            first = chunk if line_end < 0 else chunk[:line_end]
            sections.append(Section(header_path(first), chunk))
        return cls(data[:starts[0]], sections)

    def to_bytes(self) -> bytes:
        return self.preamble + b"".join(s.data for s in self.sections)

    def block_range(self, prefix: str) -> Tuple[int, int]:
        """The [start, end) indices of the sections under PREFIX (a folder path ending in /).
        They must be contiguous; when there are none, start == end is where they would go."""
        idx = [i for i, s in enumerate(self.sections) if s.path.startswith(prefix)]
        if not idx:
            k = 0
            while k < len(self.sections) and self.sections[k].path < prefix:
                k += 1
            return k, k
        if idx != list(range(idx[0], idx[-1] + 1)):
            gaps = [self.sections[i].path for i in range(idx[0], idx[-1] + 1)
                    if not self.sections[i].path.startswith(prefix)]
            raise NanoTreeError("the Nano block of the patch is not contiguous; sections inside it "
                                "for other paths: %s" % ", ".join(gaps[:5]))
        return idx[0], idx[-1] + 1

    def split_block(self, prefix: str) -> Tuple[bytes, bytes, bytes]:
        """(before, block, after) as bytes, so that before + block + after == to_bytes()."""
        i, j = self.block_range(prefix)
        before = self.preamble + b"".join(s.data for s in self.sections[:i])
        block = b"".join(s.data for s in self.sections[i:j])
        after = b"".join(s.data for s in self.sections[j:])
        return before, block, after

    def block_sections(self, prefix: str) -> List[Section]:
        i, j = self.block_range(prefix)
        return self.sections[i:j]

    def unsorted(self) -> List[Tuple[str, str]]:
        """Neighbouring section pairs out of git's path order (git sorts by path bytes)."""
        bad = []
        for a, b in zip(self.sections, self.sections[1:]):
            if a.path.encode("utf-8", "surrogateescape") >= b.path.encode("utf-8", "surrogateescape"):
                bad.append((a.path, b.path))
        return bad


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def splice(data: bytes, prefix: str, block: bytes) -> bytes:
    """DATA with its Nano block (sections under PREFIX) replaced by BLOCK."""
    before, _old, after = Patch.parse(data).split_block(prefix)
    return before + block + after


def first_difference(a: bytes, b: bytes) -> Optional[Tuple[int, bytes, bytes]]:
    """(line number, line in A, line in B) of the first line where A and B differ, else None."""
    if a == b:
        return None
    la = a.split(b"\n")
    lb = b.split(b"\n")
    for n, (x, y) in enumerate(zip(la, lb), 1):
        if x != y:
            return n, x, y
    n = min(len(la), len(lb)) + 1
    return n, (la[n - 1] if n <= len(la) else b"<end>"), (lb[n - 1] if n <= len(lb) else b"<end>")


def sections_by_path(sections: List[Section]) -> dict:
    return {s.path: s for s in sections}
