#include "push_streamer.h"
#include "camera_utils.h"
#include "capture_bridge.h"
#include <libwebrtc.h>

using libwebrtc::scoped_refptr;
#include <rtc_ice_candidate.h>
#include <rtc_mediaconstraints.h>
#include <rtc_media_stream.h>
#include <rtc_peerconnection.h>
#include <rtc_peerconnection_factory.h>
#include <rtc_rtp_capabilities.h>
#include <rtc_rtp_parameters.h>
#include <rtc_rtp_receiver.h>
#include <rtc_rtp_sender.h>
#include <rtc_rtp_transceiver.h>
#include <rtc_session_description.h>
#include <rtc_video_device.h>
#include <rtc_video_frame.h>
#include <rtc_video_source.h>
#include <rtc_video_track.h>
#include <helper.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace webrtc_demo {

namespace {

// 在无音频设备的 headless 环境中，为 ALSA 提供空设备映射，避免 ADM 初始化直接崩溃。
void EnableAlsaNullDeviceFallback() {
    const char* path = "/tmp/webrtc_demo_alsa_null.conf";
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << "pcm.!default {\n"
               "  type asym\n"
               "  playback.pcm \"null\"\n"
               "  capture.pcm \"null\"\n"
               "}\n";
        out.close();
        setenv("ALSA_CONFIG_PATH", path, 1);
    }
}

}  // namespace

class FrameCountingRenderer
    : public libwebrtc::RTCVideoRenderer<libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>> {
public:
    using OnFrameCallback = webrtc_demo::OnFrameCallback;
    explicit FrameCountingRenderer(OnFrameCallback cb) : on_frame_(std::move(cb)) {}

    void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame> frame) override {
        if (!frame) return;
        unsigned int n = ++frame_count_;
        if (on_frame_) {
            on_frame_(n, frame->width(), frame->height());
        }
    }

    unsigned int GetFrameCount() const { return frame_count_; }

private:
    std::atomic<unsigned int> frame_count_{0};
    OnFrameCallback on_frame_;
};

// 回环接收端观察者：接收 H264 解码后的帧并计数，交换 ICE
class LoopbackReceiverObserver : public libwebrtc::RTCPeerConnectionObserver {
public:
    using AddCandidateFn = std::function<void(const std::string&, int, const std::string&)>;
    explicit LoopbackReceiverObserver(AddCandidateFn add_to_sender)
        : add_to_sender_(std::move(add_to_sender)) {}

    void OnSignalingState(libwebrtc::RTCSignalingState) override {}
    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState) override {}
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
    void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
    void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
    void OnRenegotiationNeeded() override {}
    void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

    void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> c) override {
        if (add_to_sender_ && c) {
            add_to_sender_(c->sdp_mid().std_string(), c->sdp_mline_index(),
                           c->candidate().std_string());
        }
    }

    void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver> t) override {
        AttachRendererToTrack(t ? t->receiver() : nullptr);
    }

    void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
                    libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> r) override {
        AttachRendererToTrack(r);
    }

    unsigned int GetDecodedCount() const {
        return decoded_renderer_ ? decoded_renderer_->GetFrameCount() : 0;
    }

    void RemoveRenderer() {
        if (decoded_renderer_ && decoded_track_) {
            decoded_track_->RemoveRenderer(decoded_renderer_.get());
        }
        decoded_renderer_.reset();
        decoded_track_ = nullptr;
    }

private:
    void AttachRendererToTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> r) {
        if (!r) return;
        auto track = r->track();
        if (!track || std::strcmp(track->kind().c_string(), "video") != 0) return;
        auto vt = libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>(
            static_cast<libwebrtc::RTCVideoTrack*>(track.get()));
        if (!vt) return;
        decoded_renderer_ = std::make_unique<FrameCountingRenderer>(OnFrameCallback{});
        vt->AddRenderer(decoded_renderer_.get());
        decoded_track_ = vt;
    }

    AddCandidateFn add_to_sender_;
    std::unique_ptr<FrameCountingRenderer> decoded_renderer_;
    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> decoded_track_;
};

class PushStreamer::Impl : public libwebrtc::RTCPeerConnectionObserver {
public:
    explicit Impl(const PushStreamerConfig& config) : config_(config) {}

    class ExtraPeerObserver : public libwebrtc::RTCPeerConnectionObserver {
    public:
        ExtraPeerObserver(Impl* owner, std::string peer_id)
            : owner_(owner), peer_id_(std::move(peer_id)) {}

        void OnSignalingState(libwebrtc::RTCSignalingState) override {}
        void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
        void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
        void OnAddStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
        void OnRemoveStream(libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
        void OnDataChannel(libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
        void OnRenegotiationNeeded() override {}
        void OnTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}
        void OnAddTrack(libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
                        libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
        void OnRemoveTrack(libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}

        void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
            if (owner_) owner_->NotifyConnectionState(state);
        }

        void OnIceCandidate(libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
            if (owner_) owner_->OnPeerIceCandidate(peer_id_, candidate);
        }

    private:
        Impl* owner_;
        std::string peer_id_;
    };

    bool Initialize() {
        std::cout << "[PushStreamer] 初始化 LibWebRTC..." << std::endl;
        if (!libwebrtc::LibWebRTC::Initialize()) {
            std::cerr << "[PushStreamer] LibWebRTC init failed" << std::endl;
            return false;
        }
        std::cout << "[PushStreamer] LibWebRTC 初始化成功" << std::endl;

        std::cout << "[PushStreamer] 创建 PeerConnectionFactory..." << std::endl;
        factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
        if (!factory_) {
            std::cerr << "[PushStreamer] CreateRTCPeerConnectionFactory failed" << std::endl;
            return false;
        }
        // 某些无声卡/无桌面环境在 factory->Initialize() 内部会因 ADM 直接 CHECK 崩溃；
        // 纯视频模式提前注入 ALSA null，避免进入不可恢复的 abort 路径。
        if (!config_.enable_audio) {
            EnableAlsaNullDeviceFallback();
        }
        if (!factory_->Initialize()) {
            // 某些无声卡环境会在 ADM 初始化时报错；纯视频模式下尝试降级恢复。
            if (!config_.enable_audio) {
                std::cerr << "[PushStreamer] PeerConnectionFactory init failed, "
                          << "try video-only fallback (ALSA null)" << std::endl;
                EnableAlsaNullDeviceFallback();
                factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
                if (factory_ && factory_->Initialize()) {
                    std::cout << "[PushStreamer] PeerConnectionFactory 初始化恢复成功（video-only）"
                              << std::endl;
                } else {
                    std::cerr << "[PushStreamer] warning: factory initialize still failed, "
                              << "continue in best-effort video-only mode" << std::endl;
                }
            } else {
                std::cerr << "[PushStreamer] PeerConnectionFactory init failed" << std::endl;
                return false;
            }
        }
        std::cout << "[PushStreamer] PeerConnectionFactory 创建成功" << std::endl;

        return CreatePeerConnection();
    }

    void Shutdown() {
        if (frame_counter_ && video_track_) {
            video_track_->RemoveRenderer(frame_counter_.get());
        }
        frame_counter_.reset();
        if (loopback_observer_) {
            loopback_observer_->RemoveRenderer();
        }
        if (receiver_) {
            receiver_->DeRegisterRTCPeerConnectionObserver();
            receiver_->Close();
            factory_->Delete(receiver_);
            receiver_ = nullptr;
        }
        loopback_observer_.reset();
        if (peer_connection_) {
            peer_connection_->DeRegisterRTCPeerConnectionObserver();
            peer_connection_->Close();
            factory_->Delete(peer_connection_);
            peer_connection_ = nullptr;
        }
        for (auto& kv : peer_connections_) {
            auto& pc = kv.second;
            if (pc) {
                pc->DeRegisterRTCPeerConnectionObserver();
                pc->Close();
                factory_->Delete(pc);
            }
        }
        peer_connections_.clear();
        extra_peer_observers_.clear();
        local_stream_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        video_capturer_ = nullptr;

        if (capture_bridge_) {
            capture_bridge_->Stop();
            capture_bridge_.reset();
        }

        if (factory_) {
            factory_->Terminate();
            factory_ = nullptr;
        }
        libwebrtc::LibWebRTC::Terminate();
    }

    bool CreatePeerConnection() {
        std::cout << "[PushStreamer] 创建 PeerConnection (stun=" << config_.stun_server << ")..." << std::endl;
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        if (!config_.turn_server.empty()) {
            rtc_config.ice_servers[1].uri = config_.turn_server;
            rtc_config.ice_servers[1].username = config_.turn_username;
            rtc_config.ice_servers[1].password = config_.turn_password;
        }
        // 减少多网卡干扰：禁用 IPv6 和 link-local，避免 ICE 选错路径导致 0 RTP
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        // 仅 UDP：低延迟、画面稳定；本机推拉流需添加路由: sudo ip route add <本机IP>/32 dev lo
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        // 抗花屏：DSCP 标记提升 QoS，WebRTC 默认已启用 NACK/PLI 重传
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        peer_connection_ = factory_->Create(rtc_config, constraints);
        if (!peer_connection_) {
            std::cerr << "[PushStreamer] Create PeerConnection failed" << std::endl;
            return false;
        }

        peer_connection_->RegisterRTCPeerConnectionObserver(this);
        std::cout << "[PushStreamer] PeerConnection 创建成功" << std::endl;

        if (!CreateMediaTracks()) {
            return false;
        }

        if (!video_track_) {
            std::cerr << "[PushStreamer] No video track available" << std::endl;
            return false;
        }

        // Unified Plan 模式使用 AddTrack，AddStream 已废弃
        std::vector<libwebrtc::string> tmp;
        tmp.push_back(libwebrtc::string(config_.stream_id.c_str()));
        libwebrtc::vector<libwebrtc::string> stream_ids(tmp);
        peer_connection_->AddTrack(video_track_, stream_ids);
        ApplyVideoCodecPreferences(peer_connection_);
        ApplyEncodingParameters(peer_connection_);
        std::cout << "[PushStreamer] 已添加视频轨道 (stream_id=" << config_.stream_id << ")" << std::endl;

        return true;
    }

    /// 在 CreateOffer 之前调用：按 VIDEO_CODEC 将首选编码（如 H264）排到能力列表最前，影响 SDP m=video 中 PT 顺序。
    void ApplyVideoCodecPreferences(libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        if (!factory_ || !pc) return;

        std::string want = config_.video_codec;
        for (auto& ch : want) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (want.empty()) want = "h264";

        auto caps = factory_->GetRtpSenderCapabilities(libwebrtc::RTCMediaType::VIDEO);
        if (!caps) {
            std::cerr << "[PushStreamer] GetRtpSenderCapabilities(VIDEO) 为空，跳过编解码器排序" << std::endl;
            return;
        }

        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> in = caps->codecs();
        if (in.size() == 0) return;

        auto mime_lower = [](libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability> c) -> std::string {
            std::string m = c->mime_type().std_string();
            for (auto& ch : m) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return m;
        };

        auto match_want = [&](const std::string& m) -> bool {
            if (want == "h264") return m.find("h264") != std::string::npos;
            if (want == "h265" || want == "hevc") {
                return m.find("h265") != std::string::npos || m.find("hevc") != std::string::npos ||
                       m.find("hev1") != std::string::npos;
            }
            if (want == "vp8") return m.find("vp8") != std::string::npos;
            if (want == "vp9") return m.find("vp9") != std::string::npos;
            if (want == "av1") return m.find("av1") != std::string::npos;
            return m.find(want) != std::string::npos;
        };

        std::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> preferred, other;
        for (size_t i = 0; i < in.size(); ++i) {
            auto c = in[i];
            if (!c) continue;
            if (match_want(mime_lower(c))) {
                preferred.push_back(c);
            } else {
                other.push_back(c);
            }
        }

        if (preferred.empty()) {
            std::cout << "[PushStreamer] SetCodecPreferences 跳过: 无与 VIDEO_CODEC=" << config_.video_codec
                      << " 匹配的 sender 能力项" << std::endl;
            return;
        }

        std::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> ordered;
        ordered.reserve(preferred.size() + other.size());
        ordered.insert(ordered.end(), preferred.begin(), preferred.end());
        ordered.insert(ordered.end(), other.begin(), other.end());

        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpCodecCapability>> lw(ordered);

        auto transceivers = pc->transceivers();
        for (size_t i = 0; i < transceivers.size(); ++i) {
            auto tr = transceivers[i];
            if (!tr || tr->media_type() != libwebrtc::RTCMediaType::VIDEO) continue;
            tr->SetCodecPreferences(lw);
            std::cout << "[PushStreamer] SetCodecPreferences: 优先 " << want << "（" << preferred.size()
                      << " 项置前 / 共 " << ordered.size() << " 项），Offer 中对应 PT 将排在 VP8 等之前"
                      << std::endl;
            return;
        }
    }

    void ApplyEncodingParameters(
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        auto transceivers = pc->transceivers();
        for (size_t i = 0; i < transceivers.size(); ++i) {
            auto tr = transceivers[i];
            if (!tr || tr->media_type() != libwebrtc::RTCMediaType::VIDEO)
                continue;
            auto sender = tr->sender();
            if (!sender) continue;
            auto params = sender->parameters();
            if (!params) continue;
            auto encodings = params->encodings();
            for (size_t j = 0; j < encodings.size(); ++j) {
                auto enc = encodings[j];
                if (enc) {
                    enc->set_min_bitrate_bps(config_.min_bitrate_kbps * 1000);
                    enc->set_max_bitrate_bps(config_.max_bitrate_kbps * 1000);
                }
            }
            if (config_.degradation_preference == "maintain_resolution") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_RESOLUTION);
            } else if (config_.degradation_preference == "maintain_framerate") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_FRAMERATE);
            } else if (config_.degradation_preference == "balanced") {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::BALANCED);
            } else {
                params->SetDegradationPreference(
                    libwebrtc::RTCDegradationPreference::MAINTAIN_FRAMERATE);
            }
            if (sender->set_parameters(params)) {
                std::cout << "[PushStreamer] 编码参数已应用: 码率 "
                          << config_.min_bitrate_kbps << "-" << config_.max_bitrate_kbps
                          << " kbps, 策略=" << config_.degradation_preference << std::endl;
            }
            break;
        }
    }

    libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> CreatePeerConnectionWithObserver(
        libwebrtc::RTCPeerConnectionObserver* observer) {
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        if (!config_.turn_server.empty()) {
            rtc_config.ice_servers[1].uri = config_.turn_server;
            rtc_config.ice_servers[1].username = config_.turn_username;
            rtc_config.ice_servers[1].password = config_.turn_password;
        }
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        auto pc = factory_->Create(rtc_config, constraints);
        if (!pc) return nullptr;
        pc->RegisterRTCPeerConnectionObserver(observer);
        return pc;
    }

    bool EnsurePeerConnectionForPeer(const std::string& peer_id) {
        if (peer_id.empty()) return peer_connection_ != nullptr;

        std::lock_guard<std::mutex> lock(mutex_);
        if (peer_connections_.find(peer_id) != peer_connections_.end()) return true;

        auto observer = std::make_unique<ExtraPeerObserver>(this, peer_id);
        auto pc = CreatePeerConnectionWithObserver(observer.get());
        if (!pc) {
            std::cerr << "[PushStreamer] CreatePeerConnection failed for peer=" << peer_id << std::endl;
            return false;
        }
        if (!video_track_) {
            std::cerr << "[PushStreamer] video_track is null for peer=" << peer_id << std::endl;
            return false;
        }

        std::vector<libwebrtc::string> tmp;
        tmp.push_back(libwebrtc::string(config_.stream_id.c_str()));
        libwebrtc::vector<libwebrtc::string> stream_ids(tmp);
        pc->AddTrack(video_track_, stream_ids);
        ApplyVideoCodecPreferences(pc);
        ApplyEncodingParameters(pc);
        peer_connections_[peer_id] = pc;
        extra_peer_observers_[peer_id] = std::move(observer);
        std::cout << "[PushStreamer] 创建订阅者 PeerConnection 成功: " << peer_id << std::endl;
        return true;
    }

    void CreateOfferOnConnection(const std::string& peer_id,
                                 libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc) {
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        constraints->AddOptionalConstraint(
            libwebrtc::RTCMediaConstraints::kNumSimulcastLayers, "1");
        if (!config_.enable_audio) {
            constraints->AddOptionalConstraint(
                libwebrtc::RTCMediaConstraints::kOfferToReceiveAudio,
                libwebrtc::RTCMediaConstraints::kValueFalse);
        }
        auto peer_id_ptr = std::make_shared<std::string>(peer_id);
        pc->CreateOffer(
            [this, peer_id_ptr, pc](const libwebrtc::string& sdp, const libwebrtc::string& type) {
                auto type_str = std::make_shared<std::string>(type.std_string());
                auto sdp_str = std::make_shared<std::string>(sdp.std_string());
                pc->SetLocalDescription(
                    sdp, type,
                    [this, peer_id_ptr, type_str, sdp_str]() {
                        if (config_.test_encode_mode && peer_id_ptr->empty()) {
                            if (std::getenv("WEBRTC_DUMP_OFFER")) {
                                std::cout << "\n--- Local offer SDP ---\n"
                                          << *sdp_str << "\n--- End ---\n" << std::flush;
                            }
                            DoLoopbackExchange(*type_str, *sdp_str);
                        } else if (on_sdp_) {
                            on_sdp_(*peer_id_ptr, *type_str, *sdp_str);
                        }
                    },
                    [](const char* err) {
                        std::cerr << "[PushStreamer] SetLocalDescription failed: " << err << std::endl;
                    });
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] CreateOffer failed: " << err << std::endl;
            },
            constraints);
    }

    bool CreateMediaTracks() {
        std::string device_for_libwebrtc = config_.video_device_path;
        auto fmt = CaptureBridge::ParseFormat(config_.capture_format);

        // YUYV/MJPEG 模式：启动采集桥接，libwebrtc 从 loopback 读取
        if ((fmt == CaptureBridge::Format::YUYV || fmt == CaptureBridge::Format::MJPEG) &&
            !config_.loopback_device.empty() && !config_.video_device_path.empty()) {
            CaptureBridge::Config bridge_cfg;
            bridge_cfg.source_device = config_.video_device_path;
            bridge_cfg.loopback_device = config_.loopback_device;
            bridge_cfg.format = fmt;
            bridge_cfg.width = config_.video_width;
            bridge_cfg.height = config_.video_height;
            bridge_cfg.fps = config_.video_fps;
            capture_bridge_ = std::make_unique<CaptureBridge>(bridge_cfg);
            if (!capture_bridge_->Start()) {
                std::cerr << "[PushStreamer] 采集桥接启动失败，请检查 v4l2loopback 是否已加载："
                          << " sudo modprobe v4l2loopback video_nr=12" << std::endl;
                capture_bridge_.reset();
                return false;
            }
            device_for_libwebrtc = config_.loopback_device;
            std::cout << "[PushStreamer] 使用采集桥接: " << config_.video_device_path
                      << " -> " << config_.loopback_device << " ("
                      << (fmt == CaptureBridge::Format::MJPEG ? "MJPEG" : "YUYV") << ")" << std::endl;
        }

        auto video_device = factory_->GetVideoDevice();
        if (!video_device) {
            std::cerr << "[PushStreamer] GetVideoDevice() 返回空，libwebrtc 可能未启用 V4L2 或依赖缺失"
                      << "（需 libX11、libglib 等，见 docs/linux_arm64_build_notes.md）" << std::endl;
            if (capture_bridge_) {
                capture_bridge_->Stop();
                capture_bridge_.reset();
            }
            return false;
        }
        uint32_t num = video_device->NumberOfDevices();
        if (num == 0) {
            std::cerr << "[PushStreamer] libwebrtc 枚举到 0 个设备（NumberOfDevices=0）" << std::endl;
            auto v4l2_cams = webrtc_demo::ListUsbCameras();
            if (v4l2_cams.empty()) {
                std::cerr << "[PushStreamer] V4L2 也未检测到摄像头。请检查：\n"
                          << "  1. 摄像头是否已连接\n"
                          << "  2. 是否有权限访问 /dev/video*（需 video 组或 root）\n"
                          << "  3. 运行: ./webrtc_push_demo --list-cameras" << std::endl;
            } else {
                std::cerr << "[PushStreamer] 但 V4L2 检测到 " << v4l2_cams.size() << " 个设备，"
                          << "可能是 libwebrtc 与当前 V4L2 设备不兼容。"
                          << "可尝试指定设备: ./webrtc_push_demo livestream " << v4l2_cams[0].device_path
                          << std::endl;
            }
            if (capture_bridge_) {
                capture_bridge_->Stop();
                capture_bridge_.reset();
            }
            return false;
        }

        uint32_t idx = static_cast<uint32_t>(config_.video_device_index);
        const char* device_name = "camera";

        if (!device_for_libwebrtc.empty()) {
            bool found = false;
            // 先按“可视频采集节点”的顺序映射，避开同名 metadata 节点(/dev/video12 等)。
            auto v4l2_cams = webrtc_demo::ListUsbCameras();
            for (size_t i = 0; i < v4l2_cams.size(); ++i) {
                if (v4l2_cams[i].device_path == device_for_libwebrtc) {
                    idx = static_cast<uint32_t>(i);
                    device_name = device_for_libwebrtc.c_str();
                    found = true;
                    std::cout << "[PushStreamer] Using USB camera: " << config_.video_device_path
                              << " -> libwebrtc index " << idx << " (mapped by capture order)"
                              << std::endl;
                    break;
                }
            }

            if (!found) {
                // 兜底：通过 bus_info 将 V4L2 路径映射到 libwebrtc 的 device index
                std::string bus_info = webrtc_demo::GetDeviceBusInfo(device_for_libwebrtc);
                uint32_t num_devices = video_device->NumberOfDevices();
                for (uint32_t i = 0; i < num_devices; ++i) {
                    char name[256] = {0}, id[256] = {0};
                    video_device->GetDeviceName(i, name, sizeof(name), id, sizeof(id));
                    if (!bus_info.empty() && std::string(id) == bus_info) {
                        idx = i;
                        device_name = device_for_libwebrtc.c_str();
                        found = true;
                        std::cout << "[PushStreamer] Using USB camera: " << config_.video_device_path
                                  << " -> libwebrtc index " << idx << " (" << name << ")" << std::endl;
                        break;
                    }
                }
            }

            if (!found) {
                std::cerr << "[PushStreamer] No libwebrtc device matches " << device_for_libwebrtc
                          << ", using index 0" << std::endl;
                idx = 0;
                device_name = device_for_libwebrtc.c_str();
            }
        }

        uint32_t num_devices = video_device->NumberOfDevices();
        if (idx >= num_devices) {
            std::cerr << "[PushStreamer] Device index " << idx << " out of range "
                      << "(libwebrtc reports " << num_devices << " devices). "
                      << "Try --list-cameras to see V4L2 devices." << std::endl;
            return false;
        }

        video_capturer_ = video_device->Create(device_name, idx,
                                               config_.video_width, config_.video_height,
                                               config_.video_fps);
        if (!video_capturer_) {
            std::cerr << "[PushStreamer] Create video capturer failed (device index " << idx
                      << ", available: 0-" << (num_devices - 1) << ")" << std::endl;
            return false;
        }

        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        video_source_ = factory_->CreateVideoSource(video_capturer_, "video_source", constraints);
        if (!video_source_) {
            std::cerr << "[PushStreamer] Create video source failed" << std::endl;
            return false;
        }

        video_track_ = factory_->CreateVideoTrack(video_source_, "video_track");
        if (!video_track_) {
            std::cerr << "[PushStreamer] Create video track failed" << std::endl;
            return false;
        }

        if (video_capturer_->StartCapture()) {
            std::cout << "[PushStreamer] Video capture started" << std::endl;
        } else {
            // 某些平台的封装返回值并不可靠，继续走预热与后续观测，
            // 由真实收帧结果决定是否可用。
            std::cerr << "[PushStreamer] Video capture start returned false, continue probing"
                      << std::endl;
        }

        // 始终添加轻量帧计数渲染器，保证采集源在常规推流模式下也被稳定拉帧。
        if (!frame_counter_) {
            frame_counter_ = std::make_unique<FrameCountingRenderer>(on_frame_);
            video_track_->AddRenderer(frame_counter_.get());
        }

        // 摄像头预热：默认 3 秒，可通过 CAPTURE_WARMUP_SEC 调整（慢主板可酌情减小）。
        if (config_.capture_warmup_sec > 0) {
            std::cout << "[PushStreamer] 摄像头预热 " << config_.capture_warmup_sec << " 秒..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(config_.capture_warmup_sec));
        } else {
            std::cout << "[PushStreamer] 摄像头预热已关闭 (CAPTURE_WARMUP_SEC=0)" << std::endl;
        }

        return true;
    }

    void CreateOffer() {
        std::cout << "[PushStreamer] 创建默认 Offer..." << std::endl;
        CreateOfferOnConnection("", peer_connection_);
    }

    void CreateOfferForPeer(const std::string& peer_id) {
        if (peer_id.empty()) {
            CreateOffer();
            return;
        }
        if (!EnsurePeerConnectionForPeer(peer_id)) {
            return;
        }
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] peer not found: " << peer_id << std::endl;
            return;
        }
        std::cout << "[PushStreamer] 为订阅者创建 Offer: " << peer_id << std::endl;
        CreateOfferOnConnection(peer_id, pc);
    }

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        SetRemoteDescriptionForPeer("", type, sdp);
    }

    void SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type,
                                     const std::string& sdp) {
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] SetRemoteDescription 找不到 peer: " << peer_id << std::endl;
            return;
        }
        std::cout << "[PushStreamer] 设置远端描述 peer=" << (peer_id.empty() ? "default" : peer_id)
                  << " type=" << type << " len=" << sdp.size() << std::endl;
        pc->SetRemoteDescription(
            libwebrtc::string(sdp.c_str()), libwebrtc::string(type.c_str()),
            []() { std::cout << "[PushStreamer] SetRemoteDescription 成功" << std::endl; },
            [](const char* err) {
                std::cerr << "[PushStreamer] SetRemoteDescription failed: " << err << std::endl;
            });
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        AddRemoteIceCandidateForPeer("", mid, mline_index, candidate);
    }

    void AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid,
                                      int mline_index, const std::string& candidate) {
        libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnection> pc = peer_connection_;
        if (!peer_id.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = peer_connections_.find(peer_id);
            if (it != peer_connections_.end()) pc = it->second;
        }
        if (!pc) {
            std::cerr << "[PushStreamer] AddCandidate 找不到 peer: " << peer_id << std::endl;
            return;
        }
        std::cout << "[PushStreamer] 添加远端 ICE 候选 peer=" << (peer_id.empty() ? "default" : peer_id)
                  << " mid=" << mid << " idx=" << mline_index << std::endl;
        pc->AddCandidate(libwebrtc::string(mid.c_str()), mline_index, libwebrtc::string(candidate.c_str()));
    }

    void DoLoopbackExchange(const std::string& offer_type, const std::string& offer_sdp) {
        std::cout << "[PushStreamer] 本地回环：创建接收端，验证 H264 编码..." << std::endl;
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        rtc_config.enable_dscp = true;
        if (!config_.enable_audio) {
            rtc_config.offer_to_receive_audio = false;
        }
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        receiver_ = factory_->Create(rtc_config, constraints);
        if (!receiver_) {
            std::cerr << "[PushStreamer] 创建回环接收端失败" << std::endl;
            return;
        }
        loopback_observer_ = std::make_unique<LoopbackReceiverObserver>(
            [this](const std::string& mid, int idx, const std::string& cand) {
                peer_connection_->AddCandidate(libwebrtc::string(mid.c_str()), idx,
                                               libwebrtc::string(cand.c_str()));
            });
        receiver_->RegisterRTCPeerConnectionObserver(loopback_observer_.get());

        libwebrtc::vector<libwebrtc::string> stream_ids;
        libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCRtpEncodingParameters>> encodings;
        auto init = libwebrtc::RTCRtpTransceiverInit::Create(
            libwebrtc::RTCRtpTransceiverDirection::kRecvOnly, stream_ids, encodings);
        receiver_->AddTransceiver(libwebrtc::RTCMediaType::VIDEO, init);

        receiver_->SetRemoteDescription(
            libwebrtc::string(offer_sdp.c_str()), libwebrtc::string(offer_type.c_str()),
            [this]() { OnReceiverRemoteDescriptionSet(); },
            [](const char* err) {
                std::cerr << "[PushStreamer] 接收端 SetRemoteDescription 失败: " << err << std::endl;
            });
    }

    void OnReceiverRemoteDescriptionSet() {
        auto ans_constraints = libwebrtc::RTCMediaConstraints::Create();
        if (!config_.enable_audio) {
            ans_constraints->AddOptionalConstraint(
                libwebrtc::RTCMediaConstraints::kOfferToReceiveAudio,
                libwebrtc::RTCMediaConstraints::kValueFalse);
        }
        receiver_->CreateAnswer(
            [this](const libwebrtc::string& ans_sdp, const libwebrtc::string& ans_type) {
                loopback_answer_sdp_ = ans_sdp.std_string();
                loopback_answer_type_ = ans_type.std_string();
                receiver_->SetLocalDescription(
                    ans_sdp, ans_type,
                    [this]() { OnReceiverLocalDescriptionSet(); },
                    [](const char* err) {
                        std::cerr << "[PushStreamer] 接收端 SetLocalDescription 失败: " << err << std::endl;
                    });
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] CreateAnswer 失败: " << err << std::endl;
            },
            ans_constraints);
    }

    void OnReceiverLocalDescriptionSet() {
        peer_connection_->SetRemoteDescription(
            libwebrtc::string(loopback_answer_sdp_.c_str()),
            libwebrtc::string(loopback_answer_type_.c_str()),
            []() {
                std::cout << "[PushStreamer] 回环 SDP 交换完成，等待 ICE 连接..." << std::endl;
            },
            [](const char* err) {
                std::cerr << "[PushStreamer] SetRemoteDescription 失败: " << err << std::endl;
            });
    }

    // RTCPeerConnectionObserver
    void NotifyConnectionState(libwebrtc::RTCPeerConnectionState state) {
        if (on_connection_state_) {
            ConnectionState cs = ConnectionState::New;
            switch (state) {
                case libwebrtc::RTCPeerConnectionStateConnecting:
                    cs = ConnectionState::Connecting;
                    break;
                case libwebrtc::RTCPeerConnectionStateConnected:
                    cs = ConnectionState::Connected;
                    break;
                case libwebrtc::RTCPeerConnectionStateDisconnected:
                    cs = ConnectionState::Disconnected;
                    break;
                case libwebrtc::RTCPeerConnectionStateFailed:
                    cs = ConnectionState::Failed;
                    break;
                case libwebrtc::RTCPeerConnectionStateClosed:
                    cs = ConnectionState::Closed;
                    break;
                default:
                    break;
            }
            on_connection_state_(cs);
        }
    }
    void OnSignalingState(libwebrtc::RTCSignalingState state) override {}
    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
        NotifyConnectionState(state);
    }
    void OnIceGatheringState(libwebrtc::RTCIceGatheringState state) override {
        const char* names[] = {"New", "Gathering", "Complete"};
        int idx = static_cast<int>(state);
        if (idx >= 0 && idx < 3) {
            std::cout << "[PushStreamer] ICE 收集状态: " << names[idx] << std::endl;
        }
    }
    void OnIceConnectionState(libwebrtc::RTCIceConnectionState state) override {
        const char* names[] = {"New", "Checking", "Connected", "Completed", "Failed", "Disconnected", "Closed"};
        int idx = static_cast<int>(state);
        if (idx >= 0 && idx < 7) {
            std::cout << "[PushStreamer] ICE 状态: " << names[idx] << std::endl;
        }
    }
    void OnIceCandidate(scoped_refptr<libwebrtc::RTCIceCandidate> candidate) override {
        if (!candidate) return;
        if (config_.test_encode_mode && receiver_) {
            receiver_->AddCandidate(
                candidate->sdp_mid(), candidate->sdp_mline_index(), candidate->candidate());
        } else if (on_ice_candidate_) {
            on_ice_candidate_("",
                              candidate->sdp_mid().std_string(),
                              candidate->sdp_mline_index(),
                              candidate->candidate().std_string());
        }
    }
    void OnPeerIceCandidate(const std::string& peer_id,
                            scoped_refptr<libwebrtc::RTCIceCandidate> candidate) {
        if (!candidate || !on_ice_candidate_) return;
        on_ice_candidate_(peer_id,
                          candidate->sdp_mid().std_string(),
                          candidate->sdp_mline_index(),
                          candidate->candidate().std_string());
    }
    void OnAddStream(scoped_refptr<libwebrtc::RTCMediaStream> stream) override {}
    void OnRemoveStream(scoped_refptr<libwebrtc::RTCMediaStream> stream) override {}
    void OnDataChannel(scoped_refptr<libwebrtc::RTCDataChannel> data_channel) override {}
    void OnRenegotiationNeeded() override {}
    void OnTrack(scoped_refptr<libwebrtc::RTCRtpTransceiver> transceiver) override {}
    void OnAddTrack(libwebrtc::vector<scoped_refptr<libwebrtc::RTCMediaStream>> streams,
                    scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {}
    void OnRemoveTrack(scoped_refptr<libwebrtc::RTCRtpReceiver> receiver) override {}

    PushStreamerConfig config_;
    scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
    scoped_refptr<libwebrtc::RTCPeerConnection> peer_connection_;
    scoped_refptr<libwebrtc::RTCMediaStream> local_stream_;
    scoped_refptr<libwebrtc::RTCVideoTrack> video_track_;
    scoped_refptr<libwebrtc::RTCVideoSource> video_source_;
    scoped_refptr<libwebrtc::RTCVideoCapturer> video_capturer_;

    OnSdpCallback on_sdp_;
    OnIceCandidateCallback on_ice_candidate_;
    OnConnectionStateCallback on_connection_state_;
    OnFrameCallback on_frame_;
    std::unique_ptr<FrameCountingRenderer> frame_counter_;
    scoped_refptr<libwebrtc::RTCPeerConnection> receiver_;
    std::unique_ptr<LoopbackReceiverObserver> loopback_observer_;
    std::string loopback_answer_sdp_;
    std::string loopback_answer_type_;
    std::unordered_map<std::string, scoped_refptr<libwebrtc::RTCPeerConnection>> peer_connections_;
    std::unordered_map<std::string, std::unique_ptr<ExtraPeerObserver>> extra_peer_observers_;
    std::mutex mutex_;
    std::unique_ptr<CaptureBridge> capture_bridge_;

public:
    void SetOnSdp(OnSdpCallback cb) { on_sdp_ = std::move(cb); }
    void SetOnIceCandidate(OnIceCandidateCallback cb) { on_ice_candidate_ = std::move(cb); }
    void SetOnConnectionState(OnConnectionStateCallback cb) { on_connection_state_ = std::move(cb); }
    void SetOnFrame(OnFrameCallback cb) { on_frame_ = std::move(cb); }
    unsigned int GetFrameCount() const {
        return frame_counter_ ? frame_counter_->GetFrameCount() : 0;
    }
    unsigned int GetDecodedFrameCount() const {
        return loopback_observer_ ? loopback_observer_->GetDecodedCount() : 0;
    }
    bool TestCaptureOnly() const { return config_.test_capture_only; }
    bool SignalingSubscriberOfferOnly() const { return config_.signaling_subscriber_offer_only; }
};

PushStreamer::PushStreamer(const PushStreamerConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PushStreamer::~PushStreamer() {
    Stop();
}

bool PushStreamer::Start() {
    if (is_streaming_) {
        return true;
    }
    if (!impl_->Initialize()) {
        return false;
    }
    if (!impl_->TestCaptureOnly() && !impl_->SignalingSubscriberOfferOnly()) {
        impl_->CreateOffer();
    }
    is_streaming_ = true;
    return true;
}

void PushStreamer::Stop() {
    if (!is_streaming_) {
        return;
    }
    impl_->Shutdown();
    is_streaming_ = false;
}

bool PushStreamer::SetRemoteDescription(const std::string& type, const std::string& sdp) {
    impl_->SetRemoteDescription(type, sdp);
    return true;
}

bool PushStreamer::SetRemoteDescriptionForPeer(const std::string& peer_id, const std::string& type,
                                               const std::string& sdp) {
    impl_->SetRemoteDescriptionForPeer(peer_id, type, sdp);
    return true;
}

void PushStreamer::AddRemoteIceCandidate(const std::string& mid, int mline_index,
                                         const std::string& candidate) {
    impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
}

void PushStreamer::AddRemoteIceCandidateForPeer(const std::string& peer_id, const std::string& mid,
                                                int mline_index, const std::string& candidate) {
    impl_->AddRemoteIceCandidateForPeer(peer_id, mid, mline_index, candidate);
}

void PushStreamer::CreateOfferForPeer(const std::string& peer_id) {
    impl_->CreateOfferForPeer(peer_id);
}

void PushStreamer::SetOnSdpCallback(OnSdpCallback cb) {
    impl_->SetOnSdp(std::move(cb));
}

void PushStreamer::SetOnIceCandidateCallback(OnIceCandidateCallback cb) {
    impl_->SetOnIceCandidate(std::move(cb));
}

void PushStreamer::SetOnConnectionStateCallback(OnConnectionStateCallback cb) {
    impl_->SetOnConnectionState(std::move(cb));
}

void PushStreamer::SetOnFrameCallback(OnFrameCallback cb) {
    impl_->SetOnFrame(std::move(cb));
}

unsigned int PushStreamer::GetFrameCount() const {
    return impl_->GetFrameCount();
}

unsigned int PushStreamer::GetDecodedFrameCount() const {
    return impl_->GetDecodedFrameCount();
}

}  // namespace webrtc_demo
