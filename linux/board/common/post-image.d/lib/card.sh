# card.sh: shared by board/*/post-image.d/* (sourced, never run).
# SPDX-License-Identifier: MIT
#
# The per-device cards (IMPLEMENTATION E.1, E.6; decisions D13, D25, I39) are
# put together in a hardware pack's tree, after its kernel and U-Boot are
# built, from:
#   - the tier root: rootfs.erofs from the tier tree named by the target's
#     "sdk_from" (configs/targets.json), built by Buildroot under fakeroot;
#   - the family's GPU pack, gpu-<driver>.erofs from the same tier tree, when
#     thimsy-gpu-pack has made one (else the tier root's own Mesa is used);
#   - the hardware pack: this tree's usr/lib/modules and usr/lib/firmware,
#     and board/<family>/common/rootfs-overlay (the family hook);
#   - the device: board/<family>/devices/<id>.json, at
#     /usr/share/thimsy/device.json in the root (thimsy-prop's default path)
#     and at thimsy/device.json on the data partition (beside install.json).
# Parts, in order: 10-erofs.sh (each device's root), 20-data.sh (each data
# partition), the family's own steps (rk3566: 22-uboot.sh, 25-boot-img.sh),
# 30-genimage.sh (each card), 40-gpt-check.sh (each card checked).
#
# Arguments (post-image.sh passes BINARIES_DIR, then BR2_ROOTFS_POST_SCRIPT_ARGS):
#   BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY] [--tier-dir=DIR]
# "tier" trees make no card: 10-erofs.sh only checks their root. "hw FAMILY"
# and "--family=FAMILY" both name the family; post-image.sh runs
# board/FAMILY/post-image.d/ only for "--family=FAMILY".
#
# Environment (Buildroot sets the first ones; the THIMSY_ ones are seams for
# build.sh and the tests):
#   BR2_EXTERNAL_THIMSY_PATH  linux/ (default: three levels above this file)
#   BR2_CONFIG                .config (the EROFS options are read from it)
#   TARGET_DIR                this tree's target (the hardware pack's files)
#   BASE_DIR                  this tree's output directory, output-<target>
#   SOURCE_DATE_EPOCH         timestamps and the filesystem UUIDs (else 0)
#   THIMSY_TIER_DIR           the tier tree's output directory (default: the
#                             sibling output-<sdk_from> of BASE_DIR)
#   THIMSY_BUILD_TARGET       this tree's targets.json name (default: from BASE_DIR)
#   THIMSY_DEVICES            device ids to build (default: every devices/*.json)
#   THIMSY_PYTHON             the Python that runs tools/*.py (default python3)
#   THIMSY_TOOLS_DIR          where fit.py and gpt_check.py are (default linux/tools)
# Tools come from PATH, which Buildroot starts with HOST_DIR/bin: fsck.erofs,
# mkfs.erofs, mkfs.ext4, genimage, and bmaptool when the host has it.
#
# Outputs, under BINARIES_DIR:
#   cards/<id>/rootfs.erofs, data.ext4, boot.img (FIT families) and the
#   genimage inputs; thimsy-<id>.img (the card) and thimsy-<id>.img.bmap.

card_die() {
	printf '%s: %s\n' "${CARD_PART:-card}" "$*" >&2
	exit 1
}

card_say() {
	printf '%s: %s\n' "${CARD_PART:-card}" "$*"
}

# card_init "$@": ROLE (tier, hw or none), FAMILY, BIN, EXT, BOARD, TOOLS,
# PY, CARDS. Dies on a bad argument.
card_init() {
	BIN=${1:-}
	[ -n "$BIN" ] || card_die "usage: $0 BINARIES_DIR [tier NAME | hw FAMILY] [--family=FAMILY]"
	shift
	ROLE=none
	FAMILY=
	TIER_NAME=
	CARD_TIER_DIR=${THIMSY_TIER_DIR:-}
	_want=
	for _a in "$@"; do
		case $_a in
		--family=*) FAMILY=${_a#--family=} ;;
		--tier-dir=*) CARD_TIER_DIR=${_a#--tier-dir=} ;;
		tier | hw)
			ROLE=$_a
			_want=$_a
			continue
			;;
		-*) ;;
		*)
			if [ "$_want" = hw ] && [ -z "$FAMILY" ]; then
				FAMILY=$_a
			elif [ "$_want" = tier ]; then
				TIER_NAME=$_a
			fi
			;;
		esac
		_want=
	done
	if [ -n "$FAMILY" ] && [ "$ROLE" = none ]; then
		ROLE=hw
	fi
	case $FAMILY in
	*/* | .*) card_die "bad family '$FAMILY'" ;;
	esac
	_here=$(cd "$(dirname "$0")" && pwd)
	EXT=${BR2_EXTERNAL_THIMSY_PATH:-}
	if [ -z "$EXT" ]; then
		# board/<x>/post-image.d/NAME: linux/ is three levels up.
		EXT=$(cd "$_here/../../.." && pwd)
	fi
	TOOLS=${THIMSY_TOOLS_DIR:-$EXT/tools}
	PY=${THIMSY_PYTHON:-python3}
	CARDS=$BIN/cards
	BOARD=
	CARD_GPU_PACK=
	CARD_CFG=
	if [ "$ROLE" = hw ]; then
		[ -n "$FAMILY" ] || card_die "a hardware pack names its family (hw FAMILY or --family=FAMILY)"
		BOARD=$EXT/board/$FAMILY
		[ -d "$BOARD" ] || card_die "no board directory $BOARD"
		CARD_CFG=$BOARD/genimage.cfg
		[ -f "$CARD_CFG" ] || card_die "no $CARD_CFG"
		if [ -f "$BOARD/card.conf" ]; then
			. "$BOARD/card.conf"
		fi
	fi
}

# card_need_hw: exit 0 (skip the part) unless this is a hardware pack's tree.
card_need_hw() {
	if [ "$ROLE" != hw ]; then
		card_say "no card in a $ROLE tree; skipped"
		exit 0
	fi
}

# card_device_list: the device ids into DEVICES (space-separated, sorted).
# THIMSY_DEVICES limits them; an id with no JSON, or no device, is an error.
card_device_list() {
	DEVICES=
	if [ -n "${THIMSY_DEVICES:-}" ]; then
		for _d in $THIMSY_DEVICES; do
			case $_d in
			*[!a-z0-9-]* | -*) card_die "bad device id '$_d'" ;;
			esac
			[ -f "$BOARD/devices/$_d.json" ] || card_die "no $BOARD/devices/$_d.json"
		done
		DEVICES=$(printf '%s\n' $THIMSY_DEVICES | LC_ALL=C sort -u | tr '\n' ' ')
	else
		for _f in "$BOARD"/devices/*.json; do
			[ -f "$_f" ] || continue
			_d=${_f##*/}
			DEVICES="$DEVICES ${_d%.json}"
		done
		DEVICES=$(printf '%s\n' $DEVICES | LC_ALL=C sort | tr '\n' ' ')
	fi
	[ -n "$(printf '%s' "$DEVICES" | tr -d ' ')" ] ||
		card_die "no device for family $FAMILY (board/$FAMILY/devices/*.json, THIMSY_DEVICES)"
}

# card_json FILE KEY: a top-level string value of a JSON file (empty if absent
# or null). The file's "id" must equal its name: a copied JSON is refused.
card_json() {
	$PY -c '
import json, os, sys
path, key = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8") as f:
    d = json.load(f)
name = os.path.basename(path)[:-5]
if d.get("id") != name:
    sys.exit("%s: id %r is not the file name %r" % (path, d.get("id"), name))
v = d.get(key)
if v is None:
    v = ""
if not isinstance(v, str):
    sys.exit("%s: %s is not a string" % (path, key))
print(v)
' "$1" "$2" || card_die "cannot read $2 from $1"
}

# card_part_bytes CFG NAME: the size of partition NAME in a genimage.cfg, in
# bytes (sizes are explicit there; test_layout.py refuses implicit ones).
card_part_bytes() {
	awk -v want="$2" '
		/^[ \t]*partition[ \t]+/ { name = $2; sub(/[ \t]*\{.*/, "", name); inpart = 1; next }
		inpart && /^[ \t]*\}/ { inpart = 0; name = ""; next }
		inpart && name == want && /^[ \t]*size[ \t]*=/ {
			v = $0; sub(/^[^=]*=[ \t]*/, "", v); gsub(/[" \t;]/, "", v)
			m = 1
			if (v ~ /[kK]$/) m = 1024
			else if (v ~ /M$/) m = 1048576
			else if (v ~ /G$/) m = 1073741824
			if (m != 1) v = substr(v, 1, length(v) - 1)
			printf "%.0f\n", v * m
			exit
		}
	' "$1"
}

# card_cfg_images CFG: the files a genimage.cfg's partitions take, one per line.
card_cfg_images() {
	sed -n 's/^[[:space:]]*image[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$1"
}

# card_cfg_output CFG: the name of the image a genimage.cfg writes.
card_cfg_output() {
	sed -n 's/^image[[:space:]]\{1,\}\([^[:space:]{]*\)[[:space:]]*{.*/\1/p' "$1" | head -n 1
}

# card_size FILE: its size in bytes.
card_size() {
	wc -c < "$1" | tr -d ' '
}

# card_epoch: SOURCE_DATE_EPOCH, else 0 (reproducible either way).
card_epoch() {
	printf '%s\n' "${SOURCE_DATE_EPOCH:-0}"
}

# card_uuid TEXT: a name-based UUID (uuid5), so a rebuild gives the same one.
card_uuid() {
	$PY -c 'import sys, uuid; print(uuid.uuid5(uuid.NAMESPACE_URL, "thimsy:" + sys.argv[1]))' "$1" ||
		card_die "cannot make a UUID"
}

# card_cfg SYMBOL DEFAULT: a Buildroot symbol's value from BR2_CONFIG.
card_cfg() {
	_v=
	if [ -n "${BR2_CONFIG:-}" ] && [ -r "$BR2_CONFIG" ]; then
		_v=$(sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$BR2_CONFIG" | tail -n 1)
	fi
	printf '%s\n' "${_v:-$2}"
}

# card_tier_images: the tier tree's images directory, into TIER_IMAGES.
card_tier_images() {
	if [ -z "$CARD_TIER_DIR" ]; then
		[ -n "${BASE_DIR:-}" ] || card_die "THIMSY_TIER_DIR is not set and BASE_DIR is unknown"
		_target=${THIMSY_BUILD_TARGET:-}
		if [ -z "$_target" ]; then
			_target=${BASE_DIR##*/}
			_target=${_target#output-}
		fi
		_from=$($PY -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    t = json.load(f)["targets"].get(sys.argv[2])
if not t or not t.get("sdk_from"):
    sys.exit("targets.json: %s has no sdk_from" % sys.argv[2])
print(t["sdk_from"])
' "$EXT/configs/targets.json" "$_target") || card_die "cannot find the tier of $_target"
		CARD_TIER_DIR=$(dirname "$BASE_DIR")/output-$_from
	fi
	TIER_IMAGES=$CARD_TIER_DIR/images
	[ -d "$TIER_IMAGES" ] || card_die "no tier images at $TIER_IMAGES: build the tier first"
}

# card_is_erofs FILE: true when FILE starts like an EROFS image (the
# superblock's magic 0xE0F5E1E2 at byte 1024).
card_is_erofs() {
	[ -f "$1" ] || return 1
	[ "$(od -An -tx1 -j1024 -N4 "$1" 2>/dev/null | tr -d ' \n')" = e2e1f5e0 ]
}

# card_link SRC DST: DST as a hard link to SRC, or a copy where links fail.
card_link() {
	rm -f "$2"
	ln "$1" "$2" 2>/dev/null || cp "$1" "$2" || card_die "cannot copy $1 to $2"
}
