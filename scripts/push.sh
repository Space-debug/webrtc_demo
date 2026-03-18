#!/bin/bash
# 一键推流（可同时启动信令）
# 用法:
#   ./scripts/push.sh [stream_id] [camera]
# 环境变量（可选）:
#   SIGNALING_ADDR=127.0.0.1:8765
#   START_SIGNALING=1      # 1=自动启动 signaling_server（默认）
#   AUTO_LOCAL_ROUTE=1     # 本地信令时自动加 lo 路由（默认）
#   WIDTH=640 HEIGHT=480 FPS=30

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

STREAM_ID="${1:-livestream}"
CAMERA="${2:-/dev/video11}"
SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8765}"
START_SIGNALING="${START_SIGNALING:-1}"
AUTO_LOCAL_ROUTE="${AUTO_LOCAL_ROUTE:-1}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
FPS="${FPS:-30}"

case "$(uname -m)" in
    aarch64|arm64) ARCH="arm64" ;;
    x86_64|amd64) ARCH="x64" ;;
    *) ARCH="arm64" ;;
esac

LIB_PATH="${PROJECT_ROOT}/3rdparty/libwebrtc/lib/linux/${ARCH}"
export LD_LIBRARY_PATH="${LIB_PATH}:${LD_LIBRARY_PATH:-}"

PUSH_BIN="${PROJECT_ROOT}/build/bin/webrtc_push_demo"
SIG_BIN="${PROJECT_ROOT}/build/bin/signaling_server"

if [ ! -f "$PUSH_BIN" ] || [ ! -f "$SIG_BIN" ]; then
    echo "错误: 请先执行 ./scripts/build.sh 构建项目" >&2
    exit 1
fi

SIGNALING_PID=""
cleanup() {
    if [ -n "${SIGNALING_PID}" ] && kill -0 "${SIGNALING_PID}" 2>/dev/null; then
        kill "${SIGNALING_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "$START_SIGNALING" = "1" ]; then
    pkill -f "signaling_server 8765" 2>/dev/null || true
    pkill -f "signaling_server" 2>/dev/null || true
    sleep 1

    echo "[push] 启动信令服务器: 0.0.0.0:8765"
    "$SIG_BIN" 8765 &
    SIGNALING_PID=$!
    sleep 1
    if ! kill -0 "$SIGNALING_PID" 2>/dev/null; then
        echo "错误: 信令服务器启动失败（端口 8765 可能被占用）" >&2
        exit 1
    fi
fi

if [ "$AUTO_LOCAL_ROUTE" = "1" ]; then
    host="${SIGNALING_ADDR%%:*}"
    if [ "$host" = "127.0.0.1" ] || [ "$host" = "localhost" ]; then
        local_ip="$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}' | grep -v '^127\.' || true)"
        if [ -n "$local_ip" ] && ! ip route show | grep -qE "$local_ip(/32)? dev lo"; then
            sudo ip route add "$local_ip/32" dev lo 2>/dev/null || true
        fi
    fi
fi

echo "[push] stream=${STREAM_ID}, camera=${CAMERA}, signaling=${SIGNALING_ADDR}"
exec "$PUSH_BIN" \
    --signaling "$SIGNALING_ADDR" \
    --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" \
    "$STREAM_ID" \
    "$CAMERA"
