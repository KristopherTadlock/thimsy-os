#!/bin/sh
# 20-build-props.sh TARGET_DIR: /usr/share/thimsy/build.props, the build's
# ro.* properties ("thimsy-prop boot" reads them; IMPLEMENTATION B.3).
# SPDX-License-Identifier: MIT
#
# Reproducible: the date is SOURCE_DATE_EPOCH's when set (Buildroot sets it
# with BR2_REPRODUCIBLE), and nothing depends on the build host.
#   THIMSY_VERSION       the release version (build.sh passes it), else
#                        "git describe" of the external tree, else 0.0.0-dev
#   THIMSY_BUILD_TARGET  the targets.json name, else the defconfig's name
# ro.thimsy.build.type is "release" when root login is off and there is no
# serial getty (the release fragment, decision I37), else "dev".
set -eu

target=${1:?usage: 20-build-props.sh TARGET_DIR}
config=${BR2_CONFIG:-}
ext=${BR2_EXTERNAL_THIMSY_PATH:-}

# cfg SYMBOL: the value of a Buildroot symbol ("y", a string without quotes),
# or nothing.
cfg() {
	[ -n "$config" ] && [ -r "$config" ] || return 0
	sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$config" | tail -n 1
}

version=${THIMSY_VERSION:-}
commit=
if [ -n "$ext" ] && command -v git > /dev/null 2>&1 &&
	git -C "$ext" rev-parse --git-dir > /dev/null 2>&1; then
	[ -n "$version" ] || version=$(git -C "$ext" describe --tags --always --dirty 2>/dev/null || :)
	commit=$(git -C "$ext" rev-parse --short=12 HEAD 2>/dev/null || :)
fi
[ -n "$version" ] || version=0.0.0-dev

build_target=${THIMSY_BUILD_TARGET:-}
if [ -z "$build_target" ]; then
	build_target=$(cfg BR2_DEFCONFIG)
	build_target=${build_target##*/}
	build_target=${build_target%_defconfig}
fi
[ -n "$build_target" ] || build_target=unknown

if [ "$(cfg BR2_TARGET_ENABLE_ROOT_LOGIN)" = y ] || [ "$(cfg BR2_TARGET_GENERIC_GETTY)" = y ]; then
	build_type=dev
else
	build_type=release
fi

epoch=${SOURCE_DATE_EPOCH:-}
if [ -z "$epoch" ]; then
	epoch=$(date -u +%s)
fi
date_utc=$(date -u -d "@$epoch" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null ||
	date -u -r "$epoch" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "$epoch")

# The value limit is Android's (91 bytes); keep every value short.
clip() {
	printf '%s' "$1" | cut -c1-91
}

out=$target/usr/share/thimsy/build.props
mkdir -p "${out%/*}"
tmp=$out.tmp
{
	echo "# Written by post-build.d/20-build-props.sh; read by thimsy-prop boot."
	echo "ro.thimsy.build.version=$(clip "$version")"
	echo "ro.thimsy.build.commit=$(clip "$commit")"
	echo "ro.thimsy.build.target=$(clip "$build_target")"
	echo "ro.thimsy.build.type=$build_type"
	echo "ro.thimsy.build.date=$date_utc"
	echo "ro.thimsy.build.date.utc=$epoch"
	echo "ro.thimsy.build.arch=$(clip "$(cfg BR2_ARCH)")"
} > "$tmp"
mv "$tmp" "$out"
chmod 0644 "$out"
echo "build.props: $version ($build_type, $build_target)"
