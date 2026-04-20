#!/bin/bash
# Weak-network E2E runner:
# 1) apply tc netem profile
# 2) run ./scripts/pull.sh --e2e
# 3) auto cleanup tc and print quick judgement
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${CONFIG_FILE:-$ROOT/config/streams.conf}"

usage() {
  cat <<'EOF'
Usage:
  ./tools/run_e2e_weaknet.sh [options]

Options:
  --profile P        good|fair|bad|harsh|custom (default fair)
  --camera DEV       camera path for --e2e (default from config)
  --iface IFACE      tc target iface (default auto: lo when signaling is local)
  --duration-sec N   pull timeout window, exported as E2E_PULL_TIMEOUT_SEC (default 45)
  --frames N         target frames, exported as E2E_FRAMES (default 500)
  --skip-first N     exported as E2E_SKIP_FIRST_PAIRS (default 30)
  --apply-only       only apply tc netem, do not run e2e
  --clear-only       clear tc netem and exit
  --yes              skip confirmation prompt
  -h, --help         show this help

Custom profile env (used when --profile custom):
  WEAKNET_DELAY_MS            default 60
  WEAKNET_JITTER_MS           default 20
  WEAKNET_LOSS_PCT            default 1.5
  WEAKNET_REORDER_PCT         default 1
  WEAKNET_REORDER_CORR_PCT    default 25
  WEAKNET_DUP_PCT             default 0
  WEAKNET_CORRUPT_PCT         default 0
  WEAKNET_RATE                default 6mbit

Notes:
  - Requires sudo tc permissions.
  - Script always tries to clean qdisc on exit.
EOF
}

cfg_get() {
  local key="$1" def="$2"
  [ ! -f "$CONFIG" ] && echo "$def" && return
  local line
  line="$(awk -F= -v k="$key" '$1 ~ "^[[:space:]]*"k"[[:space:]]*$" {print $0}' "$CONFIG" | tail -1)" || true
  [ -z "$line" ] && echo "$def" && return
  echo "${line#*=}" | sed 's/#.*//' | xargs
}

profile="fair"
camera=""
iface=""
duration_sec=45
frames=500
skip_first=30
apply_only=0
clear_only=0
assume_yes=0

while [ $# -gt 0 ]; do
  case "$1" in
    --profile) profile="${2:-}"; shift 2 ;;
    --camera) camera="${2:-}"; shift 2 ;;
    --iface) iface="${2:-}"; shift 2 ;;
    --duration-sec) duration_sec="${2:-}"; shift 2 ;;
    --frames) frames="${2:-}"; shift 2 ;;
    --skip-first) skip_first="${2:-}"; shift 2 ;;
    --apply-only) apply_only=1; shift ;;
    --clear-only) clear_only=1; shift ;;
    --yes) assume_yes=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [ "$clear_only" = "1" ] && [ -z "$iface" ]; then
  sig_addr="$(cfg_get SIGNALING_ADDR 127.0.0.1:8765)"
  host="${sig_addr%%:*}"
  if [ "$host" = "127.0.0.1" ] || [ "$host" = "localhost" ]; then
    iface="lo"
  fi
fi

if [ "$clear_only" = "1" ]; then
  [ -z "$iface" ] && { echo "--clear-only requires --iface when signaling is not local" >&2; exit 2; }
  sudo tc qdisc del dev "$iface" root 2>/dev/null || true
  echo "[weaknet] cleared qdisc on $iface"
  exit 0
fi

if [ -z "$iface" ]; then
  sig_addr="$(cfg_get SIGNALING_ADDR 127.0.0.1:8765)"
  host="${sig_addr%%:*}"
  if [ "$host" = "127.0.0.1" ] || [ "$host" = "localhost" ]; then
    iface="lo"
  else
    echo "signaling host is $host; please provide --iface explicitly" >&2
    exit 2
  fi
fi

case "$profile" in
  good)
    delay_ms=25; jitter_ms=8; loss_pct=0.2; reorder_pct=0; reorder_corr_pct=0; dup_pct=0; corrupt_pct=0; rate="20mbit"
    ;;
  fair)
    delay_ms=60; jitter_ms=20; loss_pct=1.0; reorder_pct=1.0; reorder_corr_pct=25; dup_pct=0; corrupt_pct=0; rate="8mbit"
    ;;
  bad)
    delay_ms=120; jitter_ms=40; loss_pct=2.5; reorder_pct=3.0; reorder_corr_pct=30; dup_pct=0.2; corrupt_pct=0; rate="4mbit"
    ;;
  harsh)
    delay_ms=180; jitter_ms=60; loss_pct=4.0; reorder_pct=5.0; reorder_corr_pct=35; dup_pct=0.5; corrupt_pct=0.1; rate="2mbit"
    ;;
  custom)
    delay_ms="${WEAKNET_DELAY_MS:-60}"
    jitter_ms="${WEAKNET_JITTER_MS:-20}"
    loss_pct="${WEAKNET_LOSS_PCT:-1.5}"
    reorder_pct="${WEAKNET_REORDER_PCT:-1}"
    reorder_corr_pct="${WEAKNET_REORDER_CORR_PCT:-25}"
    dup_pct="${WEAKNET_DUP_PCT:-0}"
    corrupt_pct="${WEAKNET_CORRUPT_PCT:-0}"
    rate="${WEAKNET_RATE:-6mbit}"
    ;;
  *)
    echo "Unsupported profile: $profile" >&2
    exit 2
    ;;
esac

if [ "$assume_yes" != "1" ]; then
  echo "[weaknet] profile=$profile iface=$iface delay=${delay_ms}ms jitter=${jitter_ms}ms loss=${loss_pct}% reorder=${reorder_pct}% rate=$rate"
  read -r -p "Apply tc netem and run e2e? [y/N] " yn
  case "$yn" in
    [Yy]*) ;;
    *) echo "cancelled"; exit 0 ;;
  esac
fi

cleanup() {
  sudo tc qdisc del dev "$iface" root 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sudo tc qdisc del dev "$iface" root 2>/dev/null || true
sudo tc qdisc add dev "$iface" root netem \
  delay "${delay_ms}ms" "${jitter_ms}ms" \
  loss "${loss_pct}%" \
  reorder "${reorder_pct}%" "${reorder_corr_pct}%" \
  duplicate "${dup_pct}%" \
  corrupt "${corrupt_pct}%" \
  rate "$rate"

echo "[weaknet] applied on $iface"
sudo tc qdisc show dev "$iface"

if [ "$apply_only" = "1" ]; then
  echo "[weaknet] apply-only done"
  exit 0
fi

mkdir -p "$ROOT/build/e2e_weaknet"
ts="$(date +%Y%m%d-%H%M%S)"
run_log="$ROOT/build/e2e_weaknet/${ts}_${profile}.log"

export E2E_FRAMES="$frames"
export E2E_PULL_TIMEOUT_SEC="$duration_sec"
export E2E_SKIP_FIRST_PAIRS="$skip_first"
export E2E_LOWLAT_PROFILE="${E2E_LOWLAT_PROFILE:-steady}"
export PUSH_LOWLATENCY_PROFILE="${PUSH_LOWLATENCY_PROFILE:-stable}"

# Recommended encoder strategy defaults (can be overridden by env):
export WEBRTC_MPP_ENC_INTRA_REFRESH_MODE="${WEBRTC_MPP_ENC_INTRA_REFRESH_MODE:-1}"
export WEBRTC_MPP_ENC_INTRA_REFRESH_ARG="${WEBRTC_MPP_ENC_INTRA_REFRESH_ARG:-1}"
export WEBRTC_MPP_ENC_SPLIT_BYTES="${WEBRTC_MPP_ENC_SPLIT_BYTES:-0}"
export WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS="${WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS:-250}"
export WEBRTC_MPP_ENC_IDR_LOSS_QUICK_MS="${WEBRTC_MPP_ENC_IDR_LOSS_QUICK_MS:-180}"
export WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS="${WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS:-1200}"

echo "[weaknet] run e2e... log=$run_log"
if [ -n "$camera" ]; then
  (cd "$ROOT" && ./scripts/pull.sh --e2e "$camera") | tee "$run_log"
else
  (cd "$ROOT" && ./scripts/pull.sh --e2e) | tee "$run_log"
fi

echo ""
echo "[weaknet] quick gate (steady-state trace_id summary):"
summary_line="$(awk '/\[E2E_SUMMARY\] pairing=trace_id_steady/{line=$0} END{print line}' "$run_log")"
if [ -z "$summary_line" ]; then
  echo "  WARN: no steady summary found, check $run_log"
  exit 1
fi
echo "  $summary_line"

p95="$(echo "$summary_line" | sed -n 's/.*p95=\([0-9.]\+\)ms.*/\1/p')"
p99="$(echo "$summary_line" | sed -n 's/.*p99=\([0-9.]\+\)ms.*/\1/p')"

status="PASS"
if awk "BEGIN{exit !($p99 > 220.0)}"; then
  status="FAIL"
elif awk "BEGIN{exit !($p95 > 120.0 || $p99 > 150.0)}"; then
  status="WARN"
fi
echo "  gate: p95<=120ms and p99<=150ms preferred, p99>220ms fail"
echo "  result: $status (p95=${p95}ms p99=${p99}ms)"
echo ""
echo "[weaknet] artifacts:"
echo "  - $run_log"
echo "  - $ROOT/build/e2e_last_push.log"
echo "  - $ROOT/build/e2e_last_pull.log"
