cmake_minimum_required(VERSION 3.14)
# 不使用 project()，避免子项目重置 C++ 隐式链接导致 "DSO missing from command line"
set(CMAKE_CXX_STANDARD 17)

# SDL2: 优先 Config，否则用 pkg-config（兼容旧版 Debian）
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
endif()

# 平台与架构检测（与根 CMakeLists 一致）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(LIBWEBRTC_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(LIBWEBRTC_ARCH "x64")
else()
    set(LIBWEBRTC_ARCH "arm64")
endif()
if(WIN32)
    set(LIBWEBRTC_PLATFORM "win")
else()
    set(LIBWEBRTC_PLATFORM "linux")
endif()

set(LIBWEBRTC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/libwebrtc)
set(LIBWEBRTC_INCLUDE_DIR ${LIBWEBRTC_ROOT}/include)
set(LIBWEBRTC_LIB_DIR ${LIBWEBRTC_ROOT}/lib/${LIBWEBRTC_PLATFORM}/${LIBWEBRTC_ARCH})

add_executable(p2p_player
    main.cpp
    p2p_player.cpp
    sdl_display.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/signaling_client/signaling_client.cpp
)

target_include_directories(p2p_player PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${LIBWEBRTC_INCLUDE_DIR}
)

target_compile_definitions(p2p_player PRIVATE RTC_DESKTOP_DEVICE)

target_link_directories(p2p_player PRIVATE ${LIBWEBRTC_LIB_DIR})
# 显式链接 stdc++ 解决 "DSO missing from command line"
if(TARGET SDL2::SDL2)
    target_link_libraries(p2p_player PRIVATE webrtc pthread dl stdc++ SDL2::SDL2)
else()
    target_include_directories(p2p_player PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(p2p_player PRIVATE webrtc pthread dl stdc++ ${SDL2_LIBRARIES})
endif()

# RPATH：使用 $ORIGIN 相对路径，无需 LD_LIBRARY_PATH 即可直接运行
if(UNIX AND NOT APPLE)
    set_target_properties(p2p_player PROPERTIES
        BUILD_RPATH "\$ORIGIN/../../3rdparty/libwebrtc/lib/linux/${LIBWEBRTC_ARCH}"
        INSTALL_RPATH "\$ORIGIN/../../3rdparty/libwebrtc/lib/linux/${LIBWEBRTC_ARCH}"
    )
endif()
