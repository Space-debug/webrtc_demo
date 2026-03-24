#ifndef WEBRTC_FIELD_TRIALS_H
#define WEBRTC_FIELD_TRIALS_H

#include <string>

namespace webrtc_demo {

/// 在 libwebrtc::LibWebRTC::Initialize() 之前调用。
/// enable=true 时设置/合并 WEBRTC_FIELD_TRIALS，打开 FlexFEC-03（与 libwebrtc.so 内字符串一致）。
/// override_trials 非空时仅使用该串（高级用法）；否则在已有环境变量后追加默认 FlexFEC  trials。
/// 收发两端建议同时 enable，否则对端无法解析 FEC RTP。
void EnsureFlexfecFieldTrials(bool enable_flexfec, const std::string& override_trials = {});

}  // namespace webrtc_demo

#endif
