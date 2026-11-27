"""Changes one file's contents inside an ext4 image, in place, leaving every other byte as it was.

patch_file(IN, OUT, PATH, CHANGE, LABEL) copies IN to OUT and writes CHANGE(PATH's contents) over
PATH's own data blocks in OUT. The new contents must be the same size as the old, so nothing about
the file but its data changes: the inode, its extents, owner, mode, times and SELinux label, the
directories, the bitmaps and every other file stay byte for byte as they were, and only the data
blocks whose contents differ are written. That is a file rewritten in place, as the kernel would
write it (ext4 does not checksum data blocks).

Refused: an image with shared (deduplicated) blocks, where a block of the file may also be another
file's; a file that is not a regular file mapped by extents or block maps (inline data, fs-verity,
encryption, compression), or whose changed block is a hole or an unwritten extent; an image
e2fsck -fn does not pass, before or after (a clean ext4 has one owner per block). Afterwards the
file is read back through the filesystem (debugfs), and OUT is compared with IN: only the blocks
written may differ.

Needs on PATH: debugfs, dumpe2fs and e2fsck.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

ENV = dict(os.environ, LC_ALL="C")
EXTENTS_FL = 0x80000     # EXT4_EXTENTS_FL; any other inode flag is refused
CHUNK = 1 << 20


def _tool(cmd, label):
    proc = subprocess.run(cmd, capture_output=True, env=ENV)
    if proc.returncode:
        sys.exit("%s: %s failed (exit %d): %s" % (label, cmd[0], proc.returncode,
                                                   proc.stderr.decode(errors="replace").strip()))
    return proc


def _debugfs(image, request, label):
    """debugfs's stdout for one read-only REQUEST (debugfs exits 0 even when the request fails)."""
    return _tool(["debugfs", "-R", request, image], label).stdout


def _quote(path, label):
    if '"' in path or "\n" in path or not path.startswith("/"):
        sys.exit("%s: bad path %r" % (label, path))
    return '"%s"' % path


def superblock(image, label):
    """(block size, block count, features) from dumpe2fs -h."""
    out = _tool(["dumpe2fs", "-h", image], label).stdout.decode(errors="replace")
    try:
        bs = int(re.search(r"^Block size:\s+(\d+)$", out, re.M).group(1))
        count = int(re.search(r"^Block count:\s+(\d+)$", out, re.M).group(1))
        features = re.search(r"^Filesystem features:\s*(.*)$", out, re.M).group(1).split()
    except AttributeError:
        sys.exit("%s: not an ext4 image (dumpe2fs)" % label)
    return bs, count, features


def inode(image, path, label):
    """(inode number, size, flags, the stat text) of PATH, a regular file."""
    out = _debugfs(image, "stat " + _quote(path, label), label).decode(errors="replace")
    m = re.search(r"^Inode: (\d+)\s+Type: (\S+)\s+Mode:\s+\d+\s+Flags: (0x[0-9a-f]+)$", out, re.M)
    size = re.search(r"^User:.*\bSize: (\d+)$", out, re.M)
    if not m or not size:
        sys.exit("%s: no %s" % (label, path))
    if m.group(2) != "regular":
        sys.exit("%s: %s is not a regular file (%s)" % (label, path, m.group(2)))
    flags = int(m.group(3), 16)
    if flags & ~EXTENTS_FL:
        sys.exit("%s: %s has inode flags 0x%x (inline data, fs-verity, encryption or compression?)"
                 % (label, path, flags))
    if "[u]" in out or "Uninit" in out:
        sys.exit("%s: %s has unwritten extents" % (label, path))
    return int(m.group(1)), int(size.group(1)), flags, out


def contents(image, path, label):
    return _debugfs(image, "cat " + _quote(path, label), label)


def bmap(image, path, block, label):
    out = _debugfs(image, "bmap %s %d" % (_quote(path, label), block), label).decode().split()
    if len(out) != 1 or not out[0].isdigit():
        sys.exit("%s: can't map block %d of %s" % (label, block, path))
    return int(out[0])


def fsck(image, label, when):
    proc = subprocess.run(["e2fsck", "-fn", image], capture_output=True, env=ENV)
    if proc.returncode:
        sys.exit("%s: e2fsck -fn fails %s (exit %d):\n%s" % (
            label, when, proc.returncode, (proc.stdout + proc.stderr).decode(errors="replace")[-2000:]))


def differing_blocks(a, b, bs):
    """The block numbers where files A and B (the same size) differ."""
    out = []
    with open(a, "rb") as fa, open(b, "rb") as fb:
        base = 0
        while True:
            x, y = fa.read(CHUNK), fb.read(CHUNK)
            if x != y:
                for i in range(0, max(len(x), len(y)), bs):
                    if x[i:i + bs] != y[i:i + bs]:
                        out.append((base + i) // bs)
            if not x and not y:
                return out
            base += CHUNK


def patch_file(src, dst, path, change, label):
    """Writes CHANGE(PATH's contents) over PATH in a copy of the ext4 image SRC at DST.
    Returns [(file block, image block)] for the blocks written."""
    bs, count, features = superblock(src, label)
    if "shared_blocks" in features:
        sys.exit("%s: the image shares blocks between files (shared_blocks)" % label)
    if CHUNK % bs:
        sys.exit("%s: unsupported block size %d" % (label, bs))
    ino, size, _flags, _ = inode(src, path, label)
    fsck(src, label, "on the original image")
    old = contents(src, path, label)
    if len(old) != size:
        sys.exit("%s: read %d bytes of %s, which holds %d" % (label, len(old), path, size))
    new = change(old)
    if len(new) != len(old):
        sys.exit("%s: the new %s is %d bytes, the old %d: only a change in place is supported"
                 % (label, path, len(new), len(old)))

    writes = []
    with open(src, "rb") as f:
        for lblk in range(0, (size + bs - 1) // bs):
            a, b = old[lblk * bs:(lblk + 1) * bs], new[lblk * bs:(lblk + 1) * bs]
            if a == b:
                continue
            pblk = bmap(src, path, lblk, label)
            if pblk == 0:
                sys.exit("%s: block %d of %s is a hole" % (label, lblk, path))
            if not 0 < pblk < count or pblk in {w[1] for w in writes}:
                sys.exit("%s: block %d of %s maps to block %d" % (label, lblk, path, pblk))
            f.seek(pblk * bs)
            if f.read(len(a)) != a:
                sys.exit("%s: image block %d does not hold block %d of %s" % (label, pblk, lblk, path))
            writes.append((lblk, pblk, b))

    # A new file, not the old one truncated: a loop device left on the old one keeps its own inode.
    if os.path.lexists(dst):
        os.unlink(dst)
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        for _, pblk, data in writes:
            f.seek(pblk * bs)
            f.write(data)
        f.flush()
        os.fsync(f.fileno())

    if os.path.getsize(dst) != os.path.getsize(src):
        sys.exit("%s: the patched image changed size" % label)
    if inode(dst, path, label)[0] != ino or contents(dst, path, label) != new:
        sys.exit("%s: %s does not read back as written" % (label, path))
    fsck(dst, label, "on the patched image")
    changed = differing_blocks(src, dst, bs)
    if not set(changed) <= {p for _, p, _ in writes}:
        sys.exit("%s: the patched image differs outside %s" % (label, path))
    return [(lblk, pblk) for lblk, pblk, _ in writes]


def sha256(data):
    return hashlib.sha256(data).hexdigest()
