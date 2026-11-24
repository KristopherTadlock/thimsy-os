#!/usr/bin/env bash
# Builds hakuX (the Xbox emulator in mods/added-apps.txt) from source:
# rfandango/hakuX at HAKUX_COMMIT with the patches in tools/hakux/patches
# (series), and the native dependencies at the revisions the mirrored build
# used (the mirror release's -build-manifest.json): those hakuX's
# android/README.md names, but
# nv2a_vsh_cpu at xemu's 1115255. Runs in tools/hakux/Dockerfile's image:
#
#   docker compose run --rm hakux
#
# Writes out/hakux/:
#   hakuX-<version>-<commit>-thimsy<N>-unsigned.apk   the release build, unsigned
#   hakuX-<version>-<commit>-thimsy<N>-testkey.apk    the same, signed with a
#       throwaway key (work/hakux/testkey.jks) to try on a device; never ship it
#   hakuX-<version>-<commit>-thimsy<N>-source.tar.xz  the source it was built
#       from: hakuX with the patches applied, its FEX submodule, the native
#       dependencies and tools/hakux (GPL: ship it with the APK)
#   hakuX-<version>-<commit>-thimsy<N>-build-manifest.json   every source
#       revision, patch, tool and output hash
#
# SOURCE_DIR=<an unpacked -source.tar.xz, in the checkout> builds that source
# as it is (a copy, at the paths a normal build uses), fetching no source (Gradle
# still downloads its own dependencies the first time): the tarball's own way
# to be rebuilt, to the same APK, from the Thimsy commit that made it.
#
# KEY_PROPERTIES=<file in the checkout> (hakuX's key.properties format:
# storeFile, storePassword, keyAlias, keyPassword; a relative storeFile is
# next to that file) signs the release with that key instead, as
# hakuX-<version>-<commit>-thimsy<N>.apk. Keep the key outside tools/hakux,
# which the source tarball packs (the build refuses one there), for example in
# keys/; .gitignore keeps key.properties and keystores out of git, and the copy
# the build makes is deleted when it ends.
# Sources and Gradle's cache stay in work/hakux, so a second build downloads
# nothing; SKIP_TESTS=1 skips hakuX's host tests.
set -euo pipefail

HAKUX_REPO=https://github.com/rfandango/hakuX.git
HAKUX_COMMIT=a7cdb5a975e31730d488c47f6f10440b16c1e6f4
# Bump when the patch set changes, so each build has its own file name.
THIMSY_BUILD=1
# path in NC_DEPS, repository, commit (the tag or revision hakuX names), as
# the mirrored APK's -build-manifest.json has them: nv2a_vsh_cpu is
# xemu's 1115255, as hakuX's 561fe80 is in no published repository.
DEPS=(
  "volk https://github.com/zeux/volk.git 0b17a763ba5643e32da1b2152f8140461b3b7345 1.4.304"
  "glslang https://github.com/KhronosGroup/glslang.git b5782e52ee2f7b3e40bb9c80d15b47016e008bc9 16.1.0"
  "spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git ef913b3ab3da1becca3cf46b15a10667c67bebe5 vulkan-sdk-1.4.328.0"
  "vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git 1d8f600fd424278486eade7ed3e877c99f0846b1 v3.3.0"
  "SDL3 https://github.com/libsdl-org/SDL.git 829a65d769d935c4852f8159e964312c0957260a release-3.4.18"
  "adrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce254dfc1344527e05301e43f37dea2df80 8fae8ce"
  "root/subprojects/nv2a_vsh_cpu https://github.com/xemu-project/nv2a_vsh_cpu.git 1115255708c10c4841b65dcd2223262e7a316598 1115255"
  "libslirp https://gitlab.freedesktop.org/slirp/libslirp.git 26be815b86e8d49add8c9a8b320239b9594ff03d 4.7.0+26be815b"
)
# The FEX submodules hakuX's README says to fetch.
FEX_SUBMODULES=(External/fmt External/range-v3 External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse)

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tools/hakux"
WORK="$ROOT/work/hakux"
OUT="$ROOT/out/hakux"
SRC="$WORK/hakuX"
NC_DEPS="$WORK/deps"
if [[ -n "${SOURCE_DIR:-}" ]]; then
  SOURCE_DIR="$(realpath "$SOURCE_DIR")"
  [[ -d "$SOURCE_DIR/hakuX" && -d "$SOURCE_DIR/deps" ]] || { echo "SOURCE_DIR: no hakuX/ and deps/ in $SOURCE_DIR" >&2; exit 1; }
  # The name, patch list, pins and image are this checkout's: they must be the
  # ones the tarball was made with.
  diff -rq --exclude=__pycache__ "$SOURCE_DIR/tools/hakux" "$HERE" > /dev/null || {
    echo "SOURCE_DIR: its tools/hakux differs from this checkout's; check out the Thimsy commit that made it" >&2
    exit 1; }
  # Built where a normal build builds: the native libraries keep their source
  # paths, so this gives the same APK.
  rm -rf "$SRC" "$NC_DEPS"
  mkdir -p "$WORK"
  cp -a "$SOURCE_DIR/hakuX" "$SRC"
  cp -a "$SOURCE_DIR/deps" "$NC_DEPS"
  # Build output a run in that folder left (none in a fresh tarball).
  rm -rf "$SRC/build" "$SRC/android/app/build" "$SRC/android/.gradle"
fi
: "${ANDROID_NDK:?run it in the image of tools/hakux/Dockerfile: docker compose run --rm hakux}"
export GRADLE_USER_HOME="$WORK/gradle"
mkdir -p "$WORK" "$NC_DEPS" "$OUT"
# The signing key's passwords never stay in the hakuX checkout.
trap 'rm -f "$SRC/android/key.properties"' EXIT
if [[ -n "${KEY_PROPERTIES:-}" ]]; then
  case "$(realpath "$KEY_PROPERTIES")/" in
    "$HERE"/*) echo "KEY_PROPERTIES: keep the key outside tools/hakux, which the source tarball packs" >&2; exit 1 ;;
  esac
fi

# checkout DIR URL COMMIT: DIR at exactly COMMIT, clean.
checkout() {
  local dir="$1" url="$2" rev="$3"
  if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$rev" ]]; then
    rm -rf "$dir"; mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$rev"
    git -C "$dir" checkout -q --detach FETCH_HEAD
  fi
  git -C "$dir" reset -q --hard "$rev"
  git -C "$dir" clean -qfdx
}

patches=()
if [[ -z "${SOURCE_DIR:-}" ]]; then
  echo "== sources"
  for d in "${DEPS[@]}"; do
    read -r path url rev _ <<<"$d"
    checkout "$NC_DEPS/$path" "$url" "$rev"
  done
  git -C "$NC_DEPS/adrenotools" submodule update -q --init --recursive --depth 1
  checkout "$SRC" "$HAKUX_REPO" "$HAKUX_COMMIT"
  git -C "$SRC" submodule update -q --init --depth 1 external/fex
  git -C "$SRC/external/fex" submodule update -q --init --depth 1 "${FEX_SUBMODULES[@]}"
  # hakuX's own patch to adrenotools (its README's table).
  git -C "$NC_DEPS/adrenotools" apply "$SRC/tools/native/adrenotools-kgsl-context-priority.patch"
  while read -r file _; do
    [[ -z "$file" || "$file" == \#* ]] && continue
    git -C "$SRC" apply "$HERE/patches/$file"
    patches+=("$file")
    echo "applied $file"
  done < "$HERE/patches/series"
else
  echo "== source: $SOURCE_DIR (as it is)"
  while read -r file _; do
    [[ -z "$file" || "$file" == \#* ]] || patches+=("$file")
  done < "$HERE/patches/series"
fi

version="$(sed -n 's/^ *versionName = "\(.*\)"/\1/p' "$SRC/android/app/build.gradle.kts")"
name="hakuX-$version-${HAKUX_COMMIT:0:7}-thimsy$THIMSY_BUILD"
# FEX names its version and commit from git: in a shallow checkout no version
# (no tags) and the full commit. Given here, the source tarball (no .git)
# builds the same.
export FEX_COMMIT
if [[ -n "${SOURCE_DIR:-}" ]]; then
  # Not git: inside the checkout, git would answer for Thimsy's repository.
  FEX_COMMIT="$(sed -n 's/^FEX_COMMIT=//p' "$SOURCE_DIR/FEX_COMMIT" 2>/dev/null || true)"
else
  FEX_COMMIT="$(git -C "$SRC/external/fex" rev-parse HEAD)"
fi
[[ "$FEX_COMMIT" =~ ^[0-9a-f]{40}$ ]] || { echo "no FEX commit (git or FEX_COMMIT)" >&2; exit 1; }

# The corresponding source, before anything is built in the checkout:
# hakuX and its dependencies as patched, and tools/hakux as git has it (never a
# key or anything else left in the folder).
if [[ -z "${SOURCE_DIR:-}" ]]; then
  rm -f "$OUT"/hakuX-*-source.tar.xz
  stage="$WORK/source-tools"
  rm -rf "$stage"; mkdir -p "$stage"
  git -C "$ROOT" ls-files -z tools/hakux | (cd "$ROOT" && tar --null -T - -cf -) | tar -C "$stage" -xf -
  # The FEX commit, for a SOURCE_DIR build (the tarball has no .git).
  echo "FEX_COMMIT=$FEX_COMMIT" > "$stage/FEX_COMMIT"
  tar -C "$WORK" --exclude=.git --exclude=__pycache__ --exclude="*.pyc" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
      --mode=u+rw,go+r,go-w --transform "s,^hakuX,$name-source/hakuX," --transform "s,^deps,$name-source/deps," \
      --transform "s,^tools,$name-source/tools," --transform "s,^FEX_COMMIT,$name-source/FEX_COMMIT," \
      -cf - hakuX deps -C "$stage" tools FEX_COMMIT | xz -T0 -9 > "$OUT/$name-source.tar.xz"
  rm -rf "$stage"
fi

if [[ "${SKIP_TESTS:-}" != 1 ]]; then
  echo "== host tests"
  # At this commit the tests build with -Werror, which newer warnings in GCC 13
  # and Clang 18 trip (test_pusher.c, psh.c), and test_apu_hrtf misses -lm on
  # Linux; they pass without -Werror and with -lm. The checkout is reset each run.
  sed -i "s/ -Werror//" "$SRC/tests/hle/CMakeLists.txt"
  log="$WORK/build/hle-tests.log"; mkdir -p "$WORK/build"
  # A fresh build folder: a SOURCE_DIR build is another source tree.
  rm -rf "$WORK/build/hle-tests"
  { cmake -S "$SRC/tests/hle" -B "$WORK/build/hle-tests" -G Ninja -DNC_DEPS="$NC_DEPS" \
      -DCMAKE_C_STANDARD_LIBRARIES=-lm && ninja -C "$WORK/build/hle-tests"; } > "$log" 2>&1 \
    || { cat "$log"; exit 1; }
  (cd "$WORK/build/hle-tests" && ctest --output-on-failure)
  # 0003-xdvdfs-deep-tables.patch's test, built the way its header says.
  (cd "$SRC" && cc -O1 -I. tests/native/xc/xdvdfs_chain_test.c native/core/xdvdfs.c \
    -o "$WORK/build/xdvdfs_chain_test")
  "$WORK/build/xdvdfs_chain_test"
fi

echo "== native core"
cmake -S "$SRC/native" -B "$SRC/build/native/core" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS="$NC_DEPS" -DOVERRIDE_VERSION= -DOVERRIDE_HASH="$FEX_COMMIT" > /dev/null
ninja -C "$SRC/build/native/core" hakuxcore
bash "$SRC/tools/native/app-libs.sh" "$SRC/build/native/core" > /dev/null

echo "== app"
rm -f "$SRC/android/key.properties"
if [[ -n "${KEY_PROPERTIES:-}" ]]; then
  python3 "$HERE/keyprops.py" "$(realpath "$KEY_PROPERTIES")" "$SRC/android/key.properties"
fi
(cd "$SRC/android" && ./gradlew --no-daemon -q assembleRelease)
rm -f "$SRC/android/key.properties"
apks="$SRC/android/app/build/outputs/apk/release"
rm -f "$OUT"/hakuX-*.apk
if [[ -n "${KEY_PROPERTIES:-}" ]]; then
  cp "$apks/app-release.apk" "$OUT/$name.apk"
else
  cp "$apks/app-release-unsigned.apk" "$OUT/$name-unsigned.apk"
  # A copy signed with a throwaway key, to install on a test device.
  bt="$ANDROID_HOME/build-tools/36.1.0"
  ks="$WORK/testkey.jks"
  [[ -f "$ks" ]] || keytool -genkeypair -keystore "$ks" -storepass testkey -alias testkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=hakuX test build (not for release)" > /dev/null 2>&1
  "$bt/zipalign" -f -p 4 "$OUT/$name-unsigned.apk" "$WORK/aligned.apk"
  "$bt/apksigner" sign --ks "$ks" --ks-pass pass:testkey --out "$OUT/$name-testkey.apk" "$WORK/aligned.apk"
  rm -f "$WORK/aligned.apk" "$OUT/$name-testkey.apk.idsig"
fi

echo "== manifest"
rm -f "$OUT"/hakuX-*build-manifest.json
HAKUX_NAME="$name" python3 "$HERE/manifest.py" "$OUT/$name-build-manifest.json" "$SRC" "$NC_DEPS" "$HERE" "$OUT" "$HAKUX_REPO" "$HAKUX_COMMIT" "$THIMSY_BUILD" "${DEPS[@]}"
ls -la "$OUT"
