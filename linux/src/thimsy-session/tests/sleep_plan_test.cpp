// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <linux/input-event-codes.h>
#include <signal.h>

#include <string>
#include <vector>

#include "core/KeyMachine.h"
#include "core/SleepPlan.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

namespace {

std::vector<std::string> lines(const std::vector<Step>& steps) {
    std::vector<std::string> out;
    for (const Step& s : steps) out.push_back(describe(s));
    return out;
}

SleepContext full() {
    SleepContext c;
    c.gamePgid = 1234;
    c.wifiRunning = true;
    c.hasFamilyHook = true;
    c.hasBacklight = true;
    c.memSleepOffered = "s2idle [deep]";
    c.stateOffered = "freeze mem disk";
    return c;
}

}  // namespace

TEST(SleepPlan, EverythingInOrder) {
    SleepPlan p = planSleep(full());
    std::vector<std::string> want = {
        "set sys.thimsy.power.state=sleeping",
        "signal group 1234 STOP",
        "script D30net stop",
        "backlight off",
        "sync",
        "hook sleep-pre",
        "write /sys/power/mem_sleep deep",
        "suspend /sys/power/state mem",
    };
    EXPECT_EQ(lines(p.sleep), want);
    std::vector<std::string> wake = {
        "hook sleep-post",
        "backlight restore",
        "signal group 1234 CONT",
        "script D30net start",
        "set sys.thimsy.power.state=awake",
    };
    EXPECT_EQ(lines(p.wake), wake);
}

TEST(SleepPlan, NothingRunningNothingToStop) {
    SleepContext c;
    c.memSleepOffered = "s2idle";
    c.stateOffered = "freeze";
    SleepPlan p = planSleep(c);
    std::vector<std::string> want = {
        "set sys.thimsy.power.state=sleeping",
        "sync",
        "write /sys/power/mem_sleep s2idle",
        "suspend /sys/power/state freeze",
    };
    EXPECT_EQ(lines(p.sleep), want);
    EXPECT_EQ(lines(p.wake), std::vector<std::string>{"set sys.thimsy.power.state=awake"});
}

TEST(SleepPlan, ChoosesTheModes) {
    EXPECT_EQ(chooseMemSleep("s2idle [deep]", ""), "deep");
    EXPECT_EQ(chooseMemSleep("[s2idle] deep", "deep"), "deep");
    EXPECT_EQ(chooseMemSleep("[s2idle] shallow deep", "shallow"), "shallow");
    EXPECT_EQ(chooseMemSleep("[s2idle]", "deep"), "s2idle");
    EXPECT_EQ(chooseMemSleep("", "deep"), "");
    EXPECT_EQ(chooseState("freeze mem disk"), "mem");
    EXPECT_EQ(chooseState("freeze"), "freeze");
    EXPECT_EQ(chooseState(""), "");
}

TEST(SleepPlan, AKernelWithoutSleepStatesGetsNoSuspendStep) {
    SleepContext c = full();
    c.stateOffered = "";
    c.memSleepOffered = "";
    SleepPlan p = planSleep(c);
    for (const Step& s : p.sleep) {
        EXPECT_NE(s.kind, StepKind::Suspend);
        EXPECT_NE(s.kind, StepKind::WriteSysfs);
    }
}

// The lid closes while a game runs: the key machine asks for sleep, and the plan stops the game's
// group before the device sleeps and continues it after it wakes.
TEST(SleepPlan, LidClosingDuringAGame) {
    KeyMachine km;
    KeyContext ctx;
    ctx.nanoRunning = true;
    ctx.gameRunning = true;
    KeyEvent e;
    e.type = EV_SW;
    e.code = SW_LID;
    e.value = 1;
    e.time = 5000;
    std::vector<KeyAction> a = km.onEvent(e, ctx);
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0], KeyAction::Sleep);

    SleepContext c;
    c.gamePgid = 4242;
    c.memSleepOffered = "s2idle [deep]";
    c.stateOffered = "mem";
    SleepPlan p = planSleep(c);
    int stopAt = -1, suspendAt = -1;
    for (size_t i = 0; i < p.sleep.size(); ++i) {
        if (p.sleep[i].kind == StepKind::SignalGroup) {
            EXPECT_EQ(p.sleep[i].pgid, 4242);
            EXPECT_EQ(p.sleep[i].signal, SIGSTOP);
            stopAt = static_cast<int>(i);
        }
        if (p.sleep[i].kind == StepKind::Suspend) suspendAt = static_cast<int>(i);
    }
    ASSERT_GE(stopAt, 0);
    ASSERT_GT(suspendAt, stopAt);
    bool cont = false;
    for (const Step& s : p.wake) {
        if (s.kind == StepKind::SignalGroup && s.pgid == 4242 && s.signal == SIGCONT) cont = true;
    }
    EXPECT_TRUE(cont);
    // Reopening the lid asks for nothing: the kernel wakes the device.
    e.value = 0;
    EXPECT_TRUE(km.onEvent(e, ctx).empty());
}
