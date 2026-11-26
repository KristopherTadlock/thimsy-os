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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/SurfaceComposerClient.h>: a
// SurfaceFlinger client with no SurfaceFlinger. Nano's SurfaceFlinger paths (the overlay, the
// secondary-panel wallpapers, screen capture, the PSP clock's mirror) are dead on Linux, where DRM
// is the only display (IMPLEMENTATION.md B.4). Every query fails or is empty (no physical display,
// null tokens, no state, no mode), createSurface and the other factories return null, and every
// Transaction call does nothing.
#pragma once

#include <cstdint>
#include <vector>

#include <android/gui/BnScreenCaptureListener.h>
#include <binder/IBinder.h>
#include <gui/DisplayCaptureArgs.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/ISurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <nanostubs/stub.h>
#include <ui/DisplayId.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>
#include <ui/LayerStack.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Rotation.h>
#include <ui/StaticDisplayInfo.h>
#include <utils/Errors.h>
#include <utils/String8.h>

namespace android {

class SurfaceComposerClient : public virtual RefBase {
 public:
    class Transaction {
     public:
        Transaction& show(const sp<SurfaceControl>&) { return noop(); }
        Transaction& hide(const sp<SurfaceControl>&) { return noop(); }
        Transaction& reparent(const sp<SurfaceControl>&, const sp<SurfaceControl>& /*newParent*/) { return noop(); }
        Transaction& setLayer(const sp<SurfaceControl>&, int32_t /*z*/) { return noop(); }
        Transaction& setLayerStack(const sp<SurfaceControl>&, ui::LayerStack) { return noop(); }
        Transaction& setFlags(const sp<SurfaceControl>&, uint32_t /*flags*/, uint32_t /*mask*/) { return noop(); }
        Transaction& setMatrix(const sp<SurfaceControl>&, float /*dsdx*/, float /*dtdx*/, float /*dtdy*/,
                               float /*dsdy*/) {
            return noop();
        }
        Transaction& setBackgroundBlurRadius(const sp<SurfaceControl>&, int /*radius*/) { return noop(); }
        Transaction& setFrameRate(const sp<SurfaceControl>&, float /*frameRate*/, int8_t /*compatibility*/,
                                  int8_t /*changeFrameRateStrategy*/) {
            return noop();
        }
        void setDisplayLayerStack(const sp<IBinder>& /*token*/, ui::LayerStack) { noop(); }
        status_t setDisplaySurface(const sp<IBinder>& /*token*/, const sp<IGraphicBufferProducer>&) {
            noop();
            return NO_INIT;
        }
        void setDisplayProjection(const sp<IBinder>& /*token*/, ui::Rotation, const Rect& /*layerStackRect*/,
                                  const Rect& /*displayRect*/) {
            noop();
        }
        status_t apply(bool /*synchronous*/ = false, bool /*oneWay*/ = false) {
            NANOSTUB_ONCE();
            return NO_INIT;
        }

     private:
        Transaction& noop() { return *this; }
    };

    static std::vector<PhysicalDisplayId> getPhysicalDisplayIds() {
        NANOSTUB_ONCE();
        return {};
    }
    static sp<IBinder> getPhysicalDisplayToken(PhysicalDisplayId) { return nullptr; }
    static status_t getDisplayState(const sp<IBinder>&, ui::DisplayState*) { return NAME_NOT_FOUND; }
    static status_t getActiveDisplayMode(const sp<IBinder>&, ui::DisplayMode*) { return NAME_NOT_FOUND; }
    static status_t getStaticDisplayInfo(int64_t /*displayId*/, ui::StaticDisplayInfo*) { return NAME_NOT_FOUND; }
    static void setDisplayPowerMode(const sp<IBinder>&, int /*mode*/) {}
    static sp<IBinder> createDisplay(const String8& /*name*/, bool /*secure*/, float /*refreshRate*/ = 0) {
        NANOSTUB_ONCE();
        return nullptr;
    }
    static status_t destroyDisplay(const sp<IBinder>&) { return NAME_NOT_FOUND; }

    sp<SurfaceControl> createSurface(const String8& /*name*/, uint32_t /*w*/, uint32_t /*h*/, PixelFormat /*format*/,
                                     int32_t /*flags*/ = 0, const sp<IBinder>& /*parentHandle*/ = nullptr) {
        NANOSTUB_ONCE();
        return nullptr;
    }
    sp<SurfaceControl> mirrorDisplay(DisplayId) {
        NANOSTUB_ONCE();
        return nullptr;
    }
    status_t linkToComposerDeath(const sp<IBinder::DeathRecipient>&, void* /*cookie*/ = nullptr,
                                 uint32_t /*flags*/ = 0) {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
};

class ScreenshotClient {
 public:
    static status_t captureDisplay(const gui::DisplayCaptureArgs&, const sp<gui::IScreenCaptureListener>&) {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
    static status_t captureDisplay(DisplayId, const gui::CaptureArgs&, const sp<gui::IScreenCaptureListener>&) {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
};

}  // namespace android
