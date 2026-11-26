// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The virtual devices through /dev/uinput: the "Thimsy Gamepad" (and "Thimsy Keys" when
// device.json maps a button to a KEY_*), with UI_DEV_SETUP and UI_ABS_SETUP (uinput 5, kernel
// 4.5 and later), and the force-feedback requests a game sends to the pad.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Calibrate.h"
#include "core/Merger.h"
#include "core/Rumble.h"

namespace padd {

struct DeviceSetup {
    std::string name;
    uint16_t bus = 0, vendor = 0, product = 0, version = 0;
    std::set<uint16_t> keys;
    std::map<uint16_t, AbsInfo> abs;
    bool ff = false;  // FF_RUMBLE and FF_GAIN
    int ffEffectsMax = 16;
};

// The pad and keyboard devices for a layout (identity: decision I13).
DeviceSetup padSetup(const Layout& l, int ffEffectsMax);
DeviceSetup keysSetup(const Layout& l);

class UinputSink {
 public:
    // The test seam: every ioctl goes through this. Integer arguments arrive cast to void*.
    using IoctlFn = std::function<int(int fd, unsigned long request, void* arg)>;

    static std::unique_ptr<UinputSink> create(const std::string& uinputPath, const DeviceSetup& setup,
                                              std::string* err);
    // Sets up a device on an fd that is already open; takes the fd.
    static std::unique_ptr<UinputSink> createOnFd(int fd, const DeviceSetup& setup, IoctlFn ioctlFn,
                                                  std::string* err);
    ~UinputSink();  // UI_DEV_DESTROY, close
    UinputSink(const UinputSink&) = delete;
    UinputSink& operator=(const UinputSink&) = delete;

    int fd() const { return fd_; }

    // Writes the events meant for dev, in one write.
    bool write(const std::vector<OutEvent>& events, Dev dev);

    // What a game asks of the pad's force feedback.
    class FfHandler {
     public:
        virtual ~FfHandler() = default;
        virtual int upload(const RumbleEffect& e) = 0;  // 0 or -errno
        virtual int erase(int id) = 0;
        virtual void play(int id, int count) = 0;
        virtual void gain(uint16_t gain) = 0;
    };
    // Reads what is queued on the uinput fd (non-blocking) and answers upload and erase
    // requests (UI_BEGIN_FF_UPLOAD .. UI_END_FF_UPLOAD). False when the fd failed.
    bool handleRequests(FfHandler* h);

 private:
    UinputSink(int fd, IoctlFn fn) : fd_(fd), ioctl_(std::move(fn)) {}
    int fd_ = -1;
    IoctlFn ioctl_;
    bool created_ = false;
};

}  // namespace padd
