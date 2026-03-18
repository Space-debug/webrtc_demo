# libwebrtc Linux arm64 构建记录

## 背景

基于 Chromium M137 的 WebRTC 源码，通过 `libwebrtc` 封装层交叉编译 Linux arm64 的 shared library（`.so`），目标是生成一个纯 C++ 接口的动态库，可在 Linux ARM64 设备上直接使用。

## 环境准备

### Python 环境

系统 Python 3.8 版本过低，depot_tools 的 `ninja.py` 使用了 `list[str]` 类型注解（3.9+ 语法），需要使用 conda 的 Python 3.11：

```bash
conda activate py311
python3 --version  # Python 3.11.14
```

### 安装 ARM64 Sysroot

交叉编译需要 ARM64 的 sysroot（目标平台的系统头文件和库）。自带脚本 `install-sysroot.py` 使用 Python `urlopen` 不走系统代理，需要手动用 curl 下载：

```bash
cd ~/mirror/libwebrtc_build/src

# 查看需要下载的文件信息
python3 -c "
import json
sysroots = json.load(open('build/linux/sysroot_scripts/sysroots.json'))
d = sysroots['bullseye_arm64']
print(d['URL'] + '/' + d['Sha256Sum'])
"

# 手动下载（需要代理能访问 Google Storage）
SYSROOT_DIR="build/linux/debian_bullseye_arm64-sysroot"
mkdir -p "$SYSROOT_DIR"
curl -L -o "${SYSROOT_DIR}/debian_bullseye_arm64_sysroot.tar.xz" \
  "https://commondatastorage.googleapis.com/chrome-linux-sysroot/2f915d821eec27515c0c6d21b69898e23762908d8d7ccc1aa2a8f5f25e8b7e18"

# 验证校验和
sha256sum "${SYSROOT_DIR}/debian_bullseye_arm64_sysroot.tar.xz"
# 应输出: 2f915d821eec27515c0c6d21b69898e23762908d8d7ccc1aa2a8f5f25e8b7e18

# 解压并写入 stamp 文件
tar mxf "${SYSROOT_DIR}/debian_bullseye_arm64_sysroot.tar.xz" -C "$SYSROOT_DIR"
rm "${SYSROOT_DIR}/debian_bullseye_arm64_sysroot.tar.xz"
echo "https://commondatastorage.googleapis.com/chrome-linux-sysroot/2f915d821eec27515c0c6d21b69898e23762908d8d7ccc1aa2a8f5f25e8b7e18" > "${SYSROOT_DIR}/.stamp"
```

如果代理正常，也可以直接用脚本：

```bash
python3 build/linux/sysroot_scripts/install-sysroot.py --arch=arm64
```

## 构建命令

```bash
conda activate py311

cd ~/mirror/libwebrtc_build/src

gn gen out/Linux-arm64 --args="
  target_os=\"linux\"
  target_cpu=\"arm64\"
  is_debug=false
  rtc_include_tests=false
  rtc_use_h264=true
  ffmpeg_branding=\"Chrome\"
  is_component_build=false
  use_rtti=true
  use_custom_libcxx=false
  rtc_enable_protobuf=false
  treat_warnings_as_errors=false
  rtc_desktop_capture_supported=false
"

ninja -C out/Linux-arm64 libwebrtc
```

### 构建参数说明

| 参数 | 值 | 说明 |
|------|-----|------|
| `target_os` | `"linux"` | 目标平台 Linux |
| `target_cpu` | `"arm64"` | 目标架构 ARM64 (AArch64) |
| `is_debug` | `false` | Release 构建，启用优化，体积更小 |
| `rtc_include_tests` | `false` | 不编译测试，加快构建 |
| `rtc_use_h264` | `true` | 启用 H.264 编解码支持 |
| `ffmpeg_branding` | `"Chrome"` | 使用 Chrome 品牌的 FFmpeg（包含更多编解码器） |
| `is_component_build` | `false` | 静态链接所有依赖到单个 .so |
| `use_rtti` | `true` | 启用 C++ RTTI（运行时类型信息） |
| `use_custom_libcxx` | `false` | 使用系统 libstdc++，不用 Chromium 魔改版 libc++ |
| `rtc_enable_protobuf` | `false` | 不启用 protobuf |
| `treat_warnings_as_errors` | `false` | 警告不作为错误处理 |
| `rtc_desktop_capture_supported` | `false` | **禁用桌面/屏幕采集**，服务器端推流（USB 相机等）不需要，可减小体积、避免 vtable 与头文件不一致 |

## 遇到的问题

### 问题：Android 专属 suppressed_config 在 Linux 构建时报错

GN 生成时报错：

```
ERROR at //libwebrtc/BUILD.gn:44:5: Item not found
    "//build/config/android:hide_all_but_jni_onload",
```

原因是 `libwebrtc/BUILD.gn` 中无条件地 suppress 了 Android 专属的 config，在 Linux 构建时该 config 不存在。

**修复**：将 Android 专属的 suppressed_config 用 `if (is_android)` 包裹：

```gn
suppressed_configs += [
  "//build/config/gcc:symbol_visibility_hidden",
  "//build/config/compiler:no_unresolved_symbols",
]
if (is_android) {
  suppressed_configs += [ "//build/config/android:hide_all_but_jni_onload" ]
}
configs += [ "//build/config/gcc:symbol_visibility_default" ]
```

## 构建结果

| 指标 | 值 |
|------|-----|
| 编译目标数 | 4162 |
| 编译耗时 | ~3 分钟 |
| .so 大小 | 19 MB（not stripped） |
| 架构 | ELF 64-bit, ARM aarch64 |
| 导出符号 (T) | 530 |
| 总导出符号 | 1879 |

### 导出符号覆盖情况

所有公开 API 头文件中声明的类和方法均有对应导出符号（`rtc_desktop_capture_supported=false` 时无 DesktopCapturer 相关）：

| 模块 | 符号数 | 关键 API |
|------|--------|----------|
| LibWebRTC（核心入口） | 3 | Initialize / CreateRTCPeerConnectionFactory / Terminate |
| RTCPeerConnectionFactory | 35 | Create / CreateAudioTrack / CreateVideoTrack / CreateStream |
| RTCPeerConnection | 65 | CreateOffer / CreateAnswer / AddTransceiver / AddCandidate |
| RTCDataChannel | 18 | Send / Close / RegisterObserver |
| RTCMediaStream | 19 | AddTrack / RemoveTrack / FindAudioTrack |
| AudioDevice | 19 | PlayoutDevices / RecordingDevices / SetVolume |
| AudioTrack / VideoTrack | 25 | AddSink / AddRenderer / SetVolume |
| RTCRtpSender / Receiver / Transceiver | 51 | set_parameters / SetCodecPreferences |
| FrameCryptor | 44 | frameCryptorFromRtpSender / SetEnabled |
| DesktopCapturer / Device / MediaList | 41 | 仅 `rtc_desktop_capture_supported=true` 时存在 |
| DtlsTransport / DtmfSender | 32 | GetInformation / InsertDtmf |
| Logging / KeyProvider / 其他 | 7+ | setLogSink / Create |

## 运行时依赖

`libwebrtc.so` 已将 BoringSSL、FFmpeg、libyuv、abseil、libaom 等全部静态链接，运行时只需要以下系统标准库：

| 分类 | 依赖库 |
|------|--------|
| C/C++ 基础 | `libc.so.6`、`libstdc++.so.6`、`libgcc_s.so.1`、`libm.so.6` |
| 线程/运行时 | `libpthread.so.0`、`libdl.so.2`、`librt.so.1`、`libatomic.so.1` |
| GLib/GIO | `libglib-2.0.so.0`、`libgobject-2.0.so.0`、`libgio-2.0.so.0` |
| X11 显示 | `libX11.so.6`、`libXcomposite.so.1`、`libXdamage.so.1`、`libXext.so.6`、`libXfixes.so.3`、`libXrandr.so.2`、`libXrender.so.1`、`libXtst.so.6` |
| DRM/GBM | `libdrm.so.2`、`libgbm.so.1` |
| 其他 | `libz.so.1`、`ld-linux-aarch64.so.1` |

如果目标机器是无桌面的嵌入式/服务端环境，可能需要安装：

```bash
sudo apt install libglib2.0-0 libx11-6 libxcomposite1 libxdamage1 libxext6 \
  libxfixes3 libxrandr2 libxrender1 libxtst6 libgbm1 libdrm2 libatomic1
```

## 打包产物

最终只需要两样东西：

```
libwebrtc_linux_arm64/
├── include/          # C++ API 头文件
│   ├── base/         # 基础类型（引用计数、智能指针等）
│   ├── libwebrtc.h   # 主入口
│   └── rtc_*.h       # 各模块接口
└── lib/
    └── libwebrtc.so  # 19MB（not stripped）
```

### 打包命令

```bash
cd ~/mirror/libwebrtc_build/src

DIST_DIR="libwebrtc_linux_arm64"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/include" "$DIST_DIR/lib"

# 拷贝头文件
cp -r libwebrtc/include/* "$DIST_DIR/include/"

# 拷贝动态库
cp out/Linux-arm64/libwebrtc.so "$DIST_DIR/lib/"

# 打包
tar czf libwebrtc_linux_arm64.tar.gz "$DIST_DIR"
```

## 关于 `use_custom_libcxx=false`

与 Android 构建类似，设置了 `use_custom_libcxx=false`：

- `libwebrtc.so` 链接的是系统标准的 `libstdc++.so.6`
- 目标机器上需要有 `libstdc++.so.6`（一般都有）
- 避免与应用程序自身的 C++ 标准库产生符号冲突

## 关于 `rtc_desktop_capture_supported=false`

- **桌面采集**：用于屏幕共享（共享整个桌面或窗口），服务器端 USB 相机推流不需要
- **禁用后**：不编译 DesktopCapturer/DesktopDevice 等，库体积更小，且应用编译时**无需**定义 `RTC_DESKTOP_DEVICE`
- **若需屏幕共享**：改为 `rtc_desktop_capture_supported=true`（默认），应用侧需定义 `RTC_DESKTOP_DEVICE` 以匹配 vtable

**配套修改**：若 libwebrtc 使用 `rtc_desktop_capture_supported=false` 编译，webrtc_demo 的 CMakeLists.txt 中需**移除** `target_compile_definitions(... RTC_DESKTOP_DEVICE)`，否则 vtable 会错位。

## 备注

- 构建主机为 x86_64 Linux，通过 Chromium 自带的 clang 工具链 + ARM64 sysroot 实现交叉编译
- sysroot 基于 Debian Bullseye，编译出的 .so 兼容 Debian 11 及以上的 ARM64 系统
- 如需 debug 版本，将 `is_debug=false` 改为 `is_debug=true`，输出会大很多（含完整调试符号）
- 当前是 `symbol_visibility_default`，会导出所有符号。如需精确控制，可自定义 version script
