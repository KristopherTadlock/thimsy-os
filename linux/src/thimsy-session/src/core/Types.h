// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Small types shared by thimsy-session's cores. The cores never read a clock: every call that
// depends on time takes "now" in milliseconds of CLOCK_MONOTONIC (tests pass a fake clock), and
// every core reports its next deadline so the epoll loop knows how long it may sleep.
#pragma once

#include <cstdint>
#include <limits>

namespace thimsy {
namespace session {

using Ms = int64_t;
constexpr Ms kNever = std::numeric_limits<Ms>::max();

inline Ms earliest(Ms a, Ms b) { return a < b ? a : b; }

}  // namespace session
}  // namespace thimsy
