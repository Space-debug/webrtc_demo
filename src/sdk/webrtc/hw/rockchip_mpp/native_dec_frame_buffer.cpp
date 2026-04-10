#include "webrtc/hw/rockchip_mpp/native_dec_frame_buffer.h"

#include <cstring>

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "rtc_base/ref_counted_object.h"
#include "third_party/libyuv/include/libyuv/convert.h"

#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "rk_mpi.h"
#include "rk_type.h"

namespace webrtc_demo {

namespace {

bool CopyMppSemiPlanarToNv12(RK_U32 fmt,
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

// static
webrtc::scoped_refptr<MppNativeDecFrameBuffer> MppNativeDecFrameBuffer::CreateFromMppFrame(void* mpp_frame,
                                                                                           int width,
                                                                                           int height,
                                                                                           int hor_stride,
                                                                                           int ver_stride,
                                                                                           uint32_t mpp_fmt,
                                                                                           int64_t mjpeg_input_timestamp_us,
                                                                                           int64_t dq_time_us,
                                                                                           int64_t v4l2_timestamp_us,
                                                                                           int64_t poll_wait_us,
                                                                                           int64_t dqbuf_ioctl_us,
                                                                                           int64_t decode_queue_wait_us,
                                                                                           int64_t wall_capture_utc_ms) {
    if (!mpp_frame) {
        return nullptr;
    }
    return webrtc::scoped_refptr<MppNativeDecFrameBuffer>(
        new webrtc::RefCountedObject<MppNativeDecFrameBuffer>(mpp_frame, width, height, hor_stride, ver_stride,
                                                              mpp_fmt, mjpeg_input_timestamp_us, dq_time_us,
                                                              v4l2_timestamp_us, poll_wait_us, dqbuf_ioctl_us,
                                                              decode_queue_wait_us, wall_capture_utc_ms));
}

MppNativeDecFrameBuffer* MppNativeDecFrameBuffer::TryGet(const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer) {
    if (!buffer || buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
        return nullptr;
    }
    return dynamic_cast<MppNativeDecFrameBuffer*>(buffer.get());
}

MppNativeDecFrameBuffer::MppNativeDecFrameBuffer(void* mpp_frame,
                                                 int width,
                                                 int height,
                                                 int hor_stride,
                                                 int ver_stride,
                                                 uint32_t mpp_fmt,
                                                 int64_t mjpeg_input_timestamp_us,
                                                 int64_t dq_time_us,
                                                 int64_t v4l2_timestamp_us,
                                                 int64_t poll_wait_us,
                                                 int64_t dqbuf_ioctl_us,
                                                 int64_t decode_queue_wait_us,
                                                 int64_t wall_capture_utc_ms)
    : frame_(mpp_frame),
      width_(width),
      height_(height),
      hor_stride_(hor_stride),
      ver_stride_(ver_stride),
      mpp_fmt_(mpp_fmt),
      mjpeg_input_timestamp_us_(mjpeg_input_timestamp_us),
      dq_time_us_(dq_time_us),
      v4l2_timestamp_us_(v4l2_timestamp_us),
      poll_wait_us_(poll_wait_us),
      dqbuf_ioctl_us_(dqbuf_ioctl_us),
      decode_queue_wait_us_(decode_queue_wait_us),
      wall_capture_utc_ms_(wall_capture_utc_ms) {}

MppNativeDecFrameBuffer::~MppNativeDecFrameBuffer() {
    if (frame_) {
        MppFrame f = static_cast<MppFrame>(frame_);
        mpp_frame_deinit(&f);
        frame_ = nullptr;
    }
}

webrtc::VideoFrameBuffer::Type MppNativeDecFrameBuffer::type() const {
    return Type::kNative;
}

int MppNativeDecFrameBuffer::width() const {
    return width_;
}

int MppNativeDecFrameBuffer::height() const {
    return height_;
}

void* MppNativeDecFrameBuffer::mpp_buffer_handle() const {
    if (!frame_) {
        return nullptr;
    }
    MppFrame f = static_cast<MppFrame>(frame_);
    return mpp_frame_get_buffer(f);
}

webrtc::scoped_refptr<webrtc::I420BufferInterface> MppNativeDecFrameBuffer::ToI420() {
    MppFrame f = static_cast<MppFrame>(frame_);
    if (!f) {
        return nullptr;
    }
    MppBuffer mbuf = mpp_frame_get_buffer(f);
    if (!mbuf) {
        return nullptr;
    }
    auto* yuv = static_cast<const uint8_t*>(mpp_buffer_get_ptr(mbuf));
    if (!yuv) {
        return nullptr;
    }
    const int hs = hor_stride_;
    const int ver_stride = ver_stride_;
    const uint8_t* src_y = yuv;
    const uint8_t* src_uv = yuv + static_cast<size_t>(hs) * static_cast<size_t>(ver_stride);
    webrtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(width_, height_);
    const RK_U32 fmt = static_cast<RK_U32>(mpp_fmt_);
    int conv = -1;
    if (fmt == MPP_FMT_YUV420SP) {
        conv = libyuv::NV12ToI420(src_y, hs, src_uv, hs, i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                                  i420->StrideU(), i420->MutableDataV(), i420->StrideV(), width_, height_);
    } else if (fmt == MPP_FMT_YUV420SP_VU) {
        conv = libyuv::NV21ToI420(src_y, hs, src_uv, hs, i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                                  i420->StrideU(), i420->MutableDataV(), i420->StrideV(), width_, height_);
    }
    if (conv != 0) {
        return nullptr;
    }
    return i420;
}

webrtc::scoped_refptr<webrtc::VideoFrameBuffer> MppNativeDecFrameBuffer::GetMappedFrameBuffer(
    webrtc::ArrayView<Type> types) {
    for (webrtc::VideoFrameBuffer::Type t : types) {
        if (t == Type::kNV12) {
            webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 = webrtc::NV12Buffer::Create(width_, height_);
            MppFrame f = static_cast<MppFrame>(frame_);
            if (!f || !nv12) {
                return nullptr;
            }
            MppBuffer mbuf = mpp_frame_get_buffer(f);
            if (!mbuf) {
                return nullptr;
            }
            auto* yuv = static_cast<const uint8_t*>(mpp_buffer_get_ptr(mbuf));
            if (!yuv) {
                return nullptr;
            }
            const uint8_t* src_y = yuv;
            const uint8_t* src_uv = yuv + static_cast<size_t>(hor_stride_) * static_cast<size_t>(ver_stride_);
            if (!CopyMppSemiPlanarToNv12(static_cast<RK_U32>(mpp_fmt_), src_y, src_uv, hor_stride_, width_, height_,
                                       nv12.get())) {
                return nullptr;
            }
            return nv12;
        }
    }
    return nullptr;
}

std::string MppNativeDecFrameBuffer::storage_representation() const {
    return "mpp_mjpeg_dec_drm_frame";
}

}  // namespace webrtc_demo
