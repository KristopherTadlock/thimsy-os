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

// Game manuals: which file is a game's manual, and the pages of a comic book manual.
//
// ES-DE keeps a game's manual in downloaded_media/<system>/manuals/<game>.pdf. Nano also reads comic
// book archives with the same base name (OA-42): .cbz (a zip), .cb7 (a 7z) and .cbr (a RAR, which
// nanoarc refuses where its back end has no RAR reader, as on Android). A PDF wins when both exist.
// A comic manual's pages are the archive's images in natural order ("p2" before "p10"), read and
// decoded one at a time (NanoMenuPdf.cpp). Plain functions of names and sizes, no I/O, so the host
// tests cover them.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "NanoArchive.h"

namespace nanomanual {

// The extensions a game's manual can have, best first, null-terminated: a PDF, then the comic book
// archives.
extern const char* const kManualExts[];

enum class Kind { None, Pdf, Comic };
Kind kindOf(const std::string& path);   // by extension, in any case

// The archive format a comic book manual's extension names: .cbz Zip, .cb7 SevenZip, .cbr Rar; else None.
nanoarc::Format comicFormat(const std::string& path);
// The format a comic book manual is read as: what its first bytes announce (content, from
// nanoarc::formatFromContent) when they announce one, so a .cbz that is really a RAR is refused as a RAR
// and a .cbr that is really a zip opens; else what its extension names. None when path is not a comic
// book manual's name, whatever the content.
nanoarc::Format comicFormat(const std::string& path, nanoarc::Format content);

// An archive the reader can't open because of its format, not its content: a RAR where nanoarc has
// no RAR reader, or a 7z whose file list needs a method the decoder lacks.
bool unsupportedFormat(nanoarc::Err err);

// What one page may cost, for 1 GB devices. A page whose stored image is over kMaxPageBytes, or that
// sits in a 7z solid block over kMaxBlockBytes (decoded whole into memory), is not read; an image over
// kMaxSourcePixels is not decoded; a decoded page is at most kMaxPageDim pixels on either side.
constexpr uint64_t kMaxPageBytes = 64ull << 20;
constexpr uint64_t kMaxBlockBytes = 128ull << 20;
constexpr int64_t kMaxSourcePixels = 24ll * 1000 * 1000;
constexpr int kMaxPageDim = 4096;

// An entry that is a page: a .png, .jpg, .jpeg, .webp or .gif file, not hidden (a name starting with
// '.', as macOS's "._" files) and not in a __MACOSX folder.
bool isPageName(const std::string& entryPath);

// Natural order: runs of digits compare as numbers (leading zeros aside), letters ignore case, and
// '/' sorts first, so a folder's pages stay together. Names equal that way fall back to plain order.
bool naturalLess(const std::string& a, const std::string& b);

// The pages of a comic book archive (readIndex's entries): its non-empty page files, links left out,
// in natural order, each name once.
std::vector<std::string> comicPages(const std::vector<nanoarc::Entry>& entries);

// Whether an image of iw x ih pixels may be decoded as a page.
bool pageDecodable(int iw, int ih);

// The size a page of iw x ih pixels is decoded at for the viewer's zoom (0 fits the page in the box,
// 1 fits its width, 2 is twice the width), as PDF pages are rendered: never over kMaxPageDim on
// either side, at least 1.
void pageSize(int iw, int ih, int zoom, int boxW, int boxH, int* w, int* h);

}  // namespace nanomanual
