// 行为对齐 api/enable_media_with_defaults.cc（当前 libwebrtc.a 未链接 EnableMediaWithDefaults 时用手动配置）。

#include "webrtc_peer_connection_factory.h"

#include <memory>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/enable_media.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/thread.h"

namespace webrtc_demo {

void ConfigurePeerConnectionFactoryDependencies(
    webrtc::PeerConnectionFactoryDependencies& deps) {
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
    deps.video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  }
  if (deps.video_decoder_factory == nullptr) {
    deps.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
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
