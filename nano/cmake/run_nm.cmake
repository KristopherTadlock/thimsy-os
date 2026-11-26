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

# cmake -DNM=<nm> -DOUT=<file> -DINPUTS=<a|b|...> [-DDEMANGLE=ON] -P run_nm.cmake
# Writes nm's listing of the inputs (objects or archives) to OUT, for the stub generators
# (stubgen.cmake). A custom command can't redirect output portably; this script can.
if(NOT NM OR NOT OUT OR NOT INPUTS)
    message(FATAL_ERROR "run_nm.cmake: NM, OUT and INPUTS are required")
endif()
string(REPLACE "|" ";" _inputs "${INPUTS}")
set(_args "")
if(DEMANGLE)
    list(APPEND _args -C)
endif()
execute_process(COMMAND "${NM}" ${_args} ${_inputs}
    OUTPUT_FILE "${OUT}.new" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    file(REMOVE "${OUT}.new")
    message(FATAL_ERROR "run_nm.cmake: ${NM} failed (${_rc})")
endif()
file(RENAME "${OUT}.new" "${OUT}")
