#!/bin/bash
# 一键拉流脚本（按功能分区组织）
# - 支持本地/远端信令地址
# - 支持多 stream_id
# - 支持无头模式（SSH/远程仅打印收帧日志）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# [A] 帮助信息
print_usage() {
    cat <<'EOF'
用法:
  ./scripts/pull.sh [signaling_addr] [stream_id]

参数:
  signaling_addr   信令地址（默认取 config）
  stream_id        流 ID（默认取 config）

环境变量（按功能分区）:
  [配置文件]
    CONFIG_FILE=./config/streams.conf

  [无头拉流]
    HEADLESS=0             1=无头模式，仅打印收帧日志
    HEADLESS_FRAMES=30     无头模式最少收帧数（达到后成功退出）
    HEADLESS_TIMEOUT=120   无头模式超时秒数

示例:
  ./scripts/pull.sh
  ./scripts/pull.sh 192.168.1.10:8765
  ./scripts/pull.sh 127.0.0.1:8765 cam2
  HEADLESS=1 HEADLESS_FRAMES=20 ./scripts/pull.sh 192.168.1.10:8765 livestream
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    print_usage
    exit 0
fi

# [B] 基础参数
CONFIG_FILE="${CONFIG_FILE:-${PROJECT_ROOT}/config/streams.conf}"
SIGNALING_ADDR="${1:-}"
STREAM_ID="${2:-}"

# [C] 无头拉流参数
HEADLESS="${HEADLESS:-0}"
HEADLESS_FRAMES="${HEADLESS_FRAMES:-30}"
HEADLESS_TIMEOUT="${HEADLESS_TIMEOUT:-120}"

case "$(uname -m)" in
    aarch64|arm64) ARCH="arm64" ;;
    x86_64|amd64) ARCH="x64" ;;
    *) ARCH="arm64" ;;
esac

LIB_PATH="${PROJECT_ROOT}/3rdparty/libwebrtc/lib/linux/${ARCH}"
export LD_LIBRARY_PATH="${LIB_PATH}:${LD_LIBRARY_PATH:-}"

PLAYER_BIN="${PROJECT_ROOT}/build/bin/p2p_player"
if [ ! -f "$PLAYER_BIN" ]; then
    echo "错误: 请先执行 ./scripts/build.sh 构建项目" >&2
    exit 1
fi

# [D] 启动拉流
CMD=("$PLAYER_BIN" "--config" "$CONFIG_FILE")
echo "[pull] config=${CONFIG_FILE} signaling=${SIGNALING_ADDR:-<from-config>} stream=${STREAM_ID:-<from-config>} headless=${HEADLESS}"
if [ "$HEADLESS" = "1" ]; then
    CMD+=("--headless" "--frames" "$HEADLESS_FRAMES" "--timeout-sec" "$HEADLESS_TIMEOUT")
fi
if [ -n "$SIGNALING_ADDR" ]; then
    CMD+=("$SIGNALING_ADDR")
fi
if [ -n "$STREAM_ID" ]; then
    CMD+=("$STREAM_ID")
fi
exec "${CMD[@]}"
