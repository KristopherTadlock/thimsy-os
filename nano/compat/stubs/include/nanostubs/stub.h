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

// Layer 2 of the platform seam (docs/linux/IMPLEMENTATION.md B.4): inert stand-ins for Android
// APIs that firmware-only code inside shared Nano files calls (binder, SurfaceFlinger, sensors,
// AHardwareBuffer, AAudio, the NDK media codecs, the light and health HALs). They let those files
// compile for the Linux and host builds and do nothing there:
//
//   - every create, open or lookup fails (null, an error status or an empty list), so the caller
//     takes the path it already has for a device without the service;
//   - every other call is a no-op that returns failure or zero;
//   - each stubbed function logs once, at its first call (nanostub_log), so a stub reached at run
//     time is visible in Nano's log without flooding it.
//
// The headers are on nano_core's include path only (after the layer-1 shims in compat/include),
// never on the Android build's. The set only shrinks: tools/nano-tree/stubs-budget.txt holds the
// count of declared names and tools/nano-tree/tests/test_nano_stubs.py fails when it grows.
#pragma once

#ifdef __cplusplus
#include <atomic>
extern "C" {
#endif

// Logs "<what>: not available on this platform (inert stub)" through the compat log (tag
// "nanostub"), at warning priority in a debug build (NDEBUG unset) and debug priority otherwise.
void nanostub_log(const char* what);

#ifdef __cplusplus
}

// The first time control reaches it (per expansion site), logs the enclosing function's name.
#define NANOSTUB_ONCE()                                                                     \
    do {                                                                                     \
        static std::atomic<bool> nanostub_logged_{false};                                     \
        if (!nanostub_logged_.exchange(true, std::memory_order_relaxed))                      \
            ::nanostub_log(__PRETTY_FUNCTION__);                                              \
    } while (0)
#endif
