// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Idmap.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <map>

#include "io/Mounter.h"
#include "thimsy/atomic_file.h"

// The new mount API's system calls (Linux 5.2: open_tree, move_mount; 5.12: mount_setattr). The
// image's toolchain (Buildroot 2025.02, kernel headers 6.12, glibc 2.36 or later) has them; the host
// test build's older headers may not. These are the numbers every architecture shares from 424 on
// (arm, arm64, x86-64, riscv); alpha, ia64 and mips number them differently.
#if !defined(__NR_open_tree) || !defined(__NR_move_mount) || !defined(__NR_mount_setattr)
#if defined(__alpha__) || defined(__ia64__) || defined(__mips__)
#error "the shared system call numbers below are not this architecture's"
#endif
#endif
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif

namespace thimsy {
namespace session {

const char* toString(NanoAccess a) {
    switch (a) {
        case NanoAccess::Owned: return "owned";
        case NanoAccess::Idmapped: return "idmapped";
        case NanoAccess::ReadOnly: return "read-only";
        case NanoAccess::FsError: return "filesystem-error";
    }
    return "?";
}

namespace {

// From <linux/mount.h>, under names of their own: glibc 2.36 and later declare the same names in
// <sys/mount.h> (struct mount_attr among them) and older glibc not at all.
constexpr unsigned kOpenTreeClone = 1;             // OPEN_TREE_CLONE
constexpr unsigned kMoveMountFEmptyPath = 0x4;     // MOVE_MOUNT_F_EMPTY_PATH
constexpr uint64_t kMountAttrIdmap = 0x00100000;   // MOUNT_ATTR_IDMAP
constexpr uint64_t kMountAttrNosuid = 0x00000002;  // MOUNT_ATTR_NOSUID
constexpr uint64_t kMountAttrNodev = 0x00000004;   // MOUNT_ATTR_NODEV
constexpr uint64_t kMountAttrNoexec = 0x00000008;  // MOUNT_ATTR_NOEXEC

// struct mount_attr, MOUNT_ATTR_SIZE_VER0.
struct MountAttr {
    uint64_t attrSet;
    uint64_t attrClr;
    uint64_t propagation;
    uint64_t usernsFd;
};
static_assert(sizeof(MountAttr) == 32, "struct mount_attr is 32 bytes (MOUNT_ATTR_SIZE_VER0)");

// The flags a MS_REMOUNT | MS_BIND sets on one mount (the rest belong to the superblock).
constexpr unsigned long kPerMountFlags = MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_NODIRATIME | MS_RELATIME;

// The per-mount flags set again with the mapping (MountSys::setIdmap). The idmapped copy inherits
// them from the staging mount; setting them in the same call keeps nosuid and nodev certain.
constexpr unsigned long kIdmapFlags = MS_NOSUID | MS_NODEV | MS_NOEXEC;

// mount(2)'s per-mount flags as mount_setattr's attributes.
uint64_t mountAttrsFor(unsigned long flags) {
    uint64_t a = 0;
    if (flags & MS_NOSUID) a |= kMountAttrNosuid;
    if (flags & MS_NODEV) a |= kMountAttrNodev;
    if (flags & MS_NOEXEC) a |= kMountAttrNoexec;
    return a;
}

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

void reapChild(pid_t pid) {
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
}

class RealMountSys : public MountSys {
 public:
    ~RealMountSys() override {
        for (const auto& kv : holds_) {
            close(kv.second);
            reapChild(kv.first);
        }
    }

    int mount(const std::string& src, const std::string& dir, const std::string& fs, unsigned long flags,
              const std::string& data) override {
        if (::mount(src.empty() ? nullptr : src.c_str(), dir.c_str(), fs.empty() ? nullptr : fs.c_str(), flags,
                    data.empty() ? nullptr : data.c_str()) == 0) {
            return 0;
        }
        return errno;
    }

    int umount(const std::string& dir, int flags) override { return umount2(dir.c_str(), flags) == 0 ? 0 : errno; }

    int makeDir(const std::string& dir) override {
        errno = 0;
        if (makeDirs(dir)) return 0;
        return errno ? errno : ENOTDIR;
    }

    int ownerOf(const std::string& path, unsigned* uid, unsigned* gid) override {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return errno;
        *uid = st.st_uid;
        *gid = st.st_gid;
        return 0;
    }

    int isReadOnly(const std::string& path, bool* readOnly) override {
        struct statvfs sv;
        if (statvfs(path.c_str(), &sv) != 0) return errno;
        *readOnly = (sv.f_flag & ST_RDONLY) != 0;
        return 0;
    }

    int chown(const std::string& path, unsigned uid, unsigned gid) override {
        return ::chown(path.c_str(), static_cast<uid_t>(uid), static_cast<gid_t>(gid)) == 0 ? 0 : errno;
    }

    // The child reports unshare's result on one pipe and waits on another until the session closes
    // its end (endUsernsChild).
    int startUsernsChild(pid_t* pid) override {
        int ready[2];
        int hold[2];
        if (pipe2(ready, O_CLOEXEC) != 0) return errno;
        if (pipe2(hold, O_CLOEXEC) != 0) {
            int e = errno;
            close(ready[0]);
            close(ready[1]);
            return e;
        }
        pid_t p = fork();
        if (p < 0) {
            int e = errno;
            for (int fd : {ready[0], ready[1], hold[0], hold[1]}) close(fd);
            return e;
        }
        if (p == 0) {
            // The child: async-signal-safe calls only (the session may have threads).
            close(ready[0]);
            close(hold[1]);
            unsigned char status = unshare(CLONE_NEWUSER) == 0 ? 0 : static_cast<unsigned char>(errno);
            if (write(ready[1], &status, 1) != 1) _exit(1);
            char c;
            while (read(hold[0], &c, 1) < 0 && errno == EINTR) {
            }
            _exit(0);
        }
        close(ready[1]);
        close(hold[0]);
        unsigned char status = 0;
        ssize_t n;
        do {
            n = read(ready[0], &status, 1);
        } while (n < 0 && errno == EINTR);
        close(ready[0]);
        if (n != 1 || status != 0) {
            close(hold[1]);
            reapChild(p);
            return n == 1 ? static_cast<int>(status) : ECHILD;
        }
        holds_[p] = hold[1];
        *pid = p;
        return 0;
    }

    void endUsernsChild(pid_t pid) override {
        auto it = holds_.find(pid);
        if (it == holds_.end()) return;
        close(it->second);
        holds_.erase(it);
        reapChild(pid);
    }

    int writeFile(const std::string& path, const std::string& text) override {
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) return errno;
        ssize_t n = write(fd, text.data(), text.size());
        int e = n < 0 ? errno : (static_cast<size_t>(n) == text.size() ? 0 : EIO);
        close(fd);
        return e;
    }

    int openRead(const std::string& path, int* fd) override {
        int f = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f < 0) return errno;
        *fd = f;
        return 0;
    }

    int openTree(const std::string& path, int* fd) override {
        long f = syscall(__NR_open_tree, AT_FDCWD, path.c_str(), kOpenTreeClone | O_CLOEXEC);
        if (f < 0) return errno;
        *fd = static_cast<int>(f);
        return 0;
    }

    int setIdmap(int treeFd, int usernsFd, unsigned long flags) override {
        MountAttr a = {};
        a.attrSet = kMountAttrIdmap | mountAttrsFor(flags);
        a.usernsFd = static_cast<uint64_t>(usernsFd);
        return syscall(__NR_mount_setattr, treeFd, "", AT_EMPTY_PATH, &a, sizeof(a)) == 0 ? 0 : errno;
    }

    int moveMount(int treeFd, const std::string& dir) override {
        return syscall(__NR_move_mount, treeFd, "", AT_FDCWD, dir.c_str(), kMoveMountFEmptyPath) == 0 ? 0 : errno;
    }

    void closeFd(int fd) override { close(fd); }

 private:
    std::map<pid_t, int> holds_;  // a waiting child -> the session's end of its hold pipe
};

// Step 3 (Idmap.h): the idmapped copy of the staging mount, attached at dir. flags: the drive's
// mount(2) flags.
bool idmapOnto(MountSys& sys, const std::string& stage, const std::string& dir, unsigned uid, unsigned gid,
               unsigned long flags, std::string* why) {
    int nsFd = -1;
    if (!makeIdmapUserns(sys, uid, gid, &nsFd, why)) return false;
    int tree = -1;
    std::string step = "open_tree";
    int e = sys.openTree(stage, &tree);
    if (e == 0) {
        step = "mount_setattr(MOUNT_ATTR_IDMAP)";
        e = sys.setIdmap(tree, nsFd, flags & kIdmapFlags);
    }
    if (e == 0) {
        step = "move_mount";
        e = sys.moveMount(tree, dir);
    }
    if (tree >= 0) sys.closeFd(tree);  // a copy left detached goes with its last fd
    sys.closeFd(nsFd);
    if (e != 0) return fail(why, step + ": " + strerror(e));
    return true;
}

}  // namespace

std::unique_ptr<MountSys> makeRealMountSys() { return std::unique_ptr<MountSys>(new RealMountSys()); }

bool prepareStage(MountSys& sys, const std::string& stageRoot, std::string* err) {
    int e = sys.makeDir(stageRoot);
    if (e != 0) return fail(err, "cannot make " + stageRoot + ": " + strerror(e));
    // An earlier session's bind, and anything it left staged under it (a few at most).
    for (int i = 0; i < 8 && sys.umount(stageRoot, MNT_DETACH) == 0; ++i) {
    }
    e = sys.mount(stageRoot, stageRoot, "", MS_BIND, "");
    if (e != 0) return fail(err, "cannot bind " + stageRoot + " to itself: " + strerror(e));
    e = sys.mount("", stageRoot, "", MS_PRIVATE, "");
    if (e != 0) {
        sys.umount(stageRoot, MNT_DETACH);  // a shared bind would pass on what is staged under it
        return fail(err, "cannot make " + stageRoot + " a private mount: " + strerror(e));
    }
    return true;
}

bool makeIdmapUserns(MountSys& sys, unsigned uid, unsigned gid, int* nsFd, std::string* err) {
    pid_t pid = 0;
    int e = sys.startUsernsChild(&pid);
    if (e != 0) return fail(err, std::string("unshare(CLONE_NEWUSER): ") + strerror(e));
    const std::string proc = "/proc/" + std::to_string(pid);
    const std::string nano = " " + std::to_string(kNanoId) + " 1\n";
    // setgroups first: gid_map needs it denied for an unprivileged writer, and it costs nothing here.
    std::string step = proc + "/setgroups";
    e = sys.writeFile(step, "deny");
    if (e == 0) {
        step = proc + "/uid_map";
        e = sys.writeFile(step, std::to_string(uid) + nano);
    }
    if (e == 0) {
        step = proc + "/gid_map";
        e = sys.writeFile(step, std::to_string(gid) + nano);
    }
    int fd = -1;
    if (e == 0) {
        step = proc + "/ns/user";
        e = sys.openRead(step, &fd);
    }
    sys.endUsernsChild(pid);  // the namespace lives on in fd
    if (e != 0) return fail(err, step + ": " + strerror(e));
    *nsFd = fd;
    return true;
}

bool mountExtForNano(MountSys& sys, const std::string& stage, const std::string& dev, const std::string& dir,
                     const std::string& fs, const std::string& options, NanoMountResult* res, std::string* err) {
    unsigned long flags = 0;
    std::string data;
    splitMountOptions(options, &flags, &data);
    flags |= kDriveMountFlags;
    *res = NanoMountResult();
    int e = sys.makeDir(stage);
    if (e != 0) return fail(err, "cannot make " + stage + ": " + strerror(e));
    e = sys.mount(dev, stage, fs, flags, data);
    if (e != 0) return fail(err, strerror(e));
    e = sys.ownerOf(stage, &res->ownerUid, &res->ownerGid);
    if (e != 0) {
        sys.umount(stage, MNT_DETACH);
        return fail(err, std::string("cannot read the top folder's owner: ") + strerror(e));
    }
    // Asked for writable but read-only: the superblock is (ext4 found errors). Unknown stays false.
    bool readOnly = false;
    sys.isReadOnly(stage, &readOnly);
    const bool fsError = readOnly && !(flags & MS_RDONLY);
    if (fsError || res->ownerUid == kNanoId) {
        e = sys.mount(stage, dir, "", MS_MOVE, "");
        if (e != 0) {
            sys.umount(stage, MNT_DETACH);
            return fail(err, "moving the mount to " + dir + ": " + strerror(e));
        }
        res->access = fsError ? NanoAccess::FsError : NanoAccess::Owned;
        return true;
    }
    if (idmapOnto(sys, stage, dir, res->ownerUid, res->ownerGid, flags, &res->idmapError)) {
        sys.umount(stage, MNT_DETACH);  // the superblock stays mounted through the copy at dir
        res->access = NanoAccess::Idmapped;
        return true;
    }
    // OA-29's fallback: read-only for Nano (this mount only) until the user decides.
    e = sys.mount("", stage, "", MS_REMOUNT | MS_BIND | MS_RDONLY | (flags & kPerMountFlags), "");
    if (e == 0) e = sys.mount(stage, dir, "", MS_MOVE, "");
    if (e != 0) {
        sys.umount(stage, MNT_DETACH);
        return fail(err, "no idmapped mount (" + res->idmapError + ") and no read-only one: " + strerror(e));
    }
    res->access = NanoAccess::ReadOnly;
    return true;
}

bool takeExtOwnership(MountSys& sys, const std::string& stage, const std::string& dev, const std::string& dir,
                      const std::string& fs, const std::string& options, bool* stillMounted, std::string* err) {
    unsigned long flags = 0;
    std::string data;
    splitMountOptions(options, &flags, &data);
    flags &= ~static_cast<unsigned long>(MS_RDONLY);
    flags |= kDriveMountFlags;
    *stillMounted = true;
    int e = sys.makeDir(stage);
    if (e != 0) return fail(err, "cannot make " + stage + ": " + strerror(e));
    // The same superblock again (it is writable; only the mount at dir is read-only).
    e = sys.mount(dev, stage, fs, flags, data);
    if (e != 0) return fail(err, std::string("writable mount: ") + strerror(e));
    e = sys.chown(stage, kNanoId, kNanoId);
    if (e != 0) {
        sys.umount(stage, MNT_DETACH);
        return fail(err, std::string("chown of the top folder: ") + strerror(e));
    }
    // A per-mount flag does not propagate, so Nano's namespace gets the writable mount as a new one:
    // the read-only mount is detached (files open on it stay usable) and the new one moved in.
    e = sys.umount(dir, MNT_DETACH);
    if (e != 0) {
        sys.umount(stage, MNT_DETACH);
        return fail(err, std::string("detaching the read-only mount: ") + strerror(e));
    }
    e = sys.mount(stage, dir, "", MS_MOVE, "");
    if (e != 0) {
        sys.umount(stage, MNT_DETACH);
        *stillMounted = false;
        return fail(err, "moving the mount to " + dir + ": " + strerror(e));
    }
    return true;
}

}  // namespace session
}  // namespace thimsy
