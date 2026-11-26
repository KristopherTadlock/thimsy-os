"""tools/nano-tree on synthetic repositories made with `git init` in temporary directories: patch
splitting and splicing, the A.2 rule, fork-only paths, modes, binary sections, regen's refusal,
the three-way imports of A.5 (import-patch, the merge driver, import-base), compose,
export-assets, fetch over file:// and the Blueprint reader. docs/linux/IMPLEMENTATION.md A.6."""
from __future__ import annotations

import contextlib
import io
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TOOL_DIR)

import nano_tree  # noqa: E402
from nanotree import androidbp  # noqa: E402
from nanotree.compose import files_problems  # noqa: E402
from nanotree.config import NanoTreeError, Upstream, glob_regex  # noqa: E402
from nanotree.patchfile import Patch, first_difference, header_path, sha256, splice  # noqa: E402
from nanotree.regen import keep_committed_binaries  # noqa: E402

NANO = "frameworks/base/cmds/gammaos-nano"
SIB = "frameworks/base/cmds/drastic-nano/DsScreenLayout.h"
TOOL = os.path.join(TOOL_DIR, "nano_tree.py")
ANDROID_BP = """// Test module set.
package { default_applicable_licenses: ["x"] }
common_srcs = ["a.cpp"]
filegroup { name: "fg", srcs: ["keep.h"] }
cc_binary {
    name: "bin",
    srcs: common_srcs + [":fg", "new.cpp"], /* new.cpp arrives with the patch */
    cflags: ["-Wall"],
    enabled: true,
}
prebuilt_etc { name: "icons", srcs: ["ps3xmb/icons/*.png"], sub_dir: "nano_xmb/icons" }
prebuilt_etc { name: "runner", src: "run.sh", filename: "r.sh", sub_dir: "bin" }
"""


def lines(n, tag=""):
    return "".join("line%d%s\n" % (i, tag) for i in range(1, n + 1)).encode()


def edit_line(data: bytes, n: int, text: str) -> bytes:
    ls = data.split(b"\n")
    ls[n - 1] = text.encode()
    return b"\n".join(ls)


ENV = {}


def git(cwd, *args, input=None, check=True, env=None):
    e = dict(os.environ)
    e.update(ENV)
    if env:
        e.update(env)
    proc = subprocess.run(["git"] + list(args), cwd=cwd, input=input, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=e)
    if check and proc.returncode != 0:
        raise AssertionError("git %s: %s" % (" ".join(args), proc.stderr.decode("utf-8", "replace")))
    return proc.stdout


def write(path, data: bytes):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def read(path) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def rmtree(path):
    def fix(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=fix)


class Upstream_(object):
    """The synthetic GammaOS repository U and the patches made from it."""

    def __init__(self, top):
        self.dir = os.path.join(top, "U")
        os.makedirs(self.dir)
        g = lambda *a, **k: git(self.dir, *a, **k)  # noqa: E731
        g("init", "-q", ".")
        g("config", "uploadpack.allowFilter", "true")
        g("config", "uploadpack.allowAnySHA1InWant", "true")
        n = os.path.join(self.dir, *NANO.split("/"))
        write(os.path.join(n, "Android.bp"), ANDROID_BP.encode())
        write(os.path.join(n, "a.cpp"), lines(10))
        write(os.path.join(n, "keep.h"), lines(5, " keep"))
        write(os.path.join(n, "gone.cpp"), lines(3, " gone"))
        write(os.path.join(n, "run.sh"), b"#!/bin/sh\necho run\n")
        write(os.path.join(n, "ps3xmb", "icons", "x.png"), b"\x89PNG\0x-base" * 50)
        write(os.path.join(n, "ps3xmb", "icons", "y.png"), b"\x89PNG\0y-base" * 50)
        write(os.path.join(self.dir, *SIB.split("/")), lines(4, " sib"))
        write(os.path.join(self.dir, "other", "file.txt"), lines(3, " other"))
        g("add", "-A")
        g("update-index", "--chmod=+x", NANO + "/run.sh")
        g("commit", "-q", "-m", "base")
        self.base = g("rev-parse", "HEAD").decode().strip()
        # P0: the Thimsy patch at the fork's start.
        self.p0_commit = self.commit_from(self.base, {
            "a.cpp": edit_line(lines(10), 2, "line2 thimsy"),
            "gone.cpp": None,
            "new.cpp": lines(4, " new"),
            "ps3xmb/icons/x.png": b"\x89PNG\0x-thimsy" * 60,
        }, outside={"other/file.txt": lines(3, " other p0"), SIB: lines(4, " sib p0")},
            exe=["new.cpp"])
        self.p0 = self.diff(self.base, self.p0_commit)
        # P1: nano-144 later: another hunk of a.cpp, a file added, one deleted, the outside moved on.
        a1 = edit_line(edit_line(lines(10), 2, "line2 thimsy"), 8, "line8 nano144")
        self.p1_commit = self.commit_from(self.p0_commit, {
            "a.cpp": a1, "n2.cpp": lines(2, " n2"), "keep.h": None,
        }, outside={"other/file.txt": lines(3, " other p1")})
        self.p1 = self.diff(self.base, self.p1_commit)
        # P2: only the a.cpp hunk and the outside; P3: only the outside.
        self.p2_commit = self.commit_from(self.p0_commit, {"a.cpp": a1},
                                          outside={"other/file.txt": lines(3, " other p2")})
        self.p2 = self.diff(self.base, self.p2_commit)
        self.p3_commit = self.commit_from(self.p0_commit, {}, outside={"other/file.txt": lines(3, " other p3")})
        self.p3 = self.diff(self.base, self.p3_commit)
        # BASE2: GammaOS moves: keep.h, an asset nobody overlays, and a.cpp far from Thimsy's hunk.
        self.base2 = self.commit_from(self.base, {
            "keep.h": edit_line(lines(5, " keep"), 1, "line1 keep upstream2"),
            "ps3xmb/icons/y.png": b"\x89PNG\0y-base2" * 50,
            "a.cpp": edit_line(lines(10), 9, "line9 upstream2"),
        })

    def commit_from(self, parent, nano_changes, outside=None, exe=()):
        index = os.path.join(self.dir, ".git", "tmp-index")
        env = {"GIT_INDEX_FILE": index}
        if os.path.exists(index):
            os.remove(index)
        git(self.dir, "read-tree", parent, env=env)
        changes = {NANO + "/" + k: v for k, v in nano_changes.items()}
        changes.update(outside or {})
        for path, data in changes.items():
            if data is None:
                git(self.dir, "update-index", "--force-remove", "--", path, env=env)
                continue
            sha = git(self.dir, "hash-object", "-w", "--stdin", input=data).decode().strip()
            mode = "100755" if path[len(NANO) + 1:] in exe else "100644"
            git(self.dir, "update-index", "--add", "--cacheinfo", "%s,%s,%s" % (mode, sha, path), env=env)
        tree = git(self.dir, "write-tree", env=env).decode().strip()
        os.remove(index)
        return git(self.dir, "commit-tree", tree, "-p", parent, "-m", "c").decode().strip()

    def diff(self, a, b) -> bytes:
        return git(self.dir, "-c", "core.abbrev=11", "-c", "core.compression=1", "diff-tree", "-p",
                   "--binary", "--abbrev=11", "--no-renames", a, b)

    def applied(self, commit, dest):
        """COMMIT's Nano folder written out, as an applied AOSP tree would hold it."""
        index = os.path.join(self.dir, ".git", "tmp-index2")
        env = {"GIT_INDEX_FILE": index}
        git(self.dir, "read-tree", commit, env=env)
        git(self.dir, "checkout-index", "-a", "--prefix=" + dest.replace("\\", "/") + "/", env=env)
        os.remove(index)
        return os.path.join(dest, *NANO.split("/"))


_SHARED = {}


def setUpModule():
    top = tempfile.mkdtemp(prefix="nano-tree-test-")
    home = os.path.join(top, "home")
    os.makedirs(home)
    ENV.update({"GIT_CONFIG_NOSYSTEM": "1", "HOME": home, "XDG_CONFIG_HOME": home,
                "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                "GIT_COMMITTER_EMAIL": "t@t", "GIT_ALLOW_PROTOCOL": "file"})
    env = mock.patch.dict(os.environ, ENV)
    env.start()
    for k in ("NANO_TREE_ROOT", "NANO_TREE_CACHE", "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        os.environ.pop(k, None)
    _SHARED.update(top=top, env=env, u=Upstream_(top), template=None, n=0)


def tearDownModule():
    _SHARED["env"].stop()
    rmtree(_SHARED["top"])


class Base(unittest.TestCase):
    """The synthetic upstream is made once per module; each test gets its own fork."""

    def setUp(self):
        self.top = _SHARED["top"]
        self.u = _SHARED["u"]

    # -- the fork
    def make_fork(self):
        """A fresh fork: a clone of one made (and checked) once per module, with its cache."""
        _SHARED["n"] += 1
        self.n = _SHARED["n"]
        if _SHARED["template"] is None:
            _SHARED["template"] = self.build_fork(os.path.join(self.top, "template"))
        template = _SHARED["template"]
        f = os.path.join(self.top, "F%d" % self.n)
        git(self.top, "clone", "-q", "--no-hardlinks", template.replace("\\", "/"), f.replace("\\", "/"))
        self.f = f
        self.g("config", "core.autocrlf", "false")
        self.g("branch", "-q", "nano144", "origin/nano144")
        self.g("remote", "remove", "origin")
        shutil.copytree(os.path.join(template, "work"), os.path.join(f, "work"))
        return f

    def build_fork(self, f):
        os.makedirs(f)
        self.f = f
        g = self.g
        g("init", "-q", ".")
        g("config", "core.autocrlf", "false")
        write(os.path.join(f, ".gitignore"), b"/work/\n__pycache__/\n")
        write(os.path.join(f, "tools", "android-launcher", "fetch_nano.sh"),
              ("#!/bin/sh\nBASE=%s\n" % self.u.base).encode())
        write(os.path.join(f, "patches", "thimsy", "0001-thimsy.patch"), self.u.p0)
        g("add", "-A")
        g("commit", "-q", "-m", "root (nano-144's history)")
        g("branch", "nano144")
        write(os.path.join(f, ".gitattributes"),
              b"/nano/** -text\npatches/thimsy/0001-thimsy.patch merge=nanopatch -diff\n"
              b"tools/nano-tree/block.sha256 merge=nanoblock\n")
        cfg = {
            "repo": pathlib.Path(self.u.dir).as_uri(), "base": self.u.base, "path": NANO,
            "assets": ["ps3xmb/**"], "abbrev": 11, "deleted": [],
            "fork_only": ["platform/linux/**", "build/**", "CMakeLists.txt"], "siblings": [SIB],
        }
        import json
        write(os.path.join(f, "tools", "nano-tree", "upstream.json"), json.dumps(cfg, indent=2).encode())
        self.assertEqual(self.tool("fetch", "--from-cache", self.u.dir)[0], 0)
        applied = self.u.applied(self.u.p0_commit, os.path.join(self.top, "applied%d" % self.n))
        rc, out, err = self.tool("import-tree", applied)
        self.assertEqual(rc, 0, err)
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 0, err)
        g("add", "-A")
        g("commit", "-q", "-m", "linux: nano/")
        return f

    def g(self, *args, **kw):
        return git(self.f, *args, **kw)

    def tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = nano_tree.main(["--root", self.f] + list(args))
        return rc, out.getvalue(), err.getvalue()

    def nano(self, rel) -> str:
        return os.path.join(self.f, "nano", *rel.split("/"))

    def patch(self) -> bytes:
        return read(os.path.join(self.f, "patches", "thimsy", "0001-thimsy.patch"))

    def index_modes(self):
        out = self.g("ls-files", "-s", "nano").decode().splitlines()
        return {ln.split("\t")[1][len("nano/"):]: ln.split()[0] for ln in out}

    def driver(self, block=True):
        for name, sub in (("nanopatch", "merge-patch"), ("nanoblock", "merge-block"))[:2 if block else 1]:
            cmd = '"%s" "%s" --root "%s" %s %%O %%A %%B %%P' % (
                sys.executable.replace("\\", "/"), TOOL.replace("\\", "/"), self.f.replace("\\", "/"), sub)
            self.g("config", "merge.%s.driver" % name, cmd)


class Unit(unittest.TestCase):
    def test_header_path(self):
        self.assertEqual(header_path(b"diff --git a/x/y.c b/x/y.c"), "x/y.c")
        self.assertEqual(header_path(b"diff --git a/x y/z b/x y/z"), "x y/z")
        self.assertEqual(header_path(b'diff --git "a/q\\"t\\303\\251" "b/q\\"t\\303\\251"'), 'q"té')

    def test_split_and_splice(self):
        data = (b"pre\n" + b"diff --git a/a/1 b/a/1\n+x\n" + b"diff --git a/n/1 b/n/1\n+y\n"
                + b"diff --git a/n/2 b/n/2\n+z\n" + b"diff --git a/z/1 b/z/1\n+w\n")
        p = Patch.parse(data)
        self.assertEqual(p.to_bytes(), data)
        self.assertEqual([s.path for s in p.sections], ["a/1", "n/1", "n/2", "z/1"])
        before, block, after = p.split_block("n/")
        self.assertEqual(before + block + after, data)
        self.assertEqual(block, b"diff --git a/n/1 b/n/1\n+y\ndiff --git a/n/2 b/n/2\n+z\n")
        new = splice(data, "n/", b"diff --git a/n/3 b/n/3\n+q\n")
        self.assertEqual(new, b"pre\ndiff --git a/a/1 b/a/1\n+x\ndiff --git a/n/3 b/n/3\n+q\n"
                              b"diff --git a/z/1 b/z/1\n+w\n")
        self.assertEqual(Patch.parse(data).block_range("m/"), (1, 1))
        self.assertEqual(p.unsorted(), [])
        bad = Patch.parse(b"diff --git a/n/1 b/n/1\n+y\ndiff --git a/o/1 b/o/1\n+x\ndiff --git a/n/2 b/n/2\n")
        with self.assertRaises(NanoTreeError):
            bad.block_range("n/")
        self.assertEqual(len(bad.unsorted()), 1)

    def test_first_difference(self):
        self.assertIsNone(first_difference(b"a\nb\n", b"a\nb\n"))
        self.assertEqual(first_difference(b"a\nb\nc", b"a\nX\nc"), (2, b"b", b"X"))

    def test_section_header(self):
        s = Patch.parse(b"diff --git a/n/x b/n/x\nnew file mode 100755\nindex 0000000..1234567\n"
                        b"--- /dev/null\n+++ b/n/x\n@@ -0,0 +1 @@\n+a\n").sections[0]
        self.assertTrue(s.added)
        self.assertEqual(s.new_mode(), "100755")
        self.assertEqual(s.post_id(), "1234567")
        s = Patch.parse(b"diff --git a/n/x b/n/x\nold mode 100644\nnew mode 100755\n").sections[0]
        self.assertEqual(s.new_mode(), "100755")

    def test_globs(self):
        self.assertTrue(glob_regex("ps3xmb/**").match("ps3xmb/a/b.png"))
        self.assertFalse(glob_regex("ps3xmb/**").match("ps3xmbx/a"))
        self.assertTrue(glob_regex("CMakeLists.txt").match("CMakeLists.txt"))
        self.assertFalse(glob_regex("CMakeLists.txt").match("rcheevos/CMakeLists.txt"))
        self.assertTrue(glob_regex("**/x.h").match("x.h"))
        self.assertTrue(glob_regex("**/x.h").match("a/b/x.h"))
        self.assertTrue(glob_regex("a/*.c").match("a/b.c"))
        self.assertFalse(glob_regex("a/*.c").match("a/b/c.c"))
        self.assertTrue(glob_regex("t/*.7z.[0-9][0-9][0-9]").match("t/a.7z.001"))

    def test_keep_committed_binaries(self):
        head = (b"diff --git a/n/b.bin b/n/b.bin\nnew file mode 100644\nindex 0000000000000000000000000000000000000000.."
                b"1111111111111111111111111111111111111111\nGIT binary patch\nliteral 3\n")
        gen = head + b"KcmZQzWB>~2\n\nliteral 0\nHcmV?d00001\n\n"
        committed = head + b"OTHERzlibBYTES\n\nliteral 0\nHcmV?d00001\n\n"
        self.assertEqual(keep_committed_binaries(gen, [committed]), committed)
        other = committed.replace(b"1111111111", b"2222222222")
        self.assertEqual(keep_committed_binaries(gen, [other]), gen)

    def test_androidbp(self):
        bp = androidbp.Blueprint(ANDROID_BP)
        m = bp.module("bin")
        self.assertEqual(m.type, "cc_binary")
        self.assertEqual(bp.srcs(m), ["a.cpp", "keep.h", "new.cpp"])
        self.assertIs(m["enabled"], True)
        self.assertEqual(bp.variables["common_srcs"], ["a.cpp"])
        files = ["ps3xmb/icons/x.png", "ps3xmb/icons/y.png", "ps3xmb/icons/sub/z.png", "run.sh"]
        inst = androidbp.etc_installs(bp, files)
        self.assertEqual(sorted(i[2] for i in inst),
                         ["bin/r.sh", "nano_xmb/icons/x.png", "nano_xmb/icons/y.png"])
        with self.assertRaises(NanoTreeError):
            androidbp.Blueprint("cc_binary { srcs: [unknown_var] }")

    def test_androidbp_real_file(self):
        root = os.path.dirname(os.path.dirname(TOOL_DIR))
        path = os.path.join(root, "nano", "Android.bp")
        if not os.path.isfile(path):
            self.skipTest("no nano/Android.bp")
        bp = androidbp.Blueprint.load(path)
        nano = bp.module("gammaos-nano")
        self.assertIn("main.cpp", bp.srcs(nano))
        self.assertTrue(bp.of_type("prebuilt_etc"))
        self.assertTrue(bp.of_type("cc_test_host"))

    def test_files_problems(self):
        cfg = Upstream({"repo": "r", "base": "b", "path": "n", "assets": ["ps3xmb/**"],
                        "fork_only": ["build/**"]})
        block = Patch.parse(b"diff --git a/n/a.c b/n/a.c\nindex 1..2 100644\n"
                            b"diff --git a/n/gone.c b/n/gone.c\ndeleted file mode 100644\n"
                            b"diff --git a/n/ps3xmb/x.png b/n/ps3xmb/x.png\nindex 1..2 100644\n").sections
        base = {"a.c": ("100644", "1"), "k.h": ("100644", "3"), "gone.c": ("100644", "4"),
                "ps3xmb/x.png": ("100644", "5"), "ps3xmb/y.png": ("100644", "6")}
        good = {"a.c": 1, "k.h": 1, "ps3xmb/x.png": 1, "build/x": 1}
        self.assertEqual(files_problems(cfg, good, block, base), [])
        self.assertEqual(files_problems(cfg, {"a.c": 1, "ps3xmb/x.png": 1}, block, None), [])
        bad = {"a.c": 1, "gone.c": 1, "ps3xmb/x.png": 1, "ps3xmb/y.png": 1, "stray.c": 1}
        probs = files_problems(cfg, bad, block, base)
        self.assertEqual(len(probs), 4, probs)  # k.h missing, gone.c, y.png, stray.c


class Regen(Base):
    def test_import_tree_follows_a2(self):
        self.make_fork()
        self.assertTrue(os.path.isfile(self.nano("ps3xmb/icons/x.png")))   # modified asset: committed
        self.assertFalse(os.path.exists(self.nano("ps3xmb/icons/y.png")))  # untouched asset: fetched
        self.assertFalse(os.path.exists(self.nano("gone.cpp")))
        modes = self.index_modes()
        self.assertEqual(modes["new.cpp"], "100755")
        self.assertEqual(modes["run.sh"], "100755")
        self.assertEqual(modes["a.cpp"], "100644")
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        self.assertEqual(read(os.path.join(self.f, "tools", "nano-tree", "block.sha256")).strip().decode(),
                         sha256(Patch.parse(self.u.p0).split_block(NANO + "/")[1]))

    def test_edit_rewrites_only_its_section(self):
        self.make_fork()
        old = Patch.parse(self.patch())
        write(self.nano("a.cpp"), edit_line(read(self.nano("a.cpp")), 5, "line5 linux"))
        rc, out, err = self.tool("regen", "--check")
        self.assertEqual(rc, 1)
        self.assertIn("first at line", out)
        self.assertEqual(self.tool("regen")[0], 0)
        new = Patch.parse(self.patch())
        changed = [b.path for a, b in zip(old.sections, new.sections) if a.data != b.data]
        self.assertEqual(changed, [NANO + "/a.cpp"])
        self.assertEqual(len(old.sections), len(new.sections))
        self.assertIn(b"+line5 linux", new.sections[[s.path for s in new.sections].index(NANO + "/a.cpp")].data)
        self.assertEqual(self.tool("regen", "--check")[0], 0)

    def test_fork_only_paths_stay_out(self):
        self.make_fork()
        write(self.nano("platform/linux/x.cpp"), b"int x;\n")
        write(self.nano("CMakeLists.txt"), b"project(x)\n")
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        write(self.nano("compat/include/y.h"), b"int y;\n")
        self.assertEqual(self.tool("regen", "--check")[0], 1)
        self.assertEqual(self.tool("regen")[0], 0)
        self.assertIn(b"b/" + NANO.encode() + b"/compat/include/y.h", self.patch())
        self.assertNotIn(b"platform/linux", self.patch())

    def test_intent_to_add(self):
        """`git add -N` leaves the empty blob in the index: regen hashes the file on disk."""
        self.make_fork()
        write(self.nano("compat/include/z.h"), b"int z;\n")
        self.g("add", "-N", "nano/compat/include/z.h")
        self.assertEqual(self.tool("regen", "--check")[0], 1)
        self.assertEqual(self.tool("regen")[0], 0)
        self.assertIn(b"+int z;", self.patch())
        self.assertNotIn(b"e69de29", self.patch())
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        self.g("add", "nano/compat/include/z.h")
        self.assertEqual(self.tool("regen", "--check")[0], 0)

    def test_warns_about_uncommitted_nano_files(self):
        """gfix2 (android-safety-4): regen warns, without failing, when the block it builds
        includes nano/ files that are untracked or not committed."""
        self.make_fork()
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("warning", err)
        write(self.nano("compat/include/u.h"), b"int u;\n")                       # untracked
        write(self.nano("a.cpp"), edit_line(read(self.nano("a.cpp")), 3, "line3 dirty"))   # unstaged
        write(self.nano("platform/linux/f.cpp"), b"int f;\n")                    # fork-only: never in the block
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 0, err)
        self.assertIn("regen: warning: the block includes 2 nano/ file(s) that are not committed", err)
        self.assertIn("nano/a.cpp", err)
        self.assertIn("nano/compat/include/u.h", err)
        self.assertNotIn("platform/linux", err)
        self.assertIn(b"+int u;", self.patch())
        rc, out, err = self.tool("regen", "--check")
        self.assertEqual(rc, 0, out + err)
        self.assertIn("regen: warning", err)
        self.g("add", "nano/compat/include/u.h", "nano/a.cpp")                     # staged, not committed
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 0, err)
        self.assertIn("includes 2 nano/ file(s)", err)
        self.g("commit", "-q", "-m", "nano/ and its patch", "--", "nano/compat/include/u.h", "nano/a.cpp")
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 0, err)
        self.assertNotIn("warning", err)

    def test_mode_from_index(self):
        self.make_fork()
        self.g("update-index", "--chmod=+x", "nano/keep.h")
        self.assertEqual(self.tool("regen")[0], 0)
        self.assertIn(b"diff --git a/%s/keep.h b/%s/keep.h\nold mode 100644\nnew mode 100755\n"
                      % (NANO.encode(), NANO.encode()), self.patch())

    def test_refuses_a_block_changed_outside_regen(self):
        self.make_fork()
        path = os.path.join(self.f, "patches", "thimsy", "0001-thimsy.patch")
        write(path, self.patch().replace(b"+line2 thimsy", b"+line2 hand-edited"))
        rc, out, err = self.tool("regen")
        self.assertEqual(rc, 1)
        self.assertIn("import-patch", err)
        self.assertEqual(self.tool("regen", "--force")[0], 0)
        self.assertEqual(self.patch(), self.u.p0)

    def test_compose_and_export(self):
        self.make_fork()
        out = os.path.join(self.top, "composed%d" % self.n)
        rc, _o, err = self.tool("compose", out)
        self.assertEqual(rc, 0, err)
        self.assertEqual(read(os.path.join(out, "ps3xmb", "icons", "y.png")), b"\x89PNG\0y-base" * 50)
        self.assertEqual(read(os.path.join(out, "ps3xmb", "icons", "x.png")), b"\x89PNG\0x-thimsy" * 60)
        self.assertFalse(os.path.exists(os.path.join(out, "gone.cpp")))
        self.assertEqual(read(os.path.join(out, "_siblings", *SIB.split("/"))), lines(4, " sib p0"))
        rc, _o, err = self.tool("compose", out)  # again: it made the directory, so it may clear it
        self.assertEqual(rc, 0, err)
        other = os.path.join(self.top, "notcompose%d" % self.n)
        write(os.path.join(other, "keep.txt"), b"x")
        self.assertEqual(self.tool("compose", other)[0], 1)
        assets = os.path.join(self.top, "assets%d" % self.n)
        rc, _o, err = self.tool("export-assets", assets, "--from", out)
        self.assertEqual(rc, 0, err)
        self.assertEqual(read(os.path.join(assets, "nano_xmb", "icons", "y.png")), b"\x89PNG\0y-base" * 50)
        self.assertTrue(os.path.isfile(os.path.join(assets, "bin", "r.sh")))

    def test_status(self):
        self.make_fork()
        rc, out, _e = self.tool("status", "--check")
        self.assertEqual(rc, 0)
        self.assertIn("matches block.sha256", out)
        self.assertIn("NOT installed", out)
        self.assertEqual(self.tool("install-driver")[0], 0)
        self.assertIn(b"merge-patch %O %A %B %P", self.g("config", "merge.nanopatch.driver"))

    def test_fetch_over_file_url(self):
        self.make_fork()
        cache = os.path.join(self.f, "work", "nano-base")
        rmtree(cache)
        rc, out, err = self.tool("fetch")
        self.assertEqual(rc, 0, err + out)
        self.assertIn("(0 blobs missing)", out)
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        rc, out, err = self.tool("fetch", "--offline")  # complete: nothing to do
        self.assertEqual(rc, 0, err)


class Imports(Base):
    def commit_nano144(self, patch: bytes):
        self.g("checkout", "-q", "nano144")
        write(os.path.join(self.f, "patches", "thimsy", "0001-thimsy.patch"), patch)
        self.g("commit", "-q", "-am", "nano-144 moves")
        self.g("checkout", "-q", "-")

    def linux_edit(self, rel, data):
        write(self.nano(rel), data)
        self.assertEqual(self.tool("regen")[0], 0)
        self.g("add", "-A")
        self.g("commit", "-q", "-m", "linux edit")

    def test_import_patch(self):
        self.make_fork()
        self.linux_edit("a.cpp", edit_line(read(self.nano("a.cpp")), 5, "line5 linux"))
        self.commit_nano144(self.u.p1)
        rc, out, err = self.tool("import-patch", "nano144")
        self.assertEqual(rc, 0, err)
        a = read(self.nano("a.cpp"))
        self.assertIn(b"line5 linux", a)
        self.assertIn(b"line8 nano144", a)
        self.assertIn(b"line2 thimsy", a)
        self.assertEqual(read(self.nano("n2.cpp")), lines(2, " n2"))
        self.assertFalse(os.path.exists(self.nano("keep.h")))
        self.assertNotIn("keep.h", self.index_modes())
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        self.assertIn(b"other p0", self.patch())  # the fork's own framework sections stay

    def test_import_patch_conflict(self):
        self.make_fork()
        self.linux_edit("a.cpp", edit_line(read(self.nano("a.cpp")), 8, "line8 linux"))
        self.commit_nano144(self.u.p1)
        rc, out, err = self.tool("import-patch", "nano144")
        self.assertEqual(rc, 1)
        self.assertIn("CONFLICT nano/a.cpp", err)
        self.assertIn(b"<<<<<<< nano/", read(self.nano("a.cpp")))

    def test_round_trip(self):
        """Importing nano-144's change into an unchanged nano/ gives back exactly its block."""
        self.make_fork()
        self.commit_nano144(self.u.p1)
        self.assertEqual(self.tool("import-patch", "nano144")[0], 0)
        self.assertEqual(Patch.parse(self.patch()).split_block(NANO + "/")[1],
                         Patch.parse(self.u.p1).split_block(NANO + "/")[1])

    def test_merge_driver(self):
        self.make_fork()
        self.driver()
        self.linux_edit("keep.h", edit_line(read(self.nano("keep.h")), 3, "line3 linux"))
        self.commit_nano144(self.u.p2)
        proc = subprocess.run(["git", "merge", "--no-edit", "nano144"], cwd=self.f, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        self.assertNotEqual(proc.returncode, 0, "the driver asks for a review when nano/ changed")
        self.assertIn(b"nanopatch: nano/ was updated", proc.stderr + proc.stdout)
        self.assertIn(b"line8 nano144", read(self.nano("a.cpp")))
        self.assertIn(b"line3 linux", read(self.nano("keep.h")))
        merged = self.patch()
        self.assertIn(b"other p2", merged)       # nano-144's framework sections
        self.assertIn(b"+line8 nano144", merged)  # its Nano change, regenerated from nano/
        self.assertIn(b"+line3 linux", merged)    # and the fork's
        self.g("add", "-A")
        self.g("commit", "-q", "--no-edit")
        self.assertEqual(self.tool("regen", "--check")[0], 0)

    def test_merge_driver_outside_only(self):
        self.make_fork()
        self.driver()
        self.linux_edit("keep.h", edit_line(read(self.nano("keep.h")), 3, "line3 linux"))
        self.commit_nano144(self.u.p3)
        proc = subprocess.run(["git", "merge", "--no-edit", "nano144"], cwd=self.f, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(b"other p3", self.patch())
        self.assertIn(b"+line3 linux", self.patch())
        self.assertEqual(self.tool("regen", "--check")[0], 0)

    def nano144_with(self, nano_changes, tag):
        """nano-144's patch: P0 plus NANO_CHANGES, and the outside moved on (other p-TAG)."""
        c = self.u.commit_from(self.u.p0_commit, nano_changes, outside={"other/file.txt": lines(3, " other " + tag)})
        return self.u.diff(self.u.base, c)

    def test_fork_only_change_from_nano144_is_a_conflict(self):
        """android-safety-2: a nano-144 section for a fork-only path is never dropped quietly."""
        self.make_fork()
        self.driver()
        p = self.nano144_with({"build/NOTES.txt": lines(2, " notes"), "a.cpp": edit_line(read(self.nano("a.cpp")), 8, "line8 nano144")}, "p4")
        self.linux_edit("keep.h", edit_line(read(self.nano("keep.h")), 3, "line3 linux"))   # so git runs the driver
        self.commit_nano144(p)
        rc, out, err = self.tool("import-patch", "nano144")
        self.assertEqual(rc, 1)
        self.assertIn("CONFLICT nano/build/NOTES.txt: the incoming patch adds this path, but it is fork-only", err)
        self.g("reset", "-q", "--hard")
        self.g("clean", "-qfd", "--", "nano")
        proc = self.merge("nano144")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"fork-only", proc.stderr + proc.stdout)
        self.g("merge", "--abort")

    def test_regen_refuses_a_block_with_a_fork_only_section(self):
        self.make_fork()
        sec = (b"diff --git a/%s/build/x b/%s/build/x\nnew file mode 100644\nindex 00000000000..11111111111\n"
               b"--- /dev/null\n+++ b/%s/build/x\n@@ -0,0 +1 @@\n+x\n" % ((NANO.encode(),) * 3))
        data = self.patch()
        before, block, after = Patch.parse(data).split_block(NANO + "/")
        write(os.path.join(self.f, "patches", "thimsy", "0001-thimsy.patch"), before + block + sec + after)
        rc, out, err = self.tool("regen", "--force")
        self.assertNotEqual(rc, 0)
        self.assertIn("fork-only paths", out + err)

    def test_merge_driver_refuses_a_dirty_nano(self):
        """android-safety-4: nano-144's merge regenerates the block from nano/ on disk, so a stray
        file there would reach the merge commit; the driver stops instead."""
        self.make_fork()
        self.driver()
        self.linux_edit("keep.h", edit_line(read(self.nano("keep.h")), 3, "line3 linux"))   # so git runs the driver
        write(self.nano("stray_test.cpp"), b"int stray;\n")
        write(self.nano("platform/linux/fine.cpp"), b"int fine;\n")   # fork-only: never in the patch
        self.commit_nano144(self.u.p3)
        before = self.patch()
        proc = self.merge("nano144")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"nano/ has uncommitted changes or untracked files (stray_test.cpp)", proc.stderr + proc.stdout)
        self.assertNotIn(b"stray_test.cpp", self.patch())
        self.assertNotIn(b"int stray", self.patch())
        self.g("merge", "--abort")
        self.assertEqual(self.patch(), before)
        os.remove(self.nano("stray_test.cpp"))
        proc = self.merge("nano144")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(b"other p3", self.patch())

    def step_branch(self, name, edits, outside=None, start="master"):
        """A step branch from START: EDITS ({nano path: data}) and the patch regenerated."""
        self.g("checkout", "-q", "-b", name, start)
        for rel, data in edits.items():
            write(self.nano(rel), data)
        if outside:
            path = os.path.join(self.f, "patches", "thimsy", "0001-thimsy.patch")
            write(path, self.patch().replace(*outside))
        self.assertEqual(self.tool("regen")[0], 0)
        self.g("add", "-A")
        self.g("commit", "-q", "-m", name)

    def merge(self, *args):
        return subprocess.run(["git", "merge", "--no-edit"] + list(args), cwd=self.f,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_merge_driver_fork_branches(self):
        """Two step branches that both changed nano/ (and so the patch and block.sha256): the
        drivers regenerate both from the merged nano/ and the merge completes."""
        self.make_fork()
        self.g("branch", "-M", "master")
        self.driver()
        a = read(self.nano("a.cpp"))
        self.step_branch("s1", {"a.cpp": edit_line(a, 4, "line4 s1"), "platform/linux/p.cpp": b"p\n"})
        self.step_branch("s2", {"keep.h": edit_line(read(self.nano("keep.h")), 3, "line3 s2"),
                                "a.cpp": edit_line(a, 9, "line9 s2"), "compat/n.h": b"n\n"},
                         outside=(b"other p0", b"other s2"))
        self.g("checkout", "-q", "s1")
        proc = self.merge("s2")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.g("status", "--porcelain").strip(), b"")
        merged = self.patch()
        for want in (b"+line4 s1", b"+line9 s2", b"+line3 s2", b"other s2", b"+n"):
            self.assertIn(want, merged)
        self.assertNotIn(b"platform/linux", merged)
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        self.assertEqual(self.g("show", "HEAD:tools/nano-tree/block.sha256").decode().strip(),
                         sha256(Patch.parse(merged).split_block(NANO + "/")[1]))

    def test_merge_driver_fork_branches_conflict(self):
        """nano/ conflicts: both generated files are left as this side's and the merge stops."""
        self.make_fork()
        self.g("branch", "-M", "master")
        self.driver()
        a = read(self.nano("a.cpp"))
        self.step_branch("s1", {"a.cpp": edit_line(a, 5, "line5 s1")})
        self.step_branch("s2", {"a.cpp": edit_line(a, 5, "line5 s2")})
        self.g("checkout", "-q", "s1")
        ours = self.patch()
        proc = self.merge("s2")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"nanopatch: nano/a.cpp does not merge cleanly", proc.stderr + proc.stdout)
        self.assertEqual(self.patch(), ours)
        write(self.nano("a.cpp"), edit_line(a, 5, "line5 both"))
        self.assertEqual(self.tool("regen", "--force")[0], 0)
        self.g("add", "-A")
        self.g("commit", "-q", "--no-edit")
        self.assertIn(b"+line5 both", self.patch())
        self.assertEqual(self.tool("regen", "--check")[0], 0)

    def test_import_base(self):
        self.make_fork()
        rc, out, err = self.tool("import-base", self.u.base2)
        self.assertEqual(rc, 0, err)
        cfg = Upstream.load(self.f)
        self.assertEqual(cfg.base, self.u.base2)
        self.assertIn(b"line1 keep upstream2", read(self.nano("keep.h")))
        a = read(self.nano("a.cpp"))
        self.assertIn(b"line9 upstream2", a)
        self.assertIn(b"line2 thimsy", a)
        self.assertFalse(os.path.exists(self.nano("ps3xmb/icons/y.png")))
        self.assertEqual(self.tool("regen", "--check")[0], 0)
        block = Patch.parse(self.patch()).split_block(NANO + "/")[1]
        self.assertNotIn(b"keep.h", block)        # nano/ equals the new base there
        self.assertNotIn(b"upstream2", block)


if __name__ == "__main__":
    unittest.main()
