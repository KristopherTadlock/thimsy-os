"""4K60 as YCbCr 4:2:0: SurfaceFlinger's NV12 client target (ThimsyYuv420 in CompositionEngine).
The SurfaceFlinger files patches/thimsy/0001-thimsy.patch makes must be, byte for byte, the ones
test 42's T3 ran (surfaceflinger 65438efb..., 4K60 with correct colours on the RG Cube). The code
keeps it off unless ro.gammaos.display.y420 is set, which mods/52-monitor-4k.sh does, with
ro.gammaos.display.uhd, the property system_server lists 3840-wide modes by.
"""
import hashlib
import re
import sys

from tests import util
from tests.unit import test_monitor_audio as audio
from tests.unit import test_screen_arrangement as arrangement

PATCH = arrangement.PATCH
MOD = util.MODS + "/52-monitor-4k.sh"
SF = "frameworks/native/services/surfaceflinger/"
CE = SF + "CompositionEngine/"
# Every SurfaceFlinger file the patch changes or adds, and its git blob sha1 after the patch: the
# sources of T3's surfaceflinger (65438efb18e2b352...), from the AOSP tree it was built in.
T3_BLOBS = {
    CE + "Android.bp": "87420dd7c652b33ef89a63299e252888365bc207",
    CE + "include/compositionengine/LayerFECompositionState.h": "fb7dd4d3b6285b5a5196b4d930a8af743ca6334c",
    CE + "include/compositionengine/Output.h": "f25ac78e84f6a9b4e4a23a1143fdc03e5568d70d",
    CE + "include/compositionengine/RenderSurface.h": "06e72dbbed14b8eb0d776211a6cf0a088e957e8b",
    CE + "include/compositionengine/ThimsyYuv420.h": "4fefeb7a979bc31addb5c94bdafde2bb8f435242",
    CE + "include/compositionengine/impl/Display.h": "64f45266cbc076565b8ec3137f6e92d1f39adefd",
    CE + "include/compositionengine/impl/Output.h": "a966f05ba2bbf9cd0d8dc42fa86d24ed17556580",
    CE + "include/compositionengine/impl/OutputCompositionState.h": "2eefc7bdade56e8980207ef996865435b37ca851",
    CE + "include/compositionengine/impl/RenderSurface.h": "90b4c65e4452223f0777ff2b148879b23dc3f9ee",
    CE + "include/compositionengine/impl/ThimsyYuv420Converter.h": "d6fe55d1f01b602027e9b47e6e40e9c3c51609f6",
    CE + "include/compositionengine/mock/Output.h": "e4ef350ccc055121d81812937675ea64fabd8b0f",
    CE + "include/compositionengine/mock/RenderSurface.h": "9fd40afa9e639ad938abbaa040bcee7493237e7e",
    CE + "src/ClientCompositionRequestCache.cpp": "834ea38e8f9ccc503ffae1268a6c983bd70e3c79",
    CE + "src/Display.cpp": "04812df36f3ca596156873a168dd57d3fd296a86",
    CE + "src/LayerFECompositionState.cpp": "b14e655533b1e12059e47e434e44e6f4c66d300a",
    CE + "src/Output.cpp": "9bec0b39d2b32a6ce6613bf8b12a76913b733b2e",
    CE + "src/OutputCompositionState.cpp": "899ad5d38247bdca5a6a28f47a2cd8928b1e5e1a",
    CE + "src/OutputLayer.cpp": "16ab52162c2a748c4da200f1ab1992adcf8e4022",
    CE + "src/RenderSurface.cpp": "4aec5911ba1ac9fcd235160e7a6e5e5bc43f50c9",
    CE + "src/ThimsyYuv420Converter.cpp": "e0ee5bbe10cf813b94a35e34adf8bac025ed9bcb",
    SF + "FrontEnd/LayerSnapshotBuilder.cpp": "6bd426547dea74453c694bed12efc6229e39c568",
    SF + "Layer.cpp": "39e3f146d7a12b8f5d1212ac38942d7affdac3e0",
    SF + "LayerFE.cpp": "9ee870ef5d6dcd1d774f99442157419ed6b2c1f4",
    SF + "ScreenCaptureRenderSurface.h": "3ff8f36e7173a6c8b4b9a49ddf13ff89817cab24",
    SF + "SurfaceFlinger.cpp": "2aa65f66fb3a79e969f884256017af4041fd27b6",
    SF + "SurfaceFlinger.h": "5f33d9e5ba23846f6fce9199fcded5a5472fc932",
}
NEW_FILES = (CE + "include/compositionengine/ThimsyYuv420.h",
             CE + "include/compositionengine/impl/ThimsyYuv420Converter.h",
             CE + "src/ThimsyYuv420Converter.cpp")
REL_ARRANGEMENT = "frameworks/base/services/core/java/com/android/server/display/GammaosScreenArrangement.java"


def blob_sha(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


class SurfaceFlingerYuv420(util.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patch = util.slurp(PATCH, text=True)
        cls.sections = audio.sections(cls.patch)

    def test_files_are_t3s(self):
        """The patch's SurfaceFlinger files are T3's: no more, no fewer, each at T3's blob."""
        self.assertEqual(sorted(p for p in self.sections if p.startswith(SF)), sorted(T3_BLOBS))
        for path, blob in T3_BLOBS.items():
            with self.subTest(path=path):
                index = next(l for l in self.sections[path] if l.startswith("index "))
                post = re.match(r"index [0-9a-f]+\.\.([0-9a-f]+)( 100644)?$", index).group(1)
                self.assertTrue(blob.startswith(post), f"{path}: {index}, T3 has {blob}")
        for path in NEW_FILES:
            with self.subTest(path=path):
                text = arrangement.new_file(self.patch, path)
                self.assertIsNotNone(text, f"the patch doesn't create {path}")
                self.assertEqual(blob_sha(text.encode("utf-8")), T3_BLOBS[path])

    def test_built_with_the_new_source(self):
        bp = audio.added(self.sections[CE + "Android.bp"])
        self.assertIn('"src/ThimsyYuv420Converter.cpp"', bp)
        for lib in ('"libEGL"', '"libGLESv2"', '"libsync"'):
            self.assertIn(lib, bp)

    def test_off_unless_the_build_sets_it(self):
        """The code's default is off, and only a 3840x2160 monitor mode at 48 Hz or more turns it on."""
        sf = audio.added(self.sections[SF + "SurfaceFlinger.cpp"])
        self.assertIn('base::GetBoolProperty("ro.gammaos.display.y420"s, false)', sf)
        self.assertIn("if (!kBuild) return ThimsyYuv420::Off;", sf)
        self.assertIn("!gammaIsMonitor(*idOpt)) return ThimsyYuv420::Off;", sf)
        self.assertIn("modePtr->getWidth() != 3840 || modePtr->getHeight() != 2160", sf)
        self.assertIn("getPeriodNsecs() > 21'000'000", sf)

    def test_the_build_turns_it_on(self):
        """mods/52-monitor-4k.sh sets the property SurfaceFlinger reads and the one system_server
        lists 3840-wide modes by (GammaosScreenArrangement.UHD_PROP), and checks for the first."""
        mod = util.slurp(MOD, text=True)
        uhd = audio.java_constant(arrangement.new_file(self.patch, REL_ARRANGEMENT) or
                                  audio.added(self.sections[REL_ARRANGEMENT]), "UHD_PROP")
        self.assertEqual(uhd, "ro.gammaos.display.uhd")
        self.assertIn("\n%s=true\n" % uhd, mod)
        self.assertIn("\nro.gammaos.display.y420=true\n", mod)
        self.assertIn('grep -qaF "ro.gammaos.display.y420" "$SYSTEM/bin/surfaceflinger"', mod)
        self.assertIn('if [[ "${THIMSY_MONITOR_4K:-0}" != 1 ]]; then\n', mod)

    def test_only_with_the_driver_and_composer(self):
        """build.sh sets THIMSY_MONITOR_4K=1 (for mods/52-monitor-4k.sh) only with SYSTEM_IMG, a
        composer with CT4K (COMPOSER_FIX not 0) and a driver whose patch has Y420's mode."""
        text = util.slurp(util.ROOT + "/scripts/build.sh", text=True)
        self.assertIn('THIMSY_MONITOR_4K=0\nif [[ -n "${SYSTEM_IMG:-}" && "$COMPOSER_FIX" != 0 ]]; then\n', text)
        self.assertIn("\nexport THIMSY_MONITOR_4K\n", text)
        self.assertLess(text.index('COMPOSER_FIX="${COMPOSER_FIX:-$FIX_DEFAULT}"'), text.index("THIMSY_MONITOR_4K=0"))
        self.assertLess(text.index("export THIMSY_MONITOR_4K"), text.index("\nsudo -v\n"))
        code = re.search(r"THIMSY_MONITOR_4K=\"\$\(python3 -I -c '(.*?)' \"\$ROOT/scripts\"\)\"", text, re.S).group(1)
        drm = util.load_script("sprd_drm")
        want = int(bool(drm.UHD and drm.Y420 and drm.WIDE and drm.WIDE_MP))
        self.assertEqual(want, 1)   # test 42 ships 4K60 4:2:0: the default driver is MOD-Y1-E
        self.assertEqual(util.run([sys.executable, "-I", "-c", code, util.SCRIPTS]).stdout, "%d\n" % want)
        # The same check on a driver with UHD and Y420 on, and with them off.
        source = util.slurp(util.SCRIPTS + "/sprd_drm.py", text=True)
        for value, out in (("True", "1\n"), ("False", "0\n")):
            with self.subTest(y420=value):
                flipped, n = re.subn(r"(?m)^(UHD|Y420) = (?:True|False)\b", r"\1 = " + value, source)
                self.assertEqual(n, 2)
                self.write("drm-%s/sprd_drm.py" % value, flipped)
                self.assertEqual(util.run([sys.executable, "-I", "-c", code, self.path("drm-" + value)]).stdout, out)


if __name__ == "__main__":
    import unittest
    unittest.main()
