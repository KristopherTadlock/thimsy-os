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

// Inert stub (layer 2, see nanostubs/stub.h) of the AIDL-generated
// <aidl/android/hardware/light/ILights.h>: no lights HAL is ever found (the service manager knows
// none, and fromBinder of a null binder is null).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <aidl/android/hardware/light/HwLight.h>
#include <aidl/android/hardware/light/HwLightState.h>
#include <android/binder_auto_utils.h>

namespace aidl {
namespace android {
namespace hardware {
namespace light {

class ILights {
 public:
    virtual ~ILights() = default;
    static std::shared_ptr<ILights> fromBinder(const ::ndk::SpAIBinder& /*binder*/) { return nullptr; }
    virtual ::ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) = 0;
    virtual ::ndk::ScopedAStatus getLights(std::vector<HwLight>* lights) = 0;
};

}  // namespace light
}  // namespace hardware
}  // namespace android
}  // namespace aidl
