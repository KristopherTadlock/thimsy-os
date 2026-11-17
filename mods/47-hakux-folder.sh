#!/usr/bin/env bash
# Gives hakuX its Xbox games folder at first boot, so it opens on its library
# rather than its own folder setup. Runs after 40-added-apps.sh.
#
# hakuX has no storage permission: it lists only folders picked in Android's
# folder picker. So setup makes ROMs/<the Xbox system's folder> on internal
# storage, asks system_server to give hakuX that folder as if picked there
# (NanoNetBridge's uri_grant, the command Nano sends for Azahar's folder after
# Move Emulator Files to microSD), then extracts hakuX's settings naming it,
# as 45-armsx2-malisx2.sh does ARMSX2's. After that Nano keeps hakuX's folders
# to Game Settings (NanoMenuHakux.cpp), and gives this one itself if setup
# could not. Only a fresh hakuX gets it: a setup re-run keeps the user's
# folders. Only on the 1.4.4 (Nano) base, whose system_server has the bridge.
#
# Usage: 47-hakux-folder.sh SYSTEM_DIR CACHE_DIR
set -euo pipefail

SYSTEM="$1"
ETC="$SYSTEM/etc"
MODS="$(cd "$(dirname "$0")" && pwd)"
PKG="com.rfandango.haku_x"
PACK="hakux.tar.zst"
work="$(realpath -m "$2")/hakux-folder"
source "$MODS/../scripts/modlib.sh"

if ! is_nano_base "$SYSTEM"; then
  echo "hakuX gets its folder at first boot only on the 1.4.4 base; skipping"
  exit 0
fi
if ! sudo grep -q "^[^#][^|]*|${PKG//./\\.}|" "$ETC/gammaos/apps.list"; then
  echo "hakuX isn't in added-apps.txt; skipping"
  exit 0
fi
# The ROMs folder of the system hakuX plays (nano-systems.txt: "xbox ... player=xbox.com.rfandango.haku_x").
folder="$(awk -v pkg="$PKG" '$1 !~ /^#/ { for (i = 2; i <= NF; i++)
  if ($i ~ /^(player|package)=/ && (substr($i, length($i) - length(pkg) + 1) == pkg)) { print $1; exit } }' \
  "$MODS/nano-systems.txt")"
if [[ ! "$folder" =~ ^[A-Za-z0-9_-]+$ ]]; then
  echo "no system in nano-systems.txt plays hakuX; skipping"
  exit 0
fi
uri="content://com.android.externalstorage.documents/tree/primary%3AROMs%2F$folder"
app="data/data/$PKG"

rm -rf "$work"
mkdir -p "$work"
# The setup lines go in last: when setup.sh already has them, this mod ran on
# the image before.
cat >"$work/hook.sh" <<EOF
# Gives hakuX a folder ("primary" or a card's uuid, then ROMs/<folder>) as if picked in Android's
# folder picker: NanoNetBridge's uri_grant, sent as Nano's sdUriGrant sends it.
hakux_uri_grant() {
    local i n
    for i in \$(seq 1 30); do
        [ "\$(getprop sys.gammaos.nano.net_generation)" -gt 0 ] 2>/dev/null && break
        sleep 1
    done
    setprop sys.gammaos.nano.uri_grant ""
    for n in 1 2; do   # a Wi-Fi command Nano sends meanwhile replaces it unread
        setprop sys.gammaos.nano.net_cmd "\$(date +%s)\$\$\$n|uri_grant|$PKG|\$1|\$2"
        for i in \$(seq 1 50); do
            case "\$(getprop sys.gammaos.nano.uri_grant)" in
                "ok|$PKG") return 0 ;;
                "fail|$PKG") break 2 ;;   # another package's answer: still waiting
            esac
            sleep 0.1
        done
    done
    step "warning: hakuX was not given \$2; Nano gives it later."
    return 1
}

post_hakux() {
    # Only when hakuX has no settings yet: a re-run keeps the user's folders.
    [ -e /$app/shared_prefs/x1box_prefs.xml ] && return 0
    step "Giving hakuX the Xbox games folder."
    mkdir -p /sdcard/ROMs/$folder && hakux_uri_grant primary ROMs/$folder && \\
        extract_archive /system/etc/$PACK && own_app_data $PKG && \\
        restorecon -R /$app/shared_prefs
}
EOF
if setup_has "$SYSTEM" "$work/hook.sh"; then
  rm -rf "$work"
  echo "hakuX already gets its folder; left as it is"
  exit 0
fi

mkdir -p "$work/tree/$app/shared_prefs"
# As hakuX's own setup leaves them (AppPrefs), with the folders Nano manages in thimsy_folders.
cat >"$work/tree/$app/shared_prefs/x1box_prefs.xml" <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <string name="gamesFolders">$uri</string>
    <string name="gamesFolderUri">$uri</string>
    <boolean name="setup_complete" value="true" />
    <string name="thimsy_folders">$uri</string>
</map>
EOF
chmod 771 "$work/tree/$app/shared_prefs"
chmod 660 "$work/tree/$app/shared_prefs/x1box_prefs.xml"
# Only the app's own folder: entries for data/ or data/data/ would reset
# those directories' owner and mode when extracted.
(cd "$work/tree" && tar -cf "$work/pack.tar" --owner=0 --group=0 "$app/shared_prefs")
tar_to_payload "$work/pack.tar" "$work/$PACK"
install_system_file "$work/$PACK" "$ETC/$PACK"
apps_list "$SYSTEM" hook "$PKG" post_hakux
add_setup_functions "$SYSTEM" "$work/hook.sh"
rm -rf "$work"
echo "hakuX will start with ROMs/$folder as its games folder"
