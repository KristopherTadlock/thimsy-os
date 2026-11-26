# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# thimsy-padd for the image build (Buildroot's cmake-package). Written, not run here: the host
# machine has no CMake; the host tests run through the Makefile (make check).
#
# libthimsy comes from, in order: a thimsy::thimsy target a parent project defines; the source tree
# at THIMSY_LIBTHIMSY_SOURCE_DIR when it is set; the installed package (find_package(thimsy):
# Buildroot's staging directory, where libthimsy's recipe installs it); the repository's
# ../../../libthimsy when this is a checkout (Buildroot copies only this directory, and finds the
# staged package first).
cmake_minimum_required(VERSION 3.16)
project(thimsy-padd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THIMSY_BUILD_TESTS "Build thimsy-padd's unit tests (CTest)" OFF)
option(THIMSY_PADD_INSTALL_REPLAY "Install padd-replay (a debugging tool) too" OFF)
set(THIMSY_LIBTHIMSY_SOURCE_DIR "" CACHE PATH "libthimsy's source tree (empty: the installed package)")

find_package(Threads REQUIRED)

if(NOT TARGET thimsy::thimsy)
    if(THIMSY_LIBTHIMSY_SOURCE_DIR)
        add_subdirectory("${THIMSY_LIBTHIMSY_SOURCE_DIR}" libthimsy EXCLUDE_FROM_ALL)
    else()
        find_package(thimsy CONFIG QUIET)
        if(NOT thimsy_FOUND)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../libthimsy/CMakeLists.txt")
                add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../libthimsy" libthimsy EXCLUDE_FROM_ALL)
            else()
                message(FATAL_ERROR "thimsy-padd: libthimsy not found: install it (find_package(thimsy)) "
                                    "or set THIMSY_LIBTHIMSY_SOURCE_DIR")
            endif()
        endif()
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
target_link_libraries(padd_core PUBLIC thimsy::thimsy Threads::Threads)

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

if(THIMSY_BUILD_TESTS)
    enable_testing()
    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    set(_tests input_codes_test calibrate_test padspec_test merger_test rumble_test replay_test io_test)
    foreach(_t ${_tests})
        add_executable(${_t} tests/${_t}.cpp)
        target_include_directories(${_t} PRIVATE tests)
        target_compile_options(${_t} PRIVATE ${_warn} ${_sanitize})
        target_link_options(${_t} PRIVATE ${_sanitize})
        # libthimsy's GoogleTest stand-in (the Makefile's -I libthimsy/testing).
        target_link_libraries(${_t} PRIVATE padd_core thimsy::gtest_standin)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT
            "THIMSY_PADD_TRACES=${CMAKE_CURRENT_SOURCE_DIR}/tests/traces")
    endforeach()
endif()
