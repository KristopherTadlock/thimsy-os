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

// The host platform's Emulators (docs/linux/IMPLEMENTATION.md B.6, D.3, W8-EMU): the host config's
// "emulators" (HostConfig.h), a scripted answer to which cores and apps can run and what the image's
// default emulators are. A core is named as Nano names it, by its Android file name; a launch field's
// full path counts by its last component. Nothing listed: nothing runs (Linux's fail-closed rule).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct FakeEmulatorsConfig {
    std::vector<std::string> cores;                  // "<core>_libretro_android.so" that can run
    std::vector<std::string> apps;                   // packages that can run
    std::vector<nanoplat::SystemEmulator> defaults;  // imageDefaults(), as listed
};

std::unique_ptr<nanoplat::Emulators> makeFakeEmulators(FakeEmulatorsConfig config);

}  // namespace nanohost
