"""Shared helpers for thimsy-import's tests (standard library only; Python 3.8+).

Regenerate the golden files after an intended change:

    THIMSY_REGEN_GOLDEN=1 python3 -m unittest discover -s linux/tests/thimsy_import -t linux/tests/thimsy_import -k golden
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LINUX = HERE.parents[1]
TOOL = LINUX / "tools" / "thimsy-import"
FIXTURE = LINUX / "tests" / "fixtures" / "rocknix-mini"
EXPECTED = LINUX / "tests" / "fixtures" / "expected"
DATA = HERE / "data"
COMMIT = "0f4806b59bfd1d3e6b17e3a632e95ced82a28719"
FAMILIES = ("RK3566", "H700")

if str(TOOL) not in sys.path:
    sys.path.insert(0, str(TOOL))

from thimsy_import import cli  # noqa: E402


def run(out, family, rocknix=None, *extra):
    """Run the importer in-process; return (exit code, stdout, stderr)."""
    argv = ["--rocknix", str(rocknix or FIXTURE), "--tag", "next", "--commit", COMMIT,
            "--family", family, "--out", str(out),
            "--whence", str(FIXTURE / "linux-firmware" / "WHENCE")]
    if not any(a == "--fragment" for a in extra):
        argv += ["--fragment", "none"]
    argv += list(extra)
    o, e = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(o), contextlib.redirect_stderr(e):
        code = cli.main(argv)
    return code, o.getvalue(), e.getvalue()


def tree_bytes(root: Path) -> dict:
    out = {}
    for dirpath, _dirs, files in os.walk(str(root)):
        for f in files:
            p = Path(dirpath) / f
            out[p.relative_to(root).as_posix()] = p.read_bytes()
    return out


def snapshot(root: Path) -> dict:
    """relpath -> (bytes, mtime_ns), to show that a run wrote nothing."""
    out = {}
    for dirpath, _dirs, files in os.walk(str(root)):
        for f in files:
            p = Path(dirpath) / f
            out[p.relative_to(root).as_posix()] = (p.read_bytes(), p.stat().st_mtime_ns)
    return out


class TempDir:
    def __enter__(self) -> Path:
        self.path = Path(tempfile.mkdtemp(prefix="thimsy-import-test-"))
        return self.path

    def __exit__(self, *exc):
        shutil.rmtree(str(self.path), ignore_errors=True)


def copy_fixture(dst: Path) -> Path:
    shutil.copytree(str(FIXTURE), str(dst / "rocknix"))
    return dst / "rocknix"
