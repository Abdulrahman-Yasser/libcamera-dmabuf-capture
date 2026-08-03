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

# ── Live Wayland preview window (--preview flag) ─────────────────────────────
# Opt-in: the headless GBM/surfaceless pipeline (default) never depends on a
# compositor being present. This mirrors wayland-cxx-scanner's own examples,
# which self-skip when their dependencies are missing rather than failing the
# whole build.
option(ENABLE_WAYLAND_PREVIEW "Build the live Wayland preview window (--preview)" ON)

set(HAVE_WAYLAND_PREVIEW FALSE)
if (ENABLE_WAYLAND_PREVIEW)
    pkg_check_modules(WAYLAND_CLIENT    IMPORTED_TARGET wayland-client)
    pkg_check_modules(WAYLAND_EGL       IMPORTED_TARGET wayland-egl)
    pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
    pkg_check_modules(WAYLAND_SCANNER   wayland-scanner)

    if (WAYLAND_CLIENT_FOUND AND WAYLAND_EGL_FOUND AND
        WAYLAND_PROTOCOLS_FOUND AND WAYLAND_SCANNER_FOUND)
        if (CMAKE_CROSSCOMPILING AND NOT WAYLAND_CXX_SCANNER_EXECUTABLE)
            message(WARNING
                "ENABLE_WAYLAND_PREVIEW is ON but this is a cross build and "
                "WAYLAND_CXX_SCANNER_EXECUTABLE is unset. wayland-cxx-scanner's "
                "codegen tool must run on the HOST, not the target — build it "
                "natively once (see README.md) and pass its path via "
                "-DWAYLAND_CXX_SCANNER_EXECUTABLE=<path>. Disabling the "
                "preview window for this build.")
        else ()
            set(HAVE_WAYLAND_PREVIEW TRUE)
        endif ()
    else ()
        message(WARNING
            "ENABLE_WAYLAND_PREVIEW is ON but wayland-client/wayland-egl/"
            "wayland-protocols/wayland-scanner were not all found — building "
            "without the --preview window.")
    endif ()
endif ()

if (HAVE_WAYLAND_PREVIEW)
    set(WAYLAND_CXX_SCANNER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WAYLAND_CXX_SCANNER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    add_subdirectory(extern/wayland-cxx-scanner)

    pkg_get_variable(WAYLAND_SCANNER_PKGDATADIR   wayland-scanner   pkgdatadir)
    pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)

    set(_wl_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/wl-generated")
endif ()

add_executable(libcamera-dmabuf-capture
    main.cpp
    egl_context.cpp
    capture_session.cpp
    gpu_renderer.cpp
    dmabuf_import.cpp
    file_source.cpp
    ipm.cpp
    bev_config.cpp
)

if (HAVE_WAYLAND_PREVIEW)
    target_sources(libcamera-dmabuf-capture PRIVATE wayland_window.cpp)
    target_compile_definitions(libcamera-dmabuf-capture PRIVATE HAVE_WAYLAND_PREVIEW)
    target_link_libraries(libcamera-dmabuf-capture PRIVATE
        wayland-cxx::wayland-cxx
        PkgConfig::WAYLAND_CLIENT
        PkgConfig::WAYLAND_EGL
    )

    # Protocol headers generated straight into the target — no per-file
    # #include subdir prefix needed, matching wayland-cxx-scanner's own
    # examples/CMakeLists.txt usage.
    wayland_cxx_generate(
        PROTOCOL "${WAYLAND_SCANNER_PKGDATADIR}/wayland.xml"
        MODE client-header
        OUTPUT "wl-generated/wayland_client.hpp"
        TARGET libcamera-dmabuf-capture)
    wayland_cxx_generate(
        PROTOCOL "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml"
        MODE client-header
        OUTPUT "wl-generated/xdg_shell_client.hpp"
        TARGET libcamera-dmabuf-capture)
    # Neither agl_shell nor presentation-time has a pre-built wl_interface
    # symbol (they aren't part of upstream wayland-protocols), so both need
    # their own interface tables emitted.
    wayland_cxx_generate(
        PROTOCOL "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/presentation-time/presentation-time.xml"
        MODE client-header
        EMIT_INTERFACE_TABLES
        OUTPUT "wl-generated/presentation_time_client.hpp"
        TARGET libcamera-dmabuf-capture)
    wayland_cxx_generate(
        PROTOCOL "${CMAKE_CURRENT_SOURCE_DIR}/extern/wayland-cxx-scanner/protocols/agl-shell.xml"
        MODE client-header
        EMIT_INTERFACE_TABLES
        OUTPUT "wl-generated/agl_shell_client.hpp"
        TARGET libcamera-dmabuf-capture)
endif ()

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
