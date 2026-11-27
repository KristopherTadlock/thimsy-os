#!/bin/sh
# 30-permissions.sh TARGET_DIR: modes, mount points and runtime links of the
# read-only root, and the serial getty of dev images.
# SPDX-License-Identifier: MIT
#
# - the init scripts and D50ssh's helpers (ssh-gate, thimsy-ssh-passwd)
#   executable (a checkout that lost its modes still works);
# - mount points: /storage (the data partition, S00base);
# - /etc/localtime -> /storage/.config/thimsy/localtime (Nano rewrites the
#   target; plan 5.2, "Time");
# - /root stays the directory Buildroot's skeleton and system/device_table.txt
#   ("/root d 700") make: on dev builds, and release builds with SSH turned
#   on, S00base bind-mounts /storage/.config/thimsy/home/root over it at boot
#   (0700, owned by root), so SSH keys and shell history survive; release
#   builds with SSH off keep it empty and read-only (D50ssh binds it when
#   SSH is turned on later, through "S00base home"). A link here would
#   point at a path that does not exist at build time, which makedevs (run on
#   the device table under fakeroot) can't chmod and chown;
# - /dev/block/by-name is never part of the root (IMPLEMENTATION B.5);
# - dev images (BR2_TARGET_GENERIC_GETTY=y) get a getty line in inittab,
#   release images none (decision I37). The overlay's inittab replaces the
#   one Buildroot edits, so the line is added here.
# The /data, /system and /sdcard links are 10-android-links.sh's.
set -eu

target=${1:?usage: 30-permissions.sh TARGET_DIR}
config=${BR2_CONFIG:-}

cfg() {
	[ -n "$config" ] && [ -r "$config" ] || return 0
	sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$config" | tail -n 1
}

# Executable scripts.
for f in "$target"/etc/init.d/rcS "$target"/etc/init.d/rcK "$target"/etc/init.d/rcD \
	"$target"/etc/init.d/S[0-9][0-9]* "$target"/etc/init.d/deferred/D[0-9][0-9]* \
	"$target"/usr/share/thimsy/family/*/hook 	"$target"/usr/libexec/thimsy/ssh-gate "$target"/usr/sbin/thimsy-ssh-passwd; do
	[ -f "$f" ] && chmod 0755 "$f"
done
for f in "$target"/usr/share/thimsy/init/lib.sh "$target"/etc/thimsy/init.conf; do
	[ -f "$f" ] && chmod 0644 "$f"
done

# Mount points.
mkdir -p "$target/storage" "$target/run" "$target/tmp"

# Links into the data partition.
ln -sfn /storage/.config/thimsy/localtime "$target/etc/localtime"
# /root: a plain directory (an earlier build of this tree made it a link).
if [ -L "$target/root" ]; then
	rm -f "$target/root"
fi
mkdir -p "$target/root"
chmod 0700 "$target/root"

# Never a by-name directory in the root.
if [ -e "$target/dev/block/by-name" ]; then
	echo "30-permissions: $target/dev/block/by-name must not exist" >&2
	exit 1
fi

# The serial getty.
inittab=$target/etc/inittab
if [ -f "$inittab" ]; then
	sed -i '/# THIMSY_SERIAL$/d' "$inittab"
	if [ "$(cfg BR2_TARGET_GENERIC_GETTY)" = y ]; then
		port=$(cfg BR2_TARGET_GENERIC_GETTY_PORT)
		baud=$(cfg BR2_TARGET_GENERIC_GETTY_BAUDRATE)
		term=$(cfg BR2_TARGET_GENERIC_GETTY_TERM)
		[ -n "$baud" ] && [ "$baud" != keep ] || baud=0
		[ -n "$term" ] || term=vt100
		if [ -n "$port" ]; then
			printf '%s::respawn:/sbin/getty -L %s %s %s # THIMSY_SERIAL\n' \
				"$port" "$port" "$baud" "$term" >> "$inittab"
		fi
	fi
fi
