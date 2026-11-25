// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/watch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "test_util.h"

using thimsy::Watcher;
using thimsy_test::ScratchDir;

namespace {

// Collects events until one for `name` with `what` arrives, or 5 s pass.
bool waitFor(Watcher& w, const std::string& name, uint32_t what) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        std::vector<Watcher::Event> ev;
        if (!w.wait(&ev, 100)) return false;
        for (const auto& e : ev) {
            if (e.name == name && (e.what & what)) return true;
        }
    }
    return false;
}

}  // namespace

TEST(Inotify, SeesWritesRenamesAndRemovals) {
    ScratchDir d;
    auto w = Watcher::createInotify();
    ASSERT_TRUE(w != nullptr);
    ASSERT_TRUE(w->addDir(d.path()));
    EXPECT_FALSE(w->addDir(d / "missing"));
    ASSERT_TRUE(thimsy::writeFileAtomic(d / "sys.a", "1"));  // temp file, then a rename
    EXPECT_TRUE(waitFor(*w, "sys.a", Watcher::kWritten));
    ASSERT_EQ(unlink((d / "sys.a").c_str()), 0);
    EXPECT_TRUE(waitFor(*w, "sys.a", Watcher::kRemoved));
    std::vector<Watcher::Event> ev;
    EXPECT_TRUE(w->wait(&ev, 0));  // nothing pending: returns at once
}

TEST(Inotify, WakeInterruptsAWait) {
    auto w = Watcher::createInotify();
    ASSERT_TRUE(w != nullptr);
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        w->wake();
    });
    const auto start = std::chrono::steady_clock::now();
    std::vector<Watcher::Event> ev;
    EXPECT_TRUE(w->wait(&ev, 10000));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    EXPECT_TRUE(ev.empty());
    t.join();
}

TEST(FakeWatcher, DeliversPushedEventsInOrder) {
    thimsy::FakeWatcher w;
    EXPECT_TRUE(w.addDir("/run/x"));
    EXPECT_EQ(w.dirs(), std::vector<std::string>{"/run/x"});
    std::vector<Watcher::Event> ev;
    EXPECT_TRUE(w.wait(&ev, 10));  // times out empty
    EXPECT_TRUE(ev.empty());
    w.push({"/run/x", "a", Watcher::kWritten});
    w.push({"/run/x", "b", Watcher::kRemoved});
    EXPECT_TRUE(w.wait(&ev, -1));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].name, "a");
    EXPECT_EQ(ev[1].what, static_cast<uint32_t>(Watcher::kRemoved));
}
