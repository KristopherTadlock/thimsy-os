// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// A parser for kernel NETLINK_KOBJECT_UEVENT messages: "ACTION@DEVPATH\0KEY=VALUE\0...". Pure: no
// sockets here, so it is tested on captured messages. Messages from udev's libudev monitor (which
// start with "libudev\0") are rejected: Thimsy has no udev, and they are binary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace thimsy {

struct Uevent {
    std::string action;     // add, remove, change, move, online, offline, bind, unbind
    std::string devpath;    // /devices/... (without /sys)
    std::string subsystem;  // SUBSYSTEM
    std::string devname;    // DEVNAME (e.g. "mmcblk1p1"), empty if none
    std::string devtype;    // DEVTYPE (e.g. "partition", "disk")
    int major = -1;         // MAJOR, -1 if none
    int minor = -1;         // MINOR, -1 if none
    uint64_t seqnum = 0;    // SEQNUM, 0 if none
    std::map<std::string, std::string> env;  // every KEY=VALUE, the ones above included

    // The value of an environment key, or def.
    std::string get(std::string_view key, std::string_view def = {}) const;
};

// Parses one datagram. Returns false (and fills *err) if it is not a well-formed kernel uevent:
// no '@' header, an empty action or devpath, a header that disagrees with ACTION or DEVPATH, a
// field without '=', a non-numeric MAJOR, MINOR or SEQNUM, or a libudev message.
bool parseUevent(const char* buf, size_t len, Uevent* out, std::string* err = nullptr);

}  // namespace thimsy
