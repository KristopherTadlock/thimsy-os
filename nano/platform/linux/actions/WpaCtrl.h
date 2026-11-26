// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// A small client for wpa_supplicant's control interface (G7-power), written for Thimsy in place of
// hostap's wpa_ctrl.c, which is not on this machine to vendor (docs/linux/IMPLEMENTATION.md B.6
// names it). The protocol is the documented one: a Unix datagram socket per interface in the
// ctrl_interface directory (/run/wpa_supplicant/wlan0, D30net's), the client binds a socket of its
// own so the daemon can answer, sends one command ("SCAN", "REMOVE_NETWORK 3", "SAVE_CONFIG") per
// datagram and reads one reply ("OK\n", "FAIL\n", or the command's text). Unsolicited event
// messages ("<3>CTRL-EVENT-...") only reach a client that sent ATTACH; any that arrive anyway are
// skipped. Not thread-safe: one Ctrl per thread.
#pragma once

#include <string>
#include <vector>

namespace nanolinux {
namespace wpa {

class Ctrl {
 public:
    Ctrl() = default;
    ~Ctrl();
    Ctrl(const Ctrl&) = delete;
    Ctrl& operator=(const Ctrl&) = delete;

    // Connects to the daemon's socket `path`, binding this client's socket in `localDir`
    // (thimsy-wpa-<pid>-<n>, removed by close()). false and *err when it can't.
    bool open(const std::string& path, const std::string& localDir, std::string* err);

    // Sends `command` and waits up to `timeoutMs` for its reply, in *reply (as the daemon sent it,
    // newline included). false and *err on a send or receive failure or the timeout.
    bool request(const std::string& command, std::string* reply, int timeoutMs, std::string* err);

    void close();
    bool isOpen() const { return fd_ >= 0; }

 private:
    int fd_ = -1;
    std::string local_;
};

// The control sockets in `dir` (one per interface), by name with P2P devices' ("p2p-dev-wlan0")
// last; a Ctrl's own sockets (thimsy-wpa-*) are left out.
std::vector<std::string> sockets(const std::string& dir);

// "OK" as wpa_supplicant answers it (with or without the newline).
bool isOk(const std::string& reply);

}  // namespace wpa
}  // namespace nanolinux
