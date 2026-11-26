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

// Inert stub (layer 2, see nanostubs/stub.h) of libbinder's <binder/IBinder.h>. NanoMenu derives
// from IBinder::DeathRecipient and holds sp<IBinder> display tokens; there is no binder on Linux or
// the host, so no IBinder object is ever made (every token Nano gets is null) and no death is ever
// reported.
#pragma once

#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android {

class IBinder : public virtual RefBase {
 public:
    class DeathRecipient : public virtual RefBase {
     public:
        virtual void binderDied(const wp<IBinder>& who) = 0;
    };
};

}  // namespace android
