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

int realUnshare(int flags) { return unshare(flags); }
int realMount(const char* source, const char* target, const char* type, unsigned long flags, const void* data) {
    return mount(source, target, type, flags, data);
}
int realDropCap(int cap) { return prctl(PR_CAPBSET_DROP, cap, 0, 0, 0); }

// An empty directory nothing can be created in: read-only, at most a few inodes and one page.
constexpr const char* kDirMaskOptions = "size=4k,nr_inodes=4,mode=0555";
constexpr unsigned long kDirMaskFlags = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC;

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

const std::vector<std::string>& jailDirMasks() {
    static const std::vector<std::string> dirs = {"/sys/class/typec"};
    return dirs;
}

std::vector<std::string> jailDirsFor(bool rootShared, bool emmcMasked) {
    if (!rootShared && !emmcMasked) return {};
    return jailDirMasks();
}

const JailOps& realJailOps() {
    static const JailOps ops = {realUnshare, realMount, realDropCap};
    return ops;
}

bool applyJail(const std::vector<std::string>& mask, const std::vector<std::string>& dirMasks, std::string* err,
               const JailOps& ops) {
    if (mask.empty() && dirMasks.empty()) return true;
    auto fail = [err](const std::string& what) {
        if (err) *err = what + ": " + strerror(errno);
        return false;
    };
    if (ops.unshareNs(CLONE_NEWNS) != 0) return fail("unshare(CLONE_NEWNS)");
    if (ops.mountFs(nullptr, "/", nullptr, MS_REC | MS_SLAVE, nullptr) != 0) return fail("making / a slave");
    for (const std::string& p : mask) {
        if (ops.mountFs("/dev/null", p.c_str(), nullptr, MS_BIND, nullptr) != 0) {
            if (errno == ENOENT) continue;  // gone since the plan was made
            return fail("binding /dev/null over " + p);
        }
        if (ops.mountFs(nullptr, p.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
            return fail("making " + p + " read-only");
        }
    }
    for (const std::string& d : dirMasks) {
        if (ops.mountFs("tmpfs", d.c_str(), "tmpfs", kDirMaskFlags, kDirMaskOptions) != 0) {
            if (errno == ENOENT) continue;  // no such class on this kernel or board
            return fail("mounting an empty tmpfs over " + d);
        }
    }
    if (ops.dropCap(CAP_MKNOD) != 0) return fail("dropping CAP_MKNOD");
    return true;
}

}  // namespace session
}  // namespace thimsy
