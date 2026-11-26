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

// Inert stub (layer 2, see nanostubs/stub.h) of libbinder's <binder/IPCThreadState.h>: there is no
// binder thread pool, so stopProcess (NanoMenu's teardown) does nothing.
#pragma once

#include <nanostubs/stub.h>

namespace android {

class IPCThreadState {
 public:
    static IPCThreadState* self() {
        static IPCThreadState sState;
        return &sState;
    }
    void stopProcess(bool /*immediate*/ = true) { NANOSTUB_ONCE(); }
};

}  // namespace android
