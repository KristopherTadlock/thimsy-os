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

// Thimsy's <android-base/strings.h> for the Linux and host builds of Nano: libbase's string
// helpers, header-only.
#pragma once

#include <ctype.h>
#include <strings.h>

#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace android {
namespace base {

// Splits on any of the delimiter characters; empty pieces are kept ("a,,b" is "a", "", "b", and ""
// is one empty piece).
inline std::vector<std::string> Split(const std::string& s, const std::string& delimiters) {
    std::vector<std::string> result;
    size_t base = 0;
    size_t found;
    while (true) {
        found = s.find_first_of(delimiters, base);
        result.push_back(s.substr(base, found - base));
        if (found == s.npos) break;
        base = found + 1;
    }
    return result;
}

// White space (isspace) removed from both ends.
template <typename T>
inline std::string Trim(T&& t) {
    std::string_view sv = t;
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return std::string(sv);
}

template <typename ContainerT, typename SeparatorT>
inline std::string Join(const ContainerT& things, SeparatorT separator) {
    if (things.empty()) return "";
    std::ostringstream result;
    result << *things.begin();
    for (auto it = std::next(things.begin()); it != things.end(); ++it) result << separator << *it;
    return result.str();
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}
inline bool StartsWith(std::string_view s, char prefix) { return !s.empty() && s.front() == prefix; }
inline bool StartsWithIgnoreCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && strncasecmp(s.data(), prefix.data(), prefix.size()) == 0;
}
inline bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
inline bool EndsWith(std::string_view s, char suffix) { return !s.empty() && s.back() == suffix; }
inline bool EndsWithIgnoreCase(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           strncasecmp(s.data() + (s.size() - suffix.size()), suffix.data(), suffix.size()) == 0;
}
inline bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() && strncasecmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

// Removes the prefix (or suffix) and returns true if s has it.
inline bool ConsumePrefix(std::string_view* s, std::string_view prefix) {
    if (!StartsWith(*s, prefix)) return false;
    s->remove_prefix(prefix.size());
    return true;
}
inline bool ConsumeSuffix(std::string_view* s, std::string_view suffix) {
    if (!EndsWith(*s, suffix)) return false;
    s->remove_suffix(suffix.size());
    return true;
}

// Replaces the first (all == false) or every occurrence of `from` (non-empty) with `to`.
inline std::string StringReplace(std::string_view s, std::string_view from, std::string_view to,
                                 bool all) {
    if (from.empty()) return std::string(s);
    std::string result;
    std::string_view::size_type start = 0;
    while (true) {
        const auto pos = s.find(from, start);
        if (pos == std::string_view::npos) break;
        result.append(s.data() + start, pos - start);
        result.append(to.data(), to.size());
        start = pos + from.size();
        if (!all) break;
    }
    result.append(s.data() + start, s.size() - start);
    return result;
}

}  // namespace base
}  // namespace android
