#include "pull_subscriber.h"
#include "signaling_client.h"
#include "platform/alsa_null_fallback.h"
#include "webrtc_field_trials.h"
#include "webrtc_peer_connection_factory.h"

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_frame_buffer.h"
#include "libyuv/convert.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace webrtc_demo {

namespace {

static bool NameContainsFec(const std::string& n) {
    std::string lower;
    lower.reserve(n.size());
    for (unsigned char c : n) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower.find("fec") != std::string::npos;
}

static uint64_t StatsAttrToUint(const webrtc::Attribute& m) {
    if (!m.has_value()) {
        return 0;
    }
    if (m.holds_alternative<uint64_t>()) {
        const auto& o = m.as_optional<uint64_t>();
        return o.has_value() ? *o : 0;
    }
    if (m.holds_alternative<uint32_t>()) {
        const auto& o = m.as_optional<uint32_t>();
        return o.has_value() ? static_cast<uint64_t>(*o) : 0;
    }
    if (m.holds_alternative<int64_t>()) {
        const auto& o = m.as_optional<int64_t>();
        return o.has_value() && *o > 0 ? static_cast<uint64_t>(*o) : 0;
    }
    if (m.holds_alternative<int32_t>()) {
        const auto& o = m.as_optional<int32_t>();
        return o.has_value() && *o > 0 ? static_cast<uint64_t>(*o) : 0;
    }
    return 0;
}

static void AccumulateFecFromReport(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report,
                                    bool* any_fec_named_counter,
                                    uint64_t* max_value) {
    if (!report || !any_fec_named_counter || !max_value) {
        return;
    }
    for (const webrtc::RTCStats& st : *report) {
        for (const webrtc::Attribute& mem : st.Attributes()) {
            if (!mem.has_value()) {
                continue;
            }
            std::string name = mem.name();
            if (!NameContainsFec(name)) {
                continue;
            }
            uint64_t v = StatsAttrToUint(mem);
            *any_fec_named_counter = true;
            if (v > *max_value) {
                *max_value = v;
            }
        }
    }
}

uint64_t JsonUintAfterKey(const std::string& j, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) {
        return UINT64_MAX;
    }
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) {
        return UINT64_MAX;
    }
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) {
        ++p;
    }
    if (p >= j.size() || !std::isdigit(static_cast<unsigned char>(j[p]))) {
        return UINT64_MAX;
    }
    uint64_t v = 0;
    while (p < j.size() && std::isdigit(static_cast<unsigned char>(j[p]))) {
        v = v * 10 + static_cast<uint64_t>(j[p] - '0');
        ++p;
    }
    return v;
}

webrtc::PeerConnectionInterface::RTCConfiguration MakeRtcConfig() {
    webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.push_back("stun:stun.l.google.com:19302");
    rtc_config.servers.push_back(stun);
    rtc_config.disable_ipv6_on_wifi = true;
    rtc_config.max_ipv6_networks = 0;
    rtc_config.disable_link_local_networks = true;
    rtc_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    rtc_config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
    rtc_config.set_dscp(true);
    rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    return rtc_config;
}

class CreateAnswerObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    using OkFn = std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>)>;
    using FailFn = std::function<void(webrtc::RTCError)>;
    CreateAnswerObserver(OkFn ok, FailFn fail) : ok_(std::move(ok)), fail_(std::move(fail)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::unique_ptr<webrtc::SessionDescriptionInterface> owned(desc);
        if (ok_) {
            ok_(std::move(owned));
        }
    }
    void OnFailure(webrtc::RTCError err) override {
        if (fail_) {
            fail_(std::move(err));
        }
    }

private:
    OkFn ok_;
    FailFn fail_;
};

class SetLocalDescObserver : public webrtc::SetLocalDescriptionObserverInterface {
public:
    explicit SetLocalDescObserver(std::function<void(webrtc::RTCError)> fn) : fn_(std::move(fn)) {}
    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
        if (fn_) {
            fn_(std::move(error));
        }
    }

private:
    std::function<void(webrtc::RTCError)> fn_;
};

class SetRemoteDescObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    explicit SetRemoteDescObserver(std::function<void(webrtc::RTCError)> fn) : fn_(std::move(fn)) {}
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (fn_) {
            fn_(std::move(error));
        }
    }

private:
    std::function<void(webrtc::RTCError)> fn_;
};

class StatsDeliveredCallback : public webrtc::RTCStatsCollectorCallback {
public:
    explicit StatsDeliveredCallback(std::function<void(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>&)> fn)
        : fn_(std::move(fn)) {}
    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        if (fn_) {
            fn_(report);
        }
    }

private:
    std::function<void(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>&)> fn_;
};

}  // namespace

class VideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using Callback = PullSubscriber::OnVideoFrameCallback;
    explicit VideoSink(Callback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(const webrtc::VideoFrame& frame) override {
        if (!on_frame_) {
            return;
        }
        auto buf = frame.video_frame_buffer();
        if (!buf) {
            return;
        }
        int w = frame.width();
        int h = frame.height();
        int stride = w * 4;
        auto i420 = buf->ToI420();
        if (!i420) {
            return;
        }
        argb_.resize(static_cast<size_t>(stride * h));
        libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                           i420->StrideV(), argb_.data(), stride, w, h);
        static unsigned frame_count = 0;
        unsigned n = ++frame_count;
        if (n == 1 || n <= 5 || n % 30 == 0) {
            std::cout << "[VideoSink] OnFrame #" << n << std::endl;
        }
        on_frame_(argb_.data(), w, h, stride);
    }

private:
    Callback on_frame_;
    std::vector<uint8_t> argb_;
};

class PullSubscriber::Impl : public webrtc::PeerConnectionObserver {
public:
    Impl(const std::string& url, const std::string& stream_id, const PullSubscriberConfig& recv)
        : signaling_(std::make_unique<webrtc_demo::SignalingClient>(url, "subscriber", stream_id)),
          recv_config_(recv) {}

    void SetFlexfecOptions(bool enable, std::string override_trials) {
        flexfec_enable_ = enable;
        flexfec_override_ = std::move(override_trials);
    }

    bool Initialize() {
        webrtc_demo::EnsureFlexfecFieldTrials(flexfec_enable_, flexfec_override_);
        std::cout << "[PullSubscriber] Initializing WebRTC (native API)..." << std::endl;
        if (!webrtc::InitializeSSL()) {
            return false;
        }
        webrtc::PeerConnectionFactoryDependencies deps;
        webrtc_demo::ConfigurePeerConnectionFactoryDependencies(deps);
        webrtc_demo::EnableAlsaNullDeviceFallback();
        webrtc_demo::EnsureDedicatedPeerConnectionSignalingThread(deps, &owned_signaling_thread_);
        factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
        if (!factory_) {
            webrtc::CleanupSSL();
            return false;
        }
        return CreatePeerConnection();
    }

    void Shutdown() {
        if (video_sink_ && video_track_) {
            video_track_->RemoveSink(video_sink_.get());
        }
        video_sink_.reset();
        video_track_ = nullptr;
        if (peer_connection_) {
            peer_connection_->Close();
            peer_connection_ = nullptr;
        }
        factory_ = nullptr;
        if (owned_signaling_thread_) {
            owned_signaling_thread_->Stop();
            owned_signaling_thread_.reset();
        }
        webrtc::CleanupSSL();
    }

    bool CreatePeerConnection() {
        auto rtc_config = MakeRtcConfig();
        webrtc::PeerConnectionDependencies deps(this);
        auto result = factory_->CreatePeerConnectionOrError(rtc_config, std::move(deps));
        if (!result.ok()) {
            std::cerr << "[PullSubscriber] CreatePeerConnection failed: " << result.error().message() << std::endl;
            return false;
        }
        peer_connection_ = result.MoveValue();

        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        auto tr = peer_connection_->AddTransceiver(webrtc::MediaType::VIDEO, init);
        if (!tr.ok()) {
            std::cerr << "[PullSubscriber] AddTransceiver failed" << std::endl;
            return false;
        }
        std::cout << "[PullSubscriber] PeerConnection created" << std::endl;
        return true;
    }

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
        if (!pc) {
            return;
        }
        webrtc::Thread* sig = pc->signaling_thread();
        if (!sig) {
            if (on_error_) {
                on_error_("SetRemoteDescription: no signaling thread");
            }
            return;
        }
        auto work = [this, pc, type, sdp]() {
            auto opt_t = webrtc::SdpTypeFromString(type);
            if (!opt_t.has_value()) {
                if (on_error_) {
                    on_error_("bad SDP type");
                }
                return;
            }
            auto desc = webrtc::CreateSessionDescription(*opt_t, sdp);
            if (!desc) {
                if (on_error_) {
                    on_error_("parse remote SDP failed");
                }
                return;
            }
            auto obs = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
                new webrtc::RefCountedObject<SetRemoteDescObserver>([this](webrtc::RTCError err) {
                    if (!err.ok()) {
                        if (on_error_) {
                            on_error_(std::string("SetRemoteDescription: ") + err.message());
                        }
                        return;
                    }
                    std::cout << "[PullSubscriber] SetRemoteDescription OK, CreateAnswer" << std::endl;
                    CreateAnswer();
                }));
            pc->SetRemoteDescription(std::move(desc), obs);
        };
        if (sig->IsCurrent()) {
            work();
        } else {
            sig->BlockingCall(work);
        }
    }

    void CreateAnswer() {
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
        opts.offer_to_receive_audio = 0;
        opts.num_simulcast_layers = 1;

        auto obs = webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>(
            new webrtc::RefCountedObject<CreateAnswerObserver>(
                [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                    if (!desc) {
                        return;
                    }
                    std::string sdp;
                    if (!desc->ToString(&sdp)) {
                        return;
                    }
                    auto set_local = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
                        new webrtc::RefCountedObject<SetLocalDescObserver>(
                            [this, sdp](webrtc::RTCError err) {
                                if (!err.ok()) {
                                    if (on_error_) {
                                        on_error_(std::string("SetLocalDescription: ") + err.message());
                                    }
                                    return;
                                }
                                signaling_->SendAnswer(sdp);
                                std::cout << "[PullSubscriber] Answer sent" << std::endl;
                            }));
                    peer_connection_->SetLocalDescription(std::move(desc), set_local);
                },
                [this](webrtc::RTCError err) {
                    if (on_error_) {
                        on_error_(std::string("CreateAnswer: ") + err.message());
                    }
                }));

        peer_connection_->CreateAnswer(obs.get(), opts);
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
        if (!pc) {
            return;
        }
        webrtc::Thread* sig = pc->signaling_thread();
        if (!sig) {
            return;
        }
        auto work = [pc, mid, mline_index, candidate]() {
            webrtc::SdpParseError err;
            webrtc::IceCandidateInterface* cand = webrtc::CreateIceCandidate(mid, mline_index, candidate, &err);
            if (!cand) {
                std::cerr << "[PullSubscriber] CreateIceCandidate: " << err.description << std::endl;
                return;
            }
            std::unique_ptr<webrtc::IceCandidateInterface> owned(cand);
            pc->AddIceCandidate(owned.get());
        };
        if (sig->IsCurrent()) {
            work();
        } else {
            sig->BlockingCall(work);
        }
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        AttachVideo(transceiver ? transceiver->receiver() : nullptr);
    }

    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {
        AttachVideo(receiver);
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        if (!candidate || !signaling_) {
            return;
        }
        std::string sdp;
        if (!candidate->ToString(&sdp)) {
            return;
        }
        std::cout << "[PullSubscriber] Send ICE candidate mid=" << candidate->sdp_mid() << std::endl;
        signaling_->SendIceCandidate(candidate->sdp_mid(), candidate->sdp_mline_index(), sdp);
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override {
        if (!on_connection_state_) {
            return;
        }
        PullConnectionState cs = PullConnectionState::New;
        switch (state) {
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
                cs = PullConnectionState::Connecting;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
                cs = PullConnectionState::Connected;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
                cs = PullConnectionState::Disconnected;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
                cs = PullConnectionState::Failed;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
                cs = PullConnectionState::Closed;
                break;
            default:
                break;
        }
        on_connection_state_(cs);
    }

    void DumpInboundFecReceiverStats(std::ostream& out, int timeout_ms) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pc = peer_connection_;
        }
        if (!pc) {
            out << "[fec-verify] fecPacketsReceived=NO_PEER_CONNECTION\n";
            return;
        }

        struct Agg {
            bool had_key{false};
            uint64_t max_val{0};
        };
        auto agg = std::make_shared<Agg>();
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> fut = done->get_future();

        auto cb = webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>(
            new webrtc::RefCountedObject<StatsDeliveredCallback>(
                [agg, done](const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
                    const bool debug = std::getenv("P2P_FEC_STATS_DEBUG") != nullptr;
                    std::ofstream dbg;
                    if (debug) {
                        dbg.open("/tmp/webrtc_fec_stats_debug.txt", std::ios::out | std::ios::trunc);
                    }
                    if (report) {
                        if (debug && dbg) {
                            dbg << report->ToJson() << "\n";
                        }
                        AccumulateFecFromReport(report, &agg->had_key, &agg->max_val);
                        std::string j = report->ToJson();
                        static const char* kJsonKeys[] = {"fecPacketsReceived", "fecBytesReceived", "fecPacketsSent",
                                                          "fecBytesSent"};
                        for (const char* key : kJsonKeys) {
                            uint64_t v = JsonUintAfterKey(j, key);
                            if (v != UINT64_MAX) {
                                agg->had_key = true;
                                if (v > agg->max_val) {
                                    agg->max_val = v;
                                }
                            }
                        }
                    }
                    done->set_value();
                }));

        pc->GetStats(cb.get());

        if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
            out << "[fec-verify] fecPacketsReceived=STATS_TIMEOUT\n";
            return;
        }

        if (agg->had_key) {
            out << "[fec-verify] fecPacketsReceived=" << agg->max_val << "\n";
        } else {
            out << "[fec-verify] fecPacketsReceived=KEY_MISSING\n";
        }
    }

    std::unique_ptr<webrtc_demo::SignalingClient> signaling_;
    PullSubscriberConfig recv_config_;
    std::unique_ptr<webrtc::Thread> owned_signaling_thread_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    std::shared_ptr<VideoSink> video_sink_;
    std::mutex mutex_;
    OnConnectionStateCallback on_connection_state_;
    OnErrorCallback on_error_;
    bool flexfec_enable_{false};
    std::string flexfec_override_;

private:
    void AttachVideo(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> r) {
        if (!r || !video_sink_) {
            return;
        }
        auto track = r->track();
        if (!track || track->kind() != "video") {
            return;
        }
        auto* vt = static_cast<webrtc::VideoTrackInterface*>(track.get());
        if (!vt) {
            return;
        }
        if (recv_config_.jitter_buffer_min_delay_ms >= 0) {
            r->SetJitterBufferMinimumDelay(
                std::optional<double>(static_cast<double>(recv_config_.jitter_buffer_min_delay_ms) / 1000.0));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (video_track_) {
            video_track_->RemoveSink(video_sink_.get());
        }
        video_track_ = vt;
        video_track_->AddOrUpdateSink(video_sink_.get(), webrtc::VideoSinkWants());
        std::cout << "[PullSubscriber] Video track attached" << std::endl;
    }
};

PullSubscriber::PullSubscriber(const std::string& signaling_url,
                               const std::string& stream_id,
                               const PullSubscriberConfig& recv)
    : impl_(std::make_unique<Impl>(signaling_url, stream_id, recv)) {}

PullSubscriber::~PullSubscriber() {
    Stop();
}

void PullSubscriber::SetFlexfecOptions(bool enable, const std::string& field_trials_override) {
    impl_->SetFlexfecOptions(enable, field_trials_override);
}

void PullSubscriber::DumpInboundFecReceiverStats(std::ostream& out, int timeout_ms) {
    impl_->DumpInboundFecReceiverStats(out, timeout_ms);
}

void PullSubscriber::Play() {
    if (is_playing_) {
        return;
    }

    impl_->on_connection_state_ = on_connection_state_;
    impl_->on_error_ = on_error_;
    impl_->video_sink_ = std::make_shared<VideoSink>(on_video_frame_);

    impl_->signaling_->SetOnOffer([this](const std::string& peer_id, const std::string& type,
                                         const std::string& sdp) {
        std::cout << "[PullSubscriber] Received offer from=" << peer_id << " (type=" << type << ", len=" << sdp.size() << ")"
                  << std::endl;
        if (std::getenv("WEBRTC_DUMP_REMOTE_OFFER")) {
            std::cout << "\n--- Remote offer SDP ---\n" << sdp << "\n--- End ---\n" << std::flush;
        }
        impl_->SetRemoteDescription(type, sdp);
    });
    impl_->signaling_->SetOnIce([this](const std::string& peer_id, const std::string& mid, int mline_index,
                                       const std::string& candidate) {
        std::cout << "[PullSubscriber] ICE from=" << peer_id << " mid=" << mid << " idx=" << mline_index << std::endl;
        impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
    });
    impl_->signaling_->SetOnError([this](const std::string& msg) {
        if (on_error_) {
            on_error_(msg);
        }
    });

    if (!impl_->Initialize()) {
        if (on_error_) {
            on_error_("WebRTC init failed");
        }
        return;
    }
    if (!impl_->signaling_->Start()) {
        if (on_error_) {
            on_error_("Signaling failed. Run: ./build/bin/signaling_server");
        }
        return;
    }
    is_playing_ = true;
    std::cout << "[PullSubscriber] Waiting for offer from publisher..." << std::endl;
}

void PullSubscriber::Stop() {
    if (!is_playing_) {
        return;
    }
    impl_->signaling_->Stop();
    impl_->Shutdown();
    is_playing_ = false;
}

}  // namespace webrtc_demo
