"""External Display > Audio Output: Built-in Speakers keeps a monitor's audio device unregistered,
so the sound stays on the device; Monitor, the default, plays it on the monitor as before
(GammaosMonitorAudio, used by AudioDeviceBroker's vendor HDMI paths). Runs GammaosMonitorAudioCheck
on the host, compiled from the files patches/thimsy/0001-thimsy.patch creates (what the release
ships, never a stale AOSP tree's copy), and checks that Nano's row, system_server and the translations agree on the setting.
"""
import json
import os
import re
import shutil
import tempfile

from tests import util
from tests.unit import test_screen_arrangement as arrangement

PATCH = arrangement.PATCH
REL_JAVA = "frameworks/base/services/core/java/com/android/server/audio/GammaosMonitorAudio.java"
REL_CHECK = "frameworks/base/cmds/gammaos-nano/test/java/GammaosMonitorAudioCheck.java"
REL_BROKER = "frameworks/base/services/core/java/com/android/server/audio/AudioDeviceBroker.java"
REL_MENU = "frameworks/base/cmds/gammaos-nano/NanoMenuPS3Menu.cpp"
REL_I18N = "frameworks/base/cmds/gammaos-nano/ps3xmb/i18n/"
# The strings the row shows, each translated in every language.
STRINGS = ("Audio Output", "Built-in Speakers")
# AOSP's own JDK, for a host without one on PATH.
PREBUILT_JDK = os.path.join(util.AOSP, "prebuilts", "jdk", "jdk17", "linux-x86", "bin")


def java_tools():
    """javac and java: from PATH, else AOSP's prebuilt JDK; None when neither has them."""
    for tool_dir in (None, PREBUILT_JDK):
        javac = shutil.which("javac", path=tool_dir)
        java = shutil.which("java", path=tool_dir)
        if javac and java:
            return javac, java
    return None


def sections(patch):
    """{path: the lines of its section} for each file the patch changes."""
    out, lines = {}, None
    for line in patch.split("\n"):
        m = re.match(r"diff --git a/(\S+) b/", line)
        if m:
            lines = out.setdefault(m.group(1), [])
        elif lines is not None:
            lines.append(line)
    return out


def added(section_lines):
    """The text the section adds, its + lines without the +, one per line."""
    return "\n".join(l[1:] for l in section_lines if l.startswith("+") and not l.startswith("+++"))


def java_constant(text, name):
    m = re.search(r'static final String %s = "([^"]*)";' % name, text)
    return m.group(1) if m else None


class MonitorAudio(util.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patch = util.slurp(PATCH, text=True)
        cls.sections = sections(cls.patch)

    def test_check_passes(self):
        tools = java_tools()
        if not tools:
            self.skipTest("javac isn't on PATH or in the AOSP tree's prebuilts")
        javac, java = tools
        work = tempfile.mkdtemp(prefix="thimsy-monaudio-")
        try:
            sources = []
            for rel in (REL_JAVA, REL_CHECK):
                text = arrangement.new_file(self.patch, rel)
                self.assertIsNotNone(text, f"the patch doesn't create {rel}")
                sources.append(os.path.join(work, os.path.basename(rel)))
                with open(sources[-1], "w", encoding="utf-8") as f:
                    f.write(text)
            classes = os.path.join(work, "classes")
            self.assertOk(util.run([javac, "-nowarn", "-encoding", "UTF-8", "-d", classes]
                                   + sources, timeout=300))
            proc = self.assertOk(util.run([java, "-cp", classes,
                                           "com.android.server.audio.GammaosMonitorAudioCheck"]))
            self.assertIn("all passed", proc.stdout)
        finally:
            util.rmtree(work)

    def test_nano_row_writes_what_system_server_reads(self):
        java = arrangement.new_file(self.patch, REL_JAVA)
        self.assertIsNotNone(java, f"the patch doesn't create {REL_JAVA}")
        prop = java_constant(java, "PROP")
        monitor = java_constant(java, "MONITOR")
        speakers = java_constant(java, "SPEAKERS")
        self.assertEqual(prop, "persist.gammaos.display.audio")
        menu = added(self.sections[REL_MENU])
        m = re.search(r'\{"Audio Output", SettingSource::kProp, "([^"]+)", "([^"]*)",\s*"([^"]+)"\}',
                      menu)
        self.assertIsNotNone(m, "Nano has no Audio Output setting")
        self.assertEqual(m.group(1), prop)
        # Off by default: the monitor keeps the sound, as before.
        self.assertEqual(m.group(2), monitor)
        self.assertEqual(m.group(3), f"{monitor}:Monitor,{speakers}:Built-in Speakers")
        # The row is in the External Display list, after Controller Screen.
        body = menu[menu.index("void NanoMenu::buildExternalDisplaySubmenu("):]
        body = body[:body.index("\n}\n")]
        self.assertRegex(body, r'gpLeaf\(out, "Controller Screen".*\n(.*\n)?\s*gpLeaf\(out, '
                               r'"Audio Output", nullptr, \d+\);\n\s*out\.items\.back\(\)\.desc = "')
        # A rebuilt list keeps the cursor on Audio Output, not on the first row that isn't a mode.
        self.assertIn("it.kind != PS3_QUICK && it.label == keepLabel", menu)

    def test_every_vendor_path_asks_the_setting(self):
        broker = added(self.sections[REL_BROKER])
        # UniSoc (the RG Cube's DisplayPort), Allwinner and Rockchip.
        self.assertIn("gammaMonitorAudio(hasUnisocHdmiAudioEvidence())", broker)
        self.assertIn("gammaMonitorAudio(hasAllwinnerHdmiAudioEvidence())", broker)
        self.assertIn("gammaMonitorAudio(dpPresent || hdmiPresent)", broker)
        self.assertIn("GammaosMonitorAudio.register(present, mGammaMonitorAudio)", broker)
        # Read again while a monitor is in, so a change moves the sound without a reconnect.
        self.assertIn("GammaosMonitorAudio.changed(mGammaMonitorAudio", broker)
        self.assertIn("SystemProperties.get(GammaosMonitorAudio.PROP", broker)
        # The old evidence calls are gone, so no path registers the monitor regardless.
        removed = "\n".join(l for l in self.sections[REL_BROKER] if l.startswith("-"))
        for call in ("final boolean evidence = hasUnisocHdmiAudioEvidence();",
                     "final boolean evidence = hasAllwinnerHdmiAudioEvidence();",
                     "final boolean dpEvidence = hasRockchipDpAudioEvidence();"):
            self.assertIn(call, removed)

    def test_translated(self):
        langs = [p for p in self.sections if p.startswith(REL_I18N)]
        self.assertGreaterEqual(len(langs), 20)
        menu = added(self.sections[REL_MENU])
        desc = re.search(r'gpLeaf\(out, "Audio Output", nullptr, \d+\);\n\s*'
                         r'out\.items\.back\(\)\.desc = "([^"]+)";', menu).group(1)
        for rel in langs:
            text = added(self.sections[rel])
            for key in STRINGS + (desc,):
                m = re.search(r'^ %s: ("(?:[^"\\]|\\.)*"),?$' % re.escape(json.dumps(key, ensure_ascii=False)),
                              text, re.M)
                self.assertIsNotNone(m, f"{rel} doesn't translate {key!r}")
                self.assertTrue(json.loads(m.group(1)).strip(), f"{rel}: {key!r} is empty")
