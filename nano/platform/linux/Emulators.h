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

// The Linux platform's Emulators (docs/linux/IMPLEMENTATION.md B.6, W8-EMU; LIBRARY-FORMAT.md 5).
//
// - coreInstalled: Nano names a core by its Android file name (<core>_libretro_android.so), as the
//   launcher reads it (launcher::coreName); the core can run when the image ships it, a non-empty
//   regular file <coreDir>/<core>_libretro.so, the file the launcher starts RetroArch with. Anything
//   else, a name that names no core included, is false: Linux fails closed.
// - appInstalled: false for every package. The launcher starts no standalone emulator yet (plan 6.4:
//   the emulator definitions' Linux block), and an Android package is never installed here, so the
//   ES-DE move and the app-mirror sync find no Android app to act for (LIBRARY-FORMAT.md 9, G6).
// - imageDefaults: Linux's table of the image's default emulator per system (defaultEmulators()),
//   only the entries whose core the image ships; Nano writes them into the device's store of
//   defaults on its first run (OA-38).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Launcher.h"
#include "NanoPlatform.h"

namespace nanolinux {

struct EmulatorsOptions {
    std::string coreDir = launcher::Options().coreDir;  // where the image's cores are
};

// Linux's table: the default core for each system Linux has one for, every entry a core (the
// cores of linux/configs/fragments/apps-alpha.fragment and apps-v1.fragment), in Nano's names.
const std::vector<nanoplat::SystemEmulator>& defaultEmulators();

std::unique_ptr<nanoplat::Emulators> makeEmulators(EmulatorsOptions options = EmulatorsOptions());

}  // namespace nanolinux
