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

# The feature switches of docs/linux/IMPLEMENTATION.md C.2, and the [core] files each one leaves out
# of nano_core when it is OFF. A file left out is not moved out of [core] (sources.txt partitions
# Android.bp, and Android builds it): the Linux and host links get generated inert stubs for what it
# defines, exactly as for [dropped-linux] (stubgen.cmake), so the menu entries behind it fail at
# run time and nothing else changes. tools/nano-tree/tests/test_nano_cmake.py checks, from
# tools/nano-tree/include-census.json, that every [core] or [drm] file reaching a library the
# CMake build has no package for is listed here, and that every file listed is in [core].
#
#   NANO_WITH_AC3      liba52 (GPL-2.0, vendored): the AC-3 decoder for .ts, MKV and VOB audio.
#                      OFF on Linux, so no GPL code is linked (plan 3.3, the recipe's licence).
#                      ON builds liba52/liba52/*.c from Android.bp's liba52_nano.
#   NANO_WITH_PDF      pdfium, for game manuals. No package yet; ON needs an installed pdfium.
#   NANO_WITH_ARCHIVE  libziparchive and the LZMA SDK: the File Explorer's zip and 7z browsing
#                      (NanoArchive.cpp) and the OTA package check (NanoOtaCheck.cpp, Android's
#                      update flow). Both are Android libraries; C.3 leaves a portable zip back end
#                      for later. ON needs them installed.
#   NANO_WITH_MEDIA    a real media back end for AMediaCodec, AMediaExtractor and AAudio. None
#                      exists: the media, music, video and radio files compile against the layer-2
#                      stubs (B.4), which fail at run time. It drops no file; ON is refused.

set(NANO_FEATURES AC3 PDF ARCHIVE MEDIA)
set(NANO_FEATURE_FILES_AC3 NanoAc3.cpp)
set(NANO_FEATURE_FILES_PDF NanoMenuPdf.cpp)
set(NANO_FEATURE_FILES_ARCHIVE NanoArchive.cpp NanoOtaCheck.cpp)
set(NANO_FEATURE_FILES_MEDIA "")
