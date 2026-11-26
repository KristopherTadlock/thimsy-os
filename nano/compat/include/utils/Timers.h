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

/*
 * Thimsy's <utils/Timers.h> for the Linux and host builds of Nano: libutils' nanosecond time type,
 * conversions and systemTime(), on clock_gettime. There is no fake clock (IMPLEMENTATION.md B.2):
 * Nano also reads steady_clock and clock_gettime directly.
 */
#ifndef NANOCOMPAT_UTILS_TIMERS_H
#define NANOCOMPAT_UTILS_TIMERS_H

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t nsecs_t; /* nano-seconds */

#ifdef __cplusplus
#define NANOCOMPAT_TIMERS_INLINE static constexpr inline
#else
#define NANOCOMPAT_TIMERS_INLINE static inline
#endif

NANOCOMPAT_TIMERS_INLINE nsecs_t seconds_to_nanoseconds(nsecs_t secs) { return secs * 1000000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t milliseconds_to_nanoseconds(nsecs_t secs) { return secs * 1000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t microseconds_to_nanoseconds(nsecs_t secs) { return secs * 1000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t nanoseconds_to_seconds(nsecs_t secs) { return secs / 1000000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t nanoseconds_to_milliseconds(nsecs_t secs) { return secs / 1000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t nanoseconds_to_microseconds(nsecs_t secs) { return secs / 1000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t s2ns(nsecs_t v) { return v * 1000000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t ms2ns(nsecs_t v) { return v * 1000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t us2ns(nsecs_t v) { return v * 1000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t ns2s(nsecs_t v) { return v / 1000000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t ns2ms(nsecs_t v) { return v / 1000000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t ns2us(nsecs_t v) { return v / 1000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t s2ms(nsecs_t v) { return v * 1000; }
NANOCOMPAT_TIMERS_INLINE nsecs_t ms2s(nsecs_t v) { return v / 1000; }

enum {
    SYSTEM_TIME_REALTIME = 0,  /* system-wide realtime clock */
    SYSTEM_TIME_MONOTONIC = 1, /* monotonic time since unspecified starting point */
    SYSTEM_TIME_PROCESS = 2,   /* high-resolution per-process clock */
    SYSTEM_TIME_THREAD = 3,    /* high-resolution per-thread clock */
    SYSTEM_TIME_BOOTTIME = 4,  /* same as SYSTEM_TIME_MONOTONIC, but including CPU suspend time */
};

/* The time of the given clock in nanoseconds; 0 for an unknown clock. */
#ifdef __cplusplus
nsecs_t systemTime(int clock = SYSTEM_TIME_MONOTONIC);
#else
nsecs_t systemTime(int clock);
#endif

/*
 * Milliseconds from referenceTime until timeoutTime, rounded up, for poll()-style timeouts: 0 when
 * it has passed, -1 (wait forever) when it is more than INT_MAX - 1 milliseconds away.
 */
int toMillisecondTimeoutDelay(nsecs_t referenceTime, nsecs_t timeoutTime);

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_UTILS_TIMERS_H */
