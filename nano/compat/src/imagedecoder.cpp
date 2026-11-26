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

// AImageDecoder (android/imagedecoder.h) on libpng, libjpeg, libwebp and giflib, for the Linux and
// host builds of Nano (IMPLEMENTATION.md B.2, decision I30). Each library is used only if its
// header was found when this file was compiled; a format without one is unsupported, so the shim
// compiles and works (with fewer formats) on a machine that has none of them. The build must link
// the libraries whose headers it found: nano/test/host/compat/compat.mk probes the same headers.
// NANOCOMPAT_NO_PNG, _NO_JPEG, _NO_WEBP and _NO_GIF switch one off.

#include <android/imagedecoder.h>

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "imagedecoder_internal.h"

#if defined(__has_include)
#if __has_include(<png.h>) && !defined(NANOCOMPAT_NO_PNG)
#include <png.h>
#if defined(PNG_LIBPNG_VER) && PNG_LIBPNG_VER >= 10600 && defined(PNG_SIMPLIFIED_READ_SUPPORTED)
#define NANOCOMPAT_HAVE_PNG 1
#endif
#endif
#if __has_include(<jpeglib.h>) && !defined(NANOCOMPAT_NO_JPEG)
#include <jpeglib.h>
#define NANOCOMPAT_HAVE_JPEG 1
#endif
#if __has_include(<webp/decode.h>) && !defined(NANOCOMPAT_NO_WEBP)
#include <webp/decode.h>
#define NANOCOMPAT_HAVE_WEBP 1
#if __has_include(<webp/demux.h>)
#include <webp/demux.h>
#define NANOCOMPAT_HAVE_WEBP_DEMUX 1
#endif
#endif
#if __has_include(<gif_lib.h>) && !defined(NANOCOMPAT_NO_GIF)
#include <gif_lib.h>
#if defined(GIFLIB_MAJOR) && (GIFLIB_MAJOR > 5 || (GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1))
#define NANOCOMPAT_HAVE_GIF 1
#endif
#endif
#endif

// ADATASPACE_SRGB (android/data_space.h): BT709 standard, sRGB transfer, full range.
static constexpr int32_t kDataSpaceSrgb = (1 << 16) | (2 << 22) | (1 << 27);
// The largest image decoded, in pixels (256 MiB of RGBA).
static constexpr int64_t kMaxPixels = int64_t(1) << 26;

struct AImageDecoderHeaderInfo {
    int32_t width = 0;
    int32_t height = 0;
    const char* mimeType = "";
    int alphaFlags = ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
};

struct AImageDecoderFrameInfo {
    int64_t durationNs = 0;
    ARect rect = {0, 0, 0, 0};
    bool hasAlpha = false;
    int32_t disposeOp = ANDROID_IMAGE_DECODER_DISPOSE_OP_NONE;
    int32_t blendOp = ANDROID_IMAGE_DECODER_BLEND_OP_SRC_OVER;
};

namespace nanocompat {
namespace img {

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le24(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16); }
uint32_t le32(const uint8_t* p) { return le24(p) | (uint32_t(p[3]) << 24); }

bool sizeOk(int64_t w, int64_t h) { return w > 0 && h > 0 && w <= 0x7fffffff && h <= 0x7fffffff; }

HeaderResult readPng(const uint8_t* d, size_t n, Header* h) {
    // Signature (8), then IHDR: length (4), "IHDR", width (4), height (4), depth, colour type.
    if (n < 8 + 8 + 13) return HeaderResult::Incomplete;
    if (be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4) != 0) return HeaderResult::Invalid;
    const int64_t w = be32(d + 16), ht = be32(d + 20);
    if (!sizeOk(w, ht)) return HeaderResult::Invalid;
    const uint8_t colourType = d[25];
    h->width = int(w);
    h->height = int(ht);
    h->hasAlpha = colourType == 4 || colourType == 6;
    // A tRNS chunk before the image data also means alpha.
    size_t pos = 8 + 12 + 13;
    while (!h->hasAlpha && pos + 8 <= n) {
        const uint32_t len = be32(d + pos);
        const uint8_t* type = d + pos + 4;
        if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0) break;
        if (memcmp(type, "tRNS", 4) == 0) h->hasAlpha = true;
        if (len > n) break;
        pos += size_t(len) + 12;
    }
    return HeaderResult::Ok;
}

// The EXIF orientation in an APP1 segment's payload, or 0.
int exifOrientation(const uint8_t* p, size_t n) {
    if (n < 6 + 8 || memcmp(p, "Exif\0\0", 6) != 0) return 0;
    const uint8_t* t = p + 6;
    const size_t tn = n - 6;
    bool le;
    if (t[0] == 'I' && t[1] == 'I') {
        le = true;
    } else if (t[0] == 'M' && t[1] == 'M') {
        le = false;
    } else {
        return 0;
    }
    auto u16 = [&](size_t off) { return le ? le16(t + off) : be16(t + off); };
    auto u32 = [&](size_t off) { return le ? le32(t + off) : be32(t + off); };
    if (u16(2) != 42) return 0;
    const size_t ifd = u32(4);
    if (ifd + 2 > tn) return 0;
    const size_t count = u16(ifd);
    for (size_t i = 0; i < count; i++) {
        const size_t e = ifd + 2 + i * 12;
        if (e + 12 > tn) return 0;
        if (u16(e) == 0x0112 && u16(e + 2) == 3) {  // Orientation, SHORT
            const int v = u16(e + 8);
            return v >= 1 && v <= 8 ? v : 0;
        }
    }
    return 0;
}

HeaderResult readJpeg(const uint8_t* d, size_t n, Header* h) {
    size_t pos = 2;  // after SOI
    int orientation = 1;
    for (;;) {
        while (pos < n && d[pos] != 0xFF) pos++;   // tolerate junk between segments
        while (pos < n && d[pos] == 0xFF) pos++;   // fill bytes
        if (pos >= n) return HeaderResult::Incomplete;
        const uint8_t m = d[pos++];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  // no length
        if (m == 0xD9 || m == 0xDA) return HeaderResult::Invalid;           // EOI or SOS before SOF
        if (pos + 2 > n) return HeaderResult::Incomplete;
        const size_t len = be16(d + pos);
        if (len < 2) return HeaderResult::Invalid;
        const uint8_t* payload = d + pos + 2;
        const size_t plen = len - 2;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof) {
            if (pos + 2 + 5 > n) return HeaderResult::Incomplete;
            if (plen < 5) return HeaderResult::Invalid;
            const int ht = be16(payload + 1), w = be16(payload + 3);
            if (w <= 0 || ht <= 0) return HeaderResult::Invalid;
            h->orientation = orientation;
            const bool swap = orientation >= 5;
            h->width = swap ? ht : w;
            h->height = swap ? w : ht;
            h->hasAlpha = false;
            return HeaderResult::Ok;
        }
        if (m == 0xE1 && pos + len <= n) {
            const int o = exifOrientation(payload, plen);
            if (o != 0) orientation = o;
        }
        pos += len;
    }
}

HeaderResult readGif(const uint8_t* d, size_t n, Header* h) {
    if (n < 13) return HeaderResult::Incomplete;
    const int w = le16(d + 6), ht = le16(d + 8);
    if (w <= 0 || ht <= 0) return HeaderResult::Invalid;
    h->width = w;
    h->height = ht;
    size_t pos = 13;
    if (d[10] & 0x80) pos += size_t(3) << ((d[10] & 7) + 1);  // global colour table
    int frames = 0;
    bool alpha = false;
    // Walks the blocks for the frame count and transparency; stops quietly at truncated data.
    auto skipSubBlocks = [&](size_t p) -> size_t {
        while (p < n) {
            const size_t len = d[p];
            p += 1 + len;
            if (len == 0) return p;
        }
        return n + 1;
    };
    while (pos < n) {
        const uint8_t b = d[pos];
        if (b == 0x3B) break;  // trailer
        if (b == 0x21) {       // extension
            if (pos + 2 > n) break;
            const uint8_t label = d[pos + 1];
            if (label == 0xF9 && pos + 4 <= n && d[pos + 2] >= 4 && pos + 7 <= n) {
                if (d[pos + 3] & 1) alpha = true;  // graphic control: transparent colour
            }
            pos = skipSubBlocks(pos + 2);
        } else if (b == 0x2C) {  // image descriptor
            if (pos + 10 > n) break;
            const int fx = le16(d + pos + 1), fy = le16(d + pos + 3);
            const int fw = le16(d + pos + 5), fh = le16(d + pos + 7);
            if (fx > 0 || fy > 0 || fx + fw < w || fy + fh < ht) alpha = true;  // leaves canvas bare
            const uint8_t flags = d[pos + 9];
            pos += 10;
            if (flags & 0x80) pos += size_t(3) << ((flags & 7) + 1);  // local colour table
            pos += 1;                                                 // LZW minimum code size
            if (pos > n) break;
            pos = skipSubBlocks(pos);
            frames++;
        } else {
            break;  // garbage: stop counting
        }
    }
    h->hasAlpha = alpha;
    h->animated = frames > 1;
    return HeaderResult::Ok;
}

HeaderResult readWebp(const uint8_t* d, size_t n, Header* h) {
    if (n < 12 + 8) return HeaderResult::Incomplete;
    const uint8_t* chunk = d + 12;
    const uint8_t* p = chunk + 8;
    const size_t avail = n - 20;
    if (memcmp(chunk, "VP8 ", 4) == 0) {
        if (avail < 10) return HeaderResult::Incomplete;
        if (p[3] != 0x9D || p[4] != 0x01 || p[5] != 0x2A) return HeaderResult::Invalid;
        h->width = le16(p + 6) & 0x3FFF;
        h->height = le16(p + 8) & 0x3FFF;
        h->hasAlpha = false;
    } else if (memcmp(chunk, "VP8L", 4) == 0) {
        if (avail < 5) return HeaderResult::Incomplete;
        if (p[0] != 0x2F) return HeaderResult::Invalid;
        const uint32_t bits = le32(p + 1);
        h->width = int(bits & 0x3FFF) + 1;
        h->height = int((bits >> 14) & 0x3FFF) + 1;
        h->hasAlpha = ((bits >> 28) & 1) != 0;
    } else if (memcmp(chunk, "VP8X", 4) == 0) {
        if (avail < 10) return HeaderResult::Incomplete;
        h->hasAlpha = (p[0] & 0x10) != 0;
        h->animated = (p[0] & 0x02) != 0;
        h->width = int(le24(p + 4)) + 1;
        h->height = int(le24(p + 7)) + 1;
    } else {
        return HeaderResult::Invalid;
    }
    return h->width > 0 && h->height > 0 ? HeaderResult::Ok : HeaderResult::Invalid;
}

// Bilinear or area contributions of source samples to each destination sample along one axis.
struct Contribution {
    int first = 0;
    std::vector<float> weights;
};

std::vector<Contribution> contributions(int srcN, int dstN) {
    std::vector<Contribution> out(static_cast<size_t>(dstN));
    const double scale = double(srcN) / double(dstN);
    for (int o = 0; o < dstN; o++) {
        Contribution& c = out[size_t(o)];
        if (dstN < srcN) {
            const double start = o * scale, end = (o + 1) * scale;
            const int first = int(std::floor(start));
            const int last = std::min(srcN - 1, int(std::ceil(end)) - 1);
            c.first = first;
            for (int s = first; s <= last; s++) {
                const double overlap = std::min(end, double(s + 1)) - std::max(start, double(s));
                c.weights.push_back(float(std::max(0.0, overlap) / scale));
            }
        } else {
            double x = (o + 0.5) * scale - 0.5;
            if (x < 0) x = 0;
            int x0 = int(std::floor(x));
            if (x0 > srcN - 1) x0 = srcN - 1;
            const float f = float(x - x0);
            c.first = x0;
            if (x0 + 1 < srcN && f > 0.f) {
                c.weights.push_back(1.f - f);
                c.weights.push_back(f);
            } else {
                c.weights.push_back(1.f);
            }
        }
    }
    return out;
}

uint8_t clampByte(float v) {
    if (v <= 0.f) return 0;
    if (v >= 255.f) return 255;
    return uint8_t(v + 0.5f);
}

}  // namespace

const char* mimeType(Format f) {
    switch (f) {
        case Format::Png:
            return "image/png";
        case Format::Jpeg:
            return "image/jpeg";
        case Format::Gif:
            return "image/gif";
        case Format::Webp:
            return "image/webp";
        case Format::Bmp:
            return "image/bmp";
        case Format::Heif:
            return "image/heif";
        case Format::Avif:
            return "image/avif";
        case Format::Unknown:
            break;
    }
    return "";
}

Format sniff(const uint8_t* d, size_t n) {
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8 && memcmp(d, kPng, 8) == 0) return Format::Png;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Format::Jpeg;
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) return Format::Gif;
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) return Format::Webp;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return Format::Bmp;
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        const uint8_t* brand = d + 8;
        if (memcmp(brand, "avif", 4) == 0 || memcmp(brand, "avis", 4) == 0) return Format::Avif;
        // mif1 and msf1 are shared by HEIF and AVIF: the compatible brands tell them apart.
        const size_t boxEnd = std::min<size_t>(n, be32(d));
        for (size_t p = 16; p + 4 <= boxEnd; p += 4) {
            if (memcmp(d + p, "avif", 4) == 0 || memcmp(d + p, "avis", 4) == 0) return Format::Avif;
        }
        static const char* const kHeif[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1"};
        for (const char* b : kHeif) {
            if (memcmp(brand, b, 4) == 0) return Format::Heif;
        }
    }
    return Format::Unknown;
}

bool libraryAvailable(Format f) {
    switch (f) {
#if defined(NANOCOMPAT_HAVE_PNG)
        case Format::Png:
            return true;
#endif
#if defined(NANOCOMPAT_HAVE_JPEG)
        case Format::Jpeg:
            return true;
#endif
#if defined(NANOCOMPAT_HAVE_WEBP)
        case Format::Webp:
            return true;
#endif
#if defined(NANOCOMPAT_HAVE_GIF)
        case Format::Gif:
            return true;
#endif
        default:
            return false;
    }
}

HeaderResult readHeader(const uint8_t* data, size_t size, Header* out) {
    Header h;
    h.format = sniff(data, size);
    HeaderResult r;
    switch (h.format) {
        case Format::Png:
            r = readPng(data, size, &h);
            break;
        case Format::Jpeg:
            r = readJpeg(data, size, &h);
            break;
        case Format::Gif:
            r = readGif(data, size, &h);
            break;
        case Format::Webp:
            r = readWebp(data, size, &h);
            break;
        default:
            return HeaderResult::Unsupported;
    }
    if (r == HeaderResult::Ok) *out = h;
    return r;
}

void premultiply(uint8_t* px, int w, int h, size_t stride) {
    for (int y = 0; y < h; y++) {
        uint8_t* p = px + size_t(y) * stride;
        for (int x = 0; x < w; x++, p += 4) {
            const unsigned a = p[3];
            if (a == 255) continue;
            p[0] = uint8_t((p[0] * a + 127) / 255);
            p[1] = uint8_t((p[1] * a + 127) / 255);
            p[2] = uint8_t((p[2] * a + 127) / 255);
        }
    }
}

void resample(const uint8_t* src, int sw, int sh, size_t sstride, uint8_t* dst, int dw, int dh, size_t dstride,
              bool hasAlpha, bool unpremulOut) {
    const std::vector<Contribution> cx = contributions(sw, dw);
    const std::vector<Contribution> cy = contributions(sh, dh);
    // Horizontal pass into premultiplied floats, sh rows of dw pixels.
    std::vector<float> tmp(size_t(sh) * size_t(dw) * 4);
    for (int y = 0; y < sh; y++) {
        const uint8_t* row = src + size_t(y) * sstride;
        float* out = &tmp[size_t(y) * size_t(dw) * 4];
        for (int x = 0; x < dw; x++, out += 4) {
            const Contribution& c = cx[size_t(x)];
            float r = 0, g = 0, b = 0, a = 0;
            for (size_t k = 0; k < c.weights.size(); k++) {
                const uint8_t* p = row + size_t(c.first + int(k)) * 4;
                const float wt = c.weights[k];
                const float pa = hasAlpha ? p[3] / 255.f : 1.f;
                r += wt * p[0] * pa;
                g += wt * p[1] * pa;
                b += wt * p[2] * pa;
                a += wt * p[3];
            }
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
        }
    }
    // Vertical pass, back to bytes.
    for (int y = 0; y < dh; y++) {
        const Contribution& c = cy[size_t(y)];
        uint8_t* out = dst + size_t(y) * dstride;
        for (int x = 0; x < dw; x++, out += 4) {
            float r = 0, g = 0, b = 0, a = 0;
            for (size_t k = 0; k < c.weights.size(); k++) {
                const float* p = &tmp[(size_t(c.first + int(k)) * size_t(dw) + size_t(x)) * 4];
                const float wt = c.weights[k];
                r += wt * p[0];
                g += wt * p[1];
                b += wt * p[2];
                a += wt * p[3];
            }
            const uint8_t ab = hasAlpha ? clampByte(a) : 255;
            if (hasAlpha && unpremulOut) {
                if (ab == 0) {
                    out[0] = out[1] = out[2] = 0;
                } else {
                    const float k = 255.f / a;  // the unrounded alpha: no drift from rounding twice
                    out[0] = clampByte(r * k);
                    out[1] = clampByte(g * k);
                    out[2] = clampByte(b * k);
                }
            } else {
                // Premultiplied already (or opaque, where the two agree), cut to the alpha.
                out[0] = std::min(clampByte(r), ab);
                out[1] = std::min(clampByte(g), ab);
                out[2] = std::min(clampByte(b), ab);
            }
            out[3] = ab;
        }
    }
}

void applyOrientation(const uint8_t* src, int* w, int* h, int orientation, uint8_t* dst) {
    const int W = *w, H = *h;
    const bool swap = orientation >= 5 && orientation <= 8;
    const int dw = swap ? H : W, dh = swap ? W : H;
    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            int sx, sy;
            switch (orientation) {
                case 2: sx = W - 1 - dx; sy = dy; break;
                case 3: sx = W - 1 - dx; sy = H - 1 - dy; break;
                case 4: sx = dx; sy = H - 1 - dy; break;
                case 5: sx = dy; sy = dx; break;
                case 6: sx = dy; sy = H - 1 - dx; break;
                case 7: sx = W - 1 - dy; sy = H - 1 - dx; break;
                case 8: sx = W - 1 - dy; sy = dx; break;
                default: sx = dx; sy = dy; break;
            }
            memcpy(dst + (size_t(dy) * size_t(dw) + size_t(dx)) * 4, src + (size_t(sy) * size_t(W) + size_t(sx)) * 4, 4);
        }
    }
    *w = dw;
    *h = dh;
}

}  // namespace img
}  // namespace nanocompat

using nanocompat::img::Format;
using nanocompat::img::Header;
using nanocompat::img::HeaderResult;

namespace {

// An animated image: frames composited on a canvas of the header's size, straight alpha.
class Animation {
 public:
    virtual ~Animation() = default;
    virtual int frameCount() const = 0;
    virtual int32_t repeatCount() const = 0;
    // The composited canvas at frame index (frames are rendered in order; going back rewinds).
    // nullptr on a decode error. Valid until the next call.
    virtual const uint8_t* frame(int index) = 0;
    virtual void info(int index, AImageDecoderFrameInfo* out) const = 0;
};

#if defined(NANOCOMPAT_HAVE_GIF)
struct GifSource {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int gifRead(GifFileType* gif, GifByteType* buf, int len) {
    GifSource* s = static_cast<GifSource*>(gif->UserData);
    if (len <= 0) return 0;
    const size_t k = std::min(size_t(len), s->size - s->pos);
    memcpy(buf, s->data + s->pos, k);
    s->pos += k;
    return int(k);
}

class GifAnimation : public Animation {
 public:
    ~GifAnimation() override {
        if (mGif) {
            int err = 0;
            DGifCloseFile(mGif, &err);
        }
    }

    bool open(const uint8_t* data, size_t size, int w, int h) {
        mSource = GifSource{data, size, 0};
        int err = 0;
        mGif = DGifOpen(&mSource, gifRead, &err);
        if (!mGif) return false;
        if (DGifSlurp(mGif) != GIF_OK) {
            // A truncated file: keep the frames before the broken one.
            mFrames = mGif->ImageCount - 1;
        } else {
            mFrames = mGif->ImageCount;
        }
        if (mFrames < 1) return false;
        mW = w;
        mH = h;
        mCanvas.assign(size_t(w) * size_t(h) * 4, 0);
        mRepeat = 0;
        for (int i = 0; i < mFrames && mRepeat == 0; i++) {
            const SavedImage& si = mGif->SavedImages[i];
            for (int e = 0; e + 1 < si.ExtensionBlockCount; e++) {
                const ExtensionBlock& b = si.ExtensionBlocks[e];
                const ExtensionBlock& next = si.ExtensionBlocks[e + 1];
                if (b.Function == APPLICATION_EXT_FUNC_CODE && b.ByteCount == 11 &&
                    memcmp(b.Bytes, "NETSCAPE2.0", 11) == 0 && next.ByteCount >= 3 && next.Bytes[0] == 1) {
                    const int loops = next.Bytes[1] | (next.Bytes[2] << 8);
                    mRepeat = loops == 0 ? int32_t(ANDROID_IMAGE_DECODER_INFINITE) : int32_t(loops);
                    break;
                }
            }
        }
        return true;
    }

    int frameCount() const override { return mFrames; }
    int32_t repeatCount() const override { return mFrames > 1 ? mRepeat : 0; }

    const uint8_t* frame(int index) override {
        if (index < 0 || index >= mFrames) return nullptr;
        if (index < mRendered) {
            std::fill(mCanvas.begin(), mCanvas.end(), 0);
            mRendered = -1;
        }
        while (mRendered < index) {
            const int k = mRendered + 1;
            if (k > 0) {
                const GraphicsControlBlock prev = gcb(k - 1);
                if (prev.DisposalMode == DISPOSE_BACKGROUND) {
                    clearRect(k - 1);
                } else if (prev.DisposalMode == DISPOSE_PREVIOUS && !mSaved.empty()) {
                    mCanvas = mSaved;
                }
            }
            if (gcb(k).DisposalMode == DISPOSE_PREVIOUS) mSaved = mCanvas;
            draw(k);
            mRendered = k;
        }
        return mCanvas.data();
    }

    void info(int index, AImageDecoderFrameInfo* out) const override {
        const GraphicsControlBlock g = gcb(index);
        const GifImageDesc& desc = mGif->SavedImages[index].ImageDesc;
        out->durationNs = int64_t(g.DelayTime) * 10 * 1000000;
        out->rect.left = std::max(0, desc.Left);
        out->rect.top = std::max(0, desc.Top);
        out->rect.right = std::min(mW, desc.Left + desc.Width);
        out->rect.bottom = std::min(mH, desc.Top + desc.Height);
        out->hasAlpha = g.TransparentColor != NO_TRANSPARENT_COLOR;
        out->disposeOp = g.DisposalMode == DISPOSE_BACKGROUND ? ANDROID_IMAGE_DECODER_DISPOSE_OP_BACKGROUND
                         : g.DisposalMode == DISPOSE_PREVIOUS ? ANDROID_IMAGE_DECODER_DISPOSE_OP_PREVIOUS
                                                              : ANDROID_IMAGE_DECODER_DISPOSE_OP_NONE;
        out->blendOp = ANDROID_IMAGE_DECODER_BLEND_OP_SRC_OVER;
    }

 private:
    GraphicsControlBlock gcb(int index) const {
        GraphicsControlBlock g;
        g.DisposalMode = DISPOSAL_UNSPECIFIED;
        g.UserInputFlag = false;
        g.DelayTime = 0;
        g.TransparentColor = NO_TRANSPARENT_COLOR;
        DGifSavedExtensionToGCB(mGif, index, &g);
        return g;
    }

    void clearRect(int index) {
        const GifImageDesc& desc = mGif->SavedImages[index].ImageDesc;
        for (int y = std::max(0, desc.Top); y < std::min(mH, desc.Top + desc.Height); y++) {
            for (int x = std::max(0, desc.Left); x < std::min(mW, desc.Left + desc.Width); x++) {
                memset(&mCanvas[(size_t(y) * size_t(mW) + size_t(x)) * 4], 0, 4);
            }
        }
    }

    void draw(int index) {
        const SavedImage& si = mGif->SavedImages[index];
        const GifImageDesc& desc = si.ImageDesc;
        const ColorMapObject* cmap = desc.ColorMap ? desc.ColorMap : mGif->SColorMap;
        if (!cmap || !si.RasterBits) return;
        const int trans = gcb(index).TransparentColor;
        for (int y = 0; y < desc.Height; y++) {
            const int cy = desc.Top + y;
            if (cy < 0 || cy >= mH) continue;
            const GifByteType* row = si.RasterBits + size_t(y) * size_t(desc.Width);
            for (int x = 0; x < desc.Width; x++) {
                const int cx = desc.Left + x;
                if (cx < 0 || cx >= mW) continue;
                const int idx = row[x];
                if (idx == trans || idx >= cmap->ColorCount) continue;
                const GifColorType& c = cmap->Colors[idx];
                uint8_t* p = &mCanvas[(size_t(cy) * size_t(mW) + size_t(cx)) * 4];
                p[0] = c.Red;
                p[1] = c.Green;
                p[2] = c.Blue;
                p[3] = 255;
            }
        }
    }

    GifSource mSource{nullptr, 0, 0};
    GifFileType* mGif = nullptr;
    int mFrames = 0;
    int32_t mRepeat = 0;
    int mW = 0, mH = 0;
    std::vector<uint8_t> mCanvas;
    std::vector<uint8_t> mSaved;
    int mRendered = -1;
};
#endif  // NANOCOMPAT_HAVE_GIF

#if defined(NANOCOMPAT_HAVE_WEBP_DEMUX)
class WebpAnimation : public Animation {
 public:
    ~WebpAnimation() override {
        if (mDec) WebPAnimDecoderDelete(mDec);
    }

    bool open(const uint8_t* data, size_t size) {
        WebPAnimDecoderOptions opts;
        if (!WebPAnimDecoderOptionsInit(&opts)) return false;
        opts.color_mode = MODE_RGBA;
        opts.use_threads = 0;
        WebPData wd;
        wd.bytes = data;
        wd.size = size;
        mDec = WebPAnimDecoderNew(&wd, &opts);
        if (!mDec) return false;
        if (!WebPAnimDecoderGetInfo(mDec, &mInfo)) return false;
        return mInfo.frame_count > 0;
    }

    int frameCount() const override { return int(mInfo.frame_count); }
    int32_t repeatCount() const override {
        if (mInfo.frame_count <= 1) return 0;
        return mInfo.loop_count == 0 ? int32_t(ANDROID_IMAGE_DECODER_INFINITE) : int32_t(mInfo.loop_count) - 1;
    }

    const uint8_t* frame(int index) override {
        if (index < 0 || index >= frameCount()) return nullptr;
        if (index < mRendered) {
            WebPAnimDecoderReset(mDec);
            mRendered = -1;
            mCurrent = nullptr;
        }
        while (mRendered < index) {
            uint8_t* buf = nullptr;
            int timestamp = 0;
            if (!WebPAnimDecoderGetNext(mDec, &buf, &timestamp)) return nullptr;
            mCurrent = buf;
            mRendered++;
        }
        return mCurrent;
    }

    void info(int index, AImageDecoderFrameInfo* out) const override {
        const WebPDemuxer* demux = WebPAnimDecoderGetDemuxer(mDec);
        WebPIterator it;
        if (!demux || !WebPDemuxGetFrame(demux, index + 1, &it)) return;
        out->durationNs = int64_t(it.duration) * 1000000;
        out->rect.left = it.x_offset;
        out->rect.top = it.y_offset;
        out->rect.right = it.x_offset + it.width;
        out->rect.bottom = it.y_offset + it.height;
        out->hasAlpha = it.has_alpha != 0;
        out->disposeOp = it.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND ? ANDROID_IMAGE_DECODER_DISPOSE_OP_BACKGROUND
                                                                          : ANDROID_IMAGE_DECODER_DISPOSE_OP_NONE;
        out->blendOp = it.blend_method == WEBP_MUX_NO_BLEND ? ANDROID_IMAGE_DECODER_BLEND_OP_SRC
                                                            : ANDROID_IMAGE_DECODER_BLEND_OP_SRC_OVER;
        WebPDemuxReleaseIterator(&it);
    }

 private:
    WebPAnimDecoder* mDec = nullptr;
    WebPAnimInfo mInfo{};
    const uint8_t* mCurrent = nullptr;
    int mRendered = -1;
};
#endif  // NANOCOMPAT_HAVE_WEBP_DEMUX

#if defined(NANOCOMPAT_HAVE_PNG)
int decodePng(const uint8_t* data, size_t size, std::vector<uint8_t>* out, int* w, int* h) {
    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, size)) return ANDROID_IMAGE_DECODER_INVALID_INPUT;
    image.format = PNG_FORMAT_RGBA;
    if (int64_t(image.width) * int64_t(image.height) > kMaxPixels) {
        png_image_free(&image);
        return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;
    }
    out->resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, out->data(), 0, nullptr)) {
        png_image_free(&image);
        return ANDROID_IMAGE_DECODER_INCOMPLETE;
    }
    *w = int(image.width);
    *h = int(image.height);
    return ANDROID_IMAGE_DECODER_SUCCESS;
}
#endif

#if defined(NANOCOMPAT_HAVE_JPEG)
struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void jpegErrorExit(j_common_ptr cinfo) {
    longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
}

void jpegQuiet(j_common_ptr) {}

// Decodes to tight RGBA in stored orientation, sampled down by 1/2, 1/4 or 1/8 while the result
// stays at least wantW x wantH (in stored orientation). Only plain locals here: setjmp.
int decodeJpegStored(const uint8_t* data, size_t size, int wantW, int wantH, std::vector<uint8_t>* out, int* w,
                     int* h) {
    jpeg_decompress_struct cinfo;
    JpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;
    jerr.pub.output_message = jpegQuiet;
    jpeg_create_decompress(&cinfo);
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return ANDROID_IMAGE_DECODER_INCOMPLETE;
    }
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return ANDROID_IMAGE_DECODER_INVALID_INPUT;
    }
    const bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    unsigned denom = 1;
    for (unsigned d = 8; d >= 2; d /= 2) {
        const unsigned sw = (cinfo.image_width + d - 1) / d, sh = (cinfo.image_height + d - 1) / d;
        if (int(sw) >= wantW && int(sh) >= wantH) {
            denom = d;
            break;
        }
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    jpeg_start_decompress(&cinfo);
    const unsigned ow = cinfo.output_width, oh = cinfo.output_height;
    const int comps = cinfo.output_components;
    if (int64_t(ow) * int64_t(oh) > kMaxPixels || (comps != 3 && comps != 4)) {
        jpeg_destroy_decompress(&cinfo);
        return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;
    }
    out->resize(size_t(ow) * size_t(oh) * 4);
    JSAMPARRAY row = (*cinfo.mem->alloc_sarray)(reinterpret_cast<j_common_ptr>(&cinfo), JPOOL_IMAGE,
                                                ow * unsigned(comps), 1);
    while (cinfo.output_scanline < oh) {
        const unsigned y = cinfo.output_scanline;
        if (jpeg_read_scanlines(&cinfo, row, 1) != 1) break;
        uint8_t* dst = out->data() + size_t(y) * size_t(ow) * 4;
        const uint8_t* src = row[0];
        for (unsigned x = 0; x < ow; x++, dst += 4, src += comps) {
            if (comps == 4) {
                // Adobe CMYK is stored inverted: R = C * K / 255 and so on.
                const unsigned k = src[3];
                dst[0] = uint8_t((src[0] * k + 127) / 255);
                dst[1] = uint8_t((src[1] * k + 127) / 255);
                dst[2] = uint8_t((src[2] * k + 127) / 255);
            } else {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
            dst[3] = 255;
        }
    }
    const bool complete = cinfo.output_scanline >= oh;
    if (complete) jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    if (!complete) return ANDROID_IMAGE_DECODER_INCOMPLETE;
    *w = int(ow);
    *h = int(oh);
    return ANDROID_IMAGE_DECODER_SUCCESS;
}
#endif

}  // namespace

struct AImageDecoder {
    std::vector<uint8_t> owned;  // the data, for createFromFd
    const uint8_t* data = nullptr;
    size_t size = 0;
    Header header;
    AImageDecoderHeaderInfo info;
    int32_t targetW = 0, targetH = 0;
    bool hasCrop = false;
    ARect crop = {0, 0, 0, 0};
    bool unpremul = false;
    std::unique_ptr<Animation> anim;  // animated images only
    int frameIndex = 0;
    bool finished = false;

    int outW() const { return hasCrop ? crop.right - crop.left : targetW; }
    int outH() const { return hasCrop ? crop.bottom - crop.top : targetH; }
};

namespace {

// Decodes a still image to tight straight-alpha RGBA, at its full size or a size the library could
// reach cheaply on the way to the target (JPEG sampling, WebP scaling). *w and *h are what it made.
int decodeStill(const AImageDecoder* d, std::vector<uint8_t>* out, int* w, int* h) {
    (void)out;  // unused when this build has no image library
    (void)w;
    (void)h;
    switch (d->header.format) {
#if defined(NANOCOMPAT_HAVE_PNG)
        case Format::Png:
            return decodePng(d->data, d->size, out, w, h);
#endif
#if defined(NANOCOMPAT_HAVE_JPEG)
        case Format::Jpeg: {
            const int o = d->header.orientation;
            const bool swap = o >= 5;
            int sw = 0, sh = 0;
            const int rc = decodeJpegStored(d->data, d->size, swap ? d->targetH : d->targetW,
                                            swap ? d->targetW : d->targetH, out, &sw, &sh);
            if (rc != ANDROID_IMAGE_DECODER_SUCCESS || o == 1) {
                *w = sw;
                *h = sh;
                return rc;
            }
            std::vector<uint8_t> turned(out->size());
            nanocompat::img::applyOrientation(out->data(), &sw, &sh, o, turned.data());
            out->swap(turned);
            *w = sw;
            *h = sh;
            return rc;
        }
#endif
#if defined(NANOCOMPAT_HAVE_WEBP)
        case Format::Webp: {
            WebPDecoderConfig config;
            if (!WebPInitDecoderConfig(&config)) return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;
            if (WebPGetFeatures(d->data, d->size, &config.input) != VP8_STATUS_OK) {
                return ANDROID_IMAGE_DECODER_INVALID_INPUT;
            }
            int ow = config.input.width, oh = config.input.height;
            if (d->targetW != ow || d->targetH != oh) {
                config.options.use_scaling = 1;
                config.options.scaled_width = d->targetW;
                config.options.scaled_height = d->targetH;
                ow = d->targetW;
                oh = d->targetH;
            }
            if (int64_t(ow) * int64_t(oh) > kMaxPixels) return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;
            out->resize(size_t(ow) * size_t(oh) * 4);
            config.output.colorspace = MODE_RGBA;
            config.output.is_external_memory = 1;
            config.output.u.RGBA.rgba = out->data();
            config.output.u.RGBA.stride = ow * 4;
            config.output.u.RGBA.size = out->size();
            const VP8StatusCode st = WebPDecode(d->data, d->size, &config);
            WebPFreeDecBuffer(&config.output);
            if (st != VP8_STATUS_OK) {
                return st == VP8_STATUS_NOT_ENOUGH_DATA ? ANDROID_IMAGE_DECODER_INCOMPLETE
                                                        : ANDROID_IMAGE_DECODER_ERROR;
            }
            *w = ow;
            *h = oh;
            return ANDROID_IMAGE_DECODER_SUCCESS;
        }
#endif
#if defined(NANOCOMPAT_HAVE_GIF)
        case Format::Gif: {
            GifAnimation g;
            if (!g.open(d->data, d->size, d->header.width, d->header.height)) {
                return ANDROID_IMAGE_DECODER_INVALID_INPUT;
            }
            const uint8_t* canvas = g.frame(0);
            if (!canvas) return ANDROID_IMAGE_DECODER_ERROR;
            out->assign(canvas, canvas + size_t(d->header.width) * size_t(d->header.height) * 4);
            *w = d->header.width;
            *h = d->header.height;
            return ANDROID_IMAGE_DECODER_SUCCESS;
        }
#endif
        default:
            return ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT;
    }
}

std::unique_ptr<Animation> openAnimation(const AImageDecoder* d) {
    switch (d->header.format) {
#if defined(NANOCOMPAT_HAVE_GIF)
        case Format::Gif: {
            auto g = std::make_unique<GifAnimation>();
            if (!g->open(d->data, d->size, d->header.width, d->header.height)) return nullptr;
            return g;
        }
#endif
#if defined(NANOCOMPAT_HAVE_WEBP_DEMUX)
        case Format::Webp: {
            auto a = std::make_unique<WebpAnimation>();
            if (!a->open(d->data, d->size)) return nullptr;
            return a;
        }
#endif
        default:
            return nullptr;
    }
}

// Copies the cropped region of tight RGBA src (sw wide) into the caller's buffer, premultiplying
// when asked.
void copyOut(const AImageDecoder* d, const uint8_t* src, int sw, uint8_t* dst, size_t stride, bool premul) {
    const int x0 = d->hasCrop ? d->crop.left : 0;
    const int y0 = d->hasCrop ? d->crop.top : 0;
    const int w = d->outW(), h = d->outH();
    for (int y = 0; y < h; y++) {
        uint8_t* row = dst + size_t(y) * stride;
        memcpy(row, src + ((size_t(y0 + y) * size_t(sw)) + size_t(x0)) * 4, size_t(w) * 4);
        if (premul) nanocompat::img::premultiply(row, w, 1, stride);
    }
}

}  // namespace

extern "C" const char* AImageDecoder_resultToString(int result) {
    switch (result) {
        case ANDROID_IMAGE_DECODER_SUCCESS: return "ANDROID_IMAGE_DECODER_SUCCESS";
        case ANDROID_IMAGE_DECODER_INCOMPLETE: return "ANDROID_IMAGE_DECODER_INCOMPLETE";
        case ANDROID_IMAGE_DECODER_ERROR: return "ANDROID_IMAGE_DECODER_ERROR";
        case ANDROID_IMAGE_DECODER_INVALID_CONVERSION: return "ANDROID_IMAGE_DECODER_INVALID_CONVERSION";
        case ANDROID_IMAGE_DECODER_INVALID_SCALE: return "ANDROID_IMAGE_DECODER_INVALID_SCALE";
        case ANDROID_IMAGE_DECODER_BAD_PARAMETER: return "ANDROID_IMAGE_DECODER_BAD_PARAMETER";
        case ANDROID_IMAGE_DECODER_INVALID_INPUT: return "ANDROID_IMAGE_DECODER_INVALID_INPUT";
        case ANDROID_IMAGE_DECODER_SEEK_ERROR: return "ANDROID_IMAGE_DECODER_SEEK_ERROR";
        case ANDROID_IMAGE_DECODER_INTERNAL_ERROR: return "ANDROID_IMAGE_DECODER_INTERNAL_ERROR";
        case ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT: return "ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT";
        case ANDROID_IMAGE_DECODER_FINISHED: return "ANDROID_IMAGE_DECODER_FINISHED";
        case ANDROID_IMAGE_DECODER_INVALID_STATE: return "ANDROID_IMAGE_DECODER_INVALID_STATE";
        default: return nullptr;
    }
}

extern "C" int AImageDecoder_createFromBuffer(const void* buffer, size_t length, AImageDecoder** outDecoder) {
    if (outDecoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    *outDecoder = nullptr;
    if (buffer == nullptr || length == 0) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    const Format f = nanocompat::img::sniff(data, length);
    if (f == Format::Unknown || !nanocompat::img::libraryAvailable(f)) {
        return ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT;
    }
    Header h;
    switch (nanocompat::img::readHeader(data, length, &h)) {
        case HeaderResult::Ok:
            break;
        case HeaderResult::Incomplete:
            return ANDROID_IMAGE_DECODER_INCOMPLETE;
        case HeaderResult::Invalid:
            return ANDROID_IMAGE_DECODER_INVALID_INPUT;
        case HeaderResult::Unsupported:
            return ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT;
    }
    if (int64_t(h.width) * int64_t(h.height) > kMaxPixels) return ANDROID_IMAGE_DECODER_INVALID_INPUT;
    std::unique_ptr<AImageDecoder> d(new AImageDecoder());
    d->data = data;
    d->size = length;
    d->header = h;
    d->info.width = h.width;
    d->info.height = h.height;
    d->info.mimeType = nanocompat::img::mimeType(f);
    d->info.alphaFlags = h.hasAlpha ? ANDROID_BITMAP_FLAGS_ALPHA_PREMUL : ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
    d->targetW = h.width;
    d->targetH = h.height;
    if (h.animated) {
        d->anim = openAnimation(d.get());
        if (!d->anim) {
            // No animation support for it here (WebP without libwebpdemux): its first frame only.
            if (f == Format::Webp) {
                return ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT;
            }
            return ANDROID_IMAGE_DECODER_INVALID_INPUT;
        }
        // A one-frame animation still decodes through the animation path, but is not animated.
        d->header.animated = d->anim->frameCount() > 1;
    }
    *outDecoder = d.release();
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int AImageDecoder_createFromFd(int fd, AImageDecoder** outDecoder) {
    if (outDecoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    *outDecoder = nullptr;
    if (fd < 0) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    std::vector<uint8_t> bytes;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        const off_t here = lseek(fd, 0, SEEK_CUR);
        if (here >= 0 && here < st.st_size) bytes.reserve(size_t(st.st_size - here));
    }
    uint8_t buf[65536];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ANDROID_IMAGE_DECODER_SEEK_ERROR;
        }
        if (n == 0) break;
        if (bytes.size() + size_t(n) > (size_t(1) << 30)) return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    if (bytes.empty()) return ANDROID_IMAGE_DECODER_INCOMPLETE;
    // The decoder keeps the bytes: move them in once it exists, so its pointers stay valid.
    AImageDecoder* d = nullptr;
    const int rc = AImageDecoder_createFromBuffer(bytes.data(), bytes.size(), &d);
    if (rc != ANDROID_IMAGE_DECODER_SUCCESS) return rc;
    d->owned.swap(bytes);  // the vector's heap buffer moves with it; d->data stays valid
    *outDecoder = d;
    return rc;
}

extern "C" void AImageDecoder_delete(AImageDecoder* decoder) { delete decoder; }

extern "C" int AImageDecoder_setAndroidBitmapFormat(AImageDecoder* decoder, int32_t format) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    return format == ANDROID_BITMAP_FORMAT_RGBA_8888 ? ANDROID_IMAGE_DECODER_SUCCESS
                                                     : ANDROID_IMAGE_DECODER_INVALID_CONVERSION;
}

extern "C" int AImageDecoder_setUnpremultipliedRequired(AImageDecoder* decoder, bool unpremultipliedRequired) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    decoder->unpremul = unpremultipliedRequired;
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int AImageDecoder_setDataSpace(AImageDecoder* decoder, int32_t dataspace) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    return dataspace == 0 || dataspace == kDataSpaceSrgb ? ANDROID_IMAGE_DECODER_SUCCESS
                                                         : ANDROID_IMAGE_DECODER_INVALID_CONVERSION;
}

extern "C" int AImageDecoder_setTargetSize(AImageDecoder* decoder, int32_t width, int32_t height) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (width <= 0 || height <= 0) return ANDROID_IMAGE_DECODER_INVALID_SCALE;
    if (int64_t(width) * int64_t(height) > kMaxPixels) return ANDROID_IMAGE_DECODER_INVALID_SCALE;
    if (decoder->anim && (width != decoder->header.width || height != decoder->header.height)) {
        return ANDROID_IMAGE_DECODER_INVALID_SCALE;
    }
    if (decoder->hasCrop && (decoder->crop.right > width || decoder->crop.bottom > height)) {
        return ANDROID_IMAGE_DECODER_INVALID_SCALE;
    }
    decoder->targetW = width;
    decoder->targetH = height;
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int AImageDecoder_computeSampledSize(const AImageDecoder* decoder, int sampleSize, int32_t* width,
                                                int32_t* height) {
    if (decoder == nullptr || width == nullptr || height == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (sampleSize < 1) return ANDROID_IMAGE_DECODER_INVALID_SCALE;
    *width = std::max(1, decoder->header.width / sampleSize);
    *height = std::max(1, decoder->header.height / sampleSize);
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int AImageDecoder_setCrop(AImageDecoder* decoder, ARect crop) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (crop.left == 0 && crop.top == 0 && crop.right == 0 && crop.bottom == 0) {
        decoder->hasCrop = false;
        decoder->crop = crop;
        return ANDROID_IMAGE_DECODER_SUCCESS;
    }
    if (crop.left < 0 || crop.top < 0 || crop.right <= crop.left || crop.bottom <= crop.top ||
        crop.right > decoder->targetW || crop.bottom > decoder->targetH) {
        return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    }
    decoder->hasCrop = true;
    decoder->crop = crop;
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" const AImageDecoderHeaderInfo* AImageDecoder_getHeaderInfo(const AImageDecoder* decoder) {
    return decoder ? &decoder->info : nullptr;
}

extern "C" int32_t AImageDecoderHeaderInfo_getWidth(const AImageDecoderHeaderInfo* info) {
    return info ? info->width : 0;
}

extern "C" int32_t AImageDecoderHeaderInfo_getHeight(const AImageDecoderHeaderInfo* info) {
    return info ? info->height : 0;
}

extern "C" const char* AImageDecoderHeaderInfo_getMimeType(const AImageDecoderHeaderInfo* info) {
    return info ? info->mimeType : nullptr;
}

extern "C" int32_t AImageDecoderHeaderInfo_getAndroidBitmapFormat(const AImageDecoderHeaderInfo* info) {
    return info ? ANDROID_BITMAP_FORMAT_RGBA_8888 : ANDROID_BITMAP_FORMAT_NONE;
}

extern "C" int AImageDecoderHeaderInfo_getAlphaFlags(const AImageDecoderHeaderInfo* info) {
    return info ? info->alphaFlags : int(ANDROID_IMAGE_DECODER_BAD_PARAMETER);
}

extern "C" int32_t AImageDecoderHeaderInfo_getDataSpace(const AImageDecoderHeaderInfo* info) {
    return info ? kDataSpaceSrgb : 0;
}

extern "C" size_t AImageDecoder_getMinimumStride(AImageDecoder* decoder) {
    return decoder ? size_t(decoder->outW()) * 4 : 0;
}

extern "C" int AImageDecoder_decodeImage(AImageDecoder* decoder, void* pixels, size_t stride, size_t size) {
    if (decoder == nullptr || pixels == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    const size_t minStride = size_t(decoder->outW()) * 4;
    const int outH = decoder->outH();
    if (stride < minStride || stride % 4 != 0) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (size < stride * size_t(outH - 1) + minStride) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    uint8_t* dst = static_cast<uint8_t*>(pixels);
    const bool premulOut = decoder->header.hasAlpha && !decoder->unpremul;

    if (decoder->anim) {
        if (decoder->finished) return ANDROID_IMAGE_DECODER_FINISHED;
        const uint8_t* canvas = decoder->anim->frame(decoder->frameIndex);
        if (canvas == nullptr) return ANDROID_IMAGE_DECODER_ERROR;
        copyOut(decoder, canvas, decoder->header.width, dst, stride, premulOut);
        return ANDROID_IMAGE_DECODER_SUCCESS;
    }

    std::vector<uint8_t> px;
    int w = 0, h = 0;
    const int rc = decodeStill(decoder, &px, &w, &h);
    if (rc != ANDROID_IMAGE_DECODER_SUCCESS) return rc;
    if (w == decoder->targetW && h == decoder->targetH) {
        copyOut(decoder, px.data(), w, dst, stride, premulOut);
        return ANDROID_IMAGE_DECODER_SUCCESS;
    }
    if (!decoder->hasCrop) {
        nanocompat::img::resample(px.data(), w, h, size_t(w) * 4, dst, decoder->targetW, decoder->targetH, stride,
                                  decoder->header.hasAlpha, decoder->unpremul);
        return ANDROID_IMAGE_DECODER_SUCCESS;
    }
    std::vector<uint8_t> scaled(size_t(decoder->targetW) * size_t(decoder->targetH) * 4);
    nanocompat::img::resample(px.data(), w, h, size_t(w) * 4, scaled.data(), decoder->targetW, decoder->targetH,
                              size_t(decoder->targetW) * 4, decoder->header.hasAlpha, decoder->unpremul);
    copyOut(decoder, scaled.data(), decoder->targetW, dst, stride, false);
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" bool AImageDecoder_isAnimated(AImageDecoder* decoder) {
    return decoder != nullptr && decoder->header.animated;
}

extern "C" int32_t AImageDecoder_getRepeatCount(AImageDecoder* decoder) {
    if (decoder == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    return decoder->anim ? decoder->anim->repeatCount() : 0;
}

extern "C" int AImageDecoder_advanceFrame(AImageDecoder* decoder) {
    if (decoder == nullptr || !decoder->anim) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (decoder->finished) return ANDROID_IMAGE_DECODER_FINISHED;
    // Render the current frame first if it was skipped: later frames are composited on it.
    if (decoder->anim->frame(decoder->frameIndex) == nullptr) return ANDROID_IMAGE_DECODER_ERROR;
    if (decoder->frameIndex + 1 >= decoder->anim->frameCount()) {
        decoder->finished = true;
        return ANDROID_IMAGE_DECODER_FINISHED;
    }
    decoder->frameIndex++;
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int AImageDecoder_rewind(AImageDecoder* decoder) {
    if (decoder == nullptr || !decoder->anim) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    decoder->frameIndex = 0;
    decoder->finished = false;
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" AImageDecoderFrameInfo* AImageDecoderFrameInfo_create(void) { return new AImageDecoderFrameInfo(); }

extern "C" void AImageDecoderFrameInfo_delete(AImageDecoderFrameInfo* info) { delete info; }

extern "C" int AImageDecoder_getFrameInfo(AImageDecoder* decoder, AImageDecoderFrameInfo* info) {
    if (decoder == nullptr || info == nullptr) return ANDROID_IMAGE_DECODER_BAD_PARAMETER;
    if (decoder->finished) return ANDROID_IMAGE_DECODER_FINISHED;
    *info = AImageDecoderFrameInfo();
    if (decoder->anim) {
        decoder->anim->info(decoder->frameIndex, info);
    } else {
        info->rect = {0, 0, decoder->header.width, decoder->header.height};
        info->hasAlpha = decoder->header.hasAlpha;
    }
    return ANDROID_IMAGE_DECODER_SUCCESS;
}

extern "C" int64_t AImageDecoderFrameInfo_getDuration(const AImageDecoderFrameInfo* info) {
    return info ? info->durationNs : int64_t(ANDROID_IMAGE_DECODER_BAD_PARAMETER);
}

extern "C" ARect AImageDecoderFrameInfo_getFrameRect(const AImageDecoderFrameInfo* info) {
    if (!info) return ARect{0, 0, 0, 0};
    return info->rect;
}

extern "C" bool AImageDecoderFrameInfo_hasAlphaWithinBounds(const AImageDecoderFrameInfo* info) {
    return info ? info->hasAlpha : false;
}

extern "C" int32_t AImageDecoderFrameInfo_getDisposeOp(const AImageDecoderFrameInfo* info) {
    return info ? info->disposeOp : int32_t(ANDROID_IMAGE_DECODER_BAD_PARAMETER);
}

extern "C" int32_t AImageDecoderFrameInfo_getBlendOp(const AImageDecoderFrameInfo* info) {
    return info ? info->blendOp : int32_t(ANDROID_IMAGE_DECODER_BAD_PARAMETER);
}

extern "C" void AImageDecoder_setInternallyHandleDisposePrevious(AImageDecoder* /*decoder*/, bool /*handle*/) {}
