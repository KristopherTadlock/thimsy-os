#!/usr/bin/env python3
"""Makes an AOSP tree's files exactly BASE with a patch applied, rewriting only
the files whose contents differ (scripts/aosp/system.sh).

    sync_patch.py TREE BASE PATCH

TREE is a git checkout of BASE (the patch is applied to the working tree only).
The patch is first applied to a scratch index, so a patch that doesn't apply
leaves the tree as it was. Then every path that may differ is compared by
content: the patch's files, the files that differ from BASE now (the last
patch's, or edits by hand, staged with git add or not), files git is told not
to look at (assume-unchanged, skip-worktree) and untracked files outside out/.
Index entries that differ from BASE (staged edits) are reset to BASE, so the
index stays at BASE. Files that already match are left alone, keeping their times, so a
build after a patch change redoes only what the change touched: changing a
.java or .cpp file doesn't send Soong through its whole analysis again, as
rewriting every patched Android.bp would.

Files under Git LFS are never compared (their working copies aren't the blobs
git stores) and are never rewritten.

Prints how many files it wrote and removed. Exits 1, changing nothing, when
the patch doesn't apply.
"""

import hashlib
import os
import subprocess
import sys
import tempfile

# Never touched: the build's output and the markers system.sh keeps.
KEEP = ("out/", ".thimsy-patch.sha256", ".thimsy-plan.sha256")


def git(tree, *args, index=None, stdin=None):
    env = dict(os.environ, GIT_LFS_SKIP_SMUDGE="1")
    if index:
        env["GIT_INDEX_FILE"] = index
    r = subprocess.run(
        ["git", "-C", tree, *args], input=stdin, capture_output=True, env=env
    )
    if r.returncode:
        raise RuntimeError(
            "git %s failed: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip())
        )
    return r.stdout


def nul_list(data):
    return [p.decode("utf-8", "surrogateescape") for p in data.split(b"\0") if p]


def blob_sha(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def current(tree, path):
    """(mode, sha) of the working tree's file, or None when there is none."""
    full = os.path.join(tree, path)
    try:
        st = os.lstat(full)
    except FileNotFoundError:
        return None
    if os.path.islink(full):
        return "120000", blob_sha(os.fsencode(os.readlink(full)))
    if not os.path.isfile(full):
        return None  # a directory where the tree wants a file: rewrite it
    mode = "100755" if st.st_mode & 0o111 else "100644"
    with open(full, "rb") as f:
        return mode, blob_sha(f.read())


def plan(tree, base, patch):
    """Writes the wanted index; returns (index path, paths to write, paths to remove,
    staged paths: the real index's entries that differ from BASE)."""
    fd, index = tempfile.mkstemp(prefix="thimsy-index-")
    os.close(fd)
    os.unlink(index)  # git read-tree makes it
    git(tree, "read-tree", base, index=index)
    git(tree, "apply", "--cached", "--whitespace=nowarn", os.path.abspath(patch), index=index)

    paths = set(nul_list(git(tree, "diff-index", "--cached", "--name-only", "--no-renames", "-z", base, index=index)))
    # What differs from BASE now, compared with BASE itself rather than the
    # index, so an edit staged with git add counts too.
    paths |= set(nul_list(git(tree, "diff", "--name-only", "--no-renames", "-z", base)))
    staged = set(nul_list(git(tree, "diff-index", "--cached", "--name-only", "--no-renames", "-z", base)))
    paths |= staged
    # git doesn't read these files (it takes the index's word for them), so
    # neither diff above sees an edit to one.
    for entry in nul_list(git(tree, "ls-files", "-v", "-z")):
        if entry[0].islower() or entry[0] == "S":
            paths.add(entry[2:])
    paths |= set(
        nul_list(
            git(tree, "ls-files", "--others", "--exclude-standard", "-z",
                "--exclude=/out/", *("--exclude=/" + k for k in KEEP[1:]))
        )
    )
    def kept(p):
        return any(p == k.rstrip("/") or p.startswith(k) for k in KEEP)

    paths = sorted(p for p in paths if not kept(p))
    staged = sorted(p for p in staged if not kept(p))
    if not paths:
        return index, [], [], staged

    attrs = nul_list(git(tree, "check-attr", "-z", "--stdin", "filter",
                         stdin="\0".join(paths).encode("utf-8", "surrogateescape") + b"\0"))
    lfs = {attrs[i] for i in range(0, len(attrs), 3) if attrs[i + 2] == "lfs"}

    want = {}
    for line in git(tree, "ls-files", "-s", "-z", index=index).split(b"\0"):
        if not line:
            continue
        meta, path = line.split(b"\t", 1)
        mode, sha, _stage = meta.decode().split()
        want[path.decode("utf-8", "surrogateescape")] = (mode, sha)

    write, remove = [], []
    for p in paths:
        if p in lfs:
            continue
        w, c = want.get(p), current(tree, p)
        if w == c:
            continue
        if w is None:
            if c is not None:
                remove.append(p)
        else:
            write.append(p)
    return index, write, remove, staged


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[3].strip(), file=sys.stderr)
        return 2
    tree, base, patch = argv[1:]
    try:
        index, write, remove, staged = plan(tree, base, patch)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    try:
        for p in remove:
            os.unlink(os.path.join(tree, p))
        if write:
            # checkout-index replaces a directory or symlink in the way too.
            git(tree, "checkout-index", "-f", "-z", "--stdin", index=index,
                stdin="\0".join(write).encode("utf-8", "surrogateescape") + b"\0")
        if staged:
            # Back to BASE in the real index (the working tree is right now);
            # only these entries, so the rest keep their cached file times.
            git(tree, "--literal-pathspecs", "reset", "-q", base, "--pathspec-from-file=-",
                "--pathspec-file-nul",
                stdin="\0".join(staged).encode("utf-8", "surrogateescape") + b"\0")
    finally:
        os.unlink(index)
    print("wrote %d file(s), removed %d" % (len(write), len(remove)))
    if staged:
        print("reset %d staged path(s) in the index to the base" % len(staged))
    for p in write[:20]:
        print("  wrote   " + p)
    for p in remove[:20]:
        print("  removed " + p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
