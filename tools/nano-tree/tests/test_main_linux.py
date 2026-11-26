"""nano/main.cpp's portable boot steps are nano/NanoBootSequence.cpp's nanoBootSequence(), which both
mains call (docs/linux/IMPLEMENTATION.md B.7, G7-main); what main.cpp does around it (the overlay
instance, the relaunch and stray-instance guards, the early DRM grab, the firmware's boot steps and
binder) is firmware-only, and nano/platform/linux/main_linux.cpp's comment says what it does instead.
This test fails when main.cpp's main() or firmwareBootSteps() changes (a merge from nano-144, say), so
that list is reviewed with it; it checks that main.cpp installs the platform first and calls the shared
sequence after the early DRM section, that the sequence keeps main.cpp's order and log tag, and that
main_linux.cpp sets up the platform before Nano's first thread. Fork-only. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_main_linux.py

After reviewing main_linux.cpp against a changed main.cpp, record the new fingerprint with
  python3 tools/nano-tree/tests/test_main_linux.py --print
"""
from __future__ import annotations

import hashlib
import os
import re
import sys
import unittest

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(TOOL_DIR))
MAIN = os.path.join(ROOT, "nano", "main.cpp")
MAIN_LINUX = os.path.join(ROOT, "nano", "platform", "linux", "main_linux.cpp")
BOOT_SEQUENCE = os.path.join(ROOT, "nano", "NanoBootSequence.cpp")

# sha256 of nano/main.cpp from firmwareBootSteps() to the end (main() included), whitespace collapsed,
# when main_linux.cpp's list was last checked.
MAIN_FINGERPRINT = "da01642c2a6750902bf80fcbade4e159c03df52afce8e8f8632b274e3af8477f"


def fingerprint(text: str) -> str:
    body = text[text.index("static void firmwareBootSteps() {"):]
    return hashlib.sha256(re.sub(r"\s+", " ", body).strip().encode("utf-8")).hexdigest()


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


@unittest.skipUnless(os.path.isfile(MAIN) and os.path.isfile(MAIN_LINUX), "no nano/main.cpp or main_linux.cpp")
class MainLinux(unittest.TestCase):
    def test_main_unchanged_since_review(self):
        self.assertEqual(fingerprint(_read(MAIN)), MAIN_FINGERPRINT,
                         "nano/main.cpp's main() or firmwareBootSteps() changed: review "
                         "nano/platform/linux/main_linux.cpp's list of main.cpp's steps against it, then "
                         "run tools/nano-tree/tests/test_main_linux.py --print")

    def test_platform_before_threads(self):
        text = _read(MAIN_LINUX)
        body = text[text.index("int main("):]
        order = ["nanocompat::setLogFile(", "nanocompat::init(", "nanolinux::controlkeys::install(",
                 "new NanoMenu()", "->run(\"GammaOSNano\"", "->join()"]
        pos = [body.find(s) for s in order]
        self.assertNotIn(-1, pos, "main_linux.cpp lacks one of %s" % order)
        self.assertEqual(pos, sorted(pos), "main_linux.cpp must set up the platform in this order: %s" % order)
        self.assertIn('"/storage/.config/thimsy/persist.props"', text)
        self.assertIn('"/run/thimsy/props"', text)
        self.assertIn('"/run/thimsy/log/nano.log"', text)
        self.assertIn("int gEarlyDrmFd = -1;", text, "main.cpp's global, read by NanoMenu.cpp")


def _function(text: str, head: str) -> str:
    """The body of the function whose definition starts with HEAD (up to the first line "}")."""
    start = text.index(head)
    return text[start:text.index("\n}\n", start) + 2]


@unittest.skipUnless(os.path.isfile(MAIN) and os.path.isfile(BOOT_SEQUENCE), "no nano/main.cpp or NanoBootSequence.cpp")
class MainCpp(unittest.TestCase):
    """main.cpp's side of G7-main: the platform before anything, the shared sequence where the moved
    steps were, the firmware's own steps where they ran."""

    def test_platform_installed_first(self):
        main = _function(_read(MAIN), "int main(int argc, char** argv) {")
        first = [ln.strip() for ln in main.split("\n")[1:] if ln.strip() and not ln.strip().startswith("//")][0]
        self.assertEqual(first, "nanoplat::install(nanoplat::createPlatform(nanoplat::PlatformArgs{argc, argv}));")
        self.assertEqual(main.count("nanoplat::install("), 1)

    def test_boot_sequence_after_the_early_drm_section(self):
        main = _function(_read(MAIN), "int main(int argc, char** argv) {")
        # The last joinThreadPool: the overlay instance has its own, earlier.
        order = ["--overlay", "auto_relaunch", "overlay_home", "DRM_IOCTL_SET_MASTER", "gEarlyDrmFd = fd;",
                 "nanoBootSequence(firmwareBootSteps);", "IPCThreadState::self()->joinThreadPool();"]
        pos = [main.find(s) for s in order[:-1]] + [main.rfind(order[-1])]
        self.assertNotIn(-1, pos, "main() lacks one of %s" % order)
        self.assertEqual(pos, sorted(pos))
        self.assertEqual(main.count("nanoBootSequence("), 1)
        # The moved steps are gone from main(); the overlay instance keeps its own.
        tail = main[main.index("gEarlyDrmFd = fd;"):]
        for gone in ("setpriority(", "drm_active", "new NanoMenu()", "startThreadPool()", "drasticNano"):
            self.assertNotIn(gone, tail)

    def test_firmware_steps(self):
        steps = _function(_read(MAIN), "static void firmwareBootSteps() {")
        order = ["const bool drasticNano = drasticNanoAvailable();", "if (drasticNano) patchDrasticAudioInit();",
                 "if (drasticNano) startDrasticLibPreloadThread();", "if (drasticNano) runDrasticInitIfNeeded();",
                 "sDrasticEarlyInitDone.store(true", "if (drasticNano) startDrasticQrTestWatcher();",
                 "sp<ProcessState> proc(ProcessState::self());", "ProcessState::self()->startThreadPool();"]
        pos = [steps.find(s) for s in order]
        self.assertNotIn(-1, pos, "firmwareBootSteps() lacks one of %s" % order)
        self.assertEqual(pos, sorted(pos))

    def test_sequence_keeps_main_cpps_order_and_tag(self):
        text = _read(BOOT_SEQUENCE)
        self.assertRegex(text, r'(?m)^#define LOG_TAG "GammaOSNano"$', "main.cpp's tag: the firmware's log is unchanged")
        seq = _function(text, "sp<NanoMenu> nanoBootSequence(void (*platformSteps)()) {")
        order = ["setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_DISPLAY);",
                 'ALOGI("GammaOS Nano starting... (earlyDrmFd=%d)", gEarlyDrmFd);',
                 'property_set("sys.gammaos.nano.drm_active", "0");',
                 "if (platformSteps) platformSteps();",
                 "sp<NanoMenu> nano = new NanoMenu();",
                 'nano->run("GammaOSNano", PRIORITY_DISPLAY);',
                 "return nano;"]
        pos = [seq.find(s) for s in order]
        self.assertNotIn(-1, pos, "nanoBootSequence() lacks one of %s" % order)
        self.assertEqual(pos, sorted(pos))
        self.assertNotIn("nanoplat::", text)


SOURCES =os.path.join(ROOT, "nano", "build", "sources.txt")
MAIN_HEADLESS = os.path.join(ROOT, "nano", "test", "host", "headless", "main_headless.cpp")


@unittest.skipUnless(os.path.isfile(SOURCES) and os.path.isfile(MAIN_HEADLESS), "no sources.txt or main_headless.cpp")
class MainGlobals(unittest.TestCase):
    """What main.cpp defines and [core]/[drm] use must be defined by every other main, with
    main.cpp's value: left to the [dropped-linux] stub generator, gEarlyDrmFd would be 0 (stdin's fd)
    and NanoMenu::readyToRun would take stdin for the DRM card (G4's hand-off)."""

    def test_main_cpp_globals_are_the_known_ones(self):
        # sources.txt's [dropped-linux] comment names what [core]/[drm] use from main.cpp; a new name
        # there needs a definition in both mains, and a line in this test.
        m = re.search(r"^# main\.cpp: \[core\]/\[drm\] name what it defines: (.*)$", _read(SOURCES), re.M)
        self.assertIsNotNone(m, "sources.txt's [dropped-linux] no longer lists main.cpp")
        self.assertEqual(m.group(1).strip(), "gEarlyDrmFd")
        self.assertRegex(_read(MAIN), r"(?m)^int gEarlyDrmFd = -1;")

    def test_every_main_defines_gearlydrmfd_as_minus_one(self):
        for path in (MAIN_LINUX, MAIN_HEADLESS):
            self.assertRegex(_read(path), r"(?m)^int gEarlyDrmFd = -1;", path)


if __name__ == "__main__":
    if sys.argv[1:] == ["--print"]:
        print(fingerprint(_read(MAIN)))
        sys.exit(0)
    unittest.main()
