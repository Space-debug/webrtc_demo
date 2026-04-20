#include "media/camera_video_track_source.h"

#include <cstdint>
#include <cstring>

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

#include "libyuv/convert.h"

#if defined(WEBRTC_LINUX) && defined(__linux__)
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
#include "webrtc/hw/rockchip_mpp/native_dec_frame_buffer.h"
#include "webrtc/hw/rockchip_mpp/mjpeg_decoder.h"
#endif
#include <cerrno>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

#include <chrono>
#include <deque>
#include <iostream>
#include <cstdlib>
#include <atomic>
#endif

namespace webrtc_demo {

#if defined(WEBRTC_LINUX) && defined(__linux__)
namespace {
bool LatencyTraceEnabled() {
    static int cached = -1;
    if (cached >= 0) {
        return cached != 0;
    }
    const char* e = std::getenv("WEBRTC_LATENCY_TRACE");
    cached = (e && e[0] == '1') ? 1 : 0;
    return cached != 0;
}

void LogMjpegDecodeTiming(const char* tag, int64_t before_us, int64_t after_us) {
    static std::atomic<unsigned> g_n{0};
    const unsigned n = ++g_n;
    if ((n % 30) != 0) {
        return;
    }
    const double ms = static_cast<double>(after_us - before_us) / 1000.0;
    std::cout << "[MJPEG decode " << tag << "] frame#" << n << " before_us=" << before_us
              << " after_us=" << after_us << " duration_ms=" << ms << std::endl;
}

}  // namespace

#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
static bool PreferMjpegNativeZeroCopyToEnc() {
    const char* e = std::getenv("WEBRTC_MJPEG_ZERO_COPY_TO_ENC");
    if (!e || e[0] == '\0') {
        return true;
    }
    return e[0] != '0';
}

#endif  // WEBRTC_DEMO_HAVE_ROCKCHIP_MPP
#endif  // WEBRTC_LINUX && __linux__

CameraVideoTrackSource::CameraVideoTrackSource() : webrtc::AdaptedVideoTrackSource() {}

CameraVideoTrackSource::~CameraVideoTrackSource() {
    Stop();
}

bool CameraVideoTrackSource::GetNegotiatedCaptureSize(int* width, int* height) const {
#if defined(WEBRTC_LINUX) && defined(__linux__)
    if (width && height && direct_cap_w_ > 0 && direct_cap_h_ > 0) {
        *width = direct_cap_w_;
        *height = direct_cap_h_;
        return true;
    }
#else
    (void)width;
    (void)height;
#endif
    return false;
}

bool CameraVideoTrackSource::GetNegotiatedCaptureFramerate(int* out_fps) const {
    if (!out_fps || negotiated_capture_fps_ <= 0) {
        return false;
    }
    *out_fps = negotiated_capture_fps_;
    return true;
}

#if defined(WEBRTC_LINUX) && defined(__linux__)

void CameraVideoTrackSource::StopDirectV4l2() {
    direct_run_ = false;
    if (direct_thread_.joinable()) {
        direct_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lk(jpeg_queue_mu_);
        decode_worker_exit_ = true;
    }
    jpeg_queue_cv_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
    {
        std::deque<MjpegPendingBuf> leftover;
        {
            std::lock_guard<std::mutex> lk(jpeg_queue_mu_);
            leftover.swap(jpeg_queue_);
        }
        for (const MjpegPendingBuf& drop : leftover) {
            QBufV4l2Index(drop.index);
        }
    }
    decode_worker_exit_ = false;
    nv12_pool_.clear();
    nv12_pool_w_ = nv12_pool_h_ = 0;
    nv12_ring_next_ = 0;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    mjpeg_mpp_.reset();
#endif
    if (direct_fd_ >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(direct_fd_, VIDIOC_STREAMOFF, &t);
    }
    for (size_t i = 0; i < direct_mmap_.size(); ++i) {
        if (direct_mmap_[i] && direct_mmap_len_[i] > 0) {
            munmap(direct_mmap_[i], direct_mmap_len_[i]);
        }
    }
    for (int fd : direct_expbuf_fd_) {
        if (fd >= 0) {
            close(fd);
        }
    }
    direct_expbuf_fd_.clear();
    direct_mmap_.clear();
    direct_mmap_len_.clear();
    if (direct_fd_ >= 0) {
        close(direct_fd_);
        direct_fd_ = -1;
    }
    direct_cap_w_ = direct_cap_h_ = 0;
    direct_pixfmt_ = 0;
}

void CameraVideoTrackSource::ApplyMjpegPipelineOptions(const V4l2MjpegPipelineOptions* p) {
    V4l2MjpegPipelineOptions def;
    const V4l2MjpegPipelineOptions& o = p ? *p : def;
    mjpeg_queue_latest_only_ = o.mjpeg_queue_latest_only;
    int qmax = o.mjpeg_queue_max;
    if (qmax < 1) {
        qmax = 1;
    }
    if (qmax > 32) {
        qmax = 32;
    }
    mjpeg_queue_max_ = static_cast<size_t>(qmax);
    int slots = o.nv12_pool_slots;
    if (slots < 4) {
        slots = 4;
    }
    if (slots > 16) {
        slots = 16;
    }
    nv12_pool_slots_ = slots;

    int nbuf = o.v4l2_buffer_count;
    if (nbuf < 2) {
        nbuf = 2;
    }
    if (nbuf > 32) {
        nbuf = 32;
    }
    v4l2_buffer_count_ = nbuf;

    int pto = o.v4l2_poll_timeout_ms;
    if (pto < 1) {
        pto = 1;
    }
    if (pto > 2000) {
        pto = 2000;
    }
    v4l2_poll_timeout_ms_ = pto;

    mjpeg_decode_inline_ = o.mjpeg_decode_inline;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    v4l2_ext_dma_config_ = o.mjpeg_v4l2_ext_dma;
    mjpeg_rga_config_ = o.mjpeg_rga_to_mpp;
#endif
    if (const char* e = std::getenv("WEBRTC_MJPEG_DECODE_INLINE")) {
        const char c = e[0];
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') {
            mjpeg_decode_inline_ = true;
        }
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') {
            mjpeg_decode_inline_ = false;
        }
    }

    if (const char* e = std::getenv("WEBRTC_MJPEG_QUEUE_LATEST_ONLY")) {
        const char c = e[0];
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') {
            mjpeg_queue_latest_only_ = true;
        }
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') {
            mjpeg_queue_latest_only_ = false;
        }
    }
    if (const char* e = std::getenv("WEBRTC_MJPEG_QUEUE_MAX")) {
        const int v = std::atoi(e);
        if (v >= 1 && v <= 32) {
            mjpeg_queue_max_ = static_cast<size_t>(v);
        }
    }
    if (const char* e = std::getenv("WEBRTC_NV12_POOL_SLOTS")) {
        const int v = std::atoi(e);
        if (v >= 4 && v <= 16) {
            nv12_pool_slots_ = v;
        }
    }
    if (const char* e = std::getenv("WEBRTC_V4L2_BUFFER_COUNT")) {
        const int v = std::atoi(e);
        if (v >= 2 && v <= 32) {
            v4l2_buffer_count_ = v;
        }
    }
    if (const char* e = std::getenv("WEBRTC_V4L2_POLL_TIMEOUT_MS")) {
        const int v = std::atoi(e);
        if (v >= 1 && v <= 2000) {
            v4l2_poll_timeout_ms_ = v;
        }
    }

    // 延迟 QBUF 时，队列里每帧占一块 mmap；须留至少 1 块在驱动里供采集写入。
    const size_t qcap = static_cast<size_t>(std::max(1, v4l2_buffer_count_ - 1));
    if (mjpeg_queue_max_ > qcap) {
        RTC_LOG(LS_INFO) << "[CameraV4L2] mjpeg_queue_max clamped to " << qcap
                         << " (v4l2_buffer_count=" << v4l2_buffer_count_ << ")";
        mjpeg_queue_max_ = qcap;
    }
}

void CameraVideoTrackSource::QBufV4l2Index(unsigned int index) {
    if (direct_fd_ < 0) {
        return;
    }
    struct v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (ioctl(direct_fd_, VIDIOC_QBUF, &buf) < 0) {
        RTC_LOG(LS_WARNING) << "[CameraV4L2] VIDIOC_QBUF index=" << index << " errno=" << errno;
    }
}

void CameraVideoTrackSource::EnsureNv12Pool(int w, int h) {
    const int slots = nv12_pool_slots_;
    if (w <= 0 || h <= 0 || slots < 4) {
        return;
    }
    if (nv12_pool_.size() == static_cast<size_t>(slots) && nv12_pool_w_ == w && nv12_pool_h_ == h) {
        return;
    }
    nv12_pool_.clear();
    nv12_pool_.reserve(static_cast<size_t>(slots));
    for (int i = 0; i < slots; ++i) {
        webrtc::scoped_refptr<webrtc::NV12Buffer> b = webrtc::NV12Buffer::Create(w, h);
        if (!b) {
            nv12_pool_.clear();
            nv12_pool_w_ = nv12_pool_h_ = 0;
            return;
        }
        nv12_pool_.push_back(std::move(b));
    }
    nv12_pool_w_ = w;
    nv12_pool_h_ = h;
    nv12_ring_next_ = 0;
}

bool CameraVideoTrackSource::StartDirectV4l2(const char* device_path, int width, int height, int fps) {
    StopDirectV4l2();

    // 建流前用阻塞 open；刚结束的上一次采集或其它进程可能短暂占设备，EBUSY 时重试。
    direct_fd_ = -1;
    for (int attempt = 0; attempt < 20; ++attempt) {
        direct_fd_ = open(device_path, O_RDWR, 0);
        if (direct_fd_ >= 0) {
            break;
        }
        if (errno != EBUSY) {
            std::cerr << "[CameraV4L2] open " << device_path << " failed errno=" << errno << std::endl;
            return false;
        }
        usleep(200 * 1000);
    }
    if (direct_fd_ < 0) {
        std::cerr << "[CameraV4L2] open " << device_path << " failed errno=EBUSY after retries" << std::endl;
        return false;
    }

    struct v4l2_capability cap {};
    if (ioctl(direct_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "[CameraV4L2] VIDIOC_QUERYCAP errno=" << errno << std::endl;
        close(direct_fd_);
        direct_fd_ = -1;
        return false;
    }
    if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
        std::cerr << "[CameraV4L2] not a VIDEO_CAPTURE node: " << device_path << std::endl;
        close(direct_fd_);
        direct_fd_ = -1;
        return false;
    }

    // UVC 等设备在已有可用格式时，STREAMOFF/REQBUFS(0)/S_FMT 可能一律 EBUSY；先读当前格式并尽量沿用。
    auto accept_gfmt_if_usable = [&]() -> bool {
        struct v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(direct_fd_, VIDIOC_G_FMT, &fmt) < 0) {
            return false;
        }
        const uint32_t pf = fmt.fmt.pix.pixelformat;
        if (pf != V4L2_PIX_FMT_MJPEG && pf != V4L2_PIX_FMT_YUYV) {
            return false;
        }
        if (fmt.fmt.pix.width < 160 || fmt.fmt.pix.height < 120) {
            return false;
        }
        direct_cap_w_ = static_cast<int>(fmt.fmt.pix.width);
        direct_cap_h_ = static_cast<int>(fmt.fmt.pix.height);
        direct_pixfmt_ = pf;
        return true;
    };

    auto try_sfmt = [&](uint32_t pixfmt, int w, int h) -> bool {
        struct v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = static_cast<__u32>(w);
        fmt.fmt.pix.height = static_cast<__u32>(h);
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        fmt.fmt.pix.sizeimage = 0;
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (ioctl(direct_fd_, VIDIOC_S_FMT, &fmt) == 0) {
                direct_cap_w_ = static_cast<int>(fmt.fmt.pix.width);
                direct_cap_h_ = static_cast<int>(fmt.fmt.pix.height);
                direct_pixfmt_ = fmt.fmt.pix.pixelformat;
                return direct_cap_w_ > 0 && direct_cap_h_ > 0;
            }
            if (errno != EBUSY) {
                return false;
            }
            usleep(150 * 1000);
        }
        return false;
    };

    // 保留驱动当前分辨率，只改像素格式（部分 ISP 节点拒绝任意 WxH）
    auto try_sfmt_keep_size = [&](uint32_t pixfmt) -> bool {
        struct v4l2_format fmt {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(direct_fd_, VIDIOC_G_FMT, &fmt) < 0) {
            return false;
        }
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;
        fmt.fmt.pix.sizeimage = 0;
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (ioctl(direct_fd_, VIDIOC_S_FMT, &fmt) == 0) {
                direct_cap_w_ = static_cast<int>(fmt.fmt.pix.width);
                direct_cap_h_ = static_cast<int>(fmt.fmt.pix.height);
                direct_pixfmt_ = fmt.fmt.pix.pixelformat;
                return direct_cap_w_ > 0 && direct_cap_h_ > 0;
            }
            if (errno != EBUSY) {
                return false;
            }
            usleep(150 * 1000);
        }
        return false;
    };

    const uint32_t yuyv = V4L2_PIX_FMT_YUYV;
    const uint32_t mjpeg = V4L2_PIX_FMT_MJPEG;

    // 与 streams.conf 的 WIDTH/HEIGHT 一致：先 S_FMT 请求目标分辨率，禁止一上来 G_FMT 沿用 1080p 导致与配置不符。
    auto try_sfmt_exact = [&](uint32_t pixfmt, int w, int h) -> bool {
        if (!try_sfmt(pixfmt, w, h)) {
            return false;
        }
        return direct_cap_w_ == w && direct_cap_h_ == h;
    };

    // 同分辨率：默认优先 YUYV，避免 MJPEG 软解；开启 MPP MJPEG 硬解时优先 MJPEG（多数 UVC 在 720p 等档位上
    // MJPEG 帧率远高于 YUYV，原先先 YUYV 会锁在 10fps 且永远测不到硬解路径）。
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    const bool prefer_mjpeg_pixfmt = prefer_mpp_mjpeg_decode_;
#else
    const bool prefer_mjpeg_pixfmt = false;
#endif
    bool fmt_ok = prefer_mjpeg_pixfmt
                      ? (try_sfmt_exact(mjpeg, width, height) || try_sfmt_exact(yuyv, width, height))
                      : (try_sfmt_exact(yuyv, width, height) || try_sfmt_exact(mjpeg, width, height));

    // 设备已是目标分辨率但 S_FMT(改分辨率) 失败时，只切像素格式或直接使用当前帧格式。
    if (!fmt_ok) {
        struct v4l2_format g {};
        g.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(direct_fd_, VIDIOC_G_FMT, &g) == 0 && static_cast<int>(g.fmt.pix.width) == width &&
            static_cast<int>(g.fmt.pix.height) == height) {
            const uint32_t pf = g.fmt.pix.pixelformat;
            if (pf == mjpeg || pf == yuyv) {
                direct_cap_w_ = width;
                direct_cap_h_ = height;
                direct_pixfmt_ = pf;
                fmt_ok = true;
            } else {
                fmt_ok = prefer_mjpeg_pixfmt ? (try_sfmt_keep_size(mjpeg) || try_sfmt_keep_size(yuyv))
                                             : (try_sfmt_keep_size(yuyv) || try_sfmt_keep_size(mjpeg));
                if (fmt_ok && (direct_cap_w_ != width || direct_cap_h_ != height)) {
                    fmt_ok = false;
                }
            }
        }
    }

    // 最后：可读且尺寸与配置一致才接受（避免 EBUSY 时默默用错误分辨率）。
    if (!fmt_ok && accept_gfmt_if_usable()) {
        if (direct_cap_w_ == width && direct_cap_h_ == height) {
            fmt_ok = true;
        } else {
            std::cerr << "[CameraV4L2] need capture " << width << "x" << height << " per config, but device is "
                      << direct_cap_w_ << "x" << direct_cap_h_
                      << " (VIDIOC_S_FMT unavailable or EBUSY). Match WIDTH/HEIGHT to the device or release the camera.\n";
            close(direct_fd_);
            direct_fd_ = -1;
            return false;
        }
    }

    if (!fmt_ok) {
        struct v4l2_format g {};
        g.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int e = errno;
        if (ioctl(direct_fd_, VIDIOC_G_FMT, &g) == 0) {
            std::cerr << "[CameraV4L2] cannot set capture " << width << "x" << height << " (device reports "
                      << g.fmt.pix.width << "x" << g.fmt.pix.height << ") errno=" << e << " (" << strerror(e) << ")\n";
        } else {
            std::cerr << "[CameraV4L2] VIDIOC_S_FMT failed for MJPEG/YUYV errno=" << e << " (" << strerror(e) << ")\n";
        }
        close(direct_fd_);
        direct_fd_ = -1;
        return false;
    }

    struct v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(direct_fd_, VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator =
            static_cast<__u32>(fps > 0 ? fps : 30);
        ioctl(direct_fd_, VIDIOC_S_PARM, &parm);
    }

    struct v4l2_requestbuffers rb {};
    rb.count = static_cast<unsigned int>(v4l2_buffer_count_);
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(direct_fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
        std::cerr << "[CameraV4L2] VIDIOC_REQBUFS failed errno=" << errno << std::endl;
        close(direct_fd_);
        direct_fd_ = -1;
        return false;
    }

    const unsigned int nbuf = rb.count;
    direct_mmap_.resize(nbuf, nullptr);
    direct_mmap_len_.resize(nbuf, 0);
    direct_expbuf_fd_.assign(nbuf, -1);
    for (unsigned int i = 0; i < nbuf; ++i) {
        struct v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(direct_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            StopDirectV4l2();
            return false;
        }
        void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, direct_fd_, buf.m.offset);
        if (p == MAP_FAILED) {
            StopDirectV4l2();
            return false;
        }
        direct_mmap_[i] = p;
        direct_mmap_len_[i] = buf.length;
        if (ioctl(direct_fd_, VIDIOC_QBUF, &buf) < 0) {
            StopDirectV4l2();
            return false;
        }
    }

#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    if (WantV4l2ExtDmabufToMpp() || WantMjpegRgaToMpp()) {
        unsigned exp_ok = 0;
        for (unsigned int i = 0; i < nbuf; ++i) {
            struct v4l2_exportbuffer exp {};
            exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index = i;
            exp.plane = 0;
            exp.flags = O_CLOEXEC;
            if (ioctl(direct_fd_, VIDIOC_EXPBUF, &exp) == 0 && exp.fd >= 0) {
                direct_expbuf_fd_[i] = exp.fd;
                ++exp_ok;
            }
        }
        if (exp_ok == nbuf) {
            if (WantV4l2ExtDmabufToMpp()) {
                std::cout << "[CameraV4L2] VIDIOC_EXPBUF: " << nbuf
                          << " dma-buf fd(s) → MPP JPEG EXT_DMA import\n";
            } else {
                std::cout << "[CameraV4L2] VIDIOC_EXPBUF: " << nbuf
                          << " dma-buf fd(s) → RGA copy to MPP input (WEBRTC_MJPEG_RGA_TO_MPP)\n";
            }
        } else if (exp_ok > 0) {
            std::cout << "[CameraV4L2] VIDIOC_EXPBUF: " << exp_ok << "/" << nbuf
                      << " (per-buffer dma or memcpy)\n";
        } else {
            std::cout << "[CameraV4L2] VIDIOC_EXPBUF unsupported; MPP JPEG uses memcpy from mmap\n";
        }
    }
#endif

    enum v4l2_buf_type typ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(direct_fd_, VIDIOC_STREAMON, &typ) < 0) {
        std::cerr << "[CameraV4L2] VIDIOC_STREAMON errno=" << errno << std::endl;
        StopDirectV4l2();
        return false;
    }

    // 驱动在 S_PARM/STREAMON 之后给出的实际帧间隔：fps ≈ denominator / numerator（V4L2 文档 timeperframe）。
    negotiated_capture_fps_ = (fps > 0 ? fps : 30);
    {
        struct v4l2_streamparm gparm {};
        gparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(direct_fd_, VIDIOC_G_PARM, &gparm) == 0 &&
            (gparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
            const __u32 n = gparm.parm.capture.timeperframe.numerator;
            const __u32 d = gparm.parm.capture.timeperframe.denominator;
            if (n > 0 && d > 0) {
                const int calc = static_cast<int>((static_cast<uint64_t>(d) + n / 2) / n);
                if (calc >= 1 && calc <= 480) {
                    negotiated_capture_fps_ = calc;
                }
            }
        }
    }

    {
        int fl = fcntl(direct_fd_, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(direct_fd_, F_SETFL, fl | O_NONBLOCK);
        }
    }

    direct_run_ = true;
    decode_worker_exit_ = false;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    if (prefer_mpp_mjpeg_decode_ && direct_pixfmt_ == V4L2_PIX_FMT_MJPEG) {
        auto dec = std::make_unique<RkMppMjpegDecoder>();
        if (dec->Init()) {
            dec->SetPipelineV4l2ExtDmabuf(v4l2_ext_dma_config_);
            dec->SetPipelineRgaToMpp(mjpeg_rga_config_);
            mjpeg_mpp_ = std::move(dec);
            std::cout << "[CameraV4L2] MJPEG: Rockchip MPP decode -> NV12 (zero I420/libyuv chroma conversion in HW encode path)\n";
        }
    }
#endif
    const bool mjpeg_async =
        (direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG)) && !mjpeg_decode_inline_;
    if (mjpeg_async) {
        decode_thread_ = std::thread([this]() { DecodeWorkerThreadMain(); });
    }
    direct_thread_ = std::thread([this]() { DirectCaptureThreadMain(); });
    std::cout << "[CameraV4L2] Direct capture " << device_path << " " << direct_cap_w_ << "x" << direct_cap_h_
              << " @" << negotiated_capture_fps_ << "fps fourcc=0x" << std::hex << direct_pixfmt_ << std::dec
              << " mmap_bufs=" << nbuf << " poll_timeout_ms=" << v4l2_poll_timeout_ms_;
    if (direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG)) {
        if (mjpeg_decode_inline_) {
            std::cout << " mjpeg_inline_decode=1";
        } else {
            std::cout << " mjpeg_deferred_qbuf=1";
        }
    }
    std::cout << std::endl;
    return true;
}

void CameraVideoTrackSource::DecodeWorkerThreadMain() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "wrtc_mjpg_dec");
#endif
    while (true) {
        MjpegPendingBuf job{};
        bool have_job = false;
        bool shutdown_skip_decode = false;
        {
            std::unique_lock<std::mutex> lk(jpeg_queue_mu_);
            jpeg_queue_cv_.wait(lk, [this] { return decode_worker_exit_ || !jpeg_queue_.empty(); });
            if (!jpeg_queue_.empty()) {
                job = jpeg_queue_.front();
                jpeg_queue_.pop_front();
                have_job = true;
                shutdown_skip_decode = decode_worker_exit_;
            } else if (decode_worker_exit_) {
                break;
            }
        }
        if (!have_job) {
            continue;
        }
        if (!shutdown_skip_decode && job.index < direct_mmap_.size() && direct_mmap_[job.index] &&
            job.bytesused > 0) {
            const uint8_t* src = static_cast<const uint8_t*>(direct_mmap_[job.index]);
            const int64_t decode_queue_wait_us =
                (job.enqueue_time_us > 0) ? (webrtc::TimeMicros() - job.enqueue_time_us) : 0;
            ProcessV4l2CapturedFrame(job.index, src, job.bytesused, job.dq_time_us, job.v4l2_timestamp_us,
                                     job.poll_wait_us, job.dqbuf_ioctl_us, decode_queue_wait_us);
        }
        QBufV4l2Index(job.index);
    }
}

void CameraVideoTrackSource::ProcessV4l2CapturedFrame(unsigned int buf_index,
                                                      const uint8_t* src,
                                                      size_t bytesused,
                                                      int64_t dq_time_us,
                                                      int64_t v4l2_timestamp_us,
                                                      int64_t poll_wait_us,
                                                      int64_t dqbuf_ioctl_us,
                                                      int64_t decode_queue_wait_us) {
    if (!src || bytesused == 0) {
        return;
    }
    // 供 WEBRTC_MJPEG_TO_H264_TRACE：与编码器内 TimeMicros 差值 = 从进入本函数（MJPEG 已在 mmap）到 H264 输出的大致链路耗时（含解码、VSE 排队、编码）。
    const int64_t pipeline_t0_us = webrtc::TimeMicros();
    const int w = direct_cap_w_;
    const int h = direct_cap_h_;
    int dma_fd = -1;
    size_t dma_cap = 0;
    if (buf_index < direct_expbuf_fd_.size() && buf_index < direct_mmap_len_.size()) {
        dma_fd = direct_expbuf_fd_[buf_index];
        dma_cap = direct_mmap_len_[buf_index];
    }
    bool ok = false;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    const bool mpp_jpeg_dma =
        (dma_fd >= 0 && dma_cap >= bytesused && (WantV4l2ExtDmabufToMpp() || WantMjpegRgaToMpp()));
    const int dma_arg_fd = mpp_jpeg_dma ? dma_fd : -1;
    const size_t dma_arg_cap = mpp_jpeg_dma ? dma_cap : 0;
    if (mjpeg_mpp_ && direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG)) {
        if (PreferMjpegNativeZeroCopyToEnc()) {
            webrtc::scoped_refptr<MppNativeDecFrameBuffer> native;
            // 始终传 mmap 指针：RGA 失败时会 memcpy 回退；EXT_DMA 成功时解码器忽略指针。
            const bool dec_native =
                mjpeg_mpp_->DecodeJpegToNativeDecFrame(src, bytesused, w, h, &native, dma_arg_fd, dma_arg_cap,
                                                       dq_time_us, v4l2_timestamp_us, poll_wait_us, dqbuf_ioctl_us,
                                                       decode_queue_wait_us);
            if (dec_native && native) {
                webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                               .set_video_frame_buffer(native)
                                               .set_timestamp_us(pipeline_t0_us)
                                               .set_rotation(webrtc::kVideoRotation_0)
                                               .build();
                OnFrame(frame);
                return;
            }
        }
        EnsureNv12Pool(w, h);
        if (!nv12_pool_.empty()) {
            webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 =
                nv12_pool_[nv12_ring_next_ % nv12_pool_.size()];
            ++nv12_ring_next_;
            const int64_t decode_before_us = webrtc::TimeMicros();
            const bool dec_ok =
                mjpeg_mpp_->DecodeJpegToNV12(src, bytesused, w, h, nv12.get(), dma_arg_fd, dma_arg_cap);
            const int64_t decode_after_us = webrtc::TimeMicros();
            if (dec_ok) {
                LogMjpegDecodeTiming("mpp-nv12-pool", decode_before_us, decode_after_us);
                webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                               .set_video_frame_buffer(nv12)
                                               .set_timestamp_us(pipeline_t0_us)
                                               .set_rotation(webrtc::kVideoRotation_0)
                                               .build();
                OnFrame(frame);
                return;
            }
        } else {
            webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 = webrtc::NV12Buffer::Create(w, h);
            const int64_t decode_before_us = webrtc::TimeMicros();
            const bool dec_ok =
                nv12 && mjpeg_mpp_->DecodeJpegToNV12(src, bytesused, w, h, nv12.get(), dma_arg_fd, dma_arg_cap);
            const int64_t decode_after_us = webrtc::TimeMicros();
            if (dec_ok) {
                LogMjpegDecodeTiming("mpp-nv12-alloc", decode_before_us, decode_after_us);
                webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                               .set_video_frame_buffer(nv12)
                                               .set_timestamp_us(pipeline_t0_us)
                                               .set_rotation(webrtc::kVideoRotation_0)
                                               .build();
                OnFrame(frame);
                return;
            }
        }
    }
#endif
    webrtc::scoped_refptr<webrtc::I420Buffer> i420 = webrtc::I420Buffer::Create(w, h);
    const bool log_libyuv_mjpeg =
        (direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG));
    int64_t decode_before_us = 0;
    if (log_libyuv_mjpeg) {
        decode_before_us = webrtc::TimeMicros();
    }
    int conv_ret = 0;
    {
        const webrtc::VideoType vtype = (direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG))
                                              ? webrtc::VideoType::kMJPEG
                                              : webrtc::VideoType::kYUY2;
        conv_ret = libyuv::ConvertToI420(src, bytesused, i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                                         i420->StrideU(), i420->MutableDataV(), i420->StrideV(), 0, 0, w, h, w, h,
                                         libyuv::kRotate0, webrtc::ConvertVideoType(vtype));
        int conv = conv_ret;
        if (conv != 0 && direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG)) {
            conv = libyuv::MJPGToI420(src, bytesused, i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                                      i420->StrideU(), i420->MutableDataV(), i420->StrideV(), w, h, w, h);
        }
        ok = (conv == 0);
    }
    if (!ok) {
        static std::atomic<unsigned> conv_fail_n{0};
        const unsigned n = ++conv_fail_n;
        if ((n <= 5u) || ((n % 30u) == 0u)) {
            std::cerr << "[CameraV4L2] frame convert failed pixfmt=0x" << std::hex << direct_pixfmt_ << std::dec
                      << " bytes=" << bytesused << " size=" << w << "x" << h << " conv_ret=" << conv_ret
                      << " (frame dropped)" << std::endl;
        }
    }
    if (ok && log_libyuv_mjpeg) {
        LogMjpegDecodeTiming("libyuv-i420", decode_before_us, webrtc::TimeMicros());
    }
    if (ok) {
        webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                       .set_video_frame_buffer(i420)
                                       .set_timestamp_us(pipeline_t0_us)
                                       .set_rotation(webrtc::kVideoRotation_0)
                                       .build();
        OnFrame(frame);
    }
}

void CameraVideoTrackSource::DirectCaptureThreadMain() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "wrtc_v4l2_cap");
#endif
    std::atomic<unsigned> poll_log_counter{0};
    while (direct_run_.load(std::memory_order_relaxed)) {
        struct pollfd pfd {};
        pfd.fd = direct_fd_;
        pfd.events = POLLIN;
        const auto poll_t0 = std::chrono::steady_clock::now();
        int pr = poll(&pfd, 1, v4l2_poll_timeout_ms_);
        const int64_t poll_wait_us = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poll_t0).count());
        if (LatencyTraceEnabled() && pr > 0) {
            const auto poll_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - poll_t0)
                                     .count();
            const unsigned n = ++poll_log_counter;
            if ((n % 90u) == 0u && poll_ms > 3.0) {
                std::cout << "[Latency] V4L2 poll→readable wait_ms=" << poll_ms << " (frame#" << n << ")\n";
            }
        }
        if (pr <= 0) {
            continue;
        }
        struct v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        const int64_t dq_ioctl_t0_us = webrtc::TimeMicros();
        if (ioctl(direct_fd_, VIDIOC_DQBUF, &buf) < 0) {
            continue;
        }
        const int64_t dq_time_us = webrtc::TimeMicros();
        const int64_t dqbuf_ioctl_us = dq_time_us - dq_ioctl_t0_us;
        const int64_t v4l2_timestamp_us =
            static_cast<int64_t>(buf.timestamp.tv_sec) * webrtc::kNumMicrosecsPerSec + buf.timestamp.tv_usec;
        if (buf.index >= direct_mmap_.size() || !direct_mmap_[buf.index]) {
            ioctl(direct_fd_, VIDIOC_QBUF, &buf);
            continue;
        }
        const uint8_t* src = static_cast<const uint8_t*>(direct_mmap_[buf.index]);
        if (direct_pixfmt_ == static_cast<uint32_t>(V4L2_PIX_FMT_MJPEG)) {
            if (buf.bytesused == 0) {
                ioctl(direct_fd_, VIDIOC_QBUF, &buf);
                continue;
            }
            if (mjpeg_decode_inline_) {
                static std::atomic<unsigned> inline_counter{0};
                const int64_t t0_us = webrtc::TimeMicros();
                ProcessV4l2CapturedFrame(buf.index, src, buf.bytesused, dq_time_us, v4l2_timestamp_us, poll_wait_us,
                                         dqbuf_ioctl_us, 0);
                if (LatencyTraceEnabled()) {
                    const unsigned n = ++inline_counter;
                    if ((n % 30u) == 0u) {
                        const double ms = static_cast<double>(webrtc::TimeMicros() - t0_us) / 1000.0;
                        std::cout << "[Latency] MJPEG inline decode+OnFrame ms=" << ms << " (sample#" << n << ")\n";
                    }
                }
                ioctl(direct_fd_, VIDIOC_QBUF, &buf);
                continue;
            }
            // 延迟 QBUF：解码线程从 mmap 读 JPEG 并入 MPP 后再归还驱动，去掉「整帧 memcpy 到队列」。
            {
                std::deque<MjpegPendingBuf> dropped;
                {
                    std::unique_lock<std::mutex> lk(jpeg_queue_mu_);
                    if (mjpeg_queue_latest_only_) {
                        dropped.swap(jpeg_queue_);
                    } else {
                        while (jpeg_queue_.size() >= mjpeg_queue_max_) {
                            dropped.push_back(jpeg_queue_.front());
                            jpeg_queue_.pop_front();
                        }
                    }
                    jpeg_queue_.push_back(
                        MjpegPendingBuf{buf.index, buf.bytesused, dq_time_us, v4l2_timestamp_us, poll_wait_us,
                                       dqbuf_ioctl_us, webrtc::TimeMicros()});
                }
                for (const MjpegPendingBuf& drop : dropped) {
                    QBufV4l2Index(drop.index);
                }
            }
            jpeg_queue_cv_.notify_one();
            continue;
        }
        ProcessV4l2CapturedFrame(buf.index, src, buf.bytesused, dq_time_us, v4l2_timestamp_us, poll_wait_us,
                                 dqbuf_ioctl_us, 0);
        ioctl(direct_fd_, VIDIOC_QBUF, &buf);
    }
}

#endif  // WEBRTC_LINUX && __linux__

void CameraVideoTrackSource::Stop() {
    negotiated_capture_fps_ = 0;
#if defined(WEBRTC_LINUX) && defined(__linux__)
    StopDirectV4l2();
#endif
    if (vcm_) {
        vcm_->DeRegisterCaptureDataCallback();
        vcm_->StopCapture();
        vcm_ = nullptr;
    }
    device_info_.reset();
}

bool CameraVideoTrackSource::Start(const char* device_unique_id, int width, int height, int fps,
                                   bool prefer_mpp_mjpeg_decode,
                                   const V4l2MjpegPipelineOptions* mjpeg_pipeline) {
    Stop();
    prefer_mpp_mjpeg_decode_ = prefer_mpp_mjpeg_decode;
#if defined(WEBRTC_LINUX) && defined(__linux__)
    ApplyMjpegPipelineOptions(mjpeg_pipeline);
#endif
    if (!device_unique_id || !device_unique_id[0]) {
        return false;
    }
#if defined(WEBRTC_LINUX) && defined(__linux__)
    if (strncmp(device_unique_id, "/dev/video", 10) == 0) {
        if (StartDirectV4l2(device_unique_id, width, height, fps)) {
            return true;
        }
        std::cerr << "[CameraVideoTrackSource] direct V4L2 open failed for " << device_unique_id << std::endl;
        return false;
    }
#endif
    device_info_.reset(webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!device_info_) {
        RTC_LOG(LS_ERROR) << "CreateDeviceInfo failed";
        return false;
    }
    vcm_ = webrtc::VideoCaptureFactory::Create(device_unique_id);
    if (!vcm_) {
        RTC_LOG(LS_ERROR) << "VideoCaptureFactory::Create failed for " << device_unique_id;
        return false;
    }
    vcm_->RegisterCaptureDataCallback(this);

    webrtc::VideoCaptureCapability requested;
    requested.width = width;
    requested.height = height;
    requested.maxFPS = fps;
    requested.videoType = webrtc::VideoType::kI420;

    webrtc::VideoCaptureCapability used;
    if (device_info_->GetBestMatchedCapability(device_unique_id, requested, used) < 0) {
        if (device_info_->NumberOfCapabilities(device_unique_id) > 0 &&
            device_info_->GetCapability(device_unique_id, 0, used) == 0) {
        } else {
            used = requested;
        }
    }

    if (vcm_->StartCapture(used) != 0) {
        RTC_LOG(LS_ERROR) << "StartCapture failed";
        vcm_->DeRegisterCaptureDataCallback();
        vcm_ = nullptr;
        return false;
    }
    negotiated_capture_fps_ = used.maxFPS > 0 ? used.maxFPS : (fps > 0 ? fps : 30);
    return true;
}

void CameraVideoTrackSource::OnFrame(const webrtc::VideoFrame& frame) {
#if defined(WEBRTC_LINUX) && defined(__linux__) && defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    if (MppNativeDecFrameBuffer* native = MppNativeDecFrameBuffer::TryGet(frame.video_frame_buffer())) {
        native->SetOnFrameEnterUs(webrtc::TimeMicros());
    }
#endif
    captured_frames_.fetch_add(1, std::memory_order_relaxed);
    AdaptedVideoTrackSource::OnFrame(frame);
}

#if defined(WEBRTC_LINUX) && defined(__linux__) && defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
bool CameraVideoTrackSource::WantV4l2ExtDmabufToMpp() const {
    if (const char* e = std::getenv("WEBRTC_MJPEG_V4L2_DMABUF")) {
        return e[0] != '0';
    }
    return v4l2_ext_dma_config_;
}

bool CameraVideoTrackSource::WantMjpegRgaToMpp() const {
#if defined(WEBRTC_DEMO_HAVE_LIBRGA)
    if (const char* e = std::getenv("WEBRTC_MJPEG_RGA_TO_MPP")) {
        return e[0] != '0';
    }
    return mjpeg_rga_config_;
#else
    (void)mjpeg_rga_config_;
    return false;
#endif
}
#endif

}  // namespace webrtc_demo
