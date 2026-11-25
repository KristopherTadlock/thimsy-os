// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/watch.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <map>

namespace thimsy {
namespace {

class InotifyWatcher : public Watcher {
 public:
    InotifyWatcher(int ifd, int efd) : ifd_(ifd), efd_(efd) {}
    ~InotifyWatcher() override {
        ::close(ifd_);
        ::close(efd_);
    }

    bool addDir(const std::string& dir) override {
        const int wd = inotify_add_watch(ifd_, dir.c_str(),
                                         IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                                             IN_MOVED_FROM | IN_ONLYDIR);
        if (wd < 0) return false;
        std::lock_guard<std::mutex> lock(mu_);
        dirs_[wd] = dir;
        return true;
    }

    bool wait(std::vector<Event>* out, int timeoutMs) override {
        struct pollfd fds[2] = {{ifd_, POLLIN, 0}, {efd_, POLLIN, 0}};
        int n;
        do {
            n = ::poll(fds, 2, timeoutMs);
        } while (n < 0 && errno == EINTR);
        if (n < 0) return false;
        if (fds[1].revents & POLLIN) {
            uint64_t v;
            (void)!::read(efd_, &v, sizeof v);
        }
        if (!(fds[0].revents & POLLIN)) return true;
        alignas(struct inotify_event) char buf[16384];
        const ssize_t len = ::read(ifd_, buf, sizeof buf);
        if (len < 0) return errno == EAGAIN || errno == EINTR;
        std::lock_guard<std::mutex> lock(mu_);
        for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(inotify_event)) <= len;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            if (ev->mask & IN_Q_OVERFLOW) {
                out->push_back(Event{"", "", kOverflow});
                continue;
            }
            auto it = dirs_.find(ev->wd);
            if (it == dirs_.end() || ev->len == 0) continue;
            Event e;
            e.dir = it->second;
            e.name = ev->name;  // NUL-padded by the kernel
            if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) e.what |= kWritten;
            if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) e.what |= kRemoved;
            if (e.what) out->push_back(std::move(e));
        }
        return true;
    }

    void wake() override {
        const uint64_t one = 1;
        (void)!::write(efd_, &one, sizeof one);
    }

 private:
    int ifd_;
    int efd_;
    std::mutex mu_;
    std::map<int, std::string> dirs_;
};

}  // namespace

std::unique_ptr<Watcher> Watcher::createInotify() {
    const int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return nullptr;
    const int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        ::close(ifd);
        return nullptr;
    }
    return std::unique_ptr<Watcher>(new InotifyWatcher(ifd, efd));
}

bool FakeWatcher::addDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    dirs_.push_back(dir);
    return true;
}

bool FakeWatcher::wait(std::vector<Event>* out, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    auto ready = [this] { return !queue_.empty() || woken_; };
    if (timeoutMs < 0) {
        cv_.wait(lock, ready);
    } else {
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
    }
    woken_ = false;
    while (!queue_.empty()) {
        out->push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return true;
}

void FakeWatcher::wake() {
    std::lock_guard<std::mutex> lock(mu_);
    woken_ = true;
    cv_.notify_all();
}

void FakeWatcher::push(Event e) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(std::move(e));
    cv_.notify_all();
}

std::vector<std::string> FakeWatcher::dirs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dirs_;
}

}  // namespace thimsy
