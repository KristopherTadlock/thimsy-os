#!/bin/sh
# 40-gpt-check.sh BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]:
# every card genimage wrote, checked by tools/gpt_check.py (IMPLEMENTATION
# E.6; decisions I7, I39).
# SPDX-License-Identifier: MIT
#
# Hardware pack trees only. For each device, thimsy-<id>.img is read with
#   gpt_check.py IMAGE --mode image --family FAMILY --layout thimsy
#                [--idbloader-size BYTES]
# The family is passed explicitly (an image's path does not name it), and
# the idbloader's size is the size of the idbloader.img genimage took, so
# the check does not have to guess its end from the last non-zero sector.
# Any problem fails the build: no card with a vold-mounted entry, a broken
# GPT or a misplaced partition leaves this step.
set -eu
CARD_PART=40-gpt-check
. "$(dirname "$0")/lib/card.sh"
card_init "$@"
card_need_hw
card_device_list

uses_idbloader=0
for f in $(card_cfg_images "$CARD_CFG"); do
	if [ "$f" = idbloader.img ]; then uses_idbloader=1; fi
done

failed=
for id in $DEVICES; do
	card=$BIN/thimsy-$id.img
	[ -f "$card" ] || card_die "$id: no card $card"
	if [ "$uses_idbloader" = 1 ]; then
		idb=$CARDS/$id/idbloader.img
		[ -f "$idb" ] || card_die "$id: no $idb to size the idbloader by"
		set -- --idbloader-size "$(card_size "$idb")"
	else
		set --
	fi
	card_say "$id: gpt_check"
	if ! $PY "$TOOLS/gpt_check.py" "$card" --mode image --family "$FAMILY" --layout thimsy "$@"; then
		failed="$failed $id"
	fi
done
[ -z "$failed" ] || card_die "the GPT check failed for:$failed"
