#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Which of Nano's assets a Linux image may carry, from ASSETS.md (decision I38).

Usage:
  assets_allowlist.py [--assets FILE] [--android-bp FILE] [--out FILE]
                      write package/thimsy-data/assets.list
  assets_allowlist.py --check [...]
                      exit 1 if the committed list differs from a fresh one
  assets_allowlist.py install --list FILE [--report FILE] [--replacements DIR] SRC DEST
                      copy the files of SRC (an asset export, laid out as
                      /system/etc) that the list allows into DEST, and the
                      replacements of denied files (below) from DIR
  assets_allowlist.py filter --list FILE [--licence FILE ...] SRC DEST
                      the same copy without replacements, for linux/build.sh
                      before it packs thimsy-data's source tarball: DEST gets
                      only what the list allows, so the tarball legal-info
                      publishes holds no REPLACE or ASK asset, and
                      DEST/.thimsy-left-out.txt names (only names) what was
                      left out, so "install" of DEST still knows which denied
                      files to replace; each --licence FILE (the repository's
                      ATTRIBUTION.md and NOTICE) is copied to
                      DEST/.thimsy-licences/<name>, which thimsy-data's
                      LICENSE_FILES names so legal-info saves them, and which
                      "install" skips like every .thimsy- file

The list is generated, never written by hand, and never mirrors Android.bp's
prebuilt_etc modules: what ships is what ASSETS.md marks KEEP. Every row of
ASSETS.md's tables is read by its "path" and "verdict" columns:
  KEEP ...           its paths become "keep" lines;
  REPLACE ..., ASK   its paths become "deny" lines, which win over any keep
                     line, so no REPLACE or ASK asset can reach an image even
                     when a keep glob also matches it;
  anything else      ("not installed", "n/a") is left out.
A sentence "<Name> counts as REPLACE" (or ASK) in the section "Changes since the
audit" turns every keep path whose file name contains <Name> into a deny line
(Akrobat today). Paths are written as ASSETS.md writes them: `ps3xmb/` and
`nano/` are in Nano's folder, `colorful/` is the Colorful theme under
/system/etc/nano_esde_themes/, and `/system/etc/...` is an installed path (only
in the folders Nano's files are installed in).
Inside a cell, a bare name of the same kind (extension) continues the
previous path's folder, "-bold.ttf" replaces the previous name's last "-" part, "{a,b}" and "000..070" expand, and
`*` is a glob within one folder. Names inside an archive (`x.zip`: `member`)
are not files of the export: a KEEP row about members adds nothing, and a
REPLACE or ASK row about members denies the whole archive.

A Nano path becomes an installed path through Android.bp's prebuilt_etc
modules: their folders and file names say where Nano looks for each file.
Android.bp is used only for that mapping; a folder it does not install is
mapped by Nano's convention (ps3xmb/X to nano_xmb/X, any other folder to
itself).

Besides ASSETS.md's images, fonts and sounds, Nano reads data files that
ASSETS.md does not cover (translations, the button-map and controller data,
Daijishou's platform definitions). DATA lists them with their licences
(ATTRIBUTION.md); they are kept unless a deny line says otherwise.
THEME_DATA does the same for the Colorful theme that linux/build.sh prepares
with scripts/colorful_theme.sh, as Android does (decision OA-1): its XML, its
README (the licence, credits and Thimsy's changes) and the licence texts the
script adds.

That theme carries one of this project's console pictures under the name of a
theme file ASSETS.md denies (quake.webp, mods/console-pictures). For each such
file, from THIMSY_PICTURES, the list has a "keep-sha256" line with the
picture's sha256: the file is installed when its bytes are exactly the
picture's, so the denied original never is.

Lines of the list: "keep<TAB>PATTERN<TAB>WHY", "deny<TAB>PATTERN<TAB>WHY" and
"keep-sha256<TAB>PATH<TAB>SHA256<TAB>WHY", PATTERN an installed path under
/system/etc in which '*' stays inside one folder and "**/" stands for any
number of folders. "install" copies a file only if a keep pattern matches it
and no deny pattern does, or a keep-sha256 line names it with its sha256; it
lists in --report every file it left out, and refuses a symlink or a path
that leaves SRC.

Some denied assets have a free replacement that Android installs under the
same name (mods/50-nano.sh): Nano's two Akrobat files become Barlow Semi
Condensed Medium and SemiBold, with Barlow's OFL text beside them, because
Nano opens those paths for theme text with no fontPath. REPLACEMENTS maps
them. With --replacements DIR (thimsy-data's downloads), each such path that
SRC has, and the list denies, is installed from DIR instead, as Android does
it; without it the path is only left out.

Exit status: 0 done, 1 an error or (--check) a stale list, 2 usage.
Standard library only; Python 3.8 and later. Generating needs the repository
(ASSETS.md, nano/Android.bp, tools/nano-tree and mods/console-pictures);
"install" needs only the list.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

LINUX = Path(__file__).resolve().parent.parent
REPO = LINUX.parent
DEFAULT_ASSETS = REPO / "ASSETS.md"
DEFAULT_BP = REPO / "nano" / "Android.bp"
DEFAULT_OUT = LINUX / "package" / "thimsy-data" / "assets.list"

HEADER = """\
# Generated by linux/tools/assets_allowlist.py from ASSETS.md (and nano/Android.bp
# for where each file is installed); do not edit. Regenerate with
#   python3 linux/tools/assets_allowlist.py
# keep: installed when a file of the asset export matches; deny: never installed,
# even when a keep line matches (decision I38); keep-sha256: installed only with
# exactly this sha256 (a Thimsy file under a denied name). Patterns are paths under
# /system/etc ('*' stays inside one folder, '**/' is any number of folders).
# thimsy-data.mk installs with
#   assets_allowlist.py install --list assets.list SRC DEST
"""

# Nano's data files that ASSETS.md (images, fonts, sounds) does not cover:
# (path in Nano's folder, why). Licences as ATTRIBUTION.md gives them.
DATA: Tuple[Tuple[str, str], ...] = (
    ("ps3xmb/i18n/*.json", "data: Nano's translations (Apache-2.0)"),
    ("ps3xmb/buttonmap/*.json", "data: the button map's pads (Apache-2.0)"),
    ("ps3xmb/controllers/controllers.json", "data: controller drawings' layout (CC BY-NC 4.0, ATTRIBUTION.md)"),
    ("ps3xmb/controllers/LICENSE", "data: the controller drawings' licence"),
    ("ps3xmb/controllers/svg/*.svg", "data: controller drawings (CC BY-NC 4.0, ATTRIBUTION.md)"),
    ("ps3xmb/daijishou/*.json", "data: Daijishou's platform definitions (MIT, ATTRIBUTION.md)"),
)

# The Colorful theme as scripts/colorful_theme.sh prepares it (decision OA-1):
# its data files, which ASSETS.md (images, fonts, sounds) does not cover, as
# (installed pattern, why).
COLORFUL = "nano_esde_themes/colorful-simplified-es-de"
THEME_DATA: Tuple[Tuple[str, str], ...] = (
    (COLORFUL + "/**/*.xml", "data: the Colorful theme's XML (CC BY-NC-SA 2.0) and the console "
     "pictures' metadata (CC BY-NC 4.0, ATTRIBUTION.md)"),
    (COLORFUL + "/README.md", "data: the theme's licence, credits and Thimsy's changes"),
    (COLORFUL + "/LICENSE*", "data: the theme's licence"),
    (COLORFUL + "/_inc/fonts/Urbanist-OFL.txt", "data: Urbanist's licence (OFL-1.1)"),
    (COLORFUL + "/_inc/fonts/Barlow-OFL.txt", "data: Barlow Semi Condensed's licence (OFL-1.1)"),
    (COLORFUL + "/_inc/systems/THIMSY-PICTURES-LICENSE.txt",
     "data: the console pictures' sources and licences (mods/console-pictures/LICENSE)"),
)
# This project's files that the prepared theme installs, as (folder in the
# repository, installed folder): one whose installed path a deny line names
# gets a keep-sha256 line with its hash.
THIMSY_PICTURES: Tuple[Tuple[str, str], ...] = (
    ("mods/console-pictures/images", COLORFUL + "/_inc/systems/images"),
)

# Denied assets installed under the same name from a free replacement, as
# mods/50-nano.sh does on Android: (installed path, file in --replacements DIR).
# REPLACEMENT_EXTRAS go in beside them when any of them is installed.
REPLACEMENTS: Tuple[Tuple[str, str], ...] = (
    ("nano_xmb/fonts/akrobat-regular.ttf", "BarlowSemiCondensed-Medium.ttf"),
    ("nano_xmb/fonts/akrobat-semibold.ttf", "BarlowSemiCondensed-SemiBold.ttf"),
)
REPLACEMENT_EXTRAS: Tuple[Tuple[str, str], ...] = (
    ("nano_xmb/fonts/Barlow-OFL.txt", "OFL.txt"),
)

CHANGES_HEADING = "changes since the audit"
COUNTS_AS_RE = re.compile(r"\b([A-Z][A-Za-z0-9-]+) counts as (REPLACE|ASK)\b")
TOKEN_RE = re.compile(r"`([^`]+)`")
RANGE_RE = re.compile(r"(\d+)\.\.(\d+)")
BRACE_RE = re.compile(r"\{([^{}]*)\}")


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class Entry:
    def __init__(self, action: str, pattern: str, why: str, sha256: str = "") -> None:
        self.action = action
        self.pattern = pattern
        self.why = why
        self.sha256 = sha256

    def line(self) -> str:
        if self.action == "keep-sha256":
            return "%s\t%s\t%s\t%s" % (self.action, self.pattern, self.sha256, self.why)
        return "%s\t%s\t%s" % (self.action, self.pattern, self.why)


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(str(path), "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


# -- patterns ----------------------------------------------------------------

def glob_regex(pattern: str) -> "re.Pattern[str]":
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:[^/]+/)*")
            i += 3
            continue
        ch = pattern[i]
        if ch == "*":
            out.append("[^/]*")
        elif ch == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(ch))
        i += 1
    return re.compile("^" + "".join(out) + "$")


def has_glob(s: str) -> bool:
    return "*" in s or "?" in s


def expand(token: str) -> List[str]:
    """Brace lists and numeric ranges (000..070, zero-padded to the first's width)."""
    mo = BRACE_RE.search(token)
    if mo:
        out = []
        for alt in mo.group(1).split(","):
            out.extend(expand(token[:mo.start()] + alt + token[mo.end():]))
        return out
    mo = RANGE_RE.search(token)
    if mo:
        a, b = mo.group(1), mo.group(2)
        width = len(a)
        lo, hi = int(a), int(b)
        if hi < lo or hi - lo > 2000:
            raise ValueError("bad range in %r" % token)
        return [x for n in range(lo, hi + 1)
                for x in expand(token[:mo.start()] + str(n).zfill(width) + token[mo.end():])]
    return [token]


# -- ASSETS.md ---------------------------------------------------------------

def split_row(line: str) -> List[str]:
    cells = []
    cur = []
    in_code = False
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|"):
        body = body[:-1]
    for ch in body:
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            cells.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    cells.append("".join(cur).strip())
    return cells


def verdict_kind(cell: str) -> Optional[str]:
    word = cell.strip().split(" ", 1)[0].strip("*").upper() if cell.strip() else ""
    if word == "KEEP":
        return "keep"
    if word in ("REPLACE", "ASK"):
        return "deny"
    return None


class Row:
    def __init__(self, line_no: int, kind: str, verdict: str, tokens: List[str],
                 archive: bool) -> None:
        self.line_no = line_no
        self.kind = kind
        self.verdict = verdict
        self.tokens = tokens
        self.archive = archive


def read_assets(text: str) -> Tuple[List[Row], List[Tuple[str, str, int]]]:
    """The table rows with a KEEP, REPLACE or ASK verdict, and the "counts as" changes."""
    rows: List[Row] = []
    changes: List[Tuple[str, str, int]] = []
    section = ""
    cols: Optional[Tuple[int, int]] = None
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip()
        if line.startswith("#"):
            section = line.lstrip("#").strip().lower()
            cols = None
            continue
        if section == CHANGES_HEADING:
            for mo in COUNTS_AS_RE.finditer(line):
                changes.append((mo.group(1), mo.group(2), n))
        if not line.lstrip().startswith("|"):
            cols = None
            continue
        cells = split_row(line)
        if all(re.match(r"^:?-+:?$", c) for c in cells if c):
            continue
        lower = [c.lower() for c in cells]
        if "path" in lower and "verdict" in lower:
            cols = (lower.index("path"), lower.index("verdict"))
            continue
        if cols is None or len(cells) <= max(cols):
            continue
        kind = verdict_kind(cells[cols[1]])
        if kind is None:
            continue
        path_cell = cells[cols[0]]
        tokens = TOKEN_RE.findall(path_cell)
        archive = bool(re.search(r"`[^`]+\.zip`\s*:", path_cell))
        rows.append(Row(n, kind, cells[cols[1]].split(" ", 1)[0].strip("*").upper(), tokens, archive))
    return rows, changes


def row_paths(row: Row, zips: Dict[str, str]) -> List[str]:
    """The paths a row names, as written (prefixes kept), expanded."""
    out: List[str] = []
    prev: Optional[str] = None
    for tok in row.tokens:
        tok = tok.strip()
        if not tok:
            continue
        if tok.endswith(".zip"):
            full = tok if "/" in tok else zips.get(tok, tok)
            if "/" in tok:
                zips[os.path.basename(tok)] = tok
            if row.archive:
                if row.kind == "deny":
                    out.append(full)
                prev = None
                break
            out.append(full)
            prev = full
            continue
        if tok.startswith("-") and prev:
            base = os.path.basename(prev)
            stem = base.rsplit("-", 1)[0] if "-" in base else os.path.splitext(base)[0]
            tok = posix_join(os.path.dirname(prev), stem + tok)
        elif "/" not in tok and prev and ext(tok) == ext(prev):
            tok = posix_join(os.path.dirname(prev), tok)
        elif "/" not in tok:
            continue  # a bare name of another kind (logo.img): not a file of that folder
        try:
            paths = expand(tok)
        except ValueError:
            paths = [tok]
        out.extend(paths)
        prev = tok
    return out


def ext(path: str) -> str:
    return os.path.splitext(path)[1].lower()


def posix_join(a: str, b: str) -> str:
    return b if not a else a.rstrip("/") + "/" + b


# -- where a path is installed --------------------------------------------------

class Mapper:
    """Nano's folder to /system/etc, from Android.bp's prebuilt_etc modules."""

    def __init__(self, files: Dict[str, str], dirs: Dict[str, str]) -> None:
        self.files = files
        self.dirs = dirs

    @classmethod
    def from_blueprint(cls, bp_path: Path) -> "Mapper":
        sys.path.insert(0, str(REPO / "tools" / "nano-tree"))
        try:
            from nanotree import androidbp  # type: ignore
        finally:
            sys.path.pop(0)
        bp = androidbp.Blueprint.load(str(bp_path))
        files: Dict[str, str] = {}
        dirs: Dict[str, str] = {}
        for m in bp.of_type("prebuilt_etc"):
            sub = m.get("sub_dir") or m.get("relative_install_path") or ""
            src = m.get("src")
            if isinstance(src, str):
                name = m.get("filename") or os.path.basename(src)
                files[src] = posix_join(sub, name)
                dirs.setdefault(os.path.dirname(src), sub)
            for s in m.strings("srcs"):
                d = os.path.dirname(s)
                if d in dirs and dirs[d] != sub:
                    raise ValueError("Android.bp installs %s/ into both %s and %s" % (d, dirs[d], sub))
                dirs[d] = sub
                if not has_glob(s):
                    files[s] = posix_join(sub, os.path.basename(s))
        return cls(files, dirs)

    def installed(self, written: str) -> Optional[str]:
        """The installed path (under /system/etc) of a path as ASSETS.md writes it."""
        if written.startswith("/system/etc/"):
            inst = written[len("/system/etc/"):]
            return inst if inst.split("/", 1)[0] in self.roots() else None
        if written.startswith("colorful/"):
            return "nano_esde_themes/colorful-simplified-es-de/" + written[len("colorful/"):]
        if written.startswith("ps3xmb/"):
            src = written
        elif written.startswith("nano/"):
            src = written[len("nano/"):]
        else:
            return None
        return self.nano_path(src)

    def roots(self) -> List[str]:
        """The folders of /system/etc that Nano's files are installed in."""
        found = {"nano_xmb", "nano_esde_themes"}
        for d in list(self.dirs.values()) + list(self.files.values()):
            if d:
                found.add(d.split("/", 1)[0])
        return sorted(found)

    def nano_path(self, src: str) -> str:
        if src in self.files:
            return self.files[src]
        d, base = os.path.dirname(src), os.path.basename(src)
        if d in self.dirs:
            return posix_join(self.dirs[d], base)
        if src.startswith("ps3xmb/"):
            return "nano_xmb/" + src[len("ps3xmb/"):]
        return src


# -- the list ----------------------------------------------------------------

def generate(assets_text: str, mapper: Mapper,
             repo: Optional[Path] = None) -> Tuple[List[Entry], List[str]]:
    """The list's entries and ASSETS.md's unmapped paths; with repo, the
    keep-sha256 lines of THIMSY_PICTURES too."""
    rows, changes = read_assets(assets_text)
    entries: List[Entry] = []
    unmapped: List[str] = []
    zips: Dict[str, str] = {}
    for row in rows:
        for written in row_paths(row, zips):
            inst = mapper.installed(written)
            if inst is None:
                unmapped.append("%s (ASSETS.md:%d %s)" % (written, row.line_no, row.verdict))
                continue
            entries.append(Entry(row.kind, inst, "ASSETS.md:%d %s" % (row.line_no, row.verdict)))
    for src, why in DATA:
        entries.append(Entry("keep", mapper.nano_path(src), why))
    for pattern, why in THEME_DATA:
        entries.append(Entry("keep", pattern, why))
    # "Changes since the audit": a font family that counts as REPLACE or ASK.
    for name, verdict, line_no in changes:
        for e in list(entries):
            if e.action == "keep" and name.lower() in os.path.basename(e.pattern).lower():
                entries.append(Entry("deny", e.pattern, "ASSETS.md:%d %s counts as %s"
                                     % (line_no, name, verdict)))
    # This project's pictures under a denied name: kept only with their own bytes.
    if repo is not None:
        denied = Allowlist(entries)
        for folder, inst_dir in THIMSY_PICTURES:
            for p in sorted((repo / folder).iterdir()):
                inst = inst_dir + "/" + p.name
                why = denied.decide(inst)[1]
                if p.is_file() and why.startswith("denied: "):
                    entries.append(Entry("keep-sha256", inst, "%s/%s, not the file %s denies"
                                         % (folder, p.name, why[len("denied: "):].split(" (", 1)[1][:-1]),
                                         file_sha256(p)))
    # One line per (action, pattern): the first reason found.
    seen = {}
    for e in entries:
        seen.setdefault((e.action, e.pattern), e)
    out = sorted(seen.values(), key=lambda e: (e.pattern, e.action))
    return out, sorted(set(unmapped))


def render(entries: Sequence[Entry], unmapped: Sequence[str]) -> str:
    lines = [HEADER.rstrip("\n")]
    for e in entries:
        lines.append(e.line())
    if unmapped:
        lines.append("# Paths of ASSETS.md that are not in Nano's asset export (other trees, AOSP")
        lines.append("# or the mods), listed so a reader sees they were read:")
        for u in unmapped:
            lines.append("#   " + u)
    return "\n".join(lines) + "\n"


def read_list(path: Path) -> List[Entry]:
    entries = []
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        parts = raw.split("\t")
        if parts[0] == "keep-sha256":
            if len(parts) < 3 or not parts[1] or has_glob(parts[1]) or not SHA256_RE.match(parts[2]):
                raise ValueError("%s:%d: not a keep-sha256 line (PATH, a sha256)" % (path, n))
            entries.append(Entry(parts[0], parts[1], parts[3] if len(parts) > 3 else "", parts[2]))
            continue
        if len(parts) < 2 or parts[0] not in ("keep", "deny") or not parts[1]:
            raise ValueError("%s:%d: not a keep or deny line" % (path, n))
        entries.append(Entry(parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    return entries


class Allowlist:
    def __init__(self, entries: Iterable[Entry]) -> None:
        entries = list(entries)
        self.keep = [(glob_regex(e.pattern), e) for e in entries if e.action == "keep"]
        self.deny = [(glob_regex(e.pattern), e) for e in entries if e.action == "deny"]
        self.exact = {e.pattern: e for e in entries if e.action == "keep-sha256"}

    def decide(self, rel: str, path: Optional[Path] = None) -> Tuple[bool, str]:
        """Whether the file at rel is installed; a keep-sha256 line needs the file (path)."""
        e = self.exact.get(rel)
        if e is not None and path is not None and file_sha256(path) == e.sha256:
            return True, "%s (sha256 %s)" % (e.pattern, e.sha256)
        for r, e in self.deny:
            if r.match(rel):
                return False, "denied: %s (%s)" % (e.pattern, e.why)
        for r, e in self.keep:
            if r.match(rel):
                return True, e.pattern
        return False, "not on the list"


def overlaps(entries: Sequence[Entry]) -> List[str]:
    """Deny paths without a glob that a keep glob would match (the deny wins; listed for review)."""
    al = Allowlist(entries)
    out = []
    for r, e in al.deny:
        if has_glob(e.pattern):
            continue
        for kr, ke in al.keep:
            if kr.match(e.pattern):
                out.append("%s: keep %s, deny wins (%s)" % (e.pattern, ke.pattern, e.why))
    return out


# -- install -----------------------------------------------------------------

LEFT_OUT = ".thimsy-left-out.txt"
LICENCES = ".thimsy-licences"


def install(list_path: Path, src: Path, dest: Path, report: Optional[Path],
            replacements: Optional[Path] = None, left_out: Optional[Path] = None) -> int:
    al = Allowlist(read_list(list_path))
    installed, left, replaced = [], [], []
    denied = set()
    src = src.resolve()
    # A filtered export (filter, below) no longer has its denied files, only their names.
    if (src / LEFT_OUT).is_file() and not (src / LEFT_OUT).is_symlink():
        for line in (src / LEFT_OUT).read_text(encoding="utf-8").splitlines():
            rel = line.split("\t", 1)[0]
            if rel and not al.decide(rel)[0]:
                denied.add(rel)
    for dirpath, dirs, files in os.walk(str(src)):
        dirs.sort()
        for name in sorted(files) + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))]:
            p = Path(dirpath) / name
            rel = p.relative_to(src).as_posix()
            if rel.startswith(".thimsy-") or rel.startswith(".stamp_") or rel in (".files-list.txt",):
                continue  # Buildroot's and this tool's own files in the build dir
            if p.is_symlink():
                print("assets_allowlist: refusing the symlink %s" % rel, file=sys.stderr)
                return 1
            ok, why = al.decide(rel, p)
            if not ok:
                left.append("%s\t%s" % (rel, why))
                denied.add(rel)
                continue
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(str(p), str(target))
            os.chmod(str(target), 0o644)
            installed.append(rel)
    if replacements is not None:
        todo = [(rel, f) for rel, f in REPLACEMENTS if rel in denied]
        if todo:
            todo += list(REPLACEMENT_EXTRAS)
        for rel, f in todo:
            p = replacements / f
            if not p.is_file() or p.is_symlink():
                print("assets_allowlist: no replacement %s in %s (for %s)" % (f, replacements, rel),
                      file=sys.stderr)
                return 1
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(str(p), str(target))
            os.chmod(str(target), 0o644)
            replaced.append("%s\t%s" % (rel, f))
    print("assets_allowlist: installed %d files into %s, left out %d, %d from replacements"
          % (len(installed), dest, len(left), len(replaced)))
    if left_out is not None:
        left_out.parent.mkdir(parents=True, exist_ok=True)
        with open(str(left_out), "w", encoding="utf-8", newline="\n") as f:
            for line in left:
                f.write(line + "\n")
    if report is not None:
        report.parent.mkdir(parents=True, exist_ok=True)
        with open(str(report), "w", encoding="utf-8", newline="\n") as f:
            for rel in installed:
                f.write("installed\t%s\n" % rel)
            for line in left:
                f.write("left out\t%s\n" % line)
            for line in replaced:
                f.write("replaced\t%s\n" % line)
    return 0


# -- main --------------------------------------------------------------------

def build(assets: Path, bp: Path, repo: Optional[Path] = None) -> str:
    """The list for ASSETS.md and Android.bp; repo (default: ASSETS.md's folder)
    holds THIMSY_PICTURES' folders."""
    mapper = Mapper.from_blueprint(bp)
    entries, unmapped = generate(assets.read_text(encoding="utf-8"), mapper,
                                 repo if repo is not None else assets.resolve().parent)
    return render(entries, unmapped)


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("install", "filter"):
        filtering = argv[0] == "filter"
        ap = argparse.ArgumentParser(prog="assets_allowlist.py " + argv[0])
        ap.add_argument("--list", required=True, type=Path)
        if filtering:
            ap.add_argument("--licence", type=Path, action="append", default=[],
                            help="a licence text for the source tarball (legal-info)")
        else:
            ap.add_argument("--report", type=Path)
            ap.add_argument("--replacements", type=Path,
                            help="the files REPLACEMENTS names (thimsy-data's downloads)")
        ap.add_argument("src", type=Path)
        ap.add_argument("dest", type=Path)
        args = ap.parse_args(argv[1:])
        try:
            if filtering:
                if args.dest.exists():
                    print("assets_allowlist: filter: %s exists" % args.dest, file=sys.stderr)
                    return 1
                names = [p.name for p in args.licence]
                if len(set(names)) != len(names):
                    print("assets_allowlist: filter: two --licence files named alike",
                          file=sys.stderr)
                    return 1
                for p in args.licence:
                    if not p.is_file() or p.is_symlink():
                        print("assets_allowlist: filter: no licence file %s" % p, file=sys.stderr)
                        return 1
                rc = install(args.list, args.src, args.dest, None, None, args.dest / LEFT_OUT)
                if rc == 0 and args.licence:
                    (args.dest / LICENCES).mkdir(parents=True, exist_ok=True)
                    for p in args.licence:
                        shutil.copyfile(str(p), str(args.dest / LICENCES / p.name))
                        os.chmod(str(args.dest / LICENCES / p.name), 0o644)
                return rc
            return install(args.list, args.src, args.dest, args.report, args.replacements)
        except (OSError, ValueError) as e:
            print("assets_allowlist: %s" % e, file=sys.stderr)
            return 1

    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--assets", type=Path, default=DEFAULT_ASSETS)
    ap.add_argument("--android-bp", type=Path, default=DEFAULT_BP)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true", help="compare with --out, write nothing")
    args = ap.parse_args(argv)
    try:
        text = build(args.assets, args.android_bp)
    except (OSError, ValueError, ImportError) as e:
        print("assets_allowlist: %s" % e, file=sys.stderr)
        return 1
    old = args.out.read_text(encoding="utf-8").replace("\r\n", "\n") if args.out.is_file() else None
    if args.check:
        if old != text:
            print("assets_allowlist: %s is %s; run linux/tools/assets_allowlist.py"
                  % (args.out, "missing" if old is None else "stale"), file=sys.stderr)
            return 1
        return 0
    if old != text:
        with open(str(args.out), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
