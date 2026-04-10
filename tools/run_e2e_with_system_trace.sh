#!/bin/bash
# 同步采集 E2E 日志 + 系统调度证据（CPU/线程/负载），用于定位 p99 慢帧来源。
#
# 用法:
#   tools/run_e2e_with_system_trace.sh [camera]
#
# 可选环境变量:
#   E2E_FRAMES=400
#   E2E_PULL_TIMEOUT_SEC=150
#   E2E_CAPTURE_INTERVAL_SEC=1
#   E2E_EXTRA_ENV="E2E_LOWLAT_PROFILE=aggressive WEBRTC_PULL_UI_POLL_SLEEP_MS=0"

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT}/build/e2e_systrace_$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT_DIR"

CAMERA="${1:-}"
CAP_INTERVAL="${E2E_CAPTURE_INTERVAL_SEC:-1}"
EXTRA_ENV="${E2E_EXTRA_ENV:-}"

echo "[systrace] output dir: $OUT_DIR"
echo "[systrace] camera: ${CAMERA:-auto-from-config}"
echo "[systrace] capture interval: ${CAP_INTERVAL}s"

start_bg() {
  local name="$1"
  shift
  if command -v "$1" >/dev/null 2>&1; then
    "$@" >"${OUT_DIR}/${name}.log" 2>&1 &
    echo $!
  else
    echo ""
  fi
}

PIDS=()

# 基础系统负载
VM_PID="$(start_bg vmstat vmstat "$CAP_INTERVAL")"
[ -n "$VM_PID" ] && PIDS+=("$VM_PID")

# 每核 CPU（若有 sysstat/mpstat）
MP_PID="$(start_bg mpstat mpstat -P ALL "$CAP_INTERVAL")"
[ -n "$MP_PID" ] && PIDS+=("$MP_PID")

# 进程视角（pidstat）
PD_PID="$(start_bg pidstat pidstat -rud -h "$CAP_INTERVAL")"
[ -n "$PD_PID" ] && PIDS+=("$PD_PID")

# top 快照
TOP_PID=""
if command -v top >/dev/null 2>&1; then
  top -b -d "$CAP_INTERVAL" >"${OUT_DIR}/top.log" 2>&1 &
  TOP_PID=$!
  PIDS+=("$TOP_PID")
fi

cleanup() {
  for p in "${PIDS[@]:-}"; do
    kill "$p" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

echo "[systrace] started collectors: ${PIDS[*]:-none}"

# 跑 E2E（主输出也留档）
set +e
if [ -n "$EXTRA_ENV" ]; then
  bash -lc "cd \"$ROOT\" && $EXTRA_ENV ./scripts/pull.sh --e2e ${CAMERA}" \
    >"${OUT_DIR}/e2e_run.log" 2>&1
  RC=$?
else
  (cd "$ROOT" && ./scripts/pull.sh --e2e "$CAMERA") >"${OUT_DIR}/e2e_run.log" 2>&1
  RC=$?
fi
set -e

cleanup
trap - EXIT INT TERM

# 复制主日志快照
cp -f "${ROOT}/build/e2e_last_push.log" "${OUT_DIR}/e2e_last_push.log" 2>/dev/null || true
cp -f "${ROOT}/build/e2e_last_pull.log" "${OUT_DIR}/e2e_last_pull.log" 2>/dev/null || true

echo "[systrace] e2e exit code: $RC"
echo "[systrace] logs:"
echo "  ${OUT_DIR}/e2e_run.log"
echo "  ${OUT_DIR}/e2e_last_push.log"
echo "  ${OUT_DIR}/e2e_last_pull.log"
echo "  ${OUT_DIR}/vmstat.log"
echo "  ${OUT_DIR}/mpstat.log"
echo "  ${OUT_DIR}/pidstat.log"
echo "  ${OUT_DIR}/top.log"

echo ""
echo "[systrace] next:"
echo "  python3 tools/parse_e2e_latency.py --skip-first-pairs 30 --dump-outliers 10 \"${OUT_DIR}/e2e_last_push.log\" \"${OUT_DIR}/e2e_last_pull.log\""
echo "  python3 tools/explain_e2e_outliers.py \"${OUT_DIR}/e2e_last_push.log\" \"${OUT_DIR}/e2e_last_pull.log\" --top 10"

exit "$RC"

