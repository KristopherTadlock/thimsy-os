// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <string>
#include <vector>

#include "core/NanoJail.h"
#include "gtest/gtest.h"

using namespace thimsy::session;

TEST(NanoJail, NodesOfADisk) {
    EXPECT_TRUE(isNodeOfDisk("mmcblk0", "mmcblk0"));
    EXPECT_TRUE(isNodeOfDisk("mmcblk0p1", "mmcblk0"));
    EXPECT_TRUE(isNodeOfDisk("mmcblk0p12", "mmcblk0"));
    EXPECT_TRUE(isNodeOfDisk("mmcblk0boot0", "mmcblk0"));
    EXPECT_TRUE(isNodeOfDisk("mmcblk0boot1", "mmcblk0"));
    EXPECT_TRUE(isNodeOfDisk("mmcblk0rpmb", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk1", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk1p1", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk01", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk0p", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk0bootx", "mmcblk0"));
    EXPECT_FALSE(isNodeOfDisk("mmcblk0", ""));
}

// The ARC-D: eMMC mmcblk0 with Android's partitions, the boot card mmcblk1, a second card mmcblk2.
TEST(NanoJail, MasksEveryEmmcNodeAndNothingElse) {
    std::vector<std::string> dev = {"console", "null", "kmsg", "input", "mmcblk0", "mmcblk0boot0", "mmcblk0boot1",
                                    "mmcblk0p1", "mmcblk0p2", "mmcblk0p17", "mmcblk0rpmb", "mmcblk1", "mmcblk1p1",
                                    "mmcblk1p5", "mmcblk2", "mmcblk2p1", "sda", "sda1"};
    std::vector<std::string> plan = planJail("/dev", dev, {"mmcblk0"});
    std::vector<std::string> want = {"/dev/mmcblk0",    "/dev/mmcblk0boot0", "/dev/mmcblk0boot1", "/dev/mmcblk0p1",
                                     "/dev/mmcblk0p17", "/dev/mmcblk0p2",    "/dev/mmcblk0rpmb"};
    EXPECT_EQ(plan, want);
}

TEST(NanoJail, NoEmmcNoMasks) {
    std::vector<std::string> dev = {"mmcblk0", "mmcblk0p1", "vda", "vda1"};
    EXPECT_TRUE(planJail("/dev", dev, {}).empty());
    std::string err;
    EXPECT_TRUE(applyJail({}, &err));  // nothing to mask: no namespace needed
}
