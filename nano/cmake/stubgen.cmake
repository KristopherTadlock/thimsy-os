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

# The stubs generated at build time from a link's own undefined symbols (docs/linux/IMPLEMENTATION.md
# C.1, decision I29), as nano/test/host/Makefile makes them: nothing generated is committed.
#
#   nano_dropped_stubs(<out var> <name> ARCHIVES <targets...> FROM <files...>)
#       nano/test/host/stubgen/gen.py over `nm -C` of the targets' archives: inert stubs for what
#       the files of FROM (sources.txt's [dropped-linux] and the files a feature switch leaves out)
#       define and the link still lacks. <out var> gets the generated .cpp.
#   nano_glstub_gen(<out var> <name> ARCHIVES <targets...> GLUE <target>)
#       nano/test/host/glstub/gen.py: the GL stub's generic part, every gl*/egl* symbol the archives
#       leave undefined that the glue does not define. <out var> gets the generated .c.
#
# The stubs depend on the whole link, the executable's main included, so they are compiled into
# the executable (thimsy-nano, nano-headless), not into a platform library: a library can't hold
# stubs made from its own final link without a cycle. That is why nano/CMakeLists.txt builds each
# main into a small static library (crt1.o's reference to main pulls it in): nm reads archives,
# which a custom command can name with $<TARGET_FILE> on every CMake from 3.16.

set(NANO_RUN_NM_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/run_nm.cmake")

# The generators are Python 3.8+ with the standard library only; nano/CMakeLists.txt finds the
# interpreter once (Python3_EXECUTABLE), before any of these is called.
macro(_nano_need_python)
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "nano: the stub generators need Python 3.8 or later (Python3_EXECUTABLE)")
    endif()
    if(NOT CMAKE_NM)
        message(FATAL_ERROR "nano: the stub generators need nm (CMAKE_NM is not set)")
    endif()
endmacro()

function(nano_dropped_stubs out name)
    cmake_parse_arguments(S "" "" "ARCHIVES;FROM" ${ARGN})
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/stubgen/${name}")
    set(_cpp "${_dir}/dropped-stubs.cpp")
    file(MAKE_DIRECTORY "${_dir}")
    if(NOT S_FROM)
        file(WRITE "${_cpp}" "// nano: no [dropped-linux] file and no feature left out; nothing to stub.\n")
        set(${out} "${_cpp}" PARENT_SCOPE)
        return()
    endif()
    _nano_need_python()
    set(_files "")
    foreach(_t IN LISTS S_ARCHIVES)
        list(APPEND _files "$<TARGET_FILE:${_t}>")
    endforeach()
    list(JOIN _files "|" _files)
    set(_from_args "")
    set(_from_paths "")
    foreach(_f IN LISTS S_FROM)
        list(APPEND _from_args --from "${NANO_DIR}/${_f}")
        list(APPEND _from_paths "${NANO_DIR}/${_f}")
    endforeach()
    add_custom_command(OUTPUT "${_cpp}"
        COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DOUT=${_dir}/link.nm" "-DINPUTS=${_files}"
                "-DDEMANGLE=ON" -P "${NANO_RUN_NM_SCRIPT}"
        COMMAND "${Python3_EXECUTABLE}" "${NANO_DIR}/test/host/stubgen/gen.py" --nm "${_dir}/link.nm"
                ${_from_args} --decls "${NANO_DIR}" --include-root "${NANO_DIR}" --out "${_cpp}"
        DEPENDS ${S_ARCHIVES} ${_from_paths} "${NANO_DIR}/test/host/stubgen/gen.py" "${NANO_RUN_NM_SCRIPT}"
        COMMENT "nano: [dropped-linux] stubs for ${name}"
        VERBATIM)
    set(${out} "${_cpp}" PARENT_SCOPE)
endfunction()

function(nano_glstub_gen out name)
    cmake_parse_arguments(S "" "GLUE" "ARCHIVES" ${ARGN})
    _nano_need_python()
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/stubgen/${name}")
    set(_c "${_dir}/glstub_gen.c")
    file(MAKE_DIRECTORY "${_dir}")
    set(_files "")
    foreach(_t IN LISTS S_ARCHIVES)
        list(APPEND _files "$<TARGET_FILE:${_t}>")
    endforeach()
    add_custom_command(OUTPUT "${_c}"
        COMMAND "${CMAKE_COMMAND}" -E env "NM=${CMAKE_NM}"
                "${Python3_EXECUTABLE}" "${NANO_DIR}/test/host/glstub/gen.py" --out "${_c}"
                --objects ${_files} --defined-objects "$<TARGET_FILE:${S_GLUE}>"
        DEPENDS ${S_ARCHIVES} ${S_GLUE} "${NANO_DIR}/test/host/glstub/gen.py"
        COMMENT "nano: the GL stub's generic part for ${name}"
        VERBATIM)
    set(${out} "${_c}" PARENT_SCOPE)
endfunction()
