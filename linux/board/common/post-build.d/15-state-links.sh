#!/bin/sh
# 15-state-links.sh TARGET_DIR: state a daemon keeps across boots, as links
# from the read-only root to the data partition.
# SPDX-License-Identifier: MIT
#
#   /var/lib/bluetooth -> /storage/.config/thimsy/bluetooth
#                         (bluetoothd's pairings and adapter settings;
#                         D35bluetooth makes the directory, 0700)
#
# Made whether or not BlueZ is in the tier: 40-bt-pack.sh then moves the
# link into the Bluetooth pack with BlueZ, or removes it when the tier has
# no BlueZ (decision OA-15). Re-runnable (Buildroot runs post-build
# again on every "make"): a path that is already the right link is kept;
# an empty directory in its place (BlueZ's install makes one) is replaced;
# anything else there fails the build, since the link would hide it.
set -eu

target=${1:?usage: 15-state-links.sh TARGET_DIR}
[ -d "$target" ] || {
	echo "15-state-links: $target is not a directory" >&2
	exit 1
}

fail() {
	echo "15-state-links: $*" >&2
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

for d in var var/lib; do
	[ ! -L "$target/$d" ] || fail "/$d is a link; /var/lib/bluetooth needs a real /$d"
done

place var/lib/bluetooth /storage/.config/thimsy/bluetooth
