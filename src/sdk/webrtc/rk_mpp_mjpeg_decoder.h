#ifndef WEBRTC_DEMO_RK_MPP_MJPEG_DECODER_H_
#define WEBRTC_DEMO_RK_MPP_MJPEG_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace webrtc {
class I420Buffer;
class NV12Buffer;
}

namespace webrtc_demo {

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

  bool DecodeJpegToI420(const uint8_t* jpeg,
                        size_t jpeg_len,
                        int expect_w,
                        int expect_h,
                        webrtc::I420Buffer* out_i420);

  /// 解码为 NV12（与 MPP 输出一致时仅 memcpy；NV21 时 libyuv::NV21ToNV12）。
  bool DecodeJpegToNV12(const uint8_t* jpeg,
                        size_t jpeg_len,
                        int expect_w,
                        int expect_h,
                        webrtc::NV12Buffer* out_nv12);

 private:
  bool EnsureJpegCopyCapacity(size_t len);
  static size_t ComputeJpegOutputBufSize(int width, int height);

  bool BuildMppPacketFromJpeg(size_t jpeg_len, void** out_packet);
  bool SendMppPacket(void* packet);
  void* PollMppFrame(int timeout_ms);
  bool HandleInfoChangeFrame(void* frame);

  void* ctx_{nullptr};
  void* mpi_{nullptr};
  void* dec_cfg_{nullptr};

  /// 与 GstMppAllocator::group 一致：EXT_BUF_GROUP，输出帧 buffer 从此组分配
  void* output_buf_group_{nullptr};
  /// 与 GstMppJpegDec::input_group 一致：JPEG 码流拷贝进 MppBuffer 再封包
  void* input_group_{nullptr};

  std::vector<uint8_t> jpeg_copy_;
  int last_expect_w_{0};
  int last_expect_h_{0};
  size_t output_buf_size_{0};
};

}  // namespace webrtc_demo

#endif
