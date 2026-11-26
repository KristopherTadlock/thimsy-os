"""Layer 2 of the platform seam (docs/linux/IMPLEMENTATION.md B.4, C.1, H.4): the inert stubs in
nano/compat/stubs, the census-made split of nano/build/sources.txt, the stub generator
(nano/test/host/stubgen/gen.py) on canned nm output, and two ratchets on shared files that no other
step owns (B.2's Bionic-only identifiers, B.5's raw block device paths). Fork-only, standard library
only; run with

  py -3.12 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests

After a deliberate change, rewrite a baseline with one of
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-budget   (only ever lowers it)
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-bionic
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-block
"""
from __future__ import annotations

import importlib.util
import os
import re
import sys
import unittest
from typing import Dict, List, Optional, Set, Tuple

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

import include_census  # noqa: E402
from nanotree.config import NanoTreeError, Upstream  # noqa: E402
from nanotree.ratchets import lex, read_list, shared_sources, write_list  # noqa: E402

NANO = os.path.join(ROOT, "nano")
STUBS = os.path.join(NANO, "compat", "stubs")
STUBS_INCLUDE = os.path.join(STUBS, "include")
SHIMS_INCLUDE = os.path.join(NANO, "compat", "include")
STUBGEN_DIR = os.path.join(NANO, "test", "host", "stubgen")
BUDGET = os.path.join(TOOL_DIR, "stubs-budget.txt")
BIONIC = os.path.join(TOOL_DIR, "bionic-identifiers.txt")
BLOCK = os.path.join(TOOL_DIR, "block-paths.txt")
HAVE = os.path.isdir(STUBS_INCLUDE) and os.path.isfile(os.path.join(NANO, "Android.bp"))

# The functions G9's DRM buffer back end defines for its own render targets (B.8): the stub header
# declares them and no stub may define them (G9a's hand-off).
BACKEND_FUNCTIONS = ("AHardwareBuffer_release", "AHardwareBuffer_describe", "AHardwareBuffer_lock",
                     "AHardwareBuffer_unlock")
GBM_BACKEND = os.path.join(NANO, "platform", "linux", "display", "DrmBuffersGbm.cpp")

# --- the stub inventory -----------------------------------------------------------------------------

_TOKEN = re.compile(r'""|[A-Za-z_]\w*|::|->|\.\.\.|[^\sA-Za-z_]')
_KEYWORDS = {"const", "volatile", "static", "inline", "virtual", "explicit", "extern", "constexpr", "typename",
             "unsigned", "signed", "struct", "class", "union", "enum", "public", "private", "protected", "friend",
             "override", "final", "noexcept", "return", "template", "typedef", "using", "namespace", "operator",
             "void", "int", "char", "bool", "float", "double", "long", "short", "auto", "default", "delete"}


def _read(path: str) -> str:
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def stub_headers() -> List[str]:
    out = []
    for dirpath, dirnames, names in os.walk(STUBS_INCLUDE):
        dirnames.sort()
        for n in sorted(names):
            if n.endswith(".h"):
                out.append(os.path.relpath(os.path.join(dirpath, n), STUBS_INCLUDE).replace(os.sep, "/"))
    return out


def declared_names(text: str) -> Set[str]:
    """The names a C or C++ header declares, qualified by their namespaces and classes: macros,
    types (class, struct, union, enum, typedef, using alias), enumerators, functions (overloads count
    once) and variables or data members. Function bodies are skipped."""
    code, _strings = lex(text)
    names: Set[str] = set()
    lines = []
    for ln in code.split("\n"):
        m = re.match(r"\s*#\s*(\w+)\s*(\w*)", ln)
        if m:
            if m.group(1) == "define" and m.group(2):
                names.add(m.group(2))
            lines.append("")
        else:
            lines.append(ln)
    toks = _TOKEN.findall("\n".join(lines))
    scope: List[Tuple[str, str]] = []       # (kind, name): kind is ns, class, enum or body
    stmt: List[str] = []

    def qual(n: str) -> str:
        return "::".join([s for k, s in scope if s and k in ("ns", "class")] + [n])

    def statement_names(st: List[str]) -> List[str]:
        """The name a declaration statement (tokens up to ';' or '{', or a whole class member)
        introduces, if any."""
        if not st:
            return []
        while st and st[0] in ("public", "private", "protected") and len(st) > 1 and st[1] == ":":
            st = st[2:]
        if not st:
            return []
        if st[0] == "template":
            depth, i = 0, 1
            while i < len(st):
                depth += st[i] == "<"
                depth -= st[i] == ">"
                i += 1
                if depth == 0:
                    break
            st = st[i:]
        if not st:
            return []
        if st[0] == "using":
            return [st[1]] if len(st) > 2 and st[2] == "=" else []
        if st[0] == "typedef":
            for i in range(len(st) - 2):
                if st[i] == "(" and st[i + 1] == "*":
                    return [st[i + 2]]
            ids = [t for t in st if re.match(r"[A-Za-z_]", t)]
            return [ids[-1]] if ids else []
        if st[0] == "friend":
            return []
        if "(" in st:
            i = st.index("(")
            if i > 0 and st[i - 1] in (")", "]"):        # operator() and the like
                j = i - 1
                while j > 0 and st[j] != "operator":
                    j -= 1
                return ["operator" + "".join(st[j + 1:i])] if st[j] == "operator" else []
            if "operator" in st[:i]:
                j = st.index("operator")
                return ["operator" + "".join(st[j + 1:i])]
            name = st[i - 1] if i > 0 else ""
            if i >= 2 and st[i - 2] == "~":
                name = "~" + name
            if re.match(r"~?[A-Za-z_]\w*$", name) and name not in _KEYWORDS:
                return [name]
            return []
        # A variable or data member: the identifier before '=', '{', '[' or the end.
        end = len(st)
        for i, t in enumerate(st):
            if t in ("=", "{", "["):
                end = i
                break
        ids = [t for t in st[:end] if re.match(r"[A-Za-z_]\w*$", t) and t not in _KEYWORDS]
        if len(ids) >= 2 or (ids and len([t for t in st[:end] if t not in ("*", "&")]) >= 2):
            return [ids[-1]]
        return []

    i = 0
    while i < len(toks):
        t = toks[i]
        if scope and scope[-1][0] == "body":
            if t == "{":
                scope.append(("body", ""))
            elif t == "}":
                scope.pop()
            i += 1
            continue
        if scope and scope[-1][0] == "enum":
            if t == "}":
                scope.pop()
                stmt = []
            elif re.match(r"[A-Za-z_]\w*$", t) and (not stmt or stmt[-1] == ","):
                names.add(qual(t))
                stmt.append(t)
            else:
                stmt.append(t)
            i += 1
            continue
        if t == "{":
            head = stmt[1:] if stmt[:1] == ["}"] else stmt
            if head[:1] == ["namespace"] or head[:2] == ["extern", '""']:
                scope.append(("ns", head[1] if head[:1] == ["namespace"] and len(head) > 1 else ""))
            elif "enum" in head and "(" not in head:
                j = head.index("enum") + 1
                if j < len(head) and head[j] in ("class", "struct"):
                    j += 1
                if j < len(head) and re.match(r"[A-Za-z_]\w*$", head[j]) and head[j] != ":":
                    names.add(qual(head[j]))
                scope.append(("enum", ""))
            elif any(k in head for k in ("class", "struct", "union")) and "(" not in head and "=" not in head:
                k = next(i for i, x in enumerate(head) if x in ("class", "struct", "union"))
                n = head[k + 1] if k + 1 < len(head) and re.match(r"[A-Za-z_]\w*$", head[k + 1]) else ""
                if n:
                    names.add(qual(n))
                if head[:1] == ["typedef"]:
                    scope.append(("class", n))
                    scope[-1] = ("class", n)
                    stmt = []
                    i += 1
                    # The typedef's own name comes after the closing brace; handled at '}'.
                    continue
                scope.append(("class", n))
            elif "(" in head:
                for n in statement_names(head):
                    names.add(qual(n))
                scope.append(("body", ""))
            else:
                # An initializer: name{...}; read to the closing brace as part of the statement.
                depth = 0
                while i < len(toks):
                    depth += toks[i] == "{"
                    depth -= toks[i] == "}"
                    stmt.append(toks[i])
                    i += 1
                    if depth == 0:
                        break
                continue
            stmt = []
        elif t == "}":
            closed = scope.pop()[0] if scope else ""
            stmt = ["}"] if closed == "class" else []
        elif t == ";":
            if stmt[:1] == ["}"]:
                # The end of a class body: "} Name;" names a typedef or a variable.
                rest = [x for x in stmt[1:] if re.match(r"[A-Za-z_]\w*$", x)]
                if rest:
                    names.add(qual(rest[-1]))
            else:
                for n in statement_names(stmt):
                    names.add(qual(n))
            stmt = []
        elif t == ":" and stmt in (["public"], ["private"], ["protected"]):
            stmt = []
        else:
            stmt.append(t)
        i += 1
    return names


def inventory() -> Dict[str, Set[str]]:
    return {h: declared_names(_read(os.path.join(STUBS_INCLUDE, *h.split("/")))) for h in stub_headers()}


def stub_count() -> int:
    return len(set().union(*inventory().values())) if stub_headers() else 0


def read_budget() -> Optional[int]:
    if not os.path.isfile(BUDGET):
        return None
    for ln in read_list(BUDGET):
        if ln.strip().isdigit():
            return int(ln.strip())
    return None


BUDGET_HEADER = """The layer-2 stub budget (IMPLEMENTATION.md B.4, H.4): how many names the headers under
nano/compat/stubs/include declare (macros, types, enumerators, functions, variables and members,
qualified by scope; overloads count once), counted by tools/nano-tree/tests/test_nano_stubs.py.
The count may only shrink: a new stub needs a reviewed reason, and layer 4 (P4) lowers it as it
moves firmware code out. After removing stubs:
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-budget"""

# --- B.2 and B.5 ratchets on shared files ----------------------------------------------------------

# Bionic and libutils extras glibc 2.31 lacks (B.2, I31). A shared file's use of one is recorded in
# bionic-identifiers.txt and must be provided by a layer-1 shim (nanocompat/bionic.h and the rest).
BIONIC_PATTERN = re.compile(
    r"\b(M_PURGE(?:_ALL)?|M_DECAY_TIME|M_CACHE_(?:COUNT|SIZE)_MAX|M_TSDS_COUNT_MAX|M_BIONIC_\w+|M_LOG_STATS|"
    r"M_MEMTAG_TUNING|M_THREAD_DISABLE_MEM_INIT|strlcpy|strlcat|getprogname|setprogname|"
    r"arc4random(?:_buf|_uniform)?|gettid|pthread_gettid_np|android_mallopt|android_set_abort_message|"
    r"__BIONIC__|android_fdsan_\w+|ANDROID_PRIORITY_\w+|SLOG[VDIWE](?:_IF)?|closefrom|tgkill|reallocarray|"
    r"__system_property_\w+|android_get_application_target_sdk_version|android_get_device_api_level|"
    r"__ANDROID_API__)\b")
BIONIC_HEADER = """Bionic-only identifiers used in nano/'s shared C and C++ files (IMPLEMENTATION.md B.2, H.4,
decision I31): the ones glibc 2.31 lacks, each provided on Linux and the host by a layer-1 shim
(nano/compat/include, nanocompat/bionic.h force-included). test_nano_stubs.py fails when the set
changes or one has no shim. After a deliberate change, or a merge that brought a new one:
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-bionic"""
BLOCK_PATTERN = re.compile(r"(?<![\w.])/dev/block\b|mmcblk")
BLOCK_HEADER = """nano/'s shared source files (tests excluded) whose string literals name a raw block device
(/dev/block or mmcblk; IMPLEMENTATION.md B.5, H.4, decision I40). On Linux, thimsy-session hides the
eMMC from Nano behind /dev/null binds and /dev/block/by-name is never created; a new file here is
reviewed against that rule before the list is updated:
  py -3.12 tools/nano-tree/tests/test_nano_stubs.py --update-block"""


def _shared_files() -> List[str]:
    cfg = Upstream.load(ROOT)
    return [f for f in shared_sources(NANO, cfg) if not f.startswith("compat/")]


def bionic_uses() -> Dict[str, List[str]]:
    out: Dict[str, Set[str]] = {}
    for rel in _shared_files():
        code, _ = lex(_read(os.path.join(NANO, *rel.split("/"))))
        for m in BIONIC_PATTERN.finditer(code):
            out.setdefault(m.group(1), set()).add(rel)
    return {k: sorted(v) for k, v in sorted(out.items())}


def block_files() -> List[str]:
    out = []
    for rel in _shared_files():
        if rel.startswith("test/"):
            continue
        _code, strings = lex(_read(os.path.join(NANO, *rel.split("/"))))
        if any(BLOCK_PATTERN.search(s) for _line, s in strings):
            out.append(rel)
    return out


def _shim_text() -> str:
    parts = []
    for dirpath, _dirnames, names in os.walk(SHIMS_INCLUDE):
        for n in names:
            parts.append(_read(os.path.join(dirpath, n)))
    return "\n".join(parts)


def _stubgen():
    spec = importlib.util.spec_from_file_location("nano_stubgen_test", os.path.join(STUBGEN_DIR, "gen.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


# --- the tests ---------------------------------------------------------------------------------------

class DeclaredNames(unittest.TestCase):
    def test_counts_what_a_header_declares(self):
        text = """
#pragma once
#define NANOSTUB_X 1
extern "C" {
typedef struct AFoo AFoo;
typedef int (*AFoo_cb)(int fd);
enum { AFOO_A = 1, AFOO_B = 2 };
AFoo* AFoo_new(void);
void AFoo_delete(AFoo* f);
extern const char* AFOO_KEY;
}
namespace android {
class Bar : public virtual RefBase {
 public:
    explicit Bar(int x) : mX(x) {}
    static sp<Bar> make() { if (1) { return nullptr; } return nullptr; }
    int get() const { return mX; }
    int get(int y) const { return y; }
    enum : uint32_t { eOne = 1 };
 private:
    int mX = 0;
};
namespace ui {
enum class Rotation { R0 = 0, R90 = 1 };
constexpr Rotation ROTATION_0 = Rotation::R0;
struct Size { int32_t width = -1; Size() = default; };
inline bool operator==(Size a, Size b) { return a.width == b.width; }
}
}
"""
        self.assertEqual(sorted(declared_names(text)), sorted([
            "NANOSTUB_X", "AFoo", "AFoo_cb", "AFOO_A", "AFOO_B", "AFoo_new", "AFoo_delete", "AFOO_KEY",
            "android::Bar", "android::Bar::Bar", "android::Bar::make", "android::Bar::get", "android::Bar::eOne",
            "android::Bar::mX", "android::ui::Rotation", "android::ui::R0", "android::ui::R90",
            "android::ui::ROTATION_0", "android::ui::Size", "android::ui::Size::width", "android::ui::Size::Size",
            "android::ui::operator=="]))


@unittest.skipUnless(HAVE, "no nano/compat/stubs in this checkout")
class Stubs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = include_census.Census(ROOT)
        cls.split = include_census.Split(cls.census)

    def test_budget_only_shrinks(self):
        budget = read_budget()
        self.assertIsNotNone(budget, "no count in tools/nano-tree/stubs-budget.txt")
        n = stub_count()
        if n > budget:
            per = {h: len(v) for h, v in inventory().items()}
            self.fail("the stubs declare %d names, over the budget of %d (stubs-budget.txt); a stub may only be "
                      "added with a reviewed reason, never by raising the budget silently. Per header: %s"
                      % (n, budget, ", ".join("%s %d" % kv for kv in sorted(per.items()))))
        self.assertEqual(n, budget, "the stubs shrank to %d names: lower the budget (test_nano_stubs.py "
                                    "--update-budget)" % n)

    def test_core_reaches_no_unstubbed_android_header(self):
        """Every Android-only header a [core] or [drm] file reaches has a shim or a stub (B.4)."""
        bad = []
        for sec in ("core", "drm"):
            for p in self.split.sections[sec]:
                for cls in ("android", "sibling", "missing", "unknown"):
                    bad += ["[%s] %s: %s %s" % (sec, p, cls, h) for h in sorted(self.census.needs(p).get(cls, ()))]
        self.assertEqual(bad, [])

    def test_every_stub_header_is_used(self):
        """No dead stubs: each header under compat/stubs/include is reached from [core] or [drm],
        directly or through another stub header."""
        reached: Set[str] = set()
        todo = []
        for sec in ("core", "drm"):
            for p in self.split.sections[sec]:
                for f in self.census.reach([p]):
                    for inc in self.census.files.get(f, []):
                        if inc.cls == "stub":
                            todo.append(inc.path[len(include_census.STUBS_INCLUDE) + 1:])
        while todo:
            h = todo.pop()
            if h in reached:
                continue
            reached.add(h)
            incs, _ = include_census.scan(_read(os.path.join(STUBS_INCLUDE, *h.split("/"))))
            for inc in incs:
                if os.path.isfile(os.path.join(STUBS_INCLUDE, *inc.name.split("/"))):
                    todo.append(inc.name)
        self.assertEqual(sorted(set(stub_headers()) - reached), [])

    def test_stub_headers_include_only_stubs_shims_and_the_system(self):
        bad = []
        for h in stub_headers():
            incs, _ = include_census.scan(_read(os.path.join(STUBS_INCLUDE, *h.split("/"))))
            for inc in incs:
                if os.path.isfile(os.path.join(STUBS_INCLUDE, *inc.name.split("/"))):
                    continue
                if os.path.isfile(os.path.join(SHIMS_INCLUDE, *inc.name.split("/"))):
                    continue
                cls, _ = include_census.classify_outside(inc.name, inc.quote, set())
                if cls != "system" or inc.quote != "<":
                    bad.append("%s: %s" % (h, inc.name))
        self.assertEqual(bad, [])

    def test_backend_functions_declared_not_defined(self):
        decl = _read(os.path.join(STUBS_INCLUDE, "android", "hardware_buffer.h"))
        for fn in BACKEND_FUNCTIONS:
            self.assertRegex(decl, r"\b%s\s*\([^;{]*\)\s*;" % fn)
        for dirpath, _dirnames, names in os.walk(STUBS):
            for n in names:
                if not n.endswith((".h", ".cpp", ".c")):
                    continue
                code, _ = lex(_read(os.path.join(dirpath, n)))
                for fn in BACKEND_FUNCTIONS:
                    self.assertIsNone(re.search(r"\b%s\s*\([^;{]*\)\s*\{" % fn, code), "%s defines %s" % (n, fn))

    def test_gbm_backend_defines_them_as_declared(self):
        """G9's DrmBuffersGbm.cpp defines struct AHardwareBuffer and the four functions, extern "C",
        with the stub's return and parameter types; and the Linux platform uses no
        AHARDWAREBUFFER_ name the stub does not declare (the budget only shrinks, G4-2)."""
        if not os.path.isfile(GBM_BACKEND):
            self.skipTest("no platform/linux/display/DrmBuffersGbm.cpp")
        decl = _read(os.path.join(STUBS_INCLUDE, "android", "hardware_buffer.h"))
        gbm = _read(GBM_BACKEND)
        self.assertRegex(gbm, r"(?m)^struct AHardwareBuffer \{")
        self.assertIn("typedef struct AHardwareBuffer AHardwareBuffer;", decl)
        self.assertNotRegex(decl, r"struct AHardwareBuffer\s*\{", "the stub's AHardwareBuffer must stay opaque")

        def params(text):
            out = []
            for p in text.split(","):
                p = " ".join(p.split())
                p = re.sub(r"\s*\b[A-Za-z_]\w*$", "", p)            # the parameter's name
                out.append(p.replace(" *", "*").replace("* ", "*"))
            return out

        for fn in BACKEND_FUNCTIONS:
            d = re.search(r"(?m)^(\w[\w ]*?\**)\s*\b%s\s*\(([^)]*)\)\s*;" % fn, decl)
            g = re.search(r'(?m)^extern "C" (\w[\w ]*?\**)\s*\b%s\s*\(([^)]*)\)\s*\{' % fn, gbm)
            self.assertIsNotNone(d, "hardware_buffer.h does not declare %s" % fn)
            self.assertIsNotNone(g, 'DrmBuffersGbm.cpp does not define %s extern "C"' % fn)
            self.assertEqual(d.group(1).strip(), g.group(1).strip(), fn)
            self.assertEqual(params(d.group(2)), params(g.group(2)), fn)

        declared = set(re.findall(r"\bAHARDWAREBUFFER_\w+", decl))
        used = set()
        for dirpath, _dirnames, names in os.walk(os.path.join(NANO, "platform", "linux")):
            for n in names:
                if n.endswith((".cpp", ".h")):
                    code, _ = lex(_read(os.path.join(dirpath, n)))
                    used |= set(re.findall(r"\bAHARDWAREBUFFER_\w+", code))
        self.assertEqual(sorted(used - declared), [])

    def test_linux_defines_rgb_follow(self):
        """nanoRgbFollowSample() is declared by NanoMenuDrm.h and called every home frame, but its
        Android definition (NanoMenuRgbFollow.cpp) is [firmware]: each platform defines an inert one
        (G5-7) instead of leaving it to the stub generator."""
        rx = re.compile(r"(?m)^void nanoRgbFollowSample\(\)\s*\{\s*\}")
        for folder in ("linux", "host"):
            hits = []
            for dirpath, _dirnames, names in os.walk(os.path.join(NANO, "platform", folder)):
                for n in names:
                    if n.endswith(".cpp") and rx.search(_read(os.path.join(dirpath, n))):
                        hits.append(n)
            self.assertEqual(len(hits), 1, "platform/%s: %s" % (folder, hits))

    def test_stop_process_returns(self):
        """main_linux.cpp and main_headless.cpp end when NanoMenu::join() returns, after threadLoop's
        IPCThreadState::stopProcess(): the stub must only log, never exit or block (G9's hand-off)."""
        code, _ = lex(_read(os.path.join(STUBS_INCLUDE, "binder", "IPCThreadState.h")))
        m = re.search(r"\bvoid stopProcess\([^)]*\)\s*\{([^}]*)\}", code)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1).strip(), "NANOSTUB_ONCE();")

    def test_defined_stubs_log_once(self):
        """Every function the stubs' sources define logs at its first call (NANOSTUB_ONCE), except
        the deletes and closes of objects that never exist."""
        bad = []
        src = os.path.join(STUBS, "src")
        for n in sorted(os.listdir(src)):
            if not n.endswith(".cpp") or n == "stub_log.cpp":
                continue
            code, _ = lex(_read(os.path.join(src, n)))
            for m in re.finditer(r"\b([A-Z]\w+_\w+)\s*\([^;{]*\)\s*\{", code):
                name = m.group(1)
                body_end = include_census_match_close(code, m.end() - 1)
                body = code[m.end():body_end]
                if re.search(r"_(delete|close)$", name):
                    continue
                if "NANOSTUB_ONCE()" not in body:
                    bad.append("%s: %s" % (n, name))
        self.assertEqual(bad, [])

    def test_fork_only(self):
        cfg = Upstream.load(ROOT)
        for rel in ("compat/stubs/include/gui/Surface.h", "compat/stubs/src/ndk_media.cpp", "compat/stubs/stubs.mk",
                    "test/host/stubgen/gen.py"):
            self.assertTrue(cfg.is_fork_only(rel), rel)


def include_census_match_close(code: str, open_idx: int) -> int:
    depth = 0
    for i in range(open_idx, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(code)


@unittest.skipUnless(HAVE, "no nano/compat/stubs in this checkout")
class SplitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.census = include_census.Census(ROOT)
        cls.split = include_census.Split(cls.census)

    def test_sources_txt_is_the_census_split(self):
        path = os.path.join(ROOT, *include_census.SOURCES_REL.split("/"))
        with open(path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
        self.assertEqual(text, include_census.rewrite_sources(text, self.split),
                         "nano/build/sources.txt's split is stale: py -3.12 tools/nano-tree/include_census.py write")

    def test_hand_decisions(self):
        """The wave-2 hand-offs (G9a) and the design's [drm] rule."""
        s = self.split.sections
        self.assertEqual(s["drm"], ["NanoMenuDrm.cpp"])
        self.assertIn("NanoMenuDrmBuffers.cpp", s["firmware"])
        self.assertIn("main.cpp", s["firmware"])
        for media in ("NanoMenuVideo.cpp", "NanoMenuPhotos.cpp", "NanoMenuMusic.cpp", "NanoMenuOverlay.cpp",
                      "NanoSdMove.cpp", "NanoMenuRadio.cpp", "NanoMenuIptv.cpp", "NanoMenuPdf.cpp", "NanoMenuMtp.cpp"):
            self.assertIn(media, s["core"])
        self.assertLessEqual(set(s["dropped-linux"]), set(s["firmware"]))

    def test_every_firmware_file_has_a_reason(self):
        for p in self.split.sections["firmware"]:
            self.assertTrue(self.split.reasons.get("firmware:" + p), p)

    def test_unstubbed_header_must_be_decided(self):
        """A [core] file reaching an Android-only header that is neither stubbed nor in NOT_STUBBED
        stops the census."""
        saved = dict(include_census.NOT_STUBBED)
        try:
            include_census.NOT_STUBBED.pop("jni.h")
            with self.assertRaises(NanoTreeError):
                include_census.Split(self.census)
        finally:
            include_census.NOT_STUBBED.clear()
            include_census.NOT_STUBBED.update(saved)

    def test_rewrite_keeps_the_rest(self):
        text = "# head\n[core]\nold.cpp\n\n# about drm\n[drm]\n\n[firmware]\nx.cpp\n[dropped-linux]\n# keep\n[linux]\nl.cpp\n"

        class FakeSplit:
            def section_lines(self, sec):
                return {"core": ["a.cpp", "b.cpp"], "drm": ["d.cpp"], "firmware": ["# f.cpp: why", "f.cpp"],
                        "dropped-linux": []}[sec]
        out = include_census.rewrite_sources(text, FakeSplit())
        self.assertEqual(out, "# head\n[core]\na.cpp\nb.cpp\n\n# about drm\n[drm]\nd.cpp\n\n[firmware]\n# f.cpp: why\n"
                              "f.cpp\n[dropped-linux]\n[linux]\nl.cpp\n")


class StubGen(unittest.TestCase):
    """gen.py on canned nm output (testdata/core.nm, other.nm: what `nm -C` gives for testdata/core.cpp
    and another object); selftest.sh runs the same through a real compiler and linker."""

    @classmethod
    def setUpClass(cls):
        cls.gen = _stubgen()
        cls.td = os.path.join(STUBGEN_DIR, "testdata")

    def run_gen(self, nm_files, scopes=(), decls=("fake.h",)):
        gen = self.gen
        texts = [_read(os.path.join(self.td, n)) for n in nm_files]
        dropped = gen.dropped_definitions(_read(os.path.join(self.td, "dropped.cpp")))
        files = gen.load_decl_files([os.path.join(self.td, d) for d in decls])
        return gen.generate(gen.missing_symbols(texts), files, dropped, list(scopes), self.td)

    def test_parse_nm(self):
        u, d = self.gen.parse_nm("x.o:\n                 U foo(int)\n0000000000000010 T bar()\n"
                                 "lib.a:y.o:0000000000000000 W baz\n                 w __gmon_start__\n")
        self.assertEqual(u, {"foo(int)"})
        self.assertEqual(d, {"bar()", "baz"})

    def test_symbol(self):
        s = self.gen.Symbol("android::FakeMenu::label[abi:cxx11]() const")
        self.assertEqual((s.scopes, s.name, s.params, s.trailer), (["android", "FakeMenu"], "label", [], "const"))
        s = self.gen.Symbol("android::Runner::init(std::__cxx11::basic_string<char, std::char_traits<char>, "
                            "std::allocator<char> > const&, int)")
        self.assertEqual(len(s.params), 2)
        d = self.gen.Symbol("android::FakeMenu::sCount")
        self.assertFalse(d.is_function)
        with self.assertRaises(self.gen.GenError):
            self.gen.Symbol("android::operator==(android::A, android::A)")

    def test_dropped_definitions(self):
        self.assertEqual(sorted(self.gen.dropped_definitions(_read(os.path.join(self.td, "dropped.cpp")))), [
            "android::FakeMenu::FakeMenu", "android::FakeMenu::drasticParkEnabled", "android::FakeMenu::ids",
            "android::FakeMenu::label", "android::FakeMenu::mode", "android::FakeMenu::nsTick",
            "android::FakeMenu::sCount", "android::FakeMenu::~FakeMenu", "android::Runner::Runner",
            "android::Runner::init", "android::Runner::matrix", "android::gDroppedValue",
            "android::nanoRgbFollowSample"])

    def test_generate(self):
        text, done, errors = self.run_gen(["core.nm", "other.nm"])
        self.assertEqual(errors, [])
        # Runner::matrix is defined by other.nm's object; FakeMenu::core is inline (W); printf and the
        # rest are not the dropped file's.
        self.assertEqual(len(done), 12)
        self.assertNotIn("android::Runner::matrix()", done)
        self.assertIn('#include "fake.h"', text)
        for want in (
                "auto android::FakeMenu::drasticParkEnabled() const -> bool {",
                "auto android::FakeMenu::label() const -> const std::string& {",
                "auto android::FakeMenu::mode() const -> Mode {",
                "auto android::FakeMenu::ids(unsigned int) noexcept -> std::vector<int> {",
                "android::FakeMenu::FakeMenu() {}",
                "android::FakeMenu::~FakeMenu() {}",
                "decltype(android::FakeMenu::sCount) android::FakeMenu::sCount{};",
                "decltype(android::gDroppedValue) android::gDroppedValue{};",
                "namespace android {\nauto nanoRgbFollowSample() -> void {",
                "return ::nanostub_gen::Zero<bool>::get();"):
            self.assertIn(want, text)
        self.assertEqual(text.count("NANOSTUB_ONCE();"), 7)   # 12 less 3 constructors/destructors and 2 variables

    def test_scope_selects_without_from(self):
        gen = self.gen
        files = gen.load_decl_files([os.path.join(self.td, "fake.h")])
        text, done, errors = gen.generate(gen.missing_symbols([_read(os.path.join(self.td, "core.nm"))]), files,
                                          set(), ["android::FakeMenu::"], self.td)
        self.assertEqual(errors, [])
        self.assertTrue(done)
        self.assertTrue(all(d.startswith("android::FakeMenu::") for d in done))

    def test_errors(self):
        gen = self.gen
        files = gen.load_decl_files([os.path.join(self.td, "fake.h")])
        _t, _d, errors = gen.generate(["android::FakeMenu::nowhere(int)"], files, set(), ["android::"], self.td)
        self.assertEqual(len(errors), 1)
        self.assertIn("no declaration", errors[0])
        amb = gen.DeclFile("amb.h", "namespace android {\nint twice(int);\nnamespace x { float twice(int); }\n}\n")
        _t, _d, errors = gen.generate(["android::twice(int)"], [amb], set(), ["android::"], None)
        self.assertIn("ambiguous", errors[0])


@unittest.skipUnless(os.path.isdir(NANO), "no nano/ in this checkout")
class SharedFileRatchets(unittest.TestCase):
    def test_bionic_identifiers_match_baseline(self):
        uses = bionic_uses()
        baseline = set(read_list(BIONIC)) if os.path.isfile(BIONIC) else set()
        msg = ["new: %s (in %s)" % (k, ", ".join(uses[k][:3])) for k in sorted(set(uses) - baseline)]
        msg += ["gone: %s" % k for k in sorted(baseline - set(uses))]
        self.assertFalse(msg, "Bionic-only identifiers in shared files differ from tools/nano-tree/"
                              "bionic-identifiers.txt; give a new one a shim, then --update-bionic:\n  "
                         + "\n  ".join(msg))

    def test_bionic_identifiers_have_shims(self):
        shims = _shim_text()
        self.assertEqual([k for k in read_list(BIONIC) if not re.search(r"\b%s\b" % re.escape(k), shims)], [])

    def test_block_device_files_match_baseline(self):
        have = block_files()
        baseline = read_list(BLOCK) if os.path.isfile(BLOCK) else []
        self.assertEqual(have, baseline, "shared files naming /dev/block or mmcblk differ from tools/nano-tree/"
                                         "block-paths.txt (review against I40, then --update-block)")


def _update(argv: List[str]) -> int:
    if argv == ["--update-budget"]:
        n = stub_count()
        old = read_budget()
        if old is not None and n > old:
            print("stubs-budget.txt: %d names, over the budget of %d; the budget is not raised by this command"
                  % (n, old), file=sys.stderr)
            return 1
        write_list(BUDGET, BUDGET_HEADER, [str(n)])
        print("stubs-budget.txt: %d" % n)
        return 0
    if argv == ["--init-budget"]:
        write_list(BUDGET, BUDGET_HEADER, [str(stub_count())])
        return 0
    if argv == ["--update-bionic"]:
        uses = bionic_uses()
        write_list(BIONIC, BIONIC_HEADER, list(uses))
        print("bionic-identifiers.txt: %d" % len(uses))
        return 0
    if argv == ["--update-block"]:
        files = block_files()
        write_list(BLOCK, BLOCK_HEADER, files)
        print("block-paths.txt: %s" % ", ".join(files))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    if sys.argv[1:2] and sys.argv[1].startswith("--"):
        sys.exit(_update(sys.argv[1:]))
    unittest.main()
