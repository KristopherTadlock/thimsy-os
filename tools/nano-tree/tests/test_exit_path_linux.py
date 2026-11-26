"""threadLoop's exit path after a launch (nano/NanoMenu.cpp) waits up to 600 frames (~10 s, drawing
"Loading..." on the SurfaceFlinger path) for sys.gammaos.nano.app_launched=1, which Android's
framework sets as it starts the app. On Linux the game starts only after the menu has ended, so the
Linux launcher sets the key itself when it takes Nano's launch (nano/platform/linux/Launcher.cpp,
launchFromHandshake), and nano/platform/linux/tests/launcher_test.cpp proves the wait then ends on
its first look, with a copy of the wait (exitPathWaits). This test ties the three together: it fails
when NanoMenu.cpp's waits change (a merge from nano-144, say), so the copy and the launcher are
reviewed with them. Fork-only. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_exit_path_linux.py
"""
from __future__ import annotations

import os
import re
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
NANO_MENU = os.path.join(ROOT, "nano", "NanoMenu.cpp")
LAUNCHER = os.path.join(ROOT, "nano", "platform", "linux", "Launcher.cpp")
LAUNCHER_TEST = os.path.join(ROOT, "nano", "platform", "linux", "tests", "launcher_test.cpp")

# The exit path: from the DRM boot path's branch to the secondary surfaces' teardown.
EXIT_START = "        ALOGD(\"NanoMenu: DRM boot path exit, waiting for app launch\");"
EXIT_END = "    // GammaOS: tear down secondary wallpaper EGL surfaces / SurfaceControls"
LOOP_HEAD = "for (int wait = 0; wait < 600; wait++) {"
# The lines every wait is made of (whitespace collapsed), in this order.
WAIT_LINES = [
    'property_get("sys.gammaos.nano.app_launched", launched, "0");',
    'if (!strcmp(launched, "1"))',
    "break;",
    "usleep(16666);",
]


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def _squash(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def _loops(section: str):
    """The body of each `for (int wait ...)` loop in SECTION, braces matched."""
    out = []
    pos = 0
    while True:
        i = section.find(LOOP_HEAD, pos)
        if i < 0:
            return out
        j = i + len(LOOP_HEAD)
        depth = 1
        while depth:
            c = section[j]
            depth += (c == "{") - (c == "}")
            j += 1
        out.append(section[i + len(LOOP_HEAD):j - 1])
        pos = j


def _in_order(text: str, lines) -> bool:
    pos = 0
    for line in lines:
        pos = text.find(line, pos)
        if pos < 0:
            return False
        pos += len(line)
    return True


def _code(text: str) -> str:
    """TEXT without its // comments."""
    return "\n".join(re.sub(r"//.*", "", line) for line in text.splitlines())


@unittest.skipUnless(all(os.path.isfile(p) for p in (NANO_MENU, LAUNCHER, LAUNCHER_TEST)),
                     "no NanoMenu.cpp, Launcher.cpp or launcher_test.cpp")
class ExitPathLinux(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        text = _read(NANO_MENU)
        start = text.index(EXIT_START)
        cls.section = text[start:text.index(EXIT_END, start)]

    def test_the_exit_path_waits_only_for_app_launched(self):
        loops = _loops(self.section)
        self.assertEqual(len(loops), 2, "threadLoop's exit path should have two app-launch waits "
                                        "(the DRM boot path's and \"Loading...\"); review Launcher.cpp's "
                                        "launchFromHandshake and launcher_test.cpp's exitPathWaits")
        for body in loops:
            code = _squash(_code(body))
            with self.subTest(body=code[:80]):
                self.assertTrue(_in_order(code, WAIT_LINES),
                                "an exit-path wait no longer ends on app_launched == \"1\": %s" % code)
                # One way out (no other break, return or goto), and app_launched its only property.
                self.assertEqual(code.count("break;"), 1, code)
                self.assertNotIn("return", code)
                self.assertNotIn("goto", code)
                self.assertEqual(re.findall(r'property_get\w*\("([^"]+)"', code),
                                 ["sys.gammaos.nano.app_launched"], code)

    def test_the_tests_copy_is_the_same_wait(self):
        text = _read(LAUNCHER_TEST)
        m = re.search(r"// exit-path-wait: begin\n(.*?)// exit-path-wait: end", text, re.S)
        self.assertIsNotNone(m, "launcher_test.cpp lacks its exitPathWaits() copy")
        copy = _squash(_code(m.group(1)))
        self.assertIn("wait < 600", copy)
        self.assertTrue(_in_order(copy, WAIT_LINES), copy)
        self.assertIn("TheMenusExitPathEndsAtOnceAfterNanosLaunch", text)

    def test_the_linux_launcher_sets_app_launched(self):
        text = _code(_read(LAUNCHER))
        self.assertIn('kAppLaunchedKey[] = "sys.gammaos.nano.app_launched"', _squash(text))
        body = text[text.index("void launchFromHandshake("):]
        body = body[:body.index("\n}\n")]
        self.assertIn('property_set(kAppLaunchedKey, "1")', body)
        # Only after the check that the trigger is Nano's own (a refused outside trigger sets nothing).
        self.assertLess(body.index("inCommit()"), body.index("property_set(kAppLaunchedKey"))


if __name__ == "__main__":
    unittest.main()
