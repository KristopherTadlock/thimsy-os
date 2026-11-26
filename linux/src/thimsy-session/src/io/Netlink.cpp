// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Netlink.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thimsy {
namespace session {
namespace {

class NetlinkSource : public UeventSource {
 public:
    explicit NetlinkSource(int fd) : fd_(fd) {}
    ~NetlinkSource() override { close(fd_); }
    int fd() const override { return fd_; }
    bool read(std::vector<Uevent>* out) override {
        char buf[8192];
        for (;;) {
            ssize_t n = recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                if (errno == ENOBUFS) continue;  // events were lost; D10cards-style rescans cover it
                return false;
            }
            if (n == 0) return true;
            Uevent ev;
            if (parseUevent(buf, static_cast<size_t>(n), &ev)) out->push_back(ev);
        }
    }

 private:
    int fd_;
};

class FifoSource : public UeventSource {
 public:
    explicit FifoSource(int fd) : fd_(fd) {}
    ~FifoSource() override { close(fd_); }
    int fd() const override { return fd_; }
    bool read(std::vector<Uevent>* out) override {
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                return false;
            }
            if (n == 0) break;
            pending_.append(buf, static_cast<size_t>(n));
        }
        parseTextUevents(&pending_, out);
        return true;
    }

 private:
    int fd_;
    std::string pending_;
};

}  // namespace

void parseTextUevents(std::string* buf, std::vector<Uevent>* out, bool flush) {
    for (;;) {
        size_t end = buf->find("\n\n");
        size_t skip = 2;
        if (end == std::string::npos) {
            if (!flush || buf->empty()) return;
            end = buf->size();
            skip = 0;
        }
        std::string rec = buf->substr(0, end);
        buf->erase(0, end + skip);
        while (!rec.empty() && rec.front() == '\n') rec.erase(0, 1);
        if (rec.empty()) continue;
        std::string msg;
        size_t pos = 0;
        while (pos <= rec.size()) {
            size_t nl = rec.find('\n', pos);
            if (nl == std::string::npos) nl = rec.size();
            std::string line = rec.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                msg += line;
                msg.push_back('\0');
            }
            pos = nl + 1;
        }
        Uevent ev;
        if (parseUevent(msg.data(), msg.size(), &ev)) out->push_back(ev);
    }
}

std::unique_ptr<UeventSource> openNetlink(std::string* err) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        if (err) *err = std::string("socket: ") + strerror(errno);
        return nullptr;
    }
    int size = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof size);
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof sa);
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;  // the kernel's broadcast group
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa) != 0) {
        if (err) *err = std::string("bind: ") + strerror(errno);
        close(fd);
        return nullptr;
    }
    return std::unique_ptr<UeventSource>(new NetlinkSource(fd));
}

std::unique_ptr<UeventSource> openUeventFifo(const std::string& fifoPath, std::string* err) {
    struct stat st;
    if (stat(fifoPath.c_str(), &st) != 0 && mkfifo(fifoPath.c_str(), 0600) != 0) {
        if (err) *err = std::string("mkfifo: ") + strerror(errno);
        return nullptr;
    }
    // O_RDWR keeps a writer open, so the FIFO never reports end-of-file between test writes.
    int fd = open(fifoPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = std::string("open: ") + strerror(errno);
        return nullptr;
    }
    return std::unique_ptr<UeventSource>(new FifoSource(fd));
}

}  // namespace session
}  // namespace thimsy
