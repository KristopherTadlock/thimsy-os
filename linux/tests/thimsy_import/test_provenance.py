"""E.7: every patch carries its provenance line and keeps its name; headers are kept."""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402
from thimsy_import import patches  # noqa: E402


def fixture_patches(family: str) -> dict:
    """ROCKNIX name -> ROCKNIX path for the patches scripts/unpack would apply."""
    p = c.FIXTURE / "projects" / "ROCKNIX"
    dirs = {"RK3566": ["packages/linux/patches/mainline", "packages/linux/patches/mainline-rockchip",
                       "packages/linux/patches/7.3", "devices/RK3566/patches/linux"],
            "H700": ["packages/linux/patches/mainline", "packages/linux/patches/7.2",
                     "devices/H700/patches/linux"]}[family]
    out = []
    for d in dirs:
        for f in sorted((p / d).iterdir()):
            if f.name.endswith(".patch"):
                out.append(("projects/ROCKNIX/%s/%s" % (d, f.name), f))
    return out


class ProvenanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = c.TempDir()
        cls.out = cls.tmp.__enter__()
        for fam in c.FAMILIES:
            code, _o, err = c.run(cls.out, fam)
            assert code == 0, err

    @classmethod
    def tearDownClass(cls):
        cls.tmp.__exit__(None, None, None)

    def series(self, fam):
        root = self.out / "gpl" / "generated" / fam.lower() / "linux"
        names = (root / "series").read_text(encoding="utf-8").split()
        return root, names

    def test_order_names_and_provenance(self):
        for fam in c.FAMILIES:
            root, names = self.series(fam)
            want = fixture_patches(fam)
            self.assertEqual([n.split("/", 1)[1] for n in names], [Path(r).name for r, _ in want],
                             "%s: order or names differ from scripts/unpack's" % fam)
            for name, (rel, src) in zip(names, want):
                data = (root / "patches" / name).read_bytes()
                first, rest = data.split(b"\n", 1)
                line = first.decode("utf-8")
                self.assertTrue(line.startswith(patches.PROVENANCE_PREFIX), name)
                self.assertIn("ROCKNIX %s at %s" % (rel, c.COMMIT), line)
                self.assertIn("GPL-2.0", line)
                self.assertEqual(rest, src.read_bytes(), "%s: body must be ROCKNIX's bytes" % name)

    def test_directories_follow_unpack_order(self):
        _root, names = self.series("RK3566")
        dirs = []
        for n in names:
            d = n.split("/", 1)[0]
            if d not in dirs:
                dirs.append(d)
        self.assertEqual(dirs, ["01-common-mainline", "02-common-mainline-rockchip", "03-common-7.3", "04-device"])
        _root, names = self.series("H700")
        self.assertNotIn("rocknix-joypad", " ".join(n.split("/", 1)[0] for n in names),
                         "the quoted =~ never matches, so ROCKNIX never adds rocknix-joypad")
        self.assertTrue(all(not n.endswith(".disabled") for n in names))

    def test_headers_kept_and_bare_diffs_marked_only_by_provenance(self):
        root, names = self.series("RK3566")
        by = {n.rsplit("/", 1)[1]: (root / "patches" / n).read_text(encoding="utf-8") for n in names}
        self.assertIn("\nFrom: sydarn <sydarn@proton.me>\n", by["0014-drm-panel-st7701-fixup-Anbernic-RG-Arc-panel-timings.patch"])
        self.assertTrue(by["0028-regulator-fan53555-accept-the-STI8070A-on-the-Silergy-path.patch"]
                        .split("\n")[1].startswith("From: Daniel Martin"))
        bare = by["0025-arc-swap-touch-axes.patch"].split("\n")
        self.assertTrue(bare[1].startswith("diff --git"))
        self.assertNotIn("From:", by["0025-arc-swap-touch-axes.patch"])

    def test_provenance_file_lists_every_file(self):
        for fam in c.FAMILIES:
            gpl = self.out / "gpl" / "generated" / fam.lower()
            text = (gpl / "PROVENANCE").read_text(encoding="utf-8")
            self.assertIn(c.COMMIT, text)
            for p in gpl.rglob("*"):
                if p.is_file() and p.name not in ("PROVENANCE", "series"):
                    self.assertIn(p.relative_to(gpl).as_posix() + " <- projects/ROCKNIX/", text)

    def test_config_and_dts_carry_provenance(self):
        cfg = (self.out / "gpl/generated/rk3566/linux/config").read_text(encoding="utf-8").split("\n")
        self.assertTrue(cfg[0].startswith("# " + patches.PROVENANCE_PREFIX))
        self.assertIn("devices/RK3566/linux/linux.aarch64.conf", cfg[0])
        dts = (self.out / "gpl/generated/h700/dts/allwinner/sun50i-h700-anbernic-rg40xx-h.dts").read_text(
            encoding="utf-8").split("\n")
        self.assertIn("SPDX-License-Identifier", dts[0])
        self.assertTrue(dts[1].startswith("// " + patches.PROVENANCE_PREFIX))


if __name__ == "__main__":
    unittest.main()
