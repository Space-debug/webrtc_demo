#ifndef WEBRTC_DEMO_PLATFORM_ALSA_NULL_FALLBACK_H_
#define WEBRTC_DEMO_PLATFORM_ALSA_NULL_FALLBACK_H_

namespace webrtc_demo {

/// 写入临时 ALSA 配置并设置 ALSA_CONFIG_PATH，使无真实声卡时仍可走 null 设备（Linux）。
void EnableAlsaNullDeviceFallback();

}  // namespace webrtc_demo

#endif
