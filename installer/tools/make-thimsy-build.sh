#!/usr/bin/env bash
# Turns a Thimsy firmware built by scripts/build.sh into a Thimsy package for
# the universal installer: one tar holding the shared system image, the
# device's patches against its official firmware and pack.json, the manifest
# that describes them (format, device, profile, release, min_installer, each
# file's size and sha256, the build steps, and the sha256s of the official
# firmware and of the PAC the steps rebuild). Never a whole firmware. Checks
# the installer rebuilds the exact same PAC from the package and the official
# firmware alone, then lays it out for shipping.
#
# The installer accepts any package whose own pack.json checks out, so a new
# build needs no catalog change and no new installer.
#
#   make-thimsy-build.sh [options] THIMSY_PAC OUT_DIR
#
#   --device ID       catalog device (default anbernic-rg-cube)
#   --profile NAME    light | medium | heavy (default heavy)
#   --release TEXT    the release pack.json names, e.g. "Thimsy 1.0a test 31"
#                     (default "Thimsy <catalog release>")
#   --system-name ID  shared system image id (default thimsy-system-<release>-<profile>)
#   --source PAC      the official firmware; downloaded from its upstream
#                     release (the catalog's source) when not given
#   --apply           also point the catalog's default download for the
#                     profile at this package (installer/catalog/devices/ID.yaml,
#                     see apply_pack.py); only needed when its name changes
#   --tag TAG         with --apply: download it from release TAG rather than
#                     the latest release
#
# OUT_DIR gets thimsy-build/, holding the package
# Thimsy-<release>-<device>-<profile>.tar, split into 1 GB .tar.001, .002, ...
# parts only if it's over GitHub's 2 GiB asset limit. Upload it to the
# release, or ship it next to the app (the engine looks in <app>/thimsy-build).
#
# Needs: the engine (cargo build --release in installer/engine, or ENGINE=...)
# and python3. About 3 x the PAC size of free space while it runs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
INSTALLER="$(dirname "$HERE")"
ENGINE="${ENGINE:-$INSTALLER/engine/target/release/thimsy-installer}"
DEVICE=anbernic-rg-cube PROFILE=heavy TAG="" SYSTEM_NAME="" SOURCE="" APPLY=0 RELEASE_TEXT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    --release) RELEASE_TEXT="$2"; shift 2 ;;
    --tag) TAG="$2"; shift 2 ;;
    --system-name) SYSTEM_NAME="$2"; shift 2 ;;
    --source) SOURCE="$2"; shift 2 ;;
    --apply) APPLY=1; shift ;;
    -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[[ $# -eq 2 ]] || { echo "usage: $0 [options] THIMSY_PAC OUT_DIR (--help)" >&2; exit 2; }
PAC="$(realpath "$1")"; OUT="$2"
# Absolute before anything changes directory (a bare name is looked up on PATH).
if [[ "$ENGINE" != */* ]]; then ENGINE="$(command -v "$ENGINE" || echo "./$ENGINE")"; fi
[[ -x "$ENGINE" ]] || { echo "no engine at $ENGINE (cargo build --release in installer/engine)" >&2; exit 1; }
ENGINE="$(cd "$(dirname "$ENGINE")" && pwd)/$(basename "$ENGINE")"
[[ -f "$PAC" ]] || { echo "no file $PAC" >&2; exit 1; }
CATALOG="$INSTALLER/catalog"
DEVICE_YAML="$CATALOG/devices/$DEVICE.yaml"
[[ -f "$DEVICE_YAML" ]] || { echo "no catalog device $DEVICE" >&2; exit 1; }

release=$(sed -n 's/^release: *"\{0,1\}Thimsy \([^"]*\)"\{0,1\}$/\1/p' "$CATALOG/catalog.yaml")
RELEASE_TEXT="${RELEASE_TEXT:-Thimsy $release}"
SYSTEM_NAME="${SYSTEM_NAME:-thimsy-system-$release-$PROFILE}"

mkdir -p "$OUT"; OUT="$(realpath "$OUT")"
REAL_INSTALLER="$(realpath "$INSTALLER")"
if [[ "$OUT" == "$REAL_INSTALLER" || "$OUT/" == "$REAL_INSTALLER/"* ]]; then
  echo "OUT_DIR $OUT is inside the installer's own folder; choose another folder" >&2
  exit 2
fi
WORK="$OUT/.work"; rm -rf "$WORK" "$OUT/thimsy-build"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [[ -z "$SOURCE" ]]; then
  echo "== downloading the official firmware for $DEVICE"
  SOURCE=$("$ENGINE" --catalog "$CATALOG" dev source --device "$DEVICE" --workspace "$WORK/source")
fi
SOURCE="$(realpath "$SOURCE")"

echo "== making the package's files (system image + patches + pack.json)"
"$ENGINE" --catalog "$CATALOG" dev pack --source "$SOURCE" --reference "$PAC" --out "$WORK/pack" \
  --device "$DEVICE" --profile "$PROFILE" --release "$RELEASE_TEXT" --system-name "$SYSTEM_NAME"

echo "== putting them in one archive"
mkdir -p "$OUT/thimsy-build"
BUNDLE="$OUT/thimsy-build/Thimsy-$release-$DEVICE-$PROFILE.tar"
mapfile -t members < <(python3 -I -c "
import json, sys
p = json.load(open(sys.argv[1]))
print('pack.json'); print(p['system']['file'])
for a in p['artifacts']: print(a['file'])" "$WORK/pack/pack.json")
(cd "$WORK/pack" && "$ENGINE" dev bundle --out "$BUNDLE" "${members[@]}") > "$WORK/bundle.json"
rm -rf "$WORK/pack"
"$ENGINE" dev bundle-list "$OUT/thimsy-build"/Thimsy-*.tar*

echo "== rebuilding with the installer from the package alone, and comparing"
want=$(sha256sum "$PAC" | cut -d' ' -f1)
picks=()
for f in "$OUT/thimsy-build"/Thimsy-*.tar*; do picks+=(--import "$f"); done
"$ENGINE" --catalog "$CATALOG" build --device "$DEVICE" --profile "$PROFILE" \
  --workspace "$WORK/ws" "${picks[@]}" --import "$(dirname "$SOURCE")"
got=$(sha256sum "$WORK"/ws/jobs/*/out/*.pac | cut -d' ' -f1)
if [[ "$got" != "$want" ]]; then
  echo "MISMATCH: the installer built $got, $PAC is $want" >&2; exit 1
fi
echo "byte-identical: $want"

if [[ $APPLY -eq 1 ]]; then
  python3 -I "$HERE/apply_pack.py" ${TAG:+--tag "$TAG"} "$WORK/bundle.json" "$DEVICE_YAML" "$PROFILE"
  "$ENGINE" --catalog "$CATALOG" dev check-catalog
  echo "== written into $DEVICE_YAML (rebuild the engine to embed it)"
fi
du -sh "$OUT/thimsy-build"
ls -l "$OUT/thimsy-build"
