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

// The Linux nanoplat::Boot (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 2, defect 1).
//
// - Nano owns the screen on every boot: thimsy-session starts it as the only user interface, and
//   there is no other boot screen to hand the display to. So nanoOwnsBootScreen() is true at once,
//   reading nothing (none of Android's "boot to Android" state: no /data/misc/bootanim file, no
//   persist.bootanim.skip_nano, no wait for ro.persistent_properties.ready), and
//   handOverBootScreen() is never reached; if it is, it logs, leaves the fd alone and returns.
// - The saved settings are loaded before Nano's first thread: main_linux.cpp opens the property
//   store (persist.props) synchronously before it creates the platform. So awaitSavedSettings()
//   returns at once and savedSettingsLoaded() is true.
// - Nano's sound goes to ALSA directly, with no service to start first: soundSystemReady() is true
//   (a missing card fails the stream's own open, at once).
#pragma once

#include <memory>

#include "NanoPlatform.h"

namespace nanolinux {
namespace boot {

std::unique_ptr<nanoplat::Boot> create();

}  // namespace boot
}  // namespace nanolinux
