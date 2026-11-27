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

// The host's fake boot (FakeBoot.h).

#define LOG_TAG "NanoHost"

#include "FakeBoot.h"

#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>

#include <log/log.h>

#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gRecordMu;
BootRecord gRecord;  // guarded by gRecordMu

class FakeBoot : public nanoplat::Boot {
 public:
    explicit FakeBoot(bool nanoOwnsBootScreen) : nanoOwns_(nanoOwnsBootScreen) {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord = BootRecord();
    }

    bool nanoOwnsBootScreen() override {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.ownerChecks++;
        return nanoOwns_;
    }

    void handOverBootScreen(int earlyDrmFd, bool overlayMode) override {
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.handOvers++;
        }
        ALOGI("fake boot: the boot screen is the platform's; exiting");
        event("boot", std::string("\"op\":\"hand_over\",\"overlay\":") + (overlayMode ? "true" : "false"));
        if (earlyDrmFd >= 0) close(earlyDrmFd);
        _exit(0);
    }

    void awaitSavedSettings(SettingsReader reader) override {
        std::lock_guard<std::mutex> lock(gRecordMu);
        switch (reader) {
        case SettingsReader::QuickResumeCheck:
            gRecord.quickResumeWaits++;
            break;
        case SettingsReader::MenuSetup:
            gRecord.menuSetupWaits++;
            break;
        }
    }

    bool savedSettingsLoaded() override {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.loadedChecks++;
        return true;
    }

    bool soundSystemReady() override {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.soundChecks++;
        return true;
    }

 private:
    const bool nanoOwns_;
};

}  // namespace

std::unique_ptr<nanoplat::Boot> createFakeBoot(bool nanoOwnsBootScreen) {
    return std::unique_ptr<nanoplat::Boot>(new FakeBoot(nanoOwnsBootScreen));
}

BootRecord bootRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

}  // namespace nanohost
