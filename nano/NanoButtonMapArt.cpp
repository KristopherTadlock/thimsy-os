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

#include "NanoButtonMapArt.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include "NanoBidi.h"
#include "NanoJson.h"

extern "C" {
#include "nanosvg.h"
#include "nanosvgrast.h"
}

namespace android {
namespace bmart {

using bmap::MapResult;
using bmap::PadModel;

// ---- palettes (NanoBiosUi's per-theme modal colours) ----

Palette paletteXmb() { return Palette(); }

Palette paletteDsi() {
    Palette p;
    p.dim = {0, 0, 0, 110};
    p.panel = {246, 246, 246, 250};
    p.accent = {52, 130, 214, 255};
    p.text = {40, 40, 40, 255};
    p.textDim = {120, 120, 120, 255};
    p.ink = {0x2E, 0x2E, 0x2E, 255};
    p.body = {0xED, 0xED, 0xED, 255};
    p.chip = {52, 130, 214, 255};
    p.chipText = {255, 255, 255, 255};
    p.line = {90, 120, 160, 255};
    p.radius = 60;
    p.accentBar = false;
    return p;
}

Palette paletteMinima() {
    Palette p;
    p.dim = {0, 0, 0, 140};
    p.panel = {34, 34, 38, 245};
    p.accent = {255, 196, 0, 255};
    p.text = {240, 240, 240, 255};
    p.textDim = {160, 160, 160, 255};
    p.chip = {255, 196, 0, 255};
    p.chipText = {20, 20, 20, 255};
    p.line = {200, 180, 120, 255};
    p.radius = 45;
    p.accentBar = false;
    return p;
}

// ---- scenes ----

Scene sceneFor(const MapResult& r, const PadModel& pad) {
    Scene s;
    char buf[256];
    s.title = r.consoleName;
    s.subtitle = r.emulatorName;
    if (!r.padName.empty()) s.subtitle += (s.subtitle.empty() ? "" : " \xC2\xB7 ") + r.padName;
    s.drawing = r.drawing;
    switch (r.source) {
        case bmap::Source::kConfig:
            if (r.ctx.kind != bmap::Context::kBrowser) {
                std::string emu = r.emulatorName.substr(0, r.emulatorName.find(' '));
                snprintf(buf, sizeof(buf), "From your %s settings", emu.c_str());
                s.badge = buf;
            }
            break;
        case bmap::Source::kDefaults: s.badge = "Default mapping"; break;
        case bmap::Source::kNotMapped:
            snprintf(buf, sizeof(buf), "Not mapped in %s", r.emulatorName.c_str());
            s.badge = buf;
            s.hint = r.notMappedWhere;
            break;
        case bmap::Source::kUnknown: break;
    }
    if (!r.notes.empty() && r.source != bmap::Source::kNotMapped)
        s.badge += (s.badge.empty() ? "" : " \xC2\xB7 ") + r.notes[0];
    if (r.ctx.kind == bmap::Context::kBrowser) s.consoleHeader = "Action";
    if (r.browserGreyed) {
        s.greyed = true;
        s.hint = r.browserHint;
    }
    if (r.source != bmap::Source::kNotMapped)
        for (const auto& row : r.rows) {
            SceneRow sr;
            sr.anchor = row.consoleId;
            sr.console = row.consoleName;
            sr.keys = bmap::rowKeys(row, pad);
            sr.dim = row.unbound() && row.keys.empty();
            s.rows.push_back(sr);
        }
    for (const auto& h : r.hotkeys) s.hotkeys.push_back({h.keys, h.action});
    const bmap::NavConfig nc = bmap::navConfigFor(pad);
    if (nc.back != bmap::Phys::kCount) s.footer.push_back(pad.labelOf(nc.back) + " Close");
    if (nc.toggle != bmap::Phys::kCount && !s.drawing.empty()) s.footer.push_back(pad.labelOf(nc.toggle) + " Table");
    s.pageHint = pad.labelOf(bmap::Phys::L1) + "/" + pad.labelOf(bmap::Phys::R1) + " Page";
    return s;
}

void translateScene(Scene* s, const bmap::Translator& tr) {
    auto t = [&](std::string& x) { x = bmap::translate(x, tr); };
    for (std::string* x : {&s->title, &s->subtitle, &s->badge, &s->hint, &s->controlsTitle, &s->hotkeysTitle,
                           &s->consoleHeader, &s->padHeader})
        t(*x);
    for (auto& r : s->rows) { t(r.console); t(r.keys); }
    for (auto& h : s->hotkeys) { t(h.keys); t(h.action); }
    auto hint = [&](std::string& x) {
        const size_t sp = x.find(' ');
        if (sp == std::string::npos) { t(x); return; }
        x = bmap::translate(x.substr(0, sp), tr) + " " + bmap::translate(x.substr(sp + 1), tr);
    };
    for (auto& f : s->footer) hint(f);
    hint(s->pageHint);
}

namespace {

Rgba jsonColor(const njson::Value* v, Rgba def) {
    if (!v || !v->isString() || v->str.size() < 7 || v->str[0] != '#') return def;
    const unsigned long n = strtoul(v->str.c_str() + 1, nullptr, 16);
    Rgba c;
    if (v->str.size() >= 9) { c.r = n >> 24; c.g = (n >> 16) & 255; c.b = (n >> 8) & 255; c.a = n & 255; }
    else { c.r = (n >> 16) & 255; c.g = (n >> 8) & 255; c.b = n & 255; c.a = 255; }
    return c;
}

}  // namespace

bool sceneFromJson(const std::string& json, Scene* out) {
    njson::Value v;
    if (json.size() > (1u << 20) || !njson::parse(json, &v) || !v.isObject()) return false;
    Scene s;
    const std::string pal = v.getString("palette", "xmb");
    s.pal = pal == "dsi" ? paletteDsi() : pal == "minima" ? paletteMinima() : paletteXmb();
    s.title = v.getString("title");
    s.subtitle = v.getString("subtitle");
    s.badge = v.getString("badge");
    s.hint = v.getString("hint");
    s.drawing = v.getString("drawing");
    s.table = v.getBool("table");
    s.greyed = v.getBool("greyed");
    s.rtl = v.getBool("rtl");
    s.controlsTitle = v.getString("controlsTitle", s.controlsTitle);
    s.hotkeysTitle = v.getString("hotkeysTitle", s.hotkeysTitle);
    s.consoleHeader = v.getString("consoleHeader", s.consoleHeader);
    s.padHeader = v.getString("padHeader", s.padHeader);
    if (const njson::Value* fs = v.find("fontScale")) s.fontScale = (float)fs->asNumber(1.0);
    if (const njson::Value* rows = v.find("rows"); rows && rows->isArray())
        for (const auto& r : rows->arr) {
            if (!r.isObject()) continue;
            SceneRow sr;
            sr.anchor = r.getString("anchor");
            sr.console = r.getString("console");
            sr.keys = r.getString("keys");
            sr.dim = r.getBool("dim");
            s.rows.push_back(sr);
        }
    if (const njson::Value* hk = v.find("hotkeys"); hk && hk->isArray())
        for (const auto& h : hk->arr)
            if (h.isObject()) s.hotkeys.push_back({h.getString("keys"), h.getString("action")});
    if (const njson::Value* f = v.find("footer"); f && f->isArray())
        for (const auto& x : f->arr) if (x.isString()) s.footer.push_back(x.str);
    s.pageHint = v.getString("pageHint", s.pageHint);
    if (const njson::Value* c = v.find("colors"); c && c->isObject()) {
        s.pal.panel = jsonColor(c->find("panel"), s.pal.panel);
        s.pal.accent = jsonColor(c->find("accent"), s.pal.accent);
    }
    *out = std::move(s);
    return true;
}

// ---- text ----

namespace {

struct Font {
    FT_Library lib = nullptr;
    std::vector<FT_Face> faces;   // the first, then fallbacks
    Font(const std::string& path, const std::vector<std::string>& more) {
        if (FT_Init_FreeType(&lib) != 0) { lib = nullptr; return; }
        FT_Face f = nullptr;
        if (FT_New_Face(lib, path.c_str(), 0, &f) == 0) faces.push_back(f);
        else return;
        for (const auto& m : more)
            if (FT_New_Face(lib, m.c_str(), 0, &f) == 0) faces.push_back(f);
    }
    ~Font() {
        for (FT_Face f : faces) FT_Done_Face(f);
        if (lib) FT_Done_FreeType(lib);
    }
    bool ok() const { return !faces.empty(); }
    void size(int px) {
        for (FT_Face f : faces) FT_Set_Char_Size(f, 0, (FT_F26Dot6)px * 64, 72, 72);
    }
    int ascent(int px) {
        size(px);
        return (int)((faces[0]->size->metrics.ascender + 63) >> 6);
    }
    int descent(int px) {
        size(px);
        return (int)((-faces[0]->size->metrics.descender + 63) >> 6);
    }
};

uint32_t nextCp(const std::string& t, size_t* i) {
    uint32_t cp = (unsigned char)t[*i];
    const int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
    if (n > 1) cp &= (0x7F >> n);
    for (int k = 1; k < n && *i + k < t.size(); k++) cp = (cp << 6) | ((unsigned char)t[*i + k] & 0x3F);
    *i += n;
    return cp;
}

struct Glyph { int x, y, w, h; std::vector<uint8_t> a; };
struct Shaped { std::vector<Glyph> g; int advance = 0; };

// One line, visual order, pen from 0 at the baseline. measureOnly skips the bitmaps.
Shaped shapeLine(Font& f, const std::string& logical, int px, bool measureOnly) {
    Shaped out;
    if (!f.ok() || logical.empty()) return out;
    const std::string text = nanoBidiVisual(logical);
    f.size(px);
    long pen = 0;
    FT_UInt prev = 0;
    FT_Face prevFace = nullptr;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = nextCp(text, &i);
        FT_Face face = f.faces[0];
        FT_UInt gi = FT_Get_Char_Index(face, cp);
        for (size_t k = 1; !gi && k < f.faces.size(); k++) {
            const FT_UInt g2 = FT_Get_Char_Index(f.faces[k], cp);
            if (g2) { face = f.faces[k]; gi = g2; }
        }
        if (prev && gi && face == prevFace && FT_HAS_KERNING(face)) {
            FT_Vector k;
            if (FT_Get_Kerning(face, prev, gi, FT_KERNING_UNFITTED, &k) == 0) pen += k.x;
        }
        prev = gi;
        prevFace = face;
        FT_Vector shift = {pen & 63, 0};
        FT_Set_Transform(face, nullptr, &shift);
        const bool loaded = FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING) == 0;
        const bool drawn = loaded && !measureOnly && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0;
        FT_Set_Transform(face, nullptr, nullptr);
        if (!loaded) continue;
        const FT_GlyphSlot s = face->glyph;
        if (drawn) {
            Glyph g;
            g.x = (int)(pen >> 6) + s->bitmap_left;
            g.y = -s->bitmap_top;
            g.w = (int)s->bitmap.width;
            g.h = (int)s->bitmap.rows;
            g.a.resize((size_t)g.w * g.h);
            for (int r = 0; r < g.h; r++) memcpy(&g.a[(size_t)r * g.w], s->bitmap.buffer + r * s->bitmap.pitch, g.w);
            out.g.push_back(std::move(g));
        }
        pen += s->advance.x;
    }
    out.advance = (int)((pen + 63) >> 6);
    return out;
}

int measure(Font& f, const std::string& t, int px) { return shapeLine(f, t, px, true).advance; }

// Greedy word wrap; a word wider than the line is split between characters.
std::vector<std::string> wrap(Font& f, const std::string& text, int px, int maxW) {
    std::vector<std::string> lines;
    if (measure(f, text, px) <= maxW || maxW <= 0) { lines.push_back(text); return lines; }
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        std::string word = text.substr(i, sp - i);
        i = sp + 1;
        const std::string trial = cur.empty() ? word : cur + " " + word;
        if (measure(f, trial, px) <= maxW) { cur = trial; continue; }
        if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
        while (measure(f, word, px) > maxW && word.size() > 1) {   // split a long word
            size_t cut = 0, j = 0;
            while (j < word.size()) {
                size_t k = j;
                nextCp(word, &k);
                if (measure(f, word.substr(0, k), px) > maxW) break;
                cut = j = k;
            }
            if (cut == 0) { size_t k = 0; nextCp(word, &k); cut = k; }
            lines.push_back(word.substr(0, cut));
            word = word.substr(cut);
        }
        cur = word;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace

int bodyPxFor(int w, int h, float fontScale) {
    const int unit = std::min(w, h);
    return std::clamp((int)lroundf(unit / 30.0f * fontScale), 14, 40);
}

bmap::NavModel Layout::nav() const {
    bmap::NavModel m;
    for (const auto& it : items) { m.itemY.push_back(it.y); m.itemH.push_back(it.h); }
    m.sections = sections;
    m.viewH = body.h;
    m.panel = {(float)panel.x, (float)panel.y, (float)panel.w, (float)panel.h};
    m.body = {(float)body.x, (float)body.y, (float)body.w, (float)body.h};
    const int unit = std::min(w, h);
    m.slop = std::max(10.0f, unit / 60.0f);
    m.swipeMin = unit * 0.08f;
    return m;
}

// ---- layout ----

namespace {

struct Metrics {
    int body, title, small, pad, rowH;
};

int lineH(Font& f, int px) { return f.ascent(px) + f.descent(px); }

TextBox textBox(Font& f, const std::string& t, int px, Role role, int x, int y, int w, int align = 0, bool dim = false) {
    TextBox b;
    b.text = t;
    b.px = px;
    b.role = role;
    b.textW = measure(f, t, px);
    b.r = {x, y, std::max(w, b.textW), lineH(f, px)};
    b.baseline = f.ascent(px);
    b.align = align;
    b.dim = dim;
    return b;
}

// Several lines of one text, stacked; returns the height used.
int addWrapped(Font& f, std::vector<TextBox>* out, const std::string& t, int px, Role role, int x, int y, int w,
               int align, bool dim, bool* wrapped) {
    const std::vector<std::string> lines = wrap(f, t, px, w);
    if (lines.size() > 1 && wrapped) *wrapped = true;
    int yy = y;
    for (const auto& l : lines) {
        TextBox b = textBox(f, l, px, role, x, yy, w, align, dim);
        if (b.textW > w) b.r.w = b.textW;   // one glyph wider than the line: never clipped
        out->push_back(b);
        yy += b.r.h;
    }
    return yy - y;
}

struct Cell { std::string left, right; bool dimLeft = false, dimRight = false; Role leftRole, rightRole; };

// A two-column grid of cells as items; k column pairs, each under the header when there is one.
void addGrid(Font& f, const Metrics& m, std::vector<Item>* items, const std::vector<Cell>& cells, int x0, int bodyW,
             int viewH, int section, bool rtl, bool* wrapped, int* y, const Cell* header = nullptr) {
    if (cells.empty()) return;
    const int px = m.body;
    int leftW = 0, rightW = 0;
    for (const auto& c : cells) {
        leftW = std::max(leftW, measure(f, c.left, px));
        rightW = std::max(rightW, measure(f, c.right, px));
    }
    if (*y > 0 && *y < viewH) viewH -= *y;   // what is above it on the first page (the section title)
    if (header) {
        leftW = std::max(leftW, measure(f, header->left, m.small));
        rightW = std::max(rightW, measure(f, header->right, m.small));
        viewH -= m.rowH;
    }
    const int gap = m.pad, colGap = m.pad * 2;
    const int pairW = leftW + gap + rightW;
    const int n = (int)cells.size();
    const int kmax = std::max(1, (bodyW + colGap) / std::max(1, pairW + colGap));
    const int kneed = std::max(1, (n * m.rowH + viewH - 1) / std::max(1, viewH));
    const int k = std::max(1, std::min(kmax, kneed));
    const int colW = (bodyW - (k - 1) * colGap) / k;
    int lw = leftW, rw = rightW;
    if (lw + gap + rw > colW) {   // wrap both, in proportion, the left at least a third
        lw = std::max(colW / 3, (colW - gap) * leftW / std::max(1, leftW + rightW));
        rw = colW - gap - lw;
    } else {
        rw = colW - gap - lw;
    }
    const int R = (n + k - 1) / k;
    const bool columnMajor = R * m.rowH <= viewH;
    const int lh = lineH(f, px);
    if (header) {
        Item it;
        it.section = section;
        it.y = *y;
        it.h = m.rowH;
        const int ty = it.y + (m.rowH - lineH(f, m.small)) / 2;
        for (int c = 0; c < k && c < n; c++) {
            const int cx = rtl ? x0 + bodyW - (c + 1) * colW - c * colGap : x0 + c * (colW + colGap);
            const int lx = rtl ? cx + rw + gap : cx;
            const int rx = rtl ? cx : cx + lw + gap;
            addWrapped(f, &it.texts, header->left, m.small, Role::kHeader, lx, ty, lw, rtl ? 2 : 0, true, wrapped);
            addWrapped(f, &it.texts, header->right, m.small, Role::kHeader, rx, ty, rw, rtl ? 2 : 0, true, wrapped);
        }
        *y += it.h;
        items->push_back(std::move(it));
    }
    for (int r = 0; r < R; r++) {
        Item it;
        it.section = section;
        it.y = *y;
        int maxH = m.rowH;
        for (int c = 0; c < k; c++) {
            const int idx = columnMajor ? c * R + r : r * k + c;
            if (idx >= n) continue;
            const Cell& cell = cells[idx];
            const int cx = rtl ? x0 + bodyW - (c + 1) * colW - c * colGap : x0 + c * (colW + colGap);
            const int lx = rtl ? cx + rw + gap : cx;
            const int rx = rtl ? cx : cx + lw + gap;
            const int pad = (m.rowH - lh) / 2;
            int h1 = addWrapped(f, &it.texts, cell.left, px, cell.leftRole, lx, it.y + pad, lw, rtl ? 2 : 0, cell.dimLeft, wrapped);
            int h2 = addWrapped(f, &it.texts, cell.right, px, cell.rightRole, rx, it.y + pad, rw, rtl ? 2 : 0, cell.dimRight, wrapped);
            maxH = std::max(maxH, std::max(h1, h2) + 2 * pad);
        }
        it.h = maxH;
        *y += it.h;
        items->push_back(std::move(it));
    }
}

struct Lab {
    int row;
    const bmap::Anchor* a;
    std::string text;
    int cw, ch;
    char side;
    int prio;          // lower moves to the strip first
    float x = 0, y = 0;   // chip top-left, drawing-local px
};

int prioOf(const bmap::Anchor* a) {
    const std::string& id = a->id;
    if (id == "btn-start" || id == "btn-select" || id == "btn-home" || id == "btn-mode" || id == "btn-back" ||
        id == "btn-plus" || id == "btn-minus" || id == "btn-capture")
        return 0;
    if (id == "btn-l3" || id == "btn-r3") return 1;
    return 2;
}

// Chips pushed apart along one axis, keeping their mean (the art's own label layout).
void spread(std::vector<Lab*>& g, bool vertical, int gap) {
    if (g.size() < 2) return;
    std::sort(g.begin(), g.end(), [&](const Lab* a, const Lab* b) { return vertical ? a->y < b->y : a->x < b->x; });
    float before = 0, after = 0;
    for (Lab* l : g) before += vertical ? l->y : l->x;
    for (size_t i = 1; i < g.size(); i++) {
        if (vertical) {
            const float min = g[i - 1]->y + g[i - 1]->ch + gap;
            if (g[i]->y < min) g[i]->y = min;
        } else {
            const float min = g[i - 1]->x + g[i - 1]->cw + gap;
            if (g[i]->x < min) g[i]->x = min;
        }
    }
    for (Lab* l : g) after += vertical ? l->y : l->x;
    const float shift = (before - after) / g.size();
    for (Lab* l : g) (vertical ? l->y : l->x) += shift;
}

struct Placed { float minX, minY, maxX, maxY; };

// Chips around the drawing at scale s; drawing-local px (the viewBox's origin at 0,0).
Placed place(std::vector<Lab>& labs, const bmap::Drawing& dr, float s, int gap) {
    for (Lab& l : labs) {
        const float lx = (l.a->lx - dr.vb[0]) * s, ly = (l.a->ly - dr.vb[1]) * s;
        switch (l.side) {
            case 'w': l.x = lx - l.cw; l.y = ly - l.ch / 2.0f; break;
            case 'e': l.x = lx; l.y = ly - l.ch / 2.0f; break;
            case 'n': l.x = lx - l.cw / 2.0f; l.y = ly - l.ch; break;
            default: l.x = lx - l.cw / 2.0f; l.y = ly; break;
        }
    }
    for (char side : {'w', 'e', 'n', 's'}) {
        std::vector<Lab*> g;
        for (Lab& l : labs) if (l.side == side) g.push_back(&l);
        spread(g, side == 'w' || side == 'e', gap);
    }
    Placed p{0, 0, dr.vb[2] * s, dr.vb[3] * s};
    for (const Lab& l : labs) {
        p.minX = std::min(p.minX, l.x); p.minY = std::min(p.minY, l.y);
        p.maxX = std::max(p.maxX, l.x + l.cw); p.maxY = std::max(p.maxY, l.y + l.ch);
    }
    return p;
}

bool segHitsRect(float x0, float y0, float x1, float y1, const Rect& r) {
    // Liang-Barsky against the rect shrunk by a pixel (a line may touch its neighbour's edge).
    const float rx0 = r.x + 1.0f, ry0 = r.y + 1.0f, rx1 = r.r() - 1.0f, ry1 = r.b() - 1.0f;
    if (rx1 <= rx0 || ry1 <= ry0) return false;
    float t0 = 0, t1 = 1;
    const float dx = x1 - x0, dy = y1 - y0;
    const float p[4] = {-dx, dx, -dy, dy}, q[4] = {x0 - rx0, rx1 - x0, y0 - ry0, ry1 - y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) { if (q[i] < 0) return false; continue; }
        const float t = q[i] / p[i];
        if (p[i] < 0) t0 = std::max(t0, t); else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    return true;
}

std::string dirWord(const std::string& console) {
    const size_t sp = console.find_last_of(' ');
    return sp == std::string::npos ? console : console.substr(sp + 1);
}

bool buildDrawing(Font& f, const Metrics& m, const Scene& sc, const bmap::Drawing& dr, int x0, int availW, int availH,
                  int y, Item* it, std::vector<int>* strip, int* labelledOut) {
    const int px = m.body, cpad = std::max(4, (int)lroundf(px * 0.45f)), ch = (int)lroundf(px * 1.5f), gap = 3;
    std::vector<Lab> labs;
    for (size_t i = 0; i < sc.rows.size(); i++) {
        const SceneRow& r = sc.rows[i];
        if (r.anchor.empty()) continue;
        const size_t colon = r.anchor.find(':');
        const bmap::Anchor* a = dr.find(r.anchor.substr(0, colon));
        if (!a) { strip->push_back((int)i); continue; }
        Lab l{(int)i, a, r.keys, 0, ch, a->side.empty() ? 'e' : a->side[0], prioOf(a)};
        if (colon != std::string::npos || a->kind == "dpad-dir") l.text = dirWord(r.console) + ": " + r.keys;
        l.cw = measure(f, l.text, px) + 2 * cpad;
        labs.push_back(l);
    }
    // The largest scale at which the drawing and its chips fit; a chip that still collides, or
    // whose leader crosses another chip, goes to the strip under the drawing (least important first).
    float s = 0;
    Placed p{};
    for (int round = 0; round < 64; round++) {
        float lo = 0.01f, hi = std::min(availW / dr.vb[2], availH / dr.vb[3]);
        for (int k = 0; k < 24; k++) {
            const float mid = (lo + hi) / 2;
            const Placed q = place(labs, dr, mid, gap);
            if (q.maxX - q.minX <= availW && q.maxY - q.minY <= availH) lo = mid; else hi = mid;
        }
        s = lo;
        p = place(labs, dr, s, gap);
        int worst = -1;
        auto consider = [&](int i) {
            if (worst < 0 || labs[i].prio < labs[worst].prio) worst = i;
        };
        auto rectOf = [&](const Lab& l) {
            return Rect{(int)floorf(l.x), (int)floorf(l.y), l.cw, l.ch};
        };
        for (size_t i = 0; i < labs.size(); i++)
            for (size_t j = i + 1; j < labs.size(); j++)
                if (rectOf(labs[i]).intersects(rectOf(labs[j]))) consider(labs[i].prio <= labs[j].prio ? (int)i : (int)j);
        if (worst < 0) {
            for (size_t i = 0; i < labs.size() && worst < 0; i++) {
                const Lab& l = labs[i];
                const float ax = (l.a->x - dr.vb[0]) * s, ay = (l.a->y - dr.vb[1]) * s;
                float sx, sy;
                switch (l.side) {
                    case 'w': sx = l.x + l.cw; sy = l.y + l.ch / 2.0f; break;
                    case 'e': sx = l.x; sy = l.y + l.ch / 2.0f; break;
                    case 'n': sx = l.x + l.cw / 2.0f; sy = l.y + l.ch; break;
                    default: sx = l.x + l.cw / 2.0f; sy = l.y; break;
                }
                for (size_t j = 0; j < labs.size(); j++)
                    if (j != i && segHitsRect(sx, sy, ax, ay, rectOf(labs[j]))) { consider(labs[i].prio <= labs[j].prio ? (int)i : (int)j); break; }
            }
        }
        // A drawing squeezed below a fifth of its room: the chips leave first.
        if (worst < 0 && !labs.empty() && s < 0.2f * std::min(availW / dr.vb[2], availH / dr.vb[3])) {
            for (size_t i = 0; i < labs.size(); i++) consider((int)i);
        }
        if (worst < 0) break;
        strip->push_back(labs[worst].row);
        labs.erase(labs.begin() + worst);
    }
    const float bw = p.maxX - p.minX, bh = p.maxY - p.minY;
    const float ox = x0 + (availW - bw) / 2.0f - p.minX;
    const float oy = y - p.minY;
    it->drawing = true;
    it->drawScale = s;
    it->drawBox = {(int)lroundf(ox), (int)lroundf(oy), (int)ceilf(dr.vb[2] * s), (int)ceilf(dr.vb[3] * s)};
    it->y = y;
    it->h = (int)ceilf(bh);
    const int lh = lineH(f, px);
    for (const Lab& l : labs) {
        const Rect chip{(int)lroundf(ox + l.x), (int)lroundf(oy + l.y), l.cw, l.ch};
        it->chips.push_back(chip);
        TextBox t = textBox(f, l.text, px, Role::kChip, chip.x + cpad, chip.y + (chip.h - lh) / 2, l.cw - 2 * cpad, 1,
                            sc.rows[l.row].dim);
        it->texts.push_back(t);
        const float ax = ox + (l.a->x - dr.vb[0]) * s, ay = oy + (l.a->y - dr.vb[1]) * s;
        float sx, sy;
        switch (l.side) {
            case 'w': sx = chip.r(); sy = chip.y + chip.h / 2.0f; break;
            case 'e': sx = chip.x; sy = chip.y + chip.h / 2.0f; break;
            case 'n': sx = chip.x + chip.w / 2.0f; sy = chip.b(); break;
            default: sx = chip.x + chip.w / 2.0f; sy = chip.y; break;
        }
        // To the button's edge, not its centre.
        const float dx = sx - ax, dy = sy - ay, d = sqrtf(dx * dx + dy * dy);
        const float rr = std::min(l.a->r * s, d);
        const float ex = d > 0 ? ax + dx / d * rr : ax, ey = d > 0 ? ay + dy / d * rr : ay;
        it->lines.push_back({sx, sy, ex, ey});
        if (!sc.rows[l.row].dim) it->lit.push_back(l.a->id);
    }
    *labelledOut = (int)labs.size();
    return true;
}

bool doLayout(Font& f, const Scene& sc, const Assets& as, int w, int h, int px, Layout* out) {
    Layout L;
    L.w = w; L.h = h;
    Metrics m;
    m.body = px;
    m.title = (int)lroundf(px * 1.3f);
    m.small = std::max(13, (int)lroundf(px * 0.85f));
    m.pad = (int)lroundf(px * 0.75f);
    m.rowH = (int)lroundf(px * 1.55f);
    L.bodyPx = m.body; L.titlePx = m.title; L.smallPx = m.small; L.pad = m.pad; L.rowH = m.rowH;
    const int unit = std::min(w, h);
    const float aspect = (float)w / h;
    const bool portrait = aspect < 0.8f;
    int pw, ph, margin = 0;
    if (aspect >= 1.70f) {
        ph = (int)lroundf(h * 0.90f);
        pw = std::min((int)lroundf(w * 0.80f), (int)lroundf(ph * 1.5f));
        L.centred = true;
    } else {
        margin = std::max(4, (int)lroundf(unit * 0.02f));
        pw = w - 2 * margin;
        ph = h - 2 * margin;
    }
    const int iw = pw - 2 * m.pad;
    bool wrapped = false;

    // Header and footer, relative to the panel's top and bottom.
    const int bar = sc.pal.accentBar ? std::max(2, m.pad / 3) : 0;
    std::vector<TextBox> head, foot;
    int hy = bar + m.pad;
    const int pageW = measure(f, "88/88", m.small) + m.pad;
    hy += addWrapped(f, &head, sc.title, m.title, Role::kTitle, m.pad, hy, iw - pageW, sc.rtl ? 2 : 0, false, nullptr);
    if (!sc.subtitle.empty()) hy += addWrapped(f, &head, sc.subtitle, m.small, Role::kSubtitle, m.pad, hy, iw, sc.rtl ? 2 : 0, false, nullptr);
    if (!sc.badge.empty()) hy += addWrapped(f, &head, sc.badge, m.small, Role::kBadge, m.pad, hy, iw, sc.rtl ? 2 : 0, false, nullptr);
    if (!sc.hint.empty()) hy += addWrapped(f, &head, sc.hint, m.small, Role::kHint, m.pad, hy, iw, sc.rtl ? 2 : 0, false, nullptr);
    const int headerH = hy + m.pad / 2;
    std::string footText, footPaged;
    for (const auto& s : sc.footer) footText += (footText.empty() ? "" : "     ") + s;
    footPaged = sc.pageHint.empty() ? footText : footText + (footText.empty() ? "" : "     ") + sc.pageHint;
    int footH = 0;
    if (!footPaged.empty()) {
        std::vector<TextBox> probe;
        footH = addWrapped(f, &probe, footPaged, m.small, Role::kFooter, m.pad, 0, iw, 1, false, nullptr);
    }
    const int footerH = footH + m.pad;

    // The body's width is known; its height is the panel's less the header and footer.
    const int bodyX = m.pad;
    const int viewMax = (portrait ? h - 2 * margin : ph) - headerH - footerH - m.pad / 2;
    int viewH = std::max(m.rowH * 2, viewMax);

    std::vector<Item> items;
    std::vector<int> sections;
    int y = 0;
    const bool titled = !sc.hotkeys.empty();
    auto sectionTitle = [&](const std::string& t, int section) {
        Item it;
        it.section = section;
        it.y = y;
        it.h = m.rowH;
        it.texts.push_back(textBox(f, t, m.body, Role::kSection, bodyX, y + (m.rowH - lineH(f, m.body)) / 2, iw, sc.rtl ? 2 : 0));
        y += it.h;
        items.push_back(std::move(it));
    };

    sections.push_back(0);
    if (titled) sectionTitle(sc.controlsTitle, 0);
    const bmap::Drawing* dr = as.catalog && !sc.drawing.empty() && !sc.table ? as.catalog->drawing(sc.drawing) : nullptr;
    std::vector<int> stripRows;
    if (dr) {
        // Rows with no anchor and the ones the drawing can't hold go to a strip under it.
        for (size_t i = 0; i < sc.rows.size(); i++) if (sc.rows[i].anchor.empty()) stripRows.push_back((int)i);
        const int titleH = titled ? m.rowH : 0;
        int availH = viewH - titleH - m.pad / 2;
        const int stripGuess = (int)stripRows.size() * m.rowH;
        if (stripGuess > 0 && stripGuess <= availH * 35 / 100) availH -= stripGuess;
        // The hotkeys share the first page when the drawing keeps at least 40 % of it.
        if (titled) {
            std::vector<Cell> hk;
            for (const auto& k : sc.hotkeys) hk.push_back({k.keys, k.action, false, false, Role::kKeys, Role::kBody});
            std::vector<Item> probe;
            int py = 0;
            bool w2 = false;
            addGrid(f, m, &probe, hk, bodyX, iw, viewH / 2, 1, sc.rtl, &w2, &py);
            if (availH - m.rowH - py >= viewH * 40 / 100) availH -= m.rowH + py;
        }
        if (portrait) {
            const float lbw = dr->labelBounds[2] - dr->labelBounds[0], lbh = dr->labelBounds[3] - dr->labelBounds[1];
            availH = std::min(availH, (int)(iw * lbh / std::max(1.0f, lbw)) + 4 * m.rowH);
        }
        Item it;
        std::vector<int> extra;
        buildDrawing(f, m, sc, *dr, bodyX, iw, std::max(availH, m.rowH * 3), y, &it, &extra, &L.labelled);
        y += it.h + m.pad / 2;
        it.h += m.pad / 2;
        items.push_back(std::move(it));
        L.stripRows = (int)extra.size();
        stripRows.insert(stripRows.end(), extra.begin(), extra.end());
        std::sort(stripRows.begin(), stripRows.end());
        L.rowsPlaced = L.labelled + (int)stripRows.size();
        std::vector<Cell> cells;
        for (int i : stripRows) {
            const SceneRow& r = sc.rows[i];
            cells.push_back({r.console, r.keys, false, r.dim || sc.greyed, Role::kBody, Role::kKeys});
        }
        addGrid(f, m, &items, cells, bodyX, iw, viewH, 0, sc.rtl, &wrapped, &y);
    } else if (!sc.rows.empty()) {
        std::vector<Cell> cells;
        for (const auto& r : sc.rows)
            cells.push_back({r.console, r.keys, sc.greyed, r.dim || sc.greyed, Role::kBody, Role::kKeys});
        L.rowsPlaced = (int)cells.size();
        const Cell header{sc.consoleHeader, sc.padHeader, true, true, Role::kHeader, Role::kHeader};
        addGrid(f, m, &items, cells, bodyX, iw, viewH, 0, sc.rtl, &wrapped, &y, &header);
    } else {
        Item it;
        it.y = y;
        const int hgt = addWrapped(f, &it.texts, sc.hint.empty() ? sc.badge : sc.hint, m.body, Role::kHint, bodyX, y, iw,
                                   1, false, &wrapped);
        it.h = std::max(m.rowH, hgt);
        y += it.h;
        items.push_back(std::move(it));
    }
    if (titled) {
        sections.push_back((int)items.size());
        sectionTitle(sc.hotkeysTitle, 1);
        std::vector<Cell> cells;
        for (const auto& hk : sc.hotkeys) cells.push_back({hk.keys, hk.action, false, false, Role::kKeys, Role::kBody});
        L.hotkeysPlaced = (int)cells.size();
        addGrid(f, m, &items, cells, bodyX, iw, dr ? viewH / 2 : viewH, 1, sc.rtl, &wrapped, &y);
    }
    const int contentH = y;

    // Portrait: the panel is only as tall as its content, centred.
    if (portrait) {
        const int need = headerH + std::min(contentH, viewMax) + m.pad / 2 + footerH;
        ph = std::min(h - 2 * margin, need);
        viewH = std::min(contentH, viewMax);
    }
    const int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    L.panel = {px0, py0, pw, ph};
    L.header = {px0, py0, pw, headerH};
    L.footer = {px0, py0 + ph - footerH, pw, footerH};
    L.body = {px0 + bodyX, py0 + headerH, iw, std::max(0, ph - headerH - footerH - m.pad / 2)};
    if (!portrait) viewH = L.body.h;
    L.body.h = viewH;
    L.pageBox = {px0 + pw - m.pad - pageW, py0 + bar + m.pad, pageW, lineH(f, m.small)};
    if (sc.rtl) L.pageBox.x = px0 + m.pad;
    for (auto& t : head) { t.r.x += px0; t.r.y += py0; if (sc.rtl && t.role == Role::kTitle) t.r.x += pageW; L.fixed.push_back(t); }
    const std::string& footShown = contentH > viewH ? footPaged : footText;
    if (!footShown.empty()) addWrapped(f, &foot, footShown, m.small, Role::kFooter, m.pad, 0, iw, 1, false, nullptr);
    for (auto& t : foot) { t.r.x += px0; t.r.y += L.footer.y + m.pad / 2; L.fixed.push_back(t); }
    // Items: x already includes bodyX (panel-relative); make them absolute.
    for (auto& it : items) {
        for (auto& t : it.texts) t.r.x += px0;
        for (auto& c : it.chips) c.x += px0;
        for (auto& l : it.lines) { l.x0 += px0; l.x1 += px0; }
        if (it.drawing) it.drawBox.x += px0;
    }
    L.items = std::move(items);
    L.sections = sections;
    L.contentH = contentH;
    L.wrapped = wrapped;
    *out = std::move(L);
    return true;
}

}  // namespace

bool layout(const Scene& sc, const Assets& as, int w, int h, Layout* out) {
    if (w <= 0 || h <= 0) return false;
    Font f(as.fontPath, as.fallbackFonts);
    if (!f.ok()) return false;
    const int base = bodyPxFor(w, h, sc.fontScale);
    // Shrink in 8 % steps while some body text needs a second line, never below 14 px. The header
    // and footer may wrap: a long game or file name should not shrink the whole panel.
    for (float scale = 1.0f;; scale *= 0.92f) {
        const int px = std::max(14, (int)lroundf(base * scale));
        if (!doLayout(f, sc, as, w, h, px, out)) return false;
        if (!out->wrapped || px == 14) return true;
    }
}

// ---- drawing ----

namespace {

struct Canvas {
    std::vector<uint8_t>* px;
    int w, h;
    Rect clip;
    void blend(int x, int y, Rgba c, float cov) {
        if (x < clip.x || y < clip.y || x >= clip.r() || y >= clip.b() || x < 0 || y < 0 || x >= w || y >= h) return;
        const float a = c.a / 255.0f * std::clamp(cov, 0.0f, 1.0f);
        if (a <= 0) return;
        uint8_t* p = &(*px)[((size_t)y * w + x) * 4];
        const float da = p[3] / 255.0f;
        const float oa = a + da * (1 - a);
        if (oa <= 0) return;
        p[0] = (uint8_t)lroundf((c.r * a + p[0] * da * (1 - a)) / oa);
        p[1] = (uint8_t)lroundf((c.g * a + p[1] * da * (1 - a)) / oa);
        p[2] = (uint8_t)lroundf((c.b * a + p[2] * da * (1 - a)) / oa);
        p[3] = (uint8_t)lroundf(oa * 255);
    }
    void roundRect(const Rect& r, int rad, Rgba c) {
        rad = std::min(rad, std::min(r.w, r.h) / 2);
        for (int y = r.y; y < r.b(); y++)
            for (int x = r.x; x < r.r(); x++) {
                float cov = 1;
                if (rad > 0) {
                    const float cx = x + 0.5f < r.x + rad ? r.x + rad : x + 0.5f > r.r() - rad ? r.r() - rad : x + 0.5f;
                    const float cy = y + 0.5f < r.y + rad ? r.y + rad : y + 0.5f > r.b() - rad ? r.b() - rad : y + 0.5f;
                    const float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
                    cov = std::clamp(rad - d + 0.5f, 0.0f, 1.0f);
                }
                blend(x, y, c, cov);
            }
    }
    void line(float x0, float y0, float x1, float y1, float width, Rgba c) {
        const int minX = (int)floorf(std::min(x0, x1) - width), maxX = (int)ceilf(std::max(x0, x1) + width);
        const int minY = (int)floorf(std::min(y0, y1) - width), maxY = (int)ceilf(std::max(y0, y1) + width);
        const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
        for (int y = minY; y <= maxY; y++)
            for (int x = minX; x <= maxX; x++) {
                const float px0 = x + 0.5f, py0 = y + 0.5f;
                float t = len2 > 0 ? ((px0 - x0) * dx + (py0 - y0) * dy) / len2 : 0;
                t = std::clamp(t, 0.0f, 1.0f);
                const float d = hypotf(px0 - (x0 + t * dx), py0 - (y0 + t * dy));
                blend(x, y, c, width / 2 + 0.5f - d);
            }
    }
    void dot(float cx, float cy, float r, Rgba c) {
        for (int y = (int)floorf(cy - r - 1); y <= (int)ceilf(cy + r + 1); y++)
            for (int x = (int)floorf(cx - r - 1); x <= (int)ceilf(cx + r + 1); x++)
                blend(x, y, c, r + 0.5f - hypotf(x + 0.5f - cx, y + 0.5f - cy));
    }
    void triangle(float cx, float cy, float s, bool up, Rgba c) {
        for (int y = (int)floorf(cy - s); y <= (int)ceilf(cy + s); y++)
            for (int x = (int)floorf(cx - s); x <= (int)ceilf(cx + s); x++) {
                const float fy = up ? (y + 0.5f - (cy - s / 2)) / s : ((cy + s / 2) - (y + 0.5f)) / s;
                const float half = fy * s;
                if (fy < 0 || fy > 1) continue;
                blend(x, y, c, std::clamp(half - fabsf(x + 0.5f - cx) + 0.5f, 0.0f, 1.0f));
            }
    }
    void text(Font& f, const TextBox& t, int dy, Rgba c) {
        const Shaped s = shapeLine(f, t.text, t.px, false);
        int x = t.r.x;
        if (t.align == 1) x = t.r.x + (t.r.w - s.advance) / 2;
        else if (t.align == 2) x = t.r.r() - s.advance;
        const int base = t.r.y + dy + t.baseline;
        for (const auto& g : s.g)
            for (int r = 0; r < g.h; r++)
                for (int k = 0; k < g.w; k++) {
                    const int a = g.a[(size_t)r * g.w + k];
                    if (a) blend(x + g.x + k, base + g.y + r, c, a / 255.0f);
                }
    }
};

uint32_t abgr(Rgba c) { return (uint32_t)c.a << 24 | (uint32_t)c.b << 16 | (uint32_t)c.g << 8 | c.r; }

Rgba roleColor(const Scene& sc, const TextBox& t) {
    const Palette& p = sc.pal;
    Rgba c = p.text;
    switch (t.role) {
        case Role::kSubtitle: case Role::kHeader: case Role::kFooter: case Role::kPage: c = p.textDim; break;
        case Role::kBadge: case Role::kSection: case Role::kKeys: c = p.accent; break;
        case Role::kChip: c = p.chipText; break;
        default: break;
    }
    if (t.dim || (sc.greyed && (t.role == Role::kBody || t.role == Role::kKeys))) c = p.textDim;
    return c;
}

}  // namespace

bool render(const Scene& sc, const Assets& as, const Layout& L, float scrollPx, std::vector<uint8_t>* rgba, bool moving) {
    Font f(as.fontPath, as.fallbackFonts);
    if (!f.ok() || L.w <= 0 || L.h <= 0) return false;
    rgba->assign((size_t)L.w * L.h * 4, 0);
    for (size_t i = 0; i < rgba->size(); i += 4) {
        (*rgba)[i] = sc.pal.dim.r; (*rgba)[i + 1] = sc.pal.dim.g; (*rgba)[i + 2] = sc.pal.dim.b; (*rgba)[i + 3] = sc.pal.dim.a;
    }
    Canvas cv{rgba, L.w, L.h, {0, 0, L.w, L.h}};
    const int rad = sc.pal.radius * L.bodyPx / 100;
    cv.roundRect(L.panel, rad, sc.pal.panel);
    if (sc.pal.accentBar) cv.roundRect({L.panel.x, L.panel.y, L.panel.w, std::max(2, L.pad / 3)}, 0, sc.pal.accent);
    for (const auto& t : L.fixed) cv.text(f, t, 0, roleColor(sc, t));

    bmap::Nav nav;
    nav.setModel(L.nav());
    nav.scrollTo(scrollPx);
    const float scroll = nav.scroll();
    if (nav.pageCount() > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", nav.page(), nav.pageCount());
        TextBox pg;
        pg.text = buf;
        pg.px = L.smallPx;
        pg.r = L.pageBox;
        pg.baseline = f.ascent(L.smallPx);
        pg.align = sc.rtl ? 0 : 2;
        pg.role = Role::kPage;
        cv.text(f, pg, 0, sc.pal.textDim);
    }

    cv.clip = L.body;
    const int dy = L.body.y - (int)lroundf(scroll);
    const float lw = std::max(1.5f, L.bodyPx / 12.0f);
    std::unique_ptr<NSVGrasterizer, void (*)(NSVGrasterizer*)> rast(nullptr, nsvgDeleteRasterizer);
    for (const auto& it : L.items) {
        if (it.y + it.h <= scroll || it.y >= scroll + L.body.h) continue;
        // At rest only whole items show; while the content moves the body's edge cuts them.
        if (!moving && (it.y < scroll - 0.5f || it.y + it.h > scroll + L.body.h + 0.5f)) continue;
        if (it.drawing && as.catalog) {
            const bmap::Drawing* dr = as.catalog->drawing(sc.drawing);
            std::string svg;
            if (dr) {
                if (FILE* fp = fopen((as.artDir + "/" + dr->file).c_str(), "rb")) {
                    char tmp[16384];
                    size_t n;
                    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0 && svg.size() < (2u << 20)) svg.append(tmp, n);
                    fclose(fp);
                }
            }
            if (!svg.empty()) {
                std::vector<char> buf(svg.begin(), svg.end());
                buf.push_back('\0');
                NSVGimage* img = nsvgParse(buf.data(), "px", 96.0f);
                if (img) {
                    // The art's two tones to the theme's, and labelled buttons in the accent colour.
                    const uint32_t ink = 0xEDEDED, body = 0x2E2E2E;
                    for (NSVGshape* s = img->shapes; s; s = s->next) {
                        const bool lit = std::find(it.lit.begin(), it.lit.end(), std::string(s->id)) != it.lit.end();
                        for (NSVGpaint* pnt : {&s->fill, &s->stroke}) {
                            if (pnt->type != NSVG_PAINT_COLOR) continue;
                            const uint32_t rgb = pnt->color & 0xFFFFFF;
                            if (rgb == ink) pnt->color = abgr(lit && pnt == &s->fill ? sc.pal.accent : sc.pal.ink);
                            else if (rgb == body) pnt->color = abgr(sc.pal.body);
                        }
                    }
                    const Rect box = it.drawBox;
                    if (box.w > 0 && box.h > 0 && box.w < 8192 && box.h < 8192) {
                        if (!rast) rast.reset(nsvgCreateRasterizer());
                        std::vector<uint8_t> pix((size_t)box.w * box.h * 4, 0);
                        nsvgRasterize(rast.get(), img, -dr->vb[0] * it.drawScale, -dr->vb[1] * it.drawScale, it.drawScale,
                                      pix.data(), box.w, box.h, box.w * 4);
                        for (int y = 0; y < box.h; y++)
                            for (int x = 0; x < box.w; x++) {
                                const uint8_t* q = &pix[((size_t)y * box.w + x) * 4];
                                if (!q[3]) continue;
                                // nanosvg writes premultiplied colour.
                                const float a = q[3] / 255.0f;
                                Rgba c{(uint8_t)std::min(255.0f, q[0] / a), (uint8_t)std::min(255.0f, q[1] / a),
                                       (uint8_t)std::min(255.0f, q[2] / a), 255};
                                cv.blend(box.x + x, box.y + dy + y, c, (sc.greyed ? 0.5f : 1.0f) * a);
                            }
                    }
                    nsvgDelete(img);
                }
            }
        }
        for (const auto& l : it.lines) {
            cv.line(l.x0, l.y0 + dy, l.x1, l.y1 + dy, lw, sc.pal.line);
            cv.dot(l.x1, l.y1 + dy, lw * 1.2f, sc.pal.line);
        }
        for (size_t i = 0; i < it.chips.size(); i++) {
            Rect c = it.chips[i];
            c.y += dy;
            cv.roundRect(c, c.h / 2, sc.pal.chip);
        }
        for (const auto& t : it.texts) cv.text(f, t, dy, roleColor(sc, t));
    }
    // More above or below: small arrows at the body's edge.
    cv.clip = {0, 0, L.w, L.h};
    const float ts = std::max(6.0f, L.smallPx * 0.6f);
    const float ax = sc.rtl ? L.body.x + ts : L.body.r() - ts;
    if (nav.canUp()) cv.triangle(ax, L.body.y + ts * 0.6f, ts, true, sc.pal.accent);
    if (nav.canDown()) cv.triangle(ax, L.body.b() - ts * 0.6f, ts, false, sc.pal.accent);
    return true;
}

}  // namespace bmart
}  // namespace android
