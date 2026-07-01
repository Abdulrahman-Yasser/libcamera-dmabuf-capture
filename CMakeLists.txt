cmake_minimum_required(VERSION 3.16)
project(libcamera-dmabuf-capture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When cross-compiling with the Yocto SDK, CMake probes the cross-compiler and
# picks up the host system's /usr/include/c++/11 (x86_64 headers). Those then
# mix with the aarch64 sysroot headers and break compilation.
# Strip only the host /usr/include* entries; keep the SDK-internal GCC dirs
# (e.g. .../13.4.0/include) that contain the stdlib.h wrapper (#include_next).
if(CMAKE_CROSSCOMPILING)
    list(FILTER CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES EXCLUDE REGEX "^/usr/include")
    list(FILTER CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES   EXCLUDE REGEX "^/usr/include")
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBCAMERA  REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(DRM        REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM        REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(EGL        REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2     REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(PNG        REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(GST        REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GST_APP    REQUIRED IMPORTED_TARGET gstreamer-app-1.0)
pkg_check_modules(GST_ALLOC  REQUIRED IMPORTED_TARGET gstreamer-allocators-1.0)
pkg_check_modules(GST_VIDEO  REQUIRED IMPORTED_TARGET gstreamer-video-1.0)

add_executable(libcamera-dmabuf-capture
    main.cpp
    egl_context.cpp
    capture_session.cpp
    gpu_renderer.cpp
    dmabuf_import.cpp
    file_source.cpp
)

target_link_libraries(libcamera-dmabuf-capture PRIVATE
    PkgConfig::LIBCAMERA
    PkgConfig::DRM
    PkgConfig::GBM
    PkgConfig::EGL
    PkgConfig::GLESV2
    PkgConfig::PNG
    PkgConfig::GST
    PkgConfig::GST_APP
    PkgConfig::GST_ALLOC
    PkgConfig::GST_VIDEO
)

install(TARGETS libcamera-dmabuf-capture DESTINATION bin)
