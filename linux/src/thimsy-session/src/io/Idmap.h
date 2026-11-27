// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// ext2/3/4 drives for a non-root Nano (OA-27, OA-29, OA-34). A drive made on a PC carries the PC
// user's uids, so Nano (uid and gid 1000, `nano`) could not write it. The session mounts such a drive
// in steps:
//
//   0. once, before Nano starts (prepareStage, through Mounter::prepareForNano): the stage
//      directory becomes a private bind of itself. That bind is the one mount here that propagates
//      (everything under the shared / does), so it is made while no namespace of Nano's exists to
//      receive a copy, after detaching an earlier session's (the detach propagates too);
//   1. the drive at a staging point under it, <stage>/<dev>, with the drive's usual options; nothing
//      mounted there reaches Nano's namespace;
//   2. reads the owner of the drive's top folder. A superblock that is read-only although the mount
//      asked for writable (ext4 found errors while mounting, errors=remount-ro): the staging mount
//      is moved to /storage/<id> as it is (NanoAccess::FsError). Owned by 1000 already: moved as it
//      is (NanoAccess::Owned);
//   3. else an idmapped mount (kernel 5.12 and later; ext4 has FS_ALLOW_IDMAP): a user namespace
//      whose only mapping is the owner's uid and gid to 1000 (a child unshares its user namespace
//      and waits while the session writes /proc/<pid>/setgroups "deny", uid_map and gid_map, then
//      opens /proc/<pid>/ns/user), open_tree(OPEN_TREE_CLONE) of the staging mount,
//      mount_setattr(MOUNT_ATTR_IDMAP) with that namespace, move_mount to /storage/<id>; the child
//      and the staging mount are then cleaned up. The drive's files then look owned by `nano`, and
//      what Nano creates is written as the owner's uid, so the PC sees nothing changed
//      (NanoAccess::Idmapped);
//   4. when any step of 3 fails (EINVAL, EPERM, ENOSYS or anything else), OA-29's fallback: the
//      staging mount is made read-only (for this mount only, the superblock stays writable) and
//      moved to /storage/<id>; the drive needs an ownership decision (NanoAccess::ReadOnly). If the
//      user agrees, takeExtOwnership() chowns the drive's top folder (only that folder) to 1000 and
//      mounts the drive again writable.
//
// Every mount made here carries nosuid and nodev (kDriveMountFlags), whatever the options say: with
// the mapping "0 1000 1" a uid-1000 process creates files stored as root on the drive, setuid ones
// included, and a device node on it must stay inert (W8-IDMAP-13).
//
// Every system call goes through MountSys, so the sequences are tested with a fake (idmap_test.cpp).
#pragma once

#include <sys/mount.h>
#include <sys/types.h>

#include <memory>
#include <string>

namespace thimsy {
namespace session {

// The `nano` user's uid and gid (OA-27).
constexpr unsigned kNanoId = 1000;

// The flags every drive mount for Nano carries, whatever its options say (W8-IDMAP-13).
constexpr unsigned long kDriveMountFlags = MS_NOSUID | MS_NODEV;

enum class NanoAccess {
    Owned,     // the top folder belongs to 1000: mounted as it is
    Idmapped,  // the owner's uid and gid show as 1000 through an idmapped mount
    ReadOnly,  // no idmapped mount: read-only until the user lets Thimsy take the top folder
    FsError,   // the superblock is read-only (ext4 found errors): read-only, nothing to decide
};
const char* toString(NanoAccess a);

// The system calls of the sequences. Each int result is 0 or an errno value.
class MountSys {
 public:
    virtual ~MountSys() = default;
    // mount(2); an empty src, fs or data is passed as NULL.
    virtual int mount(const std::string& src, const std::string& dir, const std::string& fs, unsigned long flags,
                      const std::string& data) = 0;
    virtual int umount(const std::string& dir, int flags) = 0;
    // mkdir -p.
    virtual int makeDir(const std::string& dir) = 0;
    virtual int ownerOf(const std::string& path, unsigned* uid, unsigned* gid) = 0;
    // statvfs(path): *readOnly is ST_RDONLY (the mount's own read-only flag or its superblock's).
    virtual int isReadOnly(const std::string& path, bool* readOnly) = 0;
    virtual int chown(const std::string& path, unsigned uid, unsigned gid) = 0;
    // A child process that has unshared its user namespace (CLONE_NEWUSER) and waits for
    // endUsernsChild(); *pid is its pid. An unshare failure is returned and the child is gone.
    virtual int startUsernsChild(pid_t* pid) = 0;
    // Lets the child exit, and reaps it.
    virtual void endUsernsChild(pid_t pid) = 0;
    // Writes text to an existing file (a /proc map file) in one write.
    virtual int writeFile(const std::string& path, const std::string& text) = 0;
    // open(path, O_RDONLY | O_CLOEXEC).
    virtual int openRead(const std::string& path, int* fd) = 0;
    // open_tree(AT_FDCWD, path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC): a detached copy of the mount.
    virtual int openTree(const std::string& path, int* fd) = 0;
    // mount_setattr(treeFd, "", AT_EMPTY_PATH, {MOUNT_ATTR_IDMAP, usernsFd}), setting with it the
    // per-mount MS_NOSUID, MS_NODEV and MS_NOEXEC in flags (MOUNT_ATTR_NOSUID and so on).
    virtual int setIdmap(int treeFd, int usernsFd, unsigned long flags) = 0;
    // move_mount(treeFd, "", AT_FDCWD, dir, MOVE_MOUNT_F_EMPTY_PATH).
    virtual int moveMount(int treeFd, const std::string& dir) = 0;
    virtual void closeFd(int fd) = 0;
};

std::unique_ptr<MountSys> makeRealMountSys();

// Step 0: makes stageRoot a private mount (a bind of itself, after detaching whatever an earlier
// session left there), so mounts under it never propagate. The bind itself does propagate, so this
// runs before Nano's namespace exists. False (with *err) on failure, with the bind detached again.
bool prepareStage(MountSys& sys, const std::string& stageRoot, std::string* err);

// The user namespace of step 3: maps uid -> 1000 and gid -> 1000, one id each. *nsFd is an open
// /proc/<pid>/ns/user; the child is gone when this returns. On failure *err names the step.
bool makeIdmapUserns(MountSys& sys, unsigned uid, unsigned gid, int* nsFd, std::string* err);

struct NanoMountResult {
    NanoAccess access = NanoAccess::Owned;
    unsigned ownerUid = 0;      // of the drive's top folder, as read in step 2
    unsigned ownerGid = 0;
    std::string idmapError;     // ReadOnly: why the idmapped mount was not made
};

// Steps 1 to 4: dev at stage, then at dir. options: mount(8) style (VolumePolicy's). False (with
// *err) when the drive could not be mounted at dir at all; nothing is then left mounted.
bool mountExtForNano(MountSys& sys, const std::string& stage, const std::string& dev, const std::string& dir,
                     const std::string& fs, const std::string& options, NanoMountResult* res, std::string* err);

// After a ReadOnly mount: mounts dev again (writable) at stage, chowns its top folder to 1000:1000,
// detaches the read-only mount at dir and moves the new one there. False (with *err) on failure;
// *stillMounted is false when the read-only mount was detached and the new one could not replace it.
bool takeExtOwnership(MountSys& sys, const std::string& stage, const std::string& dev, const std::string& dir,
                      const std::string& fs, const std::string& options, bool* stillMounted, std::string* err);

}  // namespace session
}  // namespace thimsy
