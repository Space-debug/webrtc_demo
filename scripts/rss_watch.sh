#!/usr/bin/env bash
# 周期性采样指定进程的 VmRSS（/proc），输出 CSV 并在退出时打印 [RSS_SUMMARY]（便于对比改前改后）。
# 进程匹配：用 readlink /proc/PID/exe 的 basename 与目标名相等（避免 comm 15 字符截断导致 pgrep -x 失效）。
#
# 用法示例（与 E2E 并行，E2E 结束后停止采样）：
#   RSS_WATCH_OUT=build/rss_last.csv ./scripts/rss_watch.sh &
#   RSS_WATCH_PID=$!
#   ./scripts/pull.sh --e2e /dev/video11
#   kill -TERM "$RSS_WATCH_PID"; wait "$RSS_WATCH_PID" 2>/dev/null || true
#
# 或限时采样：
#   ./scripts/rss_watch.sh --duration 120 --out build/rss.csv
#
# 环境变量：
#   RSS_WATCH_TARGETS   逗号分隔的可执行 basename，默认 signaling_server,webrtc_push_demo,webrtc_pull_demo
#   RSS_WATCH_INTERVAL  采样间隔秒，默认 0.5
#   RSS_WATCH_OUT         输出 CSV（与 --out 二选一，--out 优先）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

usage() {
    echo "用法: $0 [--out FILE] [--duration SEC] [--interval SEC] [--targets a,b,c]"
    echo "详见脚本内注释。"
    exit 2
}

OUT="${RSS_WATCH_OUT:-}"
DURATION=""
INTERVAL="${RSS_WATCH_INTERVAL:-0.5}"
TARGETS="${RSS_WATCH_TARGETS:-signaling_server,webrtc_push_demo,webrtc_pull_demo}"

while [ $# -gt 0 ]; do
    case "$1" in
        --out)
            OUT="$2"
            shift 2
            ;;
        --duration)
            DURATION="$2"
            shift 2
            ;;
        --interval)
            INTERVAL="$2"
            shift 2
            ;;
        --targets)
            TARGETS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "未知参数: $1" >&2
            usage
            ;;
    esac
done

if [ -z "$OUT" ]; then
    mkdir -p "$ROOT/build"
    OUT="$ROOT/build/rss_watch_$(date +%Y%m%d_%H%M%S).csv"
fi

mkdir -p "$(dirname "$OUT")"

IFS=',' read -r -a TARGET_ARR <<< "$TARGETS"
for i in "${!TARGET_ARR[@]}"; do
    TARGET_ARR[$i]="$(echo "${TARGET_ARR[$i]}" | xargs)"
done

rss_kb_one_pid() {
    local pid="$1"
    if [ -z "$pid" ] || [ ! -r "/proc/$pid/status" ]; then
        echo 0
        return
    fi
    awk '/^VmRSS:/{print $2+0; exit}' "/proc/$pid/status" 2>/dev/null || echo 0
}

# 一轮扫描 /proc：按 exe basename 归类累加 VmRSS（kB），避免对每个目标各扫一遍 /proc。
rss_kb_line_for_targets() {
    local -a sums
    local i pid base exe kb
    for i in "${!TARGET_ARR[@]}"; do
        sums[$i]=0
    done
    shopt -s nullglob
    for d in /proc/[0-9]*; do
        pid="${d#/proc/}"
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        exe="$d/exe"
        [ -L "$exe" ] || continue
        base="$(basename "$(readlink -f "$exe" 2>/dev/null || true)" 2>/dev/null || true)"
        [ -n "$base" ] || continue
        kb=$(rss_kb_one_pid "$pid")
        for i in "${!TARGET_ARR[@]}"; do
            if [ "$base" = "${TARGET_ARR[$i]}" ]; then
                sums[$i]=$((${sums[$i]} + kb))
            fi
        done
    done
    shopt -u nullglob
    local out=""
    for i in "${!TARGET_ARR[@]}"; do
        out+=",${sums[$i]}"
    done
    echo "${out#,}"
}

header="t_unix"
for n in "${TARGET_ARR[@]}"; do
    header+=",rss_kb_${n}"
done
echo "$header" >"$OUT"

cleanup_summary() {
    if [ ! -s "$OUT" ]; then
        echo "[RSS_WATCH] no samples: $OUT" >&2
        return
    fi
    local lines ncol
    lines=$(wc -l <"$OUT")
    if [ "$lines" -lt 2 ]; then
        echo "[RSS_WATCH] only header: $OUT" >&2
        return
    fi
    ncol=$(head -1 "$OUT" | awk -F',' '{print NF}')
    echo "[RSS_WATCH] samples=$((lines - 1)) file=$OUT" >&2
    local col hdr
    hdr="$(head -1 "$OUT")"
    for ((col = 2; col <= ncol; col++)); do
        local label
        label="$(echo "$hdr" | awk -F',' -v c="$col" '{print $c}')"
        awk -F',' -v c="$col" -v lab="$label" '
            NR == 1 { next }
            {
                v = $c + 0
                if (n == 0) { min = v; max = v; s = v; n = 1 }
                else {
                    if (v < min) min = v
                    if (v > max) max = v
                    s += v
                    n++
                }
            }
            END {
                if (n > 0)
                    printf "[RSS_SUMMARY] %s min_kb=%d max_kb=%d mean_kb=%.1f n=%d\n", lab, min, max, s / n, n
            }
        ' "$OUT" >&2
    done
}

trap 'cleanup_summary; exit 0' INT TERM

start_ts=$(date +%s)
dur_int=0
if [ -n "$DURATION" ]; then
    dur_int="${DURATION%%.*}"
    [ "$dur_int" -ge 0 ] 2>/dev/null || dur_int=0
fi

# 时长判断放在 sleep 之后：避免单次采样耗时较长时「首行即超时」导致只有 1 个样本。
while true; do
    t_unix=$(date +%s)
    rest=$(rss_kb_line_for_targets)
    echo "$t_unix,$rest" >>"$OUT"
    sleep "$INTERVAL"
    if [ -n "$DURATION" ] && [ "$dur_int" -gt 0 ]; then
        now_i=$(date +%s)
        if [ $((now_i - start_ts)) -ge "$dur_int" ]; then
            break
        fi
    fi
done

cleanup_summary
