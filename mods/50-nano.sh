#!/usr/bin/env bash
# Makes the device boot into the GammaOS Nano home (1.4.4 base only), from the
# first boot, where Nano's own setup wizard runs first-boot setup. Its home
# theme is the ES-DE theme engine with the Colorful (Simplified) theme, its
# aspect ratio picked automatically, its settings screens have the Minima theme's look on a
# Midnight background, its menu sounds are off, and its game systems are the
# ones in nano-systems.txt, with this project's default emulators. The stick
# LEDs follow the screen's colour, the performance mode is Max Performance,
# and Nano's Slide Behaviour settings are hidden (the RG Cube does not slide).
# These are build.prop defaults, so choices made later in Nano, or "Boot
# Android" in the power menu, stick. Runs after 40-added-apps.sh.
#
# The theme is prepared by scripts/colorful_theme.sh, which the Linux build
# runs too, so both ship the same theme (docs/linux/DECISIONS.md OA-1): its
# Gilroy and Akrobat fonts are replaced by OFL fonts (Urbanist, Barlow Semi
# Condensed), and its Quake picture by one of this project's console pictures
# (mods/console-pictures, made from freely licensed photos; its LICENSE credits
# each), which also cover the catalog platforms the theme has no picture for.
# Nano's own Akrobat is replaced by Barlow here. Every game system must have
# its art in every bundled theme, and every catalog platform its picture in
# Colorful (scripts/check_system_art.py). The theme settings in build.prop are
# the lines of nano-theme.props, which the Linux build uses as its defaults.
#
# It also builds in the Android Games helper (tools/android-games), which lists
# the installed Android games as the Android Games system and starts them;
# building it needs the tools scripts/build_android_games.sh lists.
#
# Usage: 50-nano.sh SYSTEM_DIR CACHE_DIR
set -euo pipefail

SYSTEM="$1"
ETC="$SYSTEM/etc"
MODS="$(cd "$(dirname "$0")" && pwd)"
source "$MODS/../scripts/modlib.sh"
# The theme's pins (THEME, THEME_COMMIT, the font URLs and hashes) and the
# functions colorful_theme_fetch and colorful_theme_build.
source "$MODS/../scripts/colorful_theme.sh"
GAMES_PKG="org.thimsy.androidgames"
cache="$(realpath -m "$2")/nano"
work="$cache/work"

if ! is_nano_base "$SYSTEM"; then
  echo "no GammaOS Nano in this base; skipping"
  exit 0
fi

# Both property files get a block appended below; a second run on the same
# image would add another.
if sudo grep -qE '^(persist\.(bootanim\.skip_nano|gammaos\.nano\.(ndstheme|esde\.|nav_sounds|minima\.bg)|gammaos\.performance_mode|gammargb\.control)|ro\.gammaos\.nano\.no_slide)' "$SYSTEM/build.prop"; then
  echo "build.prop already sets Nano's boot, theme, sounds, LEDs or performance mode" >&2
  exit 1
fi
if sudo grep -q '^persist\.gammaos\.qs\.override_default_tiles' "$SYSTEM/product/etc/build.prop"; then
  echo "the product build.prop already sets the Quick Settings tiles" >&2
  exit 1
fi

mkdir -p "$cache/fonts"
rm -rf "$work"
mkdir -p "$work"
colorful_theme_fetch "$cache"

# Nano reads bundled theme sets from /system/etc/nano_esde_themes/<name>/. The
# README carries the license and credits.
src="$work/$THEME"
colorful_theme_build "$cache" "$src" "$MODS/console-pictures"

# The Slate theme's system logos are their owners' trademarks; the source patch no longer
# bundles it, and an image built without the patch has it removed here.
sudo rm -f "$ETC/nano_esde_themes/slate-es-de.zip"
# With no theme bundled, the patched image has no themes folder.
if ! sudo test -d "$ETC/nano_esde_themes"; then
  sudo install -d -m 755 -o root -g root "$ETC/nano_esde_themes"
  sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$ETC/nano_esde_themes"
fi
dest="$ETC/nano_esde_themes/$THEME"
sudo rm -rf "$dest"
sudo cp -r --no-preserve=all "$src" "$dest"
sudo chown -R root:root "$dest"
sudo find "$dest" -type d -exec chmod 755 {} + -o -type f -exec chmod 644 {} +
sudo find "$dest" -exec setfattr -n security.selinux -v u:object_r:system_file:s0 {} +
echo "bundled $THEME ($(sudo du -sh "$dest" | cut -f1))"

# Nano draws ES-DE theme text that has no fontPath in Akrobat, from these two
# paths. Barlow Semi Condensed Medium and SemiBold take their place.
xmb_fonts="$ETC/nano_xmb/fonts"
for pair in "akrobat-regular.ttf BarlowSemiCondensed-Medium.ttf" "akrobat-semibold.ttf BarlowSemiCondensed-SemiBold.ttf"; do
  read -r old new <<<"$pair"
  if sudo test -f "$xmb_fonts/$old"; then
    write_through "$cache/fonts/$new" "$xmb_fonts/$old"
    replaced_akrobat=1
  fi
done
if [[ -n "${replaced_akrobat:-}" ]]; then
  install_system_file "$cache/fonts/Barlow-OFL.txt" "$xmb_fonts/Barlow-OFL.txt"
  echo "replaced Nano's Akrobat with Barlow Semi Condensed"
fi

bash "$MODS/../scripts/build_android_games.sh" "$work/AndroidGames.apk" "$cache/android-games.keystore"
app_dir="$SYSTEM/app/AndroidGames"
sudo install -d -m 755 -o root -g root "$app_dir"
sudo setfattr -n security.selinux -v u:object_r:system_file:s0 "$app_dir"
install_system_file "$work/AndroidGames.apk" "$app_dir/AndroidGames.apk"
# The apps setup installs are emulators and tools, not Android games, except
# the added apps marked game=1.
{ grep -E '^apk .*  game=1( |$)' "$MODS/added-apps.txt" || true; } |
  sed -nE 's/.*(^| )package=([^ ]+).*/\2/p' >"$work/added-games.txt"
sudo grep -v '^#' "$ETC/gammaos/apps.list" | cut -d'|' -f2 | grep . | grep -vxFf "$work/added-games.txt" \
  >"$work/android-games-exclude.txt"
install_system_file "$work/android-games-exclude.txt" "$ETC/gammaos/android-games-exclude.txt"

python3 -I "$MODS/../scripts/nano_systems.py" "$MODS/nano-systems.txt" "$ETC/nano_xmb/daijishou" \
  "$ETC/nano_xmb/icons_retroarch" "$ETC/gammaos/apps.list" "$MODS/retroarch-cores.txt" "$work/nano_systems.json" \
  "$GAMES_PKG"
python3 -I "$MODS/../scripts/check_system_art.py" "$work/nano_systems.json" "$ETC/nano_esde_themes" \
  "$ETC/nano_xmb/daijishou"
install_system_file "$work/nano_systems.json" "$ETC/gammaos/nano_systems.json"

# Nano's setup wizard starts Nano before setup.sh runs, and Nano writes its
# built-in systems on its first start, so a fresh setup replaces them; Nano
# reloads the file when it changes.
cat >"$work/setup-steps.sh" <<EOF
if [ "\$FRESH_SETUP" = 1 ]; then
    step "Setting up the Nano game systems."
    cp /system/etc/gammaos/nano_systems.json /data/system/nano_systems.json
    chown root:root /data/system/nano_systems.json
    chmod 644 /data/system/nano_systems.json
    restorecon /data/system/nano_systems.json
fi
step "Listing the installed Android games."
run pm grant $GAMES_PKG android.permission.READ_EXTERNAL_STORAGE
run pm grant $GAMES_PKG android.permission.WRITE_EXTERNAL_STORAGE
run am broadcast -a $GAMES_PKG.SYNC -n $GAMES_PKG/.SyncReceiver

EOF
add_setup_steps "$SYSTEM" "$work/setup-steps.sh"

sudo cat "$SYSTEM/build.prop" >"$work/build.prop"
# The theme lines are nano-theme.props', the Linux build's defaults too.
{
  cat <<EOF

# Thimsy OS: boot into Nano, with the ES-DE theme engine, Minima's
# look on Midnight for the settings, no menu sounds and no slide settings
persist.bootanim.skip_nano=0
EOF
  grep '^persist\.' "$MODS/nano-theme.props"
  cat <<EOF
ro.gammaos.nano.no_slide=1
# The stick LEDs on, following the screen's colour (the vendor sets the Follow
# effect, but nothing turns GammaRGB on), and Max Performance
persist.gammargb.control=on
persist.gammaos.performance_mode=max
# No global display shader until one is chosen
persist.gammaos.shader.enable=0
EOF
} >>"$work/build.prop"
write_through "$work/build.prop" "$SYSTEM/build.prop"

# The RG Cube's Quick Settings tiles (its vendor build.prop, without the HDMI
# ones: a monitor is always the only screen), for Android's shade and Nano's
# Quick Settings. The product partition's build.prop is read after the
# vendor's, so these replace its list.
sudo cat "$SYSTEM/product/etc/build.prop" >"$work/product.prop"
cat >>"$work/product.prop" <<EOF

# Thimsy OS: the RG Cube's Quick Settings tiles
persist.gammaos.qs.override_default_tiles=1
persist.gammaos.qs.override_default_tiles_0=internet,bt,performance,fan,gammashader,gammargb,gammaeq,rotation
persist.gammaos.qs.override_default_tiles_1=deepsleepmode,immersivemode
persist.gammaos.qs.override_default_tiles_2=abxy,dpadAnalogToggle,analogsensitivity,analogdeadzone,analogcalibration
persist.gammaos.qs.override_default_tiles_3=analogaxis,rightanalogaxis,retroarchmenubuttonoverride,mappingeditor
EOF
write_through "$work/product.prop" "$SYSTEM/product/etc/build.prop"
rm -rf "$work"
