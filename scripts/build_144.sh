#!/bin/bash
# Builds Thimsy on the GammaOS Next 1.4.4 base (README "1.4.4 base") in one
# command, from Git Bash on Windows or from Linux:
#
#   scripts/build_144.sh                   # compile the system, then the .pac
#   scripts/build_144.sh --system-only     # only compile and export the system
#   scripts/build_144.sh --pac-only        # only the .pac, from out/aosp
#
# 1. The 1.4.4 system image is compiled from GammaOS's source with
#    patches/thimsy/0001-thimsy.patch (scripts/aosp/system.sh), which Docker
#    can't do: the tree is about 300 GB. On Windows that runs in a WSL distro
#    kept for it (THIMSY_AOSP_DISTRO, default thimsy-aosp, as user
#    THIMSY_AOSP_USER, default builder; set it up as the README says), in
#    AOSP_DIR on that distro's own disk (default /aosp/gammaos). On Linux it
#    runs here, AOSP_DIR then having no default.
# 2. What the build needs from the tree (the system image, nano-bootlogo and
#    the boot logo's SVG and font) is copied to out/aosp/.
# 3. scripts/build.sh runs in Docker (docker compose run build) with
#    SYSTEM_IMG and the boot logo's files from out/aosp/, like any other build.
#    THIMSY_RELEASE, THIMSY_COMMIT, DOCK_FIX, CHARGE_FIX, COMPOSER_FIX, SKIP_TESTS,
#    NEW_KEYSTORE, REFRESH_CORES and ALLOW_BUNDLED_CORES pass through. The apps it signs
#    need their keystores in the Docker volume (README "1.4.4 base").
#
# THIMSY_AOSP_JOBS limits the compile's parallel jobs, and THIMSY_AOSP_NINJA_ONLY=1
# resumes a stopped compile without Soong's and Kati's analysis
# (scripts/aosp/system.sh).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/aosp
compile=1 pac=1
case "${1:-}" in
  "") ;;
  --system-only) pac=0 ;;
  --pac-only) compile=0 ;;
  *) sed -n '5,7p' "$0" >&2; exit 2 ;;
esac

if ((compile)); then
  if [[ ${OSTYPE:-} == msys* || ${OSTYPE:-} == cygwin* ]]; then
    # Git Bash would rewrite the Linux paths passed to wsl.exe as Windows ones.
    export MSYS_NO_PATHCONV=1
    distro=${THIMSY_AOSP_DISTRO:-thimsy-aosp}
    user=${THIMSY_AOSP_USER:-builder}
    aosp=${AOSP_DIR:-/aosp/gammaos}
    export_dir=${aosp%/}.export
    # -e runs the command itself: with --, the distro's shell would parse the
    # arguments again (dropping a Windows path's backslashes).
    wsl.exe -d "$distro" -u "$user" -e true || {
      echo "no WSL distro $distro with user $user (README \"1.4.4 base\" sets it up)" >&2; exit 1; }
    here=$(wsl.exe -d "$distro" -u "$user" -e wslpath -a "$(cygpath -w "$ROOT")" | tr -d '\r')
    [[ -n $here ]] || { echo "can't find $ROOT in $distro" >&2; exit 1; }
    # The export is copied into the checkout as root: the distro mounts
    # Windows drives owned by root, so $user can't write there. Checked now,
    # not after hours of compiling.
    mkdir -p "$ROOT/out"
    wsl.exe -d "$distro" -u root -e test -w "$here/out" || {
      echo "root in $distro can't write $here/out, where the compiled system is copied" >&2; exit 1; }
    wsl.exe -d "$distro" -u "$user" --cd / -e env THIMSY_AOSP_JOBS="${THIMSY_AOSP_JOBS:-}" \
      THIMSY_AOSP_NINJA_ONLY="${THIMSY_AOSP_NINJA_ONLY:-}" bash "$here/scripts/aosp/system.sh" "$aosp" "$export_dir"
    echo "copying $distro:$export_dir to out/aosp"
    # Into a side folder first, so an interrupted copy leaves no out/aosp.
    rm -rf "$OUT.partial"
    mkdir -p "$OUT.partial"
    wsl.exe -d "$distro" -u root -e cp -r "$export_dir/." "$here/out/aosp.partial/"
    rm -rf "$OUT"
    mv "$OUT.partial" "$OUT"
  else
    [[ -n ${AOSP_DIR:-} ]] || { echo "set AOSP_DIR to GammaOS's source tree (fetched there if missing)" >&2; exit 1; }
    bash "$ROOT/scripts/aosp/system.sh" "$AOSP_DIR" "$OUT"
  fi
fi

if ((pac)); then
  for f in system.img bin/nano-bootlogo thimsy.svg Roboto-Regular.ttf manifest.txt; do
    [[ -f $OUT/$f ]] || { echo "no out/aosp/$f: compile the system first (without --pac-only)" >&2; exit 1; }
  done
  sed 's/^/  /' "$OUT/manifest.txt"
  want=$(sha256sum "$ROOT/patches/thimsy/0001-thimsy.patch" | cut -d' ' -f1)
  grep -qx "patch $want" "$OUT/manifest.txt" || {
    echo "out/aosp/system.img was built from another version of patches/thimsy/0001-thimsy.patch; compile it again" >&2
    exit 1
  }
  env_args=()
  for v in THIMSY_RELEASE THIMSY_COMMIT DOCK_FIX CHARGE_FIX COMPOSER_FIX SKIP_TESTS NEW_KEYSTORE REFRESH_CORES ALLOW_BUNDLED_CORES; do
    [[ -n ${!v+x} ]] && env_args+=(-e "$v")
  done
  cd "$ROOT"
  MSYS_NO_PATHCONV=1 docker compose run --rm --build -T \
    -e SYSTEM_IMG=out/aosp/system.img \
    -e BOOT_LOGO_SVG=out/aosp/thimsy.svg \
    -e BOOT_LOGO_FONT=out/aosp/Roboto-Regular.ttf \
    "${env_args[@]}" \
    build bash -c 'PATH="$PWD/out/aosp/bin:$PATH" exec scripts/build.sh'
fi
