#include "camera/camera_utils.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace webrtc_demo {

static bool IsVideoCaptureDevice(const std::string& path) {
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) return false;

    struct v4l2_capability cap = {};
    bool is_capture = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        is_capture = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
    }
    close(fd);
    return is_capture;
}

std::vector<UsbCameraInfo> ListUsbCameras() {
    std::vector<UsbCameraInfo> cameras;

    // 枚举 /dev/video*
    for (int i = 0; i < 32; ++i) {
        std::string path = "/dev/video" + std::to_string(i);
        if (access(path.c_str(), F_OK) != 0) continue;
        if (!IsVideoCaptureDevice(path)) continue;

        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) continue;

        struct v4l2_capability cap = {};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            close(fd);
            continue;
        }

        UsbCameraInfo info;
        info.device_path = path;
        info.index = i;
        info.device_name = reinterpret_cast<char*>(cap.card);
        info.bus_info = reinterpret_cast<char*>(cap.bus_info);

        cameras.push_back(info);
        close(fd);
    }

    return cameras;
}

std::string GetDeviceBusInfo(const std::string& device_path) {
    int fd = open(device_path.c_str(), O_RDWR);
    if (fd < 0) return "";

    struct v4l2_capability cap = {};
    std::string bus_info;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        bus_info = reinterpret_cast<char*>(cap.bus_info);
    }
    close(fd);
    return bus_info;
}

std::string GetDeviceCardName(const std::string& device_path) {
    int fd = open(device_path.c_str(), O_RDWR);
    if (fd < 0) return "";

    struct v4l2_capability cap = {};
    std::string card;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        card = reinterpret_cast<char*>(cap.card);
    }
    close(fd);
    return card;
}

int GetWebRtcCaptureDeviceIndexForPath(const std::string& device_path) {
    static constexpr const char* kPrefix = "/dev/video";
    const size_t plen = strlen(kPrefix);
    if (device_path.size() <= plen || device_path.compare(0, plen, kPrefix) != 0) {
        return -1;
    }
    int target = std::atoi(device_path.c_str() + plen);
    if (target < 0 || target >= 64) {
        return -1;
    }
    int capture_index = 0;
    for (int n = 0; n <= target; ++n) {
        std::string trypath = std::string(kPrefix) + std::to_string(n);
        int fd = open(trypath.c_str(), O_RDONLY);
        if (fd < 0) {
            continue;
        }
        struct v4l2_capability cap = {};
        bool ok = (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0);
        close(fd);
        if (!ok) {
            continue;
        }
        if (n == target) {
            return capture_index;
        }
        ++capture_index;
    }
    return -1;
}

}  // namespace webrtc_demo
