#ifndef P2P_PLAYER_H
#define P2P_PLAYER_H

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>

namespace p2p {

enum class ConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };

class P2pPlayer {
public:
    /// stream_id: 要拉取的流 ID，多流时需指定，默认 livestream
    explicit P2pPlayer(const std::string& signaling_url,
                       const std::string& stream_id = "livestream");
    ~P2pPlayer();

    /// 与推流端一致：在 Play() 前调用；在 LibWebRTC::Initialize 前注入 FlexFEC Field Trials
    void SetFlexfecOptions(bool enable, const std::string& field_trials_override = {});

    void Play();
    void Stop();
    bool IsPlaying() const { return is_playing_; }

    using OnVideoFrameCallback = std::function<void(const uint8_t* argb, int width, int height, int stride)>;
    void SetOnVideoFrame(OnVideoFrameCallback cb) { on_video_frame_ = std::move(cb); }

    using OnConnectionStateCallback = std::function<void(ConnectionState state)>;
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }

    using OnErrorCallback = std::function<void(const std::string& msg)>;
    void SetOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

    /// 对 PeerConnection 调用 GetStats，输出 [fec-verify] fecPacketsReceived=…（供可选自动化；多数绑定无独立 FEC 计数）。
    void DumpInboundFecReceiverStats(std::ostream& out, int timeout_ms = 5000);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    OnVideoFrameCallback on_video_frame_;
    OnConnectionStateCallback on_connection_state_;
    OnErrorCallback on_error_;
    bool is_playing_{false};
};

}  // namespace p2p

#endif  // P2P_PLAYER_H
