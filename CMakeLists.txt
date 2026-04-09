cmake_minimum_required(VERSION 3.14)
project(webrtc_push_demo LANGUAGES C CXX)

# gold 链预编译 libwebrtc.a 时 .init_array 易出现 CREL 空槽 → 进 main 前 SIGSEGV；bfd 对其中 .eh_frame 报错。
# 优先使用 LLVM lld（若已安装：apt install lld），否则回退 gold。
if(UNIX AND NOT APPLE)
    find_program(WEBRTC_LD_LLD ld.lld)
    if(WEBRTC_LD_LLD)
        # 预编译 libwebrtc 含 CREL：链接期把动态重定位应用到输出，避免 .init_array 全零导致启动崩溃
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld -Wl,--apply-dynamic-relocs -no-pie")
    else()
        find_program(WEBRTC_LD_GOLD ld.gold)
        if(WEBRTC_LD_GOLD)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold")
        endif()
    endif()
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcc")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(${CMAKE_SOURCE_DIR}/cmake/LibWebRTC.cmake)
message(STATUS "3rdparty/libwebrtc: 官方头文件 + libwebrtc.a；集成方链接 webrtc_push_sdk 即可获得推/拉流与信令等能力。")

set(WEBRTC_PUSH_SDK_DIR ${CMAKE_SOURCE_DIR}/src/sdk)

option(WEBRTC_DEMO_ENABLE_ROCKCHIP_MPP
    "Use Rockchip MPP for H.264 hardware encode when librockchip_mpp is present (e.g. RK3588 BSP)"
    ON)
option(WEBRTC_SDK_INSTALL_ADVANCED_HEADERS
    "Install advanced headers (signaling/hw backend selection interfaces)"
    ON)
option(WEBRTC_SDK_INSTALL_INTERNAL_HEADERS
    "Install internal headers (utils/platform-specific internals)"
    OFF)

set(WEBRTC_SDK_SOURCES
    ${CMAKE_SOURCE_DIR}/src/utils/json_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/net_io.cpp
    ${WEBRTC_PUSH_SDK_DIR}/camera/camera_utils.cpp
    ${WEBRTC_PUSH_SDK_DIR}/config/config_loader.cpp
    ${WEBRTC_PUSH_SDK_DIR}/signaling/signaling_client.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/push_streamer.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/camera_video_track_source.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/pull_subscriber.cpp
    ${WEBRTC_PUSH_SDK_DIR}/webrtc/peer_connection_factory.cpp
    ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/backend_registry.cpp
    ${WEBRTC_PUSH_SDK_DIR}/webrtc/sequence_checker_expectation_to_string.cpp
)

if(WEBRTC_DEMO_ENABLE_ROCKCHIP_MPP AND UNIX AND NOT APPLE)
    # 勿事先 set 同名变量，否则会遮蔽 find_* 写入的 CACHE，导致永远判定为未找到。
    find_path(WEBRTC_DEMO_ROCKCHIP_MPP_INCLUDE rk_mpi.h
        PATHS /usr/include/rockchip
        DOC "Rockchip MPP headers (rk_mpi.h)")
    find_library(WEBRTC_DEMO_ROCKCHIP_MPP_LIB rockchip_mpp
        PATHS /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib)
    if(WEBRTC_DEMO_ROCKCHIP_MPP_INCLUDE AND WEBRTC_DEMO_ROCKCHIP_MPP_LIB)
        list(APPEND WEBRTC_SDK_SOURCES
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/native_dec_frame_buffer.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/h264_encoder.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/mjpeg_decoder.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/h264_decoder.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/video_decoder_factory.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/hw/rockchip_mpp/video_encoder_factory.cpp
            )
        message(STATUS "Rockchip MPP: enabled for hardware H264 (${WEBRTC_DEMO_ROCKCHIP_MPP_LIB})")
    else()
        message(STATUS "Rockchip MPP: not found (optional); H264 stays software-only on this build host")
    endif()
endif()

add_library(webrtc_push_sdk STATIC ${WEBRTC_SDK_SOURCES})

target_include_directories(webrtc_push_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${WEBRTC_PUSH_SDK_DIR}
        ${LIBWEBRTC_INCLUDE_DIR}
        ${LIBWEBRTC_ABSL_INCLUDE_DIR}
        ${LIBWEBRTC_LIBYUV_INCLUDE_DIR}
)

if(WEBRTC_DEMO_ROCKCHIP_MPP_INCLUDE)
    target_include_directories(webrtc_push_sdk PRIVATE ${WEBRTC_DEMO_ROCKCHIP_MPP_INCLUDE})
    target_compile_definitions(webrtc_push_sdk PRIVATE WEBRTC_DEMO_HAVE_ROCKCHIP_MPP=1)
    target_link_libraries(webrtc_push_sdk PUBLIC ${WEBRTC_DEMO_ROCKCHIP_MPP_LIB})
    find_library(WEBRTC_DEMO_LIBRGA rga
        PATHS /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf /usr/lib)
    if(WEBRTC_DEMO_LIBRGA)
        target_compile_definitions(webrtc_push_sdk PRIVATE WEBRTC_DEMO_HAVE_LIBRGA=1)
        target_link_libraries(webrtc_push_sdk PUBLIC ${WEBRTC_DEMO_LIBRGA})
        target_include_directories(webrtc_push_sdk PRIVATE /usr/include)
        message(STATUS "Rockchip RGA (librga): linked for MJPEG dma-buf copy (${WEBRTC_DEMO_LIBRGA})")
    else()
        message(STATUS "Rockchip RGA: librga not found (optional; WEBRTC_MJPEG_RGA_TO_MPP unavailable)")
    endif()
endif()

webrtc_push_link_libwebrtc_static_library(webrtc_push_sdk)

# libwebrtc.a 中 rotate_sme.o 需要 __arm_tpidr2_save；须在本库之后链接存根，否则 .a 内对象不会提前被拉取。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_library(sme_abi_stubs STATIC ${WEBRTC_PUSH_SDK_DIR}/webrtc/sme_abi_stubs.c)
    target_link_libraries(webrtc_push_sdk PRIVATE sme_abi_stubs)
    install(TARGETS sme_abi_stubs
        EXPORT WebrtcPushSdkTargets
        ARCHIVE DESTINATION lib)
endif()

add_executable(webrtc_push_demo src/app/push_demo.cpp)
target_include_directories(webrtc_push_demo PRIVATE ${WEBRTC_PUSH_SDK_DIR})
target_link_libraries(webrtc_push_demo PRIVATE webrtc_push_sdk)

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
endif()
if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found: webrtc_pull_demo will not be built (install libsdl2-dev).")
else()
    add_executable(webrtc_pull_demo src/app/pull_demo.cpp)
    target_include_directories(webrtc_pull_demo PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${LIBWEBRTC_INCLUDE_DIR}
        ${LIBWEBRTC_ABSL_INCLUDE_DIR}
        ${LIBWEBRTC_LIBYUV_INCLUDE_DIR}
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(webrtc_pull_demo PRIVATE webrtc_push_sdk SDL2::SDL2)
    else()
        target_include_directories(webrtc_pull_demo PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(webrtc_pull_demo PRIVATE webrtc_push_sdk ${SDL2_LIBRARIES})
    endif()
    if(UNIX AND NOT APPLE)
        target_compile_definitions(webrtc_pull_demo PRIVATE WEBRTC_POSIX WEBRTC_LINUX)
        target_compile_options(webrtc_pull_demo PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
    endif()
endif()

set(WEBRTC_SDK_PUBLIC_HEADERS
    ${CMAKE_SOURCE_DIR}/include/config_loader.h
    ${CMAKE_SOURCE_DIR}/include/pull_subscriber.h
    ${CMAKE_SOURCE_DIR}/include/push_streamer.h
    ${CMAKE_SOURCE_DIR}/include/webrtc_peer_connection_factory.h)
install(FILES ${WEBRTC_SDK_PUBLIC_HEADERS} DESTINATION include)
install(FILES ${CMAKE_SOURCE_DIR}/include/webrtc_demo/sdk.h DESTINATION include/webrtc_demo)

if(WEBRTC_SDK_INSTALL_ADVANCED_HEADERS)
    install(FILES
        ${CMAKE_SOURCE_DIR}/include/webrtc/signaling/client.h
        DESTINATION include/webrtc/signaling)
    install(FILES
        ${CMAKE_SOURCE_DIR}/include/webrtc/hw/backend_registry.h
        DESTINATION include/webrtc/hw)
endif()

if(WEBRTC_SDK_INSTALL_INTERNAL_HEADERS)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/webrtc/utils/ DESTINATION include/webrtc/utils
        FILES_MATCHING PATTERN "*.h")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/webrtc/hw/rockchip_mpp/ DESTINATION include/webrtc/hw/rockchip_mpp
        FILES_MATCHING PATTERN "*.h")
endif()

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/WebrtcPushSdkConfigVersion.cmake"
    VERSION 1.0.0
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/WebrtcPushSdkConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/WebrtcPushSdkConfig.cmake"
    INSTALL_DESTINATION lib/cmake/WebrtcPushSdk
    NO_CHECK_REQUIRED_COMPONENTS_MACRO)

install(TARGETS webrtc_push_sdk
    EXPORT WebrtcPushSdkTargets
    ARCHIVE DESTINATION lib)
install(EXPORT WebrtcPushSdkTargets
    FILE WebrtcPushSdkTargets.cmake
    NAMESPACE webrtc_demo::
    DESTINATION lib/cmake/WebrtcPushSdk)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/WebrtcPushSdkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/WebrtcPushSdkConfigVersion.cmake"
    DESTINATION lib/cmake/WebrtcPushSdk)

install(TARGETS webrtc_push_demo RUNTIME DESTINATION bin)
if(TARGET webrtc_pull_demo)
    install(TARGETS webrtc_pull_demo RUNTIME DESTINATION bin)
endif()

if(UNIX)
    add_executable(signaling_server
        src/app/signaling_server.cpp
        src/utils/json_utils.cpp
        src/utils/net_io.cpp
        src/sdk/signaling/signaling_server_main.cpp)
    target_include_directories(signaling_server PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${WEBRTC_PUSH_SDK_DIR})
    target_link_libraries(signaling_server PRIVATE pthread stdc++)
    install(TARGETS signaling_server RUNTIME DESTINATION bin)
endif()

