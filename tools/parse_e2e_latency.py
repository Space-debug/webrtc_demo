#!/usr/bin/env python3
"""
合并推流/拉流两份日志里的 [E2E_TX] / [E2E_RX]，配对后输出多段延迟。

配对优先级（可信度高 → 低）：
  1) trace_id：发送端与接收端 VideoFrame::id()（需 MPP 解码传播 tracking id）
  2) RTP 时间戳完全一致
  3) 推断固定 RTP 偏移 — 易误配帧，脚本会明确警告

时钟口径：
  • e2e_*（除 wall 外）：webrtc::TimeMicros，同机/单板可直接比；两机仅相对趋势可信。
  • e2e_wall_utc_mjpeg_to_sink：发送端 MJPEG 入 MPP 时的 TimeUTCMillis → 接收端 OnFrame 时
    TimeUTCMillis 之差（毫秒）。两台 PC 需 Chrony/NTP 同步（建议同一局域网 pool 或一台为
    server），否则会出现整体偏移或负延迟。

用法:
  python3 tools/parse_e2e_latency.py build/e2e_last_push.log build/e2e_last_pull.log
"""
from __future__ import annotations

import argparse
import os
import re
import statistics
import sys
from collections import defaultdict
from typing import Tuple

RE_TX = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+) wall_utc_ms=(-?\d+)"
)
RE_TX_OLD = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+)"
)
RE_RX = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) trace_id=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) wall_utc_ms=(-?\d+) "
    r"t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
RE_RX_NO_WALL = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) trace_id=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) "
    r"t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
RE_RX_LEGACY = re.compile(
    r"\[E2E_RX\] rtp_ts=(\d+) frame_id=(\d+) t_sink_us=(-?\d+) "
    r"t_argb_done_us=(-?\d+) t_callback_done_us=(-?\d+)"
)
RE_UI = re.compile(r"\[E2E_UI\] trace_id=(\d+) t_sink_callback_done_us=(-?\d+) t_present_submit_us=(-?\d+)")
U32 = 1 << 32

# tx tuple: trace_id, t_mjpeg, t_v4l2, t_onf, t_enc, t_after, wall_tx_ms (-1 = unknown)
TxTuple = Tuple[int, int, int, int, int, int, int]
# rx tuple: t_sink_us, t_argb_done_us, t_cb_done_us, wall_rx_ms (-1 = unknown)
RxTuple = Tuple[int, int, int, int]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="合并推流/拉流日志，输出 E2E 统计。",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("push_log", help="推流日志路径")
    p.add_argument("pull_log", help="拉流日志路径")
    p.add_argument(
        "--dump-outliers",
        type=int,
        default=0,
        metavar="N",
        help="打印最慢 N 帧（按 e2e_mjpeg_input_to_sink 排序，需 trace/direct 配对）",
    )
    p.add_argument(
        "--skip-first-pairs",
        type=int,
        default=0,
        metavar="N",
        help="稳态分析时跳过前 N 个已配对样本（用于排除起播瞬态）",
    )
    return p.parse_args()


def infer_rtp_offset(tx_keys: list[int], rx_keys: list[int]) -> int | None:
    """推断 rx_rtp ~= (tx_rtp + offset) mod 2^32 的固定偏移（易错，仅作兜底）。"""
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
    return best_off if best_cnt >= 4 else None


def percentile_sorted(sorted_values: list[int], p: float) -> int:
    n = len(sorted_values)
    if n == 0:
        return 0
    i = min(n - 1, max(0, int(round((n - 1) * p))))
    return sorted_values[i]


def print_e2e_summary(buckets: dict[str, list[int]], pairing_label: str) -> None:
    """主口径：发送端 MJPEG 进入解码链时刻 → 接收端 VideoSink::OnFrame 入口（微秒时钟）。"""
    v = buckets.get("e2e_mjpeg_input_to_sink", [])
    if len(v) < 4:
        return
    v = sorted(v)
    p50 = percentile_sorted(v, 0.50)
    p95 = percentile_sorted(v, 0.95)
    p99 = percentile_sorted(v, 0.99)
    print(
        f"\n[E2E_SUMMARY] pairing={pairing_label}  "
        f"MJPEG入队(t_mjpeg_input_us)→接收OnFrame入口(t_sink_us): "
        f"p50={p50/1000.0:.3f}ms p95={p95/1000.0:.3f}ms p99={p99/1000.0:.3f}ms n={len(v)}"
    )
    warn_ms = 30
    if "WEBRTC_E2E_WARN_MS" in os.environ:
        try:
            vms = int(os.environ["WEBRTC_E2E_WARN_MS"])
            if 1 <= vms <= 5000:
                warn_ms = vms
        except ValueError:
            pass
    warn_us = warn_ms * 1000
    over = sum(1 for x in v if x > warn_us)
    if over:
        print(
            f"  [E2E_TAIL] over_{warn_ms}ms={over}/{len(v)} "
            f"({over * 100.0 / len(v):.2f}%) max={v[-1]/1000.0:.3f}ms"
        )
    print(
        "  时钟: webrtc::TimeMicros。同机双进程可直接读；两台 PC 请优先看下方 [E2E_WALL_SUMMARY]（UTC），"
        "并对两台机做 chrony 同步。"
    )

    w = buckets.get("e2e_wall_utc_mjpeg_to_sink", [])
    if len(w) >= 4:
        w = sorted(w)
        wp50 = percentile_sorted(w, 0.50)
        wp95 = percentile_sorted(w, 0.95)
        wp99 = percentile_sorted(w, 0.99)
        neg = sum(1 for x in w if x < 0)
        print(
            f"\n[E2E_WALL_SUMMARY] pairing={pairing_label}  "
            f"UTC wall: 发送 MJPEG入MPP(wall_utc_ms)→接收 OnFrame(wall_utc_ms): "
            f"p50={wp50}ms p95={wp95}ms p99={wp99}ms n={len(w)}"
        )
        if neg > len(w) // 4:
            print(
                f"  警告: {neg}/{len(w)} 样本为负，多半是两机时钟未对齐；请先配置 chrony（例如同一 ntp pool 或 "
                "推流机 chrony server、拉流机 client）。"
            )
        else:
            print(
                "  两台 PC 请在两端执行: chronyc tracking（查看 System time / Last offset）；"
                "offset 稳定在亚毫秒～数毫秒级后，上式才接近真实网络+处理端到端。"
            )


def print_primary_path_summary(buckets: dict[str, list[int]], pairing_label: str) -> None:
    def p_ms(name: str, p: float) -> float | None:
        vals = buckets.get(name, [])
        if len(vals) < 4:
            return None
        s = sorted(vals)
        return percentile_sorted(s, p) / 1000.0

    v2s_p50 = p_ms("e2e_v4l2_to_sink", 0.50)
    v2s_p95 = p_ms("e2e_v4l2_to_sink", 0.95)
    v2s_p99 = p_ms("e2e_v4l2_to_sink", 0.99)
    v2p_p50 = p_ms("e2e_v4l2_to_present_submit", 0.50)
    v2p_p95 = p_ms("e2e_v4l2_to_present_submit", 0.95)
    v2p_p99 = p_ms("e2e_v4l2_to_present_submit", 0.99)
    m2s_p50 = p_ms("e2e_mjpeg_input_to_sink", 0.50)
    m2s_p95 = p_ms("e2e_mjpeg_input_to_sink", 0.95)
    m2s_p99 = p_ms("e2e_mjpeg_input_to_sink", 0.99)

    print(f"\n[PRIMARY_PATH] pairing={pairing_label}")
    if v2p_p50 is not None:
        print(
            f"  #1 v4l2->present_submit: p50={v2p_p50:.3f}ms p95={v2p_p95:.3f}ms p99={v2p_p99:.3f}ms"
        )
    else:
        print("  #1 v4l2->present_submit: (无样本，通常因为 headless 未产生 E2E_UI)")
    if v2s_p50 is not None:
        print(f"  #2 v4l2->sink: p50={v2s_p50:.3f}ms p95={v2s_p95:.3f}ms p99={v2s_p99:.3f}ms")
    if m2s_p50 is not None:
        print(f"  #3 mjpeg_input->sink: p50={m2s_p50:.3f}ms p95={m2s_p95:.3f}ms p99={m2s_p99:.3f}ms")
    if v2p_p50 is not None and v2s_p50 is not None:
        print(
            f"  delta(show submit - sink): p50={v2p_p50 - v2s_p50:.3f}ms "
            f"p95={v2p_p95 - v2s_p95:.3f}ms p99={v2p_p99 - v2s_p99:.3f}ms"
        )


def print_stats(name: str, values: list[int], note: str = "") -> None:
    if not values:
        return
    values.sort()
    n = len(values)

    mean_v = statistics.mean(values)
    p50 = percentile_sorted(values, 0.50)
    p95 = percentile_sorted(values, 0.95)
    p99 = percentile_sorted(values, 0.99)
    suffix = f"  {note}" if note else ""
    print(
        f"{name}: n={n} "
        f"us[min={values[0]} p50={p50} p95={p95} p99={p99} max={values[-1]} mean={mean_v:.1f}] "
        f"ms[min={values[0]/1000.0:.3f} p50={p50/1000.0:.3f} "
        f"p95={p95/1000.0:.3f} p99={p99/1000.0:.3f} max={values[-1]/1000.0:.3f} mean={mean_v/1000.0:.3f}]{suffix}"
    )


def print_stats_wall_ms(name: str, values: list[int], note: str = "") -> None:
    """延迟已为毫秒（UTC 墙钟差）。"""
    if not values:
        return
    values.sort()
    n = len(values)
    mean_v = statistics.mean(values)
    p50 = percentile_sorted(values, 0.50)
    p95 = percentile_sorted(values, 0.95)
    p99 = percentile_sorted(values, 0.99)
    suffix = f"  {note}" if note else ""
    print(
        f"{name}: n={n} "
        f"ms[min={values[0]} p50={p50} p95={p95} p99={p99} max={values[-1]} mean={mean_v:.1f}]{suffix}"
    )


def collect_deltas(tx: TxTuple, rx: RxTuple) -> dict[str, int]:
    """返回各段延迟；e2e_wall_* 单位为毫秒，其余为微秒。"""
    _trace_id, t_mjpeg, t_v4l2, t_onf, _t_enc_done, _t_after, wall_tx = tx
    t_sink, t_argb_done, t_cb_done, wall_rx = rx
    out: dict[str, int] = {}
    if wall_tx > 0 and wall_rx > 0:
        out["e2e_wall_utc_mjpeg_to_sink"] = wall_rx - wall_tx
    if t_mjpeg > 0 and t_sink >= t_mjpeg:
        out["e2e_mjpeg_input_to_sink"] = t_sink - t_mjpeg
    if t_v4l2 > 0 and t_sink >= t_v4l2:
        out["e2e_v4l2_to_sink"] = t_sink - t_v4l2
    if t_onf > 0 and t_sink >= t_onf:
        out["e2e_onframe_to_sink"] = t_sink - t_onf
    if t_mjpeg > 0 and t_argb_done >= t_mjpeg:
        out["e2e_mjpeg_input_to_argb_done"] = t_argb_done - t_mjpeg
    if t_mjpeg > 0 and t_cb_done >= t_mjpeg:
        out["e2e_mjpeg_input_to_callback_done"] = t_cb_done - t_mjpeg
    if t_argb_done >= t_sink:
        out["rx_sink_to_argb_done"] = t_argb_done - t_sink
    if t_cb_done >= t_argb_done:
        out["rx_argb_done_to_callback_done"] = t_cb_done - t_argb_done
    return out


def build_pair_record(tx_rtp: int, tx: TxTuple, rx_rtp: int, rx: RxTuple) -> dict[str, int]:
    rec = collect_deltas(tx, rx)
    rec["trace_id"] = tx[0]
    rec["tx_rtp_ts"] = tx_rtp
    rec["rx_rtp_ts"] = rx_rtp
    return rec


def merge_into(buckets: dict[str, list[int]], deltas: dict[str, int]) -> None:
    for k, v in deltas.items():
        if k in buckets:
            buckets[k].append(v)


def flush_bucket_print(buckets: dict[str, list[int]], note: str) -> None:
    print_stats_wall_ms("e2e_wall_utc_mjpeg_to_sink", buckets["e2e_wall_utc_mjpeg_to_sink"], note)
    print_stats("e2e_v4l2_to_sink", buckets["e2e_v4l2_to_sink"], note)
    print_stats("e2e_v4l2_to_present_submit", buckets["e2e_v4l2_to_present_submit"], note)
    print_stats("e2e_mjpeg_input_to_sink", buckets["e2e_mjpeg_input_to_sink"], note)
    print_stats("e2e_mjpeg_input_to_present_submit", buckets["e2e_mjpeg_input_to_present_submit"], note)
    print_stats("e2e_onframe_to_sink", buckets["e2e_onframe_to_sink"], note)
    print_stats("e2e_mjpeg_input_to_argb_done", buckets["e2e_mjpeg_input_to_argb_done"], note)
    print_stats("e2e_mjpeg_input_to_callback_done", buckets["e2e_mjpeg_input_to_callback_done"], note)
    print_stats("rx_sink_callback_to_present_submit", buckets["rx_sink_callback_to_present_submit"], note)
    print_stats("rx_sink_to_argb_done", buckets["rx_sink_to_argb_done"], note)
    print_stats("rx_argb_done_to_callback_done", buckets["rx_argb_done_to_callback_done"], note)


def print_outliers(records: list[dict[str, int]], top_n: int) -> None:
    if top_n <= 0:
        return
    key = "e2e_mjpeg_input_to_sink"
    valid = [r for r in records if key in r]
    if not valid:
        print("\n[OUTLIERS] 无可用样本（缺少 e2e_mjpeg_input_to_sink）。")
        return
    valid.sort(key=lambda r: r[key], reverse=True)
    n = min(top_n, len(valid))
    print(f"\n[OUTLIERS] top {n} by {key} (trace/direct 配对结果)")
    for i, r in enumerate(valid[:n], 1):
        print(
            "  #{:02d} trace_id={} tx_rtp={} rx_rtp={} e2e={:.3f}ms onf_to_sink={:.3f}ms v4l2_to_sink={:.3f}ms".format(
                i,
                r.get("trace_id", -1),
                r.get("tx_rtp_ts", -1),
                r.get("rx_rtp_ts", -1),
                r.get("e2e_mjpeg_input_to_sink", -1) / 1000.0,
                r.get("e2e_onframe_to_sink", -1) / 1000.0 if "e2e_onframe_to_sink" in r else -1.0,
                r.get("e2e_v4l2_to_sink", -1) / 1000.0 if "e2e_v4l2_to_sink" in r else -1.0,
            )
        )


def buckets_from_records(records: list[dict[str, int]]) -> dict[str, list[int]]:
    b = empty_buckets()
    for r in records:
        for k in b.keys():
            if k in r:
                b[k].append(r[k])
    return b


def empty_buckets() -> dict[str, list[int]]:
    return {k: [] for k in [
        "e2e_wall_utc_mjpeg_to_sink",
        "e2e_v4l2_to_sink",
        "e2e_v4l2_to_present_submit",
        "e2e_mjpeg_input_to_sink",
        "e2e_mjpeg_input_to_present_submit",
        "e2e_onframe_to_sink",
        "e2e_mjpeg_input_to_argb_done",
        "e2e_mjpeg_input_to_callback_done",
        "rx_sink_callback_to_present_submit",
        "rx_sink_to_argb_done",
        "rx_argb_done_to_callback_done",
    ]}


def parse_tx_line(line: str) -> tuple[int, TxTuple] | None:
    m = RE_TX.search(line)
    if m:
        rtp = int(m.group(1))
        trace = int(m.group(2))
        tup: TxTuple = (
            trace,
            int(m.group(3)),
            int(m.group(4)),
            int(m.group(5)),
            int(m.group(6)),
            int(m.group(7)),
            int(m.group(8)),
        )
        return rtp, tup
    m = RE_TX_OLD.search(line)
    if m:
        rtp = int(m.group(1))
        trace = int(m.group(2))
        tup = (
            trace,
            int(m.group(3)),
            int(m.group(4)),
            int(m.group(5)),
            int(m.group(6)),
            int(m.group(7)),
            -1,
        )
        return rtp, tup
    return None


def main() -> int:
    args = parse_args()
    push_path, pull_path = args.push_log, args.pull_log

    tx_by_rtp: dict[int, TxTuple] = {}
    tx_by_trace: dict[int, TxTuple] = {}
    tx_rtp_by_trace: dict[int, int] = {}
    with open(push_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parsed = parse_tx_line(line)
            if parsed:
                rtp, tup = parsed
                tx_by_rtp[rtp] = tup
                if tup[0] != 0:
                    tx_by_trace[tup[0]] = tup
                    tx_rtp_by_trace[tup[0]] = rtp

    rx_first_rtp: dict[int, RxTuple] = {}
    rx_first_trace: dict[int, RxTuple] = {}
    rx_rtp_by_trace: dict[int, int] = {}
    ui_present_by_trace: dict[int, int] = {}
    rx_count: defaultdict[int, int] = defaultdict(int)
    legacy_rx_lines = 0
    no_wall_rx_lines = 0
    with open(pull_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_RX.search(line)
            if m:
                rtp = int(m.group(1))
                trace = int(m.group(2))
                sink = int(m.group(4))
                wall = int(m.group(5))
                argb_done = int(m.group(6))
                cb_done = int(m.group(7))
                rx: RxTuple = (sink, argb_done, cb_done, wall)
                rx_count[rtp] += 1
                if rtp not in rx_first_rtp:
                    rx_first_rtp[rtp] = rx
                if trace != 0 and trace not in rx_first_trace:
                    rx_first_trace[trace] = rx
                    rx_rtp_by_trace[trace] = rtp
                continue
            m = RE_RX_NO_WALL.search(line)
            if m:
                no_wall_rx_lines += 1
                rtp = int(m.group(1))
                trace = int(m.group(2))
                sink = int(m.group(4))
                argb_done = int(m.group(5))
                cb_done = int(m.group(6))
                rx = (sink, argb_done, cb_done, -1)
                rx_count[rtp] += 1
                if rtp not in rx_first_rtp:
                    rx_first_rtp[rtp] = rx
                if trace != 0 and trace not in rx_first_trace:
                    rx_first_trace[trace] = rx
                    rx_rtp_by_trace[trace] = rtp
                continue
            m2 = RE_RX_LEGACY.search(line)
            if m2:
                legacy_rx_lines += 1
                rtp = int(m2.group(1))
                sink = int(m2.group(3))
                argb_done = int(m2.group(4))
                cb_done = int(m2.group(5))
                rx = (sink, argb_done, cb_done, -1)
                rx_count[rtp] += 1
                if rtp not in rx_first_rtp:
                    rx_first_rtp[rtp] = rx
                continue
            m3 = RE_UI.search(line)
            if m3:
                tid = int(m3.group(1))
                t_present = int(m3.group(3))
                if tid != 0 and t_present > 0 and tid not in ui_present_by_trace:
                    ui_present_by_trace[tid] = t_present

    if legacy_rx_lines:
        print(
            f"注意: 拉流日志含 {legacy_rx_lines} 行旧格式 [E2E_RX]（无 trace_id）；"
            "请用新版二进制重新采集以启用 trace_id 配对。"
        )
    if no_wall_rx_lines:
        print(
            f"注意: {no_wall_rx_lines} 行 [E2E_RX] 无 wall_utc_ms，跨两台 PC 的 UTC 端到端将无样本；"
            "请更新拉流端二进制。"
        )

    trace_buckets = empty_buckets()
    trace_records: list[dict[str, int]] = []
    trace_pairs = 0
    for tid, rx in rx_first_trace.items():
        if tid not in tx_by_trace:
            continue
        tx = tx_by_trace[tid]
        rec = build_pair_record(tx_rtp_by_trace.get(tid, -1), tx, rx_rtp_by_trace.get(tid, -1), rx)
        if tid in ui_present_by_trace and tx[2] > 0 and ui_present_by_trace[tid] >= tx[2]:
            rec["e2e_v4l2_to_present_submit"] = ui_present_by_trace[tid] - tx[2]
        if tid in ui_present_by_trace and tx[1] > 0 and ui_present_by_trace[tid] >= tx[1]:
            rec["e2e_mjpeg_input_to_present_submit"] = ui_present_by_trace[tid] - tx[1]
        if tid in ui_present_by_trace and rx[2] > 0 and ui_present_by_trace[tid] >= rx[2]:
            rec["rx_sink_callback_to_present_submit"] = ui_present_by_trace[tid] - rx[2]
        merge_into(trace_buckets, rec)
        trace_records.append(rec)
        trace_pairs += 1

    direct_buckets = empty_buckets()
    direct_records: list[dict[str, int]] = []
    direct_pairs = 0
    for rtp, rx in rx_first_rtp.items():
        if rtp not in tx_by_rtp:
            continue
        rec = build_pair_record(rtp, tx_by_rtp[rtp], rtp, rx)
        merge_into(direct_buckets, rec)
        direct_records.append(rec)
        direct_pairs += 1

    inferred_offset: int | None = None
    offset_buckets = empty_buckets()
    offset_records: list[dict[str, int]] = []
    offset_pairs = 0
    if direct_pairs == 0:
        tx_keys = sorted(tx_by_rtp.keys())
        rx_keys = sorted(rx_first_rtp.keys())
        inferred_offset = infer_rtp_offset(tx_keys, rx_keys)
        if inferred_offset is not None:
            for rx_rtp, rx in rx_first_rtp.items():
                tx_rtp = (rx_rtp - inferred_offset) % U32
                if tx_rtp not in tx_by_rtp:
                    continue
                rec = build_pair_record(tx_rtp, tx_by_rtp[tx_rtp], rx_rtp, rx)
                merge_into(offset_buckets, rec)
                offset_records.append(rec)
                offset_pairs += 1

    print(
        f"TX rtp keys={len(tx_by_rtp)} trace keys={len(tx_by_trace)}  |  "
        f"RX first-seen rtp={len(rx_first_rtp)} trace={len(rx_first_trace)}"
    )
    print(
        f"paired: trace_id={trace_pairs}  direct_rtp={direct_pairs}  "
        f"rtp_offset_inferred={offset_pairs if inferred_offset is not None else 0}"
    )

    primary_buckets: dict[str, list[int]] | None = None
    primary_records: list[dict[str, int]] | None = None
    primary_label = ""

    if trace_pairs >= 8:
        print("\n=== 以下按 trace_id 配对（推荐，跨进程最可信）===\n")
        flush_bucket_print(trace_buckets, "")
        primary_buckets, primary_label = trace_buckets, "trace_id"
        primary_records = trace_records
    elif direct_pairs >= 8:
        print("\n=== 以下按 RTP 时间戳完全一致配对 ===\n")
        flush_bucket_print(direct_buckets, "")
        primary_buckets, primary_label = direct_buckets, "direct_rtp"
        primary_records = direct_records
    elif offset_pairs > 0:
        print(
            "\n*** 警告: 未找到足够的 trace_id / 直接 RTP 配对，"
            "已退回「RTP 偏移推断」。极易把不同帧配在一起，"
            "p50 可能出现数百毫秒假象；请勿当作真实 LAN 时延。 ***\n"
        )
        print(f"推断偏移: rx_rtp ≡ tx_rtp + {inferred_offset} (mod 2^32)\n")
        flush_bucket_print(offset_buckets, "[推断配对，不可信]")
        primary_buckets, primary_label = offset_buckets, "rtp_offset_inferred_UNRELIABLE"
        primary_records = offset_records
    else:
        print("No pairs; check WEBRTC_E2E_LATENCY_TRACE=1 on both processes and overlapping run window.")
        return 1

    if primary_buckets is not None:
        print_e2e_summary(primary_buckets, primary_label)
        print_primary_path_summary(primary_buckets, primary_label)
    records_for_outliers = primary_records if primary_records is not None else []
    if primary_records is not None and args.skip_first_pairs > 0:
        skip_n = args.skip_first_pairs
        if len(primary_records) <= skip_n:
            print(
                f"\n[STEADY_STATE] skip-first-pairs={skip_n} 但样本仅 {len(primary_records)}，无法计算稳态统计。"
            )
        else:
            steady_records = primary_records[skip_n:]
            steady_buckets = buckets_from_records(steady_records)
            print(
                f"\n=== 稳态统计（跳过前 {skip_n} 个配对样本）===\n"
            )
            flush_bucket_print(steady_buckets, "[steady]")
            print_e2e_summary(steady_buckets, f"{primary_label}_steady")
            print_primary_path_summary(steady_buckets, f"{primary_label}_steady")
            records_for_outliers = steady_records
    if records_for_outliers:
        print_outliers(records_for_outliers, args.dump_outliers)

    if trace_pairs >= 8 and direct_pairs >= 8 and direct_pairs != trace_pairs:
        print("\n--- 对照: 直接 RTP 配对（若与上表差异大，说明 rtp_ts 与 trace 不一致）---\n")
        flush_bucket_print(direct_buckets, "[direct rtp]")

    if inferred_offset is not None and offset_pairs > 0 and (trace_pairs >= 8 or direct_pairs >= 8):
        print("\n--- 对照: 推断偏移配对（仅供参考）---\n")
        flush_bucket_print(offset_buckets, "[推断]")

    dups = sum(1 for rtp, c in rx_count.items() if c > 1)
    if dups:
        print(f"Note: {dups} rtp_ts had multiple RX lines (used first t_sink_us only).")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
