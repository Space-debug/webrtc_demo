#include "capture_bridge.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(HAVE_TURBOJPEG)
#include <turbojpeg.h>
#endif

/// I420 转 YUYV（无 libyuv 依赖）
static void I420ToYUYV(const uint8_t* y, int y_stride,
                       const uint8_t* u, int u_stride,
                       const uint8_t* v, int v_stride,
                       uint8_t* dst, int dst_stride, int w, int h) {
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; col += 2) {
            int y0 = y[row * y_stride + col];
            int y1 = y[row * y_stride + col + 1];
            int uv_row = row / 2;
            int uv_col = col / 2;
            int cb = u[uv_row * u_stride + uv_col];
            int cr = v[uv_row * v_stride + uv_col];
            dst[row * dst_stride + col * 2 + 0] = static_cast<uint8_t>(y0);
            dst[row * dst_stride + col * 2 + 1] = static_cast<uint8_t>(cb);
            dst[row * dst_stride + col * 2 + 2] = static_cast<uint8_t>(y1);
            dst[row * dst_stride + col * 2 + 3] = static_cast<uint8_t>(cr);
        }
    }
}

namespace webrtc_demo {

namespace {

constexpr int kNumSourceBuffers = 4;
constexpr int kNumLoopbackBuffers = 2;

uint32_t GetV4L2Format(CaptureBridge::Format fmt) {
    switch (fmt) {
        case CaptureBridge::Format::YUYV: return V4L2_PIX_FMT_YUYV;
        case CaptureBridge::Format::MJPEG: return V4L2_PIX_FMT_MJPEG;
        default: return V4L2_PIX_FMT_YUYV;
    }
}

}  // namespace

CaptureBridge::CaptureBridge(const Config& config) : config_(config) {}

CaptureBridge::~CaptureBridge() {
    Stop();
}

CaptureBridge::Format CaptureBridge::ParseFormat(const std::string& s) {
    if (s == "yuyv") return Format::YUYV;
    if (s == "mjpeg") return Format::MJPEG;
    return Format::Auto;
}

bool CaptureBridge::Start() {
    if (running_) return true;
    stop_requested_ = false;
    startup_done_ = false;
    startup_ok_ = false;
    thread_ = std::make_unique<std::thread>(&CaptureBridge::CaptureLoop, this);
    // 等待后台线程完成初始化，避免“异步失败却返回成功”。
    for (int i = 0; i < 60; ++i) {  // 最多等待约 3 秒
        if (startup_done_) break;
        usleep(50 * 1000);
    }
    if (!startup_done_ || !startup_ok_) {
        stop_requested_ = true;
        if (thread_ && thread_->joinable()) thread_->join();
        thread_.reset();
        running_ = false;
        return false;
    }
    return true;
}

void CaptureBridge::Stop() {
    stop_requested_ = true;
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    running_ = false;
}

void CaptureBridge::CaptureLoop() {
    int src_fd = -1;
    int loop_fd = -1;
    void* src_buffers[kNumSourceBuffers] = {};
    size_t src_lengths[kNumSourceBuffers] = {};
    void* loop_buffers[kNumLoopbackBuffers] = {};
    size_t loop_lengths[kNumLoopbackBuffers] = {};
    std::vector<uint8_t> yuyv_buffer;
    std::vector<uint8_t> i420_buffer;

    auto cleanup = [&]() {
        if (loop_fd >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ioctl(loop_fd, VIDIOC_STREAMOFF, &type);
            for (int i = 0; i < kNumLoopbackBuffers; ++i) {
                if (loop_buffers[i]) munmap(loop_buffers[i], loop_lengths[i]);
            }
            close(loop_fd);
            loop_fd = -1;
        }
        if (src_fd >= 0) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(src_fd, VIDIOC_STREAMOFF, &type);
            for (int i = 0; i < kNumSourceBuffers; ++i) {
                if (src_buffers[i]) munmap(src_buffers[i], src_lengths[i]);
            }
            close(src_fd);
            src_fd = -1;
        }
    };

    // 打开源设备并设置格式
    src_fd = open(config_.source_device.c_str(), O_RDWR | O_NONBLOCK);
    if (src_fd < 0) {
        std::cerr << "[CaptureBridge] 无法打开源设备 " << config_.source_device
                  << ": " << strerror(errno) << std::endl;
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    struct v4l2_format src_fmt = {};
    src_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    src_fmt.fmt.pix.width = config_.width;
    src_fmt.fmt.pix.height = config_.height;
    src_fmt.fmt.pix.pixelformat = GetV4L2Format(config_.format);
    if (ioctl(src_fd, VIDIOC_S_FMT, &src_fmt) < 0) {
        std::cerr << "[CaptureBridge] 设置源格式失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    int actual_width = src_fmt.fmt.pix.width;
    int actual_height = src_fmt.fmt.pix.height;
    size_t yuyv_size = actual_width * actual_height * 2;

    // 设置帧率
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = config_.fps;
    ioctl(src_fd, VIDIOC_S_PARM, &parm);

    // 分配源缓冲区
    struct v4l2_requestbuffers req = {};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = kNumSourceBuffers;
    if (ioctl(src_fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[CaptureBridge] 源 REQBUFS 失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(src_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            cleanup();
            startup_done_ = true;
            startup_ok_ = false;
            return;
        }
        src_buffers[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, src_fd, buf.m.offset);
        if (src_buffers[i] == MAP_FAILED) {
            src_buffers[i] = nullptr;
            cleanup();
            startup_done_ = true;
            startup_ok_ = false;
            return;
        }
        src_lengths[i] = buf.length;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ioctl(src_fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(src_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[CaptureBridge] 源 STREAMON 失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    // 打开 loopback 设备并设置输出格式为 YUYV
    loop_fd = open(config_.loopback_device.c_str(), O_RDWR);
    if (loop_fd < 0) {
        std::cerr << "[CaptureBridge] 无法打开 loopback " << config_.loopback_device
                  << ": " << strerror(errno)
                  << "。请先执行: sudo modprobe v4l2loopback video_nr="
                  << config_.loopback_device.substr(config_.loopback_device.find("video") + 5)
                  << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    struct v4l2_format loop_fmt = {};
    loop_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    loop_fmt.fmt.pix.width = actual_width;
    loop_fmt.fmt.pix.height = actual_height;
    loop_fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    if (ioctl(loop_fd, VIDIOC_S_FMT, &loop_fmt) < 0) {
        std::cerr << "[CaptureBridge] 设置 loopback 格式失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    struct v4l2_requestbuffers loop_req = {};
    loop_req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    loop_req.memory = V4L2_MEMORY_MMAP;
    loop_req.count = kNumLoopbackBuffers;
    if (ioctl(loop_fd, VIDIOC_REQBUFS, &loop_req) < 0) {
        std::cerr << "[CaptureBridge] loopback REQBUFS 失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    for (unsigned int i = 0; i < loop_req.count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(loop_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            cleanup();
            startup_done_ = true;
            startup_ok_ = false;
            return;
        }
        loop_buffers[i] = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, loop_fd, buf.m.offset);
        if (loop_buffers[i] == MAP_FAILED) {
            loop_buffers[i] = nullptr;
            cleanup();
            startup_done_ = true;
            startup_ok_ = false;
            return;
        }
        loop_lengths[i] = buf.length;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(loop_fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[CaptureBridge] loopback STREAMON 失败: " << strerror(errno) << std::endl;
        cleanup();
        startup_done_ = true;
        startup_ok_ = false;
        return;
    }

    running_ = true;
    startup_done_ = true;
    startup_ok_ = true;
    std::cout << "[CaptureBridge] 已启动: " << config_.source_device << " -> "
              << config_.loopback_device << " (" << actual_width << "x" << actual_height
              << " " << (config_.format == Format::MJPEG ? "MJPEG" : "YUYV") << ")" << std::endl;

    bool first_loopback_frame = true;

    while (!stop_requested_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(src_fd, &fds);
        struct timeval tv = {1, 0};
        int r = select(src_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        struct v4l2_buffer src_buf = {};
        src_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        src_buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(src_fd, VIDIOC_DQBUF, &src_buf) < 0) {
            if (errno != EAGAIN) break;
            continue;
        }

        const uint8_t* src_data = static_cast<const uint8_t*>(src_buffers[src_buf.index]);
        size_t src_len = src_buf.bytesused;
        const uint8_t* out_data = src_data;
        size_t out_len = src_len;

        if (config_.format == Format::MJPEG) {
#if defined(HAVE_TURBOJPEG)
            tjhandle tj = tjInitDecompress();
            if (!tj) {
                ioctl(src_fd, VIDIOC_QBUF, &src_buf);
                continue;
            }
            int w, h, subsamp;
            tjDecompressHeader3(tj, src_data, src_len, &w, &h, &subsamp, nullptr, nullptr);
            size_t yuv_size = tjBufSizeYUV2(w, 0, h, subsamp);
            i420_buffer.resize(yuv_size);
            if (tjDecompressToYUV2(tj, src_data, src_len, i420_buffer.data(), w, 0, h, 0) == 0) {
                yuyv_buffer.resize(w * h * 2);
                int y_stride = w;
                int u_stride = (w + 1) / 2;
                int v_stride = (w + 1) / 2;
                uint8_t* y_plane = i420_buffer.data();
                uint8_t* u_plane = y_plane + w * h;
                uint8_t* v_plane = u_plane + ((w + 1) / 2) * ((h + 1) / 2);
                I420ToYUYV(y_plane, y_stride, u_plane, u_stride, v_plane, v_stride,
                           yuyv_buffer.data(), w * 2, w, h);
                out_data = yuyv_buffer.data();
                out_len = yuyv_buffer.size();
            }
            tjDestroy(tj);
#else
            std::cerr << "[CaptureBridge] MJPEG 需要 libjpeg-turbo，请安装 libjpeg-turbo8-dev 并重新编译" << std::endl;
            ioctl(src_fd, VIDIOC_QBUF, &src_buf);
            break;
#endif
        } else {
            // YUYV 直通
            out_data = src_data;
            out_len = src_len;
        }

        // 写入 loopback：首帧直接 QBUF，后续帧先 DQBUF 取回 buffer
        int out_idx = 0;
        if (!first_loopback_frame) {
            struct v4l2_buffer dq = {};
            dq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            dq.memory = V4L2_MEMORY_MMAP;
            if (ioctl(loop_fd, VIDIOC_DQBUF, &dq) != 0) continue;
            out_idx = dq.index;
        }
        if (out_len > 0 && out_len <= static_cast<size_t>(loop_lengths[out_idx])) {
            memcpy(loop_buffers[out_idx], out_data, out_len);
            struct v4l2_buffer loop_buf = {};
            loop_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            loop_buf.memory = V4L2_MEMORY_MMAP;
            loop_buf.index = out_idx;
            loop_buf.bytesused = static_cast<__u32>(out_len);
            loop_buf.field = V4L2_FIELD_NONE;
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            loop_buf.timestamp.tv_sec = tv.tv_sec;
            loop_buf.timestamp.tv_usec = tv.tv_usec;
            if (ioctl(loop_fd, VIDIOC_QBUF, &loop_buf) == 0) first_loopback_frame = false;
        }

        ioctl(src_fd, VIDIOC_QBUF, &src_buf);
        frame_count_++;
    }

    running_ = false;
    if (!startup_done_) {
        startup_done_ = true;
        startup_ok_ = false;
    }
    cleanup();
    std::cout << "[CaptureBridge] 已停止，共 " << frame_count_ << " 帧" << std::endl;
}

}  // namespace webrtc_demo
