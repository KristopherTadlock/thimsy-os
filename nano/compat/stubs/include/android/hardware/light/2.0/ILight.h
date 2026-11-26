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
// <android/hardware/light/2.0/ILight.h>: there is no lights HAL (getService returns null).
// Brightness on Linux is the backlight's sysfs node, behind the platform layer (IMPLEMENTATION.md B.6).
#pragma once

#include <cstdint>
#include <string>

#include <hidl/Status.h>
#include <nanostubs/stub.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {

enum class Type : int32_t { BACKLIGHT = 0 };
enum class Flash : int32_t { NONE = 0 };
enum class Brightness : int32_t { USER = 0 };
enum class Status : int32_t { SUCCESS = 0 };

struct LightState {
    uint32_t color = 0;
    Flash flashMode = Flash::NONE;
    int32_t flashOnMs = 0;
    int32_t flashOffMs = 0;
    Brightness brightnessMode = Brightness::USER;
};

class ILight : public virtual RefBase {
 public:
    static sp<ILight> getService(const std::string& /*serviceName*/ = "default", bool /*getStub*/ = false) {
        NANOSTUB_ONCE();
        return nullptr;
    }
    virtual Return<Status> setLight(Type type, const LightState& state) = 0;
};

}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
