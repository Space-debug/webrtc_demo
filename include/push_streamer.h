#ifndef PUSH_STREAMER_H
#define PUSH_STREAMER_H

#include <functional>
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
    bool enable_audio{true};
    bool test_capture_only{false};  /// 仅测试采集，不创建 offer/连接
    bool test_encode_mode{false};   /// 本地回环验证 H264 编码（采集→编码→接收）
    std::string stun_server{"stun:stun.l.google.com:19302"};
    std::string turn_server;
    std::string turn_username;
    std::string turn_password;
};

/// SDP 回调：用于将 SDP 发送到信令服务器
using OnSdpCallback = std::function<void(const std::string& type, const std::string& sdp)>;

/// ICE 候选回调：用于将 ICE 候选发送到信令服务器
using OnIceCandidateCallback = std::function<void(const std::string& mid, int mline_index, const std::string& candidate)>;

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

    /// 添加远端 ICE 候选
    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate);

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

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    bool is_streaming_{false};
};

}  // namespace webrtc_demo

#endif  // PUSH_STREAMER_H
