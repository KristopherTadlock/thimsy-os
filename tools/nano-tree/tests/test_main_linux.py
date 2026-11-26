"""nano/platform/linux/main_linux.cpp mirrors the steps of nano/main.cpp's main() by hand until G7-main
moves them into a shared nanoBootSequence() (docs/linux/IMPLEMENTATION.md B.7). This test fails when
main() changes (a merge from nano-144, say), so main_linux.cpp's step table is reviewed with it; and it
checks that main_linux.cpp sets up the platform before Nano's first thread. Fork-only. Run with

  python3 -m unittest discover -s tools/nano-tree/tests -t tools/nano-tree/tests -p test_main_linux.py

After reviewing main_linux.cpp against a changed main(), record the new fingerprint with
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

# sha256 of nano/main.cpp's main(), whitespace collapsed, when main_linux.cpp's table was last checked.
MAIN_FINGERPRINT = "3e84fd013c48376efd9bed48c3bb34abede2da6072820723348ef4b84d33d3fd"


def fingerprint(text: str) -> str:
    body = text[text.index("int main(int argc, char** argv) {"):]
    return hashlib.sha256(re.sub(r"\s+", " ", body).strip().encode("utf-8")).hexdigest()


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


@unittest.skipUnless(os.path.isfile(MAIN) and os.path.isfile(MAIN_LINUX), "no nano/main.cpp or main_linux.cpp")
class MainLinux(unittest.TestCase):
    def test_main_unchanged_since_review(self):
        self.assertEqual(fingerprint(_read(MAIN)), MAIN_FINGERPRINT,
                         "nano/main.cpp's main() changed: review nano/platform/linux/main_linux.cpp's step "
                         "table against it, then run tools/nano-tree/tests/test_main_linux.py --print")

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


if __name__ == "__main__":
    if sys.argv[1:] == ["--print"]:
        print(fingerprint(_read(MAIN)))
        sys.exit(0)
    unittest.main()
