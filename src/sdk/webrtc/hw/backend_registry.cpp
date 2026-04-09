#include "webrtc/hw/backend_registry.h"

#include <cstdlib>

#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"

#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
#include "webrtc/hw/rockchip_mpp/video_decoder_factory.h"
#include "webrtc/hw/rockchip_mpp/video_encoder_factory.h"
#endif

namespace webrtc_demo::hw {

std::unique_ptr<webrtc::VideoEncoderFactory> CreatePreferredVideoEncoderFactory(
    const VideoBackendPreferences& prefs) {
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
  const char* dis = std::getenv("WEBRTC_DISABLE_MPP_H264");
  const bool env_disable = dis && dis[0] == '1' && dis[1] == '\0';
  if (prefs.encoder_backend == VideoCodecBackend::kRockchipMpp && !env_disable) {
    return webrtc_demo::hw::rockchip_mpp::CreateVideoEncoderFactory();
  }
#endif
  return webrtc::CreateBuiltinVideoEncoderFactory();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreatePreferredVideoDecoderFactory(
    const VideoBackendPreferences& prefs) {
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
  const char* dis = std::getenv("WEBRTC_DISABLE_MPP_H264_DECODE");
  const bool env_disable = dis && dis[0] == '1' && dis[1] == '\0';
  if (prefs.decoder_backend == VideoCodecBackend::kRockchipMpp && !env_disable) {
    return webrtc_demo::hw::rockchip_mpp::CreateVideoDecoderFactory();
  }
#endif
  return webrtc::CreateBuiltinVideoDecoderFactory();
}

}  // namespace webrtc_demo::hw

