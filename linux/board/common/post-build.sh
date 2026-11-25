#!/bin/sh
# post-build.sh: runs post-build.d/* in order (BR2_ROOTFS_POST_BUILD_SCRIPT).
# SPDX-License-Identifier: MIT
#
# Each part has one owner (IMPLEMENTATION G); this runner only orders them.
# Buildroot calls it as "post-build.sh TARGET_DIR [args...]", with BR2_CONFIG,
# HOST_DIR, TARGET_DIR, BINARIES_DIR, BR2_EXTERNAL_THIMSY_PATH and the rest
# in the environment; every part gets the same arguments and environment.
#
# Parts come from board/common/post-build.d/ and, for each "--family=NAME" in
# the arguments (BR2_ROOTFS_POST_SCRIPT_ARGS), from board/NAME/post-build.d/.
# They run sorted by file name across those directories, so a family's
# 15-x runs between 10-y and 20-z. The first part that fails stops the build.
# post-build.sh and post-image.sh are the same runner with another STAGE.
set -eu

stage=post-build

common=$(cd "$(dirname "$0")" && pwd)
boards=$(dirname "$common")

dirs=$common/$stage.d
for a in "$@"; do
	case $a in
	--family=*)
		fam=${a#--family=}
		case $fam in
		'' | */* | .*)
			echo "$stage: bad family '$fam'" >&2
			exit 2
			;;
		esac
		dirs="$dirs $boards/$fam/$stage.d"
		;;
	esac
done

parts=$(
	for d in $dirs; do
		[ -d "$d" ] || continue
		for f in "$d"/*; do
			[ -f "$f" ] || continue
			printf '%s\t%s\n' "${f##*/}" "$f"
		done
	done | LC_ALL=C sort -t "$(printf '\t')" -k1,1 | cut -f2
)

old_ifs=$IFS
IFS='
'
for f in $parts; do
	IFS=$old_ifs
	if [ ! -x "$f" ]; then
		echo "$stage: $f is not executable" >&2
		exit 1
	fi
	echo ">>> thimsy $stage: ${f#"$boards"/}"
	"$f" "$@" || {
		rc=$?
		echo "$stage: ${f#"$boards"/} failed ($rc)" >&2
		exit "$rc"
	}
done
IFS=$old_ifs
