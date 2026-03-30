#include "push_streamer.h"

#include "api/array_view.h"
#include "camera_utils.h"
#include "media/camera_video_track_source.h"
#include "platform/alsa_null_fallback.h"
#include "webrtc_field_trials.h"
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
#include "api/stats/rtc_stats_report.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unistd.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webrtc_demo {

namespace {

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

static std::optional<double> AttrToDoubleMs(const webrtc::Attribute& a) {
    if (!a.has_value()) {
        return std::nullopt;
    }
    if (a.holds_alternative<double>()) {
        const auto& o = a.as_optional<double>();
        if (o.has_value()) {
            return *o * 1000.0;
        }
    }
    if (a.holds_alternative<uint64_t>()) {
        const auto& o = a.as_optional<uint64_t>();
        if (o.has_value()) {
            return static_cast<double>(*o);
        }
    }
    if (a.holds_alternative<int64_t>()) {
        const auto& o = a.as_optional<int64_t>();
        if (o.has_value()) {
            return static_cast<double>(*o);
        }
    }
    return std::nullopt;
}

static std::optional<double> MediaSourceTotalCaptureTimeMs(const webrtc::Attribute& a) {
    if (!a.has_value()) {
        return std::nullopt;
    }
    if (a.holds_alternative<double>()) {
        const auto& o = a.as_optional<double>();
        if (o.has_value()) {
            return *o * 1000.0;
        }
    }
    if (a.holds_alternative<uint64_t>()) {
        const auto& o = a.as_optional<uint64_t>();
        if (o.has_value()) {
            return static_cast<double>(*o);
        }
    }
    return AttrToDoubleMs(a);
}

static std::optional<uint64_t> AttrToUint64(const webrtc::Attribute& a) {
    if (!a.has_value()) {
        return std::nullopt;
    }
    if (a.holds_alternative<uint64_t>()) {
        const auto& o = a.as_optional<uint64_t>();
        if (o.has_value()) {
            return *o;
        }
    }
    if (a.holds_alternative<uint32_t>()) {
        const auto& o = a.as_optional<uint32_t>();
        if (o.has_value()) {
            return static_cast<uint64_t>(*o);
        }
    }
    return std::nullopt;
}

static void MergeMaxOptional(std::optional<double>* slot, double candidate_ms) {
    if (!slot->has_value() || candidate_ms > **slot) {
        *slot = candidate_ms;
    }
}

static void AddOptionalDouble(std::optional<double>* slot, double add_ms) {
    if (slot->has_value()) {
        **slot += add_ms;
    } else {
        *slot = add_ms;
    }
}

static void AddOptionalU64(std::optional<uint64_t>* slot, uint64_t add) {
    if (slot->has_value()) {
        **slot += add;
    } else {
        *slot = add;
    }
}

struct LatencyOneLineSummary {
    std::optional<double> capture_total_ms;
    std::optional<double> encode_total_ms;
    std::optional<double> packet_send_delay_total_ms;
    std::optional<double> ice_current_rtt_ms;
    std::optional<double> remote_rtt_ms;
    std::optional<double> remote_jitter_ms;
    std::optional<double> avg_rtcp_interval_ms;
    std::optional<double> remote_total_rtt_sum_ms;
    std::optional<uint64_t> remote_rtt_measurements;
};

static void AccumulateLatencySummary(const std::string& rtype,
                                     const std::string& name,
                                     const webrtc::Attribute& m,
                                     LatencyOneLineSummary* s) {
    if (rtype == "remote-inbound-rtp" && name == "roundTripTimeMeasurements") {
        if (auto u = AttrToUint64(m)) {
            AddOptionalU64(&s->remote_rtt_measurements, *u);
        }
        return;
    }
    if (rtype == "media-source" && name == "totalCaptureTime") {
        if (auto cap_ms = MediaSourceTotalCaptureTimeMs(m)) {
            MergeMaxOptional(&s->capture_total_ms, *cap_ms);
        }
        return;
    }
    auto sec = AttrToDoubleMs(m);
    if (!sec) {
        return;
    }
    const double ms = *sec;
    if (rtype == "outbound-rtp" && name == "totalEncodeTime") {
        MergeMaxOptional(&s->encode_total_ms, ms);
    } else if (rtype == "outbound-rtp" && name == "totalPacketSendDelay") {
        MergeMaxOptional(&s->packet_send_delay_total_ms, ms);
    } else if (rtype == "candidate-pair" && name == "currentRoundTripTime") {
        if (ms > 0.0) {
            MergeMaxOptional(&s->ice_current_rtt_ms, ms);
        }
    } else if (rtype == "remote-inbound-rtp" && name == "roundTripTime") {
        MergeMaxOptional(&s->remote_rtt_ms, ms);
    } else if (rtype == "remote-inbound-rtp" && name == "jitter") {
        MergeMaxOptional(&s->remote_jitter_ms, ms);
    } else if (rtype == "remote-inbound-rtp" && name == "totalRoundTripTime") {
        AddOptionalDouble(&s->remote_total_rtt_sum_ms, ms);
    } else if (name == "averageRtcpInterval") {
        MergeMaxOptional(&s->avg_rtcp_interval_ms, ms);
    }
}

static std::string FormatMsOptional(const std::optional<double>& v, int prec) {
    if (!v.has_value()) {
        return "-";
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << *v;
    return o.str();
}

enum class LatencyPollStatus { Ok, Empty, Error, Timeout };

static bool GetStatsFullDumpEnabled() {
    const char* v = std::getenv("WEBRTC_GETSTATS_DUMP");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

static bool GetStatsDumpJsonOnly() {
    const char* v = std::getenv("WEBRTC_GETSTATS_DUMP_JSON_ONLY");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

static bool GetStatsDumpMembersExtra() {
    const char* v = std::getenv("WEBRTC_GETSTATS_DUMP_MEMBERS");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

static int GetStatsDumpIntervalMs() {
    const char* v = std::getenv("WEBRTC_GETSTATS_DUMP_INTERVAL_MS");
    if (!v || v[0] == '\0') {
        return 0;
    }
    return std::atoi(v);
}

static std::string StatsMemberValueOneLine(const webrtc::Attribute& m) {
    return m.ToString();
}

static void MaybeDumpFullGetStatsReports(std::ostream& out,
                                         const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report,
                                         const char* peer_tag) {
    if (!GetStatsFullDumpEnabled() || !report) {
        return;
    }
    const int interval_ms = GetStatsDumpIntervalMs();
    static std::mutex rate_mu;
    static std::chrono::steady_clock::time_point last_dump{};
    static bool have_last{false};
    if (interval_ms > 0) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(rate_mu);
        if (have_last) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_dump).count();
            if (elapsed < interval_ms) {
                return;
            }
        }
        last_dump = now;
        have_last = true;
    }

    const char* ptag = (peer_tag && peer_tag[0] != '\0') ? peer_tag : "(pc)";
    const bool json_only = GetStatsDumpJsonOnly();
    const bool members_extra = GetStatsDumpMembersExtra();
    std::string j = report->ToJson();
    out << "[getstats-full] peer=" << ptag << " stats_count=" << report->size() << "\n";
    if (!j.empty()) {
        out << j << "\n";
    }
    const bool dump_members = !json_only || members_extra || j.empty();
    if (dump_members) {
        for (const webrtc::RTCStats& st : *report) {
            for (const webrtc::Attribute& attr : st.Attributes()) {
                out << "  " << st.type() << " id=" << st.id() << " m[" << attr.name()
                    << "]=" << StatsMemberValueOneLine(attr) << "\n";
            }
        }
    }
    out << std::flush;
}

struct LatencyPollResult {
    LatencyPollStatus status{LatencyPollStatus::Ok};
    const char* error_message{nullptr};
    LatencyOneLineSummary sum;
    int64_t latest_ts_us{0};
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

static LatencyPollResult PollLatencySummaryForPc(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                                                 int timeout_ms,
                                                 const char* getstats_peer_tag) {
    LatencyPollResult timeout_r;
    timeout_r.status = LatencyPollStatus::Timeout;
    if (!pc) {
        LatencyPollResult r;
        r.status = LatencyPollStatus::Error;
        r.error_message = "no_pc";
        return r;
    }

    auto done = std::make_shared<std::promise<LatencyPollResult>>();
    std::future<LatencyPollResult> fut = done->get_future();

    auto cb = webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>(
        new webrtc::RefCountedObject<StatsDeliveredCallback>(
            [done, getstats_peer_tag](const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
                LatencyPollResult out;
                if (!report || report->size() == 0) {
                    out.status = LatencyPollStatus::Empty;
                    done->set_value(out);
                    return;
                }
                MaybeDumpFullGetStatsReports(std::cout, report, getstats_peer_tag);
                int64_t latest_ts_us = 0;
                for (const webrtc::RTCStats& rep : *report) {
                    int64_t tus = rep.timestamp().us();
                    if (tus > latest_ts_us) {
                        latest_ts_us = tus;
                    }
                    std::string rtype = rep.type();
                    for (const webrtc::Attribute& mem : rep.Attributes()) {
                        AccumulateLatencySummary(rtype, mem.name(), mem, &out.sum);
                    }
                }
                out.latest_ts_us = latest_ts_us;
                out.status = LatencyPollStatus::Ok;
                done->set_value(out);
            }));

    pc->GetStats(cb.get());

    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return timeout_r;
    }
    return fut.get();
}

static void DumpLatencyStatsForPc(std::ostream& out,
                                  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                                  const std::string& peer_tag,
                                  int timeout_ms) {
    using clock = std::chrono::system_clock;
    const int64_t t_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();

    LatencyPollResult r = PollLatencySummaryForPc(pc, timeout_ms, peer_tag.c_str());
    if (r.status == LatencyPollStatus::Timeout) {
        out << "[stats-latency] t_ms=" << t_ms << " peer=" << peer_tag << " err=stats_timeout\n" << std::flush;
        return;
    }
    if (r.status == LatencyPollStatus::Error) {
        out << "[stats-latency] t_ms=" << t_ms << " peer=" << peer_tag
            << " GetStats_error=" << (r.error_message ? r.error_message : "") << "\n" << std::flush;
        return;
    }
    if (r.status == LatencyPollStatus::Empty) {
        out << "[stats-latency] t_ms=" << t_ms << " peer=" << peer_tag << " note=empty_stats\n" << std::flush;
        return;
    }

    const LatencyOneLineSummary& sum = r.sum;
    const bool any = sum.capture_total_ms.has_value() || sum.encode_total_ms.has_value() ||
                     sum.packet_send_delay_total_ms.has_value() || sum.ice_current_rtt_ms.has_value() ||
                     sum.remote_rtt_ms.has_value() || sum.remote_jitter_ms.has_value() ||
                     sum.avg_rtcp_interval_ms.has_value() || sum.remote_total_rtt_sum_ms.has_value() ||
                     sum.remote_rtt_measurements.has_value();

    std::ostringstream ts_ms;
    ts_ms << std::fixed << std::setprecision(3) << (static_cast<double>(r.latest_ts_us) / 1000.0);

    if (!any) {
        out << "[stats-latency] t_ms=" << t_ms << " peer=" << peer_tag
            << " note=no_known_latency_fields stats_timestamp_ms=" << ts_ms.str()
            << " (stats_timestamp_ms=RTC report clock; unrelated to LATENCY_STATS_WINDOW_FRAMES)\n";
    } else {
        out << "[stats-latency] t_ms=" << t_ms << " peer=" << peer_tag << " stats_timestamp_ms=" << ts_ms.str()
            << " capture_total_ms=" << FormatMsOptional(sum.capture_total_ms, 2)
            << " encode_total_ms=" << FormatMsOptional(sum.encode_total_ms, 2)
            << " rtp_send_queue_total_ms=" << FormatMsOptional(sum.packet_send_delay_total_ms, 2)
            << " ice_rtt_ms=" << FormatMsOptional(sum.ice_current_rtt_ms, 3)
            << " remote_feedback_rtt_ms=" << FormatMsOptional(sum.remote_rtt_ms, 3)
            << " remote_jitter_ms=" << FormatMsOptional(sum.remote_jitter_ms, 3)
            << " avg_rtcp_interval_ms=" << FormatMsOptional(sum.avg_rtcp_interval_ms, 2)
            << " remote_rtt_sum_ms=" << FormatMsOptional(sum.remote_total_rtt_sum_ms, 2)
            << " remote_rtt_measurements=" << (sum.remote_rtt_measurements.has_value()
                                                  ? std::to_string(*sum.remote_rtt_measurements)
                                                  : std::string("-"))
            << " (cumulative=session totals; ms/frame rolling in [stats-latency-avg]; missing=-)\n";
    }
    out << std::flush;
}

static std::optional<double> DeltaPerFrameMs(const std::optional<double>& prev,
                                             const std::optional<double>& curr,
                                             unsigned int df) {
    if (df == 0 || !prev.has_value() || !curr.has_value()) {
        return std::nullopt;
    }
    return (*curr - *prev) / static_cast<double>(df);
}

static std::optional<double> RttSampleMeanMsInWindow(const std::optional<double>& prev_sum_ms,
                                                     const std::optional<double>& curr_sum_ms,
                                                     const std::optional<uint64_t>& prev_n,
                                                     const std::optional<uint64_t>& curr_n) {
    if (!prev_sum_ms || !curr_sum_ms || !prev_n || !curr_n) {
        return std::nullopt;
    }
    const int64_t dn = static_cast<int64_t>(*curr_n) - static_cast<int64_t>(*prev_n);
    if (dn <= 0) {
        return std::nullopt;
    }
    return (*curr_sum_ms - *prev_sum_ms) / static_cast<double>(dn);
}

static std::optional<double> EndpointMeanMs(const std::optional<double>& a, const std::optional<double>& b) {
    if (a && b) {
        return (*a + *b) / 2.0;
    }
    if (a) {
        return *a;
    }
    if (b) {
        return *b;
    }
    return std::nullopt;
}

static bool LatencyPipelineVerbose() {
    const char* v = std::getenv("WEBRTC_LATENCY_PIPELINE_VERBOSE");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

static void PrintLatencyPipelineLegendEnglish(std::ostream& out) {
    static std::atomic<bool> legend_printed{false};
    if (legend_printed.exchange(true)) {
        return;
    }
    out << "[latency-pipeline] legend=v1 lang=en\n";
    out << "[latency-pipeline] scope=push_path_only end_to_end_includes_receiver_wifi_jitter_buffer_decode\n";
    out << std::flush;
}

struct UserInterFrameRoll {
    double mean_ms{0};
    double std_ms{0};
    unsigned n{0};
    double nominal_if_ms{0};
};

static void AppendUserInterFrameStats(std::ostream& out, const UserInterFrameRoll& u) {
    if (u.n == 0) {
        out << " usr_if_mean_ms=- usr_if_std_ms=- usr_if_n=0 usr_if_nom_ms=- usr_if_skew_ms=-";
        return;
    }
    out << " usr_if_mean_ms=" << std::fixed << std::setprecision(3) << u.mean_ms << " usr_if_std_ms=" << std::setprecision(3)
        << u.std_ms << " usr_if_n=" << u.n;
    if (u.nominal_if_ms > 0.0) {
        const double skew = std::fabs(u.mean_ms - u.nominal_if_ms);
        out << " usr_if_nom_ms=" << std::setprecision(3) << u.nominal_if_ms << " usr_if_skew_ms=" << std::setprecision(3)
            << skew;
    } else {
        out << " usr_if_nom_ms=- usr_if_skew_ms=-";
    }
}

static void EmitEnglishPipelineStages(std::ostream& out,
                                      const std::string& peer_tag,
                                      unsigned int fc,
                                      unsigned int df,
                                      double frame_spacing_ms,
                                      const std::optional<double>& capture_ms_pf,
                                      const std::optional<double>& encode_ms_pf,
                                      const std::optional<double>& rtp_send_path_ms_pf,
                                      const UserInterFrameRoll& user_if) {
    PrintLatencyPipelineLegendEnglish(out);
    out << "[latency-pipeline] peer=" << peer_tag << " capture_frame_index=" << fc << " stats_window_frames=" << df
        << "\n";
    if (user_if.n > 0) {
        out << "[latency-pipeline] stage=UserspaceOnFrame_interval_roll_ms status=OBSERVABLE mean_ms="
            << std::fixed << std::setprecision(4) << user_if.mean_ms << " std_ms=" << user_if.std_ms << " n="
            << user_if.n << "\n";
    }
    out << "[latency-pipeline] stage=UserspaceBufferOrMmap_delivery_spacing_ms value="
        << std::fixed << std::setprecision(3) << frame_spacing_ms << "\n";
    out << "[latency-pipeline] stage=H264_Encode_ms_per_frame value_ms=" << FormatMsOptional(encode_ms_pf, 4) << "\n";
    out << "[latency-pipeline] stage=RTP_SendPath_ms_per_frame value_ms=" << FormatMsOptional(rtp_send_path_ms_pf, 4)
        << "\n";
    if (capture_ms_pf.has_value()) {
        out << "[latency-pipeline] stage=MediaSource_totalCaptureTime_ms_per_frame value_ms="
            << std::fixed << std::setprecision(4) << *capture_ms_pf << "\n";
    }
    out << std::flush;
}

struct LatencyPeerRollState {
    bool have_baseline{false};
    unsigned baseline_fc{0};
    LatencyOneLineSummary baseline;
};

static void ApplyLatencyRollStep(std::ostream& out,
                                 const std::string& peer_tag,
                                 const LatencyPollResult& poll,
                                 unsigned int fc,
                                 unsigned int window_frames,
                                 LatencyPeerRollState* st,
                                 double frame_spacing_ms,
                                 const UserInterFrameRoll& user_if) {
    using clock = std::chrono::system_clock;
    const int64_t t_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();

    if (poll.status == LatencyPollStatus::Timeout) {
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag << " error=stats_timeout\n" << std::flush;
        return;
    }
    if (poll.status == LatencyPollStatus::Error) {
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag
            << " error=GetStats msg=" << (poll.error_message ? poll.error_message : "") << "\n" << std::flush;
        return;
    }
    if (poll.status == LatencyPollStatus::Empty) {
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag << " note=empty_stats\n" << std::flush;
        return;
    }

    const LatencyOneLineSummary& cur = poll.sum;

    if (!st->have_baseline) {
        st->baseline = cur;
        st->baseline_fc = fc;
        st->have_baseline = true;
        return;
    }

    const unsigned int df = fc - st->baseline_fc;
    if (df < window_frames) {
        return;
    }

    const LatencyOneLineSummary& a = st->baseline;

    std::ostringstream ts_ms;
    ts_ms << std::fixed << std::setprecision(3) << (static_cast<double>(poll.latest_ts_us) / 1000.0);

    const auto d_capture = DeltaPerFrameMs(a.capture_total_ms, cur.capture_total_ms, df);
    const auto d_encode = DeltaPerFrameMs(a.encode_total_ms, cur.encode_total_ms, df);
    const auto d_rtp = DeltaPerFrameMs(a.packet_send_delay_total_ms, cur.packet_send_delay_total_ms, df);

    out << std::fixed << std::setprecision(3)
        << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag << " ts_ms=" << ts_ms.str()
        << " fc=" << fc << " win=" << df << " if_ms=" << frame_spacing_ms
        << " cap_ms_pf=" << FormatMsOptional(d_capture, 4)
        << " enc_ms_pf=" << FormatMsOptional(d_encode, 4)
        << " rtp_q_ms_pf=" << FormatMsOptional(d_rtp, 4)
        << " ice_ms=" << FormatMsOptional(EndpointMeanMs(a.ice_current_rtt_ms, cur.ice_current_rtt_ms), 3)
        << " rem_rtt_ms=" << FormatMsOptional(EndpointMeanMs(a.remote_rtt_ms, cur.remote_rtt_ms), 3)
        << " rem_jit_ms=" << FormatMsOptional(EndpointMeanMs(a.remote_jitter_ms, cur.remote_jitter_ms), 3)
        << " rem_rtt_samp_ms="
        << FormatMsOptional(RttSampleMeanMsInWindow(a.remote_total_rtt_sum_ms, cur.remote_total_rtt_sum_ms,
                                                    a.remote_rtt_measurements, cur.remote_rtt_measurements),
                            3);
    AppendUserInterFrameStats(out, user_if);
    out << "\n"
        << std::defaultfloat
        << std::flush;
    if (LatencyPipelineVerbose()) {
        EmitEnglishPipelineStages(out, peer_tag, fc, df, frame_spacing_ms, d_capture, d_encode, d_rtp, user_if);
    }

    st->baseline = cur;
    st->baseline_fc = fc;
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
    stun.urls.push_back(config.stun_server);
    rtc_config.servers.push_back(stun);
    if (!config.turn_server.empty()) {
        webrtc::PeerConnectionInterface::IceServer turn;
        turn.urls.push_back(config.turn_server);
        turn.username = config.turn_username;
        turn.password = config.turn_password;
        rtc_config.servers.push_back(turn);
    }
    rtc_config.disable_ipv6_on_wifi = true;
    rtc_config.max_ipv6_networks = 0;
    rtc_config.disable_link_local_networks = true;
    rtc_config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    rtc_config.tcp_candidate_policy = webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
    rtc_config.set_dscp(true);
    rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    rtc_config.prioritize_most_likely_ice_candidate_pairs = config.ice_prioritize_likely_pairs;
    return rtc_config;
}

}  // namespace

class FrameCountingSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    static constexpr size_t kInterFrameRing = 128;

    using OnFrameCallback = webrtc_demo::OnFrameCallback;
    explicit FrameCountingSink(OnFrameCallback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(const webrtc::VideoFrame& frame) override {
        const auto now = std::chrono::steady_clock::now();
        if (have_last_frame_time_) {
            const double ms = std::chrono::duration<double, std::milli>(now - last_frame_time_).count();
            last_inter_frame_ms_.store(ms, std::memory_order_relaxed);
            RecordInterFrameGap(ms);
        } else {
            have_last_frame_time_ = true;
        }
        last_frame_time_ = now;

        unsigned int n = ++frame_count_;
        if (on_frame_) {
            on_frame_(n, frame.width(), frame.height());
        }
    }

    unsigned int GetFrameCount() const { return frame_count_.load(); }

    double GetLastInterFrameMs() const { return last_inter_frame_ms_.load(std::memory_order_relaxed); }

    void GetRollingInterFrameStats(double* mean_ms, double* std_ms, unsigned* n_out) const {
        std::lock_guard<std::mutex> lock(if_mu_);
        if (if_n_ == 0) {
            *mean_ms = 0;
            *std_ms = 0;
            *n_out = 0;
            return;
        }
        double sum = 0;
        for (size_t i = 0; i < if_n_; ++i) {
            const size_t idx = (if_w_ + kInterFrameRing - 1 - i) % kInterFrameRing;
            sum += if_ring_[idx];
        }
        const double m = sum / static_cast<double>(if_n_);
        *mean_ms = m;
        if (if_n_ < 2) {
            *std_ms = 0;
        } else {
            double v = 0;
            for (size_t i = 0; i < if_n_; ++i) {
                const size_t idx = (if_w_ + kInterFrameRing - 1 - i) % kInterFrameRing;
                const double d = if_ring_[idx] - m;
                v += d * d;
            }
            *std_ms = std::sqrt(v / static_cast<double>(if_n_ - 1));
        }
        *n_out = static_cast<unsigned>(if_n_);
    }

private:
    void RecordInterFrameGap(double ms) {
        std::lock_guard<std::mutex> lock(if_mu_);
        if_ring_[if_w_ % kInterFrameRing] = ms;
        if_w_++;
        if (if_n_ < kInterFrameRing) {
            if_n_++;
        }
    }

    std::atomic<unsigned int> frame_count_{0};
    std::chrono::steady_clock::time_point last_frame_time_{};
    bool have_last_frame_time_{false};
    std::atomic<double> last_inter_frame_ms_{0.0};
    mutable std::mutex if_mu_;
    std::array<double, kInterFrameRing> if_ring_{};
    size_t if_w_{0};
    size_t if_n_{0};
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

webrtc::PeerConnectionInterface::RTCOfferAnswerOptions MakeOfferOptions(const PushStreamerConfig& cfg) {
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions o;
    o.num_simulcast_layers = 1;
    o.offer_to_receive_audio = cfg.enable_audio ? webrtc::PeerConnectionInterface::RTCOfferAnswerOptions::kOfferToReceiveMediaTrue : 0;
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
        webrtc_demo::EnsureFlexfecFieldTrials(config_.enable_flexfec, config_.flexfec_field_trials);
        std::cout << "[PushStreamer] Initializing WebRTC (native API)..." << std::endl;
        if (!webrtc::InitializeSSL()) {
            std::cerr << "[PushStreamer] InitializeSSL failed" << std::endl;
            return false;
        }

        webrtc::PeerConnectionFactoryDependencies deps;
        webrtc_demo::PeerConnectionFactoryMediaOptions media_opts;
        media_opts.prefer_rockchip_mpp_h264 = config_.use_rockchip_mpp_h264;
        webrtc_demo::ConfigurePeerConnectionFactoryDependencies(deps, &media_opts);
        if (!config_.enable_audio) {
            EnableAlsaNullDeviceFallback();
        }
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

        std::vector<std::string> stream_ids = {config_.stream_id};
        // 多订阅者 + 仅对订阅者发 Offer：同一 VideoTrack 不要同时挂到「占位」默认 PC 与订阅者 PC，
        // 否则部分 libwebrtc 版本在第二路 CreateOffer 上可能长期不回调（拉流端收不到 SDP）。
        if (!config_.signaling_subscriber_offer_only) {
            auto add = peer_connection_->AddTrack(video_track_, stream_ids);
            if (!add.ok()) {
                std::cerr << "[PushStreamer] AddTrack failed: " << add.error().message() << std::endl;
                return false;
            }
            ApplyVideoCodecPreferences(peer_connection_);
            ApplyEncodingParameters(peer_connection_);
            std::cout << "[PushStreamer] Video track added (stream_id=" << config_.stream_id << ")" << std::endl;
        } else {
            std::cout << "[PushStreamer] Video track ready (subscriber-offer-only; sender per subscriber PC, "
                         "stream_id="
                      << config_.stream_id << ")" << std::endl;
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
        std::string want = config_.video_codec;
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
            std::cout << "[PushStreamer] SetCodecPreferences skipped: no match for VIDEO_CODEC=" << config_.video_codec
                      << std::endl;
            return;
        }
        if (want == "h264") {
            const std::string want_idc = H264ProfileIdcHex2(config_.h264_profile);
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
                          << config_.h264_profile << ")" << std::endl;
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
            const webrtc::Priority net_prio = ParseVideoNetworkPriority(config_.video_network_priority);
            const int fps_cap = (config_.video_encoding_max_framerate > 0) ? config_.video_encoding_max_framerate
                                                                           : config_.video_fps;
            const double max_fps = (fps_cap > 0) ? static_cast<double>(fps_cap) : 30.0;
            for (auto& enc : params.encodings) {
                enc.min_bitrate_bps = config_.min_bitrate_kbps * 1000;
                enc.max_bitrate_bps = config_.max_bitrate_kbps * 1000;
                enc.network_priority = net_prio;
                enc.max_framerate = max_fps;
            }
            if (config_.degradation_preference == "maintain_resolution") {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
            } else if (config_.degradation_preference == "maintain_framerate") {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
            } else if (config_.degradation_preference == "balanced") {
                params.degradation_preference = webrtc::DegradationPreference::BALANCED;
            } else {
                params.degradation_preference = webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
            }
            auto err = sender->SetParameters(params);
            if (!err.ok()) {
                std::cerr << "[PushStreamer] SetParameters failed: " << err.message() << std::endl;
            } else {
                std::cout << "[PushStreamer] Encoding params: bitrate " << config_.min_bitrate_kbps << "-"
                          << config_.max_bitrate_kbps << " kbps"
                          << " max_fps=" << max_fps
                          << " network_priority=" << config_.video_network_priority << std::endl;
            }
            break;
        }
    }

    bool ResolveDeviceUniqueId(std::string* out_unique) {
        // 显式 /dev/videoN 时优先按路径直采，勿依赖 WebRTC 枚举（枚举偶发为 0 时仍应能推流）。
        // 是否 CAPTURE 由 CameraVideoTrackSource::StartDirectV4l2 再验；此处不要求枚举下标算成功。
        if (!config_.video_device_path.empty()) {
            const std::string& p = config_.video_device_path;
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

        if (!config_.video_device_path.empty()) {
            // 与 device_info_v4l2 一致：/dev/videoN → 第 N' 个 CAPTURE 节点的枚举下标（非路径配置等）
            {
                int path_idx = webrtc_demo::GetWebRtcCaptureDeviceIndexForPath(config_.video_device_path);
                if (path_idx >= 0 && static_cast<uint32_t>(path_idx) < n) {
                    char name[256] = {0};
                    char unique[256] = {0};
                    char product[256] = {0};
                    if (info->GetDeviceName(static_cast<uint32_t>(path_idx), name, sizeof(name), unique, sizeof(unique),
                                            product, sizeof(product)) == 0) {
                        *out_unique = unique;
                        std::cout << "[PushStreamer] Camera path " << config_.video_device_path
                                  << " -> /dev/video enum index match" << std::endl;
                        return true;
                    }
                }
            }
            // Linux 上 GetDeviceName 的 unique 实为 V4L2 bus_info（product 常为空，勿用 product==bus）
            std::string bus = GetDeviceBusInfo(config_.video_device_path);
            for (uint32_t i = 0; i < n; ++i) {
                char name[256] = {0};
                char unique[256] = {0};
                char product[256] = {0};
                if (info->GetDeviceName(i, name, sizeof(name), unique, sizeof(unique), product, sizeof(product)) != 0) {
                    continue;
                }
                if (!bus.empty() && std::string(unique) == bus) {
                    *out_unique = unique;
                    std::cout << "[PushStreamer] Camera path " << config_.video_device_path << " -> bus_info match"
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
                if (std::string(unique).find(config_.video_device_path) != std::string::npos) {
                    *out_unique = unique;
                    return true;
                }
            }
            // WebRTC 的 unique 往往不含 /dev/videoN；用 V4L2 card 与枚举设备名对齐（常见）
            {
                std::string card = webrtc_demo::GetDeviceCardName(config_.video_device_path);
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
                            std::cout << "[PushStreamer] Camera path " << config_.video_device_path
                                      << " -> device name/card match" << std::endl;
                            return true;
                        }
                    }
                }
            }
            std::cerr << "[PushStreamer] No device match for " << config_.video_device_path << ", using index 0"
                      << std::endl;
        }

        uint32_t idx = static_cast<uint32_t>(config_.video_device_index);
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

        bool mpp_mjpeg_decode = config_.use_rockchip_mpp_mjpeg_decode;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
        bool allow_dual_mpp = config_.use_rockchip_dual_mpp_mjpeg_h264;
        if (const char* ev = std::getenv("WEBRTC_DUAL_MPP_MJPEG_H264")) {
            if (ev[0] == '1' || ev[0] == 'y' || ev[0] == 'Y' || ev[0] == 't' || ev[0] == 'T') {
                allow_dual_mpp = true;
            }
        }
        if (mpp_mjpeg_decode && config_.use_rockchip_mpp_h264 && !allow_dual_mpp) {
            mpp_mjpeg_decode = false;
            std::cout << "[PushStreamer] MPP MJPEG decode off while MPP H.264 encode on (use libyuv for MJPEG). "
                         "Set USE_DUAL_MPP_MJPEG_H264=1 or WEBRTC_DUAL_MPP_MJPEG_H264=1 to enable both.\n";
        } else if (mpp_mjpeg_decode && config_.use_rockchip_mpp_h264 && allow_dual_mpp) {
            std::cout << "[PushStreamer] Dual MPP: MJPEG hardware decode + H.264 hardware encode (experimental).\n";
        }
#endif
        if (!static_cast<CameraVideoTrackSource*>(cam_holder)
                 ->Start(unique_id.c_str(), config_.video_width, config_.video_height, config_.video_fps,
                         mpp_mjpeg_decode)) {
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

        std::cout << "[PushStreamer] Video capture started" << std::endl;

        {
            int nw = 0;
            int nh = 0;
            if (camera_impl_->GetNegotiatedCaptureSize(&nw, &nh) &&
                (nw != config_.video_width || nh != config_.video_height)) {
                std::cout << "[PushStreamer] V4L2 negotiated " << nw << "x" << nh << ", config requests "
                          << config_.video_width << "x" << config_.video_height
                          << " — set WIDTH/HEIGHT in streams.conf to match to reduce capture/encode scaling.\n";
            }
        }

        if (config_.capture_warmup_sec > 0) {
            std::cout << "[PushStreamer] Camera warmup " << config_.capture_warmup_sec << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(config_.capture_warmup_sec));
        }
        return true;
    }

    bool WaitForCaptureGate(const std::string& context) {
        const int need = config_.capture_gate_min_frames;
        if (need <= 0 || !frame_counter_) {
            return true;
        }
        int max_wait = config_.capture_gate_max_wait_sec;
        if (max_wait < 1) {
            max_wait = 1;
        }
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(max_wait);
        std::cout << "[PushStreamer] Capture gate: need >= " << need << " frames (" << context << "), max wait "
                  << max_wait << "s" << std::endl;
        while (clock::now() < deadline) {
            const unsigned int got = EffectiveCaptureFrameCount();
            if (got >= static_cast<unsigned int>(need)) {
                std::cout << "[PushStreamer] Capture gate OK: " << got << " frames" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        std::cerr << "[PushStreamer] Capture gate failed (" << context << ")" << std::endl;
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
        // 与主线程里 LogLatencyStatsRollingAvg 等抢同一把 mutex 会死锁 → 不发 Offer、拉流永远 0 帧。
        auto observer = std::make_unique<ExtraPeerObserver>(this, peer_id);
        auto pc = CreatePcWithObserver(observer.get(), MakeRtcConfiguration(config_));
        if (!pc) {
            return false;
        }
        if (!video_track_) {
            std::cerr << "[PushStreamer] EnsurePeerConnectionForPeer: no video track" << std::endl;
            return false;
        }
        std::vector<std::string> stream_ids = {config_.stream_id};
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
        return true;
    }

    void CreateOfferOnConnection(const std::string& peer_id,
                                 webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc) {
        if (!pc) {
            return;
        }
        auto opts = MakeOfferOptions(config_);
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

                    auto set_local = webrtc::scoped_refptr<webrtc::SetLocalDescriptionObserverInterface>(
                        new webrtc::RefCountedObject<SetLocalDescObserver>(
                            [this, peer_id_ptr, type, sdp](webrtc::RTCError err) {
                                if (!err.ok()) {
                                    std::cerr << "[PushStreamer] SetLocalDescription failed: " << err.message()
                                              << std::endl;
                                    return;
                                }
                                if (config_.test_encode_mode && peer_id_ptr->empty()) {
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
        auto opts = MakeOfferOptions(config_);
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
        if (config_.test_encode_mode && receiver_) {
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
    bool TestCaptureOnly() const { return config_.test_capture_only; }
    bool SignalingSubscriberOfferOnly() const { return config_.signaling_subscriber_offer_only; }

    void LogLatencyStatsForAllPeers(std::ostream& out) {
        std::vector<std::pair<std::string, webrtc::scoped_refptr<webrtc::PeerConnectionInterface>>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : peer_connections_) {
                copy.emplace_back(kv.first, kv.second);
            }
        }
        if (copy.empty()) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pc = peer_connection_;
            }
            if (pc) {
                DumpLatencyStatsForPc(out, pc, "default", 5000);
            } else {
                using clock = std::chrono::system_clock;
                const int64_t t_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
                out << "[stats-latency] t_ms=" << t_ms << " peer=(none) note=no_peerconnection\n" << std::flush;
            }
            return;
        }
        for (const auto& pr : copy) {
            DumpLatencyStatsForPc(out, pr.second, pr.first, 5000);
        }
    }

    void LogLatencyStatsRollingAvg(std::ostream& out, unsigned int fc) {
        const unsigned int w = static_cast<unsigned int>(
            config_.latency_stats_window_frames < 1 ? 1 : config_.latency_stats_window_frames);
        std::vector<std::pair<std::string, webrtc::scoped_refptr<webrtc::PeerConnectionInterface>>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : peer_connections_) {
                copy.emplace_back(kv.first, kv.second);
            }
        }
        if (copy.empty()) {
            webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pc = peer_connection_;
            }
            if (pc) {
                LatencyPollResult r = PollLatencySummaryForPc(pc, 5000, "default");
                const double spacing = frame_counter_ ? frame_counter_->GetLastInterFrameMs() : 0.0;
                UserInterFrameRoll uif;
                if (frame_counter_) {
                    frame_counter_->GetRollingInterFrameStats(&uif.mean_ms, &uif.std_ms, &uif.n);
                }
                if (config_.video_fps > 0) {
                    uif.nominal_if_ms = 1000.0 / static_cast<double>(config_.video_fps);
                }
                std::lock_guard<std::mutex> lock(latency_roll_mu_);
                ApplyLatencyRollStep(out, "default", r, fc, w, &latency_roll_["default"], spacing, uif);
            } else {
                using clock = std::chrono::system_clock;
                const int64_t t_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
                out << "[stats-latency-avg] t_ms=" << t_ms << " peer=(none) note=no_peerconnection\n" << std::flush;
            }
            return;
        }
        const double spacing = frame_counter_ ? frame_counter_->GetLastInterFrameMs() : 0.0;
        UserInterFrameRoll uif;
        if (frame_counter_) {
            frame_counter_->GetRollingInterFrameStats(&uif.mean_ms, &uif.std_ms, &uif.n);
        }
        if (config_.video_fps > 0) {
            uif.nominal_if_ms = 1000.0 / static_cast<double>(config_.video_fps);
        }
        for (const auto& pr : copy) {
            LatencyPollResult r = PollLatencySummaryForPc(pr.second, 5000, pr.first.c_str());
            std::lock_guard<std::mutex> lock(latency_roll_mu_);
            ApplyLatencyRollStep(out, pr.first, r, fc, w, &latency_roll_[pr.first], spacing, uif);
        }
    }

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
    std::mutex latency_roll_mu_;
    std::unordered_map<std::string, LatencyPeerRollState> latency_roll_;
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

void PushStreamer::LogLatencyStatsForAllPeers(std::ostream& out) {
    impl_->LogLatencyStatsForAllPeers(out);
}

void PushStreamer::LogLatencyStatsRollingAvg(std::ostream& out) {
    impl_->LogLatencyStatsRollingAvg(out, GetFrameCount());
}

}  // namespace webrtc_demo
