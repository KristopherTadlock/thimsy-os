// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/PadSpec.h"

#include <linux/input.h>

#include "gtest/gtest.h"
#include "padd_test_util.h"

using namespace padd;
using padd_test::parseSpec;

namespace {

PadSpec load(const std::string& rel) {
    PadSpec s;
    std::string err;
    std::vector<std::string> warnings;
    bool ok = loadPadSpec(padd_test::linuxDir() + "/" + rel, &s, &err, &warnings);
    EXPECT_TRUE(ok) << err;
    for (const std::string& w : warnings) ADD_FAILURE() << rel << ": warning: " << w;
    return s;
}

std::string padJson(const std::string& body) { return "{\"pad\": {" + body + "}}"; }

const char* kOneSource =
    "\"producer\": \"padd\", \"sources\": [{\"id\": \"k\", \"role\": \"buttons\", "
    "\"match\": {\"name\": \"keys\"}}]";

std::string parseError(const std::string& body) {
    PadSpec s;
    std::string err;
    EXPECT_FALSE(parseSpec(padJson(body), &s, &err)) << body;
    return err;
}

}  // namespace

TEST(PadSpec, ArcDThreeSources) {
    PadSpec s = load("board/rk3566/devices/anbernic-rg-arc-d.json");
    EXPECT_EQ(s.producer, Producer::Padd);
    ASSERT_EQ(s.sources.size(), 3u);
    EXPECT_EQ(s.sources[0].id, "gpio-keys-control");
    EXPECT_EQ(s.sources[0].match.name, "gpio-keys-control");
    ASSERT_TRUE(s.sources[0].match.phys.has_value());
    EXPECT_EQ(*s.sources[0].match.phys, "gpio-keys/input0");
    EXPECT_TRUE(s.sources[0].required);
    EXPECT_FALSE(s.sources[1].required);
    EXPECT_EQ(s.sources[2].role, Role::Rumble);
    EXPECT_FALSE(s.sources[2].grab);
    EXPECT_EQ(s.rumbleSource, 2);
    EXPECT_EQ(s.buttons.size(), 17u);
    EXPECT_EQ(s.faceButtons, 6);
    EXPECT_EQ(s.faceLayout, "saturn");
    EXPECT_EQ(s.hotkey, BTN_MODE);
    EXPECT_EQ(s.quitCombo, (std::vector<uint16_t>{BTN_MODE, BTN_START}));
    EXPECT_FALSE(s.passThrough);
    EXPECT_TRUE(s.axes.empty());
    // South and East swapped, C and Z to the stick clicks, the home key to BTN_MODE.
    auto to = [&](int source, uint16_t code) {
        for (const ButtonMap& b : s.buttons) {
            if (b.source == source && b.code == code) return static_cast<int>(b.to.code);
        }
        return -1;
    };
    EXPECT_EQ(to(0, BTN_EAST), BTN_SOUTH);
    EXPECT_EQ(to(0, BTN_SOUTH), BTN_EAST);
    EXPECT_EQ(to(0, BTN_C), BTN_THUMBR);
    EXPECT_EQ(to(0, BTN_Z), BTN_THUMBL);
    EXPECT_EQ(to(1, BTN_BACK), BTN_MODE);
}

TEST(PadSpec, ArcSAndQemuAndTheGeneratedH700) {
    PadSpec arcS = load("board/rk3566/devices/anbernic-rg-arc-s.json");
    EXPECT_EQ(arcS.sources.size(), 3u);
    PadSpec q = load("board/qemu-aarch64-virt/devices/qemu-virt.json");
    ASSERT_EQ(q.sources.size(), 1u);
    EXPECT_EQ(q.sources[0].match.name, "QEMU Virtio Keyboard");
    EXPECT_EQ(q.buttons[0].code, KEY_ENTER);
    EXPECT_EQ(q.buttons[0].to.code, BTN_SOUTH);
    PadSpec h = load("tests/fixtures/expected/generated/h700/devices/anbernic-rg40xx-h.json");
    EXPECT_EQ(h.producer, Producer::KernelJoypad);
    EXPECT_TRUE(h.passThrough);
    EXPECT_EQ(h.axes.size(), 4u);
    EXPECT_TRUE(h.sources[0].required);  // the schema's default
}

TEST(PadSpec, TargetKinds) {
    EXPECT_EQ(targetFor(BTN_SOUTH).kind, TargetKind::PadKey);
    EXPECT_EQ(targetFor(BTN_THUMBR).kind, TargetKind::PadKey);
    EXPECT_EQ(targetFor(BTN_TRIGGER_HAPPY1).kind, TargetKind::PadKey);
    EXPECT_EQ(targetFor(BTN_DPAD_LEFT).kind, TargetKind::Hat);
    EXPECT_EQ(targetFor(KEY_VOLUMEUP).kind, TargetKind::KeysKey);
    EXPECT_EQ(targetFor(KEY_ENTER).kind, TargetKind::KeysKey);
    EXPECT_EQ(targetFor(KEY_OK).kind, TargetKind::KeysKey);
}

TEST(PadSpec, KeyTargetsAndDefaults) {
    PadSpec s;
    std::string err;
    ASSERT_TRUE(parseSpec(padJson(std::string(kOneSource) +
                                  ", \"buttons\": [{\"source\": \"k\", \"code\": \"KEY_A\", \"to\": \"KEY_ENTER\"},"
                                  " {\"source\": \"k\", \"code\": \"BTN_A\", \"to\": \"BTN_SOUTH\"}]"),
                          &s, &err))
        << err;
    EXPECT_TRUE(s.sources[0].required);
    EXPECT_TRUE(s.sources[0].grab);
    EXPECT_TRUE(s.sources[0].hide);
    EXPECT_EQ(s.buttons[0].to.kind, TargetKind::KeysKey);
    EXPECT_EQ(s.buttons[1].code, BTN_SOUTH);  // the alias reads as its number
    EXPECT_EQ(s.rumbleSource, -1);
    EXPECT_EQ(s.hotkey, -1);
}

TEST(PadSpec, Errors) {
    std::string k = kOneSource;
    EXPECT_NE(parseError("\"producer\": \"magic\", \"sources\": [], \"buttons\": []").find("producer"),
              std::string::npos);
    EXPECT_NE(parseError(k + ", \"buttons\": [{\"source\": \"x\", \"code\": \"BTN_A\", \"to\": \"BTN_SOUTH\"}]")
                  .find("unknown 'x'"),
              std::string::npos);
    EXPECT_NE(parseError(k + ", \"buttons\": [{\"source\": \"k\", \"code\": \"BTN_NOPE\", \"to\": \"BTN_SOUTH\"}]")
                  .find("BTN_NOPE"),
              std::string::npos);
    // BTN_C is a real code but not a normalised pad button.
    EXPECT_NE(parseError(k + ", \"buttons\": [{\"source\": \"k\", \"code\": \"BTN_A\", \"to\": \"BTN_C\"}]")
                  .find("not a pad button"),
              std::string::npos);
    EXPECT_NE(parseError(k + ", \"buttons\": [{\"source\": \"k\", \"code\": \"BTN_A\", \"to\": \"BTN_SOUTH\"},"
                             " {\"source\": \"k\", \"code\": \"BTN_SOUTH\", \"to\": \"BTN_EAST\"}]")
                  .find("mapped twice"),
              std::string::npos);
    EXPECT_NE(parseError("\"producer\": \"padd\", \"sources\": [{\"id\": \"k\", \"role\": \"buttons\", "
                         "\"match\": {\"name\": \"a\"}}, {\"id\": \"k\", \"role\": \"buttons\", "
                         "\"match\": {\"name\": \"b\"}}], \"buttons\": []")
                  .find("duplicate"),
              std::string::npos);
    EXPECT_NE(parseError("\"producer\": \"kernel-joypad\", \"sources\": [{\"id\": \"a\", \"role\": \"buttons\", "
                         "\"match\": {\"name\": \"a\"}}, {\"id\": \"b\", \"role\": \"sticks\", "
                         "\"match\": {\"name\": \"b\"}}], \"buttons\": []")
                  .find("exactly one"),
              std::string::npos);
    EXPECT_NE(parseError(k + ", \"buttons\": [], \"rumble\": {\"source\": \"v\"}").find("rumble"),
              std::string::npos);
    EXPECT_NE(parseError("\"producer\": \"padd\", \"sources\": [{\"id\": \"s\", \"role\": \"sticks\", "
                         "\"match\": {\"name\": \"a\"}}], \"buttons\": [], \"sticks\": ["
                         "{\"name\": \"left\", \"source\": \"s\", \"x\": {\"code\": \"ABS_X\", \"to\": \"ABS_X\"},"
                         " \"y\": {\"code\": \"ABS_Y\", \"to\": \"ABS_X\"}}]")
                  .find("target twice"),
              std::string::npos);
    EXPECT_NE(parseError("\"producer\": \"padd\", \"sources\": [{\"id\": \"s\", \"role\": \"sticks\", "
                         "\"match\": {\"name\": \"a\"}}], \"buttons\": [], \"triggers\": ["
                         "{\"source\": \"s\", \"code\": \"ABS_Z\", \"to\": \"ABS_X\"}]")
                  .find("not allowed"),
              std::string::npos);
    EXPECT_NE(parseError(k + ", \"buttons\": [], \"hotkey\": {\"button\": \"KEY_NOPE\"}").find("hotkey"),
              std::string::npos);
    PadSpec s;
    std::string err;
    EXPECT_FALSE(parseSpec("{\"id\": \"x\"}", &s, &err));
    EXPECT_NE(err.find("no pad section"), std::string::npos);
}

TEST(PadSpec, Warnings) {
    PadSpec s;
    std::string err;
    std::vector<std::string> w;
    ASSERT_TRUE(parseSpec(padJson("\"producer\": \"padd\", \"sources\": ["
                                  "{\"id\": \"k\", \"role\": \"buttons\", \"match\": {\"name\": \"keys\"}},"
                                  "{\"id\": \"idle\", \"role\": \"buttons\", \"match\": {\"name\": \"idle\"}},"
                                  "{\"id\": \"v\", \"role\": \"rumble\", \"match\": {\"name\": \"vib\"}}],"
                                  "\"buttons\": [{\"source\": \"k\", \"code\": \"BTN_A\", \"to\": \"BTN_SOUTH\"}],"
                                  "\"hotkey\": {\"button\": \"BTN_MODE\"}"),
                          &s, &err, &w))
        << err;
    ASSERT_EQ(w.size(), 3u);
    EXPECT_NE(w[0].find("BTN_MODE"), std::string::npos);
    EXPECT_NE(w[1].find("idle"), std::string::npos);
    EXPECT_NE(w[2].find("'v'"), std::string::npos);
}

TEST(PadSpec, Matching) {
    Match m;
    m.name = "gpio-keys-control";
    m.phys = "gpio-keys/input0";
    NodeInfo n;
    n.name = "gpio-keys-control";
    n.phys = "gpio-keys/input0";
    EXPECT_TRUE(matches(m, n));
    n.phys = "gpio-keys/input1";
    EXPECT_FALSE(matches(m, n));
    m.phys.reset();
    EXPECT_TRUE(matches(m, n));
    m.vendor = 0x45e;
    n.vendor = 0x45e;
    EXPECT_TRUE(matches(m, n));
    n.vendor = 0x46d;
    EXPECT_FALSE(matches(m, n));
    m.vendor = -1;
    m.uniq = "";
    n.uniq = "abc";
    EXPECT_FALSE(matches(m, n));
    n.name = "Gpio-keys-control";
    n.uniq = "";
    EXPECT_FALSE(matches(m, n));  // names compare exactly
}

TEST(PadSpec, PickSourceChecksCapabilitiesAndIgnoresOthers) {
    PadSpec s = load("board/rk3566/devices/anbernic-rg-arc-d.json");
    std::vector<bool> taken(3, false);
    NodeInfo keys;
    keys.name = "gpio-keys-control";
    keys.phys = "gpio-keys/input0";
    SourceCaps kc;
    kc.keys = {BTN_SOUTH, BTN_EAST, BTN_DPAD_UP};
    EXPECT_EQ(pickSource(s, keys, kc, taken), 0);
    taken[0] = true;
    std::string why;
    EXPECT_EQ(pickSource(s, keys, kc, taken, &why), -1);  // a second node with the same name
    EXPECT_EQ(why, "matches no source");

    NodeInfo vib;
    vib.name = "pwm-vibrator";
    SourceCaps none;
    EXPECT_EQ(pickSource(s, vib, none, taken, &why), -1);
    EXPECT_EQ(why, "pwm-vibrator: no FF_RUMBLE");
    SourceCaps ff;
    ff.ff = {FF_RUMBLE};
    EXPECT_EQ(pickSource(s, vib, ff, taken), 2);

    NodeInfo home;
    home.name = "adc-keys-home";
    SourceCaps wrongKeys;
    wrongKeys.keys = {KEY_VOLUMEUP};
    EXPECT_EQ(pickSource(s, home, wrongKeys, taken, &why), -1);
    EXPECT_EQ(why, "adc-keys-home: none of its mapped buttons");

    // An external pad: named nowhere in device.json.
    NodeInfo xbox;
    xbox.name = "Microsoft X-Box 360 pad";
    xbox.bus = BUS_USB;
    xbox.vendor = 0x045e;
    xbox.product = 0x028e;
    SourceCaps xc;
    xc.keys = {BTN_SOUTH, BTN_EAST};
    xc.ff = {FF_RUMBLE};
    EXPECT_EQ(pickSource(s, xbox, xc, {false, false, false}), -1);
}

TEST(PadSpec, CapsForSticks) {
    PadSpec s = load("tests/fixtures/expected/generated/h700/devices/anbernic-rg40xx-h.json");
    SourceCaps c;
    c.keys = {BTN_SOUTH};
    c.abs[ABS_X] = AbsInfo{};
    c.abs[ABS_Y] = AbsInfo{};
    c.abs[ABS_RX] = AbsInfo{};
    EXPECT_EQ(checkCaps(s, 0, c), "no ABS_RY");
    c.abs[ABS_RY] = AbsInfo{};
    EXPECT_EQ(checkCaps(s, 0, c), "");
    c.keys.clear();
    EXPECT_EQ(checkCaps(s, 0, c), "no EV_KEY codes");
}
