#!/bin/bash
# 信令 + 推流（后台）+ 无头拉流一次；结束后杀掉子进程
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
case "$(uname -m)" in aarch64|arm64) A=arm64;; x86_64|amd64) A=x64;; *) A=arm64;; esac
export LD_LIBRARY_PATH="$ROOT/3rdparty/libwebrtc/lib/linux/$A:${LD_LIBRARY_PATH:-}"

SIG_BIN="$ROOT/build/bin/signaling_server"
PUSH_BIN="$ROOT/build/bin/webrtc_push_demo"
PULL_BIN="$ROOT/build/bin/webrtc_pull_demo"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"
[ -x "$SIG_BIN" ] && [ -x "$PUSH_BIN" ] && [ -x "$PULL_BIN" ] || { echo "先 ./scripts/build.sh" >&2; exit 1; }

# 推流先跑够采集门限再拉；拉流侧约 10s 内能收到足够帧即判成功（可调 HEADLESS_* / E2E_PUSH_WARMUP_SEC）
WARMUP="${E2E_PUSH_WARMUP_SEC:-35}"
FRAMES="${HEADLESS_FRAMES:-10}"
TIMEOUT="${HEADLESS_TIMEOUT:-10}"
# 默认关闭推流端周期性 GetStats，减轻与建连争用（仍保留死锁修复后的 EnsurePeerConnectionForPeer）
export WEBRTC_LATENCY_STATS_PROBE="${WEBRTC_LATENCY_STATS_PROBE:-0}"

bash "$ROOT/scripts/kill_webrtc_demo.sh"
sleep 3

cleanup() {
  [ -n "${PUSH_PID:-}" ] && kill -9 "$PUSH_PID" 2>/dev/null || true
  [ -n "${SIG_PID:-}" ] && kill -9 "$SIG_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

"$SIG_BIN" 8765 &
SIG_PID=$!
# 等待信令真正 listen，避免旧进程占端口时新进程 bind 失败但脚本仍继续
for _ in $(seq 1 30); do
  if ss -ltn 2>/dev/null | grep -qE ':(8765)\s'; then break; fi
  if command -v nc >/dev/null 2>&1 && nc -z 127.0.0.1 8765 2>/dev/null; then break; fi
  sleep 0.2
done
if ! ss -ltn 2>/dev/null | grep -qE ':(8765)\s' && { ! command -v nc >/dev/null 2>&1 || ! nc -z 127.0.0.1 8765 2>/dev/null; }; then
  echo "e2e: 8765 未监听，信令可能 bind 失败；先 ./scripts/kill_webrtc_demo.sh" >&2
  exit 1
fi

if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL "$PUSH_BIN" --config "$CONFIG" &
else
  "$PUSH_BIN" --config "$CONFIG" &
fi
PUSH_PID=$!
sleep "$WARMUP"

HEADLESS=1 "$PULL_BIN" --config "$CONFIG" --headless --frames "$FRAMES" --timeout-sec "$TIMEOUT"
EC=$?
exit "$EC"
