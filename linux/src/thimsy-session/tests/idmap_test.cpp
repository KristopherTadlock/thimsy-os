// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// ext2/3/4 drives for Nano (io/Idmap.h, OA-29): the staging, owner check, idmapped mount and
// read-only fallback sequences, and taking a drive's top folder, over a fake MountSys that records
// every call and keeps a simple table of what is mounted where.
#include <errno.h>
#include <string.h>
#include <sys/mount.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "io/Idmap.h"
#include "io/Mounter.h"

using namespace thimsy::session;

namespace {

const char kOptions[] = "noatime,nodev,nosuid,noexec";  // VolumePolicy's ext4 options
const char kStage[] = "/run/thimsy/session/stage/sda1";
const char kDir[] = "/storage/0b1c2d3e-0000-4000-8000-00000000abcd";

std::string flagNames(unsigned long f) {
    struct Name {
        unsigned long bit;
        const char* name;
    };
    static const Name kNames[] = {
        {MS_BIND, "bind"},     {MS_PRIVATE, "private"}, {MS_MOVE, "move"},       {MS_REMOUNT, "remount"},
        {MS_RDONLY, "ro"},     {MS_NOSUID, "nosuid"},   {MS_NODEV, "nodev"},     {MS_NOEXEC, "noexec"},
        {MS_NOATIME, "noatime"}, {MS_DIRSYNC, "dirsync"},
    };
    std::string s;
    for (const Name& n : kNames) {
        if (!(f & n.bit)) continue;
        if (!s.empty()) s += ",";
        s += n.name;
    }
    return s;
}

class FakeSys : public MountSys {
 public:
    std::vector<std::string> calls;
    // A call whose text starts with the first member fails with the second (the call is recorded).
    std::vector<std::pair<std::string, int>> failures;
    std::set<std::string> mounted;  // mount points
    std::set<int> openFds;
    std::set<pid_t> children;
    unsigned ownerUid = 0;
    unsigned ownerGid = 0;

    int mount(const std::string& src, const std::string& dir, const std::string& fs, unsigned long flags,
              const std::string& data) override {
        std::string c = "mount " + (src.empty() ? std::string("-") : src) + " " + dir;
        if (!fs.empty()) c += " " + fs;
        std::string f = flagNames(flags);
        if (!f.empty()) c += " " + f;
        if (!data.empty()) c += " data=" + data;
        if (int e = record(c)) return e;
        if (flags & MS_MOVE) {
            if (!mounted.erase(src)) return EINVAL;
            mounted.insert(dir);
        } else if (flags & (MS_REMOUNT | MS_PRIVATE)) {
            if (!mounted.count(dir)) return EINVAL;
        } else {
            mounted.insert(dir);  // a new mount, or a bind
        }
        return 0;
    }
    int umount(const std::string& dir, int flags) override {
        if (int e = record("umount " + dir + ((flags & MNT_DETACH) ? " detach" : ""))) return e;
        return mounted.erase(dir) ? 0 : EINVAL;
    }
    int makeDir(const std::string& dir) override { return record("mkdir " + dir); }
    int ownerOf(const std::string& path, unsigned* uid, unsigned* gid) override {
        if (int e = record("owner " + path)) return e;
        if (!mounted.count(path)) return ENOENT;
        *uid = ownerUid;
        *gid = ownerGid;
        return 0;
    }
    int chown(const std::string& path, unsigned uid, unsigned gid) override {
        return record("chown " + path + " " + std::to_string(uid) + ":" + std::to_string(gid));
    }
    int startUsernsChild(pid_t* pid) override {
        if (int e = record("userns")) return e;
        *pid = nextPid_++;
        children.insert(*pid);
        return 0;
    }
    void endUsernsChild(pid_t pid) override {
        record("end " + std::to_string(pid));
        children.erase(pid);
    }
    int writeFile(const std::string& path, const std::string& text) override {
        return record("write " + path + " " + text);
    }
    int openRead(const std::string& path, int* fd) override {
        if (int e = record("open " + path)) return e;
        *fd = nextFd_++;
        openFds.insert(*fd);
        return 0;
    }
    int openTree(const std::string& path, int* fd) override {
        if (int e = record("open_tree " + path)) return e;
        if (!mounted.count(path)) return EINVAL;
        *fd = nextFd_++;
        openFds.insert(*fd);
        return 0;
    }
    int setIdmap(int treeFd, int usernsFd) override {
        return record("setattr " + std::to_string(treeFd) + " " + std::to_string(usernsFd));
    }
    int moveMount(int treeFd, const std::string& dir) override {
        if (int e = record("move_mount " + std::to_string(treeFd) + " " + dir)) return e;
        mounted.insert(dir);
        return 0;
    }
    void closeFd(int fd) override {
        record("close " + std::to_string(fd));
        openFds.erase(fd);
    }

    bool called(const std::string& prefix) const {
        for (const std::string& c : calls) {
            if (c.compare(0, prefix.size(), prefix) == 0) return true;
        }
        return false;
    }
    int countOf(const std::string& call) const {
        int n = 0;
        for (const std::string& c : calls) n += c == call ? 1 : 0;
        return n;
    }

 private:
    int record(const std::string& c) {
        calls.push_back(c);
        for (const auto& f : failures) {
            if (c.compare(0, f.first.size(), f.first) == 0) return f.second;
        }
        return 0;
    }

    pid_t nextPid_ = 4242;
    int nextFd_ = 10;
};

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += x + "\n";
    return s;
}

const std::vector<std::string> kStaged = {
    std::string("mkdir ") + kStage,
    std::string("mount /dev/sda1 ") + kStage + " ext4 nosuid,nodev,noexec,noatime",
    std::string("owner ") + kStage,
};

std::vector<std::string> plus(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool mountIt(FakeSys& sys, NanoMountResult* res, std::string* err) {
    return mountExtForNano(sys, kStage, "/dev/sda1", kDir, "ext4", kOptions, res, err);
}

}  // namespace

TEST(Idmap, ADriveWhoseTopFolderIsNanosIsMovedAsItIs) {
    FakeSys sys;
    sys.ownerUid = kNanoId;
    sys.ownerGid = 100;
    NanoMountResult res;
    std::string err;
    ASSERT_TRUE(mountIt(sys, &res, &err)) << err;
    EXPECT_EQ(res.access, NanoAccess::Owned);
    EXPECT_EQ(res.ownerUid, kNanoId);
    EXPECT_EQ(res.ownerGid, 100u);
    EXPECT_EQ(joined(sys.calls), joined(plus(kStaged, {std::string("mount ") + kStage + " " + kDir + " move"})));
    EXPECT_EQ(sys.mounted, std::set<std::string>({kDir}));
}

TEST(Idmap, APcDriveGetsAnIdmappedMountOfItsOwnerToNano) {
    FakeSys sys;  // made by root on a PC: the top folder is 0:0
    NanoMountResult res;
    std::string err;
    ASSERT_TRUE(mountIt(sys, &res, &err)) << err;
    EXPECT_EQ(res.access, NanoAccess::Idmapped);
    EXPECT_EQ(res.idmapError, "");
    EXPECT_EQ(joined(sys.calls), joined(plus(kStaged, {
                                                           "userns",
                                                           "write /proc/4242/setgroups deny",
                                                           "write /proc/4242/uid_map 0 1000 1\n",
                                                           "write /proc/4242/gid_map 0 1000 1\n",
                                                           "open /proc/4242/ns/user",
                                                           "end 4242",
                                                           std::string("open_tree ") + kStage,
                                                           "setattr 11 10",
                                                           std::string("move_mount 11 ") + kDir,
                                                           "close 11",
                                                           "close 10",
                                                           std::string("umount ") + kStage + " detach",
                                                       })));
    EXPECT_EQ(sys.mounted, std::set<std::string>({kDir}));
    EXPECT_TRUE(sys.openFds.empty());
    EXPECT_TRUE(sys.children.empty());
}

TEST(Idmap, TheOwnersOwnUidAndGidAreMapped) {
    FakeSys sys;
    sys.ownerUid = 1001;
    sys.ownerGid = 100;
    NanoMountResult res;
    std::string err;
    ASSERT_TRUE(mountIt(sys, &res, &err)) << err;
    EXPECT_EQ(res.access, NanoAccess::Idmapped);
    EXPECT_EQ(sys.countOf("write /proc/4242/uid_map 1001 1000 1\n"), 1);
    EXPECT_EQ(sys.countOf("write /proc/4242/gid_map 100 1000 1\n"), 1);
}

TEST(Idmap, AnyFailedStepFallsBackToReadOnly) {
    struct Case {
        const char* call;
        int err;
        const char* why;  // the start of idmapError
    };
    const Case kCases[] = {
        {"userns", EPERM, "unshare(CLONE_NEWUSER): "},
        {"write /proc/4242/setgroups", EACCES, "/proc/4242/setgroups: "},
        {"write /proc/4242/uid_map", EPERM, "/proc/4242/uid_map: "},
        {"write /proc/4242/gid_map", EPERM, "/proc/4242/gid_map: "},
        {"open /proc/4242/ns/user", ENOENT, "/proc/4242/ns/user: "},
        {"open_tree", ENOSYS, "open_tree: "},
        {"setattr", EINVAL, "mount_setattr(MOUNT_ATTR_IDMAP): "},
        {"move_mount", EPERM, "move_mount: "},
    };
    for (const Case& c : kCases) {
        SCOPED_TRACE(c.call);
        FakeSys sys;
        sys.failures.push_back({c.call, c.err});
        NanoMountResult res;
        std::string err;
        ASSERT_TRUE(mountIt(sys, &res, &err)) << err;
        EXPECT_EQ(res.access, NanoAccess::ReadOnly);
        EXPECT_EQ(res.idmapError, std::string(c.why) + strerror(c.err));
        ASSERT_GE(sys.calls.size(), 2u);
        EXPECT_EQ(sys.calls[sys.calls.size() - 2],
                  std::string("mount - ") + kStage + " bind,remount,ro,nosuid,nodev,noexec,noatime");
        EXPECT_EQ(sys.calls.back(), std::string("mount ") + kStage + " " + kDir + " move");
        EXPECT_EQ(sys.mounted, std::set<std::string>({kDir}));
        EXPECT_TRUE(sys.openFds.empty());
        EXPECT_TRUE(sys.children.empty());
        // The child is ended whenever it was started.
        EXPECT_EQ(sys.called("end "), std::string(c.call) != "userns");
    }
}

TEST(Idmap, NothingIsLeftMountedWhenTheDriveCannotBeMounted) {
    {
        FakeSys sys;
        sys.failures.push_back({"mount /dev/sda1", EINVAL});
        NanoMountResult res;
        std::string err;
        EXPECT_FALSE(mountIt(sys, &res, &err));
        EXPECT_EQ(err, strerror(EINVAL));
        EXPECT_TRUE(sys.mounted.empty());
        EXPECT_FALSE(sys.called("owner"));
    }
    {
        FakeSys sys;
        sys.failures.push_back({"owner", EIO});
        NanoMountResult res;
        std::string err;
        EXPECT_FALSE(mountIt(sys, &res, &err));
        EXPECT_NE(err.find("owner"), std::string::npos);
        EXPECT_TRUE(sys.mounted.empty());
    }
    {
        FakeSys sys;  // no idmapped mount, and the read-only remount fails too
        sys.failures.push_back({"setattr", EINVAL});
        sys.failures.push_back({std::string("mount - ") + kStage, EPERM});
        NanoMountResult res;
        std::string err;
        EXPECT_FALSE(mountIt(sys, &res, &err));
        EXPECT_NE(err.find("no idmapped mount"), std::string::npos);
        EXPECT_TRUE(sys.mounted.empty());
        EXPECT_TRUE(sys.openFds.empty());
    }
    {
        FakeSys sys;
        sys.ownerUid = kNanoId;
        sys.failures.push_back({std::string("mount ") + kStage + " " + kDir, EBUSY});
        NanoMountResult res;
        std::string err;
        EXPECT_FALSE(mountIt(sys, &res, &err));
        EXPECT_TRUE(sys.mounted.empty());
    }
}

TEST(Idmap, TheUserNamespaceChildIsEndedOnEveryPath) {
    FakeSys sys;
    sys.failures.push_back({"write /proc/4242/gid_map", EPERM});
    int fd = -1;
    std::string err;
    EXPECT_FALSE(makeIdmapUserns(sys, 0, 0, &fd, &err));
    EXPECT_EQ(err, std::string("/proc/4242/gid_map: ") + strerror(EPERM));
    EXPECT_EQ(fd, -1);
    EXPECT_EQ(sys.calls.back(), "end 4242");
    EXPECT_FALSE(sys.called("open "));

    FakeSys ok;
    ASSERT_TRUE(makeIdmapUserns(ok, 1001, 1002, &fd, &err)) << err;
    EXPECT_EQ(fd, 10);
    EXPECT_EQ(ok.calls.back(), "end 4242");  // the namespace outlives the child through fd
    EXPECT_TRUE(ok.children.empty());
}

TEST(Idmap, TakingTheTopFolderChownsOnlyItAndRemountsWritable) {
    FakeSys sys;
    sys.mounted.insert(kDir);  // the read-only mount
    bool still = false;
    std::string err;
    // A stray "ro" in the options is dropped: the new mount is the writable one.
    ASSERT_TRUE(takeExtOwnership(sys, kStage, "/dev/sda1", kDir, "ext4", std::string("ro,") + kOptions, &still, &err))
        << err;
    EXPECT_TRUE(still);
    EXPECT_EQ(joined(sys.calls), joined({
                                     std::string("mkdir ") + kStage,
                                     std::string("mount /dev/sda1 ") + kStage + " ext4 nosuid,nodev,noexec,noatime",
                                     std::string("chown ") + kStage + " 1000:1000",
                                     std::string("umount ") + kDir + " detach",
                                     std::string("mount ") + kStage + " " + kDir + " move",
                                 }));
    EXPECT_EQ(sys.mounted, std::set<std::string>({kDir}));
}

TEST(Idmap, TakingTheTopFolderFails) {
    {
        FakeSys sys;
        sys.mounted.insert(kDir);
        sys.failures.push_back({"chown", EROFS});
        bool still = false;
        std::string err;
        EXPECT_FALSE(takeExtOwnership(sys, kStage, "/dev/sda1", kDir, "ext4", kOptions, &still, &err));
        EXPECT_TRUE(still);
        EXPECT_EQ(err, std::string("chown of the top folder: ") + strerror(EROFS));
        EXPECT_EQ(sys.mounted, std::set<std::string>({kDir}));  // still the read-only one
        EXPECT_FALSE(sys.called(std::string("umount ") + kDir));
    }
    {
        FakeSys sys;
        sys.mounted.insert(kDir);
        sys.failures.push_back({std::string("mount ") + kStage + " " + kDir, EBUSY});
        bool still = true;
        std::string err;
        EXPECT_FALSE(takeExtOwnership(sys, kStage, "/dev/sda1", kDir, "ext4", kOptions, &still, &err));
        EXPECT_FALSE(still);
        EXPECT_TRUE(sys.mounted.empty());
    }
}

TEST(Idmap, TheStageIsAPrivateBindOfItself) {
    FakeSys sys;
    std::string err;
    ASSERT_TRUE(prepareStage(sys, "/st", &err)) << err;
    EXPECT_EQ(joined(sys.calls), joined({"mkdir /st", "umount /st detach", "mount /st /st bind", "mount - /st private"}));
    EXPECT_EQ(sys.mounted, std::set<std::string>({"/st"}));
    // A respawned session detaches its predecessor's bind first.
    sys.calls.clear();
    ASSERT_TRUE(prepareStage(sys, "/st", &err)) << err;
    EXPECT_EQ(sys.countOf("umount /st detach"), 2);
    EXPECT_EQ(sys.mounted, std::set<std::string>({"/st"}));

    FakeSys bad;
    bad.failures.push_back({"mount - /st private", EPERM});
    EXPECT_FALSE(prepareStage(bad, "/st", &err));
    EXPECT_NE(err.find("private mount"), std::string::npos);
}

TEST(Idmap, TheSyscallMounterStagesOnceAndPerDevice) {
    FakeSys* sys = new FakeSys();
    std::unique_ptr<Mounter> m = makeSyscallMounter("/st", std::unique_ptr<MountSys>(sys));
    NanoMountResult res;
    std::string err;
    ASSERT_TRUE(m->mountForNano("/dev/sda1", "/storage/A", "ext4", kOptions, &res, &err)) << err;
    EXPECT_EQ(res.access, NanoAccess::Idmapped);
    ASSERT_GE(sys->calls.size(), 5u);
    EXPECT_EQ(sys->calls[3], "mount - /st private");
    EXPECT_EQ(sys->calls[4], "mkdir /st/sda1");
    sys->failures.push_back({"setattr", EINVAL});
    ASSERT_TRUE(m->mountForNano("/dev/sdb1", "/storage/B", "ext4", kOptions, &res, &err)) << err;
    EXPECT_EQ(res.access, NanoAccess::ReadOnly);
    EXPECT_EQ(sys->countOf("mount /st /st bind"), 1);
    EXPECT_TRUE(sys->called("mkdir /st/sdb1"));
    bool still = false;
    ASSERT_TRUE(m->takeOwnership("/dev/sdb1", "/storage/B", "ext4", kOptions, &still, &err)) << err;
    EXPECT_TRUE(still);
    EXPECT_EQ(sys->countOf("chown /st/sdb1 1000:1000"), 1);
    EXPECT_EQ(sys->mounted, std::set<std::string>({"/st", "/storage/A", "/storage/B"}));
}

TEST(Idmap, AccessNames) {
    EXPECT_STREQ(toString(NanoAccess::Owned), "owned");
    EXPECT_STREQ(toString(NanoAccess::Idmapped), "idmapped");
    EXPECT_STREQ(toString(NanoAccess::ReadOnly), "read-only");
}
