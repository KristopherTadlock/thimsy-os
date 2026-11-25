// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Helpers shared by libthimsy's tests: a scratch directory and child processes.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

#include "thimsy/atomic_file.h"

namespace thimsy_test {

inline void removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(path.c_str())) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// A fresh directory under $TMPDIR (or /tmp), removed with everything in it.
class ScratchDir {
 public:
    ScratchDir() {
        const char* t = getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/libthimsy-test-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data())) path_ = buf.data();
    }
    ~ScratchDir() {
        if (!path_.empty()) removeTree(path_);
    }
    const std::string& path() const { return path_; }
    std::string operator/(const std::string& rel) const { return path_ + "/" + rel; }

 private:
    std::string path_;
};

inline std::string slurp(const std::string& path) {
    std::string s;
    thimsy::readFile(path, &s);
    return s;
}

inline void spit(const std::string& path, const std::string& data) {
    thimsy::writeFileAtomic(path, data);
}

inline std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) names.push_back(e->d_name);
        }
        closedir(d);
    }
    return names;
}

// Runs fn in a forked child, which exits with fn's return value; returns the raw wait status.
inline int runInChild(const std::function<int()>& fn) {
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) _exit(fn());
    int status = -1;
    if (pid > 0) waitpid(pid, &status, 0);
    return status;
}

inline bool exitedWith(int status, int code) { return WIFEXITED(status) && WEXITSTATUS(status) == code; }

struct ToolResult {
    int status = -1;
    std::string out, err;
    int code() const { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
};

// Runs a program with extra environment entries ("NAME=value"), capturing stdout and stderr.
inline ToolResult runTool(const std::vector<std::string>& argv, const std::vector<std::string>& env = {}) {
    ToolResult r;
    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0) return r;
    if (pipe(errPipe) != 0) return r;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(outPipe[0]);
        close(errPipe[0]);
        close(outPipe[1]);
        close(errPipe[1]);
        for (const auto& e : env) putenv(const_cast<char*>(e.c_str()));
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execv(args[0], args.data());
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    struct pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    int open = 2;
    char buf[4096];
    while (open > 0 && poll(fds, 2, -1) > 0) {
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                (i == 0 ? r.out : r.err).append(buf, static_cast<size_t>(n));
            } else {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }
    waitpid(pid, &r.status, 0);
    return r;
}

}  // namespace thimsy_test
