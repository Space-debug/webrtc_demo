#!/bin/bash
# 结束本仓库推拉流/信令进程，并释放 V4L2 设备（避免摄像头被占）
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

kill_tcp_listen_port() {
  local port="$1"
  if command -v lsof >/dev/null 2>&1; then
    # 仅监听端（避免误杀连上的客户端）
    lsof -nP -iTCP:"$port" -sTCP:LISTEN -t 2>/dev/null | while read -r pid; do
      [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
  fi
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${port}/tcp" 2>/dev/null || true
  fi
  if command -v ss >/dev/null 2>&1; then
    ss -ltnp 2>/dev/null | grep -E ":${port}\\b" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | sort -u |
      while read -r pid; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
      done
  fi
}

kill_tcp_listen_port 8765
sleep 1
kill_tcp_listen_port 8765

pkill -9 -f "$ROOT/build/bin/signaling_server" 2>/dev/null || true
pkill -9 -f "$ROOT/build/bin/webrtc_push_demo" 2>/dev/null || true
pkill -9 -f "$ROOT/build/bin/webrtc_pull_demo" 2>/dev/null || true
pkill -9 -f "webrtc_demo/build/bin/" 2>/dev/null || true
pkill -9 -f "webrtc_push_demo" 2>/dev/null || true
pkill -9 -f "webrtc_pull_demo" 2>/dev/null || true

VID="${E2E_VIDEO_DEV:-/dev/video11}"
if [ -e "$VID" ]; then
  if command -v lsof >/dev/null 2>&1; then
    lsof -t "$VID" 2>/dev/null | while read -r pid; do
      [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
  fi
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "$VID" 2>/dev/null || true
  fi
fi
sleep 1
echo "OK: 信令/推拉流进程已清理；若指定设备则已尝试释放 $VID"
