// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/UinputSink.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

#include "core/PadSpec.h"

namespace padd {
namespace {

void* intArg(unsigned long v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(v)); }

int realIoctl(int fd, unsigned long req, void* arg) { return ioctl(fd, req, arg); }

}  // namespace

DeviceSetup padSetup(const Layout& l, int ffEffectsMax) {
    DeviceSetup s;
    s.name = kPadName;
    s.bus = kBusVirtual;
    s.vendor = kVendor;
    s.product = kPadProduct;
    s.version = kVersion;
    s.keys = l.padKeys;
    s.abs = l.padAbs;
    s.ff = l.ff;
    s.ffEffectsMax = ffEffectsMax;
    return s;
}

DeviceSetup keysSetup(const Layout& l) {
    DeviceSetup s;
    s.name = kKeysName;
    s.bus = kBusVirtual;
    s.vendor = kVendor;
    s.product = kKeysProduct;
    s.version = kVersion;
    s.keys = l.keysKeys;
    return s;
}

std::unique_ptr<UinputSink> UinputSink::create(const std::string& uinputPath, const DeviceSetup& setup,
                                               std::string* err) {
    int fd = open(uinputPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = uinputPath + ": " + strerror(errno);
        return nullptr;
    }
    return createOnFd(fd, setup, realIoctl, err);
}

std::unique_ptr<UinputSink> UinputSink::createOnFd(int fd, const DeviceSetup& setup, IoctlFn fn,
                                                   std::string* err) {
    std::unique_ptr<UinputSink> s(new UinputSink(fd, fn ? std::move(fn) : IoctlFn(realIoctl)));
    auto io = [&](unsigned long req, void* arg, const char* what) {
        if (s->ioctl_(fd, req, arg) < 0) {
            if (err) *err = std::string(what) + ": " + strerror(errno);
            return false;
        }
        return true;
    };
    int version = 0;
    if (s->ioctl_(fd, UI_GET_VERSION, &version) == 0 && version < 5) {
        if (err) *err = "uinput version " + std::to_string(version) + " is older than 5";
        return nullptr;
    }
    if (!setup.keys.empty()) {
        if (!io(UI_SET_EVBIT, intArg(EV_KEY), "UI_SET_EVBIT EV_KEY")) return nullptr;
        for (uint16_t k : setup.keys) {
            if (!io(UI_SET_KEYBIT, intArg(k), "UI_SET_KEYBIT")) return nullptr;
        }
    }
    if (!setup.abs.empty()) {
        if (!io(UI_SET_EVBIT, intArg(EV_ABS), "UI_SET_EVBIT EV_ABS")) return nullptr;
        for (const auto& a : setup.abs) {
            if (!io(UI_SET_ABSBIT, intArg(a.first), "UI_SET_ABSBIT")) return nullptr;
            struct uinput_abs_setup as;
            memset(&as, 0, sizeof as);
            as.code = a.first;
            as.absinfo.minimum = a.second.min;
            as.absinfo.maximum = a.second.max;
            as.absinfo.fuzz = a.second.fuzz;
            as.absinfo.flat = a.second.flat;
            as.absinfo.resolution = a.second.resolution;
            as.absinfo.value = 0;
            if (!io(UI_ABS_SETUP, &as, "UI_ABS_SETUP")) return nullptr;
        }
    }
    if (setup.ff) {
        if (!io(UI_SET_EVBIT, intArg(EV_FF), "UI_SET_EVBIT EV_FF")) return nullptr;
        if (!io(UI_SET_FFBIT, intArg(FF_RUMBLE), "UI_SET_FFBIT FF_RUMBLE")) return nullptr;
        if (!io(UI_SET_FFBIT, intArg(FF_GAIN), "UI_SET_FFBIT FF_GAIN")) return nullptr;
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = setup.bus;
    us.id.vendor = setup.vendor;
    us.id.product = setup.product;
    us.id.version = setup.version;
    memcpy(us.name, setup.name.data(), std::min(setup.name.size(), sizeof us.name - 1));
    us.ff_effects_max = setup.ff ? static_cast<uint32_t>(setup.ffEffectsMax) : 0;
    if (!io(UI_DEV_SETUP, &us, "UI_DEV_SETUP")) return nullptr;
    if (!io(UI_DEV_CREATE, nullptr, "UI_DEV_CREATE")) return nullptr;
    s->created_ = true;
    return s;
}

UinputSink::~UinputSink() {
    if (fd_ < 0) return;
    if (created_) ioctl_(fd_, UI_DEV_DESTROY, nullptr);
    close(fd_);
}

bool UinputSink::write(const std::vector<OutEvent>& events, Dev dev) {
    std::vector<struct input_event> buf;
    for (const OutEvent& e : events) {
        if (e.dev != dev) continue;
        struct input_event ie;
        memset(&ie, 0, sizeof ie);
        ie.type = e.type;
        ie.code = e.code;
        ie.value = e.value;
        buf.push_back(ie);
    }
    if (buf.empty()) return true;
    const char* p = reinterpret_cast<const char*>(buf.data());
    size_t left = buf.size() * sizeof buf[0];
    while (left > 0) {
        ssize_t n = ::write(fd_, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool UinputSink::handleRequests(FfHandler* h) {
    struct input_event ev;
    for (;;) {
        ssize_t n = read(fd_, &ev, sizeof ev);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n != static_cast<ssize_t>(sizeof ev)) return false;
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload up;
            memset(&up, 0, sizeof up);
            up.request_id = static_cast<uint32_t>(ev.value);
            if (ioctl_(fd_, UI_BEGIN_FF_UPLOAD, &up) < 0) continue;
            if (up.effect.type == FF_RUMBLE) {
                RumbleEffect e;
                e.id = up.effect.id;
                e.strong = up.effect.u.rumble.strong_magnitude;
                e.weak = up.effect.u.rumble.weak_magnitude;
                e.lengthMs = up.effect.replay.length;
                e.delayMs = up.effect.replay.delay;
                up.retval = h->upload(e);
            } else {
                up.retval = -EINVAL;
            }
            ioctl_(fd_, UI_END_FF_UPLOAD, &up);
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
            struct uinput_ff_erase er;
            memset(&er, 0, sizeof er);
            er.request_id = static_cast<uint32_t>(ev.value);
            if (ioctl_(fd_, UI_BEGIN_FF_ERASE, &er) < 0) continue;
            er.retval = h->erase(static_cast<int>(er.effect_id));
            ioctl_(fd_, UI_END_FF_ERASE, &er);
        } else if (ev.type == EV_FF && ev.code == FF_GAIN) {
            h->gain(static_cast<uint16_t>(ev.value));
        } else if (ev.type == EV_FF && ev.code < FF_EFFECT_MIN) {
            h->play(ev.code, ev.value);  // the code is the effect id
        }
    }
}

}  // namespace padd
