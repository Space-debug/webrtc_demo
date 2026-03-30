#!/bin/bash
# 编译 webrtc_push_sdk + 演示程序 + 信令服务
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
BUILD="${ROOT}/build"
mkdir -p "$BUILD"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"
echo "OK: ${BUILD}/bin/webrtc_push_demo  ${BUILD}/bin/webrtc_pull_demo (若已装 SDL2)  ${BUILD}/bin/signaling_server"
echo "RK MPP：cmake -S \"$ROOT\" -B ${ROOT}/build_mpp -DCMAKE_BUILD_TYPE=Release -DWEBRTC_DEMO_ENABLE_ROCKCHIP_MPP=ON && cmake --build ${ROOT}/build_mpp -j\$(nproc)"
echo "端到端：WEBRTC_DEMO_BIN=${ROOT}/build_mpp/bin ./scripts/e2e_headless_once.sh"
