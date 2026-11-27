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

// FakeEmulators (platform/host/FakeEmulators.h): the host config's scripted emulators, parsed by
// HostConfig, and answered the way the platforms answer NanoPlatform.h's Emulators.

#include <gtest/gtest.h>

#include "platform/host/FakeEmulators.h"
#include "platform/host/HostConfig.h"

using namespace nanohost;

namespace {

TEST(FakeEmulatorsConfig, ParsesTheLists) {
    HostConfig c;
    std::string err;
    ASSERT_TRUE(parseHostConfig("{}", &c, &err)) << err;
    EXPECT_TRUE(c.emulators.cores.empty());
    EXPECT_TRUE(c.emulators.apps.empty());
    EXPECT_TRUE(c.emulators.defaults.empty());
    ASSERT_TRUE(parseHostConfig(R"({"emulators": {"cores": ["snes9x_libretro_android.so"],
        "apps": ["org.ppsspp.ppsspp"],
        "defaults": [{"system": "snes", "core": "snes9x_libretro_android.so"},
                     {"system": "psp", "package": "org.ppsspp.ppsspp", "intent": "-n x/.A -d {file.uri}"}]}})",
                                &c, &err)) << err;
    EXPECT_EQ(c.emulators.cores, std::vector<std::string>{"snes9x_libretro_android.so"});
    EXPECT_EQ(c.emulators.apps, std::vector<std::string>{"org.ppsspp.ppsspp"});
    ASSERT_EQ(c.emulators.defaults.size(), 2u);
    EXPECT_EQ(c.emulators.defaults[0].system, "snes");
    EXPECT_EQ(c.emulators.defaults[0].coreSo, "snes9x_libretro_android.so");
    EXPECT_EQ(c.emulators.defaults[1].package, "org.ppsspp.ppsspp");
    EXPECT_EQ(c.emulators.defaults[1].intent, "-n x/.A -d {file.uri}");
}

TEST(FakeEmulatorsConfig, RefusesWhatItCannotUse) {
    HostConfig c;
    std::string err;
    EXPECT_FALSE(parseHostConfig(R"({"emulators": {"cores": "snes9x"}})", &c, &err));
    EXPECT_FALSE(parseHostConfig(R"({"emulators": {"cores": [""]}})", &c, &err));
    EXPECT_FALSE(parseHostConfig(R"({"emulators": {"other": []}})", &c, &err));
    EXPECT_FALSE(parseHostConfig(R"({"emulators": {"defaults": [{"core": "x_libretro_android.so"}]}})", &c, &err));
    EXPECT_FALSE(parseHostConfig(R"({"emulators": {"defaults": [{"system": "snes"}]}})", &c, &err));
    EXPECT_FALSE(parseHostConfig(
            R"({"emulators": {"defaults": [{"system": "snes", "core": "x_libretro_android.so", "package": "p"}]}})",
            &c, &err));
}

TEST(FakeEmulators, AnswersFromTheLists) {
    FakeEmulatorsConfig cfg;
    cfg.cores = {"snes9x_libretro_android.so"};
    cfg.apps = {"org.ppsspp.ppsspp"};
    nanoplat::SystemEmulator d;
    d.system = "snes";
    d.coreSo = "snes9x_libretro_android.so";
    cfg.defaults = {d};
    std::unique_ptr<nanoplat::Emulators> e = makeFakeEmulators(cfg);
    EXPECT_TRUE(e->coreInstalled("snes9x_libretro_android.so"));
    EXPECT_TRUE(e->coreInstalled("/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so"));
    EXPECT_FALSE(e->coreInstalled("mednafen_psx_hw_libretro_android.so"));
    EXPECT_TRUE(e->coreInstalled(""));
    EXPECT_TRUE(e->appInstalled("org.ppsspp.ppsspp"));
    EXPECT_FALSE(e->appInstalled("com.dsemu.drastic"));
    EXPECT_TRUE(e->appInstalled(""));
    ASSERT_EQ(e->imageDefaults().size(), 1u);
    EXPECT_EQ(e->imageDefaults()[0].system, "snes");
}

TEST(FakeEmulators, NothingListedNothingRuns) {
    std::unique_ptr<nanoplat::Emulators> e = makeFakeEmulators(FakeEmulatorsConfig());
    EXPECT_FALSE(e->coreInstalled("snes9x_libretro_android.so"));
    EXPECT_FALSE(e->appInstalled("org.ppsspp.ppsspp"));
    EXPECT_TRUE(e->imageDefaults().empty());
}

}  // namespace
