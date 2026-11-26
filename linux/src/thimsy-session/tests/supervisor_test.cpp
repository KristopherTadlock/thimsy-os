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

// linux-host-impls-6: Nano exits after every launch. A launch that left no game running (it could
// not start, or the game ended at once) is not a crash: Nano comes back at once and the back-off
// that crashes built up is reset.
TEST(Supervisor, NanoExitingForALaunchIsNotACrash) {
    Supervisor s = started();
    s.firstFrame(0);
    s.nanoExited(500, 0);  // a crash half a second in
    EXPECT_EQ(s.nanoBackoff(), 1000);
    ASSERT_EQ(count(s.onTick(1500), SupKind::SpawnNano), 1);
    s.nanoStarted(1500, 200);
    for (Ms t = 3000; t <= 6000; t += 1500) {  // short launches, up to the free restarts
        EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
        EXPECT_EQ(s.nanoBackoff(), 0);
        EXPECT_EQ(s.nextDeadline(), t);
        ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
        s.nanoStarted(t, 300);
    }
    // A game still running wins: Nano waits for it, whatever the status.
    std::vector<SupAction> a = s.nanoExited(20000, 555, true);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, SupKind::WatchGame);
    EXPECT_TRUE(s.onTick(25000).empty());
    ASSERT_EQ(count(s.gameExited(26000), SupKind::SpawnNano), 1);
    s.nanoStarted(26000, 400);
    s.nanoExited(26100, 0);  // and a short run that is not a launch's end is a crash again
    EXPECT_EQ(s.nanoBackoff(), 1000);
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

// REV4-2 left a launch that fails at Nano's start on every run free to restart Nano at once forever.
// Quick launch cycles (shorter than launchLoopWindowMs) are free up to launchLoopFreeRestarts in a
// row, then wait the crash back-off; a long cycle ends the run and resets the back-off.
TEST(Supervisor, ALaunchLoopBacksOff) {
    Supervisor s = started();
    s.firstFrame(0);
    Ms t = 0;
    for (int i = 0; i < 3; i++) {  // free: Nano at once
        t += 500;
        EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
        EXPECT_EQ(s.quickLaunches(), i + 1);
        EXPECT_EQ(s.nanoBackoff(), 0);
        ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
        s.nanoStarted(t, 200 + i);
    }
    for (Ms want : {1000, 2000, 4000, 8000, 16000, 30000, 30000}) {
        t += 500;
        EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
        EXPECT_EQ(s.nanoBackoff(), want);
        EXPECT_EQ(s.nextDeadline(), t + want);
        EXPECT_TRUE(s.onTick(t + want - 1).empty());
        ASSERT_EQ(count(s.onTick(t + want), SupKind::SpawnNano), 1);
        t += want;
        s.nanoStarted(t, 300);
    }
    // A run as long as the window (the user browsed, then launched): at once, the run over.
    t += 10000;
    EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
    EXPECT_EQ(s.quickLaunches(), 0);
    EXPECT_EQ(s.nanoBackoff(), 0);
    ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
}

// A game that ends soon after Nano started (it crashed at its start, say) makes a quick cycle too:
// Nano is started again by onTick after the back-off, not by gameExited at once.
TEST(Supervisor, AGameThatEndsAtOnceCountsAsAQuickCycle) {
    Supervisor s = started();
    s.firstFrame(0);
    Ms t = 0;
    for (int i = 0; i < 3; i++) {
        t += 400;
        ASSERT_EQ(count(s.nanoExited(t, 555, true), SupKind::WatchGame), 1);
        t += 100;
        ASSERT_EQ(count(s.gameExited(t), SupKind::SpawnNano), 1);
        s.nanoStarted(t, 200);
    }
    t += 400;
    ASSERT_EQ(count(s.nanoExited(t, 555, true), SupKind::WatchGame), 1);
    t += 100;
    EXPECT_TRUE(s.gameExited(t).empty());
    EXPECT_FALSE(s.waitingForGame());
    EXPECT_EQ(s.nanoBackoff(), 1000);
    EXPECT_EQ(s.nextDeadline(), t + 1000);
    EXPECT_TRUE(s.onTick(t + 999).empty());
    ASSERT_EQ(count(s.onTick(t + 1000), SupKind::SpawnNano), 1);
    s.nanoStarted(t + 1000, 300);
    // A game played for a while: Nano at once when it ends, and the run of quick cycles is over.
    t += 1000;
    ASSERT_EQ(count(s.nanoExited(t + 400, 556, true), SupKind::WatchGame), 1);
    ASSERT_EQ(count(s.gameExited(t + 600000), SupKind::SpawnNano), 1);
    EXPECT_EQ(s.quickLaunches(), 0);
    EXPECT_EQ(s.nanoBackoff(), 0);
}

// A restart asked for (park_restart, restart-nano) starts over: the next quick cycles are free.
TEST(Supervisor, ARestartRequestEndsALaunchLoop) {
    Supervisor s = started();
    s.firstFrame(0);
    Ms t = 0;
    for (int i = 0; i < 4; i++) {
        t += 500;
        s.nanoExited(t, 0, true);
        t += s.nanoBackoff();
        ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
        s.nanoStarted(t, 200);
    }
    EXPECT_EQ(s.quickLaunches(), 4);
    ASSERT_EQ(count(s.restartNano(t + 100), SupKind::SignalGroup), 1);
    EXPECT_EQ(s.quickLaunches(), 0);
    s.nanoExited(t + 200, 0);
    ASSERT_EQ(count(s.onTick(t + 200), SupKind::SpawnNano), 1);
    s.nanoStarted(t + 200, 300);
    s.nanoExited(t + 700, 0, true);
    EXPECT_EQ(s.quickLaunches(), 1);
    EXPECT_EQ(s.nextDeadline(), t + 700);
}

// w5-linux-6: a user trying games that fail at once (a missing core, a bad ROM) has the menu on
// screen for a few seconds before each launch: never held back, however quick the cycles. Only a
// cycle in which Nano was on screen under launchLoopActMs (or never) is quick.
TEST(Supervisor, AUserTryingGamesThatFailIsNotHeldBack) {
    Supervisor s = started(true);
    Ms t = 0;
    for (int i = 0; i < 8; i++) {
        s.firstFrame(t + 1500);  // Nano's menu up 1.5 s after its start
        t += 1500 + 2500;        // the user picks a game 2.5 s later; it fails at once
        EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
        EXPECT_EQ(s.quickLaunches(), 0);
        EXPECT_EQ(s.nanoBackoff(), 0);
        ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1) << i;
        s.nanoStarted(t, 200 + i);
    }
    // The same with a game that ends at once.
    for (int i = 0; i < 8; i++) {
        s.firstFrame(t + 1500);
        t += 4000;
        ASSERT_EQ(count(s.nanoExited(t, 555, true), SupKind::WatchGame), 1);
        t += 200;
        ASSERT_EQ(count(s.gameExited(t), SupKind::SpawnNano), 1) << i;
        s.nanoStarted(t, 300 + i);
    }
    EXPECT_EQ(s.quickLaunches(), 0);
}

// An auto-launch: Nano asks for the launch as soon as its menu is up, or before.
TEST(Supervisor, ALaunchRightAfterTheFirstFrameIsQuick) {
    Supervisor s = started(true);
    Ms t = 0;
    for (int i = 0; i < 3; i++) {
        s.firstFrame(t + 1500);
        t += 1500 + 300;
        EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
        EXPECT_EQ(s.quickLaunches(), i + 1);
        ASSERT_EQ(count(s.onTick(t), SupKind::SpawnNano), 1);
        s.nanoStarted(t, 200 + i);
    }
    s.firstFrame(t + 1500);
    t += 1500 + 1999;
    EXPECT_TRUE(s.nanoExited(t, 0, true).empty());
    EXPECT_EQ(s.nanoBackoff(), 1000);
    EXPECT_EQ(s.nextDeadline(), t + 1000);
    // A first frame of an earlier Nano does not count for the next one: it was never on screen.
    ASSERT_EQ(count(s.onTick(t + 1000), SupKind::SpawnNano), 1);
    s.nanoStarted(t + 1000, 300);
    EXPECT_TRUE(s.nanoExited(t + 1500, 0, true).empty());
    EXPECT_EQ(s.nanoBackoff(), 2000);
    // A first frame while no Nano runs is not taken for the next one's.
    s.firstFrame(t + 2000);
    ASSERT_EQ(count(s.onTick(t + 3500), SupKind::SpawnNano), 1);
    s.nanoStarted(t + 3500, 301);
    EXPECT_TRUE(s.nanoExited(t + 9000, 0, true).empty());
    EXPECT_EQ(s.nanoBackoff(), 4000);
}
