"""scripts/aosp/sync_patch.py: the tree becomes exactly the base with the patch,
rewriting only what differs, on a small git repository made here."""
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

from tests import util

SCRIPT = os.path.join(util.SCRIPTS, "aosp", "sync_patch.py")


def git(repo, *args, stdin=None):
    return subprocess.run(["git", "-C", repo, *args], input=stdin, capture_output=True,
                          text=True, check=True).stdout


@unittest.skipUnless(shutil.which("git"), "needs git")
class SyncPatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.repo = os.path.join(self.tmp, "tree")
        os.makedirs(os.path.join(self.repo, "dir"))
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "t@example.invalid")
        git(self.repo, "config", "user.name", "t")
        git(self.repo, "config", "core.autocrlf", "false")
        self.write("a.txt", "a\n")
        self.write("b.txt", "b\n")
        self.write("dir/c.txt", "c\n")
        self.write("run.sh", "#!/bin/sh\n")
        self.write("big.bin", "pointer\n")
        self.write(".gitattributes", "big.bin filter=lfs\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "base")
        self.base = git(self.repo, "rev-parse", "HEAD").strip()
        # The build's output and system.sh's markers are never touched.
        self.write("out/obj.o", "built\n")
        self.write(".thimsy-patch.sha256", "x\n")
        self.write(".thimsy-plan.sha256", "y\n")

    def path(self, name):
        return os.path.join(self.repo, name)

    def write(self, name, text):
        os.makedirs(os.path.dirname(self.path(name)), exist_ok=True)
        with open(self.path(name), "w", newline="\n") as f:
            f.write(text)

    def read(self, name):
        with open(self.path(name)) as f:
            return f.read()

    def make_patch(self, name, edit):
        """A patch against the base made by EDIT(), on a scratch clone."""
        clone = os.path.join(self.tmp, "clone-" + name)
        subprocess.run(["git", "clone", "-q", self.repo, clone], check=True, capture_output=True)
        git(clone, "checkout", "-q", self.base)
        edit(clone)
        git(clone, "add", "-A")
        out = os.path.join(self.tmp, name + ".patch")
        with open(out, "w", newline="\n") as f:
            f.write(git(clone, "diff", "--cached", "--binary", self.base))
        return out

    def sync(self, patch):
        return util.run(["python3", "-I", SCRIPT, self.repo, self.base, patch])

    def age(self, *names):
        """Gives NAMES an old time, so a rewrite shows up."""
        for n in names:
            os.utime(self.path(n), (1_000_000_000, 1_000_000_000))

    def test_only_differing_files_are_rewritten(self):
        def p1(c):
            with open(os.path.join(c, "a.txt"), "w") as f:
                f.write("a1\n")
            with open(os.path.join(c, "new.txt"), "w") as f:
                f.write("new\n")
            os.remove(os.path.join(c, "b.txt"))
        patch1 = self.make_patch("p1", p1)
        self.age("dir/c.txt", "run.sh")
        r = self.sync(patch1)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 2 file(s), removed 1", r.stdout)
        self.assertEqual(self.read("a.txt"), "a1\n")
        self.assertEqual(self.read("new.txt"), "new\n")
        self.assertFalse(os.path.exists(self.path("b.txt")))
        # Untouched files keep their times; the index stays at the base.
        self.assertEqual(os.stat(self.path("dir/c.txt")).st_mtime, 1_000_000_000)
        self.assertEqual(git(self.repo, "diff", "--cached", "--name-only"), "")

        # A second patch: only what differs between the two trees changes.
        def p2(c):
            with open(os.path.join(c, "dir/c.txt"), "w") as f:
                f.write("c2\n")
            with open(os.path.join(c, "a.txt"), "w") as f:
                f.write("a1\n")  # the same as patch 1
            st = os.stat(os.path.join(c, "run.sh"))
            os.chmod(os.path.join(c, "run.sh"), st.st_mode | stat.S_IXUSR)
        patch2 = self.make_patch("p2", p2)
        # An edit by hand, outside either patch, goes too.
        self.write("dir/stray.txt", "stray\n")
        self.age("a.txt")
        r = self.sync(patch2)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.read("dir/c.txt"), "c2\n")
        self.assertEqual(self.read("b.txt"), "b\n")
        self.assertFalse(os.path.exists(self.path("new.txt")))
        self.assertFalse(os.path.exists(self.path("dir/stray.txt")))
        self.assertTrue(os.stat(self.path("run.sh")).st_mode & stat.S_IXUSR)
        self.assertEqual(os.stat(self.path("a.txt")).st_mtime, 1_000_000_000)
        self.assertEqual(self.read("out/obj.o"), "built\n")
        self.assertEqual(self.read(".thimsy-patch.sha256"), "x\n")
        self.assertEqual(self.read(".thimsy-plan.sha256"), "y\n")

        # Already in sync: nothing to do.
        r = self.sync(patch2)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 0 file(s), removed 0", r.stdout)

    def test_staged_and_hidden_edits_are_undone(self):
        def edit(c):
            with open(os.path.join(c, "a.txt"), "w") as f:
                f.write("a1\n")
        patch = self.make_patch("p", edit)
        r = self.sync(patch)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 1 file(s), removed 0", r.stdout)
        # Edits outside the patch that git add staged: a new file and a change
        # to a tracked file the patch doesn't touch.
        self.write("staged-new.txt", "hand\n")
        self.write("dir/c.txt", "handedit\n")
        git(self.repo, "add", "staged-new.txt", "dir/c.txt")
        # An edit git is told not to look at.
        git(self.repo, "update-index", "--assume-unchanged", "b.txt")
        self.write("b.txt", "hidden\n")
        r = self.sync(patch)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 2 file(s), removed 1", r.stdout)
        self.assertIn("reset 2 staged path(s) in the index to the base", r.stdout)
        self.assertFalse(os.path.exists(self.path("staged-new.txt")))
        self.assertEqual(self.read("dir/c.txt"), "c\n")
        self.assertEqual(self.read("b.txt"), "b\n")
        self.assertEqual(self.read("a.txt"), "a1\n")
        # The index is back at the base.
        self.assertEqual(git(self.repo, "diff", "--cached", "--name-only", self.base), "")
        r = self.sync(patch)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 0 file(s), removed 0", r.stdout)

    def test_lfs_files_are_left_alone(self):
        # A working copy of an LFS file is the real data, not the stored pointer.
        self.write("big.bin", "the real large file\n")
        def edit(c):
            with open(os.path.join(c, "a.txt"), "w") as f:
                f.write("a1\n")
        r = self.sync(self.make_patch("other", edit))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wrote 1 file(s), removed 0", r.stdout)
        self.assertEqual(self.read("big.bin"), "the real large file\n")

    def test_a_patch_that_does_not_apply_changes_nothing(self):
        bad = os.path.join(self.tmp, "bad.patch")
        with open(bad, "w", newline="\n") as f:
            f.write("diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-nope\n+x\n")
        self.write("a.txt", "edited\n")
        r = self.sync(bad)
        self.assertEqual(r.returncode, 1)
        self.assertIn("git apply", r.stderr)
        self.assertEqual(self.read("a.txt"), "edited\n")


if __name__ == "__main__":
    unittest.main()
