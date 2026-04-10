#!/usr/bin/env python3
"""
多轮本机 E2E 回归：重复执行 ./scripts/pull.sh --e2e，并汇总 p50/p95/p99 与尾延迟比例。

示例：
  python3 tools/run_e2e_regression.py --runs 5 --frames 400
  python3 tools/run_e2e_regression.py --runs 8 --warn-ms 20 --pull-profile aggressive
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import statistics
import subprocess
import sys
from typing import Dict, List

RE_SIG = re.compile(r"^E2E-SIGNATURE .*$", re.MULTILINE)
RE_SUM = re.compile(
    r"\[E2E_SUMMARY\].*?p50=([0-9.]+)ms p95=([0-9.]+)ms p99=([0-9.]+)ms n=(\d+)"
)
RE_TAIL = re.compile(r"\[E2E_TAIL\] over_(\d+)ms=(\d+)/(\d+) \(([0-9.]+)%\)")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--runs", type=int, default=5, help="回归轮数（默认 5）")
    p.add_argument("--frames", type=int, default=400, help="每轮采集帧数（默认 400）")
    p.add_argument("--warn-ms", type=int, default=30, help="尾延迟阈值（默认 30ms）")
    p.add_argument("--pull-profile", default="aggressive", choices=["aggressive", "balanced", "off"])
    p.add_argument("--push-profile", default="aggressive", choices=["aggressive", "balanced", "off"])
    p.add_argument("--cpu-pin-mode", default="auto", choices=["auto", "off"])
    return p.parse_args()


def ms_stats(v: List[float]) -> str:
    if not v:
        return "n=0"
    s = sorted(v)
    n = len(s)
    p50 = s[(n - 1) * 50 // 100]
    p95 = s[(n - 1) * 95 // 100]
    p99 = s[(n - 1) * 99 // 100]
    return "n={} min={:.3f} p50={:.3f} p95={:.3f} p99={:.3f} max={:.3f} mean={:.3f}".format(
        n, s[0], p50, p95, p99, s[-1], statistics.mean(s)
    )


def run_once(root: str, i: int, args: argparse.Namespace) -> Dict[str, object]:
    env = os.environ.copy()
    env["E2E_FRAMES"] = str(args.frames)
    env["WEBRTC_E2E_WARN_MS"] = str(args.warn_ms)
    env["E2E_LOWLAT_PROFILE"] = args.pull_profile
    env["PUSH_LOWLATENCY_PROFILE"] = args.push_profile
    env["E2E_CPU_PIN_MODE"] = args.cpu_pin_mode

    cmd = ["bash", "-lc", "./scripts/pull.sh --e2e"]
    proc = subprocess.run(cmd, cwd=root, env=env, text=True, capture_output=True)
    out = proc.stdout + proc.stderr

    ts = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(root, "build", "e2e_regression")
    os.makedirs(out_dir, exist_ok=True)
    run_log = os.path.join(out_dir, "run{}_{}.log".format(i, ts))
    with open(run_log, "w", encoding="utf-8") as f:
        f.write(out)

    push_src = os.path.join(root, "build", "e2e_last_push.log")
    pull_src = os.path.join(root, "build", "e2e_last_pull.log")
    push_dst = os.path.join(out_dir, "run{}_{}_push.log".format(i, ts))
    pull_dst = os.path.join(out_dir, "run{}_{}_pull.log".format(i, ts))
    if os.path.exists(push_src):
        shutil.copy2(push_src, push_dst)
    if os.path.exists(pull_src):
        shutil.copy2(pull_src, pull_dst)

    sig_m = RE_SIG.search(out)
    sum_m = RE_SUM.search(out)
    tail_m = RE_TAIL.search(out)

    result: Dict[str, object] = {
        "ok": proc.returncode == 0 and bool(sum_m),
        "rc": proc.returncode,
        "run_log": run_log,
        "signature": sig_m.group(0) if sig_m else "(missing signature)",
    }
    if sum_m:
        result["p50"] = float(sum_m.group(1))
        result["p95"] = float(sum_m.group(2))
        result["p99"] = float(sum_m.group(3))
        result["n"] = int(sum_m.group(4))
    if tail_m:
        result["tail_warn_ms"] = int(tail_m.group(1))
        result["tail_count"] = int(tail_m.group(2))
        result["tail_total"] = int(tail_m.group(3))
        result["tail_ratio"] = float(tail_m.group(4))
    return result


def main() -> int:
    args = parse_args()
    if args.runs < 1 or args.runs > 50:
        print("--runs 建议在 1..50")
        return 2

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    all_res: List[Dict[str, object]] = []
    print("E2E regression starts: runs={} frames={} warn_ms={}".format(args.runs, args.frames, args.warn_ms))

    for i in range(1, args.runs + 1):
        r = run_once(root, i, args)
        all_res.append(r)
        if r.get("ok"):
            print(
                "run#{:02d} ok  p50={:.3f} p95={:.3f} p99={:.3f} n={}  tail={}".format(
                    i,
                    r.get("p50", -1.0),
                    r.get("p95", -1.0),
                    r.get("p99", -1.0),
                    r.get("n", 0),
                    "{:.2f}%".format(r.get("tail_ratio", 0.0)) if "tail_ratio" in r else "na",
                )
            )
        else:
            print("run#{:02d} failed rc={} log={}".format(i, r.get("rc"), r.get("run_log")))

    ok = [r for r in all_res if r.get("ok")]
    print("\n=== Regression Summary ===")
    if not ok:
        print("all runs failed")
        return 1

    p50s = [float(r["p50"]) for r in ok]
    p95s = [float(r["p95"]) for r in ok]
    p99s = [float(r["p99"]) for r in ok]
    tails = [float(r["tail_ratio"]) for r in ok if "tail_ratio" in r]
    print("valid_runs={}/{}".format(len(ok), len(all_res)))
    print("p50(ms): {}".format(ms_stats(p50s)))
    print("p95(ms): {}".format(ms_stats(p95s)))
    print("p99(ms): {}".format(ms_stats(p99s)))
    if tails:
        print("tail_ratio(>={}ms, %): {}".format(args.warn_ms, ms_stats(tails)))
    print("latest_signature: {}".format(ok[-1].get("signature", "")))
    print("artifacts_dir: {}/build/e2e_regression".format(root))
    return 0


if __name__ == "__main__":
    sys.exit(main())

