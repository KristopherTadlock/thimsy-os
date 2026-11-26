// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Merger.h"

#include <linux/input.h>

#include <memory>

#include "gtest/gtest.h"
#include "padd_test_util.h"

using namespace padd;
using padd_test::feed;
using padd_test::Strings;

namespace {

// Four sources: buttons, a home key, an analog stick and trigger node, a vibrator.
const char* kSpec = R"({"pad": {
  "producer": "padd",
  "sources": [
    {"id": "keys", "role": "buttons", "match": {"name": "gpio-keys-control"}},
    {"id": "home", "role": "buttons", "match": {"name": "adc-keys-home"}, "required": false},
    {"id": "sticks", "role": "sticks", "match": {"name": "adc-joystick"}},
    {"id": "vib", "role": "rumble", "match": {"name": "pwm-vibrator"}}
  ],
  "buttons": [
    {"source": "keys", "code": "BTN_EAST", "to": "BTN_SOUTH"},
    {"source": "keys", "code": "BTN_SOUTH", "to": "BTN_EAST"},
    {"source": "keys", "code": "BTN_TL", "to": "BTN_TL"},
    {"source": "keys", "code": "BTN_START", "to": "BTN_START"},
    {"source": "keys", "code": "BTN_DPAD_UP", "to": "BTN_DPAD_UP"},
    {"source": "keys", "code": "BTN_DPAD_DOWN", "to": "BTN_DPAD_DOWN"},
    {"source": "keys", "code": "BTN_DPAD_LEFT", "to": "BTN_DPAD_LEFT"},
    {"source": "keys", "code": "BTN_DPAD_RIGHT", "to": "BTN_DPAD_RIGHT"},
    {"source": "keys", "code": "BTN_C", "to": "KEY_VOLUMEUP"},
    {"source": "home", "code": "BTN_BACK", "to": "BTN_MODE"},
    {"source": "home", "code": "BTN_SELECT", "to": "BTN_START"}
  ],
  "sticks": [
    {"name": "left", "source": "sticks",
     "x": {"code": "ABS_X", "to": "ABS_X", "calibration": {"min": 0, "max": 4000, "deadzone": 100}},
     "y": {"code": "ABS_Y", "to": "ABS_Y", "invert": true}}
  ],
  "triggers": [
    {"source": "sticks", "code": "ABS_Z", "to": "ABS_Z", "calibration": {"min": 100, "max": 1100}}
  ],
  "rumble": {"source": "vib"},
  "hotkey": {"button": "BTN_MODE"}
}})";

enum { kKeys = 0, kHome = 1, kSticks = 2, kVib = 3 };

PadSpec spec() {
    PadSpec s;
    std::string err;
    EXPECT_TRUE(padd_test::parseSpec(kSpec, &s, &err)) << err;
    return s;
}

std::vector<std::optional<SourceCaps>> caps() {
    std::vector<std::optional<SourceCaps>> c(4);
    SourceCaps st;
    AbsInfo y;
    y.min = -1000;
    y.max = 1000;
    st.abs[ABS_X] = AbsInfo{};
    st.abs[ABS_Y] = y;
    st.abs[ABS_Z] = AbsInfo{};
    c[kSticks] = st;
    return c;
}

class MergerTest : public ::testing::Test {
 protected:
    void SetUp() override { m.reset(new Merger(spec(), caps())); }
    std::unique_ptr<Merger> m;
};

}  // namespace

TEST_F(MergerTest, Layout) {
    const Layout& l = m->layout();
    EXPECT_EQ(l.padKeys, (std::set<uint16_t>{BTN_SOUTH, BTN_EAST, BTN_TL, BTN_START, BTN_MODE}));
    std::set<uint16_t> abs;
    for (const auto& a : l.padAbs) abs.insert(a.first);
    EXPECT_EQ(abs, (std::set<uint16_t>{ABS_X, ABS_Y, ABS_Z, ABS_HAT0X, ABS_HAT0Y}));
    EXPECT_EQ(l.padAbs.at(ABS_X).min, kStickMin);
    EXPECT_EQ(l.padAbs.at(ABS_Z).max, kTriggerMax);
    EXPECT_EQ(l.padAbs.at(ABS_HAT0X).min, -1);
    EXPECT_EQ(l.keysKeys, (std::set<uint16_t>{KEY_VOLUMEUP}));
    EXPECT_TRUE(l.ff);
}

TEST_F(MergerTest, NoStickStillDeclaresXAndY) {
    PadSpec s;
    std::string err;
    ASSERT_TRUE(padd_test::parseSpec(
        R"({"pad": {"producer": "padd", "sources": [{"id": "k", "role": "buttons", "match": {"name": "k"}}],
            "buttons": [{"source": "k", "code": "BTN_A", "to": "BTN_SOUTH"}]}})",
        &s, &err));
    Merger x(s, {std::nullopt});
    EXPECT_EQ(x.layout().padAbs.size(), 2u);
    EXPECT_TRUE(x.layout().padAbs.count(ABS_X));
    EXPECT_FALSE(x.layout().padAbs.count(ABS_HAT0X));
    EXPECT_FALSE(x.layout().ff);
    EXPECT_TRUE(x.layout().keysKeys.empty());
}

TEST_F(MergerTest, RemapAndBatchUntilSyn) {
    std::vector<OutEvent> out;
    SourceEvent e;
    e.source = kKeys;
    e.type = EV_KEY;
    e.code = BTN_EAST;
    e.value = 1;
    m->process(e, &out);
    EXPECT_TRUE(out.empty());  // nothing before the source's SYN_REPORT
    e.type = EV_SYN;
    e.code = SYN_REPORT;
    e.value = 0;
    m->process(e, &out);
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad BTN_SOUTH 1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_SOUTH, 1}, {EV_KEY, BTN_EAST, 0}}),
              (Strings{"pad BTN_SOUTH 0", "pad BTN_EAST 1", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, UnmappedCodesAndOtherEventsAreDropped) {
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_Z, 1}, {EV_MSC, MSC_SCAN, 4}}), Strings{});
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}, {EV_KEY, BTN_TL, 2}}),
              (Strings{"pad BTN_TL 1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_TL, 2}}), Strings{});  // autorepeat changes nothing
}

TEST_F(MergerTest, UnknownSourceIsIgnored) {
    EXPECT_EQ(feed(*m, 7, {{EV_KEY, BTN_EAST, 1}}), Strings{});
    EXPECT_EQ(feed(*m, -1, {{EV_KEY, BTN_EAST, 1}}), Strings{});
}

TEST_F(MergerTest, TwoSourcesOneTarget) {
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_START, 1}}), (Strings{"pad BTN_START 1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kHome, {{EV_KEY, BTN_SELECT, 1}}), Strings{});  // already shown pressed
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_START, 0}}), Strings{});   // still held by the other
    EXPECT_EQ(feed(*m, kHome, {{EV_KEY, BTN_SELECT, 0}}), (Strings{"pad BTN_START 0", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, DpadBecomesTheHat) {
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_DPAD_UP, 1}}), (Strings{"pad ABS_HAT0Y -1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_DPAD_RIGHT, 1}}), (Strings{"pad ABS_HAT0X 1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_DPAD_DOWN, 1}}), (Strings{"pad ABS_HAT0Y 0", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_DPAD_UP, 0}}), (Strings{"pad ABS_HAT0Y 1", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, KeyTargetGoesToTheKeyboard) {
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_C, 1}, {EV_KEY, BTN_TL, 1}}),
              (Strings{"pad BTN_TL 1", "pad SYN_REPORT 0", "keys KEY_VOLUMEUP 1", "keys SYN_REPORT 0"}));
}

TEST_F(MergerTest, StickCalibrationAndInversion) {
    EXPECT_EQ(feed(*m, kSticks, {{EV_ABS, ABS_X, 4000}, {EV_ABS, ABS_Y, -1000}}),
              (Strings{"pad ABS_X 32767", "pad ABS_Y 32767", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kSticks, {{EV_ABS, ABS_X, 2050}}), (Strings{"pad ABS_X 0", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kSticks, {{EV_ABS, ABS_X, 9000}, {EV_ABS, ABS_Y, 1000}}),
              (Strings{"pad ABS_X 32767", "pad ABS_Y -32768", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kSticks, {{EV_ABS, ABS_Z, 1100}}), (Strings{"pad ABS_Z 255", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kSticks, {{EV_ABS, ABS_RX, 5}}), Strings{});  // not mapped
}

TEST_F(MergerTest, InterceptReleasesHeldButtonsInOneBatch) {
    feed(*m, kKeys, {{EV_KEY, BTN_EAST, 1}, {EV_KEY, BTN_DPAD_LEFT, 1}, {EV_KEY, BTN_C, 1}});
    feed(*m, kSticks, {{EV_ABS, ABS_X, 0}, {EV_ABS, ABS_Z, 600}});
    std::vector<OutEvent> out;
    m->setIntercept(true, &out);
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad BTN_SOUTH 0", "pad ABS_X 0", "pad ABS_Z 0", "pad ABS_HAT0X 0",
                                            "pad SYN_REPORT 0", "keys KEY_VOLUMEUP 0", "keys SYN_REPORT 0",
                                            "tap ABS_X -32768", "tap ABS_Z 128", "tap SYN_REPORT 0"}));
    EXPECT_TRUE(m->intercept());
    // Held buttons stay suppressed for the menu too; new presses go to the tap only.
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}}), (Strings{"tap BTN_TL 1", "tap SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_EAST, 0}}), Strings{});
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_EAST, 1}}), (Strings{"tap BTN_SOUTH 1", "tap SYN_REPORT 0"}));
    out.clear();
    m->setIntercept(true, &out);
    EXPECT_TRUE(out.empty());  // already on
}

TEST_F(MergerTest, InterceptOffResumesFromTheCurrentState) {
    feed(*m, kSticks, {{EV_ABS, ABS_X, 4000}});
    std::vector<OutEvent> out;
    m->setIntercept(true, &out);
    feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}});
    feed(*m, kSticks, {{EV_ABS, ABS_X, 0}});
    out.clear();
    m->setIntercept(false, &out);
    // The menu sees everything released; the game sees the stick where it is now, and not the
    // button still held from the menu.
    EXPECT_EQ(padd_test::fmt(out), (Strings{"tap BTN_TL 0", "tap ABS_X 0", "tap SYN_REPORT 0",
                                            "pad ABS_X -32768", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_TL, 0}}), Strings{});
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}}), (Strings{"pad BTN_TL 1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(m->shown(EV_KEY, BTN_TL), 1);
}

TEST_F(MergerTest, AxisReturnsToCentreWhenItsSourceGoes) {
    feed(*m, kSticks, {{EV_ABS, ABS_X, 4000}, {EV_ABS, ABS_Z, 1100}});
    feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}});
    std::vector<OutEvent> out;
    m->dropSource(kSticks, &out);
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad ABS_X 0", "pad ABS_Z 0", "pad SYN_REPORT 0"}));
    out.clear();
    m->dropSource(kKeys, &out);
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad BTN_TL 0", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, SynDroppedWaitsForASync) {
    feed(*m, kKeys, {{EV_KEY, BTN_TL, 1}});
    std::vector<OutEvent> out;
    SourceEvent e;
    e.source = kKeys;
    e.type = EV_KEY;
    e.code = BTN_EAST;
    e.value = 1;
    m->process(e, &out);
    e.type = EV_SYN;
    e.code = SYN_DROPPED;
    m->process(e, &out);
    // Everything up to the next SYN_REPORT is discarded, the pending press included.
    EXPECT_EQ(feed(*m, kKeys, {{EV_KEY, BTN_START, 1}}), Strings{});
    EXPECT_TRUE(m->needsSync(kKeys));
    out.clear();
    m->syncSource(kKeys, {BTN_START, BTN_Z}, {}, &out);
    EXPECT_FALSE(m->needsSync(kKeys));
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad BTN_TL 0", "pad BTN_START 1", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, SyncSetsAxesToo) {
    std::vector<OutEvent> out;
    m->syncSource(kSticks, {}, {{ABS_X, 0}, {ABS_Y, 0}, {ABS_Z, 100}}, &out);
    EXPECT_EQ(padd_test::fmt(out), (Strings{"pad ABS_X -32768", "pad SYN_REPORT 0"}));
}

TEST_F(MergerTest, AttachSourceGivesLateRanges) {
    Merger x(spec(), std::vector<std::optional<SourceCaps>>(4));
    // ABS_Y has no calibration and no node range yet: passes through.
    EXPECT_EQ(feed(x, kSticks, {{EV_ABS, ABS_Y, 500}}), (Strings{"pad ABS_Y -500", "pad SYN_REPORT 0"}));
    SourceCaps c;
    AbsInfo y;
    y.min = -1000;
    y.max = 1000;
    c.abs[ABS_Y] = y;
    x.attachSource(kSticks, c);
    EXPECT_EQ(feed(x, kSticks, {{EV_ABS, ABS_Y, 1000}}), (Strings{"pad ABS_Y -32768", "pad SYN_REPORT 0"}));
}

namespace {
const char* kJoypad = R"({"pad": {
  "producer": "kernel-joypad",
  "sources": [{"id": "joy", "role": "buttons+sticks", "match": {"name": "H700 Gamepad"}}],
  "buttons": [],
  "sticks": [{"name": "left", "source": "joy",
              "x": {"code": "ABS_X", "to": "ABS_X"}, "y": {"code": "ABS_Y", "to": "ABS_Y"}}]
}})";
}

TEST(MergerPassThrough, KeysFromTheNodeAndTheHat) {
    PadSpec s;
    std::string err;
    ASSERT_TRUE(padd_test::parseSpec(kJoypad, &s, &err)) << err;
    SourceCaps c;
    c.keys = {BTN_SOUTH, BTN_EAST, BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_TRIGGER_HAPPY1};
    AbsInfo a;
    a.min = -1800;
    a.max = 1800;
    c.abs[ABS_X] = a;
    c.abs[ABS_Y] = a;
    Merger m(s, {c});
    EXPECT_EQ(m.layout().padKeys, (std::set<uint16_t>{BTN_SOUTH, BTN_EAST, BTN_TRIGGER_HAPPY1}));
    EXPECT_TRUE(m.layout().padAbs.count(ABS_HAT0Y));
    EXPECT_EQ(feed(m, 0, {{EV_KEY, BTN_EAST, 1}, {EV_KEY, BTN_DPAD_UP, 1}, {EV_ABS, ABS_X, 1800}}),
              (Strings{"pad BTN_EAST 1", "pad ABS_X 32767", "pad ABS_HAT0Y -1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(m, 0, {{EV_KEY, BTN_NORTH, 1}}), Strings{});  // not a capability of the node
}

TEST(MergerPassThrough, UnknownNodeGetsTheStandardSet) {
    PadSpec s;
    std::string err;
    ASSERT_TRUE(padd_test::parseSpec(kJoypad, &s, &err)) << err;
    Merger m(s, {std::nullopt});
    EXPECT_EQ(m.layout().padKeys.size(), 13u);
    EXPECT_TRUE(m.layout().padKeys.count(BTN_MODE));
    EXPECT_TRUE(m.layout().padAbs.count(ABS_HAT0X));
}

TEST(MergerPassThrough, NodeHatPassesThrough) {
    PadSpec s;
    std::string err;
    ASSERT_TRUE(padd_test::parseSpec(kJoypad, &s, &err)) << err;
    SourceCaps c;
    c.keys = {BTN_SOUTH};
    AbsInfo h;
    h.min = -1;
    h.max = 1;
    c.abs[ABS_HAT0X] = h;
    c.abs[ABS_X] = AbsInfo{};
    c.abs[ABS_Y] = AbsInfo{};
    Merger m(s, {c});
    EXPECT_EQ(feed(m, 0, {{EV_ABS, ABS_HAT0X, -1}}), (Strings{"pad ABS_HAT0X -1", "pad SYN_REPORT 0"}));
    EXPECT_EQ(feed(m, 0, {{EV_ABS, ABS_HAT0X, 0}}), (Strings{"pad ABS_HAT0X 0", "pad SYN_REPORT 0"}));
}
