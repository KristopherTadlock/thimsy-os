#!/usr/bin/env bash
# Adds PanVK (open Mesa Vulkan for Mali, on the vendor's mali_kbase) as an
# alternate Vulkan driver: Android's updatable graphics driver
# (ro.gfx.driver.0). Apps keep the Mali driver unless they are switched to
# PanVK under Developer options > Graphics Driver Preferences ("Game Driver");
# Android never gives it to the apps built into the system image. Emulators
# that load a driver package themselves get PanVK's in /sdcard/GPU Drivers
# (added-apps.txt).
#
# The package is signed with work/cache/panvk/panvk-driver.keystore, like the
# Android Games helper; scripts/build_panvk_driver.sh lists the tools it needs.
#
# Usage: 48-panvk.sh SYSTEM_DIR CACHE_DIR
set -euo pipefail

SYSTEM="$1"
cache="$(realpath -m "$2")/panvk"
PROP="$SYSTEM/build.prop"
PACKAGE=org.thimsy.panvk
# PanVK from Mesa 26.3.0-devel (git 795ca7ff3e), packaged by
# panvk-kbase-android 26.3.1: one library for Bifrost and Valhall Mali GPUs,
# the RG Cube's Mali-G57 among them. MIT (Mesa); its kbase UAPI headers are
# GPL-2.0 WITH Linux-syscall-note. The license texts
# (tools/panvk-driver/licenses) go to /system/etc/Mesa-PanVK-licenses.
URL=https://github.com/zenithblue-oss/panvk-kbase-android/releases/download/panvk-kbase-v26.3.1/libvulkan_panfrost-android-aarch64.so
SHA256=62c2b3f98ca987d5ab50b6f01e2d2c1c5b3ee995fe93666aa5da9415f65c3348
DRIVER="$cache/libvulkan_panfrost-26.3.1.so"
source "$(dirname "$0")/../scripts/modlib.sh"

# The license texts on both bases: 40-added-apps.sh puts the driver package in
# /system/etc and /sdcard/GPU Drivers on the 1.2 base too.
lic_dir="$SYSTEM/etc/Mesa-PanVK-licenses"
sudo install -d -m 755 -o root -g root "$lic_dir"
sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$lic_dir"
for f in "$(dirname "$0")"/../tools/panvk-driver/licenses/*; do
  install_system_file "$f" "$lic_dir/$(basename "$f")"
done
if ! is_nano_base "$SYSTEM"; then
  echo "PanVK is the system's alternate driver only on the 1.4.4 base; skipping that"
  exit 0
fi
mkdir -p "$cache"
[[ -f "$DRIVER" ]] || { curl -fsSL --retry 3 -o "$DRIVER.partial" "$URL" && mv "$DRIVER.partial" "$DRIVER"; } ||
  { echo "download failed: $URL" >&2; exit 1; }
echo "$SHA256  $DRIVER" | sha256sum -c --quiet ||
  { rm -f "$DRIVER"; echo "checksum mismatch: $DRIVER (deleted from the cache)" >&2; exit 1; }

bash "$(dirname "$0")/../scripts/build_panvk_driver.sh" "$cache/PanVKDriver.apk" "$cache/panvk-driver.keystore" "$DRIVER"
app_dir="$SYSTEM/app/PanVKDriver"
sudo install -d -m 755 -o root -g root "$app_dir"
sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$app_dir"
install_system_file "$cache/PanVKDriver.apk" "$app_dir/PanVKDriver.apk"

sudo cat "$PROP" >"$cache/build.prop"
sed -i -e '/^ro\.gfx\.driver\.0=/d' "$cache/build.prop"
echo "ro.gfx.driver.0=$PACKAGE" >>"$cache/build.prop"
write_through "$cache/build.prop" "$PROP"
rm -f "$cache/build.prop"
echo "PanVK (panvk-kbase-android 26.3.1) is the updatable Vulkan driver ($PACKAGE)"
