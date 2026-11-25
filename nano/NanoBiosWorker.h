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

// The BIOS installer's worker (NanoMenu::biosWorker runs it): no Android UI code, everything outside
// it (programs run, the screen woken, free space) through Env, so host tests can run it on a tree
// of their own.
#pragma once

#include "NanoBiosCatalog.h"
#include "NanoBiosUtil.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace android {
namespace nanobios {

inline constexpr const char* kManifestPath = "/data/system/nano_bios/installed.json";
inline constexpr const char* kReinstallMarker = "/data/system/nano_bios/reinstall_pending";   // a reinstall an exit cut short

struct InstallProgress {
    bool running = false, done = false;
    int scanned = 0, total = 0;
    std::string current;
    int installed = 0, already = 0, unknown = 0, conflicts = 0;
    bool noneFound = false;   // nothing in the folder was recognised (not said after a cancel)
    std::vector<std::string> lines;
};

// The worker's state, shared with its thread so it outlives the menu if need be.
struct InstallShared {
    std::mutex mu;                                                    // guards the three below
    InstallProgress progress;
    bool busy = false, reinstallQueued = false;
    std::atomic<bool> running{false};                                 // launches wait; ~NanoMenu waits (bounded)
    std::atomic<bool> cancel{false};
    std::atomic<bool> closing{false};                                 // ~NanoMenu: nothing new starts
    std::string queuedRaConfig;                                       // the queued reinstall's retroarch.cfg (under mu)
};

struct Env {
    std::string root;                      // every fixed path and allowed folder taken under it ("" on the device)
    RunFn run;                             // runArgv: am, restorecon, logcat, pidof
    Region region = REGION_USA;            // the recommended region for the locale
    std::function<void()> wake;            // the report is ready
    std::function<void()> wakeThrottled;   // progress
    // Free space instead of statvfs: false = not known.
    std::function<bool(const std::string& path, uint64_t* blockSize, uint64_t* avail)> freeSpace;
};

// One install (folder: the chosen folder) or reinstall from the manifest (raConfig: the retroarch.cfg
// launches use). True when a queued reinstall is to run next, its retroarch.cfg in *nextRaConfig.
bool runInstall(const Env& env, InstallShared& sh, const std::string& folder, bool fromManifest,
                const std::string& raConfig, std::string* nextRaConfig);

}  // namespace nanobios
}  // namespace android
