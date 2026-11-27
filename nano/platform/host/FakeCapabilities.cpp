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

// The host's fake capabilities (FakeCapabilities.h).

#include "FakeCapabilities.h"

#include <memory>
#include <string_view>

namespace nanohost {

namespace {

constexpr int kCapCount = static_cast<int>(nanoplat::Cap::Count);

class FakeCapabilities : public nanoplat::Capabilities {
 public:
    explicit FakeCapabilities(const CapAnswers& answers) {
        for (bool& a : answers_) a = true;
        for (const auto& a : answers) {
            const int i = static_cast<int>(a.first);
            if (i >= 0 && i < kCapCount) answers_[i] = a.second;
        }
    }

    bool has(nanoplat::Cap cap) const override {
        const int i = static_cast<int>(cap);
        return i >= 0 && i < kCapCount && answers_[i];
    }

 private:
    bool answers_[kCapCount];
};

}  // namespace

std::unique_ptr<nanoplat::Capabilities> createFakeCapabilities(const CapAnswers& answers) {
    return std::unique_ptr<nanoplat::Capabilities>(new FakeCapabilities(answers));
}

const char* capName(nanoplat::Cap cap) {
    using nanoplat::Cap;
    switch (cap) {
    case Cap::OverlayApps: return "OverlayApps";
    case Cap::DualStack: return "DualStack";
    case Cap::SystemSettingsApp: return "SystemSettingsApp";
    case Cap::FactoryReset: return "FactoryReset";
    case Cap::ResetRadios: return "ResetRadios";
    case Cap::RebootRecovery: return "RebootRecovery";
    case Cap::RebootSafeMode: return "RebootSafeMode";
    case Cap::RebootAndroid: return "RebootAndroid";
    case Cap::OtaUpdate: return "OtaUpdate";
    case Cap::Mtp: return "Mtp";
    case Cap::TypecRole: return "TypecRole";
    case Cap::MediaPlayback: return "MediaPlayback";
    case Cap::CpuProfiles: return "CpuProfiles";
    case Cap::Dnd: return "Dnd";
    case Cap::LocationToggle: return "LocationToggle";
    case Cap::DarkTheme: return "DarkTheme";
    case Cap::ImeSelect: return "ImeSelect";
    case Cap::Notifications: return "Notifications";
    case Cap::ArchiveBrowsing: return "ArchiveBrowsing";
    case Cap::ManualViewer: return "ManualViewer";
    case Cap::Count: return "";
    }
    return "";
}

bool capFromName(std::string_view name, nanoplat::Cap* cap) {
    for (int i = 0; i < kCapCount; ++i) {
        const nanoplat::Cap c = static_cast<nanoplat::Cap>(i);
        if (name == capName(c)) {
            if (cap) *cap = c;
            return true;
        }
    }
    return false;
}

}  // namespace nanohost
