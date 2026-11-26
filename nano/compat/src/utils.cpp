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

// libutils' clocks (utils/SystemClock.h, utils/Timers.h) and status strings (utils/Errors.h) for the
// Linux and host builds.

#include <utils/Errors.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>

#include <limits.h>
#include <string.h>
#include <time.h>

#include <string>

namespace {

int64_t clockNanos(clockid_t id) {
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

extern "C" nsecs_t systemTime(int clock) {
    switch (clock) {
        case SYSTEM_TIME_REALTIME:
            return clockNanos(CLOCK_REALTIME);
        case SYSTEM_TIME_MONOTONIC:
            return clockNanos(CLOCK_MONOTONIC);
        case SYSTEM_TIME_PROCESS:
            return clockNanos(CLOCK_PROCESS_CPUTIME_ID);
        case SYSTEM_TIME_THREAD:
            return clockNanos(CLOCK_THREAD_CPUTIME_ID);
        case SYSTEM_TIME_BOOTTIME:
            return clockNanos(CLOCK_BOOTTIME);
        default:
            return 0;
    }
}

extern "C" int toMillisecondTimeoutDelay(nsecs_t referenceTime, nsecs_t timeoutTime) {
    if (timeoutTime <= referenceTime) return 0;
    const uint64_t delay = static_cast<uint64_t>(timeoutTime - referenceTime);
    if (delay > static_cast<uint64_t>((INT_MAX - 1) * 1000000LL)) return -1;
    return static_cast<int>((delay + 999999ULL) / 1000000ULL);
}

namespace android {

int64_t uptimeMillis() { return nanoseconds_to_milliseconds(uptimeNanos()); }

int64_t uptimeNanos() { return clockNanos(CLOCK_MONOTONIC); }

int64_t elapsedRealtime() { return nanoseconds_to_milliseconds(elapsedRealtimeNano()); }

int64_t elapsedRealtimeNano() { return clockNanos(CLOCK_BOOTTIME); }

std::string statusToString(status_t s) {
#define NANOCOMPAT_STATUS_CASE(x) \
    case x:                       \
        return #x;
    switch (s) {
        NANOCOMPAT_STATUS_CASE(OK)
        NANOCOMPAT_STATUS_CASE(UNKNOWN_ERROR)
        NANOCOMPAT_STATUS_CASE(NO_MEMORY)
        NANOCOMPAT_STATUS_CASE(INVALID_OPERATION)
        NANOCOMPAT_STATUS_CASE(BAD_VALUE)
        NANOCOMPAT_STATUS_CASE(BAD_TYPE)
        NANOCOMPAT_STATUS_CASE(NAME_NOT_FOUND)
        NANOCOMPAT_STATUS_CASE(PERMISSION_DENIED)
        NANOCOMPAT_STATUS_CASE(NO_INIT)
        NANOCOMPAT_STATUS_CASE(ALREADY_EXISTS)
        NANOCOMPAT_STATUS_CASE(DEAD_OBJECT)
        NANOCOMPAT_STATUS_CASE(FAILED_TRANSACTION)
        NANOCOMPAT_STATUS_CASE(BAD_INDEX)
        NANOCOMPAT_STATUS_CASE(NOT_ENOUGH_DATA)
        NANOCOMPAT_STATUS_CASE(WOULD_BLOCK)
        NANOCOMPAT_STATUS_CASE(TIMED_OUT)
        NANOCOMPAT_STATUS_CASE(UNKNOWN_TRANSACTION)
        NANOCOMPAT_STATUS_CASE(FDS_NOT_ALLOWED)
        NANOCOMPAT_STATUS_CASE(UNEXPECTED_NULL)
        default:
            break;
    }
#undef NANOCOMPAT_STATUS_CASE
    std::string out = std::to_string(s);
    if (s < 0 && s > -4096) {
        out += " (";
        out += strerror(-s);
        out += ")";
    }
    return out;
}

}  // namespace android
