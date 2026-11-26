// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-session's wiring: one epoll loop over signals (signalfd), the config store's run
// directory (inotify), kernel uevents, the input nodes and pidfds, feeding the pure cores and
// running what they return. linux/docs/session.md describes the behaviour and the files.
#pragma once

#include <string>

namespace thimsy {
namespace session {

struct Options {
    std::string root;      // prefix for every path ("" on a device)
    bool fakeHw = false;   // FIFOs for uevents and keys, mount(8) programs, no namespace
    std::string toolDir;   // programs here replace the system's, by base name
    std::string nanoPath = "/usr/bin/thimsy-nano";
    std::string scrapePath = "/usr/bin/thimsy-nano-scrape";
    std::string paddPath = "/usr/bin/thimsy-padd";
    bool verbose = false;  // also log to stderr
};

// Runs until SIGTERM, SIGINT or SIGHUP. Returns the process exit status.
int runSession(const Options& opts);

}  // namespace session
}  // namespace thimsy
