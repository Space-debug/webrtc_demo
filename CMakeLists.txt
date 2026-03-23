cmake_minimum_required(VERSION 3.14)
project(webrtc_push_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 平台与架构检测
if(WIN32)
    set(LIBWEBRTC_PLATFORM "win")
elseif(UNIX)
    set(LIBWEBRTC_PLATFORM "linux")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(LIBWEBRTC_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(LIBWEBRTC_ARCH "x64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# libwebrtc 路径：最外层 include（全平台共享）+ lib/<平台>/<架构>
set(LIBWEBRTC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libwebrtc)
set(LIBWEBRTC_INCLUDE_DIR ${LIBWEBRTC_ROOT}/include)
set(LIBWEBRTC_LIB_DIR ${LIBWEBRTC_ROOT}/lib/${LIBWEBRTC_PLATFORM}/${LIBWEBRTC_ARCH})

# 检查 libwebrtc 是否存在
if(NOT EXISTS ${LIBWEBRTC_INCLUDE_DIR})
    message(FATAL_ERROR "libwebrtc include not found at ${LIBWEBRTC_INCLUDE_DIR}")
endif()
if(NOT EXISTS ${LIBWEBRTC_LIB_DIR})
    message(FATAL_ERROR "libwebrtc lib not found at ${LIBWEBRTC_LIB_DIR}. "
        "Please ensure 3rdparty/libwebrtc/lib/${LIBWEBRTC_PLATFORM}/${LIBWEBRTC_ARCH} exists.")
endif()

# libjpeg-turbo（MJPEG 解码，可选）
find_package(PkgConfig QUIET)
set(HAVE_TURBOJPEG OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(TURBOJPEG QUIET libjpeg-turbo8)
    if(TURBOJPEG_FOUND)
        set(HAVE_TURBOJPEG ON)
        message(STATUS "Found libjpeg-turbo: ${TURBOJPEG_LIBRARIES}")
    else()
        message(STATUS "libjpeg-turbo not found - MJPEG capture format will not be available")
        message(STATUS "  Install: Ubuntu/Debian: libjpeg-turbo8-dev, Fedora: libjpeg-turbo-devel")
    endif()
endif()

# 主推流可执行文件
add_executable(webrtc_push_demo
    src/main.cpp
    src/camera_utils/camera_utils.cpp
    src/capture_bridge/capture_bridge.cpp
    src/config_loader/config_loader.cpp
    src/signaling_client/signaling_client.cpp
    src/push_streamer/push_streamer.cpp
)

target_include_directories(webrtc_push_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBWEBRTC_INCLUDE_DIR}
)
# 与 libwebrtc 构建时的 RTC_DESKTOP_DEVICE 保持一致，否则 vtable 错位导致 CreateVideoTrack 崩溃
target_compile_definitions(webrtc_push_demo PRIVATE RTC_DESKTOP_DEVICE)
if(HAVE_TURBOJPEG)
    target_compile_definitions(webrtc_push_demo PRIVATE HAVE_TURBOJPEG)
    target_include_directories(webrtc_push_demo PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(webrtc_push_demo PRIVATE ${TURBOJPEG_LIBRARIES})
endif()

target_link_directories(webrtc_push_demo PRIVATE ${LIBWEBRTC_LIB_DIR})
if(WIN32)
    target_link_libraries(webrtc_push_demo PRIVATE webrtc)
else()
    target_link_libraries(webrtc_push_demo PRIVATE webrtc pthread dl stdc++)
endif()

# RPATH：使用 $ORIGIN 相对路径，无需 LD_LIBRARY_PATH 即可直接运行
if(UNIX AND NOT APPLE)
    set_target_properties(webrtc_push_demo PROPERTIES
        BUILD_RPATH "\$ORIGIN/../../3rdparty/libwebrtc/lib/linux/${LIBWEBRTC_ARCH}"
        INSTALL_RPATH "\$ORIGIN/../../3rdparty/libwebrtc/lib/linux/${LIBWEBRTC_ARCH}"
    )
endif()

# 安装规则
install(TARGETS webrtc_push_demo RUNTIME DESTINATION bin)

# P2P 拉流端（纯控制台，打印帧日志）
add_subdirectory(examples/p2p_player)

# P2P 信令服务器（纯 TCP，替代 Python）
if(UNIX)
    add_executable(signaling_server src/signaling_server/signaling_server.cpp)
    target_link_libraries(signaling_server PRIVATE pthread stdc++)
    install(TARGETS signaling_server RUNTIME DESTINATION bin)
endif()

