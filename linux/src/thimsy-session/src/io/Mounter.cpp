// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Mounter.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "io/Proc.h"

// From <linux/fs.h>, which clashes with glibc's <sys/mount.h>.
#ifndef FS_IOC_GETFLAGS
#define FS_IOC_GETFLAGS _IOR('f', 1, long)
#define FS_IOC_SETFLAGS _IOW('f', 2, long)
#endif
#ifndef FS_IMMUTABLE_FL
#define FS_IMMUTABLE_FL 0x00000010
#endif

namespace thimsy {
namespace session {

bool setImmutable(const std::string& dir, std::string* err) {
    int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = strerror(errno);
        return false;
    }
    int flags = 0;  // the kernel reads and writes an int, whatever the ioctl's declared size
    bool ok = ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0;
    if (ok && !(flags & FS_IMMUTABLE_FL)) {
        flags |= FS_IMMUTABLE_FL;
        ok = ioctl(fd, FS_IOC_SETFLAGS, &flags) == 0;
    }
    if (!ok && err) *err = strerror(errno);
    close(fd);
    return ok;
}

void splitMountOptions(const std::string& options, unsigned long* flags, std::string* data) {
    struct Flag {
        const char* name;
        unsigned long bit;
    };
    static const Flag kFlags[] = {
        {"ro", MS_RDONLY},     {"nosuid", MS_NOSUID},   {"nodev", MS_NODEV},     {"noexec", MS_NOEXEC},
        {"sync", MS_SYNCHRONOUS}, {"dirsync", MS_DIRSYNC}, {"noatime", MS_NOATIME}, {"nodiratime", MS_NODIRATIME},
    };
    *flags = 0;
    data->clear();
    size_t pos = 0;
    while (pos <= options.size()) {
        size_t comma = options.find(',', pos);
        if (comma == std::string::npos) comma = options.size();
        std::string opt = options.substr(pos, comma - pos);
        pos = comma + 1;
        if (opt.empty() || opt == "rw") continue;
        bool isFlag = false;
        for (const Flag& f : kFlags) {
            if (opt == f.name) {
                *flags |= f.bit;
                isFlag = true;
            }
        }
        if (isFlag) continue;
        if (!data->empty()) data->push_back(',');
        data->append(opt);
    }
}

namespace {

class SyscallMounter : public Mounter {
 public:
    bool mount(const std::string& dev, const std::string& dir, const std::string& fs, const std::string& options,
               std::string* err) override {
        unsigned long flags = 0;
        std::string data;
        splitMountOptions(options, &flags, &data);
        if (::mount(dev.c_str(), dir.c_str(), fs.c_str(), flags, data.empty() ? nullptr : data.c_str()) == 0) {
            return true;
        }
        if (err) *err = strerror(errno);
        return false;
    }
    bool unmount(const std::string& dir, bool lazy, std::string* err) override {
        if (umount2(dir.c_str(), lazy ? MNT_DETACH : 0) == 0) return true;
        if (err) *err = strerror(errno);
        return false;
    }
};

class CommandMounter : public Mounter {
 public:
    CommandMounter(std::string m, std::string u, std::string log, std::vector<std::string> env)
        : mount_(std::move(m)), umount_(std::move(u)), log_(std::move(log)), env_(std::move(env)) {}

    bool mount(const std::string& dev, const std::string& dir, const std::string& fs, const std::string& options,
               std::string* err) override {
        SpawnSpec s;
        s.argv = {mount_, "-t", fs, "-o", options, dev, dir};
        s.env = env_;
        s.logFile = log_;
        s.newSession = false;
        int rc = runWait(s, 10000, err);
        if (rc == 0) return true;
        if (err && err->empty()) *err = "mount exited " + std::to_string(rc);
        return false;
    }
    bool unmount(const std::string& dir, bool lazy, std::string* err) override {
        SpawnSpec s;
        s.argv = {umount_};
        if (lazy) s.argv.push_back("-l");
        s.argv.push_back(dir);
        s.env = env_;
        s.logFile = log_;
        s.newSession = false;
        int rc = runWait(s, 10000, err);
        if (rc == 0) return true;
        if (err && err->empty()) *err = "umount exited " + std::to_string(rc);
        return false;
    }

 private:
    std::string mount_, umount_, log_;
    std::vector<std::string> env_;
};

}  // namespace

std::unique_ptr<Mounter> makeSyscallMounter() { return std::unique_ptr<Mounter>(new SyscallMounter()); }

std::unique_ptr<Mounter> makeCommandMounter(std::string mountProg, std::string umountProg, std::string logFile,
                                            std::vector<std::string> env) {
    return std::unique_ptr<Mounter>(
        new CommandMounter(std::move(mountProg), std::move(umountProg), std::move(logFile), std::move(env)));
}

}  // namespace session
}  // namespace thimsy
