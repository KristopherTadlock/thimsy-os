"""packages/linux/package.mk: the family's case branch as text, PKG_VERSION, patch dirs.

The file is bash, sourced by ROCKNIX's build; here it is only read. The importer copies
what ROCKNIX *does*, not what its code appears to mean. One example from the real file:

    [[ "${DEVICE}" =~ "RK3326|RK3399|S922X|H700" ]] && PKG_PATCH_DIRS+=" rocknix-joypad"

The right-hand side is quoted, and bash matches a quoted `=~` operand literally, so the
condition is true only when the device name contains that whole string: never. The
evaluator below implements exactly that, and any PKG_PATCH_DIRS line whose shape it does
not know stops the import instead of being guessed at.

The blocks the importer translates or depends on are returned by `guard_blocks` and hashed
by guards.py, so a change to any of them stops the next import for a fresh hand-port.
"""

from __future__ import annotations

import fnmatch
import re
from collections import OrderedDict

from .options import _value
from .rocknix import ImportError_

_FUNC = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\(\)\s*\{\s*$")
_CASE = re.compile(r"^case\s+\"?\$\{?DEVICE\}?\"?\s+in\s*$")
_LABEL = re.compile(r"^\s*([A-Za-z0-9_|*.-]+)\)\s*$")
_ASSIGN = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)(\+?=)(.*)$")
_ONELINER = re.compile(r"^\s*(\[\[?\s.*?\s\]\]?)\s*&&\s*PKG_PATCH_DIRS(\+?=)(.*)$")


class Block:
    """A piece of ROCKNIX text the importer depends on."""

    def __init__(self, name: str, path: str, lines: list, line: int, kind: str):
        self.name = name
        self.path = path
        self.lines = lines
        self.line = line      # 1-based line of the first line, informational
        self.kind = kind

    @property
    def text(self) -> str:
        return "\n".join(self.lines) + "\n"


def glob_to_regex(pat: str) -> str:
    out = []
    for ch in pat:
        if ch == "*":
            out.append(".*")
        elif ch == "?":
            out.append(".")
        else:
            out.append(re.escape(ch))
    return "".join(out)


_EXP = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?:(//?)([^/}]*)/([^}]*))?\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def expand(s: str, env: dict) -> str:
    """${VAR}, $VAR and ${VAR/glob/repl} (first, longest match) as bash does them."""
    if "$(" in s or "`" in s:
        raise ImportError_("command substitution in %r: not a shape the importer reads" % s)

    def sub(m):
        name = m.group(1) or m.group(5)
        if name not in env:
            raise ImportError_("unknown variable ${%s} in %r" % (name, s))
        val = env[name]
        if m.group(2):
            rx = glob_to_regex(m.group(3))
            val = re.sub(rx, m.group(4), val, count=0 if m.group(2) == "//" else 1)
        return val

    out = _EXP.sub(sub, s)
    if "${" in out:
        raise ImportError_("unsupported parameter expansion in %r" % s)
    return out


def eval_test(cond: str, env: dict) -> bool:
    """A single `[[ "${DEVICE}" OP RHS ]]` or `[ "${DEVICE}" = "X" ]` test, bash semantics."""
    c = cond.strip()
    double = c.startswith("[[")
    inner = c[2:-2] if double else c[1:-1]
    inner = inner.strip()
    if "&&" in inner or "||" in inner or " -o " in inner or " -a " in inner:
        raise ImportError_("compound test not supported: %s" % cond)
    m = re.match(r'^"?\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?"?\s+(=~|==|=|!=)\s+(.+)$', inner)
    if not m:
        raise ImportError_("test shape not supported: %s" % cond)
    lhs = env.get(m.group(1))
    if lhs is None:
        raise ImportError_("unknown variable in test: %s" % cond)
    op, rhs = m.group(2), m.group(3).strip()
    quoted = len(rhs) >= 2 and rhs[0] == rhs[-1] and rhs[0] in "\"'"
    if quoted:
        rhs = rhs[1:-1]
    if op == "=~":
        if quoted:
            return rhs in lhs          # a quoted operand matches literally, anywhere
        return re.search(rhs, lhs) is not None
    if op in ("==", "="):
        if double and not quoted:
            return fnmatch.fnmatchcase(lhs, rhs)
        return lhs == rhs
    if op == "!=":
        if double and not quoted:
            return not fnmatch.fnmatchcase(lhs, rhs)
        return lhs != rhs
    raise ImportError_("operator %s not supported" % op)


class LinuxMk:
    def __init__(self, text: str, path: str, require_case: bool = True):
        self.path = path
        self.require_case = require_case
        self.lines = text.splitlines()
        self.func_of: list = [None] * len(self.lines)   # enclosing function per line
        self.functions: "OrderedDict[str, tuple]" = OrderedDict()
        self.case_span = None
        self._scan()

    # -- structure ---------------------------------------------------------------------
    def _scan(self):
        cur, start = None, 0
        for i, line in enumerate(self.lines):
            if cur is None:
                m = _FUNC.match(line)
                if m:
                    cur, start = m.group(1), i
                    self.func_of[i] = cur
                    continue
                if _CASE.match(line):
                    if self.case_span is not None:
                        raise ImportError_("%s: two top-level `case ${DEVICE}` blocks" % self.path)
                    end = i
                    while end < len(self.lines) and self.lines[end].strip() != "esac":
                        end += 1
                    if end == len(self.lines):
                        raise ImportError_("%s: case without esac" % self.path)
                    self.case_span = (i, end)
            else:
                self.func_of[i] = cur
                if line.rstrip() == "}":
                    self.functions[cur] = (start, i)
                    cur = None
        if cur is not None:
            raise ImportError_("%s: function %s() not closed" % (self.path, cur))
        if self.case_span is None and self.require_case:
            raise ImportError_("%s: no top-level `case ${DEVICE} in`" % self.path)

    def branches(self) -> list:
        """[(labels, first_body_line, end_line)] of the top-level case."""
        a, b = self.case_span
        out = []
        i = a + 1
        while i < b:
            m = _LABEL.match(self.lines[i])
            if m:
                j = i + 1
                while j < b and self.lines[j].strip() != ";;":
                    j += 1
                out.append((m.group(1).split("|"), i, j))
                i = j + 1
            else:
                if self.lines[i].strip() and not self.lines[i].strip().startswith("#"):
                    raise ImportError_("%s:%d: unexpected line in case: %s"
                                       % (self.path, i + 1, self.lines[i].strip()))
                i += 1
        return out

    def family_branch(self, family: str):
        hits = [br for br in self.branches() if any(fnmatch.fnmatchcase(family, p) for p in br[0])]
        if not hits:
            raise ImportError_("%s: no case branch for %s" % (self.path, family))
        return hits[0]   # bash takes the first matching branch

    def branch_assignments(self, family: str) -> list:
        labels, a, b = self.family_branch(family)
        out = []
        for i in range(a + 1, b):
            s = self.lines[i].strip()
            if not s or s.startswith("#"):
                continue
            m = _ASSIGN.match(self.lines[i])
            if not m:
                raise ImportError_("%s:%d: case branch line not understood: %s" % (self.path, i + 1, s))
            val, _dyn = _value(m.group(3))
            out.append((m.group(1), m.group(2), val, i))
        return out

    # -- facts -------------------------------------------------------------------------
    def version(self, family: str) -> "OrderedDict[str, str]":
        env = {"PKG_NAME": "linux", "DEVICE": family}
        facts: "OrderedDict[str, str]" = OrderedDict()
        for key, op, val, _i in self.branch_assignments(family):
            if key == "PKG_PATCH_DIRS":
                continue
            if op != "=":
                raise ImportError_("%s: %s%s in the %s branch" % (self.path, key, op, family))
            facts[key] = val
        if "PKG_VERSION" not in facts:
            raise ImportError_("%s: no PKG_VERSION for %s" % (self.path, family))
        env["PKG_VERSION"] = facts["PKG_VERSION"]
        out: "OrderedDict[str, str]" = OrderedDict()
        out["version"] = facts["PKG_VERSION"]
        out["sha256"] = facts.get("PKG_SHA256", "")
        out["url"] = expand(facts.get("PKG_URL", ""), env)
        if "PKG_GIT_CLONE_BRANCH" in facts:
            out["git_branch"] = facts["PKG_GIT_CLONE_BRANCH"]
        return out

    def patch_dirs(self, family: str, linux_var: str) -> list:
        """PKG_PATCH_DIRS after the file's top-level lines run for `family`."""
        version = self.version(family)["version"]
        env = {"LINUX": linux_var, "DEVICE": family, "PKG_NAME": "linux", "PKG_VERSION": version}
        dirs = None
        ca, cb = self.case_span
        for i, line in enumerate(self.lines):
            if self.func_of[i] is not None or "PKG_PATCH_DIRS" not in line:
                continue
            s = line.strip()
            if s.startswith("#"):
                continue
            if ca <= i <= cb:
                continue   # handled with the branch below, at the case's position
            m = _ONELINER.match(line)
            if m:
                if dirs is None:
                    raise ImportError_("%s:%d: append before assignment" % (self.path, i + 1))
                if eval_test(m.group(1), env):
                    val, _ = _value(m.group(3))
                    dirs = (dirs + " " if m.group(2) == "+=" else "") + expand(val, env)
                continue
            m = _ASSIGN.match(line)
            if m and m.group(1) == "PKG_PATCH_DIRS":
                val, _ = _value(m.group(3))
                ex = expand(val, env)
                dirs = ex if m.group(2) == "=" else (dirs or "") + ex
                continue
            raise ImportError_("%s:%d: PKG_PATCH_DIRS line not understood: %s" % (self.path, i + 1, s))
        # the case statement sits after the one-liners in the real file; apply it now
        for key, op, val, i in self.branch_assignments(family):
            if key != "PKG_PATCH_DIRS":
                continue
            ex = expand(val, env)
            dirs = ex if op == "=" else (dirs or "") + ex
        if dirs is None:
            raise ImportError_("%s: PKG_PATCH_DIRS never set" % self.path)
        if any(i > cb for i, l in enumerate(self.lines)
               if self.func_of[i] is None and "PKG_PATCH_DIRS" in l and not l.strip().startswith("#")):
            raise ImportError_("%s: PKG_PATCH_DIRS changed after the case block" % self.path)
        return [w for w in dirs.split() if w]

    # -- if/elif branches --------------------------------------------------------------
    def if_branches(self) -> list:
        """Every if/elif/else branch: (header_index, end_index_exclusive, depth)."""
        out = []
        stack = []   # each: [header_index]
        for i, line in enumerate(self.lines):
            s = line.strip()
            if s.startswith("#"):
                continue
            if re.match(r"^if\s", s) and not re.search(r";\s*fi\s*$", s):
                stack.append(i)
            elif re.match(r"^(elif\s|else\b)", s) and stack:
                out.append((stack[-1], i, len(stack)))
                stack[-1] = i
            elif re.match(r"^fi\b", s) and stack:
                out.append((stack.pop(), i, len(stack) + 1))
        if stack:
            raise ImportError_("%s: unbalanced if/fi" % self.path)
        out.sort()
        return out

    def _block(self, name, a, b, kind) -> Block:
        lines = [l.rstrip() for l in self.lines[a:b]]
        return Block(name, self.path, lines, a + 1, kind)

    def family_if_blocks(self, family: str) -> list:
        quoted = '"%s"' % family
        counts: dict = {}
        out = []
        for a, b, _d in self.if_branches():
            head = self.lines[a]
            if quoted not in head:
                continue
            where = self.func_of[a] or "top"
            counts[where] = counts.get(where, 0) + 1
            kind = "elif" if head.strip().startswith("elif") else "if"
            out.append(self._block("%s:%s:%s#%d" % (self.path, where, kind, counts[where]), a, b, "if"))
        return out

    def extra_firmware_block(self, family: str):
        for blk in self.family_if_blocks(family):
            if ":pre_make_target:" in blk.name and "CONFIG_EXTRA_FIRMWARE" in blk.text:
                return blk
        return None

    def cfg80211_blocks(self) -> list:
        out = []
        n = 0
        for a, b, _d in self.if_branches():
            if "CONFIG_CFG80211=y" in self.lines[a]:
                n += 1
                where = self.func_of[a] or "top"
                out.append(self._block("%s:%s:cfg80211#%d" % (self.path, where, n), a, b, "cfg80211"))
        return out

    def function_block(self, name: str):
        if name not in self.functions:
            return None
        a, b = self.functions[name]
        return self._block("%s:%s()" % (self.path, name), a, b + 1, "function")

    def guard_blocks(self, family: str, cfg80211: bool) -> list:
        labels, a, b = self.family_branch(family)
        blocks = [self._block("%s:case:%s" % (self.path, family), a + 1, b, "case")]
        pdl = [i for i, l in enumerate(self.lines)
               if self.func_of[i] is None and "PKG_PATCH_DIRS" in l
               and not l.strip().startswith("#")
               and not (self.case_span[0] <= i <= self.case_span[1])]
        blocks.append(Block("%s:patch-dirs" % self.path, self.path,
                            [self.lines[i].rstrip() for i in pdl], (pdl[0] + 1) if pdl else 0, "patch-dirs"))
        blocks.extend(self.family_if_blocks(family))
        pp = self.function_block("post_patch")
        if pp is not None:
            blocks.append(pp)
        if cfg80211:
            blocks.extend(self.cfg80211_blocks())
        return blocks

    def handport(self, family: str) -> list:
        """Family blocks the importer does not translate: the hardware recipe ports them."""
        out = []
        for blk in self.family_if_blocks(family):
            t = blk.text
            if ":pre_make_target:" in blk.name and "CONFIG_EXTRA_FIRMWARE" in t:
                continue   # translated by firmware.py
            if "generic-dsi" in t and "post_patch" in blk.name:
                what = "copies generic-dsi's panel-generic-dsi.c into drivers/gpu/drm/panel and adds it to the Makefile"
            elif "PKG_DEPENDS_UNPACK" in t:
                what = "adds unpack-time dependencies (%s)" % ", ".join(
                    sorted(set(re.findall(r'PKG_DEPENDS_UNPACK\+="\s*([^"]+)"', t))))
            elif "makeinstall_target" in blk.name:
                what = "installs DTBs under device_trees/"
            else:
                what = "family-specific shell block"
            out.append(OrderedDict([("block", blk.name), ("does", what)]))
        return out


def unpack_patch_block(text: str | None, path: str):
    """The patch loop of scripts/unpack that fixes the series order, or None if absent."""
    if text is None:
        return None
    lines = text.splitlines()
    try:
        a = next(i for i, l in enumerate(lines) if l.strip() == 'PATCH_DIRS_PKG=""')
    except StopIteration:
        raise ImportError_("%s: no PATCH_DIRS_PKG block" % path)
    b = a
    while b < len(lines) and not lines[b].rstrip().endswith("; do"):
        b += 1
    if b == len(lines):
        raise ImportError_("%s: patch loop not found" % path)
    return Block("%s:patch-order" % path, path, [l.rstrip() for l in lines[a:b + 1]], a + 1, "unpack")
