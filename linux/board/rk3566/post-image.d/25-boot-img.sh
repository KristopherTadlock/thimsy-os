#!/bin/sh
# 25-boot-img.sh BINARIES_DIR hw --family=rk3566: each device's boot.img,
# the 512-byte header and the FIT of the zstd kernel and the device's DTB
# (tools/fit.py; IMPLEMENTATION E.6, decisions G2b-5 to G2b-7).
# SPDX-License-Identifier: MIT
#
# For each device: fit.py build --family rk3566 --device ID --dtb NAME.dtb
# with BINARIES_DIR/Image (or Image.zst) and the device's DTB, which
# Buildroot copies to BINARIES_DIR without its directory (the JSON's "dtb"
# is "rockchip/NAME.dtb"; a copy kept with its directory is found too).
# The DTB name is passed on the command line: device.json's schema has
# "id" and a "dtb" with a directory, which fit.py's --device-json does not
# read yet (its TODO). fit.py writes cards/<id>/fit/{boot.its,boot.itb,
# boot.img}; boot.img moves to cards/<id>/, where genimage takes it.
# fit.py refuses a FIT or boot.img that does not fit the family's map.
set -eu
CARD_PART=25-boot-img
. "$(dirname "$0")/../../common/post-image.d/lib/card.sh"
card_init "$@"
card_need_hw
card_device_list

kernel=
for k in "$BIN/Image" "$BIN/Image.zst"; do
	if [ -f "$k" ]; then
		kernel=$k
		break
	fi
done
[ -n "$kernel" ] || card_die "no kernel (Image or Image.zst) in $BIN"

for id in $DEVICES; do
	json=$BOARD/devices/$id.json
	dtb=$(card_json "$json" dtb)
	[ -n "$dtb" ] || card_die "$json names no dtb"
	base=${dtb##*/}
	if [ -f "$BIN/$base" ]; then
		dtb_file=$BIN/$base
	elif [ -f "$BIN/$dtb" ]; then
		dtb_file=$BIN/$dtb
	else
		card_die "$id: no $base in $BIN"
	fi
	out=$CARDS/$id
	rm -rf "$out/fit"
	mkdir -p "$out/fit"
	card_say "$id: fit.py build ($base)"
	$PY "$TOOLS/fit.py" build --family "$FAMILY" --device "$id" --dtb "$base" \
		--kernel "$kernel" --dtb-file "$dtb_file" --out-dir "$out/fit" ||
		card_die "$id: fit.py build failed"
	[ -f "$out/fit/boot.img" ] || card_die "$id: fit.py wrote no boot.img"
	rm -f "$out/boot.img"
	mv "$out/fit/boot.img" "$out/boot.img"
done
