"""A monitor plugged in is display 0, with the panel switched off (docked) or on beside it (dual screen
mode), or shows the panel's display 0 (mirror mode); unplugged, the panel is display 0 again
(GammaosScreenArrangement, applied by LogicalDisplayMapper), and the old external display modes are
cleared once. A monitor runs at Auto, its stored mode or a trial (monitorMode; the store and what
gammaos-monitor-mode try refuses). Runs GammaosScreenArrangementCheck on the host, compiled from the
files patches/thimsy/0001-thimsy.patch creates: what the release ships, whatever state the AOSP tree
is in (a tree synced to an older patch would otherwise test the old code and pass).
"""
import hashlib
import os
import shutil
import tempfile

from tests import util

REL_DISPLAY = "frameworks/base/services/core/java/com/android/server/display"
REL_CHECK = "frameworks/base/cmds/gammaos-nano/test/java/GammaosScreenArrangementCheck.java"
PATCH = os.path.join(util.ROOT, "patches", "thimsy", "0001-thimsy.patch")
SOURCES = (REL_DISPLAY + "/GammaosScreenArrangement.java", REL_CHECK)


def new_file(patch, rel):
    """The text of the file PATCH creates at REL, or None when it doesn't create one there."""
    lines = None
    for line in patch.split("\n"):
        if line.startswith("diff --git "):
            if lines is not None:
                break
            if line == f"diff --git a/{rel} b/{rel}":
                lines = []
        elif lines is not None:
            lines.append(line)
    if not lines or lines[0] != "new file mode 100644":
        return None
    at = next(i for i, line in enumerate(lines) if line.startswith("@@ "))
    text = "\n".join(line[1:] for line in lines[at + 1:] if line.startswith("+"))
    return text if "\\ No newline at end of file" in lines else text + "\n"


def post_blob(patch, rel):
    """The (abbreviated) git blob id PATCH's section for REL leaves, from its index line; None when
    the patch has no section for REL, or the section has no index line."""
    inside = False
    for line in patch.split("\n"):
        if line.startswith("diff --git "):
            if inside:
                return None
            inside = line == f"diff --git a/{rel} b/{rel}"
        elif inside and line.startswith("index "):
            return line.split()[1].split("..")[1]
    return None


def blob_id(data):
    """git's blob id for the bytes DATA."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def shipped_text(patch, rel):
    """The text of REL (relative to the AOSP tree) as the release ships it: the file PATCH creates;
    else the AOSP tree's copy, but only when it is exactly what PATCH leaves there (its git blob
    matches the section's index line) or PATCH doesn't touch REL; else None. A tree synced to an
    older patch, or edited since, is never used in place of the patch."""
    text = new_file(patch, rel)
    if text is not None:
        return text
    path = os.path.join(util.AOSP, rel)
    if not os.path.isfile(path):
        return None
    data = util.slurp(path)
    want = post_blob(patch, rel)
    if want is not None and not blob_id(data).startswith(want):
        return None
    return data.decode("utf-8")


class ScreenArrangement(util.TestCase):
    def test_check_passes(self):
        if not (shutil.which("javac") and shutil.which("java")):
            self.skipTest("javac isn't on PATH")
        work = tempfile.mkdtemp(prefix="thimsy-screens-")
        try:
            patch = util.slurp(PATCH, text=True)
            sources = []
            for rel in SOURCES:
                text = new_file(patch, rel)
                self.assertIsNotNone(text, f"the patch doesn't create {rel}")
                sources.append(os.path.join(work, os.path.basename(rel)))
                with open(sources[-1], "w", encoding="utf-8") as f:
                    f.write(text)
            classes = os.path.join(work, "classes")
            self.assertOk(util.run(["javac", "-nowarn", "-encoding", "UTF-8", "-d", classes]
                                   + sources, timeout=300))
            proc = self.assertOk(util.run(["java", "-cp", classes,
                                           "com.android.server.display.GammaosScreenArrangementCheck"]))
            self.assertIn("all passed", proc.stdout)
        finally:
            util.rmtree(work)

    def test_new_file_from_patch(self):
        patch = ("diff --git a/x/A.java b/x/A.java\nnew file mode 100644\nindex 0000000..1111111\n"
                 "--- /dev/null\n+++ b/x/A.java\n@@ -0,0 +1,2 @@\n+class A {\n+}\n"
                 "diff --git a/x/B.java b/x/B.java\nindex 2222222..3333333 100644\n"
                 "--- a/x/B.java\n+++ b/x/B.java\n@@ -1 +1 @@\n-a\n+b\n")
        self.assertEqual(new_file(patch, "x/A.java"), "class A {\n}\n")
        self.assertIsNone(new_file(patch, "x/B.java"))
        self.assertIsNone(new_file(patch, "x/C.java"))
        self.assertEqual(post_blob(patch, "x/A.java"), "1111111")
        self.assertEqual(post_blob(patch, "x/B.java"), "3333333")
        self.assertIsNone(post_blob(patch, "x/C.java"))

    def test_blob_id(self):
        # git hash-object of an empty file and of a file holding "a" and a newline.
        self.assertEqual(blob_id(b""), "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")
        self.assertEqual(blob_id(b"a\n"), "78981922613b2afb6025042ff6bd878ac1994e85")

    def test_shipped_text_ignores_a_stale_tree(self):
        """A file the patch creates comes from the patch even when the tree has another copy; a file
        it changes comes from the tree only when the tree's copy is the patch's result."""
        rel_new, rel_mod = "x/New.java", "x/Mod.java"
        tree = tempfile.mkdtemp(prefix="thimsy-tree-")
        old_aosp = util.AOSP
        try:
            os.makedirs(os.path.join(tree, "x"))
            for rel, text in ((rel_new, "stale\n"), (rel_mod, "b\n")):
                with open(os.path.join(tree, rel), "w", newline="\n") as f:
                    f.write(text)
            good = blob_id(b"b\n")[:11]
            patch = ("diff --git a/x/New.java b/x/New.java\nnew file mode 100644\nindex 0000000..1111111\n"
                     "--- /dev/null\n+++ b/x/New.java\n@@ -0,0 +1 @@\n+fresh\n"
                     f"diff --git a/x/Mod.java b/x/Mod.java\nindex 2222222..{good} 100644\n"
                     "--- a/x/Mod.java\n+++ b/x/Mod.java\n@@ -1 +1 @@\n-a\n+b\n")
            util.AOSP = tree
            self.assertEqual(shipped_text(patch, rel_new), "fresh\n")
            self.assertEqual(shipped_text(patch, rel_mod), "b\n")
            with open(os.path.join(tree, rel_mod), "w", newline="\n") as f:
                f.write("a\n")
            self.assertIsNone(shipped_text(patch, rel_mod))
            self.assertIsNone(shipped_text(patch, "x/Missing.java"))
        finally:
            util.AOSP = old_aosp
            util.rmtree(tree)
