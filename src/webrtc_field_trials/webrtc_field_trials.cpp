#include "webrtc_field_trials.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace webrtc_demo {

namespace {

constexpr char kFlexfecTrialKey[] = "WebRTC-FlexFEC-03";
// 与 libwebrtc 二进制中 trial 名称一致；分组名依常见 Chromium/libwebrtc 构建
constexpr char kFlexfecDefaultTrials[] =
    "WebRTC-FlexFEC-03/Enabled/WebRTC-FlexFEC-03-Advertised/Enabled/";

void TrimTrailingSpace(std::string* s) {
    if (!s) return;
    while (!s->empty() && (s->back() == ' ' || s->back() == '\t' || s->back() == '\r' || s->back() == '\n')) {
        s->pop_back();
    }
}

}  // namespace

void EnsureFlexfecFieldTrials(bool enable_flexfec, const std::string& override_trials) {
    if (!enable_flexfec) {
        return;
    }

    std::string out;
    if (!override_trials.empty()) {
        out = override_trials;
        TrimTrailingSpace(&out);
        if (!out.empty() && out.back() != '/') {
            out += '/';
        }
    } else {
        const char* existing = std::getenv("WEBRTC_FIELD_TRIALS");
        if (existing && existing[0] != '\0') {
            out = existing;
            if (out.back() != '/') {
                out += '/';
            }
        }
        // 避免重复追加
        if (out.find(kFlexfecTrialKey) == std::string::npos) {
            out += kFlexfecDefaultTrials;
        }
    }

    if (::setenv("WEBRTC_FIELD_TRIALS", out.c_str(), 1) != 0) {
        std::cerr << "[WebRTC] setenv WEBRTC_FIELD_TRIALS failed" << std::endl;
        return;
    }

    std::cout << "[WebRTC] ENABLE_FLEXFEC: set WEBRTC_FIELD_TRIALS (FlexFEC-03), len=" << out.size()
              << ". Receiver needs ENABLE_FLEXFEC=1 too." << std::endl;
}

}  // namespace webrtc_demo
