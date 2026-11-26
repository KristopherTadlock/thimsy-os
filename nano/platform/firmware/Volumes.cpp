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

// The firmware Volumes (docs/linux/IMPLEMENTATION.md B.6, G7-volumes): the /storage and
// /mnt/media_rw scans of NanoSdMove.cpp (sdCardRoot, sdIsSlot, sdVolumeAbsent) and NanoEsdeStore.cpp
// (esdeStoreFollowCard's), moved here verbatim. Android's vold makes /storage/<id> and
// /mnt/media_rw/<id> while a volume is mounted, so the folders themselves say what is in.

#include "Volumes.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanoplat {
namespace firmware {

namespace {

// The card is the microSD slot's (an MMC disk), not a USB drive.
bool sdIsSlot(const std::string& storagePath) {
    struct stat st;
    if (stat(("/mnt/media_rw/" + storagePath.substr(storagePath.find_last_of('/') + 1)).c_str(), &st) != 0) return false;
    char sys[64], real[PATH_MAX];
    snprintf(sys, sizeof sys, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    return realpath(sys, real) != nullptr && strstr(real, "/mmc") != nullptr;
}

class FirmwareVolumes : public Volumes {
 public:
    std::string cardRoot() override;
    bool isSlot(const std::string& storagePath) override { return sdIsSlot(storagePath); }
    bool present(const std::string& vol) override;
    bool absent(const std::string& id) override;
    std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) override;
};

// The microSD card: the first removable volume Nano can write to, as apps see it (/storage/XXXX-XXXX),
// the slot's before a USB drive's.
std::string FirmwareVolumes::cardRoot() {
    std::vector<std::string> found;
    if (DIR* d = opendir("/storage")) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == ".." || n == "emulated" || n == "self") continue;
            const std::string p = "/storage/" + n;
            struct stat st;
            // A card or USB drive (a removable volume: it has its raw mount), not a network share.
            if (access(("/mnt/media_rw/" + n).c_str(), F_OK) == 0 && stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)
                && access(p.c_str(), W_OK) == 0)
                found.push_back(p);
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end());
    std::stable_partition(found.begin(), found.end(), sdIsSlot);
    return found.empty() ? std::string() : found[0];
}

bool FirmwareVolumes::present(const std::string& vol) {
    return access(("/storage/" + vol).c_str(), F_OK) == 0;
}

// A volume that is not in: its emulator_config is named in settings only because they came from it.
bool FirmwareVolumes::absent(const std::string& id) {
    struct stat st;
    return stat(("/mnt/media_rw/" + id).c_str(), &st) != 0 && stat(("/storage/" + id).c_str(), &st) != 0;
}

std::vector<std::string> FirmwareVolumes::writableFolders(const std::string& vol, const std::string& rel) {
    std::vector<std::string> found;
    if (DIR* d = opendir("/storage")) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == ".." || n == "emulated" || n == "self" || n == vol) continue;
            const std::string cand = "/storage/" + n + rel;
            struct stat st;
            if (access(("/mnt/media_rw/" + n).c_str(), F_OK) == 0 && stat(cand.c_str(), &st) == 0 && S_ISDIR(st.st_mode)
                && access(cand.c_str(), W_OK) == 0)
                found.push_back(cand);
        }
        closedir(d);
    }
    return found;
}

}  // namespace

std::unique_ptr<Volumes> makeVolumes() {
    return std::unique_ptr<Volumes>(new FirmwareVolumes);
}

}  // namespace firmware
}  // namespace nanoplat
