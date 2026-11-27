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

// The Linux nanoplat::Settings (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 3, the
// settings-dispatcher seam). A keyed table, fail closed:
//
// - kProp: a key in the table of Nano's own preferences (Settings.cpp's kNanoPreferences: settings
//   whose only reader is Nano, docs/linux/audit-2026-11-26/inv_props.md class 1a) is kept in the
//   property store (persist.props), read and written as is. Every other property is unsupported:
//   the ones another Android process acts on (class 1b: SurfaceFlinger, WindowManager, the gammapad
//   daemon, LiveDisplay, the Rockchip composer), the read-only system ones (ro.*), and any key not
//   in the table, a new upstream row included.
// - kGlobal, kSecure, kSystem: Android's settings tables. Unsupported, except the system settings
//   Linux implements itself, each kept as a property Nano reads (Settings.cpp's kNative): Font Size
//   (system font_scale, kept as the persist.gammaos.nano.fontscale Nano's render loop applies) and
//   Screen Timeout (system screen_off_timeout, kept as persist.thimsy.system.screen_off_timeout, the
//   value Nano's own idle sleep reads through this dispatcher).
// - kNone and an empty key are unsupported.
//
// An unsupported setting reads as its default. A write to it changes nothing, returns false and is
// logged once per setting. A write the store refuses (a value over its 91-byte limit) returns false.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanolinux {
namespace settings {

std::unique_ptr<nanoplat::Settings> create();

// The table, for tests and the menus' audits.
// Whether the property `key` is one of Nano's own preferences.
bool isNanoPreference(const std::string& key);
// Nano's own preferences, in the table's (strcmp) order.
std::vector<std::string> nanoPreferences();
// The property a system setting Linux implements is kept in, or "" when Linux does not implement it
// (always "" for kProp and kNone).
std::string nativeStoreKey(nanoplat::SettingSource source, const std::string& key);

}  // namespace settings
}  // namespace nanolinux
