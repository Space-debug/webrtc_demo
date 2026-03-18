#include "camera_utils.h"
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <iostream>

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
    std::vector<std::string> video_devices;

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

int ParseVideoDeviceIndex(const std::string& device_path) {
    // /dev/video11 -> 11
    size_t pos = device_path.rfind("video");
    if (pos == std::string::npos) return -1;
    pos += 5;  // strlen("video")
    if (pos >= device_path.size()) return -1;
    return std::atoi(device_path.c_str() + pos);
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

}  // namespace webrtc_demo
