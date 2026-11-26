// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Rumble.h"

#include <errno.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"

using namespace padd;

namespace {

constexpr int64_t kMs = 1000000;

std::string fmt(const VibCommand& c) {
    if (!c.on) return "off";
    return "on " + std::to_string(c.strong) + "/" + std::to_string(c.weak) + " for " + std::to_string(c.durationMs);
}

std::vector<std::string> fmt(const std::vector<VibCommand>& cs) {
    std::vector<std::string> out;
    for (const VibCommand& c : cs) out.push_back(fmt(c));
    return out;
}

RumbleEffect effect(int id, uint16_t strong, uint16_t weak, uint32_t lengthMs, uint32_t delayMs = 0) {
    RumbleEffect e;
    e.id = id;
    e.strong = strong;
    e.weak = weak;
    e.lengthMs = lengthMs;
    e.delayMs = delayMs;
    return e;
}

using Strings = std::vector<std::string>;

}  // namespace

TEST(Rumble, UploadAndPlayMapToOneCommand) {
    Rumble r;
    std::vector<VibCommand> c;
    EXPECT_EQ(r.upload(effect(0, 0x8000, 0x4000, 200), 0, &c), 0);
    EXPECT_TRUE(c.empty());  // uploading does not start it
    r.play(0, 1, 10 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 32768/16384 for 200"}));
    EXPECT_EQ(r.nextDeadlineNs(), 210 * kMs);
    c.clear();
    r.tick(209 * kMs, &c);
    EXPECT_TRUE(c.empty());
    r.tick(210 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"off"}));
    EXPECT_EQ(r.nextDeadlineNs(), -1);
}

TEST(Rumble, StopAndErase) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(1, 0xffff, 0, 0), 0, &c);
    r.play(1, 1, 0, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 65535/0 for 0"}));  // length 0: until stopped
    EXPECT_EQ(r.nextDeadlineNs(), -1);
    c.clear();
    r.play(1, 0, 5 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"off"}));
    c.clear();
    r.play(1, 1, 6 * kMs, &c);
    EXPECT_EQ(r.erase(1, 7 * kMs, &c), 0);
    EXPECT_EQ(fmt(c), (Strings{"on 65535/0 for 0", "off"}));
    EXPECT_EQ(r.erase(1, 8 * kMs, &c), -EINVAL);
    c.clear();
    r.play(1, 1, 9 * kMs, &c);  // erased: ignored
    EXPECT_TRUE(c.empty());
}

TEST(Rumble, BadIdsAreRefused) {
    Rumble r(4);
    std::vector<VibCommand> c;
    EXPECT_EQ(r.upload(effect(4, 1, 1, 1), 0, &c), -EINVAL);
    EXPECT_EQ(r.upload(effect(-1, 1, 1, 1), 0, &c), -EINVAL);
    EXPECT_EQ(r.upload(effect(3, 1, 1, 1), 0, &c), 0);
}

TEST(Rumble, DelayAndRepeats) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(0, 1000, 0, 100, 50), 0, &c);
    r.play(0, 2, 0, &c);
    EXPECT_TRUE(c.empty());  // the delay first
    EXPECT_EQ(r.nextDeadlineNs(), 50 * kMs);
    std::vector<int64_t> times;
    for (int i = 0; i < 10 && r.nextDeadlineNs() >= 0; ++i) {
        int64_t d = r.nextDeadlineNs();
        times.push_back(d / kMs);
        r.tick(d, &c);
    }
    EXPECT_EQ(times, (std::vector<int64_t>{50, 150, 200, 300}));
    EXPECT_EQ(fmt(c), (Strings{"on 1000/0 for 100", "off", "on 1000/0 for 100", "off"}));
}

TEST(Rumble, EffectsSumAndClamp) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(0, 0xc000, 0x1000, 0), 0, &c);
    r.upload(effect(1, 0xc000, 0x2000, 300), 0, &c);
    r.play(0, 1, 0, &c);
    r.play(1, 1, 0, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 49152/4096 for 0", "on 65535/12288 for 300"}));
    c.clear();
    r.tick(300 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 49152/4096 for 0"}));
}

TEST(Rumble, UpdateWhilePlayingKeepsPlaying) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(0, 0x1000, 0, 0), 0, &c);
    r.play(0, 1, 0, &c);
    c.clear();
    r.upload(effect(0, 0x2000, 0, 0), 5 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 8192/0 for 0"}));
}

TEST(Rumble, Gain) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(0, 0xffff, 0x8000, 0), 0, &c);
    r.setGain(0x8000, 0, &c);
    EXPECT_TRUE(c.empty());
    r.play(0, 1, 0, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 32768/16384 for 0"}));
    c.clear();
    r.setGain(0, 1, &c);
    EXPECT_EQ(fmt(c), (Strings{"off"}));
}

TEST(Rumble, SameSumAcrossAHandOverIsRenewed) {
    // Effect 0 ends at 100 ms exactly when effect 1 (same strength) starts: the motor's own
    // timer from the first command would stop it, so the command is sent again.
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(0, 0x4000, 0, 100), 0, &c);
    r.upload(effect(1, 0x4000, 0, 100, 100), 0, &c);
    r.play(0, 1, 0, &c);
    r.play(1, 1, 0, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 16384/0 for 100"}));
    c.clear();
    r.tick(100 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 16384/0 for 100"}));
    c.clear();
    r.tick(200 * kMs, &c);
    EXPECT_EQ(fmt(c), (Strings{"off"}));
}

TEST(Rumble, StopAllKeepsEffects) {
    Rumble r;
    std::vector<VibCommand> c;
    r.upload(effect(2, 100, 100, 0), 0, &c);
    r.play(2, 1, 0, &c);
    c.clear();
    r.stopAll(1, &c);
    EXPECT_EQ(fmt(c), (Strings{"off"}));
    c.clear();
    r.play(2, 1, 2, &c);
    EXPECT_EQ(fmt(c), (Strings{"on 100/100 for 0"}));
}
