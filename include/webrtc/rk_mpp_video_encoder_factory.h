#ifndef WEBRTC_DEMO_RK_MPP_VIDEO_ENCODER_FACTORY_H_
#define WEBRTC_DEMO_RK_MPP_VIDEO_ENCODER_FACTORY_H_

#include <memory>

namespace webrtc {
class VideoEncoderFactory;
}

namespace webrtc_demo {

/// H.264 优先走 Rockchip MPP 硬件编码，失败时由 libwebrtc 内置 OpenH264 回退。
std::unique_ptr<webrtc::VideoEncoderFactory> CreateRockchipMppPreferredVideoEncoderFactory();

}  // namespace webrtc_demo

#endif
