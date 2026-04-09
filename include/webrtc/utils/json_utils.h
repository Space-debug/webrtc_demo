#ifndef WEBRTC_DEMO_UTILS_JSON_UTILS_H_
#define WEBRTC_DEMO_UTILS_JSON_UTILS_H_

#include <string>
#include <string_view>

namespace webrtc_demo::utils {

// Internal utility: shared by SDK modules, not a stable external API.
std::string EscapeJsonString(std::string_view input);
// Internal utility: lightweight field extraction for simple signaling JSON lines.
std::string ExtractJsonString(std::string_view line, std::string_view key);
// Internal utility: integer field extraction with fallback.
int ExtractJsonInt(std::string_view line, std::string_view key, int fallback = 0);

}  // namespace webrtc_demo::utils

#endif  // WEBRTC_DEMO_UTILS_JSON_UTILS_H_

