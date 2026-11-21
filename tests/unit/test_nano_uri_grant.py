"""NanoUriGrant: the folders Nano may give an app through system_server (NanoNetBridge's uri_grant).

hakuX, Azahar, SeedlessDS, WatermelonDS and ScummVM may be given any folder of internal storage or a card
or drive, but never a volume's root, Android/ (other apps' files; an app's own Android/data, media and obb folders
are its to have), the user's photos, videos, recordings and documents, or Nano Backup. Any other app is refused, so a property any root process can set never grants
more.
"""
import os
import shutil
import tempfile

from tests import util

GAMMAOS = os.path.join(util.AOSP, "frameworks/base/services/core/java/com/android/server/gammaos")
FIXTURES = os.path.join(util.TESTS, "fixtures/uri_grant")
CARD = "1A2B-3C4D"


class UriGrantArgs(util.TestCase):
    classes = None

    @classmethod
    def setUpClass(cls):
        cls.build_error = None
        if not (shutil.which("javac") and shutil.which("java")):
            cls.build_error = "javac isn't on PATH"
            return
        if not os.path.isfile(os.path.join(GAMMAOS, "NanoUriGrant.java")):
            cls.build_error = f"no AOSP tree at {util.AOSP}"
            return
        cls.work = tempfile.mkdtemp(prefix="thimsy-urigrant-")
        cls.classes = os.path.join(cls.work, "classes")
        os.makedirs(cls.classes)
        proc = util.run(["javac", "-nowarn", "-encoding", "UTF-8", "-d", cls.classes,
                         os.path.join(GAMMAOS, "NanoUriGrant.java"),
                         os.path.join(FIXTURES, "UriGrantCheck.java")], timeout=300)
        if proc.returncode:
            raise AssertionError("javac failed:\n" + proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "work", None):
            util.rmtree(cls.work)

    def setUp(self):
        super().setUp()
        if self.build_error:
            self.skipTest(self.build_error)

    def parse(self, *args):
        proc = self.assertOk(util.run(["java", "-cp", self.classes, "com.android.server.gammaos.UriGrantCheck"],
                                      input="".join(a + "\n" for a in args)))
        return [None if line == "-" else tuple(line.split("\t")) for line in proc.stdout.splitlines()]

    def test_emulator_folders(self):
        self.assertEqual(self.parse(f"org.azahar_emu.azahar|{CARD}|emulator_config/azahar",
                                    f"com.seedlessds.app|{CARD.lower()}|emulator_config/seedlessds",
                                    "com.rfandango.haku_x|primary|ROMs/xbox",
                                    "org.azahar_emu.azahar|primary|3DS/sdmc"),
                         [("org.azahar_emu.azahar", f"{CARD}:emulator_config/azahar"),
                          ("com.seedlessds.app", f"{CARD.lower()}:emulator_config/seedlessds"),
                          ("com.rfandango.haku_x", "primary:ROMs/xbox"),
                          ("org.azahar_emu.azahar", "primary:3DS/sdmc")])

    def test_drive_with_a_long_uuid(self):
        uuid = "0123456789abcdef0123456789ABCDEF"
        self.assertEqual(self.parse(f"com.rfandango.haku_x|{uuid}|ROMs/xbox"),
                         [("com.rfandango.haku_x", f"{uuid}:ROMs/xbox")])

    def test_any_folder_but_the_protected_ones(self):
        self.assertEqual(self.parse("me.magnum.melondualds|primary|ROMs/nds",
                                    f"me.magnum.melondualds|{CARD}|BIOS",
                                    "me.magnum.melondualds|primary|Download",
                                    "org.scummvm.scummvm|primary|ROMs/scummvm/Monkey Island (CD)",
                                    f"org.scummvm.scummvm|{CARD}|Games/ScummVM",
                                    "org.scummvm.scummvm|primary|Music/Androids",
                                    "com.rfandango.haku_x|primary|ROMs"),
                         [("me.magnum.melondualds", "primary:ROMs/nds"),
                          ("me.magnum.melondualds", f"{CARD}:BIOS"),
                          ("me.magnum.melondualds", "primary:Download"),
                          ("org.scummvm.scummvm", "primary:ROMs/scummvm/Monkey Island (CD)"),
                          ("org.scummvm.scummvm", f"{CARD}:Games/ScummVM"),
                          ("org.scummvm.scummvm", "primary:Music/Androids"),
                          ("com.rfandango.haku_x", "primary:ROMs")])

    def test_the_apps_own_android_folders(self):
        self.assertEqual(self.parse("org.scummvm.scummvm|primary|Android/data/org.scummvm.scummvm/files",
                                    "me.magnum.melondualds|primary|Android/media/me.magnum.melondualds",
                                    f"org.vita3k.emulator|{CARD}|Android/data/org.vita3k.emulator",
                                    f"com.rfandango.haku_x|{CARD}|android/obb/com.rfandango.haku_x"),
                         [("org.scummvm.scummvm", "primary:Android/data/org.scummvm.scummvm/files"),
                          ("me.magnum.melondualds", "primary:Android/media/me.magnum.melondualds"),
                          None,                                     # not one of the apps
                          ("com.rfandango.haku_x", f"{CARD}:android/obb/com.rfandango.haku_x")])

    def test_refused(self):
        refused = [
            "me.magnum.melondualds|primary|",                      # the whole storage
            f"org.scummvm.scummvm|{CARD}|",
            "com.rfandango.haku_x|primary|Android",                # other apps' files
            "org.scummvm.scummvm|primary|android/data/org.ppsspp.ppsspp",
            "org.scummvm.scummvm|primary|Android/data",
            "org.scummvm.scummvm|primary|Android/data/org.scummvm.scummvm.evil",
            "org.scummvm.scummvm|primary|Android/data/ORG.SCUMMVM.SCUMMVM",
            "org.scummvm.scummvm|primary|Android/system/org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|Android/data/../data/org.ppsspp.ppsspp",
            f"org.azahar_emu.azahar|{CARD}|Android/obb",
            "me.magnum.melondualds|primary|DCIM",                  # the user's own files
            "com.seedlessds.app|primary|DCIM/Camera",
            "org.scummvm.scummvm|primary|pictures",
            "org.scummvm.scummvm|primary|Movies",
            "com.rfandango.haku_x|primary|Recordings",
            "org.scummvm.scummvm|primary|Documents/ROMs",
            "me.magnum.melondualds|primary|Nano Backup",           # what Nano set aside
            "me.magnum.melondualds|primary|ROMs/../DCIM",
            f"org.azahar_emu.azahar|{CARD}|emulator_config/../Android/data",
            "me.magnum.melondualds|primary|./DCIM",
            "com.rfandango.haku_x|primary|ROMs/",
            "me.magnum.melondualds|primary|ROMs//nds",
            "me.magnum.melondualds|primary|/ROMs",
            "me.magnum.melondualds|primary|ROMs/a:b",
            "me.magnum.melondualds|primary|ROMs/a\\b",
            "com.rfandango.haku_x|primary:ROMs|xbox",
            "com.rfandango.haku_x|primary|ROMs/xbox|extra",
            "com.rfandango.haku_x|primary",
            "",
            "|primary|ROMs/xbox",                                 # no package
            "haku_x|primary|ROMs/xbox",                           # not one of the apps
            "com.example.app|primary|ROMs/xbox",
            f"com.example.app|{CARD}|emulator_config/azahar",
            "com.rfandango.haku x|primary|ROMs/xbox",
            "com.rfandango.haku_x|Primary|ROMs/xbox",             # not a volume
            "com.rfandango.haku_x|-|ROMs/xbox",
            "com.rfandango.haku_x|1A2B|ROMs/xbox",
            f"com.rfandango.haku_x|{CARD}-0|ROMs/xbox",
            "org.azahar_emu.azahar|../x|emulator_config/azahar",
            "org.azahar_emu.azahar||emulator_config/azahar",
        ]
        self.assertEqual(self.parse(*refused), [None] * len(refused))

    def test_other_spellings_of_protected_folders(self):
        refused = [
            "me.magnum.melondualds|primary|DCIM.",                 # trailing dot
            "me.magnum.melondualds|primary|DCIM ",                 # trailing space
            "me.magnum.melondualds|primary|DC\u200bIM",            # zero-width space
            "me.magnum.melondualds|primary|D\u00adCIM/Camera",     # soft hyphen
            "me.magnum.melondualds|primary|NANOBA~1",              # FAT 8.3 alias of Nano Backup
            f"me.magnum.melondualds|{CARD}|DOCUME~1/ROMs",
            f"me.magnum.melondualds|{CARD}|PICTUR~1",
            f"me.magnum.melondualds|{CARD}|RE1A2B~1",
            "me.magnum.melondualds|primary|Nano Bac\u212aup",      # Kelvin sign folds to k
            "me.magnum.melondualds|primary|Picture\u017f",         # long s folds to s
            "me.magnum.melondualds|primary|\u200b",                # folds to nothing
            "org.scummvm.scummvm|primary|Android./data/org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|Andr\u200boid/data/org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|Android/data./org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|Android/da\u200bta/org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|ANDROI~1/data/org.scummvm.scummvm",
            "org.scummvm.scummvm|primary|Android ",
        ]
        self.assertEqual(self.parse(*refused), [None] * len(refused))

    def test_non_ascii_folders(self):
        allowed = [
            "com.rfandango.haku_x|primary|\u30b2\u30fc\u30e0/GBA",
            "com.rfandango.haku_x|primary|Juegos cl\u00e1sicos",
            "org.scummvm.scummvm|primary|M\u00fasica",
            f"org.scummvm.scummvm|{CARD}|\u00c9mulateurs/ScummVM",
            "org.scummvm.scummvm|primary|GAMES~1",
            "org.scummvm.scummvm|primary|DCIM2",
        ]
        results = self.parse(*allowed)
        self.assertEqual(len(results), len(allowed))
        for arg, result in zip(allowed, results):
            self.assertIsNotNone(result, arg)
            self.assertEqual(result[0], arg.split("|")[0])
