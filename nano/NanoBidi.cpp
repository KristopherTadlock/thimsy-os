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

// Logical-to-visual bidi transform and Arabic contextual shaping shared by
// gammaos-nano and drastic-nano. Must stay free of NanoMenu dependencies.

#include <cstdint>
#include <string>
#include <vector>

#include "NanoBidi.h"
#include "NanoOskArabic.h"        // native Arabic contextual-shaping table

namespace android {

namespace {

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
    // A truncated or broken sequence is one replacement character, so it cannot swallow the
    // character after it.
    if (pos + len > n) { advance = 1; return 0xFFFD; }
    for (int k = 1; k < len; k++) {
        const unsigned char b = (unsigned char)s[pos + k];
        if ((b & 0xC0) != 0x80) { advance = 1; return 0xFFFD; }
        cp = (cp << 6) | (b & 0x3F);
    }
    advance = len;
    return cp;
}

// --- Native Arabic contextual shaping (replaces ICU u_shapeArabic) ---
// Table-driven: each Arabic letter resolves to one of four presentation forms
// (isolated/final/initial/medial) based on whether it connects to its logical
// neighbours, plus the lam-alef ligature. Data lives in NanoOskArabic.h; the
// shaping itself (arShapeVector) lives with the bidi engine below, which the
// glyph renderer runs on every drawn string via textForDisplay.

const OskArShape* arShapeFor(uint32_t cp) {
    int lo = 0, hi = kOskArShapeCount - 1;     // table is sorted by base
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (kOskArShape[m].base == cp) return &kOskArShape[m];
        if (kOskArShape[m].base < cp) lo = m + 1; else hi = m - 1;
    }
    return nullptr;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// UBA-lite bidirectional layout for the glyph renderer (see NanoBidi.h).
//
// drawText/measureText walk the string in order and advance the pen along +x,
// so RTL scripts need a logical-to-visual transform up front: Arabic letters
// substitute their contextual presentation forms and RTL runs reverse, while
// embedded Latin/digit runs keep flowing left-to-right. This is the small
// subset of UAX #9 the nano UI needs: a single embedding level, the first
// strong codepoint sets the paragraph direction, a neutral span between two
// runs of the same direction joins that direction and otherwise takes the
// paragraph direction, numbers always read left-to-right, combining marks
// travel with their base letter, and paired brackets mirror inside reversed
// runs. ZWJ/ZWNJ are honoured by the shaper and then stripped along with the
// other zero-width direction marks: no bundled font has glyphs for them, so
// left in the stream they surface as '?' fallback boxes.
// ---------------------------------------------------------------------------

namespace {

enum BidiClass : uint8_t { BC_L = 0, BC_R, BC_NUM, BC_NEU, BC_MARK };

// Zero-width format codepoints stripped from the visual stream (ZWSP..RLM,
// embedding controls, word joiner + invisible operators, isolates, variation
// selectors, BOM/ZWNBSP, and the Arabic letter mark).
bool bidiIsZeroWidth(uint32_t cp) {
    return cp == 0x061C
        || (cp >= 0x200B && cp <= 0x200F)
        || (cp >= 0x202A && cp <= 0x202E)
        || (cp >= 0x2060 && cp <= 0x2064)
        || (cp >= 0x2066 && cp <= 0x2069)
        || (cp >= 0xFE00 && cp <= 0xFE0F)
        || cp == 0xFEFF;
}

// Combining marks that overlay the preceding base glyph (zero advance in the
// Noto faces): they must stay directly after their base in the visual stream
// and are transparent for Arabic join context.
bool bidiIsCombining(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F)                                  // Latin/Greek/Cyrillic
        || (cp >= 0x0591 && cp <= 0x05C7)                                  // Hebrew points
        || (cp >= 0x0610 && cp <= 0x061A)                                  // Arabic honorific signs
        || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670                  // harakat + superscript alef
        || (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4)
        || (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED)
        || (cp >= 0x08D3 && cp <= 0x08FF);                                 // Arabic Extended-A marks
}

// Strong right-to-left letters (Hebrew + Arabic blocks and their presentation
// forms). Combining marks and digits from these blocks are classified before
// this test, so the broad ranges are safe.
bool bidiIsRtlLetter(uint32_t cp) {
    return (cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0xFB1D && cp <= 0xFB4F)
        || (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F)
        || (cp >= 0x08A0 && cp <= 0x08D2)
        || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFE);
}

bool bidiIsDigit(uint32_t cp) {
    return (cp >= '0' && cp <= '9')
        || (cp >= 0x0660 && cp <= 0x0669)    // Arabic-Indic digits
        || (cp >= 0x06F0 && cp <= 0x06F9);   // extended Arabic-Indic digits
}

// Direction-neutral codepoints: spacing, ASCII/Latin-1 punctuation and
// symbols, general punctuation, arrows/math/misc symbols, CJK and fullwidth
// punctuation. Anything unlisted that is not a digit/mark/RTL letter counts
// as strong LTR, which keeps unknown scripts in logical order.
bool bidiIsNeutral(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n'
        || (cp >= 0x21 && cp <= 0x2F) || (cp >= 0x3A && cp <= 0x40)
        || (cp >= 0x5B && cp <= 0x60) || (cp >= 0x7B && cp <= 0x7E)
        || (cp >= 0x00A0 && cp <= 0x00BF) || cp == 0x00D7 || cp == 0x00F7
        || (cp >= 0x2000 && cp <= 0x2BFF)
        || (cp >= 0x3000 && cp <= 0x3020)
        || (cp >= 0xFE30 && cp <= 0xFE6F)
        || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20)
        || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}

// Paired-bracket mirroring for codepoints that land inside a reversed run.
uint32_t bidiMirror(uint32_t cp) {
    switch (cp) {
        case '(':    return ')';    case ')':    return '(';
        case '[':    return ']';    case ']':    return '[';
        case '{':    return '}';    case '}':    return '{';
        case '<':    return '>';    case '>':    return '<';
        case 0x00AB: return 0x00BB; case 0x00BB: return 0x00AB;   // double angle quotes
        case 0x2039: return 0x203A; case 0x203A: return 0x2039;   // single angle quotes
        default:     return cp;
    }
}

// Does the nearest non-transparent logical neighbour of cps[i] (step = -1
// toward the string start, +1 toward the end) join across the boundary?
// Combining marks and zero-width formats (except ZWNJ/ZWJ) are transparent;
// ZWJ forces the join, ZWNJ blocks it. A letter neighbour joins when the
// facing side of its shape entry connects: the left neighbour must join left
// (toward its follower), the right neighbour must join right (toward its
// predecessor).
bool arNeighbourJoins(const std::vector<uint32_t>& cps, int i, int step) {
    for (int j = i + step; j >= 0 && j < (int)cps.size(); j += step) {
        uint32_t cp = cps[j];
        if (cp == 0x200D) return true;    // zero-width joiner
        if (cp == 0x200C) return false;   // zero-width non-joiner
        if (bidiIsCombining(cp) || bidiIsZeroWidth(cp)) continue;
        const OskArShape* sh = arShapeFor(cp);
        if (!sh) return false;
        return (step < 0) ? (sh->joinsLeft != 0) : (sh->joinsRight != 0);
    }
    return false;
}

// Codepoint-level Arabic contextual shaping: resolves each letter to its
// isolated/final/initial/medial presentation form, ligates directly adjacent
// lam+alef, and looks THROUGH combining marks and zero-width formats for join
// context so harakat do not break the join between their neighbours.
void arShapeVector(std::vector<uint32_t>& cps) {
    const int n = (int)cps.size();
    std::vector<uint32_t> out;
    out.reserve(n);
    for (int i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        if (cp == 0x0644 && i + 1 < n) {   // lam + alef variant -> ligature
            const OskArLamAlef* lig = nullptr;
            for (int k = 0; k < kOskArLamAlefCount; k++)
                if (kOskArLamAlef[k].alef == cps[i + 1]) { lig = &kOskArLamAlef[k]; break; }
            if (lig) {
                bool joinPrev = arNeighbourJoins(cps, i, -1);
                uint32_t g = joinPrev ? lig->fin : lig->iso;
                out.push_back(g ? g : cp);
                i++;                 // consume the alef
                continue;
            }
        }
        const OskArShape* sh = arShapeFor(cp);
        if (!sh) { out.push_back(cp); continue; }
        bool joinPrev = sh->joinsRight && arNeighbourJoins(cps, i, -1);
        bool joinNext = sh->joinsLeft  && arNeighbourJoins(cps, i, +1);
        uint32_t g;
        if (joinPrev && joinNext) g = sh->med ? sh->med : (sh->fin ? sh->fin : sh->iso);
        else if (joinPrev)        g = sh->fin ? sh->fin : sh->iso;
        else if (joinNext)        g = sh->ini ? sh->ini : sh->iso;
        else                      g = sh->iso;
        out.push_back(g ? g : cp);
    }
    cps.swap(out);
}

} // namespace

bool nanoTextIsRtl(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; ) {
        uint32_t cp; uint8_t b0 = *p;
        if (b0 < 0x80) { cp = b0; p++; }
        else if ((b0 & 0xE0) == 0xC0 && p[1]) { cp = ((b0 & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((b0 & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((b0 & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { p++; continue; }
        if (bidiIsZeroWidth(cp) || bidiIsCombining(cp) || bidiIsDigit(cp) || bidiIsNeutral(cp))
            continue;                       // weak/neutral: keep scanning
        return bidiIsRtlLetter(cp);         // first strong codepoint decides
    }
    return false;
}

// True when the string contains ANY RTL letter (not just first-strong). The
// OSK preview uses this to avoid the caret-split path for a value that is
// LTR-first but has an embedded Arabic/Hebrew run: splitting at the caret and
// bidi-transforming each half independently would garble that run.
bool nanoTextHasRtl(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; ) {
        uint32_t cp; uint8_t b0 = *p;
        if (b0 < 0x80) { cp = b0; p++; }
        else if ((b0 & 0xE0) == 0xC0 && p[1]) { cp = ((b0 & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((b0 & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((b0 & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { p++; continue; }
        if (!bidiIsCombining(cp) && bidiIsRtlLetter(cp)) return true;
    }
    return false;
}

std::string nanoBidiVisual(const std::string& s) {
    if (s.empty()) return s;
    // Decode, noting whether any work is needed at all.
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    bool hasRtl = false, hasZw = false;
    for (int i = 0; i < (int)s.size(); ) {
        int adv = 0;
        uint32_t cp = utf8DecodeAt(s, i, adv);
        i += (adv > 0 ? adv : 1);
        cps.push_back(cp);
        if (bidiIsZeroWidth(cp)) hasZw = true;
        else if (!bidiIsCombining(cp) && bidiIsRtlLetter(cp)) hasRtl = true;
    }
    if (!hasRtl && !hasZw) return s;

    if (hasRtl) arShapeVector(cps);   // ZWJ/ZWNJ still present for join context

    if (hasZw) {
        std::vector<uint32_t> kept;
        kept.reserve(cps.size());
        for (uint32_t cp : cps)
            if (!bidiIsZeroWidth(cp)) kept.push_back(cp);
        cps.swap(kept);
    }
    if (!hasRtl) {   // only stripped zero-width marks: logical order stands
        std::string out;
        out.reserve(s.size());
        for (uint32_t cp : cps) out += utf8Encode(cp);
        return out;
    }

    const int n = (int)cps.size();
    std::vector<uint8_t> cls(n);
    for (int i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        if (bidiIsCombining(cp))      cls[i] = BC_MARK;
        else if (bidiIsDigit(cp))     cls[i] = BC_NUM;
        else if (bidiIsRtlLetter(cp)) cls[i] = BC_R;
        else if (bidiIsNeutral(cp))   cls[i] = BC_NEU;
        else                          cls[i] = BC_L;
    }
    // Combining marks take their base's class (marks with no base act neutral).
    for (int i = 0; i < n; i++)
        if (cls[i] == BC_MARK) cls[i] = (i > 0) ? cls[i - 1] : (uint8_t)BC_NEU;
    // Paragraph direction: first strong letter (digits are weak, neutrals skip).
    uint8_t para = BC_L;
    for (int i = 0; i < n; i++) {
        if (cls[i] == BC_R) { para = BC_R; break; }
        if (cls[i] == BC_L) { para = BC_L; break; }
    }
    // Neutral spans join their surrounding direction when both sides agree,
    // otherwise the paragraph direction. Numbers count as LTR context.
    auto strongOf = [](uint8_t c) -> uint8_t { return c == BC_NUM ? (uint8_t)BC_L : c; };
    {
        uint8_t prev = para;
        for (int i = 0; i < n; ) {
            if (cls[i] != BC_NEU) { prev = strongOf(cls[i]); i++; continue; }
            int j = i;
            while (j < n && cls[j] == BC_NEU) j++;
            uint8_t next = (j < n) ? strongOf(cls[j]) : para;
            uint8_t fill = (prev == next) ? prev : para;
            for (int k = i; k < j; k++) cls[k] = fill;
            i = j;
        }
    }
    for (int i = 0; i < n; i++) cls[i] = strongOf(cls[i]);   // fold numbers into LTR runs

    // Emit runs: paragraph-RTL lists runs right-to-left; RTL runs reverse
    // cluster-wise (base + trailing combining marks stay together, brackets
    // mirror); LTR runs stay in logical order.
    std::string out;
    out.reserve(s.size());
    auto emitRun = [&](int a, int b, bool rtl) {   // inclusive range
        if (!rtl) {
            for (int i = a; i <= b; i++) out += utf8Encode(cps[i]);
            return;
        }
        std::vector<int> starts;
        for (int i = a; i <= b; i++)
            if (i == a || !bidiIsCombining(cps[i])) starts.push_back(i);
        for (int k = (int)starts.size() - 1; k >= 0; k--) {
            int cs = starts[k];
            int ce = (k + 1 < (int)starts.size()) ? starts[k + 1] - 1 : b;
            out += utf8Encode(bidiMirror(cps[cs]));
            for (int i = cs + 1; i <= ce; i++) out += utf8Encode(cps[i]);
        }
    };
    struct Run { int a, b; uint8_t c; };
    std::vector<Run> runs;
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && cls[j] == cls[i]) j++;
        runs.push_back({i, j - 1, cls[i]});
        i = j;
    }
    if (para == BC_R)
        for (int k = (int)runs.size() - 1; k >= 0; k--) emitRun(runs[k].a, runs[k].b, runs[k].c == BC_R);
    else
        for (int k = 0; k < (int)runs.size(); k++)       emitRun(runs[k].a, runs[k].b, runs[k].c == BC_R);
    return out;
}

} // namespace android
