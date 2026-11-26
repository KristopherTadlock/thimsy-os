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

# Reads string lists out of nano/Android.bp at configure time (docs/linux/IMPLEMENTATION.md C.2), so
# the flags of gammaos-nano and the vendored libraries' file lists are written once, in Android.bp.
#
#   nano_bp_list(<Android.bp> <module name> <property path> <out var>)
#       the strings of the list property, in order: "cflags" (a module's top-level cflags),
#       "arch.arm64.cflags" (nested), "srcs", "local_include_dirs". A property that is absent gives
#       an empty list; a module that is absent is a configure error.
#
# This is not a Blueprint parser. It reads the layout bpfmt writes, which every Android.bp keeps
# (Soong's presubmit runs bpfmt): a module opens with "<type> {" in column 0 and closes with "}" in
# column 0; a property is one line per nesting level of four spaces; a list either fits on its line
# ("cflags: ["-Wall", "-Werror"],") or has one entry per line and closes with "]," at the list's
# indent; "//" starts a comment outside a string. tools/nano-tree/tests/test_nano_cmake.py runs the
# same algorithm, with the regular expressions read from this file, against
# tools/nano-tree/nanotree/androidbp.py's real parser on nano/Android.bp, so a layout this reader
# would misread fails that test before it reaches a build.
#
# To survive CMake's list splitting, the text's ';', '\', '[' and ']' become <SEMI>, <BS>, <LB>
# and <RB> before it is cut into lines; a value that holds <SEMI> is refused.

set(NANO_BP_RE_MODULE_START "^([A-Za-z_][A-Za-z0-9_]*) \\{$")
set(NANO_BP_RE_MODULE_END "^\\}$")
set(NANO_BP_RE_NAME "^    name: \"([^\"]*)\",?$")
set(NANO_BP_RE_MAP_START "^( *)([A-Za-z_][A-Za-z0-9_]*): \\{$")
set(NANO_BP_RE_MAP_END "^( *)\\},?$")
set(NANO_BP_RE_LIST_START "^( *)([A-Za-z_][A-Za-z0-9_]*): <LB>(.*)$")
set(NANO_BP_RE_LIST_END "^( *)<RB>,?$")
set(NANO_BP_RE_LIST_INLINE_END "<RB>,?$")
set(NANO_BP_RE_CODE "^(([^\"/]|\"[^\"]*\"|/[^/\"])*)")
set(NANO_BP_RE_STRING "\"[^\"]*\"")

function(_nano_bp_lines file out)
    file(READ "${file}" _text)
    string(REPLACE ";" "<SEMI>" _text "${_text}")
    string(REPLACE "\\" "<BS>" _text "${_text}")
    string(REPLACE "[" "<LB>" _text "${_text}")
    string(REPLACE "]" "<RB>" _text "${_text}")
    string(REPLACE "\r" "" _text "${_text}")
    string(REPLACE "\n" ";" _text "${_text}")
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()

# The strings of one line's code part (the part before a "//" comment).
function(_nano_bp_strings line out)
    set(_vals "")
    if(line MATCHES "${NANO_BP_RE_CODE}")
        set(_code "${CMAKE_MATCH_1}")
        string(REGEX MATCHALL "${NANO_BP_RE_STRING}" _quoted "${_code}")
        foreach(_q IN LISTS _quoted)
            string(LENGTH "${_q}" _len)
            math(EXPR _len "${_len} - 2")
            string(SUBSTRING "${_q}" 1 ${_len} _v)
            if(_v MATCHES "<SEMI>")
                message(FATAL_ERROR "nano: Android.bp: a value holds ';', which nano_bp_list can't return: ${_v}")
            endif()
            string(REPLACE "<BS>" "\\" _v "${_v}")
            string(REPLACE "<LB>" "[" _v "${_v}")
            string(REPLACE "<RB>" "]" _v "${_v}")
            list(APPEND _vals "${_v}")
        endforeach()
    endif()
    set(${out} "${_vals}" PARENT_SCOPE)
endfunction()

function(nano_bp_list file module path out)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    _nano_bp_lines("${file}" _lines)
    set(_in_module FALSE)
    set(_module_lines "")
    set(_found FALSE)
    set(_name "")
    foreach(_line IN LISTS _lines)
        if(NOT _in_module)
            if(_line MATCHES "${NANO_BP_RE_MODULE_START}")
                set(_in_module TRUE)
                set(_module_lines "")
                set(_name "")
            endif()
            continue()
        endif()
        if(_line MATCHES "${NANO_BP_RE_MODULE_END}")
            set(_in_module FALSE)
            if(_name STREQUAL module)
                set(_found TRUE)
                break()
            endif()
            continue()
        endif()
        if(_name STREQUAL "" AND _line MATCHES "${NANO_BP_RE_NAME}")
            set(_name "${CMAKE_MATCH_1}")
        endif()
        list(APPEND _module_lines "${_line}")
    endforeach()
    if(NOT _found)
        message(FATAL_ERROR "nano: ${file} has no module named ${module}")
    endif()

    # Walk the module's lines with a stack of the maps they are in. _mode is "" (between
    # properties), "want" (inside the list asked for) or "skip" (inside another multi-line list).
    set(_stack "")
    set(_mode "")
    set(_list_indent "")
    set(_result "")
    foreach(_line IN LISTS _module_lines)
        if(NOT _mode STREQUAL "")
            set(_closes FALSE)
            if(_line MATCHES "${NANO_BP_RE_LIST_END}")
                if("${CMAKE_MATCH_1}" STREQUAL "${_list_indent}")
                    set(_closes TRUE)
                endif()
            endif()
            if(_closes)
                if(_mode STREQUAL "want")
                    break()
                endif()
                set(_mode "")
            elseif(_mode STREQUAL "want")
                _nano_bp_strings("${_line}" _vals)
                list(APPEND _result ${_vals})
            endif()
            continue()
        endif()
        if(_line MATCHES "${NANO_BP_RE_MAP_START}")
            list(APPEND _stack "${CMAKE_MATCH_2}")
        elseif(_line MATCHES "${NANO_BP_RE_MAP_END}")
            list(LENGTH _stack _depth)
            if(_depth GREATER 0)
                list(REMOVE_AT _stack -1)
            endif()
        elseif(_line MATCHES "${NANO_BP_RE_LIST_START}")
            set(_indent "${CMAKE_MATCH_1}")
            set(_rest "${CMAKE_MATCH_3}")
            set(_keypath ${_stack} "${CMAKE_MATCH_2}")
            list(JOIN _keypath "." _keypath)
            if(_keypath STREQUAL path)
                _nano_bp_strings("${_rest}" _vals)
                list(APPEND _result ${_vals})
                if(_rest MATCHES "${NANO_BP_RE_LIST_INLINE_END}")
                    break()
                endif()
                set(_mode "want")
            elseif(NOT _rest MATCHES "${NANO_BP_RE_LIST_INLINE_END}")
                set(_mode "skip")
            endif()
            set(_list_indent "${_indent}")
        endif()
    endforeach()
    set(${out} "${_result}" PARENT_SCOPE)
endfunction()
