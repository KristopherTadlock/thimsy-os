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

#include "NanoBootArt.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "nanosvg.h"
#include "nanosvgrast.h"
}

namespace nanoboot {

// Proportions of the frame's shorter side, chosen so a 640x480 screen still has room below the
// status line: the logo is half of it, the title and text sit just outside the logo.
static constexpr float kLogoH = 0.50f;
static constexpr float kTitlePx = 0.085f, kTitleGap = 0.050f;
static constexpr float kStatusPx = 0.036f, kStatusGap = 0.040f;
static constexpr float kTitleWeight = 600.0f;   // Roboto's weight axis; the rest is its default (400)

namespace {

struct Svg {
    NSVGimage* img = nullptr;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the drawing's own bounds, in SVG units
    explicit Svg(const std::string& path) {
        std::vector<char> buf;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char tmp[65536];
            size_t n;
            while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
            fclose(f);
        }
        if (buf.empty()) return;
        buf.push_back('\0');
        img = nsvgParse(buf.data(), "px", 96.0f);   // parses in place
        if (!img) return;
        bool any = false;
        for (NSVGshape* s = img->shapes; s; s = s->next) {
            if (!(s->flags & NSVG_FLAGS_VISIBLE)) continue;
            if (!any) { x0 = s->bounds[0]; y0 = s->bounds[1]; x1 = s->bounds[2]; y1 = s->bounds[3]; any = true; }
            x0 = std::min(x0, s->bounds[0]); y0 = std::min(y0, s->bounds[1]);
            x1 = std::max(x1, s->bounds[2]); y1 = std::max(y1, s->bounds[3]);
        }
        if (!any || x1 <= x0 || y1 <= y0) { nsvgDelete(img); img = nullptr; }
    }
    ~Svg() { if (img) nsvgDelete(img); }
};

struct Font {
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    std::vector<FT_Face> fallbacks;
    explicit Font(const std::string& path, const std::vector<std::string>& more = {}) {
        if (FT_Init_FreeType(&lib) != 0) { lib = nullptr; return; }
        if (FT_New_Face(lib, path.c_str(), 0, &face) != 0) face = nullptr;
        for (const auto& m : more) {
            FT_Face f = nullptr;
            if (FT_New_Face(lib, m.c_str(), 0, &f) == 0) fallbacks.push_back(f);
        }
    }
    ~Font() {
        for (FT_Face f : fallbacks) FT_Done_Face(f);
        if (face) FT_Done_Face(face);
        if (lib) FT_Done_FreeType(lib);
    }
    void weight(float w) {   // a variable font's weight; a static one stays as it is
        FT_MM_Var* mm = nullptr;
        if (!face || !FT_HAS_MULTIPLE_MASTERS(face) || FT_Get_MM_Var(face, &mm) != 0) return;
        std::vector<FT_Fixed> c(mm->num_axis);
        for (FT_UInt i = 0; i < mm->num_axis; i++) {
            c[i] = mm->axis[i].def;
            if (mm->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                c[i] = std::clamp<FT_Fixed>((FT_Fixed)(w * 65536.0f), mm->axis[i].minimum, mm->axis[i].maximum);
        }
        FT_Set_Var_Design_Coordinates(face, mm->num_axis, c.data());
        FT_Done_MM_Var(lib, mm);
    }
};

// Text measured and drawn in 26.6 pixels, so a line's glyphs keep their sub-pixel advances.
struct Glyphs {
    struct G { int x, y, w, h; std::vector<uint8_t> a; };   // left/top offsets from the pen at the baseline
    std::vector<G> g;
    int left = 0, right = 0, top = 0, bottom = 0;   // the line's ink, relative to its origin
};

Glyphs shape(Font& font, const std::string& text, float px) {
    Glyphs out;
    if (!font.face || text.empty()) return out;
    FT_Set_Char_Size(font.face, 0, (FT_F26Dot6)lroundf(px * 64.0f), 72, 72);
    for (FT_Face f : font.fallbacks) FT_Set_Char_Size(f, 0, (FT_F26Dot6)lroundf(px * 64.0f), 72, 72);
    long pen = 0;   // 26.6
    FT_UInt prev = 0;
    FT_Face prevFace = nullptr;
    bool any = false;
    for (size_t i = 0; i < text.size();) {
        // UTF-8 to a code point
        uint32_t cp = (unsigned char)text[i];
        int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
        if (n > 1) cp &= (0x7F >> n);
        for (int k = 1; k < n && i + k < text.size(); k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
        i += n;
        FT_Face face = font.face;
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        for (size_t f = 0; !gi && f < font.fallbacks.size(); f++) {
            const FT_UInt g2 = FT_Get_Char_Index(font.fallbacks[f], cp);
            if (g2) { face = font.fallbacks[f]; gi = g2; }
        }
        if (prev && gi && face == prevFace && FT_HAS_KERNING(face)) {
            FT_Vector k;
            if (FT_Get_Kerning(face, prev, gi, FT_KERNING_UNFITTED, &k) == 0) pen += k.x;
        }
        prev = gi;
        prevFace = face;
        FT_Vector shift = {pen & 63, 0};
        FT_Set_Transform(face, nullptr, &shift);
        const bool drawn = FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) == 0
                           && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0;
        FT_Set_Transform(face, nullptr, nullptr);
        if (!drawn) continue;
        const FT_GlyphSlot s = face->glyph;
        Glyphs::G g;
        g.x = (int)(pen >> 6) + s->bitmap_left;
        g.y = -s->bitmap_top;
        g.w = (int)s->bitmap.width;
        g.h = (int)s->bitmap.rows;
        g.a.resize((size_t)g.w * g.h);
        for (int r = 0; r < g.h; r++) memcpy(&g.a[(size_t)r * g.w], s->bitmap.buffer + r * s->bitmap.pitch, g.w);
        if (g.w > 0 && g.h > 0) {
            if (!any) { out.left = g.x; out.right = g.x + g.w; out.top = g.y; out.bottom = g.y + g.h; any = true; }
            out.left = std::min(out.left, g.x); out.right = std::max(out.right, g.x + g.w);
            out.top = std::min(out.top, g.y); out.bottom = std::max(out.bottom, g.y + g.h);
        }
        pen += s->advance.x;
        out.g.push_back(std::move(g));
    }
    return out;
}

// White text over an RGBA frame, its line centred on cx with its baseline at base.
void blit(const Glyphs& t, int cx, int base, int w, int h, std::vector<uint8_t>* rgba) {
    const int ox = cx - (t.left + t.right) / 2;
    for (const auto& g : t.g)
        for (int r = 0; r < g.h; r++) {
            const int y = base + g.y + r;
            if (y < 0 || y >= h) continue;
            for (int c = 0; c < g.w; c++) {
                const int x = ox + g.x + c;
                if (x < 0 || x >= w) continue;
                const int a = g.a[(size_t)r * g.w + c];
                uint8_t* p = &(*rgba)[((size_t)y * w + x) * 4];
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)((255 * a + p[k] * (255 - a) + 127) / 255);
            }
        }
}

}  // namespace

bool layout(const Art& art, int w, int h, Layout* out) {
    Svg svg(art.svgPath);
    if (!svg.img || w <= 0 || h <= 0) return false;
    const float u = (float)std::min(w, h);
    Layout L;
    L.w = w; L.h = h;
    L.logoScale = kLogoH * u / (svg.y1 - svg.y0);
    L.logoW = (int)ceilf((svg.x1 - svg.x0) * L.logoScale);
    L.logoH = (int)ceilf((svg.y1 - svg.y0) * L.logoScale);
    L.logoLeft = (w - L.logoW) / 2;
    L.logoTop = (h - L.logoH) / 2;
    L.logoX = L.logoLeft - (int)lroundf(svg.x0 * L.logoScale);
    L.logoY = L.logoTop - (int)lroundf(svg.y0 * L.logoScale);
    L.titlePx = kTitlePx * u;
    L.statusPx = kStatusPx * u;
    // Baselines: the title's sits a gap above the logo; the status line's is a gap plus its ascent
    // (taken as 0.72 of its size, Roboto's cap height) below the logo.
    L.titleBase = L.logoTop - (int)lroundf(kTitleGap * u);
    L.statusBase = L.logoTop + L.logoH + (int)lroundf(kStatusGap * u + 0.72f * L.statusPx);
    *out = L;
    return true;
}

bool renderBase(const Art& art, int w, int h, std::vector<uint8_t>* rgba) {
    Layout L;
    if (!layout(art, w, h, &L)) return false;
    rgba->resize((size_t)w * h * 4);
    for (size_t i = 0; i < rgba->size(); i += 4) {
        for (int k = 0; k < 3; k++) (*rgba)[i + k] = art.background[k];
        (*rgba)[i + 3] = 255;
    }

    // The logo, rasterised at its final size (never scaled as pixels), over the background.
    Svg svg(art.svgPath);
    if (!svg.img) return false;
    std::vector<uint8_t> logo((size_t)w * h * 4, 0);
    if (NSVGrasterizer* r = nsvgCreateRasterizer()) {
        nsvgRasterize(r, svg.img, (float)L.logoX, (float)L.logoY, L.logoScale, logo.data(), w, h, w * 4);
        nsvgDeleteRasterizer(r);
    }
    for (size_t i = 0; i < logo.size(); i += 4) {
        const int a = logo[i + 3];
        for (int k = 0; k < 3; k++) (*rgba)[i + k] = (uint8_t)((logo[i + k] * a + (*rgba)[i + k] * (255 - a) + 127) / 255);
    }

    Font font(art.fontPath);
    if (!font.face) return false;
    font.weight(kTitleWeight);
    blit(shape(font, art.title, L.titlePx), w / 2, L.titleBase, w, h, rgba);
    return true;
}

Line renderStatus(const Art& art, int w, int h, const std::string& text) {
    Line out;
    Layout L;
    if (text.empty() || !layout(art, w, h, &L)) return out;
    Font font(art.fontPath, art.fallbackFonts);
    if (!font.face) return out;
    font.weight(400.0f);
    const Glyphs t = shape(font, text, L.statusPx);
    if (t.right <= t.left || t.bottom <= t.top) return out;
    out.w = t.right - t.left;
    out.h = t.bottom - t.top;
    out.x = w / 2 - (t.left + t.right) / 2 + t.left;
    out.y = L.statusBase + t.top;
    out.rgba.assign((size_t)out.w * out.h * 4, 0);
    for (const auto& g : t.g)
        for (int r = 0; r < g.h; r++)
            for (int c = 0; c < g.w; c++) {
                const int x = g.x - t.left + c, y = g.y - t.top + r;
                uint8_t* p = &out.rgba[((size_t)y * out.w + x) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = std::max<uint8_t>(p[3], g.a[(size_t)r * g.w + c]);
            }
    return out;
}

}  // namespace nanoboot
