#!/usr/bin/env bash
# compose.yaml's entrypoint: runs the command, then gives the files it created
# in the checkout back to the checkout's owner. The container runs as root (the
# build loop-mounts images), so on a Linux host they would otherwise be root's;
# on Docker Desktop the checkout already shows as root's and nothing changes.
# work/ is a Docker volume and stays root's.
#
# As PID 1 it alone gets `docker stop`'s SIGTERM, and SIGINT when there is no
# terminal, so it passes them on to the whole process group (the command, and
# what it is running: a mod, gradle, ninja), and waits for the command to
# exit; with a terminal, Ctrl-C reaches them all directly. compose.yaml gives
# the build time to clean up (stop_grace_period).
set -u
# A command run with & would ignore SIGINT (bash does that without job
# control), and Ctrl-C with it; env gives it the default back.
env --default-signal=INT,QUIT "$@" <&0 &
child=$!
trap 'trap "" TERM; kill -TERM 0 2>/dev/null' TERM
trap '[[ -t 0 ]] || { trap "" INT; kill -INT 0 2>/dev/null; }' INT
# A trapped signal ends `wait` early: wait again until the command is gone.
while :; do
  wait "$child"
  rc=$?
  kill -0 "$child" 2>/dev/null && continue
  # It may have exited while the trap ran: its status is still to collect
  # (127 when the wait above already did).
  wait "$child" 2>/dev/null
  r=$?
  ((r == 127)) || rc=$r
  break
done
owner="$(stat -c %u:%g .)"
if [[ "$owner" != 0:0 ]]; then
  find . -path ./work -prune -o -xdev -user 0 -exec chown -h "$owner" {} + 2>/dev/null
fi
exit "$rc"
