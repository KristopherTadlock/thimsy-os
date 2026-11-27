#!/bin/sh
# 40-bt-pack.sh TARGET_DIR [tier NAME | hw FAMILY ...]: BlueZ and D-Bus out of
# a tier's root, into bluetooth.erofs (decision OA-15).
# SPDX-License-Identifier: MIT
#
# Only devices whose device.json says "bluetooth": true get BlueZ, D-Bus and
# D35bluetooth. A tier root serves every device of its cards, so in a tier
# tree tools/bt_pack.py takes those files out of TARGET_DIR (Buildroot's
# packages-file-list.txt says which are BlueZ's and D-Bus's) and writes
# BINARIES_DIR/bluetooth.erofs; post-image.d/10-erofs.sh unpacks it into the
# roots of the devices with Bluetooth only. A tier built without BlueZ
# (bluetooth.fragment not listed) keeps neither D35bluetooth nor the
# /var/lib/bluetooth link. Last of the post-build parts, so the overlay's
# D35bluetooth and 15-state-links.sh's link are there to take. Hardware
# packs build no userland: nothing to do there.
#
# Environment: Buildroot's BR2_CONFIG, BUILD_DIR (else TARGET_DIR's sibling
# build/; guess that Buildroot passes it to post-build scripts),
# BINARIES_DIR, SOURCE_DATE_EPOCH; THIMSY_PYTHON (default python3) and
# THIMSY_TOOLS_DIR (default linux/tools), as card assembly uses them.
# mkfs.erofs comes from PATH, which Buildroot starts with HOST_DIR/bin, or
# from THIMSY_MKFS_EROFS (the tests' seam).
set -eu

target=${1:?usage: 40-bt-pack.sh TARGET_DIR [tier NAME]}
[ "${2:-}" = tier ] || exit 0
config=${BR2_CONFIG:-}
here=$(cd "$(dirname "$0")" && pwd)
ext=${BR2_EXTERNAL_THIMSY_PATH:-$(cd "$here/../../.." && pwd)}
tools=${THIMSY_TOOLS_DIR:-$ext/tools}
py=${THIMSY_PYTHON:-python3}
out=${BINARIES_DIR:-$(dirname "$target")/images}

cfg() {
	[ -n "$config" ] && [ -r "$config" ] || return 0
	sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$config" | tail -n 1
}

if [ "$(cfg BR2_PACKAGE_BLUEZ5_UTILS)" != y ]; then
	exec $py "$tools/bt_pack.py" --target "$target" --none --out "$out"
fi

build=${BUILD_DIR:-$(dirname "$target")/build}
list=$build/packages-file-list.txt
[ -f "$list" ] || {
	echo "40-bt-pack: no $list: cannot tell BlueZ's files" >&2
	exit 1
}
mkfs=${THIMSY_MKFS_EROFS:-}
[ -n "$mkfs" ] || mkfs=$(command -v mkfs.erofs) || {
	echo "40-bt-pack: no mkfs.erofs on PATH" >&2
	exit 1
}
set -- --target "$target" --work "$build/thimsy-bt-pack-work" --file-list "$list" \
	--out "$out" --mkfs "$mkfs"
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
	set -- "$@" --epoch "$SOURCE_DATE_EPOCH"
fi
exec $py "$tools/bt_pack.py" "$@"
