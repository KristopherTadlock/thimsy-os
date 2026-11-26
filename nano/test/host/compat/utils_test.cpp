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

// utils/SystemClock.h, utils/Timers.h, utils/Errors.h, system/thread_defs.h,
// utils/ThreadDefs.h and private/android_filesystem_config.h.

#include <private/android_filesystem_config.h>
#include <system/thread_defs.h>
#include <utils/Errors.h>
#include <utils/SystemClock.h>
#include <utils/ThreadDefs.h>
#include <utils/Timers.h>

#include <errno.h>
#include <time.h>

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace {

int64_t nowNs(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

TEST(Clocks, UptimeIsMonotonic) {
    const int64_t before = nowNs(CLOCK_MONOTONIC);
    const int64_t up = android::uptimeNanos();
    const int64_t after = nowNs(CLOCK_MONOTONIC);
    EXPECT_GE(up, before);
    EXPECT_LE(up, after);
    const int64_t ms = android::uptimeMillis();
    EXPECT_NEAR(double(ms), double(nowNs(CLOCK_MONOTONIC) / 1000000), 50.0);
    const int64_t a = android::uptimeMillis();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_GE(android::uptimeMillis() - a, 19);
}

TEST(Clocks, ElapsedRealtimeCountsSuspend) {
    // CLOCK_BOOTTIME never runs behind CLOCK_MONOTONIC.
    EXPECT_GE(android::elapsedRealtimeNano(), android::uptimeNanos());
    EXPECT_GE(android::elapsedRealtime(), android::uptimeMillis());
    EXPECT_NEAR(double(android::elapsedRealtime()), double(nowNs(CLOCK_BOOTTIME) / 1000000), 50.0);
}

TEST(Clocks, SystemTime) {
    EXPECT_NEAR(double(systemTime()), double(nowNs(CLOCK_MONOTONIC)), 50e6);
    EXPECT_NEAR(double(systemTime(SYSTEM_TIME_MONOTONIC)), double(nowNs(CLOCK_MONOTONIC)), 50e6);
    EXPECT_NEAR(double(systemTime(SYSTEM_TIME_REALTIME)), double(time(nullptr)) * 1e9, 2e9);
    EXPECT_NEAR(double(systemTime(SYSTEM_TIME_BOOTTIME)), double(nowNs(CLOCK_BOOTTIME)), 50e6);
    EXPECT_GT(systemTime(SYSTEM_TIME_PROCESS), 0);
    EXPECT_GT(systemTime(SYSTEM_TIME_THREAD), 0);
    EXPECT_EQ(systemTime(99), 0);
}

TEST(Clocks, Conversions) {
    static_assert(ms2ns(3) == 3000000, "constexpr");
    EXPECT_EQ(s2ns(2), 2000000000LL);
    EXPECT_EQ(us2ns(5), 5000);
    EXPECT_EQ(ns2s(2500000000LL), 2);
    EXPECT_EQ(ns2ms(2500000), 2);
    EXPECT_EQ(ns2us(2500), 2);
    EXPECT_EQ(seconds_to_nanoseconds(1), 1000000000LL);
    EXPECT_EQ(milliseconds_to_nanoseconds(1), 1000000);
    EXPECT_EQ(nanoseconds_to_milliseconds(1999999), 1);
}

TEST(Clocks, MillisecondTimeoutDelay) {
    EXPECT_EQ(toMillisecondTimeoutDelay(100, 100), 0);
    EXPECT_EQ(toMillisecondTimeoutDelay(100, 50), 0);
    EXPECT_EQ(toMillisecondTimeoutDelay(0, 1), 1);  // rounded up
    EXPECT_EQ(toMillisecondTimeoutDelay(0, 1000000), 1);
    EXPECT_EQ(toMillisecondTimeoutDelay(0, 1000001), 2);
    EXPECT_EQ(toMillisecondTimeoutDelay(0, s2ns(int64_t(1) << 31)), -1);  // too far: wait forever
}

TEST(Errors, AndroidValues) {
    EXPECT_EQ(android::OK, 0);
    EXPECT_EQ(android::NO_ERROR, 0);
    EXPECT_EQ(android::BAD_VALUE, -EINVAL);
    EXPECT_EQ(android::NAME_NOT_FOUND, -ENOENT);
    EXPECT_EQ(android::WOULD_BLOCK, -EWOULDBLOCK);
    EXPECT_EQ(android::INVALID_OPERATION, -ENOSYS);
    EXPECT_EQ(android::TIMED_OUT, -ETIMEDOUT);
    EXPECT_EQ(android::UNKNOWN_ERROR, INT32_MIN);
    EXPECT_EQ(android::statusToString(android::OK), "OK");
    EXPECT_EQ(android::statusToString(android::NAME_NOT_FOUND), "NAME_NOT_FOUND");
    EXPECT_EQ(android::statusToString(android::UNKNOWN_ERROR), "UNKNOWN_ERROR");
    const std::string other = android::statusToString(-EIO);
    EXPECT_NE(other.find("-5"), std::string::npos) << other;
}

TEST(ThreadDefs, AndroidValues) {
    EXPECT_EQ(ANDROID_PRIORITY_NORMAL, 0);
    EXPECT_EQ(ANDROID_PRIORITY_BACKGROUND, 10);
    EXPECT_EQ(ANDROID_PRIORITY_DISPLAY, -4);
    EXPECT_EQ(ANDROID_PRIORITY_URGENT_DISPLAY, -8);
    EXPECT_EQ(ANDROID_PRIORITY_AUDIO, -16);
    EXPECT_EQ(ANDROID_PRIORITY_LOWEST, 19);
    EXPECT_EQ(ANDROID_PRIORITY_HIGHEST, -20);
    EXPECT_EQ(android::PRIORITY_DISPLAY, ANDROID_PRIORITY_DISPLAY);
    EXPECT_EQ(android::PRIORITY_DEFAULT, ANDROID_PRIORITY_NORMAL);
}

TEST(FilesystemConfig, AndroidIds) {
    EXPECT_EQ(AID_ROOT, 0);
    EXPECT_EQ(AID_SYSTEM, 1000);
    EXPECT_EQ(AID_SDCARD_RW, 1015);
    EXPECT_EQ(AID_MEDIA_RW, 1023);
    EXPECT_EQ(AID_EXT_DATA_RW, 1078);
    EXPECT_EQ(AID_EXT_OBB_RW, 1079);
    EXPECT_EQ(AID_SHELL, 2000);
    EXPECT_EQ(AID_EVERYBODY, 9997);
    EXPECT_EQ(AID_APP_START, 10000);
    EXPECT_EQ(AID_USER_OFFSET, 100000);
}
