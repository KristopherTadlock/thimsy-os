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

// The Linux nanoplat::Capabilities (Capabilities.h). The switch has no default, so a capability
// added to NanoPlatform.h does not compile here until Linux's answer is written down.

#include "Capabilities.h"

#include <memory>

namespace nanolinux {
namespace caps {

namespace {

class LinuxCapabilities : public nanoplat::Capabilities {
 public:
    bool has(nanoplat::Cap cap) const override {
        using nanoplat::Cap;
        switch (cap) {
        // Android's own apps, services and boot modes: Linux has no counterpart to offer.
        case Cap::OverlayApps:         // no Android apps run beside Nano
        case Cap::SystemSettingsApp:   // Nano's menus are the only settings
        case Cap::RebootRecovery:      // no recovery system
        case Cap::RebootSafeMode:      // no safe mode
        case Cap::RebootAndroid:       // no Android home
        case Cap::Notifications:       // no notification service
        case Cap::ImeSelect:           // Nano's keyboard is the only one
        case Cap::Dnd:
        case Cap::LocationToggle:
        case Cap::DarkTheme:           // Nano's themes carry their own dark variants
            return false;
        // Linux could have these and does not yet; each turns true with its native implementation.
        case Cap::DualStack:           // the second panel is not driven yet
        case Cap::FactoryReset:        // nothing wipes the user's data yet (Android's is a property
                                       // its framework acts on)
        case Cap::ResetRadios:         // the Reset Options row runs Android's cmd wifi and
                                       // bluetooth_manager; Linux's radios are SystemActions'
        case Cap::OtaUpdate:           // off until Thimsy has its own update channel (OA-16)
        case Cap::Mtp:                 // no USB gadget yet
        case Cap::TypecRole:           // Nano's view of /sys/class/typec is masked (step 1)
        case Cap::MediaPlayback:       // the media decode interface has not landed (OA-24)
        case Cap::CpuProfiles:         // the family hooks do not act on fixedperf yet
        case Cap::ArchiveBrowsing:     // NANO_WITH_ARCHIVE is off on Linux until OA-16's libraries
        case Cap::ManualViewer:        // NANO_WITH_PDF likewise
            return false;
        case Cap::Count:
            return false;
        }
        return false;
    }
};

}  // namespace

std::unique_ptr<nanoplat::Capabilities> create() {
    return std::unique_ptr<nanoplat::Capabilities>(new LinuxCapabilities);
}

}  // namespace caps
}  // namespace nanolinux
