#include "push_streamer.h"
#include "camera_utils.h"
#include "signaling_client.h"
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

using namespace webrtc_demo;

static PushStreamer* g_streamer = nullptr;
static SignalingClient* g_signaling = nullptr;

void SignalHandler(int signum) {
    std::cout << "\n[Main] Received signal " << signum << ", stopping..." << std::endl;
    if (g_streamer) {
        g_streamer->Stop();
    }
}

void ListCameras() {
    std::cout << "=== 本地 USB 摄像头列表 (V4L2) ===" << std::endl;
    auto cameras = ListUsbCameras();
    if (cameras.empty()) {
        std::cout << "未检测到摄像头" << std::endl;
        return;
    }
    for (const auto& cam : cameras) {
        std::cout << "  [" << cam.index << "] " << cam.device_path
                  << " - " << cam.device_name;
        if (!cam.bus_info.empty()) {
            std::cout << " (" << cam.bus_info << ")";
        }
        std::cout << std::endl;
    }
    std::cout << "\n使用示例: ./webrtc_push_demo stream_001 /dev/video11" << std::endl;
    std::cout << "或: ./webrtc_push_demo stream_001 11" << std::endl;
}

void PrintUsage(const char* prog) {
    std::cout << "用法: " << prog << " [选项] [stream_id] [camera]\n"
              << "选项:\n"
              << "  --list-cameras    列出本地 USB 摄像头后退出\n"
              << "  --test-capture    仅验证采集，运行 10 秒后打印帧数并退出\n"
              << "  --test-encode     本地回环验证 H264 编码，运行 10 秒后退出\n"
              << "  --signaling ADDR  P2P 信令服务器地址，默认 127.0.0.1:8765\n"
              << "  --width W         分辨率宽，默认 640\n"
              << "  --height H        分辨率高，默认 360\n"
              << "  --fps F           帧率，默认 15\n"
              << "参数:\n"
              << "  stream_id         流 ID，默认 livestream\n"
              << "  camera            摄像头路径或索引，如 /dev/video11 或 11\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== WebRTC P2P 推流 Demo ===" << std::endl;
    std::cout << "[Main] 解析参数..." << std::endl;

    PushStreamerConfig config;
    std::string signaling_url = "127.0.0.1:8765";
    bool use_signaling = true;
    bool test_capture = false;
    bool test_encode = false;

    int arg_idx = 1;
    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--list-cameras") == 0) {
            ListCameras();
            return 0;
        }
        if (strcmp(argv[arg_idx], "-h") == 0 || strcmp(argv[arg_idx], "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (strcmp(argv[arg_idx], "--test-capture") == 0) {
            test_capture = true;
            use_signaling = false;
        } else if (strcmp(argv[arg_idx], "--test-encode") == 0) {
            test_encode = true;
            use_signaling = false;
        } else if (strcmp(argv[arg_idx], "--signaling") == 0 && arg_idx + 1 < argc) {
            signaling_url = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "--width") == 0 && arg_idx + 1 < argc) {
            config.video_width = std::atoi(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--height") == 0 && arg_idx + 1 < argc) {
            config.video_height = std::atoi(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--fps") == 0 && arg_idx + 1 < argc) {
            config.video_fps = std::atoi(argv[++arg_idx]);
        } else if (argv[arg_idx][0] != '-') {
            break;
        }
        arg_idx++;
    }
    if (arg_idx < argc) config.stream_id = argv[arg_idx++];
    if (arg_idx < argc) {
        const char* cam = argv[arg_idx];
        if (strncmp(cam, "/dev/video", 10) == 0 || cam[0] == '/') {
            config.video_device_path = cam;
        } else {
            config.video_device_index = std::atoi(cam);
        }
    }

    if (test_capture) config.test_capture_only = true;
    if (test_encode) config.test_encode_mode = true;

    std::cout << "[Main] 配置: stream_id=" << config.stream_id
              << " 分辨率=" << config.video_width << "x" << config.video_height
              << " fps=" << config.video_fps
              << " 设备=" << (config.video_device_path.empty() ? std::to_string(config.video_device_index) : config.video_device_path)
              << std::endl;

    PushStreamer streamer(config);
    g_streamer = &streamer;

    std::unique_ptr<SignalingClient> signaling;
    if (use_signaling) {
        std::cout << "[Main] 使用信令模式，服务器: " << signaling_url << std::endl;
        signaling = std::make_unique<SignalingClient>(signaling_url, "publisher");
        g_signaling = signaling.get();
    } else {
        std::cout << "[Main] 无信令模式 (test-capture/test-encode)" << std::endl;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (use_signaling && signaling) {
        signaling->SetOnAnswer([&streamer](const std::string& type, const std::string& sdp) {
            std::cout << "[信令] 收到 answer (type=" << type << ", sdp_len=" << sdp.size() << ")，设置远端描述" << std::endl;
            streamer.SetRemoteDescription(type, sdp);
        });
        signaling->SetOnIce([&streamer](const std::string& mid, int mline_index,
                                        const std::string& candidate) {
            std::cout << "[信令] 收到 ICE 候选 mid=" << mid << " idx=" << mline_index
                      << " candidate=" << (candidate.size() > 60 ? candidate.substr(0, 60) + "..." : candidate) << std::endl;
            streamer.AddRemoteIceCandidate(mid, mline_index, candidate);
        });
        signaling->SetOnError([](const std::string& msg) {
            std::cerr << "[信令] 错误: " << msg << std::endl;
        });

        streamer.SetOnSdpCallback([&streamer, &signaling](const std::string& type,
                                                          const std::string& sdp) {
            if (type != "offer") return;
            std::cout << "[信令] 发送 offer (sdp_len=" << sdp.size() << ")" << std::endl;
            signaling->SendOffer(sdp);
        });
        streamer.SetOnIceCandidateCallback(
            [&signaling](const std::string& mid, int mline_index, const std::string& candidate) {
                std::cout << "[信令] 发送 ICE 候选 mid=" << mid << " idx=" << mline_index << std::endl;
                signaling->SendIceCandidate(mid, mline_index, candidate);
            });

        std::cout << "[Main] 连接信令服务器..." << std::endl;
        if (!signaling->Start()) {
            std::cerr << "[信令] 启动失败。请先运行: ./build/bin/signaling_server 或 ./scripts/start_p2p.sh" << std::endl;
            return 1;
        }
        std::cout << "[Main] 信令连接成功" << std::endl;
    } else {
        streamer.SetOnSdpCallback([](const std::string& type, const std::string& sdp) {
            std::cout << "\n--- Local " << type << " ---\n" << sdp << "\n--- End ---\n" << std::endl;
        });
        streamer.SetOnIceCandidateCallback(
            [](const std::string& mid, int mline_index, const std::string& candidate) {
                std::cout << "[ICE] mid=" << mid << " index=" << mline_index
                          << " candidate=" << candidate << std::endl;
            });
    }

    streamer.SetOnConnectionStateCallback([](ConnectionState state) {
        const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
        std::cout << "[State] " << names[static_cast<int>(state)] << std::endl;
    });

    if (test_capture) {
        streamer.SetOnFrameCallback([](unsigned int n, int w, int h) {
            if (n == 1) std::cout << "[采集] 首帧: " << w << "x" << h << std::endl;
            else if (n % 50 == 0) std::cout << "[采集] 已收到 " << n << " 帧" << std::endl;
        });
    }
    if (test_encode) {
        streamer.SetOnConnectionStateCallback([](ConnectionState state) {
            const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            std::cout << "[回环] " << names[static_cast<int>(state)] << std::endl;
        });
    }

    std::cout << "[Main] 启动推流器..." << std::endl;
    if (!streamer.Start()) {
        std::cerr << "[Main] 启动失败。Try --list-cameras to see available cameras." << std::endl;
        return 1;
    }
    std::cout << "[Main] 推流器已启动" << std::endl;

    if (test_capture) {
        std::cout << "[test-capture] 采集 10 秒..." << std::endl;
        for (int i = 0; i < 10 && streamer.IsStreaming(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        unsigned int total = streamer.GetFrameCount();
        streamer.Stop();
        std::cout << "总帧数: " << total << std::endl;
        return total > 0 ? 0 : 1;
    }

    if (test_encode) {
        std::cout << "[test-encode] 本地回环 10 秒..." << std::endl;
        for (int i = 0; i < 10 && streamer.IsStreaming(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        unsigned int total = streamer.GetDecodedFrameCount();
        streamer.Stop();
        std::cout << "解码帧数: " << total << std::endl;
        return total > 0 ? 0 : 1;
    }

    std::cout << "推流已启动。拉流端: ./build/bin/p2p_player" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    while (streamer.IsStreaming()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    g_streamer = nullptr;
    g_signaling = nullptr;
    std::cout << "Demo exited." << std::endl;
    return 0;
}
