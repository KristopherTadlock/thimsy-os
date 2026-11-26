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

// The Linux platform's createPlatform() (LinuxPlatform.cpp), installed the way main_linux.cpp
// installs it, with NanoPlatform.cpp's install() and get(). The display's KMS binding
// (display/DisplayKms.cpp) needs NanoMenuDrm.cpp, so this program defines createKms() itself.

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "AudioPull.h"
#include "Launcher.h"
#include "NanoPlatform.h"
#include "display/Display.h"

namespace {

class NoKms : public nanolinux::display::Kms {
 public:
    void earlySplash(int) override {}
    void finishGpu() override {}
    bool active() const override { return false; }
    void setRotationDeg(int) override {}
    std::string panelOrientation() override { return ""; }
    void waitForFlips() override {}
    int disableOverlayPlanes() override { return 0; }
    void rememberInstallMatrix() override {}
    void stop() override {}
    bool takeBack(int) override { return false; }
};

}  // namespace

std::unique_ptr<nanolinux::display::Kms> nanolinux::display::createKms() {
    return std::unique_ptr<Kms>(new NoKms);
}

// AudioAlsa.cpp needs alsa-lib's headers, which this build has not: its makeAudio() is stood in
// for by a PullAudio whose sink never opens (G7-audio).
std::unique_ptr<nanoplat::Audio> nanolinux::makeAudio() {
    return std::unique_ptr<nanoplat::Audio>(new PullAudio([](const nanoplat::AudioFormat&, int* error) {
        *error = -19;
        return std::unique_ptr<PcmSink>();
    }));
}

namespace {

TEST(LinuxPlatform, CreatesAndInstalls) {
    char arg0[] = "thimsy-nano";
    char* argv[] = {arg0, nullptr};
    nanoplat::PlatformArgs args;
    args.argc = 1;
    args.argv = argv;
    std::unique_ptr<nanoplat::Platform> platform = nanoplat::createPlatform(args);
    ASSERT_TRUE(platform != nullptr);
    EXPECT_TRUE(platform->volumes != nullptr);   // G7-volumes
    nanoplat::Platform* raw = platform.get();
    nanoplat::install(std::move(platform));
    EXPECT_TRUE(&nanoplat::get() == raw);
}

TEST(LinuxPlatform, WiresTheAudio) {
    std::unique_ptr<nanoplat::Platform> platform = nanoplat::createPlatform(nanoplat::PlatformArgs());
    ASSERT_TRUE(platform != nullptr);
    ASSERT_TRUE(platform->audio != nullptr);
    nanoplat::AudioFormat format;
    format.name = "Test";
    format.rate = 48000;
    format.channels = 2;
    EXPECT_TRUE(platform->audio->open(format, nullptr, nullptr) == nullptr);  // the stand-in's sink fails
}

TEST(LinuxPlatform, EachCallMakesItsOwn) {
    nanoplat::PlatformArgs args;
    std::unique_ptr<nanoplat::Platform> a = nanoplat::createPlatform(args);
    std::unique_ptr<nanoplat::Platform> b = nanoplat::createPlatform(args);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(a.get() != b.get());
}

TEST(LinuxPlatform, HasEveryService) {
    nanoplat::PlatformArgs args;
    std::unique_ptr<nanoplat::Platform> p = nanoplat::createPlatform(args);
    ASSERT_TRUE(p != nullptr);
    EXPECT_TRUE(p->display != nullptr);
    // With no display found, the hand-off has nothing to release.
    EXPECT_EQ(p->display->startScanout(-1, false, nullptr), -1);
    EXPECT_FALSE(p->display->releaseForChild());
    // The launcher (G7-launch-b): Nano stays; nothing is running until a launch.
    ASSERT_TRUE(p->launcher != nullptr);
    EXPECT_FALSE(p->launcher->nanoExitsOnLaunch());
    EXPECT_TRUE(dynamic_cast<nanolinux::launcher::LinuxLauncher*>(p->launcher.get()) != nullptr);
    EXPECT_FALSE(p->launcher->childRunning());
    EXPECT_FALSE(p->launcher->pollChild().has_value());
    // G7-power: the power and the system actions (no lights HAL on Linux).
    ASSERT_TRUE(p->power != nullptr);
    EXPECT_FALSE(p->power->setBrightnessViaHal(10));
    EXPECT_TRUE(p->actions != nullptr);
    EXPECT_TRUE(p->input != nullptr);  // G7-input
}

}  // namespace
