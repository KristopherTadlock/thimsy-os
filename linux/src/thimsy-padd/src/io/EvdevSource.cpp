// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/EvdevSource.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "thimsy/atomic_file.h"

namespace padd {
namespace {

constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
constexpr size_t longsFor(size_t bits) { return (bits + kBitsPerLong - 1) / kBitsPerLong; }

bool testBit(const unsigned long* bits, size_t bit) {
    return (bits[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

int eventNumber(const std::string& event) { return std::atoi(event.c_str() + 5); }

std::string ioctlString(int fd, unsigned long req) {
    char buf[256];
    memset(buf, 0, sizeof buf);
    int n = ioctl(fd, req, buf);
    if (n < 0) return "";
    buf[sizeof buf - 1] = '\0';
    return buf;
}

}  // namespace

std::vector<NodeEntry> listNodes(const std::string& sysDir) {
    std::vector<NodeEntry> out;
    std::string dir = sysDir + "/class/input";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() <= 5 || name.compare(0, 5, "event") != 0) continue;
        if (name.find_first_not_of("0123456789", 5) != std::string::npos) continue;
        NodeEntry n;
        n.event = name;
        std::string text;
        if (thimsy::readFile(dir + "/" + name + "/dev", &text)) {
            unsigned maj = 0, min = 0;
            if (sscanf(text.c_str(), "%u:%u", &maj, &min) == 2) {
                n.major = maj;
                n.minor = min;
            }
        }
        if (thimsy::readFile(dir + "/" + name + "/device/name", &text)) n.name = trimmed(text);
        out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const NodeEntry& a, const NodeEntry& b) {
        return eventNumber(a.event) < eventNumber(b.event);
    });
    return out;
}

bool queryNode(int fd, NodeInfo* info, SourceCaps* caps) {
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) return false;
    info->name = ioctlString(fd, EVIOCGNAME(256));
    info->phys = ioctlString(fd, EVIOCGPHYS(256));
    info->uniq = ioctlString(fd, EVIOCGUNIQ(256));
    info->bus = id.bustype;
    info->vendor = id.vendor;
    info->product = id.product;
    info->version = id.version;

    unsigned long evBits[longsFor(EV_CNT)] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof evBits), evBits) < 0) return false;
    caps->keys.clear();
    caps->abs.clear();
    caps->ff.clear();
    if (testBit(evBits, EV_KEY)) {
        unsigned long bits[longsFor(KEY_CNT)] = {};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0) {
            for (size_t k = 0; k < KEY_CNT; ++k) {
                if (testBit(bits, k)) caps->keys.insert(static_cast<uint16_t>(k));
            }
        }
    }
    if (testBit(evBits, EV_ABS)) {
        unsigned long bits[longsFor(ABS_CNT)] = {};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) >= 0) {
            for (size_t a = 0; a < ABS_CNT; ++a) {
                if (!testBit(bits, a)) continue;
                struct input_absinfo ai;
                if (ioctl(fd, EVIOCGABS(a), &ai) < 0) continue;
                AbsInfo x;
                x.value = ai.value;
                x.min = ai.minimum;
                x.max = ai.maximum;
                x.fuzz = ai.fuzz;
                x.flat = ai.flat;
                x.resolution = ai.resolution;
                caps->abs[static_cast<uint16_t>(a)] = x;
            }
        }
    }
    if (testBit(evBits, EV_FF)) {
        unsigned long bits[longsFor(FF_CNT)] = {};
        if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof bits), bits) >= 0) {
            for (size_t f = 0; f < FF_CNT; ++f) {
                if (testBit(bits, f)) caps->ff.insert(static_cast<uint16_t>(f));
            }
        }
    }
    return true;
}

std::unique_ptr<EvdevSource> EvdevSource::open(const NodeEntry& entry, const Paths& paths,
                                               std::string* err) {
    std::string path = paths.devInputDir + "/" + entry.event;
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
        fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    bool viaPrivate = false;
    if (fd < 0 && errno == ENOENT && entry.major != 0) {
        // Unlinked by an earlier padd: make our own node for the same device, open, remove.
        if (!thimsy::makeDirs(paths.privateDir, 0700)) {
            if (err) *err = paths.privateDir + ": " + strerror(errno);
            return nullptr;
        }
        std::string tmp = paths.privateDir + "/" + entry.event;
        unlink(tmp.c_str());
        if (mknod(tmp.c_str(), S_IFCHR | 0600, makedev(entry.major, entry.minor)) != 0) {
            if (err) *err = tmp + ": mknod: " + strerror(errno);
            return nullptr;
        }
        fd = ::open(tmp.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        int saved = errno;
        unlink(tmp.c_str());
        errno = saved;
        viaPrivate = true;
    }
    if (fd < 0) {
        if (err) *err = path + ": " + strerror(errno);
        return nullptr;
    }
    std::unique_ptr<EvdevSource> s(new EvdevSource(fd, path));
    s->hidden_ = viaPrivate;
    if (!queryNode(fd, &s->info_, &s->caps_)) {
        if (err) *err = path + ": not an evdev node";
        return nullptr;
    }
    return s;
}

std::unique_ptr<EvdevSource> EvdevSource::adopt(int fd, const std::string& path, const NodeInfo& info,
                                                const SourceCaps& caps) {
    std::unique_ptr<EvdevSource> s(new EvdevSource(fd, path));
    s->info_ = info;
    s->caps_ = caps;
    s->hidden_ = true;  // nothing to unlink
    return s;
}

EvdevSource::~EvdevSource() {
    if (fd_ < 0) return;
    if (ffId_ >= 0) {
        rumble(VibCommand{});
        ioctl(fd_, EVIOCRMFF, ffId_);
    }
    close(fd_);  // also ends the grab
}

bool EvdevSource::grab() { return ioctl(fd_, EVIOCGRAB, 1) == 0; }

bool EvdevSource::hide() {
    if (hidden_) return true;
    if (unlink(path_.c_str()) == 0 || errno == ENOENT) hidden_ = true;
    return hidden_;
}

EvdevSource::Read EvdevSource::readEvents(int source, std::vector<SourceEvent>* out) {
    struct input_event buf[64];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return Read::Ok;
            if (errno == ENODEV) return Read::Gone;
            return Read::Error;
        }
        if (n == 0) return Read::Gone;
        size_t count = static_cast<size_t>(n) / sizeof buf[0];
        for (size_t i = 0; i < count; ++i) {
            SourceEvent e;
            e.source = source;
            e.type = buf[i].type;
            e.code = buf[i].code;
            e.value = buf[i].value;
#ifdef input_event_sec
            e.timeNs = static_cast<int64_t>(buf[i].input_event_sec) * 1000000000 +
                       static_cast<int64_t>(buf[i].input_event_usec) * 1000;
#else
            e.timeNs = static_cast<int64_t>(buf[i].time.tv_sec) * 1000000000 +
                       static_cast<int64_t>(buf[i].time.tv_usec) * 1000;
#endif
            out->push_back(e);
        }
        if (static_cast<size_t>(n) < sizeof buf) return Read::Ok;
    }
}

bool EvdevSource::currentState(std::set<uint16_t>* keysDown, std::map<uint16_t, int32_t>* abs) {
    keysDown->clear();
    abs->clear();
    if (!caps_.keys.empty()) {
        unsigned long bits[longsFor(KEY_CNT)] = {};
        if (ioctl(fd_, EVIOCGKEY(sizeof bits), bits) < 0) return false;
        for (uint16_t k : caps_.keys) {
            if (testBit(bits, k)) keysDown->insert(k);
        }
    }
    for (const auto& a : caps_.abs) {
        struct input_absinfo ai;
        if (ioctl(fd_, EVIOCGABS(a.first), &ai) < 0) return false;
        (*abs)[a.first] = ai.value;
    }
    return true;
}

bool EvdevSource::rumble(const VibCommand& c) {
    if (!c.on) {
        if (ffId_ < 0) return true;
        struct input_event ev = {};
        ev.type = EV_FF;
        ev.code = static_cast<uint16_t>(ffId_);
        ev.value = 0;
        return write(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev);
    }
    struct ff_effect e;
    memset(&e, 0, sizeof e);
    e.type = FF_RUMBLE;
    e.id = static_cast<int16_t>(ffId_);
    e.u.rumble.strong_magnitude = c.strong;
    e.u.rumble.weak_magnitude = c.weak;
    e.replay.length = c.durationMs > 0xffff ? 0 : static_cast<uint16_t>(c.durationMs);
    if (ioctl(fd_, EVIOCSFF, &e) < 0) return false;
    ffId_ = e.id;
    struct input_event ev = {};
    ev.type = EV_FF;
    ev.code = static_cast<uint16_t>(ffId_);
    ev.value = 1;
    return write(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev);
}

std::unique_ptr<NodeWatch> NodeWatch::create(const std::string& devInputDir) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return nullptr;
    if (inotify_add_watch(fd, devInputDir.c_str(), IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) {
        close(fd);
        return nullptr;
    }
    std::unique_ptr<NodeWatch> w(new NodeWatch);
    w->fd_ = fd;
    return w;
}

NodeWatch::~NodeWatch() {
    if (fd_ >= 0) close(fd_);
}

bool NodeWatch::drain() {
    alignas(struct inotify_event) char buf[4096];
    bool due = false;
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0 && strncmp(ev->name, "event", 5) == 0) due = true;
            if (ev->mask & IN_Q_OVERFLOW) due = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return due;
}

}  // namespace padd
