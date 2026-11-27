#!/bin/sh
# 28-info.sh BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]:
# each device's info entry, a small FAT (IMPLEMENTATION E.6; OA-2).
# SPDX-License-Identifier: MIT
#
# Hardware pack trees only. For each device: cards/<id>/info.vfat, the size
# of genimage.cfg's info partition, made by tools/card_image.py info: FAT12
# labelled THIMSY-INFO with README.TXT (board/common/card-info/README.txt,
# with the device's model from device.json, the release and the id). It is
# the one entry Android's vold mounts on a freshly written card, so Android
# does not offer to set up (format) the card before Linux's first boot;
# D60firstboot retypes it Linux reserved once games is ready. The times
# come from SOURCE_DATE_EPOCH and the volume serial is name-based, so a
# rebuild gives the same bytes.
set -eu
CARD_PART=28-info
. "$(dirname "$0")/lib/card.sh"
card_init "$@"
card_need_hw
card_device_list

size=$(card_part_bytes "$CARD_CFG" info)
[ -n "$size" ] || card_die "$CARD_CFG has no info partition (OA-2)"
epoch=$(card_epoch)
release=$(card_release)

for id in $DEVICES; do
	json=$BOARD/devices/$id.json
	model=$(card_json "$json" model)
	out=$CARDS/$id
	mkdir -p "$out"
	rm -f "$out/info.vfat"
	card_say "$id: info.vfat (${size} bytes)"
	$PY "$TOOLS/card_image.py" info --cfg "$CARD_CFG" --release "$release" --device "$id" \
		--model "${model:-$id}" --epoch "$epoch" -o "$out/info.vfat" ||
		card_die "$id: cannot make info.vfat"
	[ "$(card_size "$out/info.vfat")" -eq "$size" ] ||
		card_die "$id: info.vfat is not the info partition's $size bytes"
done
