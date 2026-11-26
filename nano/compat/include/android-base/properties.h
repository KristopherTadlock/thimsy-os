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

// Thimsy's <android-base/properties.h> for the Linux and host builds of Nano: libbase's property
// calls on the process-wide config store (nanocompat/nanocompat.h, IMPLEMENTATION.md B.2).
//
//   GetProperty         the value, or the default when the key is unset or empty
//   GetBoolProperty     ParseBool of the value (parsebool.h), else the default
//   GetIntProperty      ParseInt of the value within [min, max] (parseint.h: no trailing text,
//                       unlike libcutils' property_get_int32), else the default
//   GetUintProperty     ParseUint of the value up to max, else the default
//   SetProperty         property_set's rules (91-byte values except ro.*); false when refused
//   WaitForProperty     waits until the key has the value (or the timeout passes)
//   WaitForPropertyCreation  waits until the key has any value
#pragma once

#include <chrono>
#include <limits>
#include <string>

#include <android-base/parsebool.h>
#include <android-base/parseint.h>

namespace android {
namespace base {

std::string GetProperty(const std::string& key, const std::string& default_value);

bool GetBoolProperty(const std::string& key, bool default_value);

template <typename T>
T GetIntProperty(const std::string& key, T default_value, T min = std::numeric_limits<T>::min(),
                 T max = std::numeric_limits<T>::max()) {
    T result;
    const std::string value = GetProperty(key, "");
    if (!value.empty() && ParseInt(value, &result, min, max)) return result;
    return default_value;
}

template <typename T>
T GetUintProperty(const std::string& key, T default_value, T max = std::numeric_limits<T>::max()) {
    T result;
    const std::string value = GetProperty(key, "");
    if (!value.empty() && ParseUint(value, &result, max)) return result;
    return default_value;
}

bool SetProperty(const std::string& key, const std::string& value);

bool WaitForProperty(const std::string& key, const std::string& expected_value,
                     std::chrono::milliseconds relative_timeout = std::chrono::milliseconds::max());

bool WaitForPropertyCreation(const std::string& key, std::chrono::milliseconds relative_timeout =
                                                         std::chrono::milliseconds::max());

}  // namespace base
}  // namespace android
