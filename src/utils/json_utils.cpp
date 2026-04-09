#include "webrtc/utils/json_utils.h"

#include <cstdlib>

namespace webrtc_demo::utils {

std::string EscapeJsonString(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string ExtractJsonString(std::string_view line, std::string_view key) {
    const std::string token = std::string("\"") + std::string(key) + "\":\"";
    size_t p = line.find(token);
    if (p == std::string::npos) {
        return "";
    }
    p += token.size();

    std::string out;
    out.reserve(line.size() - p);
    for (size_t i = p; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            const char n = line[i + 1];
            if (n == 'n') {
                out.push_back('\n');
            } else if (n == 'r') {
                out.push_back('\r');
            } else {
                out.push_back(n);
            }
            ++i;
            continue;
        }
        if (line[i] == '"') {
            break;
        }
        out.push_back(line[i]);
    }
    return out;
}

int ExtractJsonInt(std::string_view line, std::string_view key, int fallback) {
    const std::string token = std::string("\"") + std::string(key) + "\":";
    size_t p = line.find(token);
    if (p == std::string::npos) {
        return fallback;
    }
    p += token.size();
    return std::atoi(std::string(line.substr(p)).c_str());
}

}  // namespace webrtc_demo::utils

