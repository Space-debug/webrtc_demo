// 行为对齐 api/enable_media_with_defaults.cc（当前 libwebrtc.a 未链接 EnableMediaWithDefaults 时用手动配置）。

#include "webrtc_peer_connection_factory.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/enable_media.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "webrtc/hw/backend_registry.h"

namespace webrtc_demo {

namespace {
  std::once_flag g_field_trials_once;
  std::string g_field_trials_storage;

  bool EnvTruthy(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
      return false;
    }
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
  }

  /// 客户端低延迟：FieldTrial WebRTC-ZeroPlayoutDelay（pacing + max_decode_queue_size）。
  /// 脚本 steady profile：2ms pacing + 队列 8；仍可用 WEBRTC_DEMO_* 覆盖（4..16）。
  std::string ZeroPlayoutDelayTrialString() {
    int pacing_ms = 1;
    if (const char* p = std::getenv("WEBRTC_DEMO_ZERO_PLAYOUT_MIN_PACING_MS")) {
      int v = std::atoi(p);
      if (v >= 0 && v <= 20) {
        pacing_ms = v;
      }
    }
    int q = 6;
    if (const char* p = std::getenv("WEBRTC_DEMO_MAX_DECODE_QUEUE_SIZE")) {
      int v = std::atoi(p);
      if (v >= 4 && v <= 16) {
        q = v;
      }
    }
    return "WebRTC-ZeroPlayoutDelay/min_pacing:" + std::to_string(pacing_ms) +
           "ms,max_decode_queue_size:" + std::to_string(q) + "/";
  }
}  // namespace

void EnsureWebrtcFieldTrialsInitialized() {
  std::call_once(g_field_trials_once, []() {
    // ForcePlayoutDelay：收端尽快解码；ZeroPlayoutDelay：低延迟路径下调度参数（可调 env）。
    // 默认关 FlexFEC 降发送开销与延迟；丢包网络可 export WEBRTC_DEMO_ENABLE_FLEXFEC=1。
    // Pacer：关键帧冲刷、快重传，减轻发送侧排队。
    g_field_trials_storage =
        "WebRTC-VideoFrameTrackingIdAdvertised/Enabled/"
        "WebRTC-ForcePlayoutDelay/min_ms:0,max_ms:0/";
    g_field_trials_storage += ZeroPlayoutDelayTrialString();
    g_field_trials_storage +=
        "WebRTC-Pacer-KeyframeFlushing/Enabled/"
        "WebRTC-Pacer-FastRetransmissions/Enabled/";
    if (EnvTruthy("WEBRTC_DEMO_ENABLE_FLEXFEC")) {
      g_field_trials_storage +=
          "WebRTC-FlexFEC-03-Advertised/Enabled/"
          "WebRTC-FlexFEC-03/Enabled/";
    }
    if (const char* extra = std::getenv("WEBRTC_DEMO_FIELD_TRIALS_APPEND")) {
      if (extra[0] != '\0') {
        g_field_trials_storage += extra;
      }
    }
    webrtc::field_trial::InitFieldTrialsFromString(g_field_trials_storage.c_str());
  });
}

void ConfigurePeerConnectionFactoryDependencies(
    webrtc::PeerConnectionFactoryDependencies& deps,
    const PeerConnectionFactoryMediaOptions* media_options) {
  if (deps.task_queue_factory == nullptr) {
    deps.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
  }
  if (deps.audio_encoder_factory == nullptr) {
    deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  }
  if (deps.audio_decoder_factory == nullptr) {
    deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  }
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (deps.audio_processing == nullptr &&
      deps.audio_processing_builder == nullptr) {
    deps.audio_processing_builder =
        std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();
  }
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  if (deps.video_encoder_factory == nullptr) {
    webrtc_demo::hw::VideoBackendPreferences prefs;
    if (media_options) {
      prefs.encoder_backend = media_options->encoder_backend == webrtc_demo::VideoCodecBackendPreference::kRockchipMpp
                                  ? webrtc_demo::hw::VideoCodecBackend::kRockchipMpp
                                  : webrtc_demo::hw::VideoCodecBackend::kBuiltin;
    }
    deps.video_encoder_factory = webrtc_demo::hw::CreatePreferredVideoEncoderFactory(prefs);
  }
  if (deps.video_decoder_factory == nullptr) {
    webrtc_demo::hw::VideoBackendPreferences prefs;
    if (media_options) {
      prefs.decoder_backend = media_options->decoder_backend == webrtc_demo::VideoCodecBackendPreference::kRockchipMpp
                                  ? webrtc_demo::hw::VideoCodecBackend::kRockchipMpp
                                  : webrtc_demo::hw::VideoCodecBackend::kBuiltin;
    }
    deps.video_decoder_factory = webrtc_demo::hw::CreatePreferredVideoDecoderFactory(prefs);
  }
  webrtc::EnableMedia(deps);
}

void EnsureDedicatedPeerConnectionSignalingThread(
    webrtc::PeerConnectionFactoryDependencies& deps,
    std::unique_ptr<webrtc::Thread>* owned_signaling_thread) {
  if (!owned_signaling_thread || deps.signaling_thread != nullptr) {
    return;
  }
  auto th = webrtc::Thread::CreateWithSocketServer();
  th->SetName("webrtc_sig", nullptr);
  th->Start();
  deps.signaling_thread = th.get();
  *owned_signaling_thread = std::move(th);
}

}  // namespace webrtc_demo
