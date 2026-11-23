#!/usr/bin/env bash
# Assembles the source the build's hakuX APK was built from (mods/added-apps.txt):
# hakuX at a7cdb5a with its FEX submodule, and the native core's third-party
# sources (hakuX's android/README.md) at the revisions that build used. The
# result is DIR/hakuX (the repository) and DIR/deps (CMake NC_DEPS). With
# TARBALL, it also packs DIR into a reproducible .tar.xz, without the .git
# folders. scripts/build_hakux.sh builds the APK from it.
#
# Usage: hakux_source.sh DIR [TARBALL]
set -euo pipefail

DIR="$1"
TARBALL="${2:-}"
[[ ! -e "$DIR" ]] || { echo "$DIR already exists" >&2; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"

# fetch URL REVISION DEST: that one commit, without history.
fetch() {
  git init -q "$3"
  git -C "$3" remote add origin "$1"
  git -C "$3" fetch -q --depth 1 origin "$2"
  git -C "$3" checkout -q --detach FETCH_HEAD
  [[ "$(git -C "$3" rev-parse HEAD)" == "$2" ]] || { echo "$3: got $(git -C "$3" rev-parse HEAD), not $2" >&2; exit 1; }
}

fetch https://github.com/rfandango/hakuX.git a7cdb5a975e31730d488c47f6f10440b16c1e6f4 "$DIR/hakuX"
git -C "$DIR/hakuX" submodule update -q --init --depth 1 external/fex
git -C "$DIR/hakuX/external/fex" submodule update -q --init --depth 1 External/fmt External/range-v3 \
  External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse
[[ "$(git -C "$DIR/hakuX/external/fex" rev-parse HEAD)" == 52018bf46a8279b22bce738548ab3f40db467064 ]]

D="$DIR/deps"
fetch https://github.com/zeux/volk.git 0b17a763ba5643e32da1b2152f8140461b3b7345 "$D/volk"
fetch https://github.com/KhronosGroup/glslang.git b5782e52ee2f7b3e40bb9c80d15b47016e008bc9 "$D/glslang"
fetch https://github.com/KhronosGroup/SPIRV-Reflect.git ef913b3ab3da1becca3cf46b15a10667c67bebe5 "$D/spirv_reflect"
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git 1d8f600fd424278486eade7ed3e877c99f0846b1 "$D/vma"
fetch https://github.com/libsdl-org/SDL.git 829a65d769d935c4852f8159e964312c0957260a "$D/SDL3"
fetch https://github.com/bylaws/libadrenotools.git 8fae8ce254dfc1344527e05301e43f37dea2df80 "$D/adrenotools"
git -C "$D/adrenotools" submodule update -q --init --depth 1
git -C "$D/adrenotools" apply "$DIR/hakuX/tools/native/adrenotools-kgsl-context-priority.patch"
fetch https://gitlab.freedesktop.org/slirp/libslirp.git 26be815b86e8d49add8c9a8b320239b9594ff03d "$D/libslirp"
# hakuX documents nv2a_vsh_cpu 561fe80, which no published repository has; the build used xemu's
# 1115255 (its vertex shader generator test passes with it).
fetch https://github.com/xemu-project/nv2a_vsh_cpu.git 1115255708c10c4841b65dcd2223262e7a316598 "$D/root/subprojects/nv2a_vsh_cpu"

cp "$HERE/hakux-build-manifest.json" "$DIR/hakuX-build-manifest.json"

if [[ -n "$TARBALL" ]]; then
  tar -C "$(dirname "$DIR")" --exclude=.git --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
      -cf - "$(basename "$DIR")" | xz -T0 -9 >"$TARBALL"
  sha256sum "$TARBALL"
fi
