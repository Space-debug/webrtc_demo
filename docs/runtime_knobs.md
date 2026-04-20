# Runtime Knobs

This document lists runtime environment variables supported by `webrtc_push_demo` / `webrtc_pull_demo`.

For weak-network E2E playbook and thresholds, see `docs/e2e_weaknet_playbook.md`.

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

## WebRTC transport (FEC / trials)

- `WEBRTC_DEMO_ENABLE_FLEXFEC=1`
  - Enables FlexFEC-03 field trials (`WebRTC-FlexFEC-03-Advertised`, `WebRTC-FlexFEC-03`). Must be set in the **process environment before** `PeerConnectionFactory` initializes (same rules as other `WEBRTC_DEMO_*` trials). **Not** read from `config/streams.conf`.
  - **Overhead / “recovery ratio”**: There is **no single fixed recovery percentage** in this project. WebRTC allocates FEC/protection **adaptively** from bitrate, observed loss, RTT, etc. (`FecControllerDefault::UpdateFecRates` in libwebrtc). Enabling FlexFEC adds a **separate FEC RTP stream** so the receiver can **sometimes** rebuild lost media packets without waiting for retransmission; how often that succeeds depends on **which** packets were lost and **how much** FEC bandwidth the controller reserved for that moment.
- `WEBRTC_DEMO_FIELD_TRIALS_APPEND=<string>`
  - Append extra field-trial tokens (advanced).

## Encoder / Trace

- `WEBRTC_MPP_ENC_GOP=<N>`
  - Override H.264 GOP frame interval for MPP encoder.
- `WEBRTC_MPP_ENC_HOR_STRIDE_ALIGN=<16|32|64>` (default **64**)
  - Encoder `prep:hor_stride` alignment. Match MPP MJPEG decode NV12 stride so `kNative` frames can bind the decoder buffer for H.264 encode without a per-frame NV12 memcpy. Set `16` only if a BSP misbehaves with 64-aligned prep.
- `WEBRTC_MPP_ENC_INTRA_REFRESH_MODE=<0|1|2|3>`
  - H.264 intra refresh mode (`0`: off, `1`: MB rows, `2`: MB columns, `3`: MB gap).
- `WEBRTC_MPP_ENC_INTRA_REFRESH_ARG=<N>`
  - Intra refresh argument for selected mode.
- `WEBRTC_MPP_ENC_SPLIT_BYTES=<N>`
  - Enable slice split by bytes (`0` disables). Useful to smooth large-frame RTP burst.
- `WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS=<N>`
  - Minimum interval for force-IDR requests, to avoid PLI/FIR-triggered IDR storms.
- `WEBRTC_MPP_ENC_IDR_LOSS_QUICK_MS=<N>`
  - Quick-IDR window for packet-loss requests. When keyframe is requested and last emitted IDR is older than this value,
    encoder may bypass `IDR_MIN_INTERVAL` for faster recovery.
- `WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS=<N>`
  - Force-IDR safeguard timeout. If no keyframe for too long, allow IDR request bypass.
- `WEBRTC_MJPEG_TO_H264_TRACE=1`
  - Periodic MJPEG->H264 pipeline latency logs.
- `WEBRTC_LATENCY_TRACE=1`
  - Enable latency logs for capture/decode/encode pipeline.
- `WEBRTC_E2E_LATENCY_TRACE=1`
  - Enable per-frame TX/RX E2E markers for same-board analysis.

