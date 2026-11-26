# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# thimsy-padd for the image build (Buildroot's cmake-package). Written, not run here: the host
# machine has no CMake; the host tests run through the Makefile (make check).
#
# libthimsy comes from, in order: a "thimsy" target already defined by a parent project, the
# source tree at THIMSY_LIBTHIMSY_SOURCE_DIR (default ../../../libthimsy, added as a subdirectory),
# or an installed libthimsy.a and its headers (Buildroot's staging directory).
cmake_minimum_required(VERSION 3.18)  # find_library(REQUIRED)
project(thimsy-padd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THIMSY_PADD_BUILD_TESTS "Build thimsy-padd's tests" OFF)
option(THIMSY_PADD_INSTALL_REPLAY "Install padd-replay (a debugging tool) too" OFF)
set(THIMSY_LIBTHIMSY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libthimsy"
    CACHE PATH "libthimsy's source tree, if building it here")

find_package(Threads REQUIRED)

if(NOT TARGET thimsy)
    if(EXISTS "${THIMSY_LIBTHIMSY_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${THIMSY_LIBTHIMSY_SOURCE_DIR}" libthimsy EXCLUDE_FROM_ALL)
    else()
        find_library(THIMSY_LIBRARY NAMES thimsy REQUIRED)
        find_path(THIMSY_INCLUDE_DIR thimsy/config.h REQUIRED)
        add_library(thimsy STATIC IMPORTED)
        set_target_properties(thimsy PROPERTIES
            IMPORTED_LOCATION "${THIMSY_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${THIMSY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES Threads::Threads)
    endif()
endif()

set(_warn -Wall -Wextra -Werror)

add_library(padd_core STATIC
    src/core/InputCodes.cpp
    src/core/PadSpec.cpp
    src/core/Calibrate.cpp
    src/core/Merger.cpp
    src/core/Rumble.cpp
    src/core/Replay.cpp
    src/io/EvdevSource.cpp
    src/io/UinputSink.cpp
    src/io/Control.cpp
)
target_include_directories(padd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(padd_core PRIVATE ${_warn})
target_link_libraries(padd_core PUBLIC thimsy Threads::Threads)

add_executable(thimsy-padd src/main.cpp)
target_compile_options(thimsy-padd PRIVATE ${_warn})
target_link_libraries(thimsy-padd PRIVATE padd_core)

add_executable(padd-replay tools/padd-replay.cpp)
target_compile_options(padd-replay PRIVATE ${_warn})
target_link_libraries(padd-replay PRIVATE padd_core)

include(GNUInstallDirs)
# /usr/bin/thimsy-padd: S02padd starts it (lib.sh's PADD).
install(TARGETS thimsy-padd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(THIMSY_PADD_INSTALL_REPLAY)
    install(TARGETS padd-replay RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(THIMSY_PADD_BUILD_TESTS)
    enable_testing()
    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    set(_tests input_codes_test calibrate_test padspec_test merger_test rumble_test replay_test io_test)
    foreach(_t ${_tests})
        add_executable(${_t} tests/${_t}.cpp)
        target_include_directories(${_t} PRIVATE tests "${THIMSY_LIBTHIMSY_SOURCE_DIR}/testing")
        target_compile_options(${_t} PRIVATE ${_warn} ${_sanitize})
        target_link_options(${_t} PRIVATE ${_sanitize})
        target_link_libraries(${_t} PRIVATE padd_core)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT
            "THIMSY_PADD_TRACES=${CMAKE_CURRENT_SOURCE_DIR}/tests/traces")
    endforeach()
endif()
