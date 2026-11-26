/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The compat layer's own controls, for the Linux and host platforms and the tests. Nano's shared
// files never include this: they reach the layer only through Android's header names
// (docs/linux/IMPLEMENTATION.md B.2).
//
// Properties. Every property shim (cutils/properties.h, android-base/properties.h,
// sys/system_properties.h) reads and writes one process-wide thimsy::ConfigStore (libthimsy, B.3).
// The platform sets it up with nanocompat_init() before any Nano thread starts. If nothing calls
// it, the first property call opens a default store:
//   - THIMSY_PROP_ROOT=<dir>: <dir>/persist.props and <dir>/run (tests and host runs);
//   - otherwise thimsy::ConfigStore::defaultRoots() (the device paths, or THIMSY_PERSIST_FILE and
//     THIMSY_RUN_DIR).
// The default store does not watch for other processes' writes. If it can't be opened, every get
// returns its default and every set fails, and the reason is logged once.
//
// Control keys. Some property writes are messages, not settings (B.2, decision I28). The store
// always stores a write first; observers registered here are told afterwards and can't veto or
// swallow it. Unlike the store's own observers, they are told of every successful set this process
// makes, even one that writes the value the key already has, because Android's property triggers
// fire on every set. Writes by other processes reach them only while the store is watching.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace thimsy {
class ConfigStore;
}

namespace nanocompat {

struct InitOptions {
    std::string persistFile;   // e.g. /storage/.config/thimsy/persist.props
    std::string runDir;        // e.g. /run/thimsy/props
    // Start the store's watcher thread, so other processes' writes reach the cache, the serials,
    // __system_property_wait and the observers at once. The Linux platform wants this.
    bool watch = true;
};

// Opens the process-wide store (and starts its watcher if asked). Call it before any Nano thread
// starts. Calling it again replaces the store: meant for tests, not for a running Nano (the old
// store is kept alive, so a thread still holding it is safe, but it stops seeing new writes).
// Returns false, with *err set, if the store can't be opened; the previous store, if any, stays.
bool init(const InitOptions& options, std::string* err = nullptr);

// The process-wide store, opened on first use as described above; nullptr if it can't be opened.
thimsy::ConfigStore* store();

// Observers of property writes. keyOrPrefix is a key, a prefix ending in '.' or '*' (the '*' is
// dropped), or empty for every key. The callback runs after the value is stored, on the thread
// that set it (or the store's watcher thread for other processes' writes); value is empty when the
// key was cleared. A callback may read and set properties; one that throws is ignored. Observers
// survive init(). Returns an id for removePropertyObserver.
using PropertyObserver = std::function<void(std::string_view key, std::string_view value)>;
int addPropertyObserver(std::string_view keyOrPrefix, PropertyObserver observer);
void removePropertyObserver(int id);

// Logging (log/log.h, android/log.h). Every message is one line to stderr,
// "<uptime ms> <V|D|I|W|E|F> <tag>: <message>"; a message with several lines is written as several
// lines with the same prefix.
// The lowest priority written (ANDROID_LOG_* values; the default writes everything).
void setLogMinPriority(int priority);
// Also append every line to `path` (the Linux image uses /run/thimsy/log/nano.log): when the file
// passes maxBytes / 2 it is renamed to `path`.1, replacing the older half, so the two together stay
// under maxBytes. An empty path turns it off. When stdout or stderr already is that file (the
// session starts Nano with both on it), the ring takes them over: lines are not also written to
// stderr, the size counts their writes, and each rotation moves them to the new half (dup2).
void setLogFile(const std::string& path, size_t maxBytes = 256 * 1024);
// Replaces stderr as the line sink (tests); nullptr restores stderr. The file above still gets
// every line. The sink gets the priority, the tag ("" for none) and one message (without the
// prefix; may contain newlines).
using LogSink = std::function<void(int priority, const char* tag, const char* message)>;
void setLogSink(LogSink sink);

}  // namespace nanocompat
