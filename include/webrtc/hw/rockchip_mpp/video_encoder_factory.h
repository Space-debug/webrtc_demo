#ifndef WEBRTC_DEMO_HW_ROCKCHIP_MPP_VIDEO_ENCODER_FACTORY_H_
#define WEBRTC_DEMO_HW_ROCKCHIP_MPP_VIDEO_ENCODER_FACTORY_H_

#include <memory>

namespace webrtc {
class VideoEncoderFactory;
}

namespace webrtc_demo::hw::rockchip_mpp {

/// H.264 优先走 Rockchip MPP 硬件编码，失败时由 libwebrtc 内置 OpenH264 回退。
std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory();

}  // namespace webrtc_demo::hw::rockchip_mpp

#endif

