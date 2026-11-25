#!/usr/bin/env python3
"""Nano's source as real files in nano/, and the patch's Nano block generated from them.

  nano_tree.py fetch [--from-cache DIR] [--base SHA]   make the base cache work/nano-base/
  nano_tree.py compose OUT                     write the applied Nano folder (base assets + nano/)
  nano_tree.py regen [--check] [--force]       rebuild the patch's Nano block from nano/
  nano_tree.py import-tree DIR                 copy an applied AOSP Nano folder into nano/
  nano_tree.py import-patch [REF] [--old REV]  three-way import of REF's Nano patch changes
  nano_tree.py merge-patch O A B P             the `nanopatch` merge driver (run by git)
  nano_tree.py import-base NEW                 move nano/ to a new GammaOS base commit
  nano_tree.py install-driver                  register the merge driver in .git/config
  nano_tree.py export-assets OUT [--from DIR]  install Nano's runtime assets as Android.bp does
  nano_tree.py status [--check]                base, cache, driver, sync and (with --check) regen

See docs/linux/IMPLEMENTATION.md, section A. Runs on Python 3.8 and later; needs git.
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nanotree import androidbp  # noqa: E402
from nanotree.compose import compose, tracked_entries  # noqa: E402
from nanotree.config import Layout, NanoTreeError, Upstream, find_root  # noqa: E402
from nanotree.gitcache import Cache, Fork, git_version, init_cache, posix  # noqa: E402
from nanotree.importer import (check_files, import_base, import_patch, import_tree,  # noqa: E402
                               merge_patch)
from nanotree.patchfile import Patch, sha256  # noqa: E402
from nanotree.regen import regen  # noqa: E402

DRIVER = "nanopatch"


def _ctx(args):
    layout = Layout(find_root(getattr(args, "root", None)))
    cfg = Upstream.load(layout.root)
    fork = Fork(layout.root)
    cache = Cache(os.environ.get("NANO_TREE_CACHE") or layout.cache)
    return layout, cfg, fork, cache


# -- fetch ---------------------------------------------------------------------------------------


def _objects_dir(repo: str) -> str:
    for cand in (os.path.join(repo, ".git", "objects"), os.path.join(repo, "objects")):
        if os.path.isdir(cand):
            return os.path.abspath(cand)
    raise NanoTreeError("%s is not a git repository" % repo)


def _git_dir(repo: str) -> str:
    d = os.path.join(repo, ".git")
    return d if os.path.isdir(d) else repo


def fetch_from_cache(cache: Cache, src: str, base: str) -> None:
    src_objects = _objects_dir(src)
    init_cache(cache.path)
    dest_objects = os.path.join(cache.git_dir, "objects")
    os.makedirs(os.path.join(dest_objects, "info"), exist_ok=True)
    try:
        alt = os.path.relpath(src_objects, dest_objects)
    except ValueError:  # another drive on Windows
        alt = src_objects
    with open(os.path.join(dest_objects, "info", "alternates"), "w", encoding="utf-8", newline="\n") as f:
        f.write(posix(alt) + "\n")
    shallow = os.path.join(_git_dir(src), "shallow")
    if os.path.isfile(shallow):
        shutil.copyfile(shallow, os.path.join(cache.git_dir, "shallow"))
    if not cache.has_commit(base):
        raise NanoTreeError("%s does not hold the base commit %s" % (src, base))


def fetch_network(cache: Cache, cfg: Upstream, base: str, offline: bool = False) -> None:
    """As fetch_nano.sh did: a blob-filtered, depth-1 fetch of BASE, then only the blobs of Nano's
    folder and the siblings. Nothing is downloaded when the cache already has them; OFFLINE (or
    NANO_TREE_OFFLINE=1) never downloads and only reports what is missing."""
    init_cache(cache.path)
    g = cache.git
    if not offline:
        if cache.git_rc("config", "remote.origin.url")[0] != 0:
            g("remote", "add", "origin", cfg.repo)
        else:
            g("config", "remote.origin.url", cfg.repo)
        g("config", "core.repositoryformatversion", "1")
        g("config", "extensions.partialclone", "origin")
        g("config", "remote.origin.promisor", "true")
        g("config", "remote.origin.partialclonefilter", "blob:none")
    if not cache.has_commit(base):
        if offline:
            raise NanoTreeError("the cache lacks %s and NANO_TREE_OFFLINE is set" % base)
        g("fetch", "-q", "--depth", "1", "--filter=blob:none", "--no-tags", "origin", base, network=True)
    want = sorted({e[1] for e in cache.ls_tree("%s:%s" % (base, cfg.path)).values()})
    for sib in cfg.siblings:
        code, out, _ = cache.git_rc("ls-tree", "-z", base, "--", sib)
        if code == 0 and out.strip(b"\0"):
            want.append(out.strip(b"\0").split(b"\t")[0].split()[2].decode())
    missing = cache.missing_objects(want)
    if missing and offline:
        print("fetch: %d blob(s) missing; not downloaded (offline)" % len(missing), file=sys.stderr)
    elif missing:
        base_args = ["-c", "fetch.negotiationAlgorithm=noop", "fetch", "-q", "origin", "--no-tags",
                     "--recurse-submodules=no", "--filter=blob:none"]
        if git_version() >= (2, 29):
            g(*(base_args + ["--no-write-fetch-head", "--stdin"]), input=("\n".join(missing) + "\n").encode(),
              network=True)
        else:
            for i in range(0, len(missing), 400):
                g(*(base_args + missing[i:i + 400]), network=True)


def cmd_fetch(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    base = args.base or cfg.base
    if args.from_cache:
        if cache.exists() and not args.force:
            raise NanoTreeError("%s already exists; pass --force to make it again" % cache.path)
        if cache.exists():
            shutil.rmtree(cache.path)
        fetch_from_cache(cache, os.path.abspath(args.from_cache), base)
    else:
        fetch_network(cache, cfg, base, offline=args.offline or os.environ.get("NANO_TREE_OFFLINE") == "1")
    cache.git("update-ref", "refs/thimsy/base" if base == cfg.base else "refs/thimsy/new-base", base)
    tree = cache.ls_tree("%s:%s" % (base, cfg.path))
    missing = cache.missing_objects(sorted({e[1] for e in tree.values()}))
    print("base    %s" % base)
    print("cache   %s" % cache.path)
    print("files   %d (%d blobs missing)" % (len(tree), len(missing)))
    return 1 if missing else 0


# -- the others ----------------------------------------------------------------------------------


def cmd_compose(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    want = compose(layout, cfg, fork, cache, os.path.abspath(args.out))
    print("compose: %d files in %s" % (len(want), args.out))
    return 0


def cmd_regen(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    return regen(layout, cfg, fork, cache, check=args.check, force=args.force)


def cmd_import_tree(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    rc = import_tree(layout, cfg, fork, cache, args.dir)
    problems = check_files(layout, cfg, fork, cache)
    for p in problems:
        print("import-tree: %s" % p, file=sys.stderr)
    return rc or (1 if problems else 0)


def cmd_import_patch(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    return import_patch(layout, cfg, fork, cache, args.ref, args.old)


def cmd_merge_patch(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    return merge_patch(layout, cfg, fork, cache, args.O, args.A, args.B, args.P)


def cmd_import_base(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    return import_base(layout, cfg, fork, cache, args.new)


def driver_command() -> str:
    exe = posix(sys.executable)
    return '"%s" tools/nano-tree/nano_tree.py merge-patch %%O %%A %%B %%P' % exe


def cmd_install_driver(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    fork.git("config", "merge.%s.name" % DRIVER, "Thimsy: import nano-144's Nano patch into nano/ and regenerate")
    fork.git("config", "merge.%s.driver" % DRIVER, driver_command())
    print("install-driver: merge.%s.driver = %s" % (DRIVER, driver_command()))
    return 0


def driver_installed(fork: Fork) -> bool:
    out = fork.git("config", "--get", "merge.%s.driver" % DRIVER, check=False, ok_codes=(0, 1))
    return b"merge-patch" in out


def cmd_export_assets(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    tmp = None
    src = args.src
    if not src:
        tmp = tempfile.mkdtemp(prefix="nano-compose-")
        src = os.path.join(tmp, "nano")
        compose(layout, cfg, fork, cache, src, with_siblings=False)
    try:
        files = []
        for dirpath, _dirs, names in os.walk(src):
            for n in names:
                files.append(os.path.relpath(os.path.join(dirpath, n), src).replace(os.sep, "/"))
        bp = androidbp.Blueprint.load(os.path.join(src, "Android.bp"))
        installs = androidbp.etc_installs(bp, files)
        out = os.path.abspath(args.out)
        os.makedirs(out, exist_ok=True)
        for _mod, s, dest in installs:
            d = os.path.join(out, *dest.split("/"))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copyfile(os.path.join(src, *s.split("/")), d)
        print("export-assets: %d files from %d modules into %s"
              % (len(installs), len({i[0] for i in installs}), out))
        return 0
    finally:
        if tmp:
            shutil.rmtree(tmp, True)


def cmd_status(args) -> int:
    layout, cfg, fork, cache = _ctx(args)
    print("base      %s" % cfg.base)
    if cache.exists() and cache.has_commit(cfg.base):
        tree = cache.ls_tree("%s:%s" % (cfg.base, cfg.path))
        missing = cache.missing_objects(sorted({e[1] for e in tree.values()}))
        print("cache     %s (%d files, %d blobs missing)" % (cache.path, len(tree), len(missing)))
    else:
        print("cache     missing (%s): run nano_tree.py fetch" % cache.path)
    print("driver    %s" % ("installed" if driver_installed(fork) else "NOT installed: run nano_tree.py install-driver"))
    if fork.rev_parse("nano-144"):
        mb = fork.git("merge-base", "HEAD", "nano-144", check=False).decode().strip()
        print("synced    nano-144 at %s" % (mb[:12] or "?"))
    else:
        print("synced    no local nano-144 branch")
    tracked = tracked_entries(fork)
    print("nano/     %d tracked files" % len(tracked))
    with open(layout.patch, "rb") as f:
        block = Patch.parse(f.read()).split_block(cfg.prefix)[1]
    rec = layout.read_block_sha()
    print("block     %s (%s)" % (sha256(block)[:16], "matches block.sha256" if rec == sha256(block)
                                 else "DIFFERS from block.sha256 %s" % (rec or "(none)")))
    if args.check:
        if not cache.exists():
            print("regen     not checked (no cache)")
            return 1
        return regen(layout, cfg, fork, cache, check=True)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="nano_tree.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", help="repository root (default: the one holding this tool)")
    sub = ap.add_subparsers(dest="cmd")
    sub.required = True
    p = sub.add_parser("fetch", help="make the base cache work/nano-base/")
    p.add_argument("--from-cache", metavar="DIR", help="adopt an existing cache (no network): alternates")
    p.add_argument("--base", help="fetch this commit instead of upstream.json's base (for import-base)")
    p.add_argument("--force", action="store_true", help="remake an existing cache (with --from-cache)")
    p.add_argument("--offline", action="store_true", help="never download (also NANO_TREE_OFFLINE=1)")
    p.set_defaults(fn=cmd_fetch)
    p = sub.add_parser("compose", help="write the applied Nano folder")
    p.add_argument("out")
    p.set_defaults(fn=cmd_compose)
    p = sub.add_parser("regen", help="regenerate the patch's Nano block from nano/")
    p.add_argument("--check", action="store_true", help="compare instead of writing")
    p.add_argument("--force", action="store_true", help="overwrite a block changed outside regen")
    p.set_defaults(fn=cmd_regen)
    p = sub.add_parser("import-tree", help="copy an applied AOSP Nano folder into nano/")
    p.add_argument("dir")
    p.set_defaults(fn=cmd_import_tree)
    p = sub.add_parser("import-patch", help="three-way import of a branch's Nano patch changes",
                       description="Merge the Nano block's changes between --old (default: the merge "
                       "base with REF) and REF (default: the working patch file) into nano/, then "
                       "regenerate. After a merge that took nano-144's patch without the driver, run "
                       "`import-patch --old <the previous merge base>` with no REF.")
    p.add_argument("ref", nargs="?")
    p.add_argument("--old", help="the commit whose patch is the old side")
    p.set_defaults(fn=cmd_import_patch)
    p = sub.add_parser("merge-patch", help="the merge driver (git runs it)")
    for n in ("O", "A", "B", "P"):
        p.add_argument(n)
    p.set_defaults(fn=cmd_merge_patch)
    p = sub.add_parser("import-base", help="move nano/ to a new GammaOS base")
    p.add_argument("new")
    p.set_defaults(fn=cmd_import_base)
    p = sub.add_parser("install-driver", help="register the merge driver in .git/config")
    p.set_defaults(fn=cmd_install_driver)
    p = sub.add_parser("export-assets", help="install Nano's prebuilt_etc assets")
    p.add_argument("out")
    p.add_argument("--from", dest="src", help="a composed tree (default: compose one)")
    p.set_defaults(fn=cmd_export_assets)
    p = sub.add_parser("status", help="base, cache, driver, sync")
    p.add_argument("--check", action="store_true", help="also run regen --check")
    p.set_defaults(fn=cmd_status)
    args = ap.parse_args(argv)
    try:
        return args.fn(args)
    except NanoTreeError as e:
        print("nano_tree.py %s: %s" % (args.cmd, e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
