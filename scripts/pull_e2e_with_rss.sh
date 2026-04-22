#!/usr/bin/env bash
# 跑同机 E2E（./scripts/pull.sh --e2e）同时在后台采样 push/pull/signaling 的 VmRSS，结束后打印 [RSS_SUMMARY]。
#
# 用法：
#   ./scripts/pull_e2e_with_rss.sh /dev/video11
#   RSS_WATCH_INTERVAL=0.25 RSS_WATCH_OUT=build/rss_run1.csv ./scripts/pull_e2e_with_rss.sh /dev/video11
#
# 透传：除本脚本解析项外，其余参数全部传给 pull.sh（例如仅改 E2E 环境变量即可）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
OUT="${RSS_WATCH_OUT:-$ROOT/build/rss_e2e_last.csv}"

mkdir -p "$(dirname "$OUT")"
echo "[RSS_WATCH] sampling VmRSS -> $OUT (interval=${RSS_WATCH_INTERVAL:-0.5}s); summary lines on stderr at exit" >&2

(
    export RSS_WATCH_OUT="$OUT"
    exec "$ROOT/scripts/rss_watch.sh" --interval "${RSS_WATCH_INTERVAL:-0.5}" --out "$OUT"
) &
WATCH_PID=$!

cleanup() {
    kill -TERM "$WATCH_PID" 2>/dev/null || true
    wait "$WATCH_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

set +e
"$ROOT/scripts/pull.sh" --e2e "$@"
RC=$?
set -e

exit "$RC"
