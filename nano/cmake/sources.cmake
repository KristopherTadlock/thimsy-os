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

# Reads nano/build/sources.txt and nano/build/unit-closure.txt (docs/linux/IMPLEMENTATION.md C.1,
# C.2), so the CMake build has no second copy of the lists. The format is the one
# tools/nano-tree/nanotree/sources.py parses and nano/test/host/Makefile's awk reads; the rules here
# are the same (a whole-line comment, a [section] header, one relative path per line), and
# tools/nano-tree/tests/test_nano_cmake.py re-implements this file's logic, with the regular
# expressions below read from this file, and checks it against sources.py on the real list.
#
#   nano_read_sources(<sources.txt> <prefix>)
#       sets <prefix>_<section> for every section of NANO_SOURCE_SECTIONS (dashes become
#       underscores: <prefix>_dropped_linux, <prefix>_test_unit), empty when the section is absent
#       or empty. A bad line is a configure error, as it is for sources.py.
#   nano_sources_require_files(<base dir> <section name> <paths...>)
#       a configure error naming each listed path that does not exist.
#   nano_read_unit_closure(<unit-closure.txt> <prefix>)
#       <prefix>_PROGRAMS, and for each program P: <prefix>_SRCS_<P> (production files),
#       <prefix>_HEADERS_<P> (outside headers) and <prefix>_ASSETS_<P> (asset folders, from @<folder>).

# Lines that are neither blank nor a comment (file(STRINGS) keeps only these).
set(NANO_SOURCES_LINE_REGEX "^[ \t]*[^# \t]")
set(NANO_SOURCES_HEADER_REGEX "^\\[([a-z][a-z0-9-]*)\\]$")
set(NANO_SOURCES_PATH_REGEX "^[A-Za-z0-9_.+@/-]+$")
set(NANO_SOURCES_UP_REGEX "(^|/)\\.\\.(/|$)")
set(NANO_SOURCE_SECTIONS core drm firmware dropped-linux linux host scrape test-unit)
# unit-closure.txt: every line that is not a comment.
set(NANO_CLOSURE_LINE_REGEX "^[^#]")

function(nano_read_sources file prefix)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "nano: ${file} does not exist")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    # Comment lines may hold any text (brackets included); only the other lines reach the list,
    # and those are headers (balanced brackets) or paths (no brackets, no ';'), so the list is safe.
    file(STRINGS "${file}" _lines ENCODING UTF-8 REGEX "${NANO_SOURCES_LINE_REGEX}")
    foreach(_s IN LISTS NANO_SOURCE_SECTIONS)
        string(REPLACE "-" "_" _v "${_s}")
        set(_sec_${_v} "")
        set(_seen_${_v} FALSE)
    endforeach()
    set(_cur "")
    foreach(_raw IN LISTS _lines)
        string(STRIP "${_raw}" _line)
        if(_line MATCHES "${NANO_SOURCES_HEADER_REGEX}")
            set(_name "${CMAKE_MATCH_1}")
            if(NOT _name IN_LIST NANO_SOURCE_SECTIONS)
                message(FATAL_ERROR "nano: ${file}: unknown section [${_name}]")
            endif()
            string(REPLACE "-" "_" _v "${_name}")
            if(_seen_${_v})
                message(FATAL_ERROR "nano: ${file}: section [${_name}] appears twice")
            endif()
            set(_seen_${_v} TRUE)
            set(_cur "${_v}")
            continue()
        endif()
        if(_cur STREQUAL "")
            message(FATAL_ERROR "nano: ${file}: '${_line}' is outside any section")
        endif()
        if(NOT _line MATCHES "${NANO_SOURCES_PATH_REGEX}" OR _line MATCHES "^/"
                OR _line MATCHES "${NANO_SOURCES_UP_REGEX}")
            message(FATAL_ERROR "nano: ${file}: not a relative path: '${_line}'")
        endif()
        if(_line IN_LIST _sec_${_cur})
            message(FATAL_ERROR "nano: ${file}: ${_line} is listed twice in one section")
        endif()
        list(APPEND _sec_${_cur} "${_line}")
    endforeach()
    foreach(_s IN LISTS NANO_SOURCE_SECTIONS)
        string(REPLACE "-" "_" _v "${_s}")
        set(${prefix}_${_v} "${_sec_${_v}}" PARENT_SCOPE)
    endforeach()
endfunction()

function(nano_sources_require_files base section)
    set(_missing "")
    foreach(_p IN LISTS ARGN)
        if(NOT EXISTS "${base}/${_p}")
            list(APPEND _missing "${_p}")
        endif()
    endforeach()
    if(_missing)
        string(REPLACE ";" " " _missing "${_missing}")
        message(FATAL_ERROR "nano: sources.txt [${section}] lists files that do not exist: ${_missing}")
    endif()
endfunction()

function(nano_read_unit_closure file prefix)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "nano: ${file} does not exist")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    file(STRINGS "${file}" _lines ENCODING UTF-8 REGEX "${NANO_CLOSURE_LINE_REGEX}")
    set(_programs "")
    foreach(_line IN LISTS _lines)
        string(REPLACE "\t" ";" _fields "${_line}")
        list(LENGTH _fields _n)
        if(_n LESS 3)
            continue()
        endif()
        list(GET _fields 0 _prog)
        list(GET _fields 1 _srcs)
        list(GET _fields 2 _needs)
        if(_srcs STREQUAL "-")
            set(_srcs "")
        endif()
        if(_needs STREQUAL "-")
            set(_needs "")
        endif()
        string(REPLACE " " ";" _srcs "${_srcs}")
        string(REPLACE " " ";" _needs "${_needs}")
        set(_headers "")
        set(_assets "")
        foreach(_w IN LISTS _needs)
            if(_w STREQUAL "")
                continue()
            elseif(_w MATCHES "^@(.+)$")
                list(APPEND _assets "${CMAKE_MATCH_1}")
            else()
                list(APPEND _headers "${_w}")
            endif()
        endforeach()
        list(REMOVE_ITEM _srcs "")
        list(APPEND _programs "${_prog}")
        set(${prefix}_SRCS_${_prog} "${_srcs}" PARENT_SCOPE)
        set(${prefix}_HEADERS_${_prog} "${_headers}" PARENT_SCOPE)
        set(${prefix}_ASSETS_${_prog} "${_assets}" PARENT_SCOPE)
    endforeach()
    set(${prefix}_PROGRAMS "${_programs}" PARENT_SCOPE)
endfunction()
