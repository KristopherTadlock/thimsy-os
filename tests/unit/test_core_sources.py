"""core_sources.py: mods/retroarch-cores.src.lock, the source commit of each Linux core, read
from the Android build's pinned buildbot binaries (docs/linux/IMPLEMENTATION.md E.3, I10).

The real-cache test runs only with THIMSY_TEST_CACHE (the build's work/cache); everything else
uses zips it writes itself."""
import contextlib
import hashlib
import io
import os
import zipfile

from tests import util

cs = util.load_script("core_sources")


def quiet_main(argv):
    """cs.main(argv) with its output captured (returned with the exit status)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = cs.main(argv)
    return code


def so_bytes(*strings):
    """A stand-in for a core's .so: binary noise around NUL-terminated strings."""
    out = b"\x7fELF\x02\x01\x01" + bytes(range(1, 32))
    for s in strings:
        out += b"\x00\x01\x02" + s.encode() + b"\x00"
    return out


class TestHashCandidates(util.TestCase):
    def test_version_and_hash(self):
        self.assertEqual(cs.commit_from_binary(so_bytes("snes9x", "1.62.3 abc1234")), "short:abc1234")
        self.assertEqual(cs.commit_from_binary(so_bytes("v2.0-rc1 0a1b2c3d4e")), "short:0a1b2c3d4e")
        self.assertEqual(cs.commit_from_binary(so_bytes("1.0.0-a1b2c3d")), "short:a1b2c3d")

    def test_bare_hash(self):
        self.assertEqual(cs.commit_from_binary(so_bytes("Genesis Plus GX", "7f3e2a1")), "short:7f3e2a1")

    def test_versioned_hash_wins_over_bare(self):
        self.assertEqual(cs.commit_from_binary(so_bytes("1.2 abc1234", "deadbe1")), "short:abc1234")

    def test_ignores_words_numbers_and_md5_tables(self):
        data = so_bytes("deadbeef", "12345678", "0123456789abcdef0123456789abcdef", "Mesen 0.9.9",
                        "0x1234abcd")
        self.assertEqual(cs.commit_from_binary(data), "pending:no-hash")

    def test_ambiguous(self):
        self.assertEqual(cs.commit_from_binary(so_bytes("1.0 abc1234", "2.0 def5678")), "pending:ambiguous")


class TestSourceLock(util.TestCase):
    def make_zip(self, name, so):
        path = self.path("tmp.zip")
        with zipfile.ZipFile(path, "w") as z:
            z.writestr(name.replace(".zip", ""), so)
        with open(path, "rb") as f:
            data = f.read()
        sha = hashlib.sha256(data).hexdigest()
        self.write("cache/emulators/buildbot/" + sha, data)
        return sha

    def setUp(self):
        super().setUp()
        sha = self.make_zip("snes9x_libretro_android.so.zip", so_bytes("1.62.3 abc1234"))
        self.snes_sha = sha
        lock = ["# test lock",
                "snes9x_libretro_android.so.zip  %s  https://example.invalid/s.zip" % sha,
                "mgba_libretro_android.so.zip  %s  https://example.invalid/m.zip" % ("0" * 64),
                "fake08_libretro_android.so.zip  bundled"]
        self.lock = self.write("retroarch-cores.lock", "\n".join(lock) + "\n")
        self.out = self.path("retroarch-cores.src.lock")

    def run_main(self, *extra):
        return quiet_main(["--lock", self.lock, "--cache", self.path("cache"), "--out", self.out] + list(extra))

    def rows(self):
        return cs.read_src_lock(self.out)

    def test_writes_every_linux_core(self):
        self.assertEqual(self.run_main(), 0)
        rows = self.rows()
        self.assertEqual(sorted(rows), sorted(cs.LINUX_CORES))
        self.assertEqual(rows["snes9x"], ("https://github.com/libretro/snes9x", "short:abc1234",
                                          "snes9x_libretro_android.so.zip@" + self.snes_sha))
        self.assertEqual(rows["mgba"][1], "pending:not-in-cache")
        self.assertEqual(rows["fake08"][1], "pending:bundled")
        self.assertEqual(rows["melondsds"][1], "pending:not-in-android-build")
        self.assertEqual(rows["flycast"][1], "pending:not-in-lock")
        with open(self.out, encoding="utf-8") as f:
            text = f.read()
        self.assertTrue(text.startswith("# The source commit"))
        self.assertNotIn("\r", text)

    def test_keeps_a_resolved_commit(self):
        full = "abc1234" + "f" * 33
        self.assertEqual(self.run_main(), 0)
        with open(self.out, encoding="utf-8") as f:
            text = f.read().replace("short:abc1234", full)
        with open(self.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.rows()["snes9x"][1], full)

    def test_drops_a_commit_whose_source_changed(self):
        self.assertEqual(self.run_main(), 0)
        with open(self.out, encoding="utf-8") as f:
            text = f.read().replace("short:abc1234", "abc1234" + "f" * 33)
        with open(self.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        sha = self.make_zip("snes9x_libretro_android.so.zip", so_bytes("1.63 0011aab"))
        with open(self.lock, encoding="utf-8") as f:
            lock = f.read().replace(self.snes_sha, sha)
        with open(self.lock, "w", encoding="utf-8") as f:
            f.write(lock)
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.rows()["snes9x"][1], "short:0011aab")

    def test_resolve_uses_the_lookup(self):
        calls = []

        def fake(repo, short, timeout=20.0):
            calls.append((repo, short))
            return short + "e" * (40 - len(short))
        orig = cs.resolve_short
        cs.resolve_short = fake
        try:
            self.assertEqual(self.run_main("--resolve"), 0)
        finally:
            cs.resolve_short = orig
        self.assertEqual(calls, [("https://github.com/libretro/snes9x", "abc1234")])
        self.assertEqual(self.rows()["snes9x"][1], "abc1234" + "e" * 33)

    def test_dry_run_writes_nothing(self):
        self.assertEqual(self.run_main("--dry-run"), 0)
        self.assertFalse(os.path.exists(self.out))

    def test_unreadable_zip_is_pending(self):
        self.write("cache/emulators/buildbot/" + "0" * 64, b"not a zip")
        self.assertEqual(self.run_main(), 0)
        self.assertEqual(self.rows()["mgba"][1], "pending:unreadable")

    def test_android_lock_names_exist(self):
        # Every Linux core that has an Android build names a file the real lock pins.
        lock = cs.read_lock(cs.LOCK)
        for core, (repo, name) in cs.LINUX_CORES.items():
            self.assertTrue(repo.startswith("https://github.com/"), core)
            if name is not None:
                self.assertIn(name, lock, core)


class TestRealCache(util.TestCase):
    def test_real_cache(self):
        if not util.TEST_CACHE or not os.path.isdir(os.path.join(util.TEST_CACHE, "emulators", "buildbot")):
            self.skipTest("THIMSY_TEST_CACHE has no emulators/buildbot (the build's downloads)")
        out = self.path("src.lock")
        self.assertEqual(quiet_main(["--cache", util.TEST_CACHE, "--out", out]), 0)
        rows = cs.read_src_lock(out)
        self.assertEqual(sorted(rows), sorted(cs.LINUX_CORES))
