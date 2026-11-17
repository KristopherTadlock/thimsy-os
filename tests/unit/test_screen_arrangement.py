"""A monitor plugged in is display 0, with the panel switched off (docked) or on beside it (dual screen
mode), or shows the panel's display 0 (mirror mode); unplugged, the panel is display 0 again
(GammaosScreenArrangement, applied by LogicalDisplayMapper), and the old external display modes are
cleared once. Runs GammaosScreenArrangementCheck on the host.
"""
import os
import shutil
import tempfile

from tests import util

DISPLAY = os.path.join(util.AOSP, "frameworks/base/services/core/java/com/android/server/display")
CHECK = os.path.join(util.AOSP, "frameworks/base/cmds/gammaos-nano/test/java/GammaosScreenArrangementCheck.java")


class ScreenArrangement(util.TestCase):
    def test_check_passes(self):
        if not (shutil.which("javac") and shutil.which("java")):
            self.skipTest("javac isn't on PATH")
        if not os.path.isfile(CHECK):
            self.skipTest(f"no AOSP tree at {util.AOSP}")
        work = tempfile.mkdtemp(prefix="thimsy-screens-")
        try:
            self.assertOk(util.run(["javac", "-nowarn", "-encoding", "UTF-8", "-d", work,
                                    os.path.join(DISPLAY, "GammaosScreenArrangement.java"), CHECK],
                                   timeout=300))
            proc = self.assertOk(util.run(["java", "-cp", work,
                                           "com.android.server.display.GammaosScreenArrangementCheck"]))
            self.assertIn("all passed", proc.stdout)
        finally:
            util.rmtree(work)
