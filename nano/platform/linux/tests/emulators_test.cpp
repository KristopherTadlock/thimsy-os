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

// The Linux Emulators (Emulators.h, W8-EMU): cores answered from a scratch core folder, every app
// refused, and the image defaults: Linux's whole table, each entry a core.

#include "Emulators.h"

#include <stdio.h>
#include <sys/stat.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "NanoPlatform.h"
#include "compat_test_util.h"

namespace {

void writeText(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    ASSERT_TRUE(f != nullptr) << path;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

class EmulatorsTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_FALSE(dir.path().empty());
        cores = dir / "libretro";
        ASSERT_EQ(mkdir(cores.c_str(), 0755), 0);
        nanolinux::EmulatorsOptions o;
        o.coreDir = cores;
        emus = nanolinux::makeEmulators(o);
    }
    void ship(const std::string& name) { writeText(cores + "/" + name + "_libretro.so", "\x7f" "ELF"); }

    compat_test::ScratchDir dir;
    std::string cores;
    std::unique_ptr<nanoplat::Emulators> emus;
};

TEST_F(EmulatorsTest, ACoreRunsWhenTheImageShipsIt) {
    ship("pcsx_rearmed");
    EXPECT_TRUE(emus->coreInstalled("pcsx_rearmed_libretro_android.so"));
    // The path Android's launch fields hold names the same core.
    EXPECT_TRUE(emus->coreInstalled("/data/data/com.retroarch.aarch64/cores/pcsx_rearmed_libretro_android.so"));
    // Android's PlayStation pick, which Linux does not build.
    EXPECT_FALSE(emus->coreInstalled("mednafen_psx_hw_libretro_android.so"));
    // No core to check.
    EXPECT_TRUE(emus->coreInstalled(""));
}

TEST_F(EmulatorsTest, FailsClosed) {
    writeText(cores + "/empty_libretro.so", "");
    EXPECT_FALSE(emus->coreInstalled("empty_libretro_android.so"));   // an empty file is no core
    ASSERT_EQ(mkdir((cores + "/dir_libretro.so").c_str(), 0755), 0);
    EXPECT_FALSE(emus->coreInstalled("dir_libretro_android.so"));     // nor is a folder
    EXPECT_FALSE(emus->coreInstalled(".so"));                         // a name that names no core
    nanolinux::EmulatorsOptions o;
    o.coreDir = dir / "missing";
    EXPECT_FALSE(nanolinux::makeEmulators(o)->coreInstalled("snes9x_libretro_android.so"));
}

TEST_F(EmulatorsTest, NoAppIsInstalled) {
    EXPECT_FALSE(emus->appInstalled("org.ppsspp.ppsspp"));
    EXPECT_FALSE(emus->appInstalled("com.retroarch.aarch64"));
    EXPECT_FALSE(emus->appInstalled("com.dsemu.drastic"));
    EXPECT_TRUE(emus->appInstalled(""));
}

TEST_F(EmulatorsTest, ImageDefaultsAreLinuxsTable) {
    // Whole, shipped or not: a launch asks coreInstalled before it uses one.
    const std::vector<nanoplat::SystemEmulator> d = emus->imageDefaults();
    ASSERT_EQ(d.size(), nanolinux::defaultEmulators().size());
    for (size_t i = 0; i < d.size(); i++) {
        EXPECT_EQ(d[i].system, nanolinux::defaultEmulators()[i].system);
        EXPECT_EQ(d[i].coreSo, nanolinux::defaultEmulators()[i].coreSo);
        EXPECT_TRUE(d[i].intent.empty()) << d[i].system;
    }
    ship("pcsx_rearmed");
    for (const nanoplat::SystemEmulator& e : d) {
        if (e.system == "psx") {
            EXPECT_TRUE(emus->coreInstalled(e.coreSo));
        }
    }
}

TEST(EmulatorsTable, EveryEntryIsACoreInNanosNames) {
    std::set<std::string> seen;
    for (const nanoplat::SystemEmulator& e : nanolinux::defaultEmulators()) {
        EXPECT_TRUE(seen.insert(e.system).second) << "two entries for " << e.system;
        const std::string suffix = "_libretro_android.so";
        ASSERT_GT(e.coreSo.size(), suffix.size()) << e.system;
        EXPECT_EQ(e.coreSo.compare(e.coreSo.size() - suffix.size(), suffix.size(), suffix), 0) << e.coreSo;
        EXPECT_TRUE(e.package.empty()) << e.system;
    }
    // Nano's built-in systems but PSP (an app) have a Linux default.
    for (const char* s : {"nes", "snes", "gb", "gbc", "gba", "n64", "nds", "genesis", "mastersystem", "gamegear",
                          "psx", "dreamcast", "ngpc", "pico8"})
        EXPECT_TRUE(seen.count(s)) << s;
    EXPECT_FALSE(seen.count("psp"));
}

}  // namespace
