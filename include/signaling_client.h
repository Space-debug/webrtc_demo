#ifndef SIGNALING_CLIENT_H
#define SIGNALING_CLIENT_H

#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>

namespace webrtc_demo {

/// P2P 信令客户端（纯 C++ TCP，无 Python/WebSocket）
/// 地址格式: "127.0.0.1:8765" 或 "ws://127.0.0.1:8765"（ws:// 会被忽略）
class SignalingClient {
public:
    /// role: "publisher" 或 "subscriber"
    /// stream_id: 流 ID，多摄像头时推流端/拉流端需指定，默认 livestream
    explicit SignalingClient(const std::string& server_addr, const std::string& role,
                            const std::string& stream_id = "livestream");
    ~SignalingClient();

    bool Start();
    void Stop();

    void SendOffer(const std::string& sdp, const std::string& to_peer_id = "");
    void SendAnswer(const std::string& sdp, const std::string& to_peer_id = "");
    void SendIceCandidate(const std::string& mid, int mline_index, const std::string& candidate,
                          const std::string& to_peer_id = "");

    using OnAnswerCallback = std::function<void(const std::string& peer_id, const std::string& type,
                                                const std::string& sdp)>;
    void SetOnAnswer(OnAnswerCallback cb) { on_answer_ = std::move(cb); }

    using OnOfferCallback = std::function<void(const std::string& peer_id, const std::string& type,
                                               const std::string& sdp)>;
    void SetOnOffer(OnOfferCallback cb) { on_offer_ = std::move(cb); }

    using OnIceCallback = std::function<void(const std::string& peer_id, const std::string& mid,
                                             int mline_index, const std::string& candidate)>;
    void SetOnIce(OnIceCallback cb) { on_ice_ = std::move(cb); }

    using OnPeerEventCallback = std::function<void(const std::string& peer_id)>;
    void SetOnSubscriberJoin(OnPeerEventCallback cb) { on_subscriber_join_ = std::move(cb); }
    void SetOnSubscriberLeave(OnPeerEventCallback cb) { on_subscriber_leave_ = std::move(cb); }

    using OnErrorCallback = std::function<void(const std::string& msg)>;
    void SetOnError(OnErrorCallback cb) { on_error_ = std::move(cb); }

private:
    void ReaderLoop();
    void ParseAndDispatch(const std::string& line);
    bool Connect();
    void SendLine(const std::string& line);
    std::string ResolveTargetPeer(const std::string& to_peer_id) const;

    std::string server_addr_;
    std::string host_;
    uint16_t port_;
    std::string role_;
    std::string stream_id_;
    std::string self_peer_id_;
    std::string last_remote_peer_id_;

    OnAnswerCallback on_answer_;
    OnOfferCallback on_offer_;
    OnIceCallback on_ice_;
    OnPeerEventCallback on_subscriber_join_;
    OnPeerEventCallback on_subscriber_leave_;
    OnErrorCallback on_error_;

    int sock_fd_{-1};
    std::unique_ptr<std::thread> reader_thread_;
    bool running_{false};
    mutable std::mutex peer_mutex_;
};

}  // namespace webrtc_demo

#endif  // SIGNALING_CLIENT_H
