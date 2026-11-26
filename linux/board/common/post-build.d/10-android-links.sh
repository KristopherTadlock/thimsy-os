#!/bin/sh
# 10-android-links.sh TARGET_DIR: the Android paths Nano names, as links in
# the read-only root (IMPLEMENTATION B.5, "Paths").
# SPDX-License-Identifier: MIT
#
# Nano's path literals work unedited on Linux because the root carries:
#   /data          -> /storage/.config/thimsy/data   (S00base makes the tree:
#                     system/, local/tmp/, data/, user/0, misc/, gammaos_ota/,
#                     media/0)
#   /sdcard        -> /storage/emulated/0
#   /system/etc    -> /usr/share/thimsy/system/etc   (thimsy-data's assets)
#   /system/fonts  -> /usr/share/fonts/thimsy        (thimsy-fonts)
#   /system/bin    -> /usr/libexec/thimsy/android-bin (the curated wrappers;
#                     empty in the alpha)
#   /mnt/media_rw  -> /run/thimsy/media_rw            (thimsy-session makes
#                     <id> -> /storage/<id> there; the root is read-only)
# /system itself is a real directory holding only those links. The targets
# inside the root are made (empty) here, so no link in the image dangles
# when a package is left out. /storage/self/primary is not made here:
# /storage is the data partition's mount point, so S00base makes it there
# on first boot, and anything under the root's /storage would be hidden.
# /mnt/shares and /vendor/bin are not made (B.5: Shares is [firmware], and
# the shell gate refuses /vendor). Nothing is made under /dev/block.
#
# Re-runnable (Buildroot runs post-build again on every "make"). A path that
# is already the right link is kept; an empty directory in its place is
# replaced; anything else in its place (a package that installed files
# under /data or /system/etc) fails the build, since the link would hide it.
set -eu

target=${1:?usage: 10-android-links.sh TARGET_DIR}
[ -d "$target" ] || {
	echo "10-android-links: $target is not a directory" >&2
	exit 1
}

fail() {
	echo "10-android-links: $*" >&2
	exit 1
}

# place LINKPATH TARGET: $target/LINKPATH becomes a link to TARGET.
place() {
	p=$target/$1
	if [ -L "$p" ]; then
		[ "$(readlink "$p")" = "$2" ] && return 0
		rm -f "$p"
	elif [ -d "$p" ]; then
		rmdir "$p" 2> /dev/null || fail "/$1 is a directory with files in it; they would be hidden by the link to $2"
	elif [ -e "$p" ]; then
		fail "/$1 exists and is not a link to $2"
	fi
	mkdir -p "$(dirname "$p")"
	ln -s "$2" "$p"
}

# The targets that live in the root.
for d in usr/share/thimsy/system/etc usr/share/fonts/thimsy usr/libexec/thimsy/android-bin; do
	[ ! -L "$target/$d" ] || fail "/$d must be a directory, not a link"
	mkdir -p "$target/$d"
	chmod 0755 "$target/$d"
done

# /system is a directory of links, never a link itself.
[ ! -L "$target/system" ] || fail "/system must be a directory, not a link"
mkdir -p "$target/system" "$target/mnt" "$target/storage"

place data /storage/.config/thimsy/data
place sdcard /storage/emulated/0
place system/etc /usr/share/thimsy/system/etc
place system/fonts /usr/share/fonts/thimsy
place system/bin /usr/libexec/thimsy/android-bin
place mnt/media_rw /run/thimsy/media_rw

# Nothing of the root may sit under /storage: the data partition covers it.
if [ -n "$(ls -A "$target/storage")" ]; then
	fail "/storage must be empty in the root (the data partition is mounted over it): $(ls -A "$target/storage" | tr '\n' ' ')"
fi
