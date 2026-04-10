#ifndef WEBRTC_DEMO_PULL_SUBSCRIBER_H_
#define WEBRTC_DEMO_PULL_SUBSCRIBER_H_

#include <functional>
#include <memory>
#include <string>

namespace webrtc_demo {

enum class PullConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };

struct PullSubscriberCommonConfig {
    /// 抖动缓冲最小延迟（毫秒）。0 倾向最低时延；调大可减轻卡顿、增加端到端延迟。
    /// 设为 -1 表示不调用 SetJitterBufferMinimumDelay，使用 WebRTC 内部默认。
    int jitter_buffer_min_delay_ms{0};
    /// 为 true 时不做 I420→ARGB（省 CPU、缩短 Sink 内耗时；回调里 pixels 为 nullptr，仅 width/height 有效）。
    /// 适合无头压测；SDL 预览须为 false。
    bool skip_sink_argb_conversion{false};
};

struct PullSubscriberBackendConfig {
    /// 为 true 时优先 Rockchip MPP H.264 硬解；否则使用 builtin decoder backend。
    bool use_rockchip_mpp_h264_decode{true};
};

/// 拉流端接收侧可选参数（低时延 / 平滑权衡 + backend 偏好）
struct PullSubscriberConfig {
    PullSubscriberCommonConfig common{};
    PullSubscriberBackendConfig backend{};
};

/// 拉流端：信令 + PeerConnection + 远端视频解码为 ARGB（与 PushStreamer 配对使用同一信令协议）
class PullSubscriber {
public:
    explicit PullSubscriber(const std::string& signaling_url,
                            const std::string& stream_id = "livestream",
                            const PullSubscriberConfig& recv = {});
    ~PullSubscriber();

    void Play();
    void Stop();
    bool IsPlaying() const { return is_playing_; }

    using OnVideoFrameCallback =
        std::function<void(const uint8_t* argb, int width, int height, int stride, uint16_t trace_id,
                           int64_t t_sink_callback_done_us)>;
    void SetOnVideoFrame(OnVideoFrameCallback cb) { on_video_frame_ = std::move(cb); }

    using OnConnectionStateCallback = std::function<void(PullConnectionState state)>;
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }

    using OnErrorCallback = std::function<void(const std::string& msg)>;
    void SetOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

    /// 异步请求入站视频 RTP 统计（含 video-timing 对应的 goog_timing_frame_info，若对端协商并发送该扩展）。
    /// 在 PeerConnection 信令线程触发 GetStats；结果在 WebRTC 内部线程回调中打印到 stdout。
    void RequestInboundVideoStatsLog();

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
