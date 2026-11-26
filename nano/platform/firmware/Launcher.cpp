/*
 * Copyright (C) 2026 GammaOS
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

// The firmware's nanoplat::Launcher (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6): the launch
// handshake's writes, moved verbatim from commitLaunch() in NanoLaunchCommit.cpp. The class sits in
// namespace android, as the moved lines did, and the file has NanoLaunchCommit.cpp's LOG_TAG
// "GammaOSNano" (NanoMenu.cpp's) for NanoMenuUtils.h's inline helpers; commit itself logs nothing.
// Android's framework starts the activity from the handshake as Nano exits, so this launcher never
// starts a child: launch() refuses, and there is never a child to poll or to quit.

#define LOG_TAG "GammaOSNano"

#include <memory>
#include <optional>
#include <string>

#include <cutils/properties.h>

#include "Launcher.h"
#include "NanoLaunchCommit.h"
#include "NanoMenuUtils.h"   // setLaunchRomPath, setLaunchGame
#include "NanoPlatform.h"

namespace android {
namespace {

class FirmwareLauncher : public nanoplat::Launcher {
 public:
    bool nanoExitsOnLaunch() const override { return true; }

    void commit(const LaunchTuple& tuple) override {
        for (const LaunchTuple::Write& w : tuple.writes) {
            switch (w.key) {
                case LaunchKey::RomPath:
                    setLaunchRomPath(w.value);
                    break;
                case LaunchKey::Game:
                    setLaunchGame(w.value, w.rom, w.core);
                    break;
                default:
                    property_set(launchKeyProperty(w.key), w.value.c_str());
                    break;
            }
        }
    }

    bool launch(const nanoplat::LaunchRequest& /*request*/, std::string* error) override {
        if (error) *error = "the firmware launches through the handshake";
        return false;
    }
    bool childRunning() const override { return false; }
    std::optional<nanoplat::ChildExit> pollChild() override { return std::nullopt; }
    void requestChildQuit() override {}
};

}  // namespace
}  // namespace android

namespace nanofirmware {

std::unique_ptr<nanoplat::Launcher> createLauncher() {
    return std::unique_ptr<nanoplat::Launcher>(new android::FirmwareLauncher);
}

}  // namespace nanofirmware
