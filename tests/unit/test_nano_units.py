"""Nano's USB-C role helper (NanoTypec) and its monitor mode helper (NanoMonitorMode), with their
AOSP unit tests (test/unit/*_test.cpp), built with g++ and run on the host against a small stand-in
for GoogleTest: libthimsy/testing's on the fork's linux branch (docs/linux/IMPLEMENTATION.md D.1),
else tests/fixtures/nano_gtest. Skipped where g++ is missing (Windows). The sources come from the
AOSP tree, or without one from
the files patches/thimsy/0001-thimsy.patch creates; on the fork's linux branch, from nano/ first
(Nano's source as files, docs/linux/IMPLEMENTATION.md A). And nothing in the patch or the mods touches the
USB controller's mode file in debugfs.
"""
import os
import re
import shutil
import tempfile

from tests import util
from tests.unit import test_screen_arrangement as arrangement

NANO = "frameworks/base/cmds/gammaos-nano"
# Nano's source as files (the fork's linux branch); absent on nano-144.
NANO_FILES = os.path.join(util.ROOT, "nano")
PATCH = arrangement.PATCH
# The GoogleTest stand-in: libthimsy's (the fork's linux branch, where Nano's host build uses it too),
# else the old one in tests/fixtures (nano-144, which has no libthimsy/).
GTEST_LIBTHIMSY = os.path.join(util.ROOT, "libthimsy", "testing")
GTEST = (GTEST_LIBTHIMSY if os.path.isfile(os.path.join(GTEST_LIBTHIMSY, "gtest", "gtest.h"))
         else os.path.join(util.TESTS, "fixtures", "nano_gtest"))
# Each program: the helper's sources and its test.
PROGRAMS = {
    "typec": ("NanoTypec.h", "NanoTypec.cpp", "test/unit/NanoTypec_test.cpp"),
    "monitor_mode": ("NanoMonitorMode.h", "NanoMonitorMode.cpp", "test/unit/NanoMonitorMode_test.cpp"),
}
# dwc3's debugfs mode file (/sys/kernel/debug/25100000.dwc3/mode): a read while the controller is
# runtime-suspended panics the kernel (usb_c_dock_plan.md R6), and a write races its state machine.
DWC3_DEBUGFS = re.compile(r"kernel/debug/[^\s\"']*dwc3")


def read_utf8(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def nano_sources(rels, patch_text=None):
    """The texts of Nano's files RELS: all from nano/ when it has them all, else all from the AOSP
    tree when it has them all, else as the patch creates them (None for a file it doesn't)."""
    paths = [os.path.join(NANO_FILES, rel) for rel in rels]
    if all(os.path.isfile(p) for p in paths):
        return [read_utf8(p) for p in paths]
    paths = [os.path.join(util.AOSP, NANO, rel) for rel in rels]
    if all(os.path.isfile(p) for p in paths):
        return [util.slurp(p, text=True) for p in paths]
    if patch_text is None:
        patch_text = util.slurp(PATCH, text=True)
    return [arrangement.new_file(patch_text, f"{NANO}/{rel}") for rel in rels]


class HostTests(util.TestCase):
    def build_and_run(self, name):
        gxx = shutil.which("g++")
        if not gxx:
            self.skipTest("g++ isn't on PATH")
        work = tempfile.mkdtemp(prefix="thimsy-nano-")
        try:
            files = []
            for rel, text in zip(PROGRAMS[name], nano_sources(PROGRAMS[name])):
                self.assertIsNotNone(text, f"neither nano/, the AOSP tree nor the patch has {rel}")
                dest = os.path.join(work, rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "w", encoding="utf-8") as f:
                    f.write(text)
                files.append(dest)
            exe = os.path.join(work, name)
            # The helper as Soong builds Nano (-Wall -Werror), with -Wextra on top; the test as the
            # unit test module builds it.
            self.assertOk(util.run([gxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-c", files[1],
                                    "-o", exe + ".o"], timeout=300))
            self.assertOk(util.run([gxx, "-std=c++17", "-Wall", "-Werror", "-I", GTEST, "-I", work,
                                    files[2], exe + ".o", "-o", exe], timeout=300))
            proc = self.assertOk(util.run([exe]))
            self.assertRegex(proc.stdout, r"\n\d+ tests, 0 failed\n$")
            self.assertNotIn("FAILED", proc.stdout)
            return proc
        finally:
            util.rmtree(work)

    def test_typec(self):
        proc = self.build_and_run("typec")
        for name in ("Typec.LeavesDocksAlone", "Typec.ADockBackDuringTheTryEndsIt",
                     "Typec.FindsThePdPortByWhatItCanDo", "Typec.PicksThePortThatNegotiates"):
            self.assertIn("[       OK ] " + name + "\n", proc.stdout)

    def test_monitor_mode(self):
        proc = self.build_and_run("monitor_mode")
        self.assertIn("[       OK ] MonitorMode.ReadsTheList\n", proc.stdout)

    def test_gtest_stand_in_reports_failures(self):
        gxx = shutil.which("g++")
        if not gxx:
            self.skipTest("g++ isn't on PATH")
        src = self.write("t.cpp", '#include <gtest/gtest.h>\n'
                         'TEST(A, Passes) { EXPECT_EQ(1, 1); ASSERT_TRUE(true) << "x"; }\n'
                         'TEST(A, Fails) { ASSERT_EQ(1, 2) << "one"; EXPECT_TRUE(false); }\n'
                         'TEST(A, Expects) { EXPECT_STREQ("a", "b"); EXPECT_STREQ(nullptr, nullptr); }\n')
        self.assertOk(util.run([gxx, "-std=c++17", "-Wall", "-Werror", "-I", GTEST, src, "-o",
                                self.path("t")], timeout=300))
        proc = util.run([self.path("t")])
        self.assertEqual(proc.returncode, 1)
        self.assertIn("[       OK ] A.Passes\n", proc.stdout)
        self.assertIn("[  FAILED  ] A.Fails\n", proc.stdout)
        self.assertIn("[  FAILED  ] A.Expects\n", proc.stdout)
        self.assertIn("3 tests, 2 failed", proc.stdout)
        self.assertIn("one", proc.stderr)
        self.assertNotIn("!(false)", proc.stderr)   # the ASSERT returned before it
        self.assertEqual(proc.stderr.count("streq"), 1)


class Dwc3Debugfs(util.TestCase):
    def test_nothing_touches_the_dwc3_mode_file(self):
        """No line the patch adds, and no file under mods/, names a dwc3 file in debugfs."""
        hits = []
        for n, line in enumerate(util.slurp(PATCH, text=True).split("\n"), 1):
            if line.startswith("+") and DWC3_DEBUGFS.search(line):
                hits.append(f"patch line {n}: {line}")
        for top, _dirs, names in os.walk(util.MODS):
            for name in names:
                path = os.path.join(top, name)
                with open(path, "rb") as f:
                    text = f.read().decode("utf-8", "replace")
                hits += [f"{path}: {m.group(0)}" for m in DWC3_DEBUGFS.finditer(text)]
        self.assertEqual(hits, [])

    def test_pattern(self):
        for text in ("echo host > /sys/kernel/debug/25100000.dwc3/mode", "cat /sys/kernel/debug/*dwc3*/mode",
                     '"/sys/kernel/debug/usb/25100000.dwc3/mode"'):
            self.assertRegex(text, DWC3_DEBUGFS)
        for text in ("/sys/kernel/debug/sprd_tcpm", "dwc3-sprd.ko", "/sys/class/typec/port0/data_role"):
            self.assertNotRegex(text, DWC3_DEBUGFS)


class MonitorModeDialogs(util.TestCase):
    def test_dialogs_clear_the_dual_screen_flag(self):
        """Output Resolution, Refresh Rate and Keep This Resolution? clear the Dual-Screen App prompt's
        flag: a prompt closed without an answer leaves it set, and X on the new dialog would then answer
        the prompt (adding its app to the run-on-primary list) instead of choosing a mode."""
        text, = nano_sources(["NanoMenuMonitorMode.cpp"])
        self.assertIsNotNone(text)
        for name in ("monitorModeChooser", "monitorModeAsk"):
            start = text.index(f"void NanoMenu::{name}(")
            body = text[start:text.index("\n}\n", start)]
            reset = body.index("mPs3DlgDualScreen = false; mPs3DlgDualScreenPkg.clear();")
            self.assertLess(reset, body.index("mPs3DlgActive = true;"), name)

    def test_x_and_o_answer_only_the_prompt(self):
        """Other dialogs close the Dual-Screen App prompt without clearing its flag too (a power hold,
        the Quick Menu, Game Settings). X and O drop a flag left set unless the open dialog is the
        prompt, before they would answer it: else X on Performance Mode's Normal would put the
        prompt's app on the run-on-primary list instead of choosing Normal."""
        path = os.path.join(NANO_FILES, "NanoMenuPS3Menu.cpp")
        if not os.path.isfile(path):
            path = os.path.join(util.AOSP, NANO, "NanoMenuPS3Menu.cpp")
        if os.path.isfile(path):
            text = read_utf8(path)
        else:
            text = section_new_side(util.slurp(PATCH, text=True), f"{NANO}/NanoMenuPS3Menu.cpp")
        title = '"Dual-Screen App"'
        guard = f"if (mPs3DlgDualScreen && mPs3DlgTitle != {title}) {{"
        lines = [line.strip() for line in text.split("\n")]
        consumers = [lines[i + 3] for i in range(len(lines) - 3) if lines[i].startswith(guard)
                     and lines[i + 1:i + 3] == ["mPs3DlgDualScreen = false; mPs3DlgDualScreenPkg.clear();", "}"]]
        self.assertEqual(consumers, ["if (mPs3DlgDualScreen) {", "if (mPs3DlgActive && mPs3DlgDualScreen) {"])
        # The prompt sets that title (openDualScreenPrompt), where the text shows it.
        if "void NanoMenu::openDualScreenPrompt(" in text or os.path.isfile(path):
            self.assertIn(f"mPs3DlgTitle = {title};", text)


def section_new_side(patch, rel):
    """The lines PATCH's section for REL leaves in the file (context and added), hunk after hunk."""
    out, inside, hunk = [], False, False
    for line in patch.split("\n"):
        if line.startswith("diff --git "):
            if inside:
                break
            inside = line == f"diff --git a/{rel} b/{rel}"
        elif inside and line.startswith("@@ "):
            hunk = True
        elif inside and hunk and line[:1] in (" ", "+"):
            out.append(line[1:])
    return "\n".join(out)
