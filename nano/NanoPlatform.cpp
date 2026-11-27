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

// install() and get() of NanoPlatform.h (docs/linux/IMPLEMENTATION.md B.6). Shared by every
// platform; createPlatform() is not here but in each platform library.

#define LOG_TAG "NanoPlatform"

#include "NanoPlatform.h"

#include <atomic>

#include <log/log.h>

namespace nanoplat {

namespace {
// Set once by install() and never cleared: the platform outlives every thread (see the header).
std::atomic<Platform*> gPlatform{nullptr};
}  // namespace

Platform::~Platform() = default;

bool Platform::has(Cap cap) const {
    return caps != nullptr && caps->has(cap);
}

void install(std::unique_ptr<Platform> platform) {
    LOG_ALWAYS_FATAL_IF(!platform, "nanoplat::install: null platform");
    Platform* expected = nullptr;
    Platform* raw = platform.get();
    LOG_ALWAYS_FATAL_IF(!gPlatform.compare_exchange_strong(expected, raw, std::memory_order_acq_rel),
                        "nanoplat::install: a platform is already installed");
    // Deliberately never deleted: services may be in use until the process exits.
    (void)platform.release();
}

Platform& get() {
    Platform* p = gPlatform.load(std::memory_order_acquire);
    LOG_ALWAYS_FATAL_IF(p == nullptr, "nanoplat::get: install() was never called");
    return *p;
}

}  // namespace nanoplat
