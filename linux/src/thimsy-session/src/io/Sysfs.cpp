// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Sysfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "thimsy/atomic_file.h"

namespace thimsy {
namespace session {

bool parseInt64(const std::string& s, int64_t* out) {
    if (s.empty() || s.size() > 18) return false;
    int64_t v = 0;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (s.size() == 1) return false;
    }
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = neg ? -v : v;
    return true;
}

bool Sysfs::exists(const std::string& abs) const {
    struct stat st;
    return stat(path(abs).c_str(), &st) == 0;
}

bool Sysfs::readAll(const std::string& abs, std::string* out) const { return readFile(path(abs), out); }

bool Sysfs::readLine(const std::string& abs, std::string* out) const {
    std::string s;
    if (!readAll(abs, &s)) return false;
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    *out = s;
    return true;
}

bool Sysfs::write(const std::string& abs, const std::string& value, std::string* err) const {
    int fd = open(path(abs).c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = std::string("open: ") + strerror(errno);
        return false;
    }
    ssize_t n;
    do {
        n = ::write(fd, value.data(), value.size());
    } while (n < 0 && errno == EINTR);
    int e = errno;
    close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        if (err) *err = std::string("write: ") + strerror(n < 0 ? e : EIO);
        return false;
    }
    return true;
}

std::vector<std::string> Sysfs::list(const std::string& absDir) const {
    std::vector<std::string> out;
    DIR* d = opendir(path(absDir).c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::map<std::string, std::string> Sysfs::readUevent(const std::string& abs) const {
    std::map<std::string, std::string> out;
    std::string s;
    if (!readAll(abs, &s)) return out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        pos = nl + 1;
    }
    return out;
}

std::vector<std::string> Sysfs::blockNames() const { return list("/sys/class/block"); }

std::string Sysfs::diskOf(const std::string& name) const {
    std::string base = "/sys/class/block/" + name;
    if (!exists(base + "/partition")) return name;
    // Real sysfs: the class entry links to .../block/<disk>/<part>; its parent is the disk.
    char buf[PATH_MAX];
    if (realpath(path(base).c_str(), buf)) {
        std::string real = buf;
        size_t slash = real.rfind('/');
        if (slash != std::string::npos && slash > 0) {
            std::string parent = real.substr(0, slash);
            std::string disk = parent.substr(parent.rfind('/') + 1);
            struct stat st;
            if (disk != "block" && stat((parent + "/dev").c_str(), &st) == 0) return disk;
        }
    }
    // A tree where the partition directories sit inside the disk's directory.
    for (const std::string& d : blockNames()) {
        struct stat st;
        if (d != name && stat(path("/sys/class/block/" + d + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return d;
        }
    }
    return name;
}

bool Sysfs::isEmmc(const std::string& disk) const {
    std::string t;
    return readLine("/sys/class/block/" + disk + "/device/type", &t) && t == "MMC";
}

std::vector<std::string> Sysfs::emmcDisks() const {
    std::vector<std::string> out;
    for (const std::string& n : blockNames()) {
        if (exists("/sys/class/block/" + n + "/partition")) continue;
        if (isEmmc(n)) out.push_back(n);
    }
    return out;
}

uint32_t Sysfs::logicalBlockSize(const std::string& disk) const {
    std::string s;
    int64_t v = 0;
    if (readLine("/sys/class/block/" + disk + "/queue/logical_block_size", &s) && parseInt64(s, &v) &&
        (v == 512 || v == 1024 || v == 2048 || v == 4096)) {
        return static_cast<uint32_t>(v);
    }
    return 512;
}

bool Sysfs::blockDev(const std::string& name, BlockDev* out) const {
    std::string base = "/sys/class/block/" + name;
    if (!exists(base)) return false;
    BlockDev d;
    d.name = name;
    d.partition = exists(base + "/partition");
    d.disk = d.partition ? diskOf(name) : name;
    std::map<std::string, std::string> ue = readUevent(base + "/uevent");
    int64_t v = 0;
    if (ue.count("PARTN") && parseInt64(ue["PARTN"], &v)) d.partn = static_cast<int>(v);
    std::string s;
    if (d.partition && d.partn == 0 && readLine(base + "/partition", &s) && parseInt64(s, &v)) {
        d.partn = static_cast<int>(v);
    }
    if (ue.count("PARTNAME")) d.partName = ue["PARTNAME"];
    if (readLine(base + "/size", &s) && parseInt64(s, &v) && v > 0) d.sizeBytes = static_cast<uint64_t>(v) * 512;
    if (readLine(base + "/ro", &s)) d.readOnly = s == "1";
    d.emmc = isEmmc(d.disk);
    if (!d.partition) {
        for (const std::string& n : blockNames()) {
            if (n != name && exists("/sys/class/block/" + n + "/partition") && diskOf(n) == name) {
                d.diskHasPartitions = true;
                break;
            }
        }
    }
    // The kind, from the disk's device: SD by its MMC type, USB and virtio by the device path.
    std::string type;
    readLine("/sys/class/block/" + d.disk + "/device/type", &type);
    char buf[PATH_MAX];
    std::string real;
    if (realpath(path("/sys/class/block/" + d.disk).c_str(), buf)) real = buf;
    if (type == "SD") {
        d.kind = "sd";
    } else if (real.find("/usb") != std::string::npos) {
        d.kind = "usb";
    } else if (real.find("/virtio") != std::string::npos || d.disk.compare(0, 2, "vd") == 0) {
        d.kind = "virtio";
    } else {
        d.kind = "other";
    }
    *out = d;
    return true;
}

std::string Sysfs::backlight() const {
    std::vector<std::string> l = list("/sys/class/backlight");
    return l.empty() ? std::string() : "/sys/class/backlight/" + l.front();
}

}  // namespace session
}  // namespace thimsy
