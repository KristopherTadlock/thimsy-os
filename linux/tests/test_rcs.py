# SPDX-License-Identifier: MIT
"""Tests of the init scripts (IMPLEMENTATION D.4, test_rcs.py row).

Static: linux/tools/check_rcs.py passes on the tree, and catches what it
should on planted faults.

Run against a fake root (THIMSY_ROOT) with fake tools (THIMSY_TOOLDIR,
rcs_fake_tool.py), unprivileged:
  * S00base picks the boot disk (mounted root, root=PARTUUID=, root=/dev/)
    and mounts only its data partition, never a second card's or the eMMC's,
    and makes the first-boot tree;
  * D60firstboot makes the games partition on the boot disk only, and
    reaches the same end state after a power cut at any step;
  * S01props, S03modules and D05family (the family hook), D10cards;
  * the post-build runner and its two parts.

The scripts run with BusyBox sh when it is installed, else /bin/sh (dash on
Ubuntu); on Windows with Git for Windows' sh.exe, where the tests that need
symbolic links are skipped (Git Bash cannot make them without developer
mode). THIMSY_SH overrides the shell.

    py -3.12 -I -m unittest discover -s linux/tests -t linux -p test_rcs.py
    python3 linux/tests/test_rcs.py           (WSL, Python 3.8)
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(LINUX / "tools"))

import check_rcs  # noqa: E402
import rcs_fake_tool as fake  # noqa: E402

OVERLAYS = [LINUX / "board" / "common" / "rootfs-overlay",
            LINUX / "board" / "rk3566" / "common" / "rootfs-overlay"]
INITD = "etc/init.d"

SHELLS = check_rcs.find_shells()
SH: Optional[List[str]] = SHELLS[0] if SHELLS else None
GIT = shutil.which("git")

RESERVED = fake.RESERVED
BASICDATA = fake.BASICDATA
LINUX_HOME = "933AC7E1-2EB4-4F13-B844-0E14E2AEF915"
LINUX_ROOT = "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"
LINUX_DATA = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
MIB = 2048  # sectors of 512 bytes


def write_lf(path: Path, text: str) -> None:
    """Write text with LF line endings (Path.write_text has no newline= before 3.10)."""
    with open(str(path), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def posix(p: Path) -> str:
    return p.as_posix()


def _probe_symlinks() -> bool:
    if SH is None:
        return False
    d = Path(tempfile.mkdtemp(prefix="rcs-ln-"))
    try:
        subprocess.run(SH + ["-c", 'ln -s /nonexistent/target "$1"', "sh",
                             posix(d / "l")], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)
        return os.path.islink(d / "l")
    finally:
        shutil.rmtree(d, ignore_errors=True)


SYMLINKS = _probe_symlinks()
needs_sh = unittest.skipIf(SH is None, "no POSIX shell found")
needs_links = unittest.skipIf(not SYMLINKS, "this shell cannot make symbolic links here")

_TEMPLATE: List[Path] = []


def _template() -> Path:
    """The overlays merged once into a local temporary directory (copying
    from a WSL /mnt/c checkout for every test is slow), scripts executable,
    with rcs_fake_tool.py beside it."""
    if not _TEMPLATE:
        base = Path(tempfile.mkdtemp(prefix="test-rcs-template-"))
        import atexit
        atexit.register(shutil.rmtree, str(base), True)
        root = base / "root"
        for ov in OVERLAYS:
            shutil.copytree(str(ov), str(root), dirs_exist_ok=True)
        for p in list((root / "etc/init.d").rglob("*")) + list(
                (root / "usr/share/thimsy/family").glob("*/hook")):
            if p.is_file():
                os.chmod(str(p), 0o755)
        shutil.copy(str(HERE / "rcs_fake_tool.py"), str(base / "rcs_fake_tool.py"))
        _TEMPLATE.append(root)
    return _TEMPLATE[0]


def image_parts(uuid_prefix: str) -> Dict[str, Dict[str, Any]]:
    """Entries 1 to 4 of the written image (IMPLEMENTATION E.6), with spares."""
    layout = [("uboot", 16 * MIB, 8 * MIB, RESERVED), ("boot", 0, 40 * MIB, RESERVED),
              ("root", 0, 1024 * MIB, LINUX_ROOT), ("data", 0, 256 * MIB, LINUX_HOME)]
    parts = {}
    pos = 0
    spare = {"uboot": 8 * MIB, "boot": 40 * MIB, "root": 1024 * MIB, "data": 0}
    for n, (name, first, size, typ) in enumerate(layout, 1):
        pos = max(pos, first)
        parts[str(n)] = {"start": pos, "size": size, "type": typ,
                         "uuid": "%s-0000-4000-8000-00000000000%d" % (uuid_prefix, n),
                         "name": name, "fs": None}
        pos += size + spare[name]
    return parts


IMAGE_END = 16 * MIB + 8 * MIB + 8 * MIB + 40 * MIB + 40 * MIB + 1024 * MIB * 2 + 256 * MIB


class World:
    """A fake root: /proc, /sys, /dev with fake disks, the overlay, fake tools."""

    PY_TOOLS = ["sfdisk", "mkfs.exfat"]
    SH_TOOLS = ["mount", "umount", "e2fsck", "sync", "modprobe", "thimsy-prop",
                "thimsy-padd", "amixer", "alsactl"]
    # The trivial fakes: log the call (as rcs_fake_tool.log_call does), fail
    # without an effect after a power cut, and do the little each one does.
    SH_FAKE = r"""#!/bin/sh
tool=${0##*/}
R=${THIMSY_ROOT:-}
note=
[ -e "$R/.powercut" ] && note="no power"
{ printf '%s\t%s' "$tool" "$note"; for a in "$@"; do printf '\t%s' "$a"; done
  printf '\n'; } >> "$THIMSY_FAKE_LOG"
[ -n "$note" ] && exit 1
case $tool in
mount)
	case " ${THIMSY_FAKE_FAIL:-} " in
	*" mount-ext4 "*) [ "$1 $2" = "-t ext4" ] && exit 32 ;;
	esac
	;;
thimsy-prop)
	if [ "${1:-}" = boot ]; then
		mkdir -p "$R/run/thimsy/props/ro"
		[ -n "${THIMSY_FAKE_FAMILY:-}" ] &&
			printf '%s\n' "$THIMSY_FAKE_FAMILY" > "$R/run/thimsy/props/ro/ro.thimsy.family"
		[ -n "${THIMSY_FAKE_MODULES:-}" ] &&
			printf '%s\n' "$THIMSY_FAKE_MODULES" > "$R/run/thimsy/props/ro/ro.thimsy.modules"
	fi
	;;
esac
exit 0
"""

    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.root = tmp / "root"
        self.bin = tmp / "bin"
        self.log = tmp / "tools.log"
        self.disks: List[str] = []
        shutil.copytree(str(_template()), str(self.root))
        for d in ("proc/self", "sys/class/block", "dev", "run", "tmp", "storage"):
            (self.root / d).mkdir(parents=True, exist_ok=True)
        self.write("proc/uptime", "1.25 0.50\n")
        self.write("proc/mounts", "/dev/root / erofs ro,relatime 0 0\n")
        self.write("dev/null", "")
        self.write("dev/kmsg", "")
        self.bin.mkdir()
        py = posix(Path(sys.executable))
        # A local copy, run isolated (-I -S): starting Python with its script
        # on a 9P mount (WSL's /mnt/c) costs a second per call.
        tool = self.bin / "rcs_fake_tool.py"
        shutil.copy(str(_template().parent / "rcs_fake_tool.py"), str(tool))
        for name in self.PY_TOOLS + self.SH_TOOLS:
            p = self.bin / name
            if name in self.PY_TOOLS:
                text = '#!/bin/sh\nexec "%s" -I -S "%s" %s "$@"\n' % (py, posix(tool), name)
            else:
                text = self.SH_FAKE
            write_lf(p, text)
            os.chmod(str(p), 0o755)
        self.env_extra: Dict[str, str] = {}

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        write_lf(p, text)

    def read(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")

    # disks ------------------------------------------------------------------
    def add_disk(self, name: str, minor: int, typ: Optional[str], parts: Dict[str, Any],
                 size_sectors: int, last_lba: Optional[int] = None) -> None:
        d = {"kind": "disk", "name": name, "sector_size": 512, "size_sectors": size_sectors,
             "first_lba": 34, "last_lba": last_lba if last_lba else size_sectors - 34,
             "removable_type": typ, "major": 179, "minor": minor, "parts": parts}
        fake.save_disk(self.root, d)
        fake.sync_kernel_view(self.root, name)
        self.disks.append(name)

    def disk(self, name: str) -> Dict[str, Any]:
        return fake.load_disk(self.root, name)

    def disk_bytes(self, name: str) -> bytes:
        return (self.root / "dev" / name).read_bytes()

    def standard(self, clone: bool = False) -> None:
        """eMMC with Android's table (one entry named data), the boot card in
        slot 1 (image smaller than the card), a second card in slot 2 with a
        data partition too (a clone of the image when clone=True)."""
        emmc = {}
        pos = 16 * MIB
        for n, nm in enumerate(["uboot", "misc", "boot", "system", "vendor", "metadata",
                                "data"], 1):
            emmc[str(n)] = {"start": pos, "size": 64 * MIB, "type": LINUX_DATA,
                            "uuid": "eeeeeeee-0000-4000-8000-00000000000%d" % n,
                            "name": nm, "fs": None}
            pos += 64 * MIB
        self.add_disk("mmcblk0", 0, "MMC", emmc, 64 * 1024 * MIB)
        self.add_disk("mmcblk1", 16, "SD", image_parts("11111111"), 16 * 1024 * MIB,
                      last_lba=IMAGE_END + MIB - 34)
        self.add_disk("mmcblk2", 32, "SD", image_parts("11111111" if clone else "22222222"),
                      32 * 1024 * MIB, last_lba=IMAGE_END + MIB - 34)

    def cmdline(self, text: str) -> None:
        self.write("proc/cmdline", text + "\n")

    def mounted_root(self, majmin: Optional[str]) -> None:
        p = self.root / "proc/self/mountinfo"
        if majmin is None:
            if p.exists():
                p.unlink()
            return
        self.write("proc/self/mountinfo",
                   "22 1 %s / / ro,relatime shared:1 - erofs /dev/root ro\n"
                   "23 22 0:5 / /dev rw,nosuid shared:2 - devtmpfs devtmpfs rw\n" % majmin)

    # running ------------------------------------------------------------------
    def run(self, rel: str, *args: str, env: Optional[Dict[str, str]] = None,
            check: bool = True) -> subprocess.CompletedProcess:
        assert SH is not None
        e = dict(os.environ)
        e.update({"THIMSY_ROOT": posix(self.root), "THIMSY_TOOLDIR": posix(self.bin),
                  "THIMSY_FAKE_LOG": posix(self.log), "LC_ALL": "C"})
        for k in ("THIMSY_FAKE_POWERCUT", "THIMSY_FAKE_FAIL", "THIMSY_FAKE_FAMILY",
                  "THIMSY_FAKE_MODULES"):
            e.pop(k, None)
        e.update(self.env_extra)
        if env:
            e.update(env)
        r = subprocess.run(SH + [posix(self.root / rel)] + list(args), env=e,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        if check and r.returncode != 0:
            raise AssertionError("%s exited %d\nstdout:\n%s\nstderr:\n%s\nboot.log:\n%s" % (
                rel, r.returncode, r.stdout.decode(errors="replace"),
                r.stderr.decode(errors="replace"), self.boot_log()))
        return r

    def boot_log(self) -> str:
        p = self.root / "run/thimsy/log/boot.log"
        return p.read_text(encoding="utf-8") if p.exists() else ""

    def calls(self) -> List[Dict[str, Any]]:
        if not self.log.exists():
            return []
        root = posix(self.root)
        out = []
        for line in self.log.read_text(encoding="utf-8").split("\n"):
            if not line:
                continue
            f = line.split("\t")
            rec: Dict[str, Any] = {"tool": f[0], "args": [a.replace(root, "") for a in f[2:]]}
            if f[1]:
                rec["note"] = f[1]
            out.append(rec)
        return out

    def clear_calls(self) -> None:
        if self.log.exists():
            self.log.unlink()

    def reboot(self) -> None:
        """A power cycle: /run empties, the kernel re-reads every table."""
        pc = self.root / ".powercut"
        if pc.exists():
            pc.unlink()
        run = self.root / "run"
        shutil.rmtree(str(run))
        run.mkdir()
        for d in self.disks:
            fake.sync_kernel_view(self.root, d)


class WorldCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="test-rcs-"))
        self.w = World(self._tmp)

    def tearDown(self) -> None:
        shutil.rmtree(str(self._tmp), ignore_errors=True)

    def assertNoCallMentions(self, *names: str) -> None:
        for c in self.w.calls():
            for a in c["args"]:
                for n in names:
                    self.assertNotIn("/" + n, a, "call touched %s: %r" % (n, c))


# --- static -----------------------------------------------------------------------

class TestCheckRcsTree(unittest.TestCase):
    def test_tree_passes(self) -> None:
        rep = check_rcs.check(LINUX, use_git=GIT is not None)
        self.assertEqual(rep.problems, [], "\n".join(rep.problems))
        if not rep.shells:
            self.skipTest("sh -n not run: no POSIX shell (the rest passed)")

    def test_order_lists_match_scripts(self) -> None:
        initd = LINUX / "board/common/rootfs-overlay/etc/init.d"
        self.assertEqual(sorted(p.name for p in initd.glob("S*")), check_rcs.S_ORDER)
        self.assertEqual(sorted(p.name for p in (initd / "deferred").glob("D*")),
                         check_rcs.D_ORDER)


class TestCheckRcsFaults(unittest.TestCase):
    """check_rcs catches planted faults in a copy of the tree."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.pristine = Path(tempfile.mkdtemp(prefix="test-rcs-board-"))
        shutil.copytree(str(LINUX / "board"), str(cls.pristine / "board"))

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(str(cls.pristine), ignore_errors=True)

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="test-rcs-chk-"))
        self.linux = self.tmp / "linux"
        shutil.copytree(str(self.pristine / "board"), str(self.linux / "board"))
        self.initd = self.linux / check_rcs.INITD

    def tearDown(self) -> None:
        shutil.rmtree(str(self.tmp), ignore_errors=True)

    def check(self, git: bool = False) -> List[str]:
        return check_rcs.check(self.linux, shells=[], use_git=git).problems

    def edit(self, rel: str, old: str, new: str) -> None:
        p = self.linux / rel
        text = p.read_text(encoding="utf-8")
        self.assertIn(old, text)
        write_lf(p, text.replace(old, new, 1))

    def append(self, path: Path, text: str) -> None:
        with open(path, "a", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def test_copy_is_clean(self) -> None:
        self.assertEqual(self.check(), [])

    def test_bashisms(self) -> None:
        cases = ['if [[ -e /x ]]; then :; fi\n', 'function f { :; }\n', 'x=${y:1:2}\n',
                 'x=${y//a/b}\n', 'cat <<< "$x"\n', 'cmd &> /dev/null\n', 'a=(1 2)\n',
                 'echo -e "x"\n', 'source /etc/x\n', '[ "$a" == b ]\n', 'read -a x\n',
                 'x=$RANDOM\n', 'echo {a,b}\n']
        for text in cases:
            with self.subTest(text=text):
                p = self.initd / "S03modules"
                orig = p.read_text(encoding="utf-8")
                self.append(p, text)
                probs = self.check()
                write_lf(p, orig)
                self.assertTrue(any("bashism" in m for m in probs), (text, probs))

    def test_comments_and_heredocs_ignored(self) -> None:
        self.append(self.initd / "rcK", "# [[ a == b ]] in a comment\ncat <<EOF\n[[ x ]]\nEOF\n")
        self.assertEqual(self.check(), [])

    def test_forbidden_on_sync_path(self) -> None:
        for text in ("sleep 1\n", 'ntpd -p pool.ntp.org &\n', '"$SFDISK" --dump /dev/x\n',
                     "/usr/sbin/wpa_supplicant -B\n", "mkfs.ext4 /dev/x\n", "fsck -p /dev/x\n"):
            with self.subTest(text=text):
                p = self.initd / "S02padd"
                orig = p.read_text(encoding="utf-8")
                self.append(p, text)
                probs = self.check()
                write_lf(p, orig)
                self.assertTrue(any("synchronous rcS path" in m for m in probs), (text, probs))

    def test_forbidden_allowed_off_sync_path_and_by_comment(self) -> None:
        self.append(self.initd / "deferred" / "D40time", "sleep 1\n")
        self.append(self.initd / "S02padd", "sleep 1 # rcs-allow: sleep\n")
        self.assertEqual(self.check(), [])

    def test_family_hook_is_sync(self) -> None:
        hook = next(self.linux.glob("board/*/common/rootfs-overlay/usr/share/thimsy/family/*/hook"))
        self.append(hook, "udevadm settle\n")
        self.assertTrue(any("'udevadm'" in m for m in self.check()))

    def test_order(self) -> None:
        (self.initd / "S03modules").rename(self.initd / "S04modules")
        self.assertTrue(any("S scripts" in m for m in self.check()))

    def test_extra_deferred(self) -> None:
        shutil.copy(str(self.initd / "deferred" / "D40time"),
                    str(self.initd / "deferred" / "D45extra"))
        self.assertTrue(any("D scripts" in m for m in self.check()))

    def test_crlf_shebang_lib_byname(self) -> None:
        p = self.initd / "S01props"
        p.write_bytes(p.read_bytes().replace(b"\n", b"\r\n"))
        self.edit(check_rcs.INITD + "/S02padd", "#!/bin/sh\n", "#!/bin/bash\n")
        self.edit(check_rcs.INITD + "/S03modules", '. "${THIMSY_ROOT:-}/usr/share/thimsy/init/lib.sh"',
                  ": lib")
        self.append(self.initd / "rcK", "ls /dev/block/by-name\n")
        probs = self.check()
        self.assertTrue(any("CR characters" in m for m in probs), probs)
        self.assertTrue(any("#!/bin/sh" in m for m in probs), probs)
        self.assertTrue(any("does not source" in m for m in probs), probs)
        self.assertTrue(any("by-name" in m for m in probs), probs)

    def test_inittab(self) -> None:
        self.edit(check_rcs.INITTAB, "::sysinit:/etc/init.d/rcS", "::sysinit:/etc/init.d/other")
        self.assertTrue(any("sysinit" in m for m in self.check()))

    @unittest.skipIf(GIT is None, "git not found")
    def test_executable_bit_in_index(self) -> None:
        def git(*a: str) -> None:
            subprocess.run([GIT, "-C", str(self.linux)] + list(a), check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git("init", "-q")
        git("config", "core.autocrlf", "false")
        git("add", "-A")
        roles = check_rcs.scripts(self.linux)
        exes = [p.relative_to(self.linux).as_posix()
                for p in check_rcs.executables(self.linux, roles)]
        git("update-index", "--chmod=+x", *exes)
        self.assertEqual(self.check(git=True), [])
        git("update-index", "--chmod=-x", check_rcs.INITD + "/deferred/D60firstboot")
        probs = self.check(git=True)
        self.assertTrue(any("D60firstboot: mode 100644" in m for m in probs), probs)


# --- S00base ----------------------------------------------------------------------

@needs_sh
@needs_links
class TestS00Base(WorldCase):
    def setUp(self) -> None:
        super().setUp()
        self.w.standard()

    def ext4_mounts(self) -> List[List[str]]:
        return [c["args"] for c in self.w.calls()
                if c["tool"] == "mount" and c["args"][:2] == ["-t", "ext4"]]

    def bootdisk(self) -> Dict[str, str]:
        out = {}
        for line in self.w.read("run/thimsy/bootdisk").splitlines():
            k, _, v = line.partition("=")
            out[k] = v
        return out

    def test_mounted_root_picks_boot_card_data_only(self) -> None:
        self.w.cmdline("console=ttyS2 root=PARTUUID=11111111-0000-4000-8000-000000000003 rootwait")
        self.w.mounted_root("179:19")  # mmcblk1p3
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts(),
                         [["-t", "ext4", "-o", "noatime,nosuid,nodev", "/dev/mmcblk1p4", "/storage"]])
        self.assertNoCallMentions("mmcblk0", "mmcblk2")
        bd = self.bootdisk()
        self.assertEqual((bd["BOOT_DISK"], bd["ROOT_PART"], bd["DATA_PART"], bd["BOOT_HOW"],
                          bd["BOOT_EMMC"]), ("mmcblk1", "mmcblk1p3", "mmcblk1p4", "mountinfo", "0"))

    def test_base_mounts(self) -> None:
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        targets = [c["args"][-1] for c in self.w.calls() if c["tool"] == "mount"]
        for t in ("/run", "/tmp", "/dev/shm", "/dev/pts", "/storage"):
            self.assertIn(t, targets)
        for t in ("/proc", "/sys", "/dev"):  # already there
            self.assertNotIn(t, targets)
        for d in ("run/thimsy/props/ro", "run/thimsy/log", "run/thimsy/boot"):
            self.assertTrue((self.w.root / d).is_dir(), d)
        self.assertTrue((self.w.root / "run/thimsy/boot/s00-done").exists())

    def test_already_mounted_not_remounted(self) -> None:
        self.w.mounted_root("179:19")
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\ntmpfs /run tmpfs rw 0 0\n"
                     "/dev/mmcblk1p4 /storage ext4 rw 0 0\n"
                     "/dev/mmcblk1p4 /root ext4 rw 0 0\n")
        self.w.run(INITD + "/S00base", "start")
        targets = [c["args"][-1] for c in self.w.calls() if c["tool"] == "mount"]
        self.assertNotIn("/run", targets)
        self.assertNotIn("/storage", targets)
        self.assertNotIn("/root", targets)

    def test_root_home_bound_from_storage(self) -> None:
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        binds = [c["args"] for c in self.w.calls() if c["tool"] == "mount" and "--bind" in c["args"]]
        self.assertEqual(len(binds), 1, binds)
        self.assertEqual(binds[0][-1], "/root")
        self.assertTrue(binds[0][-2].endswith("/storage/.config/thimsy/home/root"), binds[0])
        seq = [c["args"][-1] for c in self.w.calls() if c["tool"] == "mount"]
        self.assertLess(seq.index("/storage"), seq.index("/root"))

    def test_partuuid_when_root_is_not_seen(self) -> None:
        self.w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003 ro")
        self.w.mounted_root("0:20")  # not a block partition
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts()[0][-2], "/dev/mmcblk1p4")
        self.assertEqual(self.bootdisk()["BOOT_HOW"], "partuuid")

    def test_partuuid_case_insensitive(self) -> None:
        self.w.cmdline("root=PARTUUID=11111111-0000-4000-8000-00000000000A ro")
        d = self.w.disk("mmcblk1")
        d["parts"]["3"]["uuid"] = "11111111-0000-4000-8000-00000000000a"
        fake.save_disk(self.w.root, d)
        fake.sync_kernel_view(self.w.root, "mmcblk1")
        self.w.mounted_root(None)
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts()[0][-2], "/dev/mmcblk1p4")

    def test_root_devname(self) -> None:
        self.w.cmdline("root=/dev/mmcblk2p3 rootfstype=erofs")
        self.w.mounted_root(None)
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts()[0][-2], "/dev/mmcblk2p4")
        self.assertNoCallMentions("mmcblk0", "mmcblk1")

    def test_cloned_cards_follow_the_mounted_root(self) -> None:
        self.w = World(Path(tempfile.mkdtemp(prefix="test-rcs-clone-")))
        self.addCleanup(shutil.rmtree, str(self.w.tmp), True)
        self.w.standard(clone=True)
        self.w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
        self.w.mounted_root("179:35")  # mmcblk2p3: booted from slot 2
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts(), [["-t", "ext4", "-o", "noatime,nosuid,nodev",
                                               "/dev/mmcblk2p4", "/storage"]])
        self.assertNoCallMentions("mmcblk0", "mmcblk1")

    def test_cloned_cards_without_mounted_root_guess_nothing(self) -> None:
        self.w = World(Path(tempfile.mkdtemp(prefix="test-rcs-clone-")))
        self.addCleanup(shutil.rmtree, str(self.w.tmp), True)
        self.w.standard(clone=True)
        self.w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
        self.w.mounted_root(None)
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts(), [])
        self.assertIn(["-t", "tmpfs", "-o", "nosuid,nodev,mode=0755", "tmpfs", "/storage"],
                      [c["args"] for c in self.w.calls() if c["tool"] == "mount"])
        self.assertNoCallMentions("mmcblk0", "mmcblk1", "mmcblk2")

    def test_emmc_boot_disk_is_not_used(self) -> None:
        self.w.cmdline("root=PARTUUID=eeeeeeee-0000-4000-8000-000000000004")
        self.w.mounted_root("179:4")  # mmcblk0p4 on the eMMC
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.ext4_mounts(), [])
        self.assertNoCallMentions("mmcblk0", "mmcblk1", "mmcblk2")
        self.assertEqual(self.bootdisk()["BOOT_EMMC"], "1")
        self.assertIn("eMMC", self.w.read("dev/kmsg"))

    def test_failed_mount_checks_then_falls_back(self) -> None:
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start", env={"THIMSY_FAKE_FAIL": "mount-ext4"})
        seq = []
        for c in self.w.calls():
            if c["tool"] == "e2fsck":
                seq.append("fsck")
            elif c["tool"] == "mount" and c["args"][-1] == "/storage":
                seq.append("mount-" + c["args"][1])
        self.assertEqual(seq, ["mount-ext4", "fsck", "mount-ext4", "mount-tmpfs"])
        self.assertIn(["-p", "/dev/mmcblk1p4"],
                      [c["args"] for c in self.w.calls() if c["tool"] == "e2fsck"])
        self.assertIn("tmpfs", self.w.read("dev/kmsg") + self.w.boot_log())

    def test_first_boot_tree(self) -> None:
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        cfg = self.w.root / "storage/.config/thimsy"
        for d in ("data/system", "data/local/tmp", "data/data", "data/misc", "data/gammaos_ota",
                  "ssh", "home/root/.ssh"):
            self.assertTrue((cfg / d).is_dir(), d)
        for d in ("storage/emulated/0", "storage/.config/retroarch", "storage/thimsy"):
            self.assertTrue((self.w.root / d).is_dir(), d)
        self.assertEqual(os.readlink(str(cfg / "data/user/0")), "../data")
        self.assertEqual(os.readlink(str(cfg / "data/media/0")), "/storage/emulated/0")
        self.assertEqual(os.readlink(str(self.w.root / "storage/self/primary")),
                         "/storage/emulated/0")
        self.assertEqual(os.readlink(str(cfg / "localtime")), "/usr/share/zoneinfo/Etc/UTC")
        self.assertTrue((cfg / ".tree-v1").exists())
        if os.name != "nt":
            self.assertEqual((cfg / "ssh").stat().st_mode & 0o777, 0o700)
            self.assertEqual((cfg / "data/local/tmp").stat().st_mode & 0o7777, 0o1777)

    def test_first_boot_tree_repeats_safely(self) -> None:
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        cfg = self.w.root / "storage/.config/thimsy"
        # A power cut before the stamp: everything is made again over itself.
        (cfg / ".tree-v1").unlink()
        os.unlink(str(cfg / "data/media/0"))
        (cfg / "data/system/nano_state.txt").write_text("kept\n", encoding="utf-8")
        self.w.reboot()
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(os.readlink(str(cfg / "data/media/0")), "/storage/emulated/0")
        self.assertEqual(os.readlink(str(cfg / "data/user/0")), "../data")
        self.assertFalse((cfg / "data/data/0").exists())  # no link made inside the link
        self.assertEqual((cfg / "data/system/nano_state.txt").read_text(encoding="utf-8"), "kept\n")
        self.assertTrue((cfg / ".tree-v1").exists())


# --- S01props, S03modules, D05family, D10cards, rcS ------------------------------

@needs_sh
class TestPropsAndFamily(WorldCase):
    def setUp(self) -> None:
        super().setUp()
        for d in ("run/thimsy/props/ro", "run/thimsy/log", "run/thimsy/boot"):
            (self.w.root / d).mkdir(parents=True, exist_ok=True)
        cpu = "sys/devices/system/cpu/cpufreq"
        self.w.write(cpu + "/policy0/scaling_governor", "schedutil\n")
        self.w.write("sys/class/devfreq/fde60000.gpu/governor", "simple_ondemand\n")

    def test_family_from_prop(self) -> None:
        self.w.run(INITD + "/S01props", "start", env={"THIMSY_FAKE_FAMILY": "rk3566"})
        self.assertEqual(self.w.read("run/thimsy/family"), "rk3566\n")
        calls = [c["args"] for c in self.w.calls() if c["tool"] == "thimsy-prop"]
        self.assertEqual(len(calls), 1)
        args = calls[0]
        self.assertEqual(args[0], "boot")
        self.assertEqual(len(args), 7)
        opts = dict(zip(args[1::2], args[2::2]))
        self.assertEqual(sorted(opts), ["--build", "--device", "--install"])
        self.assertTrue(opts["--device"].endswith("/usr/share/thimsy/device.json"))
        self.assertTrue(opts["--build"].endswith("/usr/share/thimsy/build.props"))
        # G2e-14: the card's thimsy/ folder on the data partition, not /boot.
        self.assertTrue(opts["--install"].endswith("/storage/thimsy/install.json"))

    def test_install_json_path_matches_thimsy_prop(self) -> None:
        # thimsy-prop's own default (for a shell user) is the path S01props passes.
        path = LINUX.parent / "libthimsy" / "tools" / "thimsy-prop.cpp"
        if not path.is_file():
            self.skipTest("no libthimsy/ beside linux/")
        src = path.read_text(encoding="utf-8")
        self.assertIn('envOr("THIMSY_INSTALL_JSON", "/storage/thimsy/install.json")', src)
        self.assertIn('envOr("THIMSY_DEVICE_JSON", "/usr/share/thimsy/device.json")', src)
        self.assertIn('envOr("THIMSY_BUILD_PROPS", "/usr/share/thimsy/build.props")', src)

    def test_family_from_the_only_family_dir(self) -> None:
        self.w.run(INITD + "/S01props", "start")
        self.assertEqual(self.w.read("run/thimsy/family"), "rk3566\n")

    def test_unknown_family_with_two_dirs(self) -> None:
        (self.w.root / "usr/share/thimsy/family/h700").mkdir(parents=True)
        self.w.run(INITD + "/S01props", "start")
        self.assertEqual(self.w.read("run/thimsy/family"), "")
        self.assertIn("family unknown", self.w.boot_log())

    def test_modules_and_governors(self) -> None:
        self.w.run(INITD + "/S01props", "start",
                   env={"THIMSY_FAKE_FAMILY": "rk3566", "THIMSY_FAKE_MODULES": "joypad rumble"})
        self.w.run(INITD + "/S03modules", "start")
        self.assertEqual([c["args"] for c in self.w.calls() if c["tool"] == "modprobe"],
                         [["joypad"], ["rumble"]])
        cpu = "sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
        self.assertEqual(self.w.read(cpu).strip(), "performance")
        self.assertEqual(self.w.read("sys/class/devfreq/fde60000.gpu/governor").strip(),
                         "performance")
        self.w.run(INITD + "/deferred/D05family", "start")
        self.assertEqual(self.w.read(cpu).strip(), "schedutil")
        self.assertEqual(self.w.read("sys/class/devfreq/fde60000.gpu/governor").strip(),
                         "simple_ondemand")

    @unittest.skipUnless(os.name == "posix", "signals a real process")
    def test_s02padd_stop(self) -> None:
        for comm, killed in (("thimsy-padd", True), ("sleep", False)):
            with self.subTest(comm=comm):
                proc = subprocess.Popen(["sleep", "60"])
                try:
                    self.w.write("proc/%d/comm" % proc.pid, comm + "\n")
                    self.w.write("run/thimsy/padd.pid", "%d\n" % proc.pid)
                    self.w.run(INITD + "/S02padd", "stop")
                    if killed:
                        self.assertEqual(proc.wait(timeout=10), -15)
                        self.assertIn("stopped thimsy-padd", self.w.boot_log())
                    else:
                        time.sleep(0.2)
                        self.assertIsNone(proc.poll(), "a process that is not thimsy-padd was signalled")
                        self.assertIn("thimsy-padd is not running", self.w.boot_log())
                    self.assertFalse((self.w.root / "run/thimsy/padd.pid").exists())
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait()
        # No pid file at all: nothing to do, and no error.
        self.w.run(INITD + "/S02padd", "stop")

    def test_fixedperf_actions(self) -> None:
        # Run as Nano's control keys run it: sh hook fixedperf-on|off (G9-13).
        hook = "usr/share/thimsy/family/rk3566/hook"
        cpu = "sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
        gpu = "sys/class/devfreq/fde60000.gpu/governor"
        self.w.run(hook, "fixedperf-on")
        self.assertEqual(self.w.read(cpu).strip(), "performance")
        self.assertEqual(self.w.read(gpu).strip(), "performance")
        self.w.run(hook, "fixedperf-off")
        self.assertEqual(self.w.read(cpu).strip(), "schedutil")
        self.assertEqual(self.w.read(gpu).strip(), "simple_ondemand")
        # Missing or read-only files are skipped, and the hook still succeeds.
        (self.w.root / gpu).unlink()
        (self.w.root / cpu).chmod(0o444)
        try:
            r = self.w.run(hook, "fixedperf-on")
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stderr, b"")
            if os.name == "posix" and os.geteuid() != 0:
                self.assertEqual(self.w.read(cpu).strip(), "schedutil")
        finally:
            (self.w.root / cpu).chmod(0o644)
        # The control-key table names exactly these two actions.
        table = (LINUX.parent / "tools" / "nano-tree" / "control-keys.json")
        if table.is_file():
            routes = {r.get("route") for r in json.loads(table.read_text(encoding="utf-8"))["rows"]}
            self.assertLessEqual({"hook:fixedperf-on", "hook:fixedperf-off"}, routes)

    def test_d10cards_skips_the_emmc(self) -> None:
        self.w.standard()
        self.w.run(INITD + "/deferred/D10cards", "start")
        for name in ("mmcblk0", "mmcblk0p1", "mmcblk0p7"):
            self.assertNotEqual(self.w.read("sys/class/block/%s/uevent" % name), "add\n", name)
        for name in ("mmcblk1", "mmcblk1p4", "mmcblk2", "mmcblk2p4"):
            self.assertEqual(self.w.read("sys/class/block/%s/uevent" % name), "add\n", name)

    @needs_links
    def test_rcs_runs_s_scripts_in_order(self) -> None:
        self.w.standard()
        self.w.mounted_root("179:19")
        shutil.rmtree(str(self.w.root / "run"))
        (self.w.root / "run").mkdir()
        self.w.run(INITD + "/rcS", env={"THIMSY_FAKE_FAMILY": "rk3566"})
        marks = sorted(p.name for p in (self.w.root / "run/thimsy/boot").iterdir())
        self.assertEqual(marks, ["rcs-done", "s00-done", "s00-mounts", "s01-done", "s03-done"])
        self.assertIn("thimsy: rcS done", self.w.read("dev/kmsg"))
        tools = [c["tool"] for c in self.w.calls()]
        self.assertLess(tools.index("mount"), tools.index("thimsy-prop"))
        self.assertTrue((self.w.root / "run/thimsy/padd.pid").exists())
        self.assertEqual(self.w.read("run/thimsy/family"), "rk3566\n")


# --- D60firstboot -----------------------------------------------------------------

@needs_sh
class TestD60FirstBoot(WorldCase):
    D60 = INITD + "/deferred/D60firstboot"

    def setUp(self) -> None:
        super().setUp()
        self.w.standard()
        self.w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
        self.w.mounted_root("179:19")
        self.others = {n: self.w.disk_bytes(n) for n in ("mmcblk0", "mmcblk2")}

    def writes(self) -> List[List[str]]:
        return [c["args"] for c in self.w.calls()
                if c["tool"] in ("sfdisk", "mkfs.exfat") and c["args"][:1] != ["--dump"]
                and c.get("note") != "no power"]

    def assert_others_untouched(self) -> None:
        for n, data in self.others.items():
            self.assertEqual(self.w.disk_bytes(n), data, "%s changed" % n)
        self.assertNoCallMentions("mmcblk0", "mmcblk2")

    def assert_invariant(self) -> None:
        """No basic-data entry without a complete filesystem."""
        for name in ("mmcblk1",):
            for key, p in self.w.disk(name)["parts"].items():
                if p["type"] == BASICDATA:
                    self.assertTrue(p["fs"] and p["fs"]["complete"],
                                    "entry %s is basic data without a filesystem" % key)

    def assert_done(self) -> None:
        d = self.w.disk("mmcblk1")
        self.assertEqual(sorted(d["parts"], key=int), ["1", "2", "3", "4", "5"])
        p5 = d["parts"]["5"]
        data = d["parts"]["4"]
        self.assertEqual(p5["type"], BASICDATA)
        self.assertEqual(p5["name"], "games")
        self.assertEqual(p5["fs"], {"type": "exfat", "label": "THIMSY", "complete": True})
        self.assertGreaterEqual(p5["start"], data["start"] + data["size"])
        self.assertEqual(p5["start"] % MIB, 0)
        self.assertEqual(d["last_lba"], d["size_sectors"] - 34)
        card_end = (d["size_sectors"] - 34 + 1) // MIB * MIB
        self.assertEqual(p5["start"] + p5["size"], card_end)  # the rest of the card
        self.assertLessEqual(p5["start"] + p5["size"] - 1, d["last_lba"])
        for k in "1234":
            self.assertIsNone(d["parts"][k]["fs"])
        self.assert_others_untouched()

    def test_first_boot(self) -> None:
        self.w.run(self.D60, "start")
        self.assert_done()
        kinds = {"--relocate": "relocate", "-N": "create", "-L": "mkfs",
                 "--part-type": "retype"}
        steps = [next(kinds[a] for a in args if a in kinds) for args in self.writes()]
        self.assertEqual(steps, ["relocate", "create", "mkfs", "retype"])
        self.assertEqual(self.writes()[1], ["--force", "-N", "5", "/dev/mmcblk1"])
        self.assertEqual(self.writes()[2], ["-L", "THIMSY", "/dev/mmcblk1p5"])
        self.assertEqual(self.w.read("sys/class/block/mmcblk1p5/uevent"), "change\n")
        self.assertIn("games partition mmcblk1p5 ready", self.w.read("dev/kmsg"))

    def test_second_run_changes_nothing(self) -> None:
        self.w.run(self.D60, "start")
        before = self.w.disk_bytes("mmcblk1")
        self.w.clear_calls()
        self.w.reboot()
        self.w.run(self.D60, "start")
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.w.disk_bytes("mmcblk1"), before)

    def test_uses_the_disk_s00base_saved(self) -> None:
        self.w.mounted_root(None)
        self.w.cmdline("root=/dev/mmcblk2p3")  # would point elsewhere
        self.w.write("run/thimsy/bootdisk", "BOOT_DISK=mmcblk1\nROOT_PART=mmcblk1p3\n"
                     "DATA_PART=mmcblk1p4\nBOOT_HOW=mountinfo\nBOOT_EMMC=0\n")
        self.w.run(self.D60, "start")
        self.assert_done()

    def test_power_cut_at_every_step(self) -> None:
        cases = [(s, m) for s in ("relocate", "create", "mkfs", "retype")
                 for m in ("before", "after")] + [("mkfs", "torn")]
        for step, mode in cases:
            with self.subTest(step=step, mode=mode):
                w = World(Path(tempfile.mkdtemp(prefix="test-rcs-cut-")))
                self.addCleanup(shutil.rmtree, str(w.tmp), True)
                self.w = w
                w.standard()
                w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
                w.mounted_root("179:19")
                self.others = {n: w.disk_bytes(n) for n in ("mmcblk0", "mmcblk2")}
                w.run(self.D60, "start", env={"THIMSY_FAKE_POWERCUT": "%s:%s" % (step, mode)},
                      check=False)
                self.assertTrue((w.root / ".powercut").exists(), "the cut did not happen")
                self.assert_invariant()
                w.reboot()
                w.run(self.D60, "start")
                self.assert_done()
                self.assert_invariant()
                w.clear_calls()
                w.reboot()
                w.run(self.D60, "start")
                self.assertEqual(self.writes(), [])

    def test_power_cut_twice(self) -> None:
        for step in ("create", "mkfs"):
            self.w.run(self.D60, "start", env={"THIMSY_FAKE_POWERCUT": step + ":after"},
                       check=False)
            self.assert_invariant()
            self.w.reboot()
        self.w.run(self.D60, "start")
        self.assert_done()

    def test_emmc_boot_disk(self) -> None:
        self.w.mounted_root("179:4")
        self.w.run(self.D60, "start")
        self.assertEqual([c for c in self.w.calls() if c["tool"] == "sfdisk"], [])
        self.assertEqual(self.writes(), [])
        self.assert_others_untouched()

    def test_entry5_not_ours_is_left_alone(self) -> None:
        for typ, name in ((BASICDATA, "mystuff"), (LINUX_DATA, "games"), (RESERVED, "other")):
            with self.subTest(typ=typ, name=name):
                d = self.w.disk("mmcblk1")
                d["parts"]["5"] = {"start": IMAGE_END, "size": 512 * MIB, "type": typ,
                                   "uuid": "55555555-0000-4000-8000-000000000005", "name": name,
                                   "fs": None}
                fake.save_disk(self.w.root, d)
                before = self.w.disk_bytes("mmcblk1")
                self.w.clear_calls()
                self.w.reboot()
                self.w.run(self.D60, "start")
                self.assertEqual(self.writes(), [])
                self.assertEqual(self.w.disk_bytes("mmcblk1"), before)

    def test_no_room(self) -> None:
        d = self.w.disk("mmcblk1")
        d["size_sectors"] = IMAGE_END + 40 * MIB
        d["last_lba"] = d["size_sectors"] - 34
        fake.save_disk(self.w.root, d)
        self.w.reboot()
        self.w.run(self.D60, "start")
        self.assertEqual(self.writes(), [])
        self.assertIn("no games partition", self.w.boot_log())

    def test_sd_named_disk_and_4k_sectors(self) -> None:
        """A USB reader (sdX names, no device/type) with 4096-byte sectors."""
        w = World(Path(tempfile.mkdtemp(prefix="test-rcs-sd-")))
        self.addCleanup(shutil.rmtree, str(w.tmp), True)
        self.w = w
        parts = image_parts("33333333")
        for p in parts.values():
            p["start"] //= 8
            p["size"] //= 8
        w.add_disk("sda", 0, None, parts, 8 * 1024 * MIB // 8, last_lba=IMAGE_END // 8 + 256)
        d = w.disk("sda")
        d["sector_size"] = 4096
        d["major"] = 8
        fake.save_disk(w.root, d)
        fake.sync_kernel_view(w.root, "sda")
        w.cmdline("root=/dev/sda3")
        w.mounted_root(None)
        self.others = {}
        w.run(self.D60, "start")
        d = w.disk("sda")
        self.assertEqual(d["parts"]["5"]["type"], BASICDATA)
        self.assertEqual(d["parts"]["5"]["start"] % 256, 0)  # 1 MiB in 4 KiB sectors
        self.assertIn(["-L", "THIMSY", "/dev/sda5"], self.writes())


# --- post-build -------------------------------------------------------------------

@needs_sh
class TestPostBuild(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="test-rcs-pb-"))

    def tearDown(self) -> None:
        shutil.rmtree(str(self.tmp), ignore_errors=True)

    def sh(self, *args: str, env: Optional[Dict[str, str]] = None) -> subprocess.CompletedProcess:
        assert SH is not None
        e = dict(os.environ)
        if env:
            e.update(env)
        return subprocess.run(SH + list(args), env=e, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=60)

    def part(self, path: Path, body: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        write_lf(path, "#!/bin/sh\n" + body)
        os.chmod(path, 0o755)

    def test_runner_order_and_failure(self) -> None:
        board = self.tmp / "board"
        (board / "common").mkdir(parents=True)
        for name in ("post-build.sh", "post-image.sh"):
            shutil.copy(str(LINUX / "board/common" / name), str(board / "common" / name))
        out = self.tmp / "order.txt"
        rec = 'echo "${0##*/} $1" >> "%s"\n' % posix(out)
        self.part(board / "common/post-build.d/10-a.sh", rec)
        self.part(board / "common/post-build.d/30-c.sh", rec)
        self.part(board / "rk3566/post-build.d/20-b.sh", rec)
        self.part(board / "h700/post-build.d/25-x.sh", rec)
        r = self.sh(posix(board / "common/post-build.sh"), "/target", "--family=rk3566")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out.read_text(encoding="utf-8").split("\n"),
                         ["10-a.sh /target", "20-b.sh /target", "30-c.sh /target", ""])
        out.unlink()
        self.part(board / "common/post-build.d/15-fail.sh", "exit 7\n")
        r = self.sh(posix(board / "common/post-build.sh"), "/target")
        self.assertEqual(r.returncode, 7)
        self.assertEqual(out.read_text(encoding="utf-8"), "10-a.sh /target\n")
        r = self.sh(posix(board / "common/post-image.sh"), "/images")
        self.assertEqual(r.returncode, 0, r.stderr)  # no post-image.d: nothing to run

    def build_props(self, config: str, env: Dict[str, str]) -> Dict[str, str]:
        target = self.tmp / "target"
        target.mkdir(exist_ok=True)
        cfg = self.tmp / ".config"
        write_lf(cfg, config)
        e = {"BR2_CONFIG": posix(cfg), "BR2_EXTERNAL_THIMSY_PATH": ""}
        e.update(env)
        r = self.sh(posix(LINUX / "board/common/post-build.d/20-build-props.sh"), posix(target),
                    env=e)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = {}
        for line in (target / "usr/share/thimsy/build.props").read_text(
                encoding="utf-8").splitlines():
            if line and not line.startswith("#"):
                k, _, v = line.partition("=")
                out[k] = v
        return out

    def test_build_props(self) -> None:
        dev = self.build_props('BR2_ARCH="aarch64"\nBR2_TARGET_ENABLE_ROOT_LOGIN=y\n'
                               'BR2_DEFCONFIG="/x/configs/thimsy_v8_2_defconfig"\n',
                               {"THIMSY_VERSION": "0.1.0-alpha1", "SOURCE_DATE_EPOCH": "1764028800"})
        self.assertEqual(dev["ro.thimsy.build.type"], "dev")
        self.assertEqual(dev["ro.thimsy.build.version"], "0.1.0-alpha1")
        self.assertEqual(dev["ro.thimsy.build.target"], "thimsy_v8_2")
        self.assertEqual(dev["ro.thimsy.build.arch"], "aarch64")
        self.assertEqual(dev["ro.thimsy.build.date.utc"], "1764028800")
        self.assertEqual(dev["ro.thimsy.build.date"], "2025-11-25T00:00:00Z")
        rel = self.build_props('# BR2_TARGET_ENABLE_ROOT_LOGIN is not set\n',
                               {"THIMSY_BUILD_TARGET": "thimsy_v8_2_release",
                                "SOURCE_DATE_EPOCH": "0"})
        self.assertEqual(rel["ro.thimsy.build.type"], "release")
        self.assertEqual(rel["ro.thimsy.build.target"], "thimsy_v8_2_release")
        self.assertEqual(rel["ro.thimsy.build.version"], "0.0.0-dev")
        for k, v in rel.items():
            self.assertLessEqual(len(v), 91, k)

    def test_build_props_in_build_shs_container(self) -> None:
        """build-integration-2: in build.sh's container the external tree is linux/ alone (no
        .git), so the version and the commit come from what build.sh passes."""
        sha = "0123456789abcdef0123456789abcdef01234567"
        props = self.build_props('# BR2_TARGET_ENABLE_ROOT_LOGIN is not set\n',
                                 {"THIMSY_VERSION": "v0.1.0-alpha1", "THIMSY_GIT_SHA": sha,
                                  "BR2_EXTERNAL_THIMSY_PATH": posix(self.tmp), "SOURCE_DATE_EPOCH": "0"})
        self.assertEqual(props["ro.thimsy.build.version"], "v0.1.0-alpha1")
        self.assertEqual(props["ro.thimsy.build.commit"], sha[:12])
        bad = self.build_props("", {"THIMSY_GIT_SHA": "unknown", "SOURCE_DATE_EPOCH": "0"})
        self.assertEqual(bad["ro.thimsy.build.commit"], "")
        build = (LINUX / "build.sh").read_text(encoding="utf-8")
        self.assertIn('-e THIMSY_VERSION="$VERSION"', build)
        self.assertIn("export THIMSY_BUILD_TARGET THIMSY_GIT_SHA THIMSY_VERSION", build)

    @needs_links
    def test_permissions_and_getty(self) -> None:
        target = self.tmp / "target"
        shutil.copytree(str(OVERLAYS[0]), str(target))
        (target / "root").mkdir()
        for f in ("etc/init.d/rcS", "etc/init.d/S00base"):
            os.chmod(target / f, 0o644)
        cfg = self.tmp / ".config"
        script = posix(LINUX / "board/common/post-build.d/30-permissions.sh")
        cfg.write_text('BR2_TARGET_GENERIC_GETTY=y\nBR2_TARGET_GENERIC_GETTY_PORT="ttyS2"\n'
                       'BR2_TARGET_GENERIC_GETTY_BAUDRATE="1500000"\n', encoding="utf-8")
        for _ in range(2):  # idempotent: one getty line
            r = self.sh(script, posix(target), env={"BR2_CONFIG": posix(cfg)})
            self.assertEqual(r.returncode, 0, r.stderr)
        tab = (target / "etc/inittab").read_text(encoding="utf-8")
        self.assertEqual(tab.count("ttyS2::respawn:/sbin/getty -L ttyS2 1500000 vt100"), 1)
        self.assertEqual(os.readlink(str(target / "etc/localtime")),
                         "/storage/.config/thimsy/localtime")
        # /root stays a directory (Buildroot's device table has "/root d 700"; S00base
        # bind-mounts root's home over it), even where an earlier build made it a link.
        self.assertTrue((target / "root").is_dir())
        self.assertFalse((target / "root").is_symlink())
        os.rmdir(str(target / "root"))
        os.symlink("/storage/.config/thimsy/home/root", str(target / "root"))
        r = self.sh(script, posix(target), env={"BR2_CONFIG": posix(cfg)})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((target / "root").is_symlink())
        self.assertTrue((target / "root").is_dir())
        if os.name != "nt":
            self.assertEqual((target / "root").stat().st_mode & 0o777, 0o700)
        self.assertTrue((target / "storage").is_dir())
        if os.name != "nt":
            self.assertTrue(os.access(str(target / "etc/init.d/rcS"), os.X_OK))
        cfg.write_text("# BR2_TARGET_GENERIC_GETTY is not set\n", encoding="utf-8")
        r = self.sh(script, posix(target), env={"BR2_CONFIG": posix(cfg)})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("getty", (target / "etc/inittab").read_text(encoding="utf-8")
                         .split("# release images get none (decision I37).")[1])


if __name__ == "__main__":
    unittest.main()
