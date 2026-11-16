#!/usr/bin/env python3
"""Tests for apply_pack.py (stdlib only): run it on a temporary copy of the
catalog and check it sets one profile's package entry, keeps the others and
the rest of the device file, and never writes build pins.

  python3 installer/tools/test_apply_pack.py
"""
import json, os, re, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "apply_pack.py")
CATALOG = os.path.join(os.path.dirname(HERE), "catalog")
DEVICE = "anbernic-rg-cube"
LATEST = "https://github.com/KristopherTadlock/thimsy-os/releases/latest/download"


def flow_to_json(text):
    """The flow mappings apply_pack writes (bare keys, JSON values) as JSON."""
    return json.loads(re.sub(r'([{,]\s*)([A-Za-z_][\w-]*):', r'\1"\2":', text))


def top_block(text, name):
    """The indented lines of the top-level block `name:`."""
    m = re.search(rf"^{name}:[ \t]*\n((?:[ \t]+\S.*\n)*)", text, flags=re.M)
    return m.group(1) if m else None


def entries(text):
    """profile → package entry of the device file's `packages:`."""
    return {k: flow_to_json(v) for k, v in re.findall(r"^  (\w+): (\{.*\})$", top_block(text, "packages"), flags=re.M)}


class ApplyPackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="apply-pack-")
        self.cat = os.path.join(self.tmp, "catalog")
        shutil.copytree(CATALOG, self.cat)
        self.dev_yaml = os.path.join(self.cat, "devices", f"{DEVICE}.yaml")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def apply(self, profile, bundle, tag=None, ok=True):
        bj = os.path.join(self.tmp, f"{profile}.json")
        with open(bj, "w", encoding="utf-8") as f:
            json.dump(bundle, f)
        args = [sys.executable, "-I", TOOL] + (["--tag", tag] if tag else []) + [bj, self.dev_yaml, profile]
        r = subprocess.run(args, capture_output=True, text=True,
                           env={**os.environ, "LC_ALL": "C", "PYTHONIOENCODING": "ascii"})
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr)
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout)
        return r

    def read(self, path):
        with open(path, encoding="utf-8") as f:
            return f.read()

    def test_points_the_profile_at_the_latest_release(self):
        before = self.read(self.dev_yaml)
        self.apply("heavy", {"id": "T.tar", "file": "Thimsy-1.0b-anbernic-rg-cube-heavy.tar", "size": 2_100_000_000,
                             "sha256": "1" * 64})
        dev = self.read(self.dev_yaml)
        self.assertEqual(entries(dev)["heavy"], {
            "file": "Thimsy-1.0b-anbernic-rg-cube-heavy.tar",
            "approx_size": 2_100_000_000,
            "note": "Download Thimsy-1.0b-anbernic-rg-cube-heavy.tar from the Thimsy release page, then choose it "
                    "with Use a file I already downloaded.",
            "urls": [f"{LATEST}/Thimsy-1.0b-anbernic-rg-cube-heavy.tar"],
        })
        # Only that line changed; no build pins appear.
        self.assertEqual([l for l in dev.splitlines() if l not in before.splitlines()],
                         [l for l in dev.splitlines() if l.startswith("  heavy: ")])
        self.assertNotIn("1" * 64, dev)
        for pin in ("build", "expect", "artifacts", "bundles"):
            self.assertIsNone(re.search(rf"^{pin}:", dev, flags=re.M), pin)

    def test_a_tag_and_parts(self):
        parts = [{"id": f"T.tar.{i:03d}", "file": f"T.tar.{i:03d}", "size": 1000, "sha256": "2" * 64} for i in (1, 2)]
        self.apply("medium", {"id": "T.tar", "file": "T.tar", "size": 2000, "sha256": "3" * 64, "parts": parts},
                   tag="installer-v0.1.0-test18")
        e = entries(self.read(self.dev_yaml))
        base = "https://github.com/KristopherTadlock/thimsy-os/releases/download/installer-v0.1.0-test18"
        self.assertEqual(e["medium"]["parts"], [{"file": "T.tar.001", "urls": [f"{base}/T.tar.001"]},
                                                 {"file": "T.tar.002", "urls": [f"{base}/T.tar.002"]}])
        self.assertNotIn("urls", e["medium"])
        self.assertIn("from the Thimsy Installer release page", e["medium"]["note"])
        # The heavy package is kept.
        self.assertEqual(e["heavy"]["file"], "Thimsy-1.0a-anbernic-rg-cube-heavy.tar")

    def test_reapplying_replaces_only_that_profile(self):
        self.apply("medium", {"file": "M.tar", "size": 5})
        self.apply("heavy", {"file": "H1.tar", "size": 6})
        self.apply("heavy", {"file": "H2.tar", "size": 7})
        dev = self.read(self.dev_yaml)
        e = entries(dev)
        self.assertEqual({k: v["file"] for k, v in e.items()}, {"heavy": "H2.tar", "medium": "M.tar"})
        self.assertEqual(len(re.findall(r"^packages:", dev, flags=re.M)), 1)
        self.assertIn("flash:\n  method: spd\n", dev)

    def test_device_without_packages_gets_them(self):
        other = os.path.join(self.cat, "devices", "anbernic-rg557.yaml")
        self.dev_yaml = other
        before = self.read(other)
        self.assertIsNone(re.search(r"^packages:", before, flags=re.M))
        self.apply("heavy", {"file": "X.tar", "size": 1})
        dev = self.read(other)
        self.assertTrue(dev.startswith(before.split("\nflash:")[0]))
        self.assertEqual(entries(dev)["heavy"]["file"], "X.tar")

    def test_bad_input_is_refused(self):
        before = self.read(self.dev_yaml)
        self.apply("../heavy", {"file": "X.tar", "size": 1}, ok=False)
        self.apply("heavy", {"file": "../X.tar", "size": 1}, ok=False)
        self.apply("heavy", {"file": "X.tar", "size": 1}, tag="v1/../../x", ok=False)
        self.assertEqual(self.read(self.dev_yaml), before)


if __name__ == "__main__":
    unittest.main()
