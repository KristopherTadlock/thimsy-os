// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// KeyMachine with a fake clock: every event carries its own time and ticks pass "now".
#include <linux/input-event-codes.h>

#include <vector>

#include "core/KeyMachine.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

namespace {

KeyEvent key(int code, int value, Ms t) {
    KeyEvent e;
    e.type = EV_KEY;
    e.code = code;
    e.value = value;
    e.time = t;
    return e;
}

KeyEvent lid(int value, Ms t) {
    KeyEvent e;
    e.type = EV_SW;
    e.code = SW_LID;
    e.value = value;
    e.time = t;
    return e;
}

KeyEvent pad(int code, int value, Ms t) {
    KeyEvent e = key(code, value, t);
    e.source = KeyEvent::Source::Pad;
    return e;
}

KeyContext ctx(bool nano, bool game) {
    KeyContext c;
    c.nanoRunning = nano;
    c.gameRunning = game;
    return c;
}

// Feeds a press at 0, ticks every 100 ms up to the release time, then the release; returns every
// action seen, in order.
std::vector<KeyAction> pressFor(KeyMachine* km, Ms holdMs, const KeyContext& c) {
    std::vector<KeyAction> all;
    auto add = [&all](const std::vector<KeyAction>& a) { all.insert(all.end(), a.begin(), a.end()); };
    add(km->onEvent(key(KEY_POWER, 1, 0), c));
    for (Ms t = 100; t < holdMs; t += 100) add(km->onTick(t, c));
    add(km->onTick(holdMs, c));
    add(km->onEvent(key(KEY_POWER, 0, holdMs), c));
    return all;
}

}  // namespace

TEST(KeyMachine, PressOf2900msIsShortAndSleeps) {
    KeyMachine km;
    std::vector<KeyAction> a = pressFor(&km, 2900, ctx(false, false));
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::Sleep);
}

TEST(KeyMachine, PressOf3100msPowersOffWhenNanoIsNotRunning) {
    KeyMachine km;
    std::vector<KeyAction> a = pressFor(&km, 3100, ctx(false, false));
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::PowerOff);
}

TEST(KeyMachine, PressOf3100msGoesToNanoWhenItRuns) {
    KeyMachine km;
    std::vector<KeyAction> a = pressFor(&km, 3100, ctx(true, false));
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::PowerLongPress);
}

TEST(KeyMachine, LongPressFiresAtThreeSecondsWhileStillHeld) {
    KeyMachine km;
    KeyContext c = ctx(false, false);
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 1, 1000), c).empty());
    EXPECT_EQ(km.nextDeadline(), 4000);
    EXPECT_TRUE(km.onTick(3999, c).empty());
    std::vector<KeyAction> a = km.onTick(4000, c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::PowerOff);
    EXPECT_EQ(km.nextDeadline(), kNever);
    EXPECT_TRUE(km.onTick(9000, c).empty());
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 0, 9500), c).empty());  // the release does nothing more
}

TEST(KeyMachine, ALateReleaseStampedPastThreeSecondsIsStillLong) {
    // Stamped events read in one batch: the loop never ticked between them.
    KeyMachine km;
    KeyContext c = ctx(false, false);
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 1, 0), c).empty());
    std::vector<KeyAction> a = km.onEvent(key(KEY_POWER, 0, 3100), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::PowerOff);
    KeyMachine km2;
    EXPECT_TRUE(km2.onEvent(key(KEY_POWER, 1, 0), c).empty());
    a = km2.onEvent(key(KEY_POWER, 0, 2900), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::Sleep);
}

TEST(KeyMachine, AutorepeatOfThePowerKeyIsIgnored) {
    KeyMachine km;
    KeyContext c = ctx(true, false);
    km.onEvent(key(KEY_POWER, 1, 0), c);
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 2, 500), c).empty());
    std::vector<KeyAction> a = km.onEvent(key(KEY_POWER, 0, 600), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::Sleep);
}

TEST(KeyMachine, TheWakePressDoesNotSleepAgain) {
    KeyMachine km;
    KeyContext c = ctx(true, false);
    km.noteResumed(50000);
    // The press that woke the device, stamped just before and after the resume.
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 1, 49990), c).empty());
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 0, 50200), c).empty());
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 1, 50300), c).empty());
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 0, 50400), c).empty());
    EXPECT_EQ(km.nextDeadline(), kNever);
    // A deliberate press later sleeps again.
    km.onEvent(key(KEY_POWER, 1, 53000), c);
    std::vector<KeyAction> a = km.onEvent(key(KEY_POWER, 0, 53200), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::Sleep);
}

TEST(KeyMachine, AReleaseWithoutAPressIsIgnored) {
    KeyMachine km;
    EXPECT_TRUE(km.onEvent(key(KEY_POWER, 0, 100), ctx(false, false)).empty());
}

TEST(KeyMachine, VolumeKeysStepOnPressAndRepeat) {
    KeyMachine km;
    KeyContext c = ctx(true, false);
    std::vector<KeyAction> a = km.onEvent(key(KEY_VOLUMEUP, 1, 0), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::VolumeUp);
    a = km.onEvent(key(KEY_VOLUMEUP, 2, 300), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(km.onEvent(key(KEY_VOLUMEUP, 0, 400), c).empty());
    a = km.onEvent(key(KEY_VOLUMEDOWN, 1, 500), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::VolumeDown);
}

TEST(KeyMachine, ClosingTheLidSleepsOncePerClose) {
    KeyMachine km;
    KeyContext c = ctx(true, true);
    std::vector<KeyAction> a = km.onEvent(lid(1, 0), c);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::Sleep);
    EXPECT_TRUE(km.lidClosed());
    EXPECT_TRUE(km.onEvent(lid(1, 10), c).empty());
    EXPECT_TRUE(km.onEvent(lid(0, 20), c).empty());
    EXPECT_FALSE(km.lidClosed());
    EXPECT_EQ(km.onEvent(lid(1, 30), c).size(), 1u);
}

TEST(KeyMachine, QuitComboQuitsOnlyARunningGameAndOncePerHold) {
    KeyMachine km;
    KeyContext noGame = ctx(true, false);
    KeyContext game = ctx(true, true);
    EXPECT_TRUE(km.onEvent(pad(BTN_MODE, 1, 0), noGame).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_START, 1, 10), noGame).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_START, 0, 20), noGame).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_MODE, 0, 30), noGame).empty());

    EXPECT_TRUE(km.onEvent(pad(BTN_MODE, 1, 100), game).empty());
    std::vector<KeyAction> a = km.onEvent(pad(BTN_START, 1, 110), game);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], KeyAction::QuitGame);
    EXPECT_TRUE(km.onEvent(pad(BTN_START, 2, 200), game).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_SOUTH, 1, 210), game).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_START, 0, 300), game).empty());
    EXPECT_EQ(km.onEvent(pad(BTN_START, 1, 400), game).size(), 1u);  // released and pressed again
}

TEST(KeyMachine, QuitComboFromDeviceJson) {
    KeyConfig cfg;
    cfg.quitCombo = {{EV_KEY, BTN_SELECT}, {EV_KEY, BTN_START}};
    KeyMachine km(cfg);
    KeyContext game = ctx(true, true);
    EXPECT_TRUE(km.onEvent(pad(BTN_MODE, 1, 0), game).empty());
    EXPECT_TRUE(km.onEvent(pad(BTN_START, 1, 10), game).empty());
    EXPECT_EQ(km.onEvent(pad(BTN_SELECT, 1, 20), game).size(), 1u);
}

TEST(KeyMachine, PadEventsNeverActAsSystemKeys) {
    KeyMachine km;
    KeyEvent e = pad(KEY_POWER, 1, 0);
    EXPECT_TRUE(km.onEvent(e, ctx(false, false)).empty());
    EXPECT_FALSE(km.powerHeld());
}

TEST(InputCodes, NamesAndNumbers) {
    InputCode c;
    ASSERT_TRUE(inputCodeByName("BTN_MODE", &c));
    EXPECT_EQ(c.type, EV_KEY);
    EXPECT_EQ(c.code, BTN_MODE);
    ASSERT_TRUE(inputCodeByName("SW_LID", &c));
    EXPECT_EQ(c.type, EV_SW);
    ASSERT_TRUE(inputCodeByName("1:116", &c));
    EXPECT_EQ(c.code, KEY_POWER);
    EXPECT_FALSE(inputCodeByName("KEY_NOPE", &c));
    EXPECT_EQ(inputCodeName(EV_KEY, KEY_POWER), "KEY_POWER");
    EXPECT_EQ(inputCodeName(EV_KEY, 999), "1:999");
}
