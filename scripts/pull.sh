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
    # 一键低时延 profile（默认 aggressive）：可用环境变量覆盖每一项。
    # 关闭方式：E2E_LOWLAT_PROFILE=off ./scripts/pull.sh --e2e
    #   aggressive: 极限低延迟（更激进，尾部可能更抖）
    #   balanced:   稍增缓冲换稳定
    #   stable:     低延迟优先下进一步压尾巴（推荐做稳定性回归）
    local E2E_LOWLAT_PROFILE="${E2E_LOWLAT_PROFILE:-aggressive}"
    case "$E2E_LOWLAT_PROFILE" in
        aggressive)
            export WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS="${WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS:-0}"
            export WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE="${WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE:-4}"
            export WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS="${WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS:-1}"
            ;;
        balanced)
            export WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS="${WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS:-0}"
            export WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE="${WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE:-5}"
            export WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS="${WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS:-2}"
            ;;
        stable)
            export WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS="${WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS:-1}"
            export WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE="${WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE:-6}"
            export WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS="${WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS:-1}"
            ;;
        off|none)
            ;;
        *)
            echo "Unknown E2E_LOWLAT_PROFILE=$E2E_LOWLAT_PROFILE (use aggressive|balanced|stable|off)" >&2
            return 2
            ;;
    esac
    echo "E2E lowlat profile: ${E2E_LOWLAT_PROFILE}  pacing_ms=${WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS:-default}  decode_queue=${WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE:-default}"
    # 推流端 profile（与 push.sh 对齐）：默认 aggressive；也可手动覆盖参数。
    local PUSH_LOWLATENCY_PROFILE="${PUSH_LOWLATENCY_PROFILE:-aggressive}"
    if [ "${PUSH_LOWLATENCY_DEFAULTS:-0}" = "1" ] && [ "$PUSH_LOWLATENCY_PROFILE" = "off" ]; then
        PUSH_LOWLATENCY_PROFILE="aggressive"
    fi
    case "$PUSH_LOWLATENCY_PROFILE" in
        aggressive)
            export WEBRTC_V4L2_BUFFER_COUNT="${WEBRTC_V4L2_BUFFER_COUNT:-2}"
            export WEBRTC_V4L2_POLL_TIMEOUT_MS="${WEBRTC_V4L2_POLL_TIMEOUT_MS:-5}"
            export WEBRTC_MJPEG_DEC_LOW_LATENCY="${WEBRTC_MJPEG_DEC_LOW_LATENCY:-1}"
            ;;
        balanced)
            export WEBRTC_V4L2_BUFFER_COUNT="${WEBRTC_V4L2_BUFFER_COUNT:-3}"
            export WEBRTC_V4L2_POLL_TIMEOUT_MS="${WEBRTC_V4L2_POLL_TIMEOUT_MS:-8}"
            export WEBRTC_MJPEG_DEC_LOW_LATENCY="${WEBRTC_MJPEG_DEC_LOW_LATENCY:-1}"
            ;;
        stable)
            export WEBRTC_V4L2_BUFFER_COUNT="${WEBRTC_V4L2_BUFFER_COUNT:-3}"
            export WEBRTC_V4L2_POLL_TIMEOUT_MS="${WEBRTC_V4L2_POLL_TIMEOUT_MS:-5}"
            export WEBRTC_MJPEG_DEC_LOW_LATENCY="${WEBRTC_MJPEG_DEC_LOW_LATENCY:-1}"
            ;;
        off|none)
            ;;
        *)
            echo "Unknown PUSH_LOWLATENCY_PROFILE=$PUSH_LOWLATENCY_PROFILE (use aggressive|balanced|stable|off)" >&2
            return 2
            ;;
    esac
    local EXP_SIG="E2E-SIGNATURE pull_profile=${E2E_LOWLAT_PROFILE} push_profile=${PUSH_LOWLATENCY_PROFILE} "
    EXP_SIG+="pull_jitter_min_ms=${WEBRTC_PULL_JITTER_MIN_DELAY_MS:-default} "
    EXP_SIG+="pull_decode_queue=${WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE:-default} "
    EXP_SIG+="pull_pacing_ms=${WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS:-default} "
    EXP_SIG+="pull_dec_poll_ms=${WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS:-default} "
    EXP_SIG+="push_v4l2_buf=${WEBRTC_V4L2_BUFFER_COUNT:-default} "
    EXP_SIG+="push_poll_timeout_ms=${WEBRTC_V4L2_POLL_TIMEOUT_MS:-default} "
    EXP_SIG+="push_mjpeg_lowlat=${WEBRTC_MJPEG_DEC_LOW_LATENCY:-default} "
    EXP_SIG+="mpp_h264_dec_lowlat=${WEBRTC_MPP_H264_DEC_LOW_LATENCY:-default} "
    EXP_SIG+="frames=${FRAMES} timeout_s=${PULL_TO}"
    local E2E_SKIP_FIRST_PAIRS="${E2E_SKIP_FIRST_PAIRS:-30}"
    EXP_SIG+=" skip_first_pairs=${E2E_SKIP_FIRST_PAIRS}"
    local E2E_CPU_PIN_MODE="${E2E_CPU_PIN_MODE:-auto}"
    local USE_TASKSET=0 PUSH_CORE="" PULL_CORE=""
    if [ "$E2E_CPU_PIN_MODE" != "off" ] && command -v taskset >/dev/null 2>&1; then
        local NCPU
        NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
        if [ "$NCPU" -ge 4 ]; then
            USE_TASKSET=1
            if [ -n "${E2E_PUSH_CORE:-}" ] && [ -n "${E2E_PULL_CORE:-}" ]; then
                PUSH_CORE="$E2E_PUSH_CORE"
                PULL_CORE="$E2E_PULL_CORE"
            else
                PUSH_CORE="$((NCPU - 2))"
                PULL_CORE="$((NCPU - 1))"
            fi
        fi
    fi
    EXP_SIG+=" cpu_pin_mode=${E2E_CPU_PIN_MODE}"
    EXP_SIG+=" push_core=${PUSH_CORE:-na} pull_core=${PULL_CORE:-na}"
    local E2E_SCHED_MODE="${E2E_SCHED_MODE:-auto}"
    local E2E_SCHED_PRIO="${E2E_SCHED_PRIO:-20}"
    local USE_CHRT=0
    local USE_NICE=0
    if [ "$E2E_SCHED_MODE" != "off" ] && command -v chrt >/dev/null 2>&1; then
        if [ "$E2E_SCHED_MODE" = "rr" ] || [ "$E2E_SCHED_MODE" = "auto" ]; then
            if chrt -f "$E2E_SCHED_PRIO" true >/dev/null 2>&1; then
                USE_CHRT=1
            elif [ "$E2E_SCHED_MODE" = "rr" ]; then
                echo "E2E_SCHED_MODE=rr but chrt realtime scheduling unavailable (need CAP_SYS_NICE/root)." >&2
                return 2
            fi
        fi
    fi
    if [ "$USE_CHRT" = "0" ] && [ "$E2E_SCHED_MODE" = "auto" ]; then
        USE_NICE=1
    fi
    EXP_SIG+=" sched_mode=${E2E_SCHED_MODE} sched_prio=${E2E_SCHED_PRIO} use_chrt=${USE_CHRT}"
    EXP_SIG+=" use_nice=${USE_NICE}"
    local PUSH_EXTRA_ARGS=()
    [ -n "${E2E_WIDTH:-}" ] && PUSH_EXTRA_ARGS+=(--width "$E2E_WIDTH")
    [ -n "${E2E_HEIGHT:-}" ] && PUSH_EXTRA_ARGS+=(--height "$E2E_HEIGHT")
    [ -n "${E2E_FPS:-}" ] && PUSH_EXTRA_ARGS+=(--fps "$E2E_FPS")

    if [ "$USE_TASKSET" = "1" ] && [ "$USE_CHRT" = "1" ]; then
        taskset -c "$PUSH_CORE" chrt -f "$E2E_SCHED_PRIO" "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    elif [ "$USE_TASKSET" = "1" ] && [ "$USE_NICE" = "1" ]; then
        taskset -c "$PUSH_CORE" nice -n -20 "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    elif [ "$USE_TASKSET" = "1" ]; then
        taskset -c "$PUSH_CORE" "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    elif [ "$USE_CHRT" = "1" ]; then
        chrt -f "$E2E_SCHED_PRIO" "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    elif [ "$USE_NICE" = "1" ]; then
        nice -n -20 "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    else
        "$PUSH" --config "$CFG" "${PUSH_EXTRA_ARGS[@]}" "$STREAM_ID" "$CAM" >"$PUSH_LOG" 2>&1 &
    fi
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

    # 预热几秒再启动拉流测量，避开编码器/链路刚起时的瞬态抖动尾巴。
    sleep "${E2E_AFTER_PUSH_READY_SLEEP_SEC:-3}"

    local PULL_EXTRA=()
    # E2E 默认关闭周期 GetStats，避免统计回调本身引入调度抖动；诊断时再手动打开。
    if [ "${E2E_VIDEO_STATS:-0}" = "1" ]; then
        PULL_EXTRA+=(--video-stats-interval-sec 2)
    fi

    local E2E_UI_MODE="${E2E_UI_MODE:-0}"
    local USE_XVFB=0
    local PULL_MODE_ARGS=()
    if [ "$E2E_UI_MODE" = "1" ] || [ "$E2E_UI_MODE" = "true" ] || [ "$E2E_UI_MODE" = "yes" ]; then
        # UI 模式用于产出 [E2E_UI]，从而统计 v4l2->present_submit。
        # 没有图形环境时自动走 xvfb。
        PULL_MODE_ARGS=(--frames "$FRAMES" --timeout-sec "$PULL_TO")
        if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
            USE_XVFB=1
        fi
    else
        PULL_MODE_ARGS=(--headless --frames "$FRAMES" --timeout-sec "$PULL_TO")
    fi
    local PULL_CPUSET="${PULL_CORE:-}"
    if [ "$USE_TASKSET" = "1" ] && ([ "$E2E_UI_MODE" = "1" ] || [ "$E2E_UI_MODE" = "true" ] || [ "$E2E_UI_MODE" = "yes" ]); then
        if [ -n "${E2E_PULL_TASKSET:-}" ]; then
            PULL_CPUSET="$E2E_PULL_TASKSET"
        else
            local NCPU2
            NCPU2="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
            if [ "$NCPU2" -ge 4 ] && [ -n "${PULL_CORE:-}" ] && [ "$PULL_CORE" -ge 1 ]; then
                PULL_CPUSET="$((PULL_CORE - 1)),$PULL_CORE"
            fi
        fi
    fi
    EXP_SIG+=" ui_mode=${E2E_UI_MODE} use_xvfb=${USE_XVFB} pull_cpuset=${PULL_CPUSET:-na}"
    echo "$EXP_SIG"

    local PULL_RC
    set +e
    if [ "$USE_XVFB" = "1" ] && [ "$USE_TASKSET" = "1" ] && [ "$USE_CHRT" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" chrt -f "$E2E_SCHED_PRIO" \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_XVFB" = "1" ] && [ "$USE_TASKSET" = "1" ] && [ "$USE_NICE" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" nice -n -20 \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_XVFB" = "1" ] && [ "$USE_TASKSET" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_XVFB" = "1" ] && [ "$USE_CHRT" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" chrt -f "$E2E_SCHED_PRIO" \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_XVFB" = "1" ] && [ "$USE_NICE" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" nice -n -20 \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_XVFB" = "1" ]; then
        SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}" timeout "$((PULL_TO + 30))" \
            xvfb-run -a "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_TASKSET" = "1" ] && [ "$USE_CHRT" = "1" ]; then
        timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" chrt -f "$E2E_SCHED_PRIO" "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_TASKSET" = "1" ] && [ "$USE_NICE" = "1" ]; then
        timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" nice -n -20 "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_TASKSET" = "1" ]; then
        timeout "$((PULL_TO + 30))" taskset -c "$PULL_CPUSET" "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_CHRT" = "1" ]; then
        timeout "$((PULL_TO + 30))" chrt -f "$E2E_SCHED_PRIO" "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    elif [ "$USE_NICE" = "1" ]; then
        timeout "$((PULL_TO + 30))" nice -n -20 "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    else
        timeout "$((PULL_TO + 30))" "$PULL" --config "$CFG" "${PULL_MODE_ARGS[@]}" \
            "${PULL_EXTRA[@]}" >"$PULL_LOG" 2>&1
    fi
    PULL_RC=$?
    set -e

    kill "$PUSH_PID" 2>/dev/null || true
    wait "$PUSH_PID" 2>/dev/null || true
    PUSH_PID=""
    kill "$SIG_PID" 2>/dev/null || true
    SIG_PID=""
    trap - EXIT INT TERM

    local E2E_UI_TRUNCATED=0
    if [ "$E2E_UI_MODE" = "1" ] || [ "$E2E_UI_MODE" = "true" ] || [ "$E2E_UI_MODE" = "yes" ]; then
        # GUI 模式下 pull_demo 不按 --frames 退出，外层 timeout 触发 124 视为“采样窗口结束”。
        if [ "$PULL_RC" = "124" ]; then
            E2E_UI_TRUNCATED=1
            PULL_RC=0
        fi
    fi

    echo ""
    echo "========== pull exit: $PULL_RC =========="
    if [ "$E2E_UI_TRUNCATED" = "1" ]; then
        echo "[E2E] UI mode sampling ended by timeout window; continue parsing logs."
    fi
    if [ "$PULL_RC" != 0 ]; then
        echo "--- pull tail ---"
        tail -25 "$PULL_LOG"
        echo "--- push tail ---"
        tail -25 "$PUSH_LOG"
        return "$PULL_RC"
    fi

    echo "--- E2E parse ---"
    local PARSE_ARGS=()
    if [[ "$E2E_SKIP_FIRST_PAIRS" =~ ^[0-9]+$ ]] && [ "$E2E_SKIP_FIRST_PAIRS" -gt 0 ]; then
        PARSE_ARGS+=(--skip-first-pairs "$E2E_SKIP_FIRST_PAIRS")
    fi
    python3 "$PY" "${PARSE_ARGS[@]}" "$PUSH_LOG" "$PULL_LOG" || return $?

    if [ -f "$ROOT/tools/summarize_push_pipeline.py" ]; then
        echo ""
        echo "--- 推流端（服务器）编码链打点（周期日志）---"
        python3 "$ROOT/tools/summarize_push_pipeline.py" "$PUSH_LOG" || true
    fi
    if [ -f "$ROOT/tools/summarize_server_e2e_tx.py" ]; then
        echo ""
        echo "--- 推流端（服务器）逐帧 E2E_TX 分段 ---"
        python3 "$ROOT/tools/summarize_server_e2e_tx.py" "$PUSH_LOG" || true
    fi
    if [ -f "$ROOT/tools/analyze_latency_report.py" ]; then
        echo ""
        python3 "$ROOT/tools/analyze_latency_report.py" "$PUSH_LOG" "$PULL_LOG" || true
    fi

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
    echo "E2E：默认启用低时延 profile=aggressive（可设 E2E_LOWLAT_PROFILE=balanced|stable|off）。"
    echo "      推流侧 profile 也可设 PUSH_LOWLATENCY_PROFILE=aggressive|balanced|stable|off（默认 aggressive）。"
    echo "      E2E_UI_MODE=1 可启用 UI 渲染链路打点（统计 v4l2->present_submit）；无 DISPLAY 时自动 xvfb-run。"
    echo "      默认自动 taskset 分核（E2E_CPU_PIN_MODE=off 可关闭，或 E2E_PUSH_CORE/E2E_PULL_CORE 指定核心）。"
    echo "      可选实时调度：E2E_SCHED_MODE=auto|rr|off（rr 需 root/CAP_SYS_NICE），优先级 E2E_SCHED_PRIO=1..99。"
    echo "      环境变量 E2E_FRAMES、E2E_PULL_TIMEOUT_SEC、E2E_PUSH_READY_SEC、E2E_AFTER_PUSH_READY_SLEEP_SEC、"
    echo "      E2E_SKIP_FIRST_PAIRS（默认 30，稳态口径）、E2E_WIDTH/HEIGHT/FPS、E2E_VIDEO_STATS（默认 0）、"
    echo "      CONFIG_FILE、WEBRTC_DEMO_BIN 等。"
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
