#!/usr/bin/env python3
"""
定位并解释 E2E 慢帧：
1) 用 trace_id 对齐 push/pull 日志，找最慢 N 帧（按 mjpeg_input->sink）
2) 在 pull 日志中抓该 trace 附近的 TimingDelta 与 InboundVideoStats
3) 给出简要瓶颈标签（JB/解码/收包跨度）

用法:
  python3 tools/explain_e2e_outliers.py build/e2e_last_push.log build/e2e_last_pull.log --top 8
"""
from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

RE_TX = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+)"
)
RE_RX = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) trace_id=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) "
    r"(?:wall_utc_ms=(-?\d+) )?t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
RE_DELTA_F2D = re.compile(r"first_RTP→decode_start = (-?\d+) ms")
RE_DELTA_L2D = re.compile(r"last_RTP→decode_start = (-?\d+) ms")
RE_DELTA_DEC = re.compile(r"decode_start→decode_finish = (-?\d+) ms")
RE_DELTA_SPAN = re.compile(r"RTP_last−first = (-?\d+) ms")
RE_IBS = re.compile(
    r"\[InboundVideoStats\].*?frames_decoded=(\d+).*?total_decode_time_s=([0-9.]+).*?"
    r"total_processing_delay_s=([0-9.]+).*?jitter_buffer_delay_s=([0-9.]+)"
)


@dataclass
class Outlier:
    trace_id: int
    tx_rtp: int
    rx_rtp: int
    e2e_ms: float
    onf_to_sink_ms: float
    v4l2_to_sink_ms: float


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("push_log")
    p.add_argument("pull_log")
    p.add_argument("--top", type=int, default=8, help="输出最慢 N 帧")
    return p.parse_args()


def load_tx(path: str) -> Dict[int, Tuple[int, int, int, int]]:
    by_trace: Dict[int, Tuple[int, int, int, int]] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_TX.search(line)
            if not m:
                continue
            rtp = int(m.group(1))
            tid = int(m.group(2))
            t_mjpeg = int(m.group(3))
            t_v4l2 = int(m.group(4))
            t_onf = int(m.group(5))
            by_trace[tid] = (rtp, t_mjpeg, t_v4l2, t_onf)
    return by_trace


def load_rx(path: str) -> Tuple[Dict[int, Tuple[int, int]], List[str]]:
    by_trace: Dict[int, Tuple[int, int]] = {}
    lines: List[str] = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            lines.append(line.rstrip("\n"))
    for line in lines:
        m = RE_RX.search(line)
        if not m:
            continue
        rtp = int(m.group(1))
        tid = int(m.group(2))
        t_sink = int(m.group(4))
        if tid not in by_trace:
            by_trace[tid] = (rtp, t_sink)
    return by_trace, lines


def build_outliers(tx: Dict[int, Tuple[int, int, int, int]], rx: Dict[int, Tuple[int, int]], top_n: int) -> List[Outlier]:
    out: List[Outlier] = []
    for tid, (rx_rtp, t_sink) in rx.items():
        if tid not in tx:
            continue
        tx_rtp, t_mjpeg, t_v4l2, t_onf = tx[tid]
        if t_mjpeg <= 0 or t_sink < t_mjpeg:
            continue
        e2e = (t_sink - t_mjpeg) / 1000.0
        onf = (t_sink - t_onf) / 1000.0 if t_onf > 0 and t_sink >= t_onf else -1.0
        v4l2 = (t_sink - t_v4l2) / 1000.0 if t_v4l2 > 0 and t_sink >= t_v4l2 else -1.0
        out.append(Outlier(tid, tx_rtp, rx_rtp, e2e, onf, v4l2))
    out.sort(key=lambda x: x.e2e_ms, reverse=True)
    return out[: max(0, top_n)]


def find_trace_line_idx(lines: List[str], trace_id: int) -> int:
    token = f"trace_id={trace_id} "
    for i, s in enumerate(lines):
        if "[E2E_RX]" in s and token in s:
            return i
    return -1


def find_nearest_inbound(lines: List[str], idx: int) -> Optional[Tuple[int, float, float, float]]:
    if idx < 0:
        return None
    for j in range(idx, max(-1, idx - 240), -1):
        m = RE_IBS.search(lines[j])
        if m:
            fd = int(m.group(1))
            td = float(m.group(2))
            tp = float(m.group(3))
            jb = float(m.group(4))
            per_decode_ms = (td / fd * 1000.0) if fd > 0 else -1.0
            per_proc_ms = (tp / fd * 1000.0) if fd > 0 else -1.0
            per_jb_ms = (jb / fd * 1000.0) if fd > 0 else -1.0
            return fd, per_decode_ms, per_proc_ms, per_jb_ms
    return None


def find_nearest_timing_delta(lines: List[str], idx: int) -> Optional[Tuple[int, int, int, int]]:
    if idx < 0:
        return None
    for j in range(idx, min(len(lines), idx + 180)):
        if "first_RTP→decode_start" not in lines[j]:
            continue
        m1 = RE_DELTA_F2D.search(lines[j])
        m2 = RE_DELTA_L2D.search(lines[j + 1] if j + 1 < len(lines) else "")
        m3 = RE_DELTA_DEC.search(lines[j + 2] if j + 2 < len(lines) else "")
        m4 = RE_DELTA_SPAN.search(lines[j + 3] if j + 3 < len(lines) else "")
        if m1 and m2 and m3 and m4:
            return int(m1.group(1)), int(m2.group(1)), int(m3.group(1)), int(m4.group(1))
    return None


def classify(delta: Optional[Tuple[int, int, int, int]]) -> str:
    if not delta:
        return "数据不足"
    first_to_dec, last_to_dec, dec_cost, span = delta
    if last_to_dec >= 12:
        return "JB/调度排队主导"
    if dec_cost >= 10:
        return "解码耗时主导"
    if span >= 10 and first_to_dec >= 10:
        return "收包跨度+到解码前排队"
    return "轻度抖动（可接受）"


def main() -> int:
    args = parse_args()
    tx = load_tx(args.push_log)
    rx, lines = load_rx(args.pull_log)
    outliers = build_outliers(tx, rx, args.top)
    if not outliers:
        print("No outliers found with trace_id alignment.")
        return 1

    print(f"Top {len(outliers)} outliers by e2e_mjpeg_input_to_sink:")
    for i, o in enumerate(outliers, 1):
        idx = find_trace_line_idx(lines, o.trace_id)
        ibs = find_nearest_inbound(lines, idx)
        delta = find_nearest_timing_delta(lines, idx)
        print(
            f"#{i:02d} trace_id={o.trace_id} tx_rtp={o.tx_rtp} rx_rtp={o.rx_rtp} "
            f"e2e={o.e2e_ms:.3f}ms onf_to_sink={o.onf_to_sink_ms:.3f}ms v4l2_to_sink={o.v4l2_to_sink_ms:.3f}ms"
        )
        if delta:
            print(
                f"    TimingDelta: first->dec={delta[0]}ms last->dec={delta[1]}ms "
                f"decode={delta[2]}ms rtp_span={delta[3]}ms  => {classify(delta)}"
            )
        else:
            print("    TimingDelta: N/A")
        if ibs:
            print(
                f"    Inbound(avg): decode={ibs[1]:.2f}ms processing={ibs[2]:.2f}ms jitter_buffer={ibs[3]:.2f}ms"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

