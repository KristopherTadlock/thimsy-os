#!/bin/sh
# Builds one Thimsy Base target with Buildroot in Docker (IMPLEMENTATION.md E.8;
# FlipBase's build.sh is the pattern).
#
# Usage: linux/build.sh [options] TARGET [make target ...]
#        linux/build.sh [options] card DEVICE [DEVICE ...]
#   TARGET     a target of configs/targets.json (python3 tools/defconfig.py --list):
#              a CPU tier's userland with its GPU packs, or a family's hardware pack
#   card       re-runs the post-image step of an already built hardware pack
#              (Buildroot's target-post-image) with THIMSY_DEVICES=DEVICE..., so
#              only those devices' cards are put together again; the devices are
#              board/<family>/devices/<id>.json of one family, and the hardware
#              target is the one whose BR2_ROOTFS_POST_SCRIPT_ARGS has
#              --family=<family>. No --publish, --clean or make targets.
#   --publish  a public build: only a target marked "publish": true (a release),
#              from a clean tree at a tagged commit; adds legal-info and a git
#              archive of the whole repository (decisions D16, I37, I41).
#              Implies --clean (and empties images/<target>/). A hardware
#              pack also needs its tier's images/thimsy-build.stamp to say
#              the tier was built with --publish at this commit
#   (every build writes output-<target>/images/thimsy-build.stamp: sha, version,
#   target, publish, dirty; a hardware pack warns when its tier's differs)
#   --dry-run  print the steps in order and run none of them
#   --clean    start from an empty output tree
#   --force=I-checked-no-android-build-runs
#              passed to tools/build_guard.sh: skips its memory and load checks only
#   -h, --help
#
# Order: the target's own checks (it exists, its defconfig is current, the
# files it names exist, --publish only for a publish target); then
# tools/build_guard.sh, which refuses while the Android build may be running
# and holds /mnt/wsl/thimsy-build.lock for the rest of the run; then git archive
# tarballs of libthimsy/ and nano/ at HEAD and the exported Nano assets (with
# the Colorful theme prepared as Android prepares it, OA-1), with hash files,
# into the download directory (E.1: the recipes build from them, so legal-info
# saves them and nothing untracked reaches an image); then Buildroot
# in a debian:trixie-slim container, as an amd64 cross build.
# Make targets default to "all sdk" for a tier (the hardware packs build with
# its SDK) and "all" for a hardware pack.
#
# THIMSY_WORK (default <repo>/work/linux): dl/ (downloads; dl/thimsy-src/ the
#   tarballs, dl/colorful-theme/ the theme's), images/<target>/ and build logs.
# THIMSY_BUILD_DIR (default <work>/build): Buildroot, the output trees, ccache
#   and the unpacked SDK. ext4 only: never under /mnt/<drive> (plan section 10).
# THIMSY_PYTHON (default python3) runs tools/defconfig.py.
# The container exports THIMSY_BUILD_TARGET=<target>, THIMSY_GIT_SHA,
# THIMSY_VERSION (the tag with --publish, else git describe) and, for card,
# THIMSY_DEVICES to the post-build and post-image scripts.
#
# Exit status: 0 built (or dry run), 1 the build failed, 2 usage or an unknown
# target, 3 refused by the guard, 4 refused by the target's checks.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
# Buildroot: FlipBase's pin (plan 3.1), so its tree merges without a version move.
BR_VERSION="2026.08.2"
BR_SHA256="bc9497c8e3a734c277e541e2f7fcf9a6275dc92f75bd2f753b05981c54be8705"
DOCKER_IMAGE="debian:trixie-slim"
PLATFORM="linux/amd64"
PY="${THIMSY_PYTHON:-python3}"
WORK="${THIMSY_WORK:-$REPO/work/linux}"
BUILD_DIR="${THIMSY_BUILD_DIR:-$WORK/build}"
SDK_NAME="aarch64-buildroot-linux-gnu_sdk-buildroot"

say() { printf 'build.sh: %s\n' "$*" >&2; }
die() { code="$1"; shift; say "$*"; exit "$code"; }

publish=0 dry=0 clean=0 force="" guarded=0
while [ $# -gt 0 ]; do
  case "$1" in
    --publish) publish=1 ;;
    --dry-run) dry=1 ;;
    --clean) clean=1 ;;
    --force=*) force="$1" ;;
    --guarded) guarded=1 ;;  # internal: re-run by build_guard.sh, holding the lock
    -h | --help) sed -n '2,/^# target, 3 refused/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) die 2 "unknown option $1 (see --help)" ;;
    *) break ;;
  esac
  shift
done
[ $# -ge 1 ] || die 2 "no target given (see --help; tools/defconfig.py --list)"
TARGET="$1"
shift
MAKE_TARGETS="$*"
DEVICES=""
if [ "$TARGET" = card ]; then
  # card DEVICE...: the family from the device JSONs, the hardware target from
  # the defconfig whose post-script arguments name that family.
  [ $# -ge 1 ] || die 2 "card: no device given (board/<family>/devices/<id>.json)"
  [ "$publish" = 0 ] || die 2 "card: --publish builds whole targets, not single cards"
  [ "$clean" = 0 ] || die 2 "card: --clean would remove the hardware pack card needs"
  family=""
  for d in "$@"; do
    case "$d" in
      "" | -* | *[!a-z0-9-]*) die 2 "card: bad device id '$d' (lower case letters, digits and -)" ;;
    esac
    found=""
    for f in "$HERE"/board/*/devices/"$d".json; do
      if [ -f "$f" ]; then found="$f"; fi
    done
    [ -n "$found" ] || die 2 "card: no board/*/devices/$d.json"
    fam="$(basename "$(dirname "$(dirname "$found")")")"
    [ -z "$family" ] || [ "$fam" = "$family" ] ||
      die 2 "card: $d is a $fam device, not $family: one family per run"
    family="$fam"
    case " $DEVICES " in *" $d "*) ;; *) DEVICES="${DEVICES:+$DEVICES }$d" ;; esac
  done
  hw=""
  for c in "$HERE"/configs/*_defconfig; do
    if grep -q "^BR2_ROOTFS_POST_SCRIPT_ARGS=\".*--family=$family[\" ]" "$c"; then
      t="$(basename "$c" _defconfig)"
      hw="${hw:+$hw }$t"
    fi
  done
  case "$hw" in
    "") die 2 "card: no hardware target builds family $family (configs/*_defconfig)" ;;
    *" "*) die 2 "card: several hardware targets build family $family: $hw" ;;
  esac
  TARGET="$hw"
  MAKE_TARGETS="target-post-image"
fi

# step NAME TEXT: one line of the plan. A dry run prints it; a real run logs it.
step() {
  if [ "$dry" = 1 ]; then printf '%s: %s\n' "$1" "$2"; else say "$1: $2"; fi
}
# check TEXT: a refusal that a dry run reports and continues past.
check() {
  if [ "$dry" = 1 ]; then printf 'check: would refuse: %s\n' "$1"; else die 4 "refused: $1"; fi
}

# -- The target's own checks: read-only and quick, before the guard. ---------
info="$("$PY" "$HERE/tools/defconfig.py" --info "$TARGET")" ||
  die 2 "unknown target $TARGET; known: $("$PY" "$HERE/tools/defconfig.py" --list | tr '\n' ' ')"
field() { printf '%s\n' "$info" | sed -n "s/^$1=//p"; }
KIND="$(field kind)"
SDK_FROM="$(field sdk_from)"
if [ "$publish" = 1 ] && [ "$(field publish)" != true ]; then
  die 4 "refused: --publish: $TARGET is not marked \"publish\": true in configs/targets.json; publishable: $("$PY" "$HERE/tools/defconfig.py" --list --publish | tr '\n' ' ')"
fi
[ "$(field errors)" = 0 ] || die 4 "refused: $TARGET's fragments do not merge (tools/defconfig.py --check)"
"$PY" "$HERE/tools/defconfig.py" --check >/dev/null 2>&1 ||
  check "the defconfigs are stale or the tree fails its lint: run tools/defconfig.py --check"
missing="$(field missing)"
[ -z "$missing" ] || check "$TARGET needs files that do not exist yet: $missing"
case "$REPO" in
  /mnt/[a-z]/* | /[a-z]/*) check "the tree is on a Windows drive ($REPO); build from an ext4 clone" ;;
esac
case "$BUILD_DIR" in
  /mnt/[a-z]/* | /[a-z]/*) check "THIMSY_BUILD_DIR is on a Windows drive ($BUILD_DIR); use ext4" ;;
esac
TAG=""
SHA="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
DIRTY=0
[ -z "$(git -C "$REPO" status --porcelain 2>/dev/null)" ] || DIRTY=1
if [ "$publish" = 1 ]; then
  [ "$DIRTY" = 0 ] || check "--publish needs a clean tree (git status shows changes)"
  TAG="$(git -C "$REPO" describe --exact-match --tags HEAD 2>/dev/null)" ||
    check "--publish needs HEAD to carry a release tag"
  # Buildroot never removes what an earlier build put in an output tree, so a
  # public build starts from an empty one (security-2, build-integration-3).
  clean=1
fi
# ro.thimsy.build.version (post-build.d/20-build-props.sh: the container has
# linux/ only, not the repository's .git).
VERSION="${TAG:-$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo 0.0.0-dev)}"
# A hardware pack's cards carry its tier's root (card.sh: output-<sdk_from>/
# images). The tier writes images/thimsy-build.stamp when it is built; a public
# hardware pack needs a public tier build of this very commit
# (build-integration-4, security-2).
STAMP_NAME="thimsy-build.stamp"
if [ "$KIND" = hw ]; then
  stamp="$BUILD_DIR/output-$SDK_FROM/images/$STAMP_NAME"
  sget() { sed -n "s/^$1=//p" "$stamp" 2>/dev/null | head -n 1; }
  tsha="$(sget sha)" tpub="$(sget publish)" tdirty="$(sget dirty)"
  twhy="$SDK_FROM's images ($stamp) are from ${tsha:-an unknown build} (publish=${tpub:-?}, dirty=${tdirty:-?}), HEAD is $SHA"
  if [ "$publish" = 1 ]; then
    { [ "$tsha" = "$SHA" ] && [ "$tpub" = 1 ] && [ "$tdirty" = 0 ]; } ||
      check "--publish $TARGET: $twhy; run linux/build.sh --publish $SDK_FROM first"
  elif [ "$guarded" = 1 ] && { [ "$tsha" != "$SHA" ] || [ "$tdirty" != 0 ]; }; then
    say "warning: $twhy: the cards will carry that root"
  fi
fi
if [ -z "$MAKE_TARGETS" ]; then
  if [ "$KIND" = tier ]; then MAKE_TARGETS="all sdk"; else MAKE_TARGETS="all"; fi
fi
[ "$publish" = 0 ] || MAKE_TARGETS="$MAKE_TARGETS legal-info"

# -- The guard, before any work. It re-runs this script holding the lock. ----
if [ "$guarded" = 0 ]; then
  step guard "sh tools/build_guard.sh${force:+ $force} -- (this script, holding the build lock)"
  if [ "$dry" = 0 ]; then
    if [ -n "$DEVICES" ]; then
      # shellcheck disable=SC2086
      set -- card $DEVICES
    else
      set -- "$TARGET" "$@"
    fi
    [ "$clean" = 0 ] || set -- --clean "$@"
    [ "$publish" = 0 ] || set -- --publish "$@"
    exec sh "$HERE/tools/build_guard.sh" ${force:+"$force"} -- sh "$HERE/build.sh" --guarded "$@"
  fi
fi

# run CMD...: run it, or print it in a dry run.
run() {
  if [ "$dry" = 1 ]; then return 0; fi
  "$@"
}
sha256() { sha256sum "$1" | cut -d' ' -f1; }

DL="$WORK/dl"
SRC="$DL/thimsy-src"
IMAGES="$WORK/images/$TARGET"
# A public build's images directory holds only what this run writes.
[ "$publish" = 0 ] || step clean "rm -rf images/$TARGET and /build/output-$TARGET (--publish implies --clean)"
[ "$publish" = 0 ] || run rm -rf "$IMAGES"
run mkdir -p "$SRC" "$IMAGES" "$BUILD_DIR"

# -- Buildroot itself, checked against its pinned hash. -----------------------
BR_TAR="$DL/buildroot-$BR_VERSION.tar.xz"
[ -n "$DEVICES" ] || step buildroot "buildroot-$BR_VERSION.tar.xz into $DL, sha256 $BR_SHA256"
if [ "$dry" = 0 ] && [ -z "$DEVICES" ] && [ "$(sha256 "$BR_TAR" 2>/dev/null)" != "$BR_SHA256" ]; then
  curl -fSL --retry 3 -o "$BR_TAR" "https://buildroot.org/downloads/buildroot-$BR_VERSION.tar.xz"
  [ "$(sha256 "$BR_TAR")" = "$BR_SHA256" ] || die 1 "buildroot: bad sha256"
fi

# -- Source tarballs (E.1) and their hash files. ------------------------------
# hashfile PKG FILE: Buildroot reads <global patch dir>/<pkg>/<pkg>.hash, and the
# container adds dl/thimsy-src/hash to BR2_GLOBAL_PATCH_DIR. The lines of
# package/PKG/extra-downloads.hash (the package's other downloads) go in too.
hashfile() {
  mkdir -p "$SRC/hash/$1"
  printf '# Written by linux/build.sh for %s at %s\nsha256  %s  %s\n' \
    "$1" "$SHA" "$(sha256 "$SRC/$2")" "$2" >"$SRC/hash/$1/$1.hash"
  if [ -f "$HERE/package/$1/extra-downloads.hash" ]; then
    grep -v '^#' "$HERE/package/$1/extra-downloads.hash" >>"$SRC/hash/$1/$1.hash" || true
  fi
}
# archive PKG NAME DIR: git archive of DIR at HEAD as NAME-<sha>.tar.gz.
archive() {
  step archive "git archive HEAD:$3 -> dl/thimsy-src/$2-$SHA.tar.gz (package $1)"
  [ "$dry" = 0 ] || return 0
  git -C "$REPO" cat-file -e "HEAD:$3" 2>/dev/null || die 1 "$3/ is not in HEAD"
  git -C "$REPO" archive --format=tar.gz --prefix="$2-$SHA/" \
    -o "$SRC/$2-$SHA.tar.gz" "HEAD:$3"
  hashfile "$1" "$2-$SHA.tar.gz"
}
if [ -n "$DEVICES" ]; then
  step card "$TARGET's post-image again, for $DEVICES (no sources or Buildroot download: the tree is built)"
else
archive libthimsy libthimsy libthimsy
archive thimsy-nano nano nano
# The assets: nano_tree.py's export (tracked files plus the fetched upstream
# assets, decision I2), filtered through the ASSETS.md allowlist (I38) before
# it is packed reproducibly: legal-info saves this tarball as thimsy-data's
# source and --publish publishes it, so it must hold no REPLACE or ASK asset
# (security-1). thimsy-data's install applies the same list again.
# export-assets takes its output directory as a positional argument.
step archive "nano_tree.py export-assets, filtered by package/thimsy-data/assets.list -> dl/thimsy-src/nano-assets-$SHA.tar.gz (package thimsy-data)"
step theme "scripts/colorful_theme.sh prepare (HEAD's, downloads in dl/colorful-theme) into the export before the filter, Slate removed, and mods/nano-theme.props as .thimsy-defaults/ (OA-1)"
if [ "$dry" = 0 ]; then
  [ -f "$REPO/tools/nano-tree/nano_tree.py" ] ||
    die 1 "tools/nano-tree/nano_tree.py is missing (step G0); cannot export the assets"
  assets="$BUILD_DIR/nano-assets-$SHA"
  rm -rf "$assets"
  "$PY" "$REPO/tools/nano-tree/nano_tree.py" export-assets "$assets/export"
  # The default ES-DE theme is Android's (OA-1): Colorful (Simplified), prepared
  # by the script mods/50-nano.sh runs, at HEAD, with the same pins and console
  # pictures, so both builds ship the same tree; the filter below still applies
  # assets.list to it. Slate is not shipped, as on Android.
  git -C "$REPO" archive -o "$assets/theme-src.tar" HEAD scripts/colorful_theme.sh mods/console-pictures
  mkdir -p "$assets/theme-src"
  tar -xf "$assets/theme-src.tar" -C "$assets/theme-src"
  theme_sh="$assets/theme-src/scripts/colorful_theme.sh"
  themes="$assets/export/nano_esde_themes"
  rm -f "$themes/slate-es-de.zip"
  mkdir -p "$themes"
  THIMSY_PYTHON="$PY" bash "$theme_sh" prepare "$DL/colorful-theme" "$themes/$(bash "$theme_sh" name)" ||
    die 1 "the Colorful theme could not be prepared (scripts/colorful_theme.sh)"
  # The licence texts legal-info saves with the package (thimsy-data.mk's
  # LICENSE_FILES), at HEAD like the rest: ATTRIBUTION.md and NOTICE.
  mkdir -p "$assets/licences"
  for f in ATTRIBUTION.md NOTICE; do
    git -C "$REPO" show "HEAD:$f" >"$assets/licences/$f" || die 1 "$f is not in HEAD"
  done
  "$PY" "$HERE/tools/assets_allowlist.py" filter --list "$HERE/package/thimsy-data/assets.list" \
    --licence "$assets/licences/ATTRIBUTION.md" --licence "$assets/licences/NOTICE" \
    "$assets/export" "$assets/nano-assets-$SHA"
  # Nano's theme settings, the lines Android's build.prop gets (OA-1), at HEAD:
  # thimsy-data installs them as /usr/share/thimsy/persist-defaults.props.
  mkdir -p "$assets/nano-assets-$SHA/.thimsy-defaults"
  git -C "$REPO" show HEAD:mods/nano-theme.props \
    >"$assets/nano-assets-$SHA/.thimsy-defaults/nano-theme.props" || die 1 "mods/nano-theme.props is not in HEAD"
  rm -rf "$assets/export" "$assets/theme-src" "$assets/theme-src.tar"
  epoch="$(git -C "$REPO" log -1 --format=%ct HEAD)"
  tar -C "$assets" --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -cf - "nano-assets-$SHA" | gzip -9n >"$SRC/nano-assets-$SHA.tar.gz"
  rm -rf "$assets"
  hashfile thimsy-data "nano-assets-$SHA.tar.gz"
fi
if [ "$publish" = 1 ]; then
  step archive "git archive HEAD -> images/$TARGET/thimsy-os-$TAG-source.tar.gz (the whole repository, I41)"
  run git -C "$REPO" archive --format=tar.gz --prefix="thimsy-os-$TAG/" \
    -o "$IMAGES/thimsy-os-$TAG-source.tar.gz" HEAD
fi
fi

# -- Buildroot in Docker. -----------------------------------------------------
envtxt=""
[ -z "$DEVICES" ] || envtxt=" THIMSY_DEVICES=\"$DEVICES\""
step docker "$DOCKER_IMAGE ($PLATFORM): make O=/build/output-$TARGET ${TARGET}_defconfig, then make $MAKE_TARGETS (log: images/$TARGET/build.log; environment THIMSY_BUILD_TARGET=$TARGET$envtxt)"
[ "$dry" = 0 ] || exit 0
docker run --rm --platform "$PLATFORM" \
  -v "$BUILD_DIR":/build -v "$DL":/dl -v "$IMAGES":/images -v "$HERE":/thimsy:ro \
  -e BR_VERSION="$BR_VERSION" -e TARGET="$TARGET" -e KIND="$KIND" -e SDK_FROM="$SDK_FROM" \
  -e SDK_NAME="$SDK_NAME" -e MAKE_TARGETS="$MAKE_TARGETS" -e CLEAN="$clean" \
  -e THIMSY_GIT_SHA="$SHA" -e THIMSY_BUILD_TARGET="$TARGET" -e THIMSY_DEVICES="$DEVICES" \
  -e THIMSY_VERSION="$VERSION" -e THIMSY_DIRTY="$DIRTY" -e PUBLISH="$publish" -e STAMP_NAME="$STAMP_NAME" \
  "$DOCKER_IMAGE" sh -euc '
  export DEBIAN_FRONTEND=noninteractive FORCE_UNSAFE_CONFIGURE=1
  apt-get -qq update >/dev/null
  apt-get -qq install -y --no-install-recommends build-essential file wget cpio unzip \
    rsync bc git ca-certificates python3 perl patch xz-utils bzip2 gzip libncurses-dev \
    cmake >/dev/null
  B="/build/buildroot-$BR_VERSION"
  [ -d "$B" ] || tar -xJf "/dl/buildroot-$BR_VERSION.tar.xz" -C /build
  O="/build/output-$TARGET"
  [ "$CLEAN" = 0 ] || rm -rf "$O"
  # Seen by the post-build and post-image scripts (20-build-props.sh, card.sh).
  export THIMSY_BUILD_TARGET THIMSY_GIT_SHA THIMSY_VERSION
  if [ -n "$THIMSY_DEVICES" ]; then
    export THIMSY_DEVICES
    [ -f "$O/.config" ] || { echo "card: $O is not built: run linux/build.sh $TARGET first" >&2; exit 1; }
  else
    unset THIMSY_DEVICES
  fi
  if [ "$KIND" = hw ] && [ -z "${THIMSY_DEVICES:-}" ]; then
    # A hardware pack builds with the SDK of its tier (hw-common.fragment).
    sdk="/build/output-$SDK_FROM/images/$SDK_NAME.tar.gz"
    [ -f "$sdk" ] || { echo "no SDK at $sdk: build $SDK_FROM first (its default targets include sdk)" >&2; exit 1; }
    rm -rf /build/sdk && mkdir -p /build/sdk && tar -xzf "$sdk" -C /build/sdk
    "/build/sdk/$SDK_NAME/relocate-sdk.sh"
  fi
  cp "/thimsy/configs/${TARGET}_defconfig" /tmp/defconfig
  gpd="$(sed -n "s/^BR2_GLOBAL_PATCH_DIR=\"\(.*\)\"\$/\1/p" /tmp/defconfig)"
  printf "BR2_GLOBAL_PATCH_DIR=\"%s /dl/thimsy-src/hash\"\n" "$gpd" >>/tmp/defconfig
  make -C "$B" O="$O" BR2_EXTERNAL=/thimsy BR2_DEFCONFIG=/tmp/defconfig defconfig >/dev/null
  # The stamp says which commit the images are from; it goes first, so a failed
  # build leaves none (build.sh reads a tier stamp before a hardware pack).
  [ -n "${THIMSY_DEVICES:-}" ] || rm -f "$O/images/$STAMP_NAME"
  # shellcheck disable=SC2086
  make -C "$B" O="$O" THIMSY_GIT_SHA="$THIMSY_GIT_SHA" THIMSY_SRC_DIR=/dl/thimsy-src \
    $MAKE_TARGETS >/images/build.log 2>&1 || { tail -40 /images/build.log >&2; exit 1; }
  if [ -z "${THIMSY_DEVICES:-}" ]; then
    mkdir -p "$O/images"
    printf "sha=%s\nversion=%s\ntarget=%s\npublish=%s\ndirty=%s\n" "$THIMSY_GIT_SHA" \
      "$THIMSY_VERSION" "$TARGET" "$PUBLISH" "$THIMSY_DIRTY" >"$O/images/$STAMP_NAME"
  fi
  cp -r --sparse=never "$O"/images/. /images/
  [ ! -d "$O/legal-info" ] || cp -r "$O/legal-info" /images/
' || die 1 "the build failed; log: $IMAGES/build.log"
say "done: $IMAGES"
