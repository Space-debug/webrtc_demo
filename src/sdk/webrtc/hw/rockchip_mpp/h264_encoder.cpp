// Rockchip MPP H.264 encoder for WebRTC (RK3588 等 BSP 已带 librockchip_mpp).

#define MODULE_TAG "webrtc_demo_mpp"

#include "webrtc/hw/rockchip_mpp/h264_encoder.h"

#include "webrtc/hw/rockchip_mpp/native_dec_frame_buffer.h"

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
#include "rk_mpi.h"
#include "rk_type.h"
#include "rk_venc_cfg.h"
#include "rk_venc_rc.h"

namespace webrtc_demo {

namespace {

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

static size_t EncMdInfoBytesRk3588Style(int width, int height) {
    const int w = MPP_ALIGN(width, 64);
    const int h = MPP_ALIGN(height, 64);
    return static_cast<size_t>((w >> 6) * (h >> 6) * 32);
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
}

bool RkMppH264Encoder::ApplyRcToCfg() {
    if (!mpp_cfg_ || !mpi_) {
        return false;
    }
    MppEncCfg cfg = reinterpret_cast<MppEncCfg>(mpp_cfg_);
    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", target_bps_);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", min_bps_);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", max_bps_);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", static_cast<RK_S32>(fps_));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", static_cast<RK_S32>(fps_));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
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
    hor_stride_ = MPP_ALIGN(width_, 16);
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

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] mpp_create failed";
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    mpp_ctx_ = ctx;
    mpi_ = mpi;
    // 与 mpi_enc_test 一致：非分片时每帧一次 get_packet 即结束；若误用 is_eoi 当循环条件会在第二次 get_packet 永久阻塞。
    RK_S64 output_timeout_ms = 4000;
    if (const char* ev = std::getenv("WEBRTC_MPP_ENC_OUTPUT_TIMEOUT_MS")) {
        long v = std::strtol(ev, nullptr, 10);
        if (v > 0 && v <= 8000) {
            output_timeout_ms = static_cast<RK_S64>(v);
        }
    }
    if (mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &output_timeout_ms) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] MPP_SET_OUTPUT_TIMEOUT failed";
    }

    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] mpp_init(ENC, AVC) failed";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_ERROR;
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

    ApplyRcToCfg();

    const int prof = H264ProfileIdForMpp(inst);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", prof);
    mpp_enc_cfg_set_s32(cfg, "h264:level", MppH264LevelForSize(width_, height_, fps_));
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", prof >= 100 ? 1 : 0);
    // WebRTC RtpPacketizerH264 依赖 Annex B 起始码解析 NAL；MPP 默认 stream_type=1 为裸 NAL，会导致 0 包 RTP。
    if (mpp_enc_cfg_set_s32(cfg, "h264:stream_type", 0) != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264] h264:stream_type=0 (Annex B) not applied, RTP may fail";
    }

    if (mpi->control(ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264] MPP_ENC_SET_CFG failed";
        DestroyMpp();
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    const size_t nv12_size = static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_) * 3 / 2;
    const size_t pkt_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3 / 2;
    MppBuffer fb = nullptr;
    MppBuffer pb = nullptr;
    MppBuffer mb = nullptr;
    MppBufferGroup grp = nullptr;
    // 编码器需 DMA 内存；组创建或 frm/pkt get 失败时换下一类 buffer type。
    static const MppBufferType kBufTypes[] = {
        MPP_BUFFER_TYPE_DRM,
        static_cast<MppBufferType>(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE),
        MPP_BUFFER_TYPE_DMA_HEAP,
        MPP_BUFFER_TYPE_ION,
        MPP_BUFFER_TYPE_NORMAL,
    };
    bool buffers_ok = false;
    for (MppBufferType buf_type : kBufTypes) {
        grp = nullptr;
        fb = pb = nullptr;
        if (mpp_buffer_group_get(&grp, buf_type, MPP_BUFFER_INTERNAL, MODULE_TAG, __func__) != MPP_OK ||
            !grp) {
            continue;
        }
        if (mpp_buffer_get(grp, &fb, nv12_size) == MPP_OK &&
            mpp_buffer_get(grp, &pb, pkt_size) == MPP_OK) {
            buffers_ok = true;
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

    const size_t md_sz = EncMdInfoBytesRk3588Style(width_, height_);
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
                     << "fps bps=" << target_bps_;
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

int32_t RkMppH264Encoder::Encode(const webrtc::VideoFrame& frame,
                                 const std::vector<webrtc::VideoFrameType>* frame_types) {
    if (!initialized_ || !callback_ || !mpi_ || !mpp_ctx_) {
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
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
        mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
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

    MppPacket packet = nullptr;
    if (mpp_packet_init_with_buffer(&packet, reinterpret_cast<MppBuffer>(pkt_buf_)) != MPP_OK) {
        mpp_frame_deinit(&mframe);
        return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    mpp_packet_set_length(packet, 0);

    MppMeta meta = mpp_frame_get_meta(mframe);
    if (meta) {
        mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);
        if (md_buf_) {
            mpp_meta_set_buffer(meta, KEY_MOTION_INFO, reinterpret_cast<MppBuffer>(md_buf_));
        }
    }

    const int64_t encode_before_us = webrtc::TimeMicros();
    MPP_RET ret = mpi->encode_put_frame(ctx, mframe);
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
    if (ret != MPP_OK) {
        mpp_packet_deinit(&packet);
        RTC_LOG(LS_ERROR) << "[RkMppH264] encode_put_frame ret=" << ret;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    webrtc::H264BitstreamParser qp_parser;
    // mpi_enc_test：非分片码流每帧通常一包，外层 eoi 初值为 1，仅分片模式用 mpp_packet_is_eoi 拼帧。
    bool frame_output_done = false;
    int safety = 0;
    do {
        MppPacket out_pkt = nullptr;
        ret = mpi->encode_get_packet(ctx, &out_pkt);
        if (ret == MPP_ERR_TIMEOUT) {
            mpp_packet_deinit(&packet);
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet timeout (check WEBRTC_MPP_ENC_OUTPUT_TIMEOUT_MS)";
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (ret != MPP_OK) {
            mpp_packet_deinit(&packet);
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet ret=" << ret;
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        if (!out_pkt) {
            break;
        }
        const RK_U32 is_part = mpp_packet_is_partition(out_pkt);
        const bool pkt_eoi = (is_part == 0) || (mpp_packet_is_eoi(out_pkt) != 0);
        const size_t len = mpp_packet_get_length(out_pkt);
        void* pos = mpp_packet_get_pos(out_pkt);
        if (len > 0 && pos) {
            const uint8_t* raw = static_cast<const uint8_t*>(pos);
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
                    mpp_packet_deinit(&out_pkt);
                    mpp_packet_deinit(&packet);
                    RTC_LOG(LS_WARNING) << "[RkMppH264] unparseable bitstream, request SW fallback; len="
                                        << len;
                    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
                }
                buf = webrtc::EncodedImageBuffer::Create(annex_scratch_.size());
                memcpy(buf->data(), annex_scratch_.data(), annex_scratch_.size());
            }
            webrtc::EncodedImage encoded;
            encoded.SetEncodedData(buf);
            encoded._encodedWidth = width_;
            encoded._encodedHeight = height_;
            encoded.SetRtpTimestamp(frame.rtp_timestamp());
            encoded.SetColorSpace(frame.color_space());
            encoded.capture_time_ms_ = frame.render_time_ms();
            // 与 VideoFrame::timestamp_us（同 webrtc::TimeMicros）对齐，供 video-timing 扩展算 delta。
            const int64_t encode_finish_us = webrtc::TimeMicros();
            encoded.SetEncodeTime(encode_before_us / webrtc::kNumMicrosecsPerMillisec,
                                  encode_finish_us / webrtc::kNumMicrosecsPerMillisec);
            encoded.video_timing_mutable()->flags = webrtc::VideoSendTiming::kNotTriggered;

            if (const char* lt = std::getenv("WEBRTC_LATENCY_TRACE"); lt && lt[0] == '1') {
                static std::atomic<unsigned> enc_lat_n{0};
                const unsigned n = ++enc_lat_n;
                if ((n % 30u) == 0u) {
                    const double ms = static_cast<double>(encode_finish_us - encode_before_us) / 1000.0;
                    std::cout << "[Latency] MPP H264 encode put+get ms=" << ms << " sample#" << n << std::endl;
                }
            }

            RK_S32 intra = 0;
            if (mpp_packet_has_meta(out_pkt) &&
                mpp_meta_get_s32(mpp_packet_get_meta(out_pkt), KEY_OUTPUT_INTRA, &intra) == MPP_OK &&
                intra) {
                encoded._frameType = webrtc::VideoFrameType::kVideoFrameKey;
            } else if (AnnexBHasIdrNalu(buf->data(), buf->size())) {
                encoded._frameType = webrtc::VideoFrameType::kVideoFrameKey;
            } else {
                encoded._frameType = webrtc::VideoFrameType::kVideoFrameDelta;
            }

            qp_parser.ParseBitstream(encoded);
            encoded.qp_ = qp_parser.GetLastSliceQp().value_or(-1);

            const uint32_t trace_tid = next_video_frame_tracking_id_;
            unsigned trace_every_n = 45u;
            if (const char* ev = std::getenv("WEBRTC_MPP_ENC_TRACE_EVERY_N")) {
                const int v = std::atoi(ev);
                if (v >= 1 && v <= 600) {
                    trace_every_n = static_cast<unsigned>(v);
                }
            }
            const bool trace_periodic_log = (trace_tid % trace_every_n == 0u);
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
            webrtc::EncodedImageCallback::Result res =
                callback_->OnEncodedImage(encoded, &specifics);
            const int64_t after_on_encoded_cb_us = webrtc::TimeMicros();
            const int64_t webrtc_onencodedimage_us = after_on_encoded_cb_us - before_on_encoded_cb_us;
            if (res.error != webrtc::EncodedImageCallback::Result::OK) {
                mpp_packet_deinit(&out_pkt);
                mpp_packet_deinit(&packet);
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            if (const char* e2e = std::getenv("WEBRTC_E2E_LATENCY_TRACE"); e2e && e2e[0] == '1') {
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
                std::cout << "[E2E_TX] rtp_ts=" << encoded.RtpTimestamp() << " trace_id="
                          << static_cast<unsigned>(trace_tid) << " t_mjpeg_input_us=" << t_mjpeg_input_us
                          << " t_v4l2_us=" << t_v4l2_us << " t_on_frame_us=" << t_on_frame_us
                          << " t_enc_done_us=" << encode_finish_us
                          << " t_after_onencoded_us=" << after_on_encoded_cb_us << " wall_utc_ms=" << wall_utc_ms
                          << std::endl;
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
                std::cout << "[" << CurrentLocalDateTimeYmdHmsMs() << "]: current_video_frame_tracking_id_="
                          << trace_tid
                          << ", mjpeg_input_to_encode_done_us=" << mjpeg_input_to_encode_done_us
                          << " (" << (static_cast<double>(mjpeg_input_to_encode_done_us) / 1000.0) << " ms)"
                          << ", mjpeg_input_to_after_onencoded_us=" << mjpeg_input_to_after_onencoded_us
                          << " (" << (static_cast<double>(mjpeg_input_to_after_onencoded_us) / 1000.0) << " ms)"
                          << ", webrtc_onencodedimage_us=" << webrtc_onencodedimage_us
                          << " (" << (static_cast<double>(webrtc_onencodedimage_us) / 1000.0) << " ms)"
                          << ", usb_to_frame_timestamp_us=" << usb_to_frame_timestamp_us;
                if (usb_to_frame_timestamp_us >= 0) {
                    std::cout << " (" << (static_cast<double>(usb_to_frame_timestamp_us) / 1000.0) << " ms)";
                }
                std::cout << ", decode_queue_wait_us=" << decode_queue_wait_us
                          << " (" << (static_cast<double>(decode_queue_wait_us) / 1000.0) << " ms)"
                          << ", on_frame_to_encode_enter_us=" << on_frame_to_encode_enter_us;
                if (on_frame_to_encode_enter_us >= 0) {
                    std::cout << " (" << (static_cast<double>(on_frame_to_encode_enter_us) / 1000.0) << " ms)";
                }
                std::cout << std::endl;
            }
            if (const char* ev = std::getenv("WEBRTC_MJPEG_TO_H264_TRACE")) {
                if (ev[0] != '0') {
                    static std::atomic<unsigned> g_pipe_n{0};
                    const unsigned pn = ++g_pipe_n;
                    if (pn % 30u == 0u) {
                        const int64_t delta_us = encode_finish_us - frame.timestamp_us();
                        std::cout << "[Pipe MJPEG→H264] frame#" << pn
                                  << " v4l2_mjpeg_process_start_to_h264_ready_us=" << delta_us << " ("
                                  << (static_cast<double>(delta_us) / 1000.0) << " ms)" << std::endl;
                    }
                }
            }
        }
        mpp_packet_deinit(&out_pkt);
        if (pkt_eoi) {
            frame_output_done = true;
        }
        if (++safety > 64) {
            RTC_LOG(LS_ERROR) << "[RkMppH264] encode_get_packet exceeded safety iterations";
            mpp_packet_deinit(&packet);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    } while (!frame_output_done);

    mpp_packet_deinit(&packet);
    return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace webrtc_demo
