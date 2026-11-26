"""The jail nano-headless runs in (docs/linux/IMPLEMENTATION.md D.3, B.5).

Nano writes /data/..., reads /system/etc/..., /proc/self and /proc/meminfo, and opens
/dev/input/eventN. The jail is a root in a temporary directory with the Linux image's layout, so
those path literals work unedited, as they do on the device:

  /data -> /storage/.config/thimsy/data    (system/, local/tmp/, data/, user/0, misc/, gammaos_ota/,
                                            media/0 -> /storage/emulated/0)
  /sdcard, /storage/self/primary -> /storage/emulated/0
  /system/etc -> /usr/share/thimsy/system/etc     (Nano's assets, export-assets' output)
  /system/fonts -> /usr/share/fonts/thimsy
  /system/bin -> /usr/libexec/thimsy/android-bin  (empty, as in the alpha)
  /storage/<drive>/ROMs, /storage/<drive>/ES-DE   (a fixture drive; /mnt/media_rw/<drive> links to it)
  /run/thimsy/props, /run/thimsy/log, /storage/.config/thimsy/persist.props  (the property store)
  /run/thimsy/headless/                            (nano-headless's config, events and control FIFO)
  /dev/input/eventN                                (FIFOs: the fake input devices)

Building the layout is plain Python and needs no privilege (JailLayout.build). Running in it uses
`unshare --user --map-root-user --mount --pid --fork --mount-proc` and chroot; inside the
namespace a generated script mounts a tmpfs at the jail's /usr, binds the host's /usr/bin, /usr/lib,
/usr/lib64 (when present) and /usr/share/zoneinfo, /etc, the top-level /bin, /lib, ... where they
are directories (where they are links into /usr, as on merged-/usr systems, the layout has the same
links), the assets and fonts, the binary's folder at /opt/nano, and /dev/null, /dev/zero,
/dev/random and /dev/urandom one by one; then remounts every bind read-only (devices excepted:
they keep working on a read-only mount, and the remount is skipped where it is not allowed) and
mounts /proc. Only then are /usr/share/thimsy/system/etc and /usr/share/fonts/thimsy made on the
tmpfs. There is no real-root fallback: without user namespaces, Jail.available() is false and the
tests skip (D.3).

Standard library only; Python 3.8 or later.
"""
from __future__ import annotations

import os
import shlex
import shutil
import stat
import subprocess
from typing import Dict, List, Optional, Sequence

# The links of the Linux image's root that Nano's paths go through (B.5), relative to the root.
IMAGE_LINKS = {
    "data": "/storage/.config/thimsy/data",
    "sdcard": "/storage/emulated/0",
    "storage/self/primary": "/storage/emulated/0",
    "system/etc": "/usr/share/thimsy/system/etc",
    "system/fonts": "/usr/share/fonts/thimsy",
    "system/bin": "/usr/libexec/thimsy/android-bin",
    "storage/.config/thimsy/data/media/0": "/storage/emulated/0",
}
# The writable data tree first boot makes (B.5).
DATA_DIRS = ("system", "local/tmp", "data", "user/0", "misc", "gammaos_ota")
# Writable directories of the root.
ROOT_DIRS = ("tmp", "run/thimsy/props/ro", "run/thimsy/log", "run/thimsy/headless", "storage/emulated/0",
             "storage/.config/thimsy", "mnt/media_rw", "dev/input", "proc", "sys", "opt/nano", "etc", "usr")
# Host folders bound read-only under the jail's /usr tmpfs, when they exist.
USR_BINDS = ("bin", "sbin", "lib", "lib32", "lib64", "libx32", "share/zoneinfo")
# Top-level host folders: a link into usr/ on merged-/usr systems (the jail gets the same link), a
# directory elsewhere (bound read-only).
TOP_LEVEL = ("bin", "sbin", "lib", "lib32", "lib64", "libx32")
DEVICE_NODES = ("null", "zero", "random", "urandom")
HEADLESS_DIR = "/run/thimsy/headless"
PERSIST_FILE = "/storage/.config/thimsy/persist.props"
RUN_DIR = "/run/thimsy/props"
LOG_FILE = "/run/thimsy/log/nano.log"


class Mount:
    """One step of the namespace's set-up: kind is "tmpfs", "bind", "proc" or "mkdir"."""

    def __init__(self, kind: str, target: str, source: str = "", readonly: bool = False, device: bool = False):
        self.kind = kind
        self.target = target          # inside the jail, absolute
        self.source = source          # on the host, absolute (binds)
        self.readonly = readonly
        self.device = device          # a device node: a file bind

    def __repr__(self) -> str:
        return "Mount(%s %s <- %s%s)" % (self.kind, self.target, self.source, " ro" if self.readonly else "")


class JailLayout:
    """The root's files: built with no privilege, then used by Jail to run something in it.

    root:     the jail's root (made if missing; must be empty or a previous layout)
    devices:  the fake input nodes, e.g. ["event0"]
    drive:    the fixture drive's id (/storage/<drive>), or "" for none
    assets:   a folder that becomes /system/etc (nano_tree.py export-assets' output), or None
    fonts:    a folder that becomes /system/fonts, or None
    binary_dir: the folder holding nano-headless, bound at /opt/nano
    host:     the host's root, for tests ("/" normally)
    """

    def __init__(self, root: str, devices: Sequence[str] = ("event0",), drive: str = "1234-ABCD",
                 assets: Optional[str] = None, fonts: Optional[str] = None, binary_dir: Optional[str] = None,
                 host: str = "/"):
        self.root = os.path.abspath(root)
        if any(c.isspace() or c == "\\" for c in self.root.replace(os.sep, "/")):
            # /proc/self/mountinfo escapes them, and the set-up script matches mount points by name.
            raise ValueError("a jail root may not contain white space or backslashes: %r" % self.root)
        self.devices = list(devices)
        self.drive = drive
        self.assets = os.path.abspath(assets) if assets else None
        self.fonts = os.path.abspath(fonts) if fonts else None
        self.binary_dir = os.path.abspath(binary_dir) if binary_dir else None
        self.host = host

    # -- paths
    def path(self, inside: str) -> str:
        """The outside path of INSIDE (an absolute path in the jail), without following links."""
        return os.path.join(self.root, inside.lstrip("/"))

    def resolve(self, inside: str) -> str:
        """The outside path of INSIDE, following the jail's own links as the jailed process would
        (links are absolute inside the jail)."""
        parts = [p for p in inside.split("/") if p]
        done: List[str] = []
        hops = 0
        while parts:
            done.append(parts.pop(0))
            here = os.path.join(self.root, *done)
            if os.path.islink(here):
                hops += 1
                if hops > 40:
                    raise OSError("too many links resolving %s" % inside)
                target = os.readlink(here)
                if target.startswith("/"):
                    parts = [p for p in target.split("/") if p] + parts
                    done = []
                else:
                    done.pop()
                    parts = [p for p in target.split("/") if p] + parts
        return os.path.join(self.root, *done) if done else self.root

    def fifo(self, node: str) -> str:
        return self.path("/dev/input/" + node)

    # -- building
    def build(self) -> "JailLayout":
        os.makedirs(self.root, exist_ok=True)
        for d in ROOT_DIRS:
            os.makedirs(self.path("/" + d), exist_ok=True)
        for d in DATA_DIRS:
            os.makedirs(self.path("/storage/.config/thimsy/data/" + d), exist_ok=True)
        os.chmod(self.path("/tmp"), 0o1777)
        for link, target in sorted(IMAGE_LINKS.items()):
            self._link(link, target)
        if self.drive:
            for d in ("ROMs", "ES-DE"):
                os.makedirs(self.path("/storage/%s/%s" % (self.drive, d)), exist_ok=True)
            self._link("mnt/media_rw/" + self.drive, "/storage/" + self.drive)
        for name in TOP_LEVEL:
            host = os.path.join(self.host, name)
            if os.path.islink(host):
                self._link(name, os.readlink(host))
            elif os.path.isdir(host):
                os.makedirs(self.path("/" + name), exist_ok=True)
        for node in DEVICE_NODES:
            p = self.path("/dev/" + node)
            if not os.path.lexists(p):
                open(p, "w").close()
        for node in self.devices:
            p = self.fifo(node)
            if not os.path.lexists(p):
                os.mkfifo(p, 0o660)
            elif not stat.S_ISFIFO(os.lstat(p).st_mode):
                raise OSError("%s exists and is not a FIFO" % p)
        return self

    def _link(self, rel: str, target: str) -> None:
        p = self.path("/" + rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.islink(p):
            if os.readlink(p) == target:
                return
            os.unlink(p)
        os.symlink(target, p)

    # -- the property store's files, written as libthimsy writes them (thimsy-prop boot's job)
    def write_props(self, props: Dict[str, str]) -> None:
        """persist.* into persist.props (sorted key=value, values escaped), ro.* into the run dir's ro/,
        the rest one file per key in the run dir."""
        persist: Dict[str, str] = {}
        if os.path.exists(self.path(PERSIST_FILE)):
            persist = read_persist(self.path(PERSIST_FILE))
        for key, value in props.items():
            if key.startswith("persist."):
                persist[key] = value
            else:
                d = self.path(RUN_DIR + ("/ro" if key.startswith("ro.") else ""))
                with open(os.path.join(d, key), "w", encoding="utf-8", newline="") as f:
                    f.write(value)
        lines = ["# Thimsy persistent settings (persist.*), written by libthimsy. Use thimsy-prop to change them."]
        for k in sorted(persist):
            if persist[k]:
                lines.append("%s=%s" % (k, escape_value(persist[k])))
        path = self.path(PERSIST_FILE)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(lines) + "\n")
        os.chmod(path, 0o600)

    def prop(self, key: str) -> str:
        """A property's value as the store keeps it on disk ("" when unset)."""
        if key.startswith("persist."):
            p = self.path(PERSIST_FILE)
            return read_persist(p).get(key, "") if os.path.exists(p) else ""
        p = os.path.join(self.path(RUN_DIR + ("/ro" if key.startswith("ro.") else "")), key)
        try:
            with open(p, "r", encoding="utf-8", newline="") as f:
                return f.read()
        except OSError:
            return ""

    # -- the namespace's set-up
    def mount_plan(self) -> List[Mount]:
        """Every mount, in order. Pure: reads the host's folder layout, mounts nothing."""
        plan = [Mount("tmpfs", "/usr")]
        for d in USR_BINDS:
            src = os.path.join(self.host, "usr", d)
            if os.path.isdir(src) and not os.path.islink(src):
                plan.append(Mount("bind", "/usr/" + d, src, readonly=True))
        for name in TOP_LEVEL:
            src = os.path.join(self.host, name)
            if os.path.isdir(src) and not os.path.islink(src):
                plan.append(Mount("bind", "/" + name, src, readonly=True))
        plan.append(Mount("bind", "/etc", os.path.join(self.host, "etc"), readonly=True))
        for node in DEVICE_NODES:
            plan.append(Mount("bind", "/dev/" + node, os.path.join(self.host, "dev", node), readonly=True,
                              device=True))
        if self.binary_dir:
            plan.append(Mount("bind", "/opt/nano", self.binary_dir, readonly=True))
        plan.append(Mount("proc", "/proc"))
        # Made on the tmpfs only after the binds (D.3), then filled read-only.
        for d in ("/usr/share/thimsy/system/etc", "/usr/share/fonts/thimsy", "/usr/libexec/thimsy/android-bin"):
            plan.append(Mount("mkdir", d))
        if self.assets:
            plan.append(Mount("bind", "/usr/share/thimsy/system/etc", self.assets, readonly=True))
        if self.fonts:
            plan.append(Mount("bind", "/usr/share/fonts/thimsy", self.fonts, readonly=True))
        return plan

    def setup_script(self, argv: Sequence[str], env: Dict[str, str], aslr_off: bool = False) -> str:
        """The sh script run inside the namespace: the mounts, then chroot and exec ARGV with exactly
        ENV."""
        q = shlex.quote
        root = q(self.root)
        out = ["set -eu", "PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH", "R=%s" % root, RO_TREE]
        for m in self.mount_plan():
            t = '"$R"' + q(m.target)
            if m.kind == "tmpfs":
                out.append("mount -t tmpfs -o mode=0755 tmpfs %s" % t)
            elif m.kind == "mkdir":
                out.append("mkdir -p %s" % t)
            elif m.kind == "proc":
                out.append("mount -t proc -o nosuid,nodev,noexec proc %s" % t)
            elif m.kind == "bind" and m.device:
                # A device node keeps working on a read-only mount; if a kernel refuses the remount of
                # one, the bind stays as it is.
                out.append("[ -e %s ] || : > %s" % (t, t))
                out.append("mount --bind %s %s" % (q(m.source), t))
                if m.readonly:
                    out.append("mount -o remount,bind,ro %s 2>/dev/null || true" % t)
            elif m.kind == "bind":
                # Recursive: in a user namespace a plain bind of a folder with mounts below it (WSL's
                # /usr/lib/wsl/*) is refused, since it would uncover what they hide.
                out.append("mkdir -p %s" % t)
                out.append("mount --rbind %s %s" % (q(m.source), t))
                if m.readonly:
                    out.append("ro_tree %s" % t)
        cmd = ["/usr/bin/env", "-i"] + ["%s=%s" % (k, v) for k, v in sorted(env.items())]
        if aslr_off:
            cmd += ["/usr/bin/setarch", os.uname().machine, "-R"]
        cmd += list(argv)
        out.append("cd \"$R\"")
        out.append("exec chroot \"$R\" " + " ".join(q(c) for c in cmd))
        return "\n".join(out) + "\n"


# Remounts a bind and every mount below it read-only. A user namespace may not clear the flags the
# original mount locked (nosuid, nodev, noexec, the atime flags), so each remount repeats them. The
# top must turn read-only; a mount below that refuses is reported and left as it is.
RO_TREE = r"""ro_tree() {
  top=$1
  awk -v t="$top" '$5 == t || index($5, t "/") == 1 { print $5, $6 }' /proc/self/mountinfo |
  while read -r mp opts; do
    keep=$(printf '%s' "$opts" | tr ',' '\n' | grep -E '^(nosuid|nodev|noexec|noatime|nodiratime|relatime)$' | tr '\n' ',')
    if ! mount -o "remount,bind,ro${keep:+,${keep%,}}" "$mp" 2>/dev/null; then
      [ "$mp" = "$top" ] && { echo "nano_jail: can't make $mp read-only" >&2; exit 1; }
      echo "nano_jail: warning: $mp stays writable" >&2
    fi
  done
  awk -v t="$top" '$5 == t && $6 ~ /^ro(,|$)/ { f = 1 } END { exit !f }' /proc/self/mountinfo ||
    { echo "nano_jail: $top is not read-only" >&2; return 1; }
}"""


def escape_value(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def unescape_value(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            i += 1
            c = {"n": "\n", "r": "\r"}.get(text[i], text[i])
        out.append(c)
        i += 1
    return "".join(out)


def read_persist(path: str) -> Dict[str, str]:
    """persist.props as libthimsy reads it."""
    out: Dict[str, str] = {}
    with open(path, "r", encoding="utf-8", newline="") as f:
        for line in f.read().split("\n"):
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = unescape_value(v)
            if v:
                out[k] = v
    return out


DEFAULT_ENV = {
    "PATH": "/usr/bin:/bin",
    "HOME": "/data/local/tmp",
    "TMPDIR": "/data/local/tmp",
    "LANG": "C.UTF-8",
}

_AVAILABLE: Optional[bool] = None


class Jail:
    """Runs commands in a JailLayout's root, in a new user, mount and pid namespace."""

    UNSHARE = ["unshare", "--user", "--map-root-user", "--mount", "--pid", "--fork", "--mount-proc"]

    def __init__(self, layout: JailLayout, work: str):
        self.layout = layout
        self.work = os.path.abspath(work)  # outside the root: scripts and logs
        os.makedirs(self.work, exist_ok=True)

    @staticmethod
    def available() -> bool:
        """Whether unprivileged user and mount namespaces work here (WSL Ubuntu: yes)."""
        global _AVAILABLE
        if _AVAILABLE is None:
            if os.name != "posix" or not shutil.which("unshare") or not shutil.which("chroot"):
                _AVAILABLE = False
            else:
                try:
                    proc = subprocess.run(["unshare", "--user", "--map-root-user", "--mount", "sh", "-c",
                                           "mount -t tmpfs tmpfs /mnt 2>/dev/null || mount -t tmpfs tmpfs /tmp"],
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
                    _AVAILABLE = proc.returncode == 0
                except (OSError, subprocess.SubprocessError):
                    _AVAILABLE = False
        return _AVAILABLE

    def command(self, argv: Sequence[str], env: Optional[Dict[str, str]] = None, aslr_off: bool = False,
                name: str = "run") -> List[str]:
        e = dict(DEFAULT_ENV)
        e.update(env or {})
        script = os.path.join(self.work, name + ".sh")
        with open(script, "w", encoding="utf-8", newline="\n") as f:
            f.write(self.layout.setup_script(argv, e, aslr_off=aslr_off))
        return self.UNSHARE + ["sh", script]

    def spawn(self, argv: Sequence[str], env: Optional[Dict[str, str]] = None, aslr_off: bool = False,
              name: str = "run", **popen) -> subprocess.Popen:
        return subprocess.Popen(self.command(argv, env, aslr_off, name), **popen)

    def run(self, argv: Sequence[str], env: Optional[Dict[str, str]] = None, timeout: float = 60,
            name: str = "run") -> subprocess.CompletedProcess:
        return subprocess.run(self.command(argv, env, name=name), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout)


class Fifos:
    """Holds every fake input FIFO and the control FIFO open read-write, so the jailed process's
    O_RDONLY | O_NONBLOCK opens succeed and its reads never see end-of-file."""

    def __init__(self, layout: JailLayout, control: Optional[str] = None):
        self.fds: Dict[str, int] = {}
        for node in layout.devices:
            self.fds[node] = os.open(layout.fifo(node), os.O_RDWR | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0))
        self.control_fd = -1
        if control:
            if not os.path.exists(control):
                os.mkfifo(control, 0o600)
            self.control_fd = os.open(control, os.O_RDWR | getattr(os, "O_CLOEXEC", 0))

    def write(self, node: str, data: bytes) -> None:
        fd = self.fds[node]
        view = memoryview(data)
        while view:
            n = os.write(fd, view)
            view = view[n:]

    def control(self, line: str) -> None:
        if self.control_fd < 0:
            raise RuntimeError("no control FIFO")
        os.write(self.control_fd, (line.rstrip("\n") + "\n").encode("utf-8"))

    def close(self) -> None:
        for fd in list(self.fds.values()) + ([self.control_fd] if self.control_fd >= 0 else []):
            try:
                os.close(fd)
            except OSError:
                pass
        self.fds = {}
        self.control_fd = -1
