#!/bin/sh
# 30-genimage.sh BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]:
# one card per device, through genimage (IMPLEMENTATION E.6; decision D13).
# SPDX-License-Identifier: MIT
#
# Hardware pack trees only. board/<family>/genimage.cfg is run once per
# device, as cards/<id>/genimage.cfg: the board's file with the GPT GUIDs
# pinned for this release and device added (tools/card_image.py cfg, OA-9),
# with cards/<id>/ as its --inputpath: the device's own files are there
# already (rootfs.erofs, data.ext4, info.vfat, and boot.img on FIT families);
# every other file the cfg names (idbloader.img, u-boot.itb) is linked in
# from cards/.family/ (the family's steps) or BINARIES_DIR. The image the
# cfg writes is renamed thimsy-<id>.img in BINARIES_DIR, with a .bmap when
# bmaptool is on PATH (guess that the pinned Buildroot's host-bmap-tools
# installs it under that name). A missing input stops the build here, so
# genimage never runs with a partition empty that should not be.
set -eu
CARD_PART=30-genimage
. "$(dirname "$0")/lib/card.sh"
card_init "$@"
card_need_hw
card_device_list

name=$(card_cfg_output "$CARD_CFG")
[ -n "$name" ] || card_die "$CARD_CFG names no image"
inputs=$(card_cfg_images "$CARD_CFG")
release=$(card_release)

for id in $DEVICES; do
	in=$CARDS/$id
	mkdir -p "$in"
	for f in $inputs; do
		case $f in
		*/*) card_die "$CARD_CFG: image \"$f\" must be a file name" ;;
		esac
		[ -f "$in/$f" ] && continue
		if [ -f "$CARDS/.family/$f" ]; then
			card_link "$CARDS/.family/$f" "$in/$f"
		elif [ -f "$BIN/$f" ]; then
			card_link "$BIN/$f" "$in/$f"
		else
			card_die "$id: genimage needs $f; it is in neither $in, $CARDS/.family nor $BIN"
		fi
	done

	cfg=$in/genimage.cfg
	rm -f "$cfg"
	card_say "$id: GPT GUIDs pinned for $release"
	$PY "$TOOLS/card_image.py" cfg "$CARD_CFG" --family "$FAMILY" --release "$release" \
		--device "$id" -o "$cfg" || card_die "$id: cannot pin the GPT GUIDs"
	work=$in/genimage
	rm -rf "$work"
	mkdir -p "$work/root" "$work/tmp" "$work/out"
	card_say "$id: genimage"
	genimage --rootpath "$work/root" --tmppath "$work/tmp" --inputpath "$in" \
		--outputpath "$work/out" --config "$cfg" || card_die "$id: genimage failed"
	[ -f "$work/out/$name" ] || card_die "$id: genimage wrote no $name"
	card=$BIN/thimsy-$id.img
	rm -f "$card" "$card.bmap"
	mv "$work/out/$name" "$card"
	rm -rf "$work"
	if command -v bmaptool > /dev/null 2>&1; then
		bmaptool create -o "$card.bmap" "$card" || card_die "$id: bmaptool failed"
	fi
	card_say "$id: $card"
done
