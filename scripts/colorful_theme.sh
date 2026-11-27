#!/usr/bin/env bash
# The Colorful (Simplified) ES-DE theme as Thimsy OS ships it, on Android
# (mods/50-nano.sh) and on Linux (linux/build.sh, for thimsy-data): one script
# with one set of pins, so both builds ship the same theme tree (decision OA-1
# in docs/linux/DECISIONS.md).
#
# Everything shipped must be free to redistribute. The theme's Gilroy font is
# commercial and its Akrobat's free-font EULA forbids redistributing it, so
# OFL fonts (Urbanist, Barlow Semi Condensed) replace them, with their
# licenses, and theme.xml names the new files. Its Quake picture, a Quake
# Champions render (id Software), is replaced by one of this project's console
# pictures (mods/console-pictures, made from freely licensed photos; its
# LICENSE credits each), which also cover the catalog platforms the theme has
# no picture for, with the theme's colours for those systems matched to the
# new pictures (theme-colours.json). The README records the changes.
#
# Usage: colorful_theme.sh fetch CACHE_DIR
#          downloads the theme and the fonts into CACHE_DIR (kept between runs:
#          a file already there is only checked) and checks each against its
#          pinned sha256
#        colorful_theme.sh build CACHE_DIR OUT [PICTURES_DIR]
#          makes the theme folder OUT, which must not exist, from what fetch
#          put in CACHE_DIR, with the console pictures of PICTURES_DIR (default
#          mods/console-pictures beside this script); plain file operations,
#          no download and no checksum
#        colorful_theme.sh prepare CACHE_DIR OUT
#          fetch, then build
#        colorful_theme.sh name
#          prints the theme's folder name
# Sourced (mods/50-nano.sh), it only defines the variables below and the
# functions colorful_theme_fetch and colorful_theme_build. CACHE_DIR/fonts
# keeps the Barlow Semi Condensed Medium and SemiBold files and Barlow-OFL.txt,
# which 50-nano.sh also installs in place of Nano's own Akrobat.
#
# Needs curl and sha256sum (fetch), unzip and python3 (build; THIMSY_PYTHON
# names another interpreter).
# Exit status: 0 done, 1 a download, checksum or change failed, 2 usage.
# (mods/50-nano.sh, which sources it, sets the same shell options.)
set -euo pipefail

# Colorful (Simplified) by ant, from Colorful by viking and faeran. CC BY-NC-SA 2.0.
THEME="colorful-simplified-es-de"
THEME_COMMIT="669e7f6a2de57c5ab69fd865e70d1cd357dbabbb"
THEME_URL="https://github.com/anthonycaccese/$THEME/archive/$THEME_COMMIT.zip"
THEME_SHA256="b36570e97d9233343872d8079c99bddc4116283c274785d086ca63be6eee3211"
# OFL fonts for the ES-DE theme: Urbanist 1.330 (its metrics match Gilroy's)
# and Barlow Semi Condensed 1.422 (close to Akrobat's).
URBANIST_URL="https://github.com/coreyhu/Urbanist/releases/download/1.330/Urbanist-fonts.zip"
URBANIST_SHA256="4b808f025f020d8801bb55a7c3ea855e934527be70bf451ab4205e62bfb8fb60"
URBANIST_OFL_URL="https://raw.githubusercontent.com/coreyhu/Urbanist/cdcd01ab1efda2cdc7ae5453209de0f06ab1a7b4/OFL.txt"
URBANIST_OFL_SHA256="502f01e7ab22276fa50b68f6e4b9db36d2928e08de227484ec5f430cb0d9defa"
BARLOW_URL="https://raw.githubusercontent.com/jpt/barlow/697600fcece1685b0ba409eb82868546def9a84e"
BARLOW_FILES=(
  "fonts/ttf/BarlowSemiCondensed-Medium.ttf ef5c39de3ac1764a24c7495098672ccc034e39679ccd65d76eed752da59e11e1"
  "fonts/ttf/BarlowSemiCondensed-SemiBold.ttf 5358c566b2039988c68b4ea2af05270da055110a5d23fbd686d5a11fbecf4e03"
  "fonts/ttf/BarlowSemiCondensed-Bold.ttf bd434e949d128dd0e5fab17f60e70fcb367bff1c3e0506812c8bd734e001b356"
  "OFL.txt 186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f"
)
COLORFUL_PICTURES="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/mods/console-pictures"

colorful_theme_download() {
  [[ -f "$3" ]] || { curl -fsSL --retry 3 -o "$3.partial" "$1" && mv "$3.partial" "$3"; } || { echo "download failed: $1" >&2; exit 1; }
  echo "$2  $3" | sha256sum -c --quiet || { rm -f "$3"; echo "checksum mismatch: $3 (deleted from the cache)" >&2; exit 1; }
}

# colorful_theme_fetch CACHE_DIR
colorful_theme_fetch() {
  local cache="$1" entry path sha name
  mkdir -p "$cache/fonts"
  colorful_theme_download "$THEME_URL" "$THEME_SHA256" "$cache/$THEME-$THEME_COMMIT.zip"
  colorful_theme_download "$URBANIST_URL" "$URBANIST_SHA256" "$cache/fonts/Urbanist-fonts-1.330.zip"
  colorful_theme_download "$URBANIST_OFL_URL" "$URBANIST_OFL_SHA256" "$cache/fonts/Urbanist-OFL.txt"
  for entry in "${BARLOW_FILES[@]}"; do
    read -r path sha <<<"$entry"
    name="$(basename "$path")"
    [[ "$name" == OFL.txt ]] && name="Barlow-OFL.txt"
    colorful_theme_download "$BARLOW_URL/$path" "$sha" "$cache/fonts/$name"
  done
}

# colorful_theme_build CACHE_DIR OUT [PICTURES_DIR]
colorful_theme_build() {
  local cache="$1" src="$2" pictures="${3:-$COLORFUL_PICTURES}" unpack fonts weight kind f
  local python="${THIMSY_PYTHON:-python3}"
  if [[ -e "$src" ]]; then
    echo "$src already exists" >&2
    exit 1
  fi
  # The archive holds one folder, THEME-THEME_COMMIT; it becomes OUT.
  unpack="$src.unpack"
  rm -rf "$unpack"
  mkdir -p "$unpack"
  unzip -q "$cache/$THEME-$THEME_COMMIT.zip" -d "$unpack"
  mv "$unpack/$THEME-$THEME_COMMIT" "$src"
  rmdir "$unpack"
  rm -f "$src/.gitignore"

  fonts="$src/_inc/fonts"
  rm "$fonts"/Gilroy-{Light,Regular,SemiBold,Bold}.ttf "$fonts/Akrobat-Bold.ttf" "$src/_inc/systems/images/quake.webp"
  for weight in Light Regular SemiBold Bold; do
    unzip -q -j "$cache/fonts/Urbanist-fonts-1.330.zip" "Urbanist-fonts/fonts/ttf/Urbanist-$weight.ttf" -d "$fonts"
  done
  cp "$cache/fonts/BarlowSemiCondensed-Bold.ttf" "$cache/fonts/Urbanist-OFL.txt" "$cache/fonts/Barlow-OFL.txt" "$fonts/"
  "$python" -I - "$src" <<'PYEOF'
import glob, os, sys
src = sys.argv[1]
path = os.path.join(src, "theme.xml")
s = open(path, encoding="utf-8").read()
for old, new in [("Gilroy-Light", "Urbanist-Light"), ("Gilroy-Regular", "Urbanist-Regular"),
                 ("Gilroy-SemiBold", "Urbanist-SemiBold"), ("Gilroy-Bold", "Urbanist-Bold"),
                 ("Akrobat-Bold", "BarlowSemiCondensed-Bold")]:
    old, new = f"./_inc/fonts/{old}.ttf", f"./_inc/fonts/{new}.ttf"
    assert s.count(old) == 1, f"theme.xml: {old} not found once"
    s = s.replace(old, new)
open(path, "w", encoding="utf-8").write(s)
for xml in glob.glob(os.path.join(src, "**", "*.xml"), recursive=True):
    t = open(xml, encoding="utf-8").read()
    assert "Gilroy" not in t and "Akrobat" not in t and "quake.webp" not in t, f"{xml} still names a removed file"
PYEOF
  # This project's console pictures and their metadata, for systems the theme
  # has none for; none of them may replace a file of the theme's.
  for kind in images metadata-global metadata-custom; do
    for f in "$pictures/$kind"/*; do
      if [[ -e "$src/_inc/systems/$kind/$(basename "$f")" ]]; then
        echo "the theme already has _inc/systems/$kind/$(basename "$f")" >&2
        exit 1
      fi
      cp "$f" "$src/_inc/systems/$kind/"
    done
  done
  cp "$pictures/LICENSE" "$src/_inc/systems/THIMSY-PICTURES-LICENSE.txt"
  # The theme's own colours for systems whose picture is now this project's,
  # matched to the new picture (theme-colours.json says why).
  "$python" -I - "$src/_inc/systems/metadata-global" "$pictures/theme-colours.json" <<'PYEOF'
import json, os, re, sys
meta, colours = sys.argv[1], json.load(open(sys.argv[2], encoding="utf-8"))
for system, values in colours.items():
    path = os.path.join(meta, f"{system}.xml")
    s = open(path, encoding="utf-8").read()
    for tag, value in values.items():
        if tag == "why":
            continue
        s, n = re.subn(rf"<{tag}>[0-9A-Fa-f]{{6}}</{tag}>", f"<{tag}>{value}</{tag}>", s)
        assert n == 1, f"{path}: <{tag}> not found once"
    open(path, "w", encoding="utf-8").write(s)
PYEOF
  cat >>"$src/README.md" <<'EOF'

## Changes in Thimsy OS

[Thimsy OS](https://github.com/KristopherTadlock/thimsy-os) ships this theme
with these changes, under the same CC BY-NC-SA 2.0 license:

- The Gilroy font (Radomir Tinkov, commercial) is replaced by
  [Urbanist](https://github.com/coreyhu/Urbanist) 1.330 (Light, Regular,
  SemiBold, Bold), Copyright 2024 The Urbanist Project Authors, SIL Open Font
  License 1.1 (`_inc/fonts/Urbanist-OFL.txt`).
- The help bar's Akrobat Bold (Fontfabric, whose free-font license does not
  allow redistributing it) is replaced by
  [Barlow](https://github.com/jpt/barlow) Semi Condensed Bold 1.422, Copyright
  2017 The Barlow Project Authors, SIL Open Font License 1.1
  (`_inc/fonts/Barlow-OFL.txt`).
- `theme.xml` names the new font files.
- `_inc/systems/images/quake.webp`, a Quake Champions render (id Software),
  is replaced by a Thimsy OS picture.
- Pictures, with their `metadata-global` and `metadata-custom` entries, are
  added for the systems the theme had no picture for (the Quake picture is
  one of them). They are not under this theme's license: they are made by the
  Thimsy OS project from freely licensed photos on Wikimedia Commons, and
  `_inc/systems/THIMSY-PICTURES-LICENSE.txt` gives, for each picture, its
  source photos with their authors and licenses, and its own license (CC
  BY-SA where a source photo is CC BY-SA, otherwise CC BY-NC 4.0).
- The colours in `_inc/systems/metadata-global/moonlight.xml` and
  `quake.xml` are changed to match those systems' new pictures.
EOF
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  usage() {
    echo "usage: $0 fetch CACHE_DIR | build CACHE_DIR OUT [PICTURES_DIR] | prepare CACHE_DIR OUT | name" >&2
    exit 2
  }
  case "${1:-}" in
    fetch) [[ $# -eq 2 ]] || usage; colorful_theme_fetch "$2" ;;
    build) [[ $# -eq 3 || $# -eq 4 ]] || usage; colorful_theme_build "$2" "$3" "${4:-$COLORFUL_PICTURES}" ;;
    prepare) [[ $# -eq 3 ]] || usage; colorful_theme_fetch "$2"; colorful_theme_build "$2" "$3" ;;
    name) [[ $# -eq 1 ]] || usage; echo "$THEME" ;;
    *) usage ;;
  esac
fi
