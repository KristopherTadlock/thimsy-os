"""The readers on their own: options, package.mk evaluation, YAML, quirks, links, schema."""

from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _common as c  # noqa: E402
from thimsy_import import devicejson, inputplumber, linuxmk, options, quirks, rocknix  # noqa: E402
from thimsy_import.rocknix import ImportError_  # noqa: E402

MK = (c.FIXTURE / "projects/ROCKNIX/packages/linux/package.mk").read_text(encoding="utf-8")


class OptionsTest(unittest.TestCase):
    def test_real_rk3566_options(self):
        o = options.Options().read((c.FIXTURE / "projects/ROCKNIX/devices/RK3566/options")
                                   .read_text(encoding="utf-8"), "options")
        self.assertEqual(o.get("TARGET_CPU"), "cortex-a55")       # the aarch64 branch, not arm
        self.assertIsNone(o.get("TARGET_FPU"))                    # only in the arm branch
        self.assertEqual(o.get("FIRMWARE"), "extra-firmware")
        self.assertEqual(o.get("HW_CPU"), "Rockchip RK3566")
        self.assertIsNone(o.get("KERNEL_MAKE_EXTRACMD"))          # dynamic: never expanded
        self.assertIn("KERNEL_MAKE_EXTRACMD", o.dynamic)

    def test_append(self):
        o = options.Options().read('UBOOT_FIRMWARE="rkbin"\nUBOOT_FIRMWARE+=" atf"\n', "x")
        self.assertEqual(o.get("UBOOT_FIRMWARE"), "rkbin atf")


class LinuxMkTest(unittest.TestCase):
    def setUp(self):
        self.mk = linuxmk.LinuxMk(MK, "package.mk")

    def test_quoted_regex_is_literal(self):
        env = {"DEVICE": "H700"}
        self.assertFalse(linuxmk.eval_test('[[ "${DEVICE}" =~ "RK3326|RK3399|S922X|H700" ]]', env))
        self.assertTrue(linuxmk.eval_test('[[ "${DEVICE}" =~ RK3326|H700 ]]', env))
        self.assertTrue(linuxmk.eval_test('[[ "${DEVICE}" == H7* ]]', env))
        self.assertFalse(linuxmk.eval_test('[[ "${DEVICE}" == "H7*" ]]', env))

    def test_patch_dirs_and_version(self):
        self.assertEqual(self.mk.patch_dirs("RK3566", ""), ["mainline", "RK3566", "default", "mainline-rockchip", "7.3"])
        self.assertEqual(self.mk.patch_dirs("H700", ""), ["mainline", "H700", "default"])
        self.assertEqual(self.mk.patch_dirs("RK3588", "x"), ["x", "RK3588", "default"])
        v = self.mk.version("RK3566")
        self.assertEqual(v["url"], "https://www.kernel.org/pub/linux/kernel/v7.x/linux-7.3.5.tar.xz")
        self.assertEqual(self.mk.version("RK3588")["url"],
                         "https://github.com/armbian/linux-rockchip/archive/b8e62bed74766b6c8c423a767b35495e78b64caf.tar.gz")

    def test_family_without_branch(self):
        with self.assertRaises(ImportError_):
            self.mk.version("S922X")

    def test_extra_firmware_block(self):
        self.assertIsNone(self.mk.extra_firmware_block("RK3566"))
        self.assertIn('"H700"', self.mk.extra_firmware_block("H700").lines[0])


class YamlTest(unittest.TestCase):
    def test_subset(self):
        doc = inputplumber.load_yaml(
            "a: 1\nb:\n  - x: \"q\"\n    y: [1, 2]\n  - plain\nc:\n  d: true  # c\nempty: []\n")
        self.assertEqual(json.loads(json.dumps(doc)),
                         {"a": 1, "b": [{"x": "q", "y": [1, 2]}, "plain"], "c": {"d": True}, "empty": []})

    def test_refuses_anchors(self):
        with self.assertRaises(ImportError_):
            inputplumber.load_yaml("a: &x 1\n")


class QuirksTest(unittest.TestCase):
    def test_facts_only_from_profile_heredocs(self):
        text = ("#!/bin/sh\ncat <<EOF >/storage/.config/profile.d/020-gpios\nDEVICE_PWM_MOTOR=\"pwmchip1\"\n"
                "BAD=\"$(id)\"\nEOF\necho 1 > /sys/x\ncat <<EOF >/storage/x.ini\nNOT_A_FACT=1\nEOF\n")
        facts, rejected = quirks.read_facts(text)
        self.assertEqual(dict(facts), {"DEVICE_PWM_MOTOR": "pwmchip1"})
        self.assertEqual(rejected, ['BAD="$(id)"'])

    def test_text_link_followed(self):
        tree = rocknix.RocknixTree(str(c.FIXTURE), "next", c.COMMIT)
        qf = quirks.for_device(tree, "RK3566", "Anbernic RG ARC-S")
        self.assertEqual(qf.facts.get("DEVICE_PWM_MOTOR"), "pwmchip1")
        self.assertIn("projects/ROCKNIX/packages/hardware/quirks/devices/Anbernic RG ARC-S/020-gpios", qf.sources)


class SchemaTest(unittest.TestCase):
    BOARDS = ["board/rk3566/devices/anbernic-rg-arc-d.json", "board/rk3566/devices/anbernic-rg-arc-s.json",
              "board/qemu-aarch64-virt/devices/qemu-virt.json"]

    def test_board_files_valid(self):
        for rel in self.BOARDS:
            doc = json.loads((c.LINUX / rel).read_text(encoding="utf-8"))
            self.assertEqual(devicejson.check(doc), [], rel)

    def test_thimsy_prop_fields(self):
        """modules, product and props: the fields thimsy-prop boot reads (libthimsy/tools/thimsy-prop.cpp)."""
        doc = json.loads((c.LINUX / self.BOARDS[0]).read_text(encoding="utf-8"))
        ok = json.loads(json.dumps(doc))
        ok["modules"] = ["joypad", "rocknix-singleadc-joypad"]
        ok["product"] = {"manufacturer": "Anbernic", "model": "RG ARC-D", "platform": "rk3566"}
        ok["props"] = {"ro.gammaos.device": "arc-d", "ro.thimsy.audio.volume_control": "Master", "ro.x.n": 3}
        self.assertEqual(devicejson.check(ok), [])
        for field, value in (("modules", ["bad name"]), ("modules", "joypad"), ("modules", ["a", "a"]),
                             ("product", {"serial": "x"}), ("product", {"model": ""}),
                             ("props", {"sys.x": "1"}), ("props", {"ro.x": ["list"]})):
            bad = json.loads(json.dumps(doc))
            bad[field] = value
            self.assertTrue(devicejson.check(bad), "%s=%r accepted" % (field, value))

    def test_qemu_enter_is_south(self):
        doc = json.loads((c.LINUX / self.BOARDS[2]).read_text(encoding="utf-8"))
        self.assertIn({"source": "virtio-keyboard", "code": "KEY_ENTER", "to": "BTN_SOUTH"}, doc["pad"]["buttons"])

    def test_bad_pads_rejected(self):
        doc = json.loads((c.LINUX / self.BOARDS[0]).read_text(encoding="utf-8"))
        bad = json.loads(json.dumps(doc))
        bad["pad"]["buttons"][0]["to"] = "BTN_TRIGGER_HAPPY1"
        self.assertTrue(devicejson.check(bad))
        bad = json.loads(json.dumps(doc))
        bad["pad"]["buttons"][0]["source"] = "nope"
        self.assertTrue(any("unknown source" in e for e in devicejson.check(bad)))
        bad = json.loads(json.dumps(doc))
        bad["pad"]["face_buttons"] = 5
        self.assertTrue(devicejson.check(bad))
        bad = json.loads(json.dumps(doc))
        bad["quirk_facts"] = {"X": "$(reboot)"}
        self.assertTrue(devicejson.check(bad))
        bad = json.loads(json.dumps(doc))
        bad["pad"]["sticks"] = [{"name": "left", "source": "gpio-keys-control",
                                 "x": {"code": "ABS_X", "to": "ABS_Z"}, "y": {"code": "ABS_Y", "to": "ABS_Y"}}]
        self.assertTrue(devicejson.check(bad))

    def test_generated_matches_hand_written_pad_facts(self):
        """The importer's ARC facts agree with the hand-written board file where both speak."""
        with c.TempDir() as tmp:
            self.assertEqual(c.run(tmp, "RK3566")[0], 0)
            gen = json.loads((tmp / "generated/rk3566/devices/anbernic-rg-arc-d.json").read_text(encoding="utf-8"))
        hand = json.loads((c.LINUX / self.BOARDS[0]).read_text(encoding="utf-8"))
        self.assertEqual(gen["model"], hand["model"])
        gmap = {(b["source"], b["code"]): b["to"] for b in gen["pad"]["buttons"] if b["source"] == "gpio-keys-control"}
        for b in hand["pad"]["buttons"]:
            if b["source"] == "gpio-keys-control":
                self.assertEqual(gmap[(b["source"], b["code"])], b["to"], b["code"])
        self.assertEqual(gen["pad"]["face_buttons"], hand["pad"]["face_buttons"])
        self.assertEqual(gen["vibrator"], hand["vibrator"])

    def test_merge(self):
        m = devicejson.merge({"a": {"b": 1, "c": [1]}, "d": 1}, {"a": {"c": [2]}, "e": 2})
        self.assertEqual(json.loads(json.dumps(m)), {"a": {"b": 1, "c": [2]}, "d": 1, "e": 2})


if __name__ == "__main__":
    unittest.main()
