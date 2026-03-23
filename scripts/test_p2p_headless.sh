#!/bin/bash
# 本机无头验证：信令 + 推流 + p2p_player --headless（通过帧计数判定成功）
# 默认尽量按 config/streams.conf 运行，参数仅在显式传入时覆盖
# 用法: ./scripts/test_p2p_headless.sh [摄像头设备]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

CONFIG_FILE="${CONFIG_FILE:-${PROJECT_ROOT}/config/streams.conf}"
cfg_get() {
    local key="$1"
    local def="$2"
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "$def"
        return
    fi
    local v
    v="$(
        awk -F= -v key="$key" '
            function trim(s){ gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", s); return s }
            {
                line=$0
                sub(/#.*/, "", line)
                if (index(line, "=")==0) next
                k=trim(substr(line, 1, index(line, "=")-1))
                val=trim(substr(line, index(line, "=")+1))
                if (k==key) last=val
            }
            END { if (last!="") print last }
        ' "$CONFIG_FILE"
    )"
    if [ -n "$v" ]; then
        echo "$v"
    else
        echo "$def"
    fi
}

STREAM_ID="${STREAM_ID:-$(cfg_get DEFAULT_STREAM livestream)}"
CAMERA="${1:-$(cfg_get DEFAULT_CAMERA /dev/video11)}"
# 避免与已占用 8765 的信令冲突
PORT="${P2P_TEST_PORT:-18765}"

case "$(uname -m)" in
    aarch64|arm64) ARCH="arm64" ;;
    x86_64|amd64) ARCH="x64" ;;
    *) ARCH="arm64" ;;
esac

export LD_LIBRARY_PATH="${PROJECT_ROOT}/3rdparty/libwebrtc/lib/linux/${ARCH}:${LD_LIBRARY_PATH:-}"

SIG="${PROJECT_ROOT}/build/bin/signaling_server"
PUSH="${PROJECT_ROOT}/build/bin/webrtc_push_demo"
PULL="${PROJECT_ROOT}/build/bin/p2p_player"

for f in "$SIG" "$PUSH" "$PULL"; do
    if [ ! -x "$f" ]; then
        echo "请先构建: ./scripts/build.sh" >&2
        exit 1
    fi
done

PUSH_LOG=""
cleanup() {
    rm -f "${PUSH_LOG:-}"
    if [ -n "${PUSH_PID:-}" ] && kill -0 "$PUSH_PID" 2>/dev/null; then
        kill "$PUSH_PID" 2>/dev/null || true
        wait "$PUSH_PID" 2>/dev/null || true
    fi
    if [ -n "${SIG_PID:-}" ] && kill -0 "$SIG_PID" 2>/dev/null; then
        kill "$SIG_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "[test] 启动信令 :$PORT"
"$SIG" "$PORT" &
SIG_PID=$!
sleep 0.5

PUSH_LOG=$(mktemp)

echo "[test] 启动推流 camera=$CAMERA stream=$STREAM_ID（日志: $PUSH_LOG）"
"$PUSH" --config "$CONFIG_FILE" --signaling "127.0.0.1:$PORT" \
    "$STREAM_ID" "$CAMERA" >"$PUSH_LOG" 2>&1 &
PUSH_PID=$!

# 等推流完成初始化（含摄像头预热），避免拉流端过早协商导致 0 帧
echo "[test] 等待推流端打印「推流器已启动」..."
READY=0
for _ in $(seq 1 90); do
    if grep -q "推流器已启动" "$PUSH_LOG" 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "$PUSH_PID" 2>/dev/null; then
        echo "[test] 推流进程已退出，日志:" >&2
        tail -80 "$PUSH_LOG" >&2
        exit 1
    fi
    sleep 1
done
if [ "$READY" != 1 ]; then
    echo "[test] 超时未就绪，推流日志:" >&2
    tail -80 "$PUSH_LOG" >&2
    exit 1
fi
sleep 1

echo "[test] 无头拉流（需收到至少 20 帧）"
"$PULL" --config "$CONFIG_FILE" --headless --frames 20 --timeout-sec 90 "127.0.0.1:$PORT" "$STREAM_ID"
RC=$?
echo "[test] p2p_player 退出码: $RC"
exit "$RC"
