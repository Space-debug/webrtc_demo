#ifndef WEBRTC_DEMO_CAMERA_VIDEO_TRACK_SOURCE_H_
#define WEBRTC_DEMO_CAMERA_VIDEO_TRACK_SOURCE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/video_sink_interface.h"
#include "media/base/adapted_video_track_source.h"
#include "modules/video_capture/video_capture.h"

namespace webrtc_demo {

/// Connects VideoCaptureModule output to AdaptedVideoTrackSource for CreateVideoTrack().
class CameraVideoTrackSource : public webrtc::AdaptedVideoTrackSource,
                               public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    CameraVideoTrackSource();
    ~CameraVideoTrackSource() override;

    CameraVideoTrackSource(const CameraVideoTrackSource&) = delete;
    CameraVideoTrackSource& operator=(const CameraVideoTrackSource&) = delete;

    /// Start V4L2 capture on device unique id (from DeviceInfo).
    bool Start(const char* device_unique_id, int width, int height, int fps);

    /// Linux 直采路径在 Start 成功后可用；与 config WIDTH/HEIGHT 对比可判断是否要改配置以减少缩放。
    bool GetNegotiatedCaptureSize(int* width, int* height) const;

    void Stop();

    void OnFrame(const webrtc::VideoFrame& frame) override;

    /// 每帧进入 AdaptedVideoTrackSource::OnFrame 的次数（直采与 VCM 共用；用于采集门限，不依赖 VideoSink）。
    uint32_t CapturedFrameCount() const {
        return captured_frames_.load(std::memory_order_relaxed);
    }

    webrtc::MediaSourceInterface::SourceState state() const override {
        return webrtc::MediaSourceInterface::kLive;
    }
    bool remote() const override { return false; }

    bool is_screencast() const override { return false; }
    std::optional<bool> needs_denoising() const override { return std::nullopt; }

private:
    std::atomic<uint32_t> captured_frames_{0};
#if defined(WEBRTC_LINUX) && defined(__linux__)
    bool StartDirectV4l2(const char* device_path, int width, int height, int fps);
    void StopDirectV4l2();
    void DirectCaptureThreadMain();

    std::thread direct_thread_;
    std::atomic<bool> direct_run_{false};
    int direct_fd_{-1};
    int direct_cap_w_{0};
    int direct_cap_h_{0};
    uint32_t direct_pixfmt_{0};
    std::vector<void*> direct_mmap_;
    std::vector<size_t> direct_mmap_len_;
#endif
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> vcm_;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info_;
};

}  // namespace webrtc_demo

#endif
