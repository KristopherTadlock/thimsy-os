/*
 * Copyright (C) 2026 GammaOS
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

// The buffer back end of NanoMenuDrm.cpp's render targets (docs/linux/IMPLEMENTATION.md B.8):
// drmAllocAhbTarget allocates an AHardwareBuffer, imports it as an EGL image bound to a
// texture and an FBO, and best-effort as a DRM framebuffer (PRIME, with the gralloc AFBC
// modifier in AFBC mode). Moved here verbatim from NanoMenuDrm.cpp, so that NanoMenuDrm.cpp's
// KMS and flip logic no longer calls gralloc or the vendor AHardwareBuffer API, and another
// platform can link its own back end against NanoMenuDrmBuffers.h instead of this file.
// Includes: NanoMenuDrm.cpp's own list, so the moved code sees the same declarations, plus
// <sys/ioctl.h> for ioctl().

#define LOG_TAG "GammaOSNano"

#include <sched.h>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <vector>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>  // AHardwareBuffer_getNativeHandle
#include <cutils/native_handle.h>  // native_handle_t layout
#include <ui/GraphicBufferMapper.h>  // gralloc metadata (AFBC modifier/layout)
#include <ui/GraphicTypes.h>         // ui::PlaneLayout
#include <sys/ioctl.h>

#include "NanoMenuDrmBuffers.h"

namespace android {

// Build an AHB-backed FBO of the given dimensions. Fills out `target` on
// success; leaves it zeroed and returns false on failure. Factored out so we
// can allocate both primary and secondary render targets from the same code.
bool drmAllocAhbTarget(EGLDisplay eglDpy, uint32_t w, uint32_t h,
                        AhbRenderTarget* target, const char* label) {
    AHardwareBuffer_Desc desc = {};
    desc.width = w;
    desc.height = h;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    // CPU_READ_OFTEN is required on RK3568 Mali gralloc for the AHB
    // to be allocated LINEAR (ABGR8888). Removing it picks an AFBC
    // compressed / tiled layout and DRM_IOCTL_MODE_ADDFB2 rejects
    // with EINVAL, breaking PRIME. COMPOSER_OVERLAY is a hint that
    // the buffer is for direct composition -- some gralloc impls
    // use it to pick a DRM-scanout-friendly (coherent, linear)
    // layout. Kept alongside CPU_READ so the blit fallback path
    // still has a CPU mapping if PRIME ever fails at runtime.
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                 AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    // AFBC mode (rk356x + Low Latency): drop CPU_READ_OFTEN so Mali gralloc
    // allocates an AFBC-compressed buffer. On RK3568 the Cluster planes (which,
    // unlike the Smart planes, do NOT carry the per-VP output-pipeline offset
    // that desyncs the two DSI panels) can ONLY scan AFBC. The DRM import below
    // tags the fb with the matching AFBC modifier and the flip drives Cluster.
    // If any of that fails we fall back to linear+Smart (drmFbId stays 0).
    if (sDrmAfbcMode) {
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    }
    if (AHardwareBuffer_allocate(&desc, &target->ahb) != 0 || !target->ahb) {
        ALOGW("NanoMenu DRM zero-copy: AHardwareBuffer_allocate(%s) failed", label);
        return false;
    }

    EGLClientBuffer clientBuf = sEglGetNativeClientBufferANDROID(target->ahb);
    if (!clientBuf) {
        ALOGW("NanoMenu DRM zero-copy: eglGetNativeClientBufferANDROID(%s) failed", label);
        AHardwareBuffer_release(target->ahb); target->ahb = nullptr;
        return false;
    }

    EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    target->eglImage = sEglCreateImageKHR(eglDpy, EGL_NO_CONTEXT,
                                           EGL_NATIVE_BUFFER_ANDROID, clientBuf, imgAttrs);
    if (target->eglImage == EGL_NO_IMAGE_KHR) {
        ALOGW("NanoMenu DRM zero-copy: eglCreateImageKHR(%s) failed", label);
        AHardwareBuffer_release(target->ahb); target->ahb = nullptr;
        return false;
    }

    glGenTextures(1, &target->glTexture);
    glBindTexture(GL_TEXTURE_2D, target->glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    sGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)target->eglImage);
    if (glGetError() != GL_NO_ERROR) {
        ALOGW("NanoMenu DRM zero-copy: glEGLImageTargetTexture2DOES(%s) failed", label);
        glDeleteTextures(1, &target->glTexture); target->glTexture = 0;
        sEglDestroyImageKHR(eglDpy, target->eglImage); target->eglImage = EGL_NO_IMAGE_KHR;
        AHardwareBuffer_release(target->ahb); target->ahb = nullptr;
        return false;
    }

    glGenFramebuffers(1, &target->glFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target->glFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, target->glTexture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ALOGW("NanoMenu DRM zero-copy: AHB FBO(%s) incomplete 0x%x", label, status);
        glDeleteFramebuffers(1, &target->glFbo); target->glFbo = 0;
        glDeleteTextures(1, &target->glTexture); target->glTexture = 0;
        sEglDestroyImageKHR(eglDpy, target->eglImage); target->eglImage = EGL_NO_IMAGE_KHR;
        AHardwareBuffer_release(target->ahb); target->ahb = nullptr;
        return false;
    }

    target->w = w;
    target->h = h;

    // Best-effort DRM PRIME import: try to make this AHB directly
    // scanout-able by the DRM panel, so drmFlipRingSlot can page flip
    // straight to it instead of CPU-blit'ing AHB->dumb buffer.
    //
    // Sequence: native_handle's first fd is the dma-buf for the AHB's
    // backing store -> PRIME_FD_TO_HANDLE in our DRM context returns
    // a GEM handle -> ADDFB2 with DRM_FORMAT_ABGR8888 (matches AHB's
    // R8G8B8A8 memory order) gives us a fb_id.
    //
    // If any step fails (driver doesn't accept foreign dma-buf imports,
    // or the AHB has a non-trivial gralloc layout), we leave drmFbId=0
    // and the flip path falls back to the blit path. So this is a
    // no-risk experiment.
    target->drmFbId = 0;
    target->drmGemHandle = 0;
    if (sDrmFd >= 0) {
        const native_handle_t* nh = AHardwareBuffer_getNativeHandle(target->ahb);
        if (nh && nh->numFds > 0) {
            int dmabufFd = nh->data[0];
            // Pull the AHB's stride (in pixels) for fb pitch.
            AHardwareBuffer_Desc d = {};
            AHardwareBuffer_describe(target->ahb, &d);
            uint32_t pitch = d.stride * 4;  // R8G8B8A8 = 4 bytes/px

            // AFBC mode: the linear pitch/offset/modifier are all wrong for a
            // compressed buffer -- the kernel's fb size checks then reject
            // ADDFB2 (EINVAL). Query gralloc for the REAL modifier and plane
            // layout (byte stride + offset) so the import matches Mali's exact
            // AFBC allocation. This is what drm_hwcomposer does for SF.
            unsigned long long afbcMod = 0;
            if (sDrmAfbcMode) {
                using android::GraphicBufferMapper;
                using android::ui::PlaneLayout;
                auto& gm = GraphicBufferMapper::get();
                uint64_t realMod = 0;
                if (gm.getPixelFormatModifier(nh, &realMod) == android::OK &&
                    realMod != 0) {
                    afbcMod = (unsigned long long)realMod;
                }
                std::vector<PlaneLayout> layouts;
                if (gm.getPlaneLayouts(nh, &layouts) == android::OK &&
                    !layouts.empty()) {
                    if (layouts[0].strideInBytes > 0)
                        pitch = (uint32_t)layouts[0].strideInBytes;
                }
                // A prop override still wins, for A/B tuning if gralloc lies.
                unsigned long long ov = (unsigned long long)property_get_int64(
                        "sys.gammaos.drastic_nano.afbc_mod", 0);
                if (ov) afbcMod = ov;
                ALOGW("NanoMenu DRM AFBC: gralloc modifier=0x%llx pitch=%u (%s)",
                      afbcMod, pitch, label);
            }

            struct drm_prime_handle ph = {};
            ph.fd = dmabufFd;
            ph.flags = 0;
            ph.handle = 0;
            if (ioctl(sDrmFd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) == 0
                && ph.handle != 0) {
                struct drm_mode_fb_cmd2 cmd = {};
                cmd.width = w;
                cmd.height = h;
                cmd.pixel_format = DRM_FORMAT_ABGR8888;
                cmd.flags = 0;
                cmd.handles[0] = ph.handle;
                cmd.pitches[0] = pitch;
                cmd.offsets[0] = 0;
                // AFBC mode: tag the fb with the real gralloc AFBC modifier so
                // the Cluster planes accept it and decode the exact layout.
                if (sDrmAfbcMode && afbcMod) {
                    cmd.flags = DRM_MODE_FB_MODIFIERS;
                    cmd.modifier[0] = afbcMod;
                }
                if (ioctl(sDrmFd, DRM_IOCTL_MODE_ADDFB2, &cmd) == 0
                    && cmd.fb_id != 0) {
                    target->drmFbId = cmd.fb_id;
                    target->drmGemHandle = ph.handle;
                    ALOGW("NanoMenu DRM PRIME: AHB(%s) imported as fb_id=%u "
                          "(gem=%u dmabuf_fd=%d pitch=%u mod=0x%llx)",
                          label, target->drmFbId, target->drmGemHandle,
                          dmabufFd, pitch, afbcMod);
                } else {
                    ALOGW("NanoMenu DRM PRIME: ADDFB2 failed for AHB(%s) "
                          "(errno=%d) -- will fall back to blit path",
                          label, errno);
                    // GEM handle leaks slightly; close via GEM_CLOSE
                    struct drm_gem_close gc = {};
                    gc.handle = ph.handle;
                    ioctl(sDrmFd, DRM_IOCTL_GEM_CLOSE, &gc);
                }
            } else {
                ALOGW("NanoMenu DRM PRIME: PRIME_FD_TO_HANDLE failed for "
                      "AHB(%s) (errno=%d) -- will fall back to blit path",
                      label, errno);
            }
        }
    }

    ALOGW("NanoMenu DRM zero-copy: AHB(%s) ENABLED — fbo=%u tex=%u (%ux%u) drmFb=%u",
          label, target->glFbo, target->glTexture, w, h, target->drmFbId);
    return true;
}

} // namespace android
