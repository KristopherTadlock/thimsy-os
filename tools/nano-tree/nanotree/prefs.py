"""The settings schema inventory (portable_settings_plan.md §3.3 and §7.1 phase 0): every setting key
Nano reads or writes, found by scanning nano/'s C and C++ sources, and its row in
tools/nano-tree/prefs.json.

What the scan finds, in every C and C++ source under nano/ (Android-compiled and fork-only), tests and
the host build's fake platform left out:
- property keys: string literals matching ratchets.PROP_KEY (`persist.gammaos.nano.iptv`), key
  prefixes ending in "." (`persist.gammaos.nano.screencal.`) and printf templates
  (`persist.gammaos.gamepad.pa%d_pkg`);
- keys inside shell strings: `setprop K V`, `getprop K`;
- keys packed into a chooser spec, "K|v1:l1,v2:l2|Title" (QA_SHADER_OPT_MENU's spec, which
  openShaderOptChooser splits at '|'): the key and its options. A string that starts with a key and
  goes on after another separator ("K=...", "K,...") is a packed form the scan does not read: a problem;
- Android settings-provider keys, written `global:K`, `secure:K` and `system:K`: settings-table rows
  (`SettingSource::kGlobal`, `"airplane_mode_on"`) and `settings get|put <namespace> K` shell strings.
  `SettingSource::kProp` rows are property keys.

Keys built at run time are found through RUNTIME_FAMILIES, which says how each family is built:
- a key prefix joined to a field of a table (`"persist.gammaos.shader." + defs[i].suffix` over the
  GsParamDef arrays): the scan reads the table's rows and lists each key;
- a template whose prefix comes at run time (`"%sact%d_code"` with actScopePrefix()'s prefixes,
  `"%s_%d"` with the base keys passed to the chunked-list helpers): the scan lists each prefix with
  each template.
Three checks keep the families honest. A `"%s..."` key template the families do not claim is a problem
(`"%sact%d_code"`, `"%s.top"`, `"%s.%s"`). So is a key literal not ending in "." joined to a run-time value
with + (`"persist.gammaos.nano.slot" + std::to_string(i)`), unless it is a family's prefix in that file.
And so is a key prefix (a key literal ending in ".") with an exact row and no family. A new family is
declared in RUNTIME_FAMILIES, or covered by a glob row. The template check reads every file that could
hold such a template (_TEMPLATE_HINT), not only the files that name a key (_WORTH): a helper that builds
keys from a prefix passed in by another file names no key itself.

Init and shell scripts under nano/ (`gammaos-nano.rc`, `thimsy-dexopt.sh` and the like) are scanned
for `property:K`, `setprop K` and `getprop K` in Nano's namespaces (PROP_NAMESPACES), comments left
out. Keys in other namespaces there (`bluetooth.profile.*`, `apexd.status`) are Android's and left out.

For each key the scan keeps the files, how it is used (get, set, a settings row, built from a family,
other) and the default values the code passes (the default argument of property_get and its kin, a
settings row's default and options, and a family table's default column).

Glob rows take in any new key under them. For the persist.* globs (the namespaces that can hold a
pref) the covered keys are listed in tools/nano-tree/prefs-globs.txt, so a new key there is reviewed
(glob_lines).

On Linux, libthimsy's ConfigStore serves the same property names (nano/compat/src/properties.cpp, no
renaming), so a property key's Linux equivalent is the key itself. Linux-only keys
(`persist.thimsy.wifi.enabled`) come from the fork-only files and are scanned like the rest. The
settings-provider keys have no Linux store (platform/linux/ShellGate.cpp refuses `settings`): a
portable one says in its "linux" field where Linux will keep it.

Other Linux components keep keys in the same store: thimsy-session (`persist.thimsy.audio.volume`) and
the init scripts (`thimsy_persist persist.thimsy.time.ntp 1` in D40time). With linux_root, scan() also
reads the C and C++ sources under linux/src (tests left out; the same checks as nano/'s, with no
runtime families) and the shell scripts in linux/board's root file system overlays: `property:K`, and
the KEY of thimsy_persist, thimsy_ro, thimsy_build_prop, setprop and getprop, bare or quoted. A KEY the
scan cannot read is a problem: a variable (`thimsy_persist "$KEY" 1`), a key with a $-expansion in it
(`persist.thimsy.bar.$x`), or persist.* text in a grep, sed or awk line (`grep "^persist.thimsy.qux=1"`).
Their files are named from the repository root ("linux/src/..."); nano/'s from nano/. A pref-namespace
key that only those files name has an exact row with an "owner": the component that keeps it.
Not scanned: libthimsy/tools/thimsy-prop.cpp and linux/board/common/post-build.d/20-build-props.sh.
They write only ro.* keys (device.json's scalars, build.props), all under the ro.* glob row (class R).
"""
from __future__ import annotations

import json
import os
import re
from typing import Dict, Iterator, List, Optional, Set

from .config import Upstream
from .ratchets import PROP_KEY, PROP_NAMESPACES, SOURCE_EXT

CLASSES = ("P", "M", "A", "L", "E", "D", "S", "R", "?")
PORTABLE = ("P", "M", "A", "L")
TYPES = ("bool", "int", "float", "enum", "string", "list", "color", "path", "url")
ENCODINGS = ("1/0", "true/false")  # how a bool is stored in the device store
SETTINGS_NS = {"kGlobal": "global", "kSecure": "secure", "kSystem": "system"}
SKIP_DIRS = {"test", "tests", "testdata"}
SKIP_PATHS = ("platform/host/",)  # the host build's fake platform: test code, never on a device
SCRIPT_EXT = (".rc", ".sh")
# Namespaces that never hold a pref (not persistent, read-only or init's): their globs need no baseline.
NON_PREF_NAMESPACES = tuple(ns + "." for ns in PROP_NAMESPACES if ns != "persist")

# Keys built at run time (see the module docstring). Each family names the file that builds the keys
# and one way to list them:
#   prefix + array/array_type + field [+ suffixes]: prefix + the string in column FIELD of each row of
#     the array named ARRAY (or of every array of type ARRAY_TYPE) in FILE, then each of SUFFIXES;
#     default_field: the column holding the key's default;
#   templates + prefixes: each "%s..." literal in FILE starting with TEMPLATES, its %s replaced by
#     each of PREFIXES (the part of a prefix before any "%" must be a key literal in FILE);
#   templates + bases_from: the same, with the prefixes taken from the first argument of every call
#     to the functions in BASES_FROM (a key literal, or a name given a key literal in the same file;
#     PARAMS are the helpers' own parameter names, skipped).
RUNTIME_FAMILIES = (
    {"name": "shader parameters", "file": "NanoMenuPS3Menu.cpp", "prefix": "persist.gammaos.shader.",
     "array_type": "GsParamDef", "field": 0},
    {"name": "hotkey buttons", "file": "NanoMenuButtonMap.cpp", "prefix": "persist.gammaos.hotkeys.",
     "array": "kHot", "field": 0, "default_field": 1},
    {"name": "scraper extra media", "file": "NanoScraper.cpp", "prefix": "persist.gammaos.scraper.media.",
     "array": "kExtraMedia", "field": 0},
    {"name": "composer BCSH per display", "file": "NanoScreenCal.cpp", "prefix": "persist.vendor.",
     "array": "kField", "field": 0, "suffixes": (".main", ".aux")},
    {"name": "button-action rules", "file": "NanoMenuPS3Menu.cpp", "templates": "%sact",
     "prefixes": ("persist.gammaos.gamepad.", "persist.gammaos.gamepad.pa%d_")},
    {"name": "chunked list continuations", "file": "NanoMenuPS3Menu.cpp", "templates": "%s_%d",
     "bases_from": ("nanoPkgListRead", "nanoPkgListHas", "nanoPkgListSet", "qsSpecList"),
     "params": ("baseProp", "base")},
)
# A printf template whose prefix comes at run time: "%s" then key characters ("%sact", "%s.top") and
# %d, %u or %s fields ("%s_%d", "%s.%s").
RUNTIME_TEMPLATE = re.compile(r"%s[_A-Za-z.](?:[A-Za-z0-9_.\-]|%0?\d*[dus])*\Z")

# A printf template of a key: a property key with %s, %d, %u or %02d style fields in it.
PROP_TEMPLATE = re.compile(r"(?:%s)\.(?:[A-Za-z0-9_\-]|%%0?\d*[sdu])+(?:\.(?:[A-Za-z0-9_\-]|%%0?\d*[sdu])+)*\.?\Z"
                           % "|".join(PROP_NAMESPACES))
_KEYCHARS = r"(?:%s)\.[A-Za-z0-9_.\-]+" % "|".join(PROP_NAMESPACES)
SHELL_PROP = re.compile(r"(?:^|[\s;&|(`])(setprop|getprop)\s+(%s)" % _KEYCHARS)
SHELL_SETTINGS = re.compile(r"(?:^|[\s;&|(`])settings\s+(get|put)\s+(global|secure|system)\s+([A-Za-z0-9_.]+)")
# A chooser spec packed into one string: "key|v1:l1,v2:l2,...|title" (QA_SHADER_OPT_MENU's it.value;
# openShaderOptChooser and shaderOptCurrentLabel take the part before the first '|' as the key).
PACKED_SPEC = re.compile(r"(%s)\|([^|]*)" % _KEYCHARS)
# A string that starts with a key and goes on after a separator the scan does not read ("key=1",
# "key,other", "key:..."): another packed form. Prose ("ro.x is not set") goes on after a space.
PACKED_OTHER = re.compile(r"(%s)[^A-Za-z0-9_.\-%%\s|]" % _KEYCHARS)

# Calls that read a key, and the index of their default argument (None: no default).
GET_DEFAULT_ARG = {
    "property_get": 2, "property_get_bool": 1, "property_get_int32": 1, "property_get_int64": 1,
    "GetProperty": 1, "GetBoolProperty": 1, "GetIntProperty": 1, "GetUintProperty": 1,
    "drastic_settings::get": 2, "drastic_settings::getInt": 1, "drastic_settings::getBool": 1,
    "propF": 1, "propFloat": 1, "propInt": 1, "propBool": 1, "rf": 1, "rd": 2, "num": 1,
    "__system_property_find": None, "prop": 1, "propEq": None,
}
SET_CALLS = ("property_set", "SetProperty", "drastic_settings::set")

_TOKEN = re.compile(r"""
    (?P<comment>//[^\n]*|/\*.*?(?:\*/|\Z))
  | (?<![A-Za-z0-9_])(?:u8|u|U|L)?R"(?P<delim>[^()\\\s"]{0,16})\((?P<raw>.*?)\)(?P=delim)"
  | (?:u8|u|U|L)?"(?P<str>(?:[^"\\\n]|\\.)*)"?
  | '(?:[^'\\\n]|\\.)*'?
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*(?:\s*::\s*[A-Za-z_][A-Za-z0-9_]*)*)
  | (?P<num>-?\d[\w.+\-]*)
  | (?P<punct>[(){}\[\],;])
  | (?P<ws>\s+)
  | [^\w\s"'/(){}\[\],;]+
  | .
""", re.S | re.X)


# A file worth tokenising names a key in a literal, a settings row or a shell property command.
_WORTH = re.compile(r'"(?:%s)\.|SettingSource::|setprop|getprop|settings (?:get|put)' % "|".join(PROP_NAMESPACES))
# A file that may hold a "%s..." template (RUNTIME_TEMPLATE), whether or not it names a key: "%s" then a
# key character or ".", a line continuation, or the end of a literal joined to the next ("%s" "_%d").
# Only a "%s" spelled with an escape ("\x25s") or split across literals ("%" "s_%d") gets past it.
_TEMPLATE_HINT = re.compile(r'%s(?:[_A-Za-z.]|\\\r?\n|"\s*(?:u8|u|U|L)?R?")')


class Tok(tuple):
    """(kind, text, line): kind is "s" (a string, adjacent literals joined), "i" (an identifier, with
    any :: scope), "n" (a number), "p" (one of ( ) { } [ ] , ;), "c" (a character) or "o" (other)."""
    kind = property(lambda self: self[0])
    text = property(lambda self: self[1])
    line = property(lambda self: self[2])


def tokens(text: str) -> List[Tok]:
    out: List[Tok] = []
    line = 1
    for m in _TOKEN.finditer(text):
        tok = m.group(0)
        if m.group("raw") is not None:
            s = m.group("raw")
        elif m.group("str") is not None:
            s = m.group("str")
        else:
            s = None
        if s is not None:
            if out and out[-1].kind == "s":
                prev = out.pop()
                out.append(Tok(("s", prev.text + s, prev.line)))
            else:
                out.append(Tok(("s", s, line)))
        elif m.group("id") is not None:
            out.append(Tok(("i", re.sub(r"\s+", "", tok), line)))
        elif m.group("num") is not None:
            out.append(Tok(("n", tok, line)))
        elif m.group("punct") is not None:
            out.append(Tok(("p", tok, line)))
        elif m.group("comment") is not None or m.group("ws") is not None:
            pass
        elif tok.startswith("'"):
            out.append(Tok(("c", tok, line)))
        else:
            out.append(Tok(("o", tok, line)))
        line += tok.count("\n")
    return out


def call_args(toks: List[Tok], open_idx: int, limit: int = 400) -> Optional[List[List[Tok]]]:
    """The arguments of the call whose "(" is at OPEN_IDX (or the items of the "{" list there), each a
    token list; None if unbalanced within LIMIT tokens."""
    depth = 0
    args: List[List[Tok]] = [[]]
    for j in range(open_idx, min(len(toks), open_idx + limit)):
        t = toks[j]
        if t.kind == "p" and t.text in "([{":
            depth += 1
            if depth == 1:
                continue
        elif t.kind == "p" and t.text in ")]}":
            depth -= 1
            if depth == 0:
                return args
        elif t.kind == "p" and t.text == "," and depth == 1:
            args.append([])
            continue
        args[-1].append(t)
    return None


def literal(arg: List[Tok]) -> Optional[str]:
    """A default argument's value when it is one literal (a string, a number, true/false)."""
    if len(arg) == 1:
        t = arg[0]
        if t.kind == "s" or t.kind == "n":
            return t.text
        if t.kind == "i" and t.text in ("true", "false"):
            return t.text
    if len(arg) == 2 and arg[0].kind == "o" and arg[0].text == "-" and arg[1].kind == "n":
        return "-" + arg[1].text
    return None


def _short(name: str) -> str:
    for p in ("android::base::", "android::", "base::", "::"):
        if name.startswith(p):
            name = name[len(p):]
    return name


def is_key(s: str) -> bool:
    return bool(PROP_KEY.match(s) or (("%" in s) and PROP_TEMPLATE.match(s)))


class Use:
    def __init__(self) -> None:
        self.files: Set[str] = set()
        self.ops: Set[str] = set()
        self.defaults: Set[str] = set()
        self.options: List[str] = []
        self.calls: Set[str] = set()
        self.where: List[str] = []

    def to_json(self) -> dict:
        return {"files": sorted(self.files), "ops": sorted(self.ops), "defaults": sorted(self.defaults),
                "options": self.options, "calls": sorted(self.calls), "where": self.where[:6]}


class Context:
    """What the runtime families need from the scan: "%s..." templates, key literals joined with +,
    helper calls, named key literals and each file's tokens."""

    def __init__(self) -> None:
        self.templates: Dict[str, Dict[str, int]] = {}      # file -> {template: first line}
        self.joins: Dict[str, Dict[str, int]] = {}          # file -> {key joined with +: first line}
        self.consts: Dict[str, Dict[str, str]] = {}         # file -> {name: key literal}
        self.toks: Dict[str, List[Tok]] = {}


def _family_files() -> Set[str]:
    return {f["file"] for f in RUNTIME_FAMILIES}


def _base_calls() -> Set[str]:
    return {fn for f in RUNTIME_FAMILIES for fn in f.get("bases_from", ())}


def _joined(toks: List[Tok], i: int) -> bool:
    """True when the string literal at I is joined to what follows with +: `"K" + x` or
    `std::string("K") + x`."""
    j = i + 1
    if j < len(toks) and toks[j].text == ")" and i >= 2 and toks[i - 1].text == "(" and toks[i - 2].kind == "i" \
            and toks[i - 2].text.endswith("string"):
        j += 1
    return j < len(toks) and toks[j].kind == "o" and toks[j].text == "+"


def scan_text(text: str, rel: str, out: Dict[str, Use], ctx: Optional[Context] = None) -> None:
    if not _WORTH.search(text) and not (ctx is not None
                                        and (rel in _family_files() or _TEMPLATE_HINT.search(text))):
        return
    toks = tokens(text)
    if ctx is not None:
        ctx.toks[rel] = toks

    def use(key: str, op: str, line: int) -> Use:
        u = out.setdefault(key, Use())
        u.files.add(rel)
        u.ops.add(op)
        if len(u.where) < 12:
            u.where.append("%s:%d" % (rel, line))
        return u

    n = len(toks)
    consumed: Set[int] = set()  # the key literals of settings rows, counted once as "row"
    for i, t in enumerate(toks):
        if i in consumed:
            continue
        if t.kind == "i" and t.text.endswith(tuple("SettingSource::" + k for k in ("kProp",) + tuple(SETTINGS_NS))):
            # A settings row: SettingSource::kX, "key", "default"[, "options"]
            src = t.text.rsplit("::", 1)[1]
            vals = []
            j = i + 1
            while j + 1 < n and len(vals) < 3 and toks[j].kind == "p" and toks[j].text == "," and toks[j + 1].kind == "s":
                vals.append(toks[j + 1].text)
                if len(vals) == 1:
                    consumed.add(j + 1)
                j += 2
            if not vals:
                continue
            key = vals[0] if src == "kProp" else "%s:%s" % (SETTINGS_NS[src], vals[0])
            if src == "kProp" and not is_key(key):
                continue
            u = use(key, "row", t.line)
            if len(vals) > 1:
                u.defaults.add(vals[1])
            if len(vals) > 2 and vals[2] not in u.options:
                u.options.append(vals[2])
            continue
        if t.kind != "s":
            continue
        s = t.text
        if ctx is not None and RUNTIME_TEMPLATE.match(s):
            ctx.templates.setdefault(rel, {}).setdefault(s, t.line)
            continue
        if ctx is not None and PROP_KEY.match(s) and i >= 2 and toks[i - 1].kind == "o" and toks[i - 1].text == "=" \
                and toks[i - 2].kind == "i":
            ctx.consts.setdefault(rel, {})[toks[i - 2].text] = s
        if ctx is not None and PROP_KEY.match(s) and not s.endswith(".") and _joined(toks, i):
            ctx.joins.setdefault(rel, {}).setdefault(s, t.line)
        if is_key(s):
            op = "other"
            fn = ""
            default = None
            if i >= 2 and toks[i - 1].kind == "p" and toks[i - 1].text == "(" and toks[i - 2].kind == "i":
                fn = _short(toks[i - 2].text)
                if fn in SET_CALLS or fn.startswith(("set", "Set")) or fn.endswith(("Set", "_set")):
                    op = "set"
                elif fn in GET_DEFAULT_ARG or "get" in fn.lower() or fn.lower().startswith("prop"):
                    op = "get"
                    k = GET_DEFAULT_ARG.get(fn)
                    args = call_args(toks, i - 1) if k is not None else None
                    if args is not None and len(args) > k:
                        default = literal(args[k])
            u = use(s, op, t.line)
            if op != "other":
                u.calls.add(fn)
            if default is not None:
                u.defaults.add(default)
            continue
        m = PACKED_SPEC.match(s)
        if m:
            u = use(m.group(1), "spec", t.line)
            if m.group(2) and m.group(2) not in u.options:
                u.options.append(m.group(2))
            continue
        m = PACKED_OTHER.match(s)
        if m and isinstance(out, Found):
            out.problems.append("%s:%d packs key %s into %r, a form prefs.scan_text does not read: teach it "
                                "the form" % (rel, t.line, m.group(1), s[:60]))
        for m in SHELL_PROP.finditer(s):
            use(m.group(2).rstrip("."), "set" if m.group(1) == "setprop" else "get", t.line)
        for m in SHELL_SETTINGS.finditer(s):
            use("%s:%s" % (m.group(2), m.group(3)), "set" if m.group(1) == "put" else "get", t.line)


_SCRIPT_KEY = re.compile(r"(property:|setprop\s+|getprop\s+)(%s)" % _KEYCHARS)


def scan_script(text: str, rel: str, out: Dict[str, Use]) -> None:
    """Keys in an init (.rc) or shell (.sh) script: property:K triggers, setprop K and getprop K."""
    for n, ln in enumerate(text.split("\n"), 1):
        if ln.lstrip().startswith("#"):
            continue
        for m in _SCRIPT_KEY.finditer(ln):
            key = m.group(2).rstrip(".")
            u = out.setdefault(key, Use())
            u.files.add(rel)
            u.ops.add("set" if m.group(1).startswith("setprop") else "get")
            if len(u.where) < 12:
                u.where.append("%s:%d" % (rel, n))


# Where the Linux components outside nano/ keep their keys (relative to the repository root).
LINUX_SRC = "linux/src"
LINUX_BOARD = "linux/board"
OUTSIDE = "linux/"  # a file named from the repository root; nano/ has no linux/ folder
# The init library's readers: thimsy_persist KEY DEFAULT (persist.props), thimsy_ro KEY DEFAULT,
# thimsy_build_prop KEY DEFAULT (usr/share/thimsy/init/lib.sh); and setprop K V, getprop K. A word is
# bare or quoted, or both joined (persist.thimsy."$x").
_SHELL_WORD = r"""(?:"[^"]*"|'[^']*'|[^\s;&|()`"']+)+"""
_LINUX_SCRIPT_CALL = re.compile(r"(?<![A-Za-z0-9_\-])(thimsy_persist|thimsy_ro|thimsy_build_prop|setprop|getprop)"
                                r"[ \t]+(%s)(?:[ \t]+(%s))?" % (_SHELL_WORD, _SHELL_WORD))
_LINUX_READERS = ("thimsy_persist", "thimsy_ro", "thimsy_build_prop")
_SCRIPT_TRIGGER = re.compile(r"property:(%s)" % _KEYCHARS)
_SCRIPT_KEY_WORD = re.compile(r"(%s)\Z" % _KEYCHARS)
_SCRIPT_NS = re.compile(r"(?:%s)\." % "|".join(PROP_NAMESPACES))
# A line that runs grep, sed or awk, and persist.* text in it ("^persist\.thimsy\.x=", escaped dots
# included). A path's last part ("$THIMSY_CONFIG/persist.props") is a file, not a key, and so is the
# store's own name, persist.props.
_PATTERN_CMD = re.compile(r"(?:^|[\s;&|(`!])(?:grep|egrep|fgrep|sed|awk)(?=\s|\Z)")
_PATTERN_KEY = re.compile(r"(?<![A-Za-z0-9_./\\\-])persist\\{0,2}\.[A-Za-z0-9_.\\\-]*")
STORE_FILE = "persist.props"


def scan_linux_script(text: str, rel: str, out: Dict[str, Use]) -> None:
    """Keys in an init or shell script of linux/board: property:K, and the KEY of the init library's
    readers (with their defaults), setprop and getprop, bare or quoted. With OUT a Found, a KEY the scan
    cannot read is a problem: a variable, a key with a $-expansion in it, or persist.* text in a grep,
    sed or awk line."""
    problems = out.problems if isinstance(out, Found) else None

    def note(key: str, op: str, n: int) -> Use:
        u = out.setdefault(key, Use())
        u.files.add(rel)
        u.ops.add(op)
        if len(u.where) < 12:
            u.where.append("%s:%d" % (rel, n))
        return u

    for n, ln in enumerate(text.split("\n"), 1):
        if ln.lstrip().startswith("#"):
            continue
        for m in _SCRIPT_TRIGGER.finditer(ln):
            note(m.group(1).rstrip("."), "get", n)
        for m in _LINUX_SCRIPT_CALL.finditer(ln):
            cmd, word, arg = m.groups()
            bare = word.replace('"', "").replace("'", "")
            if "$" in bare:
                if problems is not None and (bare.startswith("$") or _SCRIPT_NS.match(bare)):
                    problems.append("%s:%d: %s %s: the key is %s, which prefs.scan_linux_script cannot read: "
                                    "name the key in full" % (rel, n, cmd, word, "a variable" if bare.startswith("$")
                                                              else "built with a $-expansion"))
                continue
            if not _SCRIPT_KEY_WORD.match(bare):
                continue  # another namespace (setprop bluetooth.profile.x false), not Nano's
            u = note(bare.rstrip("."), "set" if cmd == "setprop" else "get", n)
            if cmd in _LINUX_READERS:
                u.calls.add(cmd)
                if arg is not None and "$" not in arg:
                    u.defaults.add(arg.replace('"', "").replace("'", ""))
        if problems is not None and _PATTERN_CMD.search(ln):
            for m in _PATTERN_KEY.finditer(ln):
                if m.group(0) != STORE_FILE:
                    problems.append("%s:%d: %s is in a grep, sed or awk line, a form prefs.scan_linux_script does "
                                    "not read: read the key with thimsy_persist, or teach the scan the form"
                                    % (rel, n, m.group(0)))


def linux_files(root: str) -> List[str]:
    """Paths (relative to the repository root) of the C and C++ sources under linux/src (tests left
    out) and the shell scripts in linux/board's root file system overlays (*/rootfs-overlay*/: a .sh
    file or a file whose #! line names a shell). Nothing else outside nano/ is scanned: not libthimsy
    (libthimsy/tools/thimsy-prop.cpp writes device.json's scalars as ro.thimsy.*), and not
    linux/board's build scripts (post-build.d/20-build-props.sh writes build.props, ro.* only). Every
    key those two write is under the ro.* glob row (class R)."""
    out = []
    for top, want_src in ((LINUX_SRC, True), (LINUX_BOARD, False)):
        for dirpath, dirnames, names in os.walk(os.path.join(root, *top.split("/"))):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
            for nm in sorted(names):
                path = os.path.join(dirpath, nm)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                if want_src:
                    if nm.endswith(SOURCE_EXT):
                        out.append(rel)
                    continue
                if "/rootfs-overlay" not in rel:
                    continue
                with open(path, "rb") as f:
                    head = f.read(64)
                if nm.endswith(".sh") or (head.startswith(b"#!") and b"sh" in head.split(b"\n", 1)[0]):
                    out.append(rel)
    return out


def scanned_files(nano_dir: str) -> List[str]:
    """Paths (relative to nano/) of every C and C++ source and every init or shell script under nano/,
    tests left out."""
    out = []
    for dirpath, dirnames, names in os.walk(nano_dir):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for nm in sorted(names):
            rel = os.path.relpath(os.path.join(dirpath, nm), nano_dir).replace(os.sep, "/")
            if nm.endswith(SOURCE_EXT + SCRIPT_EXT) and not rel.startswith(SKIP_PATHS):
                out.append(rel)
    return out


class Found(dict):
    """{key: Use}, plus the runtime families' problems and the key prefixes they account for."""

    def __init__(self, *a, **kw) -> None:
        super().__init__(*a, **kw)
        self.problems: List[str] = []
        self.family_prefixes: Set[str] = set()


def _array_items(toks: List[Tok], name: Optional[str], type_: Optional[str]) -> Iterator[List[List[Tok]]]:
    """The rows of the array NAME, or of every array of type TYPE_, as lists of fields."""
    n = len(toks)
    for i, t in enumerate(toks):
        if t.kind != "i":
            continue
        if name is not None:
            if _short(t.text) != name and t.text.rsplit("::", 1)[-1] != name:
                continue
            j = i + 1
        else:
            if t.text != type_ or i + 1 >= n or toks[i + 1].kind != "i":
                continue
            j = i + 2
        if j >= n or toks[j].text != "[":
            continue
        while j < n and toks[j].text != "]":
            j += 1
        if j + 2 >= n or toks[j + 1].text != "=" or toks[j + 2].text != "{":
            continue
        items = call_args(toks, j + 2, limit=20000) or []
        for it in items:
            if not it:
                continue
            if it[0].kind == "p" and it[0].text == "{":
                yield call_args(it, 0, limit=len(it) + 1) or []
            else:
                yield [it]


def _string(field: List[Tok]) -> Optional[str]:
    return field[0].text if len(field) == 1 and field[0].kind == "s" else None


def expand_runtime(found: Found, ctx: Context, families=RUNTIME_FAMILIES) -> None:
    """Adds the keys of each runtime family to FOUND (op "built"), and the problems: a family that
    lists nothing, a prefix no longer in its file, an unresolved base, a template no family claims, a
    key literal joined with + that is no family's prefix in its file."""

    def add(key: str, fam: dict, where: str, default: Optional[str] = None) -> None:
        u = found.setdefault(key, Use())
        u.files.add(fam["file"])
        u.ops.add("built")
        u.calls.add(fam["name"])
        if len(u.where) < 12:
            u.where.append(where)
        if default is not None:
            u.defaults.add(default)

    claimed: Set[tuple] = set()
    for fam in families:
        rel, name = fam["file"], fam["name"]
        toks = ctx.toks.get(rel)
        if toks is None:
            found.problems.append("runtime family %r: %s is not scanned" % (name, rel))
            continue
        made = 0
        if "prefix" in fam:
            found.family_prefixes.add(fam["prefix"])
            if fam["prefix"] not in found:
                found.problems.append("runtime family %r: no key literal %s in nano/" % (name, fam["prefix"]))
            for row in _array_items(toks, fam.get("array"), fam.get("array_type")):
                k = fam["field"]
                v = _string(row[k]) if len(row) > k else None
                if v is None:
                    continue
                df = fam.get("default_field")
                d = _string(row[df]) if df is not None and len(row) > df else None
                for suf in fam.get("suffixes", ("",)):
                    add(fam["prefix"] + v + suf, fam, "%s (%s)" % (rel, fam.get("array") or fam.get("array_type")), d)
                    made += 1
        else:
            temps = {t: ln for t, ln in ctx.templates.get(rel, {}).items() if t.startswith(fam["templates"])}
            for t in temps:
                claimed.add((rel, t))
            if "prefixes" in fam:
                prefixes = list(fam["prefixes"])
                for p in prefixes:
                    found.family_prefixes.add(p)
                    head = p.split("%", 1)[0]
                    if rel not in getattr(found.get(head), "files", ()):
                        found.problems.append("runtime family %r: prefix %s is no longer a key literal in %s"
                                              % (name, head, rel))
            else:
                prefixes = []
                skip = set(fam.get("params", ()))
                for frel, ftoks in sorted(ctx.toks.items()):
                    for i, tk in enumerate(ftoks):
                        if tk.kind != "i" or _short(tk.text) not in fam["bases_from"]:
                            continue
                        if i + 1 >= len(ftoks) or ftoks[i + 1].text != "(":
                            continue
                        args = call_args(ftoks, i + 1)
                        if not args or len(args[0]) != 1:
                            continue
                        a = args[0][0]
                        if a.kind == "s":
                            base = a.text
                        elif a.kind == "i" and a.text in skip:
                            continue
                        elif a.kind == "i":
                            base = ctx.consts.get(frel, {}).get(a.text)
                            if base is None:
                                found.problems.append("runtime family %r: cannot resolve %s at %s:%d to a key "
                                                      "literal" % (name, a.text, frel, tk.line))
                                continue
                        else:
                            continue
                        if base not in prefixes:
                            prefixes.append(base)
            for t, ln in sorted(temps.items()):
                for p in prefixes:
                    add(p + t[2:], fam, "%s:%d" % (rel, ln))
                    made += 1
        if not made:
            found.problems.append("runtime family %r lists no keys (did %s change?)" % (name, rel))
    for rel, temps in sorted(ctx.templates.items()):
        for t, ln in sorted(temps.items()):
            if (rel, t) not in claimed:
                found.problems.append("%s:%d builds keys from %r with a prefix chosen at run time: declare the "
                                      "family in prefs.RUNTIME_FAMILIES" % (rel, ln, t))
    heads: Dict[str, Set[str]] = {}
    for fam in families:
        own = heads.setdefault(fam["file"], set())
        if "prefix" in fam:
            own.add(fam["prefix"])
        own.update(p.split("%", 1)[0] for p in fam.get("prefixes", ()))
    for rel, joins in sorted(ctx.joins.items()):
        for k, ln in sorted(joins.items()):
            if k not in heads.get(rel, ()):
                found.problems.append("%s:%d joins key %s to a value chosen at run time with +: the keys built on it "
                                      "have no rows. Declare the family in prefs.RUNTIME_FAMILIES" % (rel, ln, k))


def scan(nano_dir: str, files: Optional[List[str]] = None, linux_root: Optional[str] = None) -> Found:
    """{key: Use} over FILES (default: scanned_files), with the runtime families expanded; with
    LINUX_ROOT (the repository root), also over linux_files. The runtime families are nano/'s only: a
    "%s..." template or a key joined with + in linux/src is a problem."""
    out = Found()
    ctx = Context()
    for rel in files if files is not None else scanned_files(nano_dir):
        with open(os.path.join(nano_dir, *rel.split("/")), "rb") as f:
            text = f.read().decode("utf-8", "replace")
        if rel.endswith(SCRIPT_EXT):
            scan_script(text, rel, out)
        else:
            scan_text(text, rel, out, ctx)
    expand_runtime(out, ctx)
    if linux_root is not None:
        lctx = Context()
        for rel in linux_files(linux_root):
            with open(os.path.join(linux_root, *rel.split("/")), "rb") as f:
                text = f.read().decode("utf-8", "replace")
            if rel.startswith(LINUX_SRC + "/"):
                scan_text(text, rel, out, lctx)
            else:
                scan_linux_script(text, rel, out)
        expand_runtime(out, lctx, ())
    res = Found(sorted(out.items()))
    res.problems, res.family_prefixes = out.problems, out.family_prefixes
    return res


def linux_only(use: Use, cfg: Upstream) -> bool:
    """True when every file naming the key is fork-only (the Linux build's own code) or outside nano/
    (linux/src, linux/board)."""
    return all(f.startswith(OUTSIDE) or cfg.is_fork_only(f) for f in use.files)


def outside_only(use: Use) -> bool:
    """True when only files outside nano/ name the key."""
    return bool(use.files) and all(f.startswith(OUTSIDE) for f in use.files)


# ---- prefs.json --------------------------------------------------------------------------------

def load_table(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def is_glob(match: str) -> bool:
    return match.endswith("*") and " " not in match


def covering_row(key: str, rows: List[dict]) -> Optional[dict]:
    """KEY's row: its exact row, else the longest glob row ("prefix*") whose prefix it starts with."""
    best = None
    for r in rows:
        m = r["match"]
        if m == key:
            return r
        if is_glob(m) and key.startswith(m[:-1]) and (best is None or len(m) > len(best["match"])):
            best = r
    return best


INI_KEY = re.compile(r"(?:device\.\*\.|android\.|linux\.)?[a-z0-9_]+(?:\.[a-z0-9_]+)+\Z")


def bool_norm(v: str) -> str:
    return {"true": "1", "false": "0", "on": "1", "off": "0", "yes": "1", "no": "0"}.get(v.lower(), v)


def option_values(options: str) -> List[str]:
    """The values of a settings row's options string ("v:Label,v:Label"); [] for sliders and specials."""
    if not options or options.startswith(("@", "slider:")):
        return []
    return [o.split(":", 1)[0] for o in options.split(",") if o]


def check_table(table: dict, found: Dict[str, Use]) -> List[str]:
    """Problems: malformed rows (an enum's default must be one of its values), scanned keys with no
    row, rows that cover nothing, a broken unclassified list, and portable rows whose default or values
    disagree with the code."""
    problems: List[str] = list(getattr(found, "problems", []))
    rows = table.get("rows", [])
    unclassified = table.get("unclassified", [])
    seen: Set[str] = set()
    ini_seen: Dict[str, str] = {}
    exact = {r.get("match"): r for r in rows}
    for r in rows:
        m = r.get("match", "")
        if not m:
            problems.append("a row without a match: %r" % r)
            continue
        if m in seen:
            problems.append("two rows for %s" % m)
        seen.add(m)
        c = r.get("class")
        if c not in CLASSES:
            problems.append("%s: class %r is not one of %s" % (m, c, ", ".join(CLASSES)))
        if not r.get("note") and not r.get("label"):
            problems.append("%s: no note or label" % m)
        if r.get("type") is not None and r["type"] not in TYPES:
            problems.append("%s: type %r is not one of %s" % (m, r["type"], ", ".join(TYPES)))
        if r.get("type") == "enum":
            if not r.get("values"):
                problems.append("%s: an enum, but no \"values\"" % m)
            elif "default" in r and str(r["default"]) not in r["values"]:
                problems.append("%s: an enum whose default %r is not one of its values (%s)"
                                % (m, r["default"], ", ".join(r["values"])))
        ini = r.get("ini")
        part_of = r.get("part_of")
        if part_of:
            base = exact.get(part_of)
            if base is None:
                problems.append("%s: part_of %s, which has no row" % (m, part_of))
            elif base.get("class") != c:
                problems.append("%s: class %s, but it is part of %s, class %s" % (m, c, part_of, base.get("class")))
            if ini:
                problems.append("%s: part of %s, whose ini key covers it, but has its own \"ini\"" % (m, part_of))
        elif c in PORTABLE:
            if is_glob(m):
                problems.append("%s: a class %s row must name one key, not a glob" % (m, c))
            if not ini:
                problems.append("%s: class %s, but no settings.ini key (\"ini\")" % (m, c))
            if not r.get("type"):
                problems.append("%s: class %s, but no type" % (m, c))
            if "default" not in r:
                problems.append("%s: class %s, but no default" % (m, c))
            if c in ("P", "M", "L") and ":" in m and not r.get("linux"):
                problems.append("%s: a settings-provider key on the card for Linux too, but no \"linux\" says where "
                                "Linux keeps it (ShellGate refuses `settings`)" % m)
            if r.get("type") == "bool":
                enc = r.get("encoding")
                if enc not in ENCODINGS:
                    problems.append("%s: a bool, but its device-store encoding is %r, not one of %s"
                                    % (m, enc, ", ".join(ENCODINGS)))
                elif str(r.get("default", "")) not in ("",) + tuple(enc.split("/")):
                    problems.append("%s: default %r is not in its encoding %s" % (m, r.get("default"), enc))
        elif ini:
            problems.append("%s: class %s never goes on the card, but has \"ini\"" % (m, c))
        if ini:
            if not INI_KEY.match(ini):
                problems.append("%s: ini key %r is not section.name" % (m, ini))
            if c == "M" and not ini.startswith("device.*."):
                problems.append("%s: class M, but its ini key %s is not in [device.*]" % (m, ini))
            if c == "A" and not ini.startswith("android."):
                problems.append("%s: class A, but its ini key %s is not in [android]" % (m, ini))
            if c == "L" and not ini.startswith("linux."):
                problems.append("%s: class L, but its ini key %s is not in [linux]" % (m, ini))
            if c == "P" and ini.startswith(("device.", "android.", "linux.")):
                problems.append("%s: class P, but its ini key %s is in a per-model or platform section" % (m, ini))
            if ini in ini_seen:
                problems.append("%s: ini key %s is also %s's" % (m, ini, ini_seen[ini]))
            ini_seen[ini] = m
        if c == "?" and m not in unclassified:
            problems.append("%s: class ?, but not in the unclassified list" % m)
        if c != "?" and m in unclassified:
            problems.append("%s: in the unclassified list, but its class is %s" % (m, c))
        if r.get("es_de") and c not in PORTABLE + ("E",):
            problems.append("%s: an ES-DE mirror on a class %s row" % (m, c))
        u = found.get(m)
        if u is not None and c in PORTABLE and "default" in r:
            d = str(r["default"])
            code = {bool_norm(x) for x in u.defaults} if r.get("type") == "bool" else set(u.defaults)
            want = bool_norm(d) if r.get("type") == "bool" else d
            if code and want not in code and not r.get("default_note"):
                problems.append("%s: default %r is not one the code uses (%s); fix it or say why in "
                                "\"default_note\"" % (m, d, ", ".join(sorted(u.defaults))))
            if u.options and r.get("values") is not None and r.get("type") == "enum":
                code_vals = set()
                for o in u.options:
                    code_vals.update(option_values(o))
                missing = sorted(code_vals - set(r["values"]))
                if missing:
                    problems.append("%s: the code offers %s (a settings row or a chooser spec), which \"values\" "
                                    "lacks" % (m, ", ".join(missing)))
        if r.get("owner") and u is not None and not any(f.startswith(OUTSIDE) for f in u.files):
            problems.append("%s: \"owner\" %s, but nothing outside nano/ names it" % (m, r["owner"]))
    for k in unclassified:
        if k not in seen:
            problems.append("unclassified %s has no row" % k)
    prefixes = getattr(found, "family_prefixes", None)
    for key, u in found.items():
        r = covering_row(key, rows)
        if r is None:
            problems.append("no row for %s (in %s)" % (key, ", ".join(u.where[:3])))
        elif outside_only(u) and not key.startswith(NON_PREF_NAMESPACES) and ":" not in key \
                and (r["match"] != key or not r.get("owner")):
            problems.append("%s: only %s name it, outside nano/: give it an exact row with an \"owner\" (the "
                            "component that keeps it)" % (key, ", ".join(sorted(u.files)[:3])))
        elif prefixes is not None and key.endswith(".") and not is_glob(r["match"]) and key not in prefixes:
            problems.append("%s is a key prefix (in %s): the keys built on it at run time have no rows. Declare "
                            "the family in prefs.RUNTIME_FAMILIES, or cover it with a glob row"
                            % (key, ", ".join(u.where[:3])))
    for r in rows:
        m = r.get("match", "")
        if not m or r.get("source") == "manual":
            continue
        if is_glob(m):
            if not any(covering_row(k, rows) is r for k in found):
                problems.append("glob row %s covers nothing" % m)
        elif m not in found:
            problems.append("stale row %s: nothing in nano/, linux/src or linux/board names it (drop it, or mark "
                            "it \"source\": \"manual\" with a reason)" % m)
    return problems


def glob_lines(found: Dict[str, Use], rows: List[dict]) -> List[str]:
    """"<glob> <key>" for each scanned key a glob row covers, in the namespaces that can hold a pref
    (the baseline in prefs-globs.txt)."""
    out = []
    for key in found:
        if key.startswith(NON_PREF_NAMESPACES) or ":" in key:
            continue
        r = covering_row(key, rows)
        if r is not None and is_glob(r["match"]):
            out.append("%s %s" % (r["match"], key))
    return sorted(out)


def report(found: Dict[str, Use], rows: List[dict]) -> Iterator[str]:
    for key, u in found.items():
        r = covering_row(key, rows)
        yield "%-58s %-2s %-26s %-18s %s" % (key[:58], r["class"] if r else "--", (r or {}).get("ini", "")[:26],
                                            ",".join(sorted(u.defaults))[:18], u.where[0] if u.where else "")
