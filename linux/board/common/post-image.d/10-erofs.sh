#!/bin/sh
# 10-erofs.sh BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]:
# each device's read-only root (IMPLEMENTATION E.1, E.6; decision D25).
# SPDX-License-Identifier: MIT
#
# In a tier tree: checks the root Buildroot made (rootfs.erofs, under
# fakeroot, so users.table and device.table are applied) and that it fits
# the smallest root slot of any family's genimage.cfg. Nothing is written.
#
# In a hardware pack's tree, once per device (lib/card.sh says where each
# part comes from): the tier's rootfs.erofs is unpacked with fsck.erofs, the
# family's GPU pack over it, then this tree's modules and firmware, the
# family's overlay and the device's JSON at /usr/share/thimsy/device.json,
# and mkfs.erofs writes cards/<id>/rootfs.erofs with the tier's EROFS
# options (lz4hc level and physical cluster size from BR2_CONFIG), the
# timestamp of SOURCE_DATE_EPOCH, a name-based UUID and every file owned by
# root (users.table defines no other user). The result must fit the
# family's root partition.
#
# (guess) fsck.erofs of the pinned erofs-utils (1.7 or later) has --extract
# and --overwrite; unpacking as root keeps owners and modes.
set -eu
CARD_PART=10-erofs
. "$(dirname "$0")/lib/card.sh"
card_init "$@"

if [ "$ROLE" != hw ]; then
	root=$BIN/rootfs.erofs
	if [ "$ROLE" = none ]; then
		card_say "no tier or hardware pack named; skipped"
		exit 0
	fi
	card_is_erofs "$root" || card_die "$root is missing or not an EROFS image"
	size=$(card_size "$root")
	smallest=
	for cfg in "$EXT"/board/*/genimage.cfg; do
		[ -f "$cfg" ] || continue
		slot=$(card_part_bytes "$cfg" root)
		[ -n "$slot" ] || continue
		if [ -z "$smallest" ] || [ "$slot" -lt "$smallest" ]; then
			smallest=$slot
		fi
	done
	if [ -n "$smallest" ] && [ "$size" -gt "$smallest" ]; then
		card_die "rootfs.erofs is $size bytes; the smallest root slot is $smallest"
	fi
	card_say "tier ${TIER_NAME:-?}: rootfs.erofs, $size bytes (root slot ${smallest:-unknown})"
	exit 0
fi

card_device_list
card_tier_images
tier_root=$TIER_IMAGES/rootfs.erofs
card_is_erofs "$tier_root" || card_die "$tier_root is missing or not an EROFS image"
slot=$(card_part_bytes "$CARD_CFG" root)
[ -n "$slot" ] || card_die "$CARD_CFG has no root partition size"

level=$(card_cfg BR2_TARGET_ROOTFS_EROFS_LZ4HC_LEVEL 12)
pcluster=$(card_cfg BR2_TARGET_ROOTFS_EROFS_PCLUSTERSIZE 16384)
epoch=$(card_epoch)

stage=$CARDS/.root-stage
rm -rf "$stage"
mkdir -p "$stage"

# The tier root, then the family's GPU pack over it.
card_say "unpacking $tier_root"
fsck.erofs --extract="$stage" --overwrite "$tier_root" ||
	card_die "fsck.erofs could not unpack $tier_root"
if [ -n "${CARD_GPU_PACK:-}" ]; then
	pack=$TIER_IMAGES/gpu-$CARD_GPU_PACK.erofs
	if [ -f "$pack" ]; then
		card_is_erofs "$pack" || card_die "$pack is not an EROFS image"
		card_say "GPU pack $CARD_GPU_PACK"
		fsck.erofs --extract="$stage" --overwrite "$pack" ||
			card_die "fsck.erofs could not unpack $pack"
	else
		# thimsy-gpu-pack is not written yet: the tier root keeps Mesa.
		card_say "no gpu-$CARD_GPU_PACK.erofs in the tier; the tier root's own Mesa is used"
	fi
fi

# The hardware pack: modules and firmware (merged /usr: usr/lib is the real
# directory; a lib that is a directory is copied too).
target=${TARGET_DIR:-}
if [ -n "$target" ]; then
	for d in usr/lib/modules usr/lib/firmware lib/modules lib/firmware; do
		[ -d "$target/$d" ] && [ ! -L "$target/$d" ] || continue
		case $d in
		lib/*) [ ! -L "$target/lib" ] || continue ;;
		esac
		card_say "hardware pack: /$d"
		mkdir -p "$stage/$d"
		cp -a "$target/$d/." "$stage/$d/"
	done
else
	card_say "TARGET_DIR is not set: no modules or firmware"
fi

# The family's overlay (its rcS hook).
overlay=$BOARD/common/rootfs-overlay
if [ -d "$overlay" ]; then
	card_say "family overlay board/$FAMILY/common/rootfs-overlay"
	cp -a "$overlay/." "$stage/"
	for h in "$stage"/usr/share/thimsy/family/*/hook; do
		if [ -f "$h" ]; then chmod 0755 "$h"; fi
	done
fi
mkdir -p "$stage/usr/share/thimsy"

for id in $DEVICES; do
	json=$BOARD/devices/$id.json
	[ "$(card_json "$json" family)" = "$FAMILY" ] || card_die "$json is not for family $FAMILY"
	out=$CARDS/$id
	mkdir -p "$out"
	cp "$json" "$stage/usr/share/thimsy/device.json"
	chmod 0644 "$stage/usr/share/thimsy/device.json"
	rm -f "$out/rootfs.erofs"
	uuid=$(card_uuid "$id:root:$epoch")
	card_say "$id: mkfs.erofs"
	mkfs.erofs -zlz4hc,"$level" -C"$pcluster" -T"$epoch" -U"$uuid" --all-root \
		"$out/rootfs.erofs" "$stage" || card_die "$id: mkfs.erofs failed"
	size=$(card_size "$out/rootfs.erofs")
	[ "$size" -le "$slot" ] ||
		card_die "$id: rootfs.erofs is $size bytes; the root partition holds $slot"
	card_say "$id: rootfs.erofs, $size bytes (slot $slot)"
done
rm -rf "$stage"
