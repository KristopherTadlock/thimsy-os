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

// The Linux buffer back end of NanoMenuDrm.cpp's render targets (NanoMenuDrmBuffers.h;
// docs/linux/IMPLEMENTATION.md B.5 and B.8). The Linux build links this file instead of
// NanoMenuDrmBuffers.cpp, so NanoMenuDrm.cpp's KMS and flip logic runs unchanged on GBM buffers:
//
// - drmAllocAhbTarget allocates a linear ABGR8888 gbm_bo (scanout and rendering) on the gbm_device
//   of the current sDrmFd, imports its dma-buf as an EGL image (EGL_EXT_image_dma_buf_import) bound
//   to a texture and a complete FBO, and adds it as a DRM framebuffer (ADDFB2). Every step must
//   work: Linux has no SurfaceFlinger and no CPU-blit fallback for these targets, so it fails rather
//   than return drmFbId == 0 (B.8, "Scanout"), and in AFBC mode, which this back end can't allocate,
//   it fails too.
// - `ahb` is this file's own AHardwareBuffer (a gbm_bo, its dma-buf fd and its layout), never null
//   on success: the flip path reads `ahb != nullptr` as "this slot exists".
// - drmGemHandle stays 0: the GEM handle belongs to the gbm_bo (gbm_bo_get_handle on sDrmFd; a
//   PRIME import would return the same handle), so NanoMenuDrm.cpp must not GEM_CLOSE it.
//   AHardwareBuffer_release destroys the gbm_bo, which closes it.
// - NanoMenuDrm.cpp frees a target inline (drmStop(), drmSetupZeroCopy()'s unwind loops) and reads
//   a buffer back through AHardwareBuffer_describe/_lock/_unlock: those four NDK functions are
//   defined here for this file's handles (link-time substitution). _lock fails: it is reached only
//   for a target with drmFbId == 0, which this file never makes.
// - drmReacquireForHome reopens /dev/dri/card0, so sDrmFd changes between allocations: the
//   gbm_device follows the current sDrmFd, and a device is destroyed with its last buffer.
//
// The EGL gate in front of all this (drmSetupZeroCopy's check for Android's EGL names) is met by
// EglWraps.cpp. Written, not compiled here (no libdrm, GBM or EGL headers on this machine): G10
// runs -fsyntax-only on it against them. It needs <android/hardware_buffer.h> from the layer-2
// stubs (G4), which declares the opaque AHardwareBuffer, AHardwareBuffer_Desc, the constants
// [core] and [drm] use and the four functions defined here, and must not define them. The usage
// bits this file reports that the stub does not declare are kUsage* below (the NDK's values; the
// stub budget only shrinks, G4-2).
//
// It also defines nanoRgbFollowSample() as a no-op: NanoMenuDrm.h declares it and NanoMenu.cpp calls
// it every home frame, but its Android definition (NanoMenuRgbFollow.cpp, GammaRGB's follow-screen
// sampler) is [firmware]; v1 has no RGB follow on Linux (G5-7, the host's DrmHost.cpp does the same).

#define LOG_TAG "ThimsyDrmGbm"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mutex>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>

#include <android/hardware_buffer.h>
#include <log/log.h>

#include "NanoMenuDrmBuffers.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

namespace {

// The NDK usage bits a render target has (<android/hardware_buffer.h>'s values), reported by
// AHardwareBuffer_describe.
constexpr uint64_t kUsageGpuSampledImage = 1ULL << 8;    // AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
constexpr uint64_t kUsageGpuFramebuffer = 1ULL << 9;     // AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER
constexpr uint64_t kUsageComposerOverlay = 1ULL << 11;   // AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY

// One gbm_device per DRM fd it was made on; destroyed with its last buffer.
struct GbmDevice {
    gbm_device* dev = nullptr;
    int fd = -1;
    int live = 0;        // buffers allocated on it and not yet released
};

std::mutex gMu;
std::vector<GbmDevice*> gDevices;   // at most one per open DRM fd; usually one

// The device for FD, made if needed. Called with gMu held.
GbmDevice* deviceFor(int fd) {
    for (GbmDevice* d : gDevices)
        if (d->fd == fd) return d;
    gbm_device* dev = gbm_create_device(fd);
    if (!dev) {
        ALOGE("gbm_create_device(fd %d) failed: %s", fd, strerror(errno));
        return nullptr;
    }
    GbmDevice* d = new GbmDevice;
    d->dev = dev;
    d->fd = fd;
    gDevices.push_back(d);
    ALOGI("gbm device on DRM fd %d (%s)", fd, gbm_device_get_backend_name(dev));
    return d;
}

// Drops one buffer of D; destroys D with its last one. Called with gMu held.
void dropDevice(GbmDevice* d) {
    if (--d->live > 0) return;
    for (size_t i = 0; i < gDevices.size(); i++) {
        if (gDevices[i] == d) {
            gDevices.erase(gDevices.begin() + (long)i);
            break;
        }
    }
    gbm_device_destroy(d->dev);
    delete d;
}

bool hasEglExtension(EGLDisplay dpy, const char* name) {
    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!exts) return false;
    const size_t n = strlen(name);
    for (const char* p = exts; (p = strstr(p, name)) != nullptr; p += n)
        if ((p == exts || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) return true;
    return false;
}

}  // namespace

// This file's AHardwareBuffer: the NDK type is opaque to every other file.
struct AHardwareBuffer {
    gbm_bo* bo = nullptr;
    GbmDevice* device = nullptr;
    int dmabufFd = -1;
    uint32_t width = 0, height = 0;
    uint32_t stride = 0;      // bytes
    uint32_t offset = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    uint64_t usage = 0;
};

namespace {

void destroyBuffer(AHardwareBuffer* b) {
    if (!b) return;
    if (b->dmabufFd >= 0) close(b->dmabufFd);
    std::lock_guard<std::mutex> l(gMu);
    if (b->bo) gbm_bo_destroy(b->bo);
    if (b->device) dropDevice(b->device);
    delete b;
}

AHardwareBuffer* allocBuffer(int drmFd, uint32_t w, uint32_t h, const char* label) {
    AHardwareBuffer* b = new AHardwareBuffer;
    {
        std::lock_guard<std::mutex> l(gMu);
        GbmDevice* d = deviceFor(drmFd);
        if (!d) {
            delete b;
            return nullptr;
        }
        b->bo = gbm_bo_create(d->dev, w, h, GBM_FORMAT_ABGR8888,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!b->bo) {
            ALOGW("gbm_bo_create(%s, %ux%u ABGR8888 linear scanout) failed: %s", label, w, h, strerror(errno));
            if (d->live == 0) {
                d->live = 1;   // dropDevice destroys a device nothing uses
                dropDevice(d);
            }
            delete b;
            return nullptr;
        }
        b->device = d;
        d->live++;
    }
    b->width = w;
    b->height = h;
    b->stride = gbm_bo_get_stride(b->bo);
    b->offset = gbm_bo_get_offset(b->bo, 0);
    b->modifier = gbm_bo_get_modifier(b->bo);
    b->usage = kUsageGpuFramebuffer | kUsageGpuSampledImage | kUsageComposerOverlay;
    b->dmabufFd = gbm_bo_get_fd(b->bo);
    if (b->dmabufFd < 0) {
        ALOGW("gbm_bo_get_fd(%s) failed: %s", label, strerror(errno));
        destroyBuffer(b);
        return nullptr;
    }
    return b;
}

}  // namespace

namespace android {

bool drmAllocAhbTarget(EGLDisplay eglDpy, uint32_t w, uint32_t h, AhbRenderTarget* target, const char* label) {
    if (sDrmAfbcMode) {
        // B.8: a back end that can't allocate AFBC fails in AFBC mode. (rk356x's Low Latency
        // dual-display mode, drastic-nano only: not on Linux.)
        ALOGW("DRM zero-copy (%s): AFBC mode is not supported by the GBM back end", label);
        return false;
    }
    if (sDrmFd < 0 || w == 0 || h == 0 || !sEglCreateImageKHR || !sEglDestroyImageKHR ||
        !sGlEGLImageTargetTexture2DOES) {
        ALOGW("DRM zero-copy (%s): no DRM fd or no EGL image functions", label);
        return false;
    }
    if (!hasEglExtension(eglDpy, "EGL_EXT_image_dma_buf_import")) {
        ALOGW("DRM zero-copy (%s): EGL_EXT_image_dma_buf_import missing", label);
        return false;
    }

    AHardwareBuffer* b = allocBuffer(sDrmFd, w, h, label);
    if (!b) return false;

    // The dma-buf as an EGL image. The explicit modifier only when the driver says it knows one
    // and EGL can take it; a linear buffer imports the same without it.
    std::vector<EGLint> attrs = {
        EGL_WIDTH, (EGLint)w,
        EGL_HEIGHT, (EGLint)h,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, b->dmabufFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)b->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)b->stride,
    };
#ifdef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
    if (b->modifier != DRM_FORMAT_MOD_INVALID && b->modifier != DRM_FORMAT_MOD_LINEAR &&
        hasEglExtension(eglDpy, "EGL_EXT_image_dma_buf_import_modifiers")) {
        attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
        attrs.push_back((EGLint)(b->modifier & 0xffffffffu));
        attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
        attrs.push_back((EGLint)(b->modifier >> 32));
    }
#endif
    attrs.push_back(EGL_NONE);
    EGLImageKHR image = sEglCreateImageKHR(eglDpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                           (EGLClientBuffer) nullptr, attrs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        ALOGW("DRM zero-copy (%s): eglCreateImageKHR(dma-buf) failed: 0x%x", label, eglGetError());
        destroyBuffer(b);
        return false;
    }

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    sGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    GLenum err = glGetError();
    GLenum status = GL_FRAMEBUFFER_UNSUPPORTED;
    if (err == GL_NO_ERROR) {
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (err != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE) {
        ALOGW("DRM zero-copy (%s): texture from the dma-buf image failed (gl 0x%x, fbo 0x%x)", label, err, status);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        sEglDestroyImageKHR(eglDpy, image);
        destroyBuffer(b);
        return false;
    }

    // The scanout framebuffer, on the gbm_bo's own GEM handle (it was made on sDrmFd).
    struct drm_mode_fb_cmd2 cmd = {};
    cmd.width = w;
    cmd.height = h;
    cmd.pixel_format = DRM_FORMAT_ABGR8888;
    cmd.handles[0] = gbm_bo_get_handle(b->bo).u32;
    cmd.pitches[0] = b->stride;
    cmd.offsets[0] = b->offset;
    if (b->modifier != DRM_FORMAT_MOD_INVALID && b->modifier != DRM_FORMAT_MOD_LINEAR) {
        cmd.flags = DRM_MODE_FB_MODIFIERS;
        cmd.modifier[0] = b->modifier;
    }
    if (cmd.handles[0] == 0 || ioctl(sDrmFd, DRM_IOCTL_MODE_ADDFB2, &cmd) != 0 || cmd.fb_id == 0) {
        ALOGW("DRM zero-copy (%s): ADDFB2 failed: %s", label, strerror(errno));
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        sEglDestroyImageKHR(eglDpy, image);
        destroyBuffer(b);
        return false;
    }

    target->ahb = b;
    target->eglImage = image;
    target->glTexture = tex;   // left bound to GL_TEXTURE_2D, as the Android back end leaves it
    target->glFbo = fbo;
    target->w = w;
    target->h = h;
    target->drmFbId = cmd.fb_id;
    target->drmGemHandle = 0;  // the gbm_bo owns the handle; AHardwareBuffer_release closes it
    ALOGI("DRM zero-copy (%s): gbm %ux%u stride %u mod 0x%llx -> fb %u, fbo %u", label, w, h, b->stride,
          (unsigned long long)b->modifier, cmd.fb_id, fbo);
    return true;
}

// GammaRGB's follow-screen sampler (NanoMenuRgbFollow.cpp on Android): not on Linux v1.
void nanoRgbFollowSample() {}

}  // namespace android

// ------------------------------------------------- the NDK functions NanoMenuDrm.cpp calls on targets

extern "C" void AHardwareBuffer_release(AHardwareBuffer* buffer) {
    destroyBuffer(buffer);
}

extern "C" void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (!outDesc) return;
    memset(outDesc, 0, sizeof *outDesc);
    if (!buffer) return;
    outDesc->width = buffer->width;
    outDesc->height = buffer->height;
    outDesc->layers = 1;
    outDesc->format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    outDesc->usage = buffer->usage;
    outDesc->stride = buffer->stride / 4;   // in pixels, as Android reports it
}

extern "C" int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                                    void** outVirtualAddress) {
    (void)buffer;
    (void)usage;
    (void)rect;
    if (fence >= 0) close(fence);   // Android takes ownership of the fence
    if (outVirtualAddress) *outVirtualAddress = nullptr;
    // Only the CPU-read path for a target with drmFbId == 0 locks, and this back end makes none.
    return -EINVAL;
}

extern "C" int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence) {
    (void)buffer;
    if (fence) *fence = -1;
    return -EINVAL;
}
