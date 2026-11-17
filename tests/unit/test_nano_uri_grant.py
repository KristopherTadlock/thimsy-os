"""NanoUriGrant: the folders Nano may give an app through system_server (NanoNetBridge's uri_grant).

Only these go: Azahar and SeedlessDS get emulator_config/<one folder> on a card or drive; hakuX gets
ROMs/<one folder> on internal storage or a card. Anything else, any other app included, is refused, so a
property any root process can set never grants more.
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

    def test_card_emulator_folders_as_before(self):
        self.assertEqual(self.parse(f"org.azahar_emu.azahar|{CARD}|emulator_config/azahar",
                                    f"com.seedlessds.app|{CARD.lower()}|emulator_config/seedlessds"),
                         [("org.azahar_emu.azahar", f"{CARD}:emulator_config/azahar"),
                          ("com.seedlessds.app", f"{CARD.lower()}:emulator_config/seedlessds")])

    def test_rom_folder_on_internal_storage(self):
        self.assertEqual(self.parse("com.rfandango.haku_x|primary|ROMs/xbox", "com.rfandango.haku_x|primary|ROMs/Xbox_360-2"),
                         [("com.rfandango.haku_x", "primary:ROMs/xbox"), ("com.rfandango.haku_x", "primary:ROMs/Xbox_360-2")])

    def test_drive_with_a_long_uuid(self):
        uuid = "0123456789abcdef0123456789ABCDEF"
        self.assertEqual(self.parse(f"com.rfandango.haku_x|{uuid}|ROMs/xbox"),
                         [("com.rfandango.haku_x", f"{uuid}:ROMs/xbox")])

    def test_rom_folder_on_a_card(self):
        self.assertEqual(self.parse(f"com.rfandango.haku_x|{CARD}|ROMs/xbox", f"com.rfandango.haku_x|{CARD.lower()}|ROMs/Xbox"),
                         [("com.rfandango.haku_x", f"{CARD}:ROMs/xbox"),
                          ("com.rfandango.haku_x", f"{CARD.lower()}:ROMs/Xbox")])

    def test_refused(self):
        refused = [
            "com.rfandango.haku_x|primary|ROMs",                  # the whole ROMs folder
            "com.rfandango.haku_x|primary|ROMs/",
            "com.rfandango.haku_x|primary|ROMs/xbox/saves",       # one folder only
            "com.rfandango.haku_x|primary|ROMs/..",
            "com.rfandango.haku_x|primary|ROMs/x.box",
            "com.rfandango.haku_x|primary|ROMs/x box",
            "com.rfandango.haku_x|primary|roms/xbox",
            "com.rfandango.haku_x|primary|Download",
            "com.rfandango.haku_x|primary|emulator_config/azahar",  # internal storage: ROM folders only
            "com.rfandango.haku_x|Primary|ROMs/xbox",
            "com.rfandango.haku_x|primary:ROMs|xbox",
            f"com.rfandango.haku_x|{CARD}|ROMs",                 # a card: one ROM folder, as on internal storage
            f"com.rfandango.haku_x|{CARD}|ROMs/xbox/saves",
            f"com.rfandango.haku_x|{CARD}|ROMs/..",
            f"com.rfandango.haku_x|{CARD}|roms/xbox",
            f"com.rfandango.haku_x|{CARD}|xbox",
            f"org.azahar_emu.azahar|{CARD}|emulator_config/../Android/data",
            f"org.azahar_emu.azahar|{CARD}|Android/data",
            "org.azahar_emu.azahar|../x|emulator_config/azahar",
            "org.azahar_emu.azahar||emulator_config/azahar",
            "com.rfandango.haku_x|primary|ROMs/xbox|extra",
            "com.rfandango.haku_x|primary",
            "",
            "|primary|ROMs/xbox",                                 # no package
            "haku_x|primary|ROMs/xbox",                           # not a package name
            "com.rfandango.haku x|primary|ROMs/xbox",
            "com.rfandango.haku_x|-|ROMs/xbox",                   # not a volume uuid
            "com.rfandango.haku_x|1A2B|ROMs/xbox",
            f"com.rfandango.haku_x|{CARD}-0|ROMs/xbox",
            f"org.azahar_emu.azahar|{CARD}|emulator_config",      # the whole folder
            f"org.azahar_emu.azahar|{CARD}|emulator_config/",
            f"org.azahar_emu.azahar|{CARD}|emulator_config/azahar/sdmc",  # one folder only
            f"org.azahar_emu.azahar|{CARD}|emulator_config/.",
            "a.b|primary|ROMs/xbox",                              # only hakuX gets ROM folders
            f"org.azahar_emu.azahar|{CARD}|ROMs/xbox",
            f"com.rfandango.haku_x|{CARD}|emulator_config/azahar",  # nor another app's settings
            f"com.example.app|{CARD}|emulator_config/azahar",
        ]
        self.assertEqual(self.parse(*refused), [None] * len(refused))
