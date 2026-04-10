#!/usr/bin/env python3
"""
推流编码打点 + 拉流 InboundVideoStats + 延迟来源说明（E2E 数字请先看 parse_e2e_latency.py 输出）。

用法:
  python3 tools/analyze_latency_report.py build/e2e_last_push.log build/e2e_last_pull.log
"""
from __future__ import annotations

import re
import sys

RE_PUSH_ENC = re.compile(
    r"mjpeg_input_to_encode_done_us=(\d+).*mjpeg_input_to_after_onencoded_us=(\d+).*webrtc_onencodedimage_us=(\d+)"
)
RE_INBOUND = re.compile(
    r"\[InboundVideoStats\].*frames_decoded=(\d+).*total_decode_time_s=([\d.]+).*"
    r"total_processing_delay_s=([\d.]+).*jitter_buffer_delay_s=([\d.]+)"
)


def pct(sorted_vals: list[int], p: float) -> float:
    n = len(sorted_vals)
    i = min(n - 1, max(0, int(round((n - 1) * p))))
    return float(sorted_vals[i])


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    push_path, pull_path = sys.argv[1], sys.argv[2]

    print("========== 延迟来源分析（接在 parse_e2e_latency.py 之后看）==========")

    print("\n--- A) 推流端（服务器）：MJPEG → H.264 → OnEncodedImage ---")
    enc_us: list[int] = []
    after_us: list[int] = []
    with open(push_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_PUSH_ENC.search(line)
            if m:
                enc_us.append(int(m.group(1)))
                after_us.append(int(m.group(2)))
    if enc_us:
        enc_us.sort()
        after_us.sort()
        print(
            f"  mjpeg→encode_done:     p50={pct(enc_us, 0.5) / 1000:.2f} ms  "
            f"p95={pct(enc_us, 0.95) / 1000:.2f} ms  n={len(enc_us)}"
        )
        print(
            f"  mjpeg→after_onenc:    p50={pct(after_us, 0.5) / 1000:.2f} ms  "
            f"p95={pct(after_us, 0.95) / 1000:.2f} ms"
        )
        print("  → 说明：这是相机链 + MPP 解码 + MPP 编码 + 进 WebRTC 发送回调；通常仅数毫秒。")
    else:
        print("  （无周期编码打点；多跑帧数或等编码器每 ~120 帧日志）")

    print("\n--- B) 拉流端：WebRTC（最后一行 InboundVideoStats）---")
    last = None
    with open(pull_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_INBOUND.search(line)
            if m:
                last = m
    if last:
        dec_frames = int(last.group(1))
        t_dec = float(last.group(2))
        t_proc = float(last.group(3))
        t_jb = float(last.group(4))
        per_jb_ms = (t_jb / dec_frames * 1000.0) if dec_frames else 0.0
        per_dec_ms = (t_dec / dec_frames * 1000.0) if dec_frames else 0.0
        per_proc_ms = (t_proc / dec_frames * 1000.0) if dec_frames else 0.0
        print(f"  frames_decoded={dec_frames}")
        print(f"  平均每帧累计 jitter_buffer_delay ≈ {per_jb_ms:.2f} ms（收包抖动缓冲主因）")
        print(f"  平均每帧 total_processing_delay   ≈ {per_proc_ms:.2f} ms（含 JB+解码+调度）")
        print(f"  平均每帧 total_decode_time        ≈ {per_dec_ms:.2f} ms（纯解码耗时）")
    else:
        print("  （无 InboundVideoStats；用 E2E_VIDEO_STATS=1 跑 pull.sh --e2e）")

    print("\n--- C) 小结 ---")
    print("  • e2e_mjpeg_input_to_sink − e2e_onframe_to_sink ≈ 采集/解码锚点到进 WebRTC 前（通常很小）。")
    print("  • 与「推流编码仅数 ms」的差，主要在：RTP pacer、网络、收端 JB、解码与渲染调度。")
    print("  • 要「极低延迟 + 画面稳」：采集用 FIFO 小队列（勿 latest-only 狂丢）、NV12/码率留余量、")
    print("    收端 ZeroPlayout 的 decode_queue 勿过小（易卡顿）；弱网可设 WEBRTC_DEMO_ENABLE_FLEXFEC=1。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
