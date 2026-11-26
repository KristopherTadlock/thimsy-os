// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Kernel uevents: a NETLINK_KOBJECT_UEVENT socket on a device; on fake hardware a FIFO (and a
// replay file read once at start) carrying the same messages as text, one record per block:
//
//   add@/devices/platform/mmc1/block/mmcblk1/mmcblk1p5
//   ACTION=add
//   SUBSYSTEM=block
//   DEVNAME=mmcblk1p5
//   DEVTYPE=partition
//   PARTN=5
//   <blank line>
//
// Both are parsed by libthimsy's parseUevent (the text form is turned into the kernel's
// NUL-separated one first).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "thimsy/uevent.h"

namespace thimsy {
namespace session {

class UeventSource {
 public:
    virtual ~UeventSource() = default;
    virtual int fd() const = 0;
    // Reads what is ready (non-blocking). False on an unrecoverable error.
    virtual bool read(std::vector<Uevent>* out) = 0;
};

std::unique_ptr<UeventSource> openNetlink(std::string* err);
// fifoPath: created if missing.
std::unique_ptr<UeventSource> openUeventFifo(const std::string& fifoPath, std::string* err);

// Parses text records (blank-line separated). Complete records go to *out; an unfinished tail is
// left in *buf. Malformed records are skipped.
void parseTextUevents(std::string* buf, std::vector<Uevent>* out, bool flush = false);

}  // namespace session
}  // namespace thimsy
