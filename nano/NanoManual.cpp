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

#include "NanoManual.h"

#include <string.h>
#include <strings.h>

#include <algorithm>
#include <cmath>

namespace nanomanual {
namespace {

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() > n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A character as natural order compares it: ASCII letters folded to lower case, '/' before all else.
int foldChar(char ch) {
    const unsigned char c = (unsigned char)ch;
    if (c == '/') return -1;
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// <0, 0 or >0, ignoring case and leading zeros.
int naturalCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && isDigit(a[ie])) ie++;
            while (je < b.size() && isDigit(b[je])) je++;
            // Past the leading zeros, keeping the last digit, so "007" and "7" are the same number and
            // a number of any length compares without overflow: the longer one is the larger.
            size_t is = i, js = j;
            while (is + 1 < ie && a[is] == '0') is++;
            while (js + 1 < je && b[js] == '0') js++;
            const size_t la = ie - is, lb = je - js;
            if (la != lb) return la < lb ? -1 : 1;
            const int c = a.compare(is, la, b, js, lb);
            if (c != 0) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        const int ca = foldChar(a[i]), cb = foldChar(b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
        j++;
    }
    if (i < a.size()) return 1;    // b is a prefix of a
    if (j < b.size()) return -1;
    return 0;
}

}  // namespace

const char* const kManualExts[] = {".pdf", ".cbz", ".cb7", ".cbr", nullptr};

Kind kindOf(const std::string& path) {
    if (endsWithNoCase(path, ".pdf")) return Kind::Pdf;
    return comicFormat(path) != nanoarc::Format::None ? Kind::Comic : Kind::None;
}

nanoarc::Format comicFormat(const std::string& path) {
    if (endsWithNoCase(path, ".cbz")) return nanoarc::Format::Zip;
    if (endsWithNoCase(path, ".cb7")) return nanoarc::Format::SevenZip;
    if (endsWithNoCase(path, ".cbr")) return nanoarc::Format::Rar;
    return nanoarc::Format::None;
}

nanoarc::Format comicFormat(const std::string& path, nanoarc::Format content) {
    const nanoarc::Format byName = comicFormat(path);
    if (byName == nanoarc::Format::None) return byName;
    return content != nanoarc::Format::None ? content : byName;
}

bool unsupportedFormat(nanoarc::Err err) {
    return err == nanoarc::Err::Rar || err == nanoarc::Err::Unsupported;
}

bool isPageName(const std::string& entryPath) {
    const size_t sl = entryPath.find_last_of('/');
    const std::string base = sl == std::string::npos ? entryPath : entryPath.substr(sl + 1);
    if (base.empty() || base[0] == '.') return false;
    if (entryPath.compare(0, 9, "__MACOSX/") == 0 || entryPath.find("/__MACOSX/") != std::string::npos)
        return false;
    static const char* const kPageExts[] = {".png", ".jpg", ".jpeg", ".webp", ".gif"};
    for (const char* ext : kPageExts)
        if (endsWithNoCase(base, ext)) return true;
    return false;
}

bool naturalLess(const std::string& a, const std::string& b) {
    const int c = naturalCompare(a, b);
    return c != 0 ? c < 0 : a < b;
}

std::vector<std::string> comicPages(const std::vector<nanoarc::Entry>& entries) {
    std::vector<std::string> pages;
    for (const nanoarc::Entry& e : entries)
        if (!e.isDir && !e.isLink && e.size > 0 && isPageName(e.path)) pages.push_back(e.path);
    std::sort(pages.begin(), pages.end(), naturalLess);
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    return pages;
}

bool pageDecodable(int iw, int ih) {
    return iw > 0 && ih > 0 && (int64_t)iw * (int64_t)ih <= kMaxSourcePixels;
}

void pageSize(int iw, int ih, int zoom, int boxW, int boxH, int* w, int* h) {
    const float pw = (float)std::max(1, iw), ph = (float)std::max(1, ih);
    float sc = zoom == 0 ? std::min(boxW / pw, boxH / ph) : (zoom == 1 ? 1.0f : 2.0f) * boxW / pw;
    sc = std::min(sc, std::min(kMaxPageDim / pw, kMaxPageDim / ph));
    *w = std::max(1, std::min(kMaxPageDim, (int)std::lround(pw * sc)));
    *h = std::max(1, std::min(kMaxPageDim, (int)std::lround(ph * sc)));
}

}  // namespace nanomanual
