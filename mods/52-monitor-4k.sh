#!/usr/bin/env bash
# Lets a monitor or TV run at 3840x2160, at 60 Hz as YCbCr 4:2:0 (1.4.4 base
# only). Two build.prop defaults turn it on:
#
# - ro.gammaos.display.uhd: system_server lists a monitor's 3840-wide modes
#   (GammaosScreenArrangement, in the patch); without it Output Resolution
#   never offers them.
# - ro.gammaos.display.y420: at a 3840x2160 mode of 48 Hz or more on a
#   monitor, SurfaceFlinger composes every layer into one NV12 (4:2:0) client
#   target (ThimsyYuv420 in CompositionEngine, in the patch), which the
#   DisplayPort driver sends as YCbCr 4:2:0: the only way 4K60 fits the Cube's
#   two DisplayPort lanes, and DPU1 has no RGB-to-YUV converter of its own.
#
# Both only take effect when the DisplayPort driver offers a 3840x2160 mode
# (scripts/sprd_drm.py, UHD and Y420) and the monitor lists it, so a monitor
# without 4K, or a driver without those modes, runs as before. Anything else
# (another resolution, a rate below 48 Hz, the Cube's own screen, a converter
# that can't start) keeps SurfaceFlinger's ordinary RGB composition. This is
# what test 42's T3 set by hand (resetprop) when it showed 4K60 with correct
# colours.
#
# The system's SurfaceFlinger must have the 4:2:0 output (a system compiled
# from the current patch): without it, 3840-wide modes would be listed with
# nothing composing them into one layer.
#
# scripts/build.sh sets THIMSY_MONITOR_4K=1 only when the driver it builds
# offers 4K60 4:2:0 (sprd_drm.py's Y420) and its composer keeps a 3840-wide
# client target (COMPOSER_FIX 1 or ct4k); without it this changes nothing, so
# no build lists a 4K mode its driver or composer can't show.
#
# Usage: 52-monitor-4k.sh SYSTEM_DIR CACHE_DIR
set -euo pipefail

SYSTEM="$1"
PROP="$SYSTEM/build.prop"
MODS="$(cd "$(dirname "$0")" && pwd)"
work="$(realpath -m "$2")/monitor-4k"
source "$MODS/../scripts/modlib.sh"

if ! is_nano_base "$SYSTEM"; then
  echo "4K monitor modes come only with the 1.4.4 base; skipping"
  exit 0
fi
if [[ "${THIMSY_MONITOR_4K:-0}" != 1 ]]; then
  echo "this build's DisplayPort driver or composer has no 4K60 4:2:0 (THIMSY_MONITOR_4K, scripts/build.sh); skipping"
  exit 0
fi
# The property SurfaceFlinger's 4:2:0 output reads (gammaYuv420Mode, SurfaceFlinger.cpp).
if ! sudo grep -qaF "ro.gammaos.display.y420" "$SYSTEM/bin/surfaceflinger"; then
  echo "this system's SurfaceFlinger has no 4:2:0 output (ro.gammaos.display.y420); compile the system from the current patches/thimsy/0001-thimsy.patch" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"
sudo cat "$PROP" >"$work/build.prop"
sed -i -E -e '/^ro\.gammaos\.display\.(uhd|y420)=/d' \
  -e '/^# Thimsy OS: 4K monitor modes/d' "$work/build.prop"
cat >>"$work/build.prop" <<'EOF'
# Thimsy OS: 4K monitor modes, 4K60 as YCbCr 4:2:0 (mods/52-monitor-4k.sh)
ro.gammaos.display.uhd=true
ro.gammaos.display.y420=true
EOF
write_through "$work/build.prop" "$PROP"
rm -rf "$work"
echo "3840x2160 monitor modes on, 4K60 as YCbCr 4:2:0"
