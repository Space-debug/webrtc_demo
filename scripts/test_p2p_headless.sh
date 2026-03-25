#!/bin/bash
# 本机无头验证：信令 + 推流 + p2p_player --headless（通过帧计数判定成功）
# 默认尽量按 config/streams.conf 运行，参数仅在显式传入时覆盖
#
# 用法:
#   ./scripts/test_p2p_headless.sh [摄像头设备]
#   ./scripts/test_p2p_headless.sh --strict-fps [摄像头设备]  # 无头拉流 ≥10s、≥200 帧，校验解码 FPS≈30（±12%）
#   ./scripts/test_p2p_headless.sh --fec [摄像头设备]       # SDP：发送端 Offer 与客户端收到的 Offer 均含 flexfec/ulpfec
#   ./scripts/test_p2p_headless.sh --fec-link [摄像头设备] # 已废弃：与 --fec 相同（推流端 FEC 链上统计探针已移除）
# 环境变量:
#   FEC_VERIFY=1        与 --fec 等效
#   ENABLE_FLEXFEC=1    由脚本在 FEC 模式下自动 export
#   FEC_PCAP_VERIFY=1   可选：test-encode 解析 PT + tcpdump+tshark（SRTP 下常仍无 rtp 层）
#   P2P_TEST_PORT       信令端口，默认 18765
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

FEC_VERIFY="${FEC_VERIFY:-0}"
STRICT_FPS=0
if [ "${1:-}" = "--fec-link" ]; then
    echo "[test] 提示: --fec-link 已废弃（FEC 链上 GetStats 探针已从推流端移除），按 --fec 运行" >&2
    FEC_VERIFY=1
    shift
elif [ "${1:-}" = "--fec" ]; then
    FEC_VERIFY=1
    shift
elif [ "${1:-}" = "--strict-fps" ]; then
    STRICT_FPS=1
    shift
fi

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

STREAM_ID="${STREAM_ID:-$(cfg_get STREAM_ID livestream)}"
CAMERA="${1:-$(cfg_get "STREAM_${STREAM_ID}_CAMERA" /dev/video11)}"
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

# FEC_PCAP 用 PT（仅 FEC_PCAP_VERIFY=1 时由 test-encode 解析）
FEC_RTP_PT=""
FEC_SDP_KIND=""
PCAP_FILE=""
TCPDUMP_PID=""
PUSH_LOG=""
PULL_OUT=""

# FEC 模式：Field Trials + 信令 SDP 落盘（推流本地 Offer / 拉流远端 Offer）
if [ "$FEC_VERIFY" = "1" ]; then
    export ENABLE_FLEXFEC=1
    export WEBRTC_DUMP_REMOTE_OFFER=1
    echo "[test][fec] ENABLE_FLEXFEC=1、WEBRTC_DUMP_REMOTE_OFFER=1；推流进程将带 WEBRTC_DUMP_OFFER=1 打印本地 Offer"
    if [ "${FEC_PCAP_VERIFY:-0}" = "1" ]; then
        SDP_F=$(mktemp)
        echo "[test][fec] FEC_PCAP_VERIFY=1：短时 test-encode 解析 FEC PT…"
        if ! ENABLE_FLEXFEC=1 WEBRTC_DUMP_OFFER=1 timeout 30 "$PUSH" --test-encode --config "$CONFIG_FILE" \
            "$STREAM_ID" "$CAMERA" >"$SDP_F" 2>&1; then
            echo "[test][fec] 警告: test-encode 未正常结束，仍尝试解析 SDP" >&2
        fi
        FEC_RTP_PT=$(grep -i 'a=rtpmap:' "$SDP_F" | grep -i flexfec | head -1 | sed -E 's/.*a=rtpmap:([0-9]+).*/\1/') || true
        FEC_SDP_KIND="flexfec"
        if [ -z "$FEC_RTP_PT" ]; then
            FEC_RTP_PT=$(grep -i 'a=rtpmap:' "$SDP_F" | grep -i ulpfec | head -1 | sed -E 's/.*a=rtpmap:([0-9]+).*/\1/') || true
            FEC_SDP_KIND="ulpfec"
        fi
        rm -f "$SDP_F"
        sleep 2
        if [ -z "$FEC_RTP_PT" ]; then
            echo "[test][fec] 错误: FEC_PCAP_VERIFY=1 但 test-encode SDP 未解析到 flexfec/ulpfec PT" >&2
            exit 2
        fi
        echo "[test][fec] pcap 过滤用 PT=$FEC_RTP_PT ($FEC_SDP_KIND)"
        if ! command -v tcpdump >/dev/null 2>&1; then
            echo "[test][fec] 错误: 未找到 tcpdump" >&2
            exit 2
        fi
        if ! command -v tshark >/dev/null 2>&1; then
            echo "[test][fec] 错误: 未找到 tshark" >&2
            exit 2
        fi
        PCAP_FILE=$(mktemp /tmp/p2p_fec_test.XXXXXX.pcap)
        tcpdump -i any -U -w "$PCAP_FILE" udp >/dev/null 2>&1 &
        TCPDUMP_PID=$!
        sleep 0.4
        if ! kill -0 "$TCPDUMP_PID" 2>/dev/null; then
            echo "[test][fec] tcpdump 未存活（需 root/cap_net_raw）" >&2
            rm -f "$PCAP_FILE"
            PCAP_FILE=""
            exit 2
        fi
    fi
    PULL_OUT=$(mktemp)
fi

cleanup() {
    if [ -n "${TCPDUMP_PID:-}" ] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill "$TCPDUMP_PID" 2>/dev/null || true
        wait "$TCPDUMP_PID" 2>/dev/null || true
    fi
    rm -f "${PUSH_LOG:-}"
    rm -f "${PULL_OUT:-}"
    rm -f "${PCAP_FILE:-}"
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
if [ "$FEC_VERIFY" = "1" ]; then
    WEBRTC_DUMP_OFFER=1 "$PUSH" --config "$CONFIG_FILE" --signaling "127.0.0.1:$PORT" \
        "$STREAM_ID" "$CAMERA" >"$PUSH_LOG" 2>&1 &
else
    "$PUSH" --config "$CONFIG_FILE" --signaling "127.0.0.1:$PORT" \
        "$STREAM_ID" "$CAMERA" >"$PUSH_LOG" 2>&1 &
fi
PUSH_PID=$!

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

H_FRAMES=20
H_TO=90
if [ "$STRICT_FPS" = 1 ]; then
    echo "[test] strict-fps：按解码回调时间戳统计平均 FPS（expect=30 ±12%，≥10s ≥200 帧）"
    H_FRAMES=200
    H_TO=120
fi

echo "[test] 无头拉流（$([ "$STRICT_FPS" = 1 ] && echo "strict FPS" || echo "至少 ${H_FRAMES} 帧")）"
if [ -n "${PULL_OUT:-}" ]; then
    if [ "$STRICT_FPS" = 1 ]; then
        "$PULL" --config "$CONFIG_FILE" --headless --strict-fps --expect-fps 30 --fps-tol 0.12 \
            --fps-min-sec 10 --fps-min-frames 200 --frames 200 --timeout-sec "$H_TO" \
            "127.0.0.1:$PORT" "$STREAM_ID" 2>&1 | tee "$PULL_OUT"
    else
        "$PULL" --config "$CONFIG_FILE" --headless --frames "$H_FRAMES" --timeout-sec "$H_TO" "127.0.0.1:$PORT" "$STREAM_ID" 2>&1 | tee "$PULL_OUT"
    fi
    RC=${PIPESTATUS[0]}
else
    if [ "$STRICT_FPS" = 1 ]; then
        "$PULL" --config "$CONFIG_FILE" --headless --strict-fps --expect-fps 30 --fps-tol 0.12 \
            --fps-min-sec 10 --fps-min-frames 200 --frames 200 --timeout-sec "$H_TO" \
            "127.0.0.1:$PORT" "$STREAM_ID"
    else
        "$PULL" --config "$CONFIG_FILE" --headless --frames "$H_FRAMES" --timeout-sec "$H_TO" "127.0.0.1:$PORT" "$STREAM_ID"
    fi
    RC=$?
fi
echo "[test] p2p_player 退出码: $RC"

if [ "$FEC_VERIFY" = "1" ] && [ "$RC" -eq 0 ] && [ -n "${PULL_OUT:-}" ] && [ -f "$PULL_OUT" ]; then
    # SDP：flexfec-03 / ulpfec 等
    fec_rtpmap_re='a=rtpmap:[0-9]+[[:space:]]+[^[:cntrl:]]*(flexfec|ulpfec)'
    PUSH_LINE=$(grep -iE "$fec_rtpmap_re" "$PUSH_LOG" 2>/dev/null | head -1 || true)
    PULL_LINE=$(grep -iE "$fec_rtpmap_re" "$PULL_OUT" 2>/dev/null | head -1 || true)
    PUSH_LINE="${PUSH_LINE//$'\r'/}"
    PULL_LINE="${PULL_LINE//$'\r'/}"
    if [ -z "$PUSH_LINE" ]; then
        echo "[test][fec] 失败: 推流日志未见 a=rtpmap … flexfec/ulpfec（发送端本地 Offer 未带 FEC 媒体行）" >&2
        exit 3
    fi
    if [ -z "$PULL_LINE" ]; then
        echo "[test][fec] 失败: 拉流日志未见 a=rtpmap … flexfec/ulpfec（客户端未收到含 FEC 的远端 Offer）" >&2
        exit 3
    fi
    echo "[test][fec] 通过(SDP): 发送端本地 Offer 含 FEC 行: $PUSH_LINE"
    echo "[test][fec] 通过(SDP): 客户端收到的远端 Offer 含 FEC 行: $PULL_LINE"

    if [ -n "${TCPDUMP_PID:-}" ] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill "$TCPDUMP_PID" 2>/dev/null || true
        wait "$TCPDUMP_PID" 2>/dev/null || true
    fi
    TCPDUMP_PID=""
    if [ "${FEC_PCAP_VERIFY:-0}" = "1" ] && [ -n "${PCAP_FILE:-}" ] && [ -f "$PCAP_FILE" ]; then
        FEC_COUNT=$(tshark -r "$PCAP_FILE" -Y "rtp && rtp.p_type == ${FEC_RTP_PT}" 2>/dev/null | wc -l)
        FEC_COUNT="${FEC_COUNT//[[:space:]]/}"
        RTP_TOTAL=$(tshark -r "$PCAP_FILE" -Y "rtp" 2>/dev/null | wc -l)
        RTP_TOTAL="${RTP_TOTAL//[[:space:]]/}"
        echo "[test][fec] pcap 参考: RTP 解析行=$RTP_TOTAL, FEC PT=${FEC_RTP_PT} 行数=$FEC_COUNT（SRTP 时常为 0）"
        rm -f "$PCAP_FILE"
        PCAP_FILE=""
    else
        rm -f "${PCAP_FILE:-}"
        PCAP_FILE=""
    fi
elif [ -n "${TCPDUMP_PID:-}" ] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
    kill "$TCPDUMP_PID" 2>/dev/null || true
    wait "$TCPDUMP_PID" 2>/dev/null || true
    TCPDUMP_PID=""
    rm -f "${PCAP_FILE:-}"
    PCAP_FILE=""
fi

exit "$RC"
