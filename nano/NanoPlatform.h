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

// Nano's platform seam, layer 4 (docs/linux/IMPLEMENTATION.md B.6). C++17; no Android or Linux
// system header is included, and none may be.
//
// Declared when used, not ahead: this header holds only the platform object and how it is made,
// installed and reached. Each interface (Display, Launcher, Audio, Volumes, Power, SystemActions,
// InputPolicy, and the Cap set) is declared by the step that moves the code behind it, in the same
// commit as the move, sized to that code.
//
// Who may use it. drastic-nano, gammaos-nano-scrape, the host tests and the other binaries built
// from Nano's files never call install(). No file they compile may include this header or name
// nanoplat:: (tools/nano-tree/tests/test_nano_ratchets.py holds that rule); they reach platform
// behaviour through the shims and link-time substitution (layers 1 to 3).
//
// Ownership and threading. main() calls createPlatform(), then install(), before any Nano thread
// starts. The platform then lives until the process exits and is never destroyed, so a reference
// from get() stays valid on every thread, including during exit. Which platform is linked is a
// link-time choice: each platform library (platform/firmware, platform/linux, platform/host) defines
// createPlatform() once.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace nanoplat {

// --- Volumes (G7-volumes) -------------------------------------------------------------------------
// The removable volumes, a microSD card or a USB drive, as apps see them: /storage/<id>. What
// NanoSdMove.cpp's and NanoEsdeStore.cpp's /storage and /mnt/media_rw scans asked the filesystem,
// moved behind one interface. Any thread. Every call may touch the filesystem (on Linux it also
// reads /run/thimsy/volumes.json when the session has changed it), so none is for a frame's hot path.
class Volumes {
 public:
    virtual ~Volumes() = default;
    // The microSD card: the first removable volume Nano can write to (/storage/<id>), the slot's
    // before a USB drive's; "" when there is none (NanoMenu::sdCardRoot).
    virtual std::string cardRoot() = 0;
    // The volume at storagePath (/storage/<id>) is the microSD slot's (an MMC disk), not a USB
    // drive's (NanoSdMove.cpp's sdIsSlot).
    virtual bool isSlot(const std::string& storagePath) = 0;
    // The volume `id` is in: its /storage/<id> is there (NanoMenu::esdeStoreFollowCard).
    virtual bool present(const std::string& id) = 0;
    // The volume `id` is not in: neither its raw mount nor /storage/<id> is there (NanoSdMove.cpp's
    // sdVolumeAbsent).
    virtual bool absent(const std::string& id) = 0;
    // /storage/<id><rel> for each removable volume but `vol` where that is a folder Nano can write
    // to, in no set order (NanoMenu::esdeStoreFollowCard: the ES-DE folder on another card).
    virtual std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) = 0;
};

// The platform's services, one member per G7 area. Each platform library derives from it to keep
// its own state, and its createPlatform() sets every member, so call sites use them untested.
struct Platform {
    virtual ~Platform();
    std::unique_ptr<Volumes> volumes;   // G7-volumes
};

struct PlatformArgs {
    int argc = 0;
    char** argv = nullptr;
};

// Made by the linked platform library. Returns nullptr when the platform can't start (the reason
// is logged); main() then exits with an error.
std::unique_ptr<Platform> createPlatform(const PlatformArgs& args);

// Makes `platform` the process's platform. Call it once, from main(), before any Nano thread
// starts. Aborts with a message when `platform` is null or a platform is already installed.
void install(std::unique_ptr<Platform> platform);

// The installed platform; any thread. Never null: aborts with a message if install() was never
// called (which the rule above keeps out of the binaries that don't call it).
Platform& get();

}  // namespace nanoplat
