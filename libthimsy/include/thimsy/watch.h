// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// A small directory watcher: inotify on Linux, and a fake with the same interface for tests (the
// test seam). Events name a directory added with addDir and an entry in it.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

namespace thimsy {

class Watcher {
 public:
    enum : uint32_t {
        kWritten = 1u << 0,  // an entry was created, closed after writing, or renamed into the dir
        kRemoved = 1u << 1,  // an entry was deleted or renamed out of the dir
        kOverflow = 1u << 2, // events were lost: rescan everything (dir and name are empty)
    };
    struct Event {
        std::string dir;   // as passed to addDir
        std::string name;  // the entry's name, without the directory
        uint32_t what = 0; // the k* bits above
    };

    virtual ~Watcher() = default;
    // Starts watching a directory (not recursive). Returns false if it can't.
    virtual bool addDir(const std::string& dir) = 0;
    // Waits up to timeoutMs (negative: forever) for events or wake(); appends them to *out.
    // Returns false only on an unrecoverable error.
    virtual bool wait(std::vector<Event>* out, int timeoutMs) = 0;
    // Makes a blocked wait() return at once; safe from any thread.
    virtual void wake() = 0;

    // The inotify implementation; nullptr if inotify is unavailable.
    static std::unique_ptr<Watcher> createInotify();
};

// The test seam: events are pushed by the test, not seen on disk.
class FakeWatcher : public Watcher {
 public:
    bool addDir(const std::string& dir) override;
    bool wait(std::vector<Event>* out, int timeoutMs) override;
    void wake() override;

    void push(Event e);
    std::vector<std::string> dirs() const;

 private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
    std::vector<std::string> dirs_;
    bool woken_ = false;
};

}  // namespace thimsy
