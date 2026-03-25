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
    std::string capture_format{"auto"};  /// 采集格式: auto|yuyv|mjpeg。yuyv/mjpeg 需 v4l2loopback
    std::string loopback_device;    /// v4l2loopback 设备路径，capture_format 非 auto 时使用
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

    /// FlexFEC-03：在 LibWebRTC::Initialize 前注入 WEBRTC_FIELD_TRIALS；拉流端也需开启
    bool enable_flexfec{false};
    /// 非空则作为完整 trials 串覆盖默认；通常留空即可
    std::string flexfec_field_trials;
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

    /// 对每个活跃 PeerConnection 调用 GetStats，单行打印中文时延摘要（毫秒，含采集/编码/ICE RTT 等）。
    /// MJPEG+采集桥接且启用 libjpeg-turbo 时附加 JPEG 解码最近/平均耗时。
    /// 无多订阅者连接时，若存在默认 peer_connection_ 则对其采样（如非信令单连接）。
    /// 与 WEBRTC_LATENCY_STATS_PROBE 环境变量配合周期打印。
    void LogLatencyStatsForAllPeers(std::ostream& out);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    bool is_streaming_{false};
};

}  // namespace webrtc_demo

#endif  // PUSH_STREAMER_H
