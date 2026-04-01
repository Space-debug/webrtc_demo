#define MODULE_TAG "webrtc_mpp_mjpeg"

#include "webrtc/rk_mpp_mjpeg_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "third_party/libyuv/include/libyuv/convert.h"
#include "third_party/libyuv/include/libyuv/planar_functions.h"

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_type.h"
#include "rk_vdec_cfg.h"

#define MPP_ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))

namespace webrtc_demo {

namespace {

static bool MjpegDecTraceEnabled() {
    static const char* k = std::getenv("WEBRTC_MJPEG_DEC_TRACE");
    return k && k[0] != '0';
}

static constexpr int kMppBufferGroupIndex = 1;

/// MPP 帧（NV12/NV21）写入 WebRTC NV12Buffer（逐行拷贝，避免整块 stride 假设错误）
static bool CopyMppSemiPlanarToNv12(RK_U32 fmt,
                                    const uint8_t* src_y,
                                    const uint8_t* src_uv,
                                    int src_stride,
                                    int width,
                                    int height,
                                    webrtc::NV12Buffer* out) {
    if (!out || width <= 0 || height <= 0) {
        return false;
    }
    uint8_t* dst_y = out->MutableDataY();
    uint8_t* dst_uv = out->MutableDataUV();
    const int dst_sy = out->StrideY();
    const int dst_suv = out->StrideUV();
    if (fmt == MPP_FMT_YUV420SP) {
        for (int r = 0; r < height; ++r) {
            std::memcpy(dst_y + r * dst_sy, src_y + r * src_stride, static_cast<size_t>(width));
        }
        for (int r = 0; r < height / 2; ++r) {
            std::memcpy(dst_uv + r * dst_suv, src_uv + r * src_stride, static_cast<size_t>(width));
        }
        return true;
    }
    if (fmt == MPP_FMT_YUV420SP_VU) {
        return libyuv::NV21ToNV12(src_y, src_stride, src_uv, src_stride, dst_y, dst_sy, dst_uv, dst_suv, width,
                                  height) == 0;
    }
    return false;
}

}  // namespace

RkMppMjpegDecoder::RkMppMjpegDecoder() = default;

RkMppMjpegDecoder::~RkMppMjpegDecoder() {
    Close();
}

void RkMppMjpegDecoder::Close() {
    if (dec_cfg_) {
        mpp_dec_cfg_deinit(reinterpret_cast<MppDecCfg>(dec_cfg_));
        dec_cfg_ = nullptr;
    }
    if (input_group_) {
        mpp_buffer_group_put(reinterpret_cast<MppBufferGroup>(input_group_));
        input_group_ = nullptr;
    }
    if (output_buf_group_) {
        mpp_buffer_group_put(reinterpret_cast<MppBufferGroup>(output_buf_group_));
        output_buf_group_ = nullptr;
    }
    if (ctx_) {
        mpp_destroy(reinterpret_cast<MppCtx>(ctx_));
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
    last_expect_w_ = last_expect_h_ = 0;
    output_buf_size_ = 0;
}

size_t RkMppMjpegDecoder::ComputeJpegOutputBufSize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    // 对齐 gstmppjpegdec.c：NV12 对齐 + MAX(size, plane1_offset*2)
    const RK_U32 hor = MPP_ALIGN(static_cast<RK_U32>(width), 16);
    const RK_U32 ver = MPP_ALIGN(static_cast<RK_U32>(height), 16);
    const RK_U32 plane0 = hor * ver;
    const RK_U32 nv12 = plane0 + plane0 / 2;
    return static_cast<size_t>(std::max(nv12, plane0 * 2u));
}

bool RkMppMjpegDecoder::Init() {
    Close();

    MppBufferGroup output_grp = nullptr;
    MppBufferGroup input_grp = nullptr;
    if (mpp_buffer_group_get_internal(&output_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK || !output_grp) {
        std::cerr << "[RkMppMjpeg] output mpp_buffer_group_get_internal(DRM) failed\n";
        return false;
    }
    if (mpp_buffer_group_get_internal(&input_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK || !input_grp) {
        std::cerr << "[RkMppMjpeg] input mpp_buffer_group_get_internal(DRM) failed\n";
        mpp_buffer_group_put(output_grp);
        return false;
    }
    output_buf_group_ = output_grp;
    input_group_ = input_grp;

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        std::cerr << "[RkMppMjpeg] mpp_create failed\n";
        Close();
        return false;
    }
    ctx_ = ctx;
    mpi_ = mpi;

    RK_S64 out_timeout = MPP_POLL_BLOCK;
    mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &out_timeout);

    // gstmppdec.c：须在 mpp_init 之前
    RK_U32 fast_mode = 1;
    mpi->control(ctx, MPP_DEC_SET_PARSER_FAST_MODE, &fast_mode);

    if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG) != MPP_OK) {
        std::cerr << "[RkMppMjpeg] mpp_init(MJPEG) failed\n";
        Close();
        return false;
    }

    // gstmppdec.c set_format：mpp_init 后立即绑定 allocator 的 group
    if (mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, output_grp) != MPP_OK) {
        std::cerr << "[RkMppMjpeg] MPP_DEC_SET_EXT_BUF_GROUP failed\n";
        Close();
        return false;
    }

    MppDecCfg cfg = nullptr;
    if (mpp_dec_cfg_init(&cfg) != MPP_OK || !cfg) {
        Close();
        return false;
    }
    dec_cfg_ = cfg;
    if (mpi->control(ctx, MPP_DEC_GET_CFG, cfg) != MPP_OK) {
        Close();
        return false;
    }
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", 0);
    if (mpi->control(ctx, MPP_DEC_SET_CFG, cfg) != MPP_OK) {
        Close();
        return false;
    }

    MppFrameFormat want = MPP_FMT_YUV420SP;
    if (mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &want) != MPP_OK) {
        if (MjpegDecTraceEnabled()) {
            std::cerr << "[RkMppMjpeg] MPP_DEC_SET_OUTPUT_FORMAT(NV12) failed (non-fatal)\n";
        }
    }

    if (MjpegDecTraceEnabled()) {
        std::cerr << "[RkMppMjpeg] Init OK (GStreamer-style: DRM groups + EXT_BUF_GROUP at init + task path)\n";
    }
    return true;
}

bool RkMppMjpegDecoder::BuildMppPacketFromJpeg(const uint8_t* jpeg, size_t jpeg_len, void** out_packet) {
    if (!jpeg || jpeg_len == 0) {
        return false;
    }
    MppBufferGroup ig = reinterpret_cast<MppBufferGroup>(input_group_);
    MppBuffer mbuf = nullptr;
    if (mpp_buffer_get(ig, &mbuf, jpeg_len) != MPP_OK || !mbuf) {
        if (MjpegDecTraceEnabled()) {
            std::cerr << "[RkMppMjpeg] mpp_buffer_get input jpeg failed len=" << jpeg_len << "\n";
        }
        return false;
    }
    void* dst = mpp_buffer_get_ptr(mbuf);
    if (!dst) {
        mpp_buffer_put(mbuf);
        return false;
    }
    std::memcpy(dst, jpeg, jpeg_len);
    mpp_buffer_set_index(mbuf, kMppBufferGroupIndex);

    MppPacket packet = nullptr;
    if (mpp_packet_init_with_buffer(&packet, mbuf) != MPP_OK || !packet) {
        mpp_buffer_put(mbuf);
        return false;
    }
    mpp_buffer_put(mbuf);
    mpp_packet_set_size(packet, jpeg_len);
    mpp_packet_set_length(packet, jpeg_len);
    *out_packet = packet;
    return true;
}

bool RkMppMjpegDecoder::SendMppPacket(void* packet_vp) {
    MppCtx ctx = reinterpret_cast<MppCtx>(ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);
    MppPacket packet = static_cast<MppPacket>(packet_vp);
    MppBufferGroup og = reinterpret_cast<MppBufferGroup>(output_buf_group_);

    const int timeout_ms = 200;
    const int interval_ms = 5;
    int wait_left = 2000;

    while (wait_left > 0) {
        mpi->poll(ctx, MPP_PORT_INPUT, static_cast<MppPollType>(interval_ms));
        MppTask task = nullptr;
        const MPP_RET dq = mpi->dequeue(ctx, MPP_PORT_INPUT, &task);
        if (dq < 0 || !task) {
            wait_left -= interval_ms;
            continue;
        }

        mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);

        MppBuffer out_mbuf = nullptr;
        if (mpp_buffer_get(og, &out_mbuf, output_buf_size_) != MPP_OK || !out_mbuf) {
            if (MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] mpp_buffer_get output failed size=" << output_buf_size_ << "\n";
            }
            mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, nullptr);
            mpi->enqueue(ctx, MPP_PORT_INPUT, task);
            return false;
        }
        mpp_buffer_set_index(out_mbuf, kMppBufferGroupIndex);

        MppFrame mframe = nullptr;
        mpp_frame_init(&mframe);
        mpp_frame_set_buffer(mframe, out_mbuf);
        mpp_buffer_put(out_mbuf);

        MppMeta meta = mpp_frame_get_meta(mframe);
        mpp_meta_set_packet(meta, KEY_INPUT_PACKET, packet);

        mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, mframe);

        if (mpi->enqueue(ctx, MPP_PORT_INPUT, task) < 0) {
            mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, nullptr);
            mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, nullptr);
            mpi->enqueue(ctx, MPP_PORT_INPUT, task);
            mpp_frame_deinit(&mframe);
            return false;
        }
        return true;
    }
    if (MjpegDecTraceEnabled()) {
        std::cerr << "[RkMppMjpeg] SendMppPacket: dequeue input task timeout\n";
    }
    return false;
}

void* RkMppMjpegDecoder::PollMppFrame(int timeout_ms) {
    MppCtx ctx = reinterpret_cast<MppCtx>(ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);

    // 与 gstmppjpegdec poll_mpp_frame 一致：非 0 表示未就绪/超时
    if (mpi->poll(ctx, MPP_PORT_OUTPUT, static_cast<MppPollType>(timeout_ms))) {
        return nullptr;
    }
    MppTask task = nullptr;
    if (mpi->dequeue(ctx, MPP_PORT_OUTPUT, &task) < 0 || !task) {
        return nullptr;
    }
    MppFrame mframe = nullptr;
    mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &mframe);
    if (!mframe) {
        mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
        return nullptr;
    }
    MppMeta meta = mpp_frame_get_meta(mframe);
    MppPacket mpkt = nullptr;
    mpp_meta_get_packet(meta, KEY_INPUT_PACKET, &mpkt);
    if (mpkt) {
        mpp_packet_deinit(&mpkt);
    }
    mpi->enqueue(ctx, MPP_PORT_OUTPUT, task);
    return mframe;
}

bool RkMppMjpegDecoder::HandleInfoChangeFrame(void* frame_vp) {
    MppFrame frame = static_cast<MppFrame>(frame_vp);
    MppCtx ctx = reinterpret_cast<MppCtx>(ctx_);
    MppApi* mpi = reinterpret_cast<MppApi*>(mpi_);

    const RK_U32 bs = mpp_frame_get_buf_size(frame);
    if (bs > 0 && static_cast<size_t>(bs) > output_buf_size_) {
        output_buf_size_ = bs;
        if (MjpegDecTraceEnabled()) {
            std::cerr << "[RkMppMjpeg] info_change: bump output_buf_size to " << output_buf_size_ << "\n";
        }
    }

    if (mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr) != MPP_OK) {
        std::cerr << "[RkMppMjpeg] MPP_DEC_SET_INFO_CHANGE_READY failed\n";
        return false;
    }
    return true;
}

bool RkMppMjpegDecoder::DecodeJpegToI420(const uint8_t* jpeg,
                                         size_t jpeg_len,
                                         int expect_w,
                                         int expect_h,
                                         webrtc::I420Buffer* out_i420) {
    if (!ctx_ || !mpi_ || !jpeg || jpeg_len == 0 || !out_i420) {
        return false;
    }
    if (expect_w <= 0 || expect_h <= 0) {
        return false;
    }
    if (expect_w != last_expect_w_ || expect_h != last_expect_h_) {
        last_expect_w_ = expect_w;
        last_expect_h_ = expect_h;
        output_buf_size_ = ComputeJpegOutputBufSize(expect_w, expect_h);
    }
    if (output_buf_size_ == 0) {
        return false;
    }

    bool decoded = false;
    constexpr int kMaxSubmit = 8;
    constexpr int kMaxPollPerSubmit = 500;

    for (int submit = 0; submit < kMaxSubmit && !decoded; ++submit) {
        MppPacket packet = nullptr;
        if (!BuildMppPacketFromJpeg(jpeg, jpeg_len, reinterpret_cast<void**>(&packet))) {
            return false;
        }
        if (!SendMppPacket(packet)) {
            mpp_packet_deinit(&packet);
            if (MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] SendMppPacket failed submit=" << submit << "\n";
            }
            return false;
        }

        bool need_resubmit = false;
        for (int poll_i = 0; poll_i < kMaxPollPerSubmit; ++poll_i) {
            MppFrame frame = static_cast<MppFrame>(PollMppFrame(30));
            if (!frame) {
                usleep(2000);
                continue;
            }
            if (mpp_frame_get_info_change(frame)) {
                if (!HandleInfoChangeFrame(frame)) {
                    mpp_frame_deinit(&frame);
                    return false;
                }
                mpp_frame_deinit(&frame);
                need_resubmit = true;
                break;
            }

            const RK_U32 err = mpp_frame_get_errinfo(frame);
            const RK_U32 discard = mpp_frame_get_discard(frame);
            if (err || discard) {
                if (MjpegDecTraceEnabled()) {
                    std::cerr << "[RkMppMjpeg] frame errinfo=" << err << " discard=" << discard << "\n";
                }
                mpp_frame_deinit(&frame);
                return false;
            }

            const int fw = static_cast<int>(mpp_frame_get_width(frame));
            const int fh = static_cast<int>(mpp_frame_get_height(frame));
            if (fw != expect_w || fh != expect_h) {
                if (MjpegDecTraceEnabled()) {
                    std::cerr << "[RkMppMjpeg] size mismatch decoded " << fw << "x" << fh << " expect " << expect_w << "x"
                              << expect_h << "\n";
                }
                mpp_frame_deinit(&frame);
                return false;
            }

            MppBuffer mbuf = mpp_frame_get_buffer(frame);
            if (!mbuf) {
                mpp_frame_deinit(&frame);
                return false;
            }
            auto* yuv = static_cast<const uint8_t*>(mpp_buffer_get_ptr(mbuf));
            if (!yuv) {
                mpp_frame_deinit(&frame);
                return false;
            }

            const RK_U32 fmt = mpp_frame_get_fmt(frame);
            const int hs = static_cast<int>(mpp_frame_get_hor_stride(frame));
            const int ver_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
            const uint8_t* src_y = yuv;
            const uint8_t* src_uv = yuv + static_cast<size_t>(hs) * static_cast<size_t>(ver_stride);

            int conv = 0;
            if (fmt == MPP_FMT_YUV420SP) {
                conv = libyuv::NV12ToI420(src_y, hs, src_uv, hs, out_i420->MutableDataY(), out_i420->StrideY(),
                                          out_i420->MutableDataU(), out_i420->StrideU(), out_i420->MutableDataV(),
                                          out_i420->StrideV(), fw, fh);
            } else if (fmt == MPP_FMT_YUV420SP_VU) {
                conv = libyuv::NV21ToI420(src_y, hs, src_uv, hs, out_i420->MutableDataY(), out_i420->StrideY(),
                                          out_i420->MutableDataU(), out_i420->StrideU(), out_i420->MutableDataV(),
                                          out_i420->StrideV(), fw, fh);
            } else {
                if (MjpegDecTraceEnabled()) {
                    std::cerr << "[RkMppMjpeg] unexpected fmt=" << fmt << "\n";
                }
                mpp_frame_deinit(&frame);
                return false;
            }

            mpp_frame_deinit(&frame);
            decoded = (conv == 0);
            if (!decoded && MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] libyuv convert failed conv=" << conv << "\n";
            }
            break;
        }

        if (!decoded && !need_resubmit) {
            if (MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] decode timeout (no output frame) submit=" << submit << "\n";
            }
            return false;
        }
    }

    if (!decoded && MjpegDecTraceEnabled()) {
        std::cerr << "[RkMppMjpeg] DecodeJpegToI420 failed after resubmit loop\n";
    }
    return decoded;
}

bool RkMppMjpegDecoder::DecodeJpegToNV12(const uint8_t* jpeg,
                                         size_t jpeg_len,
                                         int expect_w,
                                         int expect_h,
                                         webrtc::NV12Buffer* out_nv12) {
    if (!ctx_ || !mpi_ || !jpeg || jpeg_len == 0 || !out_nv12) {
        return false;
    }
    if (expect_w <= 0 || expect_h <= 0) {
        return false;
    }
    if (out_nv12->width() != expect_w || out_nv12->height() != expect_h) {
        return false;
    }
    if (expect_w != last_expect_w_ || expect_h != last_expect_h_) {
        last_expect_w_ = expect_w;
        last_expect_h_ = expect_h;
        output_buf_size_ = ComputeJpegOutputBufSize(expect_w, expect_h);
    }
    if (output_buf_size_ == 0) {
        return false;
    }

    bool decoded = false;
    constexpr int kMaxSubmit = 8;
    constexpr int kMaxPollPerSubmit = 500;

    for (int submit = 0; submit < kMaxSubmit && !decoded; ++submit) {
        MppPacket packet = nullptr;
        if (!BuildMppPacketFromJpeg(jpeg, jpeg_len, reinterpret_cast<void**>(&packet))) {
            return false;
        }
        if (!SendMppPacket(packet)) {
            mpp_packet_deinit(&packet);
            if (MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] SendMppPacket failed submit=" << submit << "\n";
            }
            return false;
        }

        bool need_resubmit = false;
        for (int poll_i = 0; poll_i < kMaxPollPerSubmit; ++poll_i) {
            MppFrame frame = static_cast<MppFrame>(PollMppFrame(30));
            if (!frame) {
                usleep(2000);
                continue;
            }
            if (mpp_frame_get_info_change(frame)) {
                if (!HandleInfoChangeFrame(frame)) {
                    mpp_frame_deinit(&frame);
                    return false;
                }
                mpp_frame_deinit(&frame);
                need_resubmit = true;
                break;
            }

            const RK_U32 err = mpp_frame_get_errinfo(frame);
            const RK_U32 discard = mpp_frame_get_discard(frame);
            if (err || discard) {
                if (MjpegDecTraceEnabled()) {
                    std::cerr << "[RkMppMjpeg] frame errinfo=" << err << " discard=" << discard << "\n";
                }
                mpp_frame_deinit(&frame);
                return false;
            }

            const int fw = static_cast<int>(mpp_frame_get_width(frame));
            const int fh = static_cast<int>(mpp_frame_get_height(frame));
            if (fw != expect_w || fh != expect_h) {
                if (MjpegDecTraceEnabled()) {
                    std::cerr << "[RkMppMjpeg] size mismatch decoded " << fw << "x" << fh << " expect " << expect_w << "x"
                              << expect_h << "\n";
                }
                mpp_frame_deinit(&frame);
                return false;
            }

            MppBuffer mbuf = mpp_frame_get_buffer(frame);
            if (!mbuf) {
                mpp_frame_deinit(&frame);
                return false;
            }
            auto* yuv = static_cast<const uint8_t*>(mpp_buffer_get_ptr(mbuf));
            if (!yuv) {
                mpp_frame_deinit(&frame);
                return false;
            }

            const RK_U32 fmt = mpp_frame_get_fmt(frame);
            const int hs = static_cast<int>(mpp_frame_get_hor_stride(frame));
            const int ver_stride = static_cast<int>(mpp_frame_get_ver_stride(frame));
            const uint8_t* src_y = yuv;
            const uint8_t* src_uv = yuv + static_cast<size_t>(hs) * static_cast<size_t>(ver_stride);

            decoded = CopyMppSemiPlanarToNv12(fmt, src_y, src_uv, hs, fw, fh, out_nv12);
            mpp_frame_deinit(&frame);
            if (!decoded && MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] CopyMppSemiPlanarToNv12 failed fmt=" << fmt << "\n";
            }
            break;
        }

        if (!decoded && !need_resubmit) {
            if (MjpegDecTraceEnabled()) {
                std::cerr << "[RkMppMjpeg] decode timeout (no output frame) submit=" << submit << "\n";
            }
            return false;
        }
    }

    if (!decoded && MjpegDecTraceEnabled()) {
        std::cerr << "[RkMppMjpeg] DecodeJpegToNV12 failed after resubmit loop\n";
    }
    return decoded;
}

}  // namespace webrtc_demo
