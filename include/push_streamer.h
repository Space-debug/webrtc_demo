#ifndef PUSH_STREAMER_H
#define PUSH_STREAMER_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace webrtc_demo {

struct PushStreamerCommonConfig {
    std::string stream_id{"stream_001"};
    int video_width{1280};
    int video_height{720};
    int video_fps{30};
    int video_device_index{0};
    std::string video_device_path;
    bool test_capture_only{false};
    bool test_encode_mode{false};
    bool signaling_subscriber_offer_only{false};
    std::string stun_server{"stun:stun.l.google.com:19302"};
    std::string turn_server;
    std::string turn_username;
    std::string turn_password;

    std::string bitrate_mode{"vbr"};
    int target_bitrate_kbps{1000};
    int min_bitrate_kbps{100};
    int max_bitrate_kbps{2000};
    std::string degradation_preference{"balanced"};
    bool ice_prioritize_likely_pairs{true};
    std::string video_network_priority{"high"};
    int video_encoding_max_framerate{0};

    std::string video_codec{"h264"};
    std::string h264_profile{"main"};
    std::string h264_level{"3.0"};
    int keyframe_interval{0};
    int capture_warmup_sec{0};
    int capture_gate_min_frames{0};
    int capture_gate_max_wait_sec{20};
};

struct PushStreamerBackendConfig {
    bool use_rockchip_mpp_h264{true};
    bool use_rockchip_mpp_mjpeg_decode{true};
    bool use_rockchip_dual_mpp_mjpeg_h264{false};
    bool mjpeg_queue_latest_only{false};
    int mjpeg_queue_max{8};
    int v4l2_buffer_count{2};
    int v4l2_poll_timeout_ms{50};
    int nv12_pool_slots{6};
    bool mjpeg_decode_inline{false};
    bool mjpeg_v4l2_ext_dma{false};
    bool mjpeg_rga_to_mpp{false};
};

/// 推流配置
struct PushStreamerConfig {
    PushStreamerCommonConfig common{};
    PushStreamerBackendConfig backend{};
};

/// SDP 回调：用于将 SDP 发送到信令服务器
using OnSdpCallback = std::function<void(const std::string& peer_id, const std::string& type,
                                         const std::string& sdp)>;

/// ICE 候选回调：用于将 ICE 候选发送到信令服务器
using OnIceCandidateCallback = std::function<void(const std::string& peer_id, const std::string& mid,
                                                  int mline_index, const std::string& candidate)>;

/// 帧回调：每收到一帧调用，参数为 (帧数, 宽, 高)，用于验证采集是否正常
using OnFrameCallback = std::function<void(unsigned int frame_count, int width, int height)>;

/// 连接状态回调
enum class ConnectionState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed
};
using OnConnectionStateCallback = std::function<void(ConnectionState state)>;

/// WebRTC 推流器
class PushStreamer {
public:
    explicit PushStreamer(const PushStreamerConfig& config);
    ~PushStreamer();

    PushStreamer(const PushStreamer&) = delete;
    PushStreamer& operator=(const PushStreamer&) = delete;

    /// 初始化并开始推流
    bool Start();

    /// 停止推流
    void Stop();

    /// 设置远端 SDP（Answer），用于 P2P 模式
    bool SetRemoteDescription(const std::string& type, const std::string& sdp);
    bool SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type,
                                     const std::string& sdp);

    /// 添加远端 ICE 候选
    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate);
    void AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid,
                                      int mline_index, const std::string& candidate);
    void CreateOfferForPeer(const std::string& peer_id);

    /// 回调设置
    void SetOnSdpCallback(OnSdpCallback cb);
    void SetOnIceCandidateCallback(OnIceCandidateCallback cb);
    void SetOnConnectionStateCallback(OnConnectionStateCallback cb);
    void SetOnFrameCallback(OnFrameCallback cb);

    /// 获取采集帧数（需先 SetOnFrameCallback 或 --test-capture）
    unsigned int GetFrameCount() const;

    /// 获取解码帧数（--test-encode 模式下，接收端收到的 H264 解码后帧数）
    unsigned int GetDecodedFrameCount() const;

    /// 是否正在推流
    bool IsStreaming() const { return is_streaming_.load(std::memory_order_acquire); }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> is_streaming_{false};
};

}  // namespace webrtc_demo

#endif  // PUSH_STREAMER_H
