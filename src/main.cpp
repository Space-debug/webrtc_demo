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
              << "  --capture-format FMT  采集格式: auto|yuyv|mjpeg（yuyv/mjpeg 需 v4l2loopback）\n"
              << "  --loopback DEV    v4l2loopback 设备路径，如 /dev/video12\n"
              << "  --no-audio        纯视频推流（默认，与 ENABLE_AUDIO=0 一致）\n"
              << "  --enable-audio    允许 SDP 协商音频意向（未实现麦克风采集）\n"
              << "  --enable-flexfec  启用 FlexFEC-03（与配置 ENABLE_FLEXFEC=1 等效，收发端都需开）\n"
              << "参数:\n"
              << "  stream_id         流 ID，默认 livestream\n"
              << "  camera            摄像头路径或索引，如 /dev/video11 或 11\n"
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
    std::string cmdline_capture_format, cmdline_loopback;
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
        } else if (strcmp(argv[arg_idx], "--capture-format") == 0 && arg_idx + 1 < argc) {
            cmdline_capture_format = argv[++arg_idx];
        } else if (strcmp(argv[arg_idx], "--loopback") == 0 && arg_idx + 1 < argc) {
            cmdline_loopback = argv[++arg_idx];
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
        if (stream_id.empty()) stream_id = cfg.Get("DEFAULT_STREAM", "livestream");
        signaling_url = cfg.GetStream(stream_id, "SIGNALING_ADDR",
                                      cfg.Get("SIGNALING_ADDR", "127.0.0.1:8765"));
        config.video_width = cfg.GetStreamInt(stream_id, "WIDTH", cfg.GetInt("WIDTH", 640));
        config.video_height = cfg.GetStreamInt(stream_id, "HEIGHT", cfg.GetInt("HEIGHT", 480));
        config.video_fps = cfg.GetStreamInt(stream_id, "FPS", cfg.GetInt("FPS", 30));
        if (camera_arg.empty()) {
            camera_arg = cfg.GetStream(stream_id, "CAMERA",
                                      cfg.Get("DEFAULT_CAMERA", "/dev/video11"));
        }
        config.bitrate_mode = cfg.GetStream(stream_id, "BITRATE_MODE", cfg.Get("BITRATE_MODE", "vbr"));
        config.target_bitrate_kbps = cfg.GetStreamInt(stream_id, "TARGET_BITRATE", cfg.GetInt("TARGET_BITRATE", 1000));
        config.min_bitrate_kbps = cfg.GetStreamInt(stream_id, "MIN_BITRATE", cfg.GetInt("MIN_BITRATE", 100));
        config.max_bitrate_kbps = cfg.GetStreamInt(stream_id, "MAX_BITRATE", cfg.GetInt("MAX_BITRATE", 2000));
        config.degradation_preference = cfg.GetStream(stream_id, "DEGRADATION_PREFERENCE", cfg.Get("DEGRADATION_PREFERENCE", "maintain_framerate"));
        config.video_codec = cfg.GetStream(stream_id, "VIDEO_CODEC", cfg.Get("VIDEO_CODEC", "h264"));
        config.h264_profile = cfg.GetStream(stream_id, "H264_PROFILE", cfg.Get("H264_PROFILE", "main"));
        config.h264_level = cfg.GetStream(stream_id, "H264_LEVEL", cfg.Get("H264_LEVEL", "3.0"));
        config.keyframe_interval = cfg.GetStreamInt(stream_id, "KEYFRAME_INTERVAL", cfg.GetInt("KEYFRAME_INTERVAL", 0));
        config.capture_warmup_sec = cfg.GetStreamInt(stream_id, "CAPTURE_WARMUP_SEC",
                                                     cfg.GetInt("CAPTURE_WARMUP_SEC", 0));
        config.capture_format = cfg.GetStream(stream_id, "CAPTURE_FORMAT", cfg.Get("CAPTURE_FORMAT", "auto"));
        config.loopback_device = cfg.GetStream(stream_id, "LOOPBACK_DEVICE", cfg.Get("LOOPBACK_DEVICE", ""));
        {
            std::string ea = cfg.GetStream(stream_id, "ENABLE_AUDIO", cfg.Get("ENABLE_AUDIO", "0"));
            for (auto& c : ea) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.enable_audio = (ea == "1" || ea == "true" || ea == "yes" || ea == "on");
        }
        {
            std::string ef = cfg.GetStream(stream_id, "ENABLE_FLEXFEC", cfg.Get("ENABLE_FLEXFEC", "0"));
            for (auto& c : ef) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.enable_flexfec =
                (ef == "1" || ef == "true" || ef == "yes" || ef == "on");
            config.flexfec_field_trials =
                cfg.GetStream(stream_id, "FLEXFEC_FIELD_TRIALS", cfg.Get("FLEXFEC_FIELD_TRIALS", ""));
        }
    }
    if (!cmdline_capture_format.empty()) config.capture_format = cmdline_capture_format;
    if (!cmdline_loopback.empty()) config.loopback_device = cmdline_loopback;
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
              << " 采集格式=" << config.capture_format
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

    const char* fec_link_probe = std::getenv("WEBRTC_FEC_LINK_PROBE");
    int fec_link_interval_sec = 2;
    if (const char* iv = std::getenv("WEBRTC_FEC_LINK_PROBE_INTERVAL_SEC")) {
        int v = std::atoi(iv);
        if (v > 0) {
            fec_link_interval_sec = v;
        }
    }

    while (streamer.IsStreaming()) {
        if (fec_link_probe && fec_link_probe[0] != '\0' && std::strcmp(fec_link_probe, "0") != 0) {
            std::this_thread::sleep_for(std::chrono::seconds(fec_link_interval_sec));
            if (streamer.IsStreaming()) {
                streamer.LogFecLinkStatsForAllPeers(std::cout);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    g_streamer = nullptr;
    g_signaling = nullptr;
    std::cout << "Demo exited." << std::endl;
    return 0;
}
