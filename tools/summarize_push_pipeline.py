#!/usr/bin/env python3
"""从推流日志中提取 MPP 周期打点（需编码器跑过足够帧，约每 120 帧一条）。"""
from __future__ import annotations

import re
import statistics
import sys

RE = re.compile(
    r"mjpeg_input_to_encode_done_us=(\d+).*mjpeg_input_to_after_onencoded_us=(\d+).*webrtc_onencodedimage_us=(\d+)"
)


def pct(sorted_vals: list[int], p: float) -> int:
    n = len(sorted_vals)
    i = min(n - 1, max(0, int(round((n - 1) * p))))
    return sorted_vals[i]


def main() -> int:
    if len(sys.argv) != 2:
        print("用法: python3 tools/summarize_push_pipeline.py build/e2e_last_push.log", file=sys.stderr)
        return 2
    enc: list[int] = []
    full: list[int] = []
    webrtc_cb: list[int] = []
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE.search(line)
            if m:
                enc.append(int(m.group(1)))
                full.append(int(m.group(2)))
                webrtc_cb.append(int(m.group(3)))
    if not enc:
        print("未匹配到 mjpeg_input_to_encode_done_us 行（编码器需运行且产生周期日志）。")
        return 1

    def show(name: str, vals: list[int]) -> None:
        vals = sorted(vals)
        n = len(vals)
        mean_v = statistics.mean(vals)
        print(
            f"{name}: n={n} us "
            f"min={vals[0]} p50={pct(vals, 0.5)} p95={pct(vals, 0.95)} max={vals[-1]} mean={mean_v:.0f}  "
            f"(ms p50={pct(vals, 0.5) / 1000.0:.3f})"
        )

    print("--- 推流端（服务器）MJPEG 入链 → H.264 编码完成 / WebRTC OnEncodedImage ---")
    show("mjpeg→encode_done", enc)
    show("mjpeg→after_onencoded_cb", full)
    show("webrtc OnEncodedImage cb only", webrtc_cb)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
