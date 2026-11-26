// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Mounting drives: mount(2) and umount2(2) on a device; on fake hardware, the mount and umount
// programs from the tool directory (which log what they were asked), with the same options.
#pragma once

#include <memory>
#include <string>
#include <vector>

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
};

std::unique_ptr<Mounter> makeSyscallMounter();
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
