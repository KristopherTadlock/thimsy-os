#!/bin/bash
# Build bbread statically for arm64 (Android/Linux) and x86_64 (host test),
# using the AOSP prebuilt clang and AOSP musl sysroots. Run inside the
# thimsy-aosp WSL distro:  bash build.sh
set -euo pipefail
cd "$(dirname "$0")"
AOSP=${AOSP:-/aosp/gammaos}
CLANG=${CLANG:-$AOSP/prebuilts/clang/host/linux-x86/clang-r487747c/bin/clang}
SR=$AOSP/prebuilts/build-tools/sysroots
RT=$(dirname "$CLANG")/../lib/clang/17/lib/linux
CFLAGS="-Os -Wall -Wextra -Wno-unused-parameter -static -fuse-ld=lld -nostdlib++ -rtlib=compiler-rt --unwindlib=none"

"$CLANG" --target=aarch64-unknown-linux-musl --sysroot="$SR/aarch64-unknown-linux-musl" \
	$CFLAGS -o bbread-arm64 bbread.c
"$(dirname "$CLANG")/llvm-strip" bbread-arm64

"$CLANG" --target=x86_64-unknown-linux-musl --sysroot="$SR/x86_64-unknown-linux-musl" \
	$CFLAGS -o bbread-x86_64 bbread.c
"$(dirname "$CLANG")/llvm-strip" bbread-x86_64

file bbread-arm64 bbread-x86_64 2>/dev/null || true
ls -l bbread-arm64 bbread-x86_64
sha256sum bbread-arm64 bbread-x86_64
