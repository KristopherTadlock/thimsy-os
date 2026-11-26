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

// utils/Thread.h: run, readyToRun, the threadLoop loop, exit requests, join, the thread's hold on
// its object, onFirstRef starting a thread (as BootAnimation-style classes do), names, priority.

#include <utils/Thread.h>

#include <errno.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "compat_test_util.h"

using android::sp;
using android::status_t;
using android::Thread;
using android::wp;

namespace {

pid_t tid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

struct Shared {
    std::atomic<int> ready{0};
    std::atomic<int> loops{0};
    std::atomic<int> destroyed{0};
    std::atomic<pid_t> loopTid{0};
    std::atomic<bool> sawExitPending{false};
    std::atomic<status_t> selfWait{0};
    std::atomic<int> nice{-100};
    char name[17] = {};
};

class Worker : public Thread {
 public:
    // loopsBeforeFalse < 0: loop until an exit is requested.
    Worker(Shared* s, int loopsBeforeFalse, status_t readyStatus = android::OK)
        : Thread(false), s_(s), limit_(loopsBeforeFalse), readyStatus_(readyStatus) {}
    ~Worker() override { s_->destroyed++; }

    bool callExitPending() const { return exitPending(); }

 private:
    status_t readyToRun() override {
        s_->ready++;
        return readyStatus_;
    }
    bool threadLoop() override {
        const int n = ++s_->loops;
        s_->loopTid = tid();
        if (n == 1) {
            prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(s_->name), 0, 0, 0);
            errno = 0;
            s_->nice = getpriority(PRIO_PROCESS, static_cast<id_t>(tid()));
        }
        if (exitPending()) s_->sawExitPending = true;
        if (limit_ >= 0) return n < limit_;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

    Shared* s_;
    int limit_;
    status_t readyStatus_;
};

class SelfWaiter : public Thread {
 public:
    explicit SelfWaiter(Shared* s) : s_(s) {}

 private:
    bool threadLoop() override {
        s_->selfWait = requestExitAndWait();
        s_->loops++;
        return false;
    }
    Shared* s_;
};

// Starts its own thread when the first strong reference is taken.
class StartsOnFirstRef : public Thread {
 public:
    explicit StartsOnFirstRef(Shared* s) : s_(s) {}
    ~StartsOnFirstRef() override { s_->destroyed++; }
    status_t started = -1;

 private:
    void onFirstRef() override { started = run("StartsOnFirstRef", android::PRIORITY_DEFAULT); }
    bool threadLoop() override {
        s_->loops++;
        return false;
    }
    Shared* s_;
};

}  // namespace

TEST(Thread, RunsReadyToRunOnceThenLoopsUntilFalse) {
    Shared s;
    sp<Worker> w = new Worker(&s, 3);
    EXPECT_FALSE(w->isRunning());
    EXPECT_EQ(w->getTid(), -1);
    ASSERT_EQ(w->run("Worker"), android::OK);
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_FALSE(w->isRunning());
    EXPECT_EQ(s.ready.load(), 1);
    EXPECT_EQ(s.loops.load(), 3);
    EXPECT_NE(s.loopTid.load(), tid());  // on its own thread
    EXPECT_EQ(w->getTid(), -1);
}

TEST(Thread, ReadyToRunFailureEndsItWithThatStatus) {
    Shared s;
    sp<Worker> w = new Worker(&s, 3, android::NO_INIT);
    ASSERT_EQ(w->run("Worker"), android::OK);
    EXPECT_EQ(w->join(), android::NO_INIT);
    EXPECT_EQ(s.loops.load(), 0);
}

TEST(Thread, RequestExitAndWait) {
    Shared s;
    sp<Worker> w = new Worker(&s, -1);
    ASSERT_EQ(w->run("Worker"), android::OK);
    ASSERT_TRUE(compat_test::eventually([&] { return s.loops.load() >= 2; }));
    EXPECT_TRUE(w->isRunning());
    const pid_t running = w->getTid();
    EXPECT_GT(running, 0);
    EXPECT_EQ(running, s.loopTid.load());
    EXPECT_EQ(w->requestExitAndWait(), android::OK);
    EXPECT_FALSE(w->isRunning());
    EXPECT_FALSE(w->callExitPending());  // cleared again, as on Android
    const int loops = s.loops.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(s.loops.load(), loops);  // stopped for good
}

TEST(Thread, RequestExitIsSeenInsideTheLoop) {
    Shared s;
    sp<Worker> w = new Worker(&s, -1);
    ASSERT_EQ(w->run("Worker"), android::OK);
    ASSERT_TRUE(compat_test::eventually([&] { return s.loops.load() >= 1; }));
    w->requestExit();
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_FALSE(w->isRunning());
}

TEST(Thread, RunWhileRunningIsRefusedAndRunAgainAfterwardsWorks) {
    Shared s;
    sp<Worker> w = new Worker(&s, -1);
    ASSERT_EQ(w->run("Worker"), android::OK);
    EXPECT_EQ(w->run("Worker"), android::INVALID_OPERATION);
    EXPECT_EQ(w->requestExitAndWait(), android::OK);
    const int before = s.ready.load();
    ASSERT_EQ(w->run("Worker"), android::OK);
    ASSERT_TRUE(compat_test::eventually([&] { return s.ready.load() == before + 1; }));
    EXPECT_EQ(w->requestExitAndWait(), android::OK);
}

TEST(Thread, WaitingOnItselfWouldBlock) {
    Shared s;
    sp<SelfWaiter> w = new SelfWaiter(&s);
    ASSERT_EQ(w->run("SelfWaiter"), android::OK);
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_EQ(s.selfWait.load(), android::WOULD_BLOCK);
}

TEST(Thread, TheThreadHoldsItsObjectOnlyDuringALoop) {
    // libutils' rule: the thread holds a strong reference while threadLoop() runs and only a weak
    // one between two calls, so once the last outside reference goes the loop ends (after at most
    // the current call) and the Thread is destroyed on its own thread.
    Shared s;
    {
        sp<Worker> w = new Worker(&s, -1);  // would loop until asked to stop
        ASSERT_EQ(w->run("Worker"), android::OK);
        ASSERT_TRUE(compat_test::eventually([&] { return s.loops.load() >= 1; }));
    }
    ASSERT_TRUE(compat_test::eventually([&] { return s.destroyed.load() == 1; }));
    const int loops = s.loops.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(s.loops.load(), loops);
}

TEST(Thread, AThreadNobodyElseHoldsFinishesItsFirstLoop) {
    // run() and the start of the thread hold it, so an object whose sp goes away at once still
    // runs readyToRun() and one threadLoop().
    Shared s;
    {
        sp<Worker> w = new Worker(&s, 1000);
        ASSERT_EQ(w->run("Worker"), android::OK);
    }
    ASSERT_TRUE(compat_test::eventually([&] { return s.destroyed.load() == 1; }));
    EXPECT_EQ(s.ready.load(), 1);
    EXPECT_GE(s.loops.load(), 1);
}

TEST(Thread, OnFirstRefMayStartTheThread) {
    Shared s;
    {
        sp<StartsOnFirstRef> t = new StartsOnFirstRef(&s);
        EXPECT_EQ(t->started, android::OK);
        EXPECT_EQ(t->join(), android::OK);
        EXPECT_EQ(s.loops.load(), 1);
    }
    ASSERT_TRUE(compat_test::eventually([&] { return s.destroyed.load() == 1; }));
}

TEST(Thread, NameKeepsTheLast15Bytes) {
    Shared s;
    sp<Worker> w = new Worker(&s, 1);
    ASSERT_EQ(w->run("GammaOSNanoOverlayThread"), android::OK);
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_STREQ(s.name, "noOverlayThread");

    Shared s2;
    sp<Worker> w2 = new Worker(&s2, 1);
    ASSERT_EQ(w2->run("Short"), android::OK);
    EXPECT_EQ(w2->join(), android::OK);
    EXPECT_STREQ(s2.name, "Short");
}

TEST(Thread, PriorityIsANiceValue) {
    EXPECT_EQ(android::PRIORITY_DISPLAY, -4);
    EXPECT_EQ(android::PRIORITY_BACKGROUND, 10);
    Shared s;
    sp<Worker> w = new Worker(&s, 1);
    // The lowest priority: lowering needs no privilege, even under `nice -n 19`.
    ASSERT_EQ(w->run("Worker", android::PRIORITY_LOWEST), android::OK);
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_EQ(s.nice.load(), 19);

    Shared s2;
    errno = 0;
    const int mine = getpriority(PRIO_PROCESS, 0);
    sp<Worker> w2 = new Worker(&s2, 1);
    ASSERT_EQ(w2->run("Worker", android::PRIORITY_DEFAULT), android::OK);
    EXPECT_EQ(w2->join(), android::OK);
    EXPECT_EQ(s2.nice.load(), mine);  // inherited
}

TEST(Thread, JoinBeforeRunReturnsAtOnce) {
    Shared s;
    sp<Worker> w = new Worker(&s, 1);
    EXPECT_EQ(w->join(), android::OK);
    EXPECT_EQ(w->requestExitAndWait(), android::OK);
}
