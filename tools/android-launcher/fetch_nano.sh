#!/bin/sh
# Nano's source as Thimsy builds it, outside an AOSP tree: GammaOS's
# frameworks/base/cmds/gammaos-nano at the base commit in tools/nano-tree/upstream.json, with
# Thimsy's changes (nano/ in this repository) on top. For android_launcher_plan.md (Phase 0, item 1).
#
#   tools/android-launcher/fetch_nano.sh [DEST]      (default: work/android-launcher/upstream)
#
# A wrapper around tools/nano-tree/nano_tree.py (docs/linux/IMPLEMENTATION.md A.4): `fetch` makes
# the base cache work/nano-base/ (only the Nano folder's blobs, about 100 MB, and nothing when the
# cache is already complete), then `compose` writes DEST/frameworks/base/cmds/gammaos-nano: the
# base's asset files overlaid by nano/. The files the patch's Nano block would make are exactly
# nano/'s (nano_tree.py regen --check). Set PYTHON to choose the interpreter.
set -eu

NANO=frameworks/base/cmds/gammaos-nano

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:-$ROOT/work/android-launcher/upstream}
TOOL=$ROOT/tools/nano-tree/nano_tree.py

if [ -n "${PYTHON:-}" ]; then
    PY=$PYTHON
elif python3 -c 'import sys' >/dev/null 2>&1; then
    PY=python3
elif py -3 -c 'import sys' >/dev/null 2>&1; then
    PY="py -3"
else
    echo "fetch_nano.sh: needs Python 3.8 or later (set PYTHON)" >&2
    exit 1
fi

$PY "$TOOL" fetch
rm -rf "${DEST:?}/$NANO"
mkdir -p "$DEST/$(dirname "$NANO")"
$PY "$TOOL" compose "$DEST/$NANO"

echo "source  $DEST/$NANO"
find "$DEST/$NANO" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' \) | wc -l | sed 's/^ */files   /'
