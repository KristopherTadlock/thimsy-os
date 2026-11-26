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

// Thimsy's <android-base/parsebool.h> for the Linux and host builds of Nano: libbase's ParseBool.
// "1", "y", "yes", "on", "true" are kTrue; "0", "n", "no", "off", "false" are kFalse (exact case);
// anything else is kError.
#pragma once

#include <string_view>

namespace android {
namespace base {

enum class ParseBoolResult {
    kError,
    kFalse,
    kTrue,
};

inline ParseBoolResult ParseBool(std::string_view s) {
    if (s == "1" || s == "y" || s == "yes" || s == "on" || s == "true") return ParseBoolResult::kTrue;
    if (s == "0" || s == "n" || s == "no" || s == "off" || s == "false") return ParseBoolResult::kFalse;
    return ParseBoolResult::kError;
}

}  // namespace base
}  // namespace android
