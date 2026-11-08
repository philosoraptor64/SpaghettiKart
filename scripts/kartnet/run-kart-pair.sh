#!/bin/bash
# Run two SpaghettiKart instances on this Mac connected by KartNet netplay
# (host = player 1, guest = player 2) and report whether they stayed in sync.
#
# Options (environment):
#   FRAMES=n     quit after n netplay frames (default 0 = play until you close a window)
#   BOT=1        scripted drivers on both sides (no controller needed)
#   HOST_BOT/GUEST_BOT=seed   bot on one side only
#   DELAY=n      input delay in frames (default 2)
#   MODE=gp|vs   Grand Prix with CPUs (default) or Versus
#   VIEW=full|split|single (HOST_VIEW/GUEST_VIEW per side), DRIFT=1 bots also drift
#   HOST_REPLAY/GUEST_REPLAY=file  drive that side from a ReplayCheck input recording
#   CUP, COURSE, CC, HOST_CHAR, GUEST_CHAR, SEED
set -u
SK="$(cd "$(dirname "$0")/../.." && pwd)"; ROOT="${KARTNET_LOGS_ROOT:-$SK/..}"
APP="$SK/build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart"
STAMP=$(date +%Y%m%d-%H%M%S)
LOGS="$ROOT/scripts/logs/kart-$STAMP"
mkdir -p "$LOGS"

FRAMES=${FRAMES:-0}
DELAY=${DELAY:-2}
MODE=${MODE:-gp}
HOST_BOT=${HOST_BOT:-${BOT:+11}}
GUEST_BOT=${GUEST_BOT:-${BOT:+22}}

common=(MK64_NET=1 MK64_NET_FRAMES="$FRAMES")
host_env=("${common[@]}" MK64_NET_HOST=1 MK64_NET_BIND=127.0.0.1:7101 MK64_NET_DELAY="$DELAY"
    MK64_NET_MODE="$MODE" MK64_NET_LOG="$LOGS/host.log")
[ -n "${CUP:-}" ] && host_env+=(MK64_NET_CUP="$CUP")
[ -n "${COURSE:-}" ] && host_env+=(MK64_NET_COURSE="$COURSE")
[ -n "${CC:-}" ] && host_env+=(MK64_NET_CC="$CC")
[ -n "${SEED:-}" ] && host_env+=(MK64_NET_SEED="$SEED")
[ -n "${HOST_CHAR:-}" ] && host_env+=(MK64_NET_CHAR="$HOST_CHAR")
[ -n "${HOST_BOT:-}" ] && host_env+=(MK64_NET_BOT="$HOST_BOT")
guest_env=("${common[@]}" MK64_NET_BIND=127.0.0.1:7102 MK64_NET_PEER=127.0.0.1:7101 MK64_NET_LOG="$LOGS/guest.log")
[ -n "${GUEST_CHAR:-}" ] && guest_env+=(MK64_NET_CHAR="$GUEST_CHAR")
[ -n "${GUEST_BOT:-}" ] && guest_env+=(MK64_NET_BOT="$GUEST_BOT")
[ -n "${HOST_VIEW:-${VIEW:-}}" ] && host_env+=(MK64_NET_VIEW="${HOST_VIEW:-$VIEW}")
[ -n "${GUEST_VIEW:-${VIEW:-}}" ] && guest_env+=(MK64_NET_VIEW="${GUEST_VIEW:-$VIEW}")
[ -n "${HOST_REPLAY:-}" ] && host_env+=(MK64_NET_REPLAY="$HOST_REPLAY")
[ -n "${GUEST_REPLAY:-}" ] && guest_env+=(MK64_NET_REPLAY="$GUEST_REPLAY")
[ -n "${DRIFT:-}" ] && host_env+=(MK64_NET_BOT_DRIFT=1) && guest_env+=(MK64_NET_BOT_DRIFT=1)
if [ -n "${DUMP:-}" ]; then host_env+=(MK64_NET_DUMP="$LOGS/host-dump.bin"); guest_env+=(MK64_NET_DUMP="$LOGS/guest-dump.bin"); fi

echo "Logs: $LOGS"
env "${host_env[@]}" "$APP" -ApplePersistenceIgnoreState YES > "$LOGS/host-stdout.log" 2>&1 &
HPID=$!
sleep ${GUEST_LATE:-1}
env "${guest_env[@]}" "$APP" -ApplePersistenceIgnoreState YES > "$LOGS/guest-stdout.log" 2>&1 &
GPID=$!

# When one side quits, stop the other.
while kill -0 $HPID 2>/dev/null && kill -0 $GPID 2>/dev/null; do sleep 1; done
sleep 3
kill -TERM $HPID $GPID 2>/dev/null
wait $HPID 2>/dev/null; HRC=$?
wait $GPID 2>/dev/null; GRC=$?

echo "--- host";  grep -v '^#' "$LOGS/host.log" | tail -3
echo "--- guest"; grep -v '^#' "$LOGS/guest.log" | tail -3
if grep -q DESYNC "$LOGS/host.log" "$LOGS/guest.log"; then
    echo "RESULT: DESYNC"; exit 1
elif grep -q 'OK' "$LOGS/host.log" && grep -q 'OK' "$LOGS/guest.log"; then
    echo "RESULT: IN SYNC (exit codes $HRC/$GRC)"
else
    echo "RESULT: no sync checkpoints (did they connect?)"; exit 2
fi
