#ifndef PUSH_STREAMER_H
#define PUSH_STREAMER_H

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace webrtc_demo {

/// 推流配置
struct PushStreamerConfig {
    std::string stream_id{"stream_001"};
    int video_width{640};
    int video_height{480};
    int video_fps{30};
    int video_device_index{0};      /// 设备索引，与 --list-cameras 输出对应
    std::string video_device_path;  /// 设备路径，如 /dev/video11，优先于 index
    bool enable_audio{false};  /// 默认纯视频；true 时仍不创建麦克风流，仅放宽 SDP 音频意向
    bool test_capture_only{false};  /// 仅测试采集，不创建 offer/连接
    bool test_encode_mode{false};   /// 本地回环验证 H264 编码（采集→编码→接收）
    /// 信令多订阅者模式：仅对加入的拉流端 CreateOfferForPeer，不在 Start 末尾发 default offer
    bool signaling_subscriber_offer_only{false};
    std::string stun_server{"stun:stun.l.google.com:19302"};
    std::string turn_server;
    std::string turn_username;
    std::string turn_password;

    // 码率控制
    std::string bitrate_mode{"vbr"};  /// cbr=固定码率, vbr=可变码率
    int target_bitrate_kbps{1000};
    int min_bitrate_kbps{100};
    int max_bitrate_kbps{2000};

    // 可变分辨率策略
    std::string degradation_preference{"maintain_framerate"};

    // 编解码
    std::string video_codec{"h264"};  /// h264|h265|vp8|vp9|av1
    std::string h264_profile{"main"}; /// baseline|main|high
    std::string h264_level{"3.0"};
    int keyframe_interval{0};        /// GOP 帧数，0=自动
    int capture_warmup_sec{0};       /// 摄像头预热秒数，默认 0（极速启动）
    /// 创建 Offer 前至少收到多少帧（与 FrameCountingRenderer 同源）；0=关闭。减轻 OpenH264「input fps 0」类告警。
    int capture_gate_min_frames{0};
    /// 等待采集门限的最长时间（秒），超时则放弃本次 Offer 并打日志
    int capture_gate_max_wait_sec{20};

    /// FlexFEC-03：在 LibWebRTC::Initialize 前注入 WEBRTC_FIELD_TRIALS；拉流端也需开启
    bool enable_flexfec{false};
    /// 非空则作为完整 trials 串覆盖默认；通常留空即可
    std::string flexfec_field_trials;

    /// 周期性 GetStats：与采集帧计数对齐，对累计类指标做相邻两次采样的差分，再除以间隔帧数得到 ms/帧（近似窗内平均）
    bool latency_stats_enable{false};
    /// 至少间隔多少采集帧才输出一行「窗内平均」（默认 60）
    int latency_stats_window_frames{60};
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
    bool IsStreaming() const { return is_streaming_; }

    /// GetStats one-line latency summary per active PeerConnection (English logs).
    void LogLatencyStatsForAllPeers(std::ostream& out);

    /// Rolling window delta/GetStats (see LATENCY_STATS_*). English [stats-latency-avg] and [latency-pipeline].
    void LogLatencyStatsRollingAvg(std::ostream& out);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    bool is_streaming_{false};
};

}  // namespace webrtc_demo

#endif  // PUSH_STREAMER_H
