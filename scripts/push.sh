#!/bin/bash
# 推流：读 config/streams.conf，可选自动起信令、本机回环路由
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"

cfg_get() {
    local key="$1" def="$2"
    [ ! -f "$CONFIG" ] && echo "$def" && return
    local line
    line="$(grep -E "^[[:space:]]*${key}=" "$CONFIG" | tail -1)" || true
    [ -z "$line" ] && echo "$def" && return
    echo "${line#*=}" | sed 's/#.*//' | xargs
}

[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && {
    echo "Usage: ./scripts/push.sh [stream_id] [camera]"
    echo "环境: CONFIG_FILE  SIGNALING_ADDR  START_SIGNALING  AUTO_LOCAL_ROUTE  WIDTH HEIGHT FPS"
    exit 0
}

STREAM_ID="${1:-}"
CAMERA="${2:-}"
SIG_ADDR="${SIGNALING_ADDR:-$(cfg_get SIGNALING_ADDR 127.0.0.1:8765)}"
START_SIGNALING="${START_SIGNALING:-$(cfg_get START_SIGNALING 1)}"
AUTO_LOCAL_ROUTE="${AUTO_LOCAL_ROUTE:-$(cfg_get AUTO_LOCAL_ROUTE 1)}"

case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

PUSH="$ROOT/build/bin/webrtc_push_demo"
SIG="$ROOT/build/bin/signaling_server"
[ -f "$PUSH" ] && [ -f "$SIG" ] || { echo "先执行: ./scripts/build.sh" >&2; exit 1; }

PORT="${SIG_ADDR##*:}"
[[ "$PORT" =~ ^[0-9]+$ ]] || PORT=8765
SIG_PID=""
cleanup() { [ -n "$SIG_PID" ] && kill "$SIG_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

if [ "$START_SIGNALING" = "1" ]; then
    pkill -f "signaling_server" 2>/dev/null || true
    sleep 1
    "$SIG" "$PORT" &
    SIG_PID=$!
    sleep 1
fi

if [ "$AUTO_LOCAL_ROUTE" = "1" ]; then
    H="${SIG_ADDR%%:*}"
    if [ "$H" = "127.0.0.1" ] || [ "$H" = "localhost" ]; then
        LIP="$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}' | grep -v '^127\.' || true)"
        [ -n "$LIP" ] && ! ip route show | grep -qE "$LIP(/32)? dev lo" && sudo ip route add "$LIP/32" dev lo 2>/dev/null || true
    fi
fi

CMD=("$PUSH" "--config" "$CONFIG")
[ -n "${SIGNALING_ADDR+x}" ] && CMD+=("--signaling" "$SIG_ADDR")
[ -n "${WIDTH+x}" ] && CMD+=("--width" "$WIDTH")
[ -n "${HEIGHT+x}" ] && CMD+=("--height" "$HEIGHT")
[ -n "${FPS+x}" ] && CMD+=("--fps" "$FPS")
[ -n "$STREAM_ID" ] && CMD+=("$STREAM_ID")
[ -n "$CAMERA" ] && CMD+=("$CAMERA")
exec "${CMD[@]}"
