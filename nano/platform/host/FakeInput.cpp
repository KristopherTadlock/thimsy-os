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

// The host's fake input policy (FakeInput.h).

#define LOG_TAG "NanoHost"

#include "FakeInput.h"

#include <errno.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>

#include <memory>
#include <mutex>
#include <string>

#include <cutils/properties.h>
#include <log/log.h>

#include "HostControl.h"

namespace nanohost {

namespace {

std::mutex gRecordMu;
InputRecord gRecord;  // guarded by gRecordMu

class FakeInput : public nanoplat::InputPolicy {
 public:
    FakeInput() {
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord = InputRecord();
    }

    void deviceOpened(int fd, const char* path, Opened) override {
        const bool grab = property_get_bool("persist.gammaos.nano.grab_input", false);
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
            ALOGW("fake input: EVIOCGRAB failed for %s: %s", path, strerror(errno));
        }
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.opened++;
        if (grab) gRecord.grabs++;
    }

    void deviceClosing(int fd) override {
        ioctl(fd, EVIOCGRAB, 0);
        std::lock_guard<std::mutex> lock(gRecordMu);
        gRecord.closing++;
    }

    void silenceGamePad(bool on) override {
        {
            std::lock_guard<std::mutex> lock(gRecordMu);
            gRecord.silenceCalls++;
            gRecord.silenced = on;
        }
        event("input", std::string("\"op\":\"silence\",\"on\":") + (on ? "true" : "false"));
    }
};

}  // namespace

std::unique_ptr<nanoplat::InputPolicy> createFakeInput() {
    return std::unique_ptr<nanoplat::InputPolicy>(new FakeInput);
}

InputRecord inputRecord() {
    std::lock_guard<std::mutex> lock(gRecordMu);
    return gRecord;
}

}  // namespace nanohost
