#include "pull_subscriber.h"
#include "webrtc/signaling/client.h"
#include "webrtc_peer_connection_factory.h"

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/make_ref_counted.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_frame_buffer.h"
#include "libyuv/convert.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <functional>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
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
    std::cout << "[SIG_TIMING][pull] t_us=" << SignalingNowUs() << " " << msg << std::endl;
}

bool MediaTimingTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("WEBRTC_DEMO_MEDIA_TIMING_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}

unsigned MediaTimingTraceEveryN() {
    static const unsigned every_n = []() {
        if (const char* v = std::getenv("WEBRTC_DEMO_MEDIA_TIMING_TRACE_EVERY_N")) {
            const int n = std::atoi(v);
            if (n >= 1 && n <= 600) {
                return static_cast<unsigned>(n);
            }
        }
        return 30u;
    }();
    return every_n;
}

// 与 api/video/video_timing.cc 中 TimingFrameInfo::ToString() 输出顺序一致。
static std::vector<std::string> SplitCommaFields(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

static bool ParseInt64Strict(const std::string& s, int64_t* out) {
    if (!out || s.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) {
        return false;
    }
    *out = static_cast<int64_t>(v);
    return true;
}

/// 由 TimingFrameInfo::ToString 的 15 段推导可解释的时延（ms）。
static void PrintTimingFrameDerivedDeltas(const std::vector<std::string>& parts) {
    constexpr size_t kN = 15;
    if (parts.size() != kN) {
        return;
    }
    int64_t cap = 0;
    int64_t enc_s = 0;
    int64_t enc_f = 0;
    int64_t recv_s = 0;
    int64_t recv_f = 0;
    int64_t dec_s = 0;
    int64_t dec_f = 0;
    if (!ParseInt64Strict(parts[1], &cap) || !ParseInt64Strict(parts[2], &enc_s) ||
        !ParseInt64Strict(parts[3], &enc_f) || !ParseInt64Strict(parts[8], &recv_s) ||
        !ParseInt64Strict(parts[9], &recv_f) || !ParseInt64Strict(parts[10], &dec_s) ||
        !ParseInt64Strict(parts[11], &dec_f)) {
        std::cout << "[TimingDelta] 数值解析失败，跳过推导" << std::endl;
        return;
    }

    std::cout << "[TimingDelta] 接收端同一本地时钟（可与 decode_*、receive_* 直接相减）:" << std::endl;
    std::cout << "  first_RTP→decode_start = " << (dec_s - recv_s)
              << " ms（首包进机 → 解码开始；含网传抖动、抖动缓冲、拼帧与调度）" << std::endl;
    std::cout << "  last_RTP→decode_start = " << (dec_s - recv_f)
              << " ms（收齐该帧 → 解码开始；主要反映 JB/解码器前排队）" << std::endl;
    std::cout << "  decode_start→decode_finish = " << (dec_f - dec_s) << " ms（本帧解码耗时）" << std::endl;
    std::cout << "  RTP_last−first = " << (recv_f - recv_s) << " ms（该帧多包到达时间跨度）" << std::endl;

    if (cap >= 0) {
        std::cout << "[TimingDelta] 端到端（WebRTC 已把发端时间对齐到可与收端比较时；capture_time_ms>=0）:" << std::endl;
        std::cout << "  capture→decode_start = " << (dec_s - cap)
                  << " ms（发端采集 → 收端解码开始；最接近「编码前→解码前」里的采集到解码前）" << std::endl;
    } else {
        std::cout << "[TimingDelta] capture_time_ms<0：发收绝对时间尚未对齐，不能用 decode_start−capture 当整段端到端。"
                     " 可临时用 first_RTP→decode_start 看「到机后」延迟，或在业务层打统一时钟时间戳。"
                  << std::endl;
    }

    std::cout << "[TimingDelta] 发端侧相对量（各字段同一偏移下互减仍有意义，与收端大正数不是同一绝对时钟）:" << std::endl;
    std::cout << "  encode_start−capture = " << (enc_s - cap) << " ms" << std::endl;
    std::cout << "  encode_finish−encode_start = " << (enc_f - enc_s) << " ms（约等于本帧编码时长）" << std::endl;
}

static void PrintGoogTimingFrameInfoLabeled(const std::string& raw) {
    static constexpr const char* kFieldZh[] = {
        "rtp_timestamp — RTP 时间戳（90kHz 时钟单位，不是毫秒）",
        "capture_time_ms — 发送端：采集时刻(ms)；收端 NTP 未对齐时常为负，相对关系仍可比",
        "encode_start_ms — 发送端：编码开始(ms)",
        "encode_finish_ms — 发送端：编码结束(ms)",
        "packetization_finish_ms — 发送端：组包完成 / 进入 pacer 前(ms)",
        "pacer_exit_ms — 发送端：该帧最后一包离开 pacer(ms)；未用时常为 -1",
        "network_timestamp_ms — 发送端/扩展：网内时间戳 1(ms)",
        "network2_timestamp_ms — 发送端/扩展：网内时间戳 2(ms)",
        "receive_start_ms — 接收端本地时钟：该帧第一个 RTP 包到达(ms)",
        "receive_finish_ms — 接收端本地时钟：该帧最后一个包收齐(ms)",
        "decode_start_ms — 接收端本地时钟：解码开始(ms)",
        "decode_finish_ms — 接收端本地时钟：解码结束(ms)",
        "render_time_ms — 接收端：建议渲染时刻(ms)",
        "is_outlier — 是否按帧大小判为异常上报(0/1)",
        "is_timer_triggered — 是否由周期定时器选中为 timing 帧(0/1)",
    };
    constexpr size_t kN = sizeof(kFieldZh) / sizeof(kFieldZh[0]);
    const std::vector<std::string> parts = SplitCommaFields(raw);
    if (parts.size() != kN) {
        std::cout << "[VideoTiming] TimingFrameInfo (raw): " << raw << std::endl;
        std::cout << "[VideoTiming] 字段数=" << parts.size() << "（期望 " << kN
                  << "），与当前 libwebrtc 的 ToString 格式不一致，未逐字段标注" << std::endl;
        return;
    }
    std::cout << "[VideoTiming] TimingFrameInfo 逐字段 (goog_timing_frame_info):" << std::endl;
    for (size_t i = 0; i < kN; ++i) {
        std::cout << "  [" << (i + 1) << "] " << kFieldZh[i] << " = " << parts[i] << std::endl;
    }
    PrintTimingFrameDerivedDeltas(parts);
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

class InboundVideoStatsLogger : public webrtc::RTCStatsCollectorCallback {
public:
    void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        if (!report) {
            return;
        }
        const std::vector<const webrtc::RTCInboundRtpStreamStats*> inbound =
            report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>();
        for (const webrtc::RTCInboundRtpStreamStats* s : inbound) {
            if (!s->kind.has_value() || *s->kind != "video") {
                continue;
            }
            std::cout << "[InboundVideoStats] id=" << s->id();
            if (s->ssrc.has_value()) {
                std::cout << " ssrc=" << *s->ssrc;
            }
            if (s->frames_decoded.has_value()) {
                std::cout << " frames_decoded=" << *s->frames_decoded;
            }
            if (s->frames_received.has_value()) {
                std::cout << " frames_received=" << *s->frames_received;
            }
            if (s->total_decode_time.has_value()) {
                std::cout << " total_decode_time_s=" << *s->total_decode_time;
            }
            if (s->total_processing_delay.has_value()) {
                std::cout << " total_processing_delay_s=" << *s->total_processing_delay;
            }
            if (s->jitter_buffer_delay.has_value()) {
                std::cout << " jitter_buffer_delay_s=" << *s->jitter_buffer_delay;
            }
            std::cout << std::endl;
            if (s->goog_timing_frame_info.has_value() && !s->goog_timing_frame_info->empty()) {
                PrintGoogTimingFrameInfoLabeled(*s->goog_timing_frame_info);
            } else {
                std::cout << "[VideoTiming] goog_timing_frame_info: (empty — 对端未带 video-timing 扩展、"
                             "或尚未选中用于上报的 timing frame；仍可见上行 total_processing_delay 等)"
                          << std::endl;
            }
        }
    }
};

}  // namespace

class VideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using Callback = PullSubscriber::OnVideoFrameCallback;
    using StatsCallback = std::function<void(uint16_t trace_id, int64_t t_callback_done_us)>;
    explicit VideoSink(Callback cb, bool skip_argb_conversion, StatsCallback stats_cb = nullptr)
        : on_frame_(std::move(cb)),
          skip_argb_conversion_(skip_argb_conversion),
          on_frame_stats_(std::move(stats_cb)) {}

    void OnFrame(const webrtc::VideoFrame& frame) override {
        const bool e2e_trace = (std::getenv("WEBRTC_E2E_LATENCY_TRACE") != nullptr &&
                                std::getenv("WEBRTC_E2E_LATENCY_TRACE")[0] == '1');
        const int64_t t_sink_enter_us = e2e_trace ? webrtc::TimeMicros() : 0;
        const int64_t t_now_us = webrtc::TimeMicros();
        const int64_t wall_sink_utc_ms = e2e_trace ? webrtc::TimeUTCMillis() : int64_t{0};
        if (!on_frame_) {
            return;
        }
        auto buf = frame.video_frame_buffer();
        if (!buf) {
            return;
        }
        int w = frame.width();
        int h = frame.height();
        if (w <= 0 || h <= 0) {
            return;
        }
        if (skip_argb_conversion_) {
            const int64_t t_fast_done_us = e2e_trace ? webrtc::TimeMicros() : 0;
            static unsigned frame_count = 0;
            unsigned n = ++frame_count;
            if (n == 1 || n <= 5 || n % 30 == 0) {
                std::cout << "[VideoSink] OnFrame #" << n << " (skip ARGB)" << std::endl;
            }
            const int64_t t_callback_done_us = e2e_trace ? webrtc::TimeMicros() : 0;
            on_frame_(nullptr, w, h, 0, frame.id(), t_callback_done_us);
            if (on_frame_stats_) {
                on_frame_stats_(frame.id(), t_callback_done_us);
            }
            if (MediaTimingTraceEnabled()) {
                static std::atomic<unsigned> media_sink_n{0};
                const unsigned n_trace = ++media_sink_n;
                if ((n_trace % MediaTimingTraceEveryN()) == 0u) {
                    std::cout << "[MEDIA_TIMING][sink] t_us=" << t_now_us << " trace_id="
                              << static_cast<unsigned>(frame.id()) << " rtp_ts=" << frame.rtp_timestamp()
                              << " event=onframe_skip_argb t_callback_done_us=" << t_callback_done_us
                              << std::endl;
                }
            }
            if (e2e_trace) {
                std::cout << "[E2E_RX] rtp_ts=" << frame.rtp_timestamp() << " trace_id="
                          << static_cast<unsigned>(frame.id()) << " frame_id="
                          << static_cast<unsigned>(frame.id()) << " t_sink_us=" << t_sink_enter_us
                          << " wall_utc_ms=" << wall_sink_utc_ms << " t_argb_done_us=" << t_fast_done_us
                          << " t_callback_done_us=" << t_callback_done_us << std::endl;
            }
            return;
        }
        int stride = w * 4;
        auto i420 = buf->ToI420();
        if (!i420) {
            return;
        }
        argb_.resize(static_cast<size_t>(stride * h));
        libyuv::I420ToARGB(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                           i420->StrideV(), argb_.data(), stride, w, h);
        const int64_t t_argb_done_us = e2e_trace ? webrtc::TimeMicros() : 0;
        static unsigned frame_count = 0;
        unsigned n = ++frame_count;
        if (n == 1 || n <= 5 || n % 30 == 0) {
            std::cout << "[VideoSink] OnFrame #" << n << std::endl;
        }
        const int64_t t_callback_done_us = e2e_trace ? webrtc::TimeMicros() : 0;
        on_frame_(argb_.data(), w, h, stride, frame.id(), t_callback_done_us);
        if (on_frame_stats_) {
            on_frame_stats_(frame.id(), t_callback_done_us);
        }
        if (MediaTimingTraceEnabled()) {
            static std::atomic<unsigned> media_sink_n{0};
            const unsigned n_trace = ++media_sink_n;
            if ((n_trace % MediaTimingTraceEveryN()) == 0u) {
                std::cout << "[MEDIA_TIMING][sink] t_us=" << t_now_us << " trace_id="
                          << static_cast<unsigned>(frame.id()) << " rtp_ts=" << frame.rtp_timestamp()
                          << " event=onframe_argb_done t_argb_done_us=" << t_argb_done_us
                          << " t_callback_done_us=" << t_callback_done_us << std::endl;
            }
        }
        if (e2e_trace) {
            std::cout << "[E2E_RX] rtp_ts=" << frame.rtp_timestamp() << " trace_id="
                      << static_cast<unsigned>(frame.id()) << " frame_id="
                      << static_cast<unsigned>(frame.id()) << " t_sink_us=" << t_sink_enter_us
                      << " wall_utc_ms=" << wall_sink_utc_ms << " t_argb_done_us=" << t_argb_done_us
                      << " t_callback_done_us=" << t_callback_done_us << std::endl;
        }
    }

private:
    Callback on_frame_;
    bool skip_argb_conversion_{false};
    StatsCallback on_frame_stats_;
    std::vector<uint8_t> argb_;
};

class PullSubscriber::Impl : public webrtc::PeerConnectionObserver {
public:
    Impl(const std::string& url, const std::string& stream_id, const PullSubscriberConfig& recv)
        : signaling_(std::make_unique<webrtc_demo::SignalingClient>(url, "subscriber", stream_id)),
          recv_config_(recv) {
        stats_.t_construct_us = SignalingNowUs();
    }

    void OnSinkFrame(uint16_t trace_id, int64_t t_callback_done_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t now_us = t_callback_done_us > 0 ? t_callback_done_us : SignalingNowUs();
        ++stats_.frames_sink_total;
        stats_.last_frame_us = now_us;
        stats_.last_trace_id = trace_id;
        if (stats_.first_frame_us <= 0) {
            stats_.first_frame_us = now_us;
            std::ostringstream oss;
            oss << "frames=" << stats_.frames_sink_total << " trace_id=" << static_cast<unsigned>(trace_id);
            if (stats_.t_connected_us > 0 && stats_.first_frame_us >= stats_.t_connected_us) {
                oss << " connected_to_first_frame_ms="
                    << (stats_.first_frame_us - stats_.t_connected_us) / 1000.0;
            }
            TracePathStat("RX_FIRST_FRAME", oss.str());
        } else if (stats_.frames_sink_total % 120 == 0) {
            std::ostringstream oss;
            oss << "frames=" << stats_.frames_sink_total << " trace_id=" << static_cast<unsigned>(trace_id);
            TracePathStat("RX_FRAME_PROGRESS", oss.str());
        }
    }

    bool Initialize() {
        std::cout << "[PullSubscriber] Initializing WebRTC (native API)..." << std::endl;
        stats_.t_initialize_begin_us = SignalingNowUs();
        webrtc_demo::EnsureWebrtcFieldTrialsInitialized(); 
        if (!webrtc::InitializeSSL()) {
            return false;
        }
        webrtc::PeerConnectionFactoryDependencies deps;
        webrtc_demo::PeerConnectionFactoryMediaOptions media_opts;
        media_opts.decoder_backend = recv_config_.backend.use_rockchip_mpp_h264_decode
                                         ? webrtc_demo::VideoCodecBackendPreference::kRockchipMpp
                                         : webrtc_demo::VideoCodecBackendPreference::kBuiltin;
        webrtc_demo::ConfigurePeerConnectionFactoryDependencies(deps, &media_opts);
        webrtc_demo::EnsureDedicatedPeerConnectionSignalingThread(deps, &owned_signaling_thread_);
        factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
        if (!factory_) {
            webrtc::CleanupSSL();
            return false;
        }
        stats_.t_initialize_done_us = SignalingNowUs();
        return CreatePeerConnection();
    }

    void Shutdown() {
        if (video_sink_ && video_track_) {
            video_track_->RemoveSink(video_sink_.get());
        }
        video_sink_.reset();
        video_track_ = nullptr;
        video_rtp_receiver_ = nullptr;
        if (peer_connection_) {
            peer_connection_->Close();
            peer_connection_ = nullptr;
        }
        factory_ = nullptr;
        if (owned_signaling_thread_) {
            owned_signaling_thread_->Stop();
            owned_signaling_thread_.reset();
        }
        TracePathSummary("shutdown");
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
            ReportPathError("RX_ERR_NO_SIGNALING_THREAD", "SetRemoteDescription: no signaling thread");
            return;
        }
        auto work = [this, pc, type, sdp]() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.t_set_remote_begin_us = SignalingNowUs();
            }
            TraceSigTiming("SetRemoteDescription begin type=" + type + " sdp_len=" + std::to_string(sdp.size()));
            auto opt_t = webrtc::SdpTypeFromString(type);
            if (!opt_t.has_value()) {
                ReportPathError("RX_ERR_BAD_SDP_TYPE", "bad SDP type");
                return;
            }
            auto desc = webrtc::CreateSessionDescription(*opt_t, sdp);
            if (!desc) {
                ReportPathError("RX_ERR_PARSE_REMOTE_SDP", "parse remote SDP failed");
                return;
            }
            auto obs = webrtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface>(
                new webrtc::RefCountedObject<SetRemoteDescObserver>([this](webrtc::RTCError err) {
                    if (!err.ok()) {
                        ReportPathError("RX_ERR_SET_REMOTE_SDP", std::string("SetRemoteDescription: ") + err.message());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        stats_.t_set_remote_ok_us = SignalingNowUs();
                    }
                    TraceSigTiming("SetRemoteDescription OK -> CreateAnswer");
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
                    TraceSigTiming("CreateAnswer success sdp_len=" + std::to_string(sdp.size()));
                    if (std::getenv("WEBRTC_DUMP_LOCAL_ANSWER")) {
                        std::cout << "\n--- Local answer SDP ---\n" << sdp << "\n--- End ---\n" << std::flush;
                    }
                    auto set_local = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
                        new webrtc::RefCountedObject<SetLocalDescObserver>(
                            [this, sdp](webrtc::RTCError err) {
                                if (!err.ok()) {
                                    ReportPathError("RX_ERR_SET_LOCAL_SDP",
                                                    std::string("SetLocalDescription: ") + err.message());
                                    return;
                                }
                                TraceSigTiming("SetLocalDescription OK (answer)");
                                signaling_->SendAnswer(sdp);
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    stats_.t_answer_sent_us = SignalingNowUs();
                                }
                                TraceSigTiming("SendAnswer invoked");
                                std::cout << "[PullSubscriber] Answer sent" << std::endl;
                            }));
                    peer_connection_->SetLocalDescription(std::move(desc), set_local);
                },
                [this](webrtc::RTCError err) {
                    ReportPathError("RX_ERR_CREATE_ANSWER", std::string("CreateAnswer: ") + err.message());
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
        auto work = [this, pc, mid, mline_index, candidate]() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.remote_ice_calls;
            }
            TraceSigTiming("AddRemoteIce begin mid=" + mid + " cand_len=" + std::to_string(candidate.size()));
            webrtc::SdpParseError err;
            webrtc::IceCandidateInterface* cand = webrtc::CreateIceCandidate(mid, mline_index, candidate, &err);
            if (!cand) {
                std::cerr << "[PullSubscriber] CreateIceCandidate: " << err.description << std::endl;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.remote_ice_parse_fail;
                }
                TracePathStat("RX_ERR_REMOTE_ICE_PARSE", "mid=" + mid + " idx=" + std::to_string(mline_index));
                return;
            }
            std::unique_ptr<webrtc::IceCandidateInterface> owned(cand);
            pc->AddIceCandidate(owned.get());
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.remote_ice_added;
            }
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
        const int64_t now_us = SignalingNowUs();
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.pc_state_changes;
            if (cs == PullConnectionState::Connected && stats_.t_connected_us <= 0) {
                stats_.t_connected_us = now_us;
                TracePathStat("RX_CONN_CONNECTED", "state=connected");
            }
            if ((cs == PullConnectionState::Disconnected || cs == PullConnectionState::Failed) &&
                stats_.t_connected_us > 0 && stats_.frames_sink_total == 0) {
                std::ostringstream oss;
                oss << "state=" << (cs == PullConnectionState::Failed ? "failed" : "disconnected")
                    << " connected_without_frame_ms=" << (now_us - stats_.t_connected_us) / 1000.0;
                TracePathStat("RX_ERR_NO_FRAME_WHILE_CONNECTED", oss.str());
            }
            if ((cs == PullConnectionState::Disconnected || cs == PullConnectionState::Failed) &&
                stats_.last_frame_us > 0) {
                std::ostringstream oss;
                oss << "state=" << (cs == PullConnectionState::Failed ? "failed" : "disconnected")
                    << " since_last_frame_ms=" << (now_us - stats_.last_frame_us) / 1000.0
                    << " frames=" << stats_.frames_sink_total;
                TracePathStat("RX_CONN_FRAME_GAP", oss.str());
            }
        }
        on_connection_state_(cs);
    }

    void RequestInboundVideoStatsLog() {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc = peer_connection_;
        webrtc::scoped_refptr<webrtc::RtpReceiverInterface> recv = video_rtp_receiver_;
        if (!pc) {
            return;
        }
        webrtc::Thread* sig = pc->signaling_thread();
        if (!sig) {
            return;
        }
        auto work = [pc, recv]() {
            auto cb = webrtc::make_ref_counted<InboundVideoStatsLogger>();
            if (recv) {
                pc->GetStats(recv, cb);
            } else {
                pc->GetStats(cb.get());
            }
        };
        if (sig->IsCurrent()) {
            work();
        } else {
            sig->BlockingCall(work);
        }
    }

    std::unique_ptr<webrtc_demo::SignalingClient> signaling_;
    PullSubscriberConfig recv_config_;
    std::unique_ptr<webrtc::Thread> owned_signaling_thread_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> video_rtp_receiver_;
    std::shared_ptr<VideoSink> video_sink_;
    std::mutex mutex_;
    OnConnectionStateCallback on_connection_state_;
    OnErrorCallback on_error_;

private:
    struct RxPathStats {
        int64_t t_construct_us{0};
        int64_t t_initialize_begin_us{0};
        int64_t t_initialize_done_us{0};
        int64_t t_set_remote_begin_us{0};
        int64_t t_set_remote_ok_us{0};
        int64_t t_answer_sent_us{0};
        int64_t t_connected_us{0};
        int64_t t_first_frame_us{0};
        int64_t last_frame_us{0};
        uint16_t last_trace_id{0};
        uint64_t attach_calls{0};
        uint64_t attach_dedup{0};
        uint64_t attach_effective{0};
        uint64_t remote_ice_calls{0};
        uint64_t remote_ice_parse_fail{0};
        uint64_t remote_ice_added{0};
        uint64_t pc_state_changes{0};
        uint64_t frames_sink_total{0};
        uint64_t error_reports{0};
    };
    RxPathStats stats_{};

    void TracePathStat(const char* code, const std::string& details) const {
        std::cout << "[PATH_STAT][rx] t_us=" << SignalingNowUs() << " code=" << code;
        if (!details.empty()) {
            std::cout << " " << details;
        }
        std::cout << std::endl;
    }

    void ReportPathError(const char* code, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.error_reports;
        }
        TracePathStat(code, "msg=\"" + msg + "\"");
        if (on_error_) {
            on_error_(msg);
        }
    }

    void TracePathSummary(const char* reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        oss << "reason=" << reason << " frames=" << stats_.frames_sink_total << " attach_calls=" << stats_.attach_calls
            << " attach_dedup=" << stats_.attach_dedup << " attach_effective=" << stats_.attach_effective
            << " remote_ice_calls=" << stats_.remote_ice_calls
            << " remote_ice_parse_fail=" << stats_.remote_ice_parse_fail
            << " remote_ice_added=" << stats_.remote_ice_added << " state_changes=" << stats_.pc_state_changes
            << " errors=" << stats_.error_reports;
        if (stats_.t_connected_us > 0) {
            oss << " connected_at_us=" << stats_.t_connected_us;
        }
        if (stats_.t_first_frame_us > 0) {
            oss << " first_frame_at_us=" << stats_.t_first_frame_us;
        }
        if (stats_.t_connected_us > 0 && stats_.t_first_frame_us > 0 &&
            stats_.t_first_frame_us >= stats_.t_connected_us) {
            oss << " connected_to_first_frame_ms=" << (stats_.t_first_frame_us - stats_.t_connected_us) / 1000.0;
        }
        if (stats_.last_frame_us > 0) {
            oss << " last_frame_us=" << stats_.last_frame_us << " last_trace_id=" << stats_.last_trace_id;
        }
        TracePathStat("RX_PATH_SUMMARY", oss.str());
    }

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
        if (recv_config_.common.jitter_buffer_min_delay_ms >= 0) {
            r->SetJitterBufferMinimumDelay(
                std::optional<double>(static_cast<double>(recv_config_.common.jitter_buffer_min_delay_ms) / 1000.0));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.attach_calls;
        const bool same_track = (video_track_.get() == vt);
        const bool same_receiver = (video_rtp_receiver_.get() == r.get());
        if (same_track && same_receiver) {
            ++stats_.attach_dedup;
            return;
        }
        if (video_track_ && !same_track) {
            video_track_->RemoveSink(video_sink_.get());
        }
        video_track_ = vt;
        video_rtp_receiver_ = std::move(r);
        if (!same_track) {
            ++stats_.attach_effective;
            video_track_->AddOrUpdateSink(video_sink_.get(), webrtc::VideoSinkWants());
            std::cout << "[PullSubscriber] Video track attached" << std::endl;
            TracePathStat("RX_ATTACH_VIDEO", "attach_effective=" + std::to_string(stats_.attach_effective));
        }
    }
};

PullSubscriber::PullSubscriber(const std::string& signaling_url,
                               const std::string& stream_id,
                               const PullSubscriberConfig& recv)
    : impl_(std::make_unique<Impl>(signaling_url, stream_id, recv)) {}

PullSubscriber::~PullSubscriber() {
    Stop();
}

void PullSubscriber::Play() {
    if (is_playing_) {
        return;
    }

    impl_->on_connection_state_ = on_connection_state_;
    impl_->on_error_ = on_error_;
    impl_->video_sink_ = std::make_shared<VideoSink>(
        on_video_frame_, impl_->recv_config_.common.skip_sink_argb_conversion,
        [impl = impl_.get()](uint16_t trace_id, int64_t t_callback_done_us) {
            if (impl) {
                impl->OnSinkFrame(trace_id, t_callback_done_us);
            }
        });
    if (impl_->recv_config_.common.skip_sink_argb_conversion) {
        std::cout << "[PullSubscriber] VideoSink: skip I420→ARGB (client low-latency path)" << std::endl;
    }

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

void PullSubscriber::RequestInboundVideoStatsLog() {
    if (impl_) {
        impl_->RequestInboundVideoStatsLog();
    }
}

}  // namespace webrtc_demo
