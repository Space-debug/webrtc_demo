#!/bin/bash
# 快速验证 libwebrtc 直连摄像头采集（--test-capture，约 10s + 预热）
# 用法: ./scripts/test_capture_quick.sh [摄像头设备，默认 /dev/video11]

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAM="${1:-/dev/video11}"
STREAM="${STREAM_ID:-livestream}"
case "$(uname -m)" in
    aarch64|arm64) ARCH="arm64" ;;
    x86_64|amd64) ARCH="x64" ;;
    *) ARCH="arm64" ;;
esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/${ARCH}:${LD_LIBRARY_PATH:-}"
exec "$ROOT/build/bin/webrtc_push_demo" --test-capture --config "$ROOT/config/streams.conf" \
    "$STREAM" "$CAM"
