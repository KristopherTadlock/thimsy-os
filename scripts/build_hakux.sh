#!/usr/bin/env bash
# Builds the build's hakuX APK (mods/added-apps.txt) from the source that
# scripts/hakux_source.sh assembles, as hakuX's android/README.md describes:
# the native core (CMake Release), its libraries copied into the app, then the
# release variant, zip-aligned and signed with the given key.
#
# The native libraries embed their source paths, so the source is copied to
# BUILD_ROOT/src (hakuX) and BUILD_ROOT/deps (NC_DEPS) and built there. The
# default BUILD_ROOT is where the build's APK was built. The libraries' build
# IDs also cover the NDK's path: with the SDK at BUILD_ROOT/sdk too, as it
# was, the APK is the same byte for byte when signed with the same key.
#
# Usage: build_hakux.sh SRC_DIR KEYSTORE KEY_ALIAS PASS_FILE OUT_APK
#   SRC_DIR    hakux_source.sh's DIR (or the unpacked source tarball)
#   PASS_FILE  a file holding the keystore and key password
# Environment:
#   BUILD_ROOT    default /tmp/hakux-mali-build-20261109; its src and deps
#                 must not exist yet, and are left in place afterwards
#   ANDROID_HOME  an SDK with platforms;android-36, build-tools;36.1.0 and
#                 ndk;29.0.14206865 (sdkmanager installs them)
#   JAVA_HOME     JDK 21
set -euo pipefail

SRC="$(cd "$1" && pwd)"
KEYSTORE="$(realpath "$2")"
ALIAS="$3"
PASS="$(realpath "$4")"
OUT="$(realpath -m "$5")"
ROOT="${BUILD_ROOT:-/tmp/hakux-mali-build-20261109}"
: "${ANDROID_HOME:?set ANDROID_HOME}" "${JAVA_HOME:?set JAVA_HOME}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK="$ANDROID_HOME/ndk/29.0.14206865"
export PATH="$JAVA_HOME/bin:$PATH"
BT="$ANDROID_HOME/build-tools/36.1.0"
for t in "$ANDROID_NDK/build/cmake/android.toolchain.cmake" "$BT/zipalign" "$BT/apksigner"; do
  [[ -e "$t" ]] || { echo "missing $t" >&2; exit 1; }
done

[[ ! -e "$ROOT/src" && ! -e "$ROOT/deps" ]] || { echo "$ROOT/src or $ROOT/deps already exists" >&2; exit 1; }
mkdir -p "$ROOT"
cp -a "$SRC/hakuX" "$ROOT/src"
cp -a "$SRC/deps" "$ROOT/deps"
cd "$ROOT/src"
# FEX names its version and commit from git: in the build's shallow checkout, no version (no tags)
# and the full commit. Given here, the source tarball (no .git) builds the same.
cmake -S native -B build/native/core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS="$ROOT/deps" -DOVERRIDE_VERSION= -DOVERRIDE_HASH=52018bf46a8279b22bce738548ab3f40db467064
cmake --build build/native/core --target hakuxcore
bash tools/native/app-libs.sh

# The release variant, unsigned (no key.properties); lint is skipped, as the build's was. hakuX's
# gradle.properties names its author's JDK; this one is used instead.
(cd android && bash ./gradlew --no-daemon --console=plain "-Dorg.gradle.java.home=$JAVA_HOME" assembleRelease \
   -x lintVitalAnalyzeRelease -x lintVitalReportRelease -x lintVitalRelease)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$BT/zipalign" -f -p 4 android/app/build/outputs/apk/release/app-release-unsigned.apk "$work/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass "file:$PASS" --ks-key-alias "$ALIAS" \
  --out "$OUT" "$work/aligned.apk"
"$BT/apksigner" verify "$OUT"
sha256sum "$OUT"
