// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The messages below follow the kernel's uevent format (lib/kobject_uevent.c) with values shaped
// like an RK3566 handheld's: an SD card partition, a gpio-keys input node, the battery.
#include "thimsy/uevent.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string msg(std::initializer_list<const char*> fields, bool trailingNul = true) {
    std::string s;
    for (const char* f : fields) {
        s += f;
        s.push_back('\0');
    }
    if (!trailingNul) s.pop_back();
    return s;
}

const char kCardDevpath[] = "/devices/platform/fe2b0000.mmc/mmc_host/mmc1/mmc1:aaaa/block/mmcblk1/mmcblk1p5";

}  // namespace

TEST(Uevent, BlockPartitionAdd) {
    const std::string m = msg({"add@/devices/platform/fe2b0000.mmc/mmc_host/mmc1/mmc1:aaaa/block/mmcblk1/mmcblk1p5",
                               "ACTION=add",
                               "DEVPATH=/devices/platform/fe2b0000.mmc/mmc_host/mmc1/mmc1:aaaa/block/mmcblk1/mmcblk1p5",
                               "SUBSYSTEM=block", "MAJOR=179", "MINOR=37", "DEVNAME=mmcblk1p5", "DEVTYPE=partition",
                               "DISKSEQ=9", "PARTN=5", "PARTNAME=games", "SEQNUM=2051"});
    thimsy::Uevent ev;
    std::string err;
    ASSERT_TRUE(thimsy::parseUevent(m.data(), m.size(), &ev, &err)) << err;
    EXPECT_EQ(ev.action, "add");
    EXPECT_EQ(ev.devpath, kCardDevpath);
    EXPECT_EQ(ev.subsystem, "block");
    EXPECT_EQ(ev.devname, "mmcblk1p5");
    EXPECT_EQ(ev.devtype, "partition");
    EXPECT_EQ(ev.major, 179);
    EXPECT_EQ(ev.minor, 37);
    EXPECT_EQ(ev.seqnum, 2051u);
    EXPECT_EQ(ev.get("PARTNAME"), "games");
    EXPECT_EQ(ev.get("PARTN"), "5");
    EXPECT_EQ(ev.get("NOPE", "d"), "d");
}

TEST(Uevent, InputNodeAddWithoutAction) {
    // Some kernels' coldplug ("echo add > uevent") messages carry ACTION; the header always does.
    const std::string m = msg({"add@/devices/platform/gpio-keys-control/input/input2/event2", "SUBSYSTEM=input",
                               "MAJOR=13", "MINOR=66", "DEVNAME=input/event2", "SEQNUM=812"});
    thimsy::Uevent ev;
    ASSERT_TRUE(thimsy::parseUevent(m.data(), m.size(), &ev));
    EXPECT_EQ(ev.action, "add");
    EXPECT_EQ(ev.subsystem, "input");
    EXPECT_EQ(ev.devname, "input/event2");
    EXPECT_EQ(ev.devtype, "");
}

TEST(Uevent, BatteryChangeAndMissingFinalNul) {
    const std::string m = msg({"change@/devices/platform/fdd40000.i2c/i2c-0/0-0020/rk817-battery/power_supply/battery",
                               "ACTION=change",
                               "DEVPATH=/devices/platform/fdd40000.i2c/i2c-0/0-0020/rk817-battery/power_supply/battery",
                               "SUBSYSTEM=power_supply", "POWER_SUPPLY_NAME=battery", "POWER_SUPPLY_STATUS=Discharging",
                               "POWER_SUPPLY_CAPACITY=87", "POWER_SUPPLY_VOLTAGE_NOW=3912000", "SEQNUM=4410"},
                              /*trailingNul=*/false);
    thimsy::Uevent ev;
    ASSERT_TRUE(thimsy::parseUevent(m.data(), m.size(), &ev));
    EXPECT_EQ(ev.action, "change");
    EXPECT_EQ(ev.get("POWER_SUPPLY_CAPACITY"), "87");
    EXPECT_EQ(ev.get("SEQNUM"), "4410");
    EXPECT_EQ(ev.major, -1);
    EXPECT_EQ(ev.minor, -1);
}

TEST(Uevent, RemoveAndValueWithEquals) {
    const std::string m = msg({"remove@/devices/virtual/misc/uinput", "ACTION=remove",
                               "DEVPATH=/devices/virtual/misc/uinput", "SUBSYSTEM=misc", "X=a=b", "EMPTY=", "SEQNUM=1"});
    thimsy::Uevent ev;
    ASSERT_TRUE(thimsy::parseUevent(m.data(), m.size(), &ev));
    EXPECT_EQ(ev.action, "remove");
    EXPECT_EQ(ev.get("X"), "a=b");
    EXPECT_EQ(ev.env.count("EMPTY"), 1u);
    EXPECT_EQ(ev.get("EMPTY", "d"), "");
}

TEST(Uevent, RejectsMalformedMessages) {
    struct Case {
        const char* why;
        std::string m;
    };
    const std::string libudev = std::string("libudev\0\xfe\xed\xca\xfe", 12) + "rest";
    const Case cases[] = {
        {"empty", ""},
        {"no header", msg({"ACTION=add", "SUBSYSTEM=block"})},
        {"empty action", msg({"@/devices/x"})},
        {"empty devpath", msg({"add@"})},
        {"action mismatch", msg({"add@/devices/x", "ACTION=remove"})},
        {"devpath mismatch", msg({"add@/devices/x", "DEVPATH=/devices/y"})},
        {"field without =", msg({"add@/devices/x", "SUBSYSTEM"})},
        {"field with empty key", msg({"add@/devices/x", "=v"})},
        {"bad major", msg({"add@/devices/x", "MAJOR=1a"})},
        {"negative minor", msg({"add@/devices/x", "MINOR=-1"})},
        {"huge seqnum", msg({"add@/devices/x", "SEQNUM=999999999999999999999"})},
        {"libudev", libudev},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.why);
        thimsy::Uevent ev;
        ev.action = "untouched";
        std::string err;
        EXPECT_FALSE(thimsy::parseUevent(c.m.data(), c.m.size(), &ev, &err));
        EXPECT_FALSE(err.empty());
        EXPECT_EQ(ev.action, "untouched");
    }
    EXPECT_FALSE(thimsy::parseUevent(nullptr, 0, nullptr));
}

TEST(Uevent, EmptyFieldsBetweenNulsAreSkipped) {
    const std::string m = std::string("add@/devices/x\0\0SUBSYSTEM=block\0\0", 33);
    thimsy::Uevent ev;
    ASSERT_TRUE(thimsy::parseUevent(m.data(), m.size(), &ev));
    EXPECT_EQ(ev.subsystem, "block");
}
