# WebRTC 推流 Demo

基于 libwebrtc 的 WebRTC 推流示例项目，支持从摄像头采集视频并推送到远端。

## 项目结构

```
webrtc_demo/
├── CMakeLists.txt          # CMake 构建配置
├── README.md
├── .gitignore
├── docs/                    # 文档
├── include/                 # 项目头文件
│   └── push_streamer.h
├── scripts/                 # 构建脚本
│   ├── build.sh
│   └── run.sh
├── src/                     # 源代码
│   ├── main.cpp
│   └── push_streamer/
│       └── push_streamer.cpp
└── 3rdparty/                # 第三方依赖
    └── libwebrtc/
        ├── linux/
        │   ├── arm64/       # Linux aarch64
        │   └── x64/         # Linux x86_64
        └── win/
            └── x64/         # Windows x86_64
```

## 依赖

- CMake >= 3.14
- C++17 编译器 (GCC 7+ / Clang 5+ / MSVC)
- libwebrtc 预编译库

### 平台与架构

按平台和架构将 libwebrtc 放入对应目录，每个目录需包含 `include/` 和 `lib/`：

| 平台 | 架构 | 路径 |
|------|------|------|
| Linux | arm64 (aarch64) | `3rdparty/libwebrtc/linux/arm64/` |
| Linux | x64 (x86_64) | `3rdparty/libwebrtc/linux/x64/` |
| Windows | x64 | `3rdparty/libwebrtc/win/x64/` |

## 构建

```bash
# 使用脚本构建
./scripts/build.sh

# 或手动构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 运行

```bash
# 列出本地 USB 摄像头
./scripts/run.sh --list-cameras

# P2P 推流（一键启动信令服务器 + 推流端）
./scripts/start_p2p.sh livestream /dev/video11

# 拉流端
./build/bin/p2p_player
```

## 配置

通过命令行参数配置，见 `./build/bin/webrtc_push_demo --help`。

## License

请遵循 libwebrtc 及项目相关组件的各自许可协议。
