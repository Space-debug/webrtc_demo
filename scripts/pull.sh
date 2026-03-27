#!/bin/bash
# 拉流：可选 --config（与推流共用时可读 SIGNALING_ADDR / STREAM_ID）；无头用 HEADLESS=1 或 --headless
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"

[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && {
    echo "Usage: ./scripts/pull.sh [signaling_addr] [stream_id]"
    echo "环境: CONFIG_FILE  HEADLESS=1  HEADLESS_FRAMES  HEADLESS_TIMEOUT（传给可执行文件）"
    exit 0
}

case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

BIN="$ROOT/build/bin/webrtc_pull_demo"
[ -f "$BIN" ] || { echo "先执行 ./scripts/build.sh（并安装 libsdl2-dev 以生成拉流程序）" >&2; exit 1; }

SIGNALING="${1:-}"
STREAM="${2:-}"
HEADLESS="${HEADLESS:-0}"
FR="${HEADLESS_FRAMES:-30}"
TO="${HEADLESS_TIMEOUT:-120}"

CMD=("$BIN" "--config" "$CONFIG")
[ "$HEADLESS" = "1" ] && CMD+=("--headless" "--frames" "$FR" "--timeout-sec" "$TO")
[ -n "$SIGNALING" ] && CMD+=("$SIGNALING")
[ -n "$STREAM" ] && CMD+=("$STREAM")
exec "${CMD[@]}"
