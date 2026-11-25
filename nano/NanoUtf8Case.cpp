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

#include "NanoUtf8Case.h"

namespace android {

namespace {

// Ranges where the capital is the even code point and the small letter the odd one after it.
bool evenUpperPair(uint32_t cp) {
    return (cp >= 0x100 && cp <= 0x137 && cp != 0x130 && cp != 0x131) || (cp >= 0x14A && cp <= 0x177) ||
           (cp >= 0x1DE && cp <= 0x1EF) || (cp >= 0x1F8 && cp <= 0x21F) ||
           (cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF) || (cp >= 0x4D0 && cp <= 0x52F) ||
           (cp >= 0x1E00 && cp <= 0x1E95) || (cp >= 0x1EA0 && cp <= 0x1EFF);
}
// Ranges where the capital is the odd code point and the small letter the even one after it.
bool oddUpperPair(uint32_t cp) {
    return (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E) || (cp >= 0x1CD && cp <= 0x1DC) ||
           (cp >= 0x4C1 && cp <= 0x4CE);
}

// Decodes one character at s[i]; on a malformed sequence returns the byte itself with len 1 and
// ok false, so it is copied through untouched.
uint32_t decode(const std::string& s, size_t i, size_t& len, bool& ok) {
    const unsigned char c = (unsigned char)s[i];
    ok = true;
    len = 1;
    if (c < 0x80) return c;
    int n = (c >= 0xF0 && c <= 0xF4) ? 3 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xC2 && c <= 0xDF) ? 1 : -1;
    if (n < 0 || i + n >= s.size()) { ok = false; return c; }
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) { ok = false; return c; }
        cp = (cp << 6) | (d & 0x3F);
    }
    static const uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ok = false; return c; }
    len = (size_t)n + 1;
    return cp;
}

void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) { out += (char)cp; return; }
    if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); return; }
    if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        return;
    }
    out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
}

enum Mode { kUpper, kLower, kCapitalize };

bool isLetter(uint32_t cp) {
    if (cp < 0x80) return (cp | 0x20) >= 'a' && (cp | 0x20) <= 'z';
    return cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) && !(cp >= 0x3000 && cp <= 0x303F);
}

// A capital letter starting a word keeps its tonos (Άλφα).
uint32_t capitalKeepTonos(uint32_t cp) {
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

// Greek capitals lose their tonos when a whole string is upper-cased (as Greek is written).
uint32_t upperNoTonos(uint32_t cp) {
    switch (cp) {
        case 0x390: return 0x3AA;   // ΐ -> Ϊ
        case 0x3B0: return 0x3AB;   // ΰ -> Ϋ
        case 0x386: return 0x391;
        case 0x388: return 0x395;
        case 0x389: return 0x397;
        case 0x38A: return 0x399;
        case 0x38C: return 0x39F;
        case 0x38E: return 0x3A5;
        case 0x38F: return 0x3A9;
    }
    return cp;
}

std::string convert(const std::string& s, Mode mode) {
    std::string out;
    out.reserve(s.size());
    bool wordStart = true, prevLetter = false;
    for (size_t i = 0; i < s.size();) {
        size_t len;
        bool ok;
        const uint32_t cp = decode(s, i, len, ok);
        if (!ok) { out += s[i]; i += 1; wordStart = false; prevLetter = false; continue; }
        const bool space = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v';
        const bool up = mode == kUpper || (mode == kCapitalize && wordStart);
        if (up && cp == 0xDF) out += mode == kUpper ? "SS" : "Ss";
        else if (up) encode(mode == kUpper ? upperNoTonos(nanoToUpperCp(cp)) : capitalKeepTonos(cp), out);
        else if (cp == 0x3A3 && prevLetter) {   // sigma ending a word is written ς
            size_t nlen;
            bool nok;
            const bool nextLetter = i + len < s.size() && isLetter(decode(s, i + len, nlen, nok)) && nok;
            encode(nextLetter ? 0x3C3 : 0x3C2, out);
        } else encode(nanoToLowerCp(cp), out);
        wordStart = space;
        prevLetter = isLetter(cp);
        i += len;
    }
    return out;
}

}  // namespace

uint32_t nanoToUpperCp(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'a' && cp <= 'z') ? cp - 32 : cp;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;
    if (cp == 0xFF) return 0x178;
    if (cp == 0x131) return 'I';
    if (cp == 0x17F) return 'S';
    if (cp == 0x1A1 || cp == 0x1B0) return cp - 1;   // Vietnamese ơ ư
    if (evenUpperPair(cp)) return (cp & 1) ? cp - 1 : cp;
    if (oddUpperPair(cp)) return (cp & 1) ? cp : cp - 1;
    switch (cp) {   // Greek with tonos: capitals drop it, as Greek all-caps text does
        case 0x3AC: return 0x391;
        case 0x3AD: return 0x395;
        case 0x3AE: return 0x397;
        case 0x3AF: return 0x399;
        case 0x3CC: return 0x39F;
        case 0x3CD: return 0x3A5;
        case 0x3CE: return 0x3A9;
        case 0x3CA: return 0x3AA;
        case 0x3CB: return 0x3AB;
        case 0x3C2: return 0x3A3;   // final sigma
    }
    if (cp >= 0x3B1 && cp <= 0x3C9) return cp - 0x20;
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
    if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
    if (cp >= 0x561 && cp <= 0x586) return cp - 0x30;
    if (cp >= 0xFF41 && cp <= 0xFF5A) return cp - 0x20;
    return cp;
}

uint32_t nanoToLowerCp(uint32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if (cp == 0x178) return 0xFF;
    if (cp == 0x130) return 'i';
    if (cp == 0x1A0 || cp == 0x1AF) return cp + 1;
    if (evenUpperPair(cp)) return (cp & 1) ? cp : cp + 1;
    if (oddUpperPair(cp)) return (cp & 1) ? cp + 1 : cp;
    switch (cp) {
        case 0x386: return 0x3AC;
        case 0x388: return 0x3AD;
        case 0x389: return 0x3AE;
        case 0x38A: return 0x3AF;
        case 0x38C: return 0x3CC;
        case 0x38E: return 0x3CD;
        case 0x38F: return 0x3CE;
        case 0x3AA: return 0x3CA;
        case 0x3AB: return 0x3CB;
    }
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
    if (cp >= 0x531 && cp <= 0x556) return cp + 0x30;
    if (cp >= 0xFF21 && cp <= 0xFF3A) return cp + 0x20;
    return cp;
}

std::string nanoUtf8Upper(const std::string& s) { return convert(s, kUpper); }
std::string nanoUtf8Lower(const std::string& s) { return convert(s, kLower); }
std::string nanoUtf8Capitalize(const std::string& s) { return convert(s, kCapitalize); }

}  // namespace android
