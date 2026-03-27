#ifndef WEBRTC_DEMO_PULL_SUBSCRIBER_H_
#define WEBRTC_DEMO_PULL_SUBSCRIBER_H_

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>

namespace webrtc_demo {

enum class PullConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };

/// 拉流端接收侧可选参数（低时延 / 平滑权衡）
struct PullSubscriberConfig {
    /// 抖动缓冲最小延迟（毫秒）。0 倾向最低时延；调大可减轻卡顿、增加端到端延迟。
    /// 设为 -1 表示不调用 SetJitterBufferMinimumDelay，使用 WebRTC 内部默认。
    int jitter_buffer_min_delay_ms{0};
};

/// 拉流端：信令 + PeerConnection + 远端视频解码为 ARGB（与 PushStreamer 配对使用同一信令协议）
class PullSubscriber {
public:
    explicit PullSubscriber(const std::string& signaling_url,
                            const std::string& stream_id = "livestream",
                            const PullSubscriberConfig& recv = {});
    ~PullSubscriber();

    /// 与推流端一致：在 Play() 前调用；在 LibWebRTC::Initialize 前注入 FlexFEC Field Trials
    void SetFlexfecOptions(bool enable, const std::string& field_trials_override = {});

    void Play();
    void Stop();
    bool IsPlaying() const { return is_playing_; }

    using OnVideoFrameCallback =
        std::function<void(const uint8_t* argb, int width, int height, int stride)>;
    void SetOnVideoFrame(OnVideoFrameCallback cb) { on_video_frame_ = std::move(cb); }

    using OnConnectionStateCallback = std::function<void(PullConnectionState state)>;
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }

    using OnErrorCallback = std::function<void(const std::string& msg)>;
    void SetOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

    void DumpInboundFecReceiverStats(std::ostream& out, int timeout_ms = 5000);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    OnVideoFrameCallback on_video_frame_;
    OnConnectionStateCallback on_connection_state_;
    OnErrorCallback on_error_;
    bool is_playing_{false};
};

}  // namespace webrtc_demo

#endif
