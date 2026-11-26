#!/bin/sh
# 22-uboot.sh BINARIES_DIR hw --family=rk3566: the boot loader files the
# RK3566 card takes (genimage.cfg: idbloader.img at sector 64, u-boot.itb
# in the uboot partition at sector 16384).
# SPDX-License-Identifier: MIT
#
# The same for every device of the family (mainline U-Boot's rgxx3 board
# detects the device), so they go to cards/.family/ once and 30-genimage.sh
# links them into each card's inputs. U-Boot's build leaves either both
# files in BINARIES_DIR, or only u-boot-rockchip.bin (what hw-rk3566's
# BR2_TARGET_UBOOT_FORMAT_CUSTOM_NAME names): the idbloader followed by
# u-boot.itb at CONFIG_SPL_PAD_TO, 0x7f8000 = (16384 - 64) sectors on
# Rockchip, which is cut in two here.
set -eu
CARD_PART=22-uboot
. "$(dirname "$0")/../../common/post-image.d/lib/card.sh"
card_init "$@"
card_need_hw

fam=$CARDS/.family
mkdir -p "$fam"
pad=16320 # sectors of 512 bytes from sector 64 to sector 16384
if [ -f "$BIN/idbloader.img" ] && [ -f "$BIN/u-boot.itb" ]; then
	card_link "$BIN/idbloader.img" "$fam/idbloader.img"
	card_link "$BIN/u-boot.itb" "$fam/u-boot.itb"
elif [ -f "$BIN/u-boot-rockchip.bin" ]; then
	whole=$BIN/u-boot-rockchip.bin
	size=$(card_size "$whole")
	[ "$size" -gt $((pad * 512)) ] ||
		card_die "$whole is $size bytes: too short to hold u-boot.itb after $((pad * 512))"
	rm -f "$fam/idbloader.img" "$fam/u-boot.itb"
	dd if="$whole" of="$fam/idbloader.img" bs=512 count=$pad 2> /dev/null ||
		card_die "cannot cut the idbloader from $whole"
	dd if="$whole" of="$fam/u-boot.itb" bs=512 skip=$pad 2> /dev/null ||
		card_die "cannot cut u-boot.itb from $whole"
	card_say "cut $whole into idbloader.img and u-boot.itb"
else
	card_die "no U-Boot in $BIN: idbloader.img and u-boot.itb, or u-boot-rockchip.bin"
fi

# The idbloader starts with the Rockchip boot header's "RKNS" (warn only;
# gpt_check.py warns too).
magic=$(od -An -c -N4 "$fam/idbloader.img" 2> /dev/null | tr -d ' \n')
[ "$magic" = RKNS ] || card_say "warning: idbloader.img does not start with RKNS"
idb=$(card_size "$fam/idbloader.img")
[ "$idb" -le $((pad * 512)) ] ||
	card_die "idbloader.img is $idb bytes; it would run into the uboot partition"
uboot=$(card_size "$fam/u-boot.itb")
slot=$(card_part_bytes "$CARD_CFG" uboot)
[ -z "$slot" ] || [ "$uboot" -le "$slot" ] ||
	card_die "u-boot.itb is $uboot bytes; the uboot partition holds $slot"
card_say "idbloader.img $idb bytes, u-boot.itb $uboot bytes"
