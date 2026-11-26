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

// Inert stub (layer 2, see nanostubs/stub.h) of libhidlbase's <hidl/Status.h>: a HIDL call's
// result. No HIDL service is ever found, so no call is ever made; a Return is never ok.
#pragma once

namespace android {
namespace hardware {

template <typename T>
class Return {
 public:
    bool isOk() const { return false; }
};

template <>
class Return<void> {
 public:
    bool isOk() const { return false; }
};

}  // namespace hardware
}  // namespace android
