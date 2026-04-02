// Rockchip MPP H.264 encoder for WebRTC (RK3588 等 BSP 已带 librockchip_mpp).

#define MODULE_TAG "webrtc_demo_mpp"

#include "webrtc/rk_mpp_h264_encoder.h"

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
static std::vector<uint8_t> AvcLengthPrefixedToAnnexB(const uint8_t* p, size_t len) {
    std::vector<uint8_t> out;
    if (len < 4) {
        return out;
    }
    out.reserve(len + (len / 64) * 4 + 16);
    size_t o = 0;
    while (o + 4 <= len) {
        uint32_t nsize = (static_cast<uint32_t>(p[o]) << 24) |
                         (static_cast<uint32_t>(p[o + 1]) << 16) |
                         (static_cast<uint32_t>(p[o + 2]) << 8) | static_cast<uint32_t>(p[o + 3]);
        o += 4;
        if (nsize == 0 || o + nsize > len) {
            out.clear();
            return out;
        }
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), p + o, p + o + nsize);
        o += nsize;
    }
    if (o != len) {
        out.clear();
    }
    return out;
}

static std::vector<uint8_t> AvcLengthPrefixed16ToAnnexB(const uint8_t* p, size_t len) {
    std::vector<uint8_t> out;
    if (len < 2) {
        return out;
    }
    out.reserve(len + (len / 32) * 4 + 16);
    size_t o = 0;
    while (o + 2 <= len) {
        uint16_t nsize = (static_cast<uint16_t>(p[o]) << 8) | static_cast<uint16_t>(p[o + 1]);
        o += 2;
        if (nsize == 0 || o + nsize > len) {
            out.clear();
            return out;
        }
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(1);
        out.insert(out.end(), p + o, p + o + nsize);
        o += nsize;
    }
    if (o != len) {
        out.clear();
    }
    return out;
}

// 单 NAL 单元裸流（无长度、无起始码）
static std::vector<uint8_t> RawSingleNalToAnnexB(const uint8_t* p, size_t len) {
    std::vector<uint8_t> out;
    if (len < 1 || (p[0] & 0x80) != 0) {
        return out;
    }
    out.reserve(4 + len);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), p, p + len);
    return out;
}

/// 将 WebRTC NV12（独立 stride）拷入 MPP 帧缓冲（hor_stride×ver_stride，与 I420ToNV12 布局一致）。
static void CopyNv12ToMppBuffer(const webrtc::NV12BufferInterface* nv12,
                                uint8_t* dst_base,
                                int hor_stride,
                                int ver_stride,
                                int width,
                                int height) {
    const uint8_t* src_y = nv12->DataY();
    const uint8_t* src_uv = nv12->DataUV();
    const int sy = nv12->StrideY();
    const int suv = nv12->StrideUV();
    uint8_t* dst_y = dst_base;
    uint8_t* dst_uv = dst_base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);
    for (int y = 0; y < height; ++y) {
        memcpy(dst_y + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
               src_y + static_cast<size_t>(y) * static_cast<size_t>(sy), static_cast<size_t>(width));
    }
    const int chroma_rows = height / 2;
    for (int y = 0; y < chroma_rows; ++y) {
        memcpy(dst_uv + static_cast<size_t>(y) * static_cast<size_t>(hor_stride),
               src_uv + static_cast<size_t>(y) * static_cast<size_t>(suv), static_cast<size_t>(width));
    }
}

static int H264ProfileIdForMpp(const webrtc::VideoCodec* c) {
    (void)c;
    // SDP 侧 profile 已协商；编码器侧用 Main 作为稳妥默认（与 streams.conf H264_PROFILE 常见值一致）。
    return 77;  // MPP H.264 main profile id
}

// 与 camera_video_track_source MJPEG 解码日志一致：TimeMicros 前后戳 + duration_ms；第 1～5 帧及每 30 帧打印。
static void LogMppH264EncodeTiming(int64_t before_us, int64_t after_us) {
    static std::atomic<unsigned> g_n{0};
    const unsigned n = ++g_n;
    if ((n % 30) != 0) {
        return;
    }
    const double ms = static_cast<double>(after_us - before_us) / 1000.0;
    std::cout << "[H264 encode mpp] frame#" << n << " before_us=" << before_us << " after_us=" << after_us
              << " duration_ms=" << ms << std::endl;
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
    (void)fps;
    if (width * height >= 1920 * 1080) {
        return 41;
    }
    if (width * height >= 1280 * 720) {
        return 40;
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
    gop_ = ki;
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
    info.supports_native_handle = false;
    info.implementation_name = "rockchip_mpp_h264";
    info.has_trusted_rate_controller = false;
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    // 空列表时栈默认只喂 I420；声明 NV12 可避免采集 NV12 时在编码前被转成 I420。
    info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNV12,
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
    if (vfb->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
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
    mpp_frame_set_buffer(mframe, reinterpret_cast<MppBuffer>(frm_buf_));
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
                std::vector<uint8_t> annex = AvcLengthPrefixedToAnnexB(raw, len);
                if (annex.empty()) {
                    annex = AvcLengthPrefixed16ToAnnexB(raw, len);
                }
                if (annex.empty()) {
                    annex = RawSingleNalToAnnexB(raw, len);
                }
                if (annex.empty()) {
                    mpp_packet_deinit(&out_pkt);
                    mpp_packet_deinit(&packet);
                    RTC_LOG(LS_WARNING) << "[RkMppH264] unparseable bitstream, request SW fallback; len="
                                        << len;
                    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
                }
                buf = webrtc::EncodedImageBuffer::Create(annex.size());
                memcpy(buf->data(), annex.data(), annex.size());
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

            const uint32_t rtp_ts = frame.rtp_timestamp();
            const uint32_t rtp_over_3000 = rtp_ts / 3000u;
            if (rtp_over_3000 % 120u == 0u) {
                const int64_t encode_duration_ms =
                    (encode_finish_us - encode_before_us) / webrtc::kNumMicrosecsPerMillisec;
                std::cout << "[" << CurrentLocalDateTimeYmdHmsMs() << "]: encode_before_ms="
                          << (encode_before_us / webrtc::kNumMicrosecsPerMillisec)
                          << ", encode_duration_ms=" << encode_duration_ms
                          << ", encode_finish_ms=" << (encode_finish_us / webrtc::kNumMicrosecsPerMillisec)
                          << ", render_time_ms=" << frame.render_time_ms() << ", rtp_timestamp=" << rtp_ts
                          << ", rtp_timestamp/3000=" << rtp_over_3000 << std::endl;
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

            webrtc::CodecSpecificInfo specifics{};
            specifics.codecType = webrtc::kVideoCodecH264;
            specifics.codecSpecific.H264.packetization_mode = h264_settings_.packetization_mode;
            specifics.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
            specifics.codecSpecific.H264.base_layer_sync = false;
            specifics.codecSpecific.H264.idr_frame =
                (encoded._frameType == webrtc::VideoFrameType::kVideoFrameKey);

            webrtc::EncodedImageCallback::Result res =
                callback_->OnEncodedImage(encoded, &specifics);
            if (res.error != webrtc::EncodedImageCallback::Result::OK) {
                mpp_packet_deinit(&out_pkt);
                mpp_packet_deinit(&packet);
                return WEBRTC_VIDEO_CODEC_ERROR;
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

    LogMppH264EncodeTiming(encode_before_us, webrtc::TimeMicros());
    mpp_packet_deinit(&packet);
    return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace webrtc_demo
