# Copyright (C) 2026 The Thimsy OS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Nano for Linux, the host and (later) the app (docs/linux/IMPLEMENTATION.md C.2). Android.bp stays
# the only build of the Android firmware (decision I4): there is no nano_platform_firmware here.
# Fork-only (tools/nano-tree/upstream.json): never in the patch. Written, not configured: the
# machine it was written on has no CMake (nano/test/host/Makefile is the host build that runs today);
# tools/nano-tree/tests/test_nano_cmake.py checks it statically.
#
# The lists come from nano/build/sources.txt (cmake/sources.cmake reads it with file(STRINGS)) and
# nano/build/unit-closure.txt; the flags from nano/Android.bp (cmake/androidbp_flags.cmake). No file
# list is written here.
#
# Targets (C.2's table; a target whose sources are not in the tree yet is not defined, and says so):
#   nano_compat          static   compat/src/*.cpp (B.2's shims); links libthimsy and the image
#                                 libraries whose headers exist (the probe imagedecoder.cpp makes)
#   nano_stubs           static   compat/stubs/src/*.cpp (B.4's layer-2 stubs; G4)
#   nano_core            static   [core], minus the files a feature switch leaves out (cmake/features.cmake)
#   nano_platform_linux  static   [linux] minus main_linux.cpp and ShellGate.cpp, plus [drm]   (G9)
#   nano_shellgate       static   platform/linux/ShellGate.cpp (the --wrap targets, B.5)        (G9)
#   nano_platform_host   static   [host] minus main_headless.cpp                                (G5)
#   nano_glstub          static   test/host/glstub/glue.c (the hand-written part)               (G5)
#   thimsy-nano          exe      main_linux.cpp, the generated [dropped-linux] stubs           (G9)
#   thimsy-nano-scrape   exe      [scrape]
#   nano-headless        exe      main_headless.cpp, the generated stubs and GL stub            (G5)
#   nano_unit_tests      custom   one program per test/unit/*_test.cpp (unit-closure.txt), CTest
#
# Where this differs from C.2's table, and why:
#   - The [dropped-linux] stubs (and the GL stub's generated part) are compiled into the executable,
#     not into the platform library: they are made from `nm` of the whole link, main included
#     (cmake/stubgen.cmake), so a library holding them would depend on itself. For the same reason
#     each main is a one-file static library (crt1.o's reference to main pulls it in), so nm can
#     read every input as an archive.
#   - nano_core and the platform library call each other ([core] calls NanoMenu:: methods [drm] or
#     DrmHost.cpp defines), so the executables link those two archives inside one
#     --start-group/--end-group.
#   - nano_unit_tests is a custom target over one program per test, not one executable: each test's
#     closure differs and a program whose headers are missing is skipped alone (C.3).
#   - nano_shellgate is split out of the platform so thimsy-nano-scrape links the same shell gate.
#
# Options:
#   NANO_PLATFORM       linux (default; Buildroot), host, all, or none (the libraries, the scraper
#                       and the unit tests only)
#   NANO_BUILD_SCRAPE   thimsy-nano-scrape (ON)
#   NANO_BUILD_TESTS    the unit test programs and CTest (OFF)
#   NANO_WITH_AC3, NANO_WITH_PDF, NANO_WITH_ARCHIVE, NANO_WITH_MEDIA   feature switches, all OFF
#                       (cmake/features.cmake says what each leaves out)
#   NANO_WERROR         -Werror for the device build (OFF: GCC warns differently from Clang; the
#                       unit tests always build with -Werror)
#   NANO_REQUIRE_IMAGE_LIBS  a missing PNG, JPEG, WebP or GIF library is an error, not a format the
#                       photo viewer quietly lacks (the Buildroot recipe turns it on)
#   NANO_USE_SYSTEM_GTEST, NANO_UNIT_SANITIZE, NANO_DATA, NANO_TEST_LAUNCHER   for the unit tests
#   NANO_LIBTHIMSY_SOURCE_DIR   build libthimsy from this tree (default: a parent's thimsy::thimsy,
#                       else find_package(thimsy) as in Buildroot, else the repository's libthimsy/)
cmake_minimum_required(VERSION 3.16)
project(thimsy-nano LANGUAGES C CXX)

set(NANO_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${NANO_DIR}/cmake")
include(sources)
include(androidbp_flags)
include(features)
include(deps)
include(stubgen)
include(CheckIncludeFiles)
include(GNUInstallDirs)

# --- options ---------------------------------------------------------------------------------------
set(NANO_PLATFORMS linux host all none)
set(NANO_PLATFORM "linux" CACHE STRING "Which Nano executables to build: linux, host, all or none")
set_property(CACHE NANO_PLATFORM PROPERTY STRINGS ${NANO_PLATFORMS})
option(NANO_BUILD_SCRAPE "Build thimsy-nano-scrape, the background scraper" ON)
option(NANO_BUILD_TESTS "Build the unit test programs (test/unit) and register them with CTest" OFF)
option(NANO_WITH_AC3 "Build the AC-3 decoder (vendored liba52, GPL-2.0)" OFF)
option(NANO_WITH_PDF "Build the game manual viewer (needs pdfium)" OFF)
option(NANO_WITH_ARCHIVE "Build zip/7z browsing and the OTA check (need libziparchive and the LZMA SDK)" OFF)
option(NANO_WITH_MEDIA "Use a real media back end instead of the layer-2 stubs (none exists yet)" OFF)
option(NANO_WERROR "Treat warnings as errors in the device build" OFF)
option(NANO_REQUIRE_IMAGE_LIBS "Fail when an image library the photo viewer uses is missing" OFF)
option(NANO_USE_SYSTEM_GTEST "Unit tests: use an installed GoogleTest, else libthimsy's stand-in" ON)
option(NANO_UNIT_SANITIZE "Unit tests: build with AddressSanitizer and UBSan" ON)
if(EXISTS "${NANO_DIR}/../work/nano-composed")
    get_filename_component(_nano_data_default "${NANO_DIR}/../work/nano-composed" ABSOLUTE)
else()
    set(_nano_data_default "")
endif()
set(NANO_DATA "${_nano_data_default}" CACHE PATH
    "Unit tests: a tree with the base's asset folders (nano_tree.py compose); empty disables the tests that need them")
set(NANO_TEST_LAUNCHER "" CACHE STRING "Unit tests: a command list each test program runs under (e.g. setarch;x86_64;-R)")
set(NANO_LIBTHIMSY_SOURCE_DIR "" CACHE PATH "Build libthimsy from this source tree")

if(NOT NANO_PLATFORM IN_LIST NANO_PLATFORMS)
    message(FATAL_ERROR "nano: NANO_PLATFORM is '${NANO_PLATFORM}'; it must be one of: ${NANO_PLATFORMS}")
endif()
if(NANO_WITH_MEDIA)
    message(FATAL_ERROR "nano: NANO_WITH_MEDIA=ON: there is no Linux media back end yet; the media files "
                        "compile against the layer-2 stubs (docs/linux/IMPLEMENTATION.md B.4)")
endif()
set(NANO_BUILD_LINUX OFF)
set(NANO_BUILD_HOST OFF)
if(NANO_PLATFORM STREQUAL "linux" OR NANO_PLATFORM STREQUAL "all")
    set(NANO_BUILD_LINUX ON)
endif()
if(NANO_PLATFORM STREQUAL "host" OR NANO_PLATFORM STREQUAL "all")
    set(NANO_BUILD_HOST ON)
endif()

# -std=gnu++17 and gnu11, as C.2 and the host Makefile.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
# No LTO for Nano at first: GCC's LTO and --wrap are a doubtful mix (B.5).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

# --- the lists -------------------------------------------------------------------------------------
nano_read_sources("${NANO_DIR}/build/sources.txt" NANO_SRC)
foreach(_s IN LISTS NANO_SOURCE_SECTIONS)
    string(REPLACE "-" "_" _v "${_s}")
    nano_sources_require_files("${NANO_DIR}" "${_s}" ${NANO_SRC_${_v}})
endforeach()

# The files of [core] each OFF feature leaves out (cmake/features.cmake), and with [dropped-linux]
# the files whose definitions the Linux and host links stub.
set(NANO_LEFT_OUT "")
foreach(_f IN LISTS NANO_FEATURES)
    foreach(_file IN LISTS NANO_FEATURE_FILES_${_f})
        if(NOT _file IN_LIST NANO_SRC_core)
            message(FATAL_ERROR "nano: cmake/features.cmake lists ${_file} for ${_f}, but it is not in [core]")
        endif()
    endforeach()
    if(NOT NANO_WITH_${_f})
        list(APPEND NANO_LEFT_OUT ${NANO_FEATURE_FILES_${_f}})
    endif()
endforeach()
set(NANO_CORE_SOURCES ${NANO_SRC_core})
if(NANO_LEFT_OUT)
    list(REMOVE_ITEM NANO_CORE_SOURCES ${NANO_LEFT_OUT})
endif()
set(NANO_STUBGEN_FROM ${NANO_SRC_dropped_linux} ${NANO_LEFT_OUT})

# The fixed parts of the platform sections (G9's and G5's files).
set(NANO_LINUX_MAIN "platform/linux/main_linux.cpp")
set(NANO_LINUX_SHELLGATE "platform/linux/ShellGate.cpp")
set(NANO_LINUX_DRM_BUFFERS "platform/linux/display/DrmBuffersGbm.cpp")
set(NANO_LINUX_EGL_WRAPS "platform/linux/display/EglWraps.cpp")
set(NANO_LINUX_WRAPS_FILE "${NANO_DIR}/platform/linux/linker-wraps.txt")
set(NANO_HOST_MAIN "test/host/headless/main_headless.cpp")
set(NANO_GLSTUB_GLUE "test/host/glstub/glue.c")
set(NANO_GLSTUB_GEN "test/host/glstub/gen.py")

# --- flags -----------------------------------------------------------------------------------------
# gammaos-nano's and gammaos-nano-scrape's top-level cflags, read from Android.bp (C.2). -Werror is
# dropped unless NANO_WERROR. The arch-specific ones (arm64: -mtune=cortex-a55) are not taken: the
# Buildroot toolchain sets the -mcpu of each board's SoC, and the H700 is a Cortex-A53.
nano_bp_list("${NANO_DIR}/Android.bp" gammaos-nano cflags NANO_BP_CFLAGS)
nano_bp_list("${NANO_DIR}/Android.bp" gammaos-nano-scrape cflags NANO_BP_SCRAPE_CFLAGS)
nano_bp_list("${NANO_DIR}/Android.bp" gammaos-nano local_include_dirs NANO_BP_LOCAL_INCLUDE_DIRS)
if(NOT "-DGL_GLEXT_PROTOTYPES" IN_LIST NANO_BP_CFLAGS OR NOT "-Wall" IN_LIST NANO_BP_SCRAPE_CFLAGS)
    message(FATAL_ERROR "nano: reading gammaos-nano's cflags from Android.bp failed (got '${NANO_BP_CFLAGS}'); "
                        "has its layout changed? (cmake/androidbp_flags.cmake)")
endif()
if(NOT NANO_WERROR)
    list(REMOVE_ITEM NANO_BP_CFLAGS -Werror)
    list(REMOVE_ITEM NANO_BP_SCRAPE_CFLAGS -Werror)
endif()
# GCC-only warnings Clang does not give, so no Nano file is edited for GCC's sake (C.3). The same
# list as nano/test/host/Makefile's GCC_SUPPRESS (test_nano_cmake.py compares them).
set(NANO_GCC_SUPPRESS -Wno-class-memaccess -Wno-stringop-truncation -Wno-format-truncation -Wno-dangling-else
    -Wno-misleading-indentation)
set(NANO_SUPPRESS "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(NANO_SUPPRESS ${NANO_GCC_SUPPRESS})
endif()
# Nano's own warnings for the code Thimsy writes (compat, stubs): compat.mk's.
set(NANO_COMPAT_WARN -Wall -Wextra $<$<CXX_COMPILER_ID:GNU>:-Wno-clobbered> $<$<BOOL:${NANO_WERROR}>:-Werror>)

add_library(nano_flags INTERFACE)
target_compile_options(nano_flags INTERFACE ${NANO_BP_CFLAGS} ${NANO_SUPPRESS})
add_library(nano_scrape_flags INTERFACE)
target_compile_options(nano_scrape_flags INTERFACE ${NANO_BP_SCRAPE_CFLAGS} ${NANO_SUPPRESS})

# The symbols the executables wrap (B.5): C.2's list for every executable, ioctl for the headless
# one (FakeEvdev.cpp), and thimsy-nano's from platform/linux/linker-wraps.txt (G9: it adds the EGL
# gate of drmSetupZeroCopy), which must hold C.2's list.
set(NANO_SHELL_WRAPS system popen pclose execv execvp execve)
set(NANO_HEADLESS_WRAPS ${NANO_SHELL_WRAPS} ioctl)
set(NANO_LINUX_WRAPS ${NANO_SHELL_WRAPS})
if(EXISTS "${NANO_LINUX_WRAPS_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NANO_LINUX_WRAPS_FILE}")
    file(STRINGS "${NANO_LINUX_WRAPS_FILE}" _wraps REGEX "^[A-Za-z_][A-Za-z0-9_]*[ \t]*$")
    set(NANO_LINUX_WRAPS "")
    foreach(_w IN LISTS _wraps)
        string(STRIP "${_w}" _w)
        list(APPEND NANO_LINUX_WRAPS "${_w}")
    endforeach()
    foreach(_w IN LISTS NANO_SHELL_WRAPS)
        if(NOT _w IN_LIST NANO_LINUX_WRAPS)
            message(FATAL_ERROR "nano: platform/linux/linker-wraps.txt lacks ${_w} (C.2's shell gate list)")
        endif()
    endforeach()
endif()
function(nano_wrap target)
    foreach(_s IN LISTS ARGN)
        target_link_options(${target} PRIVATE "LINKER:--wrap=${_s}")
    endforeach()
endfunction()

# --- libthimsy -------------------------------------------------------------------------------------
# A parent's target; else the tree asked for; else the installed package (Buildroot's staging: the
# nano tarball holds only nano/, so ../libthimsy is not there); else the repository's libthimsy/.
if(NOT TARGET thimsy::thimsy)
    if(NANO_LIBTHIMSY_SOURCE_DIR)
        add_subdirectory("${NANO_LIBTHIMSY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libthimsy" EXCLUDE_FROM_ALL)
    else()
        find_package(thimsy CONFIG QUIET)
        if(NOT thimsy_FOUND)
            if(EXISTS "${NANO_DIR}/../libthimsy/CMakeLists.txt")
                add_subdirectory("${NANO_DIR}/../libthimsy" "${CMAKE_CURRENT_BINARY_DIR}/libthimsy" EXCLUDE_FROM_ALL)
            else()
                message(FATAL_ERROR "nano: libthimsy not found: install it (find_package(thimsy)) or set "
                                    "NANO_LIBTHIMSY_SOURCE_DIR")
            endif()
        endif()
    endif()
endif()

nano_dep_standard(threads REQUIRED)

# --- nano_compat: the shims (B.2) ------------------------------------------------------------------
file(GLOB NANO_COMPAT_SOURCES CONFIGURE_DEPENDS "${NANO_DIR}/compat/src/*.cpp")
add_library(nano_compat STATIC ${NANO_COMPAT_SOURCES})
target_include_directories(nano_compat PUBLIC "${NANO_DIR}/compat/include" PRIVATE "${NANO_DIR}/compat/src")
# Every Nano translation unit sees Bionic's extras first (NANOCOMPAT_CPPFLAGS in compat.mk).
target_compile_options(nano_compat PUBLIC "SHELL:-include nanocompat/bionic.h")
target_compile_options(nano_compat PRIVATE ${NANO_COMPAT_WARN})
target_link_libraries(nano_compat PUBLIC thimsy::thimsy nano::threads)

# The image decoder (compat/src/imagedecoder.cpp) decodes each format whose header __has_include
# finds; here a format whose header exists links its library, and one whose library is missing is
# compiled out with its NANOCOMPAT_NO_<FORMAT> switch, so the decoder and the link agree (G1b).
function(nano_image_format fmt header dep)
    string(MAKE_C_IDENTIFIER "NANO_HAVE_HEADER_${header}" _hv)
    check_include_files("stddef.h;stdio.h;${header}" ${_hv} LANGUAGE CXX)
    if(NOT ${_hv})
        if(NANO_REQUIRE_IMAGE_LIBS)
            message(FATAL_ERROR "nano: NANO_REQUIRE_IMAGE_LIBS: <${header}> not found (${fmt})")
        endif()
        message(STATUS "nano: image decoder without ${fmt} (no <${header}>)")
        return()
    endif()
    set(_ok TRUE)
    foreach(_d IN LISTS dep)
        if(_d STREQUAL "webp")
            nano_dep(webp PKG libwebp LIBS webp HEADER webp/decode.h)
        elseif(_d STREQUAL "webpdemux")
            nano_dep(webpdemux PKG libwebpdemux LIBS webpdemux HEADER webp/demux.h)
        else()
            nano_dep_standard(${_d})
        endif()
        if(NOT TARGET nano::${_d})
            set(_ok FALSE)
        endif()
    endforeach()
    if(_ok)
        foreach(_d IN LISTS dep)
            target_link_libraries(nano_compat PUBLIC nano::${_d})
        endforeach()
        message(STATUS "nano: image decoder with ${fmt}")
    elseif(NANO_REQUIRE_IMAGE_LIBS)
        message(FATAL_ERROR "nano: NANO_REQUIRE_IMAGE_LIBS: <${header}> exists but the ${fmt} library (${dep}) was not found")
    else()
        message(WARNING "nano: <${header}> exists but the ${fmt} library (${dep}) was not found; "
                        "the image decoder is built without ${fmt}")
        target_compile_definitions(nano_compat PRIVATE NANOCOMPAT_NO_${fmt})
    endif()
endfunction()
nano_image_format(PNG png.h png)
nano_image_format(JPEG jpeglib.h jpeg)
# Animated WebP needs libwebpdemux when its header exists; imagedecoder.cpp has no switch for the
# demuxer alone, so a missing demux library turns WebP off.
check_include_files("webp/demux.h" NANO_HAVE_HEADER_webp_demux_h LANGUAGE CXX)
if(NANO_HAVE_HEADER_webp_demux_h)
    nano_image_format(WEBP webp/decode.h "webp;webpdemux")
elseif(NANO_REQUIRE_IMAGE_LIBS)
    # The image's build (thimsy-nano selects BR2_PACKAGE_LIBWEBP_DEMUX): without the demuxer an
    # animated WebP would quietly show its first frame only.
    message(FATAL_ERROR "nano: NANO_REQUIRE_IMAGE_LIBS: <webp/demux.h> not found (animated WEBP)")
else()
    nano_image_format(WEBP webp/decode.h webp)
endif()
nano_image_format(GIF gif_lib.h gif)

# --- the vendored libraries (their file lists and flags from Android.bp) ----------------------------
nano_bp_list("${NANO_DIR}/Android.bp" libnanosvg_nano srcs NANO_BP_NANOSVG_SRCS)
nano_bp_list("${NANO_DIR}/Android.bp" libnanosvg_nano cflags NANO_BP_NANOSVG_CFLAGS)
list(TRANSFORM NANO_BP_NANOSVG_SRCS PREPEND "${NANO_DIR}/")
add_library(nano_nanosvg STATIC ${NANO_BP_NANOSVG_SRCS})
target_include_directories(nano_nanosvg PUBLIC "${NANO_DIR}/nanosvg")
# -w as on Android (the upstream source is not clean under any warning set); Clang-only -Wno-*
# flags are harmless to GCC under -w.
target_compile_options(nano_nanosvg PRIVATE ${NANO_BP_NANOSVG_CFLAGS} -w)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(nano_nanosvg PUBLIC m)
endif()

if(NANO_WITH_AC3)
    nano_bp_list("${NANO_DIR}/Android.bp" liba52_nano srcs NANO_BP_A52_SRCS)
    nano_bp_list("${NANO_DIR}/Android.bp" liba52_nano cflags NANO_BP_A52_CFLAGS)
    nano_bp_list("${NANO_DIR}/Android.bp" liba52_nano local_include_dirs NANO_BP_A52_INCLUDES)
    list(TRANSFORM NANO_BP_A52_SRCS PREPEND "${NANO_DIR}/")
    list(TRANSFORM NANO_BP_A52_INCLUDES PREPEND "${NANO_DIR}/")
    add_library(nano_a52 STATIC ${NANO_BP_A52_SRCS})
    target_include_directories(nano_a52 PRIVATE ${NANO_BP_A52_INCLUDES} PUBLIC "${NANO_DIR}/liba52/include")
    target_compile_options(nano_a52 PRIVATE ${NANO_BP_A52_CFLAGS} -w)
    message(STATUS "nano: NANO_WITH_AC3=ON links liba52 (GPL-2.0)")
endif()

# --- nano_stubs: layer 2 (B.4) ---------------------------------------------------------------------
file(GLOB NANO_STUBS_SOURCES CONFIGURE_DEPENDS "${NANO_DIR}/compat/stubs/src/*.cpp")
add_library(nano_stubs STATIC ${NANO_STUBS_SOURCES})
target_include_directories(nano_stubs PUBLIC "${NANO_DIR}/compat/stubs/include")
# EGLNativeWindowType is void*, so a stub Surface* passes to eglCreateWindowSurface as on Android
# (stubs.mk's NANOSTUBS_CPPFLAGS).
target_compile_definitions(nano_stubs PUBLIC EGL_NO_PLATFORM_SPECIFIC_TYPES)
target_compile_options(nano_stubs PRIVATE ${NANO_COMPAT_WARN})
target_link_libraries(nano_stubs PUBLIC nano_compat)

# --- nano_core -------------------------------------------------------------------------------------
set(NANO_BUILD_CORE OFF)
if(NANO_BUILD_LINUX OR NANO_BUILD_HOST)
    set(NANO_BUILD_CORE ON)
endif()

if(NANO_BUILD_CORE)
    nano_dep_standard(zlib REQUIRED)
    nano_dep_standard(crypto REQUIRED)
    nano_dep_standard(tinyxml2 REQUIRED)
    nano_dep_standard(freetype REQUIRED)
    nano_dep_standard(png REQUIRED)
    nano_dep_standard(curl REQUIRED)
    # [core] needs only the EGL, GLES and libdrm headers; the libraries are the platform's (the host
    # links the GL stub instead).
    nano_dep(egl_headers REQUIRED HEADER EGL/egl.h)
    nano_dep(gles_headers REQUIRED HEADER GLES2/gl2.h)
    nano_dep(drm_headers REQUIRED HEADER drm.h PATH_SUFFIXES libdrm)

    set(_core ${NANO_CORE_SOURCES})
    list(TRANSFORM _core PREPEND "${NANO_DIR}/")
    add_library(nano_core STATIC ${_core})
    # The order tools/nano-tree/include_census.py resolves includes in: the shims, the stubs, nano/,
    # then Android.bp's local_include_dirs (liba52/include, nanosvg).
    set(_core_includes "${NANO_DIR}/compat/include" "${NANO_DIR}/compat/stubs/include" "${NANO_DIR}")
    foreach(_d IN LISTS NANO_BP_LOCAL_INCLUDE_DIRS)
        list(APPEND _core_includes "${NANO_DIR}/${_d}")
    endforeach()
    target_include_directories(nano_core PUBLIC ${_core_includes})
    target_link_libraries(nano_core
        PUBLIC nano_stubs nano_compat nano_nanosvg nano::zlib nano::crypto nano::tinyxml2 nano::freetype
               nano::png nano::curl nano::egl_headers nano::gles_headers nano::drm_headers nano::threads
        PRIVATE nano_flags)
    if(NANO_WITH_AC3)
        target_link_libraries(nano_core PUBLIC nano_a52)
    endif()
    if(NANO_WITH_PDF)
        nano_dep(pdfium REQUIRED PKG pdfium LIBS pdfium HEADER fpdfview.h)
        target_link_libraries(nano_core PUBLIC nano::pdfium)
    endif()
    if(NANO_WITH_ARCHIVE)
        nano_dep(ziparchive REQUIRED LIBS ziparchive HEADER ziparchive/zip_archive.h)
        nano_dep(lzma_sdk REQUIRED LIBS lzma HEADER 7z.h)
        target_link_libraries(nano_core PUBLIC nano::ziparchive nano::lzma_sdk)
    endif()
    if(NANO_LEFT_OUT)
        string(REPLACE ";" " " _left "${NANO_LEFT_OUT}")
        message(STATUS "nano: left out of nano_core (stubbed): ${_left}")
    endif()
    find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
endif()

# --- the shell gate (G9) ---------------------------------------------------------------------------
if(NANO_LINUX_SHELLGATE IN_LIST NANO_SRC_linux)
    add_library(nano_shellgate STATIC "${NANO_DIR}/${NANO_LINUX_SHELLGATE}")
    target_include_directories(nano_shellgate PUBLIC "${NANO_DIR}/platform/linux")
    target_link_libraries(nano_shellgate PUBLIC nano_compat PRIVATE nano_flags)
endif()

# Links <exe> with <a> and <b> inside one group (they call each other), then the targets themselves
# for their usage requirements and the build order.
function(nano_link_group exe a b)
    target_link_libraries(${exe} PRIVATE "-Wl,--start-group" "$<TARGET_FILE:${a}>" "$<TARGET_FILE:${b}>"
                                         "-Wl,--end-group" ${a} ${b})
endfunction()

# --- Linux: nano_platform_linux and thimsy-nano (G9) -----------------------------------------------
if(NANO_BUILD_LINUX)
    if(NOT NANO_LINUX_MAIN IN_LIST NANO_SRC_linux)
        message(FATAL_ERROR "nano: NANO_PLATFORM=${NANO_PLATFORM} needs sources.txt's [linux] with "
                            "${NANO_LINUX_MAIN} (G9); [linux] is: '${NANO_SRC_linux}'")
    endif()
    # Without the GBM back end the [dropped-linux] stubs would quietly stand in for the render
    # targets' buffers (NanoMenuDrmBuffers.cpp's functions); refuse that.
    if(NOT NANO_LINUX_DRM_BUFFERS IN_LIST NANO_SRC_linux)
        message(FATAL_ERROR "nano: sources.txt's [linux] lacks ${NANO_LINUX_DRM_BUFFERS} (B.8)")
    endif()
    # linker-wraps.txt wraps eglGetProcAddress and eglQueryString; their __wrap_ definitions are
    # here, and without them the link fails on __wrap_egl* rather than at configure.
    if(NOT NANO_LINUX_EGL_WRAPS IN_LIST NANO_SRC_linux)
        message(FATAL_ERROR "nano: sources.txt's [linux] lacks ${NANO_LINUX_EGL_WRAPS} (B.8)")
    endif()
    if(NOT TARGET nano_shellgate)
        message(FATAL_ERROR "nano: sources.txt's [linux] lacks ${NANO_LINUX_SHELLGATE} (B.5)")
    endif()
    nano_dep(gbm REQUIRED PKG gbm LIBS gbm HEADER gbm.h)
    nano_dep(egl REQUIRED PKG egl LIBS EGL HEADER EGL/egl.h)
    nano_dep(glesv2 REQUIRED PKG glesv2 LIBS GLESv2 HEADER GLES2/gl2.h)
    nano_dep(drm REQUIRED PKG libdrm LIBS drm HEADER xf86drm.h)
    nano_dep_standard(alsa REQUIRED)

    set(_plat ${NANO_SRC_linux} ${NANO_SRC_drm})
    list(REMOVE_ITEM _plat "${NANO_LINUX_MAIN}" "${NANO_LINUX_SHELLGATE}")
    list(TRANSFORM _plat PREPEND "${NANO_DIR}/")
    add_library(nano_platform_linux STATIC ${_plat})
    target_include_directories(nano_platform_linux PUBLIC "${NANO_DIR}/platform/linux")
    # main_linux.cpp, ShellGate.cpp and the [dropped-linux] stubs stay out of the library (above).
    # libnanostubs (nano_stubs) is named here, as in the Makefiles, not only through nano_core.
    target_link_libraries(nano_platform_linux
        PUBLIC nano_core nano_stubs nano_shellgate nano::gbm nano::egl nano::glesv2 nano::drm nano::alsa
        PRIVATE nano_flags)

    add_library(nano_linux_main STATIC "${NANO_DIR}/${NANO_LINUX_MAIN}")
    target_link_libraries(nano_linux_main PRIVATE nano_platform_linux nano_flags)

    nano_dropped_stubs(_linux_stubs linux
        ARCHIVES nano_linux_main nano_platform_linux nano_shellgate nano_core nano_stubs nano_compat
        FROM ${NANO_STUBGEN_FROM})
    add_executable(thimsy-nano "${_linux_stubs}")
    target_link_libraries(thimsy-nano PRIVATE nano_linux_main nano_flags)
    nano_link_group(thimsy-nano nano_platform_linux nano_core)
    nano_wrap(thimsy-nano ${NANO_LINUX_WRAPS})
    install(TARGETS thimsy-nano RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# --- the host: nano_platform_host, nano_glstub and nano-headless (G5) -------------------------------
if(NANO_BUILD_HOST)
    if(NOT NANO_HOST_MAIN IN_LIST NANO_SRC_host)
        message(FATAL_ERROR "nano: NANO_PLATFORM=${NANO_PLATFORM} needs sources.txt's [host] with "
                            "${NANO_HOST_MAIN} (G5); [host] is: '${NANO_SRC_host}'")
    endif()
    if(NOT EXISTS "${NANO_DIR}/${NANO_GLSTUB_GLUE}" OR NOT EXISTS "${NANO_DIR}/${NANO_GLSTUB_GEN}")
        message(FATAL_ERROR "nano: NANO_PLATFORM=${NANO_PLATFORM} needs the GL stub "
                            "(${NANO_GLSTUB_GLUE}, ${NANO_GLSTUB_GEN}; G5)")
    endif()
    set(_plat ${NANO_SRC_host})
    list(REMOVE_ITEM _plat "${NANO_HOST_MAIN}")
    list(TRANSFORM _plat PREPEND "${NANO_DIR}/")
    add_library(nano_platform_host STATIC ${_plat})
    target_include_directories(nano_platform_host PUBLIC "${NANO_DIR}/platform/host")
    target_link_libraries(nano_platform_host PUBLIC nano_core nano_stubs PRIVATE nano_flags)

    add_library(nano_glstub STATIC "${NANO_DIR}/${NANO_GLSTUB_GLUE}")
    target_include_directories(nano_glstub PUBLIC "${NANO_DIR}/test/host/glstub")
    target_compile_options(nano_glstub PRIVATE -Wall)

    add_library(nano_host_main STATIC "${NANO_DIR}/${NANO_HOST_MAIN}")
    target_link_libraries(nano_host_main PRIVATE nano_platform_host nano_flags)

    nano_dropped_stubs(_host_stubs host
        ARCHIVES nano_host_main nano_platform_host nano_core nano_stubs nano_compat
        FROM ${NANO_STUBGEN_FROM})
    nano_glstub_gen(_host_gl host
        ARCHIVES nano_host_main nano_platform_host nano_core nano_stubs nano_compat
        GLUE nano_glstub)
    add_executable(nano-headless "${_host_stubs}" "${_host_gl}")
    target_link_libraries(nano-headless PRIVATE nano_host_main nano_glstub nano_flags)
    nano_link_group(nano-headless nano_platform_host nano_core)
    nano_wrap(nano-headless ${NANO_HEADLESS_WRAPS})
endif()

# --- thimsy-nano-scrape ----------------------------------------------------------------------------
# gammaos-nano.rc starts the scraper as its own small process on scrape_run; on Linux thimsy-session
# starts this binary (C.2, decision I25). It links the compat layer and the shell gate, not nano_core.
if(NANO_BUILD_SCRAPE)
    if(NOT TARGET nano_shellgate)
        message(FATAL_ERROR "nano: NANO_BUILD_SCRAPE needs the shell gate (${NANO_LINUX_SHELLGATE} in "
                            "sources.txt's [linux], G9) for its --wrap set; or set NANO_BUILD_SCRAPE=OFF")
    endif()
    nano_dep_standard(curl REQUIRED)
    nano_dep_standard(ssl REQUIRED)
    nano_dep_standard(crypto REQUIRED)
    nano_dep_standard(zlib REQUIRED)
    set(_scrape ${NANO_SRC_scrape})
    list(TRANSFORM _scrape PREPEND "${NANO_DIR}/")
    add_executable(thimsy-nano-scrape ${_scrape})
    target_include_directories(thimsy-nano-scrape PRIVATE "${NANO_DIR}")
    target_link_libraries(thimsy-nano-scrape PRIVATE nano_compat nano_shellgate nano::curl nano::ssl
                                                     nano::crypto nano::zlib nano_scrape_flags)
    nano_wrap(thimsy-nano-scrape ${NANO_SHELL_WRAPS})
    install(TARGETS thimsy-nano-scrape RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# --- the unit tests (C.3, D.1) ---------------------------------------------------------------------
if(NANO_BUILD_TESTS)
    enable_testing()
    nano_read_unit_closure("${NANO_DIR}/build/unit-closure.txt" NANO_UNIT)

    set(NANO_GTEST "")
    if(NANO_USE_SYSTEM_GTEST)
        find_package(GTest QUIET)
        if(TARGET GTest::gtest_main)
            set(NANO_GTEST GTest::gtest_main)
        elseif(TARGET GTest::Main)
            set(NANO_GTEST GTest::Main)
        endif()
    endif()
    if(NOT NANO_GTEST)
        set(NANO_GTEST thimsy::gtest_standin)
    endif()

    set(NANO_UNIT_SANITIZE_FLAGS "")
    if(NANO_UNIT_SANITIZE)
        set(NANO_UNIT_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer
            -fno-sanitize-recover=undefined)
    endif()
    add_library(nano_unit_flags INTERFACE)
    target_compile_options(nano_unit_flags INTERFACE -Wall -Werror ${NANO_SUPPRESS} ${NANO_UNIT_SANITIZE_FLAGS})
    target_link_options(nano_unit_flags INTERFACE ${NANO_UNIT_SANITIZE_FLAGS})
    set(_unit_includes "${NANO_DIR}")
    foreach(_d IN LISTS NANO_BP_LOCAL_INCLUDE_DIRS)
        list(APPEND _unit_includes "${NANO_DIR}/${_d}")
    endforeach()
    list(APPEND _unit_includes "${NANO_DIR}/test/unit")
    target_include_directories(nano_unit_flags INTERFACE ${_unit_includes})
    target_link_libraries(nano_unit_flags INTERFACE nano_compat)

    # The outside headers a program may need (unit-closure.txt's third field), and the dependency
    # each comes from: the host Makefile's unit_ldlibs.
    function(nano_unit_header_dep header out)
        if(header STREQUAL "zlib.h")
            set(${out} zlib PARENT_SCOPE)
        elseif(header MATCHES "^openssl/")
            set(${out} crypto PARENT_SCOPE)
        elseif(header STREQUAL "tinyxml2.h")
            set(${out} tinyxml2 PARENT_SCOPE)
        elseif(header MATCHES "^ziparchive/")
            set(${out} ziparchive PARENT_SCOPE)
        else()
            set(${out} "" PARENT_SCOPE)
        endif()
    endfunction()

    set(NANO_UNIT_ASSET_DIRS ps3xmb nano_esde_themes nano_esde_assets)
    set(NANO_UNIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/unit")
    file(MAKE_DIRECTORY "${NANO_UNIT_DIR}/run" "${NANO_UNIT_DIR}/tmp")
    list(JOIN NANO_UNIT_ASSET_DIRS "|" _asset_dirs)
    add_test(NAME nano_unit_data
        COMMAND "${CMAKE_COMMAND}" "-DNANO=${NANO_DIR}" "-DOUT=${NANO_UNIT_DIR}/data"
                "-DNANO_DATA=${NANO_DATA}" "-DASSET_DIRS=${_asset_dirs}" -P "${NANO_DIR}/cmake/unit_data.cmake")
    set_tests_properties(nano_unit_data PROPERTIES FIXTURES_SETUP nano_unit_data)

    set(_unit_targets "")
    foreach(_p IN LISTS NANO_UNIT_PROGRAMS)
        set(_missing "")
        set(_deps "")
        foreach(_h IN LISTS NANO_UNIT_HEADERS_${_p})
            nano_unit_header_dep("${_h}" _d)
            if(_d STREQUAL "")
                list(APPEND _missing "${_h} (no rule in nano_unit_header_dep)")
                continue()
            endif()
            if(_d STREQUAL "ziparchive")
                nano_dep(ziparchive LIBS ziparchive HEADER ziparchive/zip_archive.h)
            else()
                nano_dep_standard(${_d})
            endif()
            if(TARGET nano::${_d})
                list(APPEND _deps nano::${_d})
            else()
                list(APPEND _missing "${_h}")
            endif()
        endforeach()
        if(_missing)
            string(REPLACE ";" " " _missing "${_missing}")
            message(STATUS "nano: SKIP ${_p}: missing ${_missing}")
            continue()
        endif()
        list(REMOVE_DUPLICATES _deps)
        # Each production file compiles once (an object library), whatever number of programs use it.
        set(_objs "")
        foreach(_src IN LISTS NANO_UNIT_SRCS_${_p})
            string(MAKE_C_IDENTIFIER "nano_unit_obj_${_src}" _o)
            if(NOT TARGET ${_o})
                add_library(${_o} OBJECT "${NANO_DIR}/${_src}")
                target_link_libraries(${_o} PRIVATE nano_unit_flags)
            endif()
            # A file shared by programs with different needs gets the include dirs of each.
            if(_deps)
                target_link_libraries(${_o} PRIVATE ${_deps})
            endif()
            list(APPEND _objs "$<TARGET_OBJECTS:${_o}>")
        endforeach()
        add_executable(nano_unit_${_p} "${NANO_DIR}/test/unit/${_p}.cpp" ${_objs})
        target_link_libraries(nano_unit_${_p} PRIVATE nano_unit_flags ${NANO_GTEST} ${_deps})
        add_test(NAME ${_p} COMMAND ${NANO_TEST_LAUNCHER} "$<TARGET_FILE:nano_unit_${_p}>"
                 WORKING_DIRECTORY "${NANO_UNIT_DIR}/run")
        set_tests_properties(${_p} PROPERTIES
            FIXTURES_REQUIRED nano_unit_data
            ENVIRONMENT "NANO_TEST_FIXTURES=${NANO_UNIT_DIR}/data/test/fixtures;NANO_DAIJISHOU_DIR=${NANO_UNIT_DIR}/data/ps3xmb/daijishou;TMPDIR=${NANO_UNIT_DIR}/tmp;ASAN_OPTIONS=detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
        if(NANO_UNIT_ASSETS_${_p} AND NOT NANO_DATA)
            # Needs the base's asset folders (nano_tree.py compose; set NANO_DATA).
            set_tests_properties(${_p} PROPERTIES DISABLED TRUE)
        endif()
        list(APPEND _unit_targets nano_unit_${_p})
    endforeach()
    add_custom_target(nano_unit_tests)
    if(_unit_targets)
        add_dependencies(nano_unit_tests ${_unit_targets})
    endif()
endif()

# --- summary ---------------------------------------------------------------------------------------
set(_built "nano_compat nano_stubs")
foreach(_t nano_core nano_platform_linux nano_shellgate nano_platform_host nano_glstub thimsy-nano
           thimsy-nano-scrape nano-headless nano_unit_tests)
    if(TARGET ${_t})
        string(APPEND _built " ${_t}")
    endif()
endforeach()
message(STATUS "nano: NANO_PLATFORM=${NANO_PLATFORM}; targets:${_built}")
