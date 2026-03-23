#include "p2p_player.h"
#include "signaling_client.h"
#include <libwebrtc.h>
#include <rtc_mediaconstraints.h>
#include <rtc_peerconnection.h>
#include <rtc_peerconnection_factory.h>
#include <rtc_rtp_receiver.h>
#include <rtc_rtp_transceiver.h>
#include <rtc_types.h>
#include <rtc_video_frame.h>
#include <rtc_video_renderer.h>
#include <rtc_video_track.h>

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>

namespace p2p {

namespace {

void EnableAlsaNullDeviceFallback() {
    const char* path = "/tmp/webrtc_demo_alsa_null.conf";
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << "pcm.!default {\n"
               "  type asym\n"
               "  playback.pcm \"null\"\n"
               "  capture.pcm \"null\"\n"
               "}\n";
        out.close();
        setenv("ALSA_CONFIG_PATH", path, 1);
    }
}

}  // namespace

class VideoRenderer
    : public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>> {
public:
    using Callback = P2pPlayer::OnVideoFrameCallback;
    explicit VideoRenderer(Callback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override {
        if (!frame || !on_frame_) return;
        static unsigned frame_count = 0;
        unsigned n = ++frame_count;
        if (n == 1 || n <= 5 || n % 30 == 0) {
            std::cout << "[VideoRenderer] OnFrame #" << n << std::endl;
        }
        int w = frame->width(), h = frame->height();
        int stride = w * 4;
        std::vector<uint8_t> argb(stride * h);
        frame->ConvertToARGB(libwebrtc::RTCVideoFrame::Type::kARGB, argb.data(), stride, w, h);
        on_frame_(argb.data(), w, h, stride);
    }

private:
    Callback on_frame_;
};

class P2pPlayer::Impl : public libwebrtc::RTCPeerConnectionObserver {
public:
    explicit Impl(const std::string& url, const std::string& stream_id)
        : signaling_(std::make_unique<webrtc_demo::SignalingClient>(url, "subscriber", stream_id)) {}

    bool Initialize() {
        std::cout << "[P2pPlayer] 初始化 LibWebRTC..." << std::endl;
        if (!libwebrtc::LibWebRTC::Initialize()) return false;
        factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
        if (!factory_) {
            libwebrtc::LibWebRTC::Terminate();
            return false;
        }
        // 先做 ALSA null 注入，避免无声卡环境在 Initialize 内部直接 abort。
        EnableAlsaNullDeviceFallback();
        if (!factory_->Initialize()) {
            std::cerr << "[P2pPlayer] factory init failed, try video-only fallback (ALSA null)"
                      << std::endl;
            EnableAlsaNullDeviceFallback();
            factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
            if (!factory_ || !factory_->Initialize()) {
                libwebrtc::LibWebRTC::Terminate();
                return false;
            }
        }
        std::cout << "[P2pPlayer] 创建 PeerConnection (recvonly)..." << std::endl;
        return CreatePeerConnection();
    }

    void Shutdown() {
        if (peer_connection_) {
            peer_connection_->DeRegisterRTCPeerConnectionObserver();
            peer_connection_->Close();
            factory_->Delete(peer_connection_);
            peer_connection_ = nullptr;
        }
        video_track_ = nullptr;
        if (factory_) {
            factory_->Terminate();
            factory_ = nullptr;
        }
        libwebrtc::LibWebRTC::Terminate();
    }

    bool CreatePeerConnection() {
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = libwebrtc::string("stun:stun.l.google.com:19302");
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        // 仅 UDP：低延迟、画面稳定
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        // 抗花屏：DSCP QoS 标记
        rtc_config.enable_dscp = true;
        // 纯视频拉流，不协商音频
        rtc_config.offer_to_receive_audio = false;

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        peer_connection_ = factory_->Create(rtc_config, constraints);
        if (!peer_connection_) return false;
        peer_connection_->RegisterRTCPeerConnectionObserver(this);

        libwebrtc::vector<libwebrtc::string> stream_ids;
        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpEncodingParameters>> encodings;
        auto init = libwebrtc::RTCRtpTransceiverInit::Create(
            libwebrtc::RTCRtpTransceiverDirection::kRecvOnly, stream_ids, encodings);
        peer_connection_->AddTransceiver(libwebrtc::RTCMediaType::VIDEO, init);
        std::cout << "[P2pPlayer] PeerConnection 创建成功" << std::endl;
        return true;
    }

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        peer_connection_->SetRemoteDescription(
            libwebrtc::string(sdp.c_str()), libwebrtc::string(type.c_str()),
            [this]() {
                std::cout << "[P2pPlayer] SetRemoteDescription OK, CreateAnswer" << std::endl;
                CreateAnswer();
            },
            [this](const char* err) {
                if (on_error_) on_error_(std::string("SetRemoteDescription: ") + err);
            });
    }

    void CreateAnswer() {
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        constraints->AddOptionalConstraint(
            libwebrtc::RTCMediaConstraints::kOfferToReceiveAudio,
            libwebrtc::RTCMediaConstraints::kValueFalse);
        peer_connection_->CreateAnswer(
            [this](const libwebrtc::string& sdp, const libwebrtc::string& type) {
                peer_connection_->SetLocalDescription(
                    sdp, type,
                    [this, sdp]() {
                        signaling_->SendAnswer(sdp.std_string());
                        std::cout << "[P2pPlayer] Answer sent" << std::endl;
                    },
                    [this](const char* err) {
                        if (on_error_) on_error_(std::string("SetLocalDescription: ") + err);
                    });
            },
            [this](const char* err) {
                if (on_error_) on_error_(std::string("CreateAnswer: ") + err);
            },
            constraints);
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        peer_connection_->AddCandidate(libwebrtc::string(mid.c_str()), mline_index,
                                      libwebrtc::string(candidate.c_str()));
    }

    void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> t) override {
        if (!t || !t->receiver()) return;
        std::cout << "[P2pPlayer] OnTrack: 收到视频轨道" << std::endl;
        auto track = t->receiver()->track();
        if (!track || std::strcmp(track->kind().c_string(), "video") != 0) return;
        auto vt = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(
            static_cast<libwebrtc::RTCVideoTrack*>(track.get()));
        if (vt && video_renderer_) {
            std::lock_guard<std::mutex> lock(mutex_);
            video_track_ = vt;
            video_track_->AddRenderer(video_renderer_.get());
        }
    }
    void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
                    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> r) override {
        if (!r) return;
        std::cout << "[P2pPlayer] OnAddTrack: 添加视频轨道到渲染器" << std::endl;
        auto track = r->track();
        if (!track || std::strcmp(track->kind().c_string(), "video") != 0) return;
        auto vt = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(
            static_cast<libwebrtc::RTCVideoTrack*>(track.get()));
        if (vt && video_renderer_) {
            std::lock_guard<std::mutex> lock(mutex_);
            video_track_ = vt;
            video_track_->AddRenderer(video_renderer_.get());
        }
    }
    void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
    void OnSignalingState(libwebrtc::RTCSignalingState) override {}
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
    void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
    void OnRenegotiationNeeded() override {}

    void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> c) override {
        if (c && signaling_) {
            std::cout << "[P2pPlayer] 发送 ICE 候选 mid=" << c->sdp_mid().std_string() << std::endl;
            signaling_->SendIceCandidate(c->sdp_mid().std_string(), c->sdp_mline_index(),
                                         c->candidate().std_string());
        }
    }

    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
        if (on_connection_state_) {
            ConnectionState cs = ConnectionState::New;
            switch (state) {
                case libwebrtc::RTCPeerConnectionStateConnecting: cs = ConnectionState::Connecting; break;
                case libwebrtc::RTCPeerConnectionStateConnected: cs = ConnectionState::Connected; break;
                case libwebrtc::RTCPeerConnectionStateDisconnected: cs = ConnectionState::Disconnected; break;
                case libwebrtc::RTCPeerConnectionStateFailed: cs = ConnectionState::Failed; break;
                case libwebrtc::RTCPeerConnectionStateClosed: cs = ConnectionState::Closed; break;
                default: break;
            }
            on_connection_state_(cs);
        }
    }

    std::unique_ptr<webrtc_demo::SignalingClient> signaling_;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> peer_connection_;
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> video_track_;
    std::shared_ptr<VideoRenderer> video_renderer_;
    std::mutex mutex_;
    OnConnectionStateCallback on_connection_state_;
    OnErrorCallback on_error_;
};

P2pPlayer::P2pPlayer(const std::string& signaling_url, const std::string& stream_id)
    : impl_(std::make_unique<Impl>(signaling_url, stream_id)) {}

P2pPlayer::~P2pPlayer() { Stop(); }

void P2pPlayer::Play() {
    if (is_playing_) return;

    impl_->on_connection_state_ = on_connection_state_;
    impl_->on_error_ = on_error_;
    impl_->video_renderer_ = std::make_shared<VideoRenderer>(on_video_frame_);

    impl_->signaling_->SetOnOffer([this](const std::string& peer_id, const std::string& type,
                                         const std::string& sdp) {
        std::cout << "[P2pPlayer] 收到 offer from=" << peer_id
                  << " (type=" << type << ", len=" << sdp.size() << ")" << std::endl;
        impl_->SetRemoteDescription(type, sdp);
    });
    impl_->signaling_->SetOnIce([this](const std::string& peer_id, const std::string& mid, int mline_index,
                                       const std::string& candidate) {
        std::cout << "[P2pPlayer] 收到 ICE 候选 from=" << peer_id << " mid=" << mid
                  << " idx=" << mline_index << std::endl;
        impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
    });
    impl_->signaling_->SetOnError([this](const std::string& msg) {
        if (on_error_) on_error_(msg);
    });

    if (!impl_->Initialize()) {
        if (on_error_) on_error_("LibWebRTC init failed");
        return;
    }
    if (!impl_->signaling_->Start()) {
        if (on_error_) on_error_("Signaling failed. Run: ./build/bin/signaling_server or ./scripts/start_p2p.sh");
        return;
    }
    is_playing_ = true;
    std::cout << "[P2pPlayer] Waiting for offer from publisher..." << std::endl;
}

void P2pPlayer::Stop() {
    if (!is_playing_) return;
    impl_->signaling_->Stop();
    impl_->Shutdown();
    is_playing_ = false;
}

}  // namespace p2p
