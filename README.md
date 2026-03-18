# WebRTC 推流 Demo

基于 libwebrtc 的 WebRTC P2P 推拉流示例：
- 推流端采集摄像头并发送视频
- 拉流端使用 SDL2 播放
- 内置 C++ 信令服务器（TCP 8765）

## 项目结构

```text
webrtc_demo/
├── CMakeLists.txt
├── README.md
├── docs/
├── include/
├── src/
├── examples/
│   └── p2p_player/             # SDL2 拉流端
├── scripts/
│   ├── build.sh                # 编译脚本
│   ├── push.sh                 # 推流脚本（可自动拉起信令）
│   └── pull.sh                 # 拉流脚本（本地/远端通用）
└── 3rdparty/
    └── libwebrtc/
        ├── include/            # 头文件（全平台共享）
        └── lib/
            ├── linux/
            │   ├── arm64/
            │   │   └── libwebrtc.so
            │   └── x64/
            │       └── libwebrtc.so
            └── win/
                └── x64/
```

## 依赖

- CMake >= 3.14
- C++17 编译器（GCC/Clang）
- SDL2（拉流端显示）

Linux 安装 SDL2 开发包：

```bash
sudo apt install libsdl2-dev
```

## 编译

```bash
./scripts/build.sh
```

构建产物：
- `build/bin/webrtc_push_demo`
- `build/bin/p2p_player`
- `build/bin/signaling_server`

## 运行方式

### 1) 启动推流端（服务器）

```bash
./scripts/push.sh livestream /dev/video11
```

默认行为：
- 自动启动 `signaling_server`（`START_SIGNALING=1`）
- 信令地址默认 `127.0.0.1:8765`（可用 `SIGNALING_ADDR` 覆盖）
- 分辨率/帧率默认 `640x480@30`（可用 `WIDTH/HEIGHT/FPS` 覆盖）

示例（指定信令地址）：

```bash
SIGNALING_ADDR=192.168.3.222:8765 ./scripts/push.sh livestream /dev/video11
```

### 2) 启动拉流端（客户端）

#### 本地拉流（同一台机器）

```bash
./scripts/pull.sh
# 等价于 ./scripts/pull.sh 127.0.0.1:8765
```

#### 远端拉流（另一台机器）

```bash
./scripts/pull.sh 192.168.3.222:8765
```

> 远端拉流时，参数应填写“推流机上的信令地址（IP:端口）”，不能用 `127.0.0.1`。

## 常见问题

- 推流卡在连接：确认 8765 端口可达，且双方网络互通。
- 拉流无画面：先确认推流端已开始发送，再启动拉流端。
- 跨机失败：检查防火墙/路由/DNS/代理策略。

## License

请遵循 libwebrtc 及本项目相关组件各自的许可协议。
