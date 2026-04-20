# E2E 弱网压测与阈值

本页用于验证以下策略在弱网下是否达到目标：

- 常态：`P + intra refresh`
- 异常：按需触发 IDR
- 防风暴：IDR 最小间隔 + 最长等待兜底
- 降突发：按字节切片

## 一键执行

在有可用摄像头的机器运行：

```bash
./tools/run_e2e_weaknet.sh --profile fair --camera /dev/video11 --yes
```

常用档位：

- `good`：轻度抖动
- `fair`：中等弱网（默认建议先跑）
- `bad`：高抖动+中高丢包
- `harsh`：极端弱网回归

## 关键指标（看 steady 口径）

脚本会从日志中抓取：

- `[E2E_SUMMARY] pairing=trace_id_steady ... p95=... p99=...`

建议门限：

- `PASS`：`p95 <= 120ms` 且 `p99 <= 150ms`
- `WARN`：超过 PASS 但 `p99 <= 220ms`
- `FAIL`：`p99 > 220ms`

说明：

- `trace_id_steady` 是跳过暖机样本后的稳态口径，更适合调参。
- 首次接入抖动、关键帧恢复尖峰不要用来判断长期表现。

## 推荐起始参数

```bash
export WEBRTC_MPP_ENC_INTRA_REFRESH_MODE=1
export WEBRTC_MPP_ENC_INTRA_REFRESH_ARG=1
export WEBRTC_MPP_ENC_SPLIT_BYTES=1100
export WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS=800
export WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS=3000
```

## 调参指南（按现象）

- **症状：p99 偶发高尖峰，码率突发明显**
  - 先减小 `WEBRTC_MPP_ENC_SPLIT_BYTES`（1100 -> 1000 -> 900）
  - 适度增大 `WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS`（800 -> 1000/1200）

- **症状：恢复慢、花屏拖尾**
  - 先减小 `WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS`（800 -> 600/500）
  - 再减小 `WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS`（3000 -> 2000/1500）

- **症状：平均画质下降**
  - 减弱 intra refresh 强度（增大 `INTRA_REFRESH_ARG`）
  - 或在同等画质目标下略增码率预算

## 清理弱网注入

```bash
./tools/run_e2e_weaknet.sh --clear-only --iface lo --yes
```

