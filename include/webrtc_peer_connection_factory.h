#ifndef WEBRTC_PEER_CONNECTION_FACTORY_H_
#define WEBRTC_PEER_CONNECTION_FACTORY_H_

#include <memory>

#include "api/peer_connection_interface.h"

namespace webrtc {
class Thread;
}

namespace webrtc_demo {

/// 可选：影响视频编码工厂等（仅推流端传入有意义）。
struct PeerConnectionFactoryMediaOptions {
  /// 在编译启用 WEBRTC_DEMO_HAVE_ROCKCHIP_MPP 且未设置 WEBRTC_DISABLE_MPP_H264=1 时，
  /// 为 true 则 H.264 优先使用 RK MPP 硬件编码。
  bool prefer_rockchip_mpp_h264{false};
};

// 与 webrtc::EnableMediaWithDefaults 一致：在 EnableMedia 之前补全
// TaskQueue、内置音/视频编解码工厂、BuiltinAudioProcessingBuilder（若未提供 APM）。
void ConfigurePeerConnectionFactoryDependencies(
    webrtc::PeerConnectionFactoryDependencies& deps,
    const PeerConnectionFactoryMediaOptions* media_options = nullptr);

// 初始化 WebRTC field trials
void EnsureWebrtcFieldTrialsInitialized();

// CreateModularPeerConnectionFactory 在 signaling_thread==nullptr 时会绑定「当前线程」为信令线程；
// CLI 主线程通常不跑 WebRTC 消息循环，会导致 PostTask/SDP 异步链永远不执行。
// 若 deps.signaling_thread 仍为空，则创建专用线程并写入 deps；owned_signaling_thread 须保留至 Factory 销毁后再 Stop。
void EnsureDedicatedPeerConnectionSignalingThread(
    webrtc::PeerConnectionFactoryDependencies& deps,
    std::unique_ptr<webrtc::Thread>* owned_signaling_thread);

}  // namespace webrtc_demo

#endif  // WEBRTC_PEER_CONNECTION_FACTORY_H_
