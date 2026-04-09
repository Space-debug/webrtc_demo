#!/bin/bash
# 拉流：与 ./scripts/push.sh 共用 config/streams.conf（SIGNALING_ADDR、STREAM_ID 等）。
# 同机端到端延迟：./scripts/pull.sh --e2e [camera]（自启信令、后台推流、无头拉流、解析日志）
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"

e2e_cfg_get() {
    local key="$1" def="$2"
    [ ! -f "$CONFIG" ] && echo "$def" && return
    local line
    line="$(grep -E "^[[:space:]]*${key}=" "$CONFIG" | tail -1)" || true
    [ -z "$line" ] && echo "$def" && return
    echo "${line#*=}" | sed 's/#.*//' | xargs
}

# 同板 E2E：推流进程 + 拉流进程 + tools/parse_e2e_latency.py
run_same_board_e2e() {
    local CAM="${1:-}"
    if [ -z "$CAM" ] && [ -n "${E2E_CAMERA:-}" ]; then
        CAM="$E2E_CAMERA"
    fi
    if [ -z "$CAM" ]; then
        local sid
        sid="$(e2e_cfg_get STREAM_ID livestream)"
        CAM="$(e2e_cfg_get "STREAM_${sid}_CAMERA" /dev/video0)"
    fi
    [ -z "$CAM" ] && CAM="/dev/video0"

    local CFG="$CONFIG"
    local SIG_ADDR PORT STREAM_ID FRAMES PULL_TO PUSH_READY_SEC
    SIG_ADDR="$(e2e_cfg_get SIGNALING_ADDR 127.0.0.1:8765)"
    STREAM_ID="$(e2e_cfg_get STREAM_ID livestream)"
    PORT="${SIG_ADDR##*:}"
    [[ "$PORT" =~ ^[0-9]+$ ]] || PORT=8765
    FRAMES="${E2E_FRAMES:-400}"
    PULL_TO="${E2E_PULL_TIMEOUT_SEC:-150}"
    PUSH_READY_SEC="${E2E_PUSH_READY_SEC:-30}"

    case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
    export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

    local BIN_DIR PUSH PULL SIG PY
    BIN_DIR="${WEBRTC_DEMO_BIN:-$ROOT/build/bin}"
    PUSH="$BIN_DIR/webrtc_push_demo"
    PULL="$BIN_DIR/webrtc_pull_demo"
    SIG="$BIN_DIR/signaling_server"
    PY="$ROOT/tools/parse_e2e_latency.py"

    for x in "$PUSH" "$PULL" "$SIG"; do
        [ -x "$x" ] || {
            echo "缺少可执行文件: $x（先 ./scripts/build.sh）" >&2
            return 1
        }
    done
    [ -f "$PY" ] || {
        echo "缺少 $PY" >&2
        return 1
    }
    [ -e "$CAM" ] || {
        echo "摄像头不存在: $CAM" >&2
        return 1
    }

    mkdir -p "$ROOT/build"
    local PUSH_LOG="$ROOT/build/e2e_last_push.log"
    local PULL_LOG="$ROOT/build/e2e_last_pull.log"
    local PUSH_PID="" SIG_PID=""
    cleanup() {
        [ -n "${PUSH_PID:-}" ] && kill "$PUSH_PID" 2>/dev/null || true
        [ -n "${SIG_PID:-}" ] && kill "$SIG_PID" 2>/dev/null || true
    }
    trap cleanup EXIT INT TERM

    echo "E2E CONFIG=$CFG  SIGNALING=$SIG_ADDR  STREAM=$STREAM_ID  CAM=$CAM"
    echo "E2E pull --frames $FRAMES timeout ${PULL_TO}s"
    if [ -n "${E2E_WIDTH:-}${E2E_HEIGHT:-}${E2E_FPS:-}" ]; then
        echo "E2E push overrides: WIDTH=${E2E_WIDTH:-} HEIGHT=${E2E_HEIGHT:-} FPS=${E2E_FPS:-}"
    fi

    pkill -f "signaling_server" 2>/dev/null || true
    sleep 1
    "$SIG" "$PORT" &
    SIG_PID=$!
    sleep 1

    local H="${SIG_ADDR%%:*}"
    if [ "$H" = "127.0.0.1" ] || [ "$H" = "localhost" ]; then
        local LIP
        LIP="$(ip -4 route get 1 2>/dev/null | awk '{print $7; exit}' | grep -v '^127\.' || true)"
        [ -n "$LIP" ] && ! ip route show | grep -qE "$LIP(/32)? dev lo" && sudo ip route add "$LIP/32" dev lo 2>/dev/null || true
    fi

    export WEBRTC_E2E_LATENCY_TRACE=1
    export START_SIGNALING=0
    export WEBRTC_PULL_JITTER_MIN_DELAY_MS="${WEBRTC_PULL_JITTER_MIN_DELAY_MS:-0}"
    export WEBRTC_MPP_H264_DEC_LOW_LATENCY="${WEBRTC_MPP_H264_DEC_LOW_LATENCY:-1}"

    local PUSH_EXTRA_ARGS=()
    [ -n "${E2E_WIDTH:-}" ] && PUSH_EXTRA_ARGS+=(--width "$E2E_WIDTH")
    [ -n "${E2E_HEIGHT:-}" ] && PUSH_EXTRA_ARGS+=(--height "$E2E_HEIGHT")
    [ -n "${E2E_FPS:-}" ] && PUSH_EXTRA_ARGS+=(--fps "$E2E_FPS")

    "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    PUSH_PID=$!

    wait_push_ready() {
        local deadline=$((SECONDS + PUSH_READY_SEC))
        while [ "$SECONDS" -lt "$deadline" ]; do
            if grep -qE 'CameraVideoTrackSource::Start failed|\[Main\] Start failed' "$PUSH_LOG" 2>/dev/null; then
                echo "推流启动失败（摄像头打不开或 Start 失败），见日志尾部。" >&2
                return 1
            fi
            if grep -q '\[PushStreamer\] Video capture started' "$PUSH_LOG" 2>/dev/null &&
                grep -q '\[Main\] Publisher started' "$PUSH_LOG" 2>/dev/null; then
                return 0
            fi
            if ! kill -0 "$PUSH_PID" 2>/dev/null; then
                echo "推流进程已退出。" >&2
                return 1
            fi
            sleep 0.4
        done
        echo "等待 ${PUSH_READY_SEC}s 仍未看到采集就绪（Video capture started + Publisher started）。" >&2
        return 1
    }

    if ! wait_push_ready; then
        kill "$PUSH_PID" 2>/dev/null || true
        wait "$PUSH_PID" 2>/dev/null || true
        PUSH_PID=""
        echo "--- push log (tail) ---"
        tail -45 "$PUSH_LOG"
        return 3
    fi

    sleep "${E2E_AFTER_PUSH_READY_SLEEP_SEC:-1}"

    local PULL_EXTRA=()
    if [ "${E2E_VIDEO_STATS:-0}" = "1" ]; then
        PULL_EXTRA+=(--video-stats-interval-sec 2)
    fi

    local PULL_RC
    set +e
    timeout "$((PULL_TO + 30))" "$PULL" --config "$CFG" --headless --frames "$FRAMES" --timeout-sec "$PULL_TO" \
        "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    PULL_RC=$?
    set -e

    kill "$PUSH_PID" 2>/dev/null || true
    wait "$PUSH_PID" 2>/dev/null || true
    PUSH_PID=""
    kill "$SIG_PID" 2>/dev/null || true
    SIG_PID=""
    trap - EXIT INT TERM

    echo ""
    echo "========== pull exit: $PULL_RC =========="
    if [ "$PULL_RC" != 0 ]; then
        echo "--- pull tail ---"
        tail -25 "$PULL_LOG"
        echo "--- push tail ---"
        tail -25 "$PUSH_LOG"
        return "$PULL_RC"
    fi

    echo "--- E2E parse ---"
    python3 "$PY" "$PUSH_LOG" "$PULL_LOG" || return $?

    cat <<'EOF'

--- 指标怎么读（改进方向）---
  e2e_mjpeg_input_to_sink / e2e_v4l2_to_sink
      整段「采集侧锚点 → 收端解码完成进 Sink」；变大时看下面分段。
  e2e_onframe_to_sink
      已进 WebRTC 发送链路的帧 → 收端 Sink；主要反映 编码+网络+JB+解码排队。
  rx_sink_to_argb_done
      Sink 后 I420→ARGB（pull 演示）；偏大则 CPU 色彩转换或线程调度是热点。
  拉流侧 WEBRTC 内部：可再跑 E2E_VIDEO_STATS=1，看 [VideoTiming]/jitter_buffer_*。
EOF
    echo ""
    echo "日志: $PUSH_LOG  $PULL_LOG"
    return 0
}

[ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] && {
    echo "Usage:"
    echo "  ./scripts/pull.sh [--gui] [signaling_addr] [stream_id]"
    echo "  ./scripts/pull.sh --e2e [camera]     # 同机一键 E2E 延迟（需 python3）"
    echo ""
    echo "普通拉流：默认无头；HEADLESS_FRAMES>=1 时收满帧或超时退出。"
    echo "E2E：环境变量 E2E_FRAMES、E2E_PULL_TIMEOUT_SEC、E2E_PUSH_READY_SEC、E2E_WIDTH/HEIGHT/FPS、"
    echo "      E2E_VIDEO_STATS=1、CONFIG_FILE、WEBRTC_DEMO_BIN 等。"
    exit 0
}

if [ "${1:-}" = "--e2e" ]; then
    shift
    run_same_board_e2e "${1:-}"
    exit $?
fi

case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"
export WEBRTC_PULL_JITTER_MIN_DELAY_MS="${WEBRTC_PULL_JITTER_MIN_DELAY_MS:-0}"
export WEBRTC_MPP_H264_DEC_LOW_LATENCY="${WEBRTC_MPP_H264_DEC_LOW_LATENCY:-1}"

BIN="${WEBRTC_DEMO_BIN:-$ROOT/build/bin}/webrtc_pull_demo"
[ -f "$BIN" ] || {
    echo "先执行 ./scripts/build.sh 或设置 WEBRTC_DEMO_BIN（并安装 libsdl2-dev）" >&2
    exit 1
}

HEADLESS="${HEADLESS:-1}"
if [ "${1:-}" = "--gui" ] || [ "${1:-}" = "-w" ]; then
    HEADLESS=0
    shift
fi

SIGNALING="${1:-}"
STREAM="${2:-}"

CMD=("$BIN" "--config" "$CONFIG")
if [ "$HEADLESS" = "1" ] || [ "$HEADLESS" = "true" ] || [ "$HEADLESS" = "yes" ]; then
    if [[ "${HEADLESS_FRAMES:-}" =~ ^[1-9][0-9]*$ ]]; then
        CMD+=("--headless" "--frames" "$HEADLESS_FRAMES" "--timeout-sec" "${HEADLESS_TIMEOUT:-120}")
    else
        CMD+=("--headless" "--frames" "0")
    fi
fi
# 手动测 E2E：两终端分别推/拉时 export WEBRTC_E2E_LATENCY_TRACE=1，再用:
#   python3 tools/parse_e2e_latency.py build/e2e_last_push.log build/e2e_last_pull.log
[ -n "$SIGNALING" ] && CMD+=("$SIGNALING")
[ -n "$STREAM" ] && CMD+=("$STREAM")
exec "${CMD[@]}"
