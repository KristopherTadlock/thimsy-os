#!/bin/sh
# Runs linux/'s tests (IMPLEMENTATION.md D.4): Python's unittest over linux/tests,
# standard library only, seconds in all. Nothing here runs Buildroot.
#
# Usage: linux/tests/run.sh [unittest options, such as -v or -k NAME]
#
# THIMSY_PYTHON picks the interpreter (default python3; "py -3.12" on Windows).
# THIMSY_BUILDROOT=<a Buildroot tree at the pinned version> also checks every
# upstream symbol the fragments name. The guard's tests need flock(1) and run in
# WSL; elsewhere they are skipped.
set -eu
LINUX="$(cd "$(dirname "$0")/.." && pwd)"
cd "$LINUX"
# shellcheck disable=SC2086 # THIMSY_PYTHON may carry arguments
exec ${THIMSY_PYTHON:-python3} -B -I -m unittest discover -s tests -t . "$@"
