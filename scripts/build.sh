#!/bin/bash
# WebRTC 推流 Demo 构建脚本

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"

echo "=== WebRTC Push Demo Build ==="
echo "Project root: $PROJECT_ROOT"
echo "Build dir: $BUILD_DIR"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$PROJECT_ROOT" -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo ""
echo "Build complete. Executable: $BUILD_DIR/bin/webrtc_push_demo"
