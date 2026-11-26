# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Thimsy OS contributors
#
# libthimsy for the image build (Buildroot's cmake-package) and later the app. Written, not run here:
# the host machine has no CMake; its tests run through the Makefile (make check).
#
# Installed, it exports a package, so a consumer does
#     find_package(thimsy CONFIG REQUIRED)
#     target_link_libraries(x PRIVATE thimsy::thimsy)
# (Buildroot: staging's /usr/lib/cmake/thimsy). The targets:
#     thimsy::thimsy          the static library (the store, atomic file, watcher, uevent, JSON)
#     thimsy::gtest_standin   the header-only GoogleTest stand-in, which defines main
#                             (installed as include/thimsy/testing/gtest/gtest.h, so it never
#                             shadows a real <gtest/gtest.h> on the default include path)
#     thimsy::thimsy-prop     the tool (an imported executable, for tests that run it)
# Added with add_subdirectory() from a parent project, the same names exist as aliases, and nothing
# is installed unless the parent asks (THIMSY_INSTALL).
cmake_minimum_required(VERSION 3.16)
project(libthimsy VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_thimsy_top_level ON)
else()
    set(_thimsy_top_level OFF)
endif()

option(THIMSY_BUILD_TESTS "Build libthimsy's tests" OFF)
option(THIMSY_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of the stand-in" ON)
option(THIMSY_INSTALL "Install libthimsy, thimsy-prop and the package files" ${_thimsy_top_level})

if(_thimsy_top_level)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(Threads REQUIRED)

set(THIMSY_SOURCES
    src/atomic_file.cpp
    src/config.cpp
    src/json.cpp
    src/uevent.cpp
    src/watch.cpp
)

add_library(thimsy STATIC ${THIMSY_SOURCES})
add_library(thimsy::thimsy ALIAS thimsy)
target_include_directories(thimsy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# A parent project may not set the standard; the library needs C++17 either way.
target_compile_features(thimsy PUBLIC cxx_std_17)
target_compile_options(thimsy PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy PUBLIC Threads::Threads)
set_target_properties(thimsy PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME thimsy)

# The GoogleTest stand-in (header-only; it defines main). Nano's, thimsy-session's and
# thimsy-padd's tests use it when no GoogleTest is installed.
add_library(thimsy_gtest_standin INTERFACE)
add_library(thimsy::gtest_standin ALIAS thimsy_gtest_standin)
target_include_directories(thimsy_gtest_standin INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/testing>
    $<INSTALL_INTERFACE:include/thimsy/testing>
)
set_target_properties(thimsy_gtest_standin PROPERTIES EXPORT_NAME gtest_standin)

add_executable(thimsy-prop tools/thimsy-prop.cpp)
add_executable(thimsy::thimsy-prop ALIAS thimsy-prop)
target_compile_options(thimsy-prop PRIVATE -Wall -Wextra -Werror)
target_link_libraries(thimsy-prop PRIVATE thimsy)

if(THIMSY_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(THIMSY_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/thimsy)

    install(TARGETS thimsy-prop EXPORT thimsy-targets RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(TARGETS thimsy thimsy_gtest_standin EXPORT thimsy-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/thimsy DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES testing/gtest/gtest.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thimsy/testing/gtest)

    install(EXPORT thimsy-targets NAMESPACE thimsy:: DESTINATION ${THIMSY_CMAKE_DIR})
    configure_package_config_file(cmake/thimsy-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/thimsy-config.cmake
        INSTALL_DESTINATION ${THIMSY_CMAKE_DIR})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/thimsy-config-version.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/thimsy-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/thimsy-config-version.cmake
        DESTINATION ${THIMSY_CMAKE_DIR})
endif()

if(THIMSY_BUILD_TESTS)
    enable_testing()
    set(_gtest_target "")
    if(THIMSY_USE_SYSTEM_GTEST)
        find_package(GTest QUIET)
        if(TARGET GTest::gtest_main)
            set(_gtest_target GTest::gtest_main)
        elseif(TARGET GTest::Main)
            set(_gtest_target GTest::Main)
        endif()
    endif()
    if(NOT _gtest_target)
        set(_gtest_target thimsy_gtest_standin)
    endif()

    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)

    add_executable(gtest_selfcheck_cases tests/gtest_selfcheck_cases.cpp)
    target_link_libraries(gtest_selfcheck_cases PRIVATE thimsy_gtest_standin)

    set(_tests atomic_file_test json_test uevent_test watch_test config_test thimsy_prop_test)
    if(_gtest_target STREQUAL "thimsy_gtest_standin")
        # The stand-in's own tests use its internals; they only build against it.
        list(PREPEND _tests gtest_standin_test)
    endif()
    foreach(_t ${_tests})
        add_executable(${_t} tests/${_t}.cpp ${THIMSY_SOURCES})
        target_include_directories(${_t} PRIVATE include tests)
        target_compile_options(${_t} PRIVATE -Wall -Wextra -Werror ${_sanitize})
        target_link_options(${_t} PRIVATE ${_sanitize})
        target_link_libraries(${_t} PRIVATE ${_gtest_target} Threads::Threads)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT
            "THIMSY_PROP=$<TARGET_FILE:thimsy-prop>;THIMSY_GTEST_SELFCHECK=$<TARGET_FILE:gtest_selfcheck_cases>")
    endforeach()
endif()
