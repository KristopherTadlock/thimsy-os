#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
"""thimsy-session end to end, on fake hardware (IMPLEMENTATION F.1, "Host E2E").

The real binary runs as `thimsy-session --root R --fake-hw --tooldir T` against a tree that stands
in for /sys, /dev, /run, /etc, /usr and /storage: sysfs block and input entries, disk images as
/dev nodes (a GPT per card, exFAT, FAT32 and ext4 superblocks), FIFOs for the input nodes and for
uevents, and programs in T (Nano, padd, the scraper, mount, umount, fsck, amixer, poweroff) that
only log their arguments. The tests send keys and uevents, write config-store keys, and check the
files the session writes (mem_sleep, state, brightness, volumes.json, the keys) and the commands it
ran. No root is needed.

Run by `make e2e` (THIMSY_SESSION_BIN names the binary). Python 3.8+, standard library only.
"""

from __future__ import annotations

import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

BIN = os.environ.get("THIMSY_SESSION_BIN", "")

BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
LINUX_HOME = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915"
LINUX_RESERVED = "8DA63339-0007-60C0-C436-083AC8230908"
LINUX_ROOT_ARM64 = "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"

VFAT_OPTS = "utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush"
EXFAT_OPTS = "dirsync,noatime,nodev,nosuid,noexec"


# --- images ---------------------------------------------------------------------------------------

def guid_bytes(text: str) -> bytes:
    h = text.replace("-", "")
    b = bytes.fromhex(h)
    return b[3::-1] + b[5:3:-1] + b[7:5:-1] + b[8:]


def gpt_image(entries) -> bytes:
    """entries: list of (index, type, name)."""
    img = bytearray(64 * 512)
    struct.pack_into("<B", img, 446 + 4, 0xEE)
    struct.pack_into("<II", img, 446 + 8, 1, 0xFFFFFFFF)
    img[510:512] = b"\x55\xaa"
    arr = bytearray(128 * 128)
    for index, typ, name in entries:
        e = (index - 1) * 128
        arr[e:e + 16] = guid_bytes(typ)
        arr[e + 16:e + 32] = bytes([index]) * 16
        struct.pack_into("<QQ", arr, e + 32, 2048 * index, 2048 * index + 2047)
        n = name.encode("utf-16-le")
        arr[e + 56:e + 56 + len(n)] = n
    img[1024:1024 + len(arr)] = arr
    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<IIIIQQQQ", hdr, 8, 0x00010000, 92, 0, 0, 1, 0, 34, 0)
    hdr[56:72] = b"\x01" * 16
    struct.pack_into("<QIII", hdr, 72, 2, 128, 128, zlib.crc32(bytes(arr)) & 0xFFFFFFFF)
    struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)
    img[512:512 + 92] = hdr
    return bytes(img)


def exfat_image(serial: int, label: str) -> bytes:
    img = bytearray(64 * 512 + 8 * 4096)
    img[0] = 0xEB
    img[3:11] = b"EXFAT   "
    struct.pack_into("<I", img, 88, 64)
    struct.pack_into("<I", img, 96, 4)
    struct.pack_into("<I", img, 100, serial)
    img[108] = 9
    img[109] = 3
    img[510:512] = b"\x55\xaa"
    root = 64 * 512 + 2 * 4096
    img[root] = 0x81
    img[root + 32] = 0x83
    img[root + 33] = len(label)
    n = label.encode("utf-16-le")
    img[root + 34:root + 34 + len(n)] = n
    return bytes(img)


def fat32_image(serial: int, label: str) -> bytes:
    img = bytearray(1024 * 1024)
    img[0:3] = b"\xeb\x58\x90"
    img[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBH", img, 11, 512, 1, 32, 2, 0, 0, 0xF8, 0)
    struct.pack_into("<II", img, 32, 70000, 600)
    struct.pack_into("<I", img, 44, 2)
    img[66] = 0x29
    struct.pack_into("<I", img, 67, serial)
    img[71:82] = label.ljust(11).encode()
    img[82:90] = b"FAT32   "
    img[510:512] = b"\x55\xaa"
    struct.pack_into("<II", img, 32 * 512, 0x0FFFFFF8, 0x0FFFFFFF)  # FAT[1]: cleanly unmounted
    return bytes(img)


def ext4_image() -> bytes:
    img = bytearray(4096)
    struct.pack_into("<HH", img, 1024 + 56, 0xEF53, 1)
    img[1024 + 104:1024 + 120] = bytes(range(0x10, 0x20))
    img[1024 + 120:1024 + 127] = b"storage"
    return bytes(img)


# --- the fake root --------------------------------------------------------------------------------

TOOL = """#!/bin/sh
echo "$(basename "$0") $*" >> "$THIMSY_ROOT/commands.log"
"""

LONG_RUNNING = TOOL + "exec sleep 1000\n"

# Nano also records the GPU pack's environment it was given (gpu.json's "env") and its sound card.
NANO = TOOL + 'echo "nano-env GALLIUM_DRIVER=${GALLIUM_DRIVER-unset} ALSA_CARD=${ALSA_CARD-unset}" ' + \
    '>> "$THIMSY_ROOT/commands.log"\n' + \
    "exec sleep 1000\n"

# As thimsy-gpu-pack writes it for the swrast pack.
GPU_JSON = {"driver": "swrast", "env": {"GALLIUM_DRIVER": "softpipe"}, "files": ["usr/lib/libEGL.so.1"]}

DEVICE_JSON = {
    "id": "anbernic-rg-arc-d",
    "family": "rk3566",
    "power": {"mem_sleep": "deep"},
    "pad": {"hotkey": {"button": "BTN_MODE", "quit_combo": ["BTN_MODE", "BTN_START"]}},
    "audio": {"card": "rk817_ext", "card_index": 1},
}


class Root:
    def __init__(self, base: str):
        self.base = base
        self.root = os.path.join(base, "root")
        self.tools = os.path.join(base, "tools")
        os.makedirs(self.root)
        os.makedirs(self.tools)

    def p(self, rel: str) -> str:
        return self.root + rel

    def write(self, rel: str, data, mode: int = 0o644) -> None:
        path = self.p(rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb" if isinstance(data, bytes) else "w") as f:
            f.write(data)
        os.chmod(path, mode)

    def read(self, rel: str, default: str = "") -> str:
        try:
            with open(self.p(rel)) as f:
                return f.read()
        except OSError:
            return default

    def link(self, rel: str, target: str) -> None:
        path = self.p(rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        os.symlink(target, path)

    def fifo(self, rel: str) -> None:
        path = self.p(rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        os.mkfifo(path, 0o600)

    def tool(self, name: str, body: str = TOOL) -> None:
        path = os.path.join(self.tools, name)
        with open(path, "w") as f:
            f.write(body)
        os.chmod(path, 0o755)

    # sysfs, as the kernel lays it out
    def disk(self, parent: str, name: str, mmc_type: str, parts) -> None:
        d = "/sys/devices/%s/block/%s" % (parent, name)
        self.write(d + "/dev", "179:0\n")
        self.write(d + "/size", "62333952\n")
        self.write(d + "/ro", "0\n")
        self.write(d + "/uevent", "MAJOR=179\nMINOR=0\nDEVNAME=%s\nDEVTYPE=disk\n" % name)
        self.write("/sys/devices/%s/type" % parent, mmc_type + "\n")
        self.link(d + "/device", "../..")
        self.link("/sys/class/block/" + name, self.p(d))
        for n, pname in parts:
            self.partition(parent, name, n, pname)

    def partition(self, parent: str, disk: str, n: int, pname: str) -> None:
        name = "%sp%d" % (disk, n)
        d = "/sys/devices/%s/block/%s/%s" % (parent, disk, name)
        self.write(d + "/dev", "179:%d\n" % n)
        self.write(d + "/partition", "%d\n" % n)
        self.write(d + "/size", "2048000\n")
        self.write(d + "/ro", "0\n")
        self.write(d + "/uevent", "DEVNAME=%s\nDEVTYPE=partition\nPARTN=%d\nPARTNAME=%s\n" % (name, n, pname))
        self.link("/sys/class/block/" + name, self.p(d))

    def input(self, node: str, name: str, key: str = "", sw: str = "") -> None:
        d = "/sys/class/input/%s/device" % node
        self.write(d + "/name", name + "\n")
        if key:
            self.write(d + "/capabilities/key", key + "\n")
        if sw:
            self.write(d + "/capabilities/sw", sw + "\n")
        self.fifo("/dev/input/" + node)


EMMC = "platform/fe310000.mmc/mmc_host/mmc0/mmc0:0001"
SLOT1 = "platform/fe2b0000.mmc/mmc_host/mmc1/mmc1:aaaa"
SLOT2 = "platform/fe2c0000.mmc/mmc_host/mmc2/mmc2:bbbb"

CARD1_TABLE = [(1, LINUX_RESERVED, "uboot"), (2, LINUX_RESERVED, "boot"), (3, LINUX_ROOT_ARM64, "root"),
               (4, LINUX_HOME, "data")]


def build_root(base: str) -> Root:
    r = Root(base)
    # The eMMC with Android's userdata (basic data: never a drive), the boot card, a second card.
    r.disk(EMMC, "mmcblk0", "MMC", [(1, "userdata")])
    r.write("/sys/devices/%s/block/mmcblk0boot0/dev" % EMMC, "179:8\n")
    r.write("/sys/devices/%s/block/mmcblk0boot0/size" % EMMC, "8192\n")
    r.link("/sys/devices/%s/block/mmcblk0boot0/device" % EMMC, "../..")
    r.link("/sys/class/block/mmcblk0boot0", r.p("/sys/devices/%s/block/mmcblk0boot0" % EMMC))
    r.disk(SLOT1, "mmcblk1", "SD", [(1, "uboot"), (2, "boot"), (3, "root"), (4, "data")])
    r.disk(SLOT2, "mmcblk2", "SD", [(1, ""), (4, "data")])
    r.write("/dev/mmcblk0", gpt_image([(1, BASIC_DATA, "userdata")]))
    r.write("/dev/mmcblk0p1", exfat_image(0x0E3C0E3C, "EMMC"))
    r.write("/dev/mmcblk0boot0", b"\0" * 4096)
    r.write("/dev/mmcblk1", gpt_image(CARD1_TABLE))
    for n in (1, 2, 3):
        r.write("/dev/mmcblk1p%d" % n, b"\0" * 4096)
    r.write("/dev/mmcblk1p4", ext4_image())
    r.write("/dev/mmcblk2", gpt_image([(1, BASIC_DATA, ""), (4, LINUX_HOME, "data")]))
    r.write("/dev/mmcblk2p1", fat32_image(0x1234ABCD, "CARD2"))
    r.write("/dev/mmcblk2p4", ext4_image())
    r.write("/dev/kmsg", "")
    r.write("/dev/null", "")

    r.write("/sys/power/state", "freeze mem\n")
    r.write("/sys/power/mem_sleep", "[s2idle] deep\n")
    r.write("/sys/class/backlight/backlight/brightness", "100\n")
    r.write("/sys/class/backlight/backlight/max_brightness", "255\n")
    # KEY_POWER (116) and the volume keys (114, 115) sit in the second 64-bit word.
    r.input("event0", "rk805 pwrkey", key="10000000000000 0")
    r.input("event1", "gpio-keys-vol", key="c000000000000 0")
    r.input("event2", "lid-switch", sw="1")
    r.input("event3", "Thimsy Gamepad", key="7fff000000000000 0 0 0 0")

    r.write("/run/thimsy/bootdisk",
            "BOOT_DISK=mmcblk1\nROOT_PART=mmcblk1p3\nDATA_PART=mmcblk1p4\nBOOT_HOW=mountinfo\nBOOT_EMMC=0\n")
    r.write("/run/thimsy/family", "rk3566\n")
    os.makedirs(r.p("/run/thimsy/props/ro"), exist_ok=True)
    os.makedirs(r.p("/storage/.config/thimsy"), exist_ok=True)
    r.write("/usr/share/thimsy/device.json", json.dumps(DEVICE_JSON))
    r.write("/usr/share/thimsy/gpu.json", json.dumps(GPU_JSON, indent=1))
    r.write("/usr/share/thimsy/family/rk3566/hook", TOOL.replace("$(basename \"$0\")", "hook"), 0o755)
    r.write("/etc/init.d/rcD", TOOL.replace("$(basename \"$0\")", "rcD"), 0o755)
    r.write("/etc/init.d/deferred/D30net", TOOL.replace("$(basename \"$0\")", "D30net"), 0o755)

    for name in ("mount", "umount", "fsck.exfat", "fsck.fat", "e2fsck", "amixer", "poweroff", "reboot"):
        r.tool(name)
    r.tool("thimsy-nano", NANO)
    r.tool("thimsy-padd", LONG_RUNNING)
    r.tool("thimsy-nano-scrape", TOOL + "sleep 0.2\n")
    return r


# --- driving the session --------------------------------------------------------------------------

def wait_for(pred, timeout: float = 10.0, what: str = "condition"):
    end = time.monotonic() + timeout
    while True:
        v = pred()
        if v:
            return v
        if time.monotonic() > end:
            raise AssertionError("timed out waiting for " + what)
        time.sleep(0.02)


class SessionCase(unittest.TestCase):
    proc = None
    tmp = None
    r = None

    @classmethod
    def prepare(cls, r: Root) -> None:
        pass

    @classmethod
    def setUpClass(cls) -> None:
        if not BIN:
            raise unittest.SkipTest("THIMSY_SESSION_BIN is not set (run through make e2e)")
        cls.tmp = tempfile.TemporaryDirectory(prefix="thimsy-session-e2e-")
        cls.r = build_root(cls.tmp.name)
        cls.prepare(cls.r)
        env = dict(os.environ)
        cls.stderr = open(os.path.join(cls.tmp.name, "stderr.log"), "w")
        cls.proc = subprocess.Popen([BIN, "--root", cls.r.root, "--fake-hw", "--tooldir", cls.r.tools],
                                    stdin=subprocess.DEVNULL, stdout=cls.stderr, stderr=cls.stderr, env=env)
        wait_for(lambda: "thimsy-session: started" in cls.r.read("/dev/kmsg") or cls.proc.poll() is not None,
                 what="start")
        if cls.proc.poll() is not None:
            raise AssertionError("thimsy-session exited at start:\n" + cls.r.read("/run/thimsy/log/session.log"))

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.proc and cls.proc.poll() is None:
            cls.proc.send_signal(signal.SIGTERM)
            try:
                cls.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                cls.proc.kill()
                cls.proc.wait()
        if cls.r:
            # thimsy-padd outlives the session (rcK stops it on a device).
            try:
                os.kill(int(cls.r.read("/run/thimsy/padd.pid").strip() or "0"), signal.SIGKILL)
            except (OSError, ValueError):
                pass
        if cls.proc:
            cls.stderr.close()
            # Sanitizer reports land on stderr.
            with open(os.path.join(cls.tmp.name, "stderr.log")) as f:
                err = f.read()
            if "ERROR: AddressSanitizer" in err or "runtime error" in err or "LeakSanitizer" in err:
                sys.stderr.write(err)
                raise AssertionError("sanitizer report from thimsy-session")
        if cls.tmp:
            cls.tmp.cleanup()

    # helpers
    def commands(self):
        return self.r.read("/commands.log").splitlines()

    def ran(self, prefix: str):
        return [c for c in self.commands() if c.startswith(prefix)]

    def log(self) -> str:
        return self.r.read("/run/thimsy/log/session.log")

    def prop(self, key: str) -> str:
        return self.r.read("/run/thimsy/props/" + key)

    def set_prop(self, key: str, value: str) -> None:
        d = self.r.p("/run/thimsy/props")
        tmp = os.path.join(d, ".%s.tmp.test" % key)
        with open(tmp, "w") as f:
            f.write(value)
        os.rename(tmp, os.path.join(d, key))

    def keys(self, node: str, text: str) -> None:
        fd = os.open(self.r.p("/dev/input/" + node), os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, text.encode())
        os.close(fd)

    def uevent(self, action: str, name: str, devtype: str, extra: str = "") -> None:
        msg = "%s@/devices/fake/%s\nACTION=%s\nSUBSYSTEM=block\nDEVNAME=%s\nDEVTYPE=%s\n%s\n" % (
            action, name, action, name, devtype, extra)
        fd = os.open(self.r.p("/run/fake-hw/uevent"), os.O_WRONLY | os.O_NONBLOCK)
        os.write(fd, msg.encode())
        os.close(fd)

    def volumes(self):
        text = self.r.read("/run/thimsy/volumes.json")
        return json.loads(text) if text else None

    def volume(self, vid: str):
        v = self.volumes()
        for x in (v or {}).get("volumes", []):
            if x["id"] == vid:
                return x
        return None


class BootDrivesKeysAndRequests(SessionCase):
    """One session from boot: the steps run in order (their names sort that way)."""

    def test_01_nano_starts_in_its_jail_and_padd_is_started(self):
        wait_for(lambda: self.ran("thimsy-nano"), what="Nano")
        wait_for(lambda: self.ran("thimsy-padd"), what="padd")
        self.assertEqual(self.r.read("/run/thimsy/session/jail.plan").split(),
                         ["/dev/mmcblk0", "/dev/mmcblk0boot0", "/dev/mmcblk0p1"])
        self.assertIn("thimsy-session: nano started", self.r.read("/dev/kmsg"))
        v = self.volumes()
        self.assertEqual(v, {"version": 1, "volumes": []})
        self.assertGreaterEqual(int(self.prop("sys.thimsy.volumes.serial")), 1)
        self.assertTrue(os.path.exists(self.r.p("/run/thimsy/nano.pid")))
        self.assertTrue(os.path.exists(self.r.p("/run/thimsy/padd.pid")))

    def test_01b_nano_gets_the_gpu_environment(self):
        wait_for(lambda: self.ran("nano-env"), what="Nano's environment")
        # and the card /etc/asound.conf mixes on: device.json's audio.card_index
        self.assertEqual(self.ran("nano-env"), ["nano-env GALLIUM_DRIVER=softpipe ALSA_CARD=1"])
        self.assertIn("GPU environment for Nano: GALLIUM_DRIVER=softpipe", self.log())

    def test_02_first_frame_runs_the_deferred_services_once(self):
        self.assertEqual(self.ran("rcD"), [])
        self.set_prop("sys.thimsy.nano.first_frame", "1")
        wait_for(lambda: self.ran("rcD start"), what="rcD start")
        self.set_prop("sys.thimsy.nano.first_frame", "1")
        time.sleep(0.3)
        self.assertEqual(self.ran("rcD"), ["rcD start"])
        self.assertTrue(os.path.exists(self.r.p("/run/thimsy/session/deferred-started")))
        # Nano's control keys set sys.boot_completed at its first frame (G9-11); the session never.
        self.assertFalse(os.path.exists(self.r.p("/run/thimsy/props/sys.boot_completed")))

    def test_03_coldplug_mounts_only_the_second_cards_data_drive(self):
        for name, devtype in (("mmcblk0", "disk"), ("mmcblk0p1", "partition"), ("mmcblk0boot0", "disk"),
                              ("mmcblk1", "disk"), ("mmcblk1p1", "partition"), ("mmcblk1p2", "partition"),
                              ("mmcblk1p3", "partition"), ("mmcblk1p4", "partition"), ("mmcblk2", "disk"),
                              ("mmcblk2p1", "partition"), ("mmcblk2p4", "partition")):
            self.uevent("add", name, devtype)
        wait_for(lambda: self.ran("mount "), what="a mount")
        time.sleep(0.3)
        mounts = self.ran("mount ")
        self.assertEqual(mounts, ["mount -t vfat -o %s %s %s" % (VFAT_OPTS, self.r.p("/dev/mmcblk2p1"),
                                                                 self.r.p("/storage/1234-ABCD"))])
        self.assertEqual(self.ran("fsck"), [])  # a clean FAT card is not checked
        self.assertEqual(self.ran("e2fsck"), [])
        v = self.volumes()["volumes"]
        self.assertEqual(len(v), 1)
        self.assertEqual(v[0]["id"], "1234-ABCD")
        self.assertEqual(v[0]["state"], "mounted")
        self.assertEqual(v[0]["label"], "CARD2")
        self.assertEqual(v[0]["kind"], "sd")
        self.assertFalse(v[0]["boot_disk"])
        self.assertEqual(os.readlink(self.r.p("/run/thimsy/media_rw/1234-ABCD")), "/storage/1234-ABCD")
        self.assertTrue(os.path.isdir(self.r.p("/storage/1234-ABCD")))
        self.assertIn("eMMC", self.log())

    def test_04_the_games_partition_is_used_once_it_is_basic_data(self):
        r = self.r
        # D60firstboot: entry 5 made, typed Linux reserved; the kernel adds the partition.
        r.write("/dev/mmcblk1", gpt_image(CARD1_TABLE + [(5, LINUX_RESERVED, "games")]))
        r.write("/dev/mmcblk1p5", exfat_image(0x9602C755, "THIMSY"))
        r.partition(SLOT1, "mmcblk1", 5, "games")
        self.uevent("add", "mmcblk1p5", "partition", "PARTN=5\nPARTNAME=games\n")
        time.sleep(0.4)
        self.assertEqual(self.ran("fsck.exfat"), [])
        self.assertIsNone(self.volume("9602-C755"))
        # Formatted and retyped basic data; D60firstboot writes "change".
        r.write("/dev/mmcblk1", gpt_image(CARD1_TABLE + [(5, BASIC_DATA, "games")]))
        self.uevent("change", "mmcblk1p5", "partition", "PARTN=5\nPARTNAME=games\n")
        wait_for(lambda: (self.volume("9602-C755") or {}).get("state") == "mounted", what="games mounted")
        self.assertEqual(self.ran("fsck.exfat"), ["fsck.exfat -p " + r.p("/dev/mmcblk1p5")])
        self.assertIn("mount -t exfat -o %s %s %s" % (EXFAT_OPTS, r.p("/dev/mmcblk1p5"), r.p("/storage/9602-C755")),
                      self.commands())
        g = self.volume("9602-C755")
        self.assertTrue(g["boot_disk"])
        self.assertEqual(g["label"], "THIMSY")
        self.assertEqual(g["partition"], 5)

    def test_05_a_short_power_press_sleeps_and_wakes(self):
        self.keys("event0", "KEY_POWER 1 0\nKEY_POWER 0 200\n")
        wait_for(lambda: self.prop("sys.thimsy.power.state") == "awake", what="awake")
        self.assertEqual(self.r.read("/sys/power/mem_sleep"), "deep")
        self.assertEqual(self.r.read("/sys/power/state"), "mem")
        self.assertEqual(self.r.read("/sys/class/backlight/backlight/brightness").strip(), "100")
        log = self.log()
        self.assertIn("sleep: backlight off", log)
        self.assertIn("wake: backlight restore", log)
        self.assertIn("hook sleep-pre", self.commands())
        self.assertIn("hook sleep-post", self.commands())
        self.assertEqual(self.ran("D30net"), [])  # Wi-Fi was off

    def test_06_closing_the_lid_during_a_game_stops_and_continues_it(self):
        self.r.write("/run/wpa_supplicant.pid", "999999\n")
        game = subprocess.Popen(["sleep", "60"], start_new_session=True)
        try:
            self.set_prop("sys.thimsy.game.pid", str(game.pid))
            before = self.log().count("sleep: suspend")
            self.keys("event2", "SW_LID 1\n")
            wait_for(lambda: self.log().count("sleep: suspend") > before, what="the lid's sleep")
            wait_for(lambda: "wake: set sys.thimsy.power.state=awake" in self.log().split("sleep: suspend")[-1],
                     what="the wake")
            log = self.log()
            self.assertIn("sleep: signal group %d STOP" % game.pid, log)
            self.assertIn("wake: signal group %d CONT" % game.pid, log)
            stop = log.rindex("sleep: signal group %d STOP" % game.pid)
            self.assertLess(stop, log.rindex("sleep: suspend /sys/power/state mem"))
            self.assertIn("D30net stop", self.commands())
            self.assertIn("D30net start", self.commands())
            self.assertIsNone(game.poll())
            with open("/proc/%d/stat" % game.pid) as f:
                self.assertNotEqual(f.read().split(") ")[1][0], "T")  # running, not stopped
            self.keys("event2", "SW_LID 0\n")

            # The quit hotkey on the Thimsy Gamepad ends the game.
            self.keys("event3", "BTN_MODE 1\nBTN_START 1\n")
            game.wait(timeout=10)
            self.keys("event3", "BTN_START 0\nBTN_MODE 0\n")
        finally:
            if game.poll() is None:
                game.kill()
                game.wait()
            os.unlink(self.r.p("/run/wpa_supplicant.pid"))

    def test_07_volume_keys(self):
        self.keys("event1", "KEY_VOLUMEUP 1\nKEY_VOLUMEUP 0\n")
        wait_for(lambda: self.prop("sys.thimsy.hud.volume") == "65", what="hud volume")
        wait_for(lambda: self.ran("amixer"), what="amixer")
        self.assertEqual(self.ran("amixer"), ["amixer -q -c 1 sset Master 65%"])
        wait_for(lambda: "persist.thimsy.audio.volume=65" in self.r.read("/storage/.config/thimsy/persist.props"),
                 what="persisted volume")

    def test_08_a_long_power_press_goes_to_nano(self):
        self.keys("event0", "KEY_POWER 1 0\nKEY_POWER 0 3100\n")
        wait_for(lambda: self.prop("sys.thimsy.key.power_long") == "1", what="long press")
        self.assertEqual(self.ran("poweroff"), [])

    def test_09_requests(self):
        self.set_prop("sys.thimsy.session.request", "1:brightness:50")
        wait_for(lambda: self.prop("sys.thimsy.session.done") == "1:ok", what="brightness done")
        self.assertEqual(self.r.read("/sys/class/backlight/backlight/brightness"), "128")

        self.set_prop("sys.thimsy.session.request", "2:eject:1234-ABCD")
        wait_for(lambda: self.prop("sys.thimsy.session.done") == "2:ok", what="eject done")
        self.assertIn("umount " + self.r.p("/storage/1234-ABCD"), self.commands())
        self.assertEqual(self.volume("1234-ABCD")["state"], "ejected")
        self.assertFalse(os.path.lexists(self.r.p("/run/thimsy/media_rw/1234-ABCD")))

        self.set_prop("sys.thimsy.session.request", "3:eject:FFFF-FFFF")
        wait_for(lambda: self.prop("sys.thimsy.session.done").startswith("3:error:"), what="eject error")
        self.set_prop("sys.thimsy.session.request", "4:dance")
        wait_for(lambda: self.prop("sys.thimsy.session.done").startswith("4:error:unknown"), what="bad verb")

        self.set_prop("sys.gammaos.nano.scrape_run", "token-1")
        wait_for(lambda: self.ran("thimsy-nano-scrape"), what="scraper")

    def test_10_a_card_pulled_while_mounted_is_detached(self):
        self.uevent("remove", "mmcblk1p5", "partition")
        wait_for(lambda: self.volume("9602-C755") is None, what="volume gone")
        self.assertIn("umount -l " + self.r.p("/storage/9602-C755"), self.commands())
        self.assertFalse(os.path.lexists(self.r.p("/run/thimsy/media_rw/9602-C755")))

    def test_11_padd_is_started_again_when_it_dies(self):
        pid = int(self.r.read("/run/thimsy/padd.pid").strip())
        os.kill(pid, signal.SIGKILL)
        wait_for(lambda: len(self.ran("thimsy-padd")) == 2, what="padd restart")
        self.assertNotEqual(int(self.r.read("/run/thimsy/padd.pid").strip()), pid)

    def test_12_shutdown_through_sys_powerctl(self):
        self.set_prop("sys.powerctl", "shutdown,userrequested")
        wait_for(lambda: self.ran("poweroff"), what="poweroff")
        self.assertIn("thimsy-session: poweroff requested", self.r.read("/dev/kmsg"))


class LateEmmc(SessionCase):
    """An eMMC whose nodes appear after Nano started (a host controller that probes late): Nano is
    started again, and its new plan masks them (security-3)."""

    saved = {}

    @classmethod
    def prepare(cls, r: Root) -> None:
        for n in ("mmcblk0", "mmcblk0boot0", "mmcblk0p1"):
            p = r.p("/dev/" + n)
            if os.path.lexists(p):
                with open(p, "rb") as f:
                    cls.saved[n] = f.read()
                os.remove(p)

    def test_nano_is_started_again_with_the_nodes_masked(self):
        wait_for(lambda: self.ran("thimsy-nano"), what="Nano")
        wait_for(lambda: os.path.exists(self.r.p("/run/thimsy/session/jail.plan")), what="the first plan")
        self.assertEqual(self.r.read("/run/thimsy/session/jail.plan").split(), [])
        time.sleep(0.3)
        self.assertEqual(len(self.ran("thimsy-nano")), 1)
        for n, data in self.saved.items():
            with open(self.r.p("/dev/" + n), "wb") as f:
                f.write(data)
        self.uevent("add", "mmcblk0", "disk")
        wait_for(lambda: len(self.ran("thimsy-nano")) == 2, what="Nano started again")
        wait_for(lambda: self.r.read("/run/thimsy/session/jail.plan").split() ==
                 ["/dev/mmcblk0", "/dev/mmcblk0boot0", "/dev/mmcblk0p1"], what="the new plan")
        self.assertIn("eMMC nodes appeared after Nano started", self.log())
        self.uevent("add", "mmcblk0p1", "partition")   # already masked: no third start
        time.sleep(0.5)
        self.assertEqual(len(self.ran("thimsy-nano")), 2)


class RespawnedSession(SessionCase):
    """A session started again by init later in the boot: Nano from the old session is stopped,
    so is a check (fsck) it left running, the deferred services are not run again, and a drive
    still mounted is adopted, not remounted."""

    orphan = None
    orphan_fsck = None

    @classmethod
    def prepare(cls, r: Root) -> None:
        cls.orphan = subprocess.Popen(["thimsy-nano-left-over", "60"], executable="/bin/sleep",
                                      start_new_session=True)
        r.write("/run/thimsy/nano.pid", "%d\n" % cls.orphan.pid)
        cls.orphan_fsck = subprocess.Popen(["fsck.exfat-left-over", "60"], executable="/bin/sleep",
                                           start_new_session=True)
        r.write("/run/thimsy/session/fsck/mmcblk3p1.pid", "%d\n" % cls.orphan_fsck.pid)
        r.write("/run/thimsy/session/deferred-started", "1\n")
        r.write("/run/thimsy/props/sys.thimsy.nano.first_frame", "1")
        r.write("/proc/mounts", "/dev/mmcblk1p3 / erofs ro 0 0\n/dev/mmcblk1p4 /storage ext4 rw 0 0\n"
                                "/dev/mmcblk2p1 /storage/1234-ABCD vfat rw 0 0\n")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.orphan and cls.orphan.poll() is None:
            cls.orphan.kill()
        if cls.orphan:
            cls.orphan.wait()
        if cls.orphan_fsck and cls.orphan_fsck.poll() is None:
            cls.orphan_fsck.kill()
        if cls.orphan_fsck:
            cls.orphan_fsck.wait()
        super().tearDownClass()

    def test_respawn(self):
        self.orphan.wait(timeout=10)  # stopped by the new session
        self.orphan_fsck.wait(timeout=10)   # and the check it left (device-safety-4)
        self.assertFalse(os.path.exists(self.r.p("/run/thimsy/session/fsck/mmcblk3p1.pid")))
        self.assertIn("left by an earlier session", self.log())
        wait_for(lambda: self.ran("thimsy-nano"), what="Nano")
        wait_for(lambda: (self.volume("1234-ABCD") or {}).get("state") == "mounted", what="adopted drive")
        time.sleep(0.3)
        self.assertEqual(self.ran("mount"), [])  # adopted, not mounted again
        self.assertEqual(self.ran("rcD"), [])
        self.assertIn("deferred services ran before this session", self.log())


# A Nano that launches, as thimsy-nano does since linux-host-impls-1: its first run starts a game in
# its own session, writes sys.thimsy.game.pid and exits 0; its second exits 0 with no game (a launch
# that could not start); later runs stay. Each run notes when it started (ms).
NANO_LAUNCHES = TOOL + r"""R=$THIMSY_ROOT
n=$(cat "$R/nano.runs" 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" > "$R/nano.runs"
echo "$(($(date +%s%N) / 1000000))" >> "$R/nano.starts"
case $n in
1)
	setsid sleep 60 > /dev/null 2>&1 &
	printf '%s' "$!" > "$R/run/thimsy/props/.sys.thimsy.game.pid.tmp"
	mv "$R/run/thimsy/props/.sys.thimsy.game.pid.tmp" "$R/run/thimsy/props/sys.thimsy.game.pid"
	exit 0 ;;
2) exit 0 ;;
esac
exec sleep 1000
"""


class NanoLeavesAGameRunning(SessionCase):
    """linux-host-impls-1, -2, -6: Nano exits after a launch, leaving the game running. The session
    does not start Nano while the game runs, sees the game through sys.thimsy.game.pid (and clears
    it when the game ends), and starts Nano at once after the game and after a launch that did not
    start, not after a crash's back-off."""

    @classmethod
    def prepare(cls, r: Root) -> None:
        r.tool("thimsy-nano", NANO_LAUNCHES)

    def test_nano_comes_back_after_the_game_and_after_a_failed_launch(self):
        wait_for(lambda: "Nano left game" in self.log(), what="the game watched")
        game = int(self.prop("sys.thimsy.game.pid"))
        time.sleep(0.5)
        self.assertEqual(len(self.ran("thimsy-nano")), 1)  # not while the game runs
        # The session reaches the game's group through the key: quit-game ends it.
        self.set_prop("sys.thimsy.session.request", "1:quit-game")
        wait_for(lambda: self.prop("sys.thimsy.session.done") == "1:ok", what="quit-game done")
        self.assertIn("signal 15 to group %d" % game, self.log())
        wait_for(lambda: "the game Nano left running ended" in self.log(), what="the game's end")
        wait_for(lambda: len(self.ran("thimsy-nano")) == 3, what="Nano started twice more")
        self.assertEqual(self.prop("sys.thimsy.game.pid"), "")  # the session cleared it
        starts = [int(x) for x in self.r.read("/nano.starts").split()]
        self.assertEqual(len(starts), 3)
        # Run 2 exited 0 at once with no game: run 3 follows without the 1 s back-off.
        self.assertLess(starts[2] - starts[1], 900)


if __name__ == "__main__":
    unittest.main(verbosity=2)
