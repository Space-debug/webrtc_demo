#include "p2p_player.h"
#include "sdl_display.h"

#include <csignal>
#include <iostream>
#include <thread>

static p2p::P2pPlayer* g_player = nullptr;

void SignalHandler(int) {
    if (g_player) g_player->Stop();
}

int main(int argc, char* argv[]) {
    std::string url = "127.0.0.1:8765";
    if (argc >= 2) url = argv[1];

    std::cout << "=== WebRTC P2P 拉流 (SDL2 播放) ===" << std::endl;
    std::cout << "信令: " << url << std::endl;
    std::cout << "按 Esc 或关闭窗口退出" << std::endl;

    p2p::SdlDisplay display("WebRTC P2P 拉流");
    if (!display.IsOpen()) {
        std::cerr << "SDL 窗口创建失败，请安装: sudo apt install libsdl2-dev" << std::endl;
        return 1;
    }

    p2p::P2pPlayer player(url);
    g_player = &player;

    std::signal(SIGINT, SignalHandler);

    player.SetOnVideoFrame([&display](const uint8_t* argb, int width, int height, int stride) {
        display.UpdateFrame(argb, width, height, stride);
    });

    player.SetOnConnectionState([](p2p::ConnectionState state) {
        const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
        std::cout << "[State] " << names[static_cast<int>(state)] << std::endl;
    });

    player.SetOnError([](const std::string& msg) {
        std::cerr << "[Error] " << msg << std::endl;
    });

    player.Play();

    while (player.IsPlaying() && display.PollAndRender()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps UI 刷新
    }

    if (player.IsPlaying()) {
        player.Stop();
    }

    g_player = nullptr;
    std::cout << "退出." << std::endl;
    return 0;
}
