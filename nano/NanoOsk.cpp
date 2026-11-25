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

// Native on-screen keyboard runtime. A reimplementation of the Android TV
// Leanback IME keyboard, driven by D-pad / gamepad input into a UTF-8
// std::string, extended with a pluggable per-script input-method layer.
//
// Layout DATA is the generated NanoOskLayouts.h (kOskKb[] / kOskPopups[]),
// included by THIS translation unit only (the static-const tables must not be
// duplicated across TUs). Runtime state is NanoMenu::mOsk (NanoOskState).

#define LOG_TAG "GammaOSNano"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cutils/properties.h>
#include <linux/input.h>   // BTN_* codes for the footer's button prompts

#include <utils/Log.h>

#include "NanoMenu.h"
#include "NanoI18n.h"      // trDyn() runtime translation of hardcoded UI strings
#include "NanoOsk.h"
#include "NanoUtf8Case.h"
#include "NanoOskLayouts.h"
#include "NanoOskLayoutsExtra.h"  // non-Latin keyboards (Korean, ...)
#include "NanoOskInput.h"         // HangulInput and other per-script engines
#include "NanoMenuShaders.h"   // FONT_CHAR_W / FONT_CHAR_H
#include "NanoMenuStrings.h"   // tr(), nanoGetLocale(), nanoGetLocaleInfo()

namespace android {

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------
namespace {

constexpr int64_t kLongPressMs   = 350;   // hold A to open accent popup
constexpr int64_t kDoubleTapMs   = 250;   // shift double-click -> caps lock
constexpr float   kBoxAspect     = 2.488372f; // 428:172 Leanback grid aspect
constexpr float   kKeyFrac       = 0.065421f;  // 28/428 (one key cell)
constexpr size_t  kBufferCap     = 256;   // committed buffer byte cap

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- UTF-8 ---

std::string utf8Encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

// Byte offset of the start of the codepoint ending at byte `pos` (i.e. the
// previous codepoint boundary), for backspace / caret-left.
int utf8PrevStart(const std::string& s, int pos) {
    if (pos <= 0) return 0;
    int i = pos - 1;
    while (i > 0 && (((unsigned char)s[i] & 0xC0) == 0x80)) i--;
    return i;
}

// Byte offset just after the codepoint starting at byte `pos`, for caret-right.
int utf8NextStart(const std::string& s, int pos) {
    int n = (int)s.size();
    if (pos >= n) return n;
    int i = pos + 1;
    while (i < n && (((unsigned char)s[i] & 0xC0) == 0x80)) i++;
    return i;
}

uint32_t utf8DecodeAt(const std::string& s, int pos, int& advance) {
    int n = (int)s.size();
    if (pos >= n) { advance = 0; return 0; }
    unsigned char c = (unsigned char)s[pos];
    uint32_t cp; int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { advance = 1; return 0xFFFD; }
    if (pos + len > n) { advance = 1; return 0xFFFD; }
    for (int k = 1; k < len; k++) {
        const unsigned char b = (unsigned char)s[pos + k];
        if ((b & 0xC0) != 0x80) { advance = 1; return 0xFFFD; }
        cp = (cp << 6) | (b & 0x3F);
    }
    advance = len;
    return cp;
}

int utf8Len(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

// Shifted key caps: one character for one (ß stays ß), and a Greek vowel keeps its tonos.
uint32_t toUpperCp(uint32_t cp) {
    switch (cp) {
        case 0x3AC: return 0x386;
        case 0x3AD: return 0x388;
        case 0x3AE: return 0x389;
        case 0x3AF: return 0x38A;
        case 0x3CC: return 0x38C;
        case 0x3CD: return 0x38E;
        case 0x3CE: return 0x38F;
    }
    return nanoToUpperCp(cp);
}

std::string utf8Upper(const std::string& s) {
    std::string out;
    int i = 0, n = (int)s.size();
    while (i < n) {
        int adv;
        uint32_t cp = utf8DecodeAt(s, i, adv);
        if (adv <= 0) break;
        out += utf8Encode(toUpperCp(cp));
        i += adv;
    }
    return out;
}

// The glyph drawn on an icon key (state-dependent for shift). The mode key's
// ABC/SYM text is already baked per-page into the table (GLYPH_SYMBOLS vs
// GLYPH_ALPHABET) by the generator, so the page is not needed here.
const char* glyphFor(OskGlyph g, OskShift shift) {
    switch (g) {
        case GLYPH_DELETE:    return "\xE2\x8C\xAB";              // U+232B
        case GLYPH_SHIFT_OFF: return (shift == SHIFT_LOCKED) ? "\xE2\x87\xAA"  // U+21EA caps
                                                             : "\xE2\x87\xA7"; // U+21E7 shift
        case GLYPH_LEFT:      return "\xE2\x86\x90";              // U+2190
        case GLYPH_RIGHT:     return "\xE2\x86\x92";              // U+2192
        case GLYPH_SYMBOLS:   return "?!#";
        case GLYPH_ALPHABET:  return "ABC";
        case GLYPH_SPACE:     return "";   // drawn as a bar
        default:              return "";
    }
}

// Compute the aspect-aware keyboard panel geometry. Pure function of the
// surface size and the measured action-button label width, so it is
// deterministic and host-testable.
// The touch keyboard: the whole width of the screen at the bottom, with the toggle, language and
// action buttons in a bar above the keys, keys as large as the screen allows and no footer.
static OskBox oskComputeTouchBox(int W, int H, float actLabelPx, bool promptLine, float sf) {
    OskBox b{};
    b.touch = true;
    b.promptLine = promptLine;
    b.sf = sf;
    b.portrait = (H > 0 ? (float)W / (float)H : 1.6f) < 0.85f;
    b.actBelow = false;
    const float margin = 6.0f * sf, rowGap = 8.0f * sf, panelPad = 10.0f * sf, btnGap = 8.0f * sf;
    const float promptH = promptLine ? (FONT_CHAR_H * 1.5f * sf + 6.0f * sf) : 0.0f;
    const float previewH = FONT_CHAR_H * 2.0f * sf + 10.0f * sf + promptH;
    b.tbH = fmaxf((float)H * 0.065f, 28.0f * sf);
    b.kbW = (float)W - 2.0f * margin;
    b.kbH = fminf((float)H * (b.portrait ? 0.38f : 0.44f), b.kbW * 0.5f);
    float innerH = previewH + rowGap + b.tbH + rowGap + b.kbH;
    const float budget = 0.9f * (float)H - 2.0f * panelPad;
    if (innerH > budget) { b.kbH = fmaxf(b.kbH - (innerH - budget), b.kbH * 0.5f); innerH = budget; }
    b.keyPx = b.kbW * kKeyFrac;
    b.ks = sf * fminf(1.5f, fmaxf(1.0f, b.kbH * 0.2093f / (44.0f * sf)));
    b.panelX = 0.0f; b.panelW = (float)W;
    b.panelH = innerH + 2.0f * panelPad;
    b.panelY = fmaxf(0.0f, (float)H - b.panelH);
    b.kbX = margin;
    b.previewX = margin + 4.0f * sf;
    b.promptY = b.panelY + panelPad;
    b.previewY = b.promptY + promptH;
    b.tbY = b.previewY + previewH + rowGap;
    b.kbY = b.tbY + b.tbH + rowGap;
    b.actH = b.langH = b.tbH;
    b.actW = b.langW = actLabelPx + 32.0f * sf;
    b.actX = b.kbX + b.kbW - b.actW;
    b.langX = b.actX - btnGap - b.langW;
    b.actY = b.langY = b.tbY;
    b.footerY = (float)H;
    return b;
}

OskBox oskComputeBox(int W, int H, float actLabelPx, bool promptLine = false, bool touch = false) {
    float sf = fminf((float)W / 1080.0f, (float)H / 720.0f);
    if (sf < 0.5f) sf = 0.5f;
    if (touch) return oskComputeTouchBox(W, H, actLabelPx, promptLine, sf);
    OskBox b{};
    b.promptLine = promptLine;
    b.sf = sf;
    b.ks = sf;

    float aspect = (H > 0) ? (float)W / (float)H : 1.6f;
    b.portrait = aspect < 0.85f;
    b.actBelow = b.portrait;

    float wFrac = (aspect >= 1.6f) ? 0.78f
                : (aspect >= 1.2f) ? 0.86f
                : (aspect >= 0.85f) ? 0.90f : 0.96f;

    float gap        = 12.0f * sf;
    float actPadH    = 16.0f * sf;
    // Field editors put their guidance ("Folder ID (letters, digits, dashes)") on a line of its
    // own above the typed text, so a long prompt never squeezes the input into the right margin.
    float promptH    = promptLine ? (FONT_CHAR_H * 1.5f * sf + 6.0f * sf) : 0.0f;
    float previewH   = FONT_CHAR_H * 2.0f * sf + 10.0f * sf + promptH;
    float footerH    = FONT_CHAR_H * 1.4f * sf + 6.0f * sf;
    float bottomPad  = 16.0f * sf;
    float panelPad   = 10.0f * sf;

    b.actW = actLabelPx + 2.0f * actPadH;
    b.actH = 36.0f * sf;
    const float btnGap = 8.0f * sf;   // between the action and language buttons

    float availW = (float)W * wFrac;
    float kbW = b.actBelow ? availW : (availW - b.actW - gap);

    // Clamp key size for legibility / to avoid a comically large grid.
    float keyPx = kbW * kKeyFrac;
    float minKeyPx = 22.0f * sf, maxKeyPx = 64.0f * sf;
    if (keyPx > maxKeyPx) {
        kbW = maxKeyPx / kKeyFrac;
    } else if (keyPx < minKeyPx) {
        kbW = minKeyPx / kKeyFrac;
        float capW = (float)W * 0.98f - (b.actBelow ? 0.0f : (b.actW + gap));
        if (kbW > capW) kbW = capW;
    }
    float kbH = kbW / kBoxAspect;

    // Inner content stack height (preview + keyboard [+ action-below] + footer),
    // all of which live INSIDE the panel padding. Vertical budget keeps the
    // field/content above the keyboard visible; portrait gets a taller budget.
    float rowGap = 8.0f * sf;
    float extra  = b.actBelow ? (gap + 2.0f * b.actH + btnGap) : 0.0f;
    float innerH = previewH + rowGap + kbH + extra + rowGap + footerH;
    float budget = (b.portrait ? 0.85f : 0.66f) * (float)H - 2.0f * panelPad;
    if (innerH > budget && innerH > 0.0f) {
        float scale = budget / innerH;
        kbW *= scale; kbH *= scale;
        innerH = previewH + rowGap + kbH + extra + rowGap + footerH;
    }
    b.keyPx = kbW * kKeyFrac;
    b.kbW = kbW;
    b.kbH = kbH;

    float blockW = b.actBelow ? kbW : (kbW + gap + b.actW);
    b.panelW = blockW + 2.0f * panelPad;
    b.panelH = innerH + 2.0f * panelPad;
    b.panelX = ((float)W - b.panelW) / 2.0f;
    b.panelY = (float)H - bottomPad - b.panelH;
    if (b.panelY < 4.0f * sf) b.panelY = 4.0f * sf;   // never run off the top

    // Lay out top-down inside the panel.
    b.kbX = ((float)W - blockW) / 2.0f;
    b.previewX = b.kbX;
    b.promptY  = b.panelY + panelPad;
    b.previewY = b.promptY + promptH;
    b.kbY = b.previewY + previewH + rowGap;
    if (!b.actBelow) {
        b.actX = b.kbX + kbW + gap;
        b.actY = b.kbY + kbH / 2.0f - (2.0f * b.actH + btnGap) / 2.0f;
    } else {
        b.actX = ((float)W - b.actW) / 2.0f;
        b.actY = b.kbY + kbH + gap;
    }
    b.langX = b.actX; b.langW = b.actW; b.langH = b.actH;
    b.langY = b.actY + b.actH + btnGap;
    b.footerY = b.actBelow ? (b.langY + b.langH + rowGap) : (b.kbY + kbH + rowGap);
    return b;
}

inline void keyRect(const OskBox& b, float fx, float fw, float fy, float fh,
                    float& x, float& y, float& w, float& h) {
    x = b.kbX + fx * b.kbW;
    y = b.kbY + fy * b.kbH;
    w = fw * b.kbW;
    h = fh * b.kbH;
    if (b.touch) {   // the touch keyboard's keys take the gaps around them: no dead space to miss in
        // (only the gaps between keys: the outer ones keep inside the keyboard and the screen)
        const float gx = 0.028037f * b.kbW, gy = 0.046511f * b.kbH;
        const float x0 = fmaxf(b.kbX, x - gx * 0.5f), x1 = fminf(b.kbX + b.kbW, x + w + gx * 0.5f);
        const float y0 = fmaxf(b.kbY, y - gy * 0.5f), y1 = fminf(b.kbY + b.kbH, y + h + gy * 0.5f);
        x = x0; w = x1 - x0;
        y = y0; h = y1 - y0;
    }
}

// Korean dubeolsik shift: base consonant/vowel -> tense/compound jamo.
uint32_t koreanShiftJamo(uint32_t cp) {
    switch (cp) {
        case 0x3142: return 0x3143; // b  -> bb
        case 0x3148: return 0x3149; // j  -> jj
        case 0x3137: return 0x3138; // d  -> dd
        case 0x3131: return 0x3132; // g  -> gg
        case 0x3145: return 0x3146; // s  -> ss
        case 0x3150: return 0x3152; // ae -> yae
        case 0x3154: return 0x3156; // e  -> ye
        default: return cp;
    }
}

// Per-script input-method singletons (own their composing state for the run).
HangulInput* oskHangulInput() { static HangulInput inst; return &inst; }
PinyinInput* oskPinyinInput() { static PinyinInput inst; return &inst; }
JapaneseInput* oskJapaneseInput() { static JapaneseInput inst; return &inst; }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Language -> layout (faithful LeanbackKeyboardContainer.initKeyboards chain)
// ---------------------------------------------------------------------------
OskLayoutChoice oskPickLayout(const char* code, const char* region) {
    const char* c = code ? code : "";
    const char* r = region ? region : "";
    auto M = [&](const char* lang, const char* country) -> bool {
        if (lang[0] && strcmp(c, lang) != 0) return false;
        if (country[0] && strcmp(r, country) != 0) return false;
        return true;
    };
    if (M("en", "GB")) return { OSK_KB_QWERTY_EN_GB, OSK_KB_SYM_EN_GB };
    if (M("en", "IN")) return { OSK_KB_QWERTY_EN_IN, OSK_KB_SYM_EN_IN };
    if (M("es", "ES") || M("gl", "ES") || M("eu", "ES"))
        return { OSK_KB_QWERTY_ES_EU, OSK_KB_SYM_EU };
    if (M("es", ""))   return { OSK_KB_QWERTY_ES_US, OSK_KB_SYM_US };
    if (M("az", ""))   return { OSK_KB_QWERTY_AZ, OSK_KB_SYM_EU };
    if (M("ca", ""))   return { OSK_KB_QWERTY_CA, OSK_KB_SYM_EU };
    if (M("da", ""))   return { OSK_KB_QWERTY_DA, OSK_KB_SYM_EU };
    if (M("et", ""))   return { OSK_KB_QWERTY_ET, OSK_KB_SYM_EU };
    if (M("fi", ""))   return { OSK_KB_QWERTY_FI, OSK_KB_SYM_EU };
    if (M("nb", ""))   return { OSK_KB_QWERTY_NB, OSK_KB_SYM_US };
    if (M("sv", ""))   return { OSK_KB_QWERTY_SV, OSK_KB_SYM_EU };
    if (M("en", "") || M("fr", "CA")) return { OSK_KB_QWERTY_US, OSK_KB_SYM_US };
    if (M("de", "CH") || M("it", "CH")) return { OSK_KB_QWERTZ_CH, OSK_KB_SYM_EU };
    if (M("de", "") || M("hr", "") || M("cs", "") || M("fr", "CH") ||
        M("it", "CH") || M("hu", "") || M("sr", "") || M("sl", "") || M("sq", ""))
        return { OSK_KB_QWERTZ, OSK_KB_SYM_EU };
    if (M("fr", "") || M("nl", "BE")) return { OSK_KB_AZERTY, OSK_KB_SYM_AZERTY };
    return { OSK_KB_QWERTY_EU, OSK_KB_SYM_EU };
}

// In-keyboard language switch: a small curated cycle (D-pad friendly).
namespace {
struct LangCycleEntry { const char* code; const char* region; };
const LangCycleEntry kLangCycle[] = {
    { "en", "US" }, { "ko", "KR" }, { "zh", "CN" }, { "ja", "JP" }, { "ru", "RU" }, { "el", "GR" },
    { "he", "IL" }, { "ar", "SA" }, { "th", "TH" }, { "fr", "FR" },
    { "de", "DE" }, { "es", "ES" }, { "en", "GB" }, { "da", "DK" },
    { "fi", "FI" }, { "sv", "SE" }, { "nb", "NO" },
};
const int kLangCycleCount = (int)(sizeof(kLangCycle) / sizeof(kLangCycle[0]));
} // namespace

// ---------------------------------------------------------------------------
// Layout / state helpers
// ---------------------------------------------------------------------------

const OskKeyboard* NanoMenu::oskCurrentKb() const {
    const OskKeyboard* kb = (mOsk.page == PAGE_ABC) ? mOsk.abcKb : mOsk.symKb;
    return kb ? kb : &kOskKb[OSK_KB_QWERTY_EU];
}

void NanoMenu::oskApplyLocale() {
    const char* code; const char* region;
    if (mOsk.langCycleIndex >= 0 && mOsk.langCycleIndex < kLangCycleCount) {
        code   = kLangCycle[mOsk.langCycleIndex].code;
        region = kLangCycle[mOsk.langCycleIndex].region;
    } else {
        const LocaleInfo& li = nanoGetLocaleInfo(nanoGetLocale());
        code = li.code; region = li.regionCode;
    }
    oskSetLanguage(code, region);
}

void NanoMenu::oskSetLanguage(const char* code, const char* region) {
    auto setLabel = [&](const char* s) {
        size_t i = 0;
        for (; s[i] && i < sizeof(mOsk.langLabel) - 1; i++) mOsk.langLabel[i] = s[i];
        mOsk.langLabel[i] = 0;
    };
    // Non-Latin scripts: own layout + composing input method.
    if (strcmp(code, "ko") == 0) {
        mOsk.abcKb = &kKb_korean;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = oskHangulInput();
        mOsk.im->reset();
        mOsk.dir = OSK_LTR;
        setLabel("\xed\x95\x9c");           // 한
        return;
    }
    if (strcmp(code, "zh") == 0) {
        // Pinyin is typed on a Latin QWERTY; the engine surfaces Hanzi candidates.
        OskLayoutChoice c = oskPickLayout("en", "US");
        mOsk.abcKb = &kOskKb[c.abcId];
        mOsk.symKb = &kOskKb[c.symId];
        mOsk.im = oskPinyinInput();
        mOsk.im->reset();
        mOsk.dir = OSK_LTR;
        setLabel("\xe4\xb8\xad");            // 中
        return;
    }
    if (strcmp(code, "ja") == 0) {
        // Japanese romaji typed on a Latin QWERTY -> hiragana reading, with a
        // kanji/hiragana/katakana candidate bar.
        OskLayoutChoice c = oskPickLayout("en", "US");
        mOsk.abcKb = &kOskKb[c.abcId];
        mOsk.symKb = &kOskKb[c.symId];
        mOsk.im = oskJapaneseInput();
        mOsk.im->reset();
        mOsk.dir = OSK_LTR;
        setLabel("\xe3\x81\x82");            // あ
        return;
    }
    if (strcmp(code, "ru") == 0) {
        mOsk.abcKb = &kKb_russian;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = nullptr; mOsk.dir = OSK_LTR;
        setLabel("RU"); return;
    }
    if (strcmp(code, "el") == 0) {
        mOsk.abcKb = &kKb_greek;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = nullptr; mOsk.dir = OSK_LTR;
        setLabel("EL"); return;
    }
    if (strcmp(code, "he") == 0) {
        mOsk.abcKb = &kKb_hebrew;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = nullptr; mOsk.dir = OSK_RTL;   // Hebrew is right-to-left
        setLabel("HE"); return;
    }
    if (strcmp(code, "th") == 0) {
        mOsk.abcKb = &kKb_thai;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = nullptr; mOsk.dir = OSK_LTR;
        setLabel("TH"); return;
    }
    if (strcmp(code, "ar") == 0) {
        mOsk.abcKb = &kKb_arabic;
        mOsk.symKb = &kOskKb[OSK_KB_SYM_US];
        mOsk.im = nullptr; mOsk.dir = OSK_RTL;   // shaping happens in drawText
        setLabel("AR"); return;
    }
    // Latin (Leanback chain), direct input.
    OskLayoutChoice c = oskPickLayout(code, region);
    mOsk.abcKb = &kOskKb[c.abcId];
    mOsk.symKb = &kOskKb[c.symId];
    mOsk.im = nullptr;
    mOsk.dir = OSK_LTR;
    char up[3] = { code[0], code[0] ? code[1] : (char)0, 0 };
    if (up[0] >= 'a' && up[0] <= 'z') up[0] -= 32;
    if (up[1] >= 'a' && up[1] <= 'z') up[1] -= 32;
    setLabel(up[0] ? up : "EN");
}

OskBox NanoMenu::oskLayoutBox() {
    float sf = fminf((float)mWidth / 1080.0f, (float)mHeight / 720.0f);
    if (sf < 0.5f) sf = 0.5f;
    // A wizard text field (callback armed) commits a value, so the action key
    // reads "Enter", not "Search" - "Search" is only for the free search OSK.
    const char* actLabel = trDyn(mOskPasswordCallback ? "Enter" : "Search");
    float actLabelPx = measureText(actLabel, 1.7f * sf);
    // Field editors (Syncthing labels and IDs, system names, passwords) get their prompt on
    // its own line; the wizard draws the value into its dialog field and the search OSK keeps
    // its one-line "Search:" preview.
    const bool promptLine = (mOskPasswordCallback || mOskPasswordMode) && !mPs3WizActive
                            && !mOskPasswordPrompt.empty();
    return oskComputeBox(mWidth, mHeight, actLabelPx, promptLine, mOsk.touch);
}

// The button that switches between the touch and the controller keyboard: in the touch keyboard's
// bar, at its left; above the controller keyboard's panel, at its right (or at the right of its
// text line when there is no room above). Returns its label.
const char* NanoMenu::oskToggleRect(const OskBox& b, float& x, float& y, float& w, float& h) {
    const char* label = trDyn(b.touch ? "Controller keyboard" : "Touch keyboard");
    w = measureText(label, 1.5f * b.sf) + 28.0f * b.sf;
    if (b.touch) {
        x = b.kbX; y = b.tbY; h = b.tbH;
        return label;
    }
    h = b.actH;
    x = b.panelX + b.panelW - w;
    y = b.panelY - h - 6.0f * b.sf;
    if (y < 2.0f * b.sf) { x = b.kbX + b.kbW - w; y = b.previewY; }
    return label;
}

float NanoMenu::oskTouchKeyScale() const {
    return oskComputeBox(mWidth, mHeight, 0.0f, false, true).ks;
}

void NanoMenu::oskSetTouch(bool touch) {
    mOsk.touch = touch;
    mOsk.inAction = mOsk.inLang = mOsk.inCandidateBar = false;
    mOskGlassValid = false;   // the panel changed size: capture its backdrop again
    mDisplayDirty = true;
}

// A button on the touch keyboard: the controller keyboard comes up instead, and the press only
// brings it (true).
bool NanoMenu::oskButtonLeavesTouch() {
    if (!mOskActive || !mOsk.touch) return false;
    oskSetTouch(false);
    return true;
}

// Clamp focus indices into the current page (defensive after page swaps).
static void clampFocus(const OskKeyboard* kb, int& row, int& col) {
    if (row < 0) row = 0;
    if (row >= kb->rowCount) row = kb->rowCount - 1;
    if (col < 0) col = 0;
    if (col >= kb->rows[row].keyCount) col = kb->rows[row].keyCount - 1;
}

// ---------------------------------------------------------------------------
// Buffer operations (UTF-8, caret aware)
// ---------------------------------------------------------------------------

// Re-format a numeric field's raw text into its separated display form, keeping
// only digits, capping the count, and inserting the date "/" or time ":" at the
// fixed positions. fmt: 1 = date YYYY/MM/DD (8 digits), 2 = time HH:MM (4 digits).
static std::string oskFormatNumeric(const std::string& s, int fmt) {
    std::string d;
    for (char c : s) if (c >= '0' && c <= '9') d += c;
    size_t cap = (fmt == 1) ? 8 : 4;
    if (d.size() > cap) d.resize(cap);
    std::string out;
    if (fmt == 1) {
        for (size_t i = 0; i < d.size(); i++) { if (i == 4 || i == 6) out += '/'; out += d[i]; }
    } else {
        for (size_t i = 0; i < d.size(); i++) { if (i == 2) out += ':'; out += d[i]; }
    }
    return out;
}

void NanoMenu::oskInsertCp(uint32_t cp) {
    // Numeric field (date / time): digits only, auto-insert the separators, and
    // bypass the IME entirely so no kana/pinyin composition runs on a value field.
    if (mOskFieldFmt != 0) {
        if (cp >= '0' && cp <= '9') {
            mOskQuery = oskFormatNumeric(mOskQuery + (char)cp, mOskFieldFmt);
            mOsk.caret = (int)mOskQuery.size();
        }
        mDisplayDirty = true;
        return;
    }
    if (mOsk.im) {
        OskBuffer buf{ &mOskQuery, &mOsk.caret };
        if (mOsk.im->onCodepoint(cp, buf)) {
            mOsk.candidates = mOsk.im->candidates();
            if (!mOskPasswordMode) updateSearchResults();
            mDisplayDirty = true;
            return;
        }
    }
    std::string enc = utf8Encode(cp);
    if (mOskQuery.size() + enc.size() > kBufferCap) return;
    if (mOsk.caret < 0) mOsk.caret = 0;
    if (mOsk.caret > (int)mOskQuery.size()) mOsk.caret = (int)mOskQuery.size();
    mOskQuery.insert((size_t)mOsk.caret, enc);
    mOsk.caret += (int)enc.size();
    if (!mOskPasswordMode) updateSearchResults();
    mDisplayDirty = true;
}

void NanoMenu::oskType(char c) {
    oskInsertCp((uint32_t)(unsigned char)c);
}

void NanoMenu::oskBackspace() {
    // Numeric field: drop one digit then re-format (a backspace removes a digit,
    // not a separator, so "2026/06" -> "2026/0").
    if (mOskFieldFmt != 0) {
        std::string d;
        for (char c : mOskQuery) if (c >= '0' && c <= '9') d += c;
        if (!d.empty()) d.pop_back();
        mOskQuery = oskFormatNumeric(d, mOskFieldFmt);
        mOsk.caret = (int)mOskQuery.size();
        mDisplayDirty = true;
        return;
    }
    if (mOsk.im) {
        OskBuffer buf{ &mOskQuery, &mOsk.caret };
        if (mOsk.im->onBackspace(buf)) {
            mOsk.candidates = mOsk.im->candidates();
            if (!mOskPasswordMode) updateSearchResults();
            mDisplayDirty = true;
            return;
        }
    }
    if (mOsk.caret <= 0 || mOskQuery.empty()) { mDisplayDirty = true; return; }
    if (mOsk.caret > (int)mOskQuery.size()) mOsk.caret = (int)mOskQuery.size();
    int start = utf8PrevStart(mOskQuery, mOsk.caret);
    mOskQuery.erase((size_t)start, (size_t)(mOsk.caret - start));
    mOsk.caret = start;
    if (!mOskPasswordMode) updateSearchResults();
    mDisplayDirty = true;
}

void NanoMenu::oskCaretLeft() {
    if (mOsk.caret > 0) mOsk.caret = utf8PrevStart(mOskQuery, mOsk.caret);
    mDisplayDirty = true;
}

void NanoMenu::oskCaretRight() {
    if (mOsk.caret < (int)mOskQuery.size())
        mOsk.caret = utf8NextStart(mOskQuery, mOsk.caret);
    mDisplayDirty = true;
}

// Insert a whole UTF-8 string at the caret (used by Paste). Unlike oskInsertCp this does not
// route through the IME - pasted text is committed raw. Respects the committed-buffer cap,
// truncating on a UTF-8 lead boundary so a multibyte sequence is never split.
void NanoMenu::oskInsertString(const std::string& s) {
    if (s.empty()) return;
    if (mOsk.caret < 0) mOsk.caret = 0;
    if (mOsk.caret > (int)mOskQuery.size()) mOsk.caret = (int)mOskQuery.size();
    size_t room = (mOskQuery.size() < kBufferCap) ? (kBufferCap - mOskQuery.size()) : 0;
    if (room == 0) { mDisplayDirty = true; return; }
    std::string ins = s;
    if (ins.size() > room) {
        size_t cut = room;
        while (cut > 0 && (((unsigned char)ins[cut]) & 0xC0) == 0x80) cut--;  // UTF-8 lead boundary
        ins.resize(cut);
        if (ins.empty()) { mDisplayDirty = true; return; }
    }
    mOskQuery.insert((size_t)mOsk.caret, ins);
    mOsk.caret += (int)ins.size();
    if (!mOskPasswordMode) updateSearchResults();
    mDisplayDirty = true;
}

// Y while the OSK is up: request the Android clipboard from the SystemServer bridge. nano is
// native (bootanim) and cannot call ClipboardManager, so we bump sys.gammaos.nano.clip_req with a
// fresh id; the bridge reads the primary clip, writes /data/system/nano_clipboard.txt, and echoes
// the id to clip_ready. Non-blocking: oskTick() picks up the reply and inserts the text.
void NanoMenu::oskPaste() {
    if (!mOskActive) return;
    if (mOskFieldFmt != 0) return;   // numeric (date/time) fields: paste is meaningless
    mOskPasteNonce++;
    char req[32];
    snprintf(req, sizeof(req), "%ld", mOskPasteNonce);
    property_set("sys.gammaos.nano.clip_req", req);
    mOskPastePending = true;
    mOskPasteReqMs = nowMs();
}

// ---------------------------------------------------------------------------
// State machine: shift / caps / symbol page / language
// ---------------------------------------------------------------------------

void NanoMenu::oskToggleShift() {
    int64_t now = nowMs();
    if (now - mOsk.lastShiftClickMs < kDoubleTapMs) {
        mOsk.shift = SHIFT_LOCKED;                 // double-click -> caps lock
    } else if (mOsk.shift == SHIFT_OFF) {
        mOsk.shift = SHIFT_ON;
    } else {
        mOsk.shift = SHIFT_OFF;                     // single click from on/locked
    }
    mOsk.lastShiftClickMs = now;
    mDisplayDirty = true;
}

void NanoMenu::oskToggleCaps() {
    mOsk.shift = (mOsk.shift == SHIFT_LOCKED) ? SHIFT_OFF : SHIFT_LOCKED;
    mDisplayDirty = true;
}

void NanoMenu::oskToggleSym() {
    mOsk.page = (mOsk.page == PAGE_ABC) ? PAGE_SYM : PAGE_ABC;
    mOsk.miniOpen = false;
    mOsk.inAction = false; mOsk.inLang = false;
    const OskKeyboard* kb = oskCurrentKb();
    clampFocus(kb, mOsk.focusRow, mOsk.focusCol);
    mDisplayDirty = true;
}

void NanoMenu::oskCycleLanguage(int dir) {
    if (mOsk.im) {                       // commit any in-progress composition
        OskBuffer buf{ &mOskQuery, &mOsk.caret };
        mOsk.im->commitComposing(buf);
        mOsk.candidates.clear();
        mOsk.inCandidateBar = false;
    }
    int idx = mOsk.langCycleIndex;
    if (idx < 0) idx = 0; else idx += dir;
    if (idx < 0) idx = kLangCycleCount - 1;
    if (idx >= kLangCycleCount) idx = 0;
    mOsk.langCycleIndex = idx;
    mOsk.page = PAGE_ABC;
    mOsk.shift = SHIFT_OFF;
    oskApplyLocale();
    clampFocus(oskCurrentKb(), mOsk.focusRow, mOsk.focusCol);
    if (!mOskPasswordMode) updateSearchResults();
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Accent / shift popup mini-keyboard
// ---------------------------------------------------------------------------

void NanoMenu::oskOpenPopup(const OskKey& key) {
    if (key.popupIndex < 0 || key.popupIndex >= kOskPopupCount) return;
    const OskPopup& p = kOskPopups[key.popupIndex];
    if (p.keyCount <= 0) return;
    mOsk.miniOpen = true;
    mOsk.miniPopupIndex = key.popupIndex;
    mOsk.miniOriginRow = mOsk.focusRow;
    mOsk.miniOriginCol = mOsk.focusCol;
    int rowKeys = oskCurrentKb()->rows[mOsk.focusRow].keyCount;
    int base = mOsk.focusCol;
    if (base + p.keyCount > rowKeys) base = rowKeys - p.keyCount;
    if (base < 0) base = 0;
    mOsk.miniBaseCol = base;
    mOsk.miniFocus = 0;
    mDisplayDirty = true;
}

void NanoMenu::oskClosePopup() {
    mOsk.miniOpen = false;
    mOsk.miniPopupIndex = -1;
    mOsk.focusRow = mOsk.miniOriginRow;
    mOsk.focusCol = mOsk.miniOriginCol;
    mDisplayDirty = true;
}

void NanoMenu::oskCommitPopupCell() {
    if (!mOsk.miniOpen || mOsk.miniPopupIndex < 0) return;
    const OskPopup& p = kOskPopups[mOsk.miniPopupIndex];
    int idx = mOsk.miniFocus;
    if (idx < 0 || idx >= p.keyCount) { oskClosePopup(); return; }
    const OskKey& cell = p.keys[idx];
    uint32_t cp = (uint32_t)cell.code;
    if (cell.caseFoldable && mOsk.shift != SHIFT_OFF) cp = toUpperCp(cp);
    oskClosePopup();
    if (cp > 0) {
        oskInsertCp(cp);
        if (mOsk.shift == SHIFT_ON) mOsk.shift = SHIFT_OFF;
    }
    mOsk.pressStartMs = nowMs();
}

// ---------------------------------------------------------------------------
// Key activation + A press/release + per-frame tick (long-press, animation)
// ---------------------------------------------------------------------------

void NanoMenu::oskActivateKey(const OskKey& key) {
    switch (key.code) {
        case OSK_SHIFT:       oskToggleShift(); return;
        case OSK_MODE_CHANGE: oskToggleSym();   return;
        case OSK_LEFT:        oskCaretLeft();   return;
        case OSK_RIGHT:       oskCaretRight();  return;
        case OSK_DELETE:      oskBackspace();   return;
        case OSK_CAPS_LOCK:   oskToggleCaps();  return;
        case OSK_VOICE:       return;
        default: break;
    }
    if (key.code > 0) {
        uint32_t cp = (uint32_t)key.code;
        if (key.caseFoldable && mOsk.shift != SHIFT_OFF) cp = toUpperCp(cp);
        else if (mOsk.shift != SHIFT_OFF && cp >= 0x3131 && cp <= 0x3163)
            cp = koreanShiftJamo(cp);   // dubeolsik tense/compound jamo
        oskInsertCp(cp);
        if (mOsk.shift == SHIFT_ON) mOsk.shift = SHIFT_OFF; // one-shot shift
        mOsk.pressStartMs = nowMs();
    }
}

void NanoMenu::oskAPress() {
    if (mOsk.miniOpen)        { oskCommitPopupCell(); return; }
    if (mOsk.inCandidateBar)  {
        if (mOsk.im && mOsk.candFocus >= 0) {
            OskBuffer buf{ &mOskQuery, &mOsk.caret };
            mOsk.im->chooseCandidate(mOsk.candFocus, buf);
            mOsk.candidates = mOsk.im->candidates();
            mOsk.inCandidateBar = mOsk.candidates.empty() ? false : mOsk.inCandidateBar;
            if (mOsk.candidates.empty()) mOsk.inCandidateBar = false;
            mOsk.candFocus = 0;
            if (!mOskPasswordMode) updateSearchResults();
            mDisplayDirty = true;
        }
        return;
    }
    if (mOsk.inAction)        { oskConfirm(); return; }
    if (mOsk.inLang)          { oskCycleLanguage(1); return; }

    const OskKeyboard* kb = oskCurrentKb();
    clampFocus(kb, mOsk.focusRow, mOsk.focusCol);
    const OskKey& key = kb->rows[mOsk.focusRow].keys[mOsk.focusCol];
    // Latch which key was under the cursor when A went down. Almost every letter
    // is a deferred key (commits on RELEASE so a hold can open the accent popup),
    // and fast typing interleaves the d-pad move toward the next key ahead of the
    // current key's A-release; committing "the key at release time" then types the
    // wrong or a stray extra letter. Commit the latched press-time key instead.
    mOsk.aPressRow = mOsk.focusRow;
    mOsk.aPressCol = mOsk.focusCol;
    bool deferred = (key.code > 0 && key.popupIndex >= 0 && key.glyph == GLYPH_NONE);
    if (deferred) {
        mOsk.aDownMs = nowMs();
        mOsk.aLongFired = false;
    } else {
        oskActivateKey(key);
        mOsk.aDownMs = 0;
        mOsk.aPressRow = -1; mOsk.aPressCol = -1;   // committed on press; no latch pending
    }
}

void NanoMenu::oskARelease() {
    if (mOsk.aDownMs != 0 && !mOsk.aLongFired && mOsk.aPressRow >= 0) {
        // Short tap on a popup-bearing key: commit the base character of the key
        // that was under the cursor when A was PRESSED (the latched position), not
        // wherever the focus has since drifted -- see oskAPress.
        const OskKeyboard* kb = oskCurrentKb();
        clampFocus(kb, mOsk.aPressRow, mOsk.aPressCol);
        oskActivateKey(kb->rows[mOsk.aPressRow].keys[mOsk.aPressCol]);
    }
    mOsk.aDownMs = 0;
    mOsk.aLongFired = false;
    mOsk.aPressRow = -1; mOsk.aPressCol = -1;
}

void NanoMenu::oskTick() {
    if (!mOskActive) return;
    if (mOsk.aDownMs != 0 && !mOsk.aLongFired && !mOsk.miniOpen && mOsk.aPressRow >= 0) {
        if (nowMs() - mOsk.aDownMs > kLongPressMs) {
            const OskKeyboard* kb = oskCurrentKb();
            // Open the popup for the key that was pressed (the latched position),
            // snapping the cursor back to it if a drift moved the focus mid-hold,
            // so the accent run appears over the intended key.
            clampFocus(kb, mOsk.aPressRow, mOsk.aPressCol);
            mOsk.focusRow = mOsk.aPressRow;
            mOsk.focusCol = mOsk.aPressCol;
            const OskKey& key = kb->rows[mOsk.focusRow].keys[mOsk.focusCol];
            if (key.popupIndex >= 0) {
                oskOpenPopup(key);
                mOsk.aLongFired = true;
            }
        }
    }

    // Pick up a pending clipboard paste (see oskPaste). The bridge echoes our request id to
    // clip_ready once it has written the text file, so we insert exactly the reply we asked for
    // and never a stale value. Give up quietly after a short timeout.
    if (mOskPastePending) {
        char ready[PROPERTY_VALUE_MAX] = {0};
        property_get("sys.gammaos.nano.clip_ready", ready, "");
        char want[32];
        snprintf(want, sizeof(want), "%ld", mOskPasteNonce);
        if (strcmp(ready, want) == 0) {
            mOskPastePending = false;
            std::string clip;
            FILE* f = fopen("/data/system/nano_clipboard.txt", "re");
            if (f) {
                char buf[1024];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) clip.append(buf, n);
                fclose(f);
            }
            if (!clip.empty()) oskInsertString(clip);
        } else if (nowMs() - mOskPasteReqMs > 1500) {
            mOskPastePending = false;   // bridge did not answer; drop the request
        }
    }
}

// ---------------------------------------------------------------------------
// Touchscreen input: raw digitizer -> panel-normalized -> logical hit-test.
// Called once per SYN_REPORT while the OSK is up on the secondary panel (on the
// primary its keys are touch zones, registered in renderOsk). The tuning members
// (mOskTouchSwap/FlipX/FlipY) are read from props by the caller so this stays
// free of the property API. Works for both DRM (rotated/flipped, corrected by
// the props) and SF (upright, defaults) back-ends.
// ---------------------------------------------------------------------------
void NanoMenu::oskTouchFrame() {
    if (!mOskActive) return;
    if (mTouchMaxX <= mTouchMinX || mTouchMaxY <= mTouchMinY || mTouchRawX < 0) return;
    float nx = (float)(mTouchRawX - mTouchMinX) / (float)(mTouchMaxX - mTouchMinX);
    float ny = (float)(mTouchRawY - mTouchMinY) / (float)(mTouchMaxY - mTouchMinY);
    if (mOskTouchSwap)  { float t = nx; nx = ny; ny = t; }
    if (mOskTouchFlipX) nx = 1.0f - nx;
    if (mOskTouchFlipY) ny = 1.0f - ny;
    if (nx < 0.0f) nx = 0.0f; else if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;
    float px = nx * (float)mWidth;
    float py = ny * (float)mHeight;
    bool tap = (mTouchDown && !mTouchWasDown);   // press edge only presses a key
    if (mTouchDown) oskTouchAt(px, py, tap);
    // A key that closed the keyboard: its release must not reach the screen behind.
    if (tap && (mOsk.closing || !mOskActive)) mTouchPressUsed = true;
    mTouchWasDown = mTouchDown;
}

void NanoMenu::oskTouchAt(float px, float py, bool tap) {
    if (!mOskActive || mOsk.miniOpen || mOsk.inCandidateBar) return;
    OskBox b = oskLayoutBox();
    float tx, ty, tw, th;
    oskToggleRect(b, tx, ty, tw, th);
    if (px >= tx && px <= tx + tw && py >= ty && py <= ty + th) {
        if (tap) oskSetTouch(!mOsk.touch);
        return;
    }
    // Action button (Done / Search).
    if (px >= b.actX && px <= b.actX + b.actW && py >= b.actY && py <= b.actY + b.actH) {
        mOsk.inAction = true; mOsk.inLang = false;
        mDisplayDirty = true;
        if (tap) oskConfirm();
        return;
    }
    if (px >= b.langX && px <= b.langX + b.langW && py >= b.langY && py <= b.langY + b.langH) {
        mOsk.inLang = true; mOsk.inAction = false;
        mDisplayDirty = true;
        if (tap) oskCycleLanguage(1);
        return;
    }
    // Grid keys: on the press edge press the key under the finger; a held slide
    // just moves the focus so the user can correct before lifting.
    const OskKeyboard* kb = oskCurrentKb();
    for (int r = 0; r < kb->rowCount; r++) {
        const OskRow& rr = kb->rows[r];
        for (int c = 0; c < rr.keyCount; c++) {
            const OskKey& k = rr.keys[c];
            float kx, ky, kw, kh;
            keyRect(b, k.fx, k.fw, rr.fy, rr.fh, kx, ky, kw, kh);
            if (px >= kx && px <= kx + kw && py >= ky && py <= ky + kh) {
                mOsk.inAction = false; mOsk.inLang = false;
                mOsk.focusRow = r;
                mOsk.focusCol = c;
                mDisplayDirty = true;
                if (tap) oskActivateKey(k);
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Navigation: geometric nearest-in-direction
// ---------------------------------------------------------------------------

void NanoMenu::oskMoveCursor(NavDir dir) {
    if (!mOskActive) return;

    // Popup: navigate within the run; leaving it dismisses.
    if (mOsk.miniOpen) {
        const OskPopup& p = kOskPopups[mOsk.miniPopupIndex];
        if (dir == NavDir::Left) {
            if (mOsk.miniFocus > 0) mOsk.miniFocus--; else oskClosePopup();
        } else if (dir == NavDir::Right) {
            if (mOsk.miniFocus < p.keyCount - 1) mOsk.miniFocus++; else oskClosePopup();
        } else {
            oskClosePopup();
        }
        mDisplayDirty = true;
        return;
    }

    // Candidate bar (Phase B+).
    if (mOsk.inCandidateBar) {
        int n = (int)mOsk.candidates.size();
        if (dir == NavDir::Left)  { if (mOsk.candFocus > 0) mOsk.candFocus--; }
        else if (dir == NavDir::Right) { if (mOsk.candFocus < n - 1) mOsk.candFocus++; }
        else if (dir == NavDir::Down)  { mOsk.inCandidateBar = false; }
        mDisplayDirty = true;
        return;
    }

    const OskKeyboard* kb = oskCurrentKb();
    clampFocus(kb, mOsk.focusRow, mOsk.focusCol);
    OskBox b = oskLayoutBox();

    // Action button focus.
    if (mOsk.inAction) {
        if (dir == NavDir::Left) {
            mOsk.inAction = false;
        } else if (dir == NavDir::Down) {
            mOsk.inAction = false; mOsk.inLang = true;
        } else if (dir == NavDir::Up && !mOsk.candidates.empty()) {
            mOsk.inAction = false; mOsk.inCandidateBar = true; mOsk.candFocus = 0;
        }
        mDisplayDirty = true;
        return;
    }
    // Language button focus (below the action button).
    if (mOsk.inLang) {
        if (dir == NavDir::Left) mOsk.inLang = false;
        else if (dir == NavDir::Up) { mOsk.inLang = false; mOsk.inAction = true; }
        mDisplayDirty = true;
        return;
    }

    const OskRow& row = kb->rows[mOsk.focusRow];
    const OskKey& cur = row.keys[mOsk.focusCol];
    float cx, cy, cw, ch;
    keyRect(b, cur.fx, cur.fw, row.fy, row.fh, cx, cy, cw, ch);
    float ccx = cx + cw / 2.0f, ccy = cy + ch / 2.0f;
    bool curSpace = (cur.code == 32);

    float probeX = ccx, probeY = ccy;
    if (dir == NavDir::Left) {
        if (cur.edgeLeft) return;                       // no wrap at left edge
        probeX = cx - ch * 0.5f; probeY = ccy;
    } else if (dir == NavDir::Right) {
        if (cur.edgeRight) { mOsk.inAction = true; mDisplayDirty = true; return; }
        probeX = cx + cw + ch * 0.5f; probeY = ccy;
    } else if (dir == NavDir::Up) {
        probeY = ccy - ch * 1.25f;
        probeX = (curSpace && mOsk.rememberedX >= 0) ? mOsk.rememberedX : ccx;
        if (probeY < b.kbY && !mOsk.candidates.empty()) {
            mOsk.inCandidateBar = true; mOsk.candFocus = 0; mDisplayDirty = true; return;
        }
    } else { // Down
        probeY = ccy + ch * 1.25f;
        probeX = (curSpace && mOsk.rememberedX >= 0) ? mOsk.rememberedX : ccx;
    }

    // Nearest key center to the probe point, constrained to the move side
    // for vertical moves so we never re-select within the same row.
    int bestR = mOsk.focusRow, bestC = mOsk.focusCol;
    float bestD = 1e18f;
    for (int r = 0; r < kb->rowCount; r++) {
        const OskRow& rr = kb->rows[r];
        for (int c = 0; c < rr.keyCount; c++) {
            if (r == mOsk.focusRow && c == mOsk.focusCol) continue;
            const OskKey& k = rr.keys[c];
            float kx, ky, kw, kh;
            keyRect(b, k.fx, k.fw, rr.fy, rr.fh, kx, ky, kw, kh);
            float kcx = kx + kw / 2.0f, kcy = ky + kh / 2.0f;
            if (dir == NavDir::Up   && kcy >= ccy - 1.0f) continue;
            if (dir == NavDir::Down && kcy <= ccy + 1.0f) continue;
            float dx = kcx - probeX, dy = kcy - probeY;
            float d = dx * dx + dy * dy;
            if (d < bestD) { bestD = d; bestR = r; bestC = c; }
        }
    }
    mOsk.focusRow = bestR;
    mOsk.focusCol = bestC;

    // Remember the column for vertical travel across the space bar.
    const OskKey& nk = kb->rows[bestR].keys[bestC];
    if (nk.code != 32) {
        float nx, ny, nw, nh;
        keyRect(b, nk.fx, nk.fw, kb->rows[bestR].fy, kb->rows[bestR].fh, nx, ny, nw, nh);
        mOsk.rememberedX = nx + nw / 2.0f;
    }
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Lifecycle: open / close / confirm / password
// ---------------------------------------------------------------------------

void NanoMenu::openOsk() {
    mOskActive = true;
    mOskGlassValid = false;   // re-capture the frosted panel on open
    mOskPasswordMode = false;
    mOskPlaintext = false;
    mOskPasswordPrompt.clear();
    mOskPasswordCallback = nullptr;
    mOskFieldFmt = 0;
    mOskQuery.clear();
    mOsk.resetForOpen();
    mOsk.touch = mLastInputTouch;
    oskApplyLocale();
    mOsk.caret = 0;
    mSearchResults.clear();
    mSearchSelectedIndex = 0;
    mSearchActive = false;
    mDisplayDirty = true;
}

void NanoMenu::closeOsk() {
    if (mOsk.miniOpen) { oskClosePopup(); mDisplayDirty = true; return; }
    if (mOsk.im) { mOsk.im->reset(); mOsk.candidates.clear(); }
    mOsk.closing = true;   // fade out (renderOsk finalizes mOskActive=false)
    if (mOskPasswordMode) {
        mOskPasswordMode = false;
        mOskPlaintext = false;
        mOskPasswordPrompt.clear();
        mOskPasswordCallback = nullptr;
        mOskQuery.clear();
        mOsk.caret = 0;
        mDisplayDirty = true;
        // Cancelling a wizard text field returns to the previous wizard screen
        // (e.g. the WPA key cancels back to the access-point list), rather than
        // stranding the user on a blank field. wizBack reopens the OSK if the
        // previous screen is itself a text field.
        if (mPs3WizActive) wizBack();
        return;
    }
    if (mOskQuery.empty()) mSearchActive = false;
    mDisplayDirty = true;
}

void NanoMenu::oskConfirm() {
    if (mOsk.im) {
        OskBuffer buf{ &mOskQuery, &mOsk.caret };
        mOsk.im->commitComposing(buf);
        mOsk.im->reset();
        mOsk.candidates.clear();
        mOsk.inCandidateBar = false;
    }
    mOsk.closing = true;   // fade out (renderOsk finalizes mOskActive=false)
    // Run the registered submit callback for ANY OSK opened via openOskForPassword
    // (wizard text fields), masked or not. Keying off mOskPasswordMode meant
    // non-masked fields (SSID, IP, the Set Manually date/time) fell through to the
    // search path and never advanced the wizard; mOskPasswordMode only controls
    // visual masking, not whether a callback is owed.
    if (mOskPasswordCallback) {
        auto cb = std::move(mOskPasswordCallback);
        std::string pw = mOskQuery;
        mOskPasswordMode = false;
        mOskPlaintext = false;
        mOskPasswordPrompt.clear();
        mOskPasswordCallback = nullptr;
        mOskQuery.clear();
        mOsk.caret = 0;
        mDisplayDirty = true;
        if (cb) cb(pw);
        return;
    }
    if (!mOskQuery.empty()) {
        mSearchActive = true;
        mSearchSelectedIndex = 0;
        updateSearchResults();
    } else {
        mSearchActive = false;
    }
    mDisplayDirty = true;
}

void NanoMenu::openOskForPassword(const std::string& prompt,
                                  std::function<void(const std::string&)> onSubmit) {
    mOskPasswordMode = true;
    mOskPlaintext = false;
    mOskPasswordPrompt = prompt;
    mOskPasswordCallback = std::move(onSubmit);
    mOskFieldFmt = 0;          // wizOpenTextField sets this for date/time fields
    mOskQuery.clear();
    mOsk.resetForOpen();
    mOsk.touch = mLastInputTouch;
    oskApplyLocale();
    mOsk.caret = 0;
    mOskActive = true;
    mOskGlassValid = false;   // re-capture the frosted panel on open
    mDisplayDirty = true;
}

std::string NanoMenu::maskPassword(const std::string& s) {
    // One mask glyph per codepoint (not per byte) so multibyte input masks 1:1.
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) out += '*';
    return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void NanoMenu::renderOsk() {
    if (!mOskActive) return;
    // The keyboard always uses crisp GL_LINEAR text, even when it sits over an
    // anti-aliased home-XMB menu or setup-wizard step (whichever drew just
    // before this in render()). Mip-AA softens the small key glyphs, so keep it
    // off here regardless of the caller's state.
    setGlyphAtlasAA(false);
    // Minima OSK: flat text (no shadow/outline) to match the theme; restored before every return.
    const int oskPrevOutline = mTextOutlineMode; if (mMinimaTheme) mTextOutlineMode = 2;
    oskTick();   // long-press popup + animation clock (render runs every frame)

    // --- Show/hide fade ---
    float decay = 1.0f - expf(-16.0f * mFrameDt);
    if (mOsk.closing) {
        mOsk.anim += (0.0f - mOsk.anim) * decay;
        if (mOsk.anim < 0.02f) {           // fully hidden -> finalize
            mOskActive = false; mOsk.closing = false; mOsk.anim = 0.0f;
            mTextOutlineMode = oskPrevOutline;
            return;
        }
    } else {
        mOsk.anim += (1.0f - mOsk.anim) * decay;
        if (mOsk.anim > 0.999f) mOsk.anim = 1.0f;
    }
    const float fade = mOsk.anim;

    OskBox b = oskLayoutBox();
    const OskKeyboard* kb = oskCurrentKb();
    clampFocus(kb, mOsk.focusRow, mOsk.focusCol);

    // Touch zones: only for the keyboard in the primary panel's frame (the secondary one keeps
    // oskTouchFrame), and none while the PSP clock is up. The keyboard is modal: nothing behind it
    // is reached, and a tap off the accent popup closes it. Closing, it takes no key presses.
    const bool zones = mRenderingPanel == 0 && !mOskOnSecondary && !(mPspClockOn || mPspClockReveal > 0.0f);
    const bool keyZones = zones && !mOsk.closing;
    if (zones) {
        mTouch.block(TouchRect{0.0f, 0.0f, (float)mWidth, (float)mHeight}, [this]() {
            if (mOskActive && mOsk.miniOpen) { oskClosePopup(); mDisplayDirty = true; }
        });
    }

    // --- Palette (osk.png-inspired dark glass) ---
    const float panelRad = 20.0f * b.sf;
    const float keyRad   = 9.0f * b.sf;
    const float keyGap   = 3.0f * b.sf;     // inset of the key cap inside its cell
    // key cap fill
    float kBgR = 0.16f, kBgG = 0.19f, kBgB = 0.24f; const float kBgA = 1.0f;
    // light text on dark keys; dark text on the white focused key
    float kTxtR = 0.88f, kTxtG = 0.90f, kTxtB = 0.95f;
    float kFocTxtR = 0.08f, kFocTxtG = 0.10f, kFocTxtB = 0.14f;
    // accent (action / active shift)
    float accR = 0.27f, accG = 0.52f, accB = 0.96f;
    // Minima OSK skin: dark neutral caps, WHITE unselected glyphs, BLACK glyph on the white focused
    // cap (the focused-cap fill is already near-white at the draw sites), accent = the Colour setting.
    float oskAtc = 1.0f;   // legible text on the accent (action button), hoisted for the draw sites
    if (mMinimaTheme) {
        float ar, ag, ab; minimaAccent(ar, ag, ab);
        const float accLum = 0.299f * ar + 0.587f * ag + 0.114f * ab;
        oskAtc = (accLum > 0.62f) ? 0.0f : 1.0f;
        kBgR = 0.10f; kBgG = 0.10f; kBgB = 0.12f;
        kTxtR = 1.0f;  kTxtG = 1.0f;  kTxtB = 1.0f;      // white unselected glyphs
        kFocTxtR = 0.0f; kFocTxtG = 0.0f; kFocTxtB = 0.0f;  // black on the white focused cap
        accR = ar; accG = ag; accB = ab;
    }

    // --- Frosted-glass panel ---
    // The panel NEVER re-captures the framebuffer while open any more: the old
    // ~15Hz captureGlass() refresh lagged the 60fps wave behind it (and cost a
    // ~20ms mid-frame tile resolve per capture), which read as flicker inside
    // the panel. Instead, reuse whichever live wave-space blur the scene
    // already maintains (wizard or submenu frost, both land in mGlassBlurTex),
    // and only fall back to a single frozen capture on the legacy menu path.
    bool drewGlass = false;
    // The reused wave-space blur (mGlassBlurTex) is the XMB/submenu backdrop. In the
    // Now-Playing music player the visible background is the purple Waves morph, which
    // never refreshes that blur, so reusing it shows a green frost over a purple screen.
    // Exclude mMpActive so the player's OSK captures the current (purple) framebuffer
    // via the capture path below instead.
    if (frostBufferSharedWithClock()) {
        // Dual-screen BOTTOM panel with the PSP clock: the clock owns the shared mGlassBlurTex (it writes
        // its glow there each frame just before the OSK draws) and the visible panel scene includes the
        // clock itself, so neither reusing the blur nor a framebuffer capture yields a clean, stable
        // backdrop - both showed the clock's glow through the keys and flickered with the clock's cache.
        // Capture the WAVE directly so the OSK frosts the wallpaper/wave behind it (not the clock), fresh
        // and identical every frame. The clock cache is disabled while the OSK is up (see bcCap), so the
        // wave work texture is rebuilt every frame here.
        captureGlassFromWave();
        drawFrostedGlass(b.panelX, b.panelY, b.panelW, b.panelH, panelRad,
                         0.50f, 0.54f, 0.64f, 1.0f, fade, true);
        drewGlass = true;
    } else if (((mPs3WizActive && mPs3DlgBlurValid) || (mPs3Xmb && mPs3GlassValid)) && !mMpActive && !mMinimaTheme) {
        // Wizard fields hold a fresh dialog blur; every other PS3-path OSK
        // (the Game Systems editor) opens over a submenu, whose full-screen
        // frost refreshes the same wave-space blur each frame. Reusing it is
        // free and keeps the panel content in lockstep with the backdrop.
        // EXCLUDE Minima: its backdrop is the flat black/wallpaper secondary
        // (renderMinimaSecondary), never the wave-space blur, so reusing that
        // blur would frost the stale XMB wave behind the keyboard. Fall through
        // to the capture path so the OSK frosts the actual Minima content.
        drawFrostedGlass(b.panelX, b.panelY, b.panelW, b.panelH, panelRad,
                         0.50f, 0.54f, 0.64f, 1.0f, fade, true);
        drewGlass = true;
    } else if (!mOverlayMode && !mMinimaTheme) {
        // Legacy menu path: capture ONCE per open and keep the frozen frost
        // (a static panel cannot flicker; the periodic re-capture could). EXCLUDE Minima:
        // its flat-black canvas should get the solid dark panel below, not a wave-space frost.
        if (!mOskGlassValid && captureGlass(b.panelX, b.panelY, b.panelW, b.panelH)) {
            mOskGlassValid = true; mOskGlassT = mEffectTime;
        }
        if (mOskGlassValid) {
            drawFrostedGlass(b.panelX, b.panelY, b.panelW, b.panelH, panelRad,
                             0.50f, 0.54f, 0.64f, 1.0f, fade);
            drewGlass = true;
        }
    }
    if (!drewGlass) {
        // Overlay scrim mode lands here: nano renders on a TRANSLUCENT SF
        // layer, so a capture would just blur the scrim's alpha and let the
        // running app shimmer through the panel. Draw the solid dark panel
        // fully opaque on the layer instead (there is nothing to frost - the
        // app's pixels are composited by SF, not present in our framebuffer).
        if (mMinimaTheme) {
            // Minima: solid dark card + an accent top rule, echoing the Minima dialog/side panel.
            drawRoundedRect(b.panelX, b.panelY, b.panelW, b.panelH, panelRad,
                            0.08f, 0.08f, 0.10f, (mOverlayMode ? 1.0f : 0.96f) * fade);
            drawRoundedRect(b.panelX, b.panelY, b.panelW, fmaxf(2.0f, 4.0f * b.sf), panelRad,
                            accR, accG, accB, fade);
        } else {
            drawRoundedRect(b.panelX, b.panelY, b.panelW, b.panelH, panelRad,
                            0.12f, 0.14f, 0.18f, (mOverlayMode ? 1.0f : 0.92f) * fade);
        }
    }

    // --- Candidate bar (Phase B+): only when an engine produced candidates. ---
    if (!mOsk.candidates.empty()) {
        float chipScale = 1.7f * b.sf;
        float cx = b.previewX;
        float cy = b.panelY + 4.0f * b.sf;
        for (int i = 0; i < (int)mOsk.candidates.size() && cx < b.panelX + b.panelW; i++) {
            const std::string& cand = mOsk.candidates[i];
            float tw = measureText(cand.c_str(), chipScale);
            bool sel = (mOsk.inCandidateBar && i == mOsk.candFocus);
            if (sel)
                drawRoundedRect(cx - 6.0f * b.sf, cy - 3.0f * b.sf,
                                tw + 12.0f * b.sf, FONT_CHAR_H * chipScale + 6.0f * b.sf,
                                7.0f * b.sf, 1.0f, 1.0f, 1.0f, 0.92f * fade);
            drawText(cand.c_str(), cx, cy, chipScale,
                     sel ? kFocTxtR : accR, sel ? kFocTxtG : accG,
                     sel ? kFocTxtB : accB, fade);
            if (keyZones && !mOsk.miniOpen) {
                mTouch.tap(TouchRect{cx - 6.0f * b.sf, cy - 3.0f * b.sf,
                                     tw + 12.0f * b.sf, FONT_CHAR_H * chipScale + 6.0f * b.sf},
                           [this, i, cand]() {
                    if (!mOskActive || mOsk.closing || mOsk.miniOpen
                        || i >= (int)mOsk.candidates.size() || mOsk.candidates[i] != cand) return;
                    mOsk.inAction = false; mOsk.inLang = false;
                    mOsk.inCandidateBar = true; mOsk.candFocus = i;
                    oskAPress();
                    mDisplayDirty = true;
                });
            }
            cx += tw + 18.0f * b.sf;
        }
    }

    // --- Text preview / query line with caret ---
    // WIZARD text fields are edited like a real IME: the value + caret are
    // drawn into the actual dialog field, so the keyboard's own preview line is
    // skipped for them. Every other entry shows the line: the free search OSK
    // keeps its "Search:" preview, and field editors outside the wizard (the
    // Game Systems editor) show the field name plus the CURRENT value the
    // opener prefilled, so editing starts from the existing text and the user
    // sees what they are modifying.
    if (!(mOskPasswordCallback && mPs3WizActive)) {
        std::string label, value;
        float pr, pg, pb;
        if (mOskPasswordCallback || mOskPasswordMode) {
            if (b.promptLine) {
                // Guidance on its own line, the typed text alone on the next.
                drawText(trDyn(mOskPasswordPrompt.c_str()), b.previewX, b.promptY, 1.5f * b.sf,
                         1.0f, 0.78f, 0.40f, 0.9f * fade);
                label = "";
            } else {
                label = std::string(mOskPasswordPrompt.empty() ? trDyn("Text") : trDyn(mOskPasswordPrompt.c_str())) + ": ";
            }
            value = (mOskPasswordMode && !mOskPlaintext) ? maskPassword(mOskQuery)
                                                         : mOskQuery;
            pr = 1.0f; pg = 0.78f; pb = 0.40f;
        } else {
            label = std::string(trDyn("Search")) + ": ";
            value = mOskQuery;
            pr = 0.45f; pg = 0.78f; pb = 1.0f;
        }
        std::string composing = mOsk.im ? mOsk.im->composingText() : std::string();
        float ps = 2.0f * b.sf;
        float y = b.previewY;
        float blink = 0.55f + 0.45f * sinf((float)nowMs() * 0.006f);
        drawText(label.c_str(), b.previewX, y, ps, pr, pg, pb, fade);
        float lw = measureText(label.c_str(), ps);
        // Branch on the CONTENT direction, not the keyboard's: digits/Latin
        // typed on the Arabic/Hebrew keyboard read left-to-right and belong in
        // the LTR path (caret at the insertion point), matching the wizard
        // fields. An empty value on an RTL keyboard starts right-aligned so the
        // first letter lands on the right.
        if (nanoTextIsRtl(value.c_str())
            || (value.empty() && mOsk.dir == OSK_RTL)) {
            // Right-to-left: drawText shapes Arabic and lays the string out in
            // visual order itself now, so hand it the LOGICAL buffer; this
            // branch only right-aligns it and keeps the caret at the visual
            // left, the logical end where the next letter lands. Direct RTL
            // scripts install no composing engine, so there is no composing
            // segment to append here.
            float vw = measureText(value.c_str(), ps);
            float rightEdge = b.panelX + b.panelW - 70.0f * b.sf; // room for badge
            float minx = b.previewX + lw + 12.0f * b.sf;
            float vx = rightEdge - vw;
            if (vx < minx) vx = minx;
            drawText(value.c_str(), vx, y, ps, 1.0f, 1.0f, 1.0f, fade);
            drawQuad(vx - 4.0f * b.sf, y, 2.0f * b.sf, FONT_CHAR_H * ps,
                     1.0f, 1.0f, 1.0f, blink * fade);
        } else if (nanoTextHasRtl(value.c_str())) {
            // LTR-first value with an embedded RTL run (e.g. a prefilled name
            // being edited): draw the WHOLE value in one call so drawText's
            // bidi lays the run out correctly, and end-anchor the caret. The
            // split+window path below would bidi-transform each half of the
            // value independently and garble the RTL run as the caret moves.
            float x = b.previewX + lw;
            drawText(value.c_str(), x, y, ps, 1.0f, 1.0f, 1.0f, fade);
            float vw = measureText(value.c_str(), ps);
            if (!composing.empty()) {
                drawText(composing.c_str(), x + vw, y, ps, 0.6f, 0.9f, 1.0f, fade);
                vw += measureText(composing.c_str(), ps);
            }
            drawQuad(x + vw + 2.0f * b.sf, y, 2.0f * b.sf, FONT_CHAR_H * ps,
                     1.0f, 1.0f, 1.0f, blink * fade);
        } else {
            int caret = mOsk.caret;
            if (caret < 0) caret = 0;
            if (caret > (int)value.size()) caret = (int)value.size();
            // Slide a window over values wider than the panel (long intent
            // templates, package names): drop whole codepoints from the far
            // ends until it fits, always keeping the caret in view.
            float availW = (b.panelX + b.panelW - 70.0f * b.sf)
                         - (b.previewX + lw);
            int winStart = 0, winEnd = (int)value.size();
            if (availW > 40.0f * b.sf
                && measureText(value.c_str(), ps) > availW) {
                auto width = [&](int s, int e) {
                    return measureText(
                        value.substr((size_t)s, (size_t)(e - s)).c_str(), ps);
                };
                while (winStart < caret && width(winStart, caret) > availW * 0.8f)
                    winStart = utf8NextStart(value, winStart);
                while (winEnd > caret && width(winStart, winEnd) > availW)
                    winEnd = utf8PrevStart(value, winEnd);
                while (winStart < caret && width(winStart, winEnd) > availW)
                    winStart = utf8NextStart(value, winStart);
            }
            std::string before = value.substr((size_t)winStart,
                                              (size_t)(caret - winStart));
            std::string after = value.substr((size_t)caret,
                                             (size_t)(winEnd - caret));
            float x = b.previewX + lw;
            if (!before.empty()) {
                drawText(before.c_str(), x, y, ps, 1.0f, 1.0f, 1.0f, fade);
                x += measureText(before.c_str(), ps);
            }
            if (!composing.empty()) {
                drawText(composing.c_str(), x, y, ps, 0.6f, 0.9f, 1.0f, fade);
                float uw = measureText(composing.c_str(), ps);
                drawQuad(x, y + FONT_CHAR_H * ps, uw, 2.0f * b.sf, 0.6f, 0.9f, 1.0f, 0.9f * fade);
                x += uw;
            }
            drawQuad(x, y, 2.0f * b.sf, FONT_CHAR_H * ps, 1.0f, 1.0f, 1.0f, blink * fade);
            if (!after.empty())
                drawText(after.c_str(), x + 2.0f * b.sf, y, ps, 1.0f, 1.0f, 1.0f, fade);
        }
    }

    // Helper: draw a single key cap (rounded) + its label/glyph.
    auto drawKeyCap = [&](const OskKey& key, float x, float y, float w, float h,
                          bool focused, float dim) {
        float bx = x + keyGap, by = y + keyGap, bw = w - 2 * keyGap, bh = h - 2 * keyGap;
        bool isAccentShift = (key.code == OSK_SHIFT && mOsk.shift != SHIFT_OFF);
        // cap fill (remember the colour so the backspace icon's cut-out matches)
        float capR, capG, capB, capA;
        if (focused)            { capR = 0.96f; capG = 0.97f; capB = 0.99f; capA = 0.97f; }
        else if (isAccentShift) { capR = accR;  capG = accG;  capB = accB;  capA = 0.85f; }
        else                    { capR = kBgR;  capG = kBgG;  capB = kBgB;  capA = kBgA; }
        drawRoundedRect(bx, by, bw, bh, keyRad, capR, capG, capB, capA * dim * fade);
        // glyph for the space bar: a slim rounded bar
        if (key.glyph == GLYPH_SPACE) {
            float barW = bw * 0.5f, barH = 3.0f * b.sf;
            float tc = focused ? kFocTxtR : kTxtR;
            drawRoundedRect(bx + (bw - barW) / 2.0f, by + bh * 0.60f, barW, barH,
                            barH * 0.5f, tc, tc, tc, 0.9f * dim * fade);
            return;
        }
        std::string text;
        float scale;
        float tr, tg, tb;
        if (key.glyph != GLYPH_NONE) {
            text = (key.glyph == GLYPH_DELETE) ? std::string("\xc3\x97") // U+00D7 multiply -> backspace
                 : glyphFor(key.glyph, mOsk.shift);
            bool isMode = (key.glyph == GLYPH_SYMBOLS || key.glyph == GLYPH_ALPHABET);
            scale = (isMode ? 1.35f : 1.7f) * b.ks;
            if (isAccentShift) { tr = 1.0f; tg = 1.0f; tb = 1.0f; }
            else if (focused)  { tr = kFocTxtR; tg = kFocTxtG; tb = kFocTxtB; }
            else               { tr = kTxtR; tg = kTxtG; tb = kTxtB; }
        } else {
            text = key.label ? key.label : "";
            if (key.caseFoldable && mOsk.shift != SHIFT_OFF) text = utf8Upper(text);
            scale = (utf8Len(text) >= 2 ? 1.35f : 1.85f) * b.ks;
            if (focused) { tr = kFocTxtR; tg = kFocTxtG; tb = kFocTxtB; }
            else         { tr = kTxtR; tg = kTxtG; tb = kTxtB; }
        }
        // Real backspace icon: left-pointing arrowhead + body + a cut-out X.
        if (key.glyph == GLYPH_DELETE) {
            float cx = bx + bw / 2.0f, cy = by + bh / 2.0f, s = bh * 0.30f;
            drawTriangle(cx - s * 0.95f, cy, cx - s * 0.15f, cy - s * 0.78f,
                         cx - s * 0.15f, cy + s * 0.78f, tr, tg, tb, dim * fade);
            drawRoundedRect(cx - s * 0.15f, cy - s * 0.55f, s * 1.15f, s * 1.10f,
                            s * 0.22f, tr, tg, tb, dim * fade);
            float xs = 0.85f * b.ks;
            float xw = measureText("\xc3\x97", xs);
            drawText("\xc3\x97", cx + s * 0.18f - xw / 2.0f,
                     cy - FONT_CHAR_H * xs / 2.0f, xs, capR, capG, capB, dim * fade);
            return;
        }
        float tw = measureText(text.c_str(), scale);
        float th = FONT_CHAR_H * scale;
        drawText(text.c_str(), bx + (bw - tw) / 2.0f, by + (bh - th) / 2.0f,
                 scale, tr, tg, tb, dim * fade);
        // number-row superscript (US-style shifted symbol on digit keys)
        if (key.code >= '0' && key.code <= '9' && key.glyph == GLYPH_NONE) {
            static const char kSup[] = ")!@#$%^&*(";
            char sup[2] = { kSup[key.code - '0'], 0 };
            float ss = 1.0f * b.ks;
            float sw = measureText(sup, ss);
            drawText(sup, bx + bw - sw - 4.0f * b.sf, by + 3.0f * b.sf, ss,
                     focused ? kFocTxtR : 0.60f, focused ? kFocTxtG : 0.64f,
                     focused ? kFocTxtB : 0.72f, 0.85f * dim * fade);
        }
    };

    // --- Key caps (dark rounded) ---
    float dimAll = mOsk.miniOpen ? 0.28f : 1.0f;
    // The touch keyboard has no focus; the key under the finger lights up instead.
    float fingerX = -1.0f, fingerY = -1.0f;
    if (b.touch && mTouchDown && !touchMapRaw(mTouchRawX, mTouchRawY, fingerX, fingerY)) fingerX = fingerY = -1.0f;
    for (int r = 0; r < kb->rowCount; r++) {
        const OskRow& row = kb->rows[r];
        for (int c = 0; c < row.keyCount; c++) {
            const OskKey& key = row.keys[c];
            float x, y, w, h;
            keyRect(b, key.fx, key.fw, row.fy, row.fh, x, y, w, h);
            bool focused = b.touch
                ? (!mOsk.miniOpen && fingerX >= x && fingerX < x + w && fingerY >= y && fingerY < y + h)
                : (!mOsk.inAction && !mOsk.inLang && !mOsk.miniOpen &&
                   r == mOsk.focusRow && c == mOsk.focusCol);
            drawKeyCap(key, x, y, w, h, focused, dimAll);
            if (keyZones && !mOsk.miniOpen) {
                // Tap types the key; a long press opens its accent popup.
                auto onKey = [this, kb, r, c](bool hold) {
                    if (!mOskActive || mOsk.closing || mOsk.miniOpen || oskCurrentKb() != kb) return;
                    mOsk.inAction = false; mOsk.inLang = false; mOsk.inCandidateBar = false;
                    mOsk.focusRow = r; mOsk.focusCol = c;
                    if (hold) oskOpenPopup(kb->rows[r].keys[c]);
                    else      oskActivateKey(kb->rows[r].keys[c]);
                    mDisplayDirty = true;
                };
                mTouch.tap(TouchRect{x, y, w, h}, [onKey]() { onKey(false); },
                           key.popupIndex >= 0 ? TouchZones::Action([onKey]() { onKey(true); })
                                               : TouchZones::Action());
            }
        }
    }

    // --- Popup overlay (drawn on top of the dimmed grid) ---
    if (mOsk.miniOpen) {
        const OskPopup& p = kOskPopups[mOsk.miniPopupIndex];
        const OskRow& orow = kb->rows[mOsk.miniOriginRow];
        for (int i = 0; i < p.keyCount; i++) {
            const OskKey& cell = p.keys[i];
            float col = (float)(mOsk.miniBaseCol + i);
            float fx = col * (kKeyFrac + 0.028037f);
            float x, y, w, h;
            keyRect(b, fx, cell.fw, orow.fy, orow.fh, x, y, w, h);
            float bx = x + keyGap, by = y + keyGap, bw = w - 2 * keyGap, bh = h - 2 * keyGap;
            bool sel = (i == mOsk.miniFocus);
            if (keyZones) {
                mTouch.tap(TouchRect{x, y, w, h}, [this, popup = mOsk.miniPopupIndex, i]() {
                    if (!mOskActive || mOsk.closing || !mOsk.miniOpen || mOsk.miniPopupIndex != popup) return;
                    mOsk.miniFocus = i;
                    oskCommitPopupCell();
                    mDisplayDirty = true;
                });
            }
            if (sel) drawRoundedRect(bx, by, bw, bh, keyRad, 0.96f, 0.97f, 0.99f, 0.97f * fade);
            else     drawRoundedRect(bx, by, bw, bh, keyRad, 0.20f, 0.24f, 0.30f, 0.96f * fade);
            std::string text = cell.label ? cell.label : "";
            if (cell.caseFoldable && mOsk.shift != SHIFT_OFF) text = utf8Upper(text);
            float scale = 1.85f * b.ks;
            float tw = measureText(text.c_str(), scale);
            float th = FONT_CHAR_H * scale;
            if (sel) drawText(text.c_str(), bx + (bw - tw) / 2.0f, by + (bh - th) / 2.0f,
                              scale, kFocTxtR, kFocTxtG, kFocTxtB, fade);
            else     drawText(text.c_str(), bx + (bw - tw) / 2.0f, by + (bh - th) / 2.0f,
                              scale, kTxtR, kTxtG, kTxtB, fade);
        }
    }

    // --- Action button (Enter / Search): accent rounded key ---
    {
        const char* actLabel = trDyn(mOskPasswordCallback ? "Enter" : "Search");
        float scale = 1.6f * b.sf;
        bool foc = mOsk.inAction && !b.touch;
        if (foc) drawRoundedRect(b.actX, b.actY, b.actW, b.actH, keyRad,
                                 0.96f, 0.97f, 0.99f, 0.97f * fade);
        else     drawRoundedRect(b.actX, b.actY, b.actW, b.actH, keyRad,
                                 accR, accG, accB, 0.92f * fade);
        float tw = measureText(actLabel, scale);
        float th = FONT_CHAR_H * scale;
        if (foc) drawText(actLabel, b.actX + (b.actW - tw) / 2.0f, b.actY + (b.actH - th) / 2.0f,
                          scale, kFocTxtR, kFocTxtG, kFocTxtB, fade);
        else     drawText(actLabel, b.actX + (b.actW - tw) / 2.0f, b.actY + (b.actH - th) / 2.0f,
                          scale, oskAtc, oskAtc, oskAtc, fade);   // legible on a light Minima accent
        if (keyZones && !mOsk.miniOpen) {
            mTouch.tap(TouchRect{b.actX, b.actY, b.actW, b.actH}, [this]() {
                if (!mOskActive || mOsk.closing || mOsk.miniOpen) return;
                mOsk.inAction = true; mOsk.inLang = false; mOsk.inCandidateBar = false;
                oskConfirm();
                mDisplayDirty = true;
            });
        }
    }

    // --- Language button (below the action button): the active language; A cycles it ---
    {
        float scale = 1.6f * b.sf;
        bool foc = mOsk.inLang && !b.touch;
        if (foc) drawRoundedRect(b.langX, b.langY, b.langW, b.langH, keyRad,
                                 0.96f, 0.97f, 0.99f, 0.97f * fade);
        else     drawRoundedRect(b.langX, b.langY, b.langW, b.langH, keyRad,
                                 kBgR, kBgG, kBgB, kBgA * fade);
        float tw = measureText(mOsk.langLabel, scale);
        float th = FONT_CHAR_H * scale;
        float lr = foc ? kFocTxtR : kTxtR, lg = foc ? kFocTxtG : kTxtG, lb = foc ? kFocTxtB : kTxtB;
        drawText(mOsk.langLabel, b.langX + (b.langW - tw) / 2.0f, b.langY + (b.langH - th) / 2.0f,
                 scale, lr, lg, lb, fade);
        if (keyZones && !mOsk.miniOpen) {
            mTouch.tap(TouchRect{b.langX, b.langY, b.langW, b.langH}, [this]() {
                if (!mOskActive || mOsk.closing || mOsk.miniOpen) return;
                mOsk.inLang = true; mOsk.inAction = false; mOsk.inCandidateBar = false;
                oskCycleLanguage(1);
                mDisplayDirty = true;
            });
        }
    }

    // --- Touch / controller keyboard toggle ---
    {
        float tx, ty, tw, th;
        const char* label = oskToggleRect(b, tx, ty, tw, th);
        drawRoundedRect(tx, ty, tw, th, keyRad, kBgR, kBgG, kBgB, kBgA * fade);
        const float scale = 1.5f * b.sf;
        drawText(label, tx + (tw - measureText(label, scale)) / 2.0f, ty + (th - FONT_CHAR_H * scale) / 2.0f,
                 scale, kTxtR, kTxtG, kTxtB, fade);
        if (keyZones && !mOsk.miniOpen) {
            mTouch.tap(TouchRect{tx, ty, tw, th}, [this]() {
                if (!mOskActive || mOsk.closing || mOsk.miniOpen) return;
                oskSetTouch(!mOsk.touch);
            });
        }
    }

    // --- Footer / help line (the controller keyboard) ---
    if (!b.touch) {
        float fScale = 1.35f * b.sf;
        const std::string footerText = trDyn(mOskPasswordCallback
            ? "A:Key  X:Back  Y:Paste  L:Shift  R:Sym  Start:Enter  B:Cancel"
            : "A:Key  X:Back  Y:Paste  L:Shift  R:Sym  Start:Search  B:Cancel");
        const float fw = measureText(footerText.c_str(), fScale);
        const float fx = b.panelX + b.panelW / 2.0f - fw / 2.0f;
        // What each prompt's button does in the key handler (X erases, Y pastes), by its letter.
        static const struct { const char* id; uint16_t code; } kPrompts[] = {
            {"A", BTN_SOUTH}, {"X", BTN_NORTH}, {"Y", BTN_WEST}, {"L", BTN_TL},
            {"R", BTN_TR}, {"Start", BTN_START}, {"B", BTN_EAST},
        };
        // Each prompt (the runs between double spaces) is drawn where it sits in the centred line,
        // so it can be its own button.
        size_t pos = 0;
        while (pos < footerText.size()) {
            size_t end = footerText.find("  ", pos);
            if (end == std::string::npos) end = footerText.size();
            const std::string seg = footerText.substr(pos, end - pos);
            const float sx = fx + measureText(footerText.substr(0, pos).c_str(), fScale);
            drawText(seg.c_str(), sx, b.footerY, fScale, 0.58f, 0.60f, 0.68f, 0.80f * fade);
            if (keyZones) {
                const std::string id = seg.substr(0, seg.find(':'));
                for (const auto& p : kPrompts) {
                    if (id != p.id) continue;
                    touchButtonZone(p.code, TouchRect{sx, b.footerY, measureText(seg.c_str(), fScale),
                                                      FONT_CHAR_H * fScale});
                    break;
                }
            }
            pos = footerText.find_first_not_of(' ', end);
            if (pos == std::string::npos) break;
        }
    }
    mTextOutlineMode = oskPrevOutline;
}

} // namespace android
