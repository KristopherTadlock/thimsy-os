#!/bin/sh
# Refuses a build or a heavy test while the Android build may be running
# (IMPLEMENTATION.md E.8 and C.3, decision I18). The WSL2 VM's memory, CPU and
# kernel are shared by every distro and by Docker Desktop, so one build can get
# the other's processes OOM-killed.
#
# Usage: tools/build_guard.sh [--light [--target NAME]]
#                             [--force=I-checked-no-android-build-runs]
#                             [-- COMMAND [ARG ...]]
#
# Refuses (exit 3) when:
#   - the build lock /mnt/wsl/thimsy-build.lock is held (flock -n); /mnt/wsl is
#     shared by every WSL2 distro, so builds that take it exclude each other;
#   - MemAvailable in /proc/meminfo is under 9 GB (9000000 kB);
#   - docker ps lists a container named like the Android build's (thimsy-*build*);
#   - wsl.exe -l --running lists thimsy-aosp, where the Android build runs
#     natively and neither the lock nor docker can see it; or wsl.exe can't run;
#   - the one-minute load in /proc/loadavg is above 8 (half the threads).
# With "-- COMMAND", it holds the lock while COMMAND runs (COMMAND is exec'd
# with the lock on file descriptor 9; children inherit it).
#
# --light: the C++ test Makefiles' mode (C.3). It never takes the lock. While
#   any Android check above fires (lock, container, distro, load), only the
#   small targets pass (check-compat check-unit-0 libthimsy thimsy-session
#   thimsy-padd; --target names the one asked for, and no name counts as heavy).
#   The memory floor is 6 GB for a small target and 9 GB for the others.
# --force=I-checked-no-android-build-runs: skips the memory and load checks
#   only; the lock, distro and container checks always run. --force without
#   the exact words is refused.
#
# Every probe is overridable, for the tests: THIMSY_GUARD_LOCK, _MEMINFO,
# _LOADAVG, _MEM_FLOOR_KB, _LIGHT_MEM_FLOOR_KB, _LOAD_MAX, _DOCKER, _WSL,
# _AOSP_DISTRO, _CONTAINER_GLOB, _SMALL_TARGETS (all THIMSY_GUARD_*).
#
# Exit status: 0 passed (or COMMAND's status), 2 usage, 3 refused.
set -u

LOCK="${THIMSY_GUARD_LOCK:-/mnt/wsl/thimsy-build.lock}"
MEMINFO="${THIMSY_GUARD_MEMINFO:-/proc/meminfo}"
LOADAVG="${THIMSY_GUARD_LOADAVG:-/proc/loadavg}"
MEM_FLOOR_KB="${THIMSY_GUARD_MEM_FLOOR_KB:-9000000}"
LIGHT_MEM_FLOOR_KB="${THIMSY_GUARD_LIGHT_MEM_FLOOR_KB:-6000000}"
LOAD_MAX="${THIMSY_GUARD_LOAD_MAX:-8}"
DOCKER="${THIMSY_GUARD_DOCKER:-docker}"
WSL="${THIMSY_GUARD_WSL:-wsl.exe}"
AOSP_DISTRO="${THIMSY_GUARD_AOSP_DISTRO:-thimsy-aosp}"
CONTAINER_GLOB="${THIMSY_GUARD_CONTAINER_GLOB:-thimsy-*build*}"
SMALL_TARGETS="${THIMSY_GUARD_SMALL_TARGETS:-check-compat check-unit-0 libthimsy thimsy-session thimsy-padd}"
FORCE_WORDS="I-checked-no-android-build-runs"

say() { printf 'build_guard: %s\n' "$*" >&2; }
usage() { sed -n '7,9p' "$0" | sed 's/^# //' >&2; exit 2; }

light=0 force=0 target="" have_cmd=0
while [ $# -gt 0 ]; do
  case "$1" in
    --light) light=1 ;;
    --target=*) target="${1#--target=}" ;;
    --target) [ $# -ge 2 ] || usage; shift; target="$1" ;;
    "--force=$FORCE_WORDS") force=1 ;;
    --force | --force=*)
      say "refused: --force needs its typed confirmation: --force=$FORCE_WORDS"
      exit 3 ;;
    --) shift; have_cmd=1; break ;;
    -h | --help) sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) say "unknown option: $1"; usage ;;
  esac
  shift
done
[ "$have_cmd" = 0 ] || [ $# -gt 0 ] || usage

small=0
if [ "$light" = 1 ] && [ -n "$target" ]; then
  for t in $SMALL_TARGETS; do
    [ "$t" = "$target" ] && small=1
  done
fi

NL='
'
android=""  # signs that the Android build may be running
hard=""     # refusals for any target (memory)
notes=""
add_android() { android="$android$1$NL"; }
add_hard() { hard="$hard$1$NL"; }
add_note() { notes="$notes$1$NL"; }

tmp=""
cleanup() { [ -z "$tmp" ] || rm -f "$tmp"; }
trap cleanup EXIT
trap 'exit 3' HUP INT TERM

# The build lock. Exclusive while a full-mode COMMAND runs; otherwise a shared,
# non-blocking probe that is released at once.
locked=0
if ! command -v flock >/dev/null 2>&1; then
  add_android "cannot check the build lock: flock not found"
elif ! (: >>"$LOCK") 2>/dev/null; then
  add_android "cannot open the build lock $LOCK"
else
  exec 9>>"$LOCK"
  if [ "$light" = 0 ] && [ "$have_cmd" = 1 ]; then
    if flock -n 9; then locked=1; else add_android "the build lock $LOCK is held by another build"; fi
  else
    if flock -n -s 9; then flock -u 9; else add_android "the build lock $LOCK is held by another build"; fi
  fi
  [ "$locked" = 1 ] || exec 9>&-
fi

# Memory: the VM-wide MemAvailable, which an Android build in any distro lowers.
if [ "$force" = 0 ]; then
  floor="$MEM_FLOOR_KB"
  [ "$small" = 1 ] && floor="$LIGHT_MEM_FLOOR_KB"
  avail=$(awk '$1 == "MemAvailable:" { print $2; exit }' "$MEMINFO" 2>/dev/null)
  case "$avail" in
    "" | *[!0-9]*) add_hard "cannot read MemAvailable from $MEMINFO" ;;
    *) [ "$avail" -ge "$floor" ] ||
         add_hard "MemAvailable is $avail kB, under the floor of $floor kB" ;;
  esac
fi

# Containers named like the Android build's. A docker that is missing or whose
# daemon does not answer can't be running one; that is only noted.
if command -v "$DOCKER" >/dev/null 2>&1; then
  if names=$("$DOCKER" ps --format '{{.Names}}' 2>/dev/null); then
    set -f
    for n in $names; do
      # shellcheck disable=SC2254 # the glob is the point
      case "$n" in $CONTAINER_GLOB) add_android "docker container $n is running" ;; esac
    done
    set +f
  else
    add_note "docker ps failed; no container checked"
  fi
fi

# The Android build's own distro, seen through WSL interop. wsl.exe prints
# UTF-16 (or UTF-8 with WSL_UTF8=1) with CRLF line ends and " (Default)" marks.
if ! command -v "$WSL" >/dev/null 2>&1; then
  add_android "cannot list running WSL distros: $WSL not found (is WSL interop on?)"
else
  tmp=$(mktemp "${TMPDIR:-/tmp}/build_guard.XXXXXX") || { say "refused: mktemp failed"; exit 3; }
  if WSL_UTF8=1 "$WSL" -l --running >"$tmp" 2>/dev/null; then
    if tr -d '\000\r' <"$tmp" |
       sed -e 's/[[:space:]]*([^()]*)[[:space:]]*$//' -e 's/^[[:space:]*]*//' -e 's/[[:space:]]*$//' |
       grep -qxF "$AOSP_DISTRO"; then
      add_android "WSL distro $AOSP_DISTRO is running"
    fi
  else
    add_android "cannot list running WSL distros: $WSL -l --running failed"
  fi
fi

# The VM-wide one-minute load (all distros share one kernel).
if [ "$force" = 0 ]; then
  load=$(awk '{ print $1; exit }' "$LOADAVG" 2>/dev/null)
  case "$load" in
    "" | *[!0-9.]*) add_android "cannot read the load from $LOADAVG" ;;
    *) if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN { exit !(l + 0 > m + 0) }'; then
         add_android "the one-minute load is $load, above $LOAD_MAX"
       fi ;;
  esac
fi

print_lines() {
  prefix="$1"
  printf '%s' "$2" | while IFS= read -r line; do
    [ -z "$line" ] || say "$prefix$line"
  done
}

print_lines "note: " "$notes"
if [ -n "$hard" ] || { [ -n "$android" ] && [ "$small" = 0 ]; }; then
  print_lines "refused: " "$hard$android"
  if [ "$light" = 1 ] && [ "$small" = 0 ] && [ -z "$hard" ]; then
    say "only these targets run while an Android build may be running: $SMALL_TARGETS"
  fi
  if [ "$force" = 0 ] && [ -n "$hard" ]; then
    say "--force=$FORCE_WORDS skips the memory and load checks, never the others"
  fi
  exit 3
fi
if [ -n "$android" ]; then
  print_lines "allowed small target $target despite: " "$android"
fi

if [ "$have_cmd" = 1 ]; then
  cleanup
  trap - EXIT
  exec "$@"
fi
exit 0
