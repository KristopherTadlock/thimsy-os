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
//   in the table, a new upstream row included. The exception: the read-only system state a row shows
//   and Linux provides (Settings.cpp's kReadOnly: ro.product.model, About Device's Device Name) is
//   supported for reading; a write to it is refused like an unsupported one.
// - kGlobal, kSecure, kSystem: Android's settings tables. Unsupported, except the system settings
//   Linux implements itself, each kept as a property Nano reads (Settings.cpp's kNative): Font Size
//   (system font_scale, kept as the persist.gammaos.nano.fontscale Nano's render loop applies) and
//   Screen Timeout (system screen_off_timeout, kept as persist.thimsy.system.screen_off_timeout, the
//   value Nano's own idle sleep reads through this dispatcher); and the radio switches the Quick
//   Menu's Wi-Fi and Bluetooth rows are bound to (Settings.cpp's kRadios: global wifi_on and
//   bluetooth_on). A radio read is its switch's key (persist.thimsy.wifi.enabled or
//   persist.thimsy.bluetooth.enabled: "1", else "0"); a write of 1 or true, 0 or false is the system
//   actions' setWifiEnabled or setBluetoothEnabled, the action Nano's own switches take, and nothing
//   when the switch is already there. Bluetooth is supported only when ro.thimsy.device.bluetooth is
//   true, and neither radio without the system actions.
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

// `radios`: the platform's system actions, which turn the Wi-Fi and Bluetooth switches (they must
// outlive the settings); nullptr leaves both radios unsupported.
std::unique_ptr<nanoplat::Settings> create(nanoplat::SystemActions* radios);

// The table, for tests and the menus' audits.
// Whether the property `key` is one of Nano's own preferences.
bool isNanoPreference(const std::string& key);
// Nano's own preferences, in the table's (strcmp) order.
std::vector<std::string> nanoPreferences();
// Whether (source, key) is read-only system state Linux provides (read, never written).
bool isReadOnlySetting(nanoplat::SettingSource source, const std::string& key);
// The property a system setting Linux implements is kept in, or "" when Linux does not implement it
// (always "" for kProp and kNone).
std::string nativeStoreKey(nanoplat::SettingSource source, const std::string& key);

}  // namespace settings
}  // namespace nanolinux
