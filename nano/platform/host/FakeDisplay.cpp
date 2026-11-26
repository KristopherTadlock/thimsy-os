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

// The host's fake display (FakeDisplay.h).

#define LOG_TAG "NanoHost"

#include "FakeDisplay.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <log/log.h>

#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gRecordMu;
DisplayRecord gRecord;  // guarded by gRecordMu

class FakeDisplay : public nanoplat::Display {
 public:
    explicit FakeDisplay(std::unique_ptr<HostDrm> drm) : drm_(std::move(drm)) {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord = DisplayRecord();
    }

    int startScanout(int earlyDrmFd, bool, bool (*)()) override {
        drm_->earlySplash(earlyDrmFd);  // as on Linux: no SurfaceFlinger, so always the panel
        const int w = drm_->active() ? drm_->width() : 0;
        const int h = drm_->active() ? drm_->height() : 0;
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.starts++;
            gRecord.width = w;
            gRecord.height = h;
        }
        event("display", "\"op\":\"start\",\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                             ",\"rotation\":" + std::to_string(drm_->rotationDeg()));
        return -1;
    }

    bool releaseForChild() override {
        if (released_ || !drm_->active()) {
            ALOGW("fake display: release without the panel");
            return false;
        }
        drm_->waitForFlips();
        drm_->rememberInstallMatrix();
        drm_->stop();
        released_ = true;
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.releases++;
        }
        event("display", "\"op\":\"release\"");
        return true;
    }

    bool reacquire() override {
        if (!released_) return drm_->active();
        const bool ok = drm_->takeBack(8000);
        if (ok) released_ = false;
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.reacquires++;
        }
        event("display", std::string("\"op\":\"reacquire\",\"ok\":") + (ok ? "true" : "false"));
        return ok;
    }

 private:
    std::unique_ptr<HostDrm> drm_;
    bool released_ = false;
};

}  // namespace

std::unique_ptr<nanoplat::Display> createFakeDisplay(std::unique_ptr<HostDrm> drm) {
    if (!drm) return nullptr;
    return std::unique_ptr<nanoplat::Display>(new FakeDisplay(std::move(drm)));
}

DisplayRecord displayRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

}  // namespace nanohost
