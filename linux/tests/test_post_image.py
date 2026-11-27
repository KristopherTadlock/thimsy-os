# SPDX-License-Identifier: MIT
"""The card assembly, post-image.d, run against fake tools (IMPLEMENTATION
D.4, E.6; step G8). Nothing here runs Buildroot or genimage.

board/common/post-image.sh runs its parts as Buildroot would, in a temporary
BINARIES_DIR, TARGET_DIR and tier tree, with fake mkfs.erofs, fsck.erofs,
mkfs.ext4 and genimage first on PATH and stand-ins for tools/fit.py and
tools/gpt_check.py (post_image_fake_tool.py); tools/card_image.py is the
real one, logged. The tests check the commands, their order, the per-device
outputs (info.vfat, OA-2; the cfg with the pinned GPT GUIDs, OA-9) and how
gpt_check is called; one runs the real gpt_check.py on GPT images the fake
genimage writes. The boot script (board/rk3566/uboot/boot.cmd) is checked
statically against fit.py.

The scripts run with BusyBox sh when it is installed, else /bin/sh; on
Windows with Git for Windows' sh.exe. THIMSY_SH overrides the shell.

    py -3.12 -I -m unittest discover -s tests -t . -p test_post_image.py   (from linux/)
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(LINUX / "tools"))

import card_image  # noqa: E402
import check_rcs  # noqa: E402
import fit  # noqa: E402
import gpt_check  # noqa: E402
import post_image_fake_tool as fake  # noqa: E402
import test_card_image  # noqa: E402

SHELLS = check_rcs.find_shells()
SH: Optional[List[str]] = SHELLS[0] if SHELLS else None
needs_sh = unittest.skipIf(SH is None, "no POSIX shell found")

RUNNER = LINUX / "board" / "common" / "post-image.sh"
ARC = ["anbernic-rg-arc-d", "anbernic-rg-arc-s"]
EPOCH = "1700000000"
RELEASE = "1.2.0-test"
PAD = 16320 * 512  # u-boot.itb's offset in u-boot-rockchip.bin
# The ARC-S (no Bluetooth) is made before the Bluetooth pack is unpacked for the ARC-D (OA-15);
# card_image.py: info per device (28-info.sh), then cfg before each genimage (30-genimage.sh).
TOOLS_ORDER_RK3566 = ["fsck.erofs", "fsck.erofs", "mkfs.erofs", "fsck.erofs", "mkfs.erofs",
                      "mkfs.ext4", "mkfs.ext4", "fit.py", "fit.py", "card_image.py", "card_image.py",
                      "card_image.py", "genimage", "card_image.py", "genimage",
                      "gpt_check.py", "gpt_check.py"]
BT_FILES = {"usr/libexec/bluetooth/bluetoothd": "btd\n", "usr/bin/dbus-daemon": "bus\n",
            "etc/init.d/deferred/D35bluetooth": "d35\n",
            "usr/share/thimsy/bluetooth.json": "{}\n"}


def posix(p: Path) -> str:
    return p.as_posix()


def same(a: str, b: Path) -> bool:
    return os.path.normcase(os.path.abspath(fake.native(a))) == os.path.normcase(str(b))


def write_lf(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def fake_erofs(path: Path, files: Dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fake.write_fake_erofs(str(path), files)


class Rig:
    """A hardware pack's output tree beside its tier's, and the fakes."""

    def __init__(self, family: str = "rk3566", tier: str = "thimsy_v8_0_release",
                 hw: str = "thimsy_rk3566_hw") -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="post-image-"))
        self.family = family
        self.base = self.tmp / ("output-" + hw)
        self.bin = self.base / "images"
        self.target = self.base / "target"
        self.tier = self.tmp / ("output-" + tier)
        self.log = self.tmp / "tools.log"
        self.fakebin = self.tmp / "fakebin"
        self.tools = self.tmp / "faketools"
        for d in (self.bin, self.target, self.fakebin, self.tools):
            d.mkdir(parents=True)
        py = posix(Path(sys.executable))
        tool = posix(HERE / "post_image_fake_tool.py")
        for name in ("mkfs.erofs", "fsck.erofs", "mkfs.ext4", "genimage"):
            p = self.fakebin / name
            write_lf(p, '#!/bin/sh\nexec "%s" "%s" %s "$@"\n' % (py, tool, name))
            os.chmod(str(p), 0o755)
        stub = ("import sys\nsys.path.insert(0, %r)\nimport post_image_fake_tool as t\n"
                "sys.exit(t.main([%%r] + sys.argv[1:]))\n" % str(HERE))
        write_lf(self.tools / "fit.py", stub % "fit.py")
        write_lf(self.tools / "gpt_check.py", stub % "gpt_check.py")
        # The real card_image.py, logged; it imports the real gpt_check.py beside it.
        write_lf(self.tools / "card_image.py",
                 "import runpy, sys\nsys.path.insert(0, %r)\n"
                 "import post_image_fake_tool as t\nt.log('card_image.py', sys.argv[1:])\n"
                 "sys.argv[0] = %r\nrunpy.run_path(sys.argv[0], run_name='__main__')\n"
                 % (str(HERE), str(LINUX / "tools" / "card_image.py")))
        # The tier: its root and its GPU packs.
        fake_erofs(self.tier / "images" / "rootfs.erofs", {
            "etc/os-release": "NAME=Thimsy\n", "usr/bin/thimsy-nano": "nano\n",
            "usr/share/thimsy/build.props": "ro.thimsy.build.type=dev\n"})
        fake_erofs(self.tier / "images" / "gpu-panfrost.erofs", {
            "usr/lib/dri/panfrost_dri.so": "panfrost\n"})
        fake_erofs(self.tier / "images" / "bluetooth.erofs", BT_FILES)
        # The hardware pack's target: modules and firmware.
        write_lf(self.target / "usr/lib/modules/6.12.0/kernel/x.ko", "ko\n")
        write_lf(self.target / "usr/lib/firmware/rtw88/fw.bin", "fw\n")
        write_lf(self.target / "usr/bin/busybox", "not taken\n")
        self.config = self.tmp / ".config"
        write_lf(self.config, 'BR2_TARGET_ROOTFS_EROFS_LZ4HC_LEVEL=9\n'
                              'BR2_TARGET_ROOTFS_EROFS_PCLUSTERSIZE=65536\n')
        self.env = dict(os.environ)
        self.env.update({
            "BR2_EXTERNAL_THIMSY_PATH": posix(LINUX),
            "BR2_CONFIG": posix(self.config),
            "BINARIES_DIR": posix(self.bin),
            "TARGET_DIR": posix(self.target),
            "BASE_DIR": posix(self.base),
            "SOURCE_DATE_EPOCH": EPOCH,
            "THIMSY_VERSION": RELEASE,
            "THIMSY_PYTHON": py,
            "THIMSY_TOOLS_DIR": posix(self.tools),
            "FAKE_LOG": str(self.log),
            "PATH": str(self.fakebin) + os.pathsep + os.environ.get("PATH", ""),
        })
        for k in ("THIMSY_TIER_DIR", "THIMSY_DEVICES", "THIMSY_BUILD_TARGET", "FAKE_FAIL",
                  "FAKE_GPT_RC", "FAKE_EROFS_BYTES", "FAKE_GENIMAGE_GPT"):
            self.env.pop(k, None)

    def kernel_and_dtbs(self, devices: List[str]) -> None:
        write_lf(self.bin / "Image", "kernel\n")
        for d in devices:
            write_lf(self.bin / ("rk3566-%s.dtb" % d), "dtb of %s\n" % d)

    def uboot_combined(self) -> bytes:
        idb = b"RKNS" + b"\x01" * 4000
        itb = b"\xd0\x0d\xfe\xed" + b"\x02" * 70000
        blob = idb.ljust(PAD, b"\0") + itb
        (self.bin / "u-boot-rockchip.bin").write_bytes(blob)
        return blob

    def run(self, *args: str, **env: str) -> "subprocess.CompletedProcess[str]":
        e = dict(self.env)
        e.update(env)
        assert SH is not None
        return subprocess.run(SH + [posix(RUNNER), posix(self.bin)] + list(args), env=e,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=600)

    def calls(self) -> List[Dict[str, Any]]:
        if not self.log.exists():
            return []
        with open(str(self.log), encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]

    def close(self) -> None:
        shutil.rmtree(str(self.tmp), ignore_errors=True)


def card_files(path: Path) -> Dict[str, str]:
    data = path.read_bytes()
    if data[1024:1028] == fake.EROFS_MAGIC:
        return json.loads(data[1028:].decode())["files"]
    return json.loads(data.decode())["files"]


@needs_sh
class Rk3566CardTest(unittest.TestCase):
    """One run for both ARC devices; every test reads its results."""

    rig: Rig
    uboot: bytes
    result: "subprocess.CompletedProcess[str]"

    @classmethod
    def setUpClass(cls) -> None:
        cls.rig = Rig()
        cls.rig.kernel_and_dtbs(ARC)
        cls.uboot = cls.rig.uboot_combined()
        cls.result = cls.rig.run("hw", "--family=rk3566")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.rig.close()

    def setUp(self) -> None:
        self.assertEqual(self.result.returncode, 0, self.result.stdout)

    def test_commands_order_and_outputs(self) -> None:
        out = self.result.stdout
        calls = self.rig.calls()
        self.assertEqual([c["tool"] for c in calls], TOOLS_ORDER_RK3566, out)
        # The runner's order: common 10, 20, the family's 22 and 25, common 28, 30, 40.
        steps = re.findall(r">>> thimsy post-image: (\S+)", out)
        self.assertEqual(steps, [
            "common/post-image.d/10-erofs.sh", "common/post-image.d/20-data.sh",
            "rk3566/post-image.d/22-uboot.sh", "rk3566/post-image.d/25-boot-img.sh",
            "common/post-image.d/28-info.sh", "common/post-image.d/30-genimage.sh",
            "common/post-image.d/40-gpt-check.sh"])
        bin_ = self.rig.bin
        cards = bin_ / "cards"
        for dev in ARC:
            self.assertTrue((bin_ / ("thimsy-%s.img" % dev)).is_file())
            for f in ("rootfs.erofs", "data.ext4", "boot.img", "idbloader.img", "u-boot.itb",
                      "info.vfat", "genimage.cfg"):
                self.assertTrue((cards / dev / f).is_file(), f)
        self.assertFalse((cards / ".root-stage").exists())
        self.assertFalse((cards / ARC[0] / "data-stage").exists())
        self.assertFalse((cards / ARC[0] / "genimage").exists())

    def test_tier_root_unpacked_with_gpu_pack(self) -> None:
        fsck = [c for c in self.rig.calls() if c["tool"] == "fsck.erofs"]
        stage = self.rig.bin / "cards" / ".root-stage"
        self.assertEqual(len(fsck), 3)
        for c, img in zip(fsck, ("rootfs.erofs", "gpu-panfrost.erofs", "bluetooth.erofs")):
            self.assertEqual(len(c["argv"]), 3, c)
            self.assertTrue(c["argv"][0].startswith("--extract="))
            self.assertTrue(same(c["argv"][0].split("=", 1)[1], stage))
            self.assertEqual(c["argv"][1], "--overwrite")
            self.assertTrue(same(c["argv"][2], self.rig.tier / "images" / img))

    def test_root_per_device(self) -> None:
        mk = [c for c in self.rig.calls() if c["tool"] == "mkfs.erofs"]
        for c, dev in zip(mk, list(reversed(ARC))):
            u = str(uuid.uuid5(uuid.NAMESPACE_URL, "thimsy:%s:root:%s" % (dev, EPOCH)))
            self.assertEqual(c["argv"][:5], ["-zlz4hc,9", "-C65536", "-T" + EPOCH, "-U" + u,
                                             "--all-root"])
            self.assertTrue(same(c["argv"][5], self.rig.bin / "cards" / dev / "rootfs.erofs"))
            files = card_files(self.rig.bin / "cards" / dev / "rootfs.erofs")
            self.assertEqual(json.loads(files["usr/share/thimsy/device.json"])["id"], dev)
            self.assertIn("usr/bin/thimsy-nano", files)              # the tier
            self.assertIn("usr/lib/dri/panfrost_dri.so", files)      # the GPU pack
            self.assertIn("usr/lib/modules/6.12.0/kernel/x.ko", files)
            self.assertIn("usr/lib/firmware/rtw88/fw.bin", files)
            self.assertNotIn("usr/bin/busybox", files)               # only modules and firmware
            self.assertIn("usr/share/thimsy/family/rk3566/hook", files)

    def test_bluetooth_only_where_the_device_has_it(self) -> None:
        """OA-15: the ARC-D (bluetooth: true) gets the tier's Bluetooth pack, the ARC-S none."""
        self.assertIn("Bluetooth pack", self.result.stdout)
        with_bt = card_files(self.rig.bin / "cards" / "anbernic-rg-arc-d" / "rootfs.erofs")
        without = card_files(self.rig.bin / "cards" / "anbernic-rg-arc-s" / "rootfs.erofs")
        for f in BT_FILES:
            self.assertIn(f, with_bt)
            self.assertNotIn(f, without)
        self.assertIn("usr/bin/thimsy-nano", without)

    def test_data_partition(self) -> None:
        mk = [c for c in self.rig.calls() if c["tool"] == "mkfs.ext4"]
        for c, dev in zip(mk, ARC):
            a = c["argv"]
            u = str(uuid.uuid5(uuid.NAMESPACE_URL, "thimsy:%s:data:%s" % (dev, EPOCH)))
            self.assertEqual(a[a.index("-U") + 1], u)
            self.assertEqual(a[a.index("-L") + 1], "thimsy-data")
            self.assertEqual(a[a.index("-m") + 1], "0")
            self.assertEqual(a[-1], "262144k")  # genimage.cfg: data is 256M
            self.assertTrue(same(a[-2], self.rig.bin / "cards" / dev / "data.ext4"))
            self.assertEqual(c["env"].get("E2FSPROGS_FAKE_TIME"), EPOCH)
            files = card_files(self.rig.bin / "cards" / dev / "data.ext4")
            self.assertEqual(sorted(files), ["thimsy/device.json"])
            self.assertEqual(json.loads(files["thimsy/device.json"])["id"], dev)

    def test_boot_img_per_device(self) -> None:
        fits = [c for c in self.rig.calls() if c["tool"] == "fit.py"]
        for c, dev in zip(fits, ARC):
            a = c["argv"]
            self.assertEqual(a[0], "build")
            self.assertEqual(a[a.index("--family") + 1], "rk3566")
            self.assertEqual(a[a.index("--device") + 1], dev)
            dtb = "rk3566-%s.dtb" % dev
            self.assertEqual(a[a.index("--dtb") + 1], dtb)
            self.assertTrue(same(a[a.index("--kernel") + 1], self.rig.bin / "Image"))
            self.assertTrue(same(a[a.index("--dtb-file") + 1], self.rig.bin / dtb))
            self.assertTrue(same(a[a.index("--out-dir") + 1], self.rig.bin / "cards" / dev / "fit"))
            img = (self.rig.bin / "cards" / dev / "boot.img").read_bytes()
            self.assertEqual(img[:8], b"THIMSFIT")

    def test_uboot_cut_and_genimage_inputs(self) -> None:
        gen = [c for c in self.rig.calls() if c["tool"] == "genimage"]
        digests = []
        for c, dev in zip(gen, ARC):
            a = c["argv"]
            # The device's cfg, with its pinned GUIDs (OA-9).
            self.assertTrue(same(a[a.index("--config") + 1],
                                 self.rig.bin / "cards" / dev / "genimage.cfg"))
            self.assertTrue(same(a[a.index("--inputpath") + 1], self.rig.bin / "cards" / dev))
            card = json.loads((self.rig.bin / ("thimsy-%s.img" % dev)).read_text())
            inputs = card["inputs"]
            self.assertEqual(sorted(inputs), sorted(["boot.img", "data.ext4", "idbloader.img",
                                                     "rootfs.erofs", "u-boot.itb",
                                                     "info.vfat"]))
            self.assertEqual(inputs["idbloader.img"]["size"], PAD)
            self.assertEqual(inputs["idbloader.img"]["sha256"],
                             hashlib.sha256(self.uboot[:PAD]).hexdigest())
            self.assertEqual(inputs["u-boot.itb"]["sha256"],
                             hashlib.sha256(self.uboot[PAD:]).hexdigest())
            digests.append({k: v["sha256"] for k, v in inputs.items()})
        for k in ("boot.img", "rootfs.erofs", "data.ext4", "info.vfat"):
            self.assertNotEqual(digests[0][k], digests[1][k], k)  # per device
        for k in ("idbloader.img", "u-boot.itb"):
            self.assertEqual(digests[0][k], digests[1][k], k)     # per family

    def test_gpt_check_invocation(self) -> None:
        checks = [c for c in self.rig.calls() if c["tool"] == "gpt_check.py"]
        self.assertEqual(len(checks), 2)
        for c, dev in zip(checks, ARC):
            a = c["argv"]
            self.assertTrue(same(a[0], self.rig.bin / ("thimsy-%s.img" % dev)))
            self.assertEqual(a[1:], ["--mode", "image", "--family", "rk3566", "--layout",
                                     "thimsy", "--release", RELEASE, "--device", dev,
                                     "--idbloader-size", str(PAD)])

    def test_pinned_cfg_per_device(self) -> None:
        """OA-9: 30-genimage.sh gives genimage the board's cfg with the GUIDs pinned for this
        release and device, and nothing else changed."""
        board = (LINUX / "board" / "rk3566" / "genimage.cfg").read_text(encoding="utf-8")
        pins = [c for c in self.rig.calls()
                if c["tool"] == "card_image.py" and c["argv"][0] == "cfg"]
        self.assertEqual(len(pins), 2)
        disks = set()
        for c, dev in zip(pins, ARC):
            a = c["argv"]
            self.assertEqual(a[a.index("--release") + 1], RELEASE)
            self.assertEqual(a[a.index("--device") + 1], dev)
            self.assertEqual(a[a.index("--family") + 1], "rk3566")
            text = (self.rig.bin / "cards" / dev / "genimage.cfg").read_text(encoding="utf-8")
            self.assertEqual(text, card_image.pin_cfg(board, RELEASE, dev))
            images, problems = gpt_check.genimage_layout(text)
            self.assertEqual(problems, [])
            disk = images[0]["hdimage"]["disk-uuid"]
            self.assertEqual(disk, gpt_check.pinned_guid(RELEASE, dev, "disk"))
            disks.add(disk)
            for e in images[0]["partitions"]:
                if e["in_table"]:
                    self.assertEqual(e["uuid"],
                                     gpt_check.pinned_guid(RELEASE, dev, "part:" + e["name"]))
        self.assertEqual(len(disks), 2)

    def test_info_fat_per_device(self) -> None:
        """OA-2: each device's info.vfat is the info partition's size, FAT12 THIMSY-INFO, with
        a README that names the device."""
        infos = [c for c in self.rig.calls()
                 if c["tool"] == "card_image.py" and c["argv"][0] == "info"]
        self.assertEqual(len(infos), 2)
        for c, dev in zip(infos, ARC):
            a = c["argv"]
            self.assertEqual(a[a.index("--epoch") + 1], EPOCH)
            self.assertEqual(a[a.index("--release") + 1], RELEASE)
            model = json.loads((LINUX / "board/rk3566/devices" / (dev + ".json"))
                               .read_text(encoding="utf-8"))["model"]
            self.assertEqual(a[a.index("--model") + 1], model)
            blob = (self.rig.bin / "cards" / dev / "info.vfat").read_bytes()
            self.assertEqual(len(blob), 1024 * 1024)
            fat = test_card_image.read_fat12(blob)
            self.assertEqual(fat["label"], "THIMSY-INFO")
            readme = fat["files"]["README.TXT"].decode("utf-8")
            self.assertIn("Thimsy OS Linux %s for the %s" % (RELEASE, model), readme)
            self.assertIn("Device id: %s" % dev, readme)


@needs_sh
class Rk3566OtherRunsTest(unittest.TestCase):
    """Runs that differ from the common one: one device each, to stay quick."""

    def setUp(self) -> None:
        self.rig = Rig()
        self.rig.kernel_and_dtbs(ARC)
        self.rig.uboot_combined()

    def tearDown(self) -> None:
        self.rig.close()

    def run1(self, *args: str, **env: str) -> "subprocess.CompletedProcess[str]":
        env.setdefault("THIMSY_DEVICES", "anbernic-rg-arc-d")
        return self.rig.run(*args, **env)

    def run_ok(self, *args: str, **env: str) -> str:
        r = self.run1(*args, **env)
        self.assertEqual(r.returncode, 0, r.stdout)
        return r.stdout

    def test_separate_uboot_files(self) -> None:
        (self.rig.bin / "u-boot-rockchip.bin").unlink()
        (self.rig.bin / "idbloader.img").write_bytes(b"RKNS" + b"\x03" * 1000)
        (self.rig.bin / "u-boot.itb").write_bytes(b"itb" * 100)
        self.run_ok("hw", "--family=rk3566")
        checks = [c for c in self.rig.calls() if c["tool"] == "gpt_check.py"]
        self.assertEqual(checks[0]["argv"][-2:], ["--idbloader-size", "1004"])

    def test_family_from_hw_word_and_tier_dir_override(self) -> None:
        # "hw rk3566" names the family for the common parts; the runner adds
        # board/rk3566/post-image.d only for --family=, so ask for both.
        other = self.rig.tmp / "elsewhere"
        shutil.copytree(str(self.rig.tier), str(other))
        shutil.rmtree(str(self.rig.tier))
        self.run_ok("hw", "rk3566", "--family=rk3566", "--tier-dir=" + posix(other))
        # One device (the ARC-D): root, GPU and Bluetooth packs unpacked, then one call of
        # each other tool, and card_image.py twice (info, cfg).
        self.assertEqual(len(self.rig.calls()), 10)

    def test_device_filter(self) -> None:
        self.run_ok("hw", "--family=rk3566", THIMSY_DEVICES="anbernic-rg-arc-s")
        self.assertTrue((self.rig.bin / "thimsy-anbernic-rg-arc-s.img").is_file())
        self.assertFalse((self.rig.bin / "thimsy-anbernic-rg-arc-d.img").exists())
        self.assertEqual(len([c for c in self.rig.calls() if c["tool"] == "genimage"]), 1)
        r = self.run1("hw", "--family=rk3566", THIMSY_DEVICES="no-such-device")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no-such-device", r.stdout)

    def test_no_bluetooth_pack_for_a_device_with_bluetooth(self) -> None:
        (self.rig.tier / "images" / "bluetooth.erofs").unlink()
        r = self.run1("hw", "--family=rk3566")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("anbernic-rg-arc-d has Bluetooth", r.stdout)
        self.assertNotIn("mkfs.erofs", [c["tool"] for c in self.rig.calls()])
        # A device without Bluetooth needs no pack.
        self.run_ok("hw", "--family=rk3566", THIMSY_DEVICES="anbernic-rg-arc-s")

    def test_bluetooth_not_a_boolean(self) -> None:
        board = self.rig.tmp / "linux"
        shutil.copytree(str(LINUX / "board"), str(board / "board"))
        shutil.copytree(str(LINUX / "configs"), str(board / "configs"))
        shutil.copytree(str(LINUX / "tools"), str(board / "tools"))
        js = board / "board/rk3566/devices/anbernic-rg-arc-d.json"
        d = json.loads(js.read_text(encoding="utf-8"))
        d["bluetooth"] = "yes"
        write_lf(js, json.dumps(d))
        r = subprocess.run(SH + [posix(board / "board/common/post-image.sh"), posix(self.rig.bin),
                                 "hw", "--family=rk3566"],
                           env=dict(self.rig.env, BR2_EXTERNAL_THIMSY_PATH=posix(board),
                                    THIMSY_DEVICES="anbernic-rg-arc-d"),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=600)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bluetooth is not true or false", r.stdout)

    def test_gpt_check_failure_fails_the_build(self) -> None:
        r = self.run1("hw", "--family=rk3566", FAKE_GPT_RC="1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("the GPT check failed for: anbernic-rg-arc-d", r.stdout)
        self.assertIn("40-gpt-check.sh failed", r.stdout)

    def test_root_larger_than_its_slot(self) -> None:
        r = self.run1("hw", "--family=rk3566", FAKE_EROFS_BYTES=str(1024 ** 3 + 4096))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("the root partition holds 1073741824", r.stdout)
        self.assertNotIn("genimage", [c["tool"] for c in self.rig.calls()])

    def test_tool_failure_stops_the_build(self) -> None:
        r = self.run1("hw", "--family=rk3566", FAKE_FAIL="genimage")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("genimage failed", r.stdout)
        self.assertNotIn("gpt_check.py", [c["tool"] for c in self.rig.calls()])

    def test_no_tier(self) -> None:
        shutil.rmtree(str(self.rig.tier))
        r = self.run1("hw", "--family=rk3566")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("build the tier first", r.stdout)
        self.assertEqual(self.rig.calls(), [])

    def test_no_uboot(self) -> None:
        (self.rig.bin / "u-boot-rockchip.bin").unlink()
        r = self.run1("hw", "--family=rk3566")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no U-Boot", r.stdout)

    def test_missing_dtb(self) -> None:
        (self.rig.bin / "rk3566-anbernic-rg-arc-d.dtb").unlink()
        r = self.run1("hw", "--family=rk3566")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no rk3566-anbernic-rg-arc-d.dtb", r.stdout)

    def test_rerun_is_clean(self) -> None:
        self.run_ok("hw", "--family=rk3566")
        first = (self.rig.bin / "thimsy-anbernic-rg-arc-d.img").read_bytes()
        cfg = (self.rig.bin / "cards/anbernic-rg-arc-d/genimage.cfg").read_bytes()
        self.run_ok("hw", "--family=rk3566")
        self.assertEqual((self.rig.bin / "thimsy-anbernic-rg-arc-d.img").read_bytes(), first)
        self.assertEqual((self.rig.bin / "cards/anbernic-rg-arc-d/genimage.cfg").read_bytes(),
                         cfg)

    def test_release_changes_the_guids(self) -> None:
        """OA-9: the GUIDs follow the release; with no THIMSY_VERSION and no build.props,
        card_release's last resort is 0.0.0-dev, as 20-build-props.sh's."""
        cfg = self.rig.bin / "cards/anbernic-rg-arc-d/genimage.cfg"
        self.run_ok("hw", "--family=rk3566", THIMSY_VERSION="9.9.9")
        self.assertIn(gpt_check.pinned_guid("9.9.9", "anbernic-rg-arc-d", "disk"),
                      cfg.read_text(encoding="utf-8"))
        write_lf(self.rig.target / "usr/share/thimsy/build.props",
                 "ro.thimsy.build.version=7.0.0\n")
        self.run_ok("hw", "--family=rk3566", THIMSY_VERSION="")
        self.assertIn(gpt_check.pinned_guid("7.0.0", "anbernic-rg-arc-d", "disk"),
                      cfg.read_text(encoding="utf-8"))
        (self.rig.target / "usr/share/thimsy/build.props").unlink()
        self.run_ok("hw", "--family=rk3566", THIMSY_VERSION="")
        self.assertIn(gpt_check.pinned_guid("0.0.0-dev", "anbernic-rg-arc-d", "disk"),
                      cfg.read_text(encoding="utf-8"))

    def test_bad_release_stops_the_build(self) -> None:
        r = self.run1("hw", "--family=rk3566", THIMSY_VERSION="1.0 beta")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bad release", r.stdout)
        self.assertNotIn("genimage", [c["tool"] for c in self.rig.calls()])


@needs_sh
class RealGptCheckTest(unittest.TestCase):
    """40-gpt-check.sh's arguments, given to the real gpt_check.py."""

    def setUp(self) -> None:
        self.rig = Rig()
        self.rig.kernel_and_dtbs(ARC)
        self.rig.uboot_combined()
        write_lf(self.rig.tools / "gpt_check.py",
                 "import sys\nsys.path.insert(0, %r)\nsys.path.insert(0, %r)\n"
                 "import post_image_fake_tool as t\nt.log('gpt_check.py', sys.argv[1:])\n"
                 "import gpt_check\nsys.exit(gpt_check.main(sys.argv[1:]))\n"
                 % (str(HERE), str(LINUX / "tools")))

    def tearDown(self) -> None:
        self.rig.close()

    def test_good_cards_pass(self) -> None:
        """The image carries the cfg's pinned GUIDs and info.vfat's FAT, and the real
        gpt_check.py, given the release and the device, passes it."""
        r = self.rig.run("hw", "--family=rk3566", FAKE_GENIMAGE_GPT="good",
                         THIMSY_DEVICES="anbernic-rg-arc-d")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertEqual(r.stdout.count(": ok"), 1, r.stdout)
        rep = gpt_check.check_image(str(self.rig.bin / "thimsy-anbernic-rg-arc-d.img"),
                                    family="rk3566")
        self.assertEqual(rep.info["gpt"]["disk_guid"],
                         gpt_check.pinned_guid(RELEASE, "anbernic-rg-arc-d", "disk"))
        self.assertEqual(rep.info["entries"][4]["filesystem"], "vfat")

    def test_other_release_fails(self) -> None:
        """A cfg pinned for another release than the one 40-gpt-check.sh checks."""
        cfg_tool = self.rig.tools / "card_image.py"
        write_lf(cfg_tool, cfg_tool.read_text().replace(
            "sys.argv[0] = ", "sys.argv = [a if a != %r else '1.1.0' for a in sys.argv]\n"
                              "sys.argv[0] = " % RELEASE, 1))
        r = self.rig.run("hw", "--family=rk3566", FAKE_GENIMAGE_GPT="good",
                         THIMSY_DEVICES="anbernic-rg-arc-d")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn("(OA-9)", r.stdout)

    def test_basic_data_entry_fails(self) -> None:
        r = self.rig.run("hw", "--family=rk3566", FAKE_GENIMAGE_GPT="basic",
                         THIMSY_DEVICES="anbernic-rg-arc-d")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn("FAIL", r.stdout)


@needs_sh
class QemuCardTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rig = Rig(family="qemu-aarch64-virt", tier="thimsy_v8_0",
                       hw="thimsy_qemu_aarch64_hw")
        write_lf(self.rig.bin / "Image", "kernel\n")

    def tearDown(self) -> None:
        self.rig.close()

    def test_qemu_card(self) -> None:
        r = self.rig.run("hw", "--family=qemu-aarch64-virt")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("no gpu-swrast.erofs in the tier", r.stdout)
        tools = [c["tool"] for c in self.rig.calls()]
        self.assertEqual(tools, ["fsck.erofs", "mkfs.erofs", "mkfs.ext4", "card_image.py",
                                 "card_image.py", "genimage", "gpt_check.py"])
        card = self.rig.bin / "thimsy-qemu-virt.img"
        self.assertEqual(sorted(json.loads(card.read_text())["inputs"]),
                         ["data.ext4", "info.vfat", "rootfs.erofs"])
        gpt = self.rig.calls()[-1]["argv"]
        self.assertTrue(same(gpt[0], card))
        self.assertEqual(gpt[1:], ["--mode", "image", "--family", "qemu-aarch64-virt",
                                   "--layout", "thimsy", "--release", RELEASE,
                                   "--device", "qemu-virt"])
        root = card_files(self.rig.bin / "cards" / "qemu-virt" / "rootfs.erofs")
        self.assertEqual(json.loads(root["usr/share/thimsy/device.json"])["id"], "qemu-virt")
        for f in BT_FILES:  # the virt machine has no Bluetooth (OA-15)
            self.assertNotIn(f, root)

    def test_explicit_tier_dir(self) -> None:
        moved = self.rig.tmp / "tier-elsewhere"
        shutil.move(str(self.rig.tier), str(moved))
        r = self.rig.run("hw", "--family=qemu-aarch64-virt", THIMSY_TIER_DIR=posix(moved))
        self.assertEqual(r.returncode, 0, r.stdout)


@needs_sh
class TierTreeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rig = Rig()

    def tearDown(self) -> None:
        self.rig.close()

    def test_tier_checks_its_root_only(self) -> None:
        fake_erofs(self.rig.bin / "rootfs.erofs", {"etc/os-release": "x\n"})
        r = self.rig.run("tier", "v8_0")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("tier v8_0: rootfs.erofs", r.stdout)
        self.assertIn("root slot 1073741824", r.stdout)
        self.assertIn("no card in a tier tree; skipped", r.stdout)
        self.assertEqual(self.rig.calls(), [])
        self.assertFalse((self.rig.bin / "cards").exists())

    def test_tier_root_not_erofs(self) -> None:
        write_lf(self.rig.bin / "rootfs.erofs", "not an image\n")
        r = self.rig.run("tier", "v8_0")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not an EROFS image", r.stdout)

    def test_tier_root_too_large(self) -> None:
        p = self.rig.bin / "rootfs.erofs"
        fake_erofs(p, {})
        with open(str(p), "r+b") as f:
            f.truncate(1024 ** 3 + 512)
        r = self.rig.run("tier", "v8_0")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("the smallest root slot is 1073741824", r.stdout)


@needs_sh
class CardLibTest(unittest.TestCase):
    """lib/card.sh's genimage.cfg readers on the boards' real files."""

    def sh(self, script: str) -> str:
        assert SH is not None
        lib = posix(LINUX / "board/common/post-image.d/lib/card.sh")
        r = subprocess.run(SH + ["-c", '. "$1"; ' + script, "sh", lib],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def test_parses(self) -> None:
        assert SH is not None
        r = subprocess.run(SH + ["-n", posix(LINUX / "board/common/post-image.d/lib/card.sh")],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_cfg_readers(self) -> None:
        for fam, out, images in (
                ("rk3566", "thimsy-rk3566.img",
                 ["idbloader.img", "u-boot.itb", "boot.img", "rootfs.erofs", "data.ext4",
                  "info.vfat"]),
                ("qemu-aarch64-virt", "thimsy-qemu-aarch64-virt.img",
                 ["rootfs.erofs", "data.ext4", "info.vfat"])):
            cfg = posix(LINUX / "board" / fam / "genimage.cfg")
            got = self.sh('for p in idbloader uboot boot root data info games; do '
                          'printf "%%s=%%s\\n" $p "$(card_part_bytes %s $p)"; done; '
                          'card_cfg_output %s; card_cfg_images %s' % (cfg, cfg, cfg)).split("\n")
            sizes = dict(line.split("=") for line in got[:7])
            if fam == "rk3566":
                self.assertEqual(sizes["idbloader"], str(8160 * 1024))
            self.assertEqual(sizes["uboot"], str(8 * 1024 ** 2))
            self.assertEqual(sizes["boot"], str(40 * 1024 ** 2))
            self.assertEqual(sizes["root"], str(1024 ** 3))
            self.assertEqual(sizes["data"], str(256 * 1024 ** 2))
            self.assertEqual(sizes["info"], str(1024 ** 2))
            self.assertEqual(sizes["games"], "")
            self.assertEqual(got[7], out)
            self.assertEqual([x for x in got[8:] if x], images)

    def test_card_release(self) -> None:
        """OA-9's release: THIMSY_VERSION, else TARGET_DIR's build.props, else 0.0.0-dev."""
        tmp = Path(tempfile.mkdtemp(prefix="card-release-"))
        self.addCleanup(shutil.rmtree, str(tmp), True)
        write_lf(tmp / "usr/share/thimsy/build.props",
                 "ro.thimsy.build.type=dev\nro.thimsy.build.version=2.0.0\n")
        t = posix(tmp)
        self.assertEqual(self.sh("THIMSY_VERSION=1.0.0 TARGET_DIR=%s card_release" % t),
                         "1.0.0\n")
        self.assertEqual(self.sh("THIMSY_VERSION= TARGET_DIR=%s card_release" % t), "2.0.0\n")
        self.assertEqual(self.sh("THIMSY_VERSION= TARGET_DIR=%s/none card_release" % t),
                         "0.0.0-dev\n")


class BootCmdTest(unittest.TestCase):
    """board/rk3566/uboot/boot.cmd against fit.py's facts (G2b-5 to G2b-7)."""

    def setUp(self) -> None:
        text = (LINUX / "board/rk3566/uboot/boot.cmd").read_text(encoding="utf-8")
        self.assertNotIn("\r", text)
        self.lines = [x.strip() for x in text.split("\n")
                      if x.strip() and not x.strip().startswith("#")]
        self.code = "\n".join(self.lines)
        self.fam = fit.FAMILIES["rk3566"]

    def test_addresses_and_magic(self) -> None:
        self.assertIn("setenv thimsy_hdr 0x%08x" % self.fam["header_load"], self.lines)
        self.assertIn("setenv thimsy_fit 0x%08x" % self.fam["fit_load"], self.lines)
        w0, w1 = fit.magic_words(self.fam["magic"])
        self.assertIn('test "${thimsy_m0}" = "%s"' % w0, self.code)
        self.assertIn('test "${thimsy_m1}" = "%s"' % w1, self.code)
        # The header: magic words at +0 and +4, the sector count at +8, and
        # the FIT from the sector after the header.
        self.assertIn("setexpr.l thimsy_m0 *${thimsy_hdr}", self.lines)
        self.assertIn("setexpr thimsy_a ${thimsy_hdr} + 4", self.lines)
        self.assertIn("setexpr thimsy_a ${thimsy_hdr} + 8", self.lines)
        self.assertIn("setexpr.l thimsy_n *${thimsy_a}", self.lines)
        self.assertIn("setexpr thimsy_fs ${thimsy_bs} + 1", self.lines)
        self.assertIn("mmc read ${thimsy_fit} ${thimsy_fs} ${thimsy_n}", self.code)
        # fit.py's header agrees: the count is u32 LE at +8.
        hdr = fit.make_header(1500, self.fam["magic"])
        self.assertEqual(struct.unpack_from("<I", hdr, 8)[0], 3)

    def test_bootargs_from_the_fit_and_root_partuuid(self) -> None:
        self.assertIn("fdt get value thimsy_conf /configurations default", self.lines)
        self.assertIn("fdt get value thimsy_args /configurations/${thimsy_conf} bootargs",
                      self.lines)
        self.assertIn("part uuid mmc ${thimsy_dev}:3 thimsy_root", self.code)
        self.assertIn('setenv bootargs "${thimsy_args} root=PARTUUID=${thimsy_root}"', self.lines)
        self.assertEqual(self.lines[-1], "bootm ${thimsy_fit}")
        # The FIT's configuration really carries the bootargs the script reads.
        its = fit.write_its(fit.resolve("rk3566", "anbernic-rg-arc-d",
                                        "rk3566-anbernic-rg-arc-d.dtb", None))
        self.assertIn('default = "conf";', its)
        self.assertIn('bootargs = "%s";' % self.fam["bootargs"], its)
        self.assertNotIn("root=", self.fam["bootargs"])

    def test_partition_by_name_on_one_card(self) -> None:
        self.assertIn("part start mmc ${thimsy_dev} boot thimsy_bs", self.code)
        self.assertNotRegex(self.code, r"mmc\s+(dev|read)\s+0\b")
        self.assertNotRegex(self.code, r"\bmmc 0\b")

    def test_lines_join_into_one_bootcmd(self) -> None:
        for line in self.lines:
            words = re.findall(r"[A-Za-z_]+", line)
            self.assertEqual(words.count("if"), words.count("fi"), line)
            self.assertEqual(words.count("then"), words.count("if"), line)
            self.assertNotIn("#", re.sub(r"\$\{[^}]*\}", "", line), line)
        joined = "; ".join(self.lines)
        self.assertNotIn("\n", joined)
        self.assertLess(len(joined), 4096)


if __name__ == "__main__":
    unittest.main()
