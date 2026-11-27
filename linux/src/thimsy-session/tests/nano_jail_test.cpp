// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include <errno.h>
#include <linux/capability.h>
#include <sched.h>
#include <sys/mount.h>

#include <map>
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
    EXPECT_TRUE(applyJail({}, {}, &err));  // nothing to mask: no namespace needed
}

TEST(NanoJail, TheTypeCClassIsAlwaysHidden) {
    std::vector<std::string> want = {"/sys/class/typec"};
    EXPECT_EQ(jailDirMasks(), want);
}

// applyJail over fake system calls: what it mounts, in order, and how it fails.
namespace {

struct Call {
    std::string what;     // "unshare", "mount", "dropcap"
    std::string source, target, type, data;
    unsigned long flags = 0;
    int arg = 0;
};

std::vector<Call> gCalls;
std::map<std::string, int> gMountErrno;   // target -> errno for the first mount over it
int gUnshareErrno = 0;

int fakeUnshare(int flags) {
    Call c;
    c.what = "unshare";
    c.arg = flags;
    gCalls.push_back(c);
    if (gUnshareErrno) {
        errno = gUnshareErrno;
        return -1;
    }
    return 0;
}

int fakeMount(const char* source, const char* target, const char* type, unsigned long flags, const void* data) {
    Call c;
    c.what = "mount";
    c.source = source ? source : "";
    c.target = target ? target : "";
    c.type = type ? type : "";
    c.data = data ? static_cast<const char*>(data) : "";
    c.flags = flags;
    gCalls.push_back(c);
    auto it = gMountErrno.find(c.target);
    if (it != gMountErrno.end() && !(flags & MS_REMOUNT)) {
        errno = it->second;
        return -1;
    }
    return 0;
}

int fakeDropCap(int cap) {
    Call c;
    c.what = "dropcap";
    c.arg = cap;
    gCalls.push_back(c);
    return 0;
}

const JailOps kFake = {fakeUnshare, fakeMount, fakeDropCap};

void resetFake() {
    gCalls.clear();
    gMountErrno.clear();
    gUnshareErrno = 0;
}

}  // namespace

TEST(NanoJail, DirectoriesGetAnEmptyReadOnlyTmpfs) {
    resetFake();
    std::string err;
    ASSERT_TRUE(applyJail({}, jailDirMasks(), &err, kFake));
    // Even with no eMMC: a namespace of its own, / a slave, the tmpfs, CAP_MKNOD dropped.
    ASSERT_EQ(gCalls.size(), 4u);
    EXPECT_EQ(gCalls[0].what, std::string("unshare"));
    EXPECT_EQ(gCalls[0].arg, CLONE_NEWNS);
    EXPECT_EQ(gCalls[1].target, std::string("/"));
    EXPECT_EQ(gCalls[1].flags, (unsigned long)(MS_REC | MS_SLAVE));
    const Call& t = gCalls[2];
    EXPECT_EQ(t.what, std::string("mount"));
    EXPECT_EQ(t.source, std::string("tmpfs"));
    EXPECT_EQ(t.type, std::string("tmpfs"));
    EXPECT_EQ(t.target, std::string("/sys/class/typec"));
    EXPECT_TRUE((t.flags & MS_RDONLY) != 0);
    EXPECT_TRUE((t.flags & MS_NOEXEC) != 0);
    EXPECT_TRUE((t.flags & MS_NOSUID) != 0);
    EXPECT_TRUE((t.flags & MS_NODEV) != 0);
    EXPECT_TRUE((t.flags & MS_BIND) == 0);   // not a /dev/null bind, which would fail with ENOTDIR
    EXPECT_NE(t.data.find("mode=0555"), std::string::npos);
    EXPECT_EQ(gCalls[3].what, std::string("dropcap"));
    EXPECT_EQ(gCalls[3].arg, CAP_MKNOD);
}

TEST(NanoJail, FilesAreBoundThenDirectories) {
    resetFake();
    std::string err;
    ASSERT_TRUE(applyJail({"/dev/mmcblk0", "/dev/mmcblk0p1"}, {"/sys/class/typec"}, &err, kFake));
    std::vector<std::string> seq;
    for (const Call& c : gCalls) seq.push_back(c.what + " " + c.source + " " + c.target);
    std::vector<std::string> want = {"unshare  ",
                                     "mount  /",
                                     "mount /dev/null /dev/mmcblk0",
                                     "mount  /dev/mmcblk0",
                                     "mount /dev/null /dev/mmcblk0p1",
                                     "mount  /dev/mmcblk0p1",
                                     "mount tmpfs /sys/class/typec",
                                     "dropcap  "};
    EXPECT_EQ(seq, want);
}

TEST(NanoJail, AMissingDirectoryIsSkipped) {
    // A kernel or board without a Type-C class: Nano still starts.
    resetFake();
    gMountErrno["/sys/class/typec"] = ENOENT;
    std::string err;
    EXPECT_TRUE(applyJail({}, jailDirMasks(), &err, kFake));
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(gCalls.back().what, std::string("dropcap"));
}

TEST(NanoJail, OtherFailuresFailClosed) {
    resetFake();
    gMountErrno["/sys/class/typec"] = EPERM;
    std::string err;
    EXPECT_FALSE(applyJail({}, jailDirMasks(), &err, kFake));
    EXPECT_NE(err.find("/sys/class/typec"), std::string::npos);
    EXPECT_NE(gCalls.back().what, std::string("dropcap"));

    resetFake();
    gMountErrno["/dev/mmcblk0"] = ENOTDIR;
    err.clear();
    EXPECT_FALSE(applyJail({"/dev/mmcblk0"}, jailDirMasks(), &err, kFake));
    EXPECT_NE(err.find("/dev/mmcblk0"), std::string::npos);

    resetFake();
    gUnshareErrno = EPERM;
    err.clear();
    EXPECT_FALSE(applyJail({}, jailDirMasks(), &err, kFake));
    EXPECT_NE(err.find("unshare"), std::string::npos);
}

TEST(NanoJail, AFileGoneSinceThePlanIsSkipped) {
    resetFake();
    gMountErrno["/dev/mmcblk0p1"] = ENOENT;
    std::string err;
    EXPECT_TRUE(applyJail({"/dev/mmcblk0p1"}, {}, &err, kFake));
    EXPECT_EQ(gCalls.back().what, std::string("dropcap"));
}

// "/" not shared: a namespace of Nano's own would miss drives mounted later, so with no eMMC Nano
// keeps the session's namespace (nothing hidden, no unshare); with an eMMC the namespace is made anyway.
TEST(NanoJail, DirectoriesAreHiddenOnlyWhenLaterMountsStillReachNano) {
    EXPECT_EQ(jailDirsFor(true, false), jailDirMasks());
    EXPECT_EQ(jailDirsFor(true, true), jailDirMasks());
    EXPECT_EQ(jailDirsFor(false, true), jailDirMasks());
    EXPECT_TRUE(jailDirsFor(false, false).empty());
    resetFake();
    std::string err;
    EXPECT_TRUE(applyJail({}, jailDirsFor(false, false), &err, kFake));
    EXPECT_TRUE(gCalls.empty());
}
