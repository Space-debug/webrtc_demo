#include "push_streamer.h"
#include "camera_utils.h"
#include "config_loader.h"
#include "signaling_client.h"
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <optional>
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
              << "  --config FILE     从配置文件加载（默认 config/streams.conf）\n"
              << "  --list-cameras    列出本地 USB 摄像头后退出\n"
              << "  --test-capture    仅验证采集，运行 10 秒后打印帧数并退出\n"
              << "  --test-encode     本地回环验证 H264 编码，运行 10 秒后退出\n"
              << "  --signaling ADDR  P2P 信令服务器地址，默认 127.0.0.1:8765\n"
              << "  --width W         分辨率宽，默认 640\n"
              << "  --height H        分辨率高，默认 360\n"
              << "  --fps F           帧率，默认 15\n"
              << "  --no-audio        纯视频推流（默认，与 ENABLE_AUDIO=0 一致）\n"
              << "  --enable-audio    允许 SDP 协商音频意向（未实现麦克风采集）\n"
              << "  --enable-flexfec  启用 FlexFEC-03（与配置 ENABLE_FLEXFEC=1 等效，收发端都需开）\n"
              << "环境变量:\n"
              << "  WEBRTC_LATENCY_STATS_PROBE=0|1  若设置则覆盖配置 LATENCY_STATS_ENABLE（非 0 为开）\n"
              << "  WEBRTC_LATENCY_STATS_WINDOW_FRAMES=N  覆盖配置窗长；对累计量做 Δ/Δ帧 得到 ms/帧（默认 60）\n"
              << "  WEBRTC_CAPTURE_GATE_MIN_FRAMES=N  创建 Offer 前至少 N 帧(0=关)，覆盖配置 CAPTURE_GATE_MIN_FRAMES\n"
              << "  WEBRTC_CAPTURE_GATE_MAX_WAIT_SEC=N  等待门限最久 N 秒，覆盖 CAPTURE_GATE_MAX_WAIT_SEC\n"
              << "参数:\n"
              << "  stream_id         流 ID；省略时用配置 STREAM_ID\n"
              << "  camera            摄像头；省略时用 STREAM_<stream_id>_CAMERA\n"
              << std::endl;
}

static std::string FindConfigPath(const char* prog, const std::string& config_arg) {
    if (!config_arg.empty()) return config_arg;
    std::string prog_path(prog);
    size_t slash = prog_path.find_last_of("/\\");
    std::string dir = (slash != std::string::npos) ? prog_path.substr(0, slash) : ".";
    for (const char* sub : {"/config/streams.conf", "/../config/streams.conf"}) {
        std::string path = dir + sub;
        std::ifstream f(path);
        if (f) return path;
    }
    return "";
}

int main(int argc, char* argv[]) {
    std::cout << "=== WebRTC P2P 推流 Demo ===" << std::endl;
    std::cout << "[Main] 解析参数..." << std::endl;

    PushStreamerConfig config;
    std::string signaling_url = "127.0.0.1:8765";
    std::string config_path;
    std::optional<std::string> cmdline_signaling;
    std::optional<int> cmdline_width;
    std::optional<int> cmdline_height;
    std::optional<int> cmdline_fps;
    std::optional<bool> cmdline_enable_audio;
    bool cmdline_enable_flexfec = false;
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
        if (strcmp(argv[arg_idx], "--config") == 0 && arg_idx + 1 < argc) {
            config_path = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "--test-capture") == 0) {
            test_capture = true;
            use_signaling = false;
        } else if (strcmp(argv[arg_idx], "--test-encode") == 0) {
            test_encode = true;
            use_signaling = false;
        } else if (strcmp(argv[arg_idx], "--signaling") == 0 && arg_idx + 1 < argc) {
            cmdline_signaling = std::string(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--width") == 0 && arg_idx + 1 < argc) {
            cmdline_width = std::atoi(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--height") == 0 && arg_idx + 1 < argc) {
            cmdline_height = std::atoi(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--fps") == 0 && arg_idx + 1 < argc) {
            cmdline_fps = std::atoi(argv[++arg_idx]);
        } else if (strcmp(argv[arg_idx], "--no-audio") == 0) {
            cmdline_enable_audio = false;
        } else if (strcmp(argv[arg_idx], "--enable-audio") == 0) {
            cmdline_enable_audio = true;
        } else if (strcmp(argv[arg_idx], "--enable-flexfec") == 0) {
            cmdline_enable_flexfec = true;
        } else if (argv[arg_idx][0] != '-') {
            break;
        }
        arg_idx++;
    }
    std::string stream_id = (arg_idx < argc) ? argv[arg_idx++] : "";
    std::string camera_arg = (arg_idx < argc) ? argv[arg_idx++] : "";

    if (config_path.empty()) config_path = FindConfigPath(argv[0], "");
    webrtc_demo::ConfigLoader cfg;
    if (!config_path.empty() && cfg.Load(config_path)) {
        if (stream_id.empty()) {
            stream_id = cfg.Get("STREAM_ID", "livestream");
        }
        signaling_url = cfg.GetStream(stream_id, "SIGNALING_ADDR",
                                      cfg.Get("SIGNALING_ADDR", "127.0.0.1:8765"));
        config.video_width = cfg.GetStreamInt(stream_id, "WIDTH", 1280);
        config.video_height = cfg.GetStreamInt(stream_id, "HEIGHT", 720);
        config.video_fps = cfg.GetStreamInt(stream_id, "FPS", 30);
        if (camera_arg.empty()) {
            camera_arg = cfg.GetStream(stream_id, "CAMERA", "");
        }
        config.bitrate_mode = cfg.GetStream(stream_id, "BITRATE_MODE", "");
        if (config.bitrate_mode.empty()) {
            config.bitrate_mode = "vbr";
        }
        config.target_bitrate_kbps = cfg.GetStreamInt(stream_id, "TARGET_BITRATE", 2200);
        config.min_bitrate_kbps = cfg.GetStreamInt(stream_id, "MIN_BITRATE", 1200);
        config.max_bitrate_kbps = cfg.GetStreamInt(stream_id, "MAX_BITRATE", 3500);
        config.degradation_preference = cfg.GetStream(stream_id, "DEGRADATION_PREFERENCE", "");
        if (config.degradation_preference.empty()) {
            config.degradation_preference = "maintain_framerate";
        }
        config.video_codec = cfg.GetStream(stream_id, "VIDEO_CODEC", "");
        if (config.video_codec.empty()) {
            config.video_codec = "h264";
        }
        config.h264_profile = cfg.GetStream(stream_id, "H264_PROFILE", "");
        if (config.h264_profile.empty()) {
            config.h264_profile = "main";
        }
        config.h264_level = cfg.GetStream(stream_id, "H264_LEVEL", "");
        if (config.h264_level.empty()) {
            config.h264_level = "3.0";
        }
        config.keyframe_interval = cfg.GetStreamInt(stream_id, "KEYFRAME_INTERVAL", 0);
        config.capture_warmup_sec = cfg.GetStreamInt(stream_id, "CAPTURE_WARMUP_SEC", 0);
        config.capture_gate_min_frames = cfg.GetStreamInt(stream_id, "CAPTURE_GATE_MIN_FRAMES", 0);
        config.capture_gate_max_wait_sec = cfg.GetStreamInt(stream_id, "CAPTURE_GATE_MAX_WAIT_SEC", 20);
        {
            std::string ea = cfg.GetStream(stream_id, "ENABLE_AUDIO", "");
            if (ea.empty()) {
                ea = "0";
            }
            for (auto& c : ea) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.enable_audio = (ea == "1" || ea == "true" || ea == "yes" || ea == "on");
        }
        {
            std::string ef = cfg.GetStream(stream_id, "ENABLE_FLEXFEC", "");
            if (ef.empty()) {
                ef = "0";
            }
            for (auto& c : ef) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.enable_flexfec =
                (ef == "1" || ef == "true" || ef == "yes" || ef == "on");
            config.flexfec_field_trials = cfg.GetStream(stream_id, "FLEXFEC_FIELD_TRIALS", "");
        }
        {
            std::string ls = cfg.GetStream(stream_id, "LATENCY_STATS_ENABLE", "");
            if (ls.empty()) {
                ls = "0";
            }
            for (auto& c : ls) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.latency_stats_enable =
                (ls == "1" || ls == "true" || ls == "yes" || ls == "on");
            config.latency_stats_window_frames =
                cfg.GetStreamInt(stream_id, "LATENCY_STATS_WINDOW_FRAMES", 60);
        }
    }
    if (cmdline_enable_audio.has_value()) config.enable_audio = *cmdline_enable_audio;
    if (cmdline_signaling.has_value()) signaling_url = *cmdline_signaling;
    if (cmdline_width.has_value()) config.video_width = *cmdline_width;
    if (cmdline_height.has_value()) config.video_height = *cmdline_height;
    if (cmdline_fps.has_value()) config.video_fps = *cmdline_fps;
    if (const char* ef_env = std::getenv("ENABLE_FLEXFEC")) {
        std::string s(ef_env);
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "1" || s == "true" || s == "yes" || s == "on") {
            config.enable_flexfec = true;
        } else if (s == "0" || s == "false" || s == "no" || s == "off") {
            config.enable_flexfec = false;
        }
    }
    if (cmdline_enable_flexfec) {
        config.enable_flexfec = true;
    }
    if (const char* g = std::getenv("WEBRTC_CAPTURE_GATE_MIN_FRAMES")) {
        config.capture_gate_min_frames = std::atoi(g);
    }
    if (const char* gw = std::getenv("WEBRTC_CAPTURE_GATE_MAX_WAIT_SEC")) {
        config.capture_gate_max_wait_sec = std::atoi(gw);
    }
    if (const char* lw = std::getenv("WEBRTC_LATENCY_STATS_WINDOW_FRAMES")) {
        int v = std::atoi(lw);
        if (v > 0) {
            config.latency_stats_window_frames = v;
        }
    }
    if (stream_id.empty()) stream_id = "livestream";

    config.stream_id = stream_id;
    if (!camera_arg.empty()) {
        if (strncmp(camera_arg.c_str(), "/dev/video", 10) == 0 || camera_arg[0] == '/')
            config.video_device_path = camera_arg;
        else
            config.video_device_index = std::atoi(camera_arg.c_str());
    }

    if (test_capture) config.test_capture_only = true;
    if (test_encode) config.test_encode_mode = true;

    // 码率模式归一化：
    // - cbr: 用 target 作为固定码率（min=max=target）
    // - vbr: 继续使用 min/max 边界，target 仅用于日志与配置语义
    {
        std::string mode = config.bitrate_mode;
        for (auto& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        config.bitrate_mode = mode;
        if (config.min_bitrate_kbps <= 0) config.min_bitrate_kbps = 100;
        if (config.max_bitrate_kbps <= 0) config.max_bitrate_kbps = 2000;
        if (config.min_bitrate_kbps > config.max_bitrate_kbps) {
            std::swap(config.min_bitrate_kbps, config.max_bitrate_kbps);
        }
        if (config.target_bitrate_kbps <= 0) {
            config.target_bitrate_kbps = (config.min_bitrate_kbps + config.max_bitrate_kbps) / 2;
        }
        if (config.target_bitrate_kbps < config.min_bitrate_kbps) {
            config.target_bitrate_kbps = config.min_bitrate_kbps;
        }
        if (config.target_bitrate_kbps > config.max_bitrate_kbps) {
            config.target_bitrate_kbps = config.max_bitrate_kbps;
        }
        if (config.capture_warmup_sec < 0) {
            config.capture_warmup_sec = 0;
        }
        if (config.capture_gate_min_frames < 0) {
            config.capture_gate_min_frames = 0;
        }
        if (config.capture_gate_max_wait_sec < 1) {
            config.capture_gate_max_wait_sec = 1;
        }
        if (config.bitrate_mode == "cbr") {
            config.min_bitrate_kbps = config.target_bitrate_kbps;
            config.max_bitrate_kbps = config.target_bitrate_kbps;
        }
    }

    std::cout << "[Main] 配置: stream_id=" << config.stream_id
              << " 分辨率=" << config.video_width << "x" << config.video_height
              << " fps=" << config.video_fps
              << " 码率=" << config.target_bitrate_kbps << "kbps(" << config.bitrate_mode << ")"
              << " 编码=" << config.video_codec
              << " 预热=" << config.capture_warmup_sec << "s"
              << " 采集门限=" << (config.capture_gate_min_frames > 0
                                    ? std::to_string(config.capture_gate_min_frames) + "帧≤" +
                                          std::to_string(config.capture_gate_max_wait_sec) + "s"
                                    : std::string("off"))
              << " 设备=" << (config.video_device_path.empty() ? std::to_string(config.video_device_index) : config.video_device_path)
              << " flexfec=" << (config.enable_flexfec ? "on" : "off")
              << std::endl;
    if (config.keyframe_interval > 0) {
        std::cout << "[Main] 提示: 当前 libwebrtc 封装未暴露强制关键帧间隔接口，"
                  << "KEYFRAME_INTERVAL 会由编码器自适应与 RTCP PLI/FIR 机制主导" << std::endl;
    }

    if (use_signaling) {
        config.signaling_subscriber_offer_only = true;
    }

    PushStreamer streamer(config);
    g_streamer = &streamer;

    std::unique_ptr<SignalingClient> signaling;
    if (use_signaling) {
        std::cout << "[Main] 使用信令模式，服务器: " << signaling_url << std::endl;
        signaling = std::make_unique<SignalingClient>(signaling_url, "publisher", config.stream_id);
        g_signaling = signaling.get();
    } else {
        std::cout << "[Main] 无信令模式 (test-capture/test-encode)" << std::endl;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (use_signaling && signaling) {
        signaling->SetOnAnswer([&streamer](const std::string& peer_id, const std::string& type,
                                           const std::string& sdp) {
            std::cout << "[信令] 收到 answer peer=" << peer_id << " (type=" << type
                      << ", sdp_len=" << sdp.size() << ")，设置远端描述" << std::endl;
            streamer.SetRemoteDescriptionForPeer(peer_id, type, sdp);
        });
        signaling->SetOnIce([&streamer](const std::string& peer_id, const std::string& mid, int mline_index,
                                        const std::string& candidate) {
            std::cout << "[信令] 收到 ICE 候选 peer=" << peer_id << " mid=" << mid << " idx=" << mline_index
                      << " candidate=" << (candidate.size() > 60 ? candidate.substr(0, 60) + "..." : candidate) << std::endl;
            streamer.AddRemoteIceCandidateForPeer(peer_id, mid, mline_index, candidate);
        });
        signaling->SetOnSubscriberJoin([&streamer](const std::string& peer_id) {
            std::cout << "[信令] 新拉流端加入: " << peer_id << "，创建独立 Offer" << std::endl;
            streamer.CreateOfferForPeer(peer_id);
        });
        signaling->SetOnSubscriberLeave([](const std::string& peer_id) {
            std::cout << "[信令] 拉流端离线: " << peer_id << std::endl;
        });
        signaling->SetOnError([](const std::string& msg) {
            std::cerr << "[信令] 错误: " << msg << std::endl;
        });

        streamer.SetOnSdpCallback([&signaling](const std::string& peer_id, const std::string& type,
                                                          const std::string& sdp) {
            if (type != "offer") return;
            std::cout << "[信令] 发送 offer peer=" << (peer_id.empty() ? "default" : peer_id)
                      << " (sdp_len=" << sdp.size() << ")" << std::endl;
            signaling->SendOffer(sdp, peer_id);
        });
        streamer.SetOnIceCandidateCallback(
            [&signaling](const std::string& peer_id, const std::string& mid, int mline_index,
                         const std::string& candidate) {
                std::cout << "[信令] 发送 ICE 候选 peer=" << (peer_id.empty() ? "default" : peer_id)
                          << " mid=" << mid << " idx=" << mline_index << std::endl;
                signaling->SendIceCandidate(mid, mline_index, candidate, peer_id);
            });

        std::cout << "[Main] 连接信令服务器..." << std::endl;
        if (!signaling->Start()) {
            std::cerr << "[信令] 启动失败。请先运行: ./build/bin/signaling_server 或 ./scripts/start_p2p.sh" << std::endl;
            return 1;
        }
        std::cout << "[Main] 信令连接成功" << std::endl;
    } else {
        streamer.SetOnSdpCallback([](const std::string& peer_id, const std::string& type,
                                     const std::string& sdp) {
            std::cout << "\n--- Local " << type << " ---\n" << sdp << "\n--- End ---\n" << std::endl;
        });
        streamer.SetOnIceCandidateCallback(
            [](const std::string& peer_id, const std::string& mid, int mline_index,
               const std::string& candidate) {
                std::cout << "[ICE] peer=" << (peer_id.empty() ? "default" : peer_id)
                          << " mid=" << mid << " index=" << mline_index
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

    bool latency_probe_on = config.latency_stats_enable;
    if (const char* latency_stats_probe = std::getenv("WEBRTC_LATENCY_STATS_PROBE")) {
        latency_probe_on =
            (latency_stats_probe[0] != '\0' && std::strcmp(latency_stats_probe, "0") != 0);
    }

    while (streamer.IsStreaming()) {
        if (latency_probe_on) {
            streamer.LogLatencyStatsRollingAvg(std::cout);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!streamer.IsStreaming()) {
            break;
        }
    }

    g_streamer = nullptr;
    g_signaling = nullptr;
    std::cout << "Demo exited." << std::endl;
    return 0;
}
