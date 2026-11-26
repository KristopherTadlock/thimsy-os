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
// planJail() is pure; applyJail() runs in the child between fork and exec.
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

// In the child: a new mount namespace, "/" made a recursive slave, /dev/null bound read-only over
// each path, CAP_MKNOD dropped from the bounding set. With nothing to mask it does nothing. On
// failure returns false with *err; the caller does not exec Nano then (fail closed).
bool applyJail(const std::vector<std::string>& mask, std::string* err);

}  // namespace session
}  // namespace thimsy
