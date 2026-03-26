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
    echo "[test] Note: --fec-link is deprecated; use --fec" >&2
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
        echo "Build first: ./scripts/build.sh" >&2
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
    echo "[test][fec] ENABLE_FLEXFEC=1 WEBRTC_DUMP_REMOTE_OFFER=1; publisher uses WEBRTC_DUMP_OFFER=1 for local Offer"
    if [ "${FEC_PCAP_VERIFY:-0}" = "1" ]; then
        SDP_F=$(mktemp)
        echo "[test][fec] FEC_PCAP_VERIFY=1: short test-encode to parse FEC PT..."
        if ! ENABLE_FLEXFEC=1 WEBRTC_DUMP_OFFER=1 timeout 30 "$PUSH" --test-encode --config "$CONFIG_FILE" \
            "$STREAM_ID" "$CAMERA" >"$SDP_F" 2>&1; then
            echo "[test][fec] warn: test-encode did not finish cleanly, still parsing SDP" >&2
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
            echo "[test][fec] error: FEC_PCAP_VERIFY=1 but no flexfec/ulpfec PT in test-encode SDP" >&2
            exit 2
        fi
        echo "[test][fec] pcap filter PT=$FEC_RTP_PT ($FEC_SDP_KIND)"
        if ! command -v tcpdump >/dev/null 2>&1; then
            echo "[test][fec] error: tcpdump not found" >&2
            exit 2
        fi
        if ! command -v tshark >/dev/null 2>&1; then
            echo "[test][fec] error: tshark not found" >&2
            exit 2
        fi
        PCAP_FILE=$(mktemp /tmp/p2p_fec_test.XXXXXX.pcap)
        tcpdump -i any -U -w "$PCAP_FILE" udp >/dev/null 2>&1 &
        TCPDUMP_PID=$!
        sleep 0.4
        if ! kill -0 "$TCPDUMP_PID" 2>/dev/null; then
            echo "[test][fec] tcpdump died (need root or cap_net_raw)" >&2
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

echo "[test] Starting signaling on :$PORT"
"$SIG" "$PORT" &
SIG_PID=$!
sleep 0.5

PUSH_LOG=$(mktemp)

echo "[test] Starting publisher camera=$CAMERA stream=$STREAM_ID (log: $PUSH_LOG)"
if [ "$FEC_VERIFY" = "1" ]; then
    WEBRTC_DUMP_OFFER=1 "$PUSH" --config "$CONFIG_FILE" --signaling "127.0.0.1:$PORT" \
        "$STREAM_ID" "$CAMERA" >"$PUSH_LOG" 2>&1 &
else
    "$PUSH" --config "$CONFIG_FILE" --signaling "127.0.0.1:$PORT" \
        "$STREAM_ID" "$CAMERA" >"$PUSH_LOG" 2>&1 &
fi
PUSH_PID=$!

echo "[test] Waiting for publisher ready ([Main] Publisher started)..."
READY=0
for _ in $(seq 1 90); do
    # Match current English log or legacy Chinese (older builds).
    if grep -qE "Publisher started|推流器已启动" "$PUSH_LOG" 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "$PUSH_PID" 2>/dev/null; then
        echo "[test] Publisher exited early, log:" >&2
        tail -80 "$PUSH_LOG" >&2
        exit 1
    fi
    sleep 1
done
if [ "$READY" != 1 ]; then
    echo "[test] Timeout waiting for publisher, log:" >&2
    tail -80 "$PUSH_LOG" >&2
    exit 1
fi
sleep 1

H_FRAMES=20
H_TO=90
if [ "$STRICT_FPS" = 1 ]; then
    echo "[test] strict-fps: decode-callback FPS (expect=30 ±12%, ≥10s ≥200 frames)"
    H_FRAMES=200
    H_TO=120
fi

echo "[test] Headless pull ($([ "$STRICT_FPS" = 1 ] && echo "strict FPS" || echo "min ${H_FRAMES} frames"))"
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
echo "[test] p2p_player exit code: $RC"

if [ "$FEC_VERIFY" = "1" ] && [ "$RC" -eq 0 ] && [ -n "${PULL_OUT:-}" ] && [ -f "$PULL_OUT" ]; then
    # SDP：flexfec-03 / ulpfec 等
    fec_rtpmap_re='a=rtpmap:[0-9]+[[:space:]]+[^[:cntrl:]]*(flexfec|ulpfec)'
    PUSH_LINE=$(grep -iE "$fec_rtpmap_re" "$PUSH_LOG" 2>/dev/null | head -1 || true)
    PULL_LINE=$(grep -iE "$fec_rtpmap_re" "$PULL_OUT" 2>/dev/null | head -1 || true)
    PUSH_LINE="${PUSH_LINE//$'\r'/}"
    PULL_LINE="${PULL_LINE//$'\r'/}"
    if [ -z "$PUSH_LINE" ]; then
        echo "[test][fec] fail: no a=rtpmap flexfec/ulpfec in publisher log (local Offer missing FEC)" >&2
        exit 3
    fi
    if [ -z "$PULL_LINE" ]; then
        echo "[test][fec] fail: no a=rtpmap flexfec/ulpfec in player log (remote Offer missing FEC)" >&2
        exit 3
    fi
    echo "[test][fec] pass(SDP): publisher local Offer FEC line: $PUSH_LINE"
    echo "[test][fec] pass(SDP): player remote Offer FEC line: $PULL_LINE"

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
        echo "[test][fec] pcap: RTP lines=$RTP_TOTAL, FEC PT=${FEC_RTP_PT} lines=$FEC_COUNT (often 0 under SRTP)"
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
