#ifndef WEBRTC_DEMO_RK_MPP_MJPEG_DECODER_H_
#define WEBRTC_DEMO_RK_MPP_MJPEG_DECODER_H_

#include <cstddef>
#include <cstdint>

#include "api/scoped_refptr.h"

namespace webrtc {
class I420Buffer;
class NV12Buffer;
}

namespace webrtc_demo {

class MppNativeDecFrameBuffer;

/// Rockchip MPP MJPEG 硬件解码；可输出 I420（经 libyuv）或紧凑 NV12（直拷 Y/UV，供 MPP 硬编零 libyuv 色度转换）。
/// 实现路径对齐 GStreamer gstmppjpegdec.c + gstmppdec.c：
/// - 解码器创建后立刻 MPP_DEC_SET_EXT_BUF_GROUP（DRM internal group，与 GstMppAllocator 一致）
/// - 每帧：input_group 拷贝 JPEG → mpp_packet_init_with_buffer；MPP_PORT_INPUT Task 提交；
///   输出 buffer 从同一 EXT group 按 buf_size 分配；MPP_PORT_OUTPUT 取回 MppFrame。
class RkMppMjpegDecoder {
 public:
  RkMppMjpegDecoder();
  ~RkMppMjpegDecoder();

  RkMppMjpegDecoder(const RkMppMjpegDecoder&) = delete;
  RkMppMjpegDecoder& operator=(const RkMppMjpegDecoder&) = delete;

  bool Init();
  void Close();

  /// 与 CameraVideoTrackSource 配置对齐；若已设置 WEBRTC_MJPEG_V4L2_DMABUF 环境变量则仍以环境为准。
  void SetPipelineV4l2ExtDmabuf(bool enable) { pipeline_v4l2_ext_dma_ = enable; }
  /// 与配置 MJPEG_RGA_TO_MPP 对齐；环境变量 WEBRTC_MJPEG_RGA_TO_MPP 优先。
  void SetPipelineRgaToMpp(bool enable) { pipeline_rga_to_mpp_ = enable; }

  /// dma_buf_fd>=0：EXT_DMA（WEBRTC_MJPEG_V4L2_DMABUF）或 RGA（WEBRTC_MJPEG_RGA_TO_MPP）；capacity 一般取 QUERYBUF.length。
  /// 建议始终传入 jpeg（mmap）：RGA 失败时 memcpy 回退；EXT_DMA 成功时可忽略指针。
  bool DecodeJpegToI420(const uint8_t* jpeg,
                        size_t jpeg_len,
                        int expect_w,
                        int expect_h,
                        webrtc::I420Buffer* out_i420,
                        int dma_buf_fd = -1,
                        size_t dma_buf_capacity = 0);

  /// 解码为 NV12（与 MPP 输出一致时仅 memcpy；NV21 时 libyuv::NV21ToNV12）。
  bool DecodeJpegToNV12(const uint8_t* jpeg,
                        size_t jpeg_len,
                        int expect_w,
                        int expect_h,
                        webrtc::NV12Buffer* out_nv12,
                        int dma_buf_fd = -1,
                        size_t dma_buf_capacity = 0);

  /// 解码输出保留为 MppFrame（kNative），供 MPP H264 零拷贝编码；失败时 out 不赋值。
  bool DecodeJpegToNativeDecFrame(const uint8_t* jpeg,
                                  size_t jpeg_len,
                                  int expect_w,
                                  int expect_h,
                                  webrtc::scoped_refptr<MppNativeDecFrameBuffer>* out,
                                  int dma_buf_fd = -1,
                                  size_t dma_buf_capacity = 0,
                                  int64_t dq_time_us = 0,
                                  int64_t v4l2_timestamp_us = 0,
                                  int64_t poll_wait_us = 0,
                                  int64_t dqbuf_ioctl_us = 0,
                                  int64_t decode_queue_wait_us = 0);

 private:
  static size_t ComputeJpegOutputBufSize(int width, int height);
  bool SendMppPacket(void* packet);
  void* PollMppFrame(int timeout_ms);
  bool HandleInfoChangeFrame(void* frame);

  /// EXT_DMA / RGA / memcpy 由内部与环境变量决定；RGA 失败会 memcpy（需 jpeg 指针）。
  bool BuildMppInputPacket(int dma_buf_fd,
                           size_t dma_buf_capacity,
                           const uint8_t* jpeg,
                           size_t jpeg_len,
                           void** out_packet);

  bool WantExtDmabufImport() const;
  bool WantRgaToMpp() const;

  void* ctx_{nullptr};
  void* mpi_{nullptr};
  void* dec_cfg_{nullptr};

  /// 与 GstMppAllocator::group 一致：EXT_BUF_GROUP，输出帧 buffer 从此组分配
  void* output_buf_group_{nullptr};
  /// 与 GstMppJpegDec::input_group 一致：JPEG 码流拷贝进 MppBuffer 再封包
  void* input_group_{nullptr};

  int last_expect_w_{0};
  int last_expect_h_{0};
  size_t output_buf_size_{0};
  /// WEBRTC_MJPEG_RGA_DISABLE_AFTER_FAIL=1 时首帧 RGA 失败后本会话不再尝试 RGA。
  bool session_skip_rga_{false};
  bool pipeline_v4l2_ext_dma_{false};
  bool pipeline_rga_to_mpp_{false};
};

}  // namespace webrtc_demo

#endif
