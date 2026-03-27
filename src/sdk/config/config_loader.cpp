#include "config_loader.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace webrtc_demo {

static std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool ConfigLoader::Load(const std::string& path) {
    map_.clear();
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line = Trim(line);
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (!key.empty()) map_[key] = val;
    }
    return true;
}

std::string ConfigLoader::Get(const std::string& key, const std::string& default_val) const {
    auto it = map_.find(key);
    return it != map_.end() ? it->second : default_val;
}

int ConfigLoader::GetInt(const std::string& key, int default_val) const {
    std::string s = Get(key, "");
    if (s.empty()) return default_val;
    return std::atoi(s.c_str());
}

std::string ConfigLoader::GetStream(const std::string& stream_id, const std::string& key,
                                    const std::string& default_val) const {
    std::string stream_key = "STREAM_" + stream_id + "_" + key;
    auto it = map_.find(stream_key);
    if (it != map_.end()) return it->second;
    return Get(key, default_val);
}

int ConfigLoader::GetStreamInt(const std::string& stream_id, const std::string& key,
                               int default_val) const {
    std::string s = GetStream(stream_id, key, "");
    if (s.empty()) return default_val;
    return std::atoi(s.c_str());
}

}  // namespace webrtc_demo
