# libwebrtc 诊断报告

## 测试环境

- **系统**: Linux 5.10.110-rt53-preempt, aarch64
- **glibc**: 2.31
- **USB 相机**: LRCP imx291 (Microdia), /dev/video11

## 测试结论

### ✅ 可正常使用的功能

| 步骤 | 状态 | 说明 |
|------|------|------|
| LibWebRTC::Initialize() | OK | 初始化成功 |
| CreateRTCPeerConnectionFactory() | OK | 工厂创建成功 |
| Factory->Initialize() | OK | 工厂初始化成功 |
| GetVideoDevice() | OK | 获取视频设备成功 |
| Create(capturer) | OK | 创建采集器成功 |
| StartCapture() | OK | 开始采集成功 |

### ❌ 存在问题的功能

| 步骤 | 状态 | 说明 |
|------|------|------|
| **CreateVideoTrack()** | **段错误** | 调用时崩溃 |

## 崩溃根因

GDB 堆栈显示崩溃发生在 `CreateDesktopSource` 而非 `CreateVideoTrack`：

```
#0  libwebrtc::RTCPeerConnectionFactoryImpl::CreateDesktopSource(...)
#1  main ()
```

### 结论：vtable 错位（RTC_DESKTOP_DEVICE 不一致）

根据 `docs/linux_arm64_build_notes.md` 的构建记录，libwebrtc 是**带桌面采集**编译的（导出符号含 DesktopCapturer/Device/MediaList），即库在编译时定义了 `RTC_DESKTOP_DEVICE`。

而 webrtc_demo 应用在编译时**未定义** `RTC_DESKTOP_DEVICE`，导致头文件中的虚函数表布局与库不一致：

| 虚函数 slot | 库（有 RTC_DESKTOP_DEVICE） | 应用（无 RTC_DESKTOP_DEVICE） |
|-------------|----------------------------|-------------------------------|
| 10          | CreateDesktopSource        | **CreateVideoTrack**          |
| 11          | CreateAudioTrack           | CreateStream                  |
| 12          | **CreateVideoTrack**      | ...                           |

应用调用 `CreateVideoTrack` 时使用的是 slot 10，但库的 slot 10 实际是 `CreateDesktopSource`。于是错误地调用了 `CreateDesktopSource(RTCVideoSource*, ...)`，而该函数期望 `RTCDesktopCapturer*`，类型不匹配导致崩溃。

## libwebrtc 设备枚举

- **NumberOfDevices**: 1（仅枚举到 USB 相机）
- **设备 [0]**: LRCP imx291, id=usb-fc800000.usb-1
- **注意**：libwebrtc 的 device index 与 V4L2 的 /dev/videoN 编号**不一致**，需通过 bus_info 映射

## 修复方法（已应用）

1. **vtable 错位**：在 CMakeLists.txt 中添加 `target_compile_definitions(webrtc_push_demo PRIVATE RTC_DESKTOP_DEVICE)`，使应用与库的 vtable 布局一致。

2. **Unified Plan**：将 `AddStream` 改为 `AddTrack`，因 libwebrtc 使用 Unified Plan SDP 语义。

## 其他建议

1. **联系 libwebrtc 提供方**，确认：
   - 编译时是否启用了 `RTC_DESKTOP_DEVICE`
   - 是否有 CreateVideoTrack 相关已知问题
   - 是否有修复版本或补丁

2. **尝试其他 libwebrtc 版本**，如：
   - [webrtc-sdk/libwebrtc](https://github.com/webrtc-sdk/libwebrtc)
   - 官方 WebRTC 预编译包

3. **运行诊断测试**：
   ```bash
   ./build/bin/test_libwebrtc
   ```

## 诊断工具

- `./build/bin/test_libwebrtc` - 分步测试 libwebrtc 各接口
- `./build/bin/webrtc_push_demo --list-cameras` - 列出 V4L2 摄像头
