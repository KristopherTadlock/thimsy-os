"""A monitor plugged in is display 0, with the panel switched off (docked) or on beside it (dual screen
mode), or shows the panel's display 0 (mirror mode); unplugged, the panel is display 0 again
(GammaosScreenArrangement, applied by LogicalDisplayMapper), and the old external display modes are
cleared once. A monitor runs at Auto, its stored mode or a trial (monitorMode; the store and what
gammaos-monitor-mode try refuses). Runs GammaosScreenArrangementCheck on the host: from the AOSP
tree, or without one from the files patches/thimsy/0001-thimsy.patch creates (the check itself from
nano/ first, where the fork's linux branch keeps Nano's source as files).
"""
import os
import shutil
import tempfile

from tests import util

REL_DISPLAY = "frameworks/base/services/core/java/com/android/server/display"
REL_CHECK = "frameworks/base/cmds/gammaos-nano/test/java/GammaosScreenArrangementCheck.java"
DISPLAY = os.path.join(util.AOSP, REL_DISPLAY)
CHECK = os.path.join(util.AOSP, REL_CHECK)
# Nano's source as files (the fork's linux branch); absent on nano-144.
NANO_CHECK = os.path.join(util.ROOT, "nano", "test", "java", "GammaosScreenArrangementCheck.java")
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


class ScreenArrangement(util.TestCase):
    def test_check_passes(self):
        if not (shutil.which("javac") and shutil.which("java")):
            self.skipTest("javac isn't on PATH")
        work = tempfile.mkdtemp(prefix="thimsy-screens-")
        try:
            if os.path.isfile(CHECK):
                sources = [os.path.join(DISPLAY, "GammaosScreenArrangement.java"),
                           NANO_CHECK if os.path.isfile(NANO_CHECK) else CHECK]
            else:
                patch = util.slurp(PATCH, text=True)
                sources = []
                for rel in SOURCES:
                    if rel == REL_CHECK and os.path.isfile(NANO_CHECK):
                        sources.append(NANO_CHECK)
                        continue
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
