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

# cmake -DNANO=<nano/> -DOUT=<data dir> [-DNANO_DATA=<composed tree>] "-DASSET_DIRS=a|b" -P unit_data.cmake
#
# Lays out the unit tests' data the way Soong installs `data` and nano/test/host/Makefile's
# UNIT_DATA_SETUP does: test/fixtures from nano/, then each asset folder from NANO_DATA (the base's
# files, linked) overlaid with nano/'s own (copied: android::base::ReadFileToString refuses a
# symbolic link, as on Android). CTest runs it as the nano_unit_data fixture before the programs.
if(NOT NANO OR NOT OUT)
    message(FATAL_ERROR "unit_data.cmake: NANO and OUT are required")
endif()
string(REPLACE "|" ";" _dirs "${ASSET_DIRS}")
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/test")
file(COPY "${NANO}/test/fixtures" DESTINATION "${OUT}/test")
foreach(_d IN LISTS _dirs)
    if(NANO_DATA AND IS_DIRECTORY "${NANO_DATA}/${_d}")
        file(GLOB_RECURSE _files RELATIVE "${NANO_DATA}/${_d}" "${NANO_DATA}/${_d}/*")
        foreach(_f IN LISTS _files)
            get_filename_component(_parent "${OUT}/${_d}/${_f}" DIRECTORY)
            file(MAKE_DIRECTORY "${_parent}")
            file(CREATE_LINK "${NANO_DATA}/${_d}/${_f}" "${OUT}/${_d}/${_f}" SYMBOLIC COPY_ON_ERROR)
        endforeach()
    endif()
    if(IS_DIRECTORY "${NANO}/${_d}")
        file(GLOB_RECURSE _files RELATIVE "${NANO}/${_d}" "${NANO}/${_d}/*")
        foreach(_f IN LISTS _files)
            file(REMOVE "${OUT}/${_d}/${_f}")
            get_filename_component(_parent "${OUT}/${_d}/${_f}" DIRECTORY)
            file(COPY "${NANO}/${_d}/${_f}" DESTINATION "${_parent}")
        endforeach()
    endif()
endforeach()
