#ifndef P2P_PLAYER_H
#define P2P_PLAYER_H

#include <functional>
#include <memory>
#include <string>

namespace p2p {

enum class ConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };

class P2pPlayer {
public:
    explicit P2pPlayer(const std::string& signaling_url);
    ~P2pPlayer();

    void Play();
    void Stop();
    bool IsPlaying() const { return is_playing_; }

    using OnVideoFrameCallback = std::function<void(const uint8_t* argb, int width, int height, int stride)>;
    void SetOnVideoFrame(OnVideoFrameCallback cb) { on_video_frame_ = std::move(cb); }

    using OnConnectionStateCallback = std::function<void(ConnectionState state)>;
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }

    using OnErrorCallback = std::function<void(const std::string& msg)>;
    void SetOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

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
