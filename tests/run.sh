#!/usr/bin/env bash
# Runs the build's tests: quick (the default; build.sh runs these first), slow, or all; and on the
# fork's linux branch (docs/linux/IMPLEMENTATION.md D) the fork-only suites, which quick never runs:
#   nano    tools/nano-tree/tests (the patch tools, sources.txt, the Android surface, the ratchets,
#           and check-unit-0 through tools/nano-tree/tests/test_nano_host.py)
#   linux   linux/tests (linux/tests/run.sh)
#   cxx     the small C++ suites with g++ and make, each behind linux/tools/build_guard.sh --light and
#           the lock /tmp/thimsy-cxx.lock: libthimsy, Nano's compat shims, layer-2 stubs
#           (check-stubs), check-unit-0 and check-headless-0, the Linux platform's shell gate and
#           control keys (platform/linux/tests/platform.mk), thimsy-padd, thimsy-session. The stubs
#           and headless suites run under check-unit-0's guard class, the platform one under
#           check-compat's. On Linux directly; elsewhere in WSL (THIMSY_WSL_DISTRO,
#           default Ubuntu), on a copy under ~/.cache/thimsy/cxx-src when the checkout is on /mnt.
#           A suite the guard refuses is reported as refused, and the mode fails.
#
# Usage: tests/run.sh [quick|slow|all|nano|linux|cxx] [unittest options, such as -v or -k NAME]
#
# THIMSY_TEST_CACHE=<the build's work/cache> adds the tests of the mods that
# need its downloads; they copy what they use and never write to it.
# THIMSY_PYTHON picks the interpreter of the nano and linux modes (default python3; "py -3.12" on
# Windows).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mode="${1:-quick}"
case "$mode" in
  quick | slow | all | nano | linux | cxx) ;;
  *) echo "usage: tests/run.sh [quick|slow|all|nano|linux|cxx] [unittest options]" >&2; exit 2 ;;
esac
shift $(($# ? 1 : 0))
cd "$ROOT"

case "$mode" in
  nano)
    # shellcheck disable=SC2086 # THIMSY_PYTHON may carry arguments
    exec ${THIMSY_PYTHON:-python3} -I -B -W ignore::ResourceWarning -m unittest discover \
      -s tools/nano-tree/tests -t tools/nano-tree/tests "$@" ;;
  linux)
    exec sh linux/tests/run.sh "$@" ;;
  cxx)
    if [ "$(uname -s)" != Linux ]; then
      # Git Bash and the like: run this mode again inside WSL, on the same checkout.
      win=$(pwd -W 2>/dev/null || pwd)
      drive=$(printf '%s' "$win" | cut -c1 | tr 'A-Z' 'a-z')
      wslroot="/mnt/$drive$(printf '%s' "$win" | cut -c3- | tr '\\' '/')"
      # MSYS_NO_PATHCONV: Git Bash would turn the /mnt/... argument into a Windows path.
      MSYS_NO_PATHCONV=1 exec wsl.exe -d "${THIMSY_WSL_DISTRO:-Ubuntu}" -- bash "$wslroot/tests/run.sh" cxx "$@"
    fi
    nano_data="$ROOT/work/nano-composed"
    case "$ROOT" in
      /mnt/*)
        # A Windows checkout seen through WSL's /mnt (9p): slow, and timing-sensitive tests (the
        # session E2E) time out there. Build and test a copy on the Linux side instead.
        copy="$HOME/.cache/thimsy/cxx-src"
        echo "=== copying the sources to $copy"
        rm -rf "$copy" && mkdir -p "$copy"
        tar -C "$ROOT" -cf - nano libthimsy linux | tar -C "$copy" -xf -
        cd "$copy"
        ROOT="$copy" ;;
    esac
    [ -d "$nano_data" ] || nano_data=""
    guard="$ROOT/linux/tools/build_guard.sh"
    lock() { if command -v flock >/dev/null; then flock /tmp/thimsy-cxx.lock "$@"; else "$@"; fi; }
    failed=""
    suite() {  # suite NAME GUARD-TARGET MAKE-ARGS...
      local name="$1" target="$2"
      shift 2
      echo "=== $name"
      if ! sh "$guard" --light --target "$target"; then
        echo "=== $name: refused by the memory guard" >&2
        failed="$failed $name(refused)"
        return 0
      fi
      lock nice -n 19 make --no-print-directory -j1 "$@" || failed="$failed $name"
    }
    suite libthimsy libthimsy -C libthimsy check
    # check-compat would run libthimsy's tests again; they ran just above.
    suite nano-compat check-compat -C nano/test/host check-compat COMPAT_WITH_LIBTHIMSY=0
    suite nano-stubs check-unit-0 -C nano/test/host check-stubs
    suite nano-unit-0 check-unit-0 -C nano/test/host check-unit-0 ${nano_data:+NANO_DATA="$nano_data"}
    suite nano-headless-0 check-unit-0 -C nano/test/host check-headless-0
    suite nano-platform-linux check-compat -f nano/platform/linux/tests/platform.mk check COMPAT_WITH_LIBTHIMSY=0
    suite thimsy-padd thimsy-padd -C linux/src/thimsy-padd check
    suite thimsy-session thimsy-session -C linux/src/thimsy-session check
    if [ -n "$failed" ]; then
      echo "tests/run.sh cxx: FAILED:$failed" >&2
      exit 1
    fi
    echo "tests/run.sh cxx: every suite passed"
    exit 0 ;;
esac
# The scripts under test leave files to the garbage collector when they exit early.
THIMSY_TEST_MODE="$mode" exec python3 -I -W ignore::ResourceWarning -m unittest discover -s tests -t "$ROOT" "$@"
