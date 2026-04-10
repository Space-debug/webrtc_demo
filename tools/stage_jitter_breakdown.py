#!/usr/bin/env python3
"""
按 trace_id 对齐 push/pull/ui，输出分阶段时延与抖动贡献。

阶段定义（同机 TimeMicros 可直接相减）：
  A v4l2 -> mjpeg_input                 : 采集驱动到 MJPEG 入 MPP
  B mjpeg_input -> on_frame             : MJPEG 解码/拷贝到进 WebRTC
  C on_frame -> enc_done                : 编码线程 + MPP 编码
  D enc_done -> after_onencoded         : 发送回调前置
  E after_onencoded -> sink             : 网络 + 接收队列 + 解码前等待
  F sink -> callback_done               : 接收回调尾段
  G callback_done -> present_submit     : UI 线程提交显示
  T v4l2 -> present_submit              : 软件端到端（含 UI 提交）
"""
from __future__ import annotations

import argparse
import re
import statistics
from typing import Dict, List, Tuple

RE_TX = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+)"
)
RE_RX = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) trace_id=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) "
    r"(?:wall_utc_ms=(-?\d+) )?t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
RE_UI = re.compile(
    r"\[E2E_UI\] trace_id=(\d+) t_sink_callback_done_us=(-?\d+) t_present_submit_us=(-?\d+)"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("push_log")
    p.add_argument("pull_log")
    p.add_argument("--skip-first", type=int, default=30, help="跳过前 N 个配对样本，默认 30")
    return p.parse_args()


def pct(vals: List[int], p: float) -> float:
    s = sorted(vals)
    i = min(len(s) - 1, max(0, int(round((len(s) - 1) * p))))
    return float(s[i])


def line_for(name: str, vals: List[int]) -> str:
    if not vals:
        return f"{name:28s} n=0"
    p50 = pct(vals, 0.50) / 1000.0
    p95 = pct(vals, 0.95) / 1000.0
    p99 = pct(vals, 0.99) / 1000.0
    jitter = (pct(vals, 0.99) - pct(vals, 0.50)) / 1000.0
    mean_ms = statistics.mean(vals) / 1000.0
    return (
        f"{name:28s} n={len(vals):4d}  p50={p50:8.3f}ms  p95={p95:8.3f}ms  "
        f"p99={p99:8.3f}ms  p99-p50={jitter:8.3f}ms  mean={mean_ms:8.3f}ms"
    )


def append_if_nonneg(d: Dict[str, List[int]], key: str, a: int, b: int) -> None:
    if a > 0 and b >= a:
        d[key].append(b - a)


def main() -> int:
    args = parse_args()
    tx_by_trace: Dict[int, Tuple[int, int, int, int, int]] = {}
    rx_by_trace: Dict[int, Tuple[int, int]] = {}
    ui_by_trace: Dict[int, int] = {}

    with open(args.push_log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_TX.search(line)
            if not m:
                continue
            tid = int(m.group(2))
            tx_by_trace[tid] = (
                int(m.group(4)),  # v4l2
                int(m.group(3)),  # mjpeg
                int(m.group(5)),  # onf
                int(m.group(6)),  # enc
                int(m.group(7)),  # after
            )

    with open(args.pull_log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_RX.search(line)
            if m:
                tid = int(m.group(2))
                if tid not in rx_by_trace:
                    rx_by_trace[tid] = (int(m.group(4)), int(m.group(7)))  # sink, cb_done
                continue
            m2 = RE_UI.search(line)
            if m2:
                tid = int(m2.group(1))
                if tid not in ui_by_trace:
                    ui_by_trace[tid] = int(m2.group(3))

    tids = sorted(set(tx_by_trace.keys()) & set(rx_by_trace.keys()))
    if not tids:
        print("no trace_id pairs found")
        return 1
    if args.skip_first > 0 and len(tids) > args.skip_first:
        tids = tids[args.skip_first :]

    buckets: Dict[str, List[int]] = {
        "A v4l2->mjpeg_input": [],
        "B mjpeg->on_frame": [],
        "C on_frame->enc_done": [],
        "D enc_done->after_onenc": [],
        "E after_onenc->sink": [],
        "F sink->callback_done": [],
        "G callback->present_submit": [],
        "T v4l2->present_submit": [],
        "T2 mjpeg->sink": [],
    }

    for tid in tids:
        v4l2, mjpeg, onf, enc, after = tx_by_trace[tid]
        sink, cb_done = rx_by_trace[tid]
        append_if_nonneg(buckets, "A v4l2->mjpeg_input", v4l2, mjpeg)
        append_if_nonneg(buckets, "B mjpeg->on_frame", mjpeg, onf)
        append_if_nonneg(buckets, "C on_frame->enc_done", onf, enc)
        append_if_nonneg(buckets, "D enc_done->after_onenc", enc, after)
        append_if_nonneg(buckets, "E after_onenc->sink", after, sink)
        append_if_nonneg(buckets, "F sink->callback_done", sink, cb_done)
        append_if_nonneg(buckets, "T2 mjpeg->sink", mjpeg, sink)
        if tid in ui_by_trace:
            append_if_nonneg(buckets, "G callback->present_submit", cb_done, ui_by_trace[tid])
            append_if_nonneg(buckets, "T v4l2->present_submit", v4l2, ui_by_trace[tid])

    print(f"pairs={len(tids)} (after skip-first={args.skip_first})")
    print("")
    print(line_for("A v4l2->mjpeg_input", buckets["A v4l2->mjpeg_input"]))
    print(line_for("B mjpeg->on_frame", buckets["B mjpeg->on_frame"]))
    print(line_for("C on_frame->enc_done", buckets["C on_frame->enc_done"]))
    print(line_for("D enc_done->after_onenc", buckets["D enc_done->after_onenc"]))
    print(line_for("E after_onenc->sink", buckets["E after_onenc->sink"]))
    print(line_for("F sink->callback_done", buckets["F sink->callback_done"]))
    print(line_for("G callback->present_submit", buckets["G callback->present_submit"]))
    print("")
    print(line_for("T2 mjpeg->sink", buckets["T2 mjpeg->sink"]))
    print(line_for("T v4l2->present_submit", buckets["T v4l2->present_submit"]))

    # 按 p99-p50 排序，快速看抖动主要来自哪段。
    contrib: List[Tuple[str, float]] = []
    for k, v in buckets.items():
        if len(v) >= 10:
            contrib.append((k, (pct(v, 0.99) - pct(v, 0.50)) / 1000.0))
    contrib.sort(key=lambda x: x[1], reverse=True)
    print("\nTop jitter contributors (by p99-p50):")
    for k, j in contrib[:5]:
        print(f"  {k:28s} +{j:.3f}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

