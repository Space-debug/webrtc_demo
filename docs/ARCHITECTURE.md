# 项目架构说明

## 目录结构

```
webrtc_demo/
├── CMakeLists.txt          # 顶层 CMake 配置
├── config/                 # 配置文件
├── docs/                   # 文档
├── include/                # 项目公共头文件
├── scripts/                # 构建与运行脚本
├── src/                    # 源代码
│   ├── main.cpp            # 程序入口
│   └── push_streamer/      # 推流模块
└── 3rdparty/              # 第三方库
    └── libwebrtc/         # 按平台/架构组织
        ├── linux/arm64/
        ├── linux/x64/
        └── win/x64/
```

## 模块说明

### PushStreamer

推流核心类，负责：

- 初始化 libwebrtc
- 创建 PeerConnection 和媒体轨道
- 从摄像头采集视频
- 生成 SDP Offer 和 ICE 候选
- 通过回调将 SDP/ICE 交给上层做信令交换

### 信令流程

1. 推流端调用 `Start()`，创建 Offer
2. 通过 `OnSdpCallback` 获取 Offer，发送给对端
3. 通过 `OnIceCandidateCallback` 获取 ICE 候选，发送给对端
4. 收到对端 Answer 后调用 `SetRemoteDescription()`
5. 收到对端 ICE 候选后调用 `AddRemoteIceCandidate()`
