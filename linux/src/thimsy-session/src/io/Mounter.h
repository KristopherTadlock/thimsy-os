// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Mounting drives: mount(2) and umount2(2) on a device; on fake hardware, the mount and umount
// programs from the tool directory (which log what they were asked), with the same options.
// ext2/3/4 drives are mounted for Nano through Idmap.h's steps (OA-29).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "io/Idmap.h"

namespace thimsy {
namespace session {

class Mounter {
 public:
    virtual ~Mounter() = default;
    // options: mount(8) style ("ro,dirsync,noatime,utf8,..."); flags and data are split here.
    virtual bool mount(const std::string& dev, const std::string& dir, const std::string& fs,
                       const std::string& options, std::string* err) = 0;
    // lazy: MNT_DETACH (the device is gone).
    virtual bool unmount(const std::string& dir, bool lazy, std::string* err) = 0;
    // An ext2/3/4 drive for Nano (OA-29, Idmap.h): at dir, owned by 1000, mapped to 1000 or
    // read-only; res says which. False (with *err) when nothing could be mounted at dir.
    virtual bool mountForNano(const std::string& dev, const std::string& dir, const std::string& fs,
                              const std::string& options, NanoMountResult* res, std::string* err) = 0;
    // After NanoAccess::ReadOnly, with the user's agreement: the drive's top folder is chowned to
    // 1000 and the drive mounted writable at dir. *stillMounted: false when the read-only mount
    // went and no new one replaced it.
    virtual bool takeOwnership(const std::string& dev, const std::string& dir, const std::string& fs,
                               const std::string& options, bool* stillMounted, std::string* err) = 0;
};

// stageRoot: the private staging directory for Idmap.h's steps (made a private mount at the first
// mount for Nano).
std::unique_ptr<Mounter> makeSyscallMounter(std::string stageRoot);
// The same, with the system calls given (tests).
std::unique_ptr<Mounter> makeSyscallMounter(std::string stageRoot, std::unique_ptr<MountSys> sys);
// mountProg, umountProg: the programs to run (from the tool directory); env: extra NAME=VALUE.
std::unique_ptr<Mounter> makeCommandMounter(std::string mountProg, std::string umountProg, std::string logFile,
                                            std::vector<std::string> env = {});

// Sets the ext4 immutable flag on a mount-point directory (plan 7.3: nothing can be created in an
// empty mount point while its drive is out; it can still be mounted over). False with *err when the
// filesystem or the caller can't (tmpfs, no CAP_LINUX_IMMUTABLE).
bool setImmutable(const std::string& dir, std::string* err);

// Splits mount(8) options into mount(2) flags and the filesystem's data string.
void splitMountOptions(const std::string& options, unsigned long* flags, std::string* data);

}  // namespace session
}  // namespace thimsy
