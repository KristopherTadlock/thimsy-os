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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <android/binder_auto_utils.h>: the
// owning wrappers the AIDL clients use. With no binder objects, SpAIBinder only ever holds null and
// ScopedAStatus is always an error.
#pragma once

#ifdef __cplusplus
#include <android/binder_ibinder.h>

namespace ndk {

class SpAIBinder {
 public:
    SpAIBinder() = default;
    explicit SpAIBinder(AIBinder* binder) : mBinder(binder) {}
    AIBinder* get() const { return mBinder; }

 private:
    AIBinder* mBinder = nullptr;
};

class ScopedAStatus {
 public:
    ScopedAStatus() = default;
    bool isOk() const { return false; }
};

}  // namespace ndk
#endif
