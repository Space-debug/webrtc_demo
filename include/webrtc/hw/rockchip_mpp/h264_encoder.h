#ifndef WEBRTC_DEMO_RK_MPP_H264_ENCODER_H_
#define WEBRTC_DEMO_RK_MPP_H264_ENCODER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_buffer.h"

#include "api/environment/environment.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc_demo {

/// Rockchip MPP 硬件 H.264 编码器（RK3588 等）。NV12 直拷至 MPP；MPP MJPEG 解码的 kNative 帧在 stride 匹配时可直通缓冲免拷贝。
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
  /// 将 split_assembly_buf_ 中拼好的一帧 Annex B（或等价）码流发出一次 OnEncodedImage。
  int32_t EmitAssembledFrame(const webrtc::VideoFrame& frame,
                             const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& vfb,
                             int64_t encode_before_us,
                             int64_t on_frame_to_encode_enter_us,
                             bool mpp_reports_intra);
  /// 编码输出异常时执行软恢复（丢当前帧/必要时关闭分片），返回是否可继续会话。
  bool HandleOutputFailureAndMaybeRecover(const char* stage, int err_code);

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
  int intra_refresh_mode_{0};
  int intra_refresh_arg_{0};
  bool split_by_byte_enabled_{false};
  int split_bytes_{0};
  int idr_min_interval_ms_{0};
  int idr_loss_quick_trigger_ms_{0};
  int idr_force_max_wait_ms_{0};
  int64_t last_forced_idr_ctrl_us_{-1};
  int64_t last_idr_emit_us_{-1};
  int consecutive_output_failures_{0};
  int recover_soft_fail_threshold_{6};
  int recover_hard_fail_threshold_{30};
  bool recover_disable_split_on_failure_{true};
  bool debug_enabled_{false};
  bool latency_trace_enabled_{false};
  bool e2e_trace_enabled_{false};
  bool mjpeg_to_h264_trace_enabled_{false};
  bool use_sync_encode_{false};
  bool use_task_encode_{false};
  bool task_read_packet_{true};
  unsigned trace_every_n_{45};

  bool initialized_{false};
  /// Annex-B 转换复用缓冲，避免每帧 std::vector 堆分配（容量随帧增长后保持稳定）。
  std::vector<uint8_t> annex_scratch_;
  /// MPP_ENC_SPLIT_BY_BYTE 多包输出时在 EOI 前累积裸码流，EOI 后一次性回调 WebRTC。
  std::vector<uint8_t> split_assembly_buf_;

  /// 与 WebRTC-VideoFrameTrackingIdAdvertised 配合，供接收端 RTP 扩展关联帧。
  uint16_t next_video_frame_tracking_id_{0};
};

}  // namespace webrtc_demo

#endif
