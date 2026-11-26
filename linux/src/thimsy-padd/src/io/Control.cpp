// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Control.h"

#include <errno.h>
#include <linux/input.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "thimsy/watch.h"

namespace padd {

std::unique_ptr<Control> Control::create(const thimsy::ConfigStore::Roots& roots,
                                         std::unique_ptr<thimsy::Watcher> watcher, std::string* err) {
    std::unique_ptr<Control> c(new Control);
    c->store_ = thimsy::ConfigStore::open(roots, err);
    if (!c->store_) return nullptr;
    c->eventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (c->eventFd_ < 0) {
        if (err) *err = std::string("eventfd: ") + strerror(errno);
        return nullptr;
    }
    int efd = c->eventFd_;
    // Runs on the store's watcher thread (other processes' writes) or on ours (own sets).
    c->observer_ = c->store_->addObserver(kInterceptKey, [efd](std::string_view, std::string_view) {
        uint64_t one = 1;
        ssize_t n = write(efd, &one, sizeof one);
        (void)n;
    });
    bool ok = watcher ? c->store_->startWatching(std::move(watcher)) : c->store_->startWatching();
    if (!ok) {
        if (err) *err = "can't watch the config store";
        return nullptr;
    }
    return c;
}

Control::~Control() {
    if (store_) {
        store_->stopWatching();
        if (observer_ >= 0) store_->removeObserver(observer_);
    }
    if (eventFd_ >= 0) close(eventFd_);
}

void Control::drain() {
    uint64_t v;
    while (read(eventFd_, &v, sizeof v) == static_cast<ssize_t>(sizeof v)) {
    }
}

bool Control::intercept() const { return store_->get(kInterceptKey) == "1"; }

void Control::setReady(bool ready) { store_->set(kReadyKey, ready ? "1" : ""); }

std::unique_ptr<TapServer> TapServer::create(const std::string& path, std::string* err) {
    std::unique_ptr<TapServer> t(new TapServer);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        if (err) *err = path + ": path too long";
        return nullptr;
    }
    memcpy(addr.sun_path, path.c_str(), path.size());
    t->listenFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (t->listenFd_ < 0) {
        if (err) *err = std::string("socket: ") + strerror(errno);
        return nullptr;
    }
    unlink(path.c_str());  // a stale socket from an earlier padd
    if (bind(t->listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(t->listenFd_, 4) != 0) {
        if (err) *err = path + ": " + strerror(errno);
        return nullptr;
    }
    chmod(path.c_str(), 0660);
    t->path_ = path;
    return t;
}

TapServer::~TapServer() {
    for (int c : clients_) close(c);
    if (listenFd_ >= 0) {
        close(listenFd_);
        unlink(path_.c_str());
    }
}

void TapServer::acceptClients() {
    for (;;) {
        int c = accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR) continue;
            return;
        }
        clients_.push_back(c);
    }
}

void TapServer::send(const std::vector<OutEvent>& events) {
    std::vector<TapRecord> batch;
    for (const OutEvent& e : events) {
        if (e.dev != Dev::Tap) continue;
        batch.push_back(TapRecord{e.type, e.code, e.value});
        if (!(e.type == EV_SYN && e.code == SYN_REPORT)) continue;
        for (size_t i = 0; i < clients_.size();) {
            ssize_t n = ::send(clients_[i], batch.data(), batch.size() * sizeof(TapRecord),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(clients_[i]);
                clients_.erase(clients_.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
        }
        batch.clear();
    }
}

}  // namespace padd
