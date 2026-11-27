// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The volume policy: which partitions are drives, and the check-mount-remove life of a drive.
// The device names follow the ARC-D: mmcblk0 the eMMC, mmcblk1 the boot card (slot 1), mmcblk2 a
// second card.
#include <string>
#include <vector>

#include "core/VolumePolicy.h"
#include "gtest/gtest.h"
#include "thimsy/json.h"

using namespace thimsy;
using namespace thimsy::session;

namespace {

const uint64_t kGiB = 1024ull * 1024 * 1024;

BlockDev part(const std::string& disk, int n, const std::string& name, const std::string& type) {
    BlockDev d;
    d.disk = disk;
    d.name = disk + "p" + std::to_string(n);
    d.partition = true;
    d.partn = n;
    d.partName = name;
    d.table = "gpt";
    d.partType = type;
    d.sizeBytes = 8 * kGiB;
    d.kind = "sd";
    return d;
}

BlockDev withFs(BlockDev d, const std::string& fs, const std::string& serial, bool dirty = false) {
    d.fs.type = fs;
    d.fs.serial = serial;
    d.fs.label = "THIMSY";
    d.fs.dirty = dirty;
    return d;
}

BootDisk bootCard() {
    BootDisk b;
    b.disk = "mmcblk1";
    return b;
}

bool isDrive(const BlockDev& d, const BootDisk& b = bootCard()) {
    return classify(d, b, PolicyConfig()).disposition == Disposition::Drive;
}

int count(const std::vector<VolAction>& a, VolActKind k) {
    int n = 0;
    for (const VolAction& x : a) {
        if (x.kind == k) ++n;
    }
    return n;
}

const VolAction* find(const std::vector<VolAction>& a, VolActKind k) {
    for (const VolAction& x : a) {
        if (x.kind == k) return &x;
    }
    return nullptr;
}

VolState stateOf(const VolumeManager& m, const std::string& dev) {
    auto it = m.volumes().find(dev);
    return it == m.volumes().end() ? VolState::Unsupported : it->second.state;
}

}  // namespace

// --- classify ------------------------------------------------------------------------------------

TEST(Classify, TheBootDisksGamesPartitionIsADriveOnceItIsBasicData) {
    EXPECT_TRUE(isDrive(part("mmcblk1", 5, "games", kTypeBasicData)));
    // Still being made by D60firstboot: typed Linux reserved.
    EXPECT_FALSE(isDrive(part("mmcblk1", 5, "games", kTypeLinuxReserved)));
}

TEST(Classify, OtherPartitionsOfTheBootDiskAreNotDrives) {
    EXPECT_FALSE(isDrive(part("mmcblk1", 4, "data", kTypeLinuxHome)));
    EXPECT_FALSE(isDrive(part("mmcblk1", 3, "root", "B921B045-1DF0-41C3-AF44-4C6F280D3FAE")));
    EXPECT_FALSE(isDrive(part("mmcblk1", 1, "uboot", kTypeLinuxReserved)));
    // A basic-data partition on the root device without the games name: not a drive either.
    EXPECT_FALSE(isDrive(part("mmcblk1", 6, "stuff", kTypeBasicData)));
    // The games name with an unreadable table: the type can't be checked, so no.
    BlockDev unread = part("mmcblk1", 5, "games", "");
    unread.table = "";
    EXPECT_FALSE(isDrive(unread));
}

TEST(Classify, ASecondCardWithADataPartition) {
    // Another Thimsy card in slot 2: its data partition (Linux home) is never a drive...
    EXPECT_FALSE(isDrive(part("mmcblk2", 4, "data", kTypeLinuxHome)));
    // ...even when its table can't be read and only the name is known...
    BlockDev unread = part("mmcblk2", 4, "data", "");
    unread.table = "";
    EXPECT_FALSE(isDrive(unread));
    // ...but its games partition is, like any basic-data or Linux-data partition.
    EXPECT_TRUE(isDrive(part("mmcblk2", 5, "games", kTypeBasicData)));
    EXPECT_TRUE(isDrive(part("mmcblk2", 1, "", kTypeLinuxData)));
    EXPECT_FALSE(isDrive(part("mmcblk2", 5, "games", kTypeLinuxReserved)));
}

TEST(Classify, TheEmmcIsNeverADrive) {
    BlockDev d = part("mmcblk0", 17, "userdata", kTypeBasicData);
    d.emmc = true;  // device/type is MMC
    EXPECT_FALSE(isDrive(d));
    EXPECT_NE(classify(d, bootCard(), PolicyConfig()).reason.find("eMMC"), std::string::npos);
    BlockDev hw;
    hw.name = "mmcblk0boot0";
    hw.disk = "mmcblk0boot0";
    hw.sizeBytes = 4 * 1024 * 1024;
    EXPECT_FALSE(isDrive(hw));
    hw.name = hw.disk = "mmcblk0rpmb";
    EXPECT_FALSE(isDrive(hw));
    // Booted from the eMMC itself: still nothing of it.
    BootDisk b;
    b.disk = "mmcblk0";
    b.emmc = true;
    EXPECT_FALSE(isDrive(d, b));
}

TEST(Classify, WholeDisksAndOddDevices) {
    BlockDev disk;
    disk.name = disk.disk = "sda";
    disk.sizeBytes = 16 * kGiB;
    disk.diskHasPartitions = true;
    EXPECT_FALSE(isDrive(disk));
    disk.diskHasPartitions = false;  // a superfloppy USB stick
    EXPECT_TRUE(isDrive(disk));
    BlockDev loop = disk;
    loop.name = loop.disk = "loop0";
    EXPECT_FALSE(isDrive(loop));
    BlockDev ext;
    ext.name = "sda2";
    ext.disk = "sda";
    ext.partition = true;
    ext.table = "mbr";
    ext.partType = "05";
    ext.sizeBytes = 1024;
    EXPECT_FALSE(isDrive(ext));
    ext.sizeBytes = 8 * kGiB;
    EXPECT_FALSE(isDrive(ext));
    ext.partType = "0c";
    EXPECT_TRUE(isDrive(ext));
}

TEST(Classify, VolumeIds) {
    FsInfo f;
    f.type = "exfat";
    f.serial = "9602-C755";
    EXPECT_EQ(volumeIdFor(f), "9602-C755");
    f.serial = "0000-0000";
    EXPECT_EQ(volumeIdFor(f), "");
    f.serial = "";
    EXPECT_EQ(volumeIdFor(f), "");
    f.type = "ext4";
    f.serial = "0b1c2d3e-0000-4000-8000-00000000abcd";
    EXPECT_EQ(volumeIdFor(f), "0b1c2d3e-0000-4000-8000-00000000abcd");
    f.serial = "00000000-0000-0000-0000-000000000000";
    EXPECT_EQ(volumeIdFor(f), "");
}

TEST(Classify, MountOptionsAndChecks) {
    EXPECT_EQ(mountOptions("vfat", false), "utf8,shortname=mixed,dirsync,noatime,nodev,nosuid,noexec,flush");
    EXPECT_EQ(mountOptions("exfat", false), "dirsync,noatime,nodev,nosuid,noexec");
    EXPECT_EQ(mountOptions("exfat", true), "ro,dirsync,noatime,nodev,nosuid,noexec");
    FsInfo f;
    f.type = "vfat";
    EXPECT_FALSE(fsckNeeded(f, false));
    f.dirty = true;
    EXPECT_TRUE(fsckNeeded(f, false));
    EXPECT_FALSE(fsckNeeded(f, true));
    f.type = "exfat";
    f.dirty = false;
    EXPECT_TRUE(fsckNeeded(f, false));  // always, until Phase 0 times it (plan 7.3)
}

// --- the manager ---------------------------------------------------------------------------------

TEST(VolumeManager, AnExfatDriveIsCheckedThenMounted) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk1", 5, "games", kTypeBasicData), "exfat", "9602-C755"),
                                          bootCard());
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].kind, VolActKind::Fsck);
    EXPECT_EQ(a[0].fs, "exfat");
    EXPECT_EQ(stateOf(m, "mmcblk1p5"), VolState::Checking);
    a = m.onFsckDone(a[0].job, 1);  // errors corrected
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(a[0].kind, VolActKind::Mount);
    EXPECT_EQ(a[0].mountPoint, "/storage/9602-C755");
    EXPECT_EQ(a[0].options, "dirsync,noatime,nodev,nosuid,noexec");
    EXPECT_TRUE(m.onMountDone("mmcblk1p5", true, "").empty());
    EXPECT_EQ(stateOf(m, "mmcblk1p5"), VolState::Mounted);

    ASSERT_TRUE(m.takeChanged());
    json::Value j = m.toJson();
    ASSERT_EQ(j["volumes"].size(), 1u);
    const json::Value& v = j["volumes"][size_t{0}];
    EXPECT_EQ(v["id"].asString(), "9602-C755");
    EXPECT_EQ(v["state"].asString(), "mounted");
    EXPECT_EQ(v["path"].asString(), "/storage/9602-C755");
    EXPECT_EQ(v["media_rw"].asString(), "/mnt/media_rw/9602-C755");
    EXPECT_EQ(v["dev"].asString(), "/dev/mmcblk1p5");
    EXPECT_TRUE(v["boot_disk"].asBool());
    EXPECT_EQ(v["label"].asString(), "THIMSY");
    EXPECT_EQ(v["partition"].asInt(), 5);
}

TEST(VolumeManager, ACleanFatCardMountsWithoutACheckAndADirtyOneIsChecked) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"),
                                          bootCard());
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, VolActKind::Mount);
    VolumeManager m2;
    a = m2.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD", true), bootCard());
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, VolActKind::Fsck);
}

TEST(VolumeManager, CardRemovedMidMount) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "AAAA-0001"),
                                          bootCard());
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    int job = a[0].job;
    a = m.onRemoved("mmcblk2p1");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, VolActKind::KillFsck);
    EXPECT_EQ(a[0].job, job);
    EXPECT_TRUE(m.volumes().empty());
    // The killed check reports later: nothing is mounted.
    EXPECT_TRUE(m.onFsckDone(job, -1).empty());
    EXPECT_TRUE(m.onFsckDone(job, 0).empty());
    // The same card back in: a new check, a new job.
    a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "AAAA-0001"), bootCard());
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    EXPECT_NE(a[0].job, job);
    EXPECT_TRUE(m.onFsckDone(job, 0).empty());  // the old job still means nothing
}

TEST(VolumeManager, CardRemovedWhileMountedIsDetached) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"),
                                          bootCard());
    m.onMountDone("mmcblk2p1", true, "");
    a = m.onRemoved("mmcblk2p1");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, VolActKind::Unmount);
    EXPECT_TRUE(a[0].lazy);
    EXPECT_EQ(a[0].mountPoint, "/storage/1234-ABCD");
    EXPECT_TRUE(m.volumes().empty());
}

TEST(VolumeManager, TwoCardsWithTheSameSerialAreBothLeftAlone) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "9602-C755"),
                                          bootCard());
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    int jobA = a[0].job;
    BlockDev usb = withFs(part("sda", 1, "", kTypeBasicData), "exfat", "9602-C755");
    usb.name = "sda1";
    usb.kind = "usb";
    a = m.onDevice(usb, bootCard());
    ASSERT_EQ(count(a, VolActKind::KillFsck), 1);
    EXPECT_EQ(find(a, VolActKind::KillFsck)->job, jobA);
    EXPECT_EQ(count(a, VolActKind::Fsck), 0);
    EXPECT_EQ(count(a, VolActKind::Mount), 0);
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Duplicate);
    EXPECT_EQ(stateOf(m, "sda1"), VolState::Duplicate);
    EXPECT_TRUE(m.onFsckDone(jobA, 0).empty());
    json::Value j = m.toJson();
    EXPECT_EQ(j["volumes"][size_t{0}]["state"].asString(), "duplicate");
    EXPECT_FALSE(j["volumes"][size_t{0}]["error"].asString().empty());
    // One goes: the other is used.
    a = m.onRemoved("sda1");
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    EXPECT_EQ(find(a, VolActKind::Fsck)->dev, "mmcblk2p1");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Checking);
}

TEST(VolumeManager, ASecondCardWithTheSameSerialUnmountsTheFirst) {
    VolumeManager m;
    m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"), bootCard());
    m.onMountDone("mmcblk2p1", true, "");
    BlockDev usb = withFs(part("sda", 1, "", kTypeBasicData), "vfat", "1234-ABCD");
    usb.name = "sda1";
    std::vector<VolAction> a = m.onDevice(usb, bootCard());
    ASSERT_EQ(count(a, VolActKind::Unmount), 1);
    EXPECT_FALSE(find(a, VolActKind::Unmount)->lazy);
    EXPECT_EQ(count(a, VolActKind::Mount), 0);
    m.onUnmountDone("mmcblk2p1", true, "");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Duplicate);
    a = m.onRemoved("sda1");
    ASSERT_EQ(count(a, VolActKind::Mount), 1);  // clean FAT: mounted again without a check
}

TEST(VolumeManager, TheGamesPartitionAppearsReservedAndIsAcceptedAfterItsChange) {
    VolumeManager m;
    BlockDev games = withFs(part("mmcblk1", 5, "games", kTypeLinuxReserved), "exfat", "5E1F-0A11");
    EXPECT_TRUE(m.onDevice(games, bootCard()).empty());
    EXPECT_TRUE(m.volumes().empty());
    games.partType = kTypeBasicData;  // D60firstboot retyped it and wrote "change"
    std::vector<VolAction> a = m.onDevice(games, bootCard());
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    // The same change again (or D10cards' coldplug) does nothing more.
    EXPECT_TRUE(m.onDevice(games, bootCard()).empty());
}

TEST(VolumeManager, EmmcPartitionsNeverShowUp) {
    VolumeManager m;
    BlockDev d = withFs(part("mmcblk0", 17, "userdata", kTypeBasicData), "ext4",
                        "0b1c2d3e-0000-4000-8000-00000000abcd");
    d.emmc = true;
    EXPECT_TRUE(m.onDevice(d, bootCard()).empty());
    EXPECT_TRUE(m.volumes().empty());
}

TEST(VolumeManager, EjectUnmountsFully) {
    VolumeManager m;
    m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"), bootCard());
    m.onMountDone("mmcblk2p1", true, "");
    std::vector<VolAction> a;
    std::string err;
    EXPECT_FALSE(m.eject("FFFF-FFFF", &a, &err));
    EXPECT_FALSE(err.empty());
    ASSERT_TRUE(m.eject("1234-ABCD", &a, &err));
    ASSERT_EQ(a.size(), 1u);
    EXPECT_FALSE(a[0].lazy);
    // Busy first: still mounted, with the reason.
    m.onUnmountDone("mmcblk2p1", false, "Device or resource busy");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Mounted);
    EXPECT_NE(m.byId("1234-ABCD")->error.find("busy"), std::string::npos);
    m.onUnmountDone("mmcblk2p1", true, "");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Ejected);
    // An ejected drive stays ejected on a coldplug repeat; removing it drops it.
    EXPECT_TRUE(m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"), bootCard()).empty());
    EXPECT_TRUE(m.onRemoved("mmcblk2p1").empty());
}

TEST(VolumeManager, AFailedCheckIsShownAndNotMounted) {
    VolumeManager m;
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "AAAA-0001"),
                                          bootCard());
    EXPECT_TRUE(m.onFsckDone(a[0].job, 4).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::CheckFailed);
    EXPECT_EQ(m.toJson()["volumes"][size_t{0}]["state"].asString(), "check_failed");
}

TEST(VolumeManager, NoFilesystemOrNoSerial) {
    VolumeManager m;
    BlockDev raw = part("mmcblk2", 1, "", kTypeBasicData);
    EXPECT_TRUE(m.onDevice(raw, bootCard()).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Unsupported);
    BlockDev zero = withFs(part("mmcblk2", 2, "", kTypeBasicData), "vfat", "0000-0000");
    EXPECT_TRUE(m.onDevice(zero, bootCard()).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p2"), VolState::NoId);
    // Neither counts as a twin of anything.
    BlockDev zero2 = withFs(part("mmcblk2", 3, "", kTypeBasicData), "vfat", "0000-0000");
    EXPECT_TRUE(m.onDevice(zero2, bootCard()).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p3"), VolState::NoId);
}

TEST(VolumeManager, ARespawnedSessionAdoptsAnExistingMount) {
    VolumeManager m;
    m.adoptMounted("mmcblk2p1", "AAAA-0001");
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "AAAA-0001"),
                                          bootCard());
    EXPECT_TRUE(a.empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Mounted);
}

TEST(VolumeManager, ADriveReformattedWhileInIsRemountedUnderItsNewId) {
    VolumeManager m;
    m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"), bootCard());
    m.onMountDone("mmcblk2p1", true, "");
    std::vector<VolAction> a = m.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "exfat", "4321-DCBA"),
                                          bootCard());
    ASSERT_EQ(count(a, VolActKind::Unmount), 1);
    EXPECT_EQ(find(a, VolActKind::Unmount)->mountPoint, "/storage/1234-ABCD");
    ASSERT_EQ(count(a, VolActKind::Fsck), 1);
    EXPECT_EQ(m.byId("4321-DCBA")->dev, "mmcblk2p1");
}

// --- ext2/3/4 drives for Nano (OA-29) ----------------------------------------------------------

namespace {

const char kExtId[] = "0b1c2d3e-0000-4000-8000-00000000abcd";

BlockDev extDrive(bool readOnly = false) {
    BlockDev d = withFs(part("mmcblk2", 1, "", kTypeLinuxData), "ext4", kExtId);
    d.readOnly = readOnly;
    return d;
}

// The drive checked and its Mount action returned.
VolAction checkedExt(VolumeManager* m, bool readOnly = false) {
    std::vector<VolAction> a = m->onDevice(extDrive(readOnly), bootCard());
    if (!readOnly) {
        if (a.size() != 1 || a[0].kind != VolActKind::Fsck) return VolAction(VolActKind::Fsck);
        a = m->onFsckDone(a[0].job, 0);
    }
    return a.size() == 1 ? a[0] : VolAction(VolActKind::Fsck);
}

const json::Value& firstVolume(const json::Value& j) { return j["volumes"][size_t{0}]; }

}  // namespace

TEST(VolumeManager, OnlyWritableExtDrivesAreMountedForNano) {
    EXPECT_TRUE(mountsForNano("ext4", false));
    EXPECT_FALSE(mountsForNano("ext4", true));
    EXPECT_FALSE(mountsForNano("exfat", false));
    EXPECT_FALSE(mountsForNano("vfat", false));

    VolumeManager m;
    VolAction a = checkedExt(&m);
    ASSERT_EQ(a.kind, VolActKind::Mount);
    EXPECT_TRUE(a.forNano);
    EXPECT_EQ(a.options, "noatime,nodev,nosuid,noexec");
    EXPECT_EQ(a.mountPoint, std::string("/storage/") + kExtId);

    VolumeManager ro;
    a = checkedExt(&ro, true);
    ASSERT_EQ(a.kind, VolActKind::Mount);
    EXPECT_FALSE(a.forNano);
    EXPECT_EQ(a.options, "ro,noatime,nodev,nosuid,noexec");

    // A root Nano: mounted as they are.
    PolicyConfig rootNano;
    rootNano.nanoRunsAsNano = false;
    VolumeManager asRoot(rootNano);
    a = checkedExt(&asRoot);
    ASSERT_EQ(a.kind, VolActKind::Mount);
    EXPECT_FALSE(a.forNano);
    VolumeManager adopted(rootNano);
    adopted.adoptMounted("mmcblk2p1", kExtId, Ownership::Idmapped);
    EXPECT_TRUE(adopted.onDevice(extDrive(), bootCard()).empty());
    EXPECT_EQ(adopted.byId(kExtId)->ownership, Ownership::None);

    VolumeManager fat;
    std::vector<VolAction> f = fat.onDevice(withFs(part("mmcblk2", 1, "", kTypeBasicData), "vfat", "1234-ABCD"),
                                            bootCard());
    ASSERT_EQ(f.size(), 1u);
    EXPECT_FALSE(f[0].forNano);
    fat.onMountDone("mmcblk2p1", true, "");
    EXPECT_EQ(firstVolume(fat.toJson()).find("ownership"), nullptr);
}

TEST(VolumeManager, TheMountsOwnershipIsListed) {
    VolumeManager m;
    ASSERT_EQ(checkedExt(&m).kind, VolActKind::Mount);
    EXPECT_TRUE(m.onMountDone("mmcblk2p1", true, "", Ownership::Idmapped).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Mounted);
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::Idmapped);
    EXPECT_EQ(firstVolume(m.toJson())["ownership"].asString(), "idmapped");
    // Ejected: nothing to say about ownership any more.
    std::vector<VolAction> a;
    std::string err;
    ASSERT_TRUE(m.eject(kExtId, &a, &err));
    m.onUnmountDone("mmcblk2p1", true, "");
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::None);
    EXPECT_EQ(firstVolume(m.toJson()).find("ownership"), nullptr);

    VolumeManager failed;
    checkedExt(&failed);
    failed.onMountDone("mmcblk2p1", false, "Invalid argument", Ownership::Nano);
    EXPECT_EQ(stateOf(failed, "mmcblk2p1"), VolState::MountFailed);
    EXPECT_EQ(failed.byId(kExtId)->ownership, Ownership::None);

    EXPECT_STREQ(toString(Ownership::Nano), "nano");
    EXPECT_STREQ(toString(Ownership::NeedsDecision), "needs_decision");
}

TEST(VolumeManager, AReadOnlyFallbackWaitsForTheUsersDecision) {
    VolumeManager m;
    checkedExt(&m);
    m.onMountDone("mmcblk2p1", true, "", Ownership::NeedsDecision);
    EXPECT_EQ(firstVolume(m.toJson())["ownership"].asString(), "needs_decision");
    m.takeChanged();

    std::vector<VolAction> a;
    std::string err;
    ASSERT_TRUE(m.takeOwnership(kExtId, &a, &err)) << err;
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].kind, VolActKind::TakeOwnership);
    EXPECT_EQ(a[0].dev, "mmcblk2p1");
    EXPECT_EQ(a[0].fs, "ext4");
    EXPECT_EQ(a[0].mountPoint, std::string("/storage/") + kExtId);
    EXPECT_EQ(a[0].options, "noatime,nodev,nosuid,noexec");
    EXPECT_TRUE(a[0].forNano);

    EXPECT_TRUE(m.onTakeOwnershipDone("mmcblk2p1", true, true, "").empty());
    EXPECT_TRUE(m.takeChanged());
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::Nano);
    EXPECT_EQ(firstVolume(m.toJson())["ownership"].asString(), "nano");
    // Asked again: nothing is pending now.
    a.clear();
    EXPECT_FALSE(m.takeOwnership(kExtId, &a, &err));
    EXPECT_NE(err.find("no ownership decision pending"), std::string::npos);
    EXPECT_TRUE(a.empty());
}

TEST(VolumeManager, TakeOwnershipIsRefusedForOtherDrives) {
    VolumeManager m;
    std::vector<VolAction> a;
    std::string err;
    EXPECT_FALSE(m.takeOwnership(kExtId, &a, &err));
    EXPECT_EQ(err, std::string("no mounted drive ") + kExtId);
    checkedExt(&m);  // being checked: not mounted yet
    EXPECT_FALSE(m.takeOwnership(kExtId, &a, &err));
    m.onMountDone("mmcblk2p1", true, "", Ownership::Idmapped);
    EXPECT_FALSE(m.takeOwnership(kExtId, &a, &err));
    EXPECT_NE(err.find("no ownership decision pending"), std::string::npos);
    EXPECT_TRUE(a.empty());
}

TEST(VolumeManager, AFailedTakeOwnershipKeepsTheReadOnlyMountOrLosesIt) {
    VolumeManager m;
    checkedExt(&m);
    m.onMountDone("mmcblk2p1", true, "", Ownership::NeedsDecision);
    m.onTakeOwnershipDone("mmcblk2p1", false, true, "chown of the top folder: Read-only file system");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Mounted);
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::NeedsDecision);
    EXPECT_EQ(m.byId(kExtId)->error, "taking ownership failed: chown of the top folder: Read-only file system");

    m.onTakeOwnershipDone("mmcblk2p1", false, false, "moving the mount: Device or resource busy");
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::MountFailed);
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::None);
    // Its mount is gone, so pulling the card unmounts nothing.
    EXPECT_TRUE(m.onRemoved("mmcblk2p1").empty());
}

TEST(VolumeManager, ARespawnedSessionKeepsAnAdoptedMountsOwnership) {
    EXPECT_EQ(ownershipOfMount("ext4", "rw,nosuid,nodev,noexec,noatime,idmapped"), Ownership::Idmapped);
    EXPECT_EQ(ownershipOfMount("ext4", "ro,nosuid,nodev,noexec,noatime"), Ownership::NeedsDecision);
    EXPECT_EQ(ownershipOfMount("ext4", "rw,nosuid,nodev,noexec,noatime"), Ownership::Nano);
    EXPECT_EQ(ownershipOfMount("exfat", "ro,nosuid"), Ownership::None);
    EXPECT_EQ(ownershipOfMount("ext4", "rw,errors=remount-ro"), Ownership::Nano);

    VolumeManager m;
    m.adoptMounted("mmcblk2p1", kExtId, Ownership::NeedsDecision);
    EXPECT_TRUE(m.onDevice(extDrive(), bootCard()).empty());
    EXPECT_EQ(stateOf(m, "mmcblk2p1"), VolState::Mounted);
    EXPECT_EQ(m.byId(kExtId)->ownership, Ownership::NeedsDecision);
    std::vector<VolAction> a;
    std::string err;
    EXPECT_TRUE(m.takeOwnership(kExtId, &a, &err)) << err;

    // A read-only device's drive was mounted "ro" as it always is: no decision to make.
    VolumeManager ro;
    ro.adoptMounted("mmcblk2p1", kExtId, Ownership::NeedsDecision);
    EXPECT_TRUE(ro.onDevice(extDrive(true), bootCard()).empty());
    EXPECT_EQ(ro.byId(kExtId)->ownership, Ownership::None);
}
