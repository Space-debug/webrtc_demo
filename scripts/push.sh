#!/bin/bash
# 一键推流脚本（按功能分区组织）
# - 支持按 stream_id 推流
# - 可选自动拉起 signaling_server
# - 与 config/streams.conf、README 的术语保持一致

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# [A] 帮助信息
print_usage() {
    cat <<'EOF'
用法:
  ./scripts/push.sh [stream_id] [camera]

参数:
  stream_id   流 ID（默认取 config 的 DEFAULT_STREAM）
  camera      摄像头路径或索引（默认取 config 的 DEFAULT_CAMERA）

环境变量（按功能分区）:
  [配置文件]
    CONFIG_FILE=./config/streams.conf

  [连接与会话]
    SIGNALING_ADDR=...       # 设置时才覆盖 config
    START_SIGNALING=...      # 未设置时取 config
    AUTO_LOCAL_ROUTE=...     # 未设置时取 config

  [采样参数]
    WIDTH=...
    HEIGHT=...
    FPS=...                  # 设置时才覆盖 config

说明:
  1) 默认完全按 config/streams.conf 运行。
  2) 仅当你显式传入参数/环境变量时，才覆盖配置文件。
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    print_usage
    exit 0
fi

# [B] 配置文件与读取函数
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

# [C] 输入参数（为空则走配置）
STREAM_ID="${1:-}"
CAMERA="${2:-}"

# [D] 连接与会话（优先环境变量，其次配置）
SIGNALING_ADDR_CFG="$(cfg_get SIGNALING_ADDR 127.0.0.1:8765)"
SIGNALING_ADDR_EFFECTIVE="${SIGNALING_ADDR:-$SIGNALING_ADDR_CFG}"
START_SIGNALING="${START_SIGNALING:-$(cfg_get START_SIGNALING 1)}"
AUTO_LOCAL_ROUTE="${AUTO_LOCAL_ROUTE:-$(cfg_get AUTO_LOCAL_ROUTE 1)}"

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

# [E] 信令服务（可选自动启动）
signaling_port="${SIGNALING_ADDR_EFFECTIVE##*:}"
if ! [[ "$signaling_port" =~ ^[0-9]+$ ]]; then
    signaling_port="8765"
fi

if [ "$START_SIGNALING" = "1" ]; then
    pkill -f "signaling_server ${signaling_port}" 2>/dev/null || true
    pkill -f "signaling_server" 2>/dev/null || true
    sleep 1

    echo "[push] 启动信令服务器: 0.0.0.0:${signaling_port}"
    "$SIG_BIN" "$signaling_port" &
    SIGNALING_PID=$!
    sleep 1
    if ! kill -0 "$SIGNALING_PID" 2>/dev/null; then
        echo "错误: 信令服务器启动失败（端口 ${signaling_port} 可能被占用）" >&2
        exit 1
    fi
fi

# [F] 本机回环路由（仅 localhost 信令场景）
if [ "$AUTO_LOCAL_ROUTE" = "1" ]; then
    host="${SIGNALING_ADDR_EFFECTIVE%%:*}"
    if [ "$host" = "127.0.0.1" ] || [ "$host" = "localhost" ]; then
        local_ip="$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}' | grep -v '^127\.' || true)"
        if [ -n "$local_ip" ] && ! ip route show | grep -qE "$local_ip(/32)? dev lo"; then
            sudo ip route add "$local_ip/32" dev lo 2>/dev/null || true
        fi
    fi
fi

# [G] 启动推流
CMD=("$PUSH_BIN" "--config" "$CONFIG_FILE")

# 仅显式设置时覆盖 config
if [ -n "${SIGNALING_ADDR+x}" ]; then
    CMD+=("--signaling" "$SIGNALING_ADDR_EFFECTIVE")
fi
if [ -n "${WIDTH+x}" ]; then
    CMD+=("--width" "$WIDTH")
fi
if [ -n "${HEIGHT+x}" ]; then
    CMD+=("--height" "$HEIGHT")
fi
if [ -n "${FPS+x}" ]; then
    CMD+=("--fps" "$FPS")
fi
if [ -n "$STREAM_ID" ]; then
    CMD+=("$STREAM_ID")
fi
if [ -n "$CAMERA" ]; then
    CMD+=("$CAMERA")
fi

echo "[push] config=${CONFIG_FILE} signaling=${SIGNALING_ADDR_EFFECTIVE} stream=${STREAM_ID:-<from-config>} camera=${CAMERA:-<from-config>}"
exec "${CMD[@]}"
