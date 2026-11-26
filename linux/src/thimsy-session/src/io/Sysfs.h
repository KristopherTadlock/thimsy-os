// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Small file helpers and the sysfs reads thimsy-session needs. Every path is relative to a root
// ("" on a device; a test tree under --root), so the same code reads a fake /sys.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/VolumePolicy.h"

namespace thimsy {
namespace session {

class Sysfs {
 public:
    explicit Sysfs(std::string root = {}) : root_(std::move(root)) {}

    const std::string& root() const { return root_; }
    // root + abs ("/sys/..." -> "<root>/sys/...").
    std::string path(const std::string& abs) const { return root_ + abs; }

    bool exists(const std::string& abs) const;
    // The first line, without its newline. False if the file can't be read.
    bool readLine(const std::string& abs, std::string* out) const;
    bool readAll(const std::string& abs, std::string* out) const;
    // Writes value (no newline added) with O_WRONLY|O_TRUNC; sysfs ignores the truncation.
    bool write(const std::string& abs, const std::string& value, std::string* err = nullptr) const;
    // Entry names in a directory, sorted, without "." and "..".
    std::vector<std::string> list(const std::string& absDir) const;

    // KEY=VALUE lines of a uevent file.
    std::map<std::string, std::string> readUevent(const std::string& abs) const;

    // Block devices: /sys/class/block names, and one device's facts (no partition type, no fs).
    std::vector<std::string> blockNames() const;
    bool blockDev(const std::string& name, BlockDev* out) const;
    // The disk a block device belongs to (itself for a disk).
    std::string diskOf(const std::string& name) const;
    // device/type is MMC (internal storage; SD cards say SD).
    bool isEmmc(const std::string& disk) const;
    std::vector<std::string> emmcDisks() const;
    uint32_t logicalBlockSize(const std::string& disk) const;

    // The first /sys/class/backlight device, or empty.
    std::string backlight() const;

 private:
    std::string root_;
};

// "s1 [s2] s3" style files, and small parses.
bool parseInt64(const std::string& s, int64_t* out);

}  // namespace session
}  // namespace thimsy
