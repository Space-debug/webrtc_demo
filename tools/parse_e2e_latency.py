#!/usr/bin/env python3
"""
合并同一块板子上推流/拉流两份日志里的 [E2E_TX] / [E2E_RX]，按 rtp_ts 配对，
输出更长链路多段延迟统计（均为 webrtc::TimeMicros）。

用法:
  python3 tools/parse_e2e_latency.py build/e2e_last_push.log build/e2e_last_pull.log
"""
from __future__ import annotations

import re
import statistics
import sys
from collections import defaultdict

RE_TX = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+)"
)
RE_RX = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) "
    r"t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
U32 = 1 << 32


def infer_rtp_offset(tx_keys: list[int], rx_keys: list[int]) -> int | None:
    """推断 rx_rtp ~= (tx_rtp + offset) mod 2^32 的固定偏移。"""
    if not tx_keys or not rx_keys:
        return None
    tx_probe = tx_keys[:120]
    rx_probe = rx_keys[:120]
    score: defaultdict[int, int] = defaultdict(int)
    for tx in tx_probe:
        for rx in rx_probe:
            score[(rx - tx) % U32] += 1
    if not score:
        return None
    best_off, best_cnt = max(score.items(), key=lambda kv: kv[1])
    # 仅在出现足够多次时接受，避免误配。
    return best_off if best_cnt >= 4 else None


def print_stats(name: str, values: list[int]) -> None:
    if not values:
        return
    values.sort()
    n = len(values)

    def pct(p: float) -> int:
        i = min(n - 1, max(0, int(round((n - 1) * p))))
        return values[i]

    mean_v = statistics.mean(values)
    print(
        f"{name}: n={n} "
        f"us[min={values[0]} p50={pct(0.50)} p95={pct(0.95)} max={values[-1]} mean={mean_v:.1f}] "
        f"ms[min={values[0]/1000.0:.3f} p50={pct(0.50)/1000.0:.3f} "
        f"p95={pct(0.95)/1000.0:.3f} max={values[-1]/1000.0:.3f} mean={mean_v/1000.0:.3f}]"
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    push_path, pull_path = sys.argv[1], sys.argv[2]

    tx_by_rtp: dict[int, tuple[int, int, int, int, int, int]] = {}
    with open(push_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_TX.search(line)
            if m:
                rtp = int(m.group(1))
                tx_by_rtp[rtp] = (
                    int(m.group(2)),  # trace_id
                    int(m.group(3)),  # t_mjpeg_input_us
                    int(m.group(4)),  # t_v4l2_us
                    int(m.group(5)),  # t_on_frame_us
                    int(m.group(6)),  # t_enc_done_us
                    int(m.group(7)),  # t_after_onencoded_us
                )

    rx_first: dict[int, tuple[int, int, int]] = {}
    rx_count: defaultdict[int, int] = defaultdict(int)
    with open(pull_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_RX.search(line)
            if m:
                rtp = int(m.group(1))
                sink = int(m.group(3))
                argb_done = int(m.group(4))
                cb_done = int(m.group(5))
                rx_count[rtp] += 1
                if rtp not in rx_first:
                    rx_first[rtp] = (sink, argb_done, cb_done)

    # 多段统计集合。
    d_mjpeg_to_sink: list[int] = []
    d_v4l2_to_sink: list[int] = []
    d_onframe_to_sink: list[int] = []
    d_mjpeg_to_argb: list[int] = []
    d_mjpeg_to_cb: list[int] = []
    d_sink_to_argb: list[int] = []
    d_argb_to_cb: list[int] = []
    inferred_offset = None

    def add_pair(tx: tuple[int, int, int, int, int, int], rx: tuple[int, int, int]) -> None:
        _trace_id, t_mjpeg, t_v4l2, t_onf, _t_enc_done, _t_after = tx
        t_sink, t_argb_done, t_cb_done = rx
        if t_mjpeg > 0 and t_sink >= t_mjpeg:
            d_mjpeg_to_sink.append(t_sink - t_mjpeg)
        if t_v4l2 > 0 and t_sink >= t_v4l2:
            d_v4l2_to_sink.append(t_sink - t_v4l2)
        if t_onf > 0 and t_sink >= t_onf:
            d_onframe_to_sink.append(t_sink - t_onf)
        if t_mjpeg > 0 and t_argb_done >= t_mjpeg:
            d_mjpeg_to_argb.append(t_argb_done - t_mjpeg)
        if t_mjpeg > 0 and t_cb_done >= t_mjpeg:
            d_mjpeg_to_cb.append(t_cb_done - t_mjpeg)
        if t_argb_done >= t_sink:
            d_sink_to_argb.append(t_argb_done - t_sink)
        if t_cb_done >= t_argb_done:
            d_argb_to_cb.append(t_cb_done - t_argb_done)

    paired = 0
    for rtp, rx in rx_first.items():
        if rtp not in tx_by_rtp:
            continue
        add_pair(tx_by_rtp[rtp], rx)
        paired += 1

    if paired == 0:
        tx_keys = sorted(tx_by_rtp.keys())
        rx_keys = sorted(rx_first.keys())
        inferred_offset = infer_rtp_offset(tx_keys, rx_keys)
        if inferred_offset is not None:
            for rx_rtp, rx in rx_first.items():
                tx_rtp = (rx_rtp - inferred_offset) % U32
                if tx_rtp not in tx_by_rtp:
                    continue
                add_pair(tx_by_rtp[tx_rtp], rx)
                paired += 1

    print(f"TX matched rtp: {len(tx_by_rtp)}  RX first-seen rtp: {len(rx_first)}  paired: {paired}")
    if paired == 0:
        print("No pairs; check WEBRTC_E2E_LATENCY_TRACE=1 on both processes and overlapping run window.")
        return 1
    if inferred_offset is not None:
        print(f"RTP offset inferred: +{inferred_offset} (rx = tx + offset mod 2^32)")

    print_stats("e2e_v4l2_to_sink", d_v4l2_to_sink)
    print_stats("e2e_mjpeg_input_to_sink", d_mjpeg_to_sink)
    print_stats("e2e_onframe_to_sink", d_onframe_to_sink)
    print_stats("e2e_mjpeg_input_to_argb_done", d_mjpeg_to_argb)
    print_stats("e2e_mjpeg_input_to_callback_done", d_mjpeg_to_cb)
    print_stats("rx_sink_to_argb_done", d_sink_to_argb)
    print_stats("rx_argb_done_to_callback_done", d_argb_to_cb)

    dups = sum(1 for rtp, c in rx_count.items() if c > 1)
    if dups:
        print(f"Note: {dups} rtp_ts had multiple RX lines (used first t_sink_us only).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
