#include "p2p_player.h"
#include "sdl_display.h"
#include "config_loader.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

static p2p::P2pPlayer* g_player = nullptr;

void SignalHandler(int) {
    if (g_player) g_player->Stop();
}

static std::string FindConfigPath(const char* prog, const std::string& config_arg) {
    if (!config_arg.empty()) return config_arg;
    std::string prog_path(prog);
    size_t slash = prog_path.find_last_of("/\\");
    std::string dir = (slash != std::string::npos) ? prog_path.substr(0, slash) : ".";
    for (const char* sub : {"/config/streams.conf", "/../../config/streams.conf"}) {
        std::string path = dir + sub;
        std::ifstream f(path);
        if (f) return path;
    }
    return "";
}

static void PrintPlayerUsage(const char* prog) {
    std::cout << "用法: " << prog << " [选项] [信令地址] [stream_id]\n"
              << "选项:\n"
              << "  --config FILE      配置文件路径\n"
              << "  --headless         无窗口：仅通过控制台统计解码帧，适合 SSH/远程\n"
              << "  --frames N         headless 模式下收到至少 N 帧即成功退出（默认 30）\n"
              << "  --timeout-sec S    headless 超时秒数（默认 120）\n"
              << "  --enable-flexfec   启用 FlexFEC-03（与 ENABLE_FLEXFEC=1 一致，推流端也需开）\n"
              << "  -h, --help         显示本帮助\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::string url = "127.0.0.1:8765";
    std::string stream_id = "livestream";
    std::string config_path;
    bool cmdline_enable_flexfec = false;
    bool headless = false;
    unsigned need_frames = 30;
    int timeout_sec = 120;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintPlayerUsage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--enable-flexfec") == 0) {
            cmdline_enable_flexfec = true;
            ++i;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
            ++i;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            need_frames = static_cast<unsigned>(std::atoi(argv[i + 1]));
            if (need_frames < 1) need_frames = 1;
            i += 2;
        } else if (strcmp(argv[i], "--timeout-sec") == 0 && i + 1 < argc) {
            timeout_sec = std::atoi(argv[i + 1]);
            if (timeout_sec < 5) timeout_sec = 5;
            i += 2;
        } else {
            ++i;
        }
    }
    if (config_path.empty()) config_path = FindConfigPath(argv[0], "");
    webrtc_demo::ConfigLoader cfg;
    if (!config_path.empty() && cfg.Load(config_path)) {
        url = cfg.Get("SIGNALING_ADDR", "127.0.0.1:8765");
        stream_id = cfg.Get("DEFAULT_STREAM", "livestream");
    }
    if (i < argc) url = argv[i++];
    if (i < argc) stream_id = argv[i++];

    bool player_flexfec = false;
    std::string player_flexfec_override;
    if (!config_path.empty() && !cfg.empty()) {
        std::string ef = cfg.GetStream(stream_id, "ENABLE_FLEXFEC", cfg.Get("ENABLE_FLEXFEC", "0"));
        for (auto& c : ef) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        player_flexfec = (ef == "1" || ef == "true" || ef == "yes" || ef == "on");
        player_flexfec_override =
            cfg.GetStream(stream_id, "FLEXFEC_FIELD_TRIALS", cfg.Get("FLEXFEC_FIELD_TRIALS", ""));
    }
    if (const char* ef_env = std::getenv("ENABLE_FLEXFEC")) {
        std::string s(ef_env);
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "1" || s == "true" || s == "yes" || s == "on") {
            player_flexfec = true;
        } else if (s == "0" || s == "false" || s == "no" || s == "off") {
            player_flexfec = false;
        }
    }
    if (cmdline_enable_flexfec) {
        player_flexfec = true;
    }

    std::cout << "=== WebRTC P2P 拉流" << (headless ? " (无头/仅日志)" : " (SDL2)") << " ===" << std::endl;
    std::cout << "信令: " << url << " 流: " << stream_id << std::endl;
    if (!headless) {
        std::cout << "按 Esc 或关闭窗口退出" << std::endl;
    }

    std::unique_ptr<p2p::SdlDisplay> display;
    if (!headless) {
        display = std::make_unique<p2p::SdlDisplay>("WebRTC P2P 拉流");
        if (!display->IsOpen()) {
            std::cerr << "SDL 窗口创建失败，请安装: sudo apt install libsdl2-dev\n"
                      << "或远程/无显示时使用: " << argv[0] << " --headless ..." << std::endl;
            return 1;
        }
    }

    p2p::P2pPlayer player(url, stream_id);
    player.SetFlexfecOptions(player_flexfec, player_flexfec_override);
    g_player = &player;

    std::signal(SIGINT, SignalHandler);

    std::atomic<unsigned> decoded_frames{0};
    player.SetOnVideoFrame([&decoded_frames, &display, headless](const uint8_t* argb, int width,
                                                                 int height, int stride) {
        (void)argb;
        (void)stride;
        unsigned n = ++decoded_frames;
        if (headless) {
            if (n == 1 || n <= 5 || n % 10 == 0) {
                std::cout << "[Headless] 已收到解码帧 #" << n << " 分辨率 " << width << "x" << height
                          << std::endl;
            }
        } else if (display) {
            display->UpdateFrame(argb, width, height, stride);
        }
    });

    player.SetOnConnectionState([](p2p::ConnectionState state) {
        const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
        std::cout << "[State] " << names[static_cast<int>(state)] << std::endl;
    });

    player.SetOnError([](const std::string& msg) {
        std::cerr << "[Error] " << msg << std::endl;
    });

    player.Play();

    if (headless) {
        std::cout << "[Headless] 等待推流端 offer，目标至少 " << need_frames << " 帧，超时 " << timeout_sec
                  << " 秒..." << std::endl;
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::seconds(timeout_sec);
        while (player.IsPlaying() && clock::now() < deadline) {
            if (decoded_frames >= need_frames) {
                std::cout << "[Headless] 拉流验证成功：共收到 " << decoded_frames.load() << " 帧" << std::endl;
                if (std::getenv("P2P_FEC_STATS_VERIFY")) {
                    player.DumpInboundFecReceiverStats(std::cout, 5000);
                }
                player.Stop();
                g_player = nullptr;
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (player.IsPlaying()) {
            std::cerr << "[Headless] 超时：仅收到 " << decoded_frames.load() << " 帧（需要 " << need_frames
                      << "）" << std::endl;
            player.Stop();
            g_player = nullptr;
            return 2;
        }
    } else {
        while (player.IsPlaying() && display->PollAndRender()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps UI 刷新
        }

        if (player.IsPlaying()) {
            player.Stop();
        }
    }

    g_player = nullptr;
    std::cout << "退出." << std::endl;
    return 0;
}
