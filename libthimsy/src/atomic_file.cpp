// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/atomic_file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

namespace thimsy {
namespace {

std::atomic<AtomicWriteHook> gHook{nullptr};
std::atomic<unsigned> gCounter{0};

void splitPath(const std::string& path, std::string* dir, std::string* base) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        *dir = ".";
        *base = path;
    } else {
        *dir = slash == 0 ? "/" : path.substr(0, slash);
        *base = path.substr(slash + 1);
    }
}

void setErr(std::string* err, const std::string& what, const std::string& path, int e) {
    if (err) *err = what + " " + path + ": " + strerror(e);
}

bool writeAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

}  // namespace

void setAtomicWriteTestHook(AtomicWriteHook hook) { gHook.store(hook); }

std::string atomicTempPrefix(const std::string& path) {
    std::string dir, base;
    splitPath(path, &dir, &base);
    return "." + base + ".tmp.";
}

bool writeFileAtomic(const std::string& path, std::string_view data, const AtomicWriteOptions& opts,
                     std::string* err) {
    std::string dir, base;
    splitPath(path, &dir, &base);
    if (base.empty()) {
        if (err) *err = "no file name in " + path;
        return false;
    }
    char suffix[48];
    snprintf(suffix, sizeof suffix, "%d.%u", static_cast<int>(getpid()), gCounter.fetch_add(1));
    const std::string tmp = (dir == "/" ? "" : dir) + "/" + atomicTempPrefix(path) + suffix;

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, opts.mode & 0777);
    if (fd < 0) {
        setErr(err, "create", tmp, errno);
        return false;
    }
    bool ok = fchmod(fd, opts.mode) == 0;
    int e = ok ? 0 : errno;
    if (ok && !writeAll(fd, data.data(), data.size())) {
        ok = false;
        e = errno;
    }
    if (ok && opts.durable && fsync(fd) != 0) {
        ok = false;
        e = errno;
    }
    if (::close(fd) != 0 && ok) {
        ok = false;
        e = errno;
    }
    if (!ok) {
        setErr(err, "write", tmp, e);
        ::unlink(tmp.c_str());
        return false;
    }
    if (AtomicWriteHook hook = gHook.load()) hook(tmp.c_str());
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        setErr(err, "rename to", path, errno);
        ::unlink(tmp.c_str());
        return false;
    }
    if (opts.durable) {
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            // The new file is in place; a failed directory fsync only weakens durability.
            (void)fsync(dfd);
            ::close(dfd);
        }
    }
    return true;
}

bool readFile(const std::string& path, std::string* out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::string data;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            errno = e;
            return false;
        }
        if (n == 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    *out = std::move(data);
    return true;
}

void removeStaleTemps(const std::string& path) {
    std::string dir, base;
    splitPath(path, &dir, &base);
    const std::string prefix = atomicTempPrefix(path);
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* ent = readdir(d)) {
        if (strncmp(ent->d_name, prefix.c_str(), prefix.size()) == 0) {
            ::unlinkat(dirfd(d), ent->d_name, 0);
        }
    }
    closedir(d);
}

bool makeDirs(const std::string& dir, mode_t mode) {
    if (dir.empty()) return false;
    std::string cur;
    size_t pos = 0;
    while (pos <= dir.size()) {
        size_t next = dir.find('/', pos);
        if (next == std::string::npos) next = dir.size();
        cur = dir.substr(0, next);
        if (!cur.empty() && ::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
        pos = next + 1;
    }
    struct stat st;
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace thimsy
