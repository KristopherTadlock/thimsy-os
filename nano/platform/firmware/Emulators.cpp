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

// The firmware Emulators (docs/linux/IMPLEMENTATION.md B.6, W8-EMU): NanoMenuXmb.cpp's pre-launch
// checks (NanoMenu::coreSoExists, NanoMenu::packageInstalled), moved here verbatim. The image's
// default emulators are the systems shared code reads already (the build's nano_systems.json, else
// Nano's built-in table), so imageDefaults() has none of its own.

#include "Emulators.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "NanoPlatform.h"

namespace nanoplat {
namespace firmware {

namespace {

class FirmwareEmulators : public Emulators {
 public:
    bool coreInstalled(const std::string& coreSo) override;
    bool appInstalled(const std::string& pkg) override;
    std::vector<SystemEmulator> imageDefaults() override { return {}; }
};

// True when the RetroArch libretro core .so for a system is present on disk. RetroArch keeps its
// cores under its own data dir; this is the exact path every launch branch feeds RetroArch. Fails
// OPEN (returns true) for an empty coreSo so a non-core launch type is never blocked here.
bool FirmwareEmulators::coreInstalled(const std::string& coreSo) {
    if (coreSo.empty()) return true;   // not a libretro-core launch; nothing to check
    std::string corePath = "/data/data/com.retroarch.aarch64/cores/" + coreSo;
    struct stat st;
    if (stat(corePath.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

// True when a standalone emulator package is installed. Reads /data/system/packages.list directly
// (the authoritative DB; the package name is the first space-delimited token of each line) rather
// than mAppEntries, which drops system/com.android.*/com.gammaos.* packages an emulator may live in.
// Fails OPEN (returns true) if the list is unreadable, so a transient state never blocks a launch.
bool FirmwareEmulators::appInstalled(const std::string& pkg) {
    if (pkg.empty()) return true;
    // DS games route to drastic-nano, which runs libdrastic from /system and no
    // longer needs the DraStic APK: the system copy counts as "installed".
    if (pkg == "com.dsemu.drastic" &&
        property_get_bool("persist.gammaos.nano.drastic_nano", false) &&
        access("/system/lib64/libdrastic_arm64.so", R_OK) == 0)
        return true;
    int fd = open("/data/system/packages.list", O_RDONLY);
    if (fd < 0) return true;   // fail-open: never block a launch if the DB cannot be read
    std::string content;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0) content.append(b, (size_t)n);
    close(fd);
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        size_t sp = content.find(' ', pos);
        if (sp != std::string::npos && sp <= eol &&
            content.compare(pos, sp - pos, pkg) == 0)
            return true;
        pos = eol + 1;
    }
    return false;
}

}  // namespace

std::unique_ptr<Emulators> makeEmulators() {
    return std::unique_ptr<Emulators>(new FirmwareEmulators());
}

}  // namespace firmware
}  // namespace nanoplat
