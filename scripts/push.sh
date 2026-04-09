#!/bin/bash
# 推流：读 config/streams.conf，可选自动起信令、本机回环路由。
# 同机延迟：另开终端 ./scripts/pull.sh；一键 E2E 见 ./scripts/pull.sh --e2e
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
    echo ""
    echo "环境: CONFIG_FILE  SIGNALING_ADDR  START_SIGNALING=0|1  AUTO_LOCAL_ROUTE  WIDTH HEIGHT FPS"
    echo "可选低延迟默认: PUSH_LOWLATENCY_DEFAULTS=1 时若未设置则补全:"
    echo "  WEBRTC_V4L2_BUFFER_COUNT=2  WEBRTC_V4L2_POLL_TIMEOUT_MS=10  WEBRTC_MJPEG_DEC_LOW_LATENCY=1"
    echo ""
    echo "手动两终端测端到端延迟:"
    echo "  终端1: export WEBRTC_E2E_LATENCY_TRACE=1; ./scripts/push.sh ...  （日志可重定向到 build/e2e_last_push.log）"
    echo "  终端2: export WEBRTC_E2E_LATENCY_TRACE=1; HEADLESS_FRAMES=400 ./scripts/pull.sh ... > build/e2e_last_pull.log"
    echo "  python3 tools/parse_e2e_latency.py build/e2e_last_push.log build/e2e_last_pull.log"
    echo "一键同机 E2E: ./scripts/pull.sh --e2e [/dev/video11]"
    exit 0
}

STREAM_ID="${1:-}"
CAMERA="${2:-}"
SIG_ADDR="${SIGNALING_ADDR:-$(cfg_get SIGNALING_ADDR 127.0.0.1:8765)}"
START_SIGNALING="${START_SIGNALING:-$(cfg_get START_SIGNALING 1)}"
AUTO_LOCAL_ROUTE="${AUTO_LOCAL_ROUTE:-$(cfg_get AUTO_LOCAL_ROUTE 1)}"

case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

if [ "${PUSH_LOWLATENCY_DEFAULTS:-0}" = "1" ]; then
    export WEBRTC_V4L2_BUFFER_COUNT="${WEBRTC_V4L2_BUFFER_COUNT:-2}"
    export WEBRTC_V4L2_POLL_TIMEOUT_MS="${WEBRTC_V4L2_POLL_TIMEOUT_MS:-10}"
    export WEBRTC_MJPEG_DEC_LOW_LATENCY="${WEBRTC_MJPEG_DEC_LOW_LATENCY:-1}"
fi

BIN_DIR="${WEBRTC_DEMO_BIN:-$ROOT/build/bin}"
PUSH="$BIN_DIR/webrtc_push_demo"
SIG="$BIN_DIR/signaling_server"
[ -f "$PUSH" ] && [ -f "$SIG" ] || {
    echo "先执行: ./scripts/build.sh 或设置 WEBRTC_DEMO_BIN" >&2
    exit 1
}

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
