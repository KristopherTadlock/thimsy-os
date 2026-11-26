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

// The library-independent parts of the AImageDecoder shim (imagedecoder.cpp), declared for its
// tests: format sniffing, the header readers (which need no image library) and the resampler.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace nanocompat {
namespace img {

enum class Format { Unknown, Png, Jpeg, Gif, Webp, Bmp, Heif, Avif };

const char* mimeType(Format f);

// The format from the first bytes; Unknown when none matches.
Format sniff(const uint8_t* data, size_t size);

// Whether this build has the library for the format (decided by __has_include when it was built).
bool libraryAvailable(Format f);

struct Header {
    Format format = Format::Unknown;
    int width = 0;   // as displayed: after a JPEG's EXIF orientation
    int height = 0;
    bool hasAlpha = false;
    bool animated = false;
    int orientation = 1;  // JPEG EXIF orientation (1 to 8); 1 for the other formats
};

enum class HeaderResult { Ok, Incomplete, Invalid, Unsupported };

// Reads the format, size, alpha and animation from the data alone (no library), so it is the same
// whichever libraries this build has.
HeaderResult readHeader(const uint8_t* data, size_t size, Header* out);

// Resamples RGBA8 (straight alpha) src (sw x sh, sstride bytes per row) to dst (dw x dh), area
// averaging when shrinking and bilinear when growing, filtered in premultiplied space when
// hasAlpha. The result is premultiplied unless unpremulOut (or the image is opaque).
void resample(const uint8_t* src, int sw, int sh, size_t sstride, uint8_t* dst, int dw, int dh, size_t dstride,
              bool hasAlpha, bool unpremulOut);

// Premultiplies straight-alpha RGBA8 in place.
void premultiply(uint8_t* px, int w, int h, size_t stride);

// Rotates or flips tight RGBA8 pixels by an EXIF orientation (1 to 8). *w and *h are the stored
// size on entry and the displayed size on return.
void applyOrientation(const uint8_t* src, int* w, int* h, int orientation, uint8_t* dst);

}  // namespace img
}  // namespace nanocompat
