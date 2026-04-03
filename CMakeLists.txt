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

set(WEBRTC_SDK_SOURCES
    ${WEBRTC_PUSH_SDK_DIR}/camera/camera_utils.cpp
    ${WEBRTC_PUSH_SDK_DIR}/config/config_loader.cpp
    ${WEBRTC_PUSH_SDK_DIR}/signaling/signaling_client.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/push_streamer.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/camera_video_track_source.cpp
    ${WEBRTC_PUSH_SDK_DIR}/media/pull_subscriber.cpp
    ${WEBRTC_PUSH_SDK_DIR}/webrtc/peer_connection_factory.cpp
    ${WEBRTC_PUSH_SDK_DIR}/webrtc/compat/sequence_checker_expectation_to_string.cpp
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
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/mpp_native_dec_frame_buffer.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/rk_mpp_h264_encoder.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/rk_mpp_mjpeg_decoder.cpp
            ${WEBRTC_PUSH_SDK_DIR}/webrtc/rk_mpp_video_encoder_factory.cpp)
        message(STATUS "Rockchip MPP: enabled for hardware H264 (${WEBRTC_DEMO_ROCKCHIP_MPP_LIB})")
    else()
        message(STATUS "Rockchip MPP: not found (optional); H264 stays software-only on this build host")
    endif()
endif()

add_library(webrtc_push_sdk STATIC ${WEBRTC_SDK_SOURCES})

target_include_directories(webrtc_push_sdk
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
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
    target_link_libraries(webrtc_push_sdk PUBLIC sme_abi_stubs)
endif()

add_executable(webrtc_push_demo src/app/push_demo.cpp)
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

install(TARGETS webrtc_push_sdk ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION include
    FILES_MATCHING PATTERN "*.h")

install(TARGETS webrtc_push_demo RUNTIME DESTINATION bin)
if(TARGET webrtc_pull_demo)
    install(TARGETS webrtc_pull_demo RUNTIME DESTINATION bin)
endif()

if(UNIX)
    add_executable(signaling_server src/tools/signaling_server.cpp)
    target_link_libraries(signaling_server PRIVATE pthread stdc++)
    install(TARGETS signaling_server RUNTIME DESTINATION bin)
endif()

