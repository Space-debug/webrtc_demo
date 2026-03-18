#include "sdl_display.h"

#include <SDL.h>
#include <cstring>
#include <iostream>
#include <vector>

namespace p2p {

// 使用 SDL 的 C 接口，避免 ABI 问题
#define SDL_WINDOW   reinterpret_cast<SDL_Window*>(window_)
#define SDL_RENDERER reinterpret_cast<SDL_Renderer*>(renderer_)
#define SDL_TEXTURE  reinterpret_cast<SDL_Texture*>(texture_)

SdlDisplay::SdlDisplay(const std::string& title) : title_(title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "[SdlDisplay] SDL_Init failed: " << SDL_GetError() << std::endl;
        return;
    }
    window_ = SDL_CreateWindow(
        title_.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_RESIZABLE);
    if (!window_) {
        std::cerr << "[SdlDisplay] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return;
    }
    renderer_ = SDL_CreateRenderer(SDL_WINDOW, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer_) {
        std::cerr << "[SdlDisplay] SDL_CreateRenderer failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(SDL_WINDOW);
        window_ = nullptr;
        SDL_Quit();
        return;
    }
    std::cout << "[SdlDisplay] 窗口已创建，等待首帧..." << std::endl;
}

SdlDisplay::~SdlDisplay() {
    if (texture_) {
        SDL_DestroyTexture(SDL_TEXTURE);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(SDL_RENDERER);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(SDL_WINDOW);
        window_ = nullptr;
    }
    SDL_Quit();
}

void SdlDisplay::UpdateFrame(const uint8_t* argb, int width, int height, int stride) {
    if (!argb || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t size = static_cast<size_t>(stride) * height;
    if (frame_buffer_.size() != size || frame_width_ != width || frame_height_ != height) {
        frame_buffer_.resize(size);
        frame_width_ = width;
        frame_height_ = height;
    }
    memcpy(frame_buffer_.data(), argb, size);
    frame_dirty_ = true;
}

bool SdlDisplay::PollAndRender() {
    if (!window_) return false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) return false;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) return false;
    }

    // 渲染最新帧
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame_width_ > 0 && frame_height_ > 0) {
            // 分辨率变化时重建 texture
            if (!texture_) {
                texture_ = SDL_CreateTexture(SDL_RENDERER,
                    SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING,
                    frame_width_, frame_height_);
                if (!texture_) {
                    std::cerr << "[SdlDisplay] SDL_CreateTexture failed: " << SDL_GetError() << std::endl;
                }
            }
            if (texture_) {
                int tex_w, tex_h;
                SDL_QueryTexture(SDL_TEXTURE, nullptr, nullptr, &tex_w, &tex_h);
                if (tex_w != frame_width_ || tex_h != frame_height_) {
                    SDL_DestroyTexture(SDL_TEXTURE);
                    texture_ = SDL_CreateTexture(SDL_RENDERER,
                        SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        frame_width_, frame_height_);
                }
                if (texture_ && frame_dirty_) {
                    SDL_UpdateTexture(SDL_TEXTURE, nullptr, frame_buffer_.data(), frame_width_ * 4);
                    frame_dirty_ = false;
                }
            }
        }
    }

    SDL_SetRenderDrawColor(SDL_RENDERER, 0, 0, 0, 255);
    SDL_RenderClear(SDL_RENDERER);
    if (texture_) {
        SDL_RenderCopy(SDL_RENDERER, SDL_TEXTURE, nullptr, nullptr);
    }
    SDL_RenderPresent(SDL_RENDERER);
    return true;
}

}  // namespace p2p
