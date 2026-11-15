"""ThimsyHotkeyController's key drivers (PhoneWindowManager's universal hotkeys), on a virtual clock.

A hotkey acts on the chord's press: the injected key goes down at once. A tapped key stays down for
TAP_HOLD_MS, so an emulator that reads its key state once a frame sees the press (a down and an up
sent together can land in one poll and never register).
"""
import json
import os
import shutil
import tempfile

from tests import util

POLICY = os.path.join(util.AOSP, "frameworks/base/services/core/java/com/android/server/policy")
ORG_JSON = os.path.join(util.AOSP, "libcore/json/src/main/java/org/json")
FIXTURES = os.path.join(util.TESTS, "fixtures/hotkeys")
HOLD = 100   # ThimsyHotkeyController.TAP_HOLD_MS

F2, F4, R, F, Q, SPACE, CTRL = 132, 134, 46, 34, 45, 62, 113
META_CTRL = 0x1000 | 0x2000

# The annotations libcore's org.json carries, so it compiles on its own.
TYPE_USE = ("@java.lang.annotation.Target({java.lang.annotation.ElementType.TYPE_USE, "
            "java.lang.annotation.ElementType.PARAMETER, java.lang.annotation.ElementType.METHOD, "
            "java.lang.annotation.ElementType.FIELD})\n")
ANNOTATIONS = {
    "android/annotation/SystemApi.java":
        "package android.annotation;\n"
        "public @interface SystemApi { enum Client { PRIVILEGED_APPS, MODULE_LIBRARIES, SYSTEM_SERVER }\n"
        "  Client client() default Client.PRIVILEGED_APPS; }\n",
    "android/compat/annotation/UnsupportedAppUsage.java":
        "package android.compat.annotation;\n"
        "public @interface UnsupportedAppUsage { int maxTargetSdk() default 0; long trackingBug() default 0;\n"
        "  String implicitMember() default \"\"; String publicAlternatives() default \"\"; }\n",
    "libcore/util/NonNull.java": "package libcore.util;\n" + TYPE_USE + "public @interface NonNull {}\n",
    "libcore/util/Nullable.java": "package libcore.util;\n" + TYPE_USE + "public @interface Nullable {}\n",
}


def ev(at, key, down, meta=0):
    return {"at": at, "key": key, "down": down, "meta": meta}


class KeyDrivers(util.TestCase):
    classes = None

    @classmethod
    def setUpClass(cls):
        cls.build_error = None
        if not (shutil.which("javac") and shutil.which("java")):
            cls.build_error = "javac isn't on PATH"
            return
        if not (os.path.isfile(os.path.join(POLICY, "ThimsyHotkeyController.java")) and os.path.isdir(ORG_JSON)):
            cls.build_error = f"no AOSP tree at {util.AOSP}"
            return
        cls.work = tempfile.mkdtemp(prefix="thimsy-hotkeys-")
        src = os.path.join(cls.work, "src")
        for rel, text in ANNOTATIONS.items():
            os.makedirs(os.path.dirname(os.path.join(src, rel)), exist_ok=True)
            with open(os.path.join(src, rel), "w") as f:
                f.write(text)
        sources = [os.path.join(src, rel) for rel in ANNOTATIONS]
        for top, _dirs, files in os.walk(os.path.join(FIXTURES, "stubs")):
            sources += [os.path.join(top, n) for n in sorted(files) if n.endswith(".java")]
        sources += [os.path.join(ORG_JSON, n) for n in sorted(os.listdir(ORG_JSON)) if n.endswith(".java")]
        sources += [os.path.join(POLICY, "ThimsyHotkeyController.java"),
                    os.path.join(FIXTURES, "HotkeyControllerCheck.java")]
        cls.classes = os.path.join(cls.work, "classes")
        os.makedirs(cls.classes)
        proc = util.run(["javac", "-nowarn", "-encoding", "UTF-8", "-d", cls.classes, *sources], timeout=300)
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

    def drive(self, steps, until):
        proc = self.assertOk(util.run(["java", "-cp", self.classes, "com.android.server.policy.HotkeyControllerCheck"],
                                      input=json.dumps({"steps": steps, "until": until})))
        return json.loads(proc.stdout)

    def test_hold_is_the_controllers(self):
        text = util.slurp(os.path.join(POLICY, "ThimsyHotkeyController.java"), text=True)
        self.assertIn(f"static final long TAP_HOLD_MS = {HOLD};", text)

    def test_tap_goes_down_at_once_and_up_after_the_hold(self):
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}}], 1000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(HOLD, F2, False)])
        self.assertFalse(out["holding"])

    def test_tap_with_a_modifier_wraps_the_key(self):
        out = self.drive([{"at": 5, "run": {"driver": "key", "key": "F", "mods": ["CTRL"]}}], 1000)
        self.assertEqual(out["events"], [ev(5, CTRL, True, META_CTRL), ev(5, F, True, META_CTRL),
                                         ev(5 + HOLD, F, False, META_CTRL), ev(5 + HOLD, CTRL, False)])

    def test_same_key_again_lets_the_first_go_first(self):
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}},
                          {"at": 30, "run": {"driver": "key", "key": "F2"}}], 1000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(30, F2, False), ev(30, F2, True),
                                         ev(30 + HOLD, F2, False)])

    def test_two_keys_overlap(self):
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}},
                          {"at": 40, "run": {"driver": "key", "key": "F4"}, "action": "load_state"}], 1000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(40, F4, True), ev(HOLD, F2, False),
                                         ev(40 + HOLD, F4, False)])

    def test_reset_lets_a_tap_go_at_once(self):
        """The Quick Menu, quit, an app change or sleep: no key is left down for the app."""
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}}, {"at": 20, "reset": True}], 1000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(20, F2, False)])

    def test_exit_lets_a_tap_go_before_the_app_closes(self):
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}},
                          {"at": 20, "run": {"driver": "exit"}, "action": "quit"}], 1000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(20, F2, False)])

    def test_quit_tap_then_exit_sends_each_key_up_once(self):
        """ScummVM's quit: Ctrl+Q, then the exit if the app is still in front after timeout_ms."""
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "Q", "mods": ["CTRL"], "then": "exit",
                                            "timeout_ms": 3000}, "action": "quit"}], 5000)
        self.assertEqual(out["events"], [ev(0, CTRL, True, META_CTRL), ev(0, Q, True, META_CTRL),
                                         ev(HOLD, Q, False, META_CTRL), ev(HOLD, CTRL, False)])

    def test_toggle_tap_is_held_too(self):
        out = self.drive([{"at": 0, "run": {"driver": "key_toggle", "key": "SPACE"}, "action": "fast_forward"}], 1000)
        self.assertEqual(out["events"], [ev(0, SPACE, True), ev(HOLD, SPACE, False)])

    def test_key_hold_still_holds_until_toggled_off(self):
        spec = {"driver": "key_hold", "key": "R"}
        out = self.drive([{"at": 0, "run": spec, "action": "rewind"}], 5000)
        self.assertEqual(out["events"], [ev(0, R, True)])
        self.assertTrue(out["holding"])
        out = self.drive([{"at": 0, "run": spec, "action": "rewind"}, {"at": 900, "run": spec, "action": "rewind"}], 5000)
        self.assertEqual(out["events"], [ev(0, R, True), ev(900, R, False)])
        self.assertFalse(out["holding"])

    def test_a_tap_does_not_let_go_of_a_held_key(self):
        out = self.drive([{"at": 0, "run": {"driver": "key_hold", "key": "R"}, "action": "rewind"},
                          {"at": 50, "run": {"driver": "key", "key": "F2"}}], 5000)
        self.assertEqual(out["events"], [ev(0, R, True), ev(50, F2, True), ev(50 + HOLD, F2, False)])
        self.assertTrue(out["holding"])

    def test_a_new_hold_keeps_a_tap_held(self):
        out = self.drive([{"at": 0, "run": {"driver": "key", "key": "F2"}},
                          {"at": 10, "run": {"driver": "key_hold", "key": "R"}, "action": "rewind"}], 5000)
        self.assertEqual(out["events"], [ev(0, F2, True), ev(10, R, True), ev(HOLD, F2, False)])
