// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Calibrate.h"

#include "gtest/gtest.h"

using namespace padd;

namespace {
Calibration cal(int min, int max) {
    Calibration c;
    c.min = min;
    c.max = max;
    return c;
}
}  // namespace

TEST(Calibrate, StickEndsAndCentre) {
    AxisRange r = resolveRange(cal(0, 4000), nullptr, false, false);
    ASSERT_TRUE(r.valid);
    EXPECT_EQ(r.center, 2000);
    EXPECT_EQ(scaleStick(r, 2000), 0);
    EXPECT_EQ(scaleStick(r, 4000), kStickMax);
    EXPECT_EQ(scaleStick(r, 0), kStickMin);
    EXPECT_EQ(scaleStick(r, 3000), 16384);  // halfway up, rounded
    EXPECT_EQ(scaleStick(r, 1000), -16384);
}

TEST(Calibrate, StickClampsOutsideTheRange) {
    AxisRange r = resolveRange(cal(-1800, 1800), nullptr, false, false);
    EXPECT_EQ(scaleStick(r, 5000), kStickMax);
    EXPECT_EQ(scaleStick(r, -5000), kStickMin);
}

TEST(Calibrate, DeadzoneAroundTheCentre) {
    Calibration c = cal(0, 4000);
    c.deadzone = 100;
    AxisRange r = resolveRange(c, nullptr, false, false);
    EXPECT_EQ(scaleStick(r, 2100), 0);
    EXPECT_EQ(scaleStick(r, 1900), 0);
    EXPECT_GT(scaleStick(r, 2101), 0);
    EXPECT_LT(scaleStick(r, 1899), 0);
    EXPECT_EQ(scaleStick(r, 4000), kStickMax);  // the full range is still reached
    EXPECT_EQ(scaleStick(r, 0), kStickMin);
}

TEST(Calibrate, OffCentreStickScalesEachSideOnItsOwn) {
    Calibration c = cal(0, 4000);
    c.center = 1000;
    AxisRange r = resolveRange(c, nullptr, false, false);
    EXPECT_EQ(scaleStick(r, 1000), 0);
    EXPECT_EQ(scaleStick(r, 0), kStickMin);
    EXPECT_EQ(scaleStick(r, 4000), kStickMax);
    EXPECT_EQ(scaleStick(r, 500), -16384);
    EXPECT_EQ(scaleStick(r, 2500), 16384);
}

TEST(Calibrate, InvertedStick) {
    AxisRange r = resolveRange(cal(0, 4000), nullptr, true, false);
    EXPECT_EQ(scaleStick(r, 0), kStickMax);
    EXPECT_EQ(scaleStick(r, 4000), kStickMin);
    EXPECT_EQ(scaleStick(r, 2000), 0);
    EXPECT_EQ(scaleStick(r, 1000), 16384);
}

TEST(Calibrate, MissingValuesComeFromTheNode) {
    AbsInfo node;
    node.min = -2048;
    node.max = 2047;
    node.flat = 64;
    Calibration c;
    c.max = 1800;  // device.json narrows the top only
    AxisRange r = resolveRange(c, &node, false, false);
    EXPECT_EQ(r.min, -2048);
    EXPECT_EQ(r.max, 1800);
    EXPECT_EQ(r.deadzone, 64);
    EXPECT_EQ(scaleStick(r, 1800), kStickMax);
}

TEST(Calibrate, FlatIsTheDeadzoneWhenNoneIsGiven) {
    Calibration c = cal(0, 100);
    c.flat = 10;
    AxisRange r = resolveRange(c, nullptr, false, false);
    EXPECT_EQ(r.deadzone, 10);
    c.deadzone = 3;
    EXPECT_EQ(resolveRange(c, nullptr, false, false).deadzone, 3);
}

TEST(Calibrate, UnknownRangePassesThroughClamped) {
    AxisRange r = resolveRange(Calibration{}, nullptr, false, false);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(scaleStick(r, 1234), 1234);
    EXPECT_EQ(scaleStick(r, 99999), kStickMax);
    EXPECT_EQ(scaleTrigger(r, 300), kTriggerMax);
}

TEST(Calibrate, Trigger) {
    Calibration c = cal(100, 1100);
    AxisRange r = resolveRange(c, nullptr, false, true);
    EXPECT_EQ(r.center, 100);
    EXPECT_EQ(scaleTrigger(r, 100), kTriggerMin);
    EXPECT_EQ(scaleTrigger(r, 50), kTriggerMin);
    EXPECT_EQ(scaleTrigger(r, 1100), kTriggerMax);
    EXPECT_EQ(scaleTrigger(r, 600), 128);
    AxisRange inv = resolveRange(c, nullptr, true, true);
    EXPECT_EQ(scaleTrigger(inv, 1100), kTriggerMin);
    EXPECT_EQ(scaleTrigger(inv, 100), kTriggerMax);
}

TEST(Calibrate, TriggerDeadzoneAboveRest) {
    Calibration c = cal(0, 1000);
    c.deadzone = 100;
    AxisRange r = resolveRange(c, nullptr, false, true);
    EXPECT_EQ(scaleTrigger(r, 100), kTriggerMin);
    EXPECT_GT(scaleTrigger(r, 110), kTriggerMin);
    EXPECT_EQ(scaleTrigger(r, 1000), kTriggerMax);
}

TEST(Calibrate, DegenerateRangeIsNotValid) {
    AxisRange r = resolveRange(cal(5, 5), nullptr, false, false);
    EXPECT_FALSE(r.valid);
}
