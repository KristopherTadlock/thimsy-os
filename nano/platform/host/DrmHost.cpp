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

// The host's whole NanoMenuDrm.h API (docs/linux/IMPLEMENTATION.md B.5, D.3): nano-headless links
// this instead of NanoMenuDrm.cpp. There is no DRM device. The host "has" the displays the host
// config describes (HostControl.h's DisplayConfig); the render targets are offscreen GL textures
// with framebuffers on whatever GL is linked (the GL stub today, Mesa's llvmpipe later), and every
// flip is a present: it is counted, may be captured to a file, is paced to the refresh rate and
// passes the frame barrier (HostControl::present).
//
// What the shared code sees, kept as on a device with the PRIME path (B.8):
//   - drmEarlySplash sets sDrmActive and one DrmDisplay per configured display, so readyToRun takes
//     the DRM path (without it, it reaches the SurfaceFlinger branch and fails);
//   - drmSetupZeroCopy allocates both rings all-or-nothing at the panel's native size, with
//     non-zero drmFbId (the PRIME path), so the same Y-flip is folded into sDrmRotMat and a
//     capture's first row is the top of the panel, as scanout would show it;
//   - the EGL native fence pointers point at host fakes (a sync always signalled, no fence fd), so
//     the render loops' ring path runs as on the device instead of its no-fence fallback;
//   - the property writes the real code makes (sys.gammaos.nano.drm_active) and the persist props
//     it reads (drm_flip_h, drm_flip_v) are the same.
// The AHardwareBuffer handles are dummies nothing dereferences: the only callers of the
// AHardwareBuffer functions are firmware files the host does not link.
//
// NanoMenuDrm.h also declares nanoRgbFollowSample(), defined on Android by NanoMenuRgbFollow.cpp
// ([firmware]); the host defines it as a no-op here so the header's whole API is in one place.

#define LOG_TAG "NanoDrmHost"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

#include "HostControl.h"
#include "NanoMenuDrm.h"

// The GL stub's draw counter (nano/test/host/glstub), when the stub is what is linked.
extern "C" unsigned long glstub_draw_count(void) __attribute__((weak));

namespace android {

// ---------------------------------------------------------------------------
// The variables NanoMenuDrm.h declares
// ---------------------------------------------------------------------------

int sDrmFd = -1;
std::vector<DrmDisplay> sDrmDisplays;
bool sDrmActive = false;
int sDrmRotationDeg = 0;
bool sDrmZeroCopy = false;
bool sDrmGlRotation = false;
bool sDrmYFlipForPrime = false;
bool sDrmFlipH = false;
bool sDrmFlipV = false;
bool sDrmVblankBroken = false;
bool sDrmFrameSync = true;
bool sDrmLowLatency = false;
bool sDrmAfbcMode = false;
bool sDrmAfbcClient = false;
int64_t sDrmLastVblankUs = 0;
uint32_t sDrmSeamRotCrtc = 0;
int sDrmSecondaryRotDeg = 0;
int sDrmVp1DisplayIdx = -1;
bool sDrmPrimaryTurned = false;
bool sDrmSecondaryTurned = false;
uint32_t sDrmAfbcHalfH = 0;
int sPendingFlipEvents = 0;
uint32_t sCrtcIds[kMaxCrtcTrack] = {0};
int sCrtcPending[kMaxCrtcTrack] = {0};
int sCrtcTrackCount = 0;
int sVsyncEnabled = -1;
bool sDrasticQrFastPath = false;
float sDrmRotMat[4] = {1.0f, 0.0f, 0.0f, 1.0f};

PFNEGLCREATEIMAGEKHRPROC sEglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC sEglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC sGlEGLImageTargetTexture2DOES = nullptr;
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC sEglGetNativeClientBufferANDROID = nullptr;
PFNEGLCREATESYNCKHRPROC_LOCAL sEglCreateSyncKHR = nullptr;
PFNEGLDESTROYSYNCKHRPROC_LOCAL sEglDestroySyncKHR = nullptr;
PFNEGLCLIENTWAITSYNCKHRPROC_LOCAL sEglClientWaitSyncKHR = nullptr;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC_LOCAL sEglDupNativeFenceFDANDROID = nullptr;
EGLDisplay sRingEglDpy = EGL_NO_DISPLAY;

AhbRenderTarget sAhbRingPrimary[AHB_RING_DEPTH] = {};
AhbRenderTarget sAhbRingSecondary[AHB_RING_DEPTH] = {};
EGLSyncKHR sAhbRingSyncPrimary[AHB_RING_DEPTH] = {};
int sRingRenderIdx = 0;
int sRingPresentIdx = 0;
int sRingPrimedCount = 0;

int sDrmPrimaryIdx = 0;
int64_t sDrmRescanDeadlineNs = 0;

namespace {

// Distinct non-null AHardwareBuffer* values for the ring slots; never dereferenced.
unsigned char sFakeAhb[2 * AHB_RING_DEPTH + 8];
std::atomic<unsigned> sFakeAhbNext{0};
std::atomic<uint32_t> sFakeFbId{0x1000};
std::atomic<uintptr_t> sFakeSync{0x100};

AHardwareBuffer* fakeAhb() {
    const unsigned n = sFakeAhbNext.fetch_add(1) % sizeof(sFakeAhb);
    return reinterpret_cast<AHardwareBuffer*>(&sFakeAhb[n]);
}

// EGL_ANDROID_native_fence_sync, faked: a sync is signalled the moment it exists.
EGLSyncKHR EGLAPIENTRY hostCreateSync(EGLDisplay, EGLenum, const EGLint*) {
    return reinterpret_cast<EGLSyncKHR>(sFakeSync.fetch_add(0x10));
}
EGLBoolean EGLAPIENTRY hostDestroySync(EGLDisplay, EGLSyncKHR) { return EGL_TRUE; }
EGLint EGLAPIENTRY hostClientWaitSync(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR) {
    glFinish();
    return EGL_CONDITION_SATISFIED_KHR;
}
EGLint EGLAPIENTRY hostDupNativeFence(EGLDisplay, EGLSyncKHR) { return EGL_NO_NATIVE_FENCE_FD_ANDROID; }

bool propBool(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "0");
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

void freeTarget(AhbRenderTarget* t) {
    if (t->glFbo) glDeleteFramebuffers(1, &t->glFbo);
    if (t->glTexture) glDeleteTextures(1, &t->glTexture);
    *t = {};
}

void freeRings() {
    for (int i = 0; i < AHB_RING_DEPTH; i++) {
        freeTarget(&sAhbRingPrimary[i]);
        freeTarget(&sAhbRingSecondary[i]);
        sAhbRingSyncPrimary[i] = EGL_NO_SYNC_KHR;
    }
}

// Reads `fbo` (w x h, RGBA) back and writes it as a binary PPM, rows in read order: with the PRIME
// Y-flip folded into the matrix, the first row read is the panel's top row.
bool captureFramebuffer(GLuint fbo, uint32_t w, uint32_t h, const std::string& path) {
    if (w == 0 || h == 0) return false;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    FILE* f = fopen(path.c_str(), "wbe");
    if (!f) {
        ALOGW("capture: can't write %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<unsigned char> row((size_t)w * 3);
    bool ok = true;
    for (uint32_t y = 0; y < h && ok; y++) {
        const unsigned char* src = &rgba[(size_t)y * w * 4];
        for (uint32_t x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        ok = fwrite(row.data(), 1, row.size(), f) == row.size();
    }
    ok = (fclose(f) == 0) && ok;
    return ok;
}

void presentTarget(int slot, GLuint fbo, uint32_t w, uint32_t h) {
    nanohost::PresentInfo info;
    info.slot = slot;
    info.width = w;
    info.height = h;
    // With the GL stub there are no pixels to compare; its draw count shows frames were drawn into.
    info.draws = glstub_draw_count ? (int64_t)glstub_draw_count() : -1;
    nanohost::present(info, [fbo, w, h](const std::string& path) { return captureFramebuffer(fbo, w, h, path); });
    sDrmLastVblankUs = nanohost::monotonicMs() * 1000;
}

}  // namespace

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

EGLConfig getEglConfig(const EGLDisplay& display, bool wantAlpha) {
    // As NanoMenuDrm.cpp, plus EGL_PBUFFER_BIT: the host renders offscreen only.
    EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, wantAlpha ? 8 : 0, EGL_DEPTH_SIZE, 0, EGL_NONE
    };
    EGLint numConfigs = 0;
    EGLConfig config = nullptr;
    eglChooseConfig(display, attribs, &config, 1, &numConfigs);
    return numConfigs > 0 ? config : nullptr;
}

// ---------------------------------------------------------------------------
// Displays
// ---------------------------------------------------------------------------

int drmCrtcSlot(uint32_t crtcId) {
    for (int i = 0; i < sCrtcTrackCount; i++) {
        if (sCrtcIds[i] == crtcId) return i;
    }
    if (sCrtcTrackCount < kMaxCrtcTrack) {
        sCrtcIds[sCrtcTrackCount] = crtcId;
        return sCrtcTrackCount++;
    }
    return -1;
}

void drmPaceWithoutVsync() {
    // Every present already waits for the next "vblank" (HostControl's pacer).
}

bool drmCreateDumbBuffer(int, uint32_t, uint32_t, DrmBuffer* out) {
    if (out) *out = {};
    return false;  // no DRM device on the host
}

bool drmTryAddDisplay(int, uint32_t, uint32_t, const char*) { return false; }

void drmRescanDisplays() {}

void drmEarlySplash(int existingFd) {
    if (existingFd >= 0) close(existingFd);
    const nanohost::DisplayConfig cfg = nanohost::displayConfig();
    sDrmDisplays.clear();
    sCrtcTrackCount = 0;
    for (int i = 0; i < cfg.count; i++) {
        DrmDisplay d = {};
        d.crtcId = 100 + (uint32_t)i;
        d.connId = 200 + (uint32_t)i;
        d.w = (uint32_t)cfg.width;
        d.h = (uint32_t)cfg.height;
        d.activeBuffer = 0;
        d.mode.hdisplay = (uint16_t)cfg.width;
        d.mode.vdisplay = (uint16_t)cfg.height;
        d.mode.vrefresh = (uint32_t)cfg.refreshHz;
        d.mode.clock = (uint32_t)((int64_t)cfg.width * cfg.height * cfg.refreshHz / 1000);
        snprintf(d.mode.name, sizeof(d.mode.name), "%dx%d", cfg.width, cfg.height);
        sDrmDisplays.push_back(d);
        drmCrtcSlot(d.crtcId);
    }
    sDrmPrimaryIdx = 0;
    sDrmRotationDeg = cfg.rotationDeg;
    sDrmFlipH = propBool("persist.gammaos.nano.drm_flip_h");
    sDrmFlipV = propBool("persist.gammaos.nano.drm_flip_v");
    sDrmFd = -1;
    sDrmActive = !sDrmDisplays.empty();
    property_set("sys.gammaos.nano.drm_active", sDrmActive ? "1" : "0");
    nanohost::event("splash", "\"displays\":" + std::to_string(sDrmDisplays.size()) + ",\"w\":" +
                                  std::to_string(cfg.width) + ",\"h\":" + std::to_string(cfg.height) +
                                  ",\"rotation\":" + std::to_string(cfg.rotationDeg));
}

void drmReleaseEarly() {
    sDrmDisplays.clear();
    sDrmActive = false;
    sDrmZeroCopy = false;
    sDrmRescanDeadlineNs = 0;
    sDrmFd = -1;
    property_set("sys.gammaos.nano.drm_active", "0");
    nanohost::event("drm_release");
}

double drmProbePrimaryRefreshHz() { return (double)nanohost::displayConfig().refreshHz; }

int64_t drmLastGpuDoneUs() { return 0; }

bool drmDeferDrainActive() { return false; }

void drmSetPacerLocked(bool) {}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

bool drmAllocAhbTarget(EGLDisplay, uint32_t w, uint32_t h, AhbRenderTarget* target, const char* label) {
    if (!target || w == 0 || h == 0) return false;
    AhbRenderTarget t = {};
    glGenTextures(1, &t.glTexture);
    glBindTexture(GL_TEXTURE_2D, t.glTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.glFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.glFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.glTexture, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (t.glTexture == 0 || t.glFbo == 0 || status != GL_FRAMEBUFFER_COMPLETE) {
        ALOGW("%s: offscreen target %ux%u failed (tex %u fbo %u status 0x%x)", label ? label : "target", w, h,
              t.glTexture, t.glFbo, status);
        freeTarget(&t);
        return false;
    }
    t.ahb = fakeAhb();
    t.eglImage = EGL_NO_IMAGE_KHR;
    t.w = w;
    t.h = h;
    t.drmFbId = sFakeFbId.fetch_add(1);  // the PRIME path (B.8: never 0 off Android)
    t.drmGemHandle = 0;
    t.scanFbo = target->scanFbo;         // owned by the turned-panel resolve, untouched
    *target = t;
    return true;
}

void drmSetupZeroCopy(EGLDisplay eglDpy) {
    if (!sDrmActive || sDrmDisplays.empty()) return;
    sRingEglDpy = eglDpy;
    sEglCreateSyncKHR = hostCreateSync;
    sEglDestroySyncKHR = hostDestroySync;
    sEglClientWaitSyncKHR = hostClientWaitSync;
    sEglDupNativeFenceFDANDROID = hostDupNativeFence;

    const DrmDisplay& prim = sDrmDisplays[(size_t)sDrmPrimaryIdx];
    for (int i = 0; i < AHB_RING_DEPTH; i++) {
        char label[32];
        snprintf(label, sizeof(label), "primary[%d]", i);
        if (!drmAllocAhbTarget(eglDpy, prim.w, prim.h, &sAhbRingPrimary[i], label)) {
            freeRings();
            return;  // all or nothing, as on the device
        }
    }
    sDrmZeroCopy = true;
    if (sDrmDisplays.size() > 1) {
        const DrmDisplay& sec = sDrmDisplays[sDrmPrimaryIdx == 0 ? 1 : 0];
        for (int i = 0; i < AHB_RING_DEPTH; i++) {
            char label[48];
            snprintf(label, sizeof(label), "secondary-wallpaper[%d]", i);
            if (!drmAllocAhbTarget(eglDpy, sec.w, sec.h, &sAhbRingSecondary[i], label)) {
                for (int j = 0; j < i; j++) freeTarget(&sAhbRingSecondary[j]);
                break;
            }
        }
    }
    if (sAhbRingPrimary[0].drmFbId != 0 && !sDrmYFlipForPrime) {
        sDrmRotMat[1] = -sDrmRotMat[1];
        sDrmRotMat[3] = -sDrmRotMat[3];
        sDrmGlRotation = true;
        sDrmYFlipForPrime = true;
    }
}

void drmBindNextFbo() {
    if (!sDrmZeroCopy) return;
    glBindFramebuffer(GL_FRAMEBUFFER, sAhbTarget.glFbo);
    glViewport(0, 0, (GLsizei)sAhbTarget.w, (GLsizei)sAhbTarget.h);
}

void blitAhbToDrmBuffer(const void* ahbPtr, uint32_t ahbStride, uint32_t srcW, uint32_t srcH, void* dstMapped,
                        uint32_t dstPitch, uint32_t dstW, uint32_t dstH, int blitRotation) {
    // RGBA (memory R,G,B,A) to XRGB8888 (memory B,G,R,X), source rows bottom-up as GL reads them,
    // turned by blitRotation (0..3, quarter turns clockwise). Plain loops: nothing on the host
    // calls it in a hot path.
    if (!ahbPtr || !dstMapped) return;
    const uint8_t* src = static_cast<const uint8_t*>(ahbPtr);
    uint8_t* dst = static_cast<uint8_t*>(dstMapped);
    const int rot = blitRotation & 3;
    const uint32_t outW = (rot & 1) ? srcH : srcW;
    const uint32_t outH = (rot & 1) ? srcW : srcH;
    for (uint32_t dy = 0; dy < outH && dy < dstH; dy++) {
        uint8_t* row = dst + (size_t)dy * dstPitch;
        for (uint32_t dx = 0; dx < outW && dx < dstW; dx++) {
            uint32_t sx, sy;
            switch (rot) {
                case 1: sx = dy; sy = srcH - 1 - dx; break;
                case 2: sx = srcW - 1 - dx; sy = dy; break;
                case 3: sx = srcW - 1 - dy; sy = dx; break;
                default: sx = dx; sy = srcH - 1 - dy; break;
            }
            const uint8_t* p = src + (size_t)sy * ahbStride * 4 + (size_t)sx * 4;
            row[dx * 4 + 0] = p[2];
            row[dx * 4 + 1] = p[1];
            row[dx * 4 + 2] = p[0];
            row[dx * 4 + 3] = 0xff;
        }
    }
}

// ---------------------------------------------------------------------------
// Presents
// ---------------------------------------------------------------------------

bool drmSlotFenceReady(int) { return true; }

void drmFlipRingSlot(int idx, bool) {
    if (!sDrmActive || idx < 0 || idx >= AHB_RING_DEPTH) return;
    const AhbRenderTarget& t = sAhbRingPrimary[idx];
    presentTarget(idx, t.glFbo, t.w, t.h);
}

void drmFlipAll() { drmFlipRingSlot(0); }

bool drmAnyCrtcPending() { return false; }

void drmDrainPageFlipEvents() {}

void nanoRgbFollowSample() {}

void drmPushFrame(uint32_t glWidth, uint32_t glHeight) {
    if (!sDrmActive || sDrmDisplays.empty()) return;
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    presentTarget(-1, (GLuint)fbo, glWidth, glHeight);
}

void drmStop() {
    if (!sDrmActive) return;
    sDrmActive = false;
    property_set("sys.gammaos.nano.drm_active", "0");
    sDrmZeroCopy = false;
    sDrmPrimaryTurned = sDrmSecondaryTurned = false;
    sDrmGlRotation = false;
    sDrmRotMat[0] = 1.0f; sDrmRotMat[1] = 0.0f;
    sDrmRotMat[2] = 0.0f; sDrmRotMat[3] = 1.0f;
    sDrmYFlipForPrime = false;
    sRingRenderIdx = 0;
    sRingPresentIdx = 0;
    sRingPrimedCount = 0;
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) freeRings();
    sRingEglDpy = EGL_NO_DISPLAY;
    sPendingFlipEvents = 0;
    for (int i = 0; i < sCrtcTrackCount; i++) sCrtcPending[i] = 0;
    sDrmDisplays.clear();
    sDrmFd = -1;
    nanohost::event("drm_stop");
}

// ---------------------------------------------------------------------------
// Orientation (the same arithmetic as NanoMenuDrm.cpp: the shaders depend on it)
// ---------------------------------------------------------------------------

void drmBuildInstallMatrix(float out[4], int degrees) {
    const int deg = (degrees < 0) ? sDrmRotationDeg : (((degrees % 360) + 360) % 360);
    switch (deg) {
    case 90:  out[0] =  0.0f; out[1] = -1.0f; out[2] =  1.0f; out[3] =  0.0f; break;
    case 180: out[0] = -1.0f; out[1] =  0.0f; out[2] =  0.0f; out[3] = -1.0f; break;
    case 270: out[0] =  0.0f; out[1] =  1.0f; out[2] = -1.0f; out[3] =  0.0f; break;
    default:  out[0] =  1.0f; out[1] =  0.0f; out[2] =  0.0f; out[3] =  1.0f; break;
    }
    if (deg == 0 && sDrmRotationDeg == 0) { out[1] = -out[1]; out[3] = -out[3]; }
    if (sDrmFlipH) { out[0] = -out[0]; out[2] = -out[2]; }
    if (sDrmFlipV) { out[1] = -out[1]; out[3] = -out[3]; }
}

void nanoSetOverlayRenderRotation(int rot) {
    if (sDrmActive) return;
    switch (rot & 3) {
    case 1:  sDrmRotMat[0] = 0.0f; sDrmRotMat[1] = -1.0f; sDrmRotMat[2] = 1.0f;  sDrmRotMat[3] = 0.0f;  break;
    case 2:  sDrmRotMat[0] = -1.0f; sDrmRotMat[1] = 0.0f; sDrmRotMat[2] = 0.0f;  sDrmRotMat[3] = -1.0f; break;
    case 3:  sDrmRotMat[0] = 0.0f; sDrmRotMat[1] = 1.0f;  sDrmRotMat[2] = -1.0f; sDrmRotMat[3] = 0.0f;  break;
    default: sDrmRotMat[0] = 1.0f; sDrmRotMat[1] = 0.0f;  sDrmRotMat[2] = 0.0f;  sDrmRotMat[3] = 1.0f;  break;
    }
    sDrmGlRotation = (rot & 3) != 0;
}

void drmResolveTurnedTargets(int) {}  // no turned panels on the host

int drmPanelMountRotationDeg(int) { return 0; }

// ---------------------------------------------------------------------------
// Suspend, parking and re-acquiring
// ---------------------------------------------------------------------------

void drmResumeRecommit() {}

bool drmSuspendCycleDetected() { return false; }

void drmSuspendMarkSeen() {}

namespace {
float sParkRot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
bool sParkGlRot = false, sParkYFlip = false, sParkSaved = false;
}  // namespace

void drmRememberInstallMatrix() {
    for (int i = 0; i < 4; i++) sParkRot[i] = sDrmRotMat[i];
    sParkGlRot = sDrmGlRotation;
    sParkYFlip = sDrmYFlipForPrime;
    sParkSaved = true;
}

bool drmReacquireForHome(EGLDisplay eglDpy, int) {
    if (!sParkSaved) {
        ALOGE("drmReacquireForHome without drmRememberInstallMatrix");
        return false;
    }
    for (int i = 0; i < 4; i++) sDrmRotMat[i] = sParkRot[i];
    sDrmGlRotation = sParkGlRot;
    sDrmYFlipForPrime = sParkYFlip;
    drmEarlySplash(-1);
    if (!sDrmActive) return false;
    drmSetupZeroCopy(eglDpy);
    return sDrmZeroCopy;
}

}  // namespace android
