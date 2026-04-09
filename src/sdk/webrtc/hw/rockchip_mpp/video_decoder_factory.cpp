#include "webrtc/hw/rockchip_mpp/video_decoder_factory.h"

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"

#include "webrtc/hw/rockchip_mpp/h264_decoder.h"

namespace webrtc_demo::hw::rockchip_mpp {

namespace {

class PreferredVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    PreferredVideoDecoderFactory() : builtin_(webrtc::CreateBuiltinVideoDecoderFactory()) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return builtin_->GetSupportedFormats(); }

    webrtc::VideoDecoderFactory::CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,
                                                               bool reference_scaling) const override {
        if (format.name == "H264" && !reference_scaling) {
            webrtc::VideoDecoderFactory::CodecSupport s;
            s.is_supported = true;
            s.is_power_efficient = true;
            return s;
        }
        return builtin_->QueryCodecSupport(format, reference_scaling);
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                                 const webrtc::SdpVideoFormat& format) override {
        if (format.name == "H264") {
            return std::make_unique<H264Decoder>(env);
        }
        return builtin_->Create(env, format);
    }

private:
    std::unique_ptr<webrtc::VideoDecoderFactory> builtin_;
};

}  // namespace

std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory() {
    return std::make_unique<PreferredVideoDecoderFactory>();
}

}  // namespace webrtc_demo::hw::rockchip_mpp

