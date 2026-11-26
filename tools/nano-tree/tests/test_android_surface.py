"""The Android surface (docs/linux/IMPLEMENTATION.md H.1): the fork changes no byte of what Soong
compiles or installs from nano/ unless a commit says so.

The surface is read from nano/Android.bp at HEAD (nanotree/surface.py: every module's files, the
headers they include, what drastic-nano and the rest of the patch include from Nano's folder). Every
path of the patch's Nano block at HEAD or at `git merge-base HEAD nano-144` has its sections
compared, both read from git objects, so no base cache and no baseline file is needed; a difference
counts when the path is on the surface or is not a file of nano/ at HEAD (deleted, dropped, or never
committed: nanotree/surface.py surface_diffs). Every difference must come from a commit since the merge base whose message has a line

  android-surface: <step id> <what changed> (review: <how it was reviewed>)

and no commit without such a line may have changed a differing path while it was on the surface of
that commit's tree; a path that reached the patch off the surface and joined it later is covered by
the annotated commit that put it there (nanotree/surface.py surface_joins, G7-main). Changes nano-144
made are on both sides of the comparison. Skips without nano/ or without a local nano-144 branch. Fork-only;
run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests
"""
from __future__ import annotations

import os
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
sys.path.insert(0, TOOL_DIR)

from nanotree.androidbp import Blueprint  # noqa: E402
from nanotree.config import Upstream  # noqa: E402
from nanotree.surface import (Commit, Repo, blueprint_surface, compute_surface, coverage,  # noqa: E402
                              includes_of, module_sources, outside_sections, resolve_include,
                              surface_diffs, surface_joins)

NANO = os.path.join(ROOT, "nano")
HAVE = os.path.isdir(NANO) and os.path.isfile(os.path.join(TOOL_DIR, "upstream.json"))
PREFIX = "frameworks/base/cmds/gammaos-nano/"

FIXTURE_BP = """
cc_binary {
    name: "app",
    srcs: ["main.cpp", "sub/*.cpp"],
    local_include_dirs: ["inc"],
    init_rc: ["app.rc"],
    arch: { arm64: { srcs: ["neon.cpp"] } },
}
filegroup { name: "shared", srcs: ["shared.cpp"] }
cc_test_host { name: "t", srcs: [":shared", "test/*_test.cpp"], include_dirs: ["%s"] }
prebuilt_etc { name: "assets", srcs: ["assets/*.png"], sub_dir: "x" }
sh_binary { name: "s.sh", src: "s.sh" }
cc_library_static { name: "lib", srcs: ["lib/l.c"], export_include_dirs: ["lib/include"] }
""" % PREFIX.rstrip("/")


class SurfaceUnit(unittest.TestCase):
    """compute_surface and coverage on synthetic input: no git."""

    FILES = {
        "Android.bp": FIXTURE_BP.encode(),
        "main.cpp": b'#include "a.h"\n#include <vector>\n  #  include <b.h>\n',
        "a.h": b'#pragma once\n#include "deep/c.h"\n',
        "deep/c.h": b'#include "../d.inc"\n',
        "d.inc": b"int x;\n",
        "inc/b.h": b"// b\n",
        "sub/one.cpp": b"",
        "neon.cpp": b"",
        "shared.cpp": b'#include "shared.h"\n',
        "shared.h": b"",
        "test/x_test.cpp": b"",
        "assets/p.png": b"\x89PNG",
        "app.rc": b"service app /system/bin/app\n",
        "s.sh": b"#!/bin/sh\n",
        "unused.cpp": b'#include "unused.h"\n',
        "unused.h": b"",
        "only_outside.h": b"",
        "named_outside.cfg": b"",
        "platform/linux/x.cpp": b"",
        "lib/l.c": b"",
        "lib/include/api.h": b"",
        "lib/include/NOTES.txt": b"",
    }

    def surface(self, outside=()):
        return compute_surface(sorted(self.FILES), lambda p: self.FILES[p], PREFIX, outside)

    def test_modules_includes_and_globs(self):
        s = self.surface()
        for p in ("Android.bp", "main.cpp", "a.h", "deep/c.h", "d.inc", "inc/b.h", "sub/one.cpp",
                  "neon.cpp", "shared.cpp", "shared.h", "test/x_test.cpp", "assets/p.png", "app.rc",
                  "s.sh", "lib/l.c", "lib/include/api.h"):
            self.assertIn(p, s)
        for p in ("unused.cpp", "unused.h", "only_outside.h", "named_outside.cfg", "platform/linux/x.cpp",
                  "lib/include/NOTES.txt"):
            self.assertNotIn(p, s)

    def test_outside_sections_count(self):
        outside = [b"diff --git a/frameworks/base/cmds/other/m.cpp b/frameworks/base/cmds/other/m.cpp\n"
                   b"+++ b/frameworks/base/cmds/other/m.cpp\n"
                   b'+#include "only_outside.h"\n'
                   b'-#include "unused.h"\n'
                   b" PRODUCT_COPY_FILES += " + PREFIX.encode() + b"named_outside.cfg:system/etc/x\n"]
        s = self.surface(outside)
        self.assertIn("only_outside.h", s)
        self.assertIn("named_outside.cfg", s)
        self.assertNotIn("unused.h", s, "a removed line is not part of the post-image")

    def test_include_dirs_mapped(self):
        bp = Blueprint(FIXTURE_BP)
        _named, inc = blueprint_surface(bp, sorted(self.FILES), PREFIX)
        self.assertEqual(inc, ["", "inc", "lib/include"])

    def test_includes_parsed(self):
        self.assertEqual(includes_of(b'#include "x.h"\n # include <y/z.h>\n// #include "c.h"\n'),
                         [('"', "x.h"), ("<", "y/z.h")])
        self.assertIsNone(resolve_include("a.cpp", '"', "../../etc/passwd", [""], {"etc/passwd"}))

    def test_coverage(self):
        good = Commit("1" * 40, "Move X\n\nandroid-surface: G9a move of X (review: color-moved clean)\n",
                      {"X.cpp", "Android.bp"})
        bare = Commit("2" * 40, "Tweak X\n", {"X.cpp"})
        no_review = Commit("3" * 40, "Y\n\nandroid-surface: G7-main add Y\n", {"Y.cpp"})
        missing, unannotated = coverage({"X.cpp", "Android.bp", "Y.cpp"}, [good, bare, no_review])
        self.assertEqual(missing, ["Y.cpp"])
        self.assertEqual([(c.sha, paths) for c, paths in unannotated],
                         [("2" * 40, ["X.cpp"]), ("3" * 40, ["Y.cpp"])])
        self.assertEqual(coverage(set(), [bare]), ([], []))

    def test_surface_joins(self):
        """G7-main: P.cpp reached the patch off the surface (G5) and joined it in an annotated
        commit; Q.cpp joined it in an unannotated one; R.cpp was changed bare while on it."""
        g5 = Commit("1" * 40, "Add P, Q and R\n", {"P.cpp", "Q.cpp", "R.cpp"})
        joins = Commit("2" * 40, "Compile P\n\nandroid-surface: G7-main add P (review: color-moved clean)\n",
                       {"Android.bp"})
        bare = Commit("3" * 40, "Compile Q\n", {"Android.bp"})
        surfaces = {"1" * 40: {"Android.bp", "R.cpp"}, "2" * 40: {"Android.bp", "R.cpp", "P.cpp"},
                    "3" * 40: {"Android.bp", "R.cpp", "P.cpp", "Q.cpp"}}
        commits = [g5, joins, bare]
        missing, unannotated = coverage({"P.cpp", "Q.cpp", "R.cpp"}, commits)
        self.assertEqual(missing, ["P.cpp", "Q.cpp", "R.cpp"])
        left, still = surface_joins(missing, unannotated, commits, surfaces.get)
        self.assertEqual(left, ["Q.cpp", "R.cpp"])
        self.assertEqual([(c.sha, ps) for c, ps in still], [("1" * 40, ["R.cpp"])])
        # Without a surface for a commit (no nano/ there), nothing is relieved.
        left, still = surface_joins(missing, unannotated, commits, lambda sha: None)
        self.assertEqual(left, ["P.cpp", "Q.cpp", "R.cpp"])
        self.assertEqual([(c.sha, ps) for c, ps in still], [("1" * 40, ["P.cpp", "Q.cpp", "R.cpp"])])

    def test_surface_diffs_see_paths_gone_from_nano(self):
        """android-safety-1: a deleted file, a dropped asset and a section with no committed file
        are compared, though HEAD's surface (from nano/ at HEAD) cannot list them."""
        base = {"a.cpp": b"A", "assets/lynx.svg": b"new file lynx", "patched.png": b"patch",
                "off.txt": b"x", "same.cpp": b"s"}
        head = {"a.cpp": b"A2", "gone.h": b"deleted file", "stray.cpp": b"new file",
                "off.txt": b"y", "same.cpp": b"s"}
        files = {"a.cpp", "off.txt", "same.cpp"}       # nano/ at HEAD
        surface = {"a.cpp", "same.cpp"}
        self.assertEqual(surface_diffs(head, base, surface, files),
                         {"a.cpp", "assets/lynx.svg", "patched.png", "gone.h", "stray.cpp"})
        self.assertEqual(surface_diffs(base, base, surface, files), set())


@unittest.skipUnless(HAVE, "no nano/ in this checkout")
class AndroidSurface(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cfg = Upstream.load(ROOT)
        cls.prefix = cls.cfg.prefix
        cls.repo = Repo(ROOT)
        cls.head = cls.repo.rev("HEAD")
        cls.tree = cls.repo.ls_tree("HEAD", "nano")
        head_blob = cls.repo.patch_blob("HEAD")
        cls.head_patch = cls.repo.cat(head_blob) if head_blob else None
        cls.surface = compute_surface(sorted(cls.tree), cls.read, cls.prefix,
                                      outside_sections(cls.head_patch, cls.prefix) if cls.head_patch else ())

    @classmethod
    def tearDownClass(cls):
        cls.repo.close()

    @classmethod
    def read(cls, rel):
        data = cls.repo.cat(cls.tree[rel])
        return data if data is not None else b""

    def test_surface_covers_the_modules(self):
        bp = Blueprint(self.read("Android.bp").decode("utf-8"))
        srcs = module_sources(bp, sorted(self.tree))
        for name in ("gammaos-nano", "gammaos-nano-scrape", "drastic_nano_shared_srcs", "gammaos_nano_unit_tests"):
            self.assertIn(name, srcs)
            self.assertTrue(srcs[name] <= self.surface, "%s: %s" % (name, sorted(srcs[name] - self.surface)[:5]))
        for p in ("Android.bp", "gammaos-nano.rc", "NanoMenu.h", "NanoMenuDrm.h", "nano-ce-unlock.sh",
                  "bootscreen/thimsy.svg", "liba52/include/a52.h"):
            if p in self.tree:
                self.assertIn(p, self.surface)
        self.assertIn("NanoMenu.h", self.surface)

    def test_no_fork_only_file_in_the_surface(self):
        """Fork-only files never reach the patch, so Android must not compile or include one."""
        bad = sorted(p for p in self.surface if self.cfg.is_fork_only(p))
        self.assertEqual(bad, [], "Android.bp or a surface file reaches fork-only files")

    def test_drm_back_end_links_with_the_drm_file(self):
        """Every module that compiles NanoMenuDrm.cpp also compiles its buffer back end (G9a);
        drastic-nano gets both through the drastic_nano_shared_srcs filegroup."""
        bp = Blueprint(self.read("Android.bp").decode("utf-8"))
        if "NanoMenuDrmBuffers.cpp" not in self.tree:
            self.skipTest("no NanoMenuDrmBuffers.cpp (before G9a)")
        srcs = module_sources(bp, sorted(self.tree))
        users = sorted(n for n, s in srcs.items() if "NanoMenuDrm.cpp" in s)
        self.assertIn("gammaos-nano", users)
        self.assertIn("drastic_nano_shared_srcs", users)
        for n in users:
            self.assertIn("NanoMenuDrmBuffers.cpp", srcs[n], n)
        drastic = [d for d in outside_sections(self.head_patch, self.prefix)
                   if d.startswith(b"diff --git a/frameworks/base/cmds/drastic-nano/Android.bp ")]
        self.assertEqual(len(drastic), 1)
        post = [ln for ln in drastic[0].split(b"\n") if ln[:1] in (b"+", b" ") and not ln.startswith(b"+++")]
        self.assertTrue(any(b'":drastic_nano_shared_srcs"' in ln for ln in post),
                        "drastic-nano no longer compiles the shared filegroup")

    def test_surface_changes_are_marked(self):
        nano144 = self.repo.rev("nano-144")
        if nano144 is None:
            self.skipTest("no local nano-144 branch")
        mb = self.repo.merge_base("HEAD", nano144)
        if mb is None:
            self.skipTest("HEAD and nano-144 have no merge base")
        self.assertIsNotNone(self.head_patch, "no patch at HEAD")
        head_block = self.repo.block_at("HEAD", self.prefix)
        base_block = self.repo.block_at(mb, self.prefix)
        diffs = surface_diffs(head_block, base_block, self.surface, set(self.tree))
        if not diffs:
            return
        commits = self.repo.surface_commits(mb, "HEAD", self.prefix, diffs)
        missing, unannotated = coverage(diffs, commits)
        if missing or unannotated:
            missing, unannotated = surface_joins(missing, unannotated, commits,
                                                 lambda sha: self.repo.surface_at(sha, self.prefix))
        msg = []
        for p in missing:
            msg.append("%s differs from nano-144 (merge base %s) and no commit with an android-surface "
                       "line changed it" % (p, mb[:10]))
        for c, paths in unannotated:
            msg.append("commit %s (%s) changed %s without an 'android-surface: <step> ... (review: ...)' "
                       "line" % (c.sha[:10], c.subject, ", ".join(paths[:5])))
        self.assertFalse(msg, "Android-surface changes not marked (IMPLEMENTATION.md H.1):\n  " + "\n  ".join(msg))


if __name__ == "__main__":
    unittest.main()
