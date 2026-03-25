#ifndef CAPTURE_BRIDGE_H
#define CAPTURE_BRIDGE_H

#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace webrtc_demo {

/// V4L2 采集桥接：从真实摄像头以指定格式（YUYV/MJPEG）采集，输出到 v4l2loopback
/// 供 libwebrtc 从 loopback 设备读取。MJPEG 会先解码再转为 YUYV 输出。
class CaptureBridge {
public:
    /// 采集格式
    enum class Format { Auto, YUYV, MJPEG };

    struct Config {
        std::string source_device;   /// 源摄像头，如 /dev/video11
        std::string loopback_device; /// v4l2loopback 设备，如 /dev/video12
        Format format{Format::YUYV};
        int width{640};
        int height{480};
        int fps{30};
    };

    explicit CaptureBridge(const Config& config);
    ~CaptureBridge();

    CaptureBridge(const CaptureBridge&) = delete;
    CaptureBridge& operator=(const CaptureBridge&) = delete;

    /// 启动桥接（后台线程采集并写入 loopback）
    bool Start();
    void Stop();

    bool IsRunning() const { return running_; }
    unsigned long GetFrameCount() const { return frame_count_; }

    /// 仅 MJPEG 且已成功解码过至少一帧时返回 true；*last_ms/*avg_ms 为 turbo 解压+I420→YUYV 耗时(ms)。
    bool GetJpegDecodeTimingMs(double* last_ms, double* avg_ms) const;

    /// 从字符串解析格式：yuyv, mjpeg, auto
    static Format ParseFormat(const std::string& s);

private:
    void CaptureLoop();

    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> startup_done_{false};
    std::atomic<bool> startup_ok_{false};
    std::atomic<unsigned long> frame_count_{0};
#if defined(HAVE_TURBOJPEG)
    std::atomic<uint64_t> jpeg_decode_last_us_{0};
    std::atomic<uint64_t> jpeg_decode_sum_us_{0};
    std::atomic<unsigned> jpeg_decode_frames_{0};
    std::atomic<bool> jpeg_decode_has_sample_{false};
#endif
    std::unique_ptr<std::thread> thread_;
};

}  // namespace webrtc_demo

#endif  // CAPTURE_BRIDGE_H
