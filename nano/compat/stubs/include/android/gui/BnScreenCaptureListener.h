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

// Inert stub (layer 2, see nanostubs/stub.h) of the AIDL-generated <android/gui/BnScreenCaptureListener.h>:
// the listener a screen capture calls back. ScreenshotClient never captures, so it is never called.
#pragma once

#include <binder/Status.h>
#include <gui/ScreenCaptureResults.h>
#include <utils/RefBase.h>

namespace android {
namespace gui {

class IScreenCaptureListener : public virtual RefBase {
 public:
    virtual binder::Status onScreenCaptureCompleted(const ScreenCaptureResults& results) = 0;
};

class BnScreenCaptureListener : public IScreenCaptureListener {};

}  // namespace gui
}  // namespace android
