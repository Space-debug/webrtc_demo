# 项目架构说明

## 目录结构

```
webrtc_demo/
├── CMakeLists.txt          # 顶层 CMake 配置
├── config/                 # 配置文件
├── docs/                   # 文档
├── include/                # 项目公共头文件（如 push_streamer.h）
├── scripts/                # 构建与运行脚本
├── src/
│   ├── app/                # push_demo.cpp（推流） pull_demo.cpp（拉流）
│   ├── sdk/
│   │   └── media/          # 推拉流：PushStreamer、PullSubscriber、采集轨道等
│   └── tools/              # 独立工具（如 signaling_server）
└── 3rdparty/
    └── libwebrtc/
        ├── include/        # 官方 WebRTC 头文件树
        └── lib/linux/arm64/libwebrtc.a
```

## 模块说明

### PushStreamer / PullSubscriber

- **PushStreamer**（`include/push_streamer.h`）：推流；采集、Offer/ICE 回调、多订阅者等。
- **PullSubscriber**（`include/pull_subscriber.h`）：拉流；Answer、解码帧回调（ARGB）、与同一 TCP 信令协议对接。

演示接线见 `src/app/push_demo.cpp`、`pull_demo.cpp`（最少集成示例）。

### 信令流程

1. 推流端调用 `Start()`，创建 Offer
2. 通过 `OnSdpCallback` 获取 Offer，发送给对端
3. 通过 `OnIceCandidateCallback` 获取 ICE 候选，发送给对端
4. 收到对端 Answer 后调用 `SetRemoteDescription()`
5. 收到对端 ICE 候选后调用 `AddRemoteIceCandidate()`
