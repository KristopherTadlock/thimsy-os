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

// Inert stub (layer 2, see nanostubs/stub.h) of Android's <system/window.h>: the window struct that
// gui::Surface derives from (so a Surface* converts to ANativeWindow*, as on Android) and the one
// window call Nano makes. No window exists on Linux or the host. C++ only, unlike Android's.
#pragma once

#include <errno.h>

#include <android/native_window.h>
#include <nanostubs/stub.h>

struct ANativeWindow {
    int nanostubReserved = 0;
};

inline int native_window_set_buffers_user_dimensions(struct ANativeWindow* /*window*/, int /*w*/, int /*h*/) {
    NANOSTUB_ONCE();
    return -ENOSYS;
}
