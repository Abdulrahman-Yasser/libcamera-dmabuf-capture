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

# ── Pipeline core ────────────────────────────────────────────────────────────
# Everything the CLI and the ivi-homescreen platform view (libbev_view.so,
# below) share. Position-independent because the platform view is a shared
# object; hidden so that library exports only its bev_* C ABI.
# egl_context.cpp is deliberately not in here: its HAVE_WAYLAND_PREVIEW half
# belongs to the CLI only, so each target compiles it with its own defines.
add_library(bev_core STATIC
    capture_session.cpp
    gpu_renderer.cpp
    file_source.cpp
    ipm.cpp
    bev_config.cpp
    bev_setup.cpp
)
set_target_properties(bev_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(bev_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Header-only (github.com/nothings/stb, vendored as a git submodule) -- used
# for the car icon's image load (gpu_renderer.cpp's read_png_rgba()); no
# library to link, just needs its directory on the include path.
target_include_directories(bev_core PRIVATE extern/stb)

target_link_libraries(bev_core PUBLIC
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

add_executable(libcamera-dmabuf-capture
    main.cpp
    egl_context.cpp
    dmabuf_import.cpp
)
target_link_libraries(libcamera-dmabuf-capture PRIVATE bev_core)

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

install(TARGETS libcamera-dmabuf-capture DESTINATION bin)

# ── ivi-homescreen platform view (libbev_view.so) ────────────────────────────
# Publishes the pipeline as the "views/bev-view" ihs_pv platform view: it
# renders offscreen, hands the shell one dma-buf per frame, and the shell owns
# compositing. Built by flutter/bev_view's native-assets hook, which turns this
# on; OFF by default so the CLI build never needs an ivi-homescreen tree.
option(ENABLE_IHS_PV "Build libbev_view.so, the ivi-homescreen platform view" OFF)

# The producer always renders with GLES. What differs per shell backend is who
# allocates the buffers it renders into: on a Vulkan backend (wayland-vulkan /
# drm-kms-vulkan) they are exportable images on the shell's own device, so the
# compositor is guaranteed to import them; on an EGL backend they come from gbm.
# Only the Vulkan headers are needed -- every entry point is resolved through
# the shell's interposed loader at run time.
option(ENABLE_IHS_PV_VULKAN "Allocate platform-view buffers on the shell's Vulkan device" ON)

if (ENABLE_IHS_PV)
    # -- ihs_pv: libihs_shared and the ihs/ ABI headers -----------------------
    # Either an ivi-homescreen checkout (IHS_DIR, with its build under
    # IHS_BUILD_DIR) or an installed ivi-homescreen-shared package.
    if (NOT IHS_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../ivi-homescreen/shared/include/ihs/platform_view.h")
        get_filename_component(IHS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ivi-homescreen" ABSOLUTE)
    endif ()

    # Under a cross build the shell's build tree is emb's, not the checkout's:
    # <workspace>/cross-build-<triple>-<key>/build-<backend>/. The key moves
    # with the target configuration, so it cannot be written down -- derive the
    # workspace from CMAKE_SYSROOT (<workspace>/cross-<triple>-<key>/sysroot)
    # and take the most recently built shell for this triple. A host build's
    # x86_64 libihs_shared must never be picked up here, so the checkout glob
    # below stays off the cross path entirely.
    if (NOT IHS_BUILD_DIR AND CMAKE_CROSSCOMPILING AND CMAKE_SYSROOT)
        get_filename_component(_ihs_profile "${CMAKE_SYSROOT}" DIRECTORY)
        get_filename_component(_ihs_workspace "${_ihs_profile}" DIRECTORY)
        get_filename_component(_ihs_profile_name "${_ihs_profile}" NAME)
        string(REGEX REPLACE "^cross-(.+)-[0-9a-f]+$" "\\1" _ihs_triple "${_ihs_profile_name}")
        # The toolchain file is <triple>-<build key>-toolchain.cmake, and the
        # shell built under that same key is the one this configuration belongs
        # to. Prefer it: "newest" alone picked another board's build whenever
        # that one had been built more recently.
        set(_ihs_key "")
        if (CMAKE_TOOLCHAIN_FILE)
            get_filename_component(_ihs_tc_name "${CMAKE_TOOLCHAIN_FILE}" NAME)
            string(REGEX MATCH "-([0-9a-f]+)-toolchain\\.cmake$" _ihs_tc_match "${_ihs_tc_name}")
            set(_ihs_key "${CMAKE_MATCH_1}")
        endif ()
        # Newer shells put the library in shell/lib/, older ones in shared/;
        # the generated ihs_export.h is under shared/include either way.
        # Keyed builds first; any build for the triple only if none qualifies.
        set(_ihs_patterns "")
        if (_ihs_key)
            list(APPEND _ihs_patterns "${_ihs_workspace}/cross-build-${_ihs_triple}-${_ihs_key}/build-*")
        endif ()
        list(APPEND _ihs_patterns "${_ihs_workspace}/cross-build-${_ihs_triple}-*/build-*")
        foreach (_pattern ${_ihs_patterns})
            file(GLOB _ihs_cross_builds LIST_DIRECTORIES true "${_pattern}")
            foreach (_cand ${_ihs_cross_builds})
                set(_cand_lib "")
                foreach (_sub shell/lib shared)
                    if (EXISTS "${_cand}/${_sub}/libihs_shared.so")
                        set(_cand_lib "${_cand}/${_sub}/libihs_shared.so")
                        break ()
                    endif ()
                endforeach ()
                if (_cand_lib AND EXISTS "${_cand}/shared/include/ihs/ihs_export.h")
                    if (NOT IHS_BUILD_DIR OR "${_cand_lib}" IS_NEWER_THAN "${_ihs_best_lib}")
                        set(IHS_BUILD_DIR "${_cand}")
                        set(_ihs_best_lib "${_cand_lib}")
                    endif ()
                endif ()
            endforeach ()
            if (IHS_BUILD_DIR)
                break ()
            endif ()
        endforeach ()
        if (IHS_BUILD_DIR)
            message(STATUS "ihs build (emb cross)   = ${IHS_BUILD_DIR}")
        endif ()
    endif ()

    add_library(ihs_pv INTERFACE)
    if (IHS_DIR)
        if (NOT IHS_BUILD_DIR AND NOT CMAKE_CROSSCOMPILING)
            # Prefer whichever build directory actually has libihs_shared in it,
            # rather than pinning one name.
            file(GLOB _ihs_builds LIST_DIRECTORIES true
                "${IHS_DIR}/build-pv" "${IHS_DIR}/cmake-build-*" "${IHS_DIR}/build*")
            foreach (_cand ${_ihs_builds})
                if (EXISTS "${_cand}/shared/include/ihs/ihs_export.h")
                    set(IHS_BUILD_DIR "${_cand}")
                    break ()
                endif ()
            endforeach ()
        endif ()
        if (NOT IHS_BUILD_DIR)
            message(FATAL_ERROR
                "ENABLE_IHS_PV: no build of ihs_shared for this target; build the "
                "shell first (emb cross <ivi-homescreen> --target <t> --build) or "
                "set -DIHS_BUILD_DIR=<ivi-homescreen build dir>")
        endif ()
        # The library sits outside the sysroot, which a cross toolchain's
        # CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY would otherwise exclude.
        if (EXISTS "${IHS_BUILD_DIR}/shell/lib/libihs_shared.so")
            set(IHS_SHARED_LIB "${IHS_BUILD_DIR}/shell/lib/libihs_shared.so")
        elseif (EXISTS "${IHS_BUILD_DIR}/shared/libihs_shared.so")
            set(IHS_SHARED_LIB "${IHS_BUILD_DIR}/shared/libihs_shared.so")
        else ()
            find_library(IHS_SHARED_LIB ihs_shared
                PATHS "${IHS_BUILD_DIR}/shell/lib" "${IHS_BUILD_DIR}/shared" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        endif ()
        target_link_libraries(ihs_pv INTERFACE ${IHS_SHARED_LIB})
        # shared/include has ihs/platform_view.h; the build tree has the
        # generated ihs/ihs_export.h.
        target_include_directories(ihs_pv INTERFACE
            "${IHS_DIR}/shared/include"
            "${IHS_BUILD_DIR}/shared/include")
        message(STATUS "ihs_shared (checkout)   = ${IHS_SHARED_LIB}")
    else ()
        find_package(ivi-homescreen-shared CONFIG QUIET)
        if (ivi-homescreen-shared_FOUND)
            target_link_libraries(ihs_pv INTERFACE ivi_homescreen::ihs_shared)
            message(STATUS "ihs_shared (package)    = ${ivi-homescreen-shared_VERSION}")
        else ()
            pkg_check_modules(IHS_SHARED IMPORTED_TARGET ivi-homescreen-shared)
            if (NOT IHS_SHARED_FOUND)
                message(FATAL_ERROR
                    "ENABLE_IHS_PV: ivi-homescreen-shared not found. Install it, or "
                    "set -DIHS_DIR=<ivi-homescreen checkout> (and -DIHS_BUILD_DIR).")
            endif ()
            target_link_libraries(ihs_pv INTERFACE PkgConfig::IHS_SHARED)
            message(STATUS "ihs_shared (pkg-config) = ${IHS_SHARED_VERSION}")
        endif ()
    endif ()

    add_library(bev_view SHARED
        egl_context.cpp
        ihs/bev_ihs_pv.cpp
        ihs/bev_params.cpp
        ihs/bev_pipeline.cpp
        ihs/dmabuf_ring.cpp
    )
    target_include_directories(bev_view PRIVATE ihs)
    target_link_libraries(bev_view PRIVATE bev_core ihs_pv)
    set_target_properties(bev_view PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    # A missing library should fail here, not when the shell dlopens the view.
    # ihs_pv_assets_path is a weak import, which this still permits.
    target_link_options(bev_view PRIVATE "LINKER:--no-undefined")

    if (ENABLE_IHS_PV_VULKAN)
        find_path(VULKAN_HEADERS_DIR vulkan/vulkan.h)
        if (VULKAN_HEADERS_DIR)
            target_sources(bev_view PRIVATE ihs/vk_dmabuf_allocator.cpp)
            target_include_directories(bev_view PRIVATE "${VULKAN_HEADERS_DIR}")
            target_compile_definitions(bev_view PRIVATE BEV_IHS_PV_VULKAN=1)
            message(STATUS "bev_view Vulkan alloc   = ON (${VULKAN_HEADERS_DIR})")
        else ()
            message(WARNING
                "ENABLE_IHS_PV_VULKAN is ON but vulkan/vulkan.h was not found -- "
                "on a Vulkan shell the view will allocate from its render node "
                "instead of the shell's device.")
        endif ()
    endif ()
endif ()
