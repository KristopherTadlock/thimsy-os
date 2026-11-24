#!/bin/sh
# Nano's source as Thimsy builds it, outside an AOSP tree: GammaOS's
# frameworks/base/cmds/gammaos-nano at the 1.4.4 base commit, with the Nano part of
# patches/thimsy/0001-thimsy.patch applied. For android_launcher_plan.md (Phase 0, item 1).
#
#   tools/android-launcher/fetch_nano.sh [DEST]      (default: work/android-launcher/upstream)
#
# Only that folder's files are downloaded (about 100 MB), not the whole tree. The tree is never
# checked out: it holds file names Windows can't store, so the folder is read into an index of
# its own and written out from there.
set -eu

REPO=https://github.com/TheGammaSqueeze/GammaOSNextDistribution-14.git
# The 1.4.4 build (lineage-21.0-20261002) ends here; see BACKPORTS.md.
BASE=0bc531e2d64698316f28cedc8992ffa0e44f1a76
NANO=frameworks/base/cmds/gammaos-nano

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=${1:-$ROOT/work/android-launcher/upstream}
PATCH=$ROOT/patches/thimsy/0001-thimsy.patch

rm -rf "$DEST"
mkdir -p "$DEST"
cd "$DEST"
git init -q .
git config core.autocrlf false
git remote add origin "$REPO"
git fetch -q --depth 1 --filter=blob:none origin "$BASE"

TREE=$(git rev-parse "FETCH_HEAD:$NANO")
git ls-tree -r --format='%(objectname)' "$TREE" | sort -u > .git/nano-blobs.txt
git -c fetch.negotiationAlgorithm=noop fetch -q origin --no-tags --no-write-fetch-head \
    --recurse-submodules=no --filter=blob:none --stdin < .git/nano-blobs.txt
git read-tree --prefix="$NANO/" "$TREE"
git checkout-index -a -q

# git apply checks each file's base blob, so a patch made for another base fails here.
git apply --include="$NANO/*" --whitespace=nowarn "$PATCH"

echo "base    $BASE"
echo "source  $DEST/$NANO"
find "$NANO" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.inc' \) | wc -l | sed 's/^ */files   /'
