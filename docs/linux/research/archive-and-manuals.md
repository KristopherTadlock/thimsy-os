# Archive browsing and the manual viewer on the Linux alpha (OA-16)

Research note, 2026-11-26. Nothing here has been built or run on a device. File references are to this fork at `e002d39` (branch `linux`). Web sources are listed at the end as [S1] to [S46]. Corrected on 2026-11-27 after a fact-check. Guesses are marked *(guess)*. Licence statements are analysis, not legal advice.

OA-16 (DECISIONS.md) puts two features into the Linux alpha: archive browsing in the File Explorer (games inside .zip and .7z) and the manual viewer, for PDF and comic-archive manuals from ES-DE's downloaded media. The OTA check stays off (`Cap::OtaUpdate` false). Under OA-4 each one is a capability behind the platform layer. This note compares the libraries and recommends a staged plan.

## 1. Summary

| Feature | Android today | Recommended for Linux | Why |
|---|---|---|---|
| Zip and 7z browse, extract, Compress to Zip | AOSP libziparchive and the LZMA SDK, in `NanoArchive.cpp` | **libarchive**, as a Linux back end behind Nano's existing `nanoarc` API | Buildroot package; BSD licence; one library reads zip, 7z and RAR and writes zip; 7z is decoded as a stream, so the 512 MiB solid-block buffer goes away on 1 GB devices |
| PDF manuals | PDFium, in-process, in `NanoMenuPdf.cpp` | **Poppler in a separate helper program**, behind a new `nanoplat` PDF interface. First ES-DE's own `es-pdf-convert` (its BGRA bytes swapped to RGBA), then a Thimsy helper that keeps the document open if page turns are too slow | Buildroot package; GPL code stays outside Nano's binary (the pattern ES-DE uses); PDFium has no Buildroot package and needs Chromium's build tools; MuPDF is AGPL |
| Comic-archive manuals (.cbz, .cb7, .cbr) | Found by the theme engine, **dropped by the game information page** and refused by the viewer | **In the alpha (OA-16).** Shared viewer code: pages read through `nanoarc` (libarchive on Linux reads all three), decoded by Nano's image decoder | Needs no PDF library and no package beyond libarchive. ES-DE itself shows only PDF manuals, so this is Nano's own addition (OA-8 allows it) |
| OTA check | `NanoOtaCheck.cpp` | Stays out. Split it from the archive switch | OA-16 |

## 2. What Nano uses today

### 2.1 Archives in the File Explorer

- **API:** `nano/NanoArchive.h` (92 lines) is already free of platform types. It holds `Format` (Zip, SevenZip, Rar), `Err`, a `Progress` block, `readIndex`, `extract` and `compressToZip`. Its callers are all in `NanoMenuFileExplorer.cpp`: the error texts at :96-101, listing at :243-246 and :360, and the worker jobs at :533-537.
- **Implementation:** `nano/NanoArchive.cpp` (834 lines).
  - Zip uses AOSP's libziparchive: `OpenArchive`, `StartIteration` and `Next` to read (:249-257), and `ZipWriter` at deflate level 6 to create zips (:523-537). Its includes are at :27-28.
  - 7z uses the LZMA SDK's C decoder: `SzArEx_Open` and `SzArEx_Extract` (:387-435; includes `7z.h`, `7zCrc.h` and `7zFile.h` at :30-32). Android.bp links it as the `liblzma` module (Android.bp:227).
  - The file already has an internal back-end seam: a `Source` class (`open`, `entries`, `blockSize`, `write`; :207-220) with `ZipSource` (:243), `SevenZSource` (:362) and a `makeSource` factory (:504-512). The safety rules, extraction, rollback and free-space checks sit above that seam.
- **Libraries:** Android.bp:226-227 (`libziparchive`, `liblzma`). The host test `gammaos_nano_archive_tests` (Android.bp:864-880) runs 16 tests (`test/archive/NanoArchive_test.cpp`) against six 7z fixtures in `test/fixtures/archive/`.
- **Behaviour Linux must keep** (NanoArchive.h:1-11): archives are treated as untrusted input.
  - Entries with absolute names or `..` are skipped, and so are links.
  - Files are created with `O_CREAT|O_EXCL|O_NOFOLLOW` through directories opened without following links, and never written past their declared size.
  - A 7z solid block over `k7zBlockCap` = **512 MiB** is refused before anything is written (NanoArchive.h:48). The decoder's total memory is capped at that plus 64 MiB (NanoArchive.cpp:292).
- **Formats it does not support:**
  - RAR is refused by extension (`makeSource`, :508; the explorer checks again at NanoMenuFileExplorer.cpp:348, message "RAR archives aren't supported.").
  - Zips whose names are not UTF-8 get `Err::NameEncoding` (NanoArchive.h:29).
  - Encrypted entries, and 7z methods the SDK lacks, are refused.
- **The OTA check shares the switch.** `nano/cmake/features.cmake` puts `NanoArchive.cpp` and `NanoOtaCheck.cpp` together under `NANO_WITH_ARCHIVE`. `NanoOtaCheck.cpp` also runs `/system/bin/curl` and `unzip` (:312-334).
- **Other zip users, outside the switch:**
  - `NanoBiosCatalog` and `NanoNdsBanner` use zlib directly (IMPLEMENTATION.md, table row "Platform headers of the host-tested files"), and so already work on Linux.
  - ES-DE theme install runs `/system/bin/unzip -o -q` (`NanoMenuEsdeMenu.cpp:972`). The shell gate passes it to Linux's `unzip` today (audit-2026-11-26/inv_shell.md:153); OA-4's deny-all gate will refuse it. ARCHITECTURE-AUDIT.md step 4 plans an `Archive` interface for this site.

### 2.2 The manual viewer

- **Code:** `nano/NanoMenuPdf.cpp` (391 lines), opened with Up on the ES-DE game information page (`NanoMenuEsdeMenu.cpp:1430`).
- **Library:** PDFium (`fpdfview.h`; Android.bp:225 `libpdfium`). Calls: `FPDF_InitLibrary`, `FPDF_LoadDocument`, `FPDF_GetPageCount`, `FPDF_LoadPage`, `FPDF_GetPageWidthF`/`HeightF`, `FPDFBitmap_Create`, `FPDF_RenderPageBitmap` with `FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER`, and `FPDF_GetLastError` (to tell a password-protected file apart) (:70-120).
- **Threading:** PDFium is called from one worker thread under a global mutex, because "PDFium is not thread safe" (:36). The render thread uploads each page as an RGBA `GL_TEXTURE_2D` (GLES2, :189-196). The worker holds its own reference, so closing the viewer never waits for a page still being drawn.
- **Limits:** a page is at most `kPdfMaxDim` = 4096 px on either side, and at most `kPdfMaxTextures` = 4 pages are kept (:38-39).
- **Formats:** only `.pdf`. Any other manual shows "This manual is not a PDF and can't be opened" (:57-62).
- **How Nano finds manuals.** `esdeGameMediaPath(rom, "manual")` maps the type to the `manuals` folder and looks for `<media root>/<system>/manuals/<game>` with `.pdf`, `.cbz`, `.cbr` and `.cb7`, in that order, so a PDF wins when both exist (`NanoThemeEngine.cpp:1768-1773`, `:1915-1925`). The media roots are ES-DE's `downloaded_media` folders (`esdeMediaRoots`, just above). The scrape queue counts the same four extensions as a manual already present (`NanoScrapeQueue.cpp:513`).
- **Where comics stop today.** The game information page keeps the manual only if its name ends in `.pdf` (`NanoMenuEsdeMenu.cpp:1050-1056`, "a comic book archive manual is not offered"), and the viewer refuses anything else (`NanoMenuPdf.cpp:57-62`). The theme engine's comment says "ES-DE also opens comic book archives as manuals" (`NanoThemeEngine.cpp:1768`), but that is wrong: ES-DE's `getManualPath` looks only for `.pdf` (`es-app/src/FileData.cpp`, `extList {".pdf"}` [S1]). So ES-DE never shows a comic-archive manual, and Nano's viewer will be the only one that does. OA-16 puts it in the Linux alpha; it changes no shared file format, so OA-8 allows it.
- **How ES-DE does it.** ES-DE added its PDF manual viewer in 2.1.0 together with "a separate es-pdf-convert binary to render PDF pages using Poppler" [S2]. That helper's source says it is a separate binary because of Poppler's GPL licence. It carries `SPDX-License-Identifier: GPL-2.0-only`, and on desktop it writes raw ARGB32 pixels for one page to stdout. It loads the whole file again on every call, and `-fileinfo` lists the page sizes [S3]. Poppler's `argb32` image is one native-endian 32-bit word per pixel, 0xAARRGGBB (`poppler-image.cpp`'s `save()` takes red from bits 16-23 and blue from bits 0-7 [S46]). On little-endian arm64 the bytes in memory are therefore B, G, R, A. Its CMake file is MIT and needs only poppler-cpp [S4].

### 2.3 Images

The photo viewer (`NanoMenuPhotos.cpp:26-36`) and the ES-DE theme decode through `AImageDecoder`. On Linux that is Nano's own decoder shim on libpng, libjpeg-turbo, libwebp and giflib (`nano/compat/src/imagedecoder.cpp`; decision I30). HEIF and AVIF are unsupported there. The shim decodes at a reduced size (JPEG DCT scaling at :816-817, WebP scaling at :913-920). `thimsy-nano` already selects all four libraries (`linux/package/thimsy-nano/Config.in`).

### 2.4 The Linux build today

- `thimsy-nano.mk:52-55` passes `-DNANO_WITH_PDF=OFF -DNANO_WITH_ARCHIVE=OFF`. When those switches are ON, `nano/CMakeLists.txt:377-384` expects `pdfium` and `ziparchive` plus `lzma_sdk`. Files left out link as generated inert stubs, so the menu entries fail when used (`features.cmake` header).
- There is no `Cap` enum and no `has()` yet. `nano/NanoPlatform.h` has Display, Launcher, Audio, Volumes, Power, SystemActions, Network and InputPolicy only. `Cap` and `Platform::has()` are migration step 3 in ARCHITECTURE-AUDIT.md.

## 3. Constraints

- **Licences.**
  - Nano is Apache-2.0 and ships as a separate program in the image (linux_handheld_plan.md 3.3).
  - Apache-2.0 code may go into a GPLv3 work, but it is not compatible with GPLv2 [S5].
  - GPLv3 section 13 lets a GPLv3 work be combined with an AGPLv3 work [S6].
  - The `linux/` tree is MIT. `linux/gpl/` holds copyleft and third-party material, each item under its own licence with an SPDX line: GPL-2.0 for kernel, U-Boot and ROCKNIX imports, GPL-3.0 for RetroArch patches, zlib for SDL patches (D6 as refined by I5, I9 and I41). Package patches go in `linux/gpl/patches/<package>/` under that package's licence (I9). Selecting a Buildroot package from a defconfig copies no recipe text.
  - Whether `thimsy-nano` is already a GPL-3.0 combination because of the GPL-2.0+ Kanji table is an open owner question (plan 3.3; IMPLEMENTATION.md E.1 marks it *(guess)*). This note does not depend on its answer. The recommended path keeps GPL and AGPL code out of Nano's binary either way.
- **The GPL boundary between programs.** The FSF's FAQ says pipes, sockets and command-line arguments are "normally" used between separate programs, unless the two exchange complex internal data structures [S7]. gnu.org refused the fetch (HTTP 429), so this wording comes from a page that quotes the FAQ (see [S7]).
- **Size.** The RK3566 image's root slot is 1 GiB of EROFS compressed with LZ4HC (`linux/board/rk3566/genimage.cfg:56-61`; `common.fragment:56-57`). I found no per-feature size budget written in the docs. OA-16 asks for one to be respected, so section 7 gives estimates to be measured.
- **Memory and CPU.**
  - The floor is 1 GB, on the RG ARC-S (plan 2.1). The ARC-D runs with `mem=1G` in its place (OA-18).
  - The RK3566 has four Cortex-A55 cores at 1.8 GHz and a Mali-G52 2EE GPU [S8].
- **OA-4 rules** (ARCHITECTURE-AUDIT.md R1-R4):
  - Shared code may use portable facilities, but it runs no processes.
  - A capability Linux has not implemented reports unsupported, does nothing and logs once.
  - The UI asks `has(Cap)`.
  - Files compiled into drastic-nano, the scraper or the host tests use build-chosen translation units, not `nanoplat::` (OA-11's FrozenApi rule; ARCHITECTURE-AUDIT.md "Cautions"). `NanoArchive.cpp` is in the host archive test, so this rule applies to it.
- **OA-8 parity.** Linux supports every on-card format Android supports. Supporting more is allowed, but it changes no shared file format.
- **Build host.** Nothing may be downloaded or built in this wave. In later waves the build shares this machine with AOSP jobs (IMPLEMENTATION.md C.3), so a library needing a large separate toolchain costs real build time.

## 4. Archive libraries

| Option | Formats | Licence | Buildroot (master) | Fit |
|---|---|---|---|---|
| **libarchive** | Reads zip (a streaming and a seeking reader [S12]), 7z, RAR and RAR 5 ("with some limitations due to RAR's proprietary status"), CAB, LHA, ISO 9660 [S9]. Writes zip with deflate, levels 0-9, Zip64 and header charsets [S13] | BSD-2-Clause for most of it; a few files under BSD-3, public domain or CC0/OpenSSL/Apache-2.0 [S10] | `libarchive` 3.8.10; zlib, xz (liblzma), zstd, lz4, bzip2 and libiconv are optional [S11] | Permissive, so it fits Apache Nano and the MIT tree. One library covers both formats and the theme-install `unzip` site |
| minizip-ng plus the LZMA SDK | Zip only (Zip64, PKWARE and WinZip AES, CP437/CP932 names through iconv [S14]); 7z through the SDK | Zlib [S14]; the SDK is public domain [S15] | `minizip` 4.0.3 is minizip-ng [S16]. No LZMA SDK package [S17] | Two libraries, and still no RAR. The SDK keeps the whole-block memory problem (section 4.2) |
| libzip plus the LZMA SDK | Zip only | BSD-3-Clause [S18] | `libzip` 1.11.4 [S18] | As minizip-ng. ARCHITECTURE-AUDIT.md step 4 names libzip, but only for the zip-only shell sites |
| libziparchive plus the LZMA SDK (Android's own) | As Android today | Apache-2.0 [S19]; SDK public domain [S15] | Neither is packaged. Debian and Ubuntu build libziparchive outside AOSP (`android-libziparchive`, 145 kB installed, needs `android-libbase` and `android-liblog`) [S20][S21] | Leaves `NanoArchive.cpp` unchanged on both platforms and its tests apply as they are. But it needs two Thimsy recipes plus libbase and liblog shims, keeps the 512 MiB block buffer, and still has no RAR |
| p7zip (`7za` binary) | 7z, zip and more, as a separate program | "LGPL-2.1+ with unRAR restriction" [S22] | `p7zip` 17.06 [S22] | A process per action and output parsing; a program on Linux only. Not needed if libarchive is used |
| unrar | RAR | The unRAR licence: freely distributable, but may not be used to re-create the RAR compression algorithm or a RAR-compatible archiver [S23] | `unrar` 7.2.6 [S24] | Not free software in the usual sense. Not recommended; libarchive reads RAR without it |

### 4.1 libarchive's limits that matter here

- **No random access.** It is "heavily stream-oriented"; random access is not directly supported. For some formats it re-opens the archive and scans it again [S9].
  - Nano's `extract` walks `Source::entries()` in archive order (NanoArchive.cpp:590). So a libarchive `Source` can keep its read position and re-open only when asked for an earlier entry *(design guess)*.
  - Browsing reads only the headers.
- **7z.**
  - Supported methods: Copy, LZMA, LZMA2 (through liblzma), Deflate, BZip2, PPMd and Zstd, plus the BCJ, BCJ2, ARM, ARM64 and Delta filters.
  - Encrypted entries are flagged and fail with "Crypto codec not supported yet". That matches Android's `Unsupported` and `Encrypted` errors.
  - Data is decoded through a 64 KiB buffer and returned in pieces of at most 16 MiB, not a whole solid block. PPMd is the exception: it allocates its model up front, up to 576 MiB [S25].
- **RAR 5.**
  - Solid and multi-volume archives are supported. Encrypted headers fail; encrypted data is flagged but not decrypted.
  - Dictionaries over 64 MiB are refused, and the window costs about twice the dictionary in memory [S26].
  - RAR archives made with RAR 7's larger dictionaries would therefore fail *(guess that such archives are rare for ROMs)*.
- **Non-UTF-8 zip names.** libarchive converts header charsets through iconv. glibc has iconv built in *(guess that Buildroot's libarchive finds it without the libiconv package; check at configure)*. Android reports `NameEncoding` instead. Linux would open more zips, which is allowed.

### 4.2 Memory on a 1 GB device

- **Android's 7z path cannot run as it is on 1 GB.** The LZMA SDK decodes a whole solid block into memory, and Nano allows blocks up to 512 MiB plus 64 MiB of decoder state (NanoArchive.h:10, :48; NanoArchive.cpp:292). On a 1 GB device holding the GPU driver, Nano's textures and the page cache, a 400 MiB block would likely get Nano killed by the OOM killer *(guess)*.
- **libarchive streams LZMA and LZMA2.** Its memory is about the dictionary size plus small buffers *(guess, from [S25]'s fixed buffers and liblzma's documented decoder behaviour, not measured)*.
- **The remaining risks:**
  - A PPMd 7z: model up to 576 MiB [S25].
  - An LZMA2 archive made with a very large dictionary *(guess: 7-Zip's highest presets use 64 MiB and larger values can be set by hand)*.
  - libarchive does not take a memory limit or an allocator for these decoders *(guess; I found no such API)*.
  - Mitigations, either of which is enough: extract in a short-lived child process with `RLIMIT_AS` (a platform-layer process start, allowed by OA-5 and OA-30), or read the coder properties from the 7z header before extracting and refuse what is too large. Both are Linux-only code.
- **Size.** Ubuntu's arm64 `libarchive13t64` is 903 kB installed, built with more options than Thimsy needs [S27]. With zlib, which the image already has, plus xz's liblzma, the cost should be about 1 MB *(guess)*. liblzma is 0BSD [S28].

## 5. PDF libraries

| Option | Licence | Buildroot (master) | In Nano's process? | Fit |
|---|---|---|---|---|
| **PDFium** (what Android uses) | BSD-3-Clause plus Apache-2.0 [S29] | None (no `pdfium` package, no `gn` package) [S17] | Yes. `NanoMenuPdf.cpp` compiles unchanged | Best licence and exact Android parity. But the build uses depot_tools, `gclient`, GN and Ninja; the project means Clang, with only "community-contributed patches for gcc" [S30]. A Buildroot recipe would fetch Chromium's build tree and toolchain. The other route is a third-party prebuilt (`pdfium-binaries` has linux-arm64 glibc builds [S31]): a binary Thimsy did not build, which is the owner's call, like libmali |
| **MuPDF** | AGPL-3.0-or-later, or a paid commercial licence [S32][S33] | `mupdf` 1.23.9, AGPL-3.0+, needs freetype, gumbo, harfbuzz, jbig2dec, jpeg, lcms2, openjpeg and zlib, with no font-trimming options [S34] | Only if Nano's binary becomes an AGPL combination (GPLv3 section 13 [S6] and Apache-to-GPLv3 [S5]; *our reading*) | Fast and tunable: a soft cache cap through `fz_new_context(..., max_store)`, default 256 MiB, and a cloned context per thread [S35][S36]. Fonts dominate its size: Ubuntu's `mupdf-tools` is 86 MB installed [S37]. `TOFU`, `TOFU_CJK` and `TOFU_SIL` leave fonts out and the `FZ_ENABLE_*` switches drop document types [S38], but Buildroot's recipe sets neither, so Thimsy would need its own recipe (a copy, under `gpl/`). Out of process (`mutool draw`) it avoids AGPL in Nano, but costs a process per page |
| **Poppler** | GPL; "programs which call Poppler must be licensed under the GPL as well" [S39]. The xpdf-derived code is GPL-2 or GPL-3 only, and many newer files are "2 or later" [S40] | `poppler` 25.10.0, licence recorded as `GPL-2.0+`, needs fontconfig and zlib; jpeg, png, lcms2, openjpeg, cairo and the rest are optional, each on only when that package is selected [S41] | Same issue as MuPDF, with GPL instead of AGPL | ES-DE's proven pattern: GPL code in its own helper program [S2][S3]. Ubuntu's arm64 `libpoppler134` is 3.5 MB installed [S42]. Without embedded fonts it relies on fontconfig and the system fonts *(guess: a manual with base-14 fonts not embedded needs a fallback font in the image; PDFium and MuPDF carry their own)* |

### 5.1 Which PDF route

- **In-process MuPDF or Poppler is ruled out** for the alpha. Either one changes the licence of Nano's Linux binary, and so of any later Linux app build (C.2). OA-16 asks for exactly this licence check, and the Kanji question is still open.
- **PDFium in-process** is the only route that keeps both licence and parity. It is blocked on a build route Thimsy does not have yet. It is worth a time-boxed spike, but not for the alpha.
- **Poppler out of process** is buildable now from Buildroot packages.
  - Its first form needs no new GPL code: ES-DE's `es-pdf-convert` already prints page sizes and renders one page at a requested size to stdout [S3].
  - A recipe Thimsy writes for it is MIT and lives in `linux/package/`. The program it builds is GPL-2.0-only, and legal-info saves its source.
  - Its cost is a process start and a full re-read of the file for every page [S3]. Nano's worker thread already hides slow pages behind the current one.
  - Its output is BGRA in memory (section 2.2), but `NanoMenuPdf.cpp` uploads `GL_RGBA` bytes (:196). The Linux renderer swaps R and B in the worker before handing the page over. A `GL_BGRA_EXT` upload would avoid the pass where the driver has `EXT_texture_format_BGRA8888` *(guess for Panfrost and libmali: check in Phase 0)*; the swap is the safe default and costs one pass over a few MB.
  - Poppler must be built with `BR2_PACKAGE_JPEG` and `BR2_PACKAGE_OPENJPEG` selected. Without them Buildroot 2026.08.2's `poppler.mk` passes `-DENABLE_DCTDECODER=none` and `-DENABLE_LIBOPENJPEG=none` [S41], and JPEG and JPEG 2000 scans, which most scanned manuals are, would not render. The cpp frontend that `es-pdf-convert` needs has no Buildroot switch: it is built because upstream's `ENABLE_CPP` defaults to ON [S45] and `poppler.mk` does not turn it off.
- **If page turns are too slow**, a small persistent helper that keeps one document open would fix it: say `thimsy-pdf-render`, talking over a pipe. It links Poppler, so its source goes under `linux/gpl/src/thimsy-pdf-render/` with its own licence and an SPDX line in each file (D6 as refined).
  - **Its licence.** Poppler's xpdf-derived code is GPL-2 only or GPL-3 only, and many newer files are "2 or later" [S40]. A program linking Poppler can be passed on under GPL-2.0 or under GPL-3.0, but not as "or later". Recommended: `SPDX-License-Identifier: GPL-2.0-or-later` for the helper's own files, so the linked binary can go out under either version; ES-DE's `es-pdf-convert` uses `GPL-2.0-only` [S3], which also works. The owner confirms (section 11, Q4).
  - It must hold no Nano code: Apache-2.0 is not compatible with GPLv2 [S5] (plan 3.3: never paste Apache code into GPL-2.0 files).

## 6. Comic-archive manuals in the alpha

OA-16 puts them in the Linux alpha. They need no PDF library: the viewer reads them in shared code.

- **MuPDF's CBZ handler is no help here.**
  - It opens `.cbz`, `.zip`, `.cbt` and `.tar`, plus `.cbr` only when built with libarchive; it does not open `.cb7` [S43][S44].
  - It accepts BMP, GIF, JPEG, JPEG 2000, JBIG2, JPEG XR, PNM, PNG and TIFF pages, but not WebP [S43].
- **Finding them.** No change: `esdeGameMediaPath` already finds `.cbz`, `.cbr` and `.cb7` in ES-DE's `manuals` folders (section 2.2). The game information page's `.pdf` filter (`NanoMenuEsdeMenu.cpp:1052-1056`) becomes a check of `has(Cap::ManualComic)` for the three comic extensions.
- **Reading the pages.** Through `nanoarc`, in shared code:
  - `readIndex` lists the entries. The viewer keeps image entries (`.png`, `.jpg`, `.jpeg`, `.webp`, `.gif`), skips folders and links, and sorts by name in natural order (MuPDF sorts "page2" before "page10" the same way [S43]).
  - A new call reads one entry into memory, with a size cap: today's API only extracts to disk. The archive rules of section 2.1 still apply: the declared size is checked and the cap refuses an oversized page.
  - `AImageDecoder_createFromBuffer` decodes the page at the screen's size or the zoom's (Nano's shim on Linux, section 2.3; the NDK on Android). The RGBA result goes into the same texture, zoom and input code as a PDF page.
- **Formats on Linux.** libarchive reads `.cbz` (zip), `.cb7` (7z) and `.cbr` (RAR 4 and RAR 5, with section 4.1's limits) [S9]. Its reader picks the format from the content, so a `.cbr` that is really a zip still opens *(guess at how common that is)*. Encrypted archives are refused.
- **RAR for manuals, not for the explorer.** `makeSource` refuses RAR for every caller today (`Err::Rar`, `NanoArchive.cpp:508`). After stage 1's split, `makeSource` belongs to each back end (section 8): Android's keeps `Err::Rar`, and the libarchive one opens RAR. The File Explorer keeps its own RAR refusal (`NanoMenuFileExplorer.cpp:348`) until the owner answers Q1, so on Linux only the manual viewer opens `.cbr`.
- **On Android.** The same shared code would read `.cbz` through libziparchive and `.cb7` through the LZMA SDK (with its 512 MiB block limit); `.cbr` stays refused. Until Android reports `Cap::ManualComic`, it keeps today's behaviour. Whether it turns on in the same release is the owner's call (section 11, Q2).
- **Memory per page.** A 2000x3000 page is 24 MB as RGBA. Decoding it to the screen's size first, as the shim does, keeps one texture to a few MB. The compressed page bytes are held only until the decode ends *(arithmetic, not measured)*.
- **Page turns in a solid `.cb7`.** libarchive has no random access (section 4.1). Reaching page N of a solid 7z decodes the block from its start, and going back re-opens the archive. Backward turns in a large solid `.cb7` may be slow *(guess; measure)*. The four cached pages (`kPdfMaxTextures`) hide the common case. `.cbz` uses libarchive's seeking zip reader [S12] and does not have this cost.

## 7. Memory, CPU and size on a 1 GB RK3566

All figures below are estimates to be measured on the ARC-D with `mem=1G` (OA-18).

- **Nano's own PDF budget is the same whatever the library.**
  - Fit-to-screen pages on the 640x480 panel are about 1 to 2 MB each.
  - A 2x zoom page is about 1280x1650x4, roughly 8.5 MB.
  - The ceiling is one 4096x4096 bitmap, 64 MiB, and Nano keeps at most four pages (NanoMenuPdf.cpp:38-39) *(arithmetic on Nano's constants; the zoom formula is at :105)*.
  - With a helper process, the bitmap is copied once over the pipe, and the helper's memory is all returned when it exits.
- **The helper's peak memory** is about the file size, because `es-pdf-convert` reads the whole file [S3], plus Poppler's working set for one page *(guess: tens of MB for a typical scanned manual)*.
- **Comic pages** cost one image decode at screen size each, in Nano's process, with no helper.
- **CPU.** A scanned manual page is mostly one JPEG or JPEG 2000 image. The decode cost is the same order of magnitude in every engine on an A55 *(guess)*. The helper's start and re-parse may add 100 to 500 ms per page *(guess, to measure)*; the persistent helper removes that.
- **GPU.** A 4096 px texture is within GLES 3 limits on Mali-G52 under both libmali and Panfrost *(guess; check `GL_MAX_TEXTURE_SIZE` in Phase 0)*.
- **Image size.** libarchive and liblzma about 1 MB; comic manuals add nothing more. Poppler, poppler-cpp, fontconfig, libjpeg and OpenJPEG (freetype is already in the image) about 4 to 6 MB. The helper well under 100 kB. In total about 1% of the 1 GiB root slot *(guess; compare with `make legal-info`'s package list and `du` of the target tree)*.

## 8. How each one sits behind the platform layer (OA-4)

- **Capabilities.** Four entries for step 3's `Cap` enum: `Cap::ArchiveBrowse`, `Cap::ManualPdf`, `Cap::ManualComic` and `Cap::OtaUpdate` (false on Linux). Later, perhaps `Cap::ArchiveRar`. The File Explorer's archive rows and the Up-for-manual prompt ask `has()`.
- **Archives: a back end chosen at build time, not a `nanoplat` class.** `NanoArchive.cpp` is compiled into a host test, so the FrozenApi rule applies.
  - Split the file at its existing `Source` seam. `NanoArchive.cpp` keeps the shared part: name cleaning, safe creation, extract and rollback, free-space checks, compress orchestration, about lines 1-196 and 556-834.
  - A new internal header holds `RawEntry`, `Source`, `makeSource` and `writeZip`.
  - `NanoArchiveSourcesAndroid.cpp` gets `ZipSource`, `SevenZSource` and `writeZip` moved verbatim (about lines 198-553).
  - `nano/platform/linux/ArchiveLibarchive.cpp` implements the same four symbols on libarchive.
  - Android.bp lists the Android file; CMake lists the libarchive one when `NANO_WITH_ARCHIVE=ON` and `NANO_PLATFORM=linux`.
  - That is one Android-surface commit (a verbatim move under H.3) and no behaviour change on Android.
  - The host archive test runs against both back ends. A test of a back-end limit, such as `SevenZipOversizedBlockIsRefusedUpFront`, is skipped on the back end that has no such limit.
- **RAR stays refused in the Linux File Explorer at first**, so the two explorers match. The libarchive `makeSource` does open RAR, for comic manuals (section 6); the explorer's extension check at `NanoMenuFileExplorer.cpp:348` keeps it out of the explorer. Opening RAR there later replaces that check with a back-end `supports(Format)` query, so Android still refuses it.
- **The theme-install `unzip`** (`NanoMenuEsdeMenu.cpp:972`) belongs to step 4's `Archive` interface. On Linux it calls `nanoarc::extract` into the staging folder, so Linux needs no second zip library. That supersedes "libzip or minizip" in `audit-2026-11-26/design.md:254`.
- **PDF: a `nanoplat` interface.** `NanoMenuPdf.cpp` is a NanoMenu file that the host tests do not compile *(check against the include census before the edit)*. The interface:
  - `open(path)` returns a page count, or an error: cannot open, password, no pages, unsupported.
  - `render(page, w, h)` returns an RGBA buffer with its size. The Linux implementation swaps `es-pdf-convert`'s BGRA bytes to RGBA (section 5.1).
  - The default implementation returns `Unsupported` and logs once.
  - Firmware: the `FPDF_*` block of `NanoMenuPdf.cpp:70-120` and its mutex, moved verbatim.
  - Linux: the helper, started through the sanctioned platform spawn as the `nano` user (OA-27).
  - Host: a fake that draws a numbered test page.
  - The worker thread, request queue, textures, zoom and input stay shared. The `.pdf` check at :57-62 becomes a `supports(path)` call.
- **Comics: no `nanoplat` interface.** The viewer gets a second page source beside the PDF interface: the page count is the number of image entries, and a page is `nanoarc`'s entry read plus `AImageDecoder`. Both are already portable, so it is shared code that asks `has(Cap::ManualComic)`.
- **OTA.** `features.cmake` gains `NANO_WITH_OTA` (`NanoOtaCheck.cpp`), and `NANO_WITH_ARCHIVE` keeps only `NanoArchive.cpp`. Linux passes `-DNANO_WITH_OTA=OFF`, and CMake refuses it ON for `NANO_PLATFORM=linux` until Thimsy has an update channel. `test_nano_cmake.py` and the include census follow.

## 9. Recommendation

1. **Archives: libarchive (BSD) on Linux**, with zlib and xz's liblzma, as a Linux `Source` back end of the unchanged `nanoarc` API. It also replaces the theme-install `unzip`. Leave RAR refused in the File Explorer until the owner chooses otherwise. Guard PPMd and very large dictionaries before the alpha.
2. **PDF manuals: Poppler out of process**, built with libjpeg and OpenJPEG. First ES-DE's `es-pdf-convert`, built from ES-DE's source by an MIT Thimsy recipe, behind a new `nanoplat` PDF interface with PDFium kept verbatim on Android. Replace it with a persistent GPL helper under `linux/gpl/` only if measured page turns are too slow. Keep PDFium for Linux as a later spike, for parity, if a build route appears.
3. **Comic-archive manuals in the alpha**, in the shared viewer over `nanoarc` and Nano's image decoder: `.cbz`, `.cb7` and `.cbr` on Linux. No new library.
4. **Not chosen:**
   - MuPDF or Poppler in-process (they change Nano's licence).
   - minizip-ng, libzip or libziparchive plus the LZMA SDK (two libraries, no RAR, the 512 MiB block buffer).
   - unrar (non-free terms).
   - p7zip (a process per action, and it duplicates libarchive).

## 10. Staged plan

Legend: **AS** = an Android-surface commit (cherry-picked after the AOSP build), **LO** = Linux or tooling only. Nothing below may be built in this wave.

| Stage | Work | Kind | Done when |
|---|---|---|---|
| 0 | Split the CMake switch: `NANO_WITH_OTA` gets `NanoOtaCheck.cpp`, refused ON on Linux; `test_nano_cmake.py` and the census updated | LO (CMake and Python) | Python tests pass; OTA still stubbed on Linux |
| 1 | Select `BR2_PACKAGE_LIBARCHIVE` and `BR2_PACKAGE_XZ` from `thimsy-nano`'s Config.in; split `NanoArchive.cpp` at `Source` (verbatim move); `nanoarc` gains a capped read of one entry into memory, for comic pages; add `platform/linux/ArchiveLibarchive.cpp`; `NANO_WITH_ARCHIVE=ON` for Linux; the archive host test over both back ends, plus hostile zip and 7z fixtures for libarchive (links, `..`, size mismatch, a PPMd and a large-dictionary 7z) | AS (the move and the entry read), LO (the rest) | Host archive test green on both back ends (G10, heavy), browse and extract on the ARC-D with `mem=1G` and RSS measured |
| 2 | The `nanoplat` PDF interface; firmware gets the PDFium block verbatim; host fake; `NanoMenuPdf.cpp` calls the interface | AS | Host E2E opens a fake manual; the Android build and smoke test after the AOSP job |
| 3 | Linux PDF: Buildroot `poppler` with `BR2_PACKAGE_JPEG` and `BR2_PACKAGE_OPENJPEG` selected (JPEG and JPEG 2000 scans); its cpp frontend is built by upstream's `ENABLE_CPP` default, with no Buildroot switch; nothing that turns on its cairo, glib, Qt, NSS, curl or TIFF parts (each follows whether that package is in the image); a `thimsy-es-pdf-convert` recipe built from ES-DE's `es-pdf-converter/` at a pinned tag, a fallback font in fontconfig's path, the Linux renderer through the platform spawn, with the BGRA-to-RGBA swap, `NANO_WITH_PDF=ON` for Linux; fixture PDFs: text with non-embedded fonts, a JPEG scan, a JPEG 2000 scan, password, damaged | LO | Scans render with the right colours; page-turn time, helper peak RSS and image-size delta measured on the ARC-D at `mem=1G` |
| 4 | Only if stage 3's page turns are slow: `linux/gpl/src/thimsy-pdf-render`, a persistent helper with the same output format, `GPL-2.0-or-later` unless the owner picks otherwise (Q4) | LO | Page turns measured again |
| 5 | Comic-archive manuals (needs stages 1 and 2; can run beside 3 and 4): the viewer's comic page source, the information page's filter through `has(Cap::ManualComic)`; fixtures: a `.cbz`, a solid `.cb7`, a RAR 4 and a RAR 5 `.cbr`, a `.cbr` that is really a zip, mixed page names, an oversized page, links and `..` entries | AS (shared viewer), LO (Linux reports the cap) | Host E2E opens each fixture; on the ARC-D at `mem=1G`, page-turn time and RSS measured, backward turns in the solid `.cb7` included |
| 6 | `Cap::ArchiveBrowse`, `Cap::ManualPdf`, `Cap::ManualComic` and `Cap::OtaUpdate` in step 3's `Cap`; menus hide unsupported entries | AS | Ratchet and host tests |
| 7 (owner) | RAR in the Linux File Explorer, comic manuals on Android, a PDFium build spike | AS and LO | Owner's answers to section 11 |

## 11. Questions for the owner

1. **RAR in the Linux File Explorer.** libarchive reads it, but Android refuses it, so the two platforms would differ in what the explorer opens. Recommended: leave it off for the alpha.
2. **Comic-archive manuals on Android.** OA-16 puts them in the Linux alpha (section 6). The same shared code reads `.cbz` and `.cb7` on Android; `.cbr` stays refused there. Turn on `Cap::ManualComic` on Android in the same release? Recommended: yes, so a card's comic manuals open on both.
3. **PDFium for Linux later.** A Chromium-toolchain recipe, a third-party prebuilt such as `pdfium-binaries` [S31], or never? Recommended: a time-boxed spike after the alpha; no prebuilt.
4. **The licence of `thimsy-pdf-render`** (stage 4, only if it is needed). Poppler's GPL-2-only and GPL-3-only code allows GPL-2.0 or GPL-3.0 for the linked program (section 5.1). Recommended: `GPL-2.0-or-later` for the helper's own files.

## Sources

Repository files are cited inline by path and line. Web sources (read 2026-11-26, and [S41], [S45] and [S46] again on 2026-11-27). Buildroot facts are from `master`. The fork pins Buildroot 2026.08.2 (`linux/build.sh:56`); its `libarchive.mk`, `poppler.mk` and `mupdf.mk` were checked on 2026-11-27 and have the same versions: libarchive 3.8.10, poppler 25.10.0 and mupdf 1.23.9.

- [S1] ES-DE `es-app/src/FileData.cpp`, `getManualPath`: https://gitlab.com/es-de/emulationstation-de/-/raw/master/es-app/src/FileData.cpp
- [S2] ES-DE CHANGELOG, version 2.1.0: https://gitlab.com/es-de/emulationstation-de/-/raw/master/CHANGELOG.md
- [S3] ES-DE `es-pdf-converter/src/ConvertPDF.cpp`: https://gitlab.com/es-de/emulationstation-de/-/raw/master/es-pdf-converter/src/ConvertPDF.cpp
- [S4] ES-DE `es-pdf-converter/CMakeLists.txt`: https://gitlab.com/es-de/emulationstation-de/-/raw/master/es-pdf-converter/CMakeLists.txt
- [S5] Apache Software Foundation, GPL compatibility: https://www.apache.org/licenses/GPL-compatibility.html
- [S6] GPL-3.0 text, section 13: https://opensource.org/license/gpl-3-0
- [S7] FSF GPL FAQ, MereAggregation: https://www.gnu.org/licenses/gpl-faq.html#MereAggregation (fetch refused with HTTP 429; wording taken from quotations of it, for example https://epics.anl.gov/tech-talk/2014/msg01701.php)
- [S8] Pine64 SOQuartz (RK3566) specification: https://pine64.org/documentation/SOQuartz/Further_information/Specifications
- [S9] libarchive README: https://raw.githubusercontent.com/libarchive/libarchive/master/README.md
- [S10] libarchive COPYING: https://raw.githubusercontent.com/libarchive/libarchive/master/COPYING
- [S11] Buildroot `package/libarchive/libarchive.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/libarchive/libarchive.mk
- [S12] libarchive `archive_read_format.3`: https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_read_format.3
- [S13] libarchive `archive_write_set_options.3`: https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_write_set_options.3
- [S14] minizip-ng README: https://raw.githubusercontent.com/zlib-ng/minizip-ng/develop/README.md
- [S15] LZMA SDK: https://www.7-zip.org/sdk.html
- [S16] Buildroot `package/minizip/minizip.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/minizip/minizip.mk
- [S17] Buildroot `package/pdfium/pdfium.mk`, `package/gn/gn.mk` and `package/lzma-sdk/lzma-sdk.mk`: each HTTP 404 on `master` (no such package)
- [S18] Buildroot `package/libzip/libzip.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/libzip/libzip.mk
- [S19] AOSP `libziparchive` `zip_archive.h`: https://android.googlesource.com/platform/system/libziparchive/+/refs/heads/main/include/ziparchive/zip_archive.h
- [S20] Ubuntu `android-libziparchive` (noble, arm64): https://packages.ubuntu.com/noble/arm64/android-libziparchive
- [S21] Ubuntu source `android-platform-tools`: https://packages.ubuntu.com/source/noble/android-platform-tools
- [S22] Buildroot `package/p7zip/p7zip.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/p7zip/p7zip.mk
- [S23] unRAR `license.txt` (mirror of RARLAB's unrarsrc): https://raw.githubusercontent.com/pmachapman/unrar/master/license.txt
- [S24] Buildroot `package/unrar/unrar.mk` and `Config.in`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/unrar/unrar.mk
- [S25] libarchive `archive_read_support_format_7zip.c`: https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_read_support_format_7zip.c
- [S26] libarchive `archive_read_support_format_rar5.c`: https://raw.githubusercontent.com/libarchive/libarchive/master/libarchive/archive_read_support_format_rar5.c
- [S27] Ubuntu `libarchive13t64` (noble, arm64): https://packages.ubuntu.com/noble/arm64/libarchive13t64
- [S28] XZ Utils COPYING: https://raw.githubusercontent.com/tukaani-project/xz/master/COPYING; Buildroot `package/xz/xz.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/xz/xz.mk
- [S29] PDFium LICENSE: https://pdfium.googlesource.com/pdfium/+/refs/heads/main/LICENSE
- [S30] PDFium README: https://pdfium.googlesource.com/pdfium/+/refs/heads/main/README.md
- [S31] pdfium-binaries: https://github.com/bblanchon/pdfium-binaries
- [S32] MuPDF README: https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/README
- [S33] MuPDF licence page: https://mupdf.readthedocs.io/en/1.27.0/license.html
- [S34] Buildroot `package/mupdf/mupdf.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/mupdf/mupdf.mk
- [S35] MuPDF `include/mupdf/fitz/context.h`: https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/include/mupdf/fitz/context.h
- [S36] MuPDF context documentation: https://mupdf.readthedocs.io/en/latest/reference/c/fitz/context.html
- [S37] Ubuntu `mupdf-tools` (noble, arm64): https://packages.ubuntu.com/noble/arm64/mupdf-tools
- [S38] MuPDF `include/mupdf/fitz/config.h`: https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/include/mupdf/fitz/config.h
- [S39] Poppler README (tag poppler-24.03.0, Skia mirror): https://skia.googlesource.com/third_party/poppler/+show/refs/tags/poppler-24.03.0/README.md
- [S40] Poppler bug 103547, "License is unclear": https://bugs.freedesktop.org/show_bug.cgi?id=103547; Debian's xpdf copyright: https://tracker.debian.org/media/packages/x/xpdf/copyright-3.04git20210103-3
- [S41] Buildroot `package/poppler/poppler.mk`: https://gitlab.com/buildroot.org/buildroot/-/raw/master/package/poppler/poppler.mk; at the pinned tag (JPEG and OpenJPEG conditions, licence `GPL-2.0+`): https://gitlab.com/buildroot.org/buildroot/-/raw/2026.08.2/package/poppler/poppler.mk
- [S42] Ubuntu `libpoppler134` (noble, arm64): https://packages.ubuntu.com/noble/arm64/libpoppler134
- [S43] MuPDF `source/cbz/mucbz.c`: https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/source/cbz/mucbz.c
- [S44] MuPDF `source/fitz/unlibarchive.c` and `Makethird`: https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/source/fitz/unlibarchive.c, https://raw.githubusercontent.com/ArtifexSoftware/mupdf/master/Makethird
- [S45] Poppler `CMakeLists.txt` at tag poppler-25.10.0 (`option(ENABLE_CPP ... ON)`), Skia mirror: https://skia.googlesource.com/third_party/poppler/+show/refs/tags/poppler-25.10.0/CMakeLists.txt
- [S46] Poppler `cpp/poppler-image.cpp` at tag poppler-24.03.0 (`format_argb32` read as one 32-bit word per pixel in `save()`), Skia mirror: https://skia.googlesource.com/third_party/poppler/+show/refs/tags/poppler-24.03.0/cpp/poppler-image.cpp
