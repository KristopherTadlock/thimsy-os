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

// The EGL gate of drmSetupZeroCopy() on Linux (docs/linux/IMPLEMENTATION.md B.8, "The EGL gate").
//
// NanoMenuDrm.cpp turns the zero-copy ring on only when eglGetProcAddress resolves
// eglCreateImageKHR, glEGLImageTargetTexture2DOES and eglGetNativeClientBufferANDROID, and
// eglQueryString(dpy, EGL_EXTENSIONS) lists EGL_ANDROID_image_native_buffer and
// EGL_ANDROID_get_native_client_buffer. Mesa's GBM platform has neither Android name, and on Linux
// the ring is the only way anything reaches the panel. thimsy-nano links with
// -Wl,--wrap=eglGetProcAddress,--wrap=eglQueryString (nano/platform/linux/linker-wraps.txt), so
// Nano's calls land here; Mesa's own calls do not (--wrap leaves shared libraries alone).
//
// - eglGetProcAddress("eglGetNativeClientBufferANDROID") returns a stand-in that hands back the
//   buffer pointer: only Android's drmAllocAhbTarget calls it, and Linux links DrmBuffersGbm.cpp in
//   its place, which never does. Every other name, eglCreateImageKHR and
//   glEGLImageTargetTexture2DOES included, is Mesa's own answer (null stays null: the gate then
//   fails honestly).
// - eglQueryString(dpy, EGL_EXTENSIONS) appends the two Android names to Mesa's list, but only when
//   the display really has what DrmBuffersGbm.cpp uses instead: EGL_EXT_image_dma_buf_import and
//   EGL_KHR_image_base. Every other query passes through.
//
// Written, not compiled here (no EGL headers on this machine): G10 runs -fsyntax-only on it.

#define LOG_TAG "ThimsyEglWraps"

#include <string.h>

#include <map>
#include <mutex>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <log/log.h>

extern "C" {
__eglMustCastToProperFunctionPointerType __real_eglGetProcAddress(const char* procname);
const char* __real_eglQueryString(EGLDisplay dpy, EGLint name);
}

namespace {

constexpr const char* kAndroidExtensions[] = {
    "EGL_ANDROID_image_native_buffer",
    "EGL_ANDROID_get_native_client_buffer",
};

// Whether the space-separated LIST holds the extension NAME as a whole word.
bool hasExtension(const char* list, const char* name) {
    if (!list || !name) return false;
    const size_t n = strlen(name);
    for (const char* p = list; (p = strstr(p, name)) != nullptr; p += n) {
        const bool startOk = p == list || p[-1] == ' ';
        const bool endOk = p[n] == '\0' || p[n] == ' ';
        if (startOk && endOk) return true;
    }
    return false;
}

// The stand-in for eglGetNativeClientBufferANDROID (EGL_ANDROID_get_native_client_buffer).
EGLClientBuffer EGLAPIENTRY nativeClientBufferStandIn(const struct AHardwareBuffer* buffer) {
    return (EGLClientBuffer)buffer;
}

std::mutex gMu;
std::map<EGLDisplay, const std::string*>* gLists = nullptr;   // never freed: callers keep the pointers

}  // namespace

extern "C" __eglMustCastToProperFunctionPointerType __wrap_eglGetProcAddress(const char* procname) {
    if (procname && strcmp(procname, "eglGetNativeClientBufferANDROID") == 0) {
        __eglMustCastToProperFunctionPointerType real = __real_eglGetProcAddress(procname);
        if (real) return real;   // a driver that has it (not Mesa on GBM): use it
        ALOGI("eglGetNativeClientBufferANDROID: the Linux stand-in (DrmBuffersGbm.cpp never calls it)");
        return (__eglMustCastToProperFunctionPointerType)nativeClientBufferStandIn;
    }
    return __real_eglGetProcAddress(procname);
}

extern "C" const char* __wrap_eglQueryString(EGLDisplay dpy, EGLint name) {
    const char* real = __real_eglQueryString(dpy, name);
    if (name != EGL_EXTENSIONS || dpy == EGL_NO_DISPLAY || !real) return real;
    if (!hasExtension(real, "EGL_EXT_image_dma_buf_import") || !hasExtension(real, "EGL_KHR_image_base")) {
        ALOGW("EGL display lacks EGL_EXT_image_dma_buf_import or EGL_KHR_image_base: "
              "the DRM zero-copy ring stays off, and nothing reaches the panel");
        return real;
    }
    std::lock_guard<std::mutex> l(gMu);
    if (!gLists) gLists = new std::map<EGLDisplay, const std::string*>();
    const std::string*& list = (*gLists)[dpy];
    std::string want = real;
    for (const char* ext : kAndroidExtensions) {
        if (!hasExtension(real, ext)) {
            if (!want.empty() && want.back() != ' ') want += ' ';
            want += ext;
        }
    }
    // One string per display, kept for good: a caller may hold the pointer. A changed list (not
    // expected) gets a new string and the old one stays valid.
    if (!list || *list != want) list = new std::string(want);
    return list->c_str();
}
