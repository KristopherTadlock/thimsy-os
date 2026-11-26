#!/bin/sh
# 20-data.sh BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]:
# each device's data partition, ext4 (IMPLEMENTATION E.6; decision I7).
# SPDX-License-Identifier: MIT
#
# Hardware pack trees only. For each device: cards/<id>/data.ext4, the size
# of genimage.cfg's data partition, made with "mkfs.ext4 -d" from a small
# tree: thimsy/device.json (beside install.json, which the installer adds;
# plan 8.4). S00base makes the rest of /storage on first boot. Timestamps
# come from SOURCE_DATE_EPOCH (E2FSPROGS_FAKE_TIME) and the UUID is
# name-based, so a rebuild gives the same bytes. No blocks are reserved:
# only root writes here.
set -eu
CARD_PART=20-data
. "$(dirname "$0")/lib/card.sh"
card_init "$@"
card_need_hw
card_device_list

size=$(card_part_bytes "$CARD_CFG" data)
[ -n "$size" ] || card_die "$CARD_CFG has no data partition size"
kib=$((size / 1024))
epoch=$(card_epoch)

for id in $DEVICES; do
	json=$BOARD/devices/$id.json
	out=$CARDS/$id
	stage=$out/data-stage
	rm -rf "$stage"
	mkdir -p "$stage/thimsy"
	cp "$json" "$stage/thimsy/device.json"
	chmod 0644 "$stage/thimsy/device.json"
	rm -f "$out/data.ext4"
	uuid=$(card_uuid "$id:data:$epoch")
	card_say "$id: mkfs.ext4 (${kib} KiB)"
	E2FSPROGS_FAKE_TIME=$epoch mkfs.ext4 -F -q -L thimsy-data -U "$uuid" -m 0 \
		-E root_owner=0:0 -d "$stage" "$out/data.ext4" "${kib}k" ||
		card_die "$id: mkfs.ext4 failed"
	[ "$(card_size "$out/data.ext4")" -le "$size" ] ||
		card_die "$id: data.ext4 is larger than the data partition ($size bytes)"
	rm -rf "$stage"
done
