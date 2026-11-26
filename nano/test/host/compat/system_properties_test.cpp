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

// sys/system_properties.h: find, serials and waits, as NanoMenu's PropEdge and its wake thread use
// them; android::base::WaitForProperty.

#include <sys/system_properties.h>

#include <android-base/properties.h>
#include <cutils/properties.h>

#include <string.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

#include <thimsy/config.h>

#include "compat_test_util.h"

using compat_test::StoreTest;
using Clock = std::chrono::steady_clock;

namespace {

int64_t msSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

struct timespec ms(long n) {
    struct timespec ts;
    ts.tv_sec = n / 1000;
    ts.tv_nsec = (n % 1000) * 1000000L;
    return ts;
}

}  // namespace

class SysProps : public StoreTest {};

TEST_F(SysProps, FindNeedsAValue) {
    EXPECT_TRUE(__system_property_find("sys.test.none") == nullptr);
    EXPECT_TRUE(__system_property_find(nullptr) == nullptr);
    EXPECT_TRUE(__system_property_find("bad key") == nullptr);
    ASSERT_EQ(__system_property_set("sys.test.f", "1"), 0);
    const prop_info* pi = __system_property_find("sys.test.f");
    ASSERT_TRUE(pi != nullptr);
    EXPECT_EQ(__system_property_find("sys.test.f"), pi);  // one handle per key
    // A cleared key keeps existing, as on Android.
    ASSERT_EQ(__system_property_set("sys.test.f", ""), 0);
    EXPECT_EQ(__system_property_find("sys.test.f"), pi);
}

TEST_F(SysProps, FindSeesAnotherWriter) {
    auto other = thimsy::ConfigStore::open({dir_ / "persist.props", dir_ / "run"});
    ASSERT_TRUE(other != nullptr);
    ASSERT_TRUE(other->set("sys.test.elsewhere", "1"));
    EXPECT_TRUE(__system_property_find("sys.test.elsewhere") != nullptr);
}

TEST_F(SysProps, GetCutsTo91Bytes) {
    char buf[PROP_VALUE_MAX];
    EXPECT_EQ(__system_property_get("sys.test.none", buf), 0);
    EXPECT_STREQ(buf, "");
    ASSERT_EQ(__system_property_set("ro.test.g", std::string(150, 'g').c_str()), 0);
    EXPECT_EQ(__system_property_get("ro.test.g", buf), 91);
    EXPECT_EQ(strlen(buf), 91u);
}

TEST_F(SysProps, ReadCallbackGivesTheWholeValue) {
    ASSERT_EQ(__system_property_set("ro.test.cb", std::string(150, 'c').c_str()), 0);
    const prop_info* pi = __system_property_find("ro.test.cb");
    ASSERT_TRUE(pi != nullptr);
    struct Seen {
        std::string name, value;
        uint32_t serial = 0;
    } seen;
    __system_property_read_callback(
        pi,
        [](void* cookie, const char* name, const char* value, uint32_t serial) {
            Seen* s = static_cast<Seen*>(cookie);
            s->name = name;
            s->value = value;
            s->serial = serial;
        },
        &seen);
    EXPECT_EQ(seen.name, "ro.test.cb");
    EXPECT_EQ(seen.value.size(), 150u);
    EXPECT_EQ(seen.serial, __system_property_serial(pi));
}

TEST_F(SysProps, ForeachVisitsEveryKey) {
    ASSERT_EQ(__system_property_set("sys.test.e1", "1"), 0);
    ASSERT_EQ(__system_property_set("persist.test.e2", "2"), 0);
    std::map<std::string, int> seen;
    __system_property_foreach(
        [](const prop_info* pi, void* cookie) {
            __system_property_read_callback(
                pi,
                [](void* c, const char* name, const char*, uint32_t) {
                    (*static_cast<std::map<std::string, int>*>(c))[name]++;
                },
                cookie);
        },
        &seen);
    EXPECT_EQ(seen["sys.test.e1"], 1);
    EXPECT_EQ(seen["persist.test.e2"], 1);
}

TEST_F(SysProps, SerialMovesOnEverySet) {
    ASSERT_EQ(__system_property_set("sys.test.s", "a"), 0);
    const prop_info* pi = __system_property_find("sys.test.s");
    ASSERT_TRUE(pi != nullptr);
    const uint32_t s1 = __system_property_serial(pi);
    EXPECT_NE(s1, 0u);
    EXPECT_EQ(__system_property_serial(pi), s1);  // stable while nothing happens
    ASSERT_EQ(__system_property_set("sys.test.s", "b"), 0);
    const uint32_t s2 = __system_property_serial(pi);
    EXPECT_NE(s2, s1);
    ASSERT_EQ(__system_property_set("sys.test.s", "b"), 0);  // the same value moves it too
    EXPECT_NE(__system_property_serial(pi), s2);
}

TEST_F(SysProps, FirstWaitFromZeroReturnsAtOnce) {
    // NanoMenu's PropEdge starts with serial 0 and relies on the first wait returning the live one.
    ASSERT_EQ(__system_property_set("sys.test.w0", "1"), 0);
    const prop_info* pi = __system_property_find("sys.test.w0");
    ASSERT_TRUE(pi != nullptr);
    uint32_t serial = 0;
    struct timespec to = ms(5000);
    const auto t0 = Clock::now();
    EXPECT_TRUE(__system_property_wait(pi, 0, &serial, &to));
    EXPECT_LT(msSince(t0), 1000);
    EXPECT_EQ(serial, __system_property_serial(pi));
}

TEST_F(SysProps, WaitTimesOut) {
    ASSERT_EQ(__system_property_set("sys.test.wt", "1"), 0);
    const prop_info* pi = __system_property_find("sys.test.wt");
    uint32_t serial = __system_property_serial(pi), out = 0;
    struct timespec to = ms(60);
    const auto t0 = Clock::now();
    EXPECT_FALSE(__system_property_wait(pi, serial, &out, &to));
    EXPECT_GE(msSince(t0), 50);
    EXPECT_LT(msSince(t0), 2000);
}

TEST_F(SysProps, WaitWakesOnASet) {
    ASSERT_EQ(__system_property_set("sys.test.ww", "1"), 0);
    const prop_info* pi = __system_property_find("sys.test.ww");
    const uint32_t serial = __system_property_serial(pi);
    std::thread setter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        __system_property_set("sys.test.ww", "1");  // the same value still wakes it
    });
    uint32_t out = 0;
    struct timespec to = ms(5000);
    const auto t0 = Clock::now();
    EXPECT_TRUE(__system_property_wait(pi, serial, &out, &to));
    EXPECT_LT(msSince(t0), 3000);
    EXPECT_NE(out, serial);
    setter.join();
}

TEST_F(SysProps, WaitSeesAnotherWriterByPolling) {
    // Without the watcher, another process's write is found by re-reading the key.
    ASSERT_EQ(__system_property_set("sys.test.wo", "1"), 0);
    const prop_info* pi = __system_property_find("sys.test.wo");
    const uint32_t serial = __system_property_serial(pi);
    auto other = thimsy::ConfigStore::open({dir_ / "persist.props", dir_ / "run"});
    ASSERT_TRUE(other != nullptr);
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        other->set("sys.test.wo", "2");
    });
    uint32_t out = 0;
    struct timespec to = ms(5000);
    EXPECT_TRUE(__system_property_wait(pi, serial, &out, &to));
    writer.join();
    char buf[PROP_VALUE_MAX];
    __system_property_get("sys.test.wo", buf);
    EXPECT_STREQ(buf, "2");
}

TEST_F(SysProps, GlobalWait) {
    // NanoMenu's wake thread: wait(nullptr, 0, ...) returns the current global serial at once.
    uint32_t serial = 0;
    EXPECT_TRUE(__system_property_wait(nullptr, 0, &serial, nullptr));
    EXPECT_NE(serial, 0u);
    EXPECT_EQ(serial, __system_property_area_serial());

    uint32_t out = 0;
    struct timespec to = ms(50);
    EXPECT_FALSE(__system_property_wait(nullptr, serial, &out, &to));

    std::thread setter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        property_set("sys.test.any", "1");
    });
    to = ms(5000);
    EXPECT_TRUE(__system_property_wait(nullptr, serial, &out, &to));
    EXPECT_NE(out, serial);
    setter.join();
}

TEST_F(SysProps, BadTimeoutIsRefused) {
    struct timespec bad;
    bad.tv_sec = 0;
    bad.tv_nsec = 2000000000L;
    uint32_t out = 0;
    EXPECT_FALSE(__system_property_wait(nullptr, 0, &out, &bad));
}

TEST_F(SysProps, WaitForProperty) {
    std::thread setter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        property_set("sys.test.wf", "no");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        property_set("sys.test.wf", "yes");
    });
    EXPECT_TRUE(android::base::WaitForProperty("sys.test.wf", "yes", std::chrono::milliseconds(5000)));
    setter.join();
    const auto t0 = Clock::now();
    EXPECT_FALSE(android::base::WaitForProperty("sys.test.wf", "never", std::chrono::milliseconds(60)));
    EXPECT_GE(msSince(t0), 50);
    EXPECT_TRUE(android::base::WaitForProperty("sys.test.wf", "yes", std::chrono::milliseconds(0)));
}

TEST_F(SysProps, WaitForPropertyCreation) {
    EXPECT_FALSE(android::base::WaitForPropertyCreation("sys.test.wc", std::chrono::milliseconds(40)));
    std::thread setter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        property_set("sys.test.wc", "here");
    });
    EXPECT_TRUE(android::base::WaitForPropertyCreation("sys.test.wc", std::chrono::milliseconds(5000)));
    setter.join();
}

// With the store watching, another process's write reaches waiters and observers on its own.
class WatchingProps : public StoreTest {
 protected:
    WatchingProps() { watch_ = true; }
};

TEST_F(WatchingProps, AnotherWriterWakesTheGlobalWaitAndObservers) {
    std::atomic<int> calls{0};
    const int id = nanocompat::addPropertyObserver("sys.test.ext", [&](std::string_view, std::string_view v) {
        if (v == "2") calls++;
    });
    uint32_t serial = 0;
    ASSERT_TRUE(__system_property_wait(nullptr, 0, &serial, nullptr));
    auto other = thimsy::ConfigStore::open({dir_ / "persist.props", dir_ / "run"});
    ASSERT_TRUE(other != nullptr);
    ASSERT_TRUE(other->set("sys.test.ext", "2"));
    uint32_t out = 0;
    struct timespec to = ms(5000);
    EXPECT_TRUE(__system_property_wait(nullptr, serial, &out, &to));
    EXPECT_TRUE(compat_test::eventually([&] { return calls.load() == 1; }));
    nanocompat::removePropertyObserver(id);
}
