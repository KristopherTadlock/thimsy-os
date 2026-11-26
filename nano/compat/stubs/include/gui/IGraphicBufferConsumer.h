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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/IGraphicBufferConsumer.h>: the
// consumer end of a buffer queue. BufferQueue::createBufferQueue never makes one.
#pragma once

#include <utils/RefBase.h>
#include <utils/StrongPointer.h>

namespace android {

class IGraphicBufferConsumer : public virtual RefBase {};

}  // namespace android
