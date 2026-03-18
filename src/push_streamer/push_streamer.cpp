#include "push_streamer.h"
#include "camera_utils.h"
#include <libwebrtc.h>

using libwebrtc::scoped_refptr;
#include <rtc_ice_candidate.h>
#include <rtc_mediaconstraints.h>
#include <rtc_media_stream.h>
#include <rtc_peerconnection.h>
#include <rtc_peerconnection_factory.h>
#include <rtc_rtp_parameters.h>
#include <rtc_rtp_receiver.h>
#include <rtc_rtp_transceiver.h>
#include <rtc_session_description.h>
#include <rtc_video_device.h>
#include <rtc_video_frame.h>
#include <rtc_video_source.h>
#include <rtc_video_track.h>
#include <helper.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace webrtc_demo {

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

    bool Initialize() {
        std::cout << "[PushStreamer] 初始化 LibWebRTC..." << std::endl;
        if (!libwebrtc::LibWebRTC::Initialize()) {
            std::cerr << "[PushStreamer] LibWebRTC init failed" << std::endl;
            return false;
        }
        std::cout << "[PushStreamer] LibWebRTC 初始化成功" << std::endl;

        std::cout << "[PushStreamer] 创建 PeerConnectionFactory..." << std::endl;
        factory_ = libwebrtc::LibWebRTC::CreateRTCPeerConnectionFactory();
        if (!factory_ || !factory_->Initialize()) {
            std::cerr << "[PushStreamer] PeerConnectionFactory init failed" << std::endl;
            return false;
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
        local_stream_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        video_capturer_ = nullptr;

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
        std::cout << "[PushStreamer] 已添加视频轨道 (stream_id=" << config_.stream_id << ")" << std::endl;

        return true;
    }

    bool CreateMediaTracks() {
        auto video_device = factory_->GetVideoDevice();
        if (!video_device) {
            std::cerr << "[PushStreamer] GetVideoDevice() 返回空，libwebrtc 可能未启用 V4L2 或依赖缺失"
                      << "（需 libX11、libglib 等，见 docs/linux_arm64_build_notes.md）" << std::endl;
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
            return false;
        }

        uint32_t idx = static_cast<uint32_t>(config_.video_device_index);
        const char* device_name = "camera";

        if (!config_.video_device_path.empty()) {
            // 通过 bus_info 将 V4L2 路径映射到 libwebrtc 的 device index
            std::string bus_info = webrtc_demo::GetDeviceBusInfo(config_.video_device_path);
            uint32_t num_devices = video_device->NumberOfDevices();
            bool found = false;
            for (uint32_t i = 0; i < num_devices; ++i) {
                char name[256] = {0}, id[256] = {0};
                video_device->GetDeviceName(i, name, sizeof(name), id, sizeof(id));
                if (!bus_info.empty() && std::string(id) == bus_info) {
                    idx = i;
                    device_name = config_.video_device_path.c_str();
                    found = true;
                    std::cout << "[PushStreamer] Using USB camera: " << config_.video_device_path
                              << " -> libwebrtc index " << idx << " (" << name << ")" << std::endl;
                    break;
                }
            }
            if (!found && !bus_info.empty()) {
                std::cerr << "[PushStreamer] No libwebrtc device matches " << config_.video_device_path
                          << " (bus_info=" << bus_info << "), using index 0" << std::endl;
                idx = 0;
                device_name = config_.video_device_path.c_str();
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
        }

        // 若设置了帧回调，添加用于验证采集的帧计数渲染器
        if (on_frame_) {
            frame_counter_ = std::make_unique<FrameCountingRenderer>(on_frame_);
            video_track_->AddRenderer(frame_counter_.get());
        }

        // 摄像头预热：等待 3 秒让 V4L2 稳定输出帧，避免 OpenH264 输入帧率 0 导致 0 RTP
        std::cout << "[PushStreamer] 摄像头预热 3 秒..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));

        return true;
    }

    void CreateOffer() {
        std::cout << "[PushStreamer] 创建 Offer..." << std::endl;
        auto constraints = libwebrtc::RTCMediaConstraints::Create();
        // 禁用 simulcast，仅发送单层 640x480，避免接收端分辨率在 320x240/480x360/640x480 间切换
        constraints->AddOptionalConstraint(
            libwebrtc::RTCMediaConstraints::kNumSimulcastLayers, "1");
        peer_connection_->CreateOffer(
            [this](const libwebrtc::string& sdp, const libwebrtc::string& type) {
                std::lock_guard<std::mutex> lock(mutex_);
                std::string type_str = type.std_string();
                std::string sdp_str = sdp.std_string();
                peer_connection_->SetLocalDescription(
                    sdp, type,
                    [this, type_str, sdp_str]() {
                        std::cout << "[PushStreamer] SetLocalDescription success" << std::endl;
                        if (config_.test_encode_mode) {
                            DoLoopbackExchange(type_str, sdp_str);
                        } else if (on_sdp_) {
                            on_sdp_(type_str, sdp_str);
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

    void SetRemoteDescription(const std::string& type, const std::string& sdp) {
        std::cout << "[PushStreamer] 设置远端描述 (type=" << type << ", len=" << sdp.size() << ")..." << std::endl;
        peer_connection_->SetRemoteDescription(
            libwebrtc::string(sdp.c_str()), libwebrtc::string(type.c_str()),
            []() { std::cout << "[PushStreamer] SetRemoteDescription 成功" << std::endl; },
            [](const char* err) {
                std::cerr << "[PushStreamer] SetRemoteDescription failed: " << err << std::endl;
            });
    }

    void AddRemoteIceCandidate(const std::string& mid, int mline_index, const std::string& candidate) {
        std::cout << "[PushStreamer] 添加远端 ICE 候选 mid=" << mid << " idx=" << mline_index << std::endl;
        peer_connection_->AddCandidate(libwebrtc::string(mid.c_str()), mline_index,
                                       libwebrtc::string(candidate.c_str()));
    }

    void DoLoopbackExchange(const std::string& offer_type, const std::string& offer_sdp) {
        std::cout << "[PushStreamer] 本地回环：创建接收端，验证 H264 编码..." << std::endl;
        libwebrtc::RTCConfiguration rtc_config;
        rtc_config.ice_servers[0].uri = config_.stun_server;
        rtc_config.disable_ipv6 = true;
        rtc_config.disable_link_local_networks = true;
        rtc_config.tcp_candidate_policy = libwebrtc::TcpCandidatePolicy::kTcpCandidatePolicyDisabled;
        rtc_config.enable_dscp = true;
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
            libwebrtc::RTCMediaConstraints::Create());
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
    void OnSignalingState(libwebrtc::RTCSignalingState state) override {}
    void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState state) override {
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.test_encode_mode && receiver_ && candidate) {
            receiver_->AddCandidate(
                candidate->sdp_mid(), candidate->sdp_mline_index(), candidate->candidate());
        } else if (on_ice_candidate_ && candidate) {
            on_ice_candidate_(candidate->sdp_mid().std_string(),
                              candidate->sdp_mline_index(),
                              candidate->candidate().std_string());
        }
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
    std::mutex mutex_;

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
    if (!impl_->TestCaptureOnly()) {
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

void PushStreamer::AddRemoteIceCandidate(const std::string& mid, int mline_index,
                                         const std::string& candidate) {
    impl_->AddRemoteIceCandidate(mid, mline_index, candidate);
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
