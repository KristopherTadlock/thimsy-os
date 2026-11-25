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

// The boot screen: on a cold boot Nano's first frames show the title, the logo and what the
// device is doing, over the home that is still getting ready, and it goes as soon as the home
// (or the setup wizard) can be drawn. The picture is bootscreen/NanoBootArt's, the one the
// bootloader shows before Nano starts, so the status line is the only thing that appears.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoI18n.h"
#include "NanoOsk.h"
#include "bootscreen/NanoBootArt.h"

#include <cutils/properties.h>
#include <log/log.h>
#include <unistd.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>

namespace android {

static const char* const kBootLogo = "nano_xmb/boot/logo.svg";
static const char* const kBootFont = "/system/fonts/Roboto-Regular.ttf";
static constexpr int kBootScreenSettleFrames = 8;   // frames the ES-DE home draws under it before it goes

static nanoboot::Art bootArt() {
    nanoboot::Art art;
    const std::string dev = std::string("/data/system/") + kBootLogo;   // a development override first
    art.svgPath = access(dev.c_str(), R_OK) == 0 ? dev : std::string("/system/etc/") + kBootLogo;
    art.fontPath = kBootFont;
    art.fallbackFonts = {"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/NotoNaskhArabic-Regular.ttf",
                         "/system/fonts/NotoSansThai-Regular.ttf", "/system/fonts/NotoSansHebrew-Regular.ttf"};
    return art;
}

// Cold boot only: not a restart after an app (boot completed, or the DRM hand-back), not the
// Quick Resume path, and only in the home process.
void NanoMenu::bootScreenArm(bool quickResume) {
    if (mOverlayMode || quickResume) return;
    char bc[PROPERTY_VALUE_MAX] = {}, fd[PROPERTY_VALUE_MAX] = {};
    property_get("sys.boot_completed", bc, "0");
    property_get("sys.gammaos.nano.force_drm", fd, "0");
    if (strcmp(bc, "1") == 0 || strcmp(fd, "1") == 0) return;
    mBootScreenActive = true;
    mBootScreenFrames = 0;
    mBootScreenReadyFrames = 0;
    mBootScreenStartMs = (int64_t)uptimeMillis();
    if (mEsdeTheme && !mSetupWizardActive) bootSoundStart();
    ALOGI("bootscreen: shown");
}

// The ES-DE home loads its theme the first time it is drawn, which stalls the frame: it waits
// until the boot screen has been on screen, so what the stall is for can be read.
bool NanoMenu::bootScreenDefersHome() const {
    return mBootScreenActive && (mBootScreenFrames < 2 || mBootScreenHolding);
}

// Once the ES-DE home has settled under it and only the startup sound is awaited, the home is not
// drawn again (the boot screen is opaque) and the loop slows down, leaving the CPU and GPU to the
// system services being waited for. Decided once per frame, so the frame the boot screen goes in
// always has the home drawn under it.
void NanoMenu::bootScreenFrameStart() {
    mBootScreenHolding = mBootScreenActive && mBootScreenFrames > 0 && mEsdeTheme && !mSetupWizardActive
        && mBootScreenReadyFrames > kBootScreenSettleFrames && mEsdeArtPendingFrame == 0 && bootSoundState() == 0
        && (int64_t)uptimeMillis() - mBootScreenStartMs <= 60000;
}

// What the device is doing, in words for anyone. Android's own steps until the home starts
// getting ready, then the home's.
std::string NanoMenu::bootScreenStatus() const {
    if (mEsdeTheme && !mSetupWizardActive
            && (mBootScreenReadyFrames <= kBootScreenSettleFrames || mEsdeArtPendingFrame > 0)) {
        if (bootScreenDefersHome() && mBootScreenFrames == 1) return trDyn("Loading your theme");
        if (mEsdeLoaded && !bootScreenDefersHome()) return trDyn("Getting the menu ready");
    }
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("init.svc.zygote", v, "");
    if (strcmp(v, "running") != 0) return trDyn("Starting up");
    property_get("sys.system_server.start_count", v, "");
    if (!v[0]) return trDyn("Starting Android");
    property_get("sys.boot_completed", v, "0");
    if (v[0] != '1') return trDyn("Starting system services");
    return trDyn("Almost ready");
}

void NanoMenu::bootScreenClose() {
    if (mBootScreenBaseTex) glDeleteTextures(1, &mBootScreenBaseTex);
    if (mBootScreenStatusTex) glDeleteTextures(1, &mBootScreenStatusTex);
    mBootScreenBaseTex = mBootScreenStatusTex = 0;
    mBootScreenStatus.clear();
    mBootScreenActive = false;
    mBootScreenHolding = false;
    mDisplayDirty = true;
    bootSoundDrop();
    int artDecodes = 0;
    int64_t artMs = 0;
    esdeArtStats(&artDecodes, &artMs);
    ALOGI("bootscreen: closed after %lld ms, %d frames; theme art: %d decodes, %lld ms decoding (workers), %d pending",
          (long long)((int64_t)uptimeMillis() - mBootScreenStartMs), mBootScreenFrames, artDecodes,
          (long long)artMs, mEsdeArtPendingFrame);
}

static GLuint bootUpload(const uint8_t* rgba, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);   // drawn 1:1
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

// Drawn last in the primary pass, over whatever the home drew under it.
void NanoMenu::renderBootScreen() {
    if (!mBootScreenActive) return;
    if (mBootScreenFrames == 0) {
        std::vector<uint8_t> px;
        if (!nanoboot::renderBase(bootArt(), mWidth, mHeight, &px)) {
            ALOGW("bootscreen: the logo or font could not be read");
            bootScreenClose();
            return;
        }
        mBootScreenBaseTex = bootUpload(px.data(), mWidth, mHeight);
    }

    // Ready: the setup wizard or the home has been drawn under the boot screen (the ES-DE home once
    // its theme is in, it has drawn a few frames, none of the art it drew is still decoding and the
    // audio service is up, so its startup sound plays as the boot screen goes). A minute at most.
    const int64_t shownMs = (int64_t)uptimeMillis() - mBootScreenStartMs;
    bool ready;
    if (mSetupWizardActive || !mEsdeTheme) {
        ready = mBootScreenFrames >= 2;
    } else {
        if (mEsdeLoaded && !bootScreenDefersHome() && mBootScreenReadyFrames <= kBootScreenSettleFrames)
            mBootScreenReadyFrames++;
        ready = mBootScreenReadyFrames > kBootScreenSettleFrames && mEsdeArtPendingFrame == 0
            && bootSoundState() != 0;
    }
    if ((ready || shownMs > 60000) && !mBootScreenHolding) {
        if (mEsdeTheme && !mSetupWizardActive) bootSoundPlay();
        bootScreenClose();
        return;
    }

    const std::string status = bootScreenStatus();
    if (status != mBootScreenStatus) {
        if (mBootScreenStatusTex) glDeleteTextures(1, &mBootScreenStatusTex);
        mBootScreenStatusTex = 0;
        mBootScreenStatus = status;
        const nanoboot::Line line = nanoboot::renderStatus(bootArt(), mWidth, mHeight, nanoBidiVisual(status));
        if (!line.rgba.empty()) {
            mBootScreenStatusTex = bootUpload(line.rgba.data(), line.w, line.h);
            mBootScreenStatusRect[0] = line.x; mBootScreenStatusRect[1] = line.y;
            mBootScreenStatusRect[2] = line.w; mBootScreenStatusRect[3] = line.h;
        }
    }

    mTouch.block({0.0f, 0.0f, (float)mWidth, (float)mHeight});   // nothing under it takes touch yet
    setUiBlend();
    drawIconTex(mBootScreenBaseTex, 0.0f, 0.0f, (float)mWidth, (float)mHeight, 1.0f, 1.0f, 1.0f, 1.0f);
    if (mBootScreenStatusTex)
        drawIconTex(mBootScreenStatusTex, (float)mBootScreenStatusRect[0], (float)mBootScreenStatusRect[1],
                    (float)mBootScreenStatusRect[2], (float)mBootScreenStatusRect[3], 1.0f, 1.0f, 1.0f, 1.0f);
    mBootScreenFrames++;
    mDisplayDirty = true;
}

}  // namespace android
