#!/bin/bash
# Compiles Thimsy's GammaOS Next 1.4.4 system image (README "1.4.4 base") on a
# Linux build host, and exports what scripts/build.sh needs from the tree.
#
#   scripts/aosp/system.sh AOSP_DIR EXPORT_DIR
#
# AOSP_DIR is GammaOS's source (TheGammaSqueeze/GammaOSNextDistribution-14):
# fetched there at the 1.4.4 base commit if it isn't there yet, about 20 GB
# downloaded and 120 GB checked out, plus about 150 GB of build output. It must
# be on a case-sensitive Linux filesystem (on Windows: a WSL distro's own disk,
# never /mnt/c). The host needs the packages scripts/aosp/setup_host.sh
# installs.
#
# On every run the tree's files are made exactly the base commit with
# patches/thimsy/0001-thimsy.patch applied (scripts/aosp/sync_patch.py), so an
# edit made in the tree by hand and not yet in the patch is undone, never
# built: the manifest's patch sha256 is what the image was built from. The
# sync rewrites only the files that differ, so later builds are incremental
# and a change to a source file doesn't redo Soong's analysis. It is then built the
# way GammaOS builds a release
# (lineage_build_unified/buildbot_unified.sh treble 64GN nosync, target
# lineage_arm64_bgN, with uncompressed APEXes as in GammaOS's build.sh).
#
# EXPORT_DIR gets:
#   system.img             the system image (SYSTEM_IMG for scripts/build.sh)
#   bin/nano-bootlogo      the boot picture's host tool (a wrapper; on PATH)
#   lib64/                 the host libraries nano-bootlogo needs
#   thimsy.svg             BOOT_LOGO_SVG
#   Roboto-Regular.ttf     BOOT_LOGO_FONT
#   manifest.txt           base commit, patch and image sha256s
#
# Env: THIMSY_AOSP_JOBS limits the build's parallel jobs (default: one per
# physical core, as GammaOS's buildbot does; lower it on hosts with less than
# about 2 GB of RAM per job). THIMSY_AOSP_NINJA_ONLY=1 resumes a build that
# stopped (a crash, or stopped by hand) by running ninja on the last build's
# plan, skipping Soong's and Kati's analysis; only when that analysis finished
# on the tree as it is now (PLAN_MARK holds this patch's sha256 and the sync
# changed nothing).
set -euo pipefail

REPO=https://github.com/TheGammaSqueeze/GammaOSNextDistribution-14.git
# The 1.4.4 build (lineage-21.0-20261002) ends here; see BACKPORTS.md.
BASE=0bc531e2d64698316f28cedc8992ffa0e44f1a76
TARGET=arm64_bgN
# LFS files the build never reads: Google's apps (the Thimsy product leaves
# them out) and Windows and macOS toolchains.
LFS_EXCLUDE='vendor/gapps/**,**/windows-x86/**,**/darwin-x86/**'
# The patch the tree's files were last synced to.
MARK=.thimsy-patch.sha256
# The patch whose tree Soong's and Kati's analysis last finished on, with no
# file changed since (THIMSY_AOSP_NINJA_ONLY's condition).
PLAN_MARK=.thimsy-plan.sha256

[[ $# -eq 2 ]] || { sed -n '5,6p' "$0" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PATCH=$ROOT/patches/thimsy/0001-thimsy.patch
AOSP=$(realpath -m "$1")
EXPORT=$(realpath -m "$2")
[[ -f $PATCH ]] || { echo "no $PATCH" >&2; exit 1; }
[[ $(id -u) -ne 0 ]] || { echo "run as a normal user, not root (AOSP refuses root builds)" >&2; exit 1; }

log() { printf '\n== %s  %s\n' "$(date '+%H:%M:%S')" "$*"; }

# Case-insensitive filesystems (NTFS through /mnt/c, macOS's default) break
# the tree: it has files whose names differ only in case.
mkdir -p "$AOSP"
probe="$AOSP/.thimsy-case-probe" PROBE="$AOSP/.THIMSY-CASE-PROBE"
rm -f "$probe" "$PROBE"
touch "$probe"
if [[ -e $PROBE ]]; then
  rm -f "$probe"
  echo "$AOSP is on a case-insensitive filesystem; use a Linux filesystem (a WSL distro's own disk)" >&2
  exit 1
fi
rm -f "$probe"

cd "$AOSP"
if [[ ! -d .git ]]; then
  log "fetching GammaOS's source at ${BASE:0:9} into $AOSP"
  git init -q .
  git config core.autocrlf false
  git remote add origin "$REPO"
fi
if [[ $(git rev-parse -q --verify HEAD 2>/dev/null) != "$BASE" ]]; then
  if ! git cat-file -e "$BASE^{commit}" 2>/dev/null; then
    GIT_LFS_SKIP_SMUDGE=1 git fetch --depth 1 origin "$BASE"
  fi
  log "checking out ${BASE:0:9}"
  rm -f "$MARK" "$PLAN_MARK"
  GIT_LFS_SKIP_SMUDGE=1 git -c advice.detachedHead=false checkout -q -f "$BASE"
fi
want=$(sha256sum "$PATCH" | cut -d' ' -f1)
had=$(cat "$MARK" 2>/dev/null || true)
# Always, even when the marker says this patch is applied: the tree is edited
# by hand (the patch is a diff of it), and an edit that isn't in the patch
# must not be built under the patch's name. The tree becomes exactly the base
# with this patch, rewriting only the files whose contents differ (so an edit
# to one .cpp doesn't make Soong analyse everything again); a patch that
# doesn't apply changes nothing.
log "syncing ${BASE:0:9} with the patch (${want:0:12})"
rm -f "$MARK"
sync_out=$(python3 -I "$ROOT/scripts/aosp/sync_patch.py" "$AOSP" "$BASE" "$PATCH") || {
  printf '%s\n' "$sync_out" >&2; exit 1; }
printf '%s\n' "$sync_out"
if [[ $had != "$want" || $(head -1 <<<"$sync_out") != "wrote 0 file(s), removed 0" ]]; then
  # The tree changed, so the last build's plan may not match it any more.
  rm -f "$PLAN_MARK"
  [[ $had != "$want" ]] || log "the tree differed from the patch (edits not in it); those were undone"
fi
echo "$want" > "$MARK"
# After any checkout or reset, which leave LFS files as pointers.
log "fetching large files (Git LFS)"
git lfs pull --exclude="$LFS_EXCLUDE"

log "building lineage_${TARGET} (this takes hours the first time)"
mkdir -p out/home/build-output
# Things newer than this were made by this build.
started=out/.thimsy-build-started
touch "$started"
export OVERRIDE_PRODUCT_COMPRESSED_APEX=false
if [[ -n ${THIMSY_AOSP_JOBS:-} ]]; then
  # buildbot_unified.sh counts physical cores with lscpu; put a stand-in first.
  shim=$(mktemp -d)
  printf '#!/bin/sh\ni=0; while [ $i -lt %d ]; do echo "$i,0"; i=$((i+1)); done\n' "$THIMSY_AOSP_JOBS" > "$shim/lscpu"
  chmod +x "$shim/lscpu"
  export PATH="$shim:$PATH"
fi
plan=out/combined-lineage_${TARGET}.ninja
if [[ ${THIMSY_AOSP_NINJA_ONLY:-} == 1 ]]; then
  # Soong's and Kati's build plan from the last build, used as it is: no
  # analysis (2 h and 30 GB of memory on a 16 GB host). Only for resuming a
  # build that stopped (a crash, or stopped by hand) after that analysis
  # finished on the tree as it is now: a changed Android.bp or makefile, or
  # a new or removed source file, isn't seen by the old plan.
  [[ -f $PLAN_MARK && $(cat "$PLAN_MARK") == "$want" ]] || {
    echo "THIMSY_AOSP_NINJA_ONLY: the last build's plan isn't from the tree as it is now (the patch or the tree changed since, or its analysis never finished); build without it" >&2
    exit 1; }
  # soong_ui records the environment it ran ninja in (OUT_DIR, TMPDIR, its
  # PATH and the like, which the plan's commands use) in ninja_env.
  ninja_env=out/soong/ninja.environment
  [[ -f $plan && -f out/soong/build.lineage_${TARGET}.ninja && -f $ninja_env ]] || {
    echo "THIMSY_AOSP_NINJA_ONLY: no build plan in out/ yet; build without it first" >&2; exit 1; }
  log "running ninja on the existing build plan (no Soong or Kati)"
  # nano-bootlogo (a host tool the export needs) is named too, in case
  # systemimage doesn't pull it in.
  (
    set +eu
    source build/envsetup.sh >/dev/null
    source vendor/lineage/vars/aosp_target_release
    # Only for $OUT (where the image lands); ninja gets soong_ui's environment.
    lunch "lineage_${TARGET}-${aosp_target_release}-userdebug" >/dev/null || exit 1
    set -eu
    jobs=${THIMSY_AOSP_JOBS:-$(lscpu -b -p=Core,Socket | grep -v '^#' | sort -u | wc -l)}
    mapfile -d '' env_vars < <(python3 -I -c '
import json, sys
for e in json.load(open(sys.argv[1])):
    sys.stdout.write("%s=%s\0" % (e["Key"], e["Value"]))' "$ninja_env")
    [[ ${#env_vars[@]} -gt 0 ]] || { echo "nothing in $ninja_env" >&2; exit 1; }
    mkdir -p out/soong/.temp
    env -i "${env_vars[@]}" \
      prebuilts/build-tools/linux-x86/bin/ninja -f "$plan" -j "$jobs" \
      -o usesphonyoutputs=yes -w dupbuild=err -w missingdepfile=err systemimage nano-bootlogo
    # Where buildbot_unified.sh puts it.
    cp "$OUT/system.img" "out/home/build-output/lineage-21.0-$(date -u +%Y%m%d)-UNOFFICIAL-${TARGET}.img"
  )
else
  # The buildbot asks no questions in nosync mode; it pauses 5 s at the start.
  rc=0
  HOME="$AOSP/out/home" bash lineage_build_unified/buildbot_unified.sh treble 64GN nosync || rc=$?
  # soong_ui writes the combined plan only once Soong's and Kati's analysis
  # has finished (createCombinedBuildNinjaFile in build/soong/ui/build/
  # build.go), just before ninja runs. A build that got that far on this
  # tree, even one that then failed, leaves a plan THIMSY_AOSP_NINJA_ONLY can
  # resume.
  if [[ -f $plan && $plan -nt $started ]]; then
    echo "$want" > "$PLAN_MARK"
  fi
  [[ $rc == 0 ]] || { echo "the build failed (exit $rc)" >&2; exit "$rc"; }
fi

# || true: with no image ls fails, and pipefail would end the script here,
# without the message below.
img=$(ls -t out/home/build-output/lineage-21.0-*-UNOFFICIAL-"$TARGET".img 2>/dev/null | head -1 || true)
[[ -n $img && -f $img ]] || { echo "the build left no system image in out/home/build-output" >&2; exit 1; }
[[ $img -nt $started ]] || {
  echo "$img is older than this build" >&2; exit 1; }

log "exporting to $EXPORT"
host=out/host/linux-x86
tool=$host/bin/nano-bootlogo
svg=frameworks/base/cmds/gammaos-nano/bootscreen/thimsy.svg
font=external/roboto-fonts/Roboto-Regular.ttf
for f in "$tool" "$svg" "$font"; do
  [[ -f $f ]] || { echo "missing $AOSP/$f after the build" >&2; exit 1; }
done
rm -rf "$EXPORT"
mkdir -p "$EXPORT/bin" "$EXPORT/lib64"
cp "$img" "$EXPORT/system.img"
cp "$tool" "$EXPORT/bin/nano-bootlogo.real"
# The host libraries it loads from the tree (libc++ and the like).
ldd "$tool" | awk '$3 ~ /^\// {print $3}' | while read -r lib; do
  case $lib in "$AOSP"/*|out/*) cp -L "$lib" "$EXPORT/lib64/" ;; esac
done
for lib in $(ldd "$tool" | awk '$2 == "=>" && $3 == "not" {print $1}'); do
  [[ -f $host/lib64/$lib ]] && cp -L "$host/lib64/$lib" "$EXPORT/lib64/"
done
cat > "$EXPORT/bin/nano-bootlogo" <<'EOF'
#!/bin/sh
# nano-bootlogo from the AOSP host build, with the libraries exported beside it.
here=$(cd "$(dirname "$0")" && pwd)
LD_LIBRARY_PATH="$here/../lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/nano-bootlogo.real" "$@"
EOF
chmod +x "$EXPORT/bin/nano-bootlogo" "$EXPORT/bin/nano-bootlogo.real"
cp "$svg" "$EXPORT/thimsy.svg"
cp "$font" "$EXPORT/Roboto-Regular.ttf"
# It must run outside the tree (as it will in Docker).
"$EXPORT/bin/nano-bootlogo" "$EXPORT/thimsy.svg" "$EXPORT/Roboto-Regular.ttf" 64 64 "$EXPORT/.probe.rgba"
[[ $(stat -c %s "$EXPORT/.probe.rgba") -eq $((64 * 64 * 4)) ]] || { echo "nano-bootlogo's output is the wrong size" >&2; exit 1; }
rm -f "$EXPORT/.probe.rgba"
{
  echo "base $BASE"
  echo "patch $want"
  echo "image $(basename "$img")"
  echo "system.img $(sha256sum "$EXPORT/system.img" | cut -d' ' -f1)"
  echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$EXPORT/manifest.txt"
cat "$EXPORT/manifest.txt"
