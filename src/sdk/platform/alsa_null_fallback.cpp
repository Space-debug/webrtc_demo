#include "platform/alsa_null_fallback.h"

#include <cstdlib>
#include <fstream>

namespace webrtc_demo {

void EnableAlsaNullDeviceFallback() {
    const char* path = "/tmp/webrtc_demo_alsa_null.conf";
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << "pcm.!default {\n"
               "  type asym\n"
               "  playback.pcm \"null\"\n"
               "  capture.pcm \"null\"\n"
               "}\n";
        out.close();
        setenv("ALSA_CONFIG_PATH", path, 1);
    }
}

}  // namespace webrtc_demo
