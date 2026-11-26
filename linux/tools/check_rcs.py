#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static checks of the Linux image's init scripts (IMPLEMENTATION D.4, E.5).

    check_rcs.py [--linux DIR] [--sh "CMD ..."] [--no-git]

Checks, for every script under linux/board/*/rootfs-overlay and the
post-build runners:

  * it parses: ``sh -n`` with every POSIX shell found (BusyBox sh, dash,
    /bin/sh; Git for Windows' sh.exe on Windows); none found is a note,
    not a pass;
  * no bashisms, from a pattern list (BusyBox ash runs them on the device);
  * no CR characters; ``#!/bin/sh`` on every executable script; every rc and
    S/D script sources the init library (the test seams live there);
  * scripts that are run, not sourced, are executable in the git index;
  * the S and D scripts are exactly those of E.5, in that order, and
    inittab points at rcS and rcK;
  * the synchronous part (rcS, S*, the library, the family hooks) starts no
    service, waits, scans or disk tool from FORBIDDEN_SYNC: plan 9.3 keeps
    rcS near 0.1 s. A line may allow one name with "# rcs-allow: NAME";
  * nothing names /dev/block/by-name (IMPLEMENTATION B.5).

Exit status 1 when a problem is found. Standard library only; Python 3.8+.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

LINUX = Path(__file__).resolve().parents[1]

OVERLAY = "board/common/rootfs-overlay"
INITD = OVERLAY + "/etc/init.d"
LIB = OVERLAY + "/usr/share/thimsy/init/lib.sh"
CONF = OVERLAY + "/etc/thimsy/init.conf"
INITTAB = OVERLAY + "/etc/inittab"

# E.5's order, refined in docs/init.md (D05family added).
S_ORDER = ["S00base", "S01props", "S02padd", "S03modules"]
D_ORDER = ["D05family", "D10cards", "D20sound", "D30net", "D35bluetooth",
           "D40time", "D50ssh", "D60firstboot"]
RC_SCRIPTS = ["rcS", "rcD", "rcK"]

# Programs the synchronous rcS path must not run (plan 9.3: nothing between
# rcS and the first frame but /storage, the props and the pad).
FORBIDDEN_SYNC = [
    "wpa_supplicant", "udhcpc", "dhcpcd", "dhclient", "ntpd", "ntpdate",
    "chronyd", "hwclock", "dropbear", "sshd", "dbus-daemon", "dbus-uuidgen",
    "bluetoothd", "bluetoothctl",
    "alsactl", "sleep", "usleep", "mdev", "udevadm", "udevd", "fsck",
    "e2fsck", "mkfs", "sfdisk", "sgdisk", "gdisk", "parted", "partx",
    "resize2fs", "find", "ldconfig", "depmod", "sync", "wait",
]
# The init library's tool variables for those programs.
FORBIDDEN_SYNC_VARS = [
    "WPA", "UDHCPC", "NTPD", "DROPBEAR", "DROPBEARKEY", "ALSACTL",
    "FSCK_EXT4", "SFDISK", "MKFS_EXFAT", "SYNC", "DBUS_DAEMON", "DBUS_UUIDGEN",
    "BLUETOOTHD",
]

BASHISMS: List[Tuple[str, str]] = [
    (r"\[\[", "[[ ]] test"),
    (r"(?:^|[\s;&|(])function\s+\w+", "'function' keyword"),
    (r"\$\{[\w@*]+:[^-=?+}]", "${var:offset} substring"),
    (r"\$\{[\w@*]+//?", "${var/pattern/repl} substitution"),
    (r"\$\{\w+(?:\^|,)", "${var^}/${var,} case change"),
    (r"\$\{!\w", "${!var} indirection"),
    (r"\$'", "$'...' quoting"),
    (r"<<<", "here-string"),
    (r"&>", "&> redirection"),
    (r"\|&", "|& pipe"),
    (r"(?:^|[\s;&|(])(?:declare|typeset|let|shopt|pushd|popd|mapfile|"
     r"readarray|source|select|disown|enable)(?=\s|$)", "bash builtin"),
    (r"(?:^|[\s;&|(])\w+=\(", "array assignment"),
    (r"(?<![$(])\(\(", "(( )) command"),
    (r"(?:^|[\s;&|(])echo\s+-[neE]+\b", "echo with flags (use printf)"),
    (r"(?<![$\w\\])\{\w+\.\.\w+\}", "{a..b} brace range"),
    (r"(?<![$\w\\])\{[^{}\s$\\]*,[^{}\s\\]*\}", "{a,b} brace expansion"),
    (r"\$\{?(?:RANDOM|PIPESTATUS|EUID|UID|BASH|BASH_\w+|FUNCNAME|"
     r"BASH_SOURCE)\b", "bash variable"),
    (r"=~", "=~ match"),
    (r"\btrap\b.*\b(?:ERR|DEBUG|RETURN)\b", "bash trap"),
    (r"(?:^|[\s;&|(])read\s+-(?!r\b)\w+", "read option other than -r"),
    (r"\[\s.*\s==\s", "== in test"),
    (r"(?:^|[\s;&|(])local\s+-\w", "local with options"),
]
_BASHISMS = [(re.compile(p), m) for p, m in BASHISMS]

_HEREDOC = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")
_ALLOW = re.compile(r"#\s*rcs-allow:\s*([\w.\-, ]+)")


class Report:
    def __init__(self) -> None:
        self.problems: List[str] = []
        self.notes: List[str] = []
        self.shells: List[List[str]] = []

    def problem(self, msg: str) -> None:
        self.problems.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    @property
    def ok(self) -> bool:
        return not self.problems


# --- discovery ---------------------------------------------------------------

def family_hooks(linux: Path) -> List[Path]:
    return sorted(linux.glob("board/*/common/rootfs-overlay/usr/share/thimsy/family/*/hook"))


def scripts(linux: Path) -> Dict[str, List[Path]]:
    """The scripts by role: sync, deferred, shutdown, sourced, build."""
    initd = linux / INITD
    roles: Dict[str, List[Path]] = {
        "sync": [], "deferred": [], "shutdown": [], "sourced": [], "build": []}
    if (initd / "rcS").exists():
        roles["sync"].append(initd / "rcS")
    roles["sync"] += sorted(p for p in initd.glob("S*") if p.is_file())
    if (initd / "rcD").exists():
        roles["deferred"].append(initd / "rcD")
    roles["deferred"] += sorted(p for p in (initd / "deferred").glob("D*") if p.is_file())
    if (initd / "rcK").exists():
        roles["shutdown"].append(initd / "rcK")
    for rel in (LIB, CONF):
        if (linux / rel).exists():
            roles["sourced"].append(linux / rel)
    board = linux / "board" / "common"
    for name in ("post-build.sh", "post-image.sh"):
        if (board / name).exists():
            roles["build"].append(board / name)
    for d in ("post-build.d", "post-image.d"):
        roles["build"] += sorted(p for p in (board / d).glob("*") if p.is_file())
    roles["build"] += sorted(p for p in linux.glob("board/*/post-build.d/*") if p.is_file()
                             and p.parent.parent.name != "common")
    roles["build"] += sorted(p for p in linux.glob("board/*/post-image.d/*") if p.is_file()
                             and p.parent.parent.name != "common")
    return roles


def executables(linux: Path, roles: Dict[str, List[Path]]) -> List[Path]:
    """Scripts that are run, not only sourced."""
    out = []
    for role in ("sync", "deferred", "shutdown", "build"):
        out += roles[role]
    out += family_hooks(linux)
    return out


def all_scripts(linux: Path, roles: Dict[str, List[Path]]) -> List[Path]:
    seen: Dict[Path, None] = {}
    for role in roles.values():
        for p in role:
            seen[p] = None
    for p in family_hooks(linux):
        seen[p] = None
    return list(seen)


def find_shells(override: Optional[str] = None) -> List[List[str]]:
    """POSIX shells available for ``sh -n`` and for running scripts."""
    if override:
        return [override.split()]
    env = os.environ.get("THIMSY_SH")
    if env:
        return [env.split()]
    shells: List[List[str]] = []
    if os.name == "nt":
        for cand in (r"C:\Program Files\Git\bin\sh.exe",
                     r"C:\Program Files\Git\usr\bin\sh.exe"):
            if os.path.exists(cand):
                shells.append([cand])
                break
        return shells
    bb = shutil.which("busybox")
    if bb:
        try:
            r = subprocess.run([bb, "sh", "-c", "true"], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=10)
            if r.returncode == 0:
                shells.append([bb, "sh"])
        except (OSError, subprocess.SubprocessError):
            pass
    dash = shutil.which("dash")
    if dash:
        shells.append([dash])
    if os.path.exists("/bin/sh"):
        real = os.path.realpath("/bin/sh")
        if not dash or os.path.realpath(dash) != real:
            shells.append(["/bin/sh"])
    return shells


# --- text ----------------------------------------------------------------------

def code_lines(text: str) -> Iterable[Tuple[int, str, str]]:
    """(line number, code without comment, comment) for lines outside here-docs."""
    lines = text.split("\n")
    i = 0
    pending: List[str] = []
    while i < len(lines):
        raw = lines[i]
        i += 1
        if pending:
            if raw.strip() == pending[0] or raw.lstrip("\t") == pending[0]:
                pending.pop(0)
            continue
        code, comment = split_comment(raw)
        yield i, code, comment
        for m in _HEREDOC.finditer(code):
            pending.append(m.group(2))


def split_comment(line: str) -> Tuple[str, str]:
    """Split a line at its first unquoted '#' that starts a word."""
    quote = ""
    esc = False
    for k, ch in enumerate(line):
        if esc:
            esc = False
            continue
        if ch == "\\" and quote != "'":
            esc = True
            continue
        if quote:
            if ch == quote:
                quote = ""
            continue
        if ch in "'\"":
            quote = ch
            continue
        if ch == "#" and (k == 0 or line[k - 1] in " \t;&|("):
            return line[:k], line[k:]
    return line, ""


def _forbidden_patterns() -> List[Tuple[str, "re.Pattern[str]"]]:
    pats = []
    for name in FORBIDDEN_SYNC:
        n = re.escape(name)
        if name in ("mkfs", "fsck"):
            n += r"(?:\.\w+)?"
        pats.append((name, re.compile(
            r"(?:^|[\s;&|(`/\"'])" + n + r"(?=$|[\s;&|)`\"'])")))
    for var in FORBIDDEN_SYNC_VARS:
        pats.append((var, re.compile(r"\$\{?" + var + r"\b")))
    return pats


_FORBIDDEN = _forbidden_patterns()


# --- checks --------------------------------------------------------------------

def rel(linux: Path, p: Path) -> str:
    try:
        return p.relative_to(linux).as_posix()
    except ValueError:
        return p.as_posix()


def check_text(linux: Path, path: Path, rep: Report, *, sync: bool, executable: bool) -> None:
    name = rel(linux, path)
    data = path.read_bytes()
    if b"\r" in data:
        rep.problem(f"{name}: CR characters (needs LF line endings)")
    text = data.decode("utf-8", errors="replace")
    if executable and not text.startswith("#!/bin/sh\n"):
        rep.problem(f"{name}: first line must be #!/bin/sh")
    for lineno, code, comment in code_lines(text):
        for rx, msg in _BASHISMS:
            if rx.search(code):
                rep.problem(f"{name}:{lineno}: bashism: {msg}: {code.strip()}")
        if "/dev/block/by-name" in code and "rootfs-overlay" in name:
            rep.problem(f"{name}:{lineno}: names /dev/block/by-name (IMPLEMENTATION B.5)")
        if sync and not re.match(r"\s*thimsy_tool\s", code):
            allowed = set()
            m = _ALLOW.search(comment)
            if m:
                allowed = {a.strip() for a in re.split(r"[ ,]+", m.group(1)) if a.strip()}
            for label, rx in _FORBIDDEN:
                if label in allowed:
                    continue
                if rx.search(code):
                    rep.problem(f"{name}:{lineno}: '{label}' on the synchronous rcS path: "
                                f"{code.strip()}")


def check_sources_lib(linux: Path, rep: Report, roles: Dict[str, List[Path]]) -> None:
    want = "/usr/share/thimsy/init/lib.sh"
    for role in ("sync", "deferred", "shutdown"):
        for p in roles[role]:
            if want not in p.read_text(encoding="utf-8", errors="replace"):
                rep.problem(f"{rel(linux, p)}: does not source {want}")


def check_order(linux: Path, rep: Report) -> None:
    initd = linux / INITD
    for name in RC_SCRIPTS:
        if not (initd / name).is_file():
            rep.problem(f"{INITD}/{name}: missing")
    s_found = sorted(p.name for p in initd.glob("S*") if p.is_file())
    d_found = sorted(p.name for p in (initd / "deferred").glob("D*") if p.is_file())
    if s_found != S_ORDER:
        rep.problem(f"{INITD}: S scripts {s_found}, expected {S_ORDER} in that order")
    if d_found != D_ORDER:
        rep.problem(f"{INITD}/deferred: D scripts {d_found}, expected {D_ORDER} in that order")
    for found, kind in ((s_found, "S"), (d_found, "D")):
        nums = [n[1:3] for n in found]
        if any(not n.isdigit() for n in nums):
            rep.problem(f"{kind} scripts need two digits after {kind}: {found}")
        if len(set(nums)) != len(nums):
            rep.problem(f"{kind} scripts share a number: {found}")
    tab = linux / INITTAB
    if not tab.is_file():
        rep.problem(f"{INITTAB}: missing")
        return
    entries = [l for l in tab.read_text(encoding="utf-8").splitlines()
               if l.strip() and not l.lstrip().startswith("#")]
    for action, target in (("sysinit", "/etc/init.d/rcS"), ("shutdown", "/etc/init.d/rcK")):
        if not any(e.split(":")[2:3] == [action] and e.split(":", 3)[3].split()[0] == target
                   for e in entries if e.count(":") >= 3):
            rep.problem(f"{INITTAB}: no '::{action}:{target}' entry")
    sysinit = [e for e in entries if e.count(":") >= 3 and e.split(":")[2] == "sysinit"]
    if len(sysinit) != 1:
        rep.problem(f"{INITTAB}: {len(sysinit)} sysinit entries, expected 1 (rcS)")


def git_modes(linux: Path) -> Optional[Dict[str, str]]:
    git = shutil.which("git")
    if not git:
        return None
    try:
        r = subprocess.run([git, "-C", str(linux), "ls-files", "-s", "-z"],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    modes: Dict[str, str] = {}
    for rec in r.stdout.decode("utf-8", errors="replace").split("\0"):
        if not rec:
            continue
        meta, _, path = rec.partition("\t")
        modes[path] = meta.split()[0]
    return modes


def check_modes(linux: Path, rep: Report, exes: Sequence[Path]) -> None:
    modes = git_modes(linux)
    if modes is None:
        rep.note("git index not readable: executable bits not checked")
        return
    for p in exes:
        r = rel(linux, p)
        mode = modes.get(r)
        if mode is None:
            rep.problem(f"{r}: not in the git index")
        elif mode != "100755":
            rep.problem(f"{r}: mode {mode} in the git index, needs 100755 "
                        f"(git update-index --chmod=+x)")


def shell_path(p: Path) -> str:
    return p.as_posix()


def check_syntax(linux: Path, rep: Report, files: Sequence[Path],
                 shells: Sequence[Sequence[str]]) -> None:
    if not shells:
        rep.note("no POSIX shell found: sh -n not run")
        return
    for sh in shells:
        for p in files:
            try:
                r = subprocess.run(list(sh) + ["-n", shell_path(p)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=30)
            except (OSError, subprocess.SubprocessError) as e:
                rep.note(f"{' '.join(sh)}: could not run ({e})")
                break
            if r.returncode != 0:
                out = r.stdout.decode("utf-8", errors="replace").strip()
                rep.problem(f"{rel(linux, p)}: {' '.join(sh)} -n failed: {out}")


def check(linux: Path = LINUX, *, shells: Optional[Sequence[Sequence[str]]] = None,
          use_git: bool = True) -> Report:
    linux = Path(linux)
    rep = Report()
    roles = scripts(linux)
    exes = executables(linux, roles)
    files = all_scripts(linux, roles)
    exe_set = set(exes)
    sync_set = set(roles["sync"]) | {linux / LIB} | set(family_hooks(linux))
    for p in files:
        check_text(linux, p, rep, sync=p in sync_set, executable=p in exe_set)
    check_sources_lib(linux, rep, roles)
    check_order(linux, rep)
    if use_git:
        check_modes(linux, rep, exes)
    else:
        rep.note("executable bits not checked (--no-git)")
    if shells is None:
        shells = find_shells()
    rep.shells = [list(s) for s in shells]
    check_syntax(linux, rep, files, shells)
    return rep


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--linux", type=Path, default=LINUX, help="the linux/ tree")
    ap.add_argument("--sh", help="the shell command for sh -n (default: every one found)")
    ap.add_argument("--no-git", action="store_true", help="skip the git index modes")
    args = ap.parse_args(argv)
    shells = find_shells(args.sh)
    rep = check(args.linux, shells=shells, use_git=not args.no_git)
    for s in rep.shells:
        print(f"sh -n with: {' '.join(s)}")
    for n in rep.notes:
        print(f"note: {n}")
    for p in rep.problems:
        print(f"error: {p}")
    print(f"check_rcs: {len(rep.problems)} problem(s)")
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
