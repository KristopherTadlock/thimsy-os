// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Evdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sstream>

namespace thimsy {
namespace session {

bool capabilityHas(const std::string& bitmap, int bit) {
    std::vector<std::string> words;
    std::istringstream in(bitmap);
    std::string w;
    while (in >> w) words.push_back(w);
    const int wordBits = static_cast<int>(sizeof(long) * 8);
    size_t idx = static_cast<size_t>(bit / wordBits);  // counted from the last (least significant) word
    if (idx >= words.size()) return false;
    const std::string& hex = words[words.size() - 1 - idx];
    unsigned long long v = 0;
    for (char c : hex) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<unsigned>(d);
    }
    return (v >> (bit % wordBits)) & 1ull;
}

Evdev::Evdev(const Sysfs& sysfs, bool fake) : sysfs_(sysfs), fake_(fake) {}

Evdev::~Evdev() {
    for (auto& kv : nodes_) ::close(kv.first);
}

bool Evdev::scan() {
    bool changed = false;
    // Drop nodes whose device is gone.
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        if (!sysfs_.exists("/dev/input/" + it->second.node)) {
            ::close(it->first);
            it = nodes_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (const std::string& node : sysfs_.list("/dev/input")) {
        if (node.compare(0, 5, "event") != 0) continue;
        bool open = false;
        for (const auto& kv : nodes_) {
            if (kv.second.node == node) open = true;
        }
        if (open) continue;
        std::string dev = "/sys/class/input/" + node + "/device";
        std::string name, keys, sw;
        sysfs_.readLine(dev + "/name", &name);
        sysfs_.readLine(dev + "/capabilities/key", &keys);
        sysfs_.readLine(dev + "/capabilities/sw", &sw);
        Node n;
        n.node = node;
        n.devName = name;
        if (name == "Thimsy Gamepad") {
            n.source = KeyEvent::Source::Pad;
        } else {
            bool pad = capabilityHas(keys, BTN_GAMEPAD);
            bool wanted = capabilityHas(keys, KEY_POWER) || capabilityHas(keys, KEY_VOLUMEUP) ||
                          capabilityHas(keys, KEY_VOLUMEDOWN) || capabilityHas(sw, SW_LID);
            if (pad || !wanted) continue;
        }
        int fd = ::open(sysfs_.path("/dev/input/" + node).c_str(),
                        (fake_ ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!fake_) {
            int clk = CLOCK_MONOTONIC;
            ioctl(fd, EVIOCSCLOCKID, &clk);
        }
        nodes_[fd] = n;
        changed = true;
    }
    return changed;
}

std::vector<int> Evdev::fds() const {
    std::vector<int> out;
    for (const auto& kv : nodes_) out.push_back(kv.first);
    return out;
}

std::string Evdev::nameOf(int fd) const {
    auto it = nodes_.find(fd);
    return it == nodes_.end() ? std::string() : it->second.node + " (" + it->second.devName + ")";
}

void Evdev::close(int fd) {
    auto it = nodes_.find(fd);
    if (it == nodes_.end()) return;
    ::close(fd);
    nodes_.erase(it);
}

bool Evdev::parseLine(Node* n, const std::string& line, int64_t nowMs, KeyEvent* ev) {
    std::istringstream in(line);
    std::string code;
    long value = 0;
    if (!(in >> code >> value)) return false;
    InputCode ic;
    if (!inputCodeByName(code, &ic)) return false;
    ev->source = n->source;
    ev->type = ic.type;
    ev->code = ic.code;
    ev->value = static_cast<int>(value);
    long rel = 0;
    if (in >> rel) {
        if (!n->haveBase || rel <= n->lastRel) {
            n->haveBase = true;
            n->baseMs = nowMs;
            n->baseRel = rel;
        }
        ev->time = n->baseMs + (rel - n->baseRel);
        n->lastRel = rel;
    } else {
        ev->time = nowMs;
    }
    return true;
}

bool Evdev::read(int fd, int64_t nowMs, std::vector<KeyEvent>* out) {
    auto it = nodes_.find(fd);
    if (it == nodes_.end()) return false;
    Node& n = it->second;
    if (fake_) {
        char buf[1024];
        for (;;) {
            ssize_t r = ::read(fd, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            n.text.append(buf, static_cast<size_t>(r));
        }
        size_t nl;
        while ((nl = n.text.find('\n')) != std::string::npos) {
            std::string line = n.text.substr(0, nl);
            n.text.erase(0, nl + 1);
            KeyEvent ev;
            if (parseLine(&n, line, nowMs, &ev)) out->push_back(ev);
        }
        return true;
    }
    struct input_event evs[32];
    for (;;) {
        ssize_t r = ::read(fd, evs, sizeof evs);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return true;
            return false;  // ENODEV: unplugged
        }
        if (r == 0) return false;
        size_t count = static_cast<size_t>(r) / sizeof(struct input_event);
        for (size_t i = 0; i < count; ++i) {
            const struct input_event& e = evs[i];
            if (e.type != EV_KEY && e.type != EV_SW) continue;
            KeyEvent ev;
            ev.source = n.source;
            ev.type = e.type;
            ev.code = e.code;
            ev.value = e.value;
            ev.time = static_cast<int64_t>(e.input_event_sec) * 1000 + e.input_event_usec / 1000;
            out->push_back(ev);
        }
    }
}

}  // namespace session
}  // namespace thimsy
