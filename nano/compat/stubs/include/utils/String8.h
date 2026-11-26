/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Inert stub (layer 2, see nanostubs/stub.h) of libutils' <utils/String8.h>: Nano only builds
// names for SurfaceFlinger objects from string literals, so this is a plain UTF-8 holder.
#pragma once

#include <cstddef>
#include <string>

namespace android {

class String8 {
 public:
    String8() = default;
    explicit String8(const char* s) : mStr(s ? s : "") {}
    const char* c_str() const { return mStr.c_str(); }
    size_t size() const { return mStr.size(); }

 private:
    std::string mStr;
};

}  // namespace android
