#ifndef WEBRTC_DEMO_MPP_NATIVE_DEC_FRAME_BUFFER_H_
#define WEBRTC_DEMO_MPP_NATIVE_DEC_FRAME_BUFFER_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"

namespace webrtc_demo {

/// WebRTC kNative：持有 MPP MJPEG 解码输出 MppFrame（DRM buffer），供 RkMppH264Encoder 零拷贝入参。
/// 析构时 mpp_frame_deinit，归还解码器 buffer pool。
class MppNativeDecFrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  /// 取得 frame 所有权（不再对 frame 调用 mpp_frame_deinit，由本类析构释放）。
  static webrtc::scoped_refptr<MppNativeDecFrameBuffer> CreateFromMppFrame(void* mpp_frame,
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
                                                                           int64_t wall_capture_utc_ms);

  static MppNativeDecFrameBuffer* TryGet(const webrtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer);

  webrtc::VideoFrameBuffer::Type type() const override;
  int width() const override;
  int height() const override;
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override;
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> GetMappedFrameBuffer(
      webrtc::ArrayView<Type> types) override;
  std::string storage_representation() const override;

  int hor_stride() const { return hor_stride_; }
  int ver_stride() const { return ver_stride_; }
  uint32_t mpp_fmt() const { return mpp_fmt_; }
  int64_t mjpeg_input_timestamp_us() const { return mjpeg_input_timestamp_us_; }
  int64_t dq_time_us() const { return dq_time_us_; }
  int64_t v4l2_timestamp_us() const { return v4l2_timestamp_us_; }
  int64_t poll_wait_us() const { return poll_wait_us_; }
  int64_t dqbuf_ioctl_us() const { return dqbuf_ioctl_us_; }
  int64_t decode_queue_wait_us() const { return decode_queue_wait_us_; }
  /// MJPEG 进入 MPP 解码时刻的 UTC 毫秒（webrtc::TimeUTCMillis），与单机的 TimeMicros 解耦；两台 PC 需 NTP 同步才有可比性。
  int64_t wall_capture_utc_ms() const { return wall_capture_utc_ms_; }
  /// CameraVideoTrackSource::OnFrame 入口打点，供测量 Track→Encoder 排队延迟。
  void SetOnFrameEnterUs(int64_t t_us) {
    on_frame_enter_us_.store(t_us, std::memory_order_relaxed);
  }
  int64_t on_frame_enter_us() const { return on_frame_enter_us_.load(std::memory_order_relaxed); }
  void* mpp_frame() const { return frame_; }
  /// 与 mpp_frame_get_buffer 一致，供编码器绑定输入帧。
  void* mpp_buffer_handle() const;

  // 供 RefCountedObject 包装；外部请用 CreateFromMppFrame。
  MppNativeDecFrameBuffer(void* mpp_frame,
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
                          int64_t wall_capture_utc_ms);

 protected:
  ~MppNativeDecFrameBuffer() override;

 private:
  void* frame_;
  int width_;
  int height_;
  int hor_stride_;
  int ver_stride_;
  uint32_t mpp_fmt_;
  int64_t mjpeg_input_timestamp_us_;
  int64_t dq_time_us_;
  int64_t v4l2_timestamp_us_;
  int64_t poll_wait_us_;
  int64_t dqbuf_ioctl_us_;
  int64_t decode_queue_wait_us_;
  int64_t wall_capture_utc_ms_;
  std::atomic<int64_t> on_frame_enter_us_{0};
};

}  // namespace webrtc_demo

#endif
