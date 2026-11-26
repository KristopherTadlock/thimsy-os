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

// HostControl: JSON quoting, the event file, presents, the frame barrier, captures and the control
// channel (the FIFO included).

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/HostControl.h"

using namespace nanohost;
using headless_test::TmpDir;

namespace {

struct HostControlTest : public ::testing::Test {
    TmpDir dir;
    std::string events;
    void SetUp() override {
        resetForTest();
        DisplayConfig d;
        d.pace = false;
        setDisplayConfig(d);
        events = dir / "events.jsonl";
        ASSERT_TRUE(openEvents(events));
    }
    void TearDown() override { resetForTest(); }
    std::vector<std::string> eventLines() { return headless_test::lines(headless_test::readFile(events)); }
    size_t count(const std::string& needle) {
        size_t n = 0;
        for (const auto& l : eventLines()) n += l.find(needle) != std::string::npos;
        return n;
    }
};

bool waitFor(const std::function<bool()>& cond, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return cond();
}

TEST(JsonQuote, Escapes) {
    EXPECT_EQ(jsonQuote("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(jsonQuote("x\ny\tz\r"), "\"x\\ny\\tz\\r\"");
    EXPECT_EQ(jsonQuote(std::string("\x01", 1)), "\"\\u0001\"");
    EXPECT_EQ(jsonQuote("caf\xc3\xa9"), "\"caf\xc3\xa9\"");   // valid UTF-8 kept
    EXPECT_EQ(jsonQuote("bad\xff"), "\"bad\\ufffd\"");          // invalid byte replaced
    EXPECT_EQ(jsonQuote("\xc0\x80"), "\"\\ufffd\\ufffd\"");     // overlong form refused
    EXPECT_EQ(jsonQuote(""), "\"\"");
}

TEST_F(HostControlTest, EventLines) {
    event("hello", "\"x\":1");
    event("bare");
    const auto l = eventLines();
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0].rfind("{\"ev\":\"hello\",\"t_ms\":", 0), 0u);
    EXPECT_NE(l[0].find(",\"x\":1}"), std::string::npos);
    EXPECT_EQ(l[1].rfind("{\"ev\":\"bare\",\"t_ms\":", 0), 0u);
    EXPECT_EQ(l[1].back(), '}');
}

TEST_F(HostControlTest, PresentCountsAndCaptures) {
    PresentInfo info;
    info.slot = 2;
    info.width = 4;
    info.height = 3;
    int captures = 0;
    std::string capturedTo;
    auto cap = [&](const std::string& p) {
        ++captures;
        capturedTo = p;
        return true;
    };
    EXPECT_EQ(present(info, cap), 1u);
    EXPECT_EQ(captures, 0);
    requestCapture(dir / "f.ppm");
    EXPECT_EQ(present(info, cap), 2u);
    EXPECT_EQ(captures, 1);
    EXPECT_EQ(capturedTo, dir / "f.ppm");
    EXPECT_EQ(present(info, cap), 3u);  // a capture is taken once
    EXPECT_EQ(captures, 1);
    EXPECT_EQ(presentCount(), 3u);
    EXPECT_EQ(count("\"ev\":\"present\""), 3u);
    EXPECT_EQ(count("\"ev\":\"capture\""), 1u);
    EXPECT_EQ(count("\"slot\":2,\"w\":4,\"h\":3"), 3u);
}

TEST_F(HostControlTest, BarrierHoldsUntilGranted) {
    setBarrier(true);
    std::atomic<int> done{0};
    std::thread renderer([&] {
        PresentInfo info;
        for (int i = 0; i < 4; i++) {
            present(info, nullptr);
            done++;
        }
    });
    // The first present is logged, then held.
    ASSERT_TRUE(waitFor([&] { return presentCount() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(done.load(), 0);
    grant(2);  // releases presents 1 and 2; present 3 is logged and held
    ASSERT_TRUE(waitFor([&] { return presentCount() == 3; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(done.load(), 2);
    EXPECT_TRUE(handleControl("barrier off"));
    renderer.join();
    EXPECT_EQ(done.load(), 4);
    EXPECT_EQ(presentCount(), 4u);
}

TEST_F(HostControlTest, QuitOpensTheBarrierAndCallsTheHandler) {
    std::atomic<int> quits{0};
    setQuitHandler([&] { quits++; });
    setBarrier(true);
    std::thread renderer([] { present(PresentInfo(), nullptr); });
    ASSERT_TRUE(waitFor([] { return presentCount() == 1; }));
    EXPECT_TRUE(handleControl("quit"));
    renderer.join();
    EXPECT_EQ(quits.load(), 1);
    setBarrier(true);  // no effect once quitting
    present(PresentInfo(), nullptr);
    EXPECT_EQ(presentCount(), 2u);
}

TEST_F(HostControlTest, ControlLines) {
    EXPECT_TRUE(handleControl("barrier on"));
    EXPECT_TRUE(handleControl("  grant 3  "));
    EXPECT_TRUE(handleControl("capture /tmp/x.ppm"));
    EXPECT_TRUE(handleControl("ping abc"));
    EXPECT_FALSE(handleControl("grant -1"));
    EXPECT_FALSE(handleControl("grant x"));
    EXPECT_FALSE(handleControl("barrier maybe"));
    EXPECT_FALSE(handleControl("dance"));
    EXPECT_EQ(count("\"ev\":\"pong\",\"t_ms\":"), 1u);
    EXPECT_EQ(count("\"token\":\"abc\""), 1u);
    EXPECT_EQ(count("\"ev\":\"control_error\""), 4u);
    setBarrier(false);
}

TEST_F(HostControlTest, ControlFifo) {
    const std::string fifo = dir / "ctl";
    ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
    std::string err;
    ASSERT_TRUE(startControl(fifo, &err)) << err;
    int w = open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(w, 0);
    const std::string cmds = "ping one\nping two\r\nping three";
    ASSERT_EQ(write(w, cmds.data(), cmds.size()), (ssize_t)cmds.size());
    EXPECT_TRUE(waitFor([&] { return count("\"ev\":\"pong\"") == 2; }));
    ASSERT_EQ(write(w, "\n", 1), 1);  // completes the third line
    EXPECT_TRUE(waitFor([&] { return count("\"token\":\"three\"") == 1; }));
    EXPECT_EQ(count("\"token\":\"two\""), 1u);
    close(w);
}

TEST_F(HostControlTest, PacingFollowsTheRefreshRate) {
    DisplayConfig d;
    d.pace = true;
    d.refreshHz = 100;
    setDisplayConfig(d);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 6; i++) present(PresentInfo(), nullptr);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_GE(ms, 45);   // 5 intervals of 10 ms after the first present
    EXPECT_LT(ms, 2000);
}

}  // namespace
