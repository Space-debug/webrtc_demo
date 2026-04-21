#include "push_streamer.h"

#include "api/array_view.h"
#include "camera/camera_utils.h"
#include "media/camera_video_track_source.h"
#include "webrtc_peer_connection_factory.h"

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/priority.h"
#include "api/rtc_error.h"
#include "api/rtp_parameters.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webrtc_demo {

namespace {

bool SignalingTimingTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("WEBRTC_DEMO_SIGNALING_TIMING_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}

int64_t SignalingNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void TraceSigTiming(const std::string& msg) {
    if (!SignalingTimingTraceEnabled()) {
        return;
    }
    std::cout << "[SIG_TIMING][push] t_us=" << SignalingNowUs() << " " << msg << std::endl;
}

/// 部分平台在双 PC 回环下 PeerConnection::Close 可能长期阻塞；超时后放弃等待，由进程退出收尾。
/// @return true 表示 Close 在线程内已返回；false 表示超时（可能仍有后台线程卡在 Close 内）。
static bool ClosePeerConnectionWithDeadline(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                                            const char* log_tag,
                                            int timeout_sec) {
    if (!pc) {
        return true;
    }
    std::packaged_task<void()> task([pc]() { pc->Close(); });
    std::future<void> done = task.get_future();
    std::thread worker(std::move(task));
    if (done.wait_for(std::chrono::seconds(timeout_sec)) != std::future_status::ready) {
        std::cerr << "[PushStreamer] " << log_tag << " PeerConnection::Close exceeded " << timeout_sec
                  << "s; continuing shutdown\n"
                  << std::flush;
        worker.detach();
        return false;
    }
    worker.join();
    return true;
}

static void StopWebrtcThreadWithDeadline(webrtc::Thread* thread, int timeout_sec) {
    if (!thread) {
        return;
    }
    std::packaged_task<void()> task([thread]() { thread->Stop(); });
    std::future<void> done = task.get_future();
    std::thread worker(std::move(task));
    if (done.wait_for(std::chrono::seconds(timeout_sec)) != std::future_status::ready) {
        std::cerr << "[PushStreamer] webrtc signaling Thread::Stop exceeded " << timeout_sec
                  << "s; continuing shutdown\n"
                  << std::flush;
        worker.detach();
        return;
    }
    worker.join();
}

static webrtc::Priority ParseVideoNetworkPriority(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (unsigned char c : s) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    if (lower == "very_low" || lower == "verylow") {
        return webrtc::Priority::kVeryLow;
    }
    if (lower == "low") {
        return webrtc::Priority::kLow;
    }
    if (lower == "medium") {
        return webrtc::Priority::kMedium;
    }
    if (lower == "high") {
        return webrtc::Priority::kHigh;
    }
    return webrtc::Priority::kHigh;
}

webrtc::PeerConnectionInterface::RTCConfiguration MakeRtcConfiguration(const PushStreamerConfig& config) {
    webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.push_back(config.common.stun_server);
    rtc_config.servers.push_back(stun);
    if (!config.common.turn_server.empty()) {
        webrtc::PeerConnectionInterface::IceServer turn;
        turn.urls.push_back(config.common.turn_server);
        turn.username = config.common.turn_username;
        turn.password = config.common.turn_password;
        rtc_config.servers.push_back(turn);
    }
    rtc_config.disable_ipv6_on_wifi = true;
    rtc_config.max_ipv6_networks = 0;
    rtc_config.disable_link_local_networks = true;
    rtc_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    rtc_config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
    rtc_config.set_dscp(true);
    rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    rtc_config.prioritize_most_likely_ice_candidate_pairs = config.common.ice_prioritize_likely_pairs;
    return rtc_config;
}

}  // namespace

class FrameCountingSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using OnFrameCallback = webrtc_demo::OnFrameCallback;
    explicit FrameCountingSink(OnFrameCallback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(const webrtc::VideoFrame& frame) override {
        unsigned int n = ++frame_count_;
        if (on_frame_) {
            on_frame_(n, frame.width(), frame.height());
        }
    }

    unsigned int GetFrameCount() const { return frame_count_.load(); }

private:
    std::atomic<unsigned int> frame_count_{0};
    OnFrameCallback on_frame_;
};

class DecodedFrameSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    void OnFrame(const webrtc::VideoFrame&) override { ++count_; }
    unsigned GetCount() const { return count_.load(); }

private:
    std::atomic<unsigned int> count_{0};
};

class LoopbackPcObserver : public webrtc::PeerConnectionObserver {
public:
    using AddCandidateFn = std::function<void(const std::string&, int, const std::string&)>;

    explicit LoopbackPcObserver(AddCandidateFn add_to_sender) : add_to_sender_(std::move(add_to_sender)) {}

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        if (!add_to_sender_ || !candidate) {
            return;
        }
        std::string sdp;
        if (!candidate->ToString(&sdp)) {
            return;
        }
        add_to_sender_(candidate->sdp_mid(), candidate->sdp_mline_index(), sdp);
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        AttachSink(transceiver ? transceiver->receiver() : nullptr);
    }

    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {
        AttachSink(receiver);
    }

    unsigned int GetDecodedCount() const {
        return decoded_sink_ ? decoded_sink_->GetCount() : 0;
    }

    void Teardown() {
        if (decoded_track_ && decoded_sink_) {
            decoded_track_->RemoveSink(decoded_sink_.get());
        }
        decoded_sink_.reset();
        decoded_track_ = nullptr;
    }

private:
    void AttachSink(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> r) {
        if (!r) {
            return;
        }
        auto track = r->track();
        if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
            return;
        }
        auto* vt = static_cast<webrtc::VideoTrackInterface*>(track.get());
        if (!vt) {
            return;
        }
        if (!decoded_sink_) {
            decoded_sink_ = std::make_unique<DecodedFrameSink>();
        }
        // OnTrack + OnAddTrack 可能各回调一次；换 track 时必须先从旧 track 摘掉 sink，否则 Teardown 后旧 track
        // 仍向已销毁的 DecodedFrameSink 投递帧 → 段错误。
        if (decoded_track_.get() == vt) {
            return;
        }
        if (decoded_track_) {
            decoded_track_->RemoveSink(decoded_sink_.get());
        }
        decoded_track_ = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(vt);
        decoded_track_->AddOrUpdateSink(decoded_sink_.get(), webrtc::VideoSinkWants());
    }

    AddCandidateFn add_to_sender_;
    std::unique_ptr<DecodedFrameSink> decoded_sink_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> decoded_track_;
};

namespace {

webrtc::PeerConnectionInterface::RTCOfferAnswerOptions MakeOfferOptions() {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions o;
    o.num_simulcast_layers = 1;
    o.offer_to_receive_audio = 0;
    return o;
}

std::string MimeLower(const webrtc::RtpCodecCapability& c) {
    std::string m = c.mime_type();
    for (auto& ch : m) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return m;
}

std::string NormalizeProfileLevelIdString(std::string v) {
    for (auto& ch : v) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) {
        ++i;
    }
    v = v.substr(i);
    if (v.size() >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v = v.substr(2);
    }
    if (v.size() > 6) {
        v.resize(6);
    }
    return v;
}

/// 仅比对 profile-level-id 前两 hex（profile_idc），与 libwebrtc 常见 4d00xx/42e0xx 等格式兼容
std::string H264ProfileIdcHex2(const std::string& profile) {
    std::string p = profile;
    for (auto& c : p) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (p == "main" || p == "m") {
        return "4d";
    }
    if (p == "high" || p == "h") {
        return "64";
    }
    if (p == "baseline" || p == "base" || p == "b") {
        return "42";
    }
    return "4d";
}

bool H264CodecMatchesConfiguredProfile(const webrtc::RtpCodecCapability& c, const std::string& want_prof_idc2) {
    if (c.name != "H264") {
        return true;
    }
    auto it = c.parameters.find("profile-level-id");
    if (it == c.parameters.end()) {
        return true;
    }
    std::string cap = NormalizeProfileLevelIdString(it->second);
    if (cap.size() < 2) {
        return true;
    }
    return cap.substr(0, 2) == want_prof_idc2;
}

}  // namespace

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    using SuccessFn = std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>)>;
    using FailFn = std::function<void(webrtc::RTCError)>;

    CreateSdpObserver(SuccessFn on_ok, FailFn on_fail)
        : on_ok_(std::move(on_ok)), on_fail_(std::move(on_fail)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::unique_ptr<webrtc::SessionDescriptionInterface> owned(desc);
        if (on_ok_) {
            on_ok_(std::move(owned));
        }
    }

    void OnFailure(webrtc::RTCError err) override {
        if (on_fail_) {
            on_fail_(std::move(err));
        }
    }

private:
    SuccessFn on_ok_;
    FailFn on_fail_;
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

static bool LatencyTraceEnabled() {
    static int cached = -1;
    if (cached >= 0) {
        return cached != 0;
    }
    const char* e = std::getenv("WEBRTC_LATENCY_TRACE");
    cached = (e && e[0] == '1') ? 1 : 0;
    return cached != 0;
}

static int ReadEnvInt(const char* name, int fallback, int min_v, int max_v) {
    const char* e = std::getenv(name);
    if (!e || !e[0]) {
        return fallback;
    }
    char* end = nullptr;
    const long v = std::strtol(e, &end, 10);
    if (end == e || (end && *end != '\0')) {
        return fallback;
    }
    if (v < min_v) {
        return min_v;
    }
    if (v > max_v) {
        return max_v;
    }
    return static_cast<int>(v);
}

class OutboundVideoStatsLogger : public webrtc::RTCStatsCollectorCallback {
public:
    explicit OutboundVideoStatsLogger(std::string pc_tag) : pc_tag_(std::move(pc_tag)) {}

    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        if (!report) {
            return;
        }
        const std::vector<const webrtc::RTCOutboundRtpStreamStats*> outbound =
            report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>();
        for (const webrtc::RTCOutboundRtpStreamStats* s : outbound) {
            if (!s->kind.has_value() || *s->kind != "video") {
                continue;
            }
            std::cout << "[OutboundVideoStats] pc=" << pc_tag_ << " id=" << s->id();
            if (s->ssrc.has_value()) {
                std::cout << " ssrc=" << *s->ssrc;
            }
            if (s->frames_encoded.has_value()) {
                std::cout << " frames_encoded=" << *s->frames_encoded;
            }
            if (s->key_frames_encoded.has_value()) {
                std::cout << " key_frames_encoded=" << *s->key_frames_encoded;
            }
            if (s->packets_sent.has_value()) {
                std::cout << " packets_sent=" << *s->packets_sent;
            }
            if (s->bytes_sent.has_value()) {
                std::cout << " bytes_sent=" << *s->bytes_sent;
            }
            if (s->retransmitted_packets_sent.has_value()) {
                std::cout << " retrans_pkts=" << *s->retransmitted_packets_sent;
            }
            if (s->quality_limitation_reason.has_value()) {
                std::cout << " ql_reason=" << *s->quality_limitation_reason;
            }
            std::cout << std::endl;
        }
    }

private:
    std::string pc_tag_;
};

class PushStreamer::Impl : public webrtc::PeerConnectionObserver {
public:
    explicit Impl(const PushStreamerConfig& config) : config_(config) {}

    class ExtraPeerObserver : public webrtc::PeerConnectionObserver {
    public:
        ExtraPeerObserver(Impl* owner, std::string peer_id) : owner_(owner), peer_id_(std::move(peer_id)) {}

        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
        void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
        void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
            if (owner_ && candidate) {
                owner_->OnPeerIceCandidate(peer_id_, candidate);
            }
        }
        void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) override {
            if (owner_) {
                owner_->NotifyConnectionState(s);
            }
        }

    private:
        Impl* owner_;
        std::string peer_id_;
    };

    bool Initialize() {
        std::cout << "[PushStreamer] Initializing WebRTC (native API)..." << std::endl;
        webrtc_demo::EnsureWebrtcFieldTrialsInitialized();
        if (!webrtc::InitializeSSL()) {
            std::cerr << "[PushStreamer] InitializeSSL failed" << std::endl;
            return false;
        }

        webrtc::PeerConnectionFactoryDependencies deps;
        webrtc_demo::PeerConnectionFactoryMediaOptions media_opts;
        media_opts.encoder_backend = config_.backend.use_rockchip_mpp_h264
                                         ? webrtc_demo::VideoCodecBackendPreference::kRockchipMpp
                                         : webrtc_demo::VideoCodecBackendPreference::kBuiltin;
        webrtc_demo::ConfigurePeerConnectionFactoryDependencies(deps, &media_opts);
        webrtc_demo::EnsureDedicatedPeerConnectionSignalingThread(deps, &owned_signaling_thread_);

        factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
        if (!factory_) {
            std::cerr << "[PushStreamer] CreateModularPeerConnectionFactory failed" << std::endl;
            return false;
        }
        std::cout << "[PushStreamer] PeerConnectionFactory created" << std::endl;

        return CreatePeerConnection();
    }

    void Shutdown() {
        StopOutboundStatsLoop();
        if (frame_counter_ && camera_source_) {
            camera_source_->RemoveSink(frame_counter_.get());
        }
        frame_counter_.reset();
        camera_impl_ = nullptr;

        // 回环：先关 receiver（易阻塞，带超时），再关 sender 与多路 PC（与 pull_subscriber 一致，均在 Shutdown 调用线程上 Close）。
        if (loopback_observer_) {
            loopback_observer_->Teardown();
        }
        if (receiver_) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> recv = receiver_;
            receiver_ = nullptr;
            ClosePeerConnectionWithDeadline(recv, "loopback receiver", 8);
        }
        loopback_observer_.reset();

        if (peer_connection_) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> sender = peer_connection_;
            peer_connection_ = nullptr;
            ClosePeerConnectionWithDeadline(sender, "publisher", 8);
        }
        for (auto& kv : peer_connections_) {
            if (kv.second) {
                ClosePeerConnectionWithDeadline(kv.second, "subscriber", 8);
            }
        }
        peer_connections_.clear();
        extra_peer_observers_.clear();

        video_track_ = nullptr;
        camera_source_ = nullptr;
        camera_impl_ = nullptr;

        factory_ = nullptr;

        if (owned_signaling_thread_) {
            if (!owned_signaling_thread_->IsCurrent()) {
                StopWebrtcThreadWithDeadline(owned_signaling_thread_.get(), 6);
            }
            owned_signaling_thread_.reset();
        }

        webrtc::CleanupSSL();
    }

    bool CreatePeerConnection() {
        auto rtc_config = MakeRtcConfiguration(config_);
        auto pc = CreatePcWithObserver(this, rtc_config);
        if (!pc) {
            return false;
        }
        peer_connection_ = pc;

        if (!CreateMediaTracks()) {
            return false;
        }
        if (!video_track_) {
            std::cerr << "[PushStreamer] No video track" << std::endl;
            return false;
        }

        std::vector<std::string> stream_ids = {config_.common.stream_id};
        // 多订阅者 + 仅对订阅者发 Offer：同一 VideoTrack 不要同时挂到「占位」默认 PC 与订阅者 PC，
        // 否则部分 libwebrtc 版本在第二路 CreateOffer 上可能长期不回调（拉流端收不到 SDP）。
        if (!config_.common.signaling_subscriber_offer_only) {
            auto add = peer_connection_->AddTrack(video_track_, stream_ids);
            if (!add.ok()) {
                std::cerr << "[PushStreamer] AddTrack failed: " << add.error().message() << std::endl;
                return false;
            }
            ApplyVideoCodecPreferences(peer_connection_);
            ApplyEncodingParameters(peer_connection_);
            std::cout << "[PushStreamer] Video track added (stream_id=" << config_.common.stream_id << ")" << std::endl;
            MaybeStartOutboundStatsLoop();
        } else {
            std::cout << "[PushStreamer] Video track ready (subscriber-offer-only; sender per subscriber PC, "
                         "stream_id="
                      << config_.common.stream_id << ")" << std::endl;
        }
        return true;
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> CreatePcWithObserver(
        webrtc::PeerConnectionObserver* observer,
        const webrtc::PeerConnectionInterface::RTCConfiguration& cfg) {
        webrtc::PeerConnectionDependencies deps(observer);
        auto result = factory_->CreatePeerConnectionOrError(cfg, std::move(deps));
        if (!result.ok()) {
            std::cerr << "[PushStreamer] CreatePeerConnection failed: " << result.error().message() << std::endl;
            return nullptr;
        }
        return result.MoveValue();
    }

    void ApplyVideoCodecPreferences(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        if (!factory_ || !pc) {
            return;
        }
        std::string want = config_.common.video_codec;
        for (auto& ch : want) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (want.empty()) {
            want = "h264";
        }

        webrtc::RtpCapabilities caps = factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
        if (caps.codecs.empty()) {
            std::cerr << "[PushStreamer] GetRtpSenderCapabilities(VIDEO) empty" << std::endl;
            return;
        }

        auto match_want = [&](const std::string& m) -> bool {
            if (want == "h264") {
                return m.find("h264") != std::string::npos;
            }
            if (want == "h265" || want == "hevc") {
                return m.find("h265") != std::string::npos || m.find("hevc") != std::string::npos ||
                       m.find("hev1") != std::string::npos;
            }
            if (want == "vp8") {
                return m.find("vp8") != std::string::npos;
            }
            if (want == "vp9") {
                return m.find("vp9") != std::string::npos;
            }
            if (want == "av1") {
                return m.find("av1") != std::string::npos;
            }
            return m.find(want) != std::string::npos;
        };

        std::vector<webrtc::RtpCodecCapability> preferred;
        std::vector<webrtc::RtpCodecCapability> other;
        for (const auto& c : caps.codecs) {
            if (match_want(MimeLower(c))) {
                preferred.push_back(c);
            } else {
                other.push_back(c);
            }
        }
        if (preferred.empty()) {
            std::cout << "[PushStreamer] SetCodecPreferences skipped: no match for VIDEO_CODEC=" << config_.common.video_codec
                      << std::endl;
            return;
        }
        if (want == "h264") {
            const std::string want_idc = H264ProfileIdcHex2(config_.common.h264_profile);
            std::vector<webrtc::RtpCodecCapability> filtered;
            filtered.reserve(preferred.size());
            for (const auto& c : preferred) {
                if (H264CodecMatchesConfiguredProfile(c, want_idc)) {
                    filtered.push_back(c);
                }
            }
            if (!filtered.empty()) {
                preferred = std::move(filtered);
                std::cout << "[PushStreamer] H264 profile filter: profile_idc=0x" << want_idc << " (H264_PROFILE="
                          << config_.common.h264_profile << ")" << std::endl;
            } else {
                std::cout << "[PushStreamer] H264 profile filter skipped: no payload matched profile_idc=0x" << want_idc
                          << ", using all H264 payloads" << std::endl;
            }
        }
        std::vector<webrtc::RtpCodecCapability> ordered;
        ordered.reserve(preferred.size() + other.size());
        ordered.insert(ordered.end(), preferred.begin(), preferred.end());
        ordered.insert(ordered.end(), other.begin(), other.end());

        for (auto tr : pc->GetTransceivers()) {
            if (!tr || tr->media_type() != webrtc::MediaType::VIDEO) {
                continue;
            }
            auto err = tr->SetCodecPreferences(webrtc::ArrayView<webrtc::RtpCodecCapability>(
                ordered.data(), ordered.size()));
            if (!err.ok()) {
                std::cerr << "[PushStreamer] SetCodecPreferences: " << err.message() << std::endl;
            } else {
                std::cout << "[PushStreamer] SetCodecPreferences: prefer " << want << std::endl;
            }
            return;
        }
    }

    void ApplyEncodingParameters(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        for (auto tr : pc->GetTransceivers()) {
            if (!tr || tr->media_type() != webrtc::MediaType::VIDEO) {
                continue;
            }
            auto sender = tr->sender();
            if (!sender) {
                continue;
            }
            webrtc::RtpParameters params = sender->GetParameters();
            const webrtc::Priority net_prio = ParseVideoNetworkPriority(config_.common.video_network_priority);
            const int fps_cap = (config_.common.video_encoding_max_framerate > 0) ? config_.common.video_encoding_max_framerate
                                                                           : config_.common.video_fps;
            const double max_fps = (fps_cap > 0) ? static_cast<double>(fps_cap) : 30.0;
            for (auto& enc : params.encodings) {
                enc.min_bitrate_bps = config_.common.min_bitrate_kbps * 1000;
                enc.max_bitrate_bps = config_.common.max_bitrate_kbps * 1000;
                enc.network_priority = net_prio;
                enc.max_framerate = max_fps;
            }
            if (config_.common.degradation_preference == "maintain_resolution") {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
            } else if (config_.common.degradation_preference == "maintain_framerate") {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
            } else if (config_.common.degradation_preference == "balanced") {
                params.degradation_preference = webrtc::DegradationPreference::BALANCED;
            } else {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
            }
            auto err = sender->SetParameters(params);
            if (!err.ok()) {
                std::cerr << "[PushStreamer] SetParameters failed: " << err.message() << std::endl;
            } else {
                webrtc::BitrateSettings br;
                const int min_bps = std::max(0, config_.common.min_bitrate_kbps * 1000);
                const int target_bps = std::max(min_bps, config_.common.target_bitrate_kbps * 1000);
                const int max_bps = std::max(target_bps, config_.common.max_bitrate_kbps * 1000);
                br.min_bitrate_bps = min_bps;
                br.start_bitrate_bps = target_bps;
                br.max_bitrate_bps = max_bps;
                auto br_err = pc->SetBitrate(br);
                if (!br_err.ok()) {
                    std::cerr << "[PushStreamer] SetBitrate failed: " << br_err.message() << std::endl;
                } else {
                    std::cout << "[PushStreamer] SetBitrate: min/start/max=" << min_bps << "/" << target_bps
                              << "/" << max_bps << " bps" << std::endl;
                }
                std::cout << "[PushStreamer] Encoding params: bitrate " << config_.common.min_bitrate_kbps << "-"
                          << config_.common.max_bitrate_kbps << " kbps"
                          << " max_fps=" << max_fps
                          << " network_priority=" << config_.common.video_network_priority << std::endl;
                if (LatencyTraceEnabled()) {
                    const char* deg = "maintain_framerate";
                    if (config_.common.degradation_preference == "maintain_resolution") {
                        deg = "maintain_resolution";
                    } else if (config_.common.degradation_preference == "balanced") {
                        deg = "balanced";
                    }
                    std::cout << "[Latency] WebRTC degradation_preference=" << deg
                              << " (弱网时影响降质策略与排队感知)\n";
                }
            }
            break;
        }
    }

    void MaybeStartOutboundStatsLoop() {
        const int interval_sec = ReadEnvInt("WEBRTC_PUSH_OUTBOUND_STATS_INTERVAL_SEC", 0, 0, 60);
        if (interval_sec <= 0 || outbound_stats_started_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        outbound_stats_stop_.store(false, std::memory_order_release);
        std::cout << "[PushStreamer] Outbound stats enabled, interval=" << interval_sec << "s" << std::endl;
        outbound_stats_thread_ = std::thread([this, interval_sec]() {
            while (!outbound_stats_stop_.load(std::memory_order_acquire)) {
                auto [pc, tag] = SelectStatsPeerConnection();
                if (pc) {
                    auto cb = webrtc::make_ref_counted<OutboundVideoStatsLogger>(tag);
                    pc->GetStats(cb.get());
                }
                for (int i = 0; i < interval_sec * 10; ++i) {
                    if (outbound_stats_stop_.load(std::memory_order_acquire)) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    }

    void StopOutboundStatsLoop() {
        outbound_stats_stop_.store(true, std::memory_order_release);
        if (outbound_stats_thread_.joinable()) {
            outbound_stats_thread_.join();
        }
        outbound_stats_started_.store(false, std::memory_order_release);
    }

    std::pair<webrtc::scoped_refptr<webrtc::PeerConnectionInterface>, std::string> SelectStatsPeerConnection() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : peer_connections_) {
            if (kv.second) {
                return {kv.second, kv.first};
            }
        }
        if (peer_connection_) {
            return {peer_connection_, "default"};
        }
        return {nullptr, "none"};
    }

    bool ResolveDeviceUniqueId(std::string* out_unique) {
        // 显式 /dev/videoN 时优先按路径直采，勿依赖 WebRTC 枚举（枚举偶发为 0 时仍应能推流）。
        // 是否 CAPTURE 由 CameraVideoTrackSource::StartDirectV4l2 再验；此处不要求枚举下标算成功。
        if (!config_.common.video_device_path.empty()) {
            const std::string& p = config_.common.video_device_path;
            if (p.rfind("/dev/video", 0) == 0 && access(p.c_str(), R_OK | W_OK) == 0) {
                *out_unique = p;
                std::cout << "[PushStreamer] Camera " << p << " -> capture by device path (multi-node safe)"
                          << std::endl;
                return true;
            }
        }

        std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(webrtc::VideoCaptureFactory::CreateDeviceInfo());
        if (!info) {
            std::cerr << "[PushStreamer] CreateDeviceInfo failed" << std::endl;
            return false;
        }
        uint32_t n = info->NumberOfDevices();
        if (n == 0) {
            std::cerr << "[PushStreamer] No V4L2/video capture devices" << std::endl;
            return false;
        }

        if (!config_.common.video_device_path.empty()) {
            // 与 device_info_v4l2 一致：/dev/videoN → 第 N' 个 CAPTURE 节点的枚举下标（非路径配置等）
            {
                int path_idx = webrtc_demo::GetWebRtcCaptureDeviceIndexForPath(config_.common.video_device_path);
                if (path_idx >= 0 && static_cast<uint32_t>(path_idx) < n) {
                    char name[256] = {0};
                    char unique[256] = {0};
                    char product[256] = {0};
                    if (info->GetDeviceName(static_cast<uint32_t>(path_idx), name, sizeof(name), unique, sizeof(unique),
                                            product, sizeof(product)) == 0) {
                        *out_unique = unique;
                        std::cout << "[PushStreamer] Camera path " << config_.common.video_device_path
                                  << " -> /dev/video enum index match" << std::endl;
                        return true;
                    }
                }
            }
            // Linux 上 GetDeviceName 的 unique 实为 V4L2 bus_info（product 常为空，勿用 product==bus）
            std::string bus = GetDeviceBusInfo(config_.common.video_device_path);
            for (uint32_t i = 0; i < n; ++i) {
                char name[256] = {0};
                char unique[256] = {0};
                char product[256] = {0};
                if (info->GetDeviceName(i, name, sizeof(name), unique, sizeof(unique), product, sizeof(product)) != 0) {
                    continue;
                }
                if (!bus.empty() && std::string(unique) == bus) {
                    *out_unique = unique;
                    std::cout << "[PushStreamer] Camera path " << config_.common.video_device_path << " -> bus_info match"
                              << std::endl;
                    return true;
                }
            }
            for (uint32_t i = 0; i < n; ++i) {
                char name[256] = {0};
                char unique[256] = {0};
                char product[256] = {0};
                if (info->GetDeviceName(i, name, sizeof(name), unique, sizeof(unique), product, sizeof(product)) != 0) {
                    continue;
                }
                if (std::string(unique).find(config_.common.video_device_path) != std::string::npos) {
                    *out_unique = unique;
                    return true;
                }
            }
            // WebRTC 的 unique 往往不含 /dev/videoN；用 V4L2 card 与枚举设备名对齐（常见）
            {
                std::string card = webrtc_demo::GetDeviceCardName(config_.common.video_device_path);
                if (!card.empty()) {
                    for (uint32_t i = 0; i < n; ++i) {
                        char name[256] = {0};
                        char unique[256] = {0};
                        char product[256] = {0};
                        if (info->GetDeviceName(i, name, sizeof(name), unique, sizeof(unique), product, sizeof(product)) !=
                            0) {
                            continue;
                        }
                        if (std::string(name) == card) {
                            *out_unique = unique;
                            std::cout << "[PushStreamer] Camera path " << config_.common.video_device_path
                                      << " -> device name/card match" << std::endl;
                            return true;
                        }
                    }
                }
            }
            std::cerr << "[PushStreamer] No device match for " << config_.common.video_device_path << ", using index 0"
                      << std::endl;
        }

        uint32_t idx = static_cast<uint32_t>(config_.common.video_device_index);
        if (idx >= n) {
            std::cerr << "[PushStreamer] Device index out of range" << std::endl;
            return false;
        }
        char name[256] = {0};
        char unique[256] = {0};
        if (info->GetDeviceName(idx, name, sizeof(name), unique, sizeof(unique)) != 0) {
            return false;
        }
        *out_unique = unique;
        return true;
    }

    bool CreateMediaTracks() {
        std::string unique_id;
        if (!ResolveDeviceUniqueId(&unique_id)) {
            return false;
        }

        auto* cam_holder = new webrtc::RefCountedObject<CameraVideoTrackSource>();
        camera_impl_ = static_cast<CameraVideoTrackSource*>(cam_holder);
        camera_source_ = webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(
            static_cast<webrtc::VideoTrackSourceInterface*>(camera_impl_));

        // 须在 Start()/采集线程起来之前注册 sink，否则早期帧进 broadcaster 时 sink 列表仍为空。
        if (!frame_counter_) {
            frame_counter_ = std::make_unique<FrameCountingSink>(on_frame_);
            camera_source_->AddOrUpdateSink(frame_counter_.get(), webrtc::VideoSinkWants());
        }

        bool mpp_mjpeg_decode = config_.backend.use_rockchip_mpp_mjpeg_decode;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
        bool allow_dual_mpp = config_.backend.use_rockchip_dual_mpp_mjpeg_h264;
        if (const char* ev = std::getenv("WEBRTC_DUAL_MPP_MJPEG_H264")) {
            if (ev[0] == '1' || ev[0] == 'y' || ev[0] == 'Y' || ev[0] == 't' || ev[0] == 'T') {
                allow_dual_mpp = true;
            }
        }
        if (mpp_mjpeg_decode && config_.backend.use_rockchip_mpp_h264 && !allow_dual_mpp) {
            mpp_mjpeg_decode = false;
            std::cout << "[PushStreamer] MPP MJPEG decode off while MPP H.264 encode on (use libyuv for MJPEG). "
                         "Set USE_DUAL_MPP_MJPEG_H264=1 or WEBRTC_DUAL_MPP_MJPEG_H264=1 to enable both.\n";
        } else if (mpp_mjpeg_decode && config_.backend.use_rockchip_mpp_h264 && allow_dual_mpp) {
            std::cout << "[PushStreamer] Dual MPP: MJPEG hardware decode + H.264 hardware encode (experimental).\n";
        }
#endif
        V4l2MjpegPipelineOptions mjpeg_pipe;
        mjpeg_pipe.mjpeg_queue_latest_only = config_.backend.mjpeg_queue_latest_only;
        mjpeg_pipe.mjpeg_queue_max = config_.backend.mjpeg_queue_max;
        mjpeg_pipe.nv12_pool_slots = config_.backend.nv12_pool_slots;
        mjpeg_pipe.v4l2_buffer_count = config_.backend.v4l2_buffer_count;
        mjpeg_pipe.v4l2_poll_timeout_ms = config_.backend.v4l2_poll_timeout_ms;
        mjpeg_pipe.mjpeg_decode_inline = config_.backend.mjpeg_decode_inline;
        mjpeg_pipe.mjpeg_v4l2_ext_dma = config_.backend.mjpeg_v4l2_ext_dma;
        mjpeg_pipe.mjpeg_rga_to_mpp = config_.backend.mjpeg_rga_to_mpp;
        if (!static_cast<CameraVideoTrackSource*>(cam_holder)
                 ->Start(unique_id.c_str(), config_.common.video_width, config_.common.video_height, config_.common.video_fps,
                         mpp_mjpeg_decode, &mjpeg_pipe)) {
            std::cerr << "[PushStreamer] CameraVideoTrackSource::Start failed" << std::endl;
            if (frame_counter_ && camera_source_) {
                camera_source_->RemoveSink(frame_counter_.get());
            }
            frame_counter_.reset();
            camera_source_ = nullptr;
            camera_impl_ = nullptr;
            return false;
        }

        video_track_ = factory_->CreateVideoTrack(camera_source_, "video_track");
        if (!video_track_) {
            std::cerr << "[PushStreamer] CreateVideoTrack failed" << std::endl;
            return false;
        }
        // 提示发送链按「运动/实时」内容处理，利于码控与帧类型决策（见 VideoTrackInterface::ContentHint）。
        video_track_->set_content_hint(webrtc::VideoTrackInterface::ContentHint::kFluid);

        std::cout << "[PushStreamer] Video capture started" << std::endl;

        {
            int cam_fps = 0;
            if (camera_impl_->GetNegotiatedCaptureFramerate(&cam_fps) && cam_fps > 0) {
                if (cam_fps != config_.common.video_fps) {
                    std::cout << "[PushStreamer] Using camera actual frame rate " << cam_fps << " fps (config FPS="
                              << config_.common.video_fps << " was request only; encoding/WebRTC caps follow device)\n";
                }
                config_.common.video_fps = cam_fps;
            }
        }

        {
            int nw = 0;
            int nh = 0;
            if (camera_impl_->GetNegotiatedCaptureSize(&nw, &nh) &&
                (nw != config_.common.video_width || nh != config_.common.video_height)) {
                std::cout << "[PushStreamer] V4L2 negotiated " << nw << "x" << nh << ", config requests "
                          << config_.common.video_width << "x" << config_.common.video_height
                          << " — set WIDTH/HEIGHT in streams.conf to match to reduce capture/encode scaling.\n";
            }
        }

        if (config_.common.capture_warmup_sec > 0) {
            std::cout << "[PushStreamer] Camera warmup " << config_.common.capture_warmup_sec << "s..." << std::endl;
            const auto w0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::seconds(config_.common.capture_warmup_sec));
            if (LatencyTraceEnabled()) {
                const auto wms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0)
                                     .count();
                std::cout << "[Latency] capture_warmup actual_ms=" << wms << " configured_sec=" << config_.common.capture_warmup_sec
                          << std::endl;
            }
        }
        return true;
    }

    bool WaitForCaptureGate(const std::string& context) {
        const int need = config_.common.capture_gate_min_frames;
        if (need <= 0 || !frame_counter_) {
            return true;
        }
        int max_wait = config_.common.capture_gate_max_wait_sec;
        if (max_wait < 1) {
            max_wait = 1;
        }
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(max_wait);
        const auto gate_t0 = clock::now();
        std::cout << "[PushStreamer] Capture gate: need >= " << need << " frames (" << context << "), max wait "
                  << max_wait << "s" << std::endl;
        while (clock::now() < deadline) {
            const unsigned int got = EffectiveCaptureFrameCount();
            if (got >= static_cast<unsigned int>(need)) {
                std::cout << "[PushStreamer] Capture gate OK: " << got << " frames" << std::endl;
                if (LatencyTraceEnabled()) {
                    const auto gms =
                        std::chrono::duration<double, std::milli>(clock::now() - gate_t0).count();
                    std::cout << "[Latency] capture_gate elapsed_ms=" << gms << " frames=" << got << " need=" << need
                              << std::endl;
                }
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        std::cerr << "[PushStreamer] Capture gate failed (" << context << ")" << std::endl;
        if (LatencyTraceEnabled()) {
            const auto gms = std::chrono::duration<double, std::milli>(clock::now() - gate_t0).count();
            std::cout << "[Latency] capture_gate TIMEOUT elapsed_ms=" << gms << " last_frames=" << EffectiveCaptureFrameCount()
                      << " need=" << need << std::endl;
        }
        return false;
    }

    void CreateOffer() {
        if (!WaitForCaptureGate("default")) {
            return;
        }
        if (!peer_connection_) {
            return;
        }
        webrtc::Thread* sig = peer_connection_->signaling_thread();
        if (!sig) {
            std::cerr << "[PushStreamer] CreateOffer: no signaling thread" << std::endl;
            return;
        }
        std::cout << "[PushStreamer] Creating default Offer..." << std::endl;
        auto run = [this]() { CreateOfferOnConnection("", peer_connection_); };
        if (sig->IsCurrent()) {
            run();
        } else {
            sig->BlockingCall(run);
        }
    }

    void CreateOfferForPeer(const std::string& peer_id) {
        if (peer_id.empty()) {
            CreateOffer();
            return;
        }
        if (!peer_connection_) {
            std::cerr << "[PushStreamer] CreateOfferForPeer: no default peer connection" << std::endl;
            return;
        }
        if (!WaitForCaptureGate(std::string("peer=") + peer_id)) {
            return;
        }
        TraceSigTiming("CreateOfferForPeer gate_ok peer=" + peer_id);
        webrtc::Thread* sig = peer_connection_->signaling_thread();
        if (!sig) {
            std::cerr << "[PushStreamer] CreateOfferForPeer: no signaling thread" << std::endl;
            return;
        }
        // AddTrack / CreateOffer 必须在专用 signaling 线程（见 EnsureDedicatedPeerConnectionSignalingThread）。
        auto run = [this, peer_id]() {
            if (!EnsurePeerConnectionForPeer(peer_id)) {
                return;
            }
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = peer_connections_.find(peer_id);
                if (it != peer_connections_.end()) {
                    pc = it->second;
                }
            }
            if (!pc) {
                std::cerr << "[PushStreamer] peer not found: " << peer_id << std::endl;
                return;
            }
            std::cout << "[PushStreamer] Creating Offer for subscriber: " << peer_id << std::endl;
            CreateOfferOnConnection(peer_id, pc);
        };
        if (sig->IsCurrent()) {
            run();
        } else {
            sig->BlockingCall(run);
        }
    }

    bool EnsurePeerConnectionForPeer(const std::string& peer_id) {
        if (peer_id.empty()) {
            return peer_connection_ != nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (peer_connections_.find(peer_id) != peer_connections_.end()) {
                return true;
            }
        }
        // 禁止在持 mutex_ 期间 CreatePeerConnection/AddTrack：WebRTC 可能同步触发观察者回调，
        // 与主线程里其它持 mutex_ 的逻辑抢锁会死锁 → 不发 Offer、拉流永远 0 帧。
        auto observer = std::make_unique<ExtraPeerObserver>(this, peer_id);
        auto pc = CreatePcWithObserver(observer.get(), MakeRtcConfiguration(config_));
        if (!pc) {
            return false;
        }
        if (!video_track_) {
            std::cerr << "[PushStreamer] EnsurePeerConnectionForPeer: no video track" << std::endl;
            return false;
        }
        std::vector<std::string> stream_ids = {config_.common.stream_id};
        auto add = pc->AddTrack(video_track_, stream_ids);
        if (!add.ok()) {
            std::cerr << "[PushStreamer] AddTrack for peer failed: " << add.error().message() << std::endl;
            return false;
        }
        ApplyVideoCodecPreferences(pc);
        ApplyEncodingParameters(pc);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (peer_connections_.find(peer_id) != peer_connections_.end()) {
                return true;
            }
            peer_connections_[peer_id] = pc;
            extra_peer_observers_[peer_id] = std::move(observer);
        }
        std::cout << "[PushStreamer] Subscriber PeerConnection created: " << peer_id << std::endl;
        MaybeStartOutboundStatsLoop();
        return true;
    }

    void CreateOfferOnConnection(const std::string& peer_id,
                                 webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        if (!pc) {
            return;
        }
        auto opts = MakeOfferOptions();
        TraceSigTiming("CreateOfferOnConnection begin peer=" + (peer_id.empty() ? std::string("default") : peer_id));
        auto peer_id_ptr = std::make_shared<std::string>(peer_id);

        auto obs = webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>(
            new webrtc::RefCountedObject<CreateSdpObserver>(
                [this, peer_id_ptr, pc](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                    if (!desc) {
                        return;
                    }
                    std::string type = desc->type();
                    std::string sdp;
                    if (!desc->ToString(&sdp)) {
                        std::cerr << "[PushStreamer] SDP ToString failed" << std::endl;
                        return;
                    }
                    TraceSigTiming("CreateOffer success peer=" +
                                   (peer_id_ptr->empty() ? std::string("default") : *peer_id_ptr) +
                                   " sdp_len=" + std::to_string(sdp.size()));

                    auto set_local = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
                        new webrtc::RefCountedObject<SetLocalDescObserver>(
                            [this, peer_id_ptr, type, sdp](webrtc::RTCError err) {
                                if (!err.ok()) {
                                    std::cerr << "[PushStreamer] SetLocalDescription failed: " << err.message()
                                              << std::endl;
                                    return;
                                }
                                TraceSigTiming("SetLocalDescription OK peer=" +
                                               (peer_id_ptr->empty() ? std::string("default") : *peer_id_ptr));
                                if (config_.common.test_encode_mode && peer_id_ptr->empty()) {
                                    if (std::getenv("WEBRTC_DUMP_OFFER")) {
                                        std::cout << "\n--- Local offer SDP ---\n" << sdp << "\n--- End ---\n" << std::flush;
                                    }
                                    DoLoopbackExchange(type, sdp);
                                } else {
                                    if (std::getenv("WEBRTC_DUMP_OFFER") && on_sdp_) {
                                        std::cout << "\n--- Local offer SDP (peer=" << *peer_id_ptr << ") ---\n" << sdp
                                                  << "\n--- End ---\n" << std::flush;
                                    }
                                    if (on_sdp_) {
                                        on_sdp_(*peer_id_ptr, type, sdp);
                                    }
                                }
                            }));

                    pc->SetLocalDescription(std::move(desc), set_local);
                },
                [](webrtc::RTCError err) {
                    std::cerr << "[PushStreamer] CreateOffer failed: " << err.message() << std::endl;
                }));

        pc->CreateOffer(obs.get(), opts);
    }

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        SetRemoteDescriptionForPeer("", type, sdp);
    }

    void SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type, const std::string& sdp) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) {
                pc = it->second;
            }
        }
        if (!pc) {
            std::cerr << "[PushStreamer] SetRemoteDescription: no peer" << std::endl;
            return;
        }
        webrtc::Thread* sig = pc->signaling_thread();
        if (!sig) {
            std::cerr << "[PushStreamer] SetRemoteDescription: no signaling thread" << std::endl;
            return;
        }
        auto work = [pc, type, sdp]() {
            TraceSigTiming("SetRemoteDescription begin type=" + type + " sdp_len=" + std::to_string(sdp.size()));
            auto opt_type = webrtc::SdpTypeFromString(type);
            if (!opt_type.has_value()) {
                std::cerr << "[PushStreamer] Bad SDP type: " << type << std::endl;
                return;
            }
            auto desc = webrtc::CreateSessionDescription(*opt_type, sdp);
            if (!desc) {
                std::cerr << "[PushStreamer] CreateSessionDescription(parse) failed" << std::endl;
                return;
            }
            auto obs = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
                new webrtc::RefCountedObject<SetRemoteDescObserver>([](webrtc::RTCError err) {
                    if (!err.ok()) {
                        std::cerr << "[PushStreamer] SetRemoteDescription failed: " << err.message() << std::endl;
                    } else {
                        TraceSigTiming("SetRemoteDescription OK");
                        std::cout << "[PushStreamer] SetRemoteDescription OK" << std::endl;
                    }
                }));
            pc->SetRemoteDescription(std::move(desc), obs);
        };
        if (sig->IsCurrent()) {
            work();
        } else {
            sig->BlockingCall(work);
        }
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        AddRemoteIceCandidateForPeer("", mid, mline_index, candidate);
    }

    void AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid, int mline_index,
                                      const std::string& candidate) {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) {
                pc = it->second;
            }
        }
        if (!pc) {
            return;
        }
        webrtc::Thread* sig = pc->signaling_thread();
        if (!sig) {
            return;
        }
        auto work = [pc, mid, mline_index, candidate]() {
            TraceSigTiming("AddRemoteIce begin mid=" + mid + " cand_len=" + std::to_string(candidate.size()));
            webrtc::SdpParseError err;
            webrtc::IceCandidateInterface* cand = webrtc::CreateIceCandidate(mid, mline_index, candidate, &err);
            if (!cand) {
                std::cerr << "[PushStreamer] CreateIceCandidate failed: " << err.description << std::endl;
                return;
            }
            std::unique_ptr<webrtc::IceCandidateInterface> owned(cand);
            if (!pc->AddIceCandidate(owned.get())) {
                std::cerr << "[PushStreamer] AddIceCandidate failed" << std::endl;
            }
        };
        if (sig->IsCurrent()) {
            work();
        } else {
            sig->BlockingCall(work);
        }
    }

    void DoLoopbackExchange(const std::string& offer_type, const std::string& offer_sdp) {
        if (std::getenv("WEBRTC_SKIP_LOOPBACK_RECV")) {
            std::cout << "[PushStreamer] Loopback skipped (WEBRTC_SKIP_LOOPBACK_RECV=1)\n";
            return;
        }
        std::cout << "[PushStreamer] Loopback: creating receiver PC..." << std::endl;
        auto rtc_config = MakeRtcConfiguration(config_);
        loopback_observer_ = std::make_unique<LoopbackPcObserver>(
            [this](const std::string& mid, int idx, const std::string& cand) {
                webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
                if (!pc) {
                    return;
                }
                webrtc::Thread* sig = pc->signaling_thread();
                if (!sig) {
                    return;
                }
                auto work = [pc, mid, idx, cand]() {
                    webrtc::SdpParseError err;
                    auto* ic = webrtc::CreateIceCandidate(mid, idx, cand, &err);
                    if (!ic) {
                        return;
                    }
                    std::unique_ptr<webrtc::IceCandidateInterface> o(ic);
                    pc->AddIceCandidate(o.get());
                };
                if (sig->IsCurrent()) {
                    work();
                } else {
                    sig->BlockingCall(work);
                }
            });
        receiver_ = CreatePcWithObserver(loopback_observer_.get(), rtc_config);
        if (!receiver_) {
            return;
        }

        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
        auto tr = receiver_->AddTransceiver(webrtc::MediaType::VIDEO, init);
        if (!tr.ok()) {
            std::cerr << "[PushStreamer] AddTransceiver failed" << std::endl;
            return;
        }

        auto opt_offer = webrtc::SdpTypeFromString(offer_type);
        if (!opt_offer.has_value()) {
            return;
        }
        auto remote_offer = webrtc::CreateSessionDescription(*opt_offer, offer_sdp);
        if (!remote_offer) {
            return;
        }

        auto obs_remote = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
            new webrtc::RefCountedObject<SetRemoteDescObserver>(
                [this](webrtc::RTCError e) {
                    if (!e.ok()) {
                        std::cerr << "[PushStreamer] receiver SetRemote failed: " << e.message() << std::endl;
                        return;
                    }
                    OnReceiverRemoteDescriptionSet();
                }));
        receiver_->SetRemoteDescription(std::move(remote_offer), obs_remote);
    }

    void OnReceiverRemoteDescriptionSet() {
        auto opts = MakeOfferOptions();
        auto obs = webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>(
            new webrtc::RefCountedObject<CreateSdpObserver>(
                [this](std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
                    if (!desc) {
                        return;
                    }
                    loopback_answer_sdp_.clear();
                    loopback_answer_type_ = desc->type();
                    if (!desc->ToString(&loopback_answer_sdp_)) {
                        return;
                    }
                    auto set_local = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
                        new webrtc::RefCountedObject<SetLocalDescObserver>(
                            [this](webrtc::RTCError err) {
                                if (!err.ok()) {
                                    std::cerr << "[PushStreamer] receiver SetLocal failed: " << err.message()
                                              << std::endl;
                                    return;
                                }
                                OnReceiverLocalDescriptionSet();
                            }));
                    receiver_->SetLocalDescription(std::move(desc), set_local);
                },
                [](webrtc::RTCError err) {
                    std::cerr << "[PushStreamer] CreateAnswer failed: " << err.message() << std::endl;
                }));
        receiver_->CreateAnswer(obs.get(), opts);
    }

    void OnReceiverLocalDescriptionSet() {
        auto opt_t = webrtc::SdpTypeFromString(loopback_answer_type_);
        if (!opt_t.has_value()) {
            return;
        }
        auto answer = webrtc::CreateSessionDescription(*opt_t, loopback_answer_sdp_);
        if (!answer) {
            return;
        }
        auto obs = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
            new webrtc::RefCountedObject<SetRemoteDescObserver>([](webrtc::RTCError e) {
                if (e.ok()) {
                    std::cout << "[PushStreamer] Loopback SDP exchange done" << std::endl;
                }
            }));
        peer_connection_->SetRemoteDescription(std::move(answer), obs);
    }

    void NotifyConnectionState(webrtc::PeerConnectionInterface::PeerConnectionState state) {
        if (!on_connection_state_) {
            return;
        }
        ConnectionState cs = ConnectionState::New;
        switch (state) {
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
                cs = ConnectionState::Connecting;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
                cs = ConnectionState::Connected;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
                cs = ConnectionState::Disconnected;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
                cs = ConnectionState::Failed;
                break;
            case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
                cs = ConnectionState::Closed;
                break;
            default:
                break;
        }
        on_connection_state_(cs);
    }

    void OnPeerIceCandidate(const std::string& peer_id, const webrtc::IceCandidateInterface* candidate) {
        if (!candidate || !on_ice_candidate_) {
            return;
        }
        std::string sdp;
        if (!candidate->ToString(&sdp)) {
            return;
        }
        on_ice_candidate_(peer_id, candidate->sdp_mid(), candidate->sdp_mline_index(), sdp);
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override {
        const char* names[] = {"New", "Gathering", "Complete"};
        int idx = static_cast<int>(state);
        if (idx >= 0 && idx < 3) {
            std::cout << "[PushStreamer] ICE gathering: " << names[idx] << std::endl;
        }
    }

    void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        if (!candidate) {
            return;
        }
        std::string sdp;
        if (!candidate->ToString(&sdp)) {
            return;
        }
        if (config_.common.test_encode_mode && receiver_) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> recv = receiver_;
            webrtc::Thread* sig = recv->signaling_thread();
            if (!sig) {
                return;
            }
            const std::string mid = candidate->sdp_mid();
            const int mline_index = candidate->sdp_mline_index();
            auto work = [recv, mid, mline_index, sdp]() {
                webrtc::SdpParseError err;
                auto* ic = webrtc::CreateIceCandidate(mid, mline_index, sdp, &err);
                if (!ic) {
                    return;
                }
                std::unique_ptr<webrtc::IceCandidateInterface> o(ic);
                recv->AddIceCandidate(o.get());
            };
            if (sig->IsCurrent()) {
                work();
            } else {
                sig->BlockingCall(work);
            }
        } else if (on_ice_candidate_) {
            on_ice_candidate_("", candidate->sdp_mid(), candidate->sdp_mline_index(), sdp);
        }
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
        NotifyConnectionState(new_state);
    }

    void SetOnSdp(OnSdpCallback cb) { on_sdp_ = std::move(cb); }
    void SetOnIceCandidate(OnIceCandidateCallback cb) { on_ice_candidate_ = std::move(cb); }
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }
    void SetOnFrame(OnFrameCallback cb) {
        on_frame_ = std::move(cb);
        if (camera_source_ && frame_counter_) {
            camera_source_->RemoveSink(frame_counter_.get());
        }
        frame_counter_ = std::make_unique<FrameCountingSink>(on_frame_);
        if (camera_source_) {
            camera_source_->AddOrUpdateSink(frame_counter_.get(), webrtc::VideoSinkWants());
        }
    }

    unsigned int EffectiveCaptureFrameCount() const {
        unsigned int n = frame_counter_ ? frame_counter_->GetFrameCount() : 0;
        if (camera_impl_) {
            n = std::max(n, static_cast<unsigned int>(camera_impl_->CapturedFrameCount()));
        }
        return n;
    }

    unsigned int GetFrameCount() const {
        return EffectiveCaptureFrameCount();
    }
    unsigned int GetDecodedFrameCount() const {
        return loopback_observer_ ? loopback_observer_->GetDecodedCount() : 0;
    }
    bool TestCaptureOnly() const { return config_.common.test_capture_only; }
    bool SignalingSubscriberOfferOnly() const { return config_.common.signaling_subscriber_offer_only; }

private:
    PushStreamerConfig config_;
    std::unique_ptr<webrtc::Thread> owned_signaling_thread_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> camera_source_;
    /// 与 camera_source_ 同生命周期的具体采集实现（避免 dynamic_cast/RTTI 依赖）。
    CameraVideoTrackSource* camera_impl_{nullptr};

    OnSdpCallback on_sdp_;
    OnIceCandidateCallback on_ice_candidate_;
    OnConnectionStateCallback on_connection_state_;
    OnFrameCallback on_frame_;
    std::unique_ptr<FrameCountingSink> frame_counter_;

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> receiver_;
    std::unique_ptr<LoopbackPcObserver> loopback_observer_;
    std::string loopback_answer_sdp_;
    std::string loopback_answer_type_;

    std::unordered_map<std::string, webrtc::scoped_refptr<webrtc::PeerConnectionInterface>> peer_connections_;
    std::unordered_map<std::string, std::unique_ptr<ExtraPeerObserver>> extra_peer_observers_;
    std::mutex mutex_;
    std::thread outbound_stats_thread_;
    std::atomic<bool> outbound_stats_stop_{false};
    std::atomic<bool> outbound_stats_started_{false};
};

PushStreamer::PushStreamer(const PushStreamerConfig& config) : impl_(std::make_unique<Impl>(config)) {}

PushStreamer::~PushStreamer() {
    Stop();
}

bool PushStreamer::Start() {
    if (is_streaming_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!impl_->Initialize()) {
        return false;
    }
    if (!impl_->TestCaptureOnly() && !impl_->SignalingSubscriberOfferOnly()) {
        impl_->CreateOffer();
    }
    is_streaming_.store(true, std::memory_order_release);
    return true;
}

void PushStreamer::Stop() {
    // exchange：SIGTERM 等信号在 Shutdown 阻塞时重入 Stop 时不得第二次 Shutdown。
    if (!is_streaming_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    impl_->Shutdown();
}

bool PushStreamer::SetRemoteDescription(const std::string& type, const std::string& sdp) {
    impl_->SetRemoteDescription(type, sdp);
    return true;
}

bool PushStreamer::SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type,
                                               const std::string& sdp) {
    impl_->SetRemoteDescriptionForPeer(peer_id, type, sdp);
    return true;
}

void PushStreamer::AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
    impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
}

void PushStreamer::AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid, int mline_index,
                                                const std::string& candidate) {
    impl_->AddRemoteIceCandidateForPeer(peer_id, mid, mline_index, candidate);
}

void PushStreamer::CreateOfferForPeer(const std::string& peer_id) {
    impl_->CreateOfferForPeer(peer_id);
}

void PushStreamer::SetOnSdpCallback(OnSdpCallback cb) {
    impl_->SetOnSdp(std::move(cb));
}

void PushStreamer::SetOnIceCandidateCallback(OnIceCandidateCallback cb) {
    impl_->SetOnIceCandidate(std::move(cb));
}

void PushStreamer::SetOnConnectionStateCallback(OnConnectionStateCallback cb) {
    impl_->SetOnConnectionState(std::move(cb));
}

void PushStreamer::SetOnFrameCallback(OnFrameCallback cb) {
    impl_->SetOnFrame(std::move(cb));
}

unsigned int PushStreamer::GetFrameCount() const {
    return impl_->GetFrameCount();
}

unsigned int PushStreamer::GetDecodedFrameCount() const {
    return impl_->GetDecodedFrameCount();
}

}  // namespace webrtc_demo
