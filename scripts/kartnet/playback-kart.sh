#!/bin/bash
# Re-run a recorded netplay race offline and compare it with the recording.
# Every netplay race writes <log>.rec (pair runs: logs/kart-*/host.log.rec and
# guest.log.rec; iPhone runs: also phone.log.rec). The side whose checkpoints
# the playback matches simulated correctly.
#
#   playback-kart.sh <file.rec>            compare the checkpoints
#   SYNCTEST=8 playback-kart.sh <file.rec> also roll back 8 frames every frame
#                                          and check the re-simulation (finds
#                                          state a rollback restores wrongly)
#   VIEW=1                                 draw player 2's view
set -euo pipefail
REC=$(cd "$(dirname "${1:?record file}")" && pwd)/$(basename "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
# the repo's scripts/kartnet/, or the workspace's scripts/ next to SpaghettiKart/
for repo in "$HERE/../.." "$HERE/../SpaghettiKart"; do
    [ -d "$repo/build-cmake" ] && break
done
APP="${APP:-$repo/build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart}"
OUT="${REC%.rec}.playback"
env=(MK64_NET_PLAYBACK="$REC" MK64_NET_LOG="$OUT.log" MK64_NET_PLAYBACK_VIEW="${VIEW:-0}")
if [ -n "${SYNCTEST:-}" ]; then
    env+=(MK64_SYNCTEST="$SYNCTEST" MK64_SYNCTEST_FROM="${SYNCTEST_FROM:-40}" MK64_SYNCTEST_LOG="$OUT-synctest.log")
fi
set +e
env "${env[@]}" "$APP" -ApplePersistenceIgnoreState YES > "$OUT-stdout.log" 2>&1
code=$?
set -e
grep -h "playback\|desync detail" "$OUT.log" || tail -5 "$OUT-stdout.log"
[ -n "${SYNCTEST:-}" ] && { grep -h "Rollback:" "$OUT-stdout.log" | tail -4; echo "SyncTest log: $OUT-synctest.log"; }
exit $code
