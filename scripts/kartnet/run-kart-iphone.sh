#!/bin/bash
# KartNet netplay: this Mac hosts (player 1), the USB-connected iPhone joins
# (player 2) over Wi-Fi. Writes the phone's Documents/netplay.cfg, launches
# both, then pulls the phone's log and compares sync checkpoints.
#
# Options (environment):
#   FRAMES=n      quit after n netplay frames (default 0 = until you close the Mac window)
#   MAC_BOT=seed  scripted driver on the Mac (default: Mac controller/keyboard)
#   ROLLBACK=1    rollback netcode (n64net) instead of lockstep; WINDOW=n (8)
#   PHONE_BOT=seed scripted driver on the phone (default: phone's controller)
#   DELAY=n       input delay in frames (default 2)
#   MODE, CUP, COURSE, CC, SEED, MAC_CHAR, PHONE_CHAR
#   DEVICE=udid   phone (default: the first connected iPhone)
set -u
SK="$(cd "$(dirname "$0")/../.." && pwd)"; ROOT="${KARTNET_LOGS_ROOT:-$SK/..}"
APP="$SK/build-cmake/SpaghettiKart.app/Contents/MacOS/SpaghettiKart"
BID=io.github.philosoraptor.spaghettikart
STAMP=$(date +%Y%m%d-%H%M%S)
LOGS="$ROOT/scripts/logs/kart-iphone-$STAMP"
mkdir -p "$LOGS"
FRAMES=${FRAMES:-0}
DELAY=${DELAY:-2}
MODE=${MODE:-gp}

DEVICE=${DEVICE:-$(xcrun devicectl list devices 2>/dev/null | awk '(/connected/ || /available/) && /physical/ && /iPhone/ {for(i=1;i<=NF;i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) print $i}' | head -1)}
[ -z "$DEVICE" ] && { echo "No connected iPhone found (plug it in and unlock it)."; exit 1; }
MAC_IP=$(ipconfig getifaddr en0 || ipconfig getifaddr en1)
[ -z "$MAC_IP" ] && { echo "This Mac has no Wi-Fi/Ethernet IP."; exit 1; }
echo "Mac $MAC_IP hosts; iPhone $DEVICE joins. Logs: $LOGS"

cfg="$LOGS/netplay.cfg"
{
    echo "MK64_NET=1"
    echo "MK64_NET_PEER=$MAC_IP:7101"
    echo "MK64_NET_LOG=kartnet.log"
    [ -n "${DUMP:-}" ] && echo "MK64_NET_DUMP=kartnet-dump.bin"
    echo "MK64_NET_FRAMES=$FRAMES"
    [ -n "${PHONE_BOT:-}" ] && echo "MK64_NET_BOT=$PHONE_BOT"
    [ -n "${PHONE_CHAR:-}" ] && echo "MK64_NET_CHAR=$PHONE_CHAR"
    [ -n "${ROLLBACK:-}" ] && echo "MK64_NET_ROLLBACK=$ROLLBACK"
    [ -n "${WINDOW:-}" ] && echo "MK64_NET_ROLLBACK_WINDOW=$WINDOW"
} > "$cfg"
copy_to_phone() {
    xcrun devicectl device copy to --device "$DEVICE" --domain-type appDataContainer \
        --domain-identifier $BID --source "$1" --destination "Documents/$2" > /dev/null 2>&1
}
copy_to_phone "$cfg" netplay.cfg || { echo "Could not write netplay.cfg to the phone (is the app installed?)."; exit 1; }

host_env=(MK64_NET=1 MK64_NET_HOST=1 MK64_NET_BIND=0.0.0.0:7101 MK64_NET_DELAY="$DELAY" MK64_NET_MODE="$MODE"
    MK64_NET_FRAMES="$FRAMES" MK64_NET_LOG="$LOGS/mac.log")
[ -n "${DUMP:-}" ] && host_env+=(MK64_NET_DUMP="$LOGS/mac-dump.bin")
[ -n "${MAC_BOT:-}" ] && host_env+=(MK64_NET_BOT="$MAC_BOT")
[ -n "${ROLLBACK:-}" ] && host_env+=(MK64_NET_ROLLBACK="$ROLLBACK")
[ -n "${WINDOW:-}" ] && host_env+=(MK64_NET_ROLLBACK_WINDOW="$WINDOW")
[ -n "${MAC_CHAR:-}" ] && host_env+=(MK64_NET_CHAR="$MAC_CHAR")
[ -n "${CUP:-}" ] && host_env+=(MK64_NET_CUP="$CUP")
[ -n "${COURSE:-}" ] && host_env+=(MK64_NET_COURSE="$COURSE")
[ -n "${CC:-}" ] && host_env+=(MK64_NET_CC="$CC")
[ -n "${SEED:-}" ] && host_env+=(MK64_NET_SEED="$SEED")

env "${host_env[@]}" "$APP" -ApplePersistenceIgnoreState YES > "$LOGS/mac-stdout.log" 2>&1 &
MPID=$!
xcrun devicectl device process launch --device "$DEVICE" --terminate-existing $BID > /dev/null 2>&1 \
    || echo "Could not launch the app on the phone."

wait $MPID
sleep 3
# Stop the phone app (it quits by itself when FRAMES is set) and fetch its log.
PID=$(xcrun devicectl device info processes --device "$DEVICE" 2>/dev/null | awk '/Spaghettify.app/ {print $1}')
[ -n "$PID" ] && xcrun devicectl device process signal --device "$DEVICE" --pid "$PID" --signal SIGTERM > /dev/null 2>&1
xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier $BID \
    --source Documents/kartnet.log --destination "$LOGS/phone.log" > /dev/null 2>&1
[ -n "${DUMP:-}" ] && xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier $BID \
    --source Documents/kartnet-dump.bin --destination "$LOGS/phone-dump.bin" > /dev/null 2>&1
# Back to normal single-player launches.
echo "MK64_NET=0" > "$LOGS/netplay-off.cfg"
copy_to_phone "$LOGS/netplay-off.cfg" netplay.cfg

echo "--- mac";   grep -v '^#' "$LOGS/mac.log" 2>/dev/null | tail -3
echo "--- phone"; grep -v '^#' "$LOGS/phone.log" 2>/dev/null | tail -3
if grep -q DESYNC "$LOGS/mac.log" "$LOGS/phone.log" 2>/dev/null; then
    echo "RESULT: DESYNC"; exit 1
elif grep -q OK "$LOGS/mac.log" 2>/dev/null && grep -q OK "$LOGS/phone.log" 2>/dev/null; then
    echo "RESULT: IN SYNC"
else
    echo "RESULT: no sync checkpoints (did they connect?)"; exit 2
fi
