// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <signal.h>

#include <vector>

#include "core/Supervisor.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

namespace {

int count(const std::vector<SupAction>& a, SupKind k) {
    int n = 0;
    for (const SupAction& x : a) {
        if (x.kind == k) ++n;
    }
    return n;
}

// Starts a supervisor at t=0 with Nano running as pid 100.
Supervisor started(bool deferredRan = false) {
    Supervisor s;
    std::vector<SupAction> a = s.start(0, deferredRan, true);
    EXPECT_EQ(count(a, SupKind::SpawnNano), 1);
    s.nanoStarted(0, 100);
    return s;
}

}  // namespace

TEST(Supervisor, StartsNanoAtOnceAndPaddWhenItIsNotRunning) {
    Supervisor s;
    std::vector<SupAction> a = s.start(0, false, false);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].kind, SupKind::SpawnNano);
    EXPECT_EQ(a[1].kind, SupKind::SpawnPadd);
    Supervisor s2;
    a = s2.start(0, false, true);
    EXPECT_EQ(count(a, SupKind::SpawnPadd), 0);
}

TEST(Supervisor, NanoCrashesBackOffAndALongRunResetsIt) {
    Supervisor s = started();
    s.firstFrame(0);  // no deferred-services fallback timer in the way
    Ms t = 0;
    Ms expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (Ms want : expected) {
        t += 500;  // crashed half a second after starting
        EXPECT_TRUE(s.nanoExited(t, 0).empty());
        EXPECT_EQ(s.nanoBackoff(), want);
        EXPECT_EQ(s.nextDeadline(), t + want);
        EXPECT_TRUE(s.onTick(t + want - 1).empty());
        std::vector<SupAction> a = s.onTick(t + want);
        ASSERT_EQ(count(a, SupKind::SpawnNano), 1);
        t += want;
        s.nanoStarted(t, 200);
    }
    // Ran for a minute: started again at once, and the back-off starts over.
    t += 60000;
    s.nanoExited(t, 0);
    EXPECT_EQ(s.nanoBackoff(), 0);
    EXPECT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
    s.nanoStarted(t, 201);
    s.nanoExited(t + 100, 0);
    EXPECT_EQ(s.nanoBackoff(), 1000);
}

TEST(Supervisor, AFailedSpawnCountsAsACrash) {
    Supervisor s;
    s.start(0, false, true);
    s.nanoStarted(0, -1);
    EXPECT_FALSE(s.nanoRunning());
    EXPECT_EQ(s.nextDeadline(), 1000);
}

TEST(Supervisor, NanoLeavingAGameRunningComesBackWhenTheGameEnds) {
    Supervisor s = started();
    s.firstFrame(0);
    std::vector<SupAction> a = s.nanoExited(1000, 555);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, SupKind::WatchGame);
    EXPECT_EQ(a[0].pid, 555);
    EXPECT_TRUE(s.waitingForGame());
    EXPECT_TRUE(s.onTick(100000).empty());  // nothing while the game runs
    a = s.gameExited(200000);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, SupKind::SpawnNano);
    EXPECT_FALSE(s.waitingForGame());
    s.nanoStarted(200000, 300);
    EXPECT_TRUE(s.nanoRunning());
}

TEST(Supervisor, DeferredServicesRunOnceAtTheFirstFrame) {
    Supervisor s = started();
    std::vector<SupAction> a = s.firstFrame(800);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, SupKind::RunDeferred);
    EXPECT_TRUE(s.firstFrame(900).empty());
    EXPECT_EQ(count(s.onTick(100000), SupKind::RunDeferred), 0);
}

TEST(Supervisor, DeferredServicesRunAnywayWhenNoFrameComes) {
    Supervisor s = started();
    EXPECT_EQ(s.nextDeadline(), 30000);
    EXPECT_EQ(count(s.onTick(29999), SupKind::RunDeferred), 0);
    EXPECT_EQ(count(s.onTick(30000), SupKind::RunDeferred), 1);
    EXPECT_TRUE(s.firstFrame(31000).empty());
}

TEST(Supervisor, ARespawnedSessionDoesNotRunDeferredServicesAgain) {
    Supervisor s = started(true);
    EXPECT_TRUE(s.firstFrame(10).empty());
    EXPECT_EQ(count(s.onTick(100000), SupKind::RunDeferred), 0);
}

TEST(Supervisor, QuitTerminatesThenKills) {
    Supervisor s = started();
    std::vector<SupAction> a = s.quitGame(1000, 4242);
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].sig, SIGCONT);
    EXPECT_EQ(a[1].sig, SIGTERM);
    EXPECT_EQ(a[1].pid, 4242);
    EXPECT_TRUE(s.onTick(3999).empty());
    a = s.onTick(4000);
    ASSERT_EQ(count(a, SupKind::SignalGroup), 1);
    for (const SupAction& x : a) {
        if (x.kind == SupKind::SignalGroup) {
            EXPECT_EQ(x.sig, SIGKILL);
            EXPECT_EQ(x.pid, 4242);
        }
    }
    EXPECT_TRUE(s.quitGame(5000, 0).empty());
    EXPECT_TRUE(s.quitGame(5000, 1).empty());
}

TEST(Supervisor, ScrapeStartsOnceWhileRunning) {
    Supervisor s = started();
    EXPECT_EQ(count(s.scrapeRequested(10), SupKind::SpawnScrape), 1);
    EXPECT_TRUE(s.scrapeRequested(11).empty());  // still starting
    s.scrapeStarted(700);
    EXPECT_TRUE(s.scrapeRequested(12).empty());  // running: it reads the queue itself
    s.scrapeExited();
    EXPECT_EQ(count(s.scrapeRequested(13), SupKind::SpawnScrape), 1);
}

TEST(Supervisor, PaddIsRestartedWithBackOff) {
    Supervisor s;
    s.start(0, false, true);
    s.nanoStarted(0, 100);
    s.paddExited(100);
    EXPECT_EQ(count(s.onTick(1099), SupKind::SpawnPadd), 0);
    EXPECT_EQ(count(s.onTick(1100), SupKind::SpawnPadd), 1);
    s.paddStarted(1100, 50);
    s.paddExited(1200);
    EXPECT_EQ(count(s.onTick(3199), SupKind::SpawnPadd), 0);
    EXPECT_EQ(count(s.onTick(3200), SupKind::SpawnPadd), 1);
}

TEST(Supervisor, NothingStartsAfterShutdown) {
    Supervisor s = started();
    s.shutdown();
    s.nanoExited(100, 0);
    s.paddExited(100);
    EXPECT_TRUE(s.onTick(1000000).empty());
    EXPECT_TRUE(s.scrapeRequested(10).empty());
    EXPECT_TRUE(s.firstFrame(10).empty());
    EXPECT_EQ(s.nextDeadline(), kNever);
}

TEST(Supervisor, RestartNanoStopsItAndStartsItAtOnce) {
    Supervisor s = started();
    std::vector<SupAction> a = s.restartNano(5000);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, SupKind::SignalGroup);
    EXPECT_EQ(a[0].pid, 100);
    EXPECT_EQ(a[0].sig, SIGTERM);
    s.nanoExited(5100, 0);
    EXPECT_EQ(count(s.onTick(5100), SupKind::SpawnNano), 1);
}
