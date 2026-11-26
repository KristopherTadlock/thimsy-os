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
// installs it, with NanoPlatform.cpp's install() and get().

#include <memory>

#include <gtest/gtest.h>

#include "NanoPlatform.h"
#include "AudioPull.h"

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

}  // namespace
