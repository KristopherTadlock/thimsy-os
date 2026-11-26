#!/bin/sh
# run-qemu.sh: boots a built QEMU aarch64 virt card (IMPLEMENTATION E.2, E.9;
# decision I8). The serial console is this terminal (ttyAMA0).
# SPDX-License-Identifier: MIT
#
# Usage: board/qemu-aarch64-virt/run-qemu.sh [options] [-- QEMU ARGS...]
#   --images DIR    the hardware pack's images (default $THIMSY_QEMU_IMAGES, else
#                   <repo>/work/linux/images/thimsy_qemu_aarch64_hw)
#   --kernel FILE   the kernel (default DIR/Image)
#   --card FILE     the card post-image.d wrote (default DIR/thimsy-qemu-virt.img)
#   --work FILE     the disk QEMU writes: a sparse copy of the card, grown by
#                   --grow MiB so first boot has room for the games partition
#                   (default: the card's name with .run.img)
#   --grow MIB      default 1024
#   --in-place      boot the card itself (no copy, not grown)
#   --qmp PATH      a QMP socket at PATH (unix, server, nowait)
#   --append TEXT   more kernel arguments
#   --smp N         default 4
#   --mem MIB       default 1024
#   --dry-run       print the QEMU command and run nothing
#   -h, --help
# QEMU is $QEMU, else qemu-system-aarch64. The kernel line is E.9's: the root
# is entry 3 by device name (root=/dev/vda3), so this disk boots without
# U-Boot; S00base finds the boot disk from the mounted root (G2e-1). Without
# "quiet", the kernel log (and the init scripts' "thimsy: " lines) reach the
# serial console. KVM is used when /dev/kvm is usable on an aarch64 host,
# TCG (-cpu max) otherwise. -no-reboot makes a reboot end QEMU too.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
images=${THIMSY_QEMU_IMAGES:-$repo/work/linux/images/thimsy_qemu_aarch64_hw}
kernel= card= work= grow=1024 inplace=0 qmp= append= smp=4 mem=1024 dry=0

usage() {
	sed -n '6,27p' "$0" | sed 's/^# \{0,1\}//'
}
die() {
	echo "run-qemu: $*" >&2
	exit 2
}
value() {
	[ $# -ge 2 ] || die "$1 needs a value"
}

while [ $# -gt 0 ]; do
	case $1 in
	--images) value "$@"; images=$2; shift ;;
	--kernel) value "$@"; kernel=$2; shift ;;
	--card) value "$@"; card=$2; shift ;;
	--work) value "$@"; work=$2; shift ;;
	--grow) value "$@"; grow=$2; shift ;;
	--in-place) inplace=1 ;;
	--qmp) value "$@"; qmp=$2; shift ;;
	--append) value "$@"; append=$2; shift ;;
	--smp) value "$@"; smp=$2; shift ;;
	--mem) value "$@"; mem=$2; shift ;;
	--dry-run) dry=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	--)
		shift
		break
		;;
	*) die "unknown argument $1 (see --help)" ;;
	esac
	shift
done

for n in "$grow" "$smp" "$mem"; do
	case $n in
	'' | *[!0-9]*) die "not a number: '$n'" ;;
	esac
done
kernel=${kernel:-$images/Image}
card=${card:-$images/thimsy-qemu-virt.img}
if [ "$inplace" = 1 ]; then
	work=$card
else
	work=${work:-${card%.img}.run.img}
fi
qemu=${QEMU:-qemu-system-aarch64}

if [ "$dry" = 0 ]; then
	[ -f "$kernel" ] || die "no kernel at $kernel (build thimsy_qemu_aarch64_hw first)"
	[ -f "$card" ] || die "no card at $card"
	command -v "$qemu" > /dev/null 2>&1 || die "$qemu is not on PATH"
fi

# gpt: as the FIT's bootargs (tools/fit.py), the backup GPT when the primary is torn.
cmdline="root=/dev/vda3 rootfstype=erofs ro gpt init=/init console=ttyAMA0${append:+ $append}"

accel="-accel tcg -cpu max"
if [ "$(uname -m)" = aarch64 ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
	accel="-accel kvm -cpu host"
fi

set -- -M virt $accel -smp "$smp" -m "$mem" -nographic -no-reboot \
	-kernel "$kernel" -append "$cmdline" \
	-drive "file=$work,if=virtio,format=raw" \
	-device virtio-gpu-pci -device virtio-keyboard-pci \
	-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	${qmp:+-qmp} ${qmp:+"unix:$qmp,server,nowait"} "$@"

if [ "$dry" = 1 ]; then
	printf '%s' "$qemu"
	for a in "$@"; do
		case $a in
		*[!A-Za-z0-9_./:,=+-]*) printf " '%s'" "$a" ;;
		*) printf ' %s' "$a" ;;
		esac
	done
	printf '\n'
	exit 0
fi

if [ "$inplace" = 0 ]; then
	rm -f "$work"
	cp --sparse=always "$card" "$work" 2> /dev/null || cp "$card" "$work"
	bytes=$(wc -c < "$work" | tr -d ' ')
	mib=$(((bytes + 1048575) / 1048576 + grow))
	dd if=/dev/null of="$work" bs=1048576 seek="$mib" 2> /dev/null ||
		die "cannot grow $work to $mib MiB"
fi
exec "$qemu" "$@"
