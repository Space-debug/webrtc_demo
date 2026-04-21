// Rockchip MPP H.264 硬件解码 backend（拉流接收端）。

#include "webrtc/hw/rockchip_mpp/h264_decoder.h"

#include <cstring>
#include <cstdlib>
#include <atomic>
#include <iostream>

#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_type.h"

namespace webrtc_demo::hw::rockchip_mpp {

namespace {

static bool LowLatencyEnv() {
    const char* e = std::getenv("WEBRTC_MPP_H264_DEC_LOW_LATENCY");
    return e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
}

int DecodePollTimeoutMs() {
    // 低时延默认缩短到 1ms，减少「收齐包后等待下一次 poll」的调度量化延迟。
    int def = LowLatencyEnv() ? 1 : 5;
    if (const char* e = std::getenv("WEBRTC_MPP_H264_DEC_POLL_TIMEOUT_MS")) {
        const int v = std::atoi(e);
        if (v >= 0 && v <= 20) {
            return v;
        }
    }
    return def;
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

}  // namespace

H264Decoder::H264Decoder(const webrtc::Environment& env) : env_(env) {
    (void)env_;
}

H264Decoder::~H264Decoder() {
    Release();
}

void H264Decoder::DestroyMpp() {
    if (mpp_ctx_) {
        MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
        mpp_destroy(ctx);
        mpp_ctx_ = nullptr;
        mpi_ = nullptr;
    }
}

bool H264Decoder::EnsureMppInitialized() {
    if (mpp_ctx_) {
        return true;
    }
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        RTC_LOG(LS_ERROR) << "[RkMppH264Dec] mpp_create failed";
        return false;
    }
    mpp_ctx_ = ctx;
    mpi_ = mpi;

    if (LowLatencyEnv()) {
        RK_U32 fast = 1;
        if (mpi->control(ctx, MPP_DEC_SET_PARSER_FAST_MODE, &fast) != MPP_OK) {
            RTC_LOG(LS_WARNING) << "[RkMppH264Dec] MPP_DEC_SET_PARSER_FAST_MODE failed";
        }
    }

    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        RTC_LOG(LS_ERROR) << "[RkMppH264Dec] mpp_init(DEC, AVC) failed";
        DestroyMpp();
        return false;
    }

    if (LowLatencyEnv()) {
        RK_U32 imm = 1;
        if (mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &imm) != MPP_OK) {
            RTC_LOG(LS_WARNING) << "[RkMppH264Dec] MPP_DEC_SET_IMMEDIATE_OUT failed";
        }
    }

    if (!logged_init_) {
        logged_init_ = true;
        std::cout << "[RkMppH264Dec] Initialized (hardware) low_latency=" << (LowLatencyEnv() ? 1 : 0) << std::endl;
    }
    return true;
}

bool H264Decoder::Configure(const webrtc::VideoDecoder::Settings& settings) {
    if (settings.codec_type() != webrtc::kVideoCodecH264) {
        return false;
    }
    return EnsureMppInitialized();
}

int32_t H264Decoder::Release() {
    callback_ = nullptr;
    DestroyMpp();
    bitstream_copy_.clear();
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

void H264Decoder::DiscardPendingOutput() {
    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);
    for (int i = 0; i < 128; ++i) {
        mpi->poll(ctx, MPP_PORT_OUTPUT, MPP_POLL_NON_BLOCK);
        MppFrame frame = nullptr;
        MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
        if (ret == MPP_ERR_TIMEOUT) {
            break;
        }
        if (!frame) {
            break;
        }
        if (mpp_frame_get_info_change(frame)) {
            mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        }
        mpp_frame_deinit(&frame);
    }
}

bool H264Decoder::TryDrainOneDecodedFrame(int64_t render_time_ms, const webrtc::EncodedImage& ref_meta) {
    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);

    for (int attempt = 0; attempt < 80; ++attempt) {
        mpi->poll(ctx, MPP_PORT_OUTPUT, static_cast<MppPollType>(DecodePollTimeoutMs()));
        MppFrame frame = nullptr;
        MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
        if (ret == MPP_ERR_TIMEOUT) {
            continue;
        }
        if (!frame) {
            return false;
        }
        if (mpp_frame_get_info_change(frame)) {
            if (mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr) != MPP_OK) {
                RTC_LOG(LS_WARNING) << "[RkMppH264Dec] MPP_DEC_SET_INFO_CHANGE_READY failed";
            }
            mpp_frame_deinit(&frame);
            continue;
        }
        if (mpp_frame_get_errinfo(frame) != 0) {
            mpp_frame_deinit(&frame);
            continue;
        }
        MppBuffer mb = mpp_frame_get_buffer(frame);
        if (!mb) {
            mpp_frame_deinit(&frame);
            continue;
        }

        const RK_U32 fmt = mpp_frame_get_fmt(frame);
        if (fmt != static_cast<RK_U32>(MPP_FMT_YUV420SP) && fmt != static_cast<RK_U32>(MPP_FMT_YUV420SP_VU)) {
            RTC_LOG(LS_WARNING) << "[RkMppH264Dec] unexpected frame fmt=" << fmt;
            mpp_frame_deinit(&frame);
            continue;
        }

        const int width = static_cast<int>(mpp_frame_get_width(frame));
        const int height = static_cast<int>(mpp_frame_get_height(frame));
        const int hor_stride = static_cast<int>(mpp_frame_get_hor_stride(frame));
        const int ver_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
        if (width <= 0 || height <= 0 || hor_stride <= 0 || ver_stride <= 0) {
            mpp_frame_deinit(&frame);
            continue;
        }

        uint8_t* base = static_cast<uint8_t*>(mpp_buffer_get_ptr(mb));
        if (!base) {
            mpp_frame_deinit(&frame);
            continue;
        }
        const uint8_t* src_y = base;
        const uint8_t* src_uv = base + static_cast<size_t>(hor_stride) * static_cast<size_t>(ver_stride);

        webrtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(width, height);
        if (fmt == static_cast<RK_U32>(MPP_FMT_YUV420SP)) {
            if (libyuv::NV12ToI420(src_y, hor_stride, src_uv, hor_stride, i420->MutableDataY(), i420->StrideY(),
                                   i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(), i420->StrideV(),
                                   width, height) != 0) {
                mpp_frame_deinit(&frame);
                continue;
            }
        } else {
            if (libyuv::NV21ToI420(src_y, hor_stride, src_uv, hor_stride, i420->MutableDataY(), i420->StrideY(),
                                   i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(), i420->StrideV(),
                                   width, height) != 0) {
                mpp_frame_deinit(&frame);
                continue;
            }
        }

        int64_t ts_us = 0;
        if (render_time_ms >= 0) {
            ts_us = render_time_ms * webrtc::kNumMicrosecsPerMillisec;
        } else if (ref_meta.capture_time_ms_ >= 0) {
            ts_us = ref_meta.capture_time_ms_ * webrtc::kNumMicrosecsPerMillisec;
        }

        webrtc::VideoFrame::Builder frame_builder;
        frame_builder.set_video_frame_buffer(i420).set_rtp_timestamp(ref_meta.RtpTimestamp()).set_timestamp_us(ts_us);
        if (const auto tid = ref_meta.VideoFrameTrackingId(); tid.has_value() && *tid != webrtc::VideoFrame::kNotSetId) {
            frame_builder.set_id(*tid);
        }
        webrtc::VideoFrame out = frame_builder.build();

        mpp_frame_deinit(&frame);

        if (callback_) {
            if (!logged_first_frame_) {
                logged_first_frame_ = true;
                std::cout << "[RkMppH264Dec] Decoded first frame " << width << "x" << height << " fmt=" << fmt
                          << std::endl;
            }
            if (MediaTimingTraceEnabled()) {
                static std::atomic<unsigned> media_trace_n{0};
                const unsigned n = ++media_trace_n;
                if ((n % MediaTimingTraceEveryN()) == 0u) {
                    const auto tid = ref_meta.VideoFrameTrackingId();
                    std::cout << "[MEDIA_TIMING][dec] t_us=" << webrtc::TimeMicros()
                              << " trace_id="
                              << (tid.has_value() ? std::to_string(static_cast<unsigned>(*tid))
                                                  : std::string("-"))
                              << " rtp_ts=" << ref_meta.RtpTimestamp() << " width=" << width
                              << " height=" << height << std::endl;
                }
            }
            callback_->Decoded(out);
        }
        return true;
    }
    return false;
}

int32_t H264Decoder::Decode(const webrtc::EncodedImage& input_image, bool /*missing_frames*/, int64_t render_time_ms) {
    if (!callback_) {
        return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (!EnsureMppInitialized()) {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    const size_t len = input_image.size();
    if (len == 0) {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    const uint8_t* src = input_image.data();
    if (!src) {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    bitstream_copy_.resize(len + 64);
    memcpy(bitstream_copy_.data(), src, len);
    memset(bitstream_copy_.data() + len, 0, 64);

    MppCtx ctx = reinterpret_cast<MppCtx>(mpp_ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);

    MppPacket packet = nullptr;
    if (mpp_packet_init(&packet, bitstream_copy_.data(), bitstream_copy_.size()) != MPP_OK || !packet) {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    mpp_packet_set_length(packet, len);

    MPP_RET pr = mpi->decode_put_packet(ctx, packet);
    if (pr == MPP_ERR_BUFFER_FULL) {
        DiscardPendingOutput();
        pr = mpi->decode_put_packet(ctx, packet);
    }
    mpp_packet_deinit(&packet);

    if (pr != MPP_OK) {
        RTC_LOG(LS_WARNING) << "[RkMppH264Dec] decode_put_packet ret=" << pr;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (MediaTimingTraceEnabled()) {
        static std::atomic<unsigned> media_ingress_n{0};
        const unsigned n = ++media_ingress_n;
        if ((n % MediaTimingTraceEveryN()) == 0u) {
            const auto tid = input_image.VideoFrameTrackingId();
            std::cout << "[MEDIA_TIMING][rx] t_us=" << webrtc::TimeMicros()
                      << " trace_id="
                      << (tid.has_value() ? std::to_string(static_cast<unsigned>(*tid))
                                          : std::string("-"))
                      << " rtp_ts=" << input_image.RtpTimestamp()
                      << " enc_bytes=" << input_image.size() << " event=decode_put_packet_ok" << std::endl;
        }
    }

    while (TryDrainOneDecodedFrame(render_time_ms, input_image)) {
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

webrtc::VideoDecoder::DecoderInfo H264Decoder::GetDecoderInfo() const {
    webrtc::VideoDecoder::DecoderInfo info;
    info.implementation_name = "rockchip_mpp_h264";
    info.is_hardware_accelerated = true;
    return info;
}

}  // namespace webrtc_demo::hw::rockchip_mpp

