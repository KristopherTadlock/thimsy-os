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

// createHostDrm() for check-headless-0's programs, which link HostPlatform.cpp without DrmHost.cpp
// and FakeDisplayDrm.cpp (they need NanoMenuDrm.h): a HostDrm that finds no display.

#include <memory>

#include "platform/host/FakeDisplay.h"

namespace nanohost {

namespace {

class NoDisplay : public HostDrm {
 public:
    void earlySplash(int) override {}
    bool active() const override { return false; }
    int width() const override { return 0; }
    int height() const override { return 0; }
    int rotationDeg() const override { return 0; }
    void waitForFlips() override {}
    void rememberInstallMatrix() override {}
    void stop() override {}
    bool takeBack(int) override { return false; }
};

}  // namespace

std::unique_ptr<HostDrm> createHostDrm() { return std::unique_ptr<HostDrm>(new NoDisplay); }

}  // namespace nanohost
