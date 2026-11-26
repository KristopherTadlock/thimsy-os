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

// android/imagedecoder.h: the library-independent parts always (format sniffing, header readers,
// resampling, EXIF orientation, argument checks, unsupported formats), and real decoding of small
// PNG and GIF fixtures when this build found libpng and giflib (skipped otherwise). JPEG and WebP
// decoding have no fixture yet (there is no encoder on this machine): TODO(G10).

#include <android/bitmap.h>
#include <android/imagedecoder.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "compat_test_util.h"
#include "imagedecoder_internal.h"

using nanocompat::img::Format;
using nanocompat::img::Header;
using nanocompat::img::HeaderResult;

namespace {

// 2x2 RGBA PNG: (255,0,0,255) (0,255,0,128) / (0,0,255,0) (255,255,255,255).
const uint8_t kPng2x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x08, 0x1b, 0x18, 0xc0, 0x34, 0x10, 0x00, 0x00, 0x3f, 0xd7, 0x08, 0x79, 0x8f, 0x13, 0x8a,
    0x8a, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

// 2x2 GIF, two frames, looping forever: frame 1 red, green / blue, black for 100 ms; frame 2 paints
// (1,1) green for 200 ms (transparent index 3, 1x1 at 1,1).
const uint8_t kGif2Frames[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x04,
    0x04, 0x43, 0x71, 0x05, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x14, 0x00, 0x03, 0x00, 0x2c, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x4c, 0x01, 0x00, 0x3b,
};

std::vector<uint8_t> bytes(std::initializer_list<int> list) {
    std::vector<uint8_t> v;
    for (int b : list) v.push_back(uint8_t(b));
    return v;
}

void append(std::vector<uint8_t>* v, const char* s) { v->insert(v->end(), s, s + strlen(s)); }

// A PNG signature and IHDR (and optionally a tRNS chunk); CRCs are not checked by the reader.
std::vector<uint8_t> pngHeader(int w, int h, int colourType, bool trns) {
    std::vector<uint8_t> v = bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13});
    append(&v, "IHDR");
    for (int x : {w, h}) {
        v.push_back(uint8_t(x >> 24));
        v.push_back(uint8_t(x >> 16));
        v.push_back(uint8_t(x >> 8));
        v.push_back(uint8_t(x));
    }
    for (int b : {8, colourType, 0, 0, 0, 0, 0, 0, 0}) v.push_back(uint8_t(b));  // + CRC
    if (trns) {
        for (int b : {0, 0, 0, 6}) v.push_back(uint8_t(b));
        append(&v, "tRNS");
        for (int i = 0; i < 6 + 4; i++) v.push_back(0);
    }
    for (int b : {0, 0, 0, 0}) v.push_back(uint8_t(b));
    append(&v, "IDAT");
    return v;
}

// SOI, an APP1 EXIF block with the orientation, then SOF0 of w x h.
std::vector<uint8_t> jpegHeader(int w, int h, int orientation) {
    std::vector<uint8_t> v = bytes({0xFF, 0xD8});
    if (orientation > 0) {
        // "Exif\0\0" + "MM" 42 offset 8 + one IFD entry (0x0112, SHORT, 1, value) + next IFD 0.
        std::vector<uint8_t> exif = bytes({'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1,
                                           0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, uint8_t(orientation), 0, 0,
                                           0, 0, 0, 0});
        const size_t len = exif.size() + 2;
        for (int b : {0xFF, 0xE1, int(len >> 8), int(len & 0xFF)}) v.push_back(uint8_t(b));
        v.insert(v.end(), exif.begin(), exif.end());
    }
    for (int b : {0xFF, 0xC0, 0, 11, 8, h >> 8, h & 0xFF, w >> 8, w & 0xFF, 1, 1, 0x11, 0}) v.push_back(uint8_t(b));
    return v;
}

std::vector<uint8_t> webpHeader(const char* fourcc, std::vector<uint8_t> payload) {
    std::vector<uint8_t> v;
    append(&v, "RIFF");
    for (int b : {0, 0, 0, 0}) v.push_back(uint8_t(b));
    append(&v, "WEBP");
    append(&v, fourcc);
    for (int b : {int(payload.size()), 0, 0, 0}) v.push_back(uint8_t(b));
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

HeaderResult header(const std::vector<uint8_t>& v, Header* h) {
    return nanocompat::img::readHeader(v.data(), v.size(), h);
}

}  // namespace

TEST(ImageSniff, Formats) {
    EXPECT_TRUE(nanocompat::img::sniff(kPng2x2, sizeof kPng2x2) == Format::Png);
    EXPECT_TRUE(nanocompat::img::sniff(kGif2Frames, sizeof kGif2Frames) == Format::Gif);
    const auto jpeg = jpegHeader(4, 4, 0);
    EXPECT_TRUE(nanocompat::img::sniff(jpeg.data(), jpeg.size()) == Format::Jpeg);
    const auto webp = webpHeader("VP8L", bytes({0x2F, 0, 0, 0, 0}));
    EXPECT_TRUE(nanocompat::img::sniff(webp.data(), webp.size()) == Format::Webp);
    const auto bmp = bytes({'B', 'M', 0, 0});
    EXPECT_TRUE(nanocompat::img::sniff(bmp.data(), bmp.size()) == Format::Bmp);
    std::vector<uint8_t> heic = bytes({0, 0, 0, 24});
    append(&heic, "ftypheic");
    heic.resize(24, 0);
    EXPECT_TRUE(nanocompat::img::sniff(heic.data(), heic.size()) == Format::Heif);
    std::vector<uint8_t> avif = bytes({0, 0, 0, 24});
    append(&avif, "ftypmif1");
    for (int b : {0, 0, 0, 0}) avif.push_back(uint8_t(b));
    append(&avif, "avif");
    avif.resize(24, 0);
    EXPECT_TRUE(nanocompat::img::sniff(avif.data(), avif.size()) == Format::Avif);
    const auto junk = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    EXPECT_TRUE(nanocompat::img::sniff(junk.data(), junk.size()) == Format::Unknown);
    EXPECT_STREQ(nanocompat::img::mimeType(Format::Webp), "image/webp");
}

TEST(ImageHeader, Png) {
    Header h;
    ASSERT_TRUE(nanocompat::img::readHeader(kPng2x2, sizeof kPng2x2, &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 2);
    EXPECT_EQ(h.height, 2);
    EXPECT_TRUE(h.hasAlpha);
    EXPECT_FALSE(h.animated);
    ASSERT_TRUE(header(pngHeader(640, 480, 2, false), &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 640);
    EXPECT_FALSE(h.hasAlpha);  // RGB
    ASSERT_TRUE(header(pngHeader(640, 480, 2, true), &h) == HeaderResult::Ok);
    EXPECT_TRUE(h.hasAlpha);  // RGB with tRNS
    ASSERT_TRUE(header(pngHeader(16, 16, 4, false), &h) == HeaderResult::Ok);
    EXPECT_TRUE(h.hasAlpha);  // grey + alpha
    EXPECT_TRUE(nanocompat::img::readHeader(kPng2x2, 20, &h) == HeaderResult::Incomplete);
    EXPECT_TRUE(header(pngHeader(0, 5, 6, false), &h) == HeaderResult::Invalid);
}

TEST(ImageHeader, JpegWithOrientation) {
    Header h;
    ASSERT_TRUE(header(jpegHeader(300, 200, 0), &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 300);
    EXPECT_EQ(h.height, 200);
    EXPECT_EQ(h.orientation, 1);
    EXPECT_FALSE(h.hasAlpha);
    ASSERT_TRUE(header(jpegHeader(300, 200, 3), &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 300);
    EXPECT_EQ(h.orientation, 3);
    ASSERT_TRUE(header(jpegHeader(300, 200, 6), &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 200);  // shown rotated: the sides swap, as AImageDecoder reports them
    EXPECT_EQ(h.height, 300);
    EXPECT_EQ(h.orientation, 6);
    auto cut = jpegHeader(300, 200, 6);
    cut.resize(cut.size() - 8);
    EXPECT_TRUE(header(cut, &h) == HeaderResult::Incomplete);
}

TEST(ImageHeader, Gif) {
    Header h;
    ASSERT_TRUE(nanocompat::img::readHeader(kGif2Frames, sizeof kGif2Frames, &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 2);
    EXPECT_EQ(h.height, 2);
    EXPECT_TRUE(h.animated);
    EXPECT_TRUE(h.hasAlpha);  // a transparent index, and a frame that leaves the canvas bare
    EXPECT_TRUE(nanocompat::img::readHeader(kGif2Frames, 10, &h) == HeaderResult::Incomplete);
}

TEST(ImageHeader, Webp) {
    Header h;
    // Lossy: frame tag, start code, 14-bit sizes.
    ASSERT_TRUE(header(webpHeader("VP8 ", bytes({0, 0, 0, 0x9D, 0x01, 0x2A, 0x40, 0x01, 0xF0, 0x00})), &h) ==
                HeaderResult::Ok);
    EXPECT_EQ(h.width, 320);
    EXPECT_EQ(h.height, 240);
    EXPECT_FALSE(h.hasAlpha);
    // Lossless: 0x2F, then 14 bits width - 1, 14 bits height - 1, the alpha bit.
    const uint32_t bits = (100 - 1) | ((50 - 1) << 14) | (1u << 28);
    ASSERT_TRUE(header(webpHeader("VP8L", bytes({0x2F, int(bits & 0xFF), int((bits >> 8) & 0xFF),
                                                 int((bits >> 16) & 0xFF), int(bits >> 24)})),
                       &h) == HeaderResult::Ok);
    EXPECT_EQ(h.width, 100);
    EXPECT_EQ(h.height, 50);
    EXPECT_TRUE(h.hasAlpha);
    // Extended: flags (alpha, animation), 24-bit canvas sizes minus one.
    ASSERT_TRUE(header(webpHeader("VP8X", bytes({0x12, 0, 0, 0, 0x1F, 0x03, 0, 0x0F, 0, 0})), &h) ==
                HeaderResult::Ok);
    EXPECT_EQ(h.width, 800);
    EXPECT_EQ(h.height, 16);
    EXPECT_TRUE(h.hasAlpha);
    EXPECT_TRUE(h.animated);
    EXPECT_TRUE(header(webpHeader("VP8Z", bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0})), &h) == HeaderResult::Invalid);
}

TEST(ImageResample, ShrinksByAveraging) {
    // 4x2 opaque: left half black, right half white -> 2x1: black, white.
    std::vector<uint8_t> src(4 * 2 * 4, 0);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            uint8_t* p = &src[(y * 4 + x) * 4];
            const uint8_t v = x >= 2 ? 255 : 0;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    uint8_t dst[2 * 4];
    nanocompat::img::resample(src.data(), 4, 2, 16, dst, 2, 1, 8, false, true);
    EXPECT_EQ(dst[0], 0);
    EXPECT_EQ(dst[3], 255);
    EXPECT_EQ(dst[4], 255);
    // 4x1 -> 1x1 averages all four.
    uint8_t row[16] = {0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 100, 100, 100, 255};
    uint8_t one[4];
    nanocompat::img::resample(row, 4, 1, 16, one, 1, 1, 4, false, true);
    EXPECT_EQ(one[0], 100);
    EXPECT_EQ(one[3], 255);
}

TEST(ImageResample, FiltersAlphaPremultiplied) {
    // A transparent red pixel next to an opaque blue one: the average is half-transparent blue,
    // not purple (the transparent pixel's colour must not bleed in).
    uint8_t src[8] = {255, 0, 0, 0, 0, 0, 255, 255};
    uint8_t straight[4], premul[4];
    nanocompat::img::resample(src, 2, 1, 8, straight, 1, 1, 4, true, true);
    EXPECT_EQ(straight[0], 0);
    EXPECT_EQ(straight[2], 255);
    EXPECT_NEAR(straight[3], 128, 1);
    nanocompat::img::resample(src, 2, 1, 8, premul, 1, 1, 4, true, false);
    EXPECT_EQ(premul[0], 0);
    EXPECT_NEAR(premul[2], 128, 1);
    EXPECT_NEAR(premul[3], 128, 1);
}

TEST(ImageResample, GrowsAndKeepsCorners) {
    uint8_t src[2 * 2 * 4] = {10, 10, 10, 255, 20, 20, 20, 255, 30, 30, 30, 255, 40, 40, 40, 255};
    std::vector<uint8_t> dst(5 * 5 * 4);
    nanocompat::img::resample(src, 2, 2, 8, dst.data(), 5, 5, 20, false, true);
    EXPECT_EQ(dst[0], 10);
    EXPECT_EQ(dst[(4) * 4], 20);
    EXPECT_EQ(dst[(4 * 5) * 4], 30);
    EXPECT_EQ(dst[(4 * 5 + 4) * 4], 40);
    const int mid = dst[(2 * 5 + 2) * 4];
    EXPECT_GT(mid, 10);
    EXPECT_LT(mid, 40);
}

TEST(ImageOrientation, AllEight) {
    // 3x2 stored image; pixel value = its index.
    uint8_t src[3 * 2 * 4];
    for (int i = 0; i < 6; i++) {
        src[i * 4] = uint8_t(i);
        src[i * 4 + 1] = src[i * 4 + 2] = 0;
        src[i * 4 + 3] = 255;
    }
    // The displayed image's pixels in row order, for each orientation.
    const int expect[9][6] = {
        {}, {0, 1, 2, 3, 4, 5}, {2, 1, 0, 5, 4, 3}, {5, 4, 3, 2, 1, 0}, {3, 4, 5, 0, 1, 2},
        {0, 3, 1, 4, 2, 5},     {3, 0, 4, 1, 5, 2}, {5, 2, 4, 1, 3, 0}, {2, 5, 1, 4, 0, 3},
    };
    for (int o = 1; o <= 8; o++) {
        SCOPED_TRACE(o);
        int w = 3, h = 2;
        uint8_t dst[3 * 2 * 4];
        nanocompat::img::applyOrientation(src, &w, &h, o, dst);
        EXPECT_EQ(w, o >= 5 ? 2 : 3);
        EXPECT_EQ(h, o >= 5 ? 3 : 2);
        for (int i = 0; i < 6; i++) EXPECT_EQ(int(dst[i * 4]), expect[o][i]);
    }
}

TEST(ImageDecoder, ArgumentChecks) {
    AImageDecoder* dec = reinterpret_cast<AImageDecoder*>(1);
    EXPECT_EQ(AImageDecoder_createFromBuffer(nullptr, 10, &dec), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_TRUE(dec == nullptr);
    EXPECT_EQ(AImageDecoder_createFromBuffer(kPng2x2, 0, &dec), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_EQ(AImageDecoder_createFromBuffer(kPng2x2, sizeof kPng2x2, nullptr), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_EQ(AImageDecoder_createFromFd(-1, &dec), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_TRUE(AImageDecoder_getHeaderInfo(nullptr) == nullptr);
    EXPECT_EQ(AImageDecoder_setTargetSize(nullptr, 1, 1), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_EQ(AImageDecoder_decodeImage(nullptr, nullptr, 0, 0), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_EQ(AImageDecoder_advanceFrame(nullptr), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    AImageDecoder_delete(nullptr);
    EXPECT_STREQ(AImageDecoder_resultToString(ANDROID_IMAGE_DECODER_FINISHED), "ANDROID_IMAGE_DECODER_FINISHED");
    EXPECT_TRUE(AImageDecoder_resultToString(12345) == nullptr);
    AImageDecoderFrameInfo* fi = AImageDecoderFrameInfo_create();
    ASSERT_TRUE(fi != nullptr);
    EXPECT_EQ(AImageDecoderFrameInfo_getDuration(fi), 0);
    AImageDecoderFrameInfo_delete(fi);
}

TEST(ImageDecoder, UnsupportedFormatsAndMissingLibraries) {
    AImageDecoder* dec = nullptr;
    std::vector<uint8_t> heic = bytes({0, 0, 0, 24});
    append(&heic, "ftypheic");
    heic.resize(64, 0);
    EXPECT_EQ(AImageDecoder_createFromBuffer(heic.data(), heic.size(), &dec), ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT);
    const auto junk = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    EXPECT_EQ(AImageDecoder_createFromBuffer(junk.data(), junk.size(), &dec), ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT);
    const auto bmp = bytes({'B', 'M', 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(AImageDecoder_createFromBuffer(bmp.data(), bmp.size(), &dec), ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT);
    // A format whose library this build lacks is unsupported too, and the caller falls back.
    for (Format f : {Format::Png, Format::Jpeg, Format::Gif, Format::Webp}) {
        if (nanocompat::img::libraryAvailable(f)) continue;
        std::vector<uint8_t> data;
        if (f == Format::Png) data.assign(kPng2x2, kPng2x2 + sizeof kPng2x2);
        if (f == Format::Gif) data.assign(kGif2Frames, kGif2Frames + sizeof kGif2Frames);
        if (f == Format::Jpeg) data = jpegHeader(8, 8, 0);
        if (f == Format::Webp) data = webpHeader("VP8L", bytes({0x2F, 0, 0, 0, 0}));
        SCOPED_TRACE(nanocompat::img::mimeType(f));
        EXPECT_EQ(AImageDecoder_createFromBuffer(data.data(), data.size(), &dec),
                  ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT);
        EXPECT_TRUE(dec == nullptr);
    }
}

TEST(ImageDecoder, EmptyFdIsIncomplete) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "empty.png";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    AImageDecoder* dec = nullptr;
    EXPECT_EQ(AImageDecoder_createFromFd(fd, &dec), ANDROID_IMAGE_DECODER_INCOMPLETE);
    close(fd);
}

// ---- real decoding (needs the libraries) ----

TEST(ImageDecoderPng, DecodesStraightAndPremultiplied) {
    if (!nanocompat::img::libraryAvailable(Format::Png)) GTEST_SKIP();
    AImageDecoder* dec = nullptr;
    ASSERT_EQ(AImageDecoder_createFromBuffer(kPng2x2, sizeof kPng2x2, &dec), ANDROID_IMAGE_DECODER_SUCCESS);
    const AImageDecoderHeaderInfo* hi = AImageDecoder_getHeaderInfo(dec);
    EXPECT_EQ(AImageDecoderHeaderInfo_getWidth(hi), 2);
    EXPECT_EQ(AImageDecoderHeaderInfo_getHeight(hi), 2);
    EXPECT_STREQ(AImageDecoderHeaderInfo_getMimeType(hi), "image/png");
    EXPECT_EQ(AImageDecoderHeaderInfo_getAlphaFlags(hi), ANDROID_BITMAP_FLAGS_ALPHA_PREMUL);
    EXPECT_FALSE(AImageDecoder_isAnimated(dec));
    EXPECT_EQ(AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGB_565),
              ANDROID_IMAGE_DECODER_INVALID_CONVERSION);
    EXPECT_EQ(AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(AImageDecoder_getMinimumStride(dec), 8u);

    // Default: premultiplied.
    uint8_t px[16];
    ASSERT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_SUCCESS);
    const uint8_t premul[16] = {255, 0, 0, 255, 0, 128, 0, 128, 0, 0, 0, 0, 255, 255, 255, 255};
    EXPECT_EQ(memcmp(px, premul, 16), 0);

    // Straight, into a wider stride.
    ASSERT_EQ(AImageDecoder_setUnpremultipliedRequired(dec, true), ANDROID_IMAGE_DECODER_SUCCESS);
    uint8_t wide[2 * 12];
    memset(wide, 0xEE, sizeof wide);
    EXPECT_EQ(AImageDecoder_decodeImage(dec, wide, 12, 11), ANDROID_IMAGE_DECODER_BAD_PARAMETER);  // too small
    EXPECT_EQ(AImageDecoder_decodeImage(dec, wide, 4, sizeof wide), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    ASSERT_EQ(AImageDecoder_decodeImage(dec, wide, 12, sizeof wide), ANDROID_IMAGE_DECODER_SUCCESS);
    const uint8_t row0[8] = {255, 0, 0, 255, 0, 255, 0, 128};
    const uint8_t row1[8] = {0, 0, 255, 0, 255, 255, 255, 255};
    EXPECT_EQ(memcmp(wide, row0, 8), 0);
    EXPECT_EQ(wide[8], 0xEE);  // the stride's padding is left alone
    EXPECT_EQ(memcmp(wide + 12, row1, 8), 0);

    // Crop to the bottom-right pixel.
    ASSERT_EQ(AImageDecoder_setCrop(dec, ARect{1, 1, 2, 2}), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(AImageDecoder_getMinimumStride(dec), 4u);
    uint8_t one[4];
    ASSERT_EQ(AImageDecoder_decodeImage(dec, one, 4, 4), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(memcmp(one, row1 + 4, 4), 0);
    EXPECT_EQ(AImageDecoder_setCrop(dec, ARect{1, 1, 3, 2}), ANDROID_IMAGE_DECODER_BAD_PARAMETER);
    EXPECT_EQ(AImageDecoder_setCrop(dec, ARect{0, 0, 0, 0}), ANDROID_IMAGE_DECODER_SUCCESS);

    // Scaled to 1x1: the premultiplied average, made straight again.
    ASSERT_EQ(AImageDecoder_setTargetSize(dec, 1, 1), ANDROID_IMAGE_DECODER_SUCCESS);
    ASSERT_EQ(AImageDecoder_decodeImage(dec, one, 4, 4), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_NEAR(one[3], (255 + 128 + 0 + 255) / 4, 1);
    EXPECT_EQ(AImageDecoder_setTargetSize(dec, 0, 1), ANDROID_IMAGE_DECODER_INVALID_SCALE);
    int32_t sw = 0, sh = 0;
    EXPECT_EQ(AImageDecoder_computeSampledSize(dec, 2, &sw, &sh), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(sw, 1);
    EXPECT_EQ(sh, 1);
    EXPECT_EQ(AImageDecoder_computeSampledSize(dec, 0, &sw, &sh), ANDROID_IMAGE_DECODER_INVALID_SCALE);
    EXPECT_EQ(AImageDecoder_advanceFrame(dec), ANDROID_IMAGE_DECODER_BAD_PARAMETER);  // not animated
    AImageDecoder_delete(dec);
}

TEST(ImageDecoderPng, FromFd) {
    if (!nanocompat::img::libraryAvailable(Format::Png)) GTEST_SKIP();
    compat_test::ScratchDir dir;
    const std::string path = dir / "a.png";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(write(fd, kPng2x2, sizeof kPng2x2), ssize_t(sizeof kPng2x2));
    ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
    AImageDecoder* dec = nullptr;
    ASSERT_EQ(AImageDecoder_createFromFd(fd, &dec), ANDROID_IMAGE_DECODER_SUCCESS);
    close(fd);  // the decoder does not need it any more
    AImageDecoder_setUnpremultipliedRequired(dec, true);
    uint8_t px[16];
    ASSERT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(px[5], 255);
    EXPECT_EQ(px[7], 128);
    AImageDecoder_delete(dec);
}

TEST(ImageDecoderGif, AnimatesAndComposites) {
    if (!nanocompat::img::libraryAvailable(Format::Gif)) GTEST_SKIP();
    AImageDecoder* dec = nullptr;
    ASSERT_EQ(AImageDecoder_createFromBuffer(kGif2Frames, sizeof kGif2Frames, &dec), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_TRUE(AImageDecoder_isAnimated(dec));
    EXPECT_EQ(AImageDecoder_getRepeatCount(dec), ANDROID_IMAGE_DECODER_INFINITE);
    EXPECT_EQ(AImageDecoder_setTargetSize(dec, 1, 1), ANDROID_IMAGE_DECODER_INVALID_SCALE);  // as Nano saw
    EXPECT_EQ(AImageDecoder_setTargetSize(dec, 2, 2), ANDROID_IMAGE_DECODER_SUCCESS);
    AImageDecoder_setUnpremultipliedRequired(dec, true);
    AImageDecoderFrameInfo* fi = AImageDecoderFrameInfo_create();
    uint8_t px[16];

    ASSERT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_SUCCESS);
    const uint8_t f0[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 255};
    EXPECT_EQ(memcmp(px, f0, 16), 0);
    ASSERT_EQ(AImageDecoder_getFrameInfo(dec, fi), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(AImageDecoderFrameInfo_getDuration(fi), 100000000);

    ASSERT_EQ(AImageDecoder_advanceFrame(dec), ANDROID_IMAGE_DECODER_SUCCESS);
    ASSERT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_SUCCESS);
    const uint8_t f1[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255};
    EXPECT_EQ(memcmp(px, f1, 16), 0);  // frame 2 painted over frame 1
    ASSERT_EQ(AImageDecoder_getFrameInfo(dec, fi), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(AImageDecoderFrameInfo_getDuration(fi), 200000000);
    const ARect r = AImageDecoderFrameInfo_getFrameRect(fi);
    EXPECT_EQ(r.left, 1);
    EXPECT_EQ(r.right, 2);
    EXPECT_TRUE(AImageDecoderFrameInfo_hasAlphaWithinBounds(fi));

    EXPECT_EQ(AImageDecoder_advanceFrame(dec), ANDROID_IMAGE_DECODER_FINISHED);
    EXPECT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_FINISHED);
    ASSERT_EQ(AImageDecoder_rewind(dec), ANDROID_IMAGE_DECODER_SUCCESS);
    ASSERT_EQ(AImageDecoder_decodeImage(dec, px, 8, sizeof px), ANDROID_IMAGE_DECODER_SUCCESS);
    EXPECT_EQ(memcmp(px, f0, 16), 0);
    AImageDecoderFrameInfo_delete(fi);
    AImageDecoder_delete(dec);
}

TEST(ImageDecoderJpegWebp, NoFixturesYet) {
    // TODO(G10): decode small JPEG (with an EXIF orientation) and WebP (still and animated)
    // fixtures once an encoder is installed to make them.
    GTEST_SKIP();
}
