// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/NanoJail.h"

#include <errno.h>
#include <linux/capability.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>

#include <algorithm>
#include <cctype>

namespace thimsy {
namespace session {
namespace {

bool digitsFrom(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

}  // namespace

bool isNodeOfDisk(const std::string& name, const std::string& disk) {
    if (disk.empty() || name.compare(0, disk.size(), disk) != 0) return false;
    if (name.size() == disk.size()) return true;
    std::string rest = name.substr(disk.size());
    if (rest == "rpmb") return true;
    if (rest[0] == 'p') return digitsFrom(rest, 1);
    if (rest.compare(0, 4, "boot") == 0) return digitsFrom(rest, 4);
    return false;
}

std::vector<std::string> planJail(const std::string& devDir, const std::vector<std::string>& devEntries,
                                  const std::vector<std::string>& emmcDisks) {
    std::vector<std::string> out;
    for (const std::string& name : devEntries) {
        for (const std::string& disk : emmcDisks) {
            if (isNodeOfDisk(name, disk)) {
                out.push_back(devDir + "/" + name);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool applyJail(const std::vector<std::string>& mask, std::string* err) {
    if (mask.empty()) return true;
    auto fail = [err](const std::string& what) {
        if (err) *err = what + ": " + strerror(errno);
        return false;
    };
    if (unshare(CLONE_NEWNS) != 0) return fail("unshare(CLONE_NEWNS)");
    if (mount(nullptr, "/", nullptr, MS_REC | MS_SLAVE, nullptr) != 0) return fail("making / a slave");
    for (const std::string& p : mask) {
        if (mount("/dev/null", p.c_str(), nullptr, MS_BIND, nullptr) != 0) {
            if (errno == ENOENT) continue;  // gone since the plan was made
            return fail("binding /dev/null over " + p);
        }
        if (mount(nullptr, p.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
            return fail("making " + p + " read-only");
        }
    }
    if (prctl(PR_CAPBSET_DROP, CAP_MKNOD, 0, 0, 0) != 0) return fail("dropping CAP_MKNOD");
    return true;
}

}  // namespace session
}  // namespace thimsy
