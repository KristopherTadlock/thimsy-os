/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Fake input devices for the host (docs/linux/IMPLEMENTATION.md B.6, D.3). In the jail,
// /dev/input/eventN are FIFOs the runner holds open and writes struct input_event records into, so
// Nano's own evdev code reads them unchanged. Nano also asks each device who it is (EVIOCGNAME,
// EVIOCGID, EVIOCGBIT, EVIOCGABS, ...) and grabs it (EVIOCGRAB); a FIFO answers none of that, so
// nano-headless links with -Wl,--wrap=ioctl and __wrap_ioctl answers the evdev ('E') requests on a
// FIFO whose path is a configured device from that device's description. Every other ioctl goes to
// __real_ioctl unchanged.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nanohost {
namespace evdev {

struct AbsAxis {
    int code = 0;
    int value = 0, minimum = 0, maximum = 0, fuzz = 0, flat = 0, resolution = 0;
};

struct Device {
    std::string path;              // as the process opens it, e.g. /dev/input/event0
    std::string name, phys, uniq;  // EVIOCGNAME, EVIOCGPHYS, EVIOCGUNIQ (empty phys/uniq: ENOENT)
    uint16_t bus = 0, vendor = 0, product = 0, version = 0;
    std::vector<int> keys;         // EV_KEY codes the device reports
    std::vector<AbsAxis> abs;      // EV_ABS axes
    std::vector<int> rel;          // EV_REL codes
    std::vector<int> props;        // INPUT_PROP_* bits
};

// Replaces the device table. Descriptors already opened keep resolving by their path.
void setDevices(std::vector<Device> devices);

// Called on EVIOCGRAB (grabbed: the argument was non-zero).
using GrabObserver = std::function<void(const std::string& path, bool grabbed)>;
void setGrabObserver(GrabObserver observer);

// The evdev answer for `fd` when it is a configured device: true, with the ioctl's return value in
// *result (and errno set when it is -1). false when `fd` is not a fake device or `request` is not an
// evdev request: the caller passes it to the real ioctl.
bool handle(int fd, unsigned long request, void* arg, int* result);

}  // namespace evdev
}  // namespace nanohost
