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

// The boot screen's picture: the title and the logo centred in the frame, and a status line, drawn for any screen size from the vector logo and the font. Nano draws it on the device
// (NanoBootScreen.cpp) and the host tool nano-bootlogo draws the same picture, without a status,
// for the bootloader's logo partition, so the bootloader's picture turns into Nano's unchanged.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nanoboot {

struct Art {
    std::string svgPath;    // the logo
    std::string fontPath;   // the text's font
    std::vector<std::string> fallbackFonts;   // for the characters it lacks (the status line's languages)
    std::string title = "Thimsy";
    uint8_t background[3] = {0x1a, 0x1a, 0x2e};   // Nano's "Midnight" (Background Colour presets)
};

struct Layout {   // in pixels of a w x h frame
    int w = 0, h = 0;
    float logoScale = 0;                 // SVG units -> pixels
    int logoX = 0, logoY = 0;            // where the SVG's origin lands
    int logoW = 0, logoH = 0;            // the logo's drawn box
    int logoLeft = 0, logoTop = 0;
    float titlePx = 0, statusPx = 0;
    int titleBase = 0, statusBase = 0;   // text baselines
};

// The frame's layout. False when the logo cannot be read.
bool layout(const Art& art, int w, int h, Layout* out);

// The whole frame as RGBA (w x h, opaque): the background, the title and the logo.
bool renderBase(const Art& art, int w, int h, std::vector<uint8_t>* rgba);

// A line of text, centred: its RGBA pixels (white, alpha = coverage) and where they go in the
// frame. Empty when the text is empty or the font cannot be read.
struct Line { int x = 0, y = 0, w = 0, h = 0; std::vector<uint8_t> rgba; };
Line renderStatus(const Art& art, int w, int h, const std::string& text);

}  // namespace nanoboot
