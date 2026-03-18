#ifndef CAMERA_UTILS_H
#define CAMERA_UTILS_H

#include <string>
#include <vector>

namespace webrtc_demo {

/// USB 摄像头信息
struct UsbCameraInfo {
    std::string device_path;   // 如 /dev/video11
    std::string device_name;   // 设备名称，如 "LRCP imx291"
    std::string bus_info;      // 总线信息
    int index{-1};             // 设备编号（用于 libwebrtc 的 device_index）
};

/// 枚举本地所有 USB 摄像头（V4L2）
std::vector<UsbCameraInfo> ListUsbCameras();

/// 从设备路径解析设备编号，如 /dev/video11 -> 11
int ParseVideoDeviceIndex(const std::string& device_path);

/// 获取设备的 bus_info（用于与 libwebrtc 的 device id 匹配）
std::string GetDeviceBusInfo(const std::string& device_path);

}  // namespace webrtc_demo

#endif  // CAMERA_UTILS_H
