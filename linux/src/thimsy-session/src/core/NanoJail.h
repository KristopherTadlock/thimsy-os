// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Nano's mount namespace (IMPLEMENTATION B.5, decision I40). Nano runs as root, and its
// screen-calibration code opens /dev/block/by-name/baseparameter read-write on Android; on Linux
// neither Nano nor a game it starts may open the eMMC. So Nano is started in its own mount
// namespace in which every node of an internal-storage disk (an mmcblk whose device/type is MMC:
// the disk, its partitions, its boot0/boot1 hardware partitions and its rpmb) has /dev/null
// bind-mounted read-only over it, and without CAP_MKNOD in its bounding set, so it can't make a new
// node for the same device either. The namespace is a slave of the session's (the session makes
// "/" shared first), so drives the session mounts later still appear in it.
//
// The same namespace hides whole directories from Nano (jailDirMasks): an empty read-only tmpfs is
// mounted over each, since a /dev/null bind only goes over a file (over a directory it fails with
// ENOTDIR, which would stop Nano starting). /sys/class/typec is one (OA-4 migration step 1,
// docs/linux/ARCHITECTURE-AUDIT.md defect 4): Android's USB-C role flipper in Nano's shared code
// (NanoMenuMtp.cpp typecTick, NanoTypec.cpp) writes a port's port_type when a PD port with
// DisplayPort alt mode and a non-PD partner is found, under a crash guard (ctl.restart
// nano-typec-guard) that Linux refuses. With the directory empty it finds no port and writes nothing.
// It finds ports only through /sys/class/typec (typec::kClass), so the class directory is enough.
//
// planJail() is pure; applyJail() runs in the child between fork and exec. The plan is made when
// Nano starts; thimsy-session starts Nano again when eMMC nodes appear later (Session.cpp,
// checkNanoJail), so a late eMMC is masked too.
#pragma once

#include <string>
#include <vector>

namespace thimsy {
namespace session {

// Whether /dev/<name> belongs to the eMMC disk `disk` (e.g. "mmcblk0"): the disk itself,
// <disk>p<N>, <disk>boot<N> or <disk>rpmb.
bool isNodeOfDisk(const std::string& name, const std::string& disk);

// The /dev paths to mask: devDir is "/dev" (or a test root's), devEntries the names in it,
// emmcDisks the disks whose device/type is MMC. Sorted.
std::vector<std::string> planJail(const std::string& devDir, const std::vector<std::string>& devEntries,
                                  const std::vector<std::string>& emmcDisks);

// The directories hidden from Nano on every start: /sys/class/typec.
const std::vector<std::string>& jailDirMasks();

// The system calls applyJail makes, replaceable by tests (they return -1 and set errno on failure).
struct JailOps {
    int (*unshareNs)(int flags);
    int (*mountFs)(const char* source, const char* target, const char* type, unsigned long flags,
                   const void* data);
    int (*dropCap)(int cap);   // drops cap from the bounding set
};
const JailOps& realJailOps();

// In the child: a new mount namespace, "/" made a recursive slave, /dev/null bound read-only over
// each path of `mask`, an empty read-only tmpfs over each directory of `dirMasks`, CAP_MKNOD dropped
// from the bounding set. A path or directory that does not exist (ENOENT) is skipped. With nothing
// to mask it does nothing. On any other failure returns false with *err; the caller does not exec
// Nano then (fail closed).
bool applyJail(const std::vector<std::string>& mask, const std::vector<std::string>& dirMasks, std::string* err,
               const JailOps& ops = realJailOps());

}  // namespace session
}  // namespace thimsy
