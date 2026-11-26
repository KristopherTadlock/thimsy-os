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

// Thimsy's <android-base/parseint.h> for the Linux and host builds of Nano: libbase's ParseInt and
// ParseUint. Leading white space is skipped; the whole rest must be the number (no trailing text);
// base 16 after a leading "0x" or "0X", else base 10 ("010" is 10, "-0x1" is malformed, as in
// libbase); out of [min, max] fails with errno ERANGE, out of long long's range with ERANGE from
// strtoll, a malformed string with EINVAL. ParseUint refuses a leading '-'.
#pragma once

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <string>
#include <type_traits>

namespace android {
namespace base {

template <typename T>
bool ParseUint(const char* s, T* out, T max = std::numeric_limits<T>::max(),
               bool allow_suffixes = false) {
    static_assert(std::is_unsigned<T>::value, "ParseUint is for unsigned types");
    if (s == nullptr) {
        errno = EINVAL;
        return false;
    }
    while (isspace(static_cast<unsigned char>(*s))) s++;
    if (s[0] == '-') {
        errno = EINVAL;
        return false;
    }
    const int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    errno = 0;
    char* end = nullptr;
    unsigned long long result = strtoull(s, &end, base);
    if (errno != 0) return false;
    if (end == s) {
        errno = EINVAL;
        return false;
    }
    if (*end != '\0') {
        const char* suffixes = "bkmgtpe";
        const char* found = nullptr;
        if (!allow_suffixes || end[1] != '\0' ||
            (found = strchr(suffixes, tolower(static_cast<unsigned char>(*end)))) == nullptr) {
            errno = EINVAL;
            return false;
        }
        const int shift = 10 * static_cast<int>(found - suffixes);
        if (shift > 0 && result > (std::numeric_limits<unsigned long long>::max() >> shift)) {
            errno = ERANGE;
            return false;
        }
        result <<= shift;
    }
    if (max < result) {
        errno = ERANGE;
        return false;
    }
    if (out != nullptr) *out = static_cast<T>(result);
    return true;
}

template <typename T>
bool ParseUint(const std::string& s, T* out, T max = std::numeric_limits<T>::max(),
               bool allow_suffixes = false) {
    return ParseUint(s.c_str(), out, max, allow_suffixes);
}

template <typename T>
bool ParseByteCount(const char* s, T* out, T max = std::numeric_limits<T>::max()) {
    return ParseUint(s, out, max, true);
}

template <typename T>
bool ParseByteCount(const std::string& s, T* out, T max = std::numeric_limits<T>::max()) {
    return ParseByteCount(s.c_str(), out, max);
}

template <typename T>
bool ParseInt(const char* s, T* out, T min = std::numeric_limits<T>::min(),
              T max = std::numeric_limits<T>::max()) {
    static_assert(std::is_signed<T>::value, "ParseInt is for signed types");
    if (s == nullptr) {
        errno = EINVAL;
        return false;
    }
    while (isspace(static_cast<unsigned char>(*s))) s++;
    const int base = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    errno = 0;
    char* end = nullptr;
    long long result = strtoll(s, &end, base);
    if (errno != 0) return false;
    if (end == s || *end != '\0') {
        errno = EINVAL;
        return false;
    }
    if (max < result || min > result) {
        errno = ERANGE;
        return false;
    }
    if (out != nullptr) *out = static_cast<T>(result);
    return true;
}

template <typename T>
bool ParseInt(const std::string& s, T* out, T min = std::numeric_limits<T>::min(),
              T max = std::numeric_limits<T>::max()) {
    return ParseInt(s.c_str(), out, min, max);
}

}  // namespace base
}  // namespace android
