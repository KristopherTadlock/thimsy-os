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

// Inert stub (layer 2, see nanostubs/stub.h) of libbinder's <binder/Status.h>: the result type of
// an AIDL call, as Nano's screen-capture listener returns it.
#pragma once

namespace android {
namespace binder {

class Status {
 public:
    static Status ok() { return Status(); }
    bool isOk() const { return true; }
};

}  // namespace binder
}  // namespace android
