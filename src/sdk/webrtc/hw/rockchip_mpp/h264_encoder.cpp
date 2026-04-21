// Rockchip MPP H.264 encoder for WebRTC (RK3588 等 BSP 已带 librockchip_mpp).

#define MODULE_TAG "webrtc_demo_mpp"

#include "webrtc/hw/rockchip_mpp/h264_encoder.h"

#include "webrtc/hw/rockchip_mpp/native_dec_frame_buffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "api/array_view.h"
#include "api/video/video_codec_constants.h"
#include "modules/video_coding/codecs/interface/common_constants.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_timing.h"
#include "api/video/video_frame_buffer.h"
#include "api/video_codecs/video_codec.h"
#include "common_video/h264/h264_common.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_type.h"
#include "rk_venc_cfg.h"
#include "rk_venc_cmd.h"
#include "rk_venc_rc.h"

namespace webrtc_demo {

namespace {

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

// 本地墙钟时间，毫秒精度，形如 2026-04-01 14:30:05.123
std::string CurrentLocalDateTimeYmdHmsMs() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::system_clock;

    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long long ms =
        duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000LL;
    std::tm tm_storage {};
#if defined(_WIN32)
    if (localtime_s(&tm_storage, &t) != 0) {
        return "1970-01-01 00:00:00.000";
    }
#else
    if (!localtime_r(&t, &tm_storage)) {
        return "1970-01-01 00:00:00.000";
    }
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_storage) == 0) {
        return "1970-01-01 00:00:00.000";
    }
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf, ms >= 0 ? ms : ms + 1000LL);
    return std::string(out);
}

#define MPP_ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))

// 与官方 mpi_enc_test 中 H.264 的 mdinfo_size 一致（非 HEVC 分支）：
// (ALIGN(hor_stride,64)>>6) * (ALIGN(ver_stride,16)>>4) * 16
// 旧版 RK3588 估算 (64x64 MB)*32 与 MPP 期望不一致时会导致 encode_put_frame 失败。
static size_t EncMdInfoBytesH264MpiEncTest(int hor_stride, int ver_stride) {
    return static_cast<size_t>(MPP_ALIGN(hor_stride, 64) >> 6) *
           static_cast<size_t>(MPP_ALIGN(ver_stride, 16) >> 4) * 16;
}

static bool AnnexBHasIdrNalu(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i + 3 < len) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (i + 3 < len) {
                uint8_t nal = static_cast<uint8_t>(p[i + 3] & 0x1f);
                if (nal == 5) {
                    return true;
                }
            }
            i += 3;
            continue;
        }
        if (i + 4 < len && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            if (i + 4 < len) {
                uint8_t nal = static_cast<uint8_t>(p[i + 4] & 0x1f);
                if (nal == 5) {
                    return true;
                }
            }
            i += 4;
            continue;
        }
        ++i;
    }
    return false;
}

// MPP stream_type=1 时为 4 字节大端长度 + NAL 负载；WebRTC RTP 分包需要 Annex B 起始码。
// 写入 *out 并返回是否解析成功（复用调用方 vector，避免每帧堆分配）。
static bool FillAvcLengthPrefixedToAnnexB(const uint8_t* p, size_t len, std::vector<uint8_t>* out) {
    if (!out || len < 4) {
        return false;
    }
    out->clear();
    out->reserve(len + (len / 64) * 4 + 16);
    size_t o = 0;
    while (o + 4 <= len) {
        uint32_t nsize = (static_cast<uint32_t>(p[o]) << 24) |
                         (static_cast<uint32_t>(p[o + 1]) << 16) |
                         (static_cast<uint32_t>(p[o + 2]) << 8) | static_cast<uint32_t>(p[o + 3]);
        o += 4;
        if (nsize == 0 || o + nsize > len) {
            out->clear();
            return false;
        }
        out->push_back(0);
        out->push_back(0);
        out->push_back(0);
        out->push_back(1);
        out->insert(out->end(), p + o, p + o + nsize);
        o += nsize;
    }
    if (o != len) {
        out->clear();
        return false;
    }
    return !out->empty();
}

static bool FillAvcLengthPrefixed16ToAnnexB(const uint8_t* p, size_t len, std::vector<uint8_t>* out) {
    if (!out || len < 2) {
        return false;
    }
    out->clear();
    out->reserve(len + (len / 32) * 4 + 16);
    size_t o = 0;
    while (o + 2 <= len) {
        uint16_t nsize = (static_cast<uint16_t>(p[o]) << 8) | static_cast<uint16_t>(p[o + 1]);
        o += 2;
        if (nsize == 0 || o + nsize > len) {
            out->clear();
            return false;
        }
        out->push_back(0);
        out->push_back(0);
        out->push_back(0);
        out->push_back(1);
        out->insert(out->end(), p + o, p + o + nsize);
        o += nsize;
    }
    if (o != len) {
        out->clear();
        return false;
    }
    return !out->empty();
}

// 单 NAL 单元裸流（无长度、无起始码）
static bool FillRawSingleNalToAnnexB(const uint8_t* p, size_t len, std::vector<uint8_t>* out) {
    if (!out || len < 1 || (p[0] & 0x80) != 0) {
        return false;
    }
    out->clear();
    out->reserve(4 + len);
    out->push_back(0);
    out->push_back(0);
    out->push_back(0);
    out->push_back(1);
    out->insert(out->end(), p, p + len);
    return true;
}

/// 半平面 NV12/NV21 源（共用 chroma stride）拷入 MPP 帧缓冲。
static void CopySemiPlanarToMppBuffer(const uint8_t* src_y,
                                      const uint8_t* src_uv,
                                      int src_stride_y,
                                      int src_stride_uv,
                                      uint8_t* dst_base,
                                      int hor_stride,
                                      int ver_stride,
                                      int width,
                                      int height) {
    uint8_t* dst_y = dst_base;
    uint8_t* dst_uv = dst_base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    for (int y = 0; y < height; ++y) {
        memcpy(dst_y + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
               src_y + static_cast<size_t>(y) * static_cast<size_t>(src_stride_y), static_cast<size_t>(width));
    }
    const int chroma_rows = height / 2;
    for (int y = 0; y < chroma_rows; ++y) {
        memcpy(dst_uv + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
               src_uv + static_cast<size_t>(y) * static_cast<size_t>(src_stride_uv), static_cast<size_t>(width));
    }
}

static void CopyNv12ToMppBuffer(const webrtc::NV12BufferInterface* nv12,
                                uint8_t* dst_base,
                                int hor_stride,
                                int ver_stride,
                                int width,
                                int height) {
    CopySemiPlanarToMppBuffer(nv12->DataY(), nv12->DataUV(), nv12->StrideY(), nv12->StrideUV(), dst_base, hor_stride,
                              ver_stride, width, height);
}

static int H264ProfileIdForMpp(const webrtc::VideoCodec* c) {
    (void)c;
    // SDP 侧 profile 已协商；编码器侧用 Main 作为稳妥默认（与 streams.conf H264_PROFILE 常见值一致）。
    return 77;  // MPP H.264 main profile id
}

static int ReadEnvIntInRange(const char* name, int fallback, int min_v, int max_v) {
    const char* e = std::getenv(name);
    if (!e || !e[0]) {
        return fallback;
    }
    char* end = nullptr;
    long v = std::strtol(e, &end, 10);
    if (end == e || (end && *end != '\0')) {
        return fallback;
    }
    if (v < min_v || v > max_v) {
        return fallback;
    }
    return static_cast<int>(v);
}

/// MPP JPEG 解码 NV12 输出常见按 64 水平对齐；编码器 prep 仅 16 对齐时易与解码 stride 不一致，
/// kNative 直通会退化为每帧 CopySemiPlanarToMppBuffer。默认 64；异常 BSP 可设 WEBRTC_MPP_ENC_HOR_STRIDE_ALIGN=16。
static int MppEncHorStrideAlignPixels() {
    const char* e = std::getenv("WEBRTC_MPP_ENC_HOR_STRIDE_ALIGN");
    if (!e || !e[0]) {
        return 64;
    }
    const int v = std::atoi(e);
    if (v == 16 || v == 32 || v == 64) {
        return v;
    }
    return 64;
}

void FillMppEncRcFields(MppEncCfg cfg, int target_bps, int min_bps, int max_bps, uint32_t fps) {
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", target_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", min_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", max_bps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", static_cast<RK_S32>(fps));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", static_cast<RK_S32>(fps));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
}

}  // namespace

RkMppH264Encoder::RkMppH264Encoder(const webrtc::Environment& env, webrtc::H264EncoderSettings settings)
    : env_(env), h264_settings_(settings) {}

RkMppH264Encoder::~RkMppH264Encoder() {
    Release();
}

void RkMppH264Encoder::SetFecControllerOverride(webrtc::FecControllerOverride* o) {
    (void)o;
}

void RkMppH264Encoder::DestroyMpp() {
    initialized_ = false;
    last_forced_idr_ctrl_us_ = -1;
    last_idr_emit_us_ = -1;
    consecutive_output_failures_ = 0;
    if (frm_buf_) {
        mpp_buffer_put(reinterpret_cast<MppBuffer>(frm_buf_));
        frm_buf_ = nullptr;
    }
    if (pkt_buf_) {
        mpp_buffer_put(reinterpret_cast<MppBuffer>(pkt_buf_));
        pkt_buf_ = nullptr;
    }
    if (md_buf_) {
        mpp_buffer_put(reinterpret_cast<MppBuffer>(md_buf_));
        md_buf_ = nullptr;
    }
    if (buf_grp_) {
        mpp_buffer_group_put(reinterpret_cast<MppBufferGroup>(buf_grp_));
        buf_grp_ = nullptr;
    }
    if (mpp_cfg_) {
        mpp_enc_cfg_deinit(reinterpret_cast<MppEncCfg>(mpp_cfg_));
        mpp_cfg_ = nullptr;
    }
    if (mpp_ctx_) {
        mpp_destroy(reinterpret_cast<MppCtx>(mpp_ctx_));
        mpp_ctx_ = nullptr;
        mpi_ = nullptr;
    }
    annex_scratch_.clear();
    split_assembly_buf_.clear();
}

bool RkMppH264Encoder::ApplyRcToCfg() {
    if (!mpp_cfg_ || !mpi_) {
        return false;
    }
    MppEncCfg cfg = reinterpret_cast<MppEncCfg>(mpp_cfg_);
    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);
    FillMppEncRcFields(cfg, target_bps_, min_bps_, max_bps_, fps_);
    return mpi->control(ctx, MPP_ENC_SET_CFG, cfg) == MPP_OK;
}

int RkMppH264Encoder::MppH264LevelForSize(int width, int height, uint32_t fps) {
    // H.264 Level：720p@>30、1080p@>30 需更高 level，否则硬编/码流与规范不匹配。
    if (width * height >= 1920 * 1080) {
        return fps > 30u ? 42 : 41;
    }
    if (width * height >= 1280 * 720) {
        return fps > 30u ? 42 : 40;
    }
    return 31;
}

int RkMppH264Encoder::InitEncode(const webrtc::VideoCodec* inst,
                                 const webrtc::VideoEncoder::Settings& settings) {
    (void)settings;
    if (!inst || inst->codecType != webrtc::kVideoCodecH264) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    if (inst->numberOfSimulcastStreams > 1) {
        return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
    }
    if (inst->width < 2 || inst->height < 2 || inst->maxFramerate == 0) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }

    Release();

    width_ = static_cast<int>(inst->width);
    height_ = static_cast<int>(inst->height);
    hor_stride_ = MPP_ALIGN(width_, MppEncHorStrideAlignPixels());
    ver_stride_ = MPP_ALIGN(height_, 16);
    fps_ = inst->maxFramerate > 0 ? inst->maxFramerate : 30u;

    target_bps_ = static_cast<int>(inst->startBitrate) * 1000;
    min_bps_ = static_cast<int>(inst->minBitrate) * 1000;
    max_bps_ = static_cast<int>(inst->maxBitrate) * 1000;
    if (target_bps_ <= 0) {
        target_bps_ = 2'000'000;
    }
    if (min_bps_ <= 0) {
        min_bps_ = target_bps_ / 2;
    }
    if (max_bps_ <= 0) {
        max_bps_ = target_bps_ * 2;
    }
    if (min_bps_ > max_bps_) {
        std::swap(min_bps_, max_bps_);
    }

    int ki = inst->H264().keyFrameInterval;
    if (ki <= 0) {
        ki = static_cast<int>(fps_) * 2;
    }
    if (const char* eg = std::getenv("WEBRTC_MPP_ENC_GOP")) {
        const long v = std::strtol(eg, nullptr, 10);
        if (v >= 1 && v <= 600) {
            ki = static_cast<int>(v);
        }
    }
    gop_ = ki;
    if (const char* lt = std::getenv("WEBRTC_LATENCY_TRACE"); lt && lt[0] == '1') {
        std::cout << "[Latency] MPP H264 GOP frames=" << gop_ << " fps=" << fps_ << "\n";
    }
    mpp_rc_mode_ = MPP_ENC_RC_MODE_VBR;
    // 默认打开渐进帧内刷新，降低整帧 IDR 峰值突发；可通过环境变量关闭或改策略。
    intra_refresh_mode_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_INTRA_REFRESH_MODE", 1, 0, 3);
    const int mb_rows = std::max(1, (height_ + 15) / 16);
    const int fps_i = std::max(1, static_cast<int>(fps_));
    const int auto_refresh_arg = std::max(1, (mb_rows + fps_i - 1) / fps_i);
    intra_refresh_arg_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_INTRA_REFRESH_ARG", auto_refresh_arg, 1, 512);
    // 默认关闭按字节分片；按需通过 WEBRTC_MPP_ENC_SPLIT_BYTES 打开实验。
    split_bytes_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_SPLIT_BYTES", 0, 0, 4096);
    split_by_byte_enabled_ = split_bytes_ > 0;
    // 关键帧请求风暴保护：限制连续 IDR 注入频率，但保留最长等待兜底，避免长期无法快速恢复。
    idr_min_interval_ms_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS", 800, 0, 5000);
    // 丢包场景下“快速 I 帧”窗口：当收到关键帧请求且距离上次真正 IDR 已超过该阈值时，
    // 可提前绕过 min_interval（仍受 force_max_wait 的上界兜底保护）。
    idr_loss_quick_trigger_ms_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_IDR_LOSS_QUICK_MS", 180, 0, 2000);
    idr_force_max_wait_ms_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS", 3000, 200, 15000);
    last_forced_idr_ctrl_us_ = -1;
    last_idr_emit_us_ = -1;
    consecutive_output_failures_ = 0;
    recover_soft_fail_threshold_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_RECOVER_SOFT_FAILS", 6, 1, 200);
    recover_hard_fail_threshold_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_RECOVER_HARD_FAILS", 30, 2, 500);
    recover_disable_split_on_failure_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_RECOVER_DISABLE_SPLIT", 1, 0, 1) == 1;
    debug_enabled_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_DEBUG", 0, 0, 1) == 1;
    latency_trace_enabled_ = ReadEnvIntInRange("WEBRTC_LATENCY_TRACE", 0, 0, 1) == 1;
    e2e_trace_enabled_ = ReadEnvIntInRange("WEBRTC_E2E_LATENCY_TRACE", 0, 0, 1) == 1;
    mjpeg_to_h264_trace_enabled_ = ReadEnvIntInRange("WEBRTC_MJPEG_TO_H264_TRACE", 0, 0, 1) == 1;
    use_sync_encode_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_USE_SYNC", 0, 0, 1) == 1;
    use_task_encode_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_USE_TASK", 0, 0, 1) == 1;
    task_read_packet_ = ReadEnvIntInRange("WEBRTC_MPP_ENC_TASK_READ_PACKET", 1, 0, 1) == 1;
    trace_every_n_ =
        static_cast<unsigned>(ReadEnvIntInRange("WEBRTC_MPP_ENC_TRACE_EVERY_N", 45, 1, 600));

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] mpp_create failed";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    mpp_ctx_ = ctx;
    mpi_ = mpi;

    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] mpp_init(ENC, AVC) failed";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    // 与 mpi_enc_test 一致：非分片时每帧一次 get_packet 即结束；若误用 is_eoi 当循环条件会在第二次 get_packet 永久阻塞。
    // 注意：timeout 需在 mpp_init 之后再设置，否则部分平台会忽略。
    RK_S64 output_timeout_ms = 4000;
    if (const char* ev = std::getenv("WEBRTC_MPP_ENC_OUTPUT_TIMEOUT_MS")) {
        long v = std::strtol(ev, nullptr, 10);
        if (v > 0 && v <= 8000) {
            output_timeout_ms = static_cast<RK_S64>(v);
        }
    }
    RK_S64 input_timeout_ms = 10;
    if (const char* ev = std::getenv("WEBRTC_MPP_ENC_INPUT_TIMEOUT_MS")) {
        long v = std::strtol(ev, nullptr, 10);
        if (v >= 0 && v <= 8000) {
            input_timeout_ms = static_cast<RK_S64>(v);
        }
    }
    if (mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &input_timeout_ms) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_SET_INPUT_TIMEOUT failed";
    }
    if (mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout_ms) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_SET_OUTPUT_TIMEOUT failed";
    }

    MppEncCfg cfg = nullptr;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK || !cfg) {
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    mpp_cfg_ = cfg;

    if (mpi->control(ctx, MPP_ENC_GET_CFG, cfg) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] MPP_ENC_GET_CFG failed";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", width_);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height_);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride_);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride_);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", mpp_rc_mode_);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", gop_);
    mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:super_mode", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);

    // 仅写入 cfg，不在此处 MPP_ENC_SET_CFG：避免在 h264 等参数未就绪时提前提交（会导致部分 BSP 上 put_frame 失败）。
    FillMppEncRcFields(cfg, target_bps_, min_bps_, max_bps_, fps_);
    if (mpp_rc_mode_ == MPP_ENC_RC_MODE_VBR || mpp_rc_mode_ == MPP_ENC_RC_MODE_AVBR) {
        mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
    }

    const int prof = H264ProfileIdForMpp(inst);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", prof);
    mpp_enc_cfg_set_s32(cfg, "h264:level", MppH264LevelForSize(width_, height_, fps_));
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", prof >= 100 ? 1 : 0);
    if (intra_refresh_mode_ != 0) {
        if (mpp_enc_cfg_set_s32(cfg, "h264:intra_refresh_mode", intra_refresh_mode_) != MPP_OK ||
            mpp_enc_cfg_set_s32(cfg, "h264:intra_refresh_arg", intra_refresh_arg_) != MPP_OK) {
            RTC_LOG(LS_WARNING) << "[RkMppH264] failed to apply h264 intra refresh mode="
                                << intra_refresh_mode_ << " arg=" << intra_refresh_arg_;
            intra_refresh_mode_ = 0;
            intra_refresh_arg_ = 0;
        }
    }
    // WebRTC RtpPacketizerH264 依赖 Annex B 起始码解析 NAL；MPP 默认 stream_type=1 为裸 NAL，会导致 0 包 RTP。
    if (mpp_enc_cfg_set_s32(cfg, "h264:stream_type", 0) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] h264:stream_type=0 (Annex B) not applied, RTP may fail";
    }

    if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] MPP_ENC_SET_CFG failed";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (split_by_byte_enabled_) {
        MppEncSliceSplit split_cfg {};
        split_cfg.change = MPP_ENC_SPLIT_CFG_CHANGE_MODE | MPP_ENC_SPLIT_CFG_CHANGE_ARG |
                           MPP_ENC_SPLIT_CFG_CHANGE_OUTPUT;
        split_cfg.split_mode = MPP_ENC_SPLIT_BY_BYTE;
        split_cfg.split_arg = static_cast<RK_U32>(split_bytes_);
        split_cfg.split_out = MPP_ENC_SPLIT_OUT_LOWDELAY;
        if (mpi->control(ctx, MPP_ENC_SET_SPLIT, &split_cfg) != MPP_OK) {
            RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_ENC_SET_SPLIT failed, disable split-by-byte";
            split_by_byte_enabled_ = false;
            split_bytes_ = 0;
        }
    }
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_ENC_SET_HEADER_MODE failed";
    }
    if (debug_enabled_) {
        MppPacket extra = nullptr;
        if (mpi->control(ctx, MPP_ENC_GET_EXTRA_INFO, &extra) == MPP_OK && extra) {
            std::cout << "[RkMppH264Dbg] extra_info len=" << mpp_packet_get_length(extra) << std::endl;
            mpp_packet_deinit(&extra);
        } else {
            std::cout << "[RkMppH264Dbg] extra_info unavailable" << std::endl;
        }
    }

    // 与 mpi_enc_test 一致：编码 I/O 缓冲按64 对齐后的 YUV420SP 大小（与 hor/ver 仅 16 对齐时不同）。
    const size_t enc_io_size = static_cast<size_t>(MPP_ALIGN(hor_stride_, 64)) *
                               static_cast<size_t>(MPP_ALIGN(ver_stride_, 64)) * 3 / 2;
    const size_t nv12_size = enc_io_size;
    const size_t pkt_size = enc_io_size;
    MppBuffer fb = nullptr;
    MppBuffer pb = nullptr;
    MppBuffer mb = nullptr;
    MppBufferGroup grp = nullptr;
    static const MppBufferType kBufTypes[] = {
        MPP_BUFFER_TYPE_DRM,
        static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE),
        MPP_BUFFER_TYPE_DMA_HEAP,
        MPP_BUFFER_TYPE_ION,
        MPP_BUFFER_TYPE_NORMAL,
    };
    static const MppBufferType kBufTypesNormalOnly[] = {
        MPP_BUFFER_TYPE_NORMAL,
    };
    const bool force_normal_buf = []() {
        const char* e = std::getenv("WEBRTC_MPP_ENC_FORCE_NORMAL_BUF");
        return e && e[0] == '1';
    }();
    const MppBufferType* try_buf_types = force_normal_buf ? kBufTypesNormalOnly : kBufTypes;
    const size_t try_buf_type_count = force_normal_buf ? 1 : (sizeof(kBufTypes) / sizeof(kBufTypes[0]));
    MppBufferType chosen_buf_type = MPP_BUFFER_TYPE_NORMAL;
    bool buffers_ok = false;
    for (size_t i = 0; i < try_buf_type_count; ++i) {
        MppBufferType buf_type = try_buf_types[i];
        grp = nullptr;
        fb = pb = nullptr;
        MPP_RET grp_ret = MPP_NOK;
        if (buf_type == MPP_BUFFER_TYPE_DRM) {
            grp_ret = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM);
        } else {
            grp_ret = mpp_buffer_group_get(&grp, buf_type, MPP_BUFFER_INTERNAL, MODULE_TAG, __func__);
        }
        if (grp_ret != MPP_OK || !grp) {
            continue;
        }
        if (mpp_buffer_get(grp, &fb, nv12_size) == MPP_OK &&
            mpp_buffer_get(grp, &pb, pkt_size) == MPP_OK) {
            buffers_ok = true;
            chosen_buf_type = buf_type;
            break;
        }
        if (fb) {
            mpp_buffer_put(fb);
            fb = nullptr;
        }
        if (pb) {
            mpp_buffer_put(pb);
            pb = nullptr;
        }
        mpp_buffer_group_put(grp);
        grp = nullptr;
    }
    if (!buffers_ok || !grp) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] mpp buffer alloc failed (tried drm/drm+cache/dma_heap/ion/normal)";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    buf_grp_ = grp;
    frm_buf_ = fb;
    pkt_buf_ = pb;
    if (debug_enabled_) {
        std::cout << "[RkMppH264Dbg] buffer type=" << static_cast<int>(chosen_buf_type)
                  << " frm_fd=" << mpp_buffer_get_fd(reinterpret_cast<MppBuffer>(frm_buf_))
                  << " pkt_fd=" << mpp_buffer_get_fd(reinterpret_cast<MppBuffer>(pkt_buf_))
                  << " nv12_size=" << nv12_size << " pkt_size=" << pkt_size << std::endl;
    }

    const size_t md_sz = EncMdInfoBytesH264MpiEncTest(hor_stride_, ver_stride_);
    if (md_sz > 0 && mpp_buffer_get(grp, &mb, md_sz) == MPP_OK) {
        md_buf_ = mb;
    }

    initialized_ = true;
    // 每帧单调递增的 VideoFrameTrackingId（16bit，经 RTP 扩展带到对端）。从 500 起跳，避免与
    // VideoFrame::kNotSetId==0 混淆；InitEncode 时重置，便于与单次采集日志对齐。
    // 接收端 MPP 解码器把 EncodedImage::VideoFrameTrackingId 写入 VideoFrame::id，[E2E_RX] 的
    // trace_id 与之相同；parse_e2e_latency.py 按 trace_id 配对。[E2E_TX]/[E2E_RX] 的 wall_utc_ms
    // 为 TimeUTCMillis，两台 PC 需 chrony/NTP 同步后差值才表示真实端到端（毫秒）。
    next_video_frame_tracking_id_ = 500;
    RTC_LOG(LS_INFO) << "[RkMppH264] InitEncode ok " << width_ << "x" << height_ << "@" << fps_
                     << "fps stride=" << hor_stride_ << "x" << ver_stride_
                     << " bps=" << target_bps_ << " intra_refresh=" << intra_refresh_mode_ << ":"
                     << intra_refresh_arg_ << " split_bytes=" << split_bytes_
                     << " idr_min_interval_ms=" << idr_min_interval_ms_
                     << " idr_loss_quick_ms=" << idr_loss_quick_trigger_ms_
                     << " idr_force_max_wait_ms=" << idr_force_max_wait_ms_;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t RkMppH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t RkMppH264Encoder::Release() {
    DestroyMpp();
    return WEBRTC_VIDEO_CODEC_OK;
}

void RkMppH264Encoder::SetRates(const webrtc::VideoEncoder::RateControlParameters& parameters) {
    if (!initialized_) {
        return;
    }
    const uint32_t sum = parameters.bitrate.get_sum_bps();
    if (sum > 0) {
        target_bps_ = static_cast<int>(sum);
    }
    if (parameters.framerate_fps > 0.0) {
        fps_ = static_cast<uint32_t>(parameters.framerate_fps + 0.5);
        if (fps_ < 1) {
            fps_ = 1;
        }
    }
    min_bps_ = std::max(10'000, target_bps_ * 3 / 4);
    max_bps_ = std::max(target_bps_, min_bps_) * 4 / 3;
    if (debug_enabled_) {
        std::cout << "[RkMppH264Dbg] SetRates sum_bps=" << sum << " target_bps=" << target_bps_
                  << " min_bps=" << min_bps_ << " max_bps=" << max_bps_
                  << " fps=" << parameters.framerate_fps << std::endl;
    }
    ApplyRcToCfg();
}

webrtc::VideoEncoder::EncoderInfo RkMppH264Encoder::GetEncoderInfo() const {
    webrtc::VideoEncoder::EncoderInfo info;
    info.supports_native_handle = true;
    info.implementation_name = "rockchip_mpp_h264";
    info.has_trusted_rate_controller = false;
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    // kNative：MPP MJPEG 解码帧直通硬编；NV12/I420 仍为平面路径。
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNative,
                                    webrtc::VideoFrameBuffer::Type::kNV12,
                                    webrtc::VideoFrameBuffer::Type::kI420};
    return info;
}

int32_t RkMppH264Encoder::EmitAssembledFrame(const webrtc::VideoFrame& frame,
                                               const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& vfb,
                                               int64_t encode_before_us,
                                               int64_t on_frame_to_encode_enter_us,
                                               bool mpp_reports_intra) {
    if (split_assembly_buf_.empty()) {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    const uint8_t* raw = split_assembly_buf_.data();
    const size_t len = split_assembly_buf_.size();
    webrtc::scoped_refptr<webrtc::EncodedImageBuffer> buf;
    if (!webrtc::H264::FindNaluIndices(webrtc::ArrayView<const uint8_t>(raw, len)).empty()) {
        buf = webrtc::EncodedImageBuffer::Create(len);
        memcpy(buf->data(), raw, len);
    } else {
        bool annex_ok = FillAvcLengthPrefixedToAnnexB(raw, len, &annex_scratch_);
        if (!annex_ok) {
            annex_ok = FillAvcLengthPrefixed16ToAnnexB(raw, len, &annex_scratch_);
        }
        if (!annex_ok) {
            annex_ok = FillRawSingleNalToAnnexB(raw, len, &annex_scratch_);
        }
        if (!annex_ok) {
            RTC_LOG(LS_WARNING) << "[RkMppH264] unparseable bitstream (assembled), request SW fallback; len=" << len;
            std::cerr << "[RkMppH264Err] unparseable assembled bitstream len=" << len
                      << ", request SW fallback" << std::endl;
            return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
        }
        buf = webrtc::EncodedImageBuffer::Create(annex_scratch_.size());
        memcpy(buf->data(), annex_scratch_.data(), annex_scratch_.size());
    }
    webrtc::H264BitstreamParser qp_parser;
    webrtc::EncodedImage encoded;
    encoded.SetEncodedData(buf);
    encoded._encodedWidth = width_;
    encoded._encodedHeight = height_;
    encoded.SetRtpTimestamp(frame.rtp_timestamp());
    encoded.SetColorSpace(frame.color_space());
    encoded.capture_time_ms_ = frame.render_time_ms();
    const int64_t encode_finish_us = webrtc::TimeMicros();
    encoded.SetEncodeTime(encode_before_us / webrtc::kNumMicrosecsPerMillisec,
                          encode_finish_us / webrtc::kNumMicrosecsPerMillisec);
    encoded.video_timing_mutable()->flags = webrtc::VideoSendTiming::kNotTriggered;

    if (latency_trace_enabled_) {
        static std::atomic<unsigned> enc_lat_n{0};
        const unsigned n = ++enc_lat_n;
        if ((n % 30u) == 0u) {
            const double ms = static_cast<double>(encode_finish_us - encode_before_us) / 1000.0;
            std::cout << "[Latency] MPP H264 encode put+get ms=" << ms << " sample#" << n << std::endl;
        }
    }

    if (mpp_reports_intra) {
        encoded._frameType = webrtc::VideoFrameType::kVideoFrameKey;
    } else if (AnnexBHasIdrNalu(buf->data(), buf->size())) {
        encoded._frameType = webrtc::VideoFrameType::kVideoFrameKey;
    } else {
        encoded._frameType = webrtc::VideoFrameType::kVideoFrameDelta;
    }
    if (encoded._frameType == webrtc::VideoFrameType::kVideoFrameKey) {
        last_idr_emit_us_ = webrtc::TimeMicros();
    }

    qp_parser.ParseBitstream(encoded);
    encoded.qp_ = qp_parser.GetLastSliceQp().value_or(-1);

    const uint32_t trace_tid = next_video_frame_tracking_id_;
    const bool trace_periodic_log = (trace_tid % trace_every_n_ == 0u);
    encoded.SetVideoFrameTrackingId(trace_tid);
    ++next_video_frame_tracking_id_;
    webrtc::CodecSpecificInfo specifics{};
    specifics.codecType = webrtc::kVideoCodecH264;
    specifics.codecSpecific.H264.packetization_mode = h264_settings_.packetization_mode;
    specifics.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
    specifics.codecSpecific.H264.base_layer_sync = false;
    specifics.codecSpecific.H264.idr_frame =
        (encoded._frameType == webrtc::VideoFrameType::kVideoFrameKey);
    const int64_t before_on_encoded_cb_us = webrtc::TimeMicros();
    webrtc::EncodedImageCallback::Result res = callback_->OnEncodedImage(encoded, &specifics);
    const int64_t after_on_encoded_cb_us = webrtc::TimeMicros();
    const int64_t webrtc_onencodedimage_us = after_on_encoded_cb_us - before_on_encoded_cb_us;
    if (MediaTimingTraceEnabled()) {
        static std::atomic<unsigned> media_trace_n{0};
        const unsigned n = ++media_trace_n;
        if ((n % MediaTimingTraceEveryN()) == 0u) {
            std::cout << "[MEDIA_TIMING][tx] t_us=" << after_on_encoded_cb_us << " trace_id="
                      << static_cast<unsigned>(trace_tid) << " rtp_ts=" << encoded.RtpTimestamp()
                      << " t_frame_ts_us=" << frame.timestamp_us() << " t_encode_done_us=" << encode_finish_us
                      << " t_after_onencoded_us=" << after_on_encoded_cb_us
                      << " onencoded_cb_cost_us=" << webrtc_onencodedimage_us << std::endl;
        }
    }
    if (debug_enabled_) {
        static std::atomic<unsigned> enc_cb_n{0};
        const unsigned n = ++enc_cb_n;
        if ((n <= 5u) || ((n % 60u) == 0u)) {
            std::cout << "[RkMppH264Dbg] OnEncodedImage #" << n << " size=" << encoded.size()
                      << " type=" << (encoded._frameType == webrtc::VideoFrameType::kVideoFrameKey ? "key" : "delta")
                      << " cb_error=" << static_cast<int>(res.error) << std::endl;
        }
    }
    if (res.error != webrtc::EncodedImageCallback::Result::OK) {
        std::cerr << "[RkMppH264Err] OnEncodedImage callback error=" << static_cast<int>(res.error) << std::endl;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (e2e_trace_enabled_) {
        int64_t t_mjpeg_input_us = frame.timestamp_us();
        int64_t t_v4l2_us = -1;
        int64_t t_on_frame_us = -1;
        int64_t wall_utc_ms = -1;
        if (MppNativeDecFrameBuffer* e2e_native = MppNativeDecFrameBuffer::TryGet(vfb)) {
            const int64_t mjpeg_us = e2e_native->mjpeg_input_timestamp_us();
            if (mjpeg_us > 0) {
                t_mjpeg_input_us = mjpeg_us;
            }
            t_v4l2_us = e2e_native->v4l2_timestamp_us();
            t_on_frame_us = e2e_native->on_frame_enter_us();
            wall_utc_ms = e2e_native->wall_capture_utc_ms();
        }
        std::cout << "[E2E_TX] rtp_ts=" << encoded.RtpTimestamp() << " trace_id=" << static_cast<unsigned>(trace_tid)
                  << " t_mjpeg_input_us=" << t_mjpeg_input_us << " t_v4l2_us=" << t_v4l2_us
                  << " t_on_frame_us=" << t_on_frame_us << " t_enc_done_us=" << encode_finish_us
                  << " t_after_onencoded_us=" << after_on_encoded_cb_us << " wall_utc_ms=" << wall_utc_ms << std::endl;
    }
    if (trace_periodic_log) {
        int64_t mjpeg_input_to_encode_done_us = encode_finish_us - frame.timestamp_us();
        int64_t mjpeg_input_to_after_onencoded_us = after_on_encoded_cb_us - frame.timestamp_us();
        int64_t usb_to_frame_timestamp_us = -1;
        int64_t decode_queue_wait_us = -1;
        if (MppNativeDecFrameBuffer* native_fb = MppNativeDecFrameBuffer::TryGet(vfb)) {
            const int64_t mjpeg_input_us = native_fb->mjpeg_input_timestamp_us();
            if (mjpeg_input_us > 0) {
                mjpeg_input_to_encode_done_us = encode_finish_us - mjpeg_input_us;
                mjpeg_input_to_after_onencoded_us = after_on_encoded_cb_us - mjpeg_input_us;
            }
            const int64_t v4l2_timestamp_us = native_fb->v4l2_timestamp_us();
            if (v4l2_timestamp_us > 0) {
                usb_to_frame_timestamp_us = frame.timestamp_us() - v4l2_timestamp_us;
            }
            decode_queue_wait_us = native_fb->decode_queue_wait_us();
        }
        std::cout << "[" << CurrentLocalDateTimeYmdHmsMs() << "]: current_video_frame_tracking_id_=" << trace_tid
                  << ", mjpeg_input_to_encode_done_us=" << mjpeg_input_to_encode_done_us << " ("
                  << (static_cast<double>(mjpeg_input_to_encode_done_us) / 1000.0) << " ms)"
                  << ", mjpeg_input_to_after_onencoded_us=" << mjpeg_input_to_after_onencoded_us << " ("
                  << (static_cast<double>(mjpeg_input_to_after_onencoded_us) / 1000.0) << " ms)"
                  << ", webrtc_onencodedimage_us=" << webrtc_onencodedimage_us << " ("
                  << (static_cast<double>(webrtc_onencodedimage_us) / 1000.0) << " ms)"
                  << ", usb_to_frame_timestamp_us=" << usb_to_frame_timestamp_us;
        if (usb_to_frame_timestamp_us >= 0) {
            std::cout << " (" << (static_cast<double>(usb_to_frame_timestamp_us) / 1000.0) << " ms)";
        }
        std::cout << ", decode_queue_wait_us=" << decode_queue_wait_us << " ("
                  << (static_cast<double>(decode_queue_wait_us) / 1000.0) << " ms)"
                  << ", on_frame_to_encode_enter_us=" << on_frame_to_encode_enter_us;
        if (on_frame_to_encode_enter_us >= 0) {
            std::cout << " (" << (static_cast<double>(on_frame_to_encode_enter_us) / 1000.0) << " ms)";
        }
        std::cout << std::endl;
    }
    if (mjpeg_to_h264_trace_enabled_) {
        static std::atomic<unsigned> g_pipe_n{0};
        const unsigned pn = ++g_pipe_n;
        if (pn % 30u == 0u) {
            const int64_t delta_us = encode_finish_us - frame.timestamp_us();
            std::cout << "[Pipe MJPEG→H264] frame#" << pn << " v4l2_mjpeg_process_start_to_h264_ready_us=" << delta_us
                      << " (" << (static_cast<double>(delta_us) / 1000.0) << " ms)" << std::endl;
        }
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

bool RkMppH264Encoder::HandleOutputFailureAndMaybeRecover(const char* stage, int err_code) {
    ++consecutive_output_failures_;
    RTC_LOG(LS_WARNING) << "[RkMppH264] output failure stage=" << stage << " err=" << err_code
                        << " streak=" << consecutive_output_failures_;
    if (recover_disable_split_on_failure_ && split_by_byte_enabled_ &&
        consecutive_output_failures_ >= recover_soft_fail_threshold_) {
        split_by_byte_enabled_ = false;
        split_bytes_ = 0;
        RTC_LOG(LS_WARNING) << "[RkMppH264] auto-disable split-by-byte after failure streak="
                            << consecutive_output_failures_;
    }
    if (consecutive_output_failures_ >= recover_hard_fail_threshold_) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] failure streak reached hard threshold="
                          << recover_hard_fail_threshold_ << ", abort encode session";
        return false;
    }
    return true;
}

int32_t RkMppH264Encoder::Encode(const webrtc::VideoFrame& frame,
                                 const std::vector<webrtc::VideoFrameType>* frame_types) {
    if (!initialized_ || !callback_ || !mpi_ || !mpp_ctx_) {
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (debug_enabled_) {
        static std::atomic<unsigned> enc_call_n{0};
        const unsigned n = ++enc_call_n;
        if ((n <= 5u) || ((n % 60u) == 0u)) {
            std::cout << "[RkMppH264Dbg] Encode called #" << n << " ts_us=" << frame.timestamp_us()
                      << " rtp_ts=" << frame.rtp_timestamp() << std::endl;
        }
    }

    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);

    uint8_t* dst = static_cast<uint8_t*>(mpp_buffer_get_ptr(reinterpret_cast<MppBuffer>(frm_buf_)));
    if (!dst) {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> vfb = frame.video_frame_buffer();
    const int64_t encode_enter_us = webrtc::TimeMicros();
    int64_t on_frame_to_encode_enter_us = -1;
    if (MppNativeDecFrameBuffer* nfb = MppNativeDecFrameBuffer::TryGet(vfb)) {
        const int64_t on_frame_us = nfb->on_frame_enter_us();
        if (on_frame_us > 0 && encode_enter_us >= on_frame_us) {
            on_frame_to_encode_enter_us = encode_enter_us - on_frame_us;
        }
    }
    MppBuffer input_mpp_buf = reinterpret_cast<MppBuffer>(frm_buf_);

    if (vfb->type() == webrtc::VideoFrameBuffer::Type::kNative) {
        MppNativeDecFrameBuffer* native = MppNativeDecFrameBuffer::TryGet(vfb);
        if (native) {
            MppBuffer ext = reinterpret_cast<MppBuffer>(native->mpp_buffer_handle());
            if (!ext) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            const auto* src_base = static_cast<const uint8_t*>(mpp_buffer_get_ptr(ext));
            if (!src_base) {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            const RK_U32 fmt = static_cast<RK_U32>(native->mpp_fmt());
            const int nhs = native->hor_stride();
            const int nvs = native->ver_stride();
            const uint8_t* src_y = src_base;
            const uint8_t* src_uv = src_base + static_cast<size_t>(nhs) * static_cast<size_t>(nvs);
            const bool dims_ok = (native->width() == width_ && native->height() == height_);
            const bool stride_ok = (nhs == hor_stride_ && nvs == ver_stride_);
            const bool nv12_mpp = (fmt == MPP_FMT_YUV420SP);
            if (dims_ok && stride_ok && nv12_mpp) {
                input_mpp_buf = ext;
            } else if (dims_ok && (nv12_mpp || fmt == MPP_FMT_YUV420SP_VU)) {
                if (fmt == MPP_FMT_YUV420SP) {
                    CopySemiPlanarToMppBuffer(src_y, src_uv, nhs, nhs, dst, hor_stride_, ver_stride_, width_, height_);
                } else {
                    uint8_t* dst_uv = dst + static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
                    if (libyuv::NV21ToNV12(src_y, nhs, src_uv, nhs, dst, hor_stride_, dst_uv, hor_stride_, width_,
                                           height_) != 0) {
                        return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
                    }
                }
            } else {
                RTC_LOG(LS_WARNING) << "[RkMppH264] native dec frame mismatch expect " << width_ << "x" << height_
                                    << " stride " << hor_stride_ << "x" << ver_stride_ << " fmt " << fmt << " got "
                                    << native->width() << "x" << native->height() << " stride " << nhs << "x" << nvs;
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
        } else {
            webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = vfb->ToI420();
            if (!i420) {
                return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
            }
            if (i420->width() != width_ || i420->height() != height_) {
                RTC_LOG(LS_WARNING) << "[RkMppH264] frame size mismatch expect " << width_ << "x" << height_
                                    << " got " << i420->width() << "x" << i420->height();
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            uint8_t* dst_uv = dst + static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
            libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                               i420->StrideV(), dst, hor_stride_, dst_uv, hor_stride_, width_, height_);
        }
    } else if (vfb->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
        const webrtc::NV12BufferInterface* nv12 = vfb->GetNV12();
        if (nv12->width() != width_ || nv12->height() != height_) {
            RTC_LOG(LS_WARNING) << "[RkMppH264] NV12 frame size mismatch expect " << width_ << "x" << height_
                                << " got " << nv12->width() << "x" << nv12->height();
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        CopyNv12ToMppBuffer(nv12, dst, hor_stride_, ver_stride_, width_, height_);
    } else {
        webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 = vfb->ToI420();
        if (!i420) {
            return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
        }
        if (i420->width() != width_ || i420->height() != height_) {
            RTC_LOG(LS_WARNING) << "[RkMppH264] frame size mismatch expect " << width_ << "x" << height_
                                << " got " << i420->width() << "x" << i420->height();
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        uint8_t* dst_uv = dst + static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
        libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(), i420->DataV(),
                           i420->StrideV(), dst, hor_stride_, dst_uv, hor_stride_, width_, height_);
    }

    bool want_key = false;
    if (frame_types) {
        for (webrtc::VideoFrameType t : *frame_types) {
            if (t == webrtc::VideoFrameType::kVideoFrameKey) {
                want_key = true;
                break;
            }
        }
    }
    if (want_key) {
        bool allow_force_idr = true;
        const int64_t now_us = webrtc::TimeMicros();
        const int64_t min_interval_us = static_cast<int64_t>(std::max(0, idr_min_interval_ms_)) * 1000;
        const int64_t loss_quick_us = static_cast<int64_t>(std::max(0, idr_loss_quick_trigger_ms_)) * 1000;
        const int64_t force_max_wait_us = static_cast<int64_t>(std::max(1, idr_force_max_wait_ms_)) * 1000;
        if (min_interval_us > 0 && last_forced_idr_ctrl_us_ > 0 && now_us > last_forced_idr_ctrl_us_ &&
            (now_us - last_forced_idr_ctrl_us_) < min_interval_us) {
            allow_force_idr = false;
            if (loss_quick_us > 0 && last_idr_emit_us_ > 0 && now_us > last_idr_emit_us_ &&
                (now_us - last_idr_emit_us_) >= loss_quick_us) {
                allow_force_idr = true;
            }
            if (last_idr_emit_us_ > 0 && now_us > last_idr_emit_us_ &&
                (now_us - last_idr_emit_us_) >= force_max_wait_us) {
                allow_force_idr = true;
            }
        }
        if (allow_force_idr) {
            if (mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, nullptr) == MPP_OK) {
                last_forced_idr_ctrl_us_ = now_us;
            } else {
                RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_ENC_SET_IDR_FRAME failed";
            }
        }
    }

    MppFrame mframe = nullptr;
    if (mpp_frame_init(&mframe) != MPP_OK) {
        return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    mpp_frame_set_width(mframe, static_cast<RK_U32>(width_));
    mpp_frame_set_height(mframe, static_cast<RK_U32>(height_));
    mpp_frame_set_hor_stride(mframe, static_cast<RK_U32>(hor_stride_));
    mpp_frame_set_ver_stride(mframe, static_cast<RK_U32>(ver_stride_));
    mpp_frame_set_fmt(mframe, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(mframe, input_mpp_buf);
    mpp_frame_set_pts(mframe, static_cast<RK_S64>(frame.timestamp_us()));

    MppMeta meta = mpp_frame_get_meta(mframe);
    MppPacket prebound_pkt = nullptr;
    if (meta) {
        if (md_buf_) {
            mpp_meta_set_buffer(meta, KEY_MOTION_INFO, reinterpret_cast<MppBuffer>(md_buf_));
        }
    }

    const int64_t encode_before_us = webrtc::TimeMicros();
    const bool use_sync_encode = use_sync_encode_;
    const bool use_task_encode = use_task_encode_;
    if (debug_enabled_) {
        std::cout << "[RkMppH264Dbg] before "
                  << (use_task_encode ? "task_encode" : (use_sync_encode ? "encode" : "encode_put_frame"))
                  << " ts_us=" << frame.timestamp_us() << std::endl;
        std::cout << "[RkMppH264Dbg] frame fmt=" << static_cast<int>(mpp_frame_get_fmt(mframe))
                  << " hor_stride=" << mpp_frame_get_hor_stride(mframe)
                  << " ver_stride=" << mpp_frame_get_ver_stride(mframe)
                  << " input_fd=" << mpp_buffer_get_fd(input_mpp_buf) << std::endl;
    }
    MPP_RET ret = MPP_NOK;
    MppPacket first_out_pkt = nullptr;
    MppTask output_task = nullptr;
    auto recover_or_error = [&](const char* stage, int err_code) -> int32_t {
        split_assembly_buf_.clear();
        if (prebound_pkt) {
            mpp_packet_deinit(&prebound_pkt);
        }
        if (output_task) {
            mpi->enqueue(ctx, MPP_PORT_OUTPUT, output_task);
            output_task = nullptr;
        }
        return HandleOutputFailureAndMaybeRecover(stage, err_code) ? WEBRTC_VIDEO_CODEC_OK
                                                                    : WEBRTC_VIDEO_CODEC_ERROR;
    };
    if (use_task_encode) {
        MppTask input_task = nullptr;
        ret = mpi->poll(ctx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] poll input ret=" << ret << std::endl;
        }
        if (ret != MPP_OK) {
            mpp_frame_deinit(&mframe);
            std::cerr << "[RkMppH264Err] poll input ret=" << ret << std::endl;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        ret = mpi->dequeue(ctx, MPP_PORT_INPUT, &input_task);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] dequeue input ret=" << ret << " task=" << (input_task ? 1 : 0) << std::endl;
        }
        if (ret != MPP_OK || !input_task) {
            mpp_frame_deinit(&mframe);
            std::cerr << "[RkMppH264Err] dequeue input ret=" << ret << std::endl;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        mpp_task_meta_set_frame(input_task, KEY_INPUT_FRAME, mframe);
        if (md_buf_ && !use_task_encode) {
            mpp_task_meta_set_buffer(input_task, KEY_MOTION_INFO, reinterpret_cast<MppBuffer>(md_buf_));
        }
        ret = mpi->enqueue(ctx, MPP_PORT_INPUT, input_task);
        mpp_frame_deinit(&mframe);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] enqueue input ret=" << ret << std::endl;
        }
        if (ret != MPP_OK) {
            std::cerr << "[RkMppH264Err] enqueue input ret=" << ret << std::endl;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] poll output(task) ret=" << ret << std::endl;
        }
        if (ret != MPP_OK) {
            std::cerr << "[RkMppH264Err] poll output(task) ret=" << ret << std::endl;
            return recover_or_error("task_poll_output", ret);
        }
        ret = mpi->dequeue(ctx, MPP_PORT_OUTPUT, &output_task);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] dequeue output ret=" << ret << " task=" << (output_task ? 1 : 0)
                      << std::endl;
        }
        if (ret != MPP_OK || !output_task) {
            std::cerr << "[RkMppH264Err] dequeue output(task) ret=" << ret << std::endl;
            return recover_or_error("task_dequeue_output", ret);
        }
        // task 模式默认必须读取 KEY_OUTPUT_PACKET；关闭仅用于定位兼容性问题。
        const bool task_read_packet = task_read_packet_;
        if (task_read_packet) {
            ret = mpp_task_meta_get_packet(output_task, KEY_OUTPUT_PACKET, &first_out_pkt);
            if (debug_enabled_) {
                std::cout << "[RkMppH264Dbg] get output packet ret=" << ret
                          << " pkt=" << (first_out_pkt ? 1 : 0) << std::endl;
            }
            if (ret != MPP_OK || !first_out_pkt) {
                std::cerr << "[RkMppH264Err] task output has no packet ret=" << ret
                          << " pkt=" << (first_out_pkt ? 1 : 0) << std::endl;
                return recover_or_error("task_get_packet", ret);
            }
        } else if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] task output dequeued; packet read skipped by env" << std::endl;
        }
    } else if (use_sync_encode) {
        ret = mpi->encode(ctx, mframe, &first_out_pkt);
        mpp_frame_deinit(&mframe);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] after encode ret=" << ret
                      << " first_pkt=" << (first_out_pkt ? 1 : 0) << std::endl;
        }
        if (ret != MPP_OK) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode ret=" << ret;
            std::cerr << "[RkMppH264Err] encode ret=" << ret << std::endl;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    } else {
        if (meta && pkt_buf_) {
            if (mpp_packet_init_with_buffer(&prebound_pkt, reinterpret_cast<MppBuffer>(pkt_buf_)) == MPP_OK &&
                prebound_pkt) {
                // 跟官方 mpi_enc_test 一致：绑定输出包前必须清零长度。
                mpp_packet_set_length(prebound_pkt, 0);
                mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, prebound_pkt);
            }
        }
        ret = mpi->encode_put_frame(ctx, mframe);
        if (ret != MPP_OK && input_mpp_buf != reinterpret_cast<MppBuffer>(frm_buf_)) {
            MppNativeDecFrameBuffer* native_fb = MppNativeDecFrameBuffer::TryGet(vfb);
            if (native_fb) {
                MppBuffer ext = reinterpret_cast<MppBuffer>(native_fb->mpp_buffer_handle());
                const auto* src_base = ext ? static_cast<const uint8_t*>(mpp_buffer_get_ptr(ext)) : nullptr;
                if (src_base) {
                    const RK_U32 fmt = static_cast<RK_U32>(native_fb->mpp_fmt());
                    const int nhs = native_fb->hor_stride();
                    const int nvs = native_fb->ver_stride();
                    const uint8_t* src_y = src_base;
                    const uint8_t* src_uv = src_base + static_cast<size_t>(nhs) * static_cast<size_t>(nvs);
                    RTC_LOG(LS_WARNING) << "[RkMppH264] zero-copy encode_put_frame failed ret=" << ret
                                        << ", retry with memcpy to encoder buffer";
                    if (fmt == MPP_FMT_YUV420SP) {
                        CopySemiPlanarToMppBuffer(src_y, src_uv, nhs, nhs, dst, hor_stride_, ver_stride_, width_, height_);
                    } else if (fmt == MPP_FMT_YUV420SP_VU) {
                        uint8_t* dst_uv = dst + static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
                        libyuv::NV21ToNV12(src_y, nhs, src_uv, nhs, dst, hor_stride_, dst_uv, hor_stride_, width_, height_);
                    }
                    mpp_frame_set_buffer(mframe, reinterpret_cast<MppBuffer>(frm_buf_));
                    ret = mpi->encode_put_frame(ctx, mframe);
                }
            }
        }
        mpp_frame_deinit(&mframe);
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] after encode_put_frame ret=" << ret << std::endl;
        }
        if (ret != MPP_OK) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_put_frame ret=" << ret;
            std::cerr << "[RkMppH264Err] encode_put_frame ret=" << ret << std::endl;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    // mpi_enc_test：非分片时每帧通常一包（is_part==0 即 EOI）；分片模式下多包需拼满再 OnEncodedImage。
    split_assembly_buf_.clear();
    const int max_pkt_iterations = split_by_byte_enabled_ ? 2048 : 64;
    bool frame_output_done = false;
    bool mpp_intra_hint = false;
    int safety = 0;
    do {
        if (debug_enabled_) {
            static std::atomic<unsigned> get_pkt_n{0};
            const unsigned n = ++get_pkt_n;
            if ((n <= 5u) || ((n % 60u) == 0u)) {
                std::cout << "[RkMppH264Dbg] encode_get_packet try #" << n << std::endl;
            }
        }
        if ((use_sync_encode || use_task_encode) && !first_out_pkt) {
            MPP_RET poll_ret = mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_NON_BLOCK);
            if (debug_enabled_) {
                std::cout << "[RkMppH264Dbg] poll output ret=" << poll_ret << std::endl;
            }
        }
        MppPacket out_pkt = nullptr;
        if (first_out_pkt) {
            out_pkt = first_out_pkt;
            first_out_pkt = nullptr;
            ret = MPP_OK;
        } else {
            ret = mpi->encode_get_packet(ctx, &out_pkt);
        }
        if (out_pkt && prebound_pkt && out_pkt == prebound_pkt) {
            prebound_pkt = nullptr;
        }
        if (ret == MPP_ERR_TIMEOUT) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet timeout (check WEBRTC_MPP_ENC_OUTPUT_TIMEOUT_MS)";
            std::cerr << "[RkMppH264Err] encode_get_packet timeout; try WEBRTC_MPP_ENC_OUTPUT_TIMEOUT_MS"
                      << std::endl;
            return recover_or_error("encode_get_packet_timeout", ret);
        }
        if (ret != MPP_OK) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet ret=" << ret;
            std::cerr << "[RkMppH264Err] encode_get_packet ret=" << ret << std::endl;
            return recover_or_error("encode_get_packet_ret", ret);
        }
        if (!out_pkt) {
            break;
        }
        const RK_U32 is_part = mpp_packet_is_partition(out_pkt);
        const bool pkt_eoi = (is_part == 0) || (mpp_packet_is_eoi(out_pkt) != 0);
        {
            RK_S32 intra = 0;
            if (mpp_packet_has_meta(out_pkt) &&
                mpp_meta_get_s32(mpp_packet_get_meta(out_pkt), KEY_OUTPUT_INTRA, &intra) == MPP_OK && intra) {
                mpp_intra_hint = true;
            }
        }
        const size_t len = mpp_packet_get_length(out_pkt);
        void* pos = mpp_packet_get_pos(out_pkt);
        if (len > 0 && pos) {
            const uint8_t* raw = static_cast<const uint8_t*>(pos);
            split_assembly_buf_.insert(split_assembly_buf_.end(), raw, raw + len);
        }
        mpp_packet_deinit(&out_pkt);
        if (pkt_eoi) {
            if (split_assembly_buf_.empty()) {
                RTC_LOG(LS_ERROR) << "[RkMppH264] encoder EOI but assembly buffer empty";
                return recover_or_error("empty_eoi", -1);
            }
            const int32_t emit_ret =
                EmitAssembledFrame(frame, vfb, encode_before_us, on_frame_to_encode_enter_us, mpp_intra_hint);
            if (emit_ret != WEBRTC_VIDEO_CODEC_OK) {
                return emit_ret;
            }
            split_assembly_buf_.clear();
            mpp_intra_hint = false;
            frame_output_done = true;
        }
        if (++safety > max_pkt_iterations) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet exceeded safety iterations";
            return recover_or_error("encode_get_packet_safety", safety);
        }
    } while (!frame_output_done);
    if (!frame_output_done) {
        split_assembly_buf_.clear();
        RTC_LOG(LS_ERROR) << "[RkMppH264] encoder output finished without EOI (no complete frame)";
        return recover_or_error("no_eoi_frame", -1);
    }
    if (prebound_pkt) {
        mpp_packet_deinit(&prebound_pkt);
    }
    if (output_task) {
        if (debug_enabled_) {
            std::cout << "[RkMppH264Dbg] enqueue output task back" << std::endl;
        }
        mpi->enqueue(ctx, MPP_PORT_OUTPUT, output_task);
    }
    consecutive_output_failures_ = 0;
    return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace webrtc_demo
