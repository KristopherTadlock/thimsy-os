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

// Inert stub (layer 2, see nanostubs/stub.h) of the HIDL-generated
// <android/hardware/health/2.0/IHealth.h>: there is no health HAL (getService returns null), so
// NanoMenuSystem.cpp reads the battery from its power_supply sysfs fallback.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <android/hardware/health/1.0/types.h>
#include <hidl/Status.h>
#include <nanostubs/stub.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android {
namespace hardware {
namespace health {
namespace V2_0 {

enum class Result : int32_t { SUCCESS = 0 };

class IHealth : public virtual RefBase {
 public:
    using getCapacity_cb = std::function<void(Result result, int32_t value)>;
    using getChargeStatus_cb = std::function<void(Result result, V1_0::BatteryStatus value)>;

    static sp<IHealth> getService(const std::string& /*serviceName*/ = "default", bool /*getStub*/ = false) {
        NANOSTUB_ONCE();
        return nullptr;
    }
    virtual Return<void> getCapacity(getCapacity_cb cb) = 0;
    virtual Return<void> getChargeStatus(getChargeStatus_cb cb) = 0;
};

}  // namespace V2_0
}  // namespace health
}  // namespace hardware
}  // namespace android
