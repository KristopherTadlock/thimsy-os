#!/bin/bash
# Golden test for the button map panel: nano-buttonmap draws each scene in scenes/ with Roboto (and
# Noto Sans Hebrew for the right-to-left scene) at the screen sizes Thimsy meets, and each frame's
# SHA-256 must match its line in goldens.sha256. Any change to NanoButtonMapArt, the controller art,
# nanosvg or FreeType shows up here; if the change is meant, re-bless with --bless and check the new
# frames by eye (--keep DIR writes them as PNGs too).
#
# usage: buttonmap_golden_test.sh [--bless] [--keep DIR]
#
# Run by Soong as nano_buttonmap_golden_test (the tool, the scenes, the art, the fonts and
# goldens.sha256 are installed beside it), or straight from the source tree after `m nano-buttonmap`.
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

# The installed layout keeps each file's path relative to its module (test/buttonmap/..., ps3xmb/
# ...); a filegroup from another package lands at the top. The source tree is the fallback.
NANO_SRC=$(cd "$HERE/../.." && pwd)
TOP=${ANDROID_BUILD_TOP:-$(cd "$NANO_SRC/../../../.." && pwd)}
TOOL=$(first "$HERE/nano-buttonmap" "$HERE/../../nano-buttonmap" "${ANDROID_HOST_OUT:-$TOP/out/host/linux-x86}/bin/nano-buttonmap" \
       "$(command -v nano-buttonmap || true)") || { echo "nano-buttonmap not found (m nano-buttonmap)" >&2; exit 1; }
SCENES=$(first "$HERE/test/buttonmap/scenes" "$HERE/scenes") || { echo "scenes not found" >&2; exit 1; }
ART=$(first "$HERE/ps3xmb/controllers" "$HERE/../../ps3xmb/controllers" "$NANO_SRC/ps3xmb/controllers") \
    || { echo "ps3xmb/controllers not found" >&2; exit 1; }
FONT=$(first "$HERE/Roboto-Regular.ttf" "$HERE/../../Roboto-Regular.ttf" "$TOP/external/roboto-fonts/Roboto-Regular.ttf") \
    || { echo "Roboto-Regular.ttf not found" >&2; exit 1; }
HEBREW=$(first "$HERE/notosanshebrew/NotoSansHebrew-Regular.ttf" "$HERE/NotoSansHebrew-Regular.ttf" \
         "$TOP/external/noto-fonts/notosanshebrew/NotoSansHebrew-Regular.ttf") \
    || { echo "NotoSansHebrew-Regular.ttf not found" >&2; exit 1; }
TOPNG=$(first "$HERE/test/buttonmap/rgba2png.py" "$HERE/rgba2png.py") || TOPNG=""
if [ "$BLESS" = 1 ]; then
    GOLDEN="$NANO_SRC/test/buttonmap/goldens.sha256"
else
    GOLDEN=$(first "$HERE/test/buttonmap/goldens.sha256" "$HERE/goldens.sha256") || { echo "goldens.sha256 not found" >&2; exit 1; }
fi

OUT=${KEEP:-$(mktemp -d)}
mkdir -p "$OUT"
[ -n "$KEEP" ] || trap 'rm -rf "$OUT"' EXIT

SIZES="640x480 720x720 960x544 1024x768 1280x720 1920x1080 1080x1920"
fail=0
blessed=""
for scene in "$SCENES"/*.json; do
    name=$(basename "$scene" .json)
    sizes=$SIZES
    # The theme palettes: the two sizes Nano's themes are tuned at.
    case "$name" in snes_dsi|snes_minima) sizes="720x720 1280x720" ;; esac
    for size in $sizes; do
        w=${size%x*}; h=${size#*x}
        frame="${name}_${size}"
        "$TOOL" "$scene" "$FONT" "$ART" "$w" "$h" "$OUT/$frame.rgba" --fallback "$HEBREW"
        [ -n "$KEEP" ] && [ -n "$TOPNG" ] && python3 -I "$TOPNG" "$OUT/$frame.rgba" "$w" "$h" "$OUT/$frame.png"
        got=$(sha256sum "$OUT/$frame.rgba" | cut -d' ' -f1)
        [ -n "$KEEP" ] || rm -f "$OUT/$frame.rgba"
        if [ "$BLESS" = 1 ]; then
            blessed+="$frame $got"$'\n'
            continue
        fi
        want=$(awk -v f="$frame" '$1 == f { print $2 }' "$GOLDEN")
        if [ "$got" = "$want" ]; then
            echo "PASS $frame"
        else
            echo "FAIL $frame: got $got, want ${want:-none} (frame: $OUT/$frame.rgba, ${w}x${h} RGBA)"
            fail=1
        fi
    done
done
if [ "$BLESS" = 1 ]; then
    printf '%s' "$blessed" > "$GOLDEN"
    echo "blessed $(wc -l < "$GOLDEN") frames into $GOLDEN"
fi
exit $fail
