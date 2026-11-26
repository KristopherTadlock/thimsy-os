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

// See FakeEvdev.h. The answers follow the kernel's evdev.c: strings and bitmaps are copied up to the
// caller's size and the copied length is returned; a bitmap is BITS_TO_LONGS(<type>_CNT) longs.

#include "FakeEvdev.h"

#include <errno.h>
#include <limits.h>
#include <linux/input.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

extern "C" int __real_ioctl(int fd, unsigned long request, ...);
extern "C" int __wrap_ioctl(int fd, unsigned long request, ...);

namespace nanohost {
namespace evdev {

namespace {

std::mutex gMu;
std::vector<Device> gDevices;
GrabObserver gGrab;
// (st_dev, st_ino) of a FIFO already resolved -> index in gDevices, or -1 when it is no device.
std::map<std::pair<dev_t, ino_t>, int> gByInode;

constexpr size_t kLongBits = 8 * sizeof(long);
size_t bitmapBytes(size_t bits) { return (bits + kLongBits - 1) / kLongBits * sizeof(long); }

bool fail(int err, int* result) {
    errno = err;
    *result = -1;
    return true;
}

// Copies a bitmap of `bits` bits with `set` on, the way bits_to_user does.
int copyBits(const std::vector<int>& set, size_t bits, void* arg, size_t size) {
    std::vector<unsigned char> map(bitmapBytes(bits), 0);
    for (int b : set) {
        if (b >= 0 && (size_t)b < bits) map[(size_t)b / 8] |= (unsigned char)(1u << (b % 8));
    }
    const size_t n = std::min(size, map.size());
    memcpy(arg, map.data(), n);
    return (int)n;
}

int copyString(const std::string& s, void* arg, size_t size) {
    const size_t n = std::min(size, s.size() + 1);
    if (n == 0) return 0;
    memcpy(arg, s.c_str(), n);
    return (int)n;
}

// The device `fd` refers to (index), or -1. Caller holds gMu.
int deviceFor(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) return -1;
    const auto key = std::make_pair(st.st_dev, st.st_ino);
    auto it = gByInode.find(key);
    if (it != gByInode.end()) return it->second;
    char link[64];
    char target[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(link, target, sizeof(target) - 1);
    int index = -1;
    if (n > 0) {
        target[n] = '\0';
        for (size_t i = 0; i < gDevices.size(); ++i) {
            if (gDevices[i].path == target) {
                index = (int)i;
                break;
            }
        }
    }
    gByInode[key] = index;
    return index;
}

}  // namespace

void setDevices(std::vector<Device> devices) {
    std::lock_guard<std::mutex> lk(gMu);
    gDevices = std::move(devices);
    gByInode.clear();
}

void setGrabObserver(GrabObserver observer) {
    std::lock_guard<std::mutex> lk(gMu);
    gGrab = std::move(observer);
}

bool handle(int fd, unsigned long request, void* arg, int* result) {
    if (_IOC_TYPE(request) != 'E') return false;
    std::unique_lock<std::mutex> lk(gMu);
    const int index = deviceFor(fd);
    if (index < 0) return false;
    const Device d = gDevices[(size_t)index];
    const unsigned nr = _IOC_NR(request);
    const unsigned dir = _IOC_DIR(request);
    const size_t size = _IOC_SIZE(request);
    *result = 0;

    if (nr == _IOC_NR(EVIOCGRAB) && dir == _IOC_WRITE) {
        GrabObserver observer = gGrab;
        lk.unlock();
        if (observer) observer(d.path, (intptr_t)arg != 0);
        return true;
    }
    if (nr == _IOC_NR(EVIOCREVOKE) || nr == _IOC_NR(EVIOCSCLOCKID)) return true;
    if (dir == _IOC_WRITE) {
        // EVIOCSREP, EVIOCSABS and the like: accepted and ignored. EVIOCSFF and EVIOCRMFF: no force
        // feedback.
        if (nr == _IOC_NR(EVIOCSFF) || nr == _IOC_NR(EVIOCRMFF)) return fail(EINVAL, result);
        return true;
    }
    if (arg == nullptr) return fail(EFAULT, result);

    switch (nr) {
        case _IOC_NR(EVIOCGVERSION):
            *static_cast<int*>(arg) = EV_VERSION;
            return true;
        case _IOC_NR(EVIOCGID): {
            struct input_id id = {};
            id.bustype = d.bus;
            id.vendor = d.vendor;
            id.product = d.product;
            id.version = d.version;
            memcpy(arg, &id, std::min(size, sizeof(id)));
            return true;
        }
        case _IOC_NR(EVIOCGREP): {
            const unsigned int rep[2] = {250, 33};
            memcpy(arg, rep, std::min(size, sizeof(rep)));
            return true;
        }
        case _IOC_NR(EVIOCGEFFECTS):
            *static_cast<int*>(arg) = 0;
            return true;
        case _IOC_NR(EVIOCGNAME(0)):
            *result = copyString(d.name, arg, size);
            return true;
        case _IOC_NR(EVIOCGPHYS(0)):
            if (d.phys.empty()) return fail(ENOENT, result);
            *result = copyString(d.phys, arg, size);
            return true;
        case _IOC_NR(EVIOCGUNIQ(0)):
            if (d.uniq.empty()) return fail(ENOENT, result);
            *result = copyString(d.uniq, arg, size);
            return true;
        case _IOC_NR(EVIOCGPROP(0)):
            *result = copyBits(d.props, INPUT_PROP_CNT, arg, size);
            return true;
        case _IOC_NR(EVIOCGMTSLOTS(0)):
            return fail(EINVAL, result);  // no multitouch devices on the host
        // The current state: nothing pressed, no LED, sound or switch on.
        case _IOC_NR(EVIOCGKEY(0)):
            *result = copyBits({}, KEY_CNT, arg, size);
            return true;
        case _IOC_NR(EVIOCGLED(0)):
            *result = copyBits({}, LED_CNT, arg, size);
            return true;
        case _IOC_NR(EVIOCGSND(0)):
            *result = copyBits({}, SND_CNT, arg, size);
            return true;
        case _IOC_NR(EVIOCGSW(0)):
            *result = copyBits({}, SW_CNT, arg, size);
            return true;
        default:
            break;
    }
    if (nr >= _IOC_NR(EVIOCGBIT(0, 0)) && nr < _IOC_NR(EVIOCGBIT(EV_MAX + 1, 0))) {
        const unsigned type = nr - _IOC_NR(EVIOCGBIT(0, 0));
        switch (type) {
            case 0: {
                std::vector<int> types = {EV_SYN};
                if (!d.keys.empty()) types.push_back(EV_KEY);
                if (!d.rel.empty()) types.push_back(EV_REL);
                if (!d.abs.empty()) types.push_back(EV_ABS);
                *result = copyBits(types, EV_CNT, arg, size);
                return true;
            }
            case EV_KEY:
                *result = copyBits(d.keys, KEY_CNT, arg, size);
                return true;
            case EV_REL:
                *result = copyBits(d.rel, REL_CNT, arg, size);
                return true;
            case EV_ABS: {
                std::vector<int> codes;
                for (const AbsAxis& a : d.abs) codes.push_back(a.code);
                *result = copyBits(codes, ABS_CNT, arg, size);
                return true;
            }
            case EV_MSC: *result = copyBits({}, MSC_CNT, arg, size); return true;
            case EV_LED: *result = copyBits({}, LED_CNT, arg, size); return true;
            case EV_SND: *result = copyBits({}, SND_CNT, arg, size); return true;
            case EV_FF: *result = copyBits({}, FF_CNT, arg, size); return true;
            case EV_SW: *result = copyBits({}, SW_CNT, arg, size); return true;
            default: return fail(EINVAL, result);
        }
    }
    if (nr >= _IOC_NR(EVIOCGABS(0)) && nr < _IOC_NR(EVIOCGABS(ABS_CNT))) {
        if (d.abs.empty()) return fail(EINVAL, result);
        const int code = (int)(nr - _IOC_NR(EVIOCGABS(0)));
        struct input_absinfo info = {};
        for (const AbsAxis& a : d.abs) {
            if (a.code != code) continue;
            info.value = a.value;
            info.minimum = a.minimum;
            info.maximum = a.maximum;
            info.fuzz = a.fuzz;
            info.flat = a.flat;
            info.resolution = a.resolution;
        }
        memcpy(arg, &info, std::min(size, sizeof(info)));
        return true;
    }
    return fail(ENOTTY, result);
}

}  // namespace evdev
}  // namespace nanohost

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);  // every ioctl passes at most one pointer-sized argument
    va_end(ap);
    int result = 0;
    const int saved = errno;
    if (nanohost::evdev::handle(fd, request, arg, &result)) {
        if (result >= 0) errno = saved;
        return result;
    }
    errno = saved;
    return __real_ioctl(fd, request, arg);
}
