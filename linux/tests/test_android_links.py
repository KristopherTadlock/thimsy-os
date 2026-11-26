# SPDX-License-Identifier: MIT
"""board/common/post-build.d/10-android-links.sh against a temporary
TARGET_DIR (IMPLEMENTATION B.5, "Paths"; step G8).

The links must point into the tree S00base makes on first boot
(/storage/.config/thimsy/data and /storage/emulated/0), the /system links
into the root's own directories, nothing may sit under the root's /storage,
and a second run must change nothing. Needs a shell that can make symbolic
links: in WSL or on Linux; on Windows (Git Bash without developer mode) the
tests are skipped.

    py -3.12 -I -m unittest discover -s tests -t . -p test_android_links.py   (from linux/)
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
sys.path.insert(0, str(LINUX / "tools"))

import check_rcs  # noqa: E402

SHELLS = check_rcs.find_shells()
SH: Optional[List[str]] = SHELLS[0] if SHELLS else None
SCRIPT = LINUX / "board/common/post-build.d/10-android-links.sh"
S00BASE = LINUX / "board/common/rootfs-overlay/etc/init.d/S00base"

LINKS = {
    "data": "/storage/.config/thimsy/data",
    "sdcard": "/storage/emulated/0",
    "system/etc": "/usr/share/thimsy/system/etc",
    "system/fonts": "/usr/share/fonts/thimsy",
    "system/bin": "/usr/libexec/thimsy/android-bin",
    "mnt/media_rw": "/run/thimsy/media_rw",
}


def _probe_symlinks() -> bool:
    if SH is None:
        return False
    d = Path(tempfile.mkdtemp(prefix="links-ln-"))
    try:
        subprocess.run(SH + ["-c", 'ln -s /nonexistent/target "$1"', "sh", (d / "l").as_posix()],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        return os.path.islink(str(d / "l"))
    finally:
        shutil.rmtree(str(d), ignore_errors=True)


SYMLINKS = _probe_symlinks()


@unittest.skipIf(not SYMLINKS, "this shell cannot make symbolic links here")
class AndroidLinksTest(unittest.TestCase):
    def setUp(self) -> None:
        self.target = Path(tempfile.mkdtemp(prefix="android-links-"))
        for d in ("etc", "usr/bin", "storage", "mnt"):
            (self.target / d).mkdir(parents=True)

    def tearDown(self) -> None:
        shutil.rmtree(str(self.target), ignore_errors=True)

    def run_script(self) -> "subprocess.CompletedProcess[str]":
        assert SH is not None
        return subprocess.run(SH + [SCRIPT.as_posix(), self.target.as_posix()],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=60)

    def snapshot(self) -> List[str]:
        out = []
        for dirpath, dirnames, filenames in os.walk(str(self.target)):
            for n in sorted(dirnames + filenames):
                p = os.path.join(dirpath, n)
                rel = os.path.relpath(p, str(self.target))
                out.append("%s -> %s" % (rel, os.readlink(p)) if os.path.islink(p) else rel)
        return sorted(out)

    def test_links(self) -> None:
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout)
        for path, dest in LINKS.items():
            p = self.target / path
            self.assertTrue(os.path.islink(str(p)), path)
            self.assertEqual(os.readlink(str(p)), dest, path)
        self.assertTrue((self.target / "system").is_dir())
        self.assertFalse(os.path.islink(str(self.target / "system")))
        for d in ("usr/share/thimsy/system/etc", "usr/share/fonts/thimsy",
                  "usr/libexec/thimsy/android-bin"):
            self.assertTrue((self.target / d).is_dir(), d)
            self.assertFalse(os.path.islink(str(self.target / d)), d)
        # The data partition covers /storage, and B.5 leaves these out.
        self.assertEqual(os.listdir(str(self.target / "storage")), [])
        for absent in ("mnt/shares", "vendor", "dev/block"):
            self.assertFalse(os.path.lexists(str(self.target / absent)), absent)

    def test_rerun_changes_nothing(self) -> None:
        self.assertEqual(self.run_script().returncode, 0)
        before = self.snapshot()
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(self.snapshot(), before)

    def test_empty_directory_is_replaced(self) -> None:
        (self.target / "data").mkdir()
        (self.target / "system/etc").mkdir(parents=True)
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(os.readlink(str(self.target / "data")), LINKS["data"])
        self.assertEqual(os.readlink(str(self.target / "system/etc")), LINKS["system/etc"])

    def test_wrong_link_is_corrected(self) -> None:
        os.symlink("/somewhere/else", str(self.target / "sdcard"))
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(os.readlink(str(self.target / "sdcard")), LINKS["sdcard"])

    def test_files_that_a_link_would_hide_fail(self) -> None:
        (self.target / "data/system").mkdir(parents=True)
        (self.target / "data/system/x.json").write_text("{}")
        r = self.run_script()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("/data is a directory with files in it", r.stdout)

    def test_system_as_a_link_fails(self) -> None:
        os.symlink("/usr", str(self.target / "system"))
        r = self.run_script()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("/system must be a directory", r.stdout)

    def test_something_under_storage_fails(self) -> None:
        (self.target / "storage/emulated").mkdir()
        r = self.run_script()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("/storage must be empty", r.stdout)


class AgreesWithS00baseTest(unittest.TestCase):
    """The link targets under /storage are the tree S00base makes."""

    def test_s00base_makes_the_targets(self) -> None:
        text = S00BASE.read_text(encoding="utf-8")
        self.assertIn('d=$THIMSY_CONFIG/data', text)
        self.assertRegex(
            (LINUX / "board/common/rootfs-overlay/usr/share/thimsy/init/lib.sh").read_text(
                encoding="utf-8"),
            r"THIMSY_CONFIG=\$R/storage/\.config/thimsy\b")
        self.assertIn('"$R/storage/emulated/0"', text)
        self.assertIn('ln -sfn /storage/emulated/0 "$R/storage/self/primary"', text)
        for sub in ("system", "local/tmp", "data", "user", "misc", "gammaos_ota", "media"):
            self.assertRegex(text, r'"\$d/%s"' % re.escape(sub), sub)

    def test_script_text(self) -> None:
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("\r", text)
        self.assertTrue(text.startswith("#!/bin/sh\n"))
        for path, dest in LINKS.items():
            self.assertIn("place %s %s\n" % (path, dest), text)
        self.assertNotIn("by-name", re.sub(r"#.*", "", text))


if __name__ == "__main__":
    unittest.main()
