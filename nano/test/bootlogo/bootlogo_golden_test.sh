#!/bin/bash
# Golden test for the boot screen picture: nano-bootlogo draws bootscreen/thimsy.svg with Roboto at
# the panel sizes Thimsy ships on, with and without a status line, and each frame's SHA-256 must
# match the blessed one in this folder. Any change to NanoBootArt, nanosvg, FreeType or the art
# shows up here; if the change is meant, re-bless with --bless and check the new frames by eye.
#
# usage: bootlogo_golden_test.sh [--bless] [--keep DIR]
#
# Run by Soong as nano_bootlogo_golden_test (the tool, the SVG, the font and the .sha256 files are
# installed beside it), or straight from the source tree after `m nano-bootlogo`.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BLESS=0
KEEP=""
while [ $# -gt 0 ]; do
    case "$1" in
        --bless) BLESS=1 ;;
        --keep) KEEP="$2"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

first() { for p in "$@"; do [ -e "$p" ] && { echo "$p"; return 0; }; done; return 1; }

# The installed layout keeps each file's path relative to its module (test/bootlogo/..., bootscreen/
# ...); a filegroup from another package lands at the top. The source tree is the fallback.
NANO_SRC=$(cd "$HERE/../.." && pwd)
TOP=${ANDROID_BUILD_TOP:-$(cd "$NANO_SRC/../../../.." && pwd)}
TOOL=$(first "$HERE/nano-bootlogo" "$HERE/../../nano-bootlogo" "${ANDROID_HOST_OUT:-$TOP/out/host/linux-x86}/bin/nano-bootlogo" \
       "$(command -v nano-bootlogo || true)") || { echo "nano-bootlogo not found (m nano-bootlogo)" >&2; exit 1; }
SVG=$(first "$HERE/bootscreen/thimsy.svg" "$HERE/../../bootscreen/thimsy.svg" "$NANO_SRC/bootscreen/thimsy.svg") \
    || { echo "thimsy.svg not found" >&2; exit 1; }
FONT=$(first "$HERE/Roboto-Regular.ttf" "$HERE/../../Roboto-Regular.ttf" "$TOP/external/roboto-fonts/Roboto-Regular.ttf") \
    || { echo "Roboto-Regular.ttf not found" >&2; exit 1; }
if [ "$BLESS" = 1 ]; then
    GOLDEN="$NANO_SRC/test/bootlogo"
else
    GOLDEN=$(dirname "$(first "$HERE/test/bootlogo/base_1280x720.sha256" "$HERE/base_1280x720.sha256")") \
        || { echo "golden .sha256 files not found" >&2; exit 1; }
fi

OUT=${KEEP:-$(mktemp -d)}
mkdir -p "$OUT"
[ -n "$KEEP" ] || trap 'rm -rf "$OUT"' EXIT

STATUS="Updating..."
fail=0
for size in 1280x720 640x480; do
    w=${size%x*}; h=${size#*x}
    for kind in base status; do
        name=${kind}_${size}
        if [ "$kind" = base ]; then
            "$TOOL" "$SVG" "$FONT" "$w" "$h" "$OUT/$name.rgba"
        else
            "$TOOL" "$SVG" "$FONT" "$w" "$h" "$OUT/$name.rgba" "$STATUS"
        fi
        got=$(sha256sum "$OUT/$name.rgba" | cut -d' ' -f1)
        if [ "$BLESS" = 1 ]; then
            echo "$got" > "$GOLDEN/$name.sha256"
            echo "blessed $name $got"
            continue
        fi
        want=$(tr -d ' \n' < "$GOLDEN/$name.sha256")
        if [ "$got" = "$want" ]; then
            echo "PASS $name"
        else
            echo "FAIL $name: got $got, want $want (frame: $OUT/$name.rgba, ${w}x${h} RGBA)"
            fail=1
        fi
    done
done
exit $fail
