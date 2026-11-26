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

// Thimsy's <utils/SystemClock.h> for the Linux and host builds of Nano: libutils' clocks, on
// clock_gettime. uptime* is CLOCK_MONOTONIC (stops in suspend), elapsedRealtime* is CLOCK_BOOTTIME
// (counts suspend), as on Android.
#pragma once

#include <stdint.h>
#include <sys/time.h>

namespace android {

int64_t uptimeMillis();
int64_t uptimeNanos();
int64_t elapsedRealtime();
int64_t elapsedRealtimeNano();

}  // namespace android
