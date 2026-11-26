// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The generated name table against the kernel header it came from.
#include <linux/input.h>

#include "core/InputCodes.h"
#include "gtest/gtest.h"

using namespace padd;

#define KEY_ROW(n) {#n, n}
struct Row {
    const char* name;
    int value;
};

// Every name a device.json or a trace in this tree uses, plus the gamepad layout.
static const Row kKeys[] = {
    KEY_ROW(BTN_SOUTH), KEY_ROW(BTN_EAST), KEY_ROW(BTN_NORTH), KEY_ROW(BTN_WEST), KEY_ROW(BTN_C),
    KEY_ROW(BTN_Z), KEY_ROW(BTN_TL), KEY_ROW(BTN_TR), KEY_ROW(BTN_TL2), KEY_ROW(BTN_TR2),
    KEY_ROW(BTN_SELECT), KEY_ROW(BTN_START), KEY_ROW(BTN_MODE), KEY_ROW(BTN_THUMBL),
    KEY_ROW(BTN_THUMBR), KEY_ROW(BTN_DPAD_UP), KEY_ROW(BTN_DPAD_DOWN), KEY_ROW(BTN_DPAD_LEFT),
    KEY_ROW(BTN_DPAD_RIGHT), KEY_ROW(BTN_BACK), KEY_ROW(KEY_ENTER), KEY_ROW(KEY_ESC), KEY_ROW(KEY_S),
    KEY_ROW(KEY_A), KEY_ROW(KEY_Q), KEY_ROW(KEY_W), KEY_ROW(KEY_1), KEY_ROW(KEY_2), KEY_ROW(KEY_TAB),
    KEY_ROW(KEY_SPACE), KEY_ROW(KEY_HOME), KEY_ROW(KEY_UP), KEY_ROW(KEY_DOWN), KEY_ROW(KEY_LEFT),
    KEY_ROW(KEY_RIGHT), KEY_ROW(KEY_POWER), KEY_ROW(KEY_VOLUMEUP), KEY_ROW(KEY_VOLUMEDOWN),
    KEY_ROW(KEY_F1), KEY_ROW(BTN_TRIGGER_HAPPY1),
};
static const Row kAbs[] = {
    KEY_ROW(ABS_X),  KEY_ROW(ABS_Y),     KEY_ROW(ABS_Z),     KEY_ROW(ABS_RX),
    KEY_ROW(ABS_RY), KEY_ROW(ABS_RZ),    KEY_ROW(ABS_HAT0X), KEY_ROW(ABS_HAT0Y),
};

TEST(InputCodes, KeyNamesMatchTheHeader) {
    for (const Row& r : kKeys) {
        SCOPED_TRACE(r.name);
        EXPECT_EQ(keyCodeByName(r.name), r.value);
        EXPECT_EQ(eventCodeByName(EV_KEY, r.name), r.value);
    }
}

TEST(InputCodes, AbsNamesMatchTheHeader) {
    for (const Row& r : kAbs) {
        SCOPED_TRACE(r.name);
        EXPECT_EQ(absCodeByName(r.name), r.value);
        EXPECT_EQ(eventCodeName(EV_ABS, r.value), std::string(r.name));
    }
}

TEST(InputCodes, AliasesReadButCanonicalNamesPrint) {
    EXPECT_EQ(keyCodeByName("BTN_A"), BTN_SOUTH);
    EXPECT_EQ(keyCodeByName("BTN_GAMEPAD"), BTN_SOUTH);
    EXPECT_EQ(eventCodeName(EV_KEY, BTN_SOUTH), "BTN_SOUTH");
    EXPECT_EQ(eventCodeName(EV_KEY, BTN_TRIGGER_HAPPY1), "BTN_TRIGGER_HAPPY1");
    EXPECT_EQ(eventCodeName(EV_KEY, BTN_LEFT), "BTN_LEFT");
    EXPECT_EQ(eventCodeName(EV_KEY, KEY_COFFEE), "KEY_COFFEE");
    EXPECT_EQ(keyCodeByName("KEY_SCREENLOCK"), KEY_COFFEE);
}

TEST(InputCodes, TypesAndSyn) {
    EXPECT_EQ(eventTypeByName("EV_KEY"), EV_KEY);
    EXPECT_EQ(eventTypeByName("EV_ABS"), EV_ABS);
    EXPECT_EQ(eventTypeByName("EV_SYN"), EV_SYN);
    EXPECT_EQ(eventTypeByName("3"), EV_ABS);
    EXPECT_EQ(eventTypeByName("0x15"), EV_FF);
    EXPECT_EQ(eventTypeName(EV_FF), "EV_FF");
    EXPECT_EQ(eventCodeByName(EV_SYN, "SYN_DROPPED"), SYN_DROPPED);
    EXPECT_EQ(eventCodeName(EV_SYN, SYN_REPORT), "SYN_REPORT");
    EXPECT_EQ(eventCodeByName(EV_MSC, "MSC_SCAN"), MSC_SCAN);
}

TEST(InputCodes, UnknownAndWrongKind) {
    EXPECT_EQ(keyCodeByName("BTN_NOPE"), -1);
    EXPECT_EQ(keyCodeByName("ABS_X"), -1);
    EXPECT_EQ(absCodeByName("BTN_SOUTH"), -1);
    EXPECT_EQ(eventTypeByName("EV_NOPE"), -1);
    EXPECT_EQ(eventCodeByName(EV_KEY, "304"), 304);
    EXPECT_EQ(eventCodeName(EV_KEY, 0x2ff), "0x2ff");
    EXPECT_EQ(eventTypeName(0x1e), "0x01e");
}

TEST(InputCodes, MaxAndCountAreNotNames) {
    EXPECT_EQ(keyCodeByName("KEY_MAX"), -1);
    EXPECT_EQ(absCodeByName("ABS_CNT"), -1);
}
