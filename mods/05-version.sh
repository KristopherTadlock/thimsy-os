#!/usr/bin/env bash
# Sets the version Settings > About phone shows to this release, records it as
# ro.thimsy.version for Nano's System Information, and replaces the build
# machine's user and host names, which a source build records.
# THIMSY_RELEASE (for example "test 40") names the build within that version, as
# ro.thimsy.release: Nano shows it after the version, as "Thimsy 1.0a test 40",
# at the end of Quick Menu > Device and in System Information.
# THIMSY_COMMIT (scripts/build.sh sets it from git: "373a8f8", or "373a8f8-dirty"
# with uncommitted changes) is recorded as ro.thimsy.commit for Quick Menu > Device.
#
# Usage: 05-version.sh SYSTEM_DIR CACHE_DIR
set -euo pipefail

PROP="$1/build.prop"
VERSION="${THIMSY_BUILD_VERSION:-Thimsy_OS_v1.0}"
THIMSY="${THIMSY_VERSION:-1.0}"
RELEASE="${THIMSY_RELEASE:-}"
# Letters, digits, spaces, dots, hyphens and underscores: it goes into build.prop and Nano's menu.
[[ "$RELEASE" =~ ^[A-Za-z0-9._-]+( [A-Za-z0-9._-]+)*$ || -z "$RELEASE" ]] && ((${#RELEASE} <= 40)) ||
  { echo "THIMSY_RELEASE must be up to 40 letters, digits, dots, hyphens and underscores, in words split by single spaces: \"$RELEASE\"" >&2; exit 1; }
COMMIT="${THIMSY_COMMIT:-}"
[[ -z "$COMMIT" || "$COMMIT" =~ ^[0-9a-f]{7,40}(-dirty)?$ ]] ||
  { echo "THIMSY_COMMIT must be a git commit hash (7 to 40 hex digits), optionally with -dirty: \"$COMMIT\"" >&2; exit 1; }
work="$(realpath -m "$2")/version"
source "$(dirname "$0")/../scripts/modlib.sh"

mkdir -p "$work"
sudo cat "$PROP" >"$work/build.prop"
for key in ro.lineage.version ro.lineage.display.version; do
  grep -qE "^$key=GammaOS_Next_(Full_)?v[0-9.]+$" "$work/build.prop" ||
    { echo "build.prop has no GammaOS Next $key" >&2; exit 1; }
done
sed -i -E \
  -e "s/^(ro\.lineage(\.display)?\.version)=GammaOS_Next_(Full_)?v[0-9.]+$/\1=$VERSION/" \
  -e "s/^ro\.build\.user=.*/ro.build.user=gammaos/" \
  -e "s/^ro\.build\.host=.*/ro.build.host=localhost/" \
  -e "/^ro\.thimsy\.version=/d" \
  -e "/^ro\.thimsy\.release=/d" \
  -e "/^ro\.thimsy\.commit=/d" \
  "$work/build.prop"
echo "ro.thimsy.version=$THIMSY" >>"$work/build.prop"
[[ -z "$RELEASE" ]] || echo "ro.thimsy.release=$RELEASE" >>"$work/build.prop"
[[ -z "$COMMIT" ]] || echo "ro.thimsy.commit=$COMMIT" >>"$work/build.prop"
write_through "$work/build.prop" "$PROP"
rm -rf "$work"
echo "version: $VERSION (ro.thimsy.version=$THIMSY${RELEASE:+, ro.thimsy.release=$RELEASE}${COMMIT:+, ro.thimsy.commit=$COMMIT})"
