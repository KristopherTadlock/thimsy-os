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
 */

// NanoMenuPdf - full-screen PDF reader for game manuals (Up on the ES-DE game information page).
// PDFium renders pages on a worker thread; the render thread uploads them as textures and keeps
// the current page and its neighbours. The worker holds its own reference to the shared state, so
// closing never waits for a page that is still rendering.

#include "NanoMenu.h"
#include "NanoI18n.h"          // trDyn
#include "NanoMenuShaders.h"   // FONT_CHAR_H

#include <GLES2/gl2.h>
#include <fpdfview.h>
#include <linux/input.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace android {

namespace {
std::mutex gPdfiumMutex;   // PDFium is not thread safe; a closing reader's worker may still run
bool gPdfiumInit = false;
constexpr int kPdfMaxDim = 4096;
constexpr size_t kPdfMaxTextures = 4;
}  // namespace

struct NanoMenu::PdfShared {
    struct Req { int page, zoom, w, h; };
    struct Res { int page, zoom, w, h; std::vector<uint8_t> px; };
    std::mutex mu;
    std::condition_variable cv;
    bool quit = false;
    bool opened = false;
    std::string error;
    int pageCount = 0;
    std::vector<Req> reqs;            // wanted pages, best first
    int busyPage = -1, busyZoom = -1;  // the page being rendered
    std::vector<Res> results;
};

void NanoMenu::pdfOpen(const std::string& path) {
    pdfClose();
    std::string low = path;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.size() < 4 || low.compare(low.size() - 4, 4, ".pdf") != 0) {
        photoShowBanner(trDyn("This manual is not a PDF and can't be opened"));
        return;
    }
    auto s = std::make_shared<PdfShared>();
    mPdf = s;
    mPdfActive = true;
    mPdfPage = 0; mPdfZoom = 0; mPdfPanX = mPdfPanY = 0.0f;
    mPdfTouchDrag = false;
    ps3Sfx(PS3_SFX_OK);
    mDisplayDirty = true;
    std::thread([s, path] {
        FPDF_DOCUMENT doc = nullptr;
        int count = 0;
        unsigned long err = 0;
        {
            std::lock_guard<std::mutex> lk(gPdfiumMutex);
            if (!gPdfiumInit) { FPDF_InitLibrary(); gPdfiumInit = true; }
            doc = FPDF_LoadDocument(path.c_str(), nullptr);
            if (doc) count = FPDF_GetPageCount(doc);
            else err = FPDF_GetLastError();
        }
        {
            std::lock_guard<std::mutex> lk(s->mu);
            s->opened = true;
            s->pageCount = count;
            if (!doc) s->error = err == FPDF_ERR_PASSWORD ? trDyn("THIS MANUAL IS PASSWORD PROTECTED") : trDyn("THIS MANUAL CAN'T BE OPENED");
            else if (count <= 0) s->error = trDyn("THIS MANUAL HAS NO PAGES");
        }
        while (doc && count > 0) {
            PdfShared::Req r;
            {
                std::unique_lock<std::mutex> lk(s->mu);
                s->cv.wait(lk, [&] { return s->quit || !s->reqs.empty(); });
                if (s->quit) break;
                r = s->reqs.front();
                s->reqs.erase(s->reqs.begin());
                s->busyPage = r.page; s->busyZoom = r.zoom;
            }
            PdfShared::Res out{r.page, r.zoom, 0, 0, {}};
            {
                std::lock_guard<std::mutex> lk(gPdfiumMutex);
                if (FPDF_PAGE page = FPDF_LoadPage(doc, r.page)) {
                    const float pw = std::max(1.0f, FPDF_GetPageWidthF(page));
                    const float ph = std::max(1.0f, FPDF_GetPageHeightF(page));
                    float sc = r.zoom == 0 ? std::min(r.w / pw, r.h / ph) : (r.zoom == 1 ? 1.0f : 2.0f) * r.w / pw;
                    sc = std::min(sc, std::min(kPdfMaxDim / pw, kPdfMaxDim / ph));
                    const int w = std::max(1, (int)lroundf(pw * sc)), h = std::max(1, (int)lroundf(ph * sc));
                    if (FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0)) {
                        FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
                        FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER);
                        const uint8_t* src = (const uint8_t*)FPDFBitmap_GetBuffer(bmp);
                        const int stride = FPDFBitmap_GetStride(bmp);
                        out.px.resize((size_t)w * h * 4);
                        for (int y = 0; y < h; y++) memcpy(&out.px[(size_t)y * w * 4], src + (size_t)y * stride, (size_t)w * 4);
                        for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 0xFF;   // opaque (no alpha bitmap)
                        out.w = w; out.h = h;
                        FPDFBitmap_Destroy(bmp);
                    }
                    FPDF_ClosePage(page);
                }
            }
            std::lock_guard<std::mutex> lk(s->mu);
            s->busyPage = s->busyZoom = -1;
            if (!s->quit) s->results.push_back(std::move(out));
        }
        if (doc) {
            std::lock_guard<std::mutex> lk(gPdfiumMutex);
            FPDF_CloseDocument(doc);
        }
    }).detach();
}

void NanoMenu::pdfClose() {
    if (mPdf) {
        {
            std::lock_guard<std::mutex> lk(mPdf->mu);
            mPdf->quit = true;
            mPdf->reqs.clear();
            mPdf->results.clear();
        }
        mPdf->cv.notify_all();
        mPdf.reset();
    }
    for (auto& t : mPdfTex) if (t.tex) glDeleteTextures(1, &t.tex);
    mPdfTex.clear();
    if (mPdfActive) mDisplayDirty = true;
    mPdfActive = false;
}

// The box pages are laid out in: the whole screen above the help bar.
void NanoMenu::pdfViewBox(float* w, float* h) const {
    *w = (float)mWidth;
    *h = (float)mHeight - std::min((float)mWidth, (float)mHeight) * 0.07f;
}

const NanoMenu::PdfTex* NanoMenu::pdfCurrentTex() const {
    const PdfTex* best = nullptr;
    for (const auto& t : mPdfTex) {
        if (t.page != mPdfPage) continue;
        if (t.zoom == mPdfZoom) return &t;
        if (t.tex && (!best || t.used > best->used)) best = &t;   // another zoom of this page until the new one is ready
    }
    return best;
}

void NanoMenu::renderPdf() {
    if (!mPdfActive || !mPdf) return;
    const float W = (float)mWidth, H = (float)mHeight, unit = std::min(W, H);
    float boxW, boxH;
    pdfViewBox(&boxW, &boxH);
    mPdfClock++;

    // Take finished pages, ask for the current page and its neighbours.
    std::vector<PdfShared::Res> done;
    int count = 0;
    bool opened = false;
    std::string error;
    {
        std::lock_guard<std::mutex> lk(mPdf->mu);
        done.swap(mPdf->results);
        count = mPdf->pageCount;
        opened = mPdf->opened;
        error = mPdf->error;
    }
    for (auto& r : done) {
        PdfTex t;
        t.page = r.page; t.zoom = r.zoom; t.w = r.w; t.h = r.h; t.used = mPdfClock;
        if (r.px.empty()) { mPdfTex.push_back(t); continue; }   // a page that can't be drawn (tex 0): not asked again
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r.w, r.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, r.px.data());
        mPdfTex.push_back(t);
    }
    if (count > 0) mPdfPage = std::max(0, std::min(mPdfPage, count - 1));
    if (const PdfTex* cur = pdfCurrentTex()) const_cast<PdfTex*>(cur)->used = mPdfClock;
    // Keep the newest few, never the page on screen.
    while (mPdfTex.size() > kPdfMaxTextures) {
        auto victim = mPdfTex.end();
        for (auto it = mPdfTex.begin(); it != mPdfTex.end(); ++it) {
            if (it->page == mPdfPage && it->zoom == mPdfZoom) continue;
            if (victim == mPdfTex.end() || it->used < victim->used) victim = it;
        }
        if (victim == mPdfTex.end()) break;
        if (victim->tex) glDeleteTextures(1, &victim->tex);
        mPdfTex.erase(victim);
    }
    bool waiting = !opened;
    if (count > 0) {
        std::vector<PdfShared::Req> want;
        for (int p : {mPdfPage, mPdfPage + 1, mPdfPage - 1}) {
            if (p < 0 || p >= count) continue;
            bool have = false;
            for (const auto& t : mPdfTex) if (t.page == p && t.zoom == mPdfZoom) have = true;
            if (!have) want.push_back({p, mPdfZoom, (int)boxW, (int)boxH});
        }
        std::lock_guard<std::mutex> lk(mPdf->mu);
        mPdf->reqs.clear();
        for (const auto& r : want)
            if (r.page != mPdf->busyPage || r.zoom != mPdf->busyZoom) mPdf->reqs.push_back(r);
        if (!want.empty()) waiting = true;
        if (!mPdf->reqs.empty()) mPdf->cv.notify_one();
    }
    if (waiting) { mDisplayDirty = true; mEsdeWantsFastFrame = true; }   // keep polling the worker at full rate

    drawQuad(0.0f, 0.0f, W, H, 0.12f, 0.12f, 0.13f, 1.0f);
    // Touch: nothing under the reader is reached. A tap on the left or right third of the page turns
    // it, in the middle zooms (as A does); pdfTouchPan pans on a drag.
    mTouch.block({0.0f, 0.0f, W, H});
    if (count > 0) {
        mTouch.tap({0.0f, 0.0f, boxW / 3.0f, boxH}, [this]() { if (mPdfActive) pdfTurn(-1, false); });
        mTouch.tap({boxW / 3.0f, 0.0f, boxW / 3.0f, boxH}, [this]() { if (mPdfActive) pdfKey(BTN_SOUTH); });
        mTouch.tap({boxW * 2.0f / 3.0f, 0.0f, boxW / 3.0f, boxH}, [this]() { if (mPdfActive) pdfTurn(1, false); });
    }
    const PdfTex* t = pdfCurrentTex();
    if (t && !t->tex) {
        error = trDyn("THIS PAGE CAN'T BE SHOWN");
        t = nullptr;
    }
    if (t) {
        // Shown at the size it was rendered for this zoom (an older zoom is scaled to match).
        float dw = (float)t->w, dh = (float)t->h;
        if (t->zoom != mPdfZoom) {
            const float ar = dw / dh;
            if (mPdfZoom == 0) { dw = boxW; dh = dw / ar; if (dh > boxH) { dh = boxH; dw = dh * ar; } }
            else { dw = boxW * (mPdfZoom == 1 ? 1.0f : 2.0f); dh = dw / ar; }
        }
        mPdfPanX = std::max(0.0f, std::min(mPdfPanX, dw - boxW));
        mPdfPanY = std::max(0.0f, std::min(mPdfPanY, dh - boxH));
        const float x = dw <= boxW ? (boxW - dw) * 0.5f : -mPdfPanX;
        const float y = dh <= boxH ? (boxH - dh) * 0.5f : -mPdfPanY;
        drawIconTex(t->tex, x, y, dw, dh, 1.0f, 1.0f, 1.0f, 1.0f);
        mPdfShownW = dw; mPdfShownH = dh;
    } else {
        mPdfShownW = mPdfShownH = 0.0f;
        if (!error.empty()) {
            const float msc = (unit * 0.033f) / (float)FONT_CHAR_H;
            drawText(error.c_str(), (W - measureText(error.c_str(), msc)) * 0.5f, (boxH - unit * 0.033f) * 0.5f,
                     msc, 0.6f, 0.6f, 0.6f, 1.0f);
        } else {
            drawLoadingSpinner(W * 0.5f, boxH * 0.5f, unit * 0.08f, 1.0f);
        }
    }
    drawQuad(0.0f, boxH, W, H - boxH, 0.0f, 0.0f, 0.0f, 1.0f);

    std::vector<std::pair<std::string, std::string>> prompts;
    if (count > 1) prompts.push_back({"left/right", "page"});
    if (mPdfShownH > boxH + 0.5f) prompts.push_back({"up/down", "scroll"});
    if (count > 0) prompts.push_back({"a", "zoom"});
    prompts.push_back({"b", "back"});
    esdeDrawPlainHelp(prompts, 1.0f);
    if (count > 0) {
        char pos[48];
        snprintf(pos, sizeof(pos), trDyn("PAGE %d/%d"), mPdfPage + 1, count);
        const float fsc = (unit * 0.026f) / (float)FONT_CHAR_H;
        drawText(pos, W - unit * 0.02f - measureText(pos, fsc), H - (H - boxH) * 0.5f - unit * 0.013f,
                 fsc, 0.6f, 0.6f, 0.6f, 1.0f);
    }
}

int NanoMenu::pdfPageCount() const {
    if (!mPdf) return 0;
    std::lock_guard<std::mutex> lk(mPdf->mu);
    return mPdf->pageCount;
}

void NanoMenu::pdfTurn(int dir, bool toBottom) {
    const int count = pdfPageCount();
    const int p = std::max(0, std::min(mPdfPage + dir, count - 1));
    if (p == mPdfPage) return;
    mPdfPage = p;
    mPdfPanY = toBottom ? 1e9f : 0.0f;   // clamped to the page on the next frame
    esdeSfx(4);
    mDisplayDirty = true;
}

bool NanoMenu::pdfNav(NavDir dir) {
    if (!mPdfActive) return false;
    float boxW, boxH;
    pdfViewBox(&boxW, &boxH);
    const float step = std::min(boxW, boxH) * 0.15f;
    switch (dir) {
    case NavDir::Left:
        if (mPdfShownW > boxW + 0.5f && mPdfPanX > 0.5f) mPdfPanX = std::max(0.0f, mPdfPanX - step);
        else pdfTurn(-1, false);
        break;
    case NavDir::Right:
        if (mPdfShownW > boxW + 0.5f && mPdfPanX < mPdfShownW - boxW - 0.5f) mPdfPanX += step;
        else pdfTurn(1, false);
        break;
    case NavDir::Up:   // past the top of a page, the previous page from its bottom
        if (mPdfShownH > boxH + 0.5f && mPdfPanY > 0.5f) mPdfPanY = std::max(0.0f, mPdfPanY - step);
        else pdfTurn(-1, true);
        break;
    case NavDir::Down:
        if (mPdfShownH > boxH + 0.5f && mPdfPanY < mPdfShownH - boxH - 0.5f) mPdfPanY += step;
        else pdfTurn(1, false);
        break;
    default:
        break;
    }
    mDisplayDirty = true;
    return true;
}

bool NanoMenu::pdfKey(int code) {
    if (!mPdfActive) return false;
    switch (code) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
        return false;                                   // navPress -> pdfNav
    case BTN_TL: pdfTurn(-1, false); break;
    case BTN_TR: pdfTurn(1, false); break;
    case BTN_TL2: pdfTurn(-mPdfPage, false); break;
    case BTN_TR2: pdfTurn(pdfPageCount(), false); break;
    case BTN_SOUTH: case KEY_ENTER:                     // fit page -> fit width -> twice the width
        if (pdfPageCount() > 0) {
            float boxW, boxH;
            pdfViewBox(&boxW, &boxH);
            const float cy = mPdfShownH > 0.0f ? (mPdfPanY + boxH * 0.5f) / mPdfShownH : 0.0f;
            const float cx = mPdfShownW > 0.0f ? (mPdfPanX + boxW * 0.5f) / mPdfShownW : 0.5f;
            const float k = mPdfZoom == 0 ? 0.0f : (mPdfZoom == 1 ? 2.0f : 0.5f);
            mPdfZoom = (mPdfZoom + 1) % 3;
            // Keep the middle of the view where it was (fit page starts at the top).
            mPdfPanX = std::max(0.0f, cx * mPdfShownW * k - boxW * 0.5f);
            mPdfPanY = std::max(0.0f, cy * mPdfShownH * k - boxH * 0.5f);
            ps3Sfx(PS3_SFX_OK);
        }
        break;
    case BTN_EAST: case KEY_BACK: case KEY_ESC: case BTN_START: case BTN_SELECT:
        pdfClose();
        if (code == BTN_SELECT) mSelectSearchArmed = false;
        ps3Sfx(PS3_SFX_BACK);
        break;
    case KEY_F10: case BTN_MODE: case KEY_HOMEPAGE:     // leave, and let the key do its own job
        esdeOverlayCloseAll();
        return false;
    default:
        break;
    }
    mDisplayDirty = true;
    return true;
}

// Touch: a drag pans the page, freely in both directions, which zones can't express; the taps
// (turn a page, zoom, the help bar) are the zones renderPdf registers. Runs before touchZonesFrame.
void NanoMenu::pdfTouchPan() {
    float px, py;
    const bool down = mTouchDown, downEdge = down && !mTouchWasDown;
    if (!down) { mPdfTouchDrag = false; return; }
    if (!touchLogicalPx(px, py)) return;
    if (downEdge) {
        mPdfTouchX0 = mPdfTouchX = px; mPdfTouchY0 = mPdfTouchY = py;
        mPdfTouchDrag = false;
        return;
    }
    // Past touchZonesFrame's slop, so a press that is a tap there does not pan here.
    const float slop = touchSlopPx();
    if (!mPdfTouchDrag && std::hypot(px - mPdfTouchX0, py - mPdfTouchY0) >= slop) mPdfTouchDrag = true;
    if (mPdfTouchDrag) {
        mPdfPanX -= px - mPdfTouchX;
        mPdfPanY -= py - mPdfTouchY;
        mDisplayDirty = true;
    }
    mPdfTouchX = px; mPdfTouchY = py;
}

}  // namespace android
