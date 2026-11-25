"""The kernel patch series, flattened in scripts/unpack's order, ROCKNIX's file names kept.

scripts/unpack (ROCKNIX next, 0f4806b) applies, for the linux package:

    $PKG_DIR/patches/*.patch                         (common)
    $PKG_DIR/patches/$PATCH_ARCH/*.patch             (common - arch)
    $PKG_DIR/patches/<d>/*.patch  for d in PKG_PATCH_DIRS
    $PKG_DIR/patches/$PKG_VERSION/*.patch            (the version dir, added automatically)
    $PKG_DIR/patches/$PKG_VERSION/$PATCH_ARCH/*.patch
    projects/ROCKNIX/patches/linux/*.patch, .../$PATCH_ARCH/*.patch
    projects/ROCKNIX/patches/linux/<d>/*.patch and devices/$DEVICE/patches/linux/<d>/*.patch,
                                                     for d in PKG_PATCH_DIRS
    projects/ROCKNIX/patches/linux/$PKG_VERSION/*.patch
    projects/ROCKNIX/devices/$DEVICE/patches/linux/*.patch   (device)

Each source directory becomes one numbered directory under gpl/generated/<family>/linux/
patches/ (NN-<label>, the label after scripts/unpack's own PATCH_DESC), so Buildroot's
BR2_LINUX_KERNEL_PATCH applies the same order while every file keeps its ROCKNIX name.

Each written patch starts with one provenance line (decision Q22: no author recovery; the
patch is used under ROCKNIX's licence, GPL-2.0). patch(1) and git apply skip text before
the first diff, so the line changes nothing at apply time. Headers that exist are kept
byte for byte. Like scripts/unpack, @DEVICE@ and @TARGET_CPU@ are substituted in patches
that are not git binary or rename patches.
"""

from __future__ import annotations

import re
from collections import OrderedDict

from .rocknix import ImportError_

PROVENANCE_PREFIX = "Thimsy-Import-Provenance: "
LICENCE_NOTE = "GPL-2.0, used under ROCKNIX's licence; author not recovered (Q22)"

_GIT_APPLY = re.compile(rb"^(GIT binary patch|rename from|rename to)$", re.M)


def _label(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s)


def source_dirs(tree, family: str, version: str, patch_dirs: list, arch: str) -> list:
    """[(label, Path)] in scripts/unpack's order; directories that do not exist included."""
    pkg = tree.linux_pkg_dir() / "patches"
    prj = tree.project / "patches" / "linux"
    dev = tree.device_dir(family) / "patches" / "linux"
    out = [("common", pkg), ("common-" + arch, pkg / arch)]
    for d in patch_dirs:
        if d.startswith("/"):
            raise ImportError_("absolute patch dir %s not supported" % d)
        out.append(("common-" + d, pkg / d))
    out += [("common-" + version, pkg / version), ("common-%s-%s" % (version, arch), pkg / version / arch),
            ("project", prj), ("project-" + arch, prj / arch)]
    for d in patch_dirs:
        out.append(("project-" + d, prj / d))
        out.append(("device-" + d, dev / d))
    out += [("project-" + version, prj / version), ("device", dev)]
    return out


class Patch:
    def __init__(self, name, rel, data, dest_dir):
        self.name = name
        self.rel = rel          # ROCKNIX path
        self.data = data        # bytes, newlines normalised, substitutions done
        self.dest_dir = dest_dir

    @property
    def has_from(self) -> bool:
        head = self.data.split(b"\ndiff ", 1)[0].split(b"\n--- ", 1)[0]
        return re.search(rb"^From:", head, re.M) is not None


def flatten(tree, family: str, version: str, patch_dirs: list, arch: str, target_cpu: str) -> list:
    """The ordered series: [Patch]. Stops when a directory would be applied twice, which
    ROCKNIX's build would also do (and then fail on)."""
    series = []
    seen_dirs = OrderedDict()
    n = 0
    for label, d in source_dirs(tree, family, version, patch_dirs, arch):
        if not d.is_dir():
            continue
        files = [p for p in d.iterdir() if p.name.endswith(".patch") and not p.name.startswith(".")
                 and tree.resolve(p) is not None]
        if not files:
            continue
        key = str(d.resolve())
        if key in seen_dirs:
            raise ImportError_("%s is applied twice (as %s and %s): ROCKNIX's order is broken"
                               % (tree.rel(d), seen_dirs[key], label))
        seen_dirs[key] = label
        n += 1
        dest = "%02d-%s" % (n, _label(label))
        for p in sorted(files, key=lambda q: q.name.encode("utf-8")):
            data = tree.read_bytes(p)
            if not _GIT_APPLY.search(data):
                data = data.replace(b"@TARGET_CPU@", target_cpu.encode()).replace(b"@DEVICE@", family.encode())
            series.append(Patch(p.name, tree.rel(p), data, dest))
    names = OrderedDict()
    for p in series:
        k = (p.dest_dir, p.name)
        if k in names:
            raise ImportError_("duplicate patch %s/%s" % k)
        names[k] = p
    return series


def provenance_line(tree, rel: str) -> str:
    return "%s%s; %s" % (PROVENANCE_PREFIX, tree.provenance(rel), LICENCE_NOTE)


def render(tree, patch: Patch) -> bytes:
    return provenance_line(tree, patch.rel).encode("utf-8") + b"\n" + patch.data
