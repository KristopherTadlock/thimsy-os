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

// The buffer back end of NanoMenuDrm.cpp's render targets. Android's is NanoMenuDrmBuffers.cpp
// (AHardwareBuffer and gralloc); another platform links its own file in its place. The whole
// contract is docs/linux/IMPLEMENTATION.md B.8; in short:
//
// Allocate: drmAllocAhbTarget(eglDpy, w, h, target, label) is called by drmSetupZeroCopy() for
//   each of the AHB_RING_DEPTH slots of sAhbRingPrimary (all or none) and, with more than one
//   display, of sAhbRingSecondary (all or none; the secondary displays mirror the primary
//   otherwise), at the panel's native size (twice the height in AFBC mode), on the thread that
//   owns the EGL context. On success it fills `ahb` (non-null: the flip path reads it as "this
//   slot exists"), `eglImage`, `glTexture` and `glFbo` (an R8G8B8A8 colour attachment, complete),
//   `w` and `h`, and `drmFbId`/`drmGemHandle` (0 when the DRM import failed); it leaves `scanFbo`
//   alone and GL_FRAMEBUFFER bound to 0. On failure it returns false and owns nothing.
// Import: a target with drmFbId != 0 is page-flipped directly (DRM_FORMAT_ABGR8888, the GL
//   image's row 0 in the buffer's first row, so drmSetupZeroCopy() adds a Y-flip to sDrmRotMat).
//   A target with drmFbId == 0 is valid on Android only: the flip path then CPU-reads it through
//   AHardwareBuffer_describe/_lock/_unlock and copies it into the display's dumb buffer.
// Free: there is no free function. NanoMenuDrm.cpp frees a target inline (drmStop(), and the
//   unwind loops of drmSetupZeroCopy()): the FBO, the texture, the EGL image
//   (sEglDestroyImageKHR), in drmStop() only the framebuffer (RMFB) and the GEM handle
//   (GEM_CLOSE on sDrmFd), then AHardwareBuffer_release(ahb). A back end that is not
//   AHardwareBuffer therefore also provides AHardwareBuffer_release (and _describe, _lock,
//   _unlock) for its own handles.

#ifndef GAMMAOS_NANO_MENU_DRM_BUFFERS_H
#define GAMMAOS_NANO_MENU_DRM_BUFFERS_H

#include "NanoMenuDrm.h"

namespace android {

// Also declared in NanoMenuDrm.h, which NanoMenuDrm.cpp's callers include.
bool drmAllocAhbTarget(EGLDisplay eglDpy, uint32_t w, uint32_t h,
                        AhbRenderTarget* target, const char* label);

} // namespace android

#endif // GAMMAOS_NANO_MENU_DRM_BUFFERS_H
