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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/FenceResult.h>: a fence or the status
// that kept one from being made (Android's base::expected<sp<Fence>, status_t>, cut down to the
// calls Nano makes).
#pragma once

#include <ui/Fence.h>
#include <utils/Errors.h>

namespace android {

class FenceResult {
 public:
    FenceResult(const sp<Fence>& fence) : mFence(fence) {}  // NOLINT(google-explicit-constructor)
    bool ok() const { return mStatus == NO_ERROR; }
    sp<Fence> value_or(const sp<Fence>& other) const { return ok() ? mFence : other; }

 private:
    sp<Fence> mFence;
    status_t mStatus = NO_ERROR;
};

}  // namespace android
