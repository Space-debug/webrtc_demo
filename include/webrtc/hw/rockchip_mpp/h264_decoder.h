#ifndef WEBRTC_DEMO_HW_ROCKCHIP_MPP_H264_DECODER_H_
#define WEBRTC_DEMO_HW_ROCKCHIP_MPP_H264_DECODER_H_

#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/video_decoder.h"

namespace webrtc_demo::hw::rockchip_mpp {

/// WebRTC VideoDecoder：H.264 Annex B 码流 → MPP 硬件解码 → I420 VideoFrame。
class H264Decoder final : public webrtc::VideoDecoder {
public:
  explicit H264Decoder(const webrtc::Environment& env);
  ~H264Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Release() override;
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  webrtc::VideoDecoder::DecoderInfo GetDecoderInfo() const override;

  H264Decoder(const H264Decoder&) = delete;
  H264Decoder& operator=(const H264Decoder&) = delete;

private:
  void DestroyMpp();
  bool EnsureMppInitialized();
  bool TryDrainOneDecodedFrame(int64_t render_time_ms, const webrtc::EncodedImage& ref_meta);
  void DiscardPendingOutput();

  const webrtc::Environment& env_;
  webrtc::DecodedImageCallback* callback_{nullptr};

  void* mpp_ctx_{nullptr};
  void* mpi_{nullptr};

  std::vector<uint8_t> bitstream_copy_;
  bool logged_init_{false};
  bool logged_first_frame_{false};
};

}  // namespace webrtc_demo::hw::rockchip_mpp

#endif

