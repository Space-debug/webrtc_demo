#include "push_streamer.h"
#include "camera_utils.h"
#include "webrtc_field_trials.h"
#include <libwebrtc.h>

using libwebrtc::scoped_refptr;
#include <rtc_ice_candidate.h>
#include <rtc_mediaconstraints.h>
#include <rtc_media_stream.h>
#include <rtc_peerconnection.h>
#include <rtc_peerconnection_factory.h>
#include <rtc_rtp_capabilities.h>
#include <rtc_rtp_parameters.h>
#include <rtc_rtp_receiver.h>
#include <rtc_rtp_sender.h>
#include <rtc_rtp_transceiver.h>
#include <rtc_session_description.h>
#include <rtc_video_device.h>
#include <rtc_video_frame.h>
#include <rtc_video_source.h>
#include <rtc_video_track.h>
#include <helper.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <atomic>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace webrtc_demo {

namespace {

// 在无音频设备的 headless 环境中，为 ALSA 提供空设备映射，避免 ADM 初始化直接崩溃。
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

static std::optional<double> LatencyMemberDoubleSeconds(const libwebrtc::RTCStatsMember& m) {
    using T = libwebrtc::RTCStatsMember::Type;
    if (!m.IsDefined() || m.GetType() != T::kDouble) {
        return std::nullopt;
    }
    return m.ValueDouble();
}

/// media-source totalCaptureTime: spec is seconds (double). Some bindings expose integer milliseconds.
static std::optional<double> MediaSourceTotalCaptureTimeMs(const libwebrtc::RTCStatsMember& m) {
    using T = libwebrtc::RTCStatsMember::Type;
    if (!m.IsDefined()) {
        return std::nullopt;
    }
    if (m.GetType() == T::kDouble) {
        return m.ValueDouble() * 1000.0;
    }
    if (m.GetType() == T::kUint64) {
        return static_cast<double>(m.ValueUint64());
    }
    if (m.GetType() == T::kInt64) {
        return static_cast<double>(m.ValueInt64());
    }
    if (m.GetType() == T::kUint32) {
        return static_cast<double>(m.ValueUint32());
    }
    if (m.GetType() == T::kInt32) {
        return static_cast<double>(m.ValueInt32());
    }
    return std::nullopt;
}

static std::optional<uint64_t> LatencyMemberUint64(const libwebrtc::RTCStatsMember& m) {
    using T = libwebrtc::RTCStatsMember::Type;
    if (!m.IsDefined()) {
        return std::nullopt;
    }
    if (m.GetType() == T::kUint64) {
        return m.ValueUint64();
    }
    if (m.GetType() == T::kUint32) {
        return static_cast<uint64_t>(m.ValueUint32());
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

/// 从 GetStats 汇总推流端关心的时延指标（均为毫秒；带「累计」的为 WebRTC 累计量，非单帧）。
struct LatencyOneLineSummary {
    std::optional<double> capture_total_ms;           // media-source totalCaptureTime
    std::optional<double> encode_total_ms;            // outbound-rtp totalEncodeTime
    std::optional<double> packet_send_delay_total_ms; // outbound-rtp totalPacketSendDelay
    std::optional<double> ice_current_rtt_ms;         // candidate-pair currentRoundTripTime（多对取最大）
    std::optional<double> remote_rtt_ms;              // remote-inbound-rtp roundTripTime
    std::optional<double> remote_jitter_ms;           // remote-inbound-rtp jitter
    std::optional<double> avg_rtcp_interval_ms;       // averageRtcpInterval（多处可能出现，取最大）
    /// remote-inbound-rtp totalRoundTripTime（秒→毫秒）在单次报表内对多报告求和，便于与 roundTripTimeMeasurements 求和一致
    std::optional<double> remote_total_rtt_sum_ms;
    std::optional<uint64_t> remote_rtt_measurements;
};

static void AccumulateLatencySummary(const std::string& rtype,
                                     const std::string& name,
                                     const libwebrtc::RTCStatsMember& m,
                                     LatencyOneLineSummary* s) {
    if (rtype == "remote-inbound-rtp" && name == "roundTripTimeMeasurements") {
        if (auto u = LatencyMemberUint64(m)) {
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
    auto sec = LatencyMemberDoubleSeconds(m);
    if (!sec) {
        return;
    }
    const double ms = *sec * 1000.0;
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

/// One-line representation for RTCStatsMember (sequences/maps summarized).
static std::string StatsMemberValueOneLine(const libwebrtc::RTCStatsMember& m) {
    using T = libwebrtc::RTCStatsMember::Type;
    if (!m.IsDefined()) {
        return "(undefined)";
    }
    switch (m.GetType()) {
        case T::kBool:
            return m.ValueBool() ? "true" : "false";
        case T::kInt32:
            return std::to_string(m.ValueInt32());
        case T::kUint32:
            return std::to_string(m.ValueUint32());
        case T::kInt64:
            return std::to_string(m.ValueInt64());
        case T::kUint64:
            return std::to_string(m.ValueUint64());
        case T::kDouble:
            return std::to_string(m.ValueDouble());
        case T::kString:
            return m.ValueString().std_string();
        case T::kSequenceBool: {
            const auto s = m.ValueSequenceBool();
            return "seq<bool>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceInt32: {
            const auto s = m.ValueSequenceInt32();
            return "seq<int32>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceUint32: {
            const auto s = m.ValueSequenceUint32();
            return "seq<uint32>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceInt64: {
            const auto s = m.ValueSequenceInt64();
            return "seq<int64>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceUint64: {
            const auto s = m.ValueSequenceUint64();
            return "seq<uint64>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceDouble: {
            const auto s = m.ValueSequenceDouble();
            return "seq<double>[" + std::to_string(s.size()) + "]";
        }
        case T::kSequenceString: {
            const auto s = m.ValueSequenceString();
            return "seq<string>[" + std::to_string(s.size()) + "]";
        }
        case T::kMapStringUint64: {
            const auto mp = m.ValueMapStringUint64();
            return "map<string,uint64>[" + std::to_string(mp.size()) + "]";
        }
        case T::kMapStringDouble: {
            const auto mp = m.ValueMapStringDouble();
            return "map<string,double>[" + std::to_string(mp.size()) + "]";
        }
        default:
            return "(unknown_type)";
    }
}

static void MaybeDumpFullGetStatsReports(
    std::ostream& out,
    const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>>& reports,
    const char* peer_tag) {
    if (!GetStatsFullDumpEnabled()) {
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
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_dump).count();
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
    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& rep = reports[i];
        if (!rep) {
            continue;
        }
        const std::string j = rep->ToJson().std_string();
        out << "[getstats-full] peer=" << ptag << " idx=" << i << " type=" << rep->type().std_string()
            << " id=" << rep->id().std_string() << " ts_us=" << rep->timestamp_us() << "\n";
        if (!j.empty()) {
            out << j << "\n";
        }
        const bool dump_members = !json_only || members_extra || j.empty();
        if (dump_members) {
            const auto members = rep->Members();
            for (size_t jm = 0; jm < members.size(); ++jm) {
                const auto& mem = members[jm];
                if (!mem) {
                    continue;
                }
                out << "  m[" << mem->GetName().std_string() << "]=" << StatsMemberValueOneLine(*mem) << "\n";
            }
        }
        out << std::flush;
    }
}

struct LatencyPollResult {
    LatencyPollStatus status{LatencyPollStatus::Ok};
    const char* error_message{nullptr};
    LatencyOneLineSummary sum;
    int64_t latest_ts_us{0};
};

/// 阻塞等待 GetStats 回调，汇总 LatencyOneLineSummary（供瞬时打印与滚动差分共用）。
/// getstats_peer_tag: label for WEBRTC_GETSTATS_DUMP lines (may be null).
static LatencyPollResult PollLatencySummaryForPc(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc,
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

    pc->GetStats(
        [done, getstats_peer_tag](
            const libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::MediaRTCStats>>& reports) {
            LatencyPollResult out;
            if (reports.size() == 0) {
                out.status = LatencyPollStatus::Empty;
                done->set_value(out);
                return;
            }
            MaybeDumpFullGetStatsReports(std::cout, reports, getstats_peer_tag);
            int64_t latest_ts_us = 0;
            for (size_t i = 0; i < reports.size(); ++i) {
                const auto& rep = reports[i];
                if (!rep) {
                    continue;
                }
                if (rep->timestamp_us() > latest_ts_us) {
                    latest_ts_us = rep->timestamp_us();
                }
                std::string rtype = rep->type().std_string();
                const auto members = rep->Members();
                for (size_t j = 0; j < members.size(); ++j) {
                    const auto& mem = members[j];
                    if (!mem) {
                        continue;
                    }
                    AccumulateLatencySummary(rtype, mem->GetName().std_string(), *mem, &out.sum);
                }
            }
            out.latest_ts_us = latest_ts_us;
            out.status = LatencyPollStatus::Ok;
            done->set_value(out);
        },
        [done](const char* err) {
            LatencyPollResult out;
            out.status = LatencyPollStatus::Error;
            out.error_message = err;
            done->set_value(out);
        });

    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return timeout_r;
    }
    return fut.get();
}

/// GetStats for one PeerConnection; one line, English keys (ms).
static void DumpLatencyStatsForPc(std::ostream& out,
                                  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc,
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
    out << "[latency-pipeline] NOT_MEASURABLE_in_app=SensorExposure,SensorReadout,CameraBufferEnqueue,"
           "KernelUrbToV4l2Buffer reason=no_V4L2_timestamp_in_libwebrtc_wrapper_see_usr_if_fields\n";
    out << "[latency-pipeline] NOT_MEASURABLE_separate=MJPEGDecode reason=inside_libwebrtc_VideoCapture "
           "often_fused_with_USB_read_if_pixel_format_MJPEG\n";
    out << "[latency-pipeline] REFERENCE=UserspaceFrameReceived alias=VideoTrack_OnFrame after_capture_"
           "and_optional_MJPEG_to_I420_inside_libwebrtc\n";
    out << "[latency-pipeline] frame_spacing_ms=time_between_OnFrame_callbacks not_equal_to_sensor_latency "
           "healthy_30fps_about_33.33\n";
    out << "[latency-pipeline] H264_Encode_ms_per_frame=WebRTC_outbound_rtp_totalEncodeTime_delta_over_window\n";
    out << "[latency-pipeline] RTP_SendPath_ms_per_frame=WebRTC_outbound_rtp_totalPacketSendDelay_delta_over_"
           "window includes_pacing_queue_not_packetize_only\n";
    out << "[latency-pipeline] target_e2e_under_200ms_stable needs_wired_or_clean_5GHz plus_headroom_above_"
           "min_bitrate see_streams_conf_LATENCY_TARGET_EN\n";
    out << std::flush;
}

/// 用户态：相邻两次 OnFrame 的时间间隔（ms）的滚动统计；反映采集+投递节拍与抖动，非内核 VB2 单段时延。
struct UserInterFrameRoll {
    double mean_ms{0};
    double std_ms{0};
    unsigned n{0};
    double nominal_if_ms{0};  ///< 1000/fps（配置），用于与均值比较
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
    out << "[latency-pipeline] stage=SensorExposure_ms status=NOT_MEASURABLE\n";
    out << "[latency-pipeline] stage=SensorReadout_ms status=NOT_MEASURABLE\n";
    out << "[latency-pipeline] stage=CameraBufferEnqueue_ms status=NOT_MEASURABLE\n";
    out << "[latency-pipeline] stage=KernelUrbToV4l2Buffer_ms status=NOT_MEASURABLE\n";
    if (user_if.n > 0) {
        out << "[latency-pipeline] stage=UserspaceOnFrame_interval_roll_ms status=OBSERVABLE mean_ms="
            << std::fixed << std::setprecision(4) << user_if.mean_ms << " std_ms=" << user_if.std_ms << " n="
            << user_if.n << " note=last_up_to_128_gaps_not_kernel_latency\n";
    } else {
        out << "[latency-pipeline] stage=UserspaceOnFrame_interval_roll_ms status=NA\n";
    }
    out << "[latency-pipeline] stage=UserspaceBufferOrMmap_delivery_spacing_ms value="
        << std::fixed << std::setprecision(3) << frame_spacing_ms
        << " note=OnFrame_to_OnFrame_not_sub_stage_latency\n";
    out << "[latency-pipeline] stage=MJPEG_Decode_ms status=NOT_MEASURABLE_SEPARATE value_if_YUYV=0_typically\n";
    out << "[latency-pipeline] stage=H264_Encode_ms_per_frame status="
        << (encode_ms_pf.has_value() ? "OBSERVABLE" : "NA")
        << " value_ms=" << FormatMsOptional(encode_ms_pf, 4) << "\n";
    out << "[latency-pipeline] stage=RTP_Packetize_and_send_queue_ms_per_frame status="
        << (rtp_send_path_ms_pf.has_value() ? "OBSERVABLE" : "NA")
        << " value_ms=" << FormatMsOptional(rtp_send_path_ms_pf, 4)
        << " note=totalPacketSendDelay_delta_over_df_frames\n";
    if (capture_ms_pf.has_value()) {
        out << "[latency-pipeline] stage=MediaSource_totalCaptureTime_ms_per_frame status=OBSERVABLE "
               "value_ms="
            << std::fixed << std::setprecision(4) << *capture_ms_pf
            << " note=WebRTC_cumulative_often_sparse_check_docs\n";
    } else {
        out << "[latency-pipeline] stage=MediaSource_totalCaptureTime_ms_per_frame status=NA\n";
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
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag << " error=stats_timeout\n"
            << std::flush;
        return;
    }
    if (poll.status == LatencyPollStatus::Error) {
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag
            << " error=GetStats msg=" << (poll.error_message ? poll.error_message : "") << "\n"
            << std::flush;
        return;
    }
    if (poll.status == LatencyPollStatus::Empty) {
        out << "[stats-latency-avg] t_ms=" << t_ms << " peer=" << peer_tag << " note=empty_stats\n"
            << std::flush;
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

    // One line per rolling window (delta cumulative / df). Verbose multi-line: WEBRTC_LATENCY_PIPELINE_VERBOSE=1.
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

}  // namespace

class FrameCountingRenderer
    : public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>> {
public:
    static constexpr size_t kInterFrameRing = 128;

    using OnFrameCallback = webrtc_demo::OnFrameCallback;
    explicit FrameCountingRenderer(OnFrameCallback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override {
        if (!frame) return;
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
            on_frame_(n, frame->width(), frame->height());
        }
    }

    unsigned int GetFrameCount() const { return frame_count_; }

    /// Time between last two OnFrame callbacks (ms); ~1000/fps when healthy. Not per-sensor-stage latency.
    double GetLastInterFrameMs() const { return last_inter_frame_ms_.load(std::memory_order_relaxed); }

    /// Rolling mean/std of last up to kInterFrameRing inter-frame gaps (ms).
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

// 回环接收端观察者：接收 H264 解码后的帧并计数，交换 ICE
class LoopbackReceiverObserver : public libwebrtc::RTCPeerConnectionObserver {
public:
    using AddCandidateFn = std::function<void(const std::string&, int, const std::string&)>;
    explicit LoopbackReceiverObserver(AddCandidateFn add_to_sender)
        : add_to_sender_(std::move(add_to_sender)) {}

    void OnSignalingState(libwebrtc::RTCSignalingState) override {}
    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState) override {}
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
    void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
    void OnRenegotiationNeeded() override {}
    void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

    void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> c) override {
        if (add_to_sender_ && c) {
            add_to_sender_(c->sdp_mid().std_string(), c->sdp_mline_index(),
                           c->candidate().std_string());
        }
    }

    void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> t) override {
        AttachRendererToTrack(t ? t->receiver() : nullptr);
    }

    void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
                    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> r) override {
        AttachRendererToTrack(r);
    }

    unsigned int GetDecodedCount() const {
        return decoded_renderer_ ? decoded_renderer_->GetFrameCount() : 0;
    }

    void RemoveRenderer() {
        if (decoded_renderer_ && decoded_track_) {
            decoded_track_->RemoveRenderer(decoded_renderer_.get());
        }
        decoded_renderer_.reset();
        decoded_track_ = nullptr;
    }

private:
    void AttachRendererToTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> r) {
        if (!r) return;
        auto track = r->track();
        if (!track || std::strcmp(track->kind().c_string(), "video") != 0) return;
        auto vt = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(
            static_cast<libwebrtc::RTCVideoTrack*>(track.get()));
        if (!vt) return;
        decoded_renderer_ = std::make_unique<FrameCountingRenderer>(OnFrameCallback{});
        vt->AddRenderer(decoded_renderer_.get());
        decoded_track_ = vt;
    }

    AddCandidateFn add_to_sender_;
    std::unique_ptr<FrameCountingRenderer> decoded_renderer_;
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> decoded_track_;
};

class PushStreamer::Impl : public libwebrtc::RTCPeerConnectionObserver {
public:
    explicit Impl(const PushStreamerConfig& config) : config_(config) {}

    class ExtraPeerObserver : public libwebrtc::RTCPeerConnectionObserver {
    public:
        ExtraPeerObserver(Impl* owner, std::string peer_id)
            : owner_(owner), peer_id_(std::move(peer_id)) {}

        void OnSignalingState(libwebrtc::RTCSignalingState) override {}
        void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
        void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
        void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
        void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
        void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
        void OnRenegotiationNeeded() override {}
        void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}
        void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
                        libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
        void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

        void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
            if (owner_) owner_->NotifyConnectionState(state);
        }

        void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
            if (owner_) owner_->OnPeerIceCandidate(peer_id_, candidate);
        }

    private:
        Impl* owner_;
        std::string peer_id_;
    };

    bool Initialize() {
        webrtc_demo::EnsureFlexfecFieldTrials(config_.enable_flexfec, config_.flexfec_field_trials);
        std::cout << "[PushStreamer] Initializing LibWebRTC..." << std::endl;
        if (!libwebrtc::LibWebRTC::Initialize()) {
            std::cerr << "[PushStreamer] LibWebRTC init failed" << std::endl;
            return false;
        }
        std::cout << "[PushStreamer] LibWebRTC initialized" << std::endl;

        std::cout << "[PushStreamer] Creating PeerConnectionFactory..." << std::endl;
        factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
        if (!factory_) {
            std::cerr << "[PushStreamer] CreateRTCPeerConnectionFactory failed" << std::endl;
            return false;
        }
        // 某些无声卡/无桌面环境在 factory->Initialize() 内部会因 ADM 直接 CHECK 崩溃；
        // 纯视频模式提前注入 ALSA null，避免进入不可恢复的 abort 路径。
        if (!config_.enable_audio) {
            EnableAlsaNullDeviceFallback();
        }
        if (!factory_->Initialize()) {
            // 某些无声卡环境会在 ADM 初始化时报错；纯视频模式下尝试降级恢复。
            if (!config_.enable_audio) {
                std::cerr << "[PushStreamer] PeerConnectionFactory init failed, "
                          << "try video-only fallback (ALSA null)" << std::endl;
                EnableAlsaNullDeviceFallback();
                factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
                if (factory_ && factory_->Initialize()) {
                    std::cout << "[PushStreamer] PeerConnectionFactory init recovered (video-only)"
                              << std::endl;
                } else {
                    std::cerr << "[PushStreamer] warning: factory initialize still failed, "
                              << "continue in best-effort video-only mode" << std::endl;
                }
            } else {
                std::cerr << "[PushStreamer] PeerConnectionFactory init failed" << std::endl;
                return false;
            }
        }
        std::cout << "[PushStreamer] PeerConnectionFactory created" << std::endl;

        return CreatePeerConnection();
    }

    void Shutdown() {
        if (frame_counter_ && video_track_) {
            video_track_->RemoveRenderer(frame_counter_.get());
        }
        frame_counter_.reset();
        if (loopback_observer_) {
            loopback_observer_->RemoveRenderer();
        }
        if (receiver_) {
            receiver_->DeRegisterRTCPeerConnectionObserver();
            receiver_->Close();
            factory_->Delete(receiver_);
            receiver_ = nullptr;
        }
        loopback_observer_.reset();
        if (peer_connection_) {
            peer_connection_->DeRegisterRTCPeerConnectionObserver();
            peer_connection_->Close();
            factory_->Delete(peer_connection_);
            peer_connection_ = nullptr;
        }
        for (auto& kv : peer_connections_) {
            auto& pc = kv.second;
            if (pc) {
                pc->DeRegisterRTCPeerConnectionObserver();
                pc->Close();
                factory_->Delete(pc);
            }
        }
        peer_connections_.clear();
        extra_peer_observers_.clear();
        local_stream_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        video_capturer_ = nullptr;

        if (factory_) {
            factory_->Terminate();
            factory_ = nullptr;
        }
        libwebrtc::LibWebRTC::Terminate();
    }

    bool CreatePeerConnection() {
        std::cout << "[PushStreamer] Creating PeerConnection (stun=" << config_.stun_server << ")..." << std::endl;
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        if (!config_.turn_server.empty()) {
            rtc_config.ice_servers[1].uri = config_.turn_server;
            rtc_config.ice_servers[1].username = config_.turn_username;
            rtc_config.ice_servers[1].password = config_.turn_password;
        }
        // 减少多网卡干扰：禁用 IPv6 和 link-local，避免 ICE 选错路径导致 0 RTP
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_ipv6_on_wifi = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.bundle_policy = libwebrtc::kBundlePolicyMaxBundle;
        // 仅 UDP：低延迟、画面稳定；本机推拉流需添加路由: sudo ip route add <本机IP>/32 dev lo
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        // 抗花屏：DSCP 标记提升 QoS，WebRTC 默认已启用 NACK/PLI 重传
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        peer_connection_ = factory_->Create(rtc_config, constraints);
        if (!peer_connection_) {
            std::cerr << "[PushStreamer] Create PeerConnection failed" << std::endl;
            return false;
        }

        peer_connection_->RegisterRTCPeerConnectionObserver(this);
        std::cout << "[PushStreamer] PeerConnection created" << std::endl;

        if (!CreateMediaTracks()) {
            return false;
        }

        if (!video_track_) {
            std::cerr << "[PushStreamer] No video track available" << std::endl;
            return false;
        }

        // Unified Plan 模式使用 AddTrack，AddStream 已废弃
        std::vector<libwebrtc::string> tmp;
        tmp.push_back(libwebrtc::string(config_.stream_id.c_str()));
        libwebrtc::vector<libwebrtc::string> stream_ids(tmp);
        peer_connection_->AddTrack(video_track_, stream_ids);
        ApplyVideoCodecPreferences(peer_connection_);
        ApplyEncodingParameters(peer_connection_);
        std::cout << "[PushStreamer] Video track added (stream_id=" << config_.stream_id << ")" << std::endl;

        return true;
    }

    /// 在 CreateOffer 之前调用：按 VIDEO_CODEC 将首选编码（如 H264）排到能力列表最前，影响 SDP m=video 中 PT 顺序。
    void ApplyVideoCodecPreferences(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        if (!factory_ || !pc) return;

        std::string want = config_.video_codec;
        for (auto& ch : want) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (want.empty()) want = "h264";

        auto caps = factory_->GetRtpSenderCapabilities(libwebrtc::RTCMediaType::VIDEO);
        if (!caps) {
            std::cerr << "[PushStreamer] GetRtpSenderCapabilities(VIDEO) empty, skip codec ordering" << std::endl;
            return;
        }

        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> in = caps->codecs();
        if (in.size() == 0) return;

        auto mime_lower = [](libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability> c) -> std::string {
            std::string m = c->mime_type().std_string();
            for (auto& ch : m) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return m;
        };

        auto match_want = [&](const std::string& m) -> bool {
            if (want == "h264") return m.find("h264") != std::string::npos;
            if (want == "h265" || want == "hevc") {
                return m.find("h265") != std::string::npos || m.find("hevc") != std::string::npos ||
                       m.find("hev1") != std::string::npos;
            }
            if (want == "vp8") return m.find("vp8") != std::string::npos;
            if (want == "vp9") return m.find("vp9") != std::string::npos;
            if (want == "av1") return m.find("av1") != std::string::npos;
            return m.find(want) != std::string::npos;
        };

        std::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> preferred, other;
        for (size_t i = 0; i < in.size(); ++i) {
            auto c = in[i];
            if (!c) continue;
            if (match_want(mime_lower(c))) {
                preferred.push_back(c);
            } else {
                other.push_back(c);
            }
        }

        if (preferred.empty()) {
            std::cout << "[PushStreamer] SetCodecPreferences skipped: no sender capability matching VIDEO_CODEC="
                      << config_.video_codec << std::endl;
            return;
        }

        std::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> ordered;
        ordered.reserve(preferred.size() + other.size());
        ordered.insert(ordered.end(), preferred.begin(), preferred.end());
        ordered.insert(ordered.end(), other.begin(), other.end());

        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> lw(ordered);

        auto transceivers = pc->transceivers();
        for (size_t i = 0; i < transceivers.size(); ++i) {
            auto tr = transceivers[i];
            if (!tr || tr->media_type() != libwebrtc::RTCMediaType::VIDEO) continue;
            tr->SetCodecPreferences(lw);
            std::cout << "[PushStreamer] SetCodecPreferences: prefer " << want << " (" << preferred.size()
                      << " preferred / " << ordered.size() << " total); video PT order in Offer before VP8 etc."
                      << std::endl;
            return;
        }
    }

    void ApplyEncodingParameters(
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        auto transceivers = pc->transceivers();
        for (size_t i = 0; i < transceivers.size(); ++i) {
            auto tr = transceivers[i];
            if (!tr || tr->media_type() != libwebrtc::RTCMediaType::VIDEO)
                continue;
            auto sender = tr->sender();
            if (!sender) continue;
            auto params = sender->parameters();
            if (!params) continue;
            auto encodings = params->encodings();
            for (size_t j = 0; j < encodings.size(); ++j) {
                auto enc = encodings[j];
                if (enc) {
                    enc->set_min_bitrate_bps(config_.min_bitrate_kbps * 1000);
                    enc->set_max_bitrate_bps(config_.max_bitrate_kbps * 1000);
                }
            }
            if (config_.degradation_preference == "maintain_resolution") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_RESOLUTION);
            } else if (config_.degradation_preference == "maintain_framerate") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_FRAMERATE);
            } else if (config_.degradation_preference == "balanced") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::BALANCED);
            } else {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_FRAMERATE);
            }
            if (sender->set_parameters(params)) {
                std::cout << "[PushStreamer] Encoding params applied: bitrate "
                          << config_.min_bitrate_kbps << "-" << config_.max_bitrate_kbps
                          << " kbps, degradation=" << config_.degradation_preference << std::endl;
            }
            break;
        }
    }

    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> CreatePeerConnectionWithObserver(
        libwebrtc::RTCPeerConnectionObserver* observer) {
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        if (!config_.turn_server.empty()) {
            rtc_config.ice_servers[1].uri = config_.turn_server;
            rtc_config.ice_servers[1].username = config_.turn_username;
            rtc_config.ice_servers[1].password = config_.turn_password;
        }
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_ipv6_on_wifi = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.bundle_policy = libwebrtc::kBundlePolicyMaxBundle;
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        auto pc = factory_->Create(rtc_config, constraints);
        if (!pc) return nullptr;
        pc->RegisterRTCPeerConnectionObserver(observer);
        return pc;
    }

    bool EnsurePeerConnectionForPeer(const std::string& peer_id) {
        if (peer_id.empty()) return peer_connection_ != nullptr;

        std::lock_guard<std::mutex> lock(mutex_);
        if (peer_connections_.find(peer_id) != peer_connections_.end()) return true;

        auto observer = std::make_unique<ExtraPeerObserver>(this, peer_id);
        auto pc = CreatePeerConnectionWithObserver(observer.get());
        if (!pc) {
            std::cerr << "[PushStreamer] CreatePeerConnection failed for peer=" << peer_id << std::endl;
            return false;
        }
        if (!video_track_) {
            std::cerr << "[PushStreamer] video_track is null for peer=" << peer_id << std::endl;
            return false;
        }

        std::vector<libwebrtc::string> tmp;
        tmp.push_back(libwebrtc::string(config_.stream_id.c_str()));
        libwebrtc::vector<libwebrtc::string> stream_ids(tmp);
        pc->AddTrack(video_track_, stream_ids);
        ApplyVideoCodecPreferences(pc);
        ApplyEncodingParameters(pc);
        peer_connections_[peer_id] = pc;
        extra_peer_observers_[peer_id] = std::move(observer);
        std::cout << "[PushStreamer] Subscriber PeerConnection created: " << peer_id << std::endl;
        return true;
    }

    void CreateOfferOnConnection(const std::string& peer_id,
                                 libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        constraints->AddOptionalConstraint(
            libwebrtc::RTCMediaConstraints::kNumSimulcastLayers, "1");
        if (!config_.enable_audio) {
            constraints->AddOptionalConstraint(
                libwebrtc::RTCMediaConstraints::kOfferToReceiveAudio,
                libwebrtc::RTCMediaConstraints::kValueFalse);
        }
        auto peer_id_ptr = std::make_shared<std::string>(peer_id);
        pc->CreateOffer(
            [this, peer_id_ptr, pc](const libwebrtc::string& sdp, const libwebrtc::string& type) {
                auto type_str = std::make_shared<std::string>(type.std_string());
                auto sdp_str = std::make_shared<std::string>(sdp.std_string());
                pc->SetLocalDescription(
                    sdp, type,
                    [this, peer_id_ptr, type_str, sdp_str]() {
                        if (config_.test_encode_mode && peer_id_ptr->empty()) {
                            if (std::getenv("WEBRTC_DUMP_OFFER")) {
                                std::cout << "\n--- Local offer SDP ---\n"
                                          << *sdp_str << "\n--- End ---\n" << std::flush;
                            }
                            DoLoopbackExchange(*type_str, *sdp_str);
                        } else {
                            if (std::getenv("WEBRTC_DUMP_OFFER") && on_sdp_) {
                                std::cout << "\n--- Local offer SDP (signaling peer=" << *peer_id_ptr
                                          << ") ---\n" << *sdp_str << "\n--- End ---\n" << std::flush;
                            }
                            if (on_sdp_) {
                                on_sdp_(*peer_id_ptr, *type_str, *sdp_str);
                            }
                        }
                    },
                    [](const char* err) {
                        std::cerr << "[PushStreamer] SetLocalDescription failed: " << err << std::endl;
                    });
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] CreateOffer failed: " << err << std::endl;
            },
            constraints);
    }

    bool CreateMediaTracks() {
        const std::string device_for_libwebrtc = config_.video_device_path;

        auto video_device = factory_->GetVideoDevice();
        if (!video_device) {
            std::cerr << "[PushStreamer] GetVideoDevice() null; libwebrtc may lack V4L2 or deps "
                      << "(libX11, libglib, etc.; see docs/linux_arm64_build_notes.md)" << std::endl;
            return false;
        }
        uint32_t num = video_device->NumberOfDevices();
        if (num == 0) {
            std::cerr << "[PushStreamer] libwebrtc enumerated 0 devices (NumberOfDevices=0)" << std::endl;
            auto v4l2_cams = webrtc_demo::ListUsbCameras();
            if (v4l2_cams.empty()) {
                std::cerr << "[PushStreamer] V4L2 also found no camera. Check:\n"
                          << "  1. Camera plugged in\n"
                          << "  2. Permission on /dev/video* (video group or root)\n"
                          << "  3. Run: ./webrtc_push_demo --list-cameras" << std::endl;
            } else {
                std::cerr << "[PushStreamer] V4L2 sees " << v4l2_cams.size()
                          << " device(s); libwebrtc may be incompatible with this node. "
                          << "Try: ./webrtc_push_demo livestream " << v4l2_cams[0].device_path << std::endl;
            }
            return false;
        }

        uint32_t idx = static_cast<uint32_t>(config_.video_device_index);
        const char* device_name = "camera";

        if (!device_for_libwebrtc.empty()) {
            bool found = false;
            // 先按“可视频采集节点”的顺序映射，避开同名 metadata 节点(/dev/video12 等)。
            auto v4l2_cams = webrtc_demo::ListUsbCameras();
            for (size_t i = 0; i < v4l2_cams.size(); ++i) {
                if (v4l2_cams[i].device_path == device_for_libwebrtc) {
                    idx = static_cast<uint32_t>(i);
                    device_name = device_for_libwebrtc.c_str();
                    found = true;
                    std::cout << "[PushStreamer] Using USB camera: " << config_.video_device_path
                              << " -> libwebrtc index " << idx << " (mapped by capture order)"
                              << std::endl;
                    break;
                }
            }

            if (!found) {
                // 兜底：通过 bus_info 将 V4L2 路径映射到 libwebrtc 的 device index
                std::string bus_info = webrtc_demo::GetDeviceBusInfo(device_for_libwebrtc);
                uint32_t num_devices = video_device->NumberOfDevices();
                for (uint32_t i = 0; i < num_devices; ++i) {
                    char name[256] = {0}, id[256] = {0};
                    video_device->GetDeviceName(i, name, sizeof(name), id, sizeof(id));
                    if (!bus_info.empty() && std::string(id) == bus_info) {
                        idx = i;
                        device_name = device_for_libwebrtc.c_str();
                        found = true;
                        std::cout << "[PushStreamer] Using USB camera: " << config_.video_device_path
                                  << " -> libwebrtc index " << idx << " (" << name << ")" << std::endl;
                        break;
                    }
                }
            }

            if (!found) {
                std::cerr << "[PushStreamer] No libwebrtc device matches " << device_for_libwebrtc
                          << ", using index 0" << std::endl;
                idx = 0;
                device_name = device_for_libwebrtc.c_str();
            }
        }

        uint32_t num_devices = video_device->NumberOfDevices();
        if (idx >= num_devices) {
            std::cerr << "[PushStreamer] Device index " << idx << " out of range "
                      << "(libwebrtc reports " << num_devices << " devices). "
                      << "Try --list-cameras to see V4L2 devices." << std::endl;
            return false;
        }

        video_capturer_ = video_device->Create(device_name, idx,
                                               config_.video_width, config_.video_height,
                                               config_.video_fps);
        if (!video_capturer_) {
            std::cerr << "[PushStreamer] Create video capturer failed (device index " << idx
                      << ", available: 0-" << (num_devices - 1) << ")" << std::endl;
            return false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        video_source_ = factory_->CreateVideoSource(video_capturer_, "video_source", constraints);
        if (!video_source_) {
            std::cerr << "[PushStreamer] Create video source failed" << std::endl;
            return false;
        }

        video_track_ = factory_->CreateVideoTrack(video_source_, "video_track");
        if (!video_track_) {
            std::cerr << "[PushStreamer] Create video track failed" << std::endl;
            return false;
        }

        if (video_capturer_->StartCapture()) {
            std::cout << "[PushStreamer] Video capture started" << std::endl;
        } else {
            // 某些平台的封装返回值并不可靠，继续走预热与后续观测，
            // 由真实收帧结果决定是否可用。
            std::cerr << "[PushStreamer] Video capture start returned false, continue probing"
                      << std::endl;
        }

        // 始终添加轻量帧计数渲染器，保证采集源在常规推流模式下也被稳定拉帧。
        if (!frame_counter_) {
            frame_counter_ = std::make_unique<FrameCountingRenderer>(on_frame_);
            video_track_->AddRenderer(frame_counter_.get());
        }

        // 摄像头预热：默认 3 秒，可通过 CAPTURE_WARMUP_SEC 调整（慢主板可酌情减小）。
        if (config_.capture_warmup_sec > 0) {
            std::cout << "[PushStreamer] Camera warmup " << config_.capture_warmup_sec << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(config_.capture_warmup_sec));
        } else {
            std::cout << "[PushStreamer] Camera warmup off (CAPTURE_WARMUP_SEC=0)" << std::endl;
        }

        return true;
    }

    /// 在 CreateOffer 前等待采集帧计数，避免编码器在刚连通时尚未收到帧而报 input framerate 0。
    bool WaitForCaptureGate(const std::string& context) {
        const int need = config_.capture_gate_min_frames;
        if (need <= 0) {
            return true;
        }
        if (!frame_counter_) {
            return true;
        }
        int max_wait = config_.capture_gate_max_wait_sec;
        if (max_wait < 1) {
            max_wait = 1;
        }
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::seconds(max_wait);
        std::cout << "[PushStreamer] Capture gate: need >= " << need << " frames before Offer (" << context
                  << "), max wait " << max_wait << "s" << std::endl;
        while (clock::now() < deadline) {
            if (frame_counter_->GetFrameCount() >= static_cast<unsigned int>(need)) {
                std::cout << "[PushStreamer] Capture gate OK: " << frame_counter_->GetFrameCount() << " frames ("
                          << context << ")" << std::endl;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        const unsigned got = frame_counter_->GetFrameCount();
        std::cerr << "[PushStreamer] Capture gate failed: " << got << "/" << need << " frames in " << max_wait
                  << "s; skip Offer (" << context
                  << "). Check camera in use, device node, or raise CAPTURE_GATE_MAX_WAIT_SEC" << std::endl;
        return false;
    }

    void CreateOffer() {
        if (!WaitForCaptureGate("default")) {
            return;
        }
        std::cout << "[PushStreamer] Creating default Offer..." << std::endl;
        CreateOfferOnConnection("", peer_connection_);
    }

    void CreateOfferForPeer(const std::string& peer_id) {
        if (peer_id.empty()) {
            CreateOffer();
            return;
        }
        if (!EnsurePeerConnectionForPeer(peer_id)) {
            return;
        }
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] peer not found: " << peer_id << std::endl;
            return;
        }
        if (!WaitForCaptureGate(std::string("peer=") + peer_id)) {
            return;
        }
        std::cout << "[PushStreamer] Creating Offer for subscriber: " << peer_id << std::endl;
        CreateOfferOnConnection(peer_id, pc);
    }

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        SetRemoteDescriptionForPeer("", type, sdp);
    }

    void SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type,
                                     const std::string& sdp) {
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] SetRemoteDescription: peer not found: " << peer_id << std::endl;
            return;
        }
        std::cout << "[PushStreamer] SetRemoteDescription peer=" << (peer_id.empty() ? "default" : peer_id)
                  << " type=" << type << " len=" << sdp.size() << std::endl;
        pc->SetRemoteDescription(
            libwebrtc::string(sdp.c_str()), libwebrtc::string(type.c_str()),
            []() { std::cout << "[PushStreamer] SetRemoteDescription OK" << std::endl; },
            [](const char* err) {
                std::cerr << "[PushStreamer] SetRemoteDescription failed: " << err << std::endl;
            });
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        AddRemoteIceCandidateForPeer("", mid, mline_index, candidate);
    }

    void AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid,
                                      int mline_index, const std::string& candidate) {
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] AddCandidate: peer not found: " << peer_id << std::endl;
            return;
        }
        std::cout << "[PushStreamer] AddRemoteIceCandidate peer=" << (peer_id.empty() ? "default" : peer_id)
                  << " mid=" << mid << " idx=" << mline_index << std::endl;
        pc->AddCandidate(libwebrtc::string(mid.c_str()), mline_index, libwebrtc::string(candidate.c_str()));
    }

    void DoLoopbackExchange(const std::string& offer_type, const std::string& offer_sdp) {
        std::cout << "[PushStreamer] Loopback: creating receiver for H264 validation..." << std::endl;
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        receiver_ = factory_->Create(rtc_config, constraints);
        if (!receiver_) {
            std::cerr << "[PushStreamer] Failed to create loopback receiver" << std::endl;
            return;
        }
        loopback_observer_ = std::make_unique<LoopbackReceiverObserver>(
            [this](const std::string& mid, int idx, const std::string& cand) {
                peer_connection_->AddCandidate(libwebrtc::string(mid.c_str()), idx,
                                               libwebrtc::string(cand.c_str()));
            });
        receiver_->RegisterRTCPeerConnectionObserver(loopback_observer_.get());

        libwebrtc::vector<libwebrtc::string> stream_ids;
        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpEncodingParameters>> encodings;
        auto init = libwebrtc::RTCRtpTransceiverInit::Create(
            libwebrtc::RTCRtpTransceiverDirection::kRecvOnly, stream_ids, encodings);
        receiver_->AddTransceiver(libwebrtc::RTCMediaType::VIDEO, init);

        receiver_->SetRemoteDescription(
            libwebrtc::string(offer_sdp.c_str()), libwebrtc::string(offer_type.c_str()),
            [this]() { OnReceiverRemoteDescriptionSet(); },
            [](const char* err) {
                std::cerr << "[PushStreamer] Receiver SetRemoteDescription failed: " << err << std::endl;
            });
    }

    void OnReceiverRemoteDescriptionSet() {
        auto ans_constraints = libwebrtc::RTCMediaConstraints::Create();
        if (!config_.enable_audio) {
            ans_constraints->AddOptionalConstraint(
                libwebrtc::RTCMediaConstraints::kOfferToReceiveAudio,
                libwebrtc::RTCMediaConstraints::kValueFalse);
        }
        receiver_->CreateAnswer(
            [this](const libwebrtc::string& ans_sdp, const libwebrtc::string& ans_type) {
                loopback_answer_sdp_ = ans_sdp.std_string();
                loopback_answer_type_ = ans_type.std_string();
                receiver_->SetLocalDescription(
                    ans_sdp, ans_type,
                    [this]() { OnReceiverLocalDescriptionSet(); },
                    [](const char* err) {
                        std::cerr << "[PushStreamer] Receiver SetLocalDescription failed: " << err << std::endl;
                    });
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] CreateAnswer failed: " << err << std::endl;
            },
            ans_constraints);
    }

    void OnReceiverLocalDescriptionSet() {
        peer_connection_->SetRemoteDescription(
            libwebrtc::string(loopback_answer_sdp_.c_str()),
            libwebrtc::string(loopback_answer_type_.c_str()),
            []() {
                std::cout << "[PushStreamer] Loopback SDP exchange done, waiting for ICE..." << std::endl;
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] SetRemoteDescription failed: " << err << std::endl;
            });
    }

    // RTCPeerConnectionObserver
    void NotifyConnectionState(libwebrtc::RTCPeerConnectionState state) {
        if (on_connection_state_) {
            ConnectionState cs = ConnectionState::New;
            switch (state) {
                case libwebrtc::RTCPeerConnectionStateConnecting:
                    cs = ConnectionState::Connecting;
                    break;
                case libwebrtc::RTCPeerConnectionStateConnected:
                    cs = ConnectionState::Connected;
                    break;
                case libwebrtc::RTCPeerConnectionStateDisconnected:
                    cs = ConnectionState::Disconnected;
                    break;
                case libwebrtc::RTCPeerConnectionStateFailed:
                    cs = ConnectionState::Failed;
                    break;
                case libwebrtc::RTCPeerConnectionStateClosed:
                    cs = ConnectionState::Closed;
                    break;
                default:
                    break;
            }
            on_connection_state_(cs);
        }
    }
    void OnSignalingState(libwebrtc::RTCSignalingState state) override {}
    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
        NotifyConnectionState(state);
    }
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override {
        const char* names[] = {"New", "Gathering", "Complete"};
        int idx = static_cast<int>(state);
        if (idx >= 0 && idx < 3) {
            std::cout << "[PushStreamer] ICE gathering state: " << names[idx] << std::endl;
        }
    }
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState state) override {
        const char* names[] = {"New", "Checking", "Connected", "Completed", "Failed", "Disconnected", "Closed"};
        int idx = static_cast<int>(state);
        if (idx >= 0 && idx < 7) {
            std::cout << "[PushStreamer] ICE connection state: " << names[idx] << std::endl;
        }
    }
    void OnIceCandidate(scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
        if (!candidate) return;
        if (config_.test_encode_mode && receiver_) {
            receiver_->AddCandidate(
                candidate->sdp_mid(), candidate->sdp_mline_index(), candidate->candidate());
        } else if (on_ice_candidate_) {
            on_ice_candidate_("",
                              candidate->sdp_mid().std_string(),
                              candidate->sdp_mline_index(),
                              candidate->candidate().std_string());
        }
    }
    void OnPeerIceCandidate(const std::string& peer_id,
                            scoped_refptr<libwebrtc::RTCIceCandidate> candidate) {
        if (!candidate || !on_ice_candidate_) return;
        on_ice_candidate_(peer_id,
                          candidate->sdp_mid().std_string(),
                          candidate->sdp_mline_index(),
                          candidate->candidate().std_string());
    }
    void OnAddStream(scoped_refptr<libwebrtc::RTCMediaStream> stream) override {}
    void OnRemoveStream(scoped_refptr<libwebrtc::RTCMediaStream> stream) override {}
    void OnDataChannel(scoped_refptr<libwebrtc::RTCDataChannel> data_channel) override {}
    void OnRenegotiationNeeded() override {}
    void OnTrack(scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver) override {}
    void OnAddTrack(libwebrtc::vector<scoped_refptr<libwebrtc::RTCMediaStream>> streams,
                    scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {}
    void OnRemoveTrack(scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {}

    PushStreamerConfig config_;
    scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
    scoped_refptr<libwebrtc::RTCPeerConnection> peer_connection_;
    scoped_refptr<libwebrtc::RTCMediaStream> local_stream_;
    scoped_refptr<libwebrtc::RTCVideoTrack> video_track_;
    scoped_refptr<libwebrtc::RTCVideoSource> video_source_;
    scoped_refptr<libwebrtc::RTCVideoCapturer> video_capturer_;

    OnSdpCallback on_sdp_;
    OnIceCandidateCallback on_ice_candidate_;
    OnConnectionStateCallback on_connection_state_;
    OnFrameCallback on_frame_;
    std::unique_ptr<FrameCountingRenderer> frame_counter_;
    scoped_refptr<libwebrtc::RTCPeerConnection> receiver_;
    std::unique_ptr<LoopbackReceiverObserver> loopback_observer_;
    std::string loopback_answer_sdp_;
    std::string loopback_answer_type_;
    std::unordered_map<std::string, scoped_refptr<libwebrtc::RTCPeerConnection>> peer_connections_;
    std::unordered_map<std::string, std::unique_ptr<ExtraPeerObserver>> extra_peer_observers_;
    std::mutex mutex_;
    std::mutex latency_roll_mu_;
    std::unordered_map<std::string, LatencyPeerRollState> latency_roll_;
public:
    void SetOnSdp(OnSdpCallback cb) { on_sdp_ = std::move(cb); }
    void SetOnIceCandidate(OnIceCandidateCallback cb) { on_ice_candidate_ = std::move(cb); }
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }
    void SetOnFrame(OnFrameCallback cb) { on_frame_ = std::move(cb); }
    unsigned int GetFrameCount() const {
        return frame_counter_ ? frame_counter_->GetFrameCount() : 0;
    }
    unsigned int GetDecodedFrameCount() const {
        return loopback_observer_ ? loopback_observer_->GetDecodedCount() : 0;
    }
    bool TestCaptureOnly() const { return config_.test_capture_only; }
    bool SignalingSubscriberOfferOnly() const { return config_.signaling_subscriber_offer_only; }

    void LogLatencyStatsForAllPeers(std::ostream& out) {
        std::vector<std::pair<std::string, scoped_refptr<libwebrtc::RTCPeerConnection>>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : peer_connections_) {
                copy.emplace_back(kv.first, kv.second);
            }
        }
        if (copy.empty()) {
            scoped_refptr<libwebrtc::RTCPeerConnection> pc;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pc = peer_connection_;
            }
            if (pc) {
                DumpLatencyStatsForPc(out, pc, "default", 5000);
            } else {
                using clock = std::chrono::system_clock;
                const int64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         clock::now().time_since_epoch())
                                         .count();
                out << "[stats-latency] t_ms=" << t_ms << " peer=(none) note=no_peerconnection\n" << std::flush;
            }
            return;
        }
        for (const auto& pr : copy) {
            DumpLatencyStatsForPc(out, pr.second, pr.first, 5000);
        }
    }

    void LogLatencyStatsRollingAvg(std::ostream& out, unsigned int fc) {
        const unsigned int w = static_cast<unsigned int>(config_.latency_stats_window_frames < 1
                                                             ? 1
                                                             : config_.latency_stats_window_frames);
        std::vector<std::pair<std::string, scoped_refptr<libwebrtc::RTCPeerConnection>>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : peer_connections_) {
                copy.emplace_back(kv.first, kv.second);
            }
        }
        if (copy.empty()) {
            scoped_refptr<libwebrtc::RTCPeerConnection> pc;
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
                const int64_t t_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         clock::now().time_since_epoch())
                                         .count();
                out << "[stats-latency-avg] t_ms=" << t_ms << " peer=(none) note=no_peerconnection\n"
                    << std::flush;
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
};

PushStreamer::PushStreamer(const PushStreamerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PushStreamer::~PushStreamer() {
    Stop();
}

bool PushStreamer::Start() {
    if (is_streaming_) {
        return true;
    }
    if (!impl_->Initialize()) {
        return false;
    }
    if (!impl_->TestCaptureOnly() && !impl_->SignalingSubscriberOfferOnly()) {
        impl_->CreateOffer();
    }
    is_streaming_ = true;
    return true;
}

void PushStreamer::Stop() {
    if (!is_streaming_) {
        return;
    }
    impl_->Shutdown();
    is_streaming_ = false;
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

void PushStreamer::AddRemoteIceCandidate(const std::string& mid, int mline_index,
                                         const std::string& candidate) {
    impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
}

void PushStreamer::AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid,
                                                int mline_index, const std::string& candidate) {
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
