// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// padd's control plane:
//   - sys.thimsy.padd.intercept ("1" on, anything else off), watched through libthimsy's store;
//     a change makes fd() readable, so the epoll loop needs no thread of its own.
//   - sys.thimsy.padd.ready, set to "1" once the pad exists and cleared on exit.
//   - the intercept tap: a SOCK_SEQPACKET socket (default /run/thimsy/padd-tap.sock) on which
//     every client gets, while intercept is on, one packet per batch of the pad's events, each
//     event 8 bytes (TapRecord), the batch ending with SYN_REPORT. Nano's menu reads it
//     (G7-input); the game gets nothing meanwhile.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Merger.h"
#include "thimsy/config.h"

namespace thimsy {
class Watcher;
}

namespace padd {

constexpr const char* kInterceptKey = "sys.thimsy.padd.intercept";
constexpr const char* kReadyKey = "sys.thimsy.padd.ready";
constexpr const char* kDefaultTapPath = "/run/thimsy/padd-tap.sock";

class Control {
 public:
    // watcher: nullptr for inotify (the device); a FakeWatcher in tests.
    static std::unique_ptr<Control> create(const thimsy::ConfigStore::Roots& roots,
                                           std::unique_ptr<thimsy::Watcher> watcher, std::string* err);
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

    int fd() const { return eventFd_; }
    // Clears fd()'s readiness; call when it is readable.
    void drain();
    bool intercept() const;
    void setReady(bool ready);

 private:
    Control() = default;
    std::unique_ptr<thimsy::ConfigStore> store_;
    int eventFd_ = -1;
    int observer_ = -1;
};

#pragma pack(push, 1)
struct TapRecord {
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#pragma pack(pop)
static_assert(sizeof(TapRecord) == 8, "TapRecord is 8 bytes on the wire");

class TapServer {
 public:
    static std::unique_ptr<TapServer> create(const std::string& path, std::string* err);
    ~TapServer();
    TapServer(const TapServer&) = delete;
    TapServer& operator=(const TapServer&) = delete;

    int listenFd() const { return listenFd_; }
    // Accepts every pending client (non-blocking).
    void acceptClients();
    // Sends the Dev::Tap events as packets, one per SYN_REPORT-ended batch. A client that is
    // too slow misses the packet; a client that is gone is dropped.
    void send(const std::vector<OutEvent>& events);
    size_t clients() const { return clients_.size(); }

 private:
    TapServer() = default;
    std::string path_;
    int listenFd_ = -1;
    std::vector<int> clients_;
};

}  // namespace padd
