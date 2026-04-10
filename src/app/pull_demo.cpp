#include "pull_subscriber.h"
#include "config_loader.h"

#include <SDL.h>
#include "rtc_base/time_utils.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// 拉流窗口（仅非 headless），与可执行文件同编译单元，避免多文件
class SdlDisplay {
public:
    explicit SdlDisplay(const std::string& title) : title_(title) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::cerr << "[SdlDisplay] SDL_Init failed: " << SDL_GetError() << std::endl;
            return;
        }
        window_ = SDL_CreateWindow(title_.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
                                   SDL_WINDOW_RESIZABLE);
        if (!window_) {
            std::cerr << "[SdlDisplay] SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
            SDL_Quit();
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            const std::string accel_err = SDL_GetError();
            SDL_ClearError();
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
            if (!renderer_) {
                std::cerr << "[SdlDisplay] SDL_CreateRenderer failed (accelerated: " << accel_err
                          << "; software: " << SDL_GetError() << ")" << std::endl;
                SDL_DestroyWindow(window_);
                window_ = nullptr;
                SDL_Quit();
                return;
            }
            std::cout << "[SdlDisplay] Using software renderer (no accelerated driver; OK for display / E2E test)"
                      << std::endl;
        } else {
            std::cout << "[SdlDisplay] Using accelerated renderer" << std::endl;
        }
        std::cout << "[SdlDisplay] Window created, waiting for first frame..." << std::endl;
    }
    ~SdlDisplay() {
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        SDL_Quit();
    }
    SdlDisplay(const SdlDisplay&) = delete;
    SdlDisplay& operator=(const SdlDisplay&) = delete;

    void UpdateFrame(const uint8_t* argb, int width, int height, int stride, uint16_t trace_id,
                     int64_t t_sink_callback_done_us) {
        if (!argb || width <= 0 || height <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        size_t size = static_cast<size_t>(stride) * height;
        if (frame_buffer_.size() != size || frame_width_ != width || frame_height_ != height) {
            frame_buffer_.resize(size);
            frame_width_ = width;
            frame_height_ = height;
        }
        std::memcpy(frame_buffer_.data(), argb, size);
        frame_trace_id_ = trace_id;
        frame_sink_callback_done_us_ = t_sink_callback_done_us;
        frame_dirty_ = true;
    }

    bool PollAndRender() {
        if (!window_) return false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) return false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) return false;
        }
        // 仅在收到新帧时更新纹理并提交显示，避免空转 render loop 带来的调度扰动。
        bool has_new_frame = false;
        uint16_t trace_id = 0;
        int64_t sink_done_us = 0;
        int frame_w = 0;
        int frame_h = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frame_width_ <= 0 || frame_height_ <= 0 || !frame_dirty_) {
                return true;
            }
            has_new_frame = true;
            trace_id = frame_trace_id_;
            sink_done_us = frame_sink_callback_done_us_;
            frame_w = frame_width_;
            frame_h = frame_height_;
            if (!texture_) {
                texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                             frame_w, frame_h);
            }
            if (texture_) {
                int tex_w = 0, tex_h = 0;
                SDL_QueryTexture(texture_, nullptr, nullptr, &tex_w, &tex_h);
                if (tex_w != frame_w || tex_h != frame_h) {
                    SDL_DestroyTexture(texture_);
                    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                                 frame_w, frame_h);
                }
            }
            if (texture_) {
                SDL_UpdateTexture(texture_, nullptr, frame_buffer_.data(), frame_w * 4);
                frame_dirty_ = false;
            }
        }
        if (!has_new_frame || !texture_) {
            return true;
        }
        if (const char* e2e = std::getenv("WEBRTC_E2E_LATENCY_TRACE"); e2e && e2e[0] == '1') {
            const int64_t t_present_submit_us = webrtc::TimeMicros();
            std::cout << "[E2E_UI] trace_id=" << static_cast<unsigned>(trace_id)
                      << " t_sink_callback_done_us=" << sink_done_us
                      << " t_present_submit_us=" << t_present_submit_us << std::endl;
        }
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (texture_) {
            SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        }
        SDL_RenderPresent(renderer_);
        return true;
    }

    bool IsOpen() const { return window_ != nullptr; }

private:
    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_Texture* texture_{nullptr};
    std::string title_;
    std::mutex mutex_;
    std::vector<uint8_t> frame_buffer_;
    int frame_width_{0};
    int frame_height_{0};
    bool frame_dirty_{false};
    uint16_t frame_trace_id_{0};
    int64_t frame_sink_callback_done_us_{0};
};

}  // namespace

static webrtc_demo::PullSubscriber* g_player = nullptr;

void SignalHandler(int) {
    if (g_player) g_player->Stop();
}

static std::string FindConfigPath(const char* prog, const std::string& config_arg) {
    if (!config_arg.empty()) return config_arg;
    std::string prog_path(prog);
    size_t slash = prog_path.find_last_of("/\\");
    std::string dir = (slash != std::string::npos) ? prog_path.substr(0, slash) : ".";
    for (const char* sub : {"/config/streams.conf", "/../config/streams.conf", "/../../config/streams.conf"}) {
        std::string path = dir + sub;
        std::ifstream f(path);
        if (f) return path;
    }
    return "";
}

struct HeadlessDecodeTiming {
    std::mutex mu;
    std::chrono::steady_clock::time_point first{};
    std::chrono::steady_clock::time_point last{};
    bool have_first{false};
};

static void PrintHeadlessFpsSummary(std::ostream& out, unsigned total_frames, double span_sec) {
    out << std::fixed << std::setprecision(3);
    if (total_frames >= 2 && span_sec > 1e-9) {
        const double fps = static_cast<double>(total_frames - 1) / span_sec;
        out << "[Headless] FPS: decode span " << span_sec << " s, frames " << total_frames
            << ", avg FPS " << std::setprecision(2) << fps << " ((N-1)/delta_t)\n";
    } else {
        out << "[Headless] FPS: not enough frames or span too short\n";
    }
}

static void PrintPlayerUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [signaling_host:port] [stream_id]\n"
              << "Options:\n"
              << "  --config FILE      Optional: SIGNALING_ADDR, STREAM_ID\n"
              << "  --headless         No window; log decoded frames\n"
              << "  --frames N         Headless: exit OK after N frames (default 30); 0 = run until disconnect\n"
              << "  --timeout-sec S    Headless timeout seconds (default 120; ignored when --frames 0)\n"
              << "  --strict-fps       Headless: check avg FPS from decode callbacks\n"
              << "  --expect-fps F     With --strict-fps, expected FPS (default 30)\n"
              << "  --fps-tol R        Relative tol; pass band [F*(1-R), F*(1+R)] (default 0.12)\n"
              << "  --fps-min-sec T    Strict: min span first-to-last frame in s (default 10)\n"
              << "  --fps-min-frames M Strict: min frames (default 150, >= --frames)\n"
              << "  --video-stats-interval-sec S  Periodically log inbound video stats (goog_timing_frame_info\n"
              << "                               etc.). S=0 disables. Default: headless 2, windowed 0.\n"
              << "  --sink-argb        Headless: 仍做 I420→ARGB（默认无头跳过转换以降客户端延迟）\n"
              << "  -h, --help         This help\n"
              << "Env: WEBRTC_PULL_SKIP_ARGB=0|1 覆盖无头是否跳过 ARGB；WEBRTC_PULL_JITTER_MIN_DELAY_MS\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::string url = "127.0.0.1:8765";
    std::string stream_id = "livestream";
    std::string config_path;
    bool explicit_headless = false;
    bool headless = false;
    bool frames_from_cli = false;
    bool timeout_from_cli = false;
    unsigned need_frames = 30;
    int timeout_sec = 120;
    bool strict_fps = false;
    double expect_fps = 30.0;
    double fps_rel_tol = 0.12;
    double fps_min_measure_sec = 10.0;
    unsigned fps_min_frames = 150;
    double video_stats_interval_sec = -1.0;
    bool force_sink_argb = false;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintPlayerUsage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--headless") == 0) {
            explicit_headless = true;
            headless = true;
            ++i;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames_from_cli = true;
            need_frames = static_cast<unsigned>(std::atoi(argv[i + 1]));
            i += 2;
        } else if (strcmp(argv[i], "--timeout-sec") == 0 && i + 1 < argc) {
            timeout_from_cli = true;
            timeout_sec = std::atoi(argv[i + 1]);
            if (timeout_sec < 5) timeout_sec = 5;
            i += 2;
        } else if (strcmp(argv[i], "--strict-fps") == 0) {
            strict_fps = true;
            ++i;
        } else if (strcmp(argv[i], "--expect-fps") == 0 && i + 1 < argc) {
            expect_fps = std::atof(argv[i + 1]);
            if (expect_fps <= 0.0) expect_fps = 30.0;
            i += 2;
        } else if (strcmp(argv[i], "--fps-tol") == 0 && i + 1 < argc) {
            fps_rel_tol = std::atof(argv[i + 1]);
            if (fps_rel_tol < 0.0) fps_rel_tol = 0.0;
            if (fps_rel_tol > 0.5) fps_rel_tol = 0.5;
            i += 2;
        } else if (strcmp(argv[i], "--fps-min-sec") == 0 && i + 1 < argc) {
            fps_min_measure_sec = std::atof(argv[i + 1]);
            if (fps_min_measure_sec < 1.0) fps_min_measure_sec = 1.0;
            i += 2;
        } else if (strcmp(argv[i], "--fps-min-frames") == 0 && i + 1 < argc) {
            fps_min_frames = static_cast<unsigned>(std::atoi(argv[i + 1]));
            if (fps_min_frames < 2) fps_min_frames = 2;
            i += 2;
        } else if (strcmp(argv[i], "--video-stats-interval-sec") == 0 && i + 1 < argc) {
            video_stats_interval_sec = std::atof(argv[i + 1]);
            if (video_stats_interval_sec < 0.0) {
                video_stats_interval_sec = 0.0;
            }
            i += 2;
        } else if (strcmp(argv[i], "--sink-argb") == 0) {
            force_sink_argb = true;
            ++i;
        } else {
            ++i;
        }
    }
    if (config_path.empty()) config_path = FindConfigPath(argv[0], "");
    int jitter_min_delay_ms = 0;
    webrtc_demo::ConfigLoader cfg;
    if (!config_path.empty() && cfg.Load(config_path)) {
        url = cfg.Get("SIGNALING_ADDR", "127.0.0.1:8765");
        stream_id = cfg.Get("STREAM_ID", "livestream");
    }
    if (i < argc) url = argv[i++];
    if (i < argc) stream_id = argv[i++];

    if (!explicit_headless) {
        if (const char* he = std::getenv("HEADLESS")) {
            std::string s(he);
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (s == "1" || s == "true" || s == "yes" || s == "on") {
                headless = true;
            }
        }
    }

    if (strict_fps && !headless) {
        std::cerr << "[Headless] --strict-fps needs --headless; ignored\n";
        strict_fps = false;
    }
    if (strict_fps && need_frames == 0) {
        std::cerr << "[Headless] --strict-fps requires --frames >= 1\n";
        return 1;
    }
    if (video_stats_interval_sec < 0.0) {
        video_stats_interval_sec = headless ? 2.0 : 0.0;
    }

    int ui_poll_sleep_ms = 16;
    if (const char* e2e = std::getenv("WEBRTC_E2E_LATENCY_TRACE"); e2e && e2e[0] == '1') {
        ui_poll_sleep_ms = 1;
    }
    if (const char* ui = std::getenv("WEBRTC_PULL_UI_POLL_SLEEP_MS")) {
        const int v = std::atoi(ui);
        if (v >= 0 && v <= 33) {
            ui_poll_sleep_ms = v;
        }
    }

    if (const char* jb = std::getenv("WEBRTC_PULL_JITTER_MIN_DELAY_MS")) {
        jitter_min_delay_ms = std::atoi(jb);
    }

    bool skip_sink_argb = headless && !force_sink_argb;
    if (const char* sk = std::getenv("WEBRTC_PULL_SKIP_ARGB")) {
        if (sk[0] == '0') {
            skip_sink_argb = false;
        } else if (sk[0] == '1') {
            skip_sink_argb = headless;
        }
    }

    std::cout << "=== webrtc_pull_demo" << (headless ? " (headless)" : " (SDL2)") << " ===" << std::endl;
    std::cout << "Signaling: " << url << " stream: " << stream_id << std::endl;
    std::cout << "JitterBufferMinimumDelay(ms): " << jitter_min_delay_ms << std::endl;
    if (headless) {
        std::cout << "Sink ARGB conversion: " << (skip_sink_argb ? "OFF (client low-latency)" : "ON") << std::endl;
    }
    if (video_stats_interval_sec > 0.0) {
        std::cout << "[Stats] Inbound video stats every " << video_stats_interval_sec << " s (GetStats)\n";
    }
    if (!headless) {
        std::cout << "Press Esc or close window to exit" << std::endl;
        std::cout << "[SDL] UI poll sleep: " << ui_poll_sleep_ms << " ms" << std::endl;
    }
    std::unique_ptr<SdlDisplay> display;
    if (!headless) {
        display = std::make_unique<SdlDisplay>("webrtc_pull_demo");
        if (!display->IsOpen()) {
            std::cerr << "SDL display failed. On board: use local desktop session or set DISPLAY; try "
                         "SDL_VIDEODRIVER=x11 (or wayland).\n"
                      << "Packages: libsdl2-2.0-0 + display stack; libsdl2-dev is for building only.\n"
                      << "Or headless: " << argv[0] << " --headless ..." << std::endl;
            return 1;
        }
    }

    webrtc_demo::PullSubscriberConfig recv_cfg;
    recv_cfg.common.jitter_buffer_min_delay_ms = jitter_min_delay_ms;
    recv_cfg.common.skip_sink_argb_conversion = skip_sink_argb;
    webrtc_demo::PullSubscriber player(url, stream_id, recv_cfg);
    g_player = &player;

    std::signal(SIGINT, SignalHandler);

    std::atomic<unsigned> decoded_frames{0};
    auto decode_timing = std::make_shared<HeadlessDecodeTiming>();
    player.SetOnVideoFrame([&decoded_frames, &display, headless, decode_timing](const uint8_t* argb, int width,
                                                                               int height, int stride, uint16_t trace_id,
                                                                               int64_t t_sink_callback_done_us) {
        (void)argb;
        (void)stride;
        (void)trace_id;
        (void)t_sink_callback_done_us;
        unsigned n = ++decoded_frames;
        if (headless) {
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(decode_timing->mu);
                if (!decode_timing->have_first) {
                    decode_timing->first = now;
                    decode_timing->have_first = true;
                }
                decode_timing->last = now;
            }
            if (n == 1 || n <= 5 || n % 10 == 0) {
                std::cout << "[Headless] Decoded frame #" << n << " " << width << "x" << height << std::endl;
            }
        } else if (display) {
            display->UpdateFrame(argb, width, height, stride, trace_id, t_sink_callback_done_us);
        }
    });

    player.SetOnConnectionState([](webrtc_demo::PullConnectionState state) {
        const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
        std::cout << "[State] " << names[static_cast<int>(state)] << std::endl;
    });

    player.SetOnError([](const std::string& msg) {
        std::cerr << "[Error] " << msg << std::endl;
    });

    player.Play();

    const double video_stats_sec = video_stats_interval_sec;
    auto next_inbound_stats = std::chrono::steady_clock::now();
    auto pump_inbound_video_stats = [&player, video_stats_sec, &next_inbound_stats]() {
        if (video_stats_sec <= 0.0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_inbound_stats) {
            return;
        }
        next_inbound_stats =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(video_stats_sec));
        player.RequestInboundVideoStatsLog();
    };

    if (headless) {
        if (need_frames == 0) {
            std::cout << "[Headless] Running until publisher disconnect or Ctrl+C..." << std::endl;
            while (player.IsPlaying()) {
                pump_inbound_video_stats();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            g_player = nullptr;
            std::cout << "Exited." << std::endl;
            return 0;
        }
        const unsigned eff_min_frames =
            strict_fps ? std::max(need_frames, fps_min_frames) : need_frames;
        if (strict_fps) {
            std::cout << "[Headless] strict-fps: expect " << expect_fps << " FPS, tol ±" << (fps_rel_tol * 100.0)
                      << "%, need " << eff_min_frames << " frames and span >= " << fps_min_measure_sec << " s\n";
        }
        std::cout << "[Headless] Waiting for publisher offer, need " << eff_min_frames << " frames, timeout "
                  << timeout_sec << " s..." << std::endl;
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(timeout_sec);
        while (player.IsPlaying() && clock::now() < deadline) {
            const unsigned df = decoded_frames.load();
            double span_sec = 0.0;
            bool timing_ok = false;
            {
                std::lock_guard<std::mutex> lock(decode_timing->mu);
                if (decode_timing->have_first) {
                    span_sec = std::chrono::duration<double>(decode_timing->last - decode_timing->first).count();
                    timing_ok = true;
                }
            }

            if (strict_fps) {
                if (df >= eff_min_frames && df >= 2 && timing_ok && span_sec >= fps_min_measure_sec) {
                    const double fps = static_cast<double>(df - 1) / span_sec;
                    const double low = expect_fps * (1.0 - fps_rel_tol);
                    const double high = expect_fps * (1.0 + fps_rel_tol);
                    PrintHeadlessFpsSummary(std::cout, df, span_sec);
                    std::cout << std::fixed << std::setprecision(2) << "[Headless] strict-fps: band [" << low
                              << ", " << high << "] actual " << fps << std::endl;
                    if (fps >= low && fps <= high) {
                        std::cout << "[Headless] Playback check OK: " << df << " frames, FPS in band" << std::endl;
                        player.Stop();
                        g_player = nullptr;
                        return 0;
                    }
                    std::cerr << "[Headless] FPS check failed (exit 3)" << std::endl;
                    player.Stop();
                    g_player = nullptr;
                    return 3;
                }
            } else if (df >= need_frames) {
                if (timing_ok && df >= 2) {
                    PrintHeadlessFpsSummary(std::cout, df, span_sec);
                }
                std::cout << "[Headless] Playback check OK: " << df << " frames" << std::endl;
                player.Stop();
                g_player = nullptr;
                return 0;
            }
            pump_inbound_video_stats();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (player.IsPlaying()) {
            const unsigned df = decoded_frames.load();
            double span_sec = 0.0;
            bool timing_ok = false;
            {
                std::lock_guard<std::mutex> lock(decode_timing->mu);
                if (decode_timing->have_first) {
                    span_sec = std::chrono::duration<double>(decode_timing->last - decode_timing->first).count();
                    timing_ok = true;
                }
            }
            if (timing_ok && df >= 2) {
                PrintHeadlessFpsSummary(std::cerr, df, span_sec);
            }
            std::cerr << "[Headless] Timeout: got " << df << " frames (need " << eff_min_frames;
            if (strict_fps) {
                std::cerr << "; strict needs span >= " << fps_min_measure_sec << " s";
            }
            std::cerr << ")" << std::endl;
            player.Stop();
            g_player = nullptr;
            return 2;
        }
    } else {
        while (player.IsPlaying() && display->PollAndRender()) {
            pump_inbound_video_stats();
            if (ui_poll_sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ui_poll_sleep_ms));
            } else {
                std::this_thread::yield();
            }
        }

        if (player.IsPlaying()) {
            player.Stop();
        }
    }

    g_player = nullptr;
    std::cout << "Exited." << std::endl;
    return 0;
}
