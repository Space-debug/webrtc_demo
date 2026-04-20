#include "push_streamer.h"
#include "camera/camera_utils.h"
#include "config_loader.h"
#include "webrtc/signaling/client.h"
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    std::cout << "=== USB cameras (V4L2) ===" << std::endl;
    auto cameras = ListUsbCameras();
    if (cameras.empty()) {
        std::cout << "No cameras found." << std::endl;
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
    std::cout << "\nExample: ./webrtc_push_demo stream_001 /dev/video11" << std::endl;
    std::cout << "Or:     ./webrtc_push_demo stream_001 11" << std::endl;
}

void PrintUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] [stream_id] [camera]\n"
              << "Options:\n"
              << "  --config FILE     Config file (default config/streams.conf)\n"
              << "  --list-cameras    List USB cameras and exit\n"
              << "  --test-capture    Capture only, 10s, print frame count, exit\n"
              << "  --test-encode     Loopback H264 test, 10s, exit\n"
              << "  --signaling ADDR  Signaling server (default 127.0.0.1:8765)\n"
              << "  --width W         Video width（无 config 时默认 1280，720p）\n"
              << "  --height H        Video height（无 config 时默认 720）\n"
              << "  --fps F           Frame rate（无 config 时默认 30）\n"
              << "Environment:\n"
              << "  WEBRTC_CAPTURE_GATE_MIN_FRAMES=N   Min frames before Offer (0=off)\n"
              << "  WEBRTC_CAPTURE_GATE_MAX_WAIT_SEC=N   Max wait for gate (seconds)\n"
              << "  WEBRTC_MJPEG_TO_H264_TRACE=1   Log MJPEG process start → H264 ready (us), every 30 frames (MPP enc)\n"
              << "  WEBRTC_MJPEG_V4L2_DMABUF=1     Enable VIDIOC_EXPBUF → MPP EXT_DMA import (default off; BSP 需自测)\n"
              << "  WEBRTC_MJPEG_RGA_TO_MPP=1      RGA dma-buf → MPP input (needs librga + EXPBUF; safer than EXT_DMA)\n"
              << "  WEBRTC_MJPEG_RGA_MAX_ASPECT=N  RGA 假 Y400 布局最大长宽比（默认 64；RGA 失败可试 8~16）\n"
              << "  WEBRTC_LATENCY_TRACE=1          打印各段耗时（capture gate / V4L2 / MJPEG / MPP / 编码等）\n"
              << "  WEBRTC_CAPTURE_WARMUP_SEC=N     覆盖配置：摄像头预热秒数\n"
              << "  WEBRTC_MJPEG_DECODE_INLINE=1    MJPEG 在采集线程解码（省约 1 帧延迟；采集易被解码拖慢）\n"
              << "  WEBRTC_MJPEG_DEC_LOW_LATENCY=1  MPP MJPEG 解码 poll/休眠更激进\n"
              << "  WEBRTC_MJPEG_RGA_DISABLE_AFTER_FAIL=1  首帧 RGA 失败后本会话不再尝试 RGA\n"
              << "  WEBRTC_MPP_ENC_GOP=N            覆盖 MPP H.264 GOP（1~600；未设时用 config KEYFRAME_INTERVAL）\n"
              << "  WEBRTC_MPP_ENC_INTRA_REFRESH_MODE=0|1|2|3  H264 帧内刷新模式（默认 1=按 MB 行）\n"
              << "  WEBRTC_MPP_ENC_INTRA_REFRESH_ARG=N  帧内刷新参数（行/列/间隔；默认自动）\n"
              << "  WEBRTC_MPP_ENC_SPLIT_BYTES=N    按字节切片（默认 1100；0=关闭）\n"
              << "  WEBRTC_MPP_ENC_IDR_MIN_INTERVAL_MS=N  IDR 最小触发间隔（默认 800ms）\n"
              << "  WEBRTC_MPP_ENC_IDR_LOSS_QUICK_MS=N  丢包触发关键帧请求时的快速 I 帧窗口（默认 180ms）\n"
              << "  WEBRTC_MPP_ENC_IDR_FORCE_MAX_WAIT_MS=N  长时间无关键帧时允许强制 IDR（默认 3000ms）\n"
              << "Arguments:\n"
              << "  stream_id         Stream ID; omitted -> STREAM_ID in config\n"
              << "  camera            Device path/index; omitted -> STREAM_<id>_CAMERA\n"
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
    std::cout << "=== WebRTC P2P publisher demo ===" << std::endl;
    std::cout << "[Main] Parsing arguments..." << std::endl;

    PushStreamerConfig config;
    std::string signaling_url = "127.0.0.1:8765";
    std::string config_path;
    std::optional<std::string> cmdline_signaling;
    std::optional<int> cmdline_width;
    std::optional<int> cmdline_height;
    std::optional<int> cmdline_fps;
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
        } else if (argv[arg_idx][0] != '-') {
            break;
        }
        arg_idx++;
    }
    std::string stream_id = (arg_idx < argc) ? argv[arg_idx++] : "";
    std::string camera_arg = (arg_idx < argc) ? argv[arg_idx++] : "";

    if (config_path.empty()) config_path = FindConfigPath(argv[0], "");
    webrtc_demo::ConfigLoader cfg;
    bool config_loaded = false;
    if (!config_path.empty()) {
        if (cfg.Load(config_path)) {
            config_loaded = true;
            std::cout << "[Main] Loaded config: " << config_path << std::endl;
        } else {
            std::cerr << "[Main] Warning: failed to load config file: " << config_path
                      << " (using program defaults)\n";
        }
    }
    if (config_loaded) {
        if (stream_id.empty()) {
            stream_id = cfg.Get("STREAM_ID", "livestream");
        }
        signaling_url = cfg.GetStream(stream_id, "SIGNALING_ADDR",
                                      cfg.Get("SIGNALING_ADDR", "127.0.0.1:8765"));
        config.common.video_width = cfg.GetStreamInt(stream_id, "WIDTH", 1280);
        config.common.video_height = cfg.GetStreamInt(stream_id, "HEIGHT", 720);
        config.common.video_fps = cfg.GetStreamInt(stream_id, "FPS", 30);
        if (camera_arg.empty()) {
            camera_arg = cfg.GetStream(stream_id, "CAMERA", "");
        }
        config.common.bitrate_mode = cfg.GetStream(stream_id, "BITRATE_MODE", "");
        if (config.common.bitrate_mode.empty()) {
            config.common.bitrate_mode = "vbr";
        }
        config.common.target_bitrate_kbps = cfg.GetStreamInt(stream_id, "TARGET_BITRATE", 2200);
        config.common.min_bitrate_kbps = cfg.GetStreamInt(stream_id, "MIN_BITRATE", 1200);
        config.common.max_bitrate_kbps = cfg.GetStreamInt(stream_id, "MAX_BITRATE", 3500);
        config.common.degradation_preference = cfg.GetStream(stream_id, "DEGRADATION_PREFERENCE", "");
        if (config.common.degradation_preference.empty()) {
            config.common.degradation_preference = "balanced";
        }
        {
            std::string icep = cfg.GetStream(stream_id, "ICE_PRIORITIZE_LIKELY_PAIRS", "");
            if (icep.empty()) {
                icep = "1";
            }
            for (auto& c : icep) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            config.common.ice_prioritize_likely_pairs =
                (icep == "1" || icep == "true" || icep == "yes" || icep == "on");
        }
        config.common.video_network_priority = cfg.GetStream(stream_id, "VIDEO_NETWORK_PRIORITY", "high");
        config.common.video_encoding_max_framerate = cfg.GetStreamInt(stream_id, "VIDEO_ENCODING_MAX_FPS", 0);
        config.common.video_codec = cfg.GetStream(stream_id, "VIDEO_CODEC", "");
        if (config.common.video_codec.empty()) {
            config.common.video_codec = "h264";
        }
        config.common.h264_profile = cfg.GetStream(stream_id, "H264_PROFILE", "");
        if (config.common.h264_profile.empty()) {
            config.common.h264_profile = "main";
        }
        config.common.h264_level = cfg.GetStream(stream_id, "H264_LEVEL", "");
        if (config.common.h264_level.empty()) {
            config.common.h264_level = "3.0";
        }
        config.common.keyframe_interval = cfg.GetStreamInt(stream_id, "KEYFRAME_INTERVAL", 0);
        config.backend.use_rockchip_mpp_h264 = cfg.GetStreamInt(stream_id, "USE_ROCKCHIP_MPP_H264", 1) != 0;
        config.backend.use_rockchip_mpp_mjpeg_decode =
            cfg.GetStreamInt(stream_id, "USE_ROCKCHIP_MPP_MJPEG_DECODE", 1) != 0;
        config.backend.use_rockchip_dual_mpp_mjpeg_h264 =
            cfg.GetStreamInt(stream_id, "USE_DUAL_MPP_MJPEG_H264", 0) != 0;
        {
            std::string mql = cfg.GetStream(stream_id, "MJPEG_QUEUE_LATEST_ONLY", "");
            if (mql.empty()) {
                mql = "0";
            }
            for (auto& c : mql) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            config.backend.mjpeg_queue_latest_only =
                (mql == "1" || mql == "true" || mql == "yes" || mql == "on");
        }
        config.backend.mjpeg_queue_max = cfg.GetStreamInt(stream_id, "MJPEG_QUEUE_MAX", 8);
        config.backend.nv12_pool_slots = cfg.GetStreamInt(stream_id, "NV12_POOL_SLOTS", 6);
        config.backend.v4l2_buffer_count = cfg.GetStreamInt(stream_id, "V4L2_BUFFER_COUNT", 2);
        config.backend.v4l2_poll_timeout_ms = cfg.GetStreamInt(stream_id, "V4L2_POLL_TIMEOUT_MS", 50);
        config.common.capture_warmup_sec = cfg.GetStreamInt(stream_id, "CAPTURE_WARMUP_SEC", 0);
        config.common.capture_gate_min_frames = cfg.GetStreamInt(stream_id, "CAPTURE_GATE_MIN_FRAMES", 0);
        config.common.capture_gate_max_wait_sec = cfg.GetStreamInt(stream_id, "CAPTURE_GATE_MAX_WAIT_SEC", 20);
        {
            std::string inl = cfg.GetStream(stream_id, "MJPEG_DECODE_INLINE", "");
            for (auto& c : inl) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            config.backend.mjpeg_decode_inline =
                (inl == "1" || inl == "true" || inl == "yes" || inl == "on");
        }
        {
            std::string v = cfg.GetStream(stream_id, "MJPEG_V4L2_DMABUF", "");
            for (auto& c : v) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (!v.empty()) {
                config.backend.mjpeg_v4l2_ext_dma =
                    (v == "1" || v == "true" || v == "yes" || v == "on");
            }
        }
        {
            std::string v = cfg.GetStream(stream_id, "MJPEG_RGA_TO_MPP", "");
            for (auto& c : v) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (!v.empty()) {
                config.backend.mjpeg_rga_to_mpp = (v == "1" || v == "true" || v == "yes" || v == "on");
            }
        }
    }
    if (cmdline_signaling.has_value()) signaling_url = *cmdline_signaling;
    if (cmdline_width.has_value()) config.common.video_width = *cmdline_width;
    if (cmdline_height.has_value()) config.common.video_height = *cmdline_height;
    if (cmdline_fps.has_value()) config.common.video_fps = *cmdline_fps;
    if (const char* g = std::getenv("WEBRTC_CAPTURE_GATE_MIN_FRAMES")) {
        config.common.capture_gate_min_frames = std::atoi(g);
    }
    if (const char* gw = std::getenv("WEBRTC_CAPTURE_GATE_MAX_WAIT_SEC")) {
        config.common.capture_gate_max_wait_sec = std::atoi(gw);
    }
    if (const char* wu = std::getenv("WEBRTC_CAPTURE_WARMUP_SEC")) {
        config.common.capture_warmup_sec = std::atoi(wu);
    }
    if (const char* il = std::getenv("WEBRTC_MJPEG_DECODE_INLINE")) {
        const char c = il[0];
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') {
            config.backend.mjpeg_decode_inline = true;
        }
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') {
            config.backend.mjpeg_decode_inline = false;
        }
    }
    if (stream_id.empty()) stream_id = "livestream";

    config.common.stream_id = stream_id;
    if (!camera_arg.empty()) {
        if (strncmp(camera_arg.c_str(), "/dev/video", 10) == 0 || camera_arg[0] == '/')
            config.common.video_device_path = camera_arg;
        else
            config.common.video_device_index = std::atoi(camera_arg.c_str());
    }

    if (test_capture) config.common.test_capture_only = true;
    if (test_encode) config.common.test_encode_mode = true;

    // 码率模式归一化：
    // - cbr: 用 target 作为固定码率（min=max=target）
    // - vbr: 继续使用 min/max 边界，target 仅用于日志与配置语义
    {
        std::string mode = config.common.bitrate_mode;
        for (auto& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        config.common.bitrate_mode = mode;
        if (config.common.min_bitrate_kbps <= 0) config.common.min_bitrate_kbps = 100;
        if (config.common.max_bitrate_kbps <= 0) config.common.max_bitrate_kbps = 2000;
        if (config.common.min_bitrate_kbps > config.common.max_bitrate_kbps) {
            std::swap(config.common.min_bitrate_kbps, config.common.max_bitrate_kbps);
        }
        if (config.common.target_bitrate_kbps <= 0) {
            config.common.target_bitrate_kbps = (config.common.min_bitrate_kbps + config.common.max_bitrate_kbps) / 2;
        }
        if (config.common.target_bitrate_kbps < config.common.min_bitrate_kbps) {
            config.common.target_bitrate_kbps = config.common.min_bitrate_kbps;
        }
        if (config.common.target_bitrate_kbps > config.common.max_bitrate_kbps) {
            config.common.target_bitrate_kbps = config.common.max_bitrate_kbps;
        }
        if (config.common.capture_warmup_sec < 0) {
            config.common.capture_warmup_sec = 0;
        }
        if (config.common.capture_gate_min_frames < 0) {
            config.common.capture_gate_min_frames = 0;
        }
        if (config.common.capture_gate_max_wait_sec < 1) {
            config.common.capture_gate_max_wait_sec = 1;
        }
        if (config.backend.v4l2_buffer_count < 2) {
            config.backend.v4l2_buffer_count = 2;
        }
        if (config.backend.v4l2_buffer_count > 32) {
            config.backend.v4l2_buffer_count = 32;
        }
        if (config.backend.v4l2_poll_timeout_ms < 1) {
            config.backend.v4l2_poll_timeout_ms = 1;
        }
        if (config.backend.v4l2_poll_timeout_ms > 2000) {
            config.backend.v4l2_poll_timeout_ms = 2000;
        }
        if (config.common.bitrate_mode == "cbr") {
            config.common.min_bitrate_kbps = config.common.target_bitrate_kbps;
            config.common.max_bitrate_kbps = config.common.target_bitrate_kbps;
        }
    }

    std::cout << "[Main] Config: stream_id=" << config.common.stream_id
              << " resolution=" << config.common.video_width << "x" << config.common.video_height
              << " fps=" << config.common.video_fps
              << " rockchip_mpp_h264=" << (config.backend.use_rockchip_mpp_h264 ? "on" : "off")
              << " bitrate=" << config.common.target_bitrate_kbps << "kbps(" << config.common.bitrate_mode << ")"
              << " ice_prioritize_likely=" << (config.common.ice_prioritize_likely_pairs ? "on" : "off")
              << " video_net_prio=" << config.common.video_network_priority
              << " codec=" << config.common.video_codec
              << " warmup=" << config.common.capture_warmup_sec << "s"
              << " capture_gate=" << (config.common.capture_gate_min_frames > 0
                                        ? std::to_string(config.common.capture_gate_min_frames) + "frames<=" +
                                              std::to_string(config.common.capture_gate_max_wait_sec) + "s"
                                        : std::string("off"))
              << " mjpeg_inline_decode=" << (config.backend.mjpeg_decode_inline ? "on" : "off")
              << " mjpeg_ext_dma_cfg=" << (config.backend.mjpeg_v4l2_ext_dma ? "on" : "off")
              << " mjpeg_rga_cfg=" << (config.backend.mjpeg_rga_to_mpp ? "on" : "off")
              << " device=" << (config.common.video_device_path.empty() ? std::to_string(config.common.video_device_index) : config.common.video_device_path)
              << std::endl;

    // Rockchip MPP H.264：rc:gop 由 h264_encoder 读 WEBRTC_MPP_ENC_GOP 或 WebRTC 默认；配置文件 KEYFRAME_INTERVAL 在此注入。
    if (config.common.keyframe_interval > 600 || config.common.keyframe_interval < 0) {
        std::cerr << "[Main] KEYFRAME_INTERVAL out of range (use 0=auto or 1..600); ignoring for MPP GOP.\n";
        config.common.keyframe_interval = 0;
    }
    {
        std::string vcodec = config.common.video_codec;
        for (auto& c : vcodec) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (config.backend.use_rockchip_mpp_h264 && vcodec == "h264" && config.common.keyframe_interval >= 1) {
            if (!std::getenv("WEBRTC_MPP_ENC_GOP")) {
                const std::string g = std::to_string(config.common.keyframe_interval);
                if (setenv("WEBRTC_MPP_ENC_GOP", g.c_str(), 1) != 0) {
                    std::cerr << "[Main] setenv WEBRTC_MPP_ENC_GOP failed\n";
                } else {
                    std::cout << "[Main] MPP H.264 GOP frames=" << g << " (from KEYFRAME_INTERVAL; override with env WEBRTC_MPP_ENC_GOP)\n";
                }
            }
        } else if (config.common.keyframe_interval >= 1) {
            std::cout << "[Main] Note: KEYFRAME_INTERVAL applies to MPP H.264 GOP only when "
                         "USE_ROCKCHIP_MPP_H264=1 and VIDEO_CODEC=h264.\n";
        }
    }

    if (use_signaling) {
        config.common.signaling_subscriber_offer_only = true;
    }

    PushStreamer streamer(config);
    g_streamer = &streamer;

    std::unique_ptr<SignalingClient> signaling;
    if (use_signaling) {
        std::cout << "[Main] Signaling mode, server: " << signaling_url << std::endl;
        signaling = std::make_unique<SignalingClient>(signaling_url, "publisher", config.common.stream_id);
        g_signaling = signaling.get();
    } else {
        std::cout << "[Main] No signaling (test-capture / test-encode)" << std::endl;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::shared_ptr<std::vector<std::string>> subscriber_pending;
    std::shared_ptr<std::mutex> subscriber_pending_mu;

    if (use_signaling && signaling) {
        subscriber_pending = std::make_shared<std::vector<std::string>>();
        subscriber_pending_mu = std::make_shared<std::mutex>();
        signaling->SetOnAnswer([&streamer](const std::string& peer_id, const std::string& type,
                                           const std::string& sdp) {
            std::cout << "[Signaling] Received answer peer=" << peer_id << " (type=" << type
                      << ", sdp_len=" << sdp.size() << ") SetRemoteDescription" << std::endl;
            streamer.SetRemoteDescriptionForPeer(peer_id, type, sdp);
        });
        signaling->SetOnIce([&streamer](const std::string& peer_id, const std::string& mid, int mline_index,
                                        const std::string& candidate) {
            std::cout << "[Signaling] ICE candidate peer=" << peer_id << " mid=" << mid << " idx=" << mline_index
                      << " candidate=" << (candidate.size() > 60 ? candidate.substr(0, 60) + "..." : candidate) << std::endl;
            streamer.AddRemoteIceCandidateForPeer(peer_id, mid, mline_index, candidate);
        });
        signaling->SetOnSubscriberJoin(
            [subscriber_pending, subscriber_pending_mu](const std::string& peer_id) {
                std::cout << "[Signaling] Subscriber joined: " << peer_id << " (queued for main thread)"
                          << std::endl;
                std::lock_guard<std::mutex> lk(*subscriber_pending_mu);
                subscriber_pending->push_back(peer_id);
            });
        signaling->SetOnSubscriberLeave([](const std::string& peer_id) {
            std::cout << "[Signaling] Subscriber left: " << peer_id << std::endl;
        });
        signaling->SetOnError([](const std::string& msg) {
            std::cerr << "[Signaling] Error: " << msg << std::endl;
        });

        streamer.SetOnSdpCallback([&signaling](const std::string& peer_id, const std::string& type,
                                                          const std::string& sdp) {
            if (type != "offer") return;
            std::cout << "[Signaling] Send offer peer=" << (peer_id.empty() ? "default" : peer_id)
                      << " (sdp_len=" << sdp.size() << ")" << std::endl;
            signaling->SendOffer(sdp, peer_id);
        });
        streamer.SetOnIceCandidateCallback(
            [&signaling](const std::string& peer_id, const std::string& mid, int mline_index,
                         const std::string& candidate) {
                std::cout << "[Signaling] Send ICE candidate peer=" << (peer_id.empty() ? "default" : peer_id)
                          << " mid=" << mid << " idx=" << mline_index << std::endl;
                signaling->SendIceCandidate(mid, mline_index, candidate, peer_id);
            });

        std::cout << "[Main] Connecting to signaling..." << std::endl;
        if (!signaling->Start()) {
            std::cerr << "[Signaling] Start failed. Run ./build/bin/signaling_server or ./scripts/push.sh" << std::endl;
            return 1;
        }
        std::cout << "[Main] Signaling connected" << std::endl;
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
            if (n == 1) std::cout << "[Capture] First frame: " << w << "x" << h << std::endl;
            else if (n % 50 == 0) std::cout << "[Capture] Frames received: " << n << std::endl;
        });
    }
    if (test_encode) {
        streamer.SetOnConnectionStateCallback([](ConnectionState state) {
            const char* names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
            std::cout << "[Loopback] " << names[static_cast<int>(state)] << std::endl;
        });
    }

    std::cout << "[Main] Starting publisher..." << std::endl;
    if (!streamer.Start()) {
        std::cerr << "[Main] Start failed. Try --list-cameras." << std::endl;
        return 1;
    }
    std::cout << "[Main] Publisher started" << std::endl;

    if (test_capture) {
        std::cout << "[test-capture] Running 10s capture..." << std::endl;
        for (int i = 0; i < 10 && streamer.IsStreaming(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        unsigned int total = streamer.GetFrameCount();
        streamer.Stop();
        std::cout << "Total frames: " << total << std::endl;
        return total > 0 ? 0 : 1;
    }

    if (test_encode) {
        std::cout << "[test-encode] Loopback 10s..." << std::endl;
        for (int i = 0; i < 10 && streamer.IsStreaming(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        unsigned int total = streamer.GetDecodedFrameCount();
        streamer.Stop();
        std::cout << "Decoded frames: " << total << std::endl;
        return total > 0 ? 0 : 1;
    }

    std::cout << "Press Ctrl+C to stop." << std::endl;

    while (streamer.IsStreaming()) {
        if (subscriber_pending && subscriber_pending_mu) {
            std::vector<std::string> batch;
            {
                std::lock_guard<std::mutex> lk(*subscriber_pending_mu);
                batch.swap(*subscriber_pending);
            }
            for (const auto& peer_id : batch) {
                std::cout << "[Main] CreateOfferForPeer " << peer_id << std::endl;
                streamer.CreateOfferForPeer(peer_id);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!streamer.IsStreaming()) {
            break;
        }
    }

    g_streamer = nullptr;
    g_signaling = nullptr;
    std::cout << "Demo exited." << std::endl;
    return 0;
}
