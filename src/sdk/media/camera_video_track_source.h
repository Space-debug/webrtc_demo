#ifndef WEBRTC_DEMO_CAMERA_VIDEO_TRACK_SOURCE_H_
#define WEBRTC_DEMO_CAMERA_VIDEO_TRACK_SOURCE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(WEBRTC_LINUX) && defined(__linux__)
#include <condition_variable>
#include <deque>
#include <mutex>
#endif

#include "api/scoped_refptr.h"
#include "api/video/video_sink_interface.h"
#include "media/base/adapted_video_track_source.h"
#include "modules/video_capture/video_capture.h"

#if defined(WEBRTC_LINUX) && defined(__linux__)
#include "api/video/nv12_buffer.h"
#endif

namespace webrtc_demo {

/// Linux 直采 MJPEG 时的队列与 NV12 池参数（传入 nullptr 则用默认值）。
struct V4l2MjpegPipelineOptions {
    bool mjpeg_queue_latest_only = false;
    int mjpeg_queue_max = 8;
    int nv12_pool_slots = 6;
    /// V4L2 MMAP 缓冲个数；低延迟可设 2（需解码跟得上采集）。范围 2～32。
    int v4l2_buffer_count = 2;
    /// poll 超时毫秒；有数据即返回，仅影响无数据时的唤醒间隔。
    int v4l2_poll_timeout_ms = 50;
    /// 为 true 时不使用解码工作线程，MJPEG 在采集线程内解码（低延迟，采集易受解码耗时影响）。
    bool mjpeg_decode_inline = false;
    /// 为 true 时默认启用 V4L2 MJPEG → MPP EXT_DMA（环境变量 WEBRTC_MJPEG_V4L2_DMABUF 未设置时生效；设置了则以环境为准）。
    bool mjpeg_v4l2_ext_dma = false;
    /// 为 true 时默认启用 RGA 拷贝路径（环境变量 WEBRTC_MJPEG_RGA_TO_MPP 未设置时生效）。
    bool mjpeg_rga_to_mpp = false;
};
#if defined(WEBRTC_LINUX) && defined(__linux__) && defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
class RkMppMjpegDecoder;
#endif

/// Connects VideoCaptureModule output to AdaptedVideoTrackSource for CreateVideoTrack().
class CameraVideoTrackSource : public webrtc::AdaptedVideoTrackSource,
                               public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    CameraVideoTrackSource();
    ~CameraVideoTrackSource() override;

    CameraVideoTrackSource(const CameraVideoTrackSource&) = delete;
    CameraVideoTrackSource& operator=(const CameraVideoTrackSource&) = delete;

    /// Start V4L2 capture on device unique id (from DeviceInfo)。
    /// prefer_mpp_mjpeg_decode：Linux 且编译启用 MPP 时，MJPEG 直采可尝试硬件解码（见 USE_ROCKCHIP_MPP_MJPEG_DECODE）。
    /// mjpeg_pipeline：仅 Linux 直采 MJPEG 使用；nullptr 为默认队列与 NV12 池尺寸。
    bool Start(const char* device_unique_id, int width, int height, int fps,
               bool prefer_mpp_mjpeg_decode = true,
               const V4l2MjpegPipelineOptions* mjpeg_pipeline = nullptr);

    /// Linux 直采路径在 Start 成功后可用；与 config WIDTH/HEIGHT 对比可判断是否要改配置以减少缩放。
    bool GetNegotiatedCaptureSize(int* width, int* height) const;

    /// 直采：VIDIOC_G_PARM 读回的帧率；VCM：GetBestMatchedCapability 的 maxFPS。用于编码/WebRTC 与相机实际一致。
    bool GetNegotiatedCaptureFramerate(int* out_fps) const;

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
    int negotiated_capture_fps_{0};
    bool prefer_mpp_mjpeg_decode_{true};
#if defined(WEBRTC_LINUX) && defined(__linux__)
    bool StartDirectV4l2(const char* device_path, int width, int height, int fps);
    void StopDirectV4l2();
    void DirectCaptureThreadMain();
    /// MJPEG：拷贝压缩帧后尽快 QBUF；在此线程里解码并 OnFrame，避免采集线程被 MPP/软解拖死。
    void DecodeWorkerThreadMain();
    /// buf_index：V4L2 buffer 下标，用于 DMABUF 导入 MPP（与 mmap 同源）；YUYV/MJPEG 均传入。
    void ProcessV4l2CapturedFrame(unsigned int buf_index,
                                  const uint8_t* src,
                                  size_t bytesused,
                                  int64_t dq_time_us = 0,
                                  int64_t v4l2_timestamp_us = 0,
                                  int64_t poll_wait_us = 0,
                                  int64_t dqbuf_ioctl_us = 0,
                                  int64_t decode_queue_wait_us = 0);
    void ApplyMjpegPipelineOptions(const V4l2MjpegPipelineOptions* mjpeg_pipeline);
    void EnsureNv12Pool(int w, int h);
    void QBufV4l2Index(unsigned int index);
    /// MJPEG：仅传 mmap 索引，解码后再 QBUF，避免压缩 JPEG 再 memcpy 一整份到队列。
    struct MjpegPendingBuf {
        unsigned int index{0};
        size_t bytesused{0};
        int64_t dq_time_us{0};
        int64_t v4l2_timestamp_us{0};
        int64_t poll_wait_us{0};
        int64_t dqbuf_ioctl_us{0};
        int64_t enqueue_time_us{0};
    };

    std::thread direct_thread_;
    std::thread decode_thread_;
    std::mutex jpeg_queue_mu_;
    std::condition_variable jpeg_queue_cv_;
    std::deque<MjpegPendingBuf> jpeg_queue_;
    bool decode_worker_exit_{false};
    bool mjpeg_queue_latest_only_{false};
    size_t mjpeg_queue_max_{8};
    int nv12_pool_slots_{6};
    int v4l2_buffer_count_{2};
    int v4l2_poll_timeout_ms_{50};
    std::vector<webrtc::scoped_refptr<webrtc::NV12Buffer>> nv12_pool_;
    int nv12_pool_w_{0};
    int nv12_pool_h_{0};
    size_t nv12_ring_next_{0};
    bool mjpeg_decode_inline_{false};
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    bool v4l2_ext_dma_config_{false};
    bool mjpeg_rga_config_{false};
    bool WantV4l2ExtDmabufToMpp() const;
    bool WantMjpegRgaToMpp() const;
#endif
    std::atomic<bool> direct_run_{false};
    int direct_fd_{-1};
    int direct_cap_w_{0};
    int direct_cap_h_{0};
    uint32_t direct_pixfmt_{0};
    std::vector<void*> direct_mmap_;
    std::vector<size_t> direct_mmap_len_;
    /// VIDIOC_EXPBUF 得到的 dma-buf fd，与 mmap 同一块物理内存；-1 表示未导出或失败。
    std::vector<int> direct_expbuf_fd_;
#if defined(WEBRTC_DEMO_HAVE_ROCKCHIP_MPP)
    std::unique_ptr<RkMppMjpegDecoder> mjpeg_mpp_;
#endif
#endif
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> vcm_;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info_;
};

}  // namespace webrtc_demo

#endif
