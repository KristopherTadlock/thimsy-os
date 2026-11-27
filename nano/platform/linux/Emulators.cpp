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

// The Linux Emulators: see Emulators.h.

#include "Emulators.h"

#include <sys/stat.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Launcher.h"
#include "NanoPlatform.h"

namespace nanolinux {

namespace {

nanoplat::SystemEmulator core(const char* system, const char* name) {
    nanoplat::SystemEmulator e;
    e.system = system;
    e.coreSo = std::string(name) + "_libretro_android.so";
    return e;
}

class LinuxEmulators : public nanoplat::Emulators {
 public:
    explicit LinuxEmulators(EmulatorsOptions options) : options_(std::move(options)) {}

    bool coreInstalled(const std::string& coreSo) override {
        if (coreSo.empty()) return true;
        const std::string name = launcher::coreName(coreSo);
        if (name.empty()) return false;
        struct stat st;
        const std::string path = options_.coreDir + "/" + name + "_libretro.so";
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    }

    bool appInstalled(const std::string& package) override { return package.empty(); }

    std::vector<nanoplat::SystemEmulator> imageDefaults() override { return defaultEmulators(); }

 private:
    const EmulatorsOptions options_;
};

}  // namespace

const std::vector<nanoplat::SystemEmulator>& defaultEmulators() {
    // Keyed by the ES-DE folder names Nano's systems use; Nano's built-in systems first, in
    // kXmbSystemDefs' order (PSP has no entry: PPSSPP is an app, which Linux starts none of yet).
    static const std::vector<nanoplat::SystemEmulator> kTable = {
        core("nes", "mesen"),
        core("snes", "snes9x"),
        core("gb", "sameboy"),
        core("gbc", "sameboy"),
        core("gba", "mgba"),
        core("n64", "mupen64plus_next"),
        core("nds", "melondsds"),
        core("genesis", "genesis_plus_gx"),
        core("mastersystem", "genesis_plus_gx"),
        core("gamegear", "genesis_plus_gx"),
        core("psx", "pcsx_rearmed"),
        core("dreamcast", "flycast"),
        core("ngpc", "mednafen_ngp"),
        core("pico8", "fake08"),
        // Other systems a card's systems.json can name, with a core the image ships.
        core("megadrive", "genesis_plus_gx"),
        core("segacd", "genesis_plus_gx"),
        core("sega32x", "picodrive"),
        core("ngp", "mednafen_ngp"),
        core("pcengine", "mednafen_pce_fast"),
        core("atari2600", "stella"),
        core("atari5200", "a5200"),
        core("atari7800", "prosystem"),
        core("atarilynx", "mednafen_lynx"),
        core("colecovision", "gearcoleco"),
        core("msx", "bluemsx"),
        core("virtualboy", "mednafen_vb"),
        core("wonderswan", "mednafen_wswan"),
        core("wonderswancolor", "mednafen_wswan"),
        core("3do", "opera"),
        core("saturn", "yabasanshiro"),
        core("fbneo", "fbneo"),
        core("arcade", "mamearcade"),
        core("mame", "mame2003_plus"),
    };
    return kTable;
}

std::unique_ptr<nanoplat::Emulators> makeEmulators(EmulatorsOptions options) {
    return std::unique_ptr<nanoplat::Emulators>(new LinuxEmulators(std::move(options)));
}

}  // namespace nanolinux
