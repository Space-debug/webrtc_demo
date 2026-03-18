#ifndef P2P_PLAYER_SDL_DISPLAY_H
#define P2P_PLAYER_SDL_DISPLAY_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace p2p {

/// SDL2 视频显示窗口，线程安全：可从任意线程调用 UpdateFrame
class SdlDisplay {
public:
    explicit SdlDisplay(const std::string& title = "WebRTC P2P 拉流");
    ~SdlDisplay();

    SdlDisplay(const SdlDisplay&) = delete;
    SdlDisplay& operator=(const SdlDisplay&) = delete;

    /// 更新当前帧（ARGB，stride=width*4），可从 WebRTC 回调线程调用
    void UpdateFrame(const uint8_t* argb, int width, int height, int stride);

    /// 处理事件并渲染，返回 false 表示应退出
    bool PollAndRender();

    bool IsOpen() const { return window_ != nullptr; }

private:
    void* window_{nullptr};
    void* renderer_{nullptr};
    void* texture_{nullptr};
    std::string title_;

    std::mutex mutex_;
    std::vector<uint8_t> frame_buffer_;
    int frame_width_{0};
    int frame_height_{0};
    bool frame_dirty_{false};
};

}  // namespace p2p

#endif  // P2P_PLAYER_SDL_DISPLAY_H
