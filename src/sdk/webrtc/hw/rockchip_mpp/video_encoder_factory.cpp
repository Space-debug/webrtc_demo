#include "webrtc/hw/rockchip_mpp/video_encoder_factory.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

// 现阶段仅接入 Rockchip MPP H264 encoder；后续可在同 backend 内扩展 HEVC/AV1 等。
#include "webrtc/hw/rockchip_mpp/h264_encoder.h"

namespace webrtc_demo::hw::rockchip_mpp {

namespace {

bool IsH264FormatName(const std::string& name) {
    if (name.size() != 4) {
        return false;
    }
    return (name[0] == 'H' || name[0] == 'h') && name[1] == '2' && name[2] == '6' && name[3] == '4';
}

class MppH264PrimaryEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    explicit MppH264PrimaryEncoderFactory(webrtc::InternalEncoderFactory* query_delegate)
        : query_delegate_(query_delegate) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return webrtc::SupportedH264Codecs();
    }

    webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                                                std::optional<std::string> scalability_mode) const override {
        return query_delegate_->QueryCodecSupport(format, scalability_mode);
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        webrtc::H264EncoderSettings settings = webrtc::H264EncoderSettings::Parse(format);
        return std::make_unique<webrtc_demo::RkMppH264Encoder>(env, settings);
    }

private:
    webrtc::InternalEncoderFactory* query_delegate_;
};

class PreferredVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    PreferredVideoEncoderFactory()
        : mpp_h264_primary_(&internal_fallback_), builtin_(webrtc::CreateBuiltinVideoEncoderFactory()) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return builtin_->GetSupportedFormats(); }

    webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                                                std::optional<std::string> scalability_mode) const override {
        return builtin_->QueryCodecSupport(format, scalability_mode);
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (!IsH264FormatName(format.name)) {
            return builtin_->Create(env, format);
        }
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(env, &mpp_h264_primary_, &internal_fallback_, format);
    }

private:
    webrtc::InternalEncoderFactory internal_fallback_;
    MppH264PrimaryEncoderFactory mpp_h264_primary_;
    std::unique_ptr<webrtc::VideoEncoderFactory> builtin_;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory() {
    return std::make_unique<PreferredVideoEncoderFactory>();
}

}  // namespace webrtc_demo::hw::rockchip_mpp

