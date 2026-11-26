# Thimsy init library: sourced by rcS, rcK, rcD, the S* and D* scripts.
# SPDX-License-Identifier: MIT
#
# POSIX sh for BusyBox ash. On the boot path every fork costs milliseconds,
# so this file uses shell builtins only (read, printf, case, test) and is
# sourced, never executed. Nothing here may start a program: check_rcs.py
# treats it as part of the synchronous rcS path.
#
# Test seams (docs/init.md):
#   THIMSY_ROOT     prefix for every path the scripts touch (empty on a device)
#   THIMSY_TOOLDIR  a directory whose executables replace the named tools
# A device sets neither.

R=${THIMSY_ROOT:-}

# thimsy_tool VAR name: VAR names the program to run for "name".
thimsy_tool() {
	if [ -n "${THIMSY_TOOLDIR:-}" ] && [ -x "$THIMSY_TOOLDIR/$2" ]; then
		eval "$1=\"\$THIMSY_TOOLDIR/$2\""
	else
		eval "$1=$2"
	fi
}

thimsy_tool MOUNT mount
thimsy_tool UMOUNT umount
thimsy_tool MODPROBE modprobe
thimsy_tool PROP thimsy-prop
thimsy_tool PADD thimsy-padd
thimsy_tool FSCK_EXT4 e2fsck
thimsy_tool SFDISK sfdisk
thimsy_tool MKFS_EXFAT mkfs.exfat
thimsy_tool DD dd
thimsy_tool CHOWN chown
thimsy_tool SYNC sync
thimsy_tool ALSACTL alsactl
thimsy_tool AMIXER amixer
thimsy_tool IP ip
thimsy_tool HOSTNAME hostname
thimsy_tool WPA wpa_supplicant
thimsy_tool UDHCPC udhcpc
thimsy_tool NTPD ntpd
thimsy_tool DROPBEAR dropbear
thimsy_tool DROPBEARKEY dropbearkey
thimsy_tool DBUS_DAEMON dbus-daemon
thimsy_tool DBUS_UUIDGEN dbus-uuidgen
thimsy_tool BLUETOOTHD bluetoothd
thimsy_tool FLOCK flock

# Tunables; /etc/thimsy/init.conf may override them.
THIMSY_DATA_MOUNT_OPTS=noatime,nosuid,nodev
THIMSY_GAMES_NAME=games
THIMSY_GAMES_LABEL=THIMSY
THIMSY_GAMES_MIN_MIB=64
THIMSY_NTP_SERVERS="0.pool.ntp.org 1.pool.ntp.org 2.pool.ntp.org"
THIMSY_SOUND_WAIT_S=5
THIMSY_BT_WAIT_S=5
if [ -r "$R/etc/thimsy/init.conf" ]; then
	. "$R/etc/thimsy/init.conf"
fi

THIMSY_CONFIG=$R/storage/.config/thimsy

# --- logging -------------------------------------------------------------

# Sets _up to the seconds since boot ("0" when /proc is not there yet).
thimsy_uptime() {
	_up=0
	if [ -r "$R/proc/uptime" ]; then
		read -r _up _idle < "$R/proc/uptime" || _up=0
	fi
}

# thimsy_log TAG MESSAGE: one line in /run/thimsy/log/boot.log.
thimsy_log() {
	thimsy_uptime
	if [ -d "$R/run/thimsy/log" ]; then
		printf '%s %s: %s\n' "$_up" "$1" "$2" >> "$R/run/thimsy/log/boot.log"
	fi
}

# thimsy_kmsg MESSAGE: one line in the kernel log (and the serial console).
thimsy_kmsg() {
	{ printf 'thimsy: %s\n' "$1"; } 2>/dev/null >> "$R/dev/kmsg" || :
}

# thimsy_warn TAG MESSAGE: both.
thimsy_warn() {
	thimsy_log "$1" "$2"
	thimsy_kmsg "$1: $2"
}

# thimsy_mark NAME: the boot clock's breadcrumb, /run/thimsy/boot/NAME.
thimsy_mark() {
	thimsy_uptime
	if [ -d "$R/run/thimsy/boot" ]; then
		printf '%s\n' "$_up" > "$R/run/thimsy/boot/$1"
	fi
}

# --- small helpers ---------------------------------------------------------

# thimsy_lower_hex STRING: LOWER_OUT is STRING with A-F lowered (GUIDs).
thimsy_lower_hex() {
	_in=$1
	LOWER_OUT=
	while [ -n "$_in" ]; do
		_c=${_in%"${_in#?}"}
		_in=${_in#?}
		case $_c in
		A) _c=a ;; B) _c=b ;; C) _c=c ;; D) _c=d ;; E) _c=e ;; F) _c=f ;;
		esac
		LOWER_OUT=$LOWER_OUT$_c
	done
}

# thimsy_partname DISK N: PARTNAME_OUT is the kernel's name of entry N.
thimsy_partname() {
	case $1 in
	*[0-9]) PARTNAME_OUT=${1}p$2 ;;
	*) PARTNAME_OUT=$1$2 ;;
	esac
}

# thimsy_persist KEY DEFAULT: PERSIST_OUT is KEY from persist.props.
# The file is libthimsy's (key=value lines, B.3); values with escapes are
# not needed by the init scripts.
thimsy_persist() {
	PERSIST_OUT=$2
	[ -r "$THIMSY_CONFIG/persist.props" ] || return 0
	while IFS='=' read -r _k _v; do
		if [ "$_k" = "$1" ]; then
			PERSIST_OUT=$_v
		fi
	done < "$THIMSY_CONFIG/persist.props"
}

# thimsy_ro KEY DEFAULT: RO_OUT is the read-only property KEY, as S01props'
# "thimsy-prop boot" wrote it (/run/thimsy/props/ro/KEY: device.json's
# scalars as ro.thimsy.device.*), else DEFAULT.
thimsy_ro() {
	RO_OUT=
	if [ -r "$R/run/thimsy/props/ro/$1" ]; then
		read -r RO_OUT < "$R/run/thimsy/props/ro/$1" || :
	fi
	[ -n "$RO_OUT" ] || RO_OUT=$2
}

# thimsy_build_prop KEY DEFAULT: BUILD_PROP_OUT from /usr/share/thimsy/build.props.
thimsy_build_prop() {
	BUILD_PROP_OUT=$2
	[ -r "$R/usr/share/thimsy/build.props" ] || return 0
	while IFS='=' read -r _k _v; do
		if [ "$_k" = "$1" ]; then
			BUILD_PROP_OUT=$_v
		fi
	done < "$R/usr/share/thimsy/build.props"
}

# --- block devices ---------------------------------------------------------

# thimsy_uevent FILE: UE_* from a /sys/class/block/<name>/uevent file.
thimsy_uevent() {
	UE_DEVNAME= UE_DEVTYPE= UE_PARTN= UE_PARTNAME= UE_PARTUUID=
	[ -r "$1" ] || return 1
	while IFS='=' read -r _k _v; do
		case $_k in
		DEVNAME) UE_DEVNAME=$_v ;;
		DEVTYPE) UE_DEVTYPE=$_v ;;
		PARTN) UE_PARTN=$_v ;;
		PARTNAME) UE_PARTNAME=$_v ;;
		PARTUUID) UE_PARTUUID=$_v ;;
		esac
	done < "$1"
	return 0
}

# thimsy_is_emmc DISK: true for internal storage (device/type is MMC).
# SD cards say SD; anything else (virtio, USB) has no such file.
thimsy_is_emmc() {
	_t=
	if [ -r "$R/sys/class/block/$1/device/type" ]; then
		read -r _t < "$R/sys/class/block/$1/device/type" || :
	fi
	[ "$_t" = MMC ]
}

# thimsy_resolve_boot_disk: BOOT_DISK and ROOT_PART for the disk the root
# was mounted from, and BOOT_HOW for how it was found.
#
# The mounted root's major:minor (/proc/self/mountinfo) is what the kernel
# actually used, so it wins: two cards written from the same image carry the
# same PARTUUIDs. root=PARTUUID= and root=/dev/NAME from /proc/cmdline are
# used when the mounted root is not a block partition this can see.
thimsy_resolve_boot_disk() {
	BOOT_DISK= ROOT_PART= BOOT_HOW=
	_mm=
	if [ -r "$R/proc/self/mountinfo" ]; then
		while read -r _id _parent _majmin _mroot _mpoint _rest; do
			if [ "$_mpoint" = / ]; then
				_mm=$_majmin
			fi
		done < "$R/proc/self/mountinfo"
	fi
	if [ -n "$_mm" ]; then
		for _dv in "$R"/sys/class/block/*/dev; do
			[ -r "$_dv" ] || continue
			_x=
			read -r _x < "$_dv" || :
			if [ "$_x" = "$_mm" ]; then
				_p=${_dv%/dev}
				ROOT_PART=${_p##*/}
				BOOT_HOW=mountinfo
				break
			fi
		done
	fi
	if [ -z "$ROOT_PART" ]; then
		_cmd=
		if [ -r "$R/proc/cmdline" ]; then
			read -r _cmd < "$R/proc/cmdline" || :
		fi
		_root=
		set -f
		for _w in $_cmd; do
			case $_w in
			root=*) _root=${_w#root=} ;;
			esac
		done
		set +f
		case $_root in
		PARTUUID=*)
			_want=${_root#PARTUUID=}
			_want=${_want%%/*}
			thimsy_lower_hex "$_want"
			_want=$LOWER_OUT
			_hits=0
			for _ue in "$R"/sys/class/block/*/uevent; do
				thimsy_uevent "$_ue" || continue
				[ -n "$UE_PARTUUID" ] || continue
				thimsy_lower_hex "$UE_PARTUUID"
				if [ "$LOWER_OUT" = "$_want" ]; then
					_hits=$((_hits + 1))
					_p=${_ue%/uevent}
					ROOT_PART=${_p##*/}
				fi
			done
			if [ "$_hits" -gt 1 ]; then
				thimsy_warn boot-disk "root=PARTUUID=$_want is on $_hits partitions; not guessing"
				ROOT_PART=
			elif [ -n "$ROOT_PART" ]; then
				BOOT_HOW=partuuid
			fi
			;;
		/dev/*)
			_p=${_root#/dev/}
			if [ -e "$R/sys/class/block/$_p" ]; then
				ROOT_PART=$_p
				BOOT_HOW=devname
			fi
			;;
		esac
	fi
	[ -n "$ROOT_PART" ] || return 1
	if [ -e "$R/sys/class/block/$ROOT_PART/partition" ]; then
		for _d in "$R"/sys/class/block/*; do
			if [ -d "$_d/$ROOT_PART" ]; then
				BOOT_DISK=${_d##*/}
				break
			fi
		done
	else
		BOOT_DISK=$ROOT_PART
	fi
	[ -n "$BOOT_DISK" ]
}

# thimsy_find_part DISK NAME: FOUND_PART is DISK's partition whose GPT name
# is NAME. Only DISK is searched.
thimsy_find_part() {
	FOUND_PART=
	for _ue in "$R/sys/class/block/$1/$1"*/uevent; do
		thimsy_uevent "$_ue" || continue
		[ "$UE_PARTNAME" = "$2" ] || continue
		_p=${_ue%/uevent}
		if [ -n "$FOUND_PART" ]; then
			thimsy_warn boot-disk "two partitions named $2 on $1; using $FOUND_PART"
			break
		fi
		FOUND_PART=${_p##*/}
	done
	[ -n "$FOUND_PART" ]
}

# The boot disk as S00base resolved it, for D60firstboot and thimsy-session.
thimsy_save_bootdisk() {
	[ -d "$R/run/thimsy" ] || return 0
	printf 'BOOT_DISK=%s\nROOT_PART=%s\nDATA_PART=%s\nBOOT_HOW=%s\nBOOT_EMMC=%s\n' \
		"$BOOT_DISK" "$ROOT_PART" "${DATA_PART:-}" "$BOOT_HOW" "${BOOT_EMMC:-0}" \
		> "$R/run/thimsy/bootdisk"
}

thimsy_load_bootdisk() {
	BOOT_DISK= ROOT_PART= DATA_PART= BOOT_HOW= BOOT_EMMC=0
	[ -r "$R/run/thimsy/bootdisk" ] || return 1
	while IFS='=' read -r _k _v; do
		case $_k in
		BOOT_DISK) BOOT_DISK=$_v ;;
		ROOT_PART) ROOT_PART=$_v ;;
		DATA_PART) DATA_PART=$_v ;;
		BOOT_HOW) BOOT_HOW=$_v ;;
		BOOT_EMMC) BOOT_EMMC=$_v ;;
		esac
	done < "$R/run/thimsy/bootdisk"
	[ -n "$BOOT_DISK" ]
}

# --- the device family -----------------------------------------------------

# thimsy_family: FAMILY from /run/thimsy/family (S01props writes it), else
# from ro.thimsy.family, else the only family the root carries.
thimsy_family() {
	FAMILY=
	if [ -r "$R/run/thimsy/family" ]; then
		read -r FAMILY < "$R/run/thimsy/family" || :
		[ -n "$FAMILY" ] && return 0
	fi
	if [ -r "$R/run/thimsy/props/ro/ro.thimsy.family" ]; then
		read -r FAMILY < "$R/run/thimsy/props/ro/ro.thimsy.family" || :
		[ -n "$FAMILY" ] && return 0
	fi
	_n=0
	_only=
	for _d in "$R"/usr/share/thimsy/family/*/; do
		[ -d "$_d" ] || continue
		_n=$((_n + 1))
		_d=${_d%/}
		_only=${_d##*/}
	done
	if [ "$_n" -eq 1 ]; then
		FAMILY=$_only
	fi
	[ -n "$FAMILY" ]
}

# thimsy_family_hook ACTION: source the family's hook with THIMSY_HOOK=ACTION.
# Sourcing saves a fork on the boot path; the hook must not call exit.
thimsy_family_hook() {
	thimsy_family || return 0
	_hook=$R/usr/share/thimsy/family/$FAMILY/hook
	[ -r "$_hook" ] || return 0
	THIMSY_HOOK=$1
	. "$_hook"
}
