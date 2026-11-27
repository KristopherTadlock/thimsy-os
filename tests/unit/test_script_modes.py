"""The scripts the README and their own headers run directly (scripts/build_144.sh,
sudo scripts/aosp/setup_host.sh, ...) are executable in git. A Windows checkout has
core.filemode false, so `git add` records a new script as 100644 and a Linux checkout then
can't run it; after `git add`, `git update-index --chmod=+x` on these fixes that. A script not
yet in the index is skipped.
"""
import shutil
import subprocess
import unittest

from tests import util

# Each with a shebang, run as a command somewhere (README, a header, another script).
DIRECT = (
    "scripts/build.sh",
    "scripts/build_144.sh",
    "scripts/aosp/setup_host.sh",
    "scripts/aosp/system.sh",
    "scripts/aosp/sync_patch.py",
    "scripts/patch_vendor.py",
    "scripts/patch_vendor_dlkm.py",
    "scripts/patch_vendor_boot.py",
    "scripts/patch_dtbo.py",
)


def index_modes(paths):
    """{path: mode} for those of PATHS in the git index; None when ROOT isn't a git checkout."""
    proc = subprocess.run(["git", "-c", "safe.directory=*", "-C", util.ROOT, "ls-files", "-s", "--", *paths],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    out = {}
    for line in proc.stdout.splitlines():
        meta, path = line.split("\t", 1)
        out[path] = meta.split()[0]
    return out


@unittest.skipUnless(shutil.which("git"), "needs git")
class ScriptModes(util.TestCase):
    def test_direct_scripts_are_executable(self):
        for rel in DIRECT:
            with open(util.ROOT + "/" + rel, "rb") as f:
                self.assertEqual(f.read(2), b"#!", rel)
        modes = index_modes(DIRECT)
        if modes is None:
            self.skipTest("not a git checkout")
        wrong = sorted(p for p, m in modes.items() if m != "100755")
        self.assertEqual(wrong, [], "run `git update-index --chmod=+x` on these")
