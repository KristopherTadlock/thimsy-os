# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# thimsy-session for the image build (Buildroot's cmake-package). Written, not run here: the host
# has no CMake; the host build and its tests are the Makefile (make check).
#
# libthimsy comes from the staging directory (Buildroot installs its static library and headers),
# or, with THIMSY_LIBTHIMSY_SOURCE_DIR set, is built from source alongside.
#
# Installs the binary as /usr/libexec/thimsy/session, the path inittab respawns.
cmake_minimum_required(VERSION 3.16)
project(thimsy-session VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THIMSY_SESSION_BUILD_TESTS "Build thimsy-session's tests" OFF)
set(THIMSY_LIBTHIMSY_SOURCE_DIR "" CACHE PATH "libthimsy's source tree (empty: use the installed library)")

find_package(Threads REQUIRED)

if(THIMSY_LIBTHIMSY_SOURCE_DIR)
    add_subdirectory(${THIMSY_LIBTHIMSY_SOURCE_DIR} libthimsy EXCLUDE_FROM_ALL)
    set(_thimsy_lib thimsy)
else()
    find_path(THIMSY_INCLUDE_DIR thimsy/config.h REQUIRED)
    find_library(THIMSY_LIBRARY NAMES libthimsy.a thimsy REQUIRED)
    add_library(thimsy_imported STATIC IMPORTED)
    set_target_properties(thimsy_imported PROPERTIES
        IMPORTED_LOCATION ${THIMSY_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${THIMSY_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES Threads::Threads)
    set(_thimsy_lib thimsy_imported)
endif()

set(_core_sources
    src/core/InputCodes.cpp
    src/core/KeyMachine.cpp
    src/core/SleepPlan.cpp
    src/core/VolumePolicy.cpp
    src/core/Supervisor.cpp
    src/core/Requests.cpp
    src/core/NanoJail.cpp
    src/io/Sysfs.cpp
    src/io/Blkid.cpp
    src/io/Mounter.cpp
    src/io/Proc.cpp
    src/io/Netlink.cpp
    src/io/Evdev.cpp
)

add_library(thimsy_session_core STATIC ${_core_sources})
target_include_directories(thimsy_session_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(thimsy_session_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy_session_core PUBLIC ${_thimsy_lib} Threads::Threads)

add_executable(thimsy-session src/main.cpp src/Session.cpp)
target_compile_options(thimsy-session PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy-session PRIVATE thimsy_session_core)

include(GNUInstallDirs)
install(PROGRAMS $<TARGET_FILE:thimsy-session> DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/thimsy RENAME session)

if(THIMSY_SESSION_BUILD_TESTS)
    enable_testing()
    set(_gtest_dir "${THIMSY_LIBTHIMSY_SOURCE_DIR}/testing")
    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    foreach(_t key_machine_test sleep_plan_test volume_policy_test supervisor_test requests_test nano_jail_test
               blkid_test io_test)
        add_executable(${_t} tests/${_t}.cpp ${_core_sources})
        target_include_directories(${_t} PRIVATE src tests ${_gtest_dir})
        target_compile_options(${_t} PRIVATE -Wall -Wextra -Werror ${_sanitize})
        target_link_options(${_t} PRIVATE ${_sanitize})
        target_link_libraries(${_t} PRIVATE ${_thimsy_lib} Threads::Threads)
        add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
endif()
