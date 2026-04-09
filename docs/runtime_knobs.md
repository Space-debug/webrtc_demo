# Runtime Knobs

This document lists runtime environment variables supported by `webrtc_push_demo` / `webrtc_pull_demo`.

## Precedence

- **CLI args** override config file fields.
- **Environment variables** override config file defaults for matching knobs.
- If neither is set, built-in defaults are used.

## Media Backend Selection

- `WEBRTC_DISABLE_MPP_H264=1`
  - Force software H.264 encoder even when Rockchip MPP backend exists.
- `WEBRTC_DISABLE_MPP_H264_DECODE=1`
  - Force software H.264 decoder on receiver side.
- `WEBRTC_MPP_H264_DEC_LOW_LATENCY=1`
  - Enable low-latency options in Rockchip MPP H.264 decoder.

## Capture / Offer Gate

- `WEBRTC_CAPTURE_GATE_MIN_FRAMES`
  - Minimum captured frames required before creating offer.
- `WEBRTC_CAPTURE_GATE_MAX_WAIT_SEC`
  - Max seconds to wait for capture gate.
- `WEBRTC_CAPTURE_WARMUP_SEC`
  - Camera warmup seconds before signaling/offer.

## MJPEG Pipeline

- `WEBRTC_MJPEG_DECODE_INLINE=1`
  - Decode MJPEG inside capture thread (lower latency, more capture-thread pressure).
- `WEBRTC_MJPEG_DEC_LOW_LATENCY=1`
  - More aggressive poll/sleep behavior in MPP MJPEG decode.
- `WEBRTC_MJPEG_V4L2_DMABUF=1`
  - Enable V4L2 EXPBUF + MPP external DMA import path.
- `WEBRTC_MJPEG_RGA_TO_MPP=1`
  - Enable RGA copy path for MJPEG DMA buffer to MPP input.
- `WEBRTC_MJPEG_RGA_MAX_ASPECT=<N>`
  - Max aspect ratio constraint for RGA temporary Y400 layout.
- `WEBRTC_MJPEG_RGA_DISABLE_AFTER_FAIL=1`
  - Disable RGA path for the session after first failure.

## Encoder / Trace

- `WEBRTC_MPP_ENC_GOP=<N>`
  - Override H.264 GOP frame interval for MPP encoder.
- `WEBRTC_MJPEG_TO_H264_TRACE=1`
  - Periodic MJPEG->H264 pipeline latency logs.
- `WEBRTC_LATENCY_TRACE=1`
  - Enable latency logs for capture/decode/encode pipeline.
- `WEBRTC_E2E_LATENCY_TRACE=1`
  - Enable per-frame TX/RX E2E markers for same-board analysis.

