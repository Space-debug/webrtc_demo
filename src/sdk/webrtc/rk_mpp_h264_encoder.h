#ifndef WEBRTC_DEMO_RK_MPP_H264_ENCODER_H_
#define WEBRTC_DEMO_RK_MPP_H264_ENCODER_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc_demo {

/// Rockchip MPP 硬件 H.264 编码器（RK3588 等）。输入 NV12 时直拷至 MPP 缓冲；否则 I420 经 libyuv 转 NV12。
/// 与 OpenH264 相同走 Annex B + start code，供 WebRTC RTP 打包。
class RkMppH264Encoder final : public webrtc::VideoEncoder {
 public:
  RkMppH264Encoder(const webrtc::Environment& env, webrtc::H264EncoderSettings settings);
  ~RkMppH264Encoder() override;

  RkMppH264Encoder(const RkMppH264Encoder&) = delete;
  RkMppH264Encoder& operator=(const RkMppH264Encoder&) = delete;

  void SetFecControllerOverride(webrtc::FecControllerOverride* o) override;

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const webrtc::VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types) override;
  void SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) override;
  webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

 private:
  void DestroyMpp();
  bool ApplyRcToCfg();
  static int MppH264LevelForSize(int width, int height, uint32_t fps);

  const webrtc::Environment& env_;
  webrtc::H264EncoderSettings h264_settings_;
  webrtc::EncodedImageCallback* callback_{nullptr};

  void* mpp_ctx_{nullptr};
  void* mpi_{nullptr};
  void* mpp_cfg_{nullptr};
  void* buf_grp_{nullptr};
  void* frm_buf_{nullptr};
  void* pkt_buf_{nullptr};
  void* md_buf_{nullptr};

  int width_{0};
  int height_{0};
  int hor_stride_{0};
  int ver_stride_{0};
  uint32_t fps_{30};
  int target_bps_{0};
  int min_bps_{0};
  int max_bps_{0};
  int gop_{0};
  int mpp_rc_mode_{0};  // MppEncRcMode

  bool initialized_{false};
  /// Annex-B 转换复用缓冲，避免每帧 std::vector 堆分配（容量随帧增长后保持稳定）。
  std::vector<uint8_t> annex_scratch_;

  /// 与 WebRTC-VideoFrameTrackingIdAdvertised 配合，供接收端 RTP 扩展关联帧。
  uint16_t next_video_frame_tracking_id_{0};
};

}  // namespace webrtc_demo

#endif
