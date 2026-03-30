#!/bin/bash
# 拉流（默认无头，适合本机联调）：与 ./scripts/push.sh 共用 config/streams.conf 里的 SIGNALING_ADDR、STREAM_ID。
# 推流端：./scripts/push.sh
# 拉流端：./scripts/pull.sh   （默认无头、持续拉流至推流断开或 Ctrl+C）
# 有限帧测试：HEADLESS_FRAMES=30 HEADLESS_TIMEOUT=120 ./scripts/pull.sh
# 要窗口：./scripts/pull.sh --gui   或  HEADLESS=0 ./scripts/pull.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"

[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && {
    echo "Usage: ./scripts/pull.sh [--gui] [signaling_addr] [stream_id]"
    echo "默认：无头、持续拉流（--frames 0）；设 HEADLESS_FRAMES>=1 则收满帧或超时后退出"
    echo "环境: CONFIG_FILE  HEADLESS  HEADLESS_FRAMES  HEADLESS_TIMEOUT  WEBRTC_DEMO_BIN"
    exit 0
}

case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

BIN="${WEBRTC_DEMO_BIN:-$ROOT/build/bin}/webrtc_pull_demo"
[ -f "$BIN" ] || { echo "先执行 ./scripts/build.sh 或设置 WEBRTC_DEMO_BIN（并安装 libsdl2-dev）" >&2; exit 1; }

HEADLESS="${HEADLESS:-1}"
if [ "${1:-}" = "--gui" ] || [ "${1:-}" = "-w" ]; then
    HEADLESS=0
    shift
fi

SIGNALING="${1:-}"
STREAM="${2:-}"

CMD=("$BIN" "--config" "$CONFIG")
if [ "$HEADLESS" = "1" ] || [ "$HEADLESS" = "true" ] || [ "$HEADLESS" = "yes" ]; then
    # webrtc_pull_demo：--frames 0 表示不超时，直到断连或 Ctrl+C
    if [[ "${HEADLESS_FRAMES:-}" =~ ^[1-9][0-9]*$ ]]; then
        CMD+=("--headless" "--frames" "$HEADLESS_FRAMES" "--timeout-sec" "${HEADLESS_TIMEOUT:-120}")
    else
        CMD+=("--headless" "--frames" "0")
    fi
fi
[ -n "$SIGNALING" ] && CMD+=("$SIGNALING")
[ -n "$STREAM" ] && CMD+=("$STREAM")
exec "${CMD[@]}"
