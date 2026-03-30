# WebRTC 第三方库路径与链接约定（SDK、示例、集成方共用）
# 须由工程根目录 CMakeLists.txt include 本文件（CMAKE_CURRENT_LIST_DIR 指向 cmake/）

get_filename_component(WEBRTC_PUSH_DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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

set(LIBWEBRTC_ROOT "${WEBRTC_PUSH_DEMO_ROOT}/3rdparty/libwebrtc")
set(LIBWEBRTC_INCLUDE_DIR "${LIBWEBRTC_ROOT}/include")
set(LIBWEBRTC_LIB_DIR "${LIBWEBRTC_ROOT}/lib/${LIBWEBRTC_PLATFORM}/${LIBWEBRTC_ARCH}")
set(LIBWEBRTC_STATIC_LIB "${LIBWEBRTC_LIB_DIR}/libwebrtc.a")
set(LIBWEBRTC_ABSL_INCLUDE_DIR "${LIBWEBRTC_INCLUDE_DIR}/third_party/abseil-cpp")
set(LIBWEBRTC_LIBYUV_INCLUDE_DIR "${LIBWEBRTC_INCLUDE_DIR}/third_party/libyuv/include")

if(NOT EXISTS "${LIBWEBRTC_INCLUDE_DIR}/api")
    message(FATAL_ERROR "WebRTC headers not found under ${LIBWEBRTC_INCLUDE_DIR} (expect api/ etc.)")
endif()
if(NOT EXISTS "${LIBWEBRTC_LIB_DIR}")
    message(FATAL_ERROR "libwebrtc arch dir not found: ${LIBWEBRTC_LIB_DIR}")
endif()
if(NOT EXISTS "${LIBWEBRTC_STATIC_LIB}")
    message(FATAL_ERROR "libwebrtc.a not found at ${LIBWEBRTC_STATIC_LIB}. "
        "Official bundle is Linux arm64 only; add libwebrtc.a for ${LIBWEBRTC_ARCH} or build on aarch64.")
endif()

# 新版 libwebrtc 静态库含 Linux camera portal / xdg_desktop_portal，需 GIO(GDBus)/GLib
set(LIBWEBRTC_LINUX_GIO_TARGET "")
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GIO_PC IMPORTED_TARGET gio-2.0)
        if(TARGET PkgConfig::GIO_PC)
            set(LIBWEBRTC_LINUX_GIO_TARGET PkgConfig::GIO_PC)
        endif()
    endif()
    if(NOT LIBWEBRTC_LINUX_GIO_TARGET)
        message(WARNING "pkg-config gio-2.0 not found; linking -lgio-2.0 -lgobject-2.0 -lglib-2.0 (install libglib2.0-dev)")
        add_library(libwebrtc_linux_gio INTERFACE IMPORTED)
        target_link_libraries(libwebrtc_linux_gio INTERFACE gio-2.0 gobject-2.0 glib-2.0)
        set(LIBWEBRTC_LINUX_GIO_TARGET libwebrtc_linux_gio)
    endif()
endif()

# 供依赖 libwebrtc 的可执行文件 / 其他目标使用（含头文件与编译选项）
function(webrtc_push_target_use_libwebrtc_headers target)
    target_include_directories(${target} PRIVATE
        "${LIBWEBRTC_INCLUDE_DIR}"
        "${LIBWEBRTC_ABSL_INCLUDE_DIR}"
    )
endfunction()

function(webrtc_push_target_use_libwebrtc target)
    webrtc_push_target_use_libwebrtc_headers(${target})
    # start-group：libwebrtc.a 内部对象循环依赖时避免漏链；与 bfd 配合更稳
    target_link_libraries(${target} PRIVATE
        -Wl,--start-group "${LIBWEBRTC_STATIC_LIB}" -Wl,--end-group pthread dl stdc++)
    if(UNIX AND NOT APPLE AND LIBWEBRTC_LINUX_GIO_TARGET)
        target_link_libraries(${target} PRIVATE "${LIBWEBRTC_LINUX_GIO_TARGET}")
    endif()
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target} PRIVATE WEBRTC_POSIX WEBRTC_LINUX)
        # 仅 C++：避免 sme_abi_stubs.c 等 .c 源报「-fpermissive 不适用于 C」
        # -Wno-deprecated-declarations：压制第三方头里 SignalEvent 等弃用告警（非业务代码）
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
    endif()
endfunction()

# 静态库目标：将 libwebrtc 与编译选项作为 PUBLIC 传给链接该库的最终可执行文件 / 动态库
function(webrtc_push_link_libwebrtc_static_library target)
    target_link_libraries(${target} PUBLIC
        -Wl,--start-group "${LIBWEBRTC_STATIC_LIB}" -Wl,--end-group pthread dl stdc++)
    if(UNIX AND NOT APPLE AND LIBWEBRTC_LINUX_GIO_TARGET)
        target_link_libraries(${target} PUBLIC "${LIBWEBRTC_LINUX_GIO_TARGET}")
    endif()
    if(UNIX AND NOT APPLE)
        target_compile_definitions(${target} PRIVATE WEBRTC_POSIX WEBRTC_LINUX)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
    endif()
endfunction()
