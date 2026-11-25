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

// The button map panel's layout and its CPU drawing (button_map_overlay_plan.md §7): the console's
// controller drawing with each button labelled by the pad button that presses it, or a two-column
// table, then the hotkeys, in one RGBA frame drawn with nanosvg and FreeType (the boot screen's way,
// bootscreen/NanoBootArt). The layout is a pure function of the scene and the frame size, and
// nothing is drawn outside its rectangles, so the host tests can check it at any size; Nano and the
// golden test draw the same pixels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "NanoButtonMap.h"
#include "NanoButtonMapNav.h"

namespace android {
namespace bmart {

struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 255; };
struct Palette {
    Rgba dim{0, 0, 0, 150};            // over the game or menu
    Rgba panel{18, 20, 26, 240};       // NanoBiosUi's dark panel (0.07 0.08 0.10, 94%)
    Rgba accent{80, 150, 255, 255};
    Rgba text{236, 236, 236, 255};
    Rgba textDim{150, 156, 168, 255};
    Rgba ink{0xED, 0xED, 0xED, 255};   // the drawing's two tones (controllers.json colors)
    Rgba body{0x2E, 0x2E, 0x2E, 255};
    Rgba chip{44, 52, 66, 255};
    Rgba chipText{255, 255, 255, 255};
    Rgba line{150, 170, 200, 255};
    int radius = 0;                    // corner radius in body-text units x 100 (0 square)
    bool accentBar = true;
};
Palette paletteXmb();
Palette paletteDsi();      // the DSi theme's light message box
Palette paletteMinima();

struct SceneRow {
    std::string anchor;    // the drawing's anchor ("btn-a", "stick-l:up"), "" for a text row
    std::string console;   // "A", "Turbo B"
    std::string keys;      // "B", "D-pad / Left stick", "—"
    bool dim = false;      // unbound
};
struct SceneHotkey { std::string keys, action; };

// Everything the panel shows, already in the user's language.
struct Scene {
    std::string title, subtitle, badge;
    std::string hint;                  // the greyed browser's reason, or where to map
    std::string drawing;               // drawing id; "" = table only
    bool table = false;                // the table even when there is a drawing
    bool greyed = false;
    std::string controlsTitle = "Controls", hotkeysTitle = "Hotkeys";
    std::string consoleHeader = "Console button", padHeader = "Your controller";
    std::vector<SceneRow> rows;
    std::vector<SceneHotkey> hotkeys;
    std::vector<std::string> footer;   // "B Close", "Y Table"
    std::string pageHint;              // "L1/R1 Page", in the footer only when there is more than one page
    Palette pal;
    bool rtl = false;
    float fontScale = 1.0f;
};
// The scene for a result, in English (the caller translates the strings before drawing).
Scene sceneFor(const bmap::MapResult& r, const bmap::PadModel& pad);
// The scene's text in the user's language (bmap::translate on each string; a footer hint is its
// button's label and a word, translated apart).
void translateScene(Scene* s, const bmap::Translator& tr);
bool sceneFromJson(const std::string& json, Scene* out);   // the golden scenes (tools/nano_buttonmap.cpp)

struct Assets {
    std::string fontPath;
    std::vector<std::string> fallbackFonts;
    std::string artDir;                // controllers.json and svg/
    const bmap::Catalog* catalog = nullptr;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int r() const { return x + w; }
    int b() const { return y + h; }
    bool inside(const Rect& o) const { return x >= o.x && y >= o.y && r() <= o.r() && b() <= o.b(); }
    bool intersects(const Rect& o) const { return x < o.r() && o.x < r() && y < o.b() && o.y < b(); }
};
enum class Role : uint8_t { kTitle, kSubtitle, kBadge, kHint, kSection, kHeader, kBody, kKeys, kChip, kFooter, kPage };
struct TextBox {
    Rect r;                // the box the text is drawn in; never narrower than textW
    std::string text;      // logical order (bidi is applied when drawn)
    int px = 0;
    int textW = 0;         // measured advance width
    int baseline = 0;      // from r.y
    Role role = Role::kBody;
    int align = 0;         // 0 start, 1 centre, 2 end
    bool dim = false;
};
struct Leader { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
struct Item {              // one body entry; y from the content's top
    int y = 0, h = 0, section = 0;
    std::vector<TextBox> texts;
    std::vector<Rect> chips;
    std::vector<Leader> lines;
    bool drawing = false;
    Rect drawBox;          // the SVG's viewBox drawn here
    float drawScale = 0;
    std::vector<std::string> lit;   // anchors with a label (drawn in the accent colour)
};
struct Layout {
    int w = 0, h = 0;
    bool centred = false;
    Rect panel, header, body, footer, pageBox;
    int bodyPx = 0, titlePx = 0, smallPx = 0, pad = 0, rowH = 0;
    std::vector<TextBox> fixed;        // header and footer
    std::vector<Item> items;
    int contentH = 0;
    std::vector<int> sections;         // first item of each section
    int stripRows = 0;                 // anchored rows the drawing could not label (in the strip)
    int labelled = 0;                  // rows labelled on the drawing
    int rowsPlaced = 0;                // scene rows shown somewhere (labelled, strip or table)
    int hotkeysPlaced = 0;
    bool wrapped = false;              // some text needed a second line at the floor size
    bmap::NavModel nav() const;
};

// The panel at w x h. False when the font cannot be read.
bool layout(const Scene& s, const Assets& a, int w, int h, Layout* out);
// The frame as RGBA (w x h, straight alpha), the body scrolled by scrollPx. At rest (!moving) an
// item the body's edge would cut is left out; while a drag or fling moves the content it is cut.
bool render(const Scene& s, const Assets& a, const Layout& l, float scrollPx, std::vector<uint8_t>* rgba,
            bool moving = false);

// Body text size for a frame: min(w,h)/30, at least 14 px, at most 40.
int bodyPxFor(int w, int h, float fontScale = 1.0f);

}  // namespace bmart
}  // namespace android
