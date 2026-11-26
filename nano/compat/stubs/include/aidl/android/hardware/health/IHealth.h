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
// <aidl/android/hardware/health/IHealth.h>: no health HAL is ever found (fromBinder of a null
// binder is null), so NanoMenuSystem.cpp reads the battery from its power_supply sysfs fallback.
#pragma once

#include <cstdint>
#include <memory>

#include <aidl/android/hardware/health/BatteryStatus.h>
#include <android/binder_auto_utils.h>

namespace aidl {
namespace android {
namespace hardware {
namespace health {

class IHealth {
 public:
    virtual ~IHealth() = default;
    static std::shared_ptr<IHealth> fromBinder(const ::ndk::SpAIBinder& /*binder*/) { return nullptr; }
    virtual ::ndk::ScopedAStatus getCapacity(int32_t* capacity) = 0;
    virtual ::ndk::ScopedAStatus getChargeStatus(BatteryStatus* status) = 0;
};

}  // namespace health
}  // namespace hardware
}  // namespace android
}  // namespace aidl
