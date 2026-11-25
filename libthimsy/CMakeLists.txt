# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# libthimsy for the image build (Buildroot's cmake-package) and later the app. Written, not run here:
# the host machine has no CMake; its tests run through the Makefile (make check).
cmake_minimum_required(VERSION 3.16)
project(libthimsy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THIMSY_BUILD_TESTS "Build libthimsy's tests" OFF)
option(THIMSY_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of the stand-in" ON)

find_package(Threads REQUIRED)

add_library(thimsy STATIC
    src/atomic_file.cpp
    src/config.cpp
    src/json.cpp
    src/uevent.cpp
    src/watch.cpp
)
target_include_directories(thimsy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(thimsy PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy PUBLIC Threads::Threads)
set_target_properties(thimsy PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(thimsy-prop tools/thimsy-prop.cpp)
target_compile_options(thimsy-prop PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy-prop PRIVATE thimsy)

include(GNUInstallDirs)
install(TARGETS thimsy-prop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS thimsy EXPORT thimsy-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/thimsy DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(THIMSY_BUILD_TESTS)
    enable_testing()
    set(_gtest_target "")
    if(THIMSY_USE_SYSTEM_GTEST)
        find_package(GTest QUIET)
        if(GTest_FOUND)
            set(_gtest_target GTest::gtest_main)
        endif()
    endif()
    if(NOT _gtest_target)
        # The stand-in is header-only and defines main itself.
        add_library(thimsy_gtest_standin INTERFACE)
        target_include_directories(thimsy_gtest_standin INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/testing)
        set(_gtest_target thimsy_gtest_standin)
    endif()

    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)

    add_executable(gtest_selfcheck_cases tests/gtest_selfcheck_cases.cpp)
    target_include_directories(gtest_selfcheck_cases PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/testing)

    set(_tests atomic_file_test json_test uevent_test watch_test config_test thimsy_prop_test)
    if(_gtest_target STREQUAL "thimsy_gtest_standin")
        # The stand-in's own tests use its internals; they only build against it.
        list(PREPEND _tests gtest_standin_test)
    endif()
    foreach(_t ${_tests})
        add_executable(${_t} tests/${_t}.cpp src/atomic_file.cpp src/config.cpp src/json.cpp src/uevent.cpp
                                src/watch.cpp)
        target_include_directories(${_t} PRIVATE include tests)
        target_compile_options(${_t} PRIVATE -Wall -Wextra -Werror ${_sanitize})
        target_link_options(${_t} PRIVATE ${_sanitize})
        target_link_libraries(${_t} PRIVATE ${_gtest_target} Threads::Threads)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT
            "THIMSY_PROP=$<TARGET_FILE:thimsy-prop>;THIMSY_GTEST_SELFCHECK=$<TARGET_FILE:gtest_selfcheck_cases>")
    endforeach()
endif()
