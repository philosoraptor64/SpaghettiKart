#!/bin/bash
# Online screen test on this Mac: a local room server, then two game copies
# that press Host / Join on the Online screen by themselves (MK64_NET_UITEST),
# at different times, so they start the session from different places in the
# game (the host from the menus, the guest from the attract demo by default).
# Explorer bots play the session; both quit after FRAMES session ticks.
#
#   HOST_AT=100 GUEST_AT=700 FRAMES=4000 ./run-kart-ui-test.sh
#   ROUNDS=2 LEAVE=1200: the host leaves each session 1200 ticks in, then both
#   connect again (the last session ends at FRAMES)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
for repo in "$HERE/../.." "$HERE/../SpaghettiKart"; do
    [ -d "$repo/build-cmake" ] && break
done
APP="$repo/build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart"
STAMP=$(date +%Y%m%d-%H%M%S)
LOGS="$(cd "$repo/.." && pwd)/scripts/logs/kart-ui-$STAMP"
mkdir -p "$LOGS"
PORT=7790
python3 "$repo/n64net/server/rendezvous.py" --port $PORT > "$LOGS/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true' EXIT
sleep 1
common=(MK64_NET_UITEST_SERVER=127.0.0.1:$PORT MK64_NET_UITEST_ROOM=UITS MK64_NET_FRAMES="${FRAMES:-4000}"
        MK64_NET_BOT=1 MK64_NET_BOT_EXPLORE="${SEED:-21}" MK64_NET_UITEST_ROUNDS="${ROUNDS:-1}")
host_extra=()
[ -n "${LEAVE:-}" ] && host_extra+=(MK64_NET_UITEST_LEAVE="$LEAVE")
env "${common[@]}" ${host_extra[@]+"${host_extra[@]}"} MK64_NET_UITEST=host MK64_NET_UITEST_AT="${HOST_AT:-100}" MK64_NET_LOG="$LOGS/host.log" \
    "$APP" -ApplePersistenceIgnoreState YES > "$LOGS/host-stdout.log" 2>&1 &
H=$!
env "${common[@]}" MK64_NET_UITEST=join MK64_NET_UITEST_AT="${GUEST_AT:-700}" MK64_NET_LOG="$LOGS/guest.log" \
    "$APP" -ApplePersistenceIgnoreState YES > "$LOGS/guest-stdout.log" 2>&1 &
G=$!
wait $H || true
wait $G || true
echo "logs: $LOGS"
for s in host guest; do
    echo "--- $s"
    grep -h "UI test\|room UITS\|session started\|race starts\|DESYNC\|done after\|session over\|left" "$LOGS/$s-stdout.log" | cut -c1-160 | head -12
done
if grep -q DESYNC "$LOGS/host-stdout.log" "$LOGS/guest-stdout.log"; then echo "RESULT: DESYNC"; exit 1; fi
grep -q "done after" "$LOGS/host-stdout.log" && grep -q "done after" "$LOGS/guest-stdout.log" && echo "RESULT: IN SYNC" || echo "RESULT: INCOMPLETE"
