#!/usr/bin/env bash
# compose.yaml's entrypoint: runs the command, then gives the files it created
# in the checkout back to the checkout's owner. The container runs as root (the
# build loop-mounts images), so on a Linux host they would otherwise be root's;
# on Docker Desktop the checkout already shows as root's and nothing changes.
# work/ is a Docker volume and stays root's.
set -u
trap : INT TERM
"$@"
rc=$?
owner="$(stat -c %u:%g .)"
if [[ "$owner" != 0:0 ]]; then
  find . -path ./work -prune -o -xdev -user 0 -exec chown -h "$owner" {} + 2>/dev/null
fi
exit "$rc"
