#!/bin/bash
# 拉流脚本（本地/远端通用）
# 用法:
#   ./scripts/pull.sh [signaling_addr]
# 示例:
#   ./scripts/pull.sh                      # 本地: 127.0.0.1:8765
#   ./scripts/pull.sh 192.168.1.10:8765    # 远端

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SIGNALING_ADDR="${1:-127.0.0.1:8765}"

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

echo "[pull] signaling=${SIGNALING_ADDR}"
exec "$PLAYER_BIN" "$SIGNALING_ADDR"
