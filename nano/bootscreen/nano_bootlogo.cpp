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

// nano-bootlogo SVG FONT WIDTH HEIGHT OUT.rgba [STATUS]
// The boot screen as Nano draws it (NanoBootArt), as raw RGBA: for the bootloader's logo partition
// (no status) and for checking the layout at other screen sizes.

#include "NanoBootArt.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: nano-bootlogo SVG FONT WIDTH HEIGHT OUT.rgba [STATUS]\n");
        return 2;
    }
    nanoboot::Art art;
    art.svgPath = argv[1];
    art.fontPath = argv[2];
    const int w = atoi(argv[3]), h = atoi(argv[4]);
    std::vector<uint8_t> rgba;
    if (!nanoboot::renderBase(art, w, h, &rgba)) {
        fprintf(stderr, "nano-bootlogo: cannot draw %s with %s\n", argv[1], argv[2]);
        return 1;
    }
    if (argc == 7) {
        const nanoboot::Line s = nanoboot::renderStatus(art, w, h, argv[6]);
        for (int y = 0; y < s.h; y++)
            for (int x = 0; x < s.w; x++) {
                const int fx = s.x + x, fy = s.y + y;
                if (fx < 0 || fy < 0 || fx >= w || fy >= h) continue;
                const uint8_t* p = &s.rgba[((size_t)y * s.w + x) * 4];
                uint8_t* q = &rgba[((size_t)fy * w + fx) * 4];
                for (int k = 0; k < 3; k++) q[k] = (uint8_t)((p[k] * p[3] + q[k] * (255 - p[3]) + 127) / 255);
            }
    }
    FILE* f = fopen(argv[5], "wb");
    if (!f || fwrite(rgba.data(), 1, rgba.size(), f) != rgba.size()) { perror(argv[5]); return 1; }
    fclose(f);
    return 0;
}
