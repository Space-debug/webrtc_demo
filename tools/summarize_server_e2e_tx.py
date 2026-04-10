#!/usr/bin/env python3
"""
从推流日志中每条 [E2E_TX] 推导「服务器侧」分段耗时（微秒），需 WEBRTC_E2E_LATENCY_TRACE=1。

分段（同一进程时钟，可直接相减）:
  mjpeg→enc_done     : t_enc_done − t_mjpeg_input（MJPEG 锚点 → 硬编完成）
  on_frame→enc_done  : t_enc_done − t_on_frame（进 WebRTC 编码入口 → 硬编完成；反映 VCM/线程排队）
  enc_done→after_cb   : t_after_onencoded − t_enc_done（OnEncodedImage 及发送链前置）
"""
from __future__ import annotations

import re
import statistics
import sys

RE_TX = re.compile(
    r"\[E2E_TX\] rtp_ts=(\d+) trace_id=(\d+) t_mjpeg_input_us=(-?\d+) t_v4l2_us=(-?\d+) "
    r"t_on_frame_us=(-?\d+) t_enc_done_us=(-?\d+) t_after_onencoded_us=(-?\d+)"
)


def pct(sorted_vals: list[int], p: float) -> int:
    n = len(sorted_vals)
    i = min(n - 1, max(0, int(round((n - 1) * p))))
    return sorted_vals[i]


def show(name: str, vals: list[int]) -> None:
    if not vals:
        print(f"{name}: (无有效样本)")
        return
    vals.sort()
    n = len(vals)
    m = statistics.mean(vals)
    print(
        f"{name}: n={n} us  min={vals[0]} p50={pct(vals, 0.5)} p95={pct(vals, 0.95)} max={vals[-1]} mean={m:.0f}  "
        f"| ms p50={pct(vals, 0.5) / 1000.0:.3f}"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: python3 tools/summarize_server_e2e_tx.py build/e2e_last_push.log", file=sys.stderr)
        return 2
    path = sys.argv[1]
    d_mjpeg_enc: list[int] = []
    d_mjpeg_onf: list[int] = []
    d_onf_enc: list[int] = []
    d_enc_after: list[int] = []

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_TX.search(line)
            if not m:
                continue
            t_mj = int(m.group(3))
            t_onf = int(m.group(5))
            t_enc = int(m.group(6))
            t_aft = int(m.group(7))
            if t_mj > 0 and t_enc >= t_mj:
                d_mjpeg_enc.append(t_enc - t_mj)
            if t_mj > 0 and t_onf >= t_mj:
                d_mjpeg_onf.append(t_onf - t_mj)
            if t_onf > 0 and t_enc >= t_onf:
                d_onf_enc.append(t_enc - t_onf)
            if t_enc > 0 and t_aft >= t_enc:
                d_enc_after.append(t_aft - t_enc)

    print("=== 服务器（推流）侧：逐帧 [E2E_TX] 分段（仅分析，不碰客户端）===")
    show("mjpeg_input → enc_done (总：解码链+硬编)", d_mjpeg_enc)
    show("mjpeg_input → on_frame (MJPEG→NV12→进 WebRTC)", d_mjpeg_onf)
    show("on_frame → enc_done (VCM/编码线程→MPP 出包)", d_onf_enc)
    show("enc_done → after_OnEncodedImage_cb (WebRTC 发送回调)", d_enc_after)
    print()
    print("「不能再降」的典型边界：")
    print("  • mjpeg→enc_done 已 ~3–5ms 时，硬编+MPP 解码已接近板子极限。")
    print("  • on_frame→enc_done 若偶发很大，查 CPU 抢占或其它轨道负载。")
    print("  • 周期日志里的 usb_to_frame_timestamp_us 大：相机/USB 驱动时间线，应用层难再压缩。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
