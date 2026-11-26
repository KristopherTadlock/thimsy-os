// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// wpa_supplicant's control interface (WpaCtrl.h).

#include "WpaCtrl.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace nanolinux {
namespace wpa {

namespace {

std::atomic<unsigned> gCounter{0};

bool fillAddr(const std::string& path, struct sockaddr_un* a) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(a->sun_path)) return false;
    memcpy(a->sun_path, path.c_str(), path.size());
    return true;
}

long long monotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

Ctrl::~Ctrl() { close(); }

bool Ctrl::open(const std::string& path, const std::string& localDir, std::string* err) {
    close();
    struct sockaddr_un remote, local;
    if (!fillAddr(path, &remote)) {
        if (err) *err = "bad socket path " + path;
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (err) *err = std::string("socket: ") + strerror(errno);
        return false;
    }
    std::string name = localDir + "/thimsy-wpa-" + std::to_string(getpid()) + "-" +
                       std::to_string(gCounter.fetch_add(1));
    if (!fillAddr(name, &local)) {
        ::close(fd);
        if (err) *err = "bad local path " + name;
        return false;
    }
    ::unlink(name.c_str());
    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) != 0) {
        if (err) *err = "bind " + name + ": " + strerror(errno);
        ::close(fd);
        return false;
    }
    if (connect(fd, (struct sockaddr*)&remote, sizeof(remote)) != 0) {
        if (err) *err = "connect " + path + ": " + strerror(errno);
        ::close(fd);
        ::unlink(name.c_str());
        return false;
    }
    fd_ = fd;
    local_ = name;
    return true;
}

bool Ctrl::request(const std::string& command, std::string* reply, int timeoutMs, std::string* err) {
    if (fd_ < 0) {
        if (err) *err = "not open";
        return false;
    }
    ssize_t n;
    do {
        n = send(fd_, command.data(), command.size(), 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 || (size_t)n != command.size()) {
        if (err) *err = std::string("send: ") + (n < 0 ? strerror(errno) : "short");
        return false;
    }
    const long long deadline = monotonicMs() + std::max(0, timeoutMs);
    for (;;) {
        long long left = deadline - monotonicMs();
        if (left <= 0) {
            if (err) *err = "no reply to " + command;
            return false;
        }
        struct pollfd p = {fd_, POLLIN, 0};
        int r = poll(&p, 1, (int)left);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            if (err) *err = std::string("poll: ") + strerror(errno);
            return false;
        }
        if (r == 0) continue;
        char buf[4096];
        do {
            n = recv(fd_, buf, sizeof(buf), 0);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            if (err) *err = std::string("recv: ") + strerror(errno);
            return false;
        }
        if (n > 0 && buf[0] == '<') continue;  // an event message ("<3>CTRL-EVENT-..."), not the reply
        reply->assign(buf, (size_t)n);
        return true;
    }
}

void Ctrl::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (!local_.empty()) ::unlink(local_.c_str());
    local_.clear();
}

std::vector<std::string> sockets(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || strncmp(e->d_name, "thimsy-wpa-", 11) == 0) continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) out.push_back(e->d_name);
    }
    closedir(d);
    // By name, a P2P device's socket ("p2p-dev-wlan0") after the interfaces'.
    std::sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) {
        const bool px = x.compare(0, 4, "p2p-") == 0, py = y.compare(0, 4, "p2p-") == 0;
        return px != py ? py : x < y;
    });
    for (std::string& n : out) n = dir + "/" + n;
    return out;
}

bool isOk(const std::string& reply) { return reply == "OK" || reply == "OK\n"; }

}  // namespace wpa
}  // namespace nanolinux
