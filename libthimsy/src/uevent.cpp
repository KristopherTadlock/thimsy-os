// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/uevent.h"

#include <cstring>
#include <vector>

namespace thimsy {
namespace {

bool parseNumber(const std::string& s, uint64_t max, uint64_t* out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (max - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

}  // namespace

std::string Uevent::get(std::string_view key, std::string_view def) const {
    auto it = env.find(std::string(key));
    return it == env.end() ? std::string(def) : it->second;
}

bool parseUevent(const char* buf, size_t len, Uevent* out, std::string* err) {
    if (!buf || len == 0) return fail(err, "empty message");
    if (len >= 8 && memcmp(buf, "libudev", 8) == 0) return fail(err, "a libudev message, not the kernel's");
    // Split into NUL-terminated fields; a missing final NUL is tolerated.
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || buf[i] == '\0') {
            if (i > start) fields.emplace_back(buf + start, i - start);
            start = i + 1;
        }
    }
    if (fields.empty()) return fail(err, "empty message");
    const std::string& header = fields[0];
    const size_t at = header.find('@');
    if (at == std::string::npos) return fail(err, "no ACTION@DEVPATH header");
    Uevent ev;
    ev.action = header.substr(0, at);
    ev.devpath = header.substr(at + 1);
    if (ev.action.empty() || ev.devpath.empty()) return fail(err, "empty action or devpath in header");
    for (size_t i = 1; i < fields.size(); ++i) {
        const size_t eq = fields[i].find('=');
        if (eq == std::string::npos || eq == 0) return fail(err, "field without KEY=: " + fields[i]);
        ev.env[fields[i].substr(0, eq)] = fields[i].substr(eq + 1);
    }
    auto it = ev.env.find("ACTION");
    if (it != ev.env.end() && it->second != ev.action) return fail(err, "ACTION disagrees with header");
    it = ev.env.find("DEVPATH");
    if (it != ev.env.end() && it->second != ev.devpath) return fail(err, "DEVPATH disagrees with header");
    ev.subsystem = ev.get("SUBSYSTEM");
    ev.devname = ev.get("DEVNAME");
    ev.devtype = ev.get("DEVTYPE");
    uint64_t n;
    if ((it = ev.env.find("MAJOR")) != ev.env.end()) {
        if (!parseNumber(it->second, 0x7fffffff, &n)) return fail(err, "bad MAJOR");
        ev.major = static_cast<int>(n);
    }
    if ((it = ev.env.find("MINOR")) != ev.env.end()) {
        if (!parseNumber(it->second, 0x7fffffff, &n)) return fail(err, "bad MINOR");
        ev.minor = static_cast<int>(n);
    }
    if ((it = ev.env.find("SEQNUM")) != ev.env.end()) {
        if (!parseNumber(it->second, UINT64_MAX, &n)) return fail(err, "bad SEQNUM");
        ev.seqnum = n;
    }
    *out = std::move(ev);
    return true;
}

}  // namespace thimsy
