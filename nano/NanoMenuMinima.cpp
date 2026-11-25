/*
 * NanoMenuMinima.cpp - "Minima" home theme for gammaos-nano.
 *
 * A minimal list launcher inspired by NextUI (LoveRetro/NextUI, itself descended from MinUI):
 * a full-screen vertical TEXT list on a pure-black canvas, the selected row wrapped in a white
 * rounded capsule pill with inverted (black) text, a small accent status pill top-right and an
 * accent button-hint bar along the bottom. Like the DSi theme, Minima rides the shared PS3 XMB
 * state machine (mPs3Cats / mPs3Stack / mNdsAtRoot and the nds* nav helpers) and only swaps the
 * home render, the top-level nav mapping and the nav SFX - every modal
 * (options, dialogs, pickers, OSK, media players) still reuses the existing chrome via the
 * ndsInModal() delegation, so the whole menu tree works for free.
 *
 * Faithful NextUI proportions (authored at its unscaled 256-tall reference and scaled to the
 * panel): PILL_SIZE 30 row height, PADDING 10, BUTTON_MARGIN 5, BUTTON_PADDING 12, list font 16.
 * Everything derives from scale = panelHeight / 256 so it adapts to any resolution / orientation /
 * aspect exactly as the web app does, with no hardcoded panel size.
 */

#include "NanoMenu.h"
#include "NanoMenuShaders.h"   // FONT_CHAR_H
#include "NanoI18n.h"          // trDyn
#include "NanoUtf8Case.h"      // nanoUtf8Lower (Applications search)

#include <cutils/properties.h> // property_get (Background Colour)
#include <linux/input.h>       // BTN_* (touch button hints)
#include <utils/SystemClock.h> // uptimeMillis
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace android {

// NextUI unscaled reference constants (common/defines.h). Scaled by panelH/MIN_REF_H at render time.
// A larger reference height = smaller, more condensed UI (more rows on screen). 336 gives ~8-9 rows
// on a 480-tall panel (NextUI's Brick reference is 256; the user asked for a tighter, smaller list).
static constexpr float MIN_REF_H     = 336.0f;  // controls the whole-UI density (bigger = smaller/tighter)
static constexpr float MIN_PILL      = 30.0f;   // PILL_SIZE (row height, capsule height)
static constexpr float MIN_PAD       = 10.0f;   // PADDING (screen-edge inset)
static constexpr float MIN_BTNMARGIN = 5.0f;    // BUTTON_MARGIN
static constexpr float MIN_BTNPAD    = 12.0f;   // BUTTON_PADDING (text inset inside the pill)
static constexpr float MIN_FONT      = 16.0f;   // font.large (list rows)
static constexpr float MIN_FONT_S    = 12.0f;   // font.small (hints / clock)

// The width a row's value may take of `avail` (the space right of the label's start): all of it
// when label and value fit side by side, else what the label leaves (keeping it at least 45% of
// the row), but never under 40% of the row.
static float minimaValueMaxW(float avail, float labelW, float valueW, float sc) {
    const float gap = 12.0f * sc;
    if (labelW + gap + valueW <= avail) return valueW;
    return std::max(avail * 0.40f, avail - std::min(labelW, avail * 0.45f) - gap);
}

// Resolve the Minima accent colour. Follows the shared "Colour" theme setting (mPs3ColorIdx /
// kPs3ColorOpts) exactly like the XMB, so the user's colour choice drives it. When left at
// "Original" (index 0) it defaults to NextUI's signature berry #9B2257 rather than the XMB's
// per-month hue, so Minima looks like NextUI out of the box. (Defined in NanoMenuPS3Menu.cpp
// where kPs3ColorOpts is visible.)

void NanoMenu::minimaSfxTick() {
    // Fire exactly one nav/drill/back sound per frame by diffing this frame's focus vs last.
    if (ndsInModal()) { mMinimaSfxDepth = ndsNavDepth(); mMinimaSfxSel = ndsFocusSel(); return; }
    int depth = ndsNavDepth();
    int sel   = ndsFocusSel();
    if (mMinimaSfxDepth < 0) { mMinimaSfxDepth = depth; mMinimaSfxSel = sel; return; }
    if (depth > mMinimaSfxDepth)      minimaSfx(MIN_SFX_DRILL);   // drilled a level deeper
    else if (depth < mMinimaSfxDepth) minimaSfx(MIN_SFX_BACK);    // walked back up
    else if (sel != mMinimaSfxSel)    minimaSfx(MIN_SFX_CURSOR);  // moved within the list
    mMinimaSfxDepth = depth; mMinimaSfxSel = sel;
}

// Solid background colour (Theme Settings > Background Colour). The prop holds either "none"
// (default: pure-black NextUI canvas) or a 6-digit hex RGB. Returns true + the parsed colour when
// a solid colour is set, so the Minima backdrop fills with it instead of black / the wave. Read
// live each frame so a chooser change applies immediately (property_get is a cheap shmem read).
bool NanoMenu::minimaSolidBg(float* r, float* g, float* b) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.minima.bg", v, "none");
    if (v[0] == '\0' || !strcmp(v, "none") || !strcmp(v, "black") || !strcmp(v, "0")) return false;
    unsigned int rr = 0, gg = 0, bb = 0;
    if (strlen(v) >= 6 && sscanf(v, "%2x%2x%2x", &rr, &gg, &bb) == 3) {
        if (r) *r = (float)rr / 255.0f;
        if (g) *g = (float)gg / 255.0f;
        if (b) *b = (float)bb / 255.0f;
        return true;
    }
    return false;
}

void NanoMenu::renderMinima() {
    // Single interactive list, full panel. The dual-screen (RG DS) secondary panel is filled with
    // just the backdrop by the render() dispatch, so there is only ever one list to drive.
    // The Applications level draws as a launcher-style icon grid instead (renderMinimaAppGrid).
    if (minimaAppGridActive()) renderMinimaAppGrid(0.0f, 0.0f, (float)mWidth, (float)mHeight);
    else                       renderMinimaList(0.0f, 0.0f, (float)mWidth, (float)mHeight);
}

// Home backdrop: a custom photo / video wallpaper when set (drawn directly so it shows regardless of
// the XMB Wave toggle), else Theme Settings > Background Colour, else the opt-in XMB wave, else the
// pure-black NextUI canvas. In an in-game overlay with no overlay wallpaper nano's framebuffer clear
// already laid down the app-dimming scrim, so it is left untouched; returns true in that case.
bool NanoMenu::minimaBackdrop(float rx, float ry, float rw, float rh) {
    const bool inGameScrim = overAppScrim();
    float bgR, bgG, bgB;
    if (inGameScrim) {
        // leave the framebuffer's app-dimming scrim untouched; the white list draws over it
    } else if (wallpaperActive(mRenderingPanel)) {
        // drawWallpaperFill / drawTopVideoWallpaper already apply the adjustable "Wallpaper Dimming" scrim,
        // which keeps the white list legible over a bright wallpaper (the user tunes it up for Minima).
        if (!(mRenderingPanel == 0 && drawTopVideoWallpaper()))   // looping video on top, else the still
            drawWallpaperFill(mRenderingPanel);
    } else if (minimaSolidBg(&bgR, &bgG, &bgB)) {
        drawQuad(rx, ry, rw, rh, bgR, bgG, bgB, 1.0f);            // user-chosen solid backdrop colour
    } else if (mXmbWave && mXmbWaveExplicit) {
        renderEffect();                                           // opt-in XMB wave / gradient background
        drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 0.34f);
        if (mCurrentEffect != 0) mMinimaWantsFrame = true;        // the effect animates: never idle-skip
    } else {
        drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 1.0f);         // pure-black NextUI canvas (default)
    }
    mTextOutlineMode = 2;   // re-assert flat text after renderEffect (which sets its own outline mode)
    return inGameScrim;
}

// The top-right accent status pill: a background scrape's count, real Wi-Fi / Bluetooth / battery
// icons and the clock. Returns the pill's left edge; draws it only when draw is set, so a screen can
// lay itself out around the pill first. Its height is the chrome row (MIN_PILL), at the top inset.
float NanoMenu::minimaStatusPill(float rx, float ry, float rw, float rh, bool draw) {
    const float sc = fminf(rw, rh) / MIN_REF_H;
    const float pad = MIN_PAD * sc, btnPad = MIN_BTNPAD * sc;
    const float fsHint = (MIN_FONT_S * sc) / (float)FONT_CHAR_H;
    // Theme Settings > Battery Percentage: the charge as a number next to the battery icon (NextUI
    // Appearance "Show Battery Percent"). Shared with the DSi status bar; read live.
    char bpv[PROPERTY_VALUE_MAX] = {}; property_get("persist.gammaos.nano.battpct", bpv, "0");
    const bool minBattPct = (bpv[0] == '1' || bpv[0] == 't' || bpv[0] == 'o');
    int wl, wb, bl;
    { std::lock_guard<std::mutex> lk(mNetStateMutex); wl = mWifiLevel; wb = mWifiBars; bl = mBtLevel; }
    char clockbuf[24] = {};
    time_t tt = time(nullptr); struct tm ltm; localtime_r(&tt, &ltm);
    clockRefreshMaybe(); formatClockHM(clockbuf, sizeof(clockbuf), ltm);   // honor the 12/24-hour setting

    const float ph = MIN_PILL * sc, gap = 6.0f * sc;
    const float iconH = MIN_FONT_S * sc * 1.15f;
    const float wifiSf = iconH / 18.0f, wifiW = 22.0f * wifiSf;
    const float btSf   = iconH / 20.0f, btW = 14.0f * btSf;
    const float battBW = iconH * 1.55f, battNub = 2.0f * sc, battW = battBW + battNub;
    const bool showWifi = (wl != kWifiLevel_Off && wl != kWifiLevel_Unknown);
    const bool showBt   = (bl != kBtLevel_Off && bl != kBtLevel_Unknown);
    const bool showBatt = (mBatteryPercent >= 0);
    const float clockW = measureText(clockbuf, fsHint);
    char battpb[8] = {};
    if (showBatt && minBattPct) { int p = mBatteryPercent; if (p < 0) p = 0; if (p > 100) p = 100; snprintf(battpb, sizeof(battpb), "%d%%", p); }
    const std::string scrape = scrapeStatusLabel();   // a background scrape's count, first
    float contentW = clockW;
    if (!scrape.empty()) contentW += measureText(scrape.c_str(), fsHint) + gap * 2.0f;
    if (showWifi) contentW += wifiW + gap;
    if (showBt)   contentW += btW + gap;
    if (showBatt) contentW += battW + gap;
    if (showBatt && minBattPct) contentW += measureText(battpb, fsHint) + gap;   // widen the pill for the % label

    const float pw = contentW + btnPad * 2.0f;
    const float px = rx + rw - pad - pw, py = ry + pad;
    if (!draw) return px;
    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float accLum = 0.299f * ar + 0.587f * ag + 0.114f * ab;
    const float atc = (accLum > 0.62f) ? 0.0f : 1.0f;   // black on a light accent, white on a dark one
    drawRoundedRect(px, py, pw, ph, ph * 0.5f, ar, ag, ab, 1.0f);   // accent pill
    float ix = px + btnPad;
    const float iconY = py + (ph - iconH) * 0.5f;
    if (!scrape.empty()) {
        drawText(scrape.c_str(), ix, py + (ph - MIN_FONT_S * sc) * 0.5f, fsHint, atc, atc, atc, 1.0f);
        ix += measureText(scrape.c_str(), fsHint) + gap * 2.0f;
    }
    if (showWifi) {
        int bars = (wl == kWifiLevel_Connected) ? wb : 0;
        drawWifiIcon(ix, iconY, wifiSf, bars, atc, atc, atc, 1.0f);
        ix += wifiW + gap;
    }
    if (showBt) { drawBtIcon(ix, iconY, btSf, atc, atc, atc, 1.0f); ix += btW + gap; }
    if (showBatt) {
        float bh = iconH * 0.62f, by = py + (ph - bh) * 0.5f;
        int pct = mBatteryPercent; if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        drawRoundedRect(ix, by, battBW, bh, 2.0f * sc, atc, atc, atc, 1.0f);                                  // body
        drawRoundedRect(ix + 1.5f * sc, by + 1.5f * sc, battBW - 3.0f * sc, bh - 3.0f * sc, 1.5f * sc, ar, ag, ab, 1.0f);  // hollow
        float fillW = (battBW - 4.0f * sc) * ((float)pct / 100.0f);
        drawRoundedRect(ix + 2.0f * sc, by + 2.0f * sc, fillW, bh - 4.0f * sc, 1.0f * sc, atc, atc, atc, 1.0f);            // charge
        drawQuad(ix + battBW, by + bh * 0.28f, battNub, bh * 0.44f, atc, atc, atc, 1.0f);                     // nub
        if (mBatteryCharging) {   // charging mark: a solid bolt in the pill colour cut out of the dark fill
            float bwid = bh * 0.60f;
            drawBolt(ix + battBW * 0.5f - bwid * 0.5f, by - 1.0f * sc, bwid, bh + 2.0f * sc, ar, ag, ab, 1.0f);
        }
        ix += battW + gap;
        if (minBattPct) {
            drawText(battpb, ix, py + (ph - MIN_FONT_S * sc) * 0.5f, fsHint, atc, atc, atc, 1.0f);
            ix += measureText(battpb, fsHint) + gap;
        }
    }
    drawText(clockbuf, ix, py + (ph - MIN_FONT_S * sc) * 0.5f, fsHint, atc, atc, atc, 1.0f);
    return px;
}

void NanoMenu::renderMinimaList(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;   // Minima text is flat: no drop shadow / outline
    if (!mPs3MenuBuilt) initPs3Menu();   // build the shared XMB hierarchy that feeds the rows
    minimaSfxTick();

    const bool inGameScrim = minimaBackdrop(rx, ry, rw, rh);

    // ---- scrolling fanart hover background for a focused scraped game (same as the XMB) ----
    // A focused game with scraped fanart shows it as a slow Ken-Burns background under the list (a
    // 50% scrim keeps the white rows legible). Empty fanFile when the focus is not a scraped game, so
    // drawPs3CinfoBg fades it back out. mNdsAtRoot has no focused ROM, so focusedScrapeEntry returns null.
    const ScrapeEntry* minSe = inGameScrim ? nullptr : focusedScrapeEntry();
    // Focused app (for the Y-to-Pin legend hint below): the current row when it is a PS3_APP.
    bool minFocApp = false; std::string minFocPkg;
    if (!inGameScrim && !mNdsAtRoot) {
        const Ps3Item* fi = nullptr;
        if (!mPs3Stack.empty()) {
            const auto& its = mPs3Stack.back().items; int s = mPs3Stack.back().sel;
            if (s >= 0 && s < (int)its.size()) fi = &its[s];
        } else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) {
            const auto& its = mPs3Cats[mPs3CatIdx].items;
            if (mPs3ItemIdx >= 0 && mPs3ItemIdx < (int)its.size()) fi = &its[mPs3ItemIdx];
        }
        if (fi && fi->kind == PS3_APP && !fi->payloadStr.empty()) { minFocApp = true; minFocPkg = fi->payloadStr; }
    }
    if (!inGameScrim) {
        std::string fanFile = (minSe && scraperFanartEnabled() && !minSe->fan.empty()) ? minSe->fan : std::string();
        drawPs3CinfoBg("", fanFile);   // focusLabel is only used for the Photo Gallery cinfo (never a game)
        mTextOutlineMode = 2;          // drawPs3CinfoBg touches GL state; re-assert flat text
        // Minima only re-renders when dirty (no always-on wave), so keep the loop live while the fanart
        // is on screen - otherwise its Ken-Burns pan + fade-out freeze on the last drawn frame.
        if (mCinfoAlpha > 0.001f) mDisplayDirty = true;
    }

    float ar, ag, ab; minimaAccent(ar, ag, ab);   // accent = the Colour setting (berry by default)
    // Accent-pill text: black on a light accent (Yellow/White/Lime...), white on a dark one (berry),
    // so the status/hint legends stay legible for every colour preset.
    const float accLum = 0.299f * ar + 0.587f * ag + 0.114f * ab;
    const float atc = (accLum > 0.62f) ? 0.0f : 1.0f;

    // ---- adaptive scale: NextUI scales purely by panel HEIGHT (it targets landscape handhelds).
    // On a PORTRAIT panel that overshoots badly - the tall height inflates every row/font far beyond
    // what the narrow width needs. Scale by the SHORT edge instead so density tracks the constrained
    // dimension. On landscape the short edge IS the height, so this is byte-identical to before. ----
    const float sc      = fminf(rw, rh) / MIN_REF_H;
    const float pad     = MIN_PAD * sc;
    const float rowH    = MIN_PILL * sc;
    // At a large user font size the list text grows but a geometric row height would crowd the
    // rows and the selected pill, so scale the LIST row pitch + pill height with the font size so
    // the items get more breathing room. Chrome (status / legend pills) keeps rowH. fg 1.0 = same.
    const float lfg     = (mUserFontScale > 0.05f) ? mUserFontScale : 1.0f;
    const float listRowH = rowH * lfg;
    const float btnPad  = MIN_BTNPAD * sc;
    const float btnMg   = MIN_BTNMARGIN * sc;
    const float fsRow   = (MIN_FONT * sc) / (float)FONT_CHAR_H;
    const float fsHint  = (MIN_FONT_S * sc) / (float)FONT_CHAR_H;
    // Theme Settings > Long Names: 1 = keep the font size and scroll the focused name / clip the rest
    // (default), 0 = shrink a too-long name to fit. Read live so a chooser change applies at once.
    char nsv[PROPERTY_VALUE_MAX] = {}; property_get("persist.gammaos.nano.minima.namescroll", nsv, "1");
    const bool minNameScroll = (nsv[0] == '1' || nsv[0] == 't' || nsv[0] == 'o');

    // ---- resolve the current level's rows (categories at root, else the category/submenu items) ----
    // vals holds the inline right-aligned value for value-bearing rows (else empty). Only the
    // game-system rows (the GS_LIST On/Off list + the GS_EDITOR fields) expose their value so the
    // enable state and per-system config read at a glance; generic settings rows stay value-less
    // (enter to change), matching the DSi list decision.
    std::vector<std::string> rows;
    std::vector<std::string> vals;
    // Per-row checkbox state, parallel to rows/vals: -1 = normal row, 0 = unchecked, 1 = checked.
    // The rows are flattened to strings here, so the Ps3Item.checkState (set by the multi-select
    // builders: Slide Up/Down actions, Devices to Capture, Passthrough Blacklist) has to be carried
    // alongside or the Minima list loses it. The XMB theme draws these boxes; this keeps parity.
    std::vector<int> checks;
    auto pushItem = [&](const Ps3Item& it) {
        rows.push_back(trDyn(it.label.c_str()));
        checks.push_back(it.checkState);
        if (it.kind == PS3_GS_SYSTEM_ROW || it.kind == PS3_GS_FIELD || it.kind == PS3_CATORDER_ROW
            || it.kind == PS3_ITEMHIDE_ROW || it.kind == PS3_ST_ROW || ps3FolderRowKind(it.kind))
            vals.push_back(trDyn(it.value.c_str()));
        else vals.push_back(std::string());
    };
    int sel = 0;
    if (mNdsAtRoot) {
        for (auto& c : mPs3Cats) { rows.push_back(trDyn(c.name.c_str())); vals.push_back(std::string()); checks.push_back(-1); }
        sel = mPs3CatIdx;
    } else if (!mPs3Stack.empty()) {
        for (auto& it : mPs3Stack.back().items) pushItem(it);
        sel = mPs3Stack.back().sel;
    } else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) {
        for (auto& it : mPs3Cats[mPs3CatIdx].items) pushItem(it);
        sel = mPs3ItemIdx;
    }
    const int n = (int)rows.size();
    if (sel < 0) sel = 0; if (n > 0 && sel >= n) sel = n - 1;

    // ---- status-pill band geometry (top-right): computed up-front so the list can start BELOW the
    // pill on a narrow/portrait panel where the wide clock+icons pill would otherwise crowd the first
    // row's label into an unreadable sliver. statusPillLeft = pill's left x; statusBandBot = bottom y.
    // The pill itself is drawn later by the same minimaStatusPill. ----
    const float statusPillLeft = minimaStatusPill(rx, ry, rw, rh, false);
    const float statusBandBot  = ry + pad + rowH;

    // ---- list geometry: fills from the top inset down to above the hint bar ----
    const float listLeft   = rx + pad + btnMg;
    // Start the list below the status pill when the pill is wide relative to the row (portrait panels):
    // this stops the first row(s) being squeezed beside the pill and shrunk to an unreadable size. On a
    // wide (landscape/TV) panel the pill is a small fraction of the row, so the list stays at the top.
    const float rowContentW = (rx + rw - pad) - listLeft;
    const float pillSpanW   = (rx + rw - pad) - statusPillLeft;
    const float listTop     = (pillSpanW > rowContentW * 0.5f) ? (statusBandBot + 6.0f * sc) : (ry + pad);
    const float hintTop    = ry + rh - pad - rowH;          // hint bar occupies the bottom PILL_SIZE band
    const float listBottom = hintTop - btnMg;
    const int   visRows    = (int)fmaxf(1.0f, floorf((listBottom - listTop) / listRowH));
    const float textMaxW   = (rx + rw - pad) - listLeft - btnPad;   // room before the right edge

    // ---- ease the scroll window and the pill toward the selection (NextUI ~3-frame glide) ----
    const float dt = fmaxf(0.0f, fminf(0.1f, mFrameDt));
    float targetScroll = 0.0f;
    if (n > visRows) {
        // keep the selection inside the window, biased to centre like NextUI's paged list
        targetScroll = (float)sel - (float)(visRows / 2);
        const float maxScroll = (float)(n - visRows);
        if (targetScroll < 0.0f) targetScroll = 0.0f;
        if (targetScroll > maxScroll) targetScroll = maxScroll;
    }
    // A wrap-around jump (Up on the first row / Down on the last) snaps the window straight to the
    // new end instead of gliding through the whole list.
    if (mListWrapSnap) {
        mMinimaScroll = targetScroll; mMinimaSelAnim = (float)sel; mListWrapSnap = false;
    } else {
        // exponential approach tuned to settle in ~3 frames at 60fps (NextUI's 3-step linear glide feel)
        const float k = 1.0f - powf(1.0f - 0.55f, dt * 60.0f);
        mMinimaScroll  += (targetScroll - mMinimaScroll) * k;
        mMinimaSelAnim += ((float)sel - mMinimaSelAnim) * k;
        if (fabsf(mMinimaScroll - targetScroll)  > 0.002f) { mDisplayDirty = true; mMinimaWantsFrame = true; } else mMinimaScroll = targetScroll;
        if (fabsf(mMinimaSelAnim - (float)sel)   > 0.002f) { mDisplayDirty = true; mMinimaWantsFrame = true; } else mMinimaSelAnim = (float)sel;
    }

    // ---- detect a level change (drill/back) to arm the slide + crossfade, and snap the window so
    // the new level starts from a clean position (done before the draw so the motion is same-frame) ----
    {
        int depth = ndsNavDepth();
        if (mMinimaPrevDepth < 0) mMinimaPrevDepth = depth;
        else if (depth != mMinimaPrevDepth) {
            mMinimaTransStart = (int64_t)uptimeMillis();
            mMinimaTransDir = (depth > mMinimaPrevDepth) ? +1 : -1;
            mMinimaPrevDepth = depth;
            mMinimaScroll = targetScroll; mMinimaSelAnim = (float)sel;   // snap the window on a level change
        }
    }

    // ---- horizontal slide on a level change (NextUI folder slide: drill enters from the right,
    // back enters from the left, cubic ease-out over 150ms). The rows + pill share this x offset;
    // the crossfade is the black wash stamped in the transition block below. ----
    float slideX = 0.0f;
    if (mMinimaTransStart > 0) {
        float t = (float)((int64_t)uptimeMillis() - mMinimaTransStart) / 150.0f;
        if (t < 1.0f) {
            float ease = 1.0f - powf(1.0f - t, 3.0f);                    // NextUI TRANSITION_CURVE (1-(1-t)^3)
            slideX = (float)mMinimaTransDir * (rw * 0.22f) * (1.0f - ease);
            mDisplayDirty = true; mMinimaWantsFrame = true;
        }
    }
    const float lx = listLeft + slideX;

    // ---- left-gutter checkbox for multi-select rows (Slide Up/Down actions, Devices to Capture,
    // Passthrough Blacklist). An outlined square with a two-stroke tick when checked, drawn in the
    // passed colour so it reads on both the white idle rows and the black-on-white selected pill.
    // ckShiftW is how far a checkbox pushes its label right; sized off the row font so it tracks the
    // user font scale. Mirrors the XMB box (NanoMenuPS3Menu.cpp) and the DSi box. ----
    const float ckSide   = MIN_FONT * sc * 0.86f;
    const float ckGap    = 8.0f * sc;
    const float ckShiftW = ckSide + ckGap;
    auto drawMinCheck = [&](float x, float y, float r, float g, float b, float a, bool checked) {
        float bwid = fmaxf(1.0f, 1.5f * sc);
        drawQuad(x,                 y,                 ckSide, bwid,   r, g, b, a);   // top
        drawQuad(x,                 y + ckSide - bwid, ckSide, bwid,   r, g, b, a);   // bottom
        drawQuad(x,                 y,                 bwid,   ckSide, r, g, b, a);   // left
        drawQuad(x + ckSide - bwid, y,                 bwid,   ckSide, r, g, b, a);   // right
        if (checked) {
            float tw = fmaxf(1.5f, 2.0f * sc);
            ps3ThickLine(x + ckSide * 0.24f, y + ckSide * 0.52f, x + ckSide * 0.44f, y + ckSide * 0.72f, tw, r, g, b, a);
            ps3ThickLine(x + ckSide * 0.44f, y + ckSide * 0.72f, x + ckSide * 0.78f, y + ckSide * 0.26f, tw, r, g, b, a);
        }
    };

    // (status-pill band geometry computed up-front, before listTop, so the list starts below it)

    // Touch: a drag moves the selection a row per row pitch; a row's tap selects and opens it.
    const bool touchList = mRenderingPanel == 0 && !ndsInModal() && !(mLaunchFadeStart > 0 && !mOverlayMode)
                           && !(mOverlayMode && mOverlayLaunchPending);
    const TouchRect listBox{rx, listTop, rw, listBottom - listTop};
    const int touchDepth = ndsNavDepth();
    if (touchList && n > 0)
        mTouch.scroll(listBox, TouchZones::kUpDown, listRowH, [this, touchDepth](int dir) {
            int* s = nullptr;
            const int cnt = minimaListLevel(&s);
            if (!s || ndsNavDepth() != touchDepth || ndsInModal() || *s + dir < 0 || *s + dir >= cnt) return false;
            *s += dir;
            mDisplayDirty = true;
            return true;
        });

    // ---- draw the rows (white text) then the gliding white capsule + inverted selected label ----
    if (n == 0) {
        const char* empty = trDyn("Empty");
        float tw = measureText(empty, fsRow);
        drawText(empty, rx + rw * 0.5f - tw * 0.5f, listTop + listRowH * 0.5f, fsRow, 1.0f, 1.0f, 1.0f, 0.55f);
    }
    for (int i = 0; i < n; i++) {
        float rowY = listTop + ((float)i - mMinimaScroll) * listRowH;
        if (rowY + listRowH < listTop - 1.0f || rowY > listBottom + 1.0f) continue;   // clip to the list band
        if (touchList)
            mTouch.tap(TouchRect{lx, rowY, rx + rw - pad - lx, listRowH}.clippedTo(listBox), [this, touchDepth, i]() {
                int* s = nullptr;
                const int cnt = minimaListLevel(&s);
                if (!s || ndsNavDepth() != touchDepth || ndsInModal() || i >= cnt) return;
                *s = i;
                mDisplayDirty = true;
                ndsNavSelect(true);
            });
        if (i == sel) continue;                                                    // selected drawn on the pill below
        float ty = rowY + (listRowH - MIN_FONT * sc * lfg) * 0.5f;
        // Multi-select rows draw a white checkbox in the left gutter; the label shifts right by
        // ckShiftW so the two never overlap (idle rows are white text on the dark canvas).
        const bool  isCk = (checks[i] >= 0);
        const float lblX = (lx + btnPad) + (isCk ? ckShiftW : 0.0f);
        if (isCk)
            drawMinCheck(lx + btnPad, rowY + (listRowH - ckSide) * 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, checks[i] == 1);
        // A value-bearing row draws its value right-aligned (dim white); the label is clipped to
        // the space before it so the two never overlap. rowRight tracks slideX so the value slides
        // with the row during a level-change transition, like the label does.
        float rowRight = rx + rw - pad - btnPad + slideX;
        if (rowY < statusBandBot && rowY + listRowH > ry + pad)
            rowRight = fminf(rowRight, statusPillLeft - 8.0f * sc);   // clear the status pill
        float labelMaxW = textMaxW - (isCk ? ckShiftW : 0.0f);
        // Keep the label clear of the top-right status pill (mirror the value-column clip above):
        // in portrait a long first-row label (e.g. "Quick Menu") otherwise runs under the pill.
        if (rowY < statusBandBot && rowY + listRowH > ry + pad)
            labelMaxW = fminf(labelMaxW, (statusPillLeft - 8.0f * sc) - lblX);
        if (!vals[i].empty()) {
            // A value too long for its share of the row is clipped at its window's edge (the
            // selected row scrolls it, below) rather than running over the label.
            const float vw = measureText(vals[i].c_str(), fsRow);
            const float valMax = minimaValueMaxW(rowRight - lblX, measureText(rows[i].c_str(), fsRow), vw, sc);
            if (vw > valMax) {
                scissorLogicalRect(rowRight - valMax, rowY, valMax, listRowH);
                drawText(vals[i].c_str(), rowRight - valMax, ty, fsRow, 1.0f, 1.0f, 1.0f, 0.70f);
                glDisable(GL_SCISSOR_TEST);
            } else {
                drawText(vals[i].c_str(), rowRight - vw, ty, fsRow, 1.0f, 1.0f, 1.0f, 0.70f);
            }
            labelMaxW = (rowRight - std::min(vw, valMax) - 12.0f * sc) - lblX;
        }
        float fs = fsRow, tw = measureText(rows[i].c_str(), fs);
        if (tw > labelMaxW && labelMaxW > 0.0f) {
            if (minNameScroll) {
                // "Scroll" mode (Theme Settings > Long Names): keep the font size and CLIP the label
                // to its column instead of shrinking. The focused row marquee-scrolls (below); a
                // non-focused long name just truncates at the column edge, at full size.
                scissorLogicalRect(lblX, rowY, labelMaxW, listRowH);
                drawText(rows[i].c_str(), lblX, ty, fs, 1.0f, 1.0f, 1.0f, 1.0f);
                glDisable(GL_SCISSOR_TEST);
                continue;
            }
            fs *= labelMaxW / tw;   // default "Shrink to Fit": scale the font down so it all fits
        }
        drawText(rows[i].c_str(), lblX, ty, fs, 1.0f, 1.0f, 1.0f, 1.0f);   // COLOR_LIST_TEXT white
    }
    // The capsule pill, hugging the selected label, glided to the eased position. A label too long to
    // fit MARQUEE-scrolls (NextUI: after a short pause, 2px/frame with a 30px gap, looping) inside a
    // full-width pill, instead of shrinking to fit.
    if (n > 0) {
        const int si = sel < n ? sel : 0;
        const std::string& lbl = rows[si];
        const std::string& val = vals[si];
        // A selected multi-select row keeps its checkbox on the white pill, drawn in black to match
        // the inverted (black-on-white) selected text; the label + text width reserve ckShiftW for it.
        const bool  isCkSel  = (checks[si] >= 0);
        const float pillLblX = lx + btnPad + (isCkSel ? ckShiftW : 0.0f);
        const float fs = fsRow, tw = measureText(lbl.c_str(), fs);
        const float pillY = listTop + (mMinimaSelAnim - mMinimaScroll) * listRowH + listRowH * 0.07f;
        const float pillH = listRowH * 0.86f;
        const float ckYsel = pillY + (pillH - ckSide) * 0.5f;
        // The selected capsule normally runs to the right list margin, but the FIRST row sits under
        // the top-right status pill (portrait especially): cap the pill's right edge to statusPillLeft
        // so a long selected label (e.g. "Internet Connection") is not overlapped by the pill. This
        // mirrors the non-selected rowRight clip above and feeds all three branches via maxPillW.
        float pillRightLimit = rx + rw - pad;
        if (pillY < statusBandBot && pillY + pillH > ry + pad)
            pillRightLimit = fminf(pillRightLimit, statusPillLeft - 8.0f * sc);
        const float maxPillW = pillRightLimit - lx;
        const float maxTextW = maxPillW - btnPad * 2.0f - (isCkSel ? ckShiftW : 0.0f);
        const float ty = pillY + (pillH - MIN_FONT * sc * lfg) * 0.5f;
        if (sel != mMinimaMarqueeSel) { mMinimaMarqueeSel = sel; mMinimaMarquee = 0.0f; mMinimaMarqueeStart = (int64_t)uptimeMillis(); }
        if (!val.empty()) {
            // value-bearing selected row: full-width capsule, label left + value right (both black)
            drawRoundedRect(lx, pillY, maxPillW, pillH, pillH * 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
            if (isCkSel) drawMinCheck(lx + btnPad, ckYsel, 0.0f, 0.0f, 0.0f, 1.0f, checks[si] == 1);
            const float vw = measureText(val.c_str(), fs);
            float valRight = lx + maxPillW - btnPad;
            if (pillY < statusBandBot && pillY + pillH > ry + pad)
                valRight = fminf(valRight, statusPillLeft - 8.0f * sc);   // clear the status pill
            // A value longer than its window scrolls like a long name (pause, then loop).
            const float valMax = minimaValueMaxW(valRight - pillLblX, tw, vw, sc);
            const float vx = valRight - std::min(vw, valMax);
            if (vw > valMax) {
                const float gap = 30.0f * sc, loopW = vw + gap;
                if ((int64_t)uptimeMillis() - mMinimaMarqueeStart > 700) {
                    mMinimaMarquee += 2.0f * sc * fmaxf(0.0f, fminf(3.0f, mFrameDt * 60.0f));
                    if (mMinimaMarquee >= loopW) mMinimaMarquee -= loopW;
                }
                mDisplayDirty = true; mMinimaWantsFrame = true;
                scissorLogicalRect(vx, pillY, valMax, pillH);
                drawText(val.c_str(), vx - mMinimaMarquee,         ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);
                drawText(val.c_str(), vx - mMinimaMarquee + loopW, ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);
                glDisable(GL_SCISSOR_TEST);
            } else {
                drawText(val.c_str(), vx, ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);
            }
            const float lblMax = (vx - 12.0f * sc) - pillLblX;
            float lfs = fs; if (tw > lblMax && lblMax > 0.0f) lfs *= lblMax / tw;
            drawText(lbl.c_str(), pillLblX, ty, lfs, 0.0f, 0.0f, 0.0f, 1.0f);
        } else if (tw <= maxTextW || maxTextW <= 0.0f) {
            const float pillW = fminf((isCkSel ? ckShiftW : 0.0f) + tw + btnPad * 2.0f, maxPillW);
            drawRoundedRect(lx, pillY, pillW, pillH, pillH * 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);   // white capsule
            if (isCkSel) drawMinCheck(lx + btnPad, ckYsel, 0.0f, 0.0f, 0.0f, 1.0f, checks[si] == 1);
            drawText(lbl.c_str(), pillLblX, ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);   // COLOR_LIST_TEXT_SELECTED black
        } else {
            drawRoundedRect(lx, pillY, maxPillW, pillH, pillH * 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);   // full-width pill
            if (isCkSel) drawMinCheck(lx + btnPad, ckYsel, 0.0f, 0.0f, 0.0f, 1.0f, checks[si] == 1);
            const float gap = 30.0f * sc, loopW = tw + gap;
            if ((int64_t)uptimeMillis() - mMinimaMarqueeStart > 700) {            // ~0.7s read pause, then scroll
                mMinimaMarquee += 2.0f * sc * fmaxf(0.0f, fminf(3.0f, mFrameDt * 60.0f));   // 2px per 1/60s
                if (mMinimaMarquee >= loopW) mMinimaMarquee -= loopW;
            }
            mDisplayDirty = true; mMinimaWantsFrame = true;   // long label: keeps scrolling, never idle
            const float clipX = pillLblX, tx0 = clipX - mMinimaMarquee;
            scissorLogicalRect(clipX, pillY, maxTextW, pillH);                    // clip to the pill's text area
            drawText(lbl.c_str(), tx0,         ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);
            drawText(lbl.c_str(), tx0 + loopW, ty, fs, 0.0f, 0.0f, 0.0f, 1.0f);   // wrap copy for a seamless loop
            glDisable(GL_SCISSOR_TEST);
        }
    }

    // ---- status pill (top-right): real Wi-Fi / Bluetooth / battery icons + clock, in the accent pill ----
    minimaStatusPill(rx, ry, rw, rh, true);

    // ---- focused game boxart, bottom-right, above the A Open pill ----
    if (minSe && !minSe->box.empty() && (scraperBoxartEnabled() || romUsesCartArt(focusedRomPath()))) {
        float bar = 0.7f;
        GLuint bt = romBoxartTex(focusedRomPath(), &bar);
        if (bt) {
            float bh = rh * 0.32f, bw = bh * (bar > 0.01f ? bar : 0.7f);
            const float maxbw = rw * 0.30f;
            if (bw > maxbw) { bw = maxbw; bh = bw / (bar > 0.01f ? bar : 0.7f); }
            if (romBoxartIsPixel(focusedRomPath())) {   // PICO-8 cart label: square pixels
                const float maxSide = fminf(rh * 0.32f, maxbw);
                bw = bh = cartPixelSize(maxSide, maxSide);
            }
            float bx = rx + rw - pad - bw, by = hintTop - btnMg - bh;
            drawRoundedRect(bx - 4.0f * sc, by - 4.0f * sc, bw + 8.0f * sc, bh + 8.0f * sc, 6.0f * sc, 1.0f, 1.0f, 1.0f, 0.12f);
            drawIconTex(bt, bx, by, bw, bh, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    } else if (!inGameScrim) {
        // ---- focused APP logo, bottom-right (like boxart) - the Applications list ----
        const Ps3Item* focItem = nullptr;
        if (!mNdsAtRoot) {
            if (!mPs3Stack.empty()) {
                const auto& its = mPs3Stack.back().items; int s = mPs3Stack.back().sel;
                if (s >= 0 && s < (int)its.size()) focItem = &its[s];
            } else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) {
                const auto& its = mPs3Cats[mPs3CatIdx].items;
                if (mPs3ItemIdx >= 0 && mPs3ItemIdx < (int)its.size()) focItem = &its[mPs3ItemIdx];
            }
        }
        if (focItem && focItem->kind == PS3_APP && focItem->iconTex) {
            float isz = rh * 0.28f; const float maxsz = rw * 0.28f; if (isz > maxsz) isz = maxsz;
            float bx = rx + rw - pad - isz, by = hintTop - btnMg - isz;   // square app icon
            drawRoundedRect(bx - 4.0f * sc, by - 4.0f * sc, isz + 8.0f * sc, isz + 8.0f * sc, 8.0f * sc, 1.0f, 1.0f, 1.0f, 0.12f);
            drawIconTex(focItem->iconTex, bx, by, isz, isz, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    // ---- bottom legend: a button glyph + label, matching NextUI. B Back (left), A Open (right), and
    // a centred Y Info between them for a focused game that has scraped info. ----
    {
        const float ph = rowH, py = hintTop, gap = 5.0f * sc;
        // The label text scales with the user font size (drawText folds in the font scale),
        // so grow the button glyph to match, capped so it still fits inside the pill height.
        const float fg = (mUserFontScale > 0.05f) ? mUserFontScale : 1.0f;
        const float glyphR = fminf(MIN_FONT_S * sc * 0.70f * fg, ph * 0.40f), lw = fmaxf(1.5f, 2.0f * sc);
        auto pillW = [&](const char* label) { return glyphR * 2.0f + gap + measureText(trDyn(label), fsHint) + btnPad * 2.0f; };
        // The centre pill fits between Back and Open: its label shrinks when a translation is long.
        const float centreMaxW = rw - pad * 2.0f - 2.0f * fmaxf(pillW("Back"), pillW("Open")) - btnPad * 2.0f;
        auto drawLegend = [&](int role, const char* label, int align, uint16_t code) {   // align: 0 left, 1 right, 2 centre
            const char* lbl = trDyn(label);
            float lfs = fsHint;
            float lblW = measureText(lbl, lfs);
            const float fixedW = glyphR * 2.0f + gap + btnPad * 2.0f;
            if (align == 2 && fixedW + lblW > centreMaxW && lblW > 0.0f) {
                lfs *= fmaxf(0.5f, (centreMaxW - fixedW) / lblW);
                lblW = measureText(lbl, lfs);
            }
            float pw = fixedW + lblW;
            float px = (align == 1) ? (rx + rw - pad - pw)
                     : (align == 2) ? (rx + rw * 0.5f - pw * 0.5f)
                                    : (rx + pad);
            drawRoundedRect(px, py, pw, ph, ph * 0.5f, ar, ag, ab, 1.0f);
            if (touchList) touchButtonZone(code, {px, py, pw, ph});
            float gcx = px + btnPad + glyphR, gcy = py + ph * 0.5f;
            drawFaceGlyph(role, gcx, gcy, glyphR, lw, 1.0f, atc, atc, atc);   // A=0, B=1, Y=2 - contrast the accent pill, like the label
            // Centre the label on the pill using its rendered height (which includes the font scale).
            drawText(lbl, gcx + glyphR + gap, py + (ph - MIN_FONT_S * sc * fg * (lfs / fsHint)) * 0.5f, lfs, atc, atc, atc, 1.0f);
        };
        drawLegend(0, "Open", 1, BTN_SOUTH);    // A Open (right)
        drawLegend(1, "Back", 0, BTN_EAST);     // B Back (left)
        // Centre hint: "Y Info" for a focused game with scraped art, or "Y Pin" for a focused app
        // (mirrors the Y-to-pin shortcut so Minima advertises the affordance). Both are BTN_WEST.
        if (minSe) drawLegend(2, "Info", 2, BTN_WEST);
        else if (minFocApp) drawLegend(2, isAppPinned(minFocPkg) ? "Unpin" : "Pin", 2, BTN_WEST);
        else if (const char* ya = menuYAction()) drawLegend(2, ya, 2, BTN_WEST);   // Remove / Restore on a folder or system row
    }

    // ---- level-change black-wash crossfade (paired with the horizontal slide above) ----
    if (mMinimaTransStart > 0) {
        float a = 1.0f - (float)((int64_t)uptimeMillis() - mMinimaTransStart) / 150.0f;
        if (a > 0.0f) { drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, a); mDisplayDirty = true; mMinimaWantsFrame = true; }
        else mMinimaTransStart = 0;
    }
    // Sort / Group / Folder-view change banner (Y), drawn on top so it shows in the Minima theme too
    // (the XMB path draws it in renderPs3Xmb; the DSi path in renderNdsCarousel).
    drawPhotoBanner();
    mTextOutlineMode = minPrevOutline;
}

// RG DS bottom panel. The interactive list lives on the primary (top) screen; the secondary panel
// echoes the context in Minima's language (like the NDS top screen): the current category/level as an
// accent header, the focused item's BOXART (async/cached) - or its icon - centred, and the item label
// below, over the Minima backdrop (bottom wallpaper if set, else pure black).
void NanoMenu::renderMinimaSecondary(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;   // flat text: no drop shadow
    const bool inGameScrim = overAppScrim();
    bool drew = false;
    float bgR, bgG, bgB;
    if (wallpaperActive(1)) { drawWallpaperFill(1); drew = true; }   // applies the adjustable Wallpaper Dimming scrim
    if (drew)                        { /* dimming handled inside drawWallpaperFill */ }
    else if (inGameScrim)            { /* leave the app-dim scrim clear; the category chrome draws over it */ }
    else if (minimaSolidBg(&bgR, &bgG, &bgB)) drawQuad(rx, ry, rw, rh, bgR, bgG, bgB, 1.0f);   // solid backdrop colour
    else                             drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 1.0f);    // pure-black backdrop (no app behind)

    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float sc = fminf(rw, rh) / MIN_REF_H;
    const float cx = rx + rw * 0.5f;

    // Resolve the current level's title and the focused item, like the NDS top screen.
    std::string head, sub; const Ps3Item* selItem = nullptr;
    if (mNdsAtRoot) {
        head = "Thimsy";
        if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) sub = mPs3Cats[mPs3CatIdx].name;
    } else if (!mPs3Stack.empty()) {
        head = mPs3Stack.back().title;
        const auto& its = mPs3Stack.back().items; int s = mPs3Stack.back().sel;
        if (s >= 0 && s < (int)its.size()) { sub = its[s].label; selItem = &its[s]; }
    } else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) {
        head = mPs3Cats[mPs3CatIdx].name;
        const auto& its = mPs3Cats[mPs3CatIdx].items; int s = mPs3ItemIdx;
        if (s >= 0 && s < (int)its.size()) { sub = its[s].label; selItem = &its[s]; }
    }
    if (head.empty()) head = "Thimsy";
    head = std::string(trDyn(head.c_str()));
    sub = std::string(trDyn(sub.c_str()));

    // Boxart for the focused game (async / cached, exactly like the XMB column + NDS top screen),
    // else fall back to the item's own icon (or the category icon at root).
    std::string romPath;
    if (selItem) {
        if (selItem->kind == PS3_ROM && selItem->a >= 0 && selItem->a < (int)mXmbSystems.size()
            && selItem->b >= 0 && selItem->b < (int)mXmbSystems[selItem->a].roms.size())
            romPath = mXmbSystems[selItem->a].roms[selItem->b];
        else if (selItem->kind == PS3_RECENT && selItem->a >= 0 && selItem->a < (int)mXmbRecent.size())
            romPath = mXmbRecent[selItem->a].romPath;
    }
    GLuint boxTex = 0; float boxAR = 1.0f;
    if (!romPath.empty() && (scraperBoxartEnabled() || romUsesCartArt(romPath)))
        boxTex = romBoxartTex(romPath, &boxAR);
    GLuint iconTex = selItem ? selItem->iconTex
                   : (mNdsAtRoot && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size() ? mPs3Cats[mPs3CatIdx].iconTex : 0);

    // category header (accent, top)
    {
        float fsH = (22.0f * sc) / (float)FONT_CHAR_H;
        float tw = measureText(head.c_str(), fsH);
        drawText(head.c_str(), cx - tw * 0.5f, ry + rh * 0.08f, fsH, ar, ag, ab, 1.0f);
    }
    // boxart (or the item/category icon) centred
    {
        float artMaxH = rh * 0.50f, artMaxW = rw * 0.70f;
        if (boxTex) {
            float ah = artMaxH, aw = artMaxH * boxAR;
            if (aw > artMaxW) { aw = artMaxW; ah = aw / (boxAR > 0.01f ? boxAR : 1.0f); }
            if (romBoxartIsPixel(romPath)) {   // PICO-8 cart label: square pixels
                const float maxSide = fminf(artMaxH, artMaxW);
                aw = ah = cartPixelSize(maxSide, maxSide);
            }
            float axx = cx - aw * 0.5f, ayy = ry + rh * 0.22f;
            drawRoundedRect(axx - 4.0f * sc, ayy - 4.0f * sc, aw + 8.0f * sc, ah + 8.0f * sc, 8.0f * sc, 1.0f, 1.0f, 1.0f, 0.10f);
            drawIconTex(boxTex, axx, ayy, aw, ah, 1.0f, 1.0f, 1.0f, 1.0f);
        } else if (iconTex) {
            float isz = rh * 0.34f;
            // Icons that opt into their own flat colour (the Favourites loveheart) keep it; the rest
            // draw as the plain silhouette/art in white.
            float tr = 1.0f, tg = 1.0f, tb = 1.0f;
            if (selItem && selItem->flatOwnTint) { tr = selItem->iconR; tg = selItem->iconG; tb = selItem->iconB; }
            drawIconTex(iconTex, cx - isz * 0.5f, ry + rh * 0.26f, isz, isz, tr, tg, tb, 1.0f);
        }
    }
    // focused item label (white, bottom)
    if (!sub.empty()) {
        float fsS = (20.0f * sc) / (float)FONT_CHAR_H;
        float tw = measureText(sub.c_str(), fsS);
        float maxW = rw * 0.90f;
        if (tw > maxW && maxW > 0.0f) { fsS *= maxW / tw; tw = measureText(sub.c_str(), fsS); }
        drawText(sub.c_str(), cx - tw * 0.5f, ry + rh * 0.82f, fsS, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    // game-system rows: echo the On/Off (or field value) in the accent colour under the label,
    // so the enable state is visible on the bottom screen too.
    if (selItem && (selItem->kind == PS3_GS_SYSTEM_ROW || selItem->kind == PS3_GS_FIELD
                    || selItem->kind == PS3_CATORDER_ROW || selItem->kind == PS3_ITEMHIDE_ROW
                    || selItem->kind == PS3_ST_ROW)
        && !selItem->value.empty()) {
        float fsV = (18.0f * sc) / (float)FONT_CHAR_H;
        const char* val = trDyn(selItem->value.c_str());
        float vw = measureText(val, fsV);
        drawText(val, cx - vw * 0.5f, ry + rh * 0.90f, fsV, ar, ag, ab, 1.0f);
    }
    mTextOutlineMode = minPrevOutline;
}

// Option menu / list+slider chooser in Minima's language: a dark right-anchored panel that slides
// in, with an accent left edge + title, the options as white rows (selected in a white capsule), or a
// numeric slider. Reuses the shared option/dialog animation members so opening/closing clears cleanly.
void NanoMenu::renderMinimaSidePanel(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;
    float dt = mFrameDt; if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    const bool optSrc = (mPs3OptActive || mPs3OptClosing);
    float ap;
    if (optSrc) {
        if (mPs3OptActive) { mPs3OptClosing = false; mPs3OptAnim += (1.0f - mPs3OptAnim) * (1.0f - expf(-14.0f * dt)); if (mPs3OptAnim > 0.999f) mPs3OptAnim = 1.0f; ap = mPs3OptAnim; }
        else { mPs3OptCloseAnim -= mPs3OptCloseAnim * (1.0f - expf(-14.0f * dt)); if (mPs3OptCloseAnim < 0.02f) { mPs3OptCloseAnim = 0.0f; mPs3OptClosing = false; mTextOutlineMode = minPrevOutline; return; } ap = mPs3OptCloseAnim; }
    } else {
        if (mPs3DlgActive) { mPs3DlgClosing = false; mPs3DlgAnim += (1.0f - mPs3DlgAnim) * (1.0f - expf(-14.0f * dt)); if (mPs3DlgAnim > 0.999f) mPs3DlgAnim = 1.0f; ap = mPs3DlgAnim; }
        else { mPs3DlgCloseAnim -= mPs3DlgCloseAnim * (1.0f - expf(-14.0f * dt)); if (mPs3DlgCloseAnim < 0.02f) { mPs3DlgCloseAnim = 0.0f; mPs3DlgClosing = false; mTextOutlineMode = minPrevOutline; return; } ap = mPs3DlgCloseAnim; }
    }
    if (ap < 0.999f) mDisplayDirty = true;

    std::vector<std::string> rows; std::string title; int sel = 0; bool slider = false;
    std::vector<int> realIdx;   // a row's index in its source list (option rows skip the separators)
    const bool subOpen = optSrc && minimaOptSubOpen();
    if (optSrc) {
        if (subOpen) {
            for (const auto& sr : mPs3OptSubRows[mPs3OptSel]) { realIdx.push_back((int)rows.size()); rows.push_back(trDyn(sr.label.c_str())); }
            sel = mPs3OptSubSel;
            title = (mPs3OptSel < (int)mPs3OptLabels.size()) ? trDyn(mPs3OptLabels[mPs3OptSel].c_str()) : trDyn("Options");
        } else {
            int nn = (int)mPs3OptLabels.size();
            for (int i = 0; i < nn; i++) { if (i < (int)mPs3OptSep.size() && mPs3OptSep[i]) continue; if (i == mPs3OptSel) sel = (int)rows.size(); realIdx.push_back(i); rows.push_back(trDyn(mPs3OptLabels[i].c_str())); }
            title = mPs3OptCtxLabel.empty() ? trDyn("Options") : trDyn(mPs3OptCtxLabel.c_str());
        }
    } else {
        title = mPs3DlgTitle.empty() ? trDyn("Options") : trDyn(mPs3DlgTitle.c_str());
        slider = mPs3DlgSlider && mPs3DlgOptions.empty();
        for (const auto& o : mPs3DlgOptions) { realIdx.push_back((int)rows.size()); rows.push_back(trDyn(o.c_str())); }
        sel = mPs3DlgSel;
    }
    int n = (int)rows.size(); if (sel < 0) sel = 0; if (n > 0 && sel >= n) sel = n - 1;

    drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 0.55f * ap);          // scrim over the home
    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float sc = fminf(rw, rh) / MIN_REF_H, pad = MIN_PAD * sc, rowH = MIN_PILL * sc, btnPad = MIN_BTNPAD * sc;
    const float inset = 8.0f * sc;
    // Scale the option-list row pitch + pill with the user font size (matches the home list) so the
    // options are not crowded at a large font. fg 1.0 = unchanged.
    const float lfg = (mUserFontScale > 0.05f) ? mUserFontScale : 1.0f;
    const float listRowH = rowH * lfg;
    float panelW = fminf(rw * 0.60f, rw - pad * 2.0f);
    float px = rx + rw - panelW + (1.0f - ap) * panelW;             // slide in from the right
    drawQuad(px, ry, panelW, rh, 0.05f, 0.05f, 0.06f, 0.98f * ap);  // dark panel
    drawQuad(px, ry, fmaxf(2.0f, 3.0f * sc), rh, ar, ag, ab, ap);   // accent left edge
    drawText(title.c_str(), px + pad + inset, ry + pad, (18.0f * sc) / (float)FONT_CHAR_H, ar, ag, ab, ap);
    float listLeft = px + pad + inset;
    float contentTop = ry + pad + listRowH * 0.9f + 6.0f * sc;
    // Touch: a tap on the scrim beside the panel goes back, as B does; the panel itself is opaque.
    const bool touchPanel = mRenderingPanel == 0 && (optSrc ? mPs3OptActive : mPs3DlgActive)
                            && !(mLaunchFadeStart > 0 && !mOverlayMode);
    if (touchPanel) {
        mTouch.block({rx, ry, rw, rh}, [this, optSrc]() {
            if (!(optSrc ? mPs3OptActive : mPs3DlgActive)) return;
            ps3XmbBack();
            mDisplayDirty = true;
        });
        mTouch.block({px, ry, panelW, rh});
    }

    if (slider) {
        float mn = mPs3DlgSldMin, mx = mPs3DlgSldMax, v = mPs3DlgSldVal;
        float t = (mx > mn) ? (v - mn) / (mx - mn) : 0.0f; if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        char buf[32];
        if (mPs3DlgSldScale <= 0) snprintf(buf, sizeof(buf), "%d", (int)lroundf(v));
        else snprintf(buf, sizeof(buf), "%.*f", mPs3DlgSldScale, v);
        float fsV = (34.0f * sc) / (float)FONT_CHAR_H, tw = measureText(buf, fsV);
        drawText(buf, px + panelW * 0.5f - tw * 0.5f, ry + rh * 0.38f, fsV, 1.0f, 1.0f, 1.0f, ap);
        float bx = listLeft, bw = panelW - 2.0f * (pad + inset), by = ry + rh * 0.56f, bh = 8.0f * sc;
        drawRoundedRect(bx, by, bw, bh, bh * 0.5f, 0.28f, 0.28f, 0.30f, ap);
        drawRoundedRect(bx, by, bw * t, bh, bh * 0.5f, ar, ag, ab, ap);
        const float thumbX = bx + bw * t;
        drawRoundedRect(thumbX - 5.0f * sc, by - 5.0f * sc, 10.0f * sc, bh + 10.0f * sc, 5.0f * sc, 1.0f, 1.0f, 1.0f, ap);
        // Touch: a drag along the track moves the thumb with the finger; a tap either side of the
        // thumb steps the value once that way.
        const float range = mx - mn, pxPerStep = (range > 0.0f && mPs3DlgSldStep > 0.0f) ? bw * mPs3DlgSldStep / range : 0.0f;
        if (touchPanel && pxPerStep > 0.0f) {
            const TouchRect band{px, by - 22.0f * sc, panelW, 52.0f * sc};
            const int k = (int)ceilf(3.0f / pxPerStep);   // keep the pitch at 3 px or more
            mTouch.scroll(band, TouchZones::kAcross, pxPerStep * (float)k,
                          [this, k](int dir) { return mTouch.tracking() && minimaSliderStep(-dir * k); });
            mTouch.tap({band.x, band.y, thumbX - band.x, band.h}, [this]() { minimaSliderStep(-1); });
            mTouch.tap({thumbX, band.y, band.x + band.w - thumbX, band.h}, [this]() { minimaSliderStep(+1); });
        }
    } else if (n > 0) {
        float listBot = ry + rh - pad;
        int visRows = (int)fmaxf(1.0f, floorf((listBot - contentTop) / listRowH));
        int top = sel - visRows / 2; if (top > n - visRows) top = n - visRows; if (top < 0) top = 0;
        float textMaxW = px + panelW - pad - listLeft - btnPad;
        if (touchPanel)
            mTouch.scroll({px, contentTop, panelW, (float)visRows * listRowH}, TouchZones::kUpDown, listRowH,
                          [this, optSrc, subOpen](int dir) { return minimaPanelStep(optSrc, subOpen, dir); });
        for (int i = top; i < n && i < top + visRows; i++) {
            float rowY = contentTop + (float)(i - top) * listRowH;
            if (touchPanel)
                mTouch.tap({px, rowY, panelW, listRowH}, [this, optSrc, subOpen, ri = realIdx[i]]() {
                    if (!(optSrc ? mPs3OptActive : mPs3DlgActive) || (optSrc && minimaOptSubOpen() != subOpen)) return;
                    if (optSrc && !subOpen) {
                        if (ri >= (int)mPs3OptLabels.size()) return;
                        mPs3OptSel = ri; xmbOptEnter();
                    } else if (optSrc) {
                        if (ri >= (int)mPs3OptSubRows[mPs3OptSel].size()) return;
                        mPs3OptSubSel = ri; xmbOptEnter();
                    } else {
                        if (ri >= (int)mPs3DlgOptions.size()) return;
                        mPs3DlgSel = ri; ps3XmbSelect();
                    }
                    mDisplayDirty = true;
                });
            float ty = rowY + (listRowH - MIN_FONT * sc * lfg) * 0.5f;
            const GLuint rowIcon = ps3DlgRowIcon(optSrc ? -1 : i);
            const float iconW = rowIcon ? MIN_FONT * sc * 1.2f : 0.0f, iconGap = rowIcon ? MIN_FONT * sc * 0.4f : 0.0f;
            float fs = (MIN_FONT * sc) / (float)FONT_CHAR_H, tw = measureText(rows[i].c_str(), fs);
            const float maxW = textMaxW - iconW - iconGap;
            if (tw > maxW && maxW > 0.0f) { fs *= maxW / tw; tw = measureText(rows[i].c_str(), fs); }
            const float c = (i == sel) ? 0.0f : 1.0f;
            if (i == sel) {
                float pillH = listRowH * 0.86f, pillW = fminf(tw + iconW + iconGap + btnPad * 2.0f, px + panelW - pad - listLeft);
                drawRoundedRect(listLeft, rowY + listRowH * 0.07f, pillW, pillH, pillH * 0.5f, 1.0f, 1.0f, 1.0f, ap);
            }
            if (rowIcon) drawIconTex(rowIcon, listLeft + btnPad, rowY + (listRowH - iconW) * 0.5f, iconW, iconW, c, c, c, ap);
            drawText(rows[i].c_str(), listLeft + btnPad + iconW + iconGap, ty, fs, c, c, c, ap);
        }
    }
    mTextOutlineMode = minPrevOutline;
}

// Confirm / message dialog in Minima's language: a centred dark rounded panel with an accent top
// rule, the title in the accent, a word-wrapped body, and the options (Yes/No/OK) as pills along the
// bottom (the selected one filled with the accent).
void NanoMenu::renderMinimaDialog(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;
    float dt = mFrameDt; if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    mPs3DlgClosing = false;
    mPs3DlgAnim += (1.0f - mPs3DlgAnim) * (1.0f - expf(-15.0f * dt));
    if (mPs3DlgAnim > 0.999f) mPs3DlgAnim = 1.0f; else mDisplayDirty = true;
    float ap = mPs3DlgAnim, ease = ap * ap * (3.0f - 2.0f * ap);
    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float sc = fminf(rw, rh) / MIN_REF_H, pad = MIN_PAD * sc, rowH = MIN_PILL * sc, btnPad = MIN_BTNPAD * sc;

    drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 0.55f * ap);   // scrim
    float pw = fminf(rw * 0.74f, rw - pad * 2.0f);
    // Option pills (Yes / No / OK): one row when they fit the panel, with tighter padding and the
    // labels shrunk to as little as 75% if needed; else a column of full-width pills in a taller
    // panel. Laid out before the body, which ends above them.
    const int n = (int)mPs3DlgOptions.size();
    const float optMaxW = pw - pad * 2.0f, optGap = pad, optFs0 = (MIN_FONT_S * sc) / (float)FONT_CHAR_H;
    std::vector<float> optTw(n);
    float optTextW = 0.0f;
    for (int i = 0; i < n; i++) { optTw[i] = measureText(trDyn(mPs3DlgOptions[i].c_str()), optFs0); optTextW += optTw[i]; }
    float optScale = 1.0f, optPad = btnPad;
    bool optStack = false;
    if (n > 0 && optTextW + optGap * (float)(n - 1) + btnPad * 2.0f * (float)n > optMaxW) {
        optPad = btnPad * 0.5f;
        const float room = optMaxW - optGap * (float)(n - 1) - optPad * 2.0f * (float)n;
        optScale = (optTextW > 0.0f) ? fmaxf(0.0f, fminf(1.0f, room / optTextW)) : 1.0f;
        if (n > 1 && optScale < 0.75f) { optStack = true; optScale = 1.0f; optPad = btnPad; }
    }
    const float optPillH = optStack ? rowH * 0.8f : rowH, optVGap = pad * 0.5f;
    const float optBlockH = optStack ? optPillH * (float)n + optVGap * (float)(n - 1) : rowH;
    float ph = fminf(rh * (optStack ? 0.84f : 0.64f), rh - pad * 2.0f);
    float pxL = rx + (rw - pw) * 0.5f;
    float pyTgt = ry + (rh - ph) * 0.5f, pyOff = pyTgt + rh * 0.12f;
    float pyTop = pyOff + (pyTgt - pyOff) * ease;
    drawRoundedRect(pxL, pyTop + 4.0f * sc, pw, ph, 12.0f * sc, 0.0f, 0.0f, 0.0f, 0.5f * ap);   // shadow
    drawRoundedRect(pxL, pyTop, pw, ph, 12.0f * sc, 0.07f, 0.07f, 0.08f, ap);                   // body
    drawRoundedRect(pxL, pyTop, pw, fmaxf(2.0f, 4.0f * sc), 2.0f * sc, ar, ag, ab, ap);          // accent top rule
    // Touch: a tap on the scrim cancels; on a message (one option or none), a tap on the panel is OK.
    const bool touchDlg = mRenderingPanel == 0 && mPs3DlgActive && !(mLaunchFadeStart > 0 && !mOverlayMode);
    if (touchDlg) {
        mTouch.block({rx, ry, rw, rh}, [this]() {
            if (!mPs3DlgActive) return;
            ps3XmbBack();
            mDisplayDirty = true;
        });
        mTouch.block({pxL, pyTop, pw, ph}, [this, msg = mPs3DlgOptions.size() <= 1]() {
            if (!mPs3DlgActive || !msg || mPs3DlgOptions.size() > 1) return;
            mPs3DlgSel = 0;
            ps3XmbSelect();
            mDisplayDirty = true;
        });
    }
    float cxC = rx + rw * 0.5f;
    float yy = pyTop + pad + 6.0f * sc;
    if (!mPs3DlgTitle.empty()) {
        const char* t = trDyn(mPs3DlgTitle.c_str());
        float fs = (20.0f * sc) / (float)FONT_CHAR_H, tw = measureText(t, fs);
        const float maxTw = pw - pad * 2.0f;
        if (tw > maxTw) { fs *= maxTw / tw; tw = measureText(t, fs); }   // shrink a long title to the panel
        drawText(t, cxC - tw * 0.5f, yy, fs, ar, ag, ab, ap);
        yy += rowH * 1.05f;
    }
    // Word-wrapped body. A short confirm paragraph stays centred; a long info body (single OK -
    // Settings and Connection Status List, Internet Connection Test, System Information) PAGINATES
    // with L/R instead of being truncated, matching the DSi dialog. Honour embedded '\n' line breaks
    // so the key:value status lines are not mangled into one run (the old wrap split only on spaces).
    const bool infoStyle = ((int)mPs3DlgOptions.size() <= 1);
    mNdsInfoPageCount = 1;   // stays 1 unless an info body overflows one panel (below)
    if (!mPs3DlgBody.empty()) {
        std::string body = trDyn(mPs3DlgBody.c_str());
        float fs = (15.0f * sc) / (float)FONT_CHAR_H;
        float maxW = pw - pad * 2.0f, lineH = 18.0f * sc;
        float btnTop = pyTop + ph - pad - optBlockH;   // the OK / option pills
        float bodyTop = yy, bodyBot = btnTop - pad * 0.6f;
        // Wrap into lines, breaking on BOTH spaces and newlines.
        std::vector<std::string> lines;
        auto wrapBody = [&]() {
            lines.clear();
            std::string cur; size_t i = 0;
            auto flush = [&]() { lines.push_back(cur); cur.clear(); };
            while (i < body.size()) {
                size_t sp = body.find_first_of(" \n", i);
                std::string word = body.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                bool nl = (sp != std::string::npos && body[sp] == '\n');
                std::string trial = cur.empty() ? word : cur + " " + word;
                if (measureText(trial.c_str(), fs) > maxW && !cur.empty()) { flush(); cur = word; }
                else cur = trial;
                if (nl) flush();
                i = (sp == std::string::npos) ? body.size() : sp + 1;
            }
            if (!cur.empty()) flush();
        };
        // A question (more than one option) cannot page, so a body too long for the panel (a small or
        // square screen, a long translation) gets smaller text rather than losing its end.
        if (infoStyle) wrapBody();
        else lines = wrapTextToFit(body, fs, lineH, maxW, bodyBot - bodyTop, fmaxf(fs * 0.5f, 12.0f / (float)FONT_CHAR_H));
        const int total = (int)lines.size();
        int maxVis = (int)((bodyBot - bodyTop) / lineH); if (maxVis < 1) maxVis = 1;
        if (infoStyle && total > maxVis) {
            int visRows = maxVis - 1; if (visRows < 1) visRows = 1;   // reserve a row for the pager
            int pages = (total + visRows - 1) / visRows; mNdsInfoPageCount = pages;
            if (mNdsInfoPage < 0) mNdsInfoPage = 0;
            if (mNdsInfoPage > pages - 1) mNdsInfoPage = pages - 1;
            int first = mNdsInfoPage * visRows, last = first + visRows; if (last > total) last = total;
            float ty = bodyTop;
            for (int k = first; k < last; k++) {
                float tw = measureText(lines[k].c_str(), fs);
                drawText(lines[k].c_str(), cxC - tw * 0.5f, ty, fs, 0.90f, 0.90f, 0.92f, ap);
                ty += lineH;
            }
            // Pager row: "n / m" centred, L/R pills at the body edges (drives mNdsInfoPage via the
            // shared ndsDlgInfoPaged() L/R input path).
            float pf = (12.0f * sc) / (float)FONT_CHAR_H;
            float pgy = bodyTop + (float)visRows * lineH + 1.0f * sc;
            char pgs[24]; snprintf(pgs, sizeof(pgs), "%d / %d", mNdsInfoPage + 1, pages);
            float pgw = measureText(pgs, pf);
            drawText(pgs, cxC - pgw * 0.5f, pgy, pf, ar, ag, ab, 0.9f * ap);
            const float phh = 15.0f * sc, pwl = 22.0f * sc;
            auto pill = [&](float pxx, const char* g, bool on) {
                drawRoundedRect(pxx, pgy - 2.0f * sc, pwl, phh, 5.0f * sc,
                                on ? ar : 0.16f, on ? ag : 0.16f, on ? ab : 0.18f, (on ? 1.0f : 0.4f) * ap);
                float gw = measureText(g, pf);
                drawText(g, pxx + (pwl - gw) * 0.5f, pgy, pf, 1.0f, 1.0f, 1.0f, (on ? 1.0f : 0.4f) * ap);
            };
            pill(pxL + pad,            "L", mNdsInfoPage > 0);
            pill(pxL + pw - pad - pwl, "R", mNdsInfoPage < pages - 1);
            if (touchDlg) {   // L/R, and a swipe across the panel turns the page as they do
                mTouch.swipe({pxL, pyTop, pw, ph}, [this](int axis, int dir) {
                    if (axis == TouchZones::kAcross) touchPressButton(dir > 0 ? BTN_TL : BTN_TR);
                });
                touchButtonZone(BTN_TL, {pxL + pad, pgy - 2.0f * sc, pwl, phh});
                touchButtonZone(BTN_TR, {pxL + pw - pad - pwl, pgy - 2.0f * sc, pwl, phh});
            }
        } else {
            float ty = bodyTop;
            for (int k = 0; k < total && ty < bodyBot; k++) {
                float tw = measureText(lines[k].c_str(), fs);
                drawText(lines[k].c_str(), cxC - tw * 0.5f, ty, fs, 0.90f, 0.90f, 0.92f, ap);
                ty += lineH;
            }
        }
    }
    // option pills (laid out above), selected filled with the accent
    int sel = mPs3DlgSel; if (sel < 0) sel = 0; if (n > 0 && sel >= n) sel = n - 1;
    if (n > 0) {
        const float optTop = pyTop + ph - pad - optBlockH;
        float xx = optStack ? pxL + pad
                            : cxC - (optTextW * optScale + optPad * 2.0f * (float)n + optGap * (float)(n - 1)) * 0.5f;
        for (int i = 0; i < n; i++) {
            const char* o = trDyn(mPs3DlgOptions[i].c_str());
            bool s = (i == sel);
            const float f = !optStack ? optScale
                          : (optTw[i] > optMaxW - optPad * 2.0f ? (optMaxW - optPad * 2.0f) / optTw[i] : 1.0f);
            const float w = optStack ? optMaxW : optTw[i] * optScale + optPad * 2.0f;
            const float py = optStack ? optTop + (float)i * (optPillH + optVGap) : optTop;
            drawRoundedRect(xx, py, w, optPillH, optPillH * 0.5f, s ? ar : 0.16f, s ? ag : 0.16f, s ? ab : 0.18f, ap);
            if (touchDlg)
                mTouch.press({xx, py, w, optPillH}, [this, i, n]() {
                    if (!mPs3DlgActive || (int)mPs3DlgOptions.size() != n) return;
                    mPs3DlgSel = i;
                    ps3XmbSelect();
                    mDisplayDirty = true;
                });
            const float tw = optTw[i] * f, ty = py + (optPillH - MIN_FONT_S * sc * f) * 0.5f;
            drawText(o, xx + (w - tw) * 0.5f, ty, optFs0 * f, 1.0f, 1.0f, 1.0f, ap);
            if (!optStack) xx += w + optGap;
        }
    }
    mTextOutlineMode = minPrevOutline;
}

// ---- Minima touch (zones registered by the painters above) ----

// The home list's current level: its row count, and its live selection in *sel (null when none).
int NanoMenu::minimaListLevel(int** sel) {
    *sel = nullptr;
    if (mNdsAtRoot) { *sel = &mPs3CatIdx; return (int)mPs3Cats.size(); }
    if (!mPs3Stack.empty()) { *sel = &mPs3Stack.back().sel; return (int)mPs3Stack.back().items.size(); }
    if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) { *sel = &mPs3ItemIdx; return (int)mPs3Cats[mPs3CatIdx].items.size(); }
    return 0;
}

// The option panel shows the open submenu's rows rather than its own.
bool NanoMenu::minimaOptSubOpen() const {
    return mPs3OptSubOpen && mPs3OptSel >= 0 && mPs3OptSel < (int)mPs3OptSubRows.size()
        && !mPs3OptSubRows[mPs3OptSel].empty();
}

// One row of a drag over the side panel's list (dir +1 = later rows); false at either end or once
// the panel it was drawn for is gone.
bool NanoMenu::minimaPanelStep(bool optSrc, bool subOpen, int dir) {
    if (!(optSrc ? mPs3OptActive : mPs3DlgActive) || (optSrc && minimaOptSubOpen() != subOpen)) return false;
    if (optSrc && subOpen) {
        const int ns = mPs3OptSubSel + dir;
        if (ns < 0 || ns >= (int)mPs3OptSubRows[mPs3OptSel].size()) return false;
        mPs3OptSubSel = ns;
    } else if (optSrc) {
        int s = mPs3OptSel + dir;
        while (s >= 0 && s < (int)mPs3OptLabels.size() && s < (int)mPs3OptSep.size() && mPs3OptSep[s]) s += dir;
        if (s < 0 || s >= (int)mPs3OptLabels.size()) return false;
        mPs3OptSel = s;
    } else {
        const int ns = mPs3DlgSel + dir;
        if (ns < 0 || ns >= (int)mPs3DlgOptions.size()) return false;
        mPs3DlgSel = ns;
        if (mPs3DlgKind == 1) previewThemeSetting(mPs3DlgThemeKey, mPs3DlgSel);   // the chooser's live preview, as Up/Down
    }
    mDisplayDirty = true;
    return true;
}

// Moves the side panel's slider by `steps` of its step; false when it cannot move that way.
bool NanoMenu::minimaSliderStep(int steps) {
    if (!mPs3DlgActive || !mPs3DlgSlider || !mPs3DlgOptions.empty() || mPs3DlgSldStep <= 0.0f) return false;
    float v = mPs3DlgSldVal + (float)steps * mPs3DlgSldStep;
    v = mPs3DlgSldMin + roundf((v - mPs3DlgSldMin) / mPs3DlgSldStep) * mPs3DlgSldStep;
    if (v < mPs3DlgSldMin) v = mPs3DlgSldMin;
    if (v > mPs3DlgSldMax) v = mPs3DlgSldMax;
    if (v == mPs3DlgSldVal) return false;
    mPs3DlgSldVal = v;
    screenCalSliderPreview();
    if (mShaderParamEdit >= 0 && mShaderParamEdit < (int)mShaderParams.size()) {
        mShaderParams[mShaderParamEdit].cur = v;
        shaderApplyParamLive(mShaderParamEdit);
    }
    mDisplayDirty = true;
    return true;
}

// Game / app INFORMATION page in Minima's language. The shared info dialog carries the scraped
// cover (mPs3DlgBoxTex), fan art (mPs3DlgFanTex), metadata (mPs3RomInfo*) and a paged synopsis
// (mPs3RomInfoSyn, else the plain-facts mPs3DlgBody). Minima's generic dialog only draws a title +
// body, so a rich info page came out almost empty - this renders the full page flat, mirroring the
// DSi info page's content: a dimmed fan-art (screenshot) backdrop, the cover + metadata columns and
// the paged description. Paged with L/R exactly like the DSi page (mNdsInfoPage / mNdsInfoPageCount).
void NanoMenu::renderMinimaInfoPage(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;
    float dt = mFrameDt; if (dt < 0.0f) dt = 0.0f; if (dt > 0.1f) dt = 0.1f;
    mPs3DlgClosing = false;
    mPs3DlgAnim += (1.0f - mPs3DlgAnim) * (1.0f - expf(-15.0f * dt));
    if (mPs3DlgAnim > 0.999f) mPs3DlgAnim = 1.0f; else mDisplayDirty = true;
    const float ap = mPs3DlgAnim;

    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float sc = fminf(rw, rh) / MIN_REF_H, pad = MIN_PAD * sc;
    const float FCH = (float)FONT_CHAR_H;

    // Backdrop: opaque black, with the fan art (a "screenshot"/background) cover-fit dim behind it.
    drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, ap);
    if (mPs3DlgFanTex && mPs3DlgFanW > 0 && mPs3DlgFanH > 0) {
        float far = (float)mPs3DlgFanW / (float)mPs3DlgFanH, panelAr = rw / rh;
        float fw = rw, fh = rh, fx = rx, fy = ry;
        if (far > panelAr) { fw = rh * far; fx = rx - (fw - rw) * 0.5f; }
        else               { fh = rw / far; fy = ry - (fh - rh) * 0.5f; }
        drawIconTex(mPs3DlgFanTex, fx, fy, fw, fh, 1.0f, 1.0f, 1.0f, 0.30f * ap);
        drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, 0.55f * ap);
    }
    // Touch: a tap anywhere is OK (Back when the page has more than one option); a swipe across
    // turns the page as L/R do.
    const bool touchInfo = mRenderingPanel == 0 && mPs3DlgActive && !(mLaunchFadeStart > 0 && !mOverlayMode);
    if (touchInfo) {
        mTouch.block({rx, ry, rw, rh}, [this, msg = mPs3DlgOptions.size() <= 1]() {
            if (!mPs3DlgActive) return;
            if (msg && mPs3DlgOptions.size() <= 1) { mPs3DlgSel = 0; ps3XmbSelect(); }
            else ps3XmbBack();
            mDisplayDirty = true;
        });
        mTouch.swipe({rx, ry, rw, rh}, [this](int axis, int dir) {
            if (axis == TouchZones::kAcross) touchPressButton(dir > 0 ? BTN_TL : BTN_TR);
        });
    }

    // Header: "< Title" in the accent (matches the wallpaper picker chrome).
    const char* chev = "\xE2\x80\xB9";   // U+2039
    float chFs = (24.0f * sc) / FCH, chW = measureText(chev, chFs);
    float hx = rx + pad, hy = ry + rh * 0.05f;
    drawText(chev, hx, hy, chFs, ar, ag, ab, ap);
    std::string title = mPs3DlgTitle.empty() ? std::string(trDyn("Information")) : std::string(trDyn(mPs3DlgTitle.c_str()));
    float titleX = hx + chW + 12.0f * sc, titleFs = (20.0f * sc) / FCH;
    float titleMaxW = (rx + rw - pad) - titleX, tw = measureText(title.c_str(), titleFs);
    if (tw > titleMaxW && titleMaxW > 0.0f) titleFs *= titleMaxW / tw;
    drawText(title.c_str(), titleX, hy, titleFs, ar, ag, ab, ap);
    if (touchInfo)   // the "<" back chevron and its title
        touchButtonZone(BTN_EAST, {hx, hy, titleX - hx + measureText(title.c_str(), titleFs), 24.0f * sc});

    float descTop;
    const float rowH = 15.0f * sc;
    if (mPs3DlgRomInfo) {
        // Cover (left column).
        float colTop = ry + rh * 0.17f;
        float coverW = rw * 0.26f, coverH = coverW * 1.4f;
        if (coverH > rh * 0.40f) { coverH = rh * 0.40f; coverW = coverH / 1.4f; }
        float coverX = rx + pad, coverY = colTop;
        float metaX = coverX + coverW + rw * 0.05f;
        if (mPs3DlgBoxTex && mPs3DlgBoxW > 0 && mPs3DlgBoxH > 0) {
            float car = (float)mPs3DlgBoxW / (float)mPs3DlgBoxH;
            float cw = coverW, ch = cw / car; if (ch > coverH) { ch = coverH; cw = ch * car; }
            float cvx = coverX + (coverW - cw) * 0.5f, cvy = coverY + (coverH - ch) * 0.5f;
            drawRoundedRect(cvx - 2.0f*sc, cvy - 2.0f*sc, cw + 4.0f*sc, ch + 4.0f*sc, 4.0f*sc, 1.0f, 1.0f, 1.0f, 0.9f*ap);
            drawIconTex(mPs3DlgBoxTex, cvx, cvy, cw, ch, 1.0f, 1.0f, 1.0f, ap);
        } else {
            drawRoundedRect(coverX, coverY, coverW, coverH, 4.0f*sc, 0.12f, 0.12f, 0.14f, 0.8f*ap);   // placeholder while art loads
        }
        // Metadata rows (right of the cover).
        float mLabelFs = (11.0f * sc) / FCH, mValFs = (12.0f * sc) / FCH;
        float vy = colTop + 2.0f * sc;
        float valX = metaX + rw * 0.17f, valMaxW = (rx + rw - pad) - valX;
        auto mrow = [&](const char* label, const std::string& val) {
            if (val.empty()) return;
            drawText(label, metaX, vy, mLabelFs, ar, ag, ab, 0.9f * ap);
            float f = mValFs, vw = measureText(val.c_str(), f);
            if (vw > valMaxW && valMaxW > 0.0f) f *= valMaxW / vw;
            drawText(val.c_str(), valX, vy, f, 1.0f, 1.0f, 1.0f, ap);
            vy += rowH;
        };
        mrow(trDyn("Genre"),     mPs3RomInfoGenre);
        mrow(trDyn("Players"),   mPs3RomInfoPlayers);
        mrow(trDyn("Rating"),    mPs3RomInfoRating);
        mrow(trDyn("Released"),  mPs3RomInfoDate);
        mrow(trDyn("Developer"), mPs3RomInfoDev);
        mrow(trDyn("Publisher"), mPs3RomInfoPub);
        mrow(mPs3RomInfoCoreIsApp ? trDyn("App") : trDyn("Core"), mPs3RomInfoCore);
        mrow(trDyn("System"),    mPs3RomInfoSystem);
        descTop = fmaxf(colTop + coverH, vy) + rh * 0.03f;
    } else {
        descTop = ry + rh * 0.18f;   // plain-facts fallback wraps from near the top
    }

    // ---- Description (paged, L/R) ----
    std::string bodyText = mPs3DlgRomInfo ? mPs3RomInfoSyn : std::string(trDyn(mPs3DlgBody.c_str()));
    float bodyFs = (13.0f * sc) / FCH, lineH = 17.0f * sc;
    float bodyLeft = rx + pad, wrapW = rw - pad * 2.0f;
    float capY = descTop, bodyTop = descTop + (mPs3DlgRomInfo ? rowH : 0.0f);
    float bodyBot = ry + rh - rh * 0.10f;
    std::vector<std::string> lines;
    if (!bodyText.empty()) {
        std::string line, word;
        auto commit = [&]() {
            if (word.empty()) return;
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && measureText(trial.c_str(), bodyFs) > wrapW) { lines.push_back(line); line = word; }
            else line = trial;
            word.clear();
        };
        for (const char* q = bodyText.c_str(); ; ++q) {
            if (*q == ' ' || *q == '\n' || *q == '\0') { commit(); if (*q == '\n') lines.push_back(""); if (*q == '\0') break; }
            else word.push_back(*q);
        }
        if (!line.empty()) lines.push_back(line);
    }
    int linesPerPage = (int)((bodyBot - bodyTop) / lineH); if (linesPerPage < 1) linesPerPage = 1;
    int total = (int)lines.size();
    int pages = overlapPageCount(total, linesPerPage);
    mNdsInfoPageCount = pages;
    if (mNdsInfoPage < 0) mNdsInfoPage = 0;
    if (mNdsInfoPage > pages - 1) mNdsInfoPage = pages - 1;
    if (mPs3DlgRomInfo && total > 0)
        drawText(trDyn("Description"), bodyLeft, capY, (11.0f * sc) / FCH, ar, ag, ab, 0.9f * ap);
    float ly = bodyTop;
    int firstL = overlapPageFirst(mNdsInfoPage, linesPerPage), lastL = firstL + linesPerPage; if (lastL > total) lastL = total;
    for (int i = firstL; i < lastL; i++) {
        if (!lines[i].empty()) drawText(lines[i].c_str(), bodyLeft, ly, bodyFs, 0.90f, 0.90f, 0.92f, ap);
        ly += lineH;
    }
    // L/R pager (only with >1 page): centred count + accent corner pills.
    if (pages > 1) {
        float by = ry + rh - rh * 0.06f, pf = (11.0f * sc) / FCH;
        char pg[24]; snprintf(pg, sizeof(pg), "%d / %d", mNdsInfoPage + 1, pages);
        float pgw = measureText(pg, pf);
        drawText(pg, rx + rw * 0.5f - pgw * 0.5f, by, pf, ar, ag, ab, ap);
        auto pill = [&](float pcx, const char* g, bool on, uint16_t code) {
            float gw = measureText(g, pf), pw = gw + MIN_BTNPAD * sc * 2.0f;
            drawRoundedRect(pcx - pw * 0.5f, by - 2.0f * sc, pw, rowH, rowH * 0.5f, ar, ag, ab, (on ? 1.0f : 0.35f) * ap);
            drawText(g, pcx - gw * 0.5f, by, pf, 1.0f, 1.0f, 1.0f, (on ? 1.0f : 0.5f) * ap);
            if (touchInfo) touchButtonZone(code, {pcx - pw * 0.5f, by - 2.0f * sc, pw, rowH});
        };
        pill(rx + pad + rw * 0.05f,        "L", mNdsInfoPage > 0, BTN_TL);
        pill(rx + rw - pad - rw * 0.05f,   "R", mNdsInfoPage < pages - 1, BTN_TR);
    }
    mTextOutlineMode = minPrevOutline;
}

// ============================================================================
// Applications grid
// ============================================================================
// Under Minima the Applications level is an Android-launcher-style grid: a search bar on top and five
// columns of app icons with their names, scrolling by rows. The level's own sel stays the focused app,
// so launching (A / tap), the X option menu, Y pin and the in-place app-list refresh work on it
// unchanged; the grid only filters which items show and moves sel in two dimensions.

static constexpr int kAppGridCols = 5;
static const char* const kAppGridPrompt = "Search apps";   // the keyboard prompt and the bar's placeholder

bool NanoMenu::minimaAppGridActive() const {
    return mMinimaTheme && mPs3Xmb && !mPs3Stack.empty() && mPs3Stack.back().title == "Applications";
}

int NanoMenu::minimaAppGridPos() const {
    if (mPs3Stack.empty()) return -1;
    const int sel = mPs3Stack.back().sel;
    const int n = (int)mAppGridIdx.size();
    if (sel >= 0 && sel < n && mAppGridIdx[sel] == sel) return sel;   // the indices only ascend
    for (int i = 0; i < n; i++) if (mAppGridIdx[i] == sel) return i;
    return -1;
}

bool NanoMenu::minimaAppGridOnTile() const {
    return minimaAppGridActive() && !mAppGridOnSearch && minimaAppGridPos() >= 0;
}

void NanoMenu::minimaAppGridReset() {
    mAppGridQuery.clear();
    mAppGridOnSearch = false;
    mAppGridOskOpen = false;
    mAppGridTop = 0;
    mAppGridDirty = true;
    mAppGridSnap = true;
}

void NanoMenu::minimaAppGridSync() {
    Ps3Level& lvl = mPs3Stack.back();
    const int n = (int)lvl.items.size();
    // The keyboard's text filters as it is typed. Once it closes, a confirm has set the query and a
    // cancel keeps the text typed so far.
    if (mAppGridOskOpen) {
        if (mOskActive && !mOsk.closing && mOskPasswordPrompt == kAppGridPrompt) mAppGridQuery = mOskQuery;
        else mAppGridOskOpen = false;
    }
    const bool rebuilt = mAppGridDirty || (size_t)n != mAppGridItemN;
    if (rebuilt) {
        mAppGridKeys.clear();
        mAppGridKeys.reserve((size_t)n);
        for (const Ps3Item& it : lvl.items) mAppGridKeys.push_back(nanoUtf8Lower(it.label));
        mAppGridLbl.assign((size_t)n * 2, std::string());
        mAppGridLblOk.assign((size_t)n, 0);
        mAppGridItemN = (size_t)n;
        mAppGridDirty = false;
    }
    const bool queryChanged = mAppGridQuery != mAppGridFiltered;
    if (!rebuilt && !queryChanged) return;
    mAppGridFiltered = mAppGridQuery;
    std::string q = nanoUtf8Lower(mAppGridQuery);
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    mAppGridIdx.clear();
    for (int i = 0; i < n; i++)
        if (q.empty() || mAppGridKeys[i].find(q) != std::string::npos) mAppGridIdx.push_back(i);
    // A new query focuses its first match from the top; a rebuilt list keeps the focused app.
    if (queryChanged) mAppGridTop = 0;
    if (!mAppGridIdx.empty() && (queryChanged || minimaAppGridPos() < 0)) lvl.sel = mAppGridIdx[0];
    minimaAppGridReveal();
    mDisplayDirty = true;
}

void NanoMenu::minimaAppGridReveal() {
    const int cols = std::max(1, mAppGridCols), vis = std::max(1, mAppGridRows);
    const int n = (int)mAppGridIdx.size();
    const int maxTop = std::max(0, (n + cols - 1) / cols - vis);
    const int p = mAppGridOnSearch ? -1 : minimaAppGridPos();
    if (p >= 0) {
        const int row = p / cols;
        if (row < mAppGridTop) mAppGridTop = row;
        else if (row >= mAppGridTop + vis) mAppGridTop = row - vis + 1;
    }
    mAppGridTop = std::max(0, std::min(mAppGridTop, maxTop));
}

void NanoMenu::minimaAppGridMove(int dx, int dy) {
    if (!minimaAppGridActive()) return;
    mTouch.stopFling();
    const int n = (int)mAppGridIdx.size(), cols = std::max(1, mAppGridCols);
    int& sel = mPs3Stack.back().sel;
    int p = minimaAppGridPos();
    if (mAppGridOnSearch || p < 0) {
        // The search bar is the top: Down enters the grid on the first visible row, same column.
        if (dy <= 0 || n <= 0) return;
        const int col = (p < 0) ? 0 : p % cols;
        const int np = std::min(mAppGridTop * cols + col, n - 1);
        if (mAppGridIdx[np] == sel) minimaSfx(MIN_SFX_CURSOR);   // sel unchanged: minimaSfxTick stays quiet
        mAppGridOnSearch = false;
        sel = mAppGridIdx[np];
        minimaAppGridReveal();
        mDisplayDirty = true;
        return;
    }
    int np = p;
    if (dx < 0)      np = std::max(0, p - 1);          // Left / Right run on into the previous / next row
    else if (dx > 0) np = std::min(n - 1, p + 1);
    else if (dy < 0) {
        if (p < cols) {                                // Up from the first row: the search bar
            mAppGridOnSearch = true;
            minimaSfx(MIN_SFX_CURSOR);
            mDisplayDirty = true;
            return;
        }
        np = p - cols;
    } else if (dy > 0) {
        if (p + cols < n) np = p + cols;
        else if (p / cols < (n - 1) / cols) np = n - 1;   // a short last row: its last tile
    }
    if (np == p) return;
    sel = mAppGridIdx[np];
    minimaAppGridReveal();
    mDisplayDirty = true;
}

void NanoMenu::minimaAppGridPage(int dir) {
    if (!minimaAppGridActive()) return;
    const int n = (int)mAppGridIdx.size(), cols = std::max(1, mAppGridCols), vis = std::max(1, mAppGridRows);
    if (n <= 0) return;
    if (mAppGridOnSearch || minimaAppGridPos() < 0) { if (dir > 0) minimaAppGridMove(0, 1); return; }
    mTouch.stopFling();
    const int p = minimaAppGridPos();
    const int lastRow = (n - 1) / cols;
    const int row = std::max(0, std::min(lastRow, p / cols + dir * vis));
    const int np = std::min(row * cols + p % cols, n - 1);
    mAppGridTop += dir * vis;                          // the page turns with the cursor
    mPs3Stack.back().sel = mAppGridIdx[np];
    minimaAppGridReveal();
    mDisplayDirty = true;
}

void NanoMenu::minimaAppGridOpenSearch() {
    if (!minimaAppGridActive() || mOskActive) return;
    openOskForPassword(kAppGridPrompt, [this](const std::string& v) {
        mAppGridQuery = v;
        mAppGridOskOpen = false;
        mAppGridOnSearch = false;   // Enter moves to the results (back to the bar when there are none)
        mDisplayDirty = true;
    });
    mOskPasswordMode = false;
    mOskPlaintext = true;
    mOskQuery = mAppGridQuery;
    mOsk.caret = (int)mOskQuery.size();
    mAppGridOskOpen = true;
    mAppGridOnSearch = true;
}

void NanoMenu::minimaAppGridClear() {
    if (mAppGridQuery.empty()) return;
    mAppGridQuery.clear();
    minimaSfx(MIN_SFX_BACK);
    mDisplayDirty = true;
}

void NanoMenu::renderMinimaAppGrid(float rx, float ry, float rw, float rh) {
    setUiBlend();
    const int minPrevOutline = mTextOutlineMode; mTextOutlineMode = 2;   // flat text, as the list
    if (!mPs3MenuBuilt) initPs3Menu();
    if (mNdsAtRoot) mNdsAtRoot = false;   // a pushed level is drilled: nav depth, select and back rely on it
    minimaSfxTick();

    // ---- layout (NextUI proportions scaled by the short edge, like the list) ----
    const float sc     = fminf(rw, rh) / MIN_REF_H;
    const float pad    = MIN_PAD * sc;
    const float rowH   = MIN_PILL * sc;
    const float btnPad = MIN_BTNPAD * sc;
    const float btnMg  = MIN_BTNMARGIN * sc;
    const float fsHint = (MIN_FONT_S * sc) / (float)FONT_CHAR_H;
    const float lfg    = (mUserFontScale > 0.05f) ? mUserFontScale : 1.0f;
    const int   cols   = kAppGridCols;
    // Five columns across the panel, capped and centred on a wide screen (1080p, a TV).
    const float gridW  = fminf(rw - pad * 2.0f, fminf(rw, rh) * 1.3f);
    const float gridX  = rx + (rw - gridW) * 0.5f;
    const float cellW  = gridW / (float)cols;
    // Search bar beside the status pill when there is room, else on its own row below it.
    const float statusLeft = minimaStatusPill(rx, ry, rw, rh, false);
    float barX = gridX, barY = ry + pad, barW = fminf(gridX + gridW, statusLeft - 8.0f * sc) - gridX;
    const float barH = rowH;
    if (barW < gridW * 0.45f) { barY = ry + pad + rowH + btnMg; barW = gridW; }
    const float gridTop = barY + barH + 8.0f * sc;
    const float hintTop = ry + rh - pad - rowH;
    const float gridBot = hintTop - btnMg;
    const float gridH   = fmaxf(1.0f, gridBot - gridTop);
    // Tiles: an icon over a one-line name (two lines when the rows have the room), as many whole rows
    // as fit with the icon at least 40% of the column.
    const float lblPx  = 11.0f * sc;
    const float fsLbl  = lblPx / (float)FONT_CHAR_H;
    const float lineH  = lblPx * lfg * 1.18f;
    const float tPad   = 4.0f * sc, tGap = 3.0f * sc;
    const int   rows   = std::max(1, (int)floorf(gridH / (tPad * 2.0f + cellW * 0.40f + tGap + lineH)));
    const float pitch  = gridH / (float)rows;
    const bool  twoLines = (pitch - tPad * 2.0f - tGap - lineH * 2.0f) >= cellW * 0.5f;
    const int   nLines = twoLines ? 2 : 1;
    const float iconSz = fmaxf(8.0f * sc, fminf(cellW * 0.60f, pitch - tPad * 2.0f - tGap - lineH * (float)nLines));
    const float hlIn   = 3.0f * sc;
    const float lblMaxW = cellW - hlIn * 2.0f - 6.0f * sc;
    mAppGridCols = cols;
    mAppGridRows = rows;

    if (!mAppGridQuery.empty()) mPs3AutoScrollTarget = -1;   // the new-app glide steps through hidden items
    minimaAppGridSync();
    Ps3Level& lvl = mPs3Stack.back();
    const int n = (int)mAppGridIdx.size();
    if (n == 0) mAppGridOnSearch = true;   // nothing to focus but the search bar
    else if (!mAppGridOnSearch && minimaAppGridPos() < 0) lvl.sel = mAppGridIdx[0];   // sel moved off the matches
    minimaAppGridReveal();                 // the rows that fit may have changed with the layout
    const int totalRows = (n + cols - 1) / cols;

    const bool inGameScrim = minimaBackdrop(rx, ry, rw, rh);
    if (!inGameScrim) {
        drawPs3CinfoBg("", std::string());   // fade out a game's fanart left over from the list
        mTextOutlineMode = 2;
        if (mCinfoAlpha > 0.001f) mDisplayDirty = true;
    }
    float ar, ag, ab; minimaAccent(ar, ag, ab);
    const float atc = (0.299f * ar + 0.587f * ag + 0.114f * ab > 0.62f) ? 0.0f : 1.0f;   // legible on the accent

    // ---- ease the scroll and the highlight toward the selection (the list's ~3-frame glide) ----
    const int p = minimaAppGridPos();
    const bool onTile = !mAppGridOnSearch && p >= 0;
    const float tgtX = onTile ? (float)(p % cols) : mAppGridHlX;
    const float tgtY = onTile ? (float)(p / cols) : mAppGridHlY;
    {
        const int depth = ndsNavDepth();   // a level change arms the list's slide + black-wash crossfade
        if (mMinimaPrevDepth < 0) mMinimaPrevDepth = depth;
        else if (depth != mMinimaPrevDepth) {
            mMinimaTransStart = (int64_t)uptimeMillis();
            mMinimaTransDir = (depth > mMinimaPrevDepth) ? +1 : -1;
            mMinimaPrevDepth = depth;
            mAppGridSnap = true;
        }
    }
    if (mAppGridSnap) {
        mAppGridScroll = (float)mAppGridTop; mAppGridHlX = tgtX; mAppGridHlY = tgtY;
        mAppGridSnap = false;
    } else {
        const float dt = fmaxf(0.0f, fminf(0.1f, mFrameDt));
        const float k = 1.0f - powf(1.0f - 0.55f, dt * 60.0f);
        mAppGridScroll += ((float)mAppGridTop - mAppGridScroll) * k;
        mAppGridHlX += (tgtX - mAppGridHlX) * k;
        mAppGridHlY += (tgtY - mAppGridHlY) * k;
        bool moving = false;
        if (fabsf(mAppGridScroll - (float)mAppGridTop) > 0.002f) moving = true; else mAppGridScroll = (float)mAppGridTop;
        if (fabsf(mAppGridHlX - tgtX) > 0.002f) moving = true; else mAppGridHlX = tgtX;
        if (fabsf(mAppGridHlY - tgtY) > 0.002f) moving = true; else mAppGridHlY = tgtY;
        if (moving) { mDisplayDirty = true; mMinimaWantsFrame = true; }
    }
    float slideX = 0.0f;   // drill enters from the right, back from the left (cubic ease-out, 150ms)
    if (mMinimaTransStart > 0) {
        const float t = (float)((int64_t)uptimeMillis() - mMinimaTransStart) / 150.0f;
        if (t < 1.0f) {
            slideX = (float)mMinimaTransDir * (rw * 0.22f) * powf(1.0f - t, 3.0f);
            mDisplayDirty = true; mMinimaWantsFrame = true;
        }
    }

    // Touch: a vertical drag scrolls a row per row pitch (a flick flings), a tap on a tile launches
    // it and holding one opens its options; the bar opens the keyboard.
    const bool touchOk = mRenderingPanel == 0 && !ndsInModal() && !(mLaunchFadeStart > 0 && !mOverlayMode)
                         && !(mOverlayMode && mOverlayLaunchPending);
    const TouchRect gridBox{rx, gridTop, rw, gridH};
    if (touchOk && n > 0)
        mTouch.scroll(gridBox, TouchZones::kUpDown, pitch, [this](int dir) {
            if (!minimaAppGridActive() || ndsInModal()) return false;
            const int cnt = (int)mAppGridIdx.size(), c = std::max(1, mAppGridCols), vis = std::max(1, mAppGridRows);
            const int maxTop = std::max(0, (cnt + c - 1) / c - vis);
            const int top = mAppGridTop + dir;
            if (top < 0 || top > maxTop) return false;
            mAppGridTop = top;
            // Keep the highlight on screen: pull it into the visible rows on its column.
            const int pos = mAppGridOnSearch ? -1 : minimaAppGridPos();
            if (pos >= 0) {
                const int row = std::max(top, std::min(top + vis - 1, pos / c));
                mPs3Stack.back().sel = mAppGridIdx[std::min(row * c + pos % c, cnt - 1)];
            }
            mDisplayDirty = true;
            return true;
        });

    // ---- tiles ----
    const float labelKey = lblMaxW + fsLbl * lfg * 7919.0f + (twoLines ? 0.5f : 0.0f);
    if (labelKey != mAppGridLblKey) { mAppGridLblOk.assign(mAppGridLblOk.size(), 0); mAppGridLblKey = labelKey; }
    auto cpStart = [](const std::string& str, size_t end) {   // start of the codepoint ending at end
        if (end == 0) return (size_t)0;
        size_t k = end - 1;
        while (k > 0 && ((unsigned char)str[k] & 0xC0) == 0x80) k--;
        return k;
    };
    auto ellipsize = [&](const std::string& str) {
        if (measureText(str.c_str(), fsLbl) <= lblMaxW) return str;
        std::string t = str;
        while (!t.empty()) {
            t.erase(cpStart(t, t.size()));
            while (!t.empty() && t.back() == ' ') t.pop_back();
            const std::string e = t + "...";
            if (measureText(e.c_str(), fsLbl) <= lblMaxW) return e;
        }
        return std::string("...");
    };
    // Fit a name to the tile once per layout: ellipsized on one line, or wrapped at a space onto a
    // second line that is ellipsized.
    auto fitLabel = [&](int ii) {
        std::string& l1 = mAppGridLbl[(size_t)ii * 2];
        std::string& l2 = mAppGridLbl[(size_t)ii * 2 + 1];
        const std::string& str = lvl.items[ii].label;
        l2.clear();
        if (!twoLines || measureText(str.c_str(), fsLbl) <= lblMaxW) {
            l1 = ellipsize(str);
        } else {
            size_t cut = str.size();
            while (cut > 0 && measureText(str.substr(0, cut).c_str(), fsLbl) > lblMaxW) cut = cpStart(str, cut);
            const size_t sp = str.rfind(' ', cut);
            if (sp != std::string::npos && sp > 0) cut = sp;
            if (cut == 0) { cut = 1; while (cut < str.size() && ((unsigned char)str[cut] & 0xC0) == 0x80) cut++; }
            l1 = str.substr(0, cut);
            while (!l1.empty() && l1.back() == ' ') l1.pop_back();
            size_t rest = cut;
            while (rest < str.size() && str[rest] == ' ') rest++;
            l2 = ellipsize(str.substr(rest));
        }
        mAppGridLblOk[ii] = 1;
    };

    scissorLogicalRect(rx, gridTop, rw, gridH);   // the scrolling rows clip to the grid band
    const float tileW = cellW - hlIn * 2.0f, tileH = pitch - hlIn * 2.0f;
    if (onTile)   // the white capsule tile under the focused app (Minima's selected-row look)
        drawRoundedRect(gridX + mAppGridHlX * cellW + hlIn + slideX, gridTop + (mAppGridHlY - mAppGridScroll) * pitch + hlIn,
                        tileW, tileH, fminf(tileW, tileH) * 0.16f, 1.0f, 1.0f, 1.0f, 1.0f);
    const float blockH = iconSz + tGap + lineH * (float)nLines;
    const int firstRow = std::max(0, (int)floorf(mAppGridScroll));
    const int lastRow  = std::min(totalRows - 1, (int)ceilf(mAppGridScroll) + rows);
    for (int row = firstRow; row <= lastRow; row++) {
        for (int c = 0; c < cols; c++) {
            const int fi = row * cols + c;
            if (fi >= n) break;
            const int ii = mAppGridIdx[fi];
            const Ps3Item& it = lvl.items[ii];
            const float cx = gridX + (float)c * cellW + slideX;
            const float cy = gridTop + ((float)row - mAppGridScroll) * pitch;
            const bool focused = onTile && fi == p;
            const float iy = cy + (pitch - blockH) * 0.5f;
            if (it.iconTex) {
                // A real app icon draws in its own colours; the white placeholder glyph turns black on the
                // focused tile so it still shows.
                const float tint = (focused && it.nmapTex) ? 0.0f : 1.0f;
                drawIconTex(it.iconTex, cx + (cellW - iconSz) * 0.5f, iy, iconSz, iconSz, tint, tint, tint, 1.0f);
            }
            if (!mAppGridLblOk[ii]) fitLabel(ii);
            const float tc = focused ? 0.0f : 1.0f;   // black on the white tile, white on the canvas
            const float ly = iy + iconSz + tGap + (lineH - lblPx * lfg) * 0.5f;
            if (focused && nLines == 1 && mAppGridLbl[(size_t)ii * 2] != it.label) {
                // A focused name too long for its tile marquee-scrolls like a long list row (pause, then loop).
                if (lvl.sel != mMinimaMarqueeSel) { mMinimaMarqueeSel = lvl.sel; mMinimaMarquee = 0.0f; mMinimaMarqueeStart = (int64_t)uptimeMillis(); }
                const float gap = 30.0f * sc, loopW = measureText(it.label.c_str(), fsLbl) + gap;
                if ((int64_t)uptimeMillis() - mMinimaMarqueeStart > 700) {
                    mMinimaMarquee += 2.0f * sc * fmaxf(0.0f, fminf(3.0f, mFrameDt * 60.0f));
                    if (mMinimaMarquee >= loopW) mMinimaMarquee -= loopW;
                }
                mDisplayDirty = true; mMinimaWantsFrame = true;
                const float tx = cx + (cellW - lblMaxW) * 0.5f;
                const float clipTop = fmaxf(ly - 2.0f * sc, gridTop), clipBot = fminf(ly + lineH, gridTop + gridH);
                if (clipBot > clipTop) {
                    scissorLogicalRect(tx, clipTop, lblMaxW, clipBot - clipTop);
                    drawText(it.label.c_str(), tx - mMinimaMarquee,         ly, fsLbl, tc, tc, tc, 1.0f);
                    drawText(it.label.c_str(), tx - mMinimaMarquee + loopW, ly, fsLbl, tc, tc, tc, 1.0f);
                    scissorLogicalRect(rx, gridTop, rw, gridH);
                }
            } else {
                for (int li = 0; li < nLines; li++) {
                    const std::string& l = mAppGridLbl[(size_t)ii * 2 + (size_t)li];
                    if (l.empty()) continue;
                    const float lw = measureText(l.c_str(), fsLbl);
                    drawText(l.c_str(), cx + (cellW - lw) * 0.5f, ly + lineH * (float)li, fsLbl, tc, tc, tc, 1.0f);
                }
            }
            if (touchOk) {
                // By package: the list can be rebuilt between the finger going down and coming up.
                auto focusTile = [this, pkg = lvl.items[ii].payloadStr]() {
                    if (!minimaAppGridActive() || ndsInModal()) return false;
                    const auto& its = mPs3Stack.back().items;
                    for (int k = 0; k < (int)its.size(); k++) {
                        if (its[k].payloadStr != pkg) continue;
                        mAppGridOnSearch = false;
                        mPs3Stack.back().sel = k;
                        mDisplayDirty = true;
                        return true;
                    }
                    return false;
                };
                mTouch.tap(TouchRect{cx, cy, cellW, pitch}.clippedTo(gridBox),
                           [this, focusTile]() { if (focusTile()) ndsNavSelect(true); },   // tap: launch
                           [this, focusTile]() { if (focusTile()) openXmbOpt(); });        // hold: its options
            }
        }
    }
    glDisable(GL_SCISSOR_TEST);
    if (n == 0) {
        const char* empty = trDyn(lvl.items.empty() ? "Empty" : "No matching apps");
        const float fs = (MIN_FONT * sc) / (float)FONT_CHAR_H;
        drawText(empty, rx + rw * 0.5f - measureText(empty, fs) * 0.5f, gridTop + rowH, fs, 1.0f, 1.0f, 1.0f, 0.55f);
    }
    if (totalRows > rows) {   // slim scroll thumb in the right margin
        const float tw = fmaxf(2.0f, 3.0f * sc);
        const float tx = fminf(gridX + gridW + pad * 0.5f, rx + rw - tw) - tw * 0.5f;
        const float th = gridH * (float)rows / (float)totalRows;
        const float ty = gridTop + gridH * mAppGridScroll / (float)totalRows;
        drawRoundedRect(tx, ty, tw, th, tw * 0.5f, 1.0f, 1.0f, 1.0f, 0.35f);
    }

    // ---- search bar: a capsule, white with black ink while it has the focus ----
    {
        const bool focus = mAppGridOnSearch;
        const float ink = focus ? 0.0f : 1.0f;
        drawRoundedRect(barX, barY, barW, barH, barH * 0.5f, 1.0f, 1.0f, 1.0f, focus ? 1.0f : 0.14f);
        const float lw = fmaxf(1.5f, 2.0f * sc);
        const float mr = barH * 0.16f, mcx = barX + btnPad + mr, mcy = barY + barH * 0.46f;   // magnifier
        const bool localBatch = !mSolidBatchActive;
        if (localBatch) beginSolidBatch();
        ps3StrokeRing(mcx, mcy, mr, mr, lw, ink, ink, ink, 0.85f);
        if (localBatch) endSolidBatch();
        ps3ThickLine(mcx + mr * 0.72f, mcy + mr * 0.72f, mcx + mr * 1.65f, mcy + mr * 1.65f, lw, ink, ink, ink, 0.85f);
        const bool hasQuery = !mAppGridQuery.empty();
        const float clrW = hasQuery ? barH : 0.0f;
        if (hasQuery) {   // clear (x)
            const float ccx = barX + barW - barH * 0.5f, ccy = barY + barH * 0.5f, d = barH * 0.14f;
            ps3ThickLine(ccx - d, ccy - d, ccx + d, ccy + d, lw, ink, ink, ink, 0.85f);
            ps3ThickLine(ccx + d, ccy - d, ccx - d, ccy + d, lw, ink, ink, ink, 0.85f);
        }
        const float txtPx = 13.0f * sc, fsBar = txtPx / (float)FONT_CHAR_H;
        const float tx = mcx + mr * 1.65f + 8.0f * sc;
        const float tRight = barX + barW - (hasQuery ? clrW : btnPad);
        const float availW = fmaxf(0.0f, tRight - tx);
        const char* txt = hasQuery ? mAppGridQuery.c_str() : trDyn(kAppGridPrompt);
        const float tw = measureText(txt, fsBar);
        const float ty = barY + (barH - txtPx * lfg) * 0.5f;
        const float ta = hasQuery ? 1.0f : 0.55f;
        if (tw > availW) {   // a long query shows its end, as a text field does
            scissorLogicalRect(tx, barY, availW, barH);
            drawText(txt, tRight - tw, ty, fsBar, ink, ink, ink, ta);
            glDisable(GL_SCISSOR_TEST);
        } else {
            drawText(txt, tx, ty, fsBar, ink, ink, ink, ta);
        }
        if (mAppGridOskOpen) {   // caret while typing
            const float caretX = hasQuery ? fminf(tx + tw, tRight) + 2.0f * sc : tx;
            drawQuad(caretX, ty, fmaxf(1.5f, 2.0f * sc), txtPx * lfg, ink, ink, ink, 0.9f);
        }
        if (touchOk) {
            mTouch.tap(TouchRect{barX, barY, barW, barH}, [this]() {
                if (!minimaAppGridActive() || ndsInModal()) return;
                minimaAppGridOpenSearch();
            });
            if (hasQuery)
                mTouch.press(TouchRect{barX + barW - clrW, barY, clrW, barH}, [this]() {
                    if (!minimaAppGridActive() || ndsInModal()) return;
                    minimaAppGridClear();
                });
        }
    }

    // ---- status pill (top-right) ----
    minimaStatusPill(rx, ry, rw, rh, true);

    // ---- bottom legend: B Back (left), A Open / Search (right), and between them X Options, Y Pin and
    // SELECT Search on a tile (or Y Clear on the bar), dropping from the last while they do not fit ----
    {
        const float ph = rowH, py = hintTop, gap = 5.0f * sc;
        const float glyphR = fminf(MIN_FONT_S * sc * 0.70f * lfg, ph * 0.40f), lw = fmaxf(1.5f, 2.0f * sc);
        const float fsBadge = fsHint * 0.72f;
        struct Hint { int role; const char* label; uint16_t code; };   // role: face glyph, -1 = SELECT badge
        auto badgeW = [&](int role) { return role < 0 ? measureText("SELECT", fsBadge) + 8.0f * sc : glyphR * 2.0f; };
        auto hintW = [&](const Hint& h) { return badgeW(h.role) + gap + measureText(trDyn(h.label), fsHint) + btnPad * 2.0f; };
        auto drawHint = [&](const Hint& h, float px) {
            const float pw = hintW(h);
            drawRoundedRect(px, py, pw, ph, ph * 0.5f, ar, ag, ab, 1.0f);
            const float bw = badgeW(h.role);
            if (h.role < 0) {
                const float bh = glyphR * 1.6f, by = py + (ph - bh) * 0.5f;
                drawRoundedRing(px + btnPad, by, bw, bh, bh * 0.5f, lw * 0.75f, atc, atc, atc, 0.95f);
                const float sw = measureText("SELECT", fsBadge);
                drawText("SELECT", px + btnPad + (bw - sw) * 0.5f, py + (ph - MIN_FONT_S * sc * 0.72f * lfg) * 0.5f,
                         fsBadge, atc, atc, atc, 1.0f);
                if (touchOk) mTouch.press(TouchRect{px, py, pw, ph}, [this]() {
                    if (minimaAppGridActive() && !ndsInModal()) minimaAppGridOpenSearch();
                });
            } else {
                drawFaceGlyph(h.role, px + btnPad + glyphR, py + ph * 0.5f, glyphR, lw, 1.0f, atc, atc, atc);
                if (touchOk) touchButtonZone(h.code, {px, py, pw, ph});
            }
            drawText(trDyn(h.label), px + btnPad + bw + gap, py + (ph - MIN_FONT_S * sc * lfg) * 0.5f, fsHint, atc, atc, atc, 1.0f);
        };
        const Hint back{1, "Back", BTN_EAST};
        const Hint confirm{0, onTile ? "Open" : "Search", BTN_SOUTH};
        std::vector<Hint> mid;
        if (onTile) {
            const std::string& pkg = lvl.items[lvl.sel].payloadStr;
            if (!(mOverlayMode && !mOverlayWallpaper)) mid.push_back({3, "Options", BTN_NORTH});
            if (!pkg.empty()) mid.push_back({2, isAppPinned(pkg) ? "Unpin" : "Pin", BTN_WEST});
            mid.push_back({-1, "Search", BTN_SELECT});
        } else if (!mAppGridQuery.empty()) {
            mid.push_back({2, "Clear", BTN_WEST});
        }
        const float backW = hintW(back), confW = hintW(confirm), sp = btnMg * 2.0f;
        const float midL = rx + pad + backW + sp, midR = rx + rw - pad - confW - sp;
        float midW = 0.0f;
        while (!mid.empty()) {
            midW = 0.0f;
            for (const Hint& h : mid) midW += hintW(h);
            midW += sp * (float)(mid.size() - 1);
            if (midW <= midR - midL) break;
            mid.pop_back();
        }
        drawHint(back, rx + pad);
        drawHint(confirm, rx + rw - pad - confW);
        float hx = midL + ((midR - midL) - midW) * 0.5f;
        for (const Hint& h : mid) { drawHint(h, hx); hx += hintW(h) + sp; }
    }

    // ---- level-change black-wash crossfade, launch white-wash, then the Pin banner on top ----
    if (mMinimaTransStart > 0) {
        const float a = 1.0f - (float)((int64_t)uptimeMillis() - mMinimaTransStart) / 150.0f;
        if (a > 0.0f) { drawQuad(rx, ry, rw, rh, 0.0f, 0.0f, 0.0f, a); mDisplayDirty = true; mMinimaWantsFrame = true; }
        else mMinimaTransStart = 0;
    }
    drawPhotoBanner();
    mTextOutlineMode = minPrevOutline;
}

} // namespace android
