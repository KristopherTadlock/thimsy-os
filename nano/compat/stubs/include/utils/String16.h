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

// Inert stub (layer 2, see nanostubs/stub.h) of libutils' <utils/String16.h>: Nano only names
// binder services with it (IServiceManager::checkService), so it keeps the UTF-8 text it was made
// from.
#pragma once

#include <cstddef>
#include <string>

namespace android {

class String16 {
 public:
    String16() = default;
    explicit String16(const char* s) : mStr(s ? s : "") {}
    size_t size() const { return mStr.size(); }

 private:
    std::string mStr;
};

}  // namespace android
