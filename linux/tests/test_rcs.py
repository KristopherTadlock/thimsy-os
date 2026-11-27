# SPDX-License-Identifier: MIT
"""Tests of the init scripts (IMPLEMENTATION D.4, test_rcs.py row).

Static: linux/tools/check_rcs.py passes on the tree, and catches what it
should on planted faults.

Run against a fake root (THIMSY_ROOT) with fake tools (THIMSY_TOOLDIR,
rcs_fake_tool.py), unprivileged:
  * S00base picks the boot disk (mounted root, root=PARTUUID=, root=/dev/)
    and mounts only its data partition, never a second card's or the eMMC's,
    and makes the first-boot tree;
  * D60firstboot makes the image's GPT GUIDs random, makes the games
    partition and retires info on the boot disk only, and reaches the same
    end state after a power cut at any step;
  * S01props, S03modules and D05family (the family hook), D10cards;
  * D50ssh, ssh-gate and thimsy-ssh-passwd (decision OA-3);
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
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional

HERE = Path(__file__).resolve().parent
LINUX = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(LINUX / "tools"))

import check_rcs  # noqa: E402
import rcs_fake_tool as fake  # noqa: E402
from sha_crypt import sha512_crypt  # noqa: E402

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


INFO_FS = {"type": "vfat", "label": "THIMSY-INFO", "complete": True}


def image_parts(uuid_prefix: str, pinned: bool = False) -> Dict[str, Dict[str, Any]]:
    """Entries 1 to 5 of the written image (IMPLEMENTATION E.6), with spares; info (entry 5,
    OA-2) is basic data with its FAT. The GUIDs are version 4 (random, as the installer or
    D60 leaves them), or with pinned=True version 5, as the image carries them (OA-9)."""
    layout = [("uboot", 16 * MIB, 8 * MIB, RESERVED), ("boot", 0, 40 * MIB, RESERVED),
              ("root", 0, 1024 * MIB, LINUX_ROOT), ("data", 0, 256 * MIB, LINUX_HOME),
              ("info", 0, 1 * MIB, BASICDATA)]
    parts = {}
    pos = 0
    spare = {"uboot": 8 * MIB, "boot": 40 * MIB, "root": 1024 * MIB, "data": 0, "info": 0}
    for n, (name, first, size, typ) in enumerate(layout, 1):
        pos = max(pos, first)
        parts[str(n)] = {"start": pos, "size": size, "type": typ,
                         "uuid": "%s-0000-%d000-8000-00000000000%d" % (
                             uuid_prefix, 5 if pinned else 4, n),
                         "name": name, "fs": dict(INFO_FS) if name == "info" else None}
        pos += size + spare[name]
    return parts


DATA_END = 16 * MIB + 8 * MIB + 8 * MIB + 40 * MIB + 40 * MIB + 1024 * MIB * 2 + 256 * MIB
IMAGE_END = DATA_END + MIB   # the end of info, the image's last entry
# The written image ends 1 MiB after info (genimage.cfg's size: 2410 MiB, its
# backup GPT in the last 33 sectors) and games starts 2 MiB after info.
IMAGE_SECTORS = IMAGE_END + MIB
GAMES_START = IMAGE_END + 2 * MIB


class World:
    """A fake root: /proc, /sys, /dev with fake disks, the overlay, fake tools."""

    PY_TOOLS = ["sfdisk", "mkfs.exfat"]
    SH_TOOLS = ["mount", "umount", "e2fsck", "sync", "modprobe", "thimsy-prop",
                "thimsy-padd", "amixer", "alsactl", "dd", "chown", "dropbear",
                "dropbearkey", "tcpsvd", "dbus-daemon", "dbus-uuidgen", "bluetoothd", "flock"]
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
tcpsvd | umount)
	# THIMSY_FAKE_FAIL=tcpsvd: it cannot listen; =umount: the unbind fails.
	case " ${THIMSY_FAKE_FAIL:-} " in
	*" $tool "*) exit 1 ;;
	esac
	;;
mount)
	case " ${THIMSY_FAKE_FAIL:-} " in
	*" mount-ext4 "*) [ "$1 $2" = "-t ext4" ] && exit 32 ;;
	esac
	;;
dd)
	# The disk's bytes are dev/<disk>.img (rcs_fake_tool.py); none: a blank
	# disk (zeros). THIMSY_FAKE_FAIL=dd: a read error.
	case " ${THIMSY_FAKE_FAIL:-} " in
	*" dd "*) exit 1 ;;
	esac
	n=$#
	while [ "$n" -gt 0 ]; do
		a=$1
		shift
		n=$((n - 1))
		case $a in
		if=*)
			a=if=${a#if=}.img
			[ -f "${a#if=}" ] || a=if=/dev/zero
			;;
		esac
		set -- "$@" "$a"
	done
	exec dd "$@"
	;;
dbus-daemon)
	# The bus is up once its socket is (D35bluetooth waits for it).
	case " ${THIMSY_FAKE_FAIL:-} " in
	*" dbus "*) exit 1 ;;
	esac
	mkdir -p "$R/run/dbus"
	: > "$R/run/dbus/system_bus_socket"
	;;
dropbearkey)
	while [ "$#" -gt 1 ]; do
		[ "$1" = -f ] && { printf 'key\n' > "$2"; break; }
		shift
	done
	;;
thimsy-prop)
	if [ "${1:-}" = boot ]; then
		mkdir -p "$R/run/thimsy/props/ro"
		[ -n "${THIMSY_FAKE_FAMILY:-}" ] &&
			printf '%s\n' "$THIMSY_FAKE_FAMILY" > "$R/run/thimsy/props/ro/ro.thimsy.family"
		[ -n "${THIMSY_FAKE_MODULES:-}" ] &&
			printf '%s\n' "$THIMSY_FAKE_MODULES" > "$R/run/thimsy/props/ro/ro.thimsy.modules"
		# THIMSY_FAKE_FAIL=thimsy-prop: sealed, with warnings (exit 2).
		case " ${THIMSY_FAKE_FAIL:-} " in
		*" thimsy-prop "*) exit 2 ;;
		esac
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
        # The kernel's random GUIDs (the fake sfdisk writes a new one after each GUID write).
        self.write("proc/sys/kernel/random/uuid", str(uuid.uuid4()) + "\n")
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
        # cryptpw: logged (its arguments only: the password comes on stdin), then a real
        # SHA-512 crypt with a fixed salt (sha_crypt.py). THIMSY_FAKE_FAIL=cryptpw: it fails.
        shutil.copy(str(HERE / "sha_crypt.py"), str(self.bin / "sha_crypt.py"))
        write_lf(self.bin / "cryptpw", """#!/bin/sh
{ printf 'cryptpw\\t'; for a in "$@"; do printf '\\t%%s' "$a"; done; printf '\\n'; } >> "$THIMSY_FAKE_LOG"
case " ${THIMSY_FAKE_FAIL:-} " in
*" cryptpw "*) exit 1 ;;
esac
exec "%s" -I -S "%s" "$@"
""" % (py, posix(self.bin / "sha_crypt.py")))
        os.chmod(str(self.bin / "cryptpw"), 0o755)
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
                      last_lba=IMAGE_SECTORS - 34)
        self.add_disk("mmcblk2", 32, "SD", image_parts("11111111" if clone else "22222222"),
                      32 * 1024 * MIB, last_lba=IMAGE_SECTORS - 34)

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
    def popen(self, rel: str, *args: str) -> subprocess.Popen:
        """rel in the background, with run()'s environment."""
        assert SH is not None
        return subprocess.Popen(SH + [posix(self.root / rel)] + list(args), env=self._env(None),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def _env(self, env: Optional[Dict[str, str]]) -> Dict[str, str]:
        e = dict(os.environ)
        e.update({"THIMSY_ROOT": posix(self.root), "THIMSY_TOOLDIR": posix(self.bin),
                  "THIMSY_FAKE_LOG": posix(self.log), "LC_ALL": "C"})
        for k in ("THIMSY_FAKE_POWERCUT", "THIMSY_FAKE_FAIL", "THIMSY_FAKE_FAMILY",
                  "THIMSY_FAKE_MODULES"):
            e.pop(k, None)
        e.update(self.env_extra)
        if env:
            e.update(env)
        return e

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

    def binds(self) -> List[List[str]]:
        return [c["args"] for c in self.w.calls() if c["tool"] == "mount" and "--bind" in c["args"]]

    def build_type(self, typ: str) -> None:
        self.w.write("usr/share/thimsy/build.props", "ro.thimsy.build.type=%s\n" % typ)

    def test_root_home_bound_from_storage(self) -> None:
        self.build_type("dev")
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        binds = self.binds()
        self.assertEqual(len(binds), 1, binds)
        self.assertEqual(binds[0][-1], "/root")
        # the data partition (ext4, mounted at /storage), never the games exFAT
        self.assertEqual(binds[0][-2], "/storage/.config/thimsy/home/root")
        seq = [c["args"][-1] for c in self.w.calls() if c["tool"] == "mount"]
        self.assertLess(seq.index("/storage"), seq.index("/root"))
        chown = [c["args"] for c in self.w.calls() if c["tool"] == "chown"]
        self.assertEqual(chown, [["0:0", "/storage/.config/thimsy/home/root",
                                  "/storage/.config/thimsy/home/root/.ssh"]])

    def test_root_home_modes_every_boot(self) -> None:
        """gfix2: dropbear refuses keys others can write; the modes are set at every boot."""
        self.build_type("dev")
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        home = self.w.root / "storage/.config/thimsy/home/root"
        keys = home / ".ssh" / "authorized_keys"
        keys.write_text("ssh-ed25519 AAAA test\n", encoding="utf-8")
        os.chmod(str(keys), 0o666)
        os.chmod(str(home / ".ssh"), 0o777)
        os.chmod(str(home), 0o755)
        self.w.clear_calls()
        self.w.reboot()
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(len(self.binds()), 1)
        self.assertEqual(home.stat().st_mode & 0o777, 0o700)
        self.assertEqual((home / ".ssh").stat().st_mode & 0o777, 0o700)
        self.assertEqual(keys.stat().st_mode & 0o777, 0o600)
        chown = [c["args"] for c in self.w.calls() if c["tool"] == "chown"]
        self.assertEqual(chown[0][-1], "/storage/.config/thimsy/home/root/.ssh/authorized_keys")

    def test_release_without_ssh_keeps_the_empty_root(self) -> None:
        for props in (None, "release"):
            with self.subTest(build_props=props):
                p = self.w.root / "usr/share/thimsy/build.props"
                if props is None:
                    if p.exists():
                        p.unlink()
                else:
                    self.build_type(props)
                self.w.clear_calls()
                self.w.reboot()
                self.w.mounted_root("179:19")
                self.w.run(INITD + "/S00base", "start")
                self.assertEqual(self.binds(), [])
                self.assertEqual([c for c in self.w.calls() if c["tool"] == "chown"], [])
                self.assertIn("SSH off: /root stays", self.w.boot_log())

    def test_release_with_ssh_turned_on_binds(self) -> None:
        self.build_type("release")
        self.w.write("storage/.config/thimsy/persist.props", "persist.thimsy.ssh.enabled=1\n")
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual([b[-1] for b in self.binds()], ["/root"])

    def test_root_home_link_is_not_followed(self) -> None:
        self.build_type("dev")
        self.w.mounted_root("179:19")
        self.w.run(INITD + "/S00base", "start")
        ssh = self.w.root / "storage/.config/thimsy/home/root/.ssh"
        os.rmdir(str(ssh))
        elsewhere = self.w.root / "storage/emulated/0/keys"
        elsewhere.mkdir(parents=True)
        os.symlink(str(elsewhere), str(ssh))
        self.w.clear_calls()
        self.w.reboot()
        self.w.run(INITD + "/S00base", "start")
        self.assertEqual(self.binds(), [])
        self.assertEqual([c for c in self.w.calls() if c["tool"] == "chown"], [])
        self.assertIn("is a link: root's home is not bound", self.w.read("dev/kmsg"))

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
        self.assertEqual(len(args), 9)
        opts = dict(zip(args[1::2], args[2::2]))
        self.assertEqual(sorted(opts), ["--build", "--defaults", "--device", "--install"])
        self.assertTrue(opts["--device"].endswith("/usr/share/thimsy/device.json"))
        self.assertTrue(opts["--build"].endswith("/usr/share/thimsy/build.props"))
        # G2e-14: the card's thimsy/ folder on the data partition, not /boot.
        self.assertTrue(opts["--install"].endswith("/storage/thimsy/install.json"))
        # OA-35: thimsy-data's copy of mods/nano-theme.props (thimsy-data.mk installs it there).
        self.assertTrue(opts["--defaults"].endswith("/usr/share/thimsy/persist-defaults.props"))

    def test_install_json_path_matches_thimsy_prop(self) -> None:
        # thimsy-prop's own default (for a shell user) is the path S01props passes.
        path = LINUX.parent / "libthimsy" / "tools" / "thimsy-prop.cpp"
        if not path.is_file():
            self.skipTest("no libthimsy/ beside linux/")
        src = path.read_text(encoding="utf-8")
        self.assertIn('envOr("THIMSY_INSTALL_JSON", "/storage/thimsy/install.json")', src)
        self.assertIn('envOr("THIMSY_DEVICE_JSON", "/usr/share/thimsy/device.json")', src)
        self.assertIn('envOr("THIMSY_BUILD_PROPS", "/usr/share/thimsy/build.props")', src)

    def test_defaults_are_seeded_by_thimsy_prop(self) -> None:
        """OA-35, OA-36: Android's theme settings (persist-defaults.props, from mods/nano-theme.props)
        are stored by thimsy-prop boot --defaults (libthimsy's C++, tested in thimsy_prop_test.cpp),
        not by a loop in the script: with a defaults file and a persist.props that lacks its keys,
        S01props makes one call, boot, and writes no setting itself."""
        self.w.write("usr/share/thimsy/persist-defaults.props",
                     "persist.gammaos.nano.ndstheme=3\npersist.gammaos.nano.nav_sounds=0\n")
        self.w.write("storage/.config/thimsy/persist.props", "persist.thimsy.wifi.enabled=1\n")
        self.w.run(INITD + "/S01props", "start")
        calls = [c["args"] for c in self.w.calls() if c["tool"] == "thimsy-prop"]
        self.assertEqual([a[0] for a in calls], ["boot"])
        self.assertIn("--defaults", calls[0])
        self.assertEqual(self.w.read("storage/.config/thimsy/persist.props"), "persist.thimsy.wifi.enabled=1\n")
        src = (OVERLAYS[0] / INITD / "S01props").read_text(encoding="utf-8")
        for gone in ("persist_defaults", "thimsy_persist", '"$PROP" set', "prefs: keys from"):
            self.assertNotIn(gone, src)

    def test_thimsy_prop_boot_failure_is_a_warning(self) -> None:
        # A defaults line thimsy-prop refuses makes it exit 2 (sealed, with warnings): S01props warns
        # and goes on to the family.
        self.w.run(INITD + "/S01props", "start", env={"THIMSY_FAKE_FAIL": "thimsy-prop"})
        self.assertIn("thimsy-prop boot failed", self.w.boot_log())
        self.assertEqual(self.w.read("run/thimsy/family"), "rk3566\n")

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


# --- D50ssh, ssh-gate, thimsy-ssh-passwd ------------------------------------------

D50SSH = LINUX / "board/common/rootfs-overlay" / INITD / "deferred/D50ssh"
GATE_REL = "usr/libexec/thimsy/ssh-gate"
PASSWD_REL = "usr/sbin/thimsy-ssh-passwd"
HOST_KEY = "/storage/.config/thimsy/ssh/dropbear_ed25519_host_key"
HASH_REL = "storage/.config/thimsy/ssh/root.hash"
SHADOW_REL = "run/thimsy/ssh/shadow"
IMAGE_SHADOW = ("root:*:19000:0:99999:7:::\n"
                "daemon:*:10933:0:99999:7:::\n"
                "nobody:*:10933:0:99999:7:::\n")


def default_hash() -> str:
    for line in D50SSH.read_text(encoding="utf-8").splitlines():
        if line.startswith("DEFAULT_HASH="):
            return line.split("=", 1)[1].strip("'")
    raise AssertionError("D50ssh has no DEFAULT_HASH")


class TestSshStatic(unittest.TestCase):
    """OA-3 on the image: the BusyBox applets, no key-only -s, the default password's hash."""

    def test_default_hash_is_thimsy(self) -> None:
        h = default_hash()
        self.assertTrue(h.startswith("$6$"), h)
        self.assertEqual(sha512_crypt("thimsy", h), h)
        # the password's text is in comments only, never in code
        for f in (D50SSH, LINUX / "board/common/rootfs-overlay" / PASSWD_REL,
                  LINUX / "board/common/rootfs-overlay" / GATE_REL):
            for line in f.read_text(encoding="utf-8").splitlines():
                code, _ = check_rcs.split_comment(line)
                self.assertNotRegex(code, r"""(?:^|[\s'"=])thimsy(?:$|[\s'"])""", f)

    def test_sha512_crypt_matches_the_specification(self) -> None:
        # Drepper's own test vectors.
        self.assertEqual(
            sha512_crypt("Hello world!", "$6$saltstring"),
            "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLi"
            "BFdcbYEdFCoEOfaS35inz1")
        self.assertEqual(
            sha512_crypt("Hello world!", "$6$rounds=10000$saltstringsaltstring"),
            "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/"
            "UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.")

    def test_dropbear_never_key_only(self) -> None:
        """OA-3 drops -s: password logins work in every build."""
        for line in D50SSH.read_text(encoding="utf-8").splitlines():
            code, _ = check_rcs.split_comment(line)
            if "$DROPBEAR" in code:
                self.assertNotRegex(code, r"(?:^|\s)-[a-zA-Z]*s\b", code)
                self.assertNotIn("opts", code)

    def test_busybox_applets(self) -> None:
        bb = (LINUX / "board/common/busybox.fragment").read_text(encoding="utf-8")
        for opt in ("CONFIG_TCPSVD=y", "CONFIG_FEATURE_IPV6=y", "CONFIG_CRYPTPW=y",
                    "CONFIG_USE_BB_CRYPT=y", "CONFIG_USE_BB_CRYPT_SHA=y", "CONFIG_STTY=y",
                    "CONFIG_MV=y", "CONFIG_FLOCK=y"):
            self.assertIn(opt, bb.splitlines())

    def test_helpers_checked_and_made_executable(self) -> None:
        self.assertEqual(sorted(check_rcs.HELPERS),
                         sorted("board/common/rootfs-overlay/" + r for r in (GATE_REL, PASSWD_REL)))
        perms = (LINUX / "board/common/post-build.d/30-permissions.sh").read_text(encoding="utf-8")
        for r in (GATE_REL, PASSWD_REL):
            self.assertIn('"$target"/' + r, perms)
        self.assertIn("tcpsvd", check_rcs.FORBIDDEN_SYNC)
        self.assertIn("TCPSVD", check_rcs.FORBIDDEN_SYNC_VARS)

    def test_dropbear_in_every_image(self) -> None:
        user = (LINUX / "configs/fragments/userland.fragment").read_text(encoding="utf-8")
        self.assertIn("BR2_PACKAGE_DROPBEAR=y", user.splitlines())


@needs_sh
class TestD50Ssh(WorldCase):
    """OA-3: SSH follows ro.thimsy.build.type and persist.thimsy.ssh.enabled (dev on, release off
    by default); tcpsvd runs dropbear -i per connection, behind ssh-gate unless
    persist.thimsy.ssh.any_network=1; while it runs, /etc/shadow is bound from a copy with root's
    hash (ssh/root.hash, else "thimsy")."""

    D50 = INITD + "/deferred/D50ssh"

    def setUp(self) -> None:
        super().setUp()
        self.w.write("etc/shadow", IMAGE_SHADOW)
        (self.w.root / "run/thimsy/log").mkdir(parents=True, exist_ok=True)
        self.dropbear = posix(self.w.bin / "dropbear")

    def props(self, **keys: Optional[str]) -> None:
        lines = ["persist.thimsy.ssh.%s=%s\n" % (k, v) for k, v in keys.items() if v is not None]
        p = self.w.root / "storage/.config/thimsy/persist.props"
        if lines:
            self.w.write("storage/.config/thimsy/persist.props", "".join(lines))
        elif p.exists():
            p.unlink()

    def build(self, build_type: Optional[str]) -> None:
        p = self.w.root / "usr/share/thimsy/build.props"
        if build_type is None:
            if p.exists():
                p.unlink()
        else:
            self.w.write("usr/share/thimsy/build.props", "ro.thimsy.build.type=%s\n" % build_type)

    def start(self, build_type: Optional[str], ssh: Optional[str],
              any_network: Optional[str] = None) -> List[List[str]]:
        self.build(build_type)
        self.props(enabled=ssh, any_network=any_network)
        self.w.clear_calls()
        self.w.run(self.D50, "start")
        return [c["args"] for c in self.w.calls() if c["tool"] == "tcpsvd"]

    def lan(self, addr: str = "0.0.0.0") -> List[str]:
        return ["-c", "10", addr, "22", "/" + GATE_REL, self.dropbear, "-i", "-r", HOST_KEY]

    def shadow(self) -> str:
        return self.w.read(SHADOW_REL)

    def test_policy(self) -> None:
        for build_type, ssh, on in (
                ("dev", None, True),
                ("dev", "1", True),
                ("dev", "0", False),
                ("release", None, False),
                ("release", "0", False),
                ("release", "1", True),
                (None, None, False),                    # no build.props: as release
                (None, "1", True)):
            with self.subTest(build_type=build_type, ssh=ssh):
                self.assertEqual(self.start(build_type, ssh), [self.lan()] if on else [])
                if not on:
                    self.assertEqual([c for c in self.w.calls() if c["tool"] == "mount"], [])

    def test_lan_only_unless_any_network(self) -> None:
        for any_network, want in ((None, self.lan()), ("0", self.lan()),
                                  ("1", ["-c", "10", "0.0.0.0", "22", self.dropbear, "-i", "-r",
                                         HOST_KEY])):
            with self.subTest(any_network=any_network):
                self.assertEqual(self.start("release", "1", any_network), [want])
                self.assertIn("%s network" % ("any" if any_network == "1" else "lan"),
                              self.w.boot_log())

    def test_listens_on_ipv6_when_the_kernel_has_it(self) -> None:
        self.w.write("proc/net/if_inet6", "00000000000000000000000000000001 01 80 10 80 lo\n")
        self.assertEqual(self.start("dev", None), [self.lan("::")])

    def test_never_key_only(self) -> None:
        for build_type in ("dev", "release"):
            with self.subTest(build_type=build_type):
                for args in self.start(build_type, "1"):
                    self.assertNotIn("-s", args)

    def test_default_password_in_the_bound_shadow(self) -> None:
        self.start("release", "1")
        lines = self.shadow().splitlines()
        self.assertEqual(lines[0], "root:%s:19000:0:99999:7:::" % default_hash())
        self.assertEqual(lines[1:], IMAGE_SHADOW.splitlines()[1:])
        binds = [c["args"] for c in self.w.calls() if c["tool"] == "mount" and "--bind" in c["args"]]
        self.assertIn(["--bind", "/" + SHADOW_REL, "/etc/shadow"], binds)
        # the image's own file is never written
        self.assertEqual(self.w.read("etc/shadow"), IMAGE_SHADOW)
        if os.name != "nt":
            self.assertEqual((self.w.root / SHADOW_REL).stat().st_mode & 0o777, 0o600)
            self.assertEqual((self.w.root / "run/thimsy/ssh").stat().st_mode & 0o777, 0o700)

    def test_users_hash_from_the_data_partition(self) -> None:
        h = sha512_crypt("correct horse", "$6$usersalt")
        self.w.write(HASH_REL, h + "\n")
        self.start("release", "1")
        self.assertEqual(self.shadow().splitlines()[0], "root:%s:19000:0:99999:7:::" % h)
        self.assertNotIn("correct horse", self.shadow())

    def test_a_bad_hash_locks_the_password(self) -> None:
        for text in ("not a hash\n", "$6$salt$abc:0:0\n", "", "$6$\n", "plain$6$x$y\n"):
            with self.subTest(text=text):
                self.w.write(HASH_REL, text)
                self.w.write("dev/kmsg", "")
                self.start("release", "1")
                self.assertEqual(self.shadow().splitlines()[0], "root:*:19000:0:99999:7:::")
                self.assertIn("password logins are off", self.w.read("dev/kmsg"))

    @needs_links
    def test_a_hash_file_that_is_a_link_locks_the_password(self) -> None:
        target = self.w.root / "storage/emulated/0/hash"
        target.parent.mkdir(parents=True)
        target.write_text(sha512_crypt("x", "$6$s") + "\n", encoding="utf-8")
        (self.w.root / HASH_REL).parent.mkdir(parents=True, exist_ok=True)
        os.symlink(str(target), str(self.w.root / HASH_REL))
        self.start("release", "1")
        self.assertEqual(self.shadow().splitlines()[0], "root:*:19000:0:99999:7:::")
        self.assertIn("is a link", self.w.read("dev/kmsg"))

    def test_a_shadow_without_root_gets_one(self) -> None:
        self.w.write("etc/shadow", "nobody:*:10933:0:99999:7:::")  # and no final newline
        self.start("dev", None)
        self.assertEqual(self.shadow(), "nobody:*:10933:0:99999:7:::\nroot:%s::0:99999:7:::\n"
                         % default_hash())

    def test_shadow_already_bound_is_rewritten_not_bound_again(self) -> None:
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.start("release", "1")
        self.assertEqual([c for c in self.w.calls() if c["tool"] == "mount"
                          and "/etc/shadow" in c["args"]], [])
        self.assertTrue(self.shadow().startswith("root:" + default_hash() + ":"))

    def test_roots_home_bound_when_turned_on_after_boot(self) -> None:
        """A release build with SSH off at boot left /root unbound; turning SSH on binds it, so
        authorized_keys works."""
        self.start("release", "1")
        binds = [c["args"][-2:] for c in self.w.calls() if c["tool"] == "mount"
                 and "--bind" in c["args"]]
        self.assertIn(["/storage/.config/thimsy/home/root", "/root"], binds)
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "/dev/mmcblk1p4 /root ext4 rw 0 0\n")
        self.start("release", "1")
        binds = [c["args"][-1] for c in self.w.calls() if c["tool"] == "mount"
                 and "--bind" in c["args"]]
        self.assertNotIn("/root", binds)

    def test_host_key_made_once(self) -> None:
        self.start("dev", None)
        keys = [c for c in self.w.calls() if c["tool"] == "dropbearkey"]
        self.assertEqual(len(keys), 1)
        self.assertTrue((self.w.root / HOST_KEY.lstrip("/")).is_file())
        self.start("dev", None)
        self.assertEqual([c for c in self.w.calls() if c["tool"] == "dropbearkey"], [])

    def test_without_tcpsvd(self) -> None:
        if shutil.which("tcpsvd"):
            self.skipTest("this host has a tcpsvd on PATH")
        (self.w.bin / "tcpsvd").unlink()
        self.build("dev")
        self.props()
        r = self.w.run(self.D50, "start", check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn("tcpsvd is not on this image", self.w.read("dev/kmsg"))

    def test_stop_unbinds_the_shadow(self) -> None:
        self.start("release", "1")
        self.assertTrue((self.w.root / SHADOW_REL).exists())
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.w.clear_calls()
        self.w.run(self.D50, "stop")
        self.assertEqual([c["args"] for c in self.w.calls() if c["tool"] == "umount"],
                         [["/etc/shadow"]])
        self.assertFalse((self.w.root / SHADOW_REL).exists())

    def test_tcpsvd_that_cannot_listen_fails_and_undoes_the_shadow(self) -> None:
        self.build("release")
        self.props(enabled="1")
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.w.clear_calls()
        r = self.w.run(self.D50, "start", check=False, env={"THIMSY_FAKE_FAIL": "tcpsvd"})
        self.assertEqual(r.returncode, 1)
        self.assertIn("tcpsvd exited (1)", self.w.read("dev/kmsg"))
        self.assertNotIn("SSH on", self.w.boot_log())
        self.assertEqual([c["args"] for c in self.w.calls() if c["tool"] == "umount"],
                         [["/etc/shadow"]])
        self.assertFalse((self.w.root / SHADOW_REL).exists())
        self.assertFalse((self.w.root / "run/thimsy/ssh/tcpsvd.pid").exists())

    def test_a_failed_unbind_keeps_the_bound_file_current(self) -> None:
        """If stop cannot unbind, the bound file stays, so a later start's hash reaches the
        live /etc/shadow."""
        self.start("release", "1")
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.w.run(self.D50, "stop", env={"THIMSY_FAKE_FAIL": "umount"})
        self.assertIn("could not unbind /etc/shadow", self.w.read("dev/kmsg"))
        self.assertTrue((self.w.root / SHADOW_REL).exists())
        h = sha512_crypt("later", "$6$s3")
        self.w.write(HASH_REL, h + "\n")
        self.start("release", "1")
        self.assertEqual([c for c in self.w.calls() if c["tool"] == "mount"
                          and "/etc/shadow" in c["args"]], [])
        self.assertEqual(self.shadow().splitlines()[0], "root:%s:19000:0:99999:7:::" % h)

    def test_stop_without_a_start(self) -> None:
        self.w.run(self.D50, "stop")
        self.assertEqual([c for c in self.w.calls() if c["tool"] in ("umount", "tcpsvd")], [])

    @unittest.skipUnless(os.name == "posix", "the shell's kill needs the processes' own pids")
    def test_stop_ends_tcpsvd_and_start_does_not_double_it(self) -> None:
        log = self.w.root / "term.log"
        script = 'trap \'echo tcpsvd >> "%s"; exit 0\' TERM; while :; do sleep 0.05; done' % (
            posix(log))
        proc = subprocess.Popen(SH + ["-c", script])
        try:
            time.sleep(0.3)
            self.w.write("run/thimsy/ssh/tcpsvd.pid", "%d\n" % proc.pid)
            self.w.write("proc/%d/comm" % proc.pid, "tcpsvd\n")
            self.assertEqual(self.start("dev", None), [])
            self.w.run(self.D50, "stop")
            proc.wait(timeout=10)
            self.assertEqual(log.read_text(encoding="utf-8").split(), ["tcpsvd"])
        finally:
            if proc.poll() is None:
                proc.kill()

    def test_password_action(self) -> None:
        h = sha512_crypt("new one", "$6$s2")
        # SSH off (no shadow bound): nothing is written.
        self.w.write(HASH_REL, h + "\n")
        self.w.run(self.D50, "password")
        self.assertFalse((self.w.root / SHADOW_REL).exists())
        # SSH on: the bound copy is rewritten in place.
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.w.run(self.D50, "password")
        self.assertEqual(self.shadow().splitlines()[0], "root:%s:19000:0:99999:7:::" % h)

    def test_no_build_option_left(self) -> None:
        cfg = (LINUX / "package/thimsy-session/Config.in").read_text(encoding="utf-8")
        for sym in ("SSH_AT_BOOT", "SSH_PASSWORD"):
            self.assertNotIn(sym, cfg)
            for frag in (LINUX / "configs").rglob("*"):
                if frag.is_file():
                    self.assertNotIn(sym, frag.read_text(encoding="utf-8"), frag)


@needs_sh
class TestSshGate(WorldCase):
    """ssh-gate runs its program only for a peer on the local network (OA-3's ranges)."""

    ALLOWED = [
        "10.1.2.3:51000", "10.255.255.255:1", "172.16.0.1:2", "172.20.1.1:2", "172.31.255.255:2",
        "192.168.1.20:40000", "169.254.3.4:1", "127.0.0.1:5",
        "[::1]:22", "[fd12:3456::1]:22", "[fc00::1]:22", "[fdff:ffff::9]:22",
        "[fe80::1%wlan0]:22", "[FE80::ABCD%wlan0]:22", "[febf::1]:22",
        "[::ffff:192.168.0.5]:22", "[::ffff:10.0.0.7]:22",
        "10.0.0.1", "192.168.4.4",                  # no port
    ]
    REFUSED = [
        "8.8.8.8:53", "1.10.0.1:1", "100.64.0.1:1", "11.0.0.1:1", "172.15.255.255:1",
        "172.32.0.1:1", "192.169.0.1:1", "192.16.8.1:1", "169.255.0.1:1", "0.0.0.0:1",
        "[2001:db8::1]:22", "[2a00:1450::5]:22", "[fec0::1]:22", "[fe7f::1]:22", "[fb00::1]:22",
        "[fc::1]:22", "[fe8::1]:22", "[::]:22", "[::ffff:8.8.8.8]:22", "[::2]:22",
        "8.8.8.8", "", "garbage", "[10.0.0.1]", "fe80::1",
    ]

    def gate(self, addr: Optional[str]) -> bool:
        env = {"TCPREMOTEADDR": addr} if addr is not None else {}
        e = self.w._env(env)
        if addr is None:
            e.pop("TCPREMOTEADDR", None)
        self.w.clear_calls()
        r = subprocess.run(SH + [posix(self.w.root / GATE_REL), posix(self.w.bin / "dropbear"),
                                 "-i", "-r", "KEY"], env=e, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=60)
        ran = [c["args"] for c in self.w.calls() if c["tool"] == "dropbear"]
        if ran:
            self.assertEqual(r.returncode, 0)
            self.assertEqual(ran, [["-i", "-r", "KEY"]])
            return True
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(r.stdout, b"")  # nothing said to the peer
        return False

    def test_allowed(self) -> None:
        for addr in self.ALLOWED:
            with self.subTest(addr=addr):
                self.assertTrue(self.gate(addr))

    def test_refused(self) -> None:
        for addr in self.REFUSED + [None]:
            with self.subTest(addr=addr):
                self.assertFalse(self.gate(addr))

    def test_no_program(self) -> None:
        r = subprocess.run(SH + [posix(self.w.root / GATE_REL)],
                           env=self.w._env({"TCPREMOTEADDR": "10.0.0.1:1"}),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        self.assertEqual(r.returncode, 2)


@needs_sh
class TestSshPasswd(WorldCase):
    """thimsy-ssh-passwd keeps only a hash, 0600, on the data partition; --default removes it;
    SSH takes it at once when it runs."""

    def passwd(self, *args: str, stdin: bytes = b"", env: Optional[Dict[str, str]] = None,
               check: bool = True) -> subprocess.CompletedProcess:
        self.w.clear_calls()
        r = subprocess.run(SH + [posix(self.w.root / PASSWD_REL)] + list(args),
                           env=self.w._env(env), input=stdin, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=60)
        if check and r.returncode != 0:
            raise AssertionError("exit %d: %s" % (r.returncode, r.stderr.decode(errors="replace")))
        return r

    def stored(self) -> str:
        return self.w.read(HASH_REL)

    def test_sets_a_hash_never_the_text(self) -> None:
        self.passwd("--stdin", stdin=b"s3cret pass\n")
        self.assertEqual(self.stored(), sha512_crypt("s3cret pass", "$6$FakeSalt") + "\n")
        self.assertEqual([c["args"] for c in self.w.calls() if c["tool"] == "cryptpw"],
                         [["-m", "sha512"]])
        self.assertNotIn("s3cret", self.w.log.read_text(encoding="utf-8"))
        for f in (self.w.root / "storage").rglob("*"):
            if f.is_file():
                self.assertNotIn(b"s3cret", f.read_bytes(), f)
        self.assertFalse((self.w.root / (HASH_REL + ".new")).exists())
        if os.name != "nt":
            self.assertEqual((self.w.root / HASH_REL).stat().st_mode & 0o777, 0o600)
            self.assertEqual((self.w.root / HASH_REL).parent.stat().st_mode & 0o777, 0o700)

    def test_last_line_without_newline(self) -> None:
        self.passwd("--stdin", stdin=b"no newline")
        self.assertEqual(self.stored(), sha512_crypt("no newline", "$6$FakeSalt") + "\n")

    def test_default_removes_the_hash(self) -> None:
        self.passwd("--stdin", stdin=b"abc\n")
        self.passwd("--default")
        self.assertFalse((self.w.root / HASH_REL).exists())
        self.passwd("--default")  # again: nothing to remove

    def test_refusals_change_nothing(self) -> None:
        self.passwd("--stdin", stdin=b"first\n")
        old = self.stored()
        for args, stdin, env in ((["--stdin"], b"\n", None),
                                 (["--stdin"], b"", None),
                                 ([], b"typed\n", None),          # not a terminal
                                 (["--stdin"], b"x\n", {"THIMSY_FAKE_FAIL": "cryptpw"}),
                                 (["--bogus"], b"", None),
                                 (["--stdin", "extra"], b"y\n", None)):
            with self.subTest(args=args, stdin=stdin, env=env):
                r = self.passwd(*args, stdin=stdin, env=env, check=False)
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(self.stored(), old)

    def test_running_ssh_takes_it_at_once(self) -> None:
        self.w.write("etc/shadow", IMAGE_SHADOW)
        self.w.write("proc/mounts", "/dev/root / erofs ro 0 0\n"
                     "tmpfs /etc/shadow tmpfs rw 0 0\n")
        self.passwd("--stdin", stdin=b"swordfish\n")
        self.assertEqual(self.w.read(SHADOW_REL).splitlines()[0],
                         "root:%s:19000:0:99999:7:::" % sha512_crypt("swordfish", "$6$FakeSalt"))
        self.passwd("--default")
        self.assertEqual(self.w.read(SHADOW_REL).splitlines()[0],
                         "root:%s:19000:0:99999:7:::" % default_hash())

    def test_ssh_off_writes_no_shadow(self) -> None:
        self.passwd("--stdin", stdin=b"swordfish\n")
        self.assertFalse((self.w.root / SHADOW_REL).exists())


class TestD20Sound(WorldCase):
    """D20sound waits for device.json's audio.card_index (ro.thimsy.device.audio.card_index), the
    card the session's ALSA_CARD and asound.conf use, and warns when device.json's audio.card is
    not that card or sits at another index (the ARCs: card 1 behind HDMI's card 0, a Phase 0
    check)."""

    D20 = INITD + "/deferred/D20sound"

    def setUp(self) -> None:
        super().setUp()
        for d in ("run/thimsy/props/ro", "run/thimsy/log", "storage/.config/thimsy"):
            (self.w.root / d).mkdir(parents=True, exist_ok=True)
        conf = self.w.read("etc/thimsy/init.conf")
        self.w.write("etc/thimsy/init.conf", conf + "THIMSY_SOUND_WAIT_S=1\n")

    def device(self, index: Optional[str], name: Optional[str]) -> None:
        for key, v in (("card_index", index), ("card", name)):
            rel = "run/thimsy/props/ro/ro.thimsy.device.audio." + key
            if v is None:
                p = self.w.root / rel
                if p.exists():
                    p.unlink()
            else:
                self.w.write(rel, v + "\n")

    def cards(self, *ids: str) -> None:
        for n, i in enumerate(ids):
            self.w.write("proc/asound/card%d/id" % n, i + "\n")

    def alsactl(self) -> List[List[str]]:
        return [c["args"] for c in self.w.calls() if c["tool"] == "alsactl"]

    def kmsg(self) -> str:
        return self.w.read("dev/kmsg")

    def test_the_arcs_card_one_behind_hdmi(self) -> None:
        self.device("1", "rk817_ext")
        self.cards("hdmisound", "rk817ext")
        self.w.write("storage/.config/thimsy/asound.state", "state\n")
        self.w.run(self.D20, "start")
        self.assertEqual(self.alsactl(), [["-f", "/storage/.config/thimsy/asound.state", "restore"]])
        self.assertEqual(self.kmsg(), "")
        self.w.clear_calls()
        self.w.run(self.D20, "stop")
        self.assertEqual(self.alsactl(), [["-f", "/storage/.config/thimsy/asound.state", "store"]])

    def test_no_card_index_is_card_zero(self) -> None:
        self.device(None, None)
        self.cards("rk817ext")
        self.w.write("storage/.config/thimsy/asound.state", "state\n")
        self.w.run(self.D20, "start")
        self.assertEqual(len(self.alsactl()), 1)
        self.assertEqual(self.kmsg(), "")

    def test_a_wrong_index_is_reported(self) -> None:
        # device.json says card 1, but the kernel made the codec card 0 (no HDMI card first).
        self.device("1", "rk817_ext")
        self.cards("rk817ext")
        self.w.run(self.D20, "start")
        self.assertEqual(self.alsactl(), [])
        self.assertIn("no sound card (card1)", self.w.boot_log())
        self.assertIn("rk817_ext is card0: device.json's audio.card_index 1 is wrong", self.kmsg())
        self.w.clear_calls()
        self.w.run(self.D20, "stop")
        self.assertEqual(self.alsactl(), [])

    def test_another_card_at_the_index_is_reported(self) -> None:
        self.device("0", "rk817_ext")
        self.cards("hdmisound", "rk817ext")
        self.w.run(self.D20, "start")
        self.assertIn("card0 is 'hdmisound', not device.json's audio.card rk817_ext", self.kmsg())
        self.assertIn("rk817_ext is card1: device.json's audio.card_index 0 is wrong", self.kmsg())

    def test_a_bad_index_falls_back_to_card_zero(self) -> None:
        self.device("one", None)
        self.cards("rk817ext")
        self.w.write("storage/.config/thimsy/asound.state", "state\n")
        self.w.run(self.D20, "start")
        self.assertEqual(len(self.alsactl()), 1)
        self.assertIn("audio.card_index 'one' is not a card number", self.w.boot_log())


class TestD35Bluetooth(WorldCase):
    """D35bluetooth: D-Bus, then bluetoothd, only when device.json says "bluetooth": true and the
    user turned Bluetooth on (persist.thimsy.bluetooth.enabled=1); stop ends bluetoothd, then
    the bus."""

    D35 = INITD + "/deferred/D35bluetooth"
    DAEMONS = ("dbus-daemon", "dbus-uuidgen", "bluetoothd")

    def setUp(self) -> None:
        super().setUp()
        for d in ("run/thimsy/props/ro", "run/thimsy/log", "storage/.config/thimsy"):
            (self.w.root / d).mkdir(parents=True, exist_ok=True)
        conf = self.w.read("etc/thimsy/init.conf")
        self.w.write("etc/thimsy/init.conf", conf + "THIMSY_BT_WAIT_S=1\n")

    def device(self, has_bt: Optional[str], enabled: Optional[str], controller: bool = True,
               kernel: bool = True) -> None:
        if has_bt is not None:
            self.w.write("run/thimsy/props/ro/ro.thimsy.device.bluetooth", has_bt + "\n")
        if enabled is not None:
            self.w.write("storage/.config/thimsy/persist.props",
                         "persist.thimsy.bluetooth.enabled=%s\n" % enabled)
        if kernel:
            (self.w.root / "sys/class/bluetooth").mkdir(parents=True, exist_ok=True)
        if controller:
            (self.w.root / "sys/class/bluetooth/hci0").mkdir(parents=True, exist_ok=True)

    def daemon_calls(self):
        return [(c["tool"], c["args"]) for c in self.w.calls() if c["tool"] in self.DAEMONS]

    def test_started_only_with_the_hardware_and_the_switch(self) -> None:
        # 3: this device has no Bluetooth (Nano turns its switch back off).
        for has_bt, enabled, code in (("true", None, 0), ("true", "0", 0), (None, "1", 3),
                                      ("false", "1", 3)):
            with self.subTest(has_bt=has_bt, enabled=enabled):
                self.tearDown()
                self.setUp()
                self.device(has_bt, enabled)
                r = self.w.run(self.D35, "start", check=False)
                self.assertEqual(r.returncode, code)
                self.assertEqual(self.daemon_calls(), [])
                self.assertFalse((self.w.root / "run/dbus-daemon.pid").exists())

    def test_on_starts_the_bus_then_bluetoothd(self) -> None:
        self.device("true", "1")
        self.w.run(self.D35, "start")
        # Both daemons are background jobs: their fakes may log after the script has returned.
        end = time.monotonic() + 10
        while len(self.daemon_calls()) < 3 and time.monotonic() < end:
            time.sleep(0.05)
        self.assertEqual(self.daemon_calls(),
                         [("dbus-uuidgen", ["--ensure"]),
                          ("dbus-daemon", ["--system", "--nofork", "--nopidfile"]),
                          ("bluetoothd", ["-n"])])
        for pid in ("run/dbus-daemon.pid", "run/bluetoothd.pid"):
            self.assertTrue(self.w.read(pid).strip().isdigit(), pid)
        self.assertTrue((self.w.root / "storage/.config/thimsy/bluetooth").is_dir())
        self.assertIn("D-Bus and bluetoothd started", self.w.boot_log())
        self.w.run(self.D35, "stop")
        for pid in ("run/dbus-daemon.pid", "run/bluetoothd.pid"):
            self.assertFalse((self.w.root / pid).exists(), pid)

    def test_no_controller(self) -> None:
        self.device("true", "1", controller=False)
        t0 = time.monotonic()
        r = self.w.run(self.D35, "start", check=False)
        self.assertGreaterEqual(time.monotonic() - t0, 0.9)  # THIMSY_BT_WAIT_S=1
        self.assertEqual(r.returncode, 3)
        self.assertEqual(self.daemon_calls(), [])
        self.assertIn("there is no controller", self.w.boot_log())

    def test_no_bluetooth_in_the_kernel_does_not_wait(self) -> None:
        conf = self.w.read("etc/thimsy/init.conf")
        self.w.write("etc/thimsy/init.conf", conf + "THIMSY_BT_WAIT_S=30\n")
        self.device("true", "1", controller=False, kernel=False)
        t0 = time.monotonic()
        r = self.w.run(self.D35, "start", check=False)
        self.assertLess(time.monotonic() - t0, 15)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(self.daemon_calls(), [])
        self.assertIn("the kernel has no Bluetooth", self.w.boot_log())

    def test_no_bus_no_bluetoothd(self) -> None:
        self.device("true", "1")
        r = self.w.run(self.D35, "start", check=False, env={"THIMSY_FAKE_FAIL": "dbus"})
        self.assertEqual(r.returncode, 1)
        self.assertNotIn("bluetoothd", [t for t, _ in self.daemon_calls()])
        self.assertIn("the D-Bus system bus did not start", self.w.read("dev/kmsg"))
        self.assertFalse((self.w.root / "run/dbus-daemon.pid").exists())

    def test_stop_without_a_start(self) -> None:
        self.w.run(self.D35, "stop")
        self.assertEqual(self.daemon_calls(), [])

    @unittest.skipUnless(os.name == "posix", "the shell's kill needs the processes' own pids")
    def test_stop_ends_bluetoothd_then_the_bus(self) -> None:
        # Two real processes stand in for the daemons, each noting its TERM; the fake /proc says
        # which daemon each is.
        log = self.w.root / "order.log"
        procs = []
        for name in ("bluetoothd", "dbus-daemon"):
            script = 'trap \'echo %s >> "%s"; exit 0\' TERM; while :; do sleep 0.05; done' % (
                name, posix(log))
            procs.append(subprocess.Popen(SH + ["-c", script]))
        try:
            time.sleep(0.5)
            self.w.write("run/bluetoothd.pid", "%d\n" % procs[0].pid)
            self.w.write("run/dbus-daemon.pid", "%d\n" % procs[1].pid)
            self.w.write("proc/%d/comm" % procs[0].pid, "bluetoothd\n")
            self.w.write("proc/%d/comm" % procs[1].pid, "dbus-daemon\n")
            self.w.run(self.D35, "stop")
            for p in procs:
                p.wait(timeout=10)
            self.assertEqual(log.read_text(encoding="utf-8").split(), ["bluetoothd", "dbus-daemon"])
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill()


    @unittest.skipUnless(os.name == "posix", "the shell's kill needs the processes' own pids")
    def test_a_pid_that_is_not_the_daemon_is_left_alone(self) -> None:
        # w5-linux-4: a daemon that died leaves its pidfile, and the pid may be another process's by
        # now. stop does not signal it, and start does not take it for bluetoothd.
        other = subprocess.Popen(["sleep", "60"])
        try:
            self.w.write("proc/%d/comm" % other.pid, "sleep\n")
            for pid in ("run/bluetoothd.pid", "run/dbus-daemon.pid"):
                self.w.write(pid, "%d\n" % other.pid)
            self.w.run(self.D35, "stop")
            time.sleep(0.3)
            self.assertIsNone(other.poll())
            for pid in ("run/bluetoothd.pid", "run/dbus-daemon.pid"):
                self.assertFalse((self.w.root / pid).exists(), pid)
            self.device("true", "1")
            for pid in ("run/bluetoothd.pid", "run/dbus-daemon.pid"):
                self.w.write(pid, "%d\n" % other.pid)
            self.w.run(self.D35, "start")
            end = time.monotonic() + 10
            while len(self.daemon_calls()) < 3 and time.monotonic() < end:
                time.sleep(0.05)
            self.assertEqual([t for t, _ in self.daemon_calls()], ["dbus-uuidgen", "dbus-daemon", "bluetoothd"])
            self.assertNotEqual(self.w.read("run/bluetoothd.pid").strip(), str(other.pid))
            self.assertIsNone(other.poll())
        finally:
            other.kill()
            other.wait()

    @unittest.skipUnless(os.name == "posix" and shutil.which("flock") is not None,
                         "needs flock(1) and fcntl")
    def test_one_start_or_stop_at_a_time(self) -> None:
        # w5-linux-5: rcD's start and Nano's switch may overlap. Each waits for the lock; start then
        # reads the switch again.
        import fcntl
        (self.w.bin / "flock").unlink()  # the real flock(1)
        self.device("true", "1")
        lock = self.w.root / "run/thimsy/bluetooth.lock"
        with open(str(lock), "a") as held:
            fcntl.flock(held.fileno(), fcntl.LOCK_EX)
            start = self.w.popen(self.D35, "start")
            stop = self.w.popen(self.D35, "stop")
            time.sleep(0.7)
            self.assertIsNone(start.poll())
            self.assertIsNone(stop.poll())
            self.assertEqual(self.daemon_calls(), [])
            # The switch went off meanwhile (its stop is the one waiting).
            self.w.write("storage/.config/thimsy/persist.props", "persist.thimsy.bluetooth.enabled=0\n")
            fcntl.flock(held.fileno(), fcntl.LOCK_UN)
        self.assertEqual(start.wait(timeout=30), 0)
        self.assertEqual(stop.wait(timeout=30), 0)
        for p in (start, stop):
            p.stdout.close()
            p.stderr.close()
        self.assertEqual(self.daemon_calls(), [])
        self.assertFalse((self.w.root / "run/bluetoothd.pid").exists())


class TestStateLinks(unittest.TestCase):
    """post-build.d/15-state-links.sh: /var/lib/bluetooth links to the data partition."""

    SCRIPT = LINUX / "board/common/post-build.d/15-state-links.sh"
    LINK = "/storage/.config/thimsy/bluetooth"

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="test-rcs-links-"))
        self.target = self.tmp / "target"
        self.target.mkdir()

    def tearDown(self) -> None:
        shutil.rmtree(str(self.tmp), ignore_errors=True)

    def run_part(self) -> subprocess.CompletedProcess:
        assert SH is not None
        return subprocess.run(SH + [posix(self.SCRIPT), posix(self.target)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=60)

    @needs_links
    def test_link_made_kept_and_replacing_an_empty_dir(self) -> None:
        (self.target / "var/lib/bluetooth").mkdir(parents=True)  # BlueZ's install makes one
        for _ in range(2):  # re-runnable
            r = self.run_part()
            self.assertEqual(r.returncode, 0, r.stderr)
            p = self.target / "var/lib/bluetooth"
            self.assertTrue(os.path.islink(str(p)))
            self.assertEqual(os.readlink(str(p)), self.LINK)

    def test_files_in_the_way_fail(self) -> None:
        d = self.target / "var/lib/bluetooth"
        d.mkdir(parents=True)
        write_lf(d / "settings", "x\n")
        r = self.run_part()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(b"would be hidden", r.stderr)


# --- D60firstboot -----------------------------------------------------------------

class TestTornGpt(unittest.TestCase):
    """device-safety-1: an sfdisk write cut off between the primary entries and the primary header
    leaves only the backup GPT valid. The kernel reads it only with "gpt" on its command line, rcK's
    TERM to everyone never reaches D60's sfdisk, and rcK waits for D60's lock before its KILL."""

    def test_kernel_falls_back_to_the_backup_gpt(self) -> None:
        sys.path.insert(0, str(LINUX / "tools"))
        try:
            import fit
        finally:
            sys.path.pop(0)
        self.assertIn(" gpt ", " %s " % fit.FAMILIES["rk3566"]["bootargs"])
        qemu = (LINUX / "board/qemu-aarch64-virt/run-qemu.sh").read_text(encoding="utf-8")
        self.assertRegex(qemu, r'cmdline="root=/dev/vda3 [^"]* gpt ')

    def test_d60_ignores_term_and_rck_waits_for_it(self) -> None:
        initd = LINUX / "board/common/rootfs-overlay" / INITD
        d60 = (initd / "deferred/D60firstboot").read_text(encoding="utf-8")
        start = d60.index("\nstart)")
        self.assertLess(start, d60.index("trap '' TERM", start))
        self.assertLess(d60.index("trap '' TERM", start), d60.index("\tfirstboot\n", start))
        rck = (initd / "rcK").read_text(encoding="utf-8")
        self.assertLess(rck.index('[ -d "$R/run/thimsy/firstboot.lock" ]'), rck.index("kill -TERM -1"))
        self.assertIn("firstboot.lock", d60)


@needs_sh
class D60Case(WorldCase):
    """D60firstboot's world: the standard disks, booted from mmcblk1."""

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

    def steps(self) -> List[str]:
        """The writes by kind: guid, relocate, create, mkfs, retype (games), retire (info)."""
        out = []
        for args in self.writes():
            if "--disk-id" in args or "--part-uuid" in args:
                out.append("guid")
            elif "--part-type" in args:
                out.append("retire" if args[args.index("--part-type") + 2] == "5" else "retype")
            else:
                kinds = {"--relocate": "relocate", "-N": "create", "-L": "mkfs"}
                out.append(next(kinds[a] for a in args if a in kinds))
        return out

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
        self.assertEqual(sorted(d["parts"], key=int), ["1", "2", "3", "4", "5", "6"])
        p6 = d["parts"]["6"]
        info = d["parts"]["5"]
        self.assertEqual(p6["type"], BASICDATA)
        self.assertEqual(p6["name"], "games")
        self.assertEqual(p6["fs"], {"type": "exfat", "label": "THIMSY", "complete": True})
        self.assertGreaterEqual(p6["start"], info["start"] + info["size"])
        self.assertEqual(p6["start"] % MIB, 0)
        # GAP_MIB after info: the image ends 1 MiB after info (device-safety-2).
        self.assertEqual(p6["start"], GAMES_START)
        self.assertEqual(d["last_lba"], d["size_sectors"] - 34)
        card_end = (d["size_sectors"] - 34 + 1) // MIB * MIB
        self.assertEqual(p6["start"] + p6["size"], card_end)  # the rest of the card
        self.assertLessEqual(p6["start"] + p6["size"] - 1, d["last_lba"])
        # info (OA-2) is retired: games is the one entry vold mounts; its FAT stays.
        self.assertEqual((info["name"], info["type"], info["fs"]), ("info", RESERVED, INFO_FS))
        self.assertEqual([k for k, p in d["parts"].items() if p["type"] == BASICDATA], ["6"])
        for k in "1234":
            self.assertIsNone(d["parts"][k]["fs"])
        self.assert_others_untouched()


@needs_sh
class TestD60FirstBoot(D60Case):
    def test_first_boot(self) -> None:
        self.w.run(self.D60, "start")
        self.assert_done()
        self.assertEqual(self.steps(), ["relocate", "create", "mkfs", "retype", "retire"])
        self.assertEqual(self.writes()[1], ["--force", "-N", "6", "/dev/mmcblk1"])
        self.assertEqual(self.writes()[2], ["-L", "THIMSY", "/dev/mmcblk1p6"])
        self.assertEqual(self.writes()[3][:3], ["--part-type", "/dev/mmcblk1", "6"])
        self.assertEqual(self.writes()[4], ["--part-type", "/dev/mmcblk1", "5", RESERVED.lower()])
        self.assertEqual(self.w.read("sys/class/block/mmcblk1p6/uevent"), "change\n")
        self.assertIn("games partition mmcblk1p6 ready", self.w.read("dev/kmsg"))

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
        cases = [(s, m) for s in ("relocate", "create", "mkfs", "retype", "retire")
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

    def test_entry6_not_ours_is_left_alone(self) -> None:
        """...and info stays as it is (basic data), since games is not Thimsy's."""
        for typ, name in ((BASICDATA, "mystuff"), (LINUX_DATA, "games"), (RESERVED, "other")):
            with self.subTest(typ=typ, name=name):
                d = self.w.disk("mmcblk1")
                d["parts"]["6"] = {"start": IMAGE_END, "size": 512 * MIB, "type": typ,
                                   "uuid": "66666666-0000-4000-8000-000000000006", "name": name,
                                   "fs": {"type": "exfat", "label": "X", "complete": True}}
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
        self.assertEqual(d["parts"]["6"]["type"], BASICDATA)
        self.assertEqual(d["parts"]["6"]["start"] % 256, 0)  # 1 MiB in 4 KiB sectors
        self.assertEqual(d["parts"]["5"]["type"], RESERVED)
        self.assertIn(["-L", "THIMSY", "/dev/sda6"], self.writes())


@needs_sh
class TestD60Readopt(D60Case):
    """device-safety-2: the image written again over a used Thimsy card. The card's bytes are a
    sparse file (rcs_fake_tool.image_path) that the fake dd reads and the fake mkfs.exfat
    writes a small exFAT into."""

    MARK = b"a saved game, not to be lost"

    def setUp(self) -> None:
        super().setUp()
        self.img = self.with_bytes(self.w)

    def with_bytes(self, w: "World") -> Path:
        img = fake.image_path(w.root, "mmcblk1")
        if not fake.make_sparse(img, w.disk("mmcblk1")["size_sectors"] * 512):
            self.skipTest("this filesystem cannot make sparse files")
        return img

    def rewrite_image(self, w: Optional["World"] = None) -> None:
        """Write the image again: entries 1 to 5 (info basic data again) and the backup GPT
        at the image's end, in its last 33 sectors; every byte after the image stays."""
        w = w or self.w
        d = w.disk("mmcblk1")
        d["parts"] = image_parts("11111111")
        d["last_lba"] = IMAGE_SECTORS - 34
        fake.save_disk(w.root, d)
        fake.patch_bytes(fake.image_path(w.root, "mmcblk1"), (IMAGE_SECTORS - 33) * 512,
                         b"\xa5" * 33 * 512)
        w.clear_calls()
        w.reboot()

    def library(self, w: Optional["World"] = None) -> Dict[str, Any]:
        """A first boot that makes the library, and a saved game in it."""
        w = w or self.w
        w.run(self.D60, "start")
        p6 = w.disk("mmcblk1")["parts"]["6"]
        fake.patch_bytes(fake.image_path(w.root, "mmcblk1"),
                         (p6["start"] + 300 * MIB) * 512, self.MARK)
        return p6

    def assert_kept(self, lib: Dict[str, Any], w: Optional["World"] = None) -> None:
        w = w or self.w
        img = fake.image_path(w.root, "mmcblk1")
        now = w.disk("mmcblk1")["parts"]["6"]
        self.assertEqual((now["start"], now["size"], now["type"], now["name"]),
                         (lib["start"], lib["size"], BASICDATA, "games"))
        self.assertEqual(fake.read_bytes(img, (lib["start"] + 300 * MIB) * 512, len(self.MARK)),
                         self.MARK)
        self.assertEqual(fake.sniff_exfat(img, lib["start"] * 512),
                         {"type": "exfat", "label": "THIMSY", "complete": True})

    def test_layout_leaves_the_boot_region(self) -> None:
        """The image's last byte is 1 MiB before games, so its backup GPT misses the exFAT's
        main and backup boot regions (sectors 0 to 23)."""
        self.assertEqual(IMAGE_SECTORS, IMAGE_END + MIB)
        self.assertLessEqual(IMAGE_SECTORS + MIB, GAMES_START)
        lib = self.library()
        boot = fake.read_bytes(self.img, lib["start"] * 512, 24 * 512)
        self.rewrite_image()
        self.assertEqual(fake.read_bytes(self.img, lib["start"] * 512, 24 * 512), boot)

    def test_rewritten_card_keeps_its_library(self) -> None:
        lib = self.library()
        self.assertEqual(self.steps(), ["relocate", "create", "mkfs", "retype", "retire"])
        self.rewrite_image()
        self.assertNotIn("6", self.w.disk("mmcblk1")["parts"])
        self.assertEqual(self.w.disk("mmcblk1")["parts"]["5"]["type"], BASICDATA)
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["relocate", "create", "retire"])  # no mkfs, no retype
        create = [c for c in self.w.calls() if c["tool"] == "sfdisk" and "-N" in c["args"]]
        self.assertEqual(len(create), 1)
        self.assert_kept(lib)
        self.assert_done()
        self.assert_invariant()
        self.assertIn("re-adopting", self.w.boot_log())
        self.assertIn("mmcblk1p6 re-adopted", self.w.read("dev/kmsg"))
        self.assertEqual(self.w.read("sys/class/block/mmcblk1p6/uevent"), "change\n")
        self.assertFalse((self.w.root / "run/thimsy/firstboot.probe").exists())
        # and nothing more at the next boot
        self.w.clear_calls()
        self.w.reboot()
        self.w.run(self.D60, "start")
        self.assertEqual(self.writes(), [])

    def test_rewritten_twice(self) -> None:
        lib = self.library()
        for _ in range(2):
            self.rewrite_image()
            self.w.run(self.D60, "start")
            self.assertNotIn("mkfs", self.steps())
            self.assert_kept(lib)

    def test_power_cut_while_adopting(self) -> None:
        for step, mode in [(s, m) for s in ("relocate", "create", "retire")
                           for m in ("before", "after")]:
            with self.subTest(step=step, mode=mode):
                w = World(Path(tempfile.mkdtemp(prefix="test-rcs-adopt-")))
                self.addCleanup(shutil.rmtree, str(w.tmp), True)
                self.w = w
                w.standard()
                w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
                w.mounted_root("179:19")
                self.others = {n: w.disk_bytes(n) for n in ("mmcblk0", "mmcblk2")}
                self.with_bytes(w)
                lib = self.library(w)
                self.rewrite_image(w)
                w.run(self.D60, "start", env={"THIMSY_FAKE_POWERCUT": "%s:%s" % (step, mode)},
                      check=False)
                self.assertTrue((w.root / ".powercut").exists(), "the cut did not happen")
                self.assert_invariant()
                w.reboot()
                w.run(self.D60, "start")
                self.assertNotIn("mkfs", self.steps())
                self.assert_kept(lib, w)
                self.assert_done()
                self.assert_invariant()

    def world(self, prefix: str) -> Path:
        """A fresh world for a subtest, booted from mmcblk1; the card's bytes' path."""
        w = World(Path(tempfile.mkdtemp(prefix=prefix)))
        self.addCleanup(shutil.rmtree, str(w.tmp), True)
        self.w = w
        w.standard()
        w.cmdline("root=PARTUUID=11111111-0000-4000-8000-000000000003")
        w.mounted_root("179:19")
        self.others = {n: w.disk_bytes(n) for n in ("mmcblk0", "mmcblk2")}
        return self.with_bytes(w)

    def test_no_complete_exfat_is_formatted(self) -> None:
        """A second-hand card gets a fresh library: a boot sector without its root directory
        (a cut format), another filesystem, or nothing at all."""
        for case in ("torn", "ntfs", "zeros"):
            with self.subTest(case=case):
                img = self.world("test-rcs-other-")
                off = GAMES_START * 512
                if case == "torn":
                    fake.write_exfat(img, off, 4 * 1024 * MIB * 512, "THIMSY", complete=False)
                elif case == "ntfs":
                    fake.patch_bytes(img, off + 3, b"NTFS    ")
                self.w.run(self.D60, "start")
                self.assertEqual(self.steps(), ["relocate", "create", "mkfs", "retype", "retire"])
                self.assert_done()
                self.assertEqual(fake.sniff_exfat(img, off),
                                 {"type": "exfat", "label": "THIMSY", "complete": True})

    def test_a_renamed_or_small_library_is_adopted(self) -> None:
        """REV4 (linux-host-impls-4): a complete exFAT at the games start is the old library
        whatever its label (the user renamed or cleared it on a PC) or size: adopted, not
        formatted."""
        for case, label, mib, what in (("label OTHER", "OTHER", 4 * 1024, 'an exFAT labelled "OTHER"'),
                                       ("no label", "", 4 * 1024, "an exFAT without a label"),
                                       ("short volume", "THIMSY", 32, "exFAT THIMSY")):
            with self.subTest(case=case):
                img = self.world("test-rcs-renamed-")
                off = GAMES_START * 512
                fake.write_exfat(img, off, mib * MIB * 512, label)
                fake.patch_bytes(img, off + 20 * MIB * 512, self.MARK)
                self.w.run(self.D60, "start")
                self.assertEqual(self.steps(), ["relocate", "create", "retire"])  # no mkfs
                p6 = self.w.disk("mmcblk1")["parts"]["6"]
                self.assertEqual((p6["start"], p6["size"], p6["type"], p6["name"]),
                                 (GAMES_START, mib * MIB, BASICDATA, "games"))
                self.assertEqual(fake.read_bytes(img, off + 20 * MIB * 512, len(self.MARK)), self.MARK)
                self.assertEqual(fake.sniff_exfat(img, off), {"type": "exfat", "label": label, "complete": True})
                self.assertIn(what + " found at sector %d: re-adopting" % GAMES_START, self.w.boot_log())
                if case == "short volume":
                    self.assertIn("under 64 MiB; it is kept", self.w.boot_log())
                self.assert_invariant()
                self.assert_others_untouched()

    def test_a_read_error_formats_nothing(self) -> None:
        """REV4 (linux-host-impls-4): when the games start can't be read, no entry is made and
        nothing is formatted; the next boot reads it and adopts the library."""
        lib = self.library()
        self.rewrite_image()
        r = self.w.run(self.D60, "start", env={"THIMSY_FAKE_FAIL": "dd"}, check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.steps(), ["relocate"])  # no create, no mkfs, info kept
        self.assertNotIn("6", self.w.disk("mmcblk1")["parts"])
        self.assertEqual(self.w.disk("mmcblk1")["parts"]["5"]["type"], BASICDATA)
        self.assertIn("could not read mmcblk1 at sector %d" % GAMES_START, self.w.boot_log())
        self.assertNotIn("creating entry 6 failed", self.w.boot_log())
        self.w.clear_calls()
        self.w.reboot()
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["create", "retire"])
        self.assert_kept(lib)
        self.assert_done()

    def test_library_longer_than_the_card_is_clamped(self) -> None:
        fake.write_exfat(self.img, GAMES_START * 512, 64 * 1024 * MIB * 512, "THIMSY")
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["relocate", "create", "retire"])
        d = self.w.disk("mmcblk1")
        p6 = d["parts"]["6"]
        self.assertEqual(p6["start"], GAMES_START)
        self.assertEqual(p6["start"] + p6["size"], d["last_lba"] + 1)
        self.assertEqual(p6["type"], BASICDATA)
        self.assertIn("runs past the card", self.w.boot_log())

    def test_library_extent_is_its_volume_length(self) -> None:
        """A library smaller than the free space keeps its own size."""
        fake.write_exfat(self.img, GAMES_START * 512, 4 * 1024 * MIB * 512, "THIMSY")
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["relocate", "create", "retire"])
        p6 = self.w.disk("mmcblk1")["parts"]["6"]
        self.assertEqual((p6["start"], p6["size"]), (GAMES_START, 4 * 1024 * MIB))
        self.assert_invariant()


@needs_sh
class TestD60Guids(D60Case):
    """OA-9: a card written from the image (Etcher, dd) carries the image's pinned, name-based
    GPT GUIDs, the same on every card written from it. D60 replaces them with the kernel's
    random ones on the boot disk only, before anything else is written; a second card written
    from the same image keeps its own until it boots."""

    PINNED_DISK = "5eed0000-0000-5000-8000-000000000000"

    def pin(self, w: "World") -> None:
        for name in ("mmcblk1", "mmcblk2"):
            d = w.disk(name)
            d["parts"] = image_parts("11111111", pinned=True)
            d["disk_id"] = self.PINNED_DISK
            fake.save_disk(w.root, d)
        w.reboot()
        w.cmdline("root=PARTUUID=11111111-0000-5000-8000-000000000003")
        self.others = {n: w.disk_bytes(n) for n in ("mmcblk0", "mmcblk2")}

    def setUp(self) -> None:
        super().setUp()
        self.pin(self.w)

    def guids(self) -> List[str]:
        d = self.w.disk("mmcblk1")
        return [d["disk_id"]] + [d["parts"][k]["uuid"] for k in "12345"]

    def assert_random(self) -> None:
        got = self.guids()
        for g in got:
            self.assertEqual(uuid.UUID(g).version, 4, got)
        self.assertEqual(len(set(got)), len(got), "a GUID is used twice: %r" % got)
        self.assertNotIn(self.PINNED_DISK, got)

    def test_pinned_guids_made_random_first(self) -> None:
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["guid"] * 6 + ["relocate", "create", "mkfs", "retype",
                                                        "retire"])
        self.assertEqual(self.writes()[0][:2], ["--disk-id", "/dev/mmcblk1"])
        self.assertEqual([a[2] for a in self.writes()[1:6]], ["1", "2", "3", "4", "5"])
        self.assert_random()
        self.assert_done()
        self.assert_others_untouched()   # the clone in slot 2 keeps the image's GUIDs
        self.assertIn("GPT GUIDs of mmcblk1 are random now", self.w.read("dev/kmsg"))
        # The kernel lists the new PARTUUIDs from the next boot; U-Boot reads entry 3's
        # GUID on every boot, so root=PARTUUID= follows it.
        ue = self.w.read("sys/class/block/mmcblk1p3/uevent")
        self.assertIn("PARTUUID=11111111-0000-5000-8000-000000000003", ue)
        self.w.clear_calls()
        self.w.reboot()
        self.assertIn("PARTUUID=%s" % self.w.disk("mmcblk1")["parts"]["3"]["uuid"].lower(),
                      self.w.read("sys/class/block/mmcblk1p3/uevent"))
        self.w.run(self.D60, "start")
        self.assertEqual(self.writes(), [])   # never again, updates included

    def test_power_cut_while_making_them_random(self) -> None:
        for mode in ("before", "after"):
            with self.subTest(mode=mode):
                w = World(Path(tempfile.mkdtemp(prefix="test-rcs-guid-")))
                self.addCleanup(shutil.rmtree, str(w.tmp), True)
                self.w = w
                w.standard()
                w.mounted_root("179:19")
                self.pin(w)
                w.run(self.D60, "start", env={"THIMSY_FAKE_POWERCUT": "guid:" + mode},
                      check=False)
                self.assertTrue((w.root / ".powercut").exists(), "the cut did not happen")
                self.assertNotIn("6", w.disk("mmcblk1")["parts"])   # nothing else written
                w.reboot()
                w.run(self.D60, "start")
                self.assert_random()
                self.assert_done()
                self.assert_invariant()

    def test_no_random_source_still_makes_games(self) -> None:
        """A bad or repeating random source: nothing is half-written, the games steps still
        run, and the next boot finishes."""
        used = self.w.disk("mmcblk1")["parts"]["4"]
        used["uuid"] = "44444444-0000-4000-8000-000000000004"   # random already, not pinned
        d = self.w.disk("mmcblk1")
        d["parts"]["4"] = used
        fake.save_disk(self.w.root, d)
        for source, warning in (("not a guid", "no random GUID"),
                                ("44444444-0000-4000-8000-000000000004", "already on mmcblk1")):
            with self.subTest(source=source):
                self.w.write("proc/sys/kernel/random/uuid", source + "\n")
                self.w.clear_calls()
                self.w.run(self.D60, "start")
                self.assertNotIn("guid", self.steps())
                self.assertIn(warning, self.w.boot_log())
                self.assertIn("trying again at the next boot", self.w.boot_log())
                self.assertEqual(uuid.UUID(self.w.disk("mmcblk1")["disk_id"]).version, 5)
                self.w.reboot()
        self.assertEqual(self.w.disk("mmcblk1")["parts"]["6"]["type"], BASICDATA)
        self.w.write("proc/sys/kernel/random/uuid", str(uuid.uuid4()) + "\n")
        self.w.clear_calls()
        self.w.run(self.D60, "start")
        self.assertEqual(self.steps(), ["guid"] * 5)   # the disk and entries 1, 2, 3, 5
        self.assert_random()
        self.assert_done()

    def test_emmc_boot_disk_keeps_its_guids(self) -> None:
        d = self.w.disk("mmcblk0")
        d["disk_id"] = self.PINNED_DISK
        fake.save_disk(self.w.root, d)
        self.others["mmcblk0"] = self.w.disk_bytes("mmcblk0")
        self.w.mounted_root("179:4")
        self.w.run(self.D60, "start")
        self.assertEqual(self.writes(), [])
        self.assert_others_untouched()


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
