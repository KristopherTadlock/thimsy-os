#!/usr/bin/env bash
# Builds the PanVK driver package (tools/panvk-driver) into OUT_APK from the
# PanVK library DRIVER_SO, signed with KEYSTORE. Set NEW_KEYSTORE=1 to create
# KEYSTORE when there is none: a device only takes an update of the package
# signed with the key it has, so a new key must be a choice, not the result of
# a wiped cache.
#
# The Vulkan loader loads the driver as vulkan.<ro.board.platform>.so
# (vulkan.ums9620.so on the RG Cube) from the APK. The package manager only
# counts libraries named lib*.so when it works out an APK's ABI, without which
# the framework skips the driver, so an empty libpanvk_abi.so stands beside it.
# Both are stored uncompressed and page aligned, so they load from the APK.
#
# Needs keytool (JDK 17), the Android build tools aapt2, zipalign and
# apksigner, and zip, on PATH, and ANDROID_JAR set to an android.jar for API 34.
#
# Usage: build_panvk_driver.sh OUT_APK KEYSTORE DRIVER_SO
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../tools/panvk-driver" && pwd)"
PLATFORM=ums9620
out="$(realpath -m "$1")"
keystore="$(realpath -m "$2")"
driver="$(realpath -m "$3")"
: "${ANDROID_JAR:?set ANDROID_JAR to an API 34 android.jar}"
if [[ ! -f "$keystore" && "${NEW_KEYSTORE:-}" != 1 ]]; then
  echo "no keystore $keystore: restore it, or set NEW_KEYSTORE=1 to sign with a new key" >&2
  exit 1
fi
for tool in keytool aapt2 zipalign apksigner zip; do
  command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 1; }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
aapt2 link -o "$work/unsigned.apk" -I "$ANDROID_JAR" --manifest "$SRC/AndroidManifest.xml"
mkdir -p "$work/lib/arm64-v8a"
cp "$driver" "$work/lib/arm64-v8a/vulkan.$PLATFORM.so"
: >"$work/lib/arm64-v8a/libpanvk_abi.so"
# The release's date and fixed modes for both, with zip's local time in UTC, so the same inputs make
# the same APK on any machine.
chmod 644 "$work/lib/arm64-v8a/vulkan.$PLATFORM.so" "$work/lib/arm64-v8a/libpanvk_abi.so"
touch -d @1793404800 "$work/lib/arm64-v8a/vulkan.$PLATFORM.so" "$work/lib/arm64-v8a/libpanvk_abi.so"
(cd "$work" && TZ=UTC zip -q -0 -X unsigned.apk "lib/arm64-v8a/vulkan.$PLATFORM.so" lib/arm64-v8a/libpanvk_abi.so)
zipalign -f -p 4 "$work/unsigned.apk" "$work/aligned.apk"
zipalign -c -p 4 "$work/aligned.apk"

if [[ ! -f "$keystore" ]]; then
  mkdir -p "$(dirname "$keystore")"
  keytool -genkeypair -keystore "$keystore" -storepass android -alias panvk \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Thimsy PanVK driver" >/dev/null 2>&1
fi
mkdir -p "$(dirname "$out")"
apksigner sign --ks "$keystore" --ks-pass pass:android --out "$out" "$work/aligned.apk"
echo "built $out"
