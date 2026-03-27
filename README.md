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
├── config/
│   └── streams.conf            # 流配置（信令、分辨率、多流等）
├── docs/
├── include/
├── src/
│   ├── app/                    # 仅两个源文件：push_demo.cpp（推流） pull_demo.cpp（拉流）
│   └── sdk/                    # webrtc_push_sdk 实现
├── scripts/
│   ├── build.sh                # 编译脚本
│   ├── push.sh                 # 推流脚本（可自动拉起信令）
│   └── pull.sh                 # 拉流脚本（本地/远端通用）
└── 3rdparty/
    └── libwebrtc/
        ├── include/                  # Google WebRTC 官方头文件（api/、rtc_base/、third_party/ 等）
        ├── lib/linux/arm64/libwebrtc.a   # 官方静态库（当前仅 arm64；x64 需自备）
        └── docs_webrtc_build/        # 官方包内 build_info 等（若有）
```

**注意**：官方头文件与原先 `libwebrtc::` 封装 API 不同，业务源码需改为使用 `webrtc` 原生接口后才能编译通过。

## 依赖

- CMake >= 3.14
- C++17 编译器（GCC/Clang）
- SDL2（拉流端显示）
- libjpeg-turbo8-dev（可选，MJPEG 采集格式需要）

Linux 安装：

```bash
sudo apt install libsdl2-dev
# MJPEG 采集格式（可选）
sudo apt install libjpeg-turbo8-dev
```

## 编译

```bash
./scripts/build.sh
```

构建产物：
- `build/bin/webrtc_push_demo`（推流演示）
- `build/bin/webrtc_pull_demo`（拉流演示，需 SDL2；未安装则跳过该目标）
- `build/bin/signaling_server`

## 配置文件

直接运行可执行文件时，可用 `--config path` 指定 `config/streams.conf`。  
格式为 `KEY=value`：**全局 KEY** 作为公共默认值，`STREAM_<stream_id>_<KEY>` **同名则覆盖**（多路时公共写一块，每路只写摄像头等差异）。

```ini
SIGNALING_ADDR=127.0.0.1:8765
STREAM_ID=livestream

# 公共（分辨率、码率、编码等）
WIDTH=1280
HEIGHT=720
FPS=30
TARGET_BITRATE=2200
VIDEO_CODEC=h264

# 各路至少写 CAMERA；与公共相同的不必再写 STREAM_*_WIDTH 等
STREAM_livestream_CAMERA=/dev/video0
# STREAM_cam2_CAMERA=/dev/video13
# STREAM_cam2_TARGET_BITRATE=1500
```

说明：
- 省略命令行 `stream_id` 时用 `STREAM_ID`；摄像头来自 `STREAM_<id>_CAMERA`（无则空，需命令行或设备索引）。
- 同一物理头多档分辨率可建多个 `stream_id`，公共参数共用，各路用 `STREAM_*` 只改 `WIDTH/HEIGHT/FPS` 等差异项。
- 先用 `v4l2-ctl -d /dev/video0 --list-formats-ext` 查设备能力再填分辨率/帧率。

### 采集与像素格式（V4L2）

推流端由 **libwebrtc 直连** 摄像头设备（如 `/dev/video11`）。  
MJPEG / YUYV 等由 **库与驱动协商**，无需 v4l2loopback 采集桥接。可用 `v4l2-ctl -d /dev/video0 --list-formats-ext` 查看设备能力。

### WebRTC 自动降级（分辨率/帧率）

通过 `DEGRADATION_PREFERENCE` 控制：

| 配置值 | 网络变差时的优先策略 |
|---|---|
| `maintain_framerate` | 优先降分辨率（更保帧率） |
| `maintain_resolution` | 优先降帧率（更保分辨率） |
| `balanced` | 分辨率和帧率折中调整 |

这意味着“自动降低分辨率”和“自动降低帧率”都支持，按策略选择优先级即可。

## 运行方式

### 1) 启动推流端（服务器）

```bash
./scripts/push.sh livestream /dev/video11
```

默认行为：
- 自动启动 `signaling_server`（`START_SIGNALING=1`）
- 信令地址默认 `127.0.0.1:8765`（可用环境变量 `SIGNALING_ADDR` 覆盖）
- 分辨率/帧率默认 `640x480@30`（可用环境变量 `WIDTH/HEIGHT/FPS` 覆盖）

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

#### SSH / 无显示器：仅验证是否拉到流

```bash
# 直接运行（收到 30 帧后成功退出，退出码 0）
./build/bin/webrtc_pull_demo --headless --frames 30 --timeout-sec 120 192.168.3.222:8765 livestream

# 或用脚本
HEADLESS=1 HEADLESS_FRAMES=20 ./scripts/pull.sh 192.168.3.222:8765 livestream
```

无头拉流示例：`HEADLESS=1 ./scripts/pull.sh`，或 `webrtc_pull_demo --config config/streams.conf --headless`（`--config` 仅用于与推流一致的信令地址/流 ID，拉流专用参数用命令行或环境变量 `HEADLESS`、`HEADLESS_FRAMES` 等）。

### 3) 多摄像头推流 + 多客户端拉流

支持同一信令服务器上多个流（stream_id），每个流可有多个拉流端。

**多摄像头推流**（每个摄像头一个进程，共用一个信令）：

```bash
# 终端 1：启动信令 + 摄像头 1
./scripts/push.sh livestream /dev/video0

# 终端 2：摄像头 2（不重复启动信令）
START_SIGNALING=0 ./scripts/push.sh cam2 /dev/video11

# 终端 3：摄像头 3
START_SIGNALING=0 ./scripts/push.sh cam3 /dev/video12
```

**多客户端拉流**（同一流可被多个客户端拉取）：

```bash
# 拉取 livestream
./scripts/pull.sh 127.0.0.1:8765 livestream

# 拉取 cam2
./scripts/pull.sh 127.0.0.1:8765 cam2

# 远端拉取
./scripts/pull.sh 192.168.1.10:8765 livestream
```

## 常见问题

- **推流卡在连接**：确认 8765 端口可达，且双方网络互通。
- 拉流无画面：先确认推流端已开始发送，再启动拉流端。
- 跨机失败：检查防火墙/路由/DNS/代理策略。

## License

请遵循 libwebrtc 及本项目相关组件各自的许可协议。
