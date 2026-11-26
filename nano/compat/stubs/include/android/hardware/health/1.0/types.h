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
// <android/hardware/health/1.0/types.h>: the battery status the health HAL @2.0 reports.
#pragma once

#include <cstdint>

namespace android {
namespace hardware {
namespace health {
namespace V1_0 {

enum class BatteryStatus : int32_t { UNKNOWN = 1, CHARGING = 2, DISCHARGING = 3, NOT_CHARGING = 4, FULL = 5 };

}  // namespace V1_0
}  // namespace health
}  // namespace hardware
}  // namespace android
