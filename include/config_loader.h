#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <unordered_map>

namespace webrtc_demo {

/// 简单 KEY=value 配置加载器，兼容 config/streams.conf 格式
class ConfigLoader {
public:
    /// 从文件加载，返回是否成功
    bool Load(const std::string& path);

    /// 获取字符串，key 不存在时返回 default_val
    std::string Get(const std::string& key, const std::string& default_val = "") const;

    /// 获取整数
    int GetInt(const std::string& key, int default_val = 0) const;

    /// 获取指定 stream：优先 STREAM_<id>_KEY，否则回退全局同名 KEY（适合「公共参数 + 各路只写差异」）
    std::string GetStream(const std::string& stream_id, const std::string& key,
                         const std::string& default_val = "") const;
    int GetStreamInt(const std::string& stream_id, const std::string& key,
                     int default_val = 0) const;

    bool empty() const { return map_.empty(); }

private:
    std::unordered_map<std::string, std::string> map_;
};

}  // namespace webrtc_demo

#endif  // CONFIG_LOADER_H
