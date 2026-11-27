// Game manuals (NanoManual): which file is a manual and how it is read, and the order and choice of a
// comic book manual's pages (OA-42).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "NanoManual.h"

using nanoarc::Entry;
using nanoarc::Err;
using nanoarc::Format;
using nanomanual::Kind;

namespace {

Entry file(const std::string& path, uint64_t size = 100) {
    Entry e;
    e.path = path;
    e.size = size;
    return e;
}

Entry linkEntry(const std::string& path) {
    Entry e = file(path);
    e.isLink = true;
    return e;
}

Entry folder(const std::string& path) {
    Entry e;
    e.path = path;
    e.isDir = true;
    return e;
}

}  // namespace

TEST(Manual, ExtensionsPdfFirstThenComics) {
    std::vector<std::string> exts;
    for (const char* const* e = nanomanual::kManualExts; *e; e++) exts.push_back(*e);
    EXPECT_EQ(exts, (std::vector<std::string>{".pdf", ".cbz", ".cb7", ".cbr"}));
}

TEST(Manual, KindByExtension) {
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.pdf"), Kind::Pdf);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.PDF"), Kind::Pdf);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.cbz"), Kind::Comic);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.CB7"), Kind::Comic);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.cbr"), Kind::Comic);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.zip"), Kind::None);
    EXPECT_EQ(nanomanual::kindOf("/m/manuals/Game.txt"), Kind::None);
    EXPECT_EQ(nanomanual::kindOf(".pdf"), Kind::None);   // a name, not only an extension
    EXPECT_EQ(nanomanual::kindOf(""), Kind::None);
}

TEST(Manual, ComicFormatByExtension) {
    EXPECT_EQ(nanomanual::comicFormat("a.cbz"), Format::Zip);
    EXPECT_EQ(nanomanual::comicFormat("a.CBZ"), Format::Zip);
    EXPECT_EQ(nanomanual::comicFormat("a.cb7"), Format::SevenZip);
    EXPECT_EQ(nanomanual::comicFormat("a.cbr"), Format::Rar);
    EXPECT_EQ(nanomanual::comicFormat("a.pdf"), Format::None);
    EXPECT_EQ(nanomanual::comicFormat("a.zip"), Format::None);   // only a manual's names
}

TEST(Manual, UnsupportedFormatIsRarOrAnUndecodableFileList) {
    EXPECT_TRUE(nanomanual::unsupportedFormat(Err::Rar));
    EXPECT_TRUE(nanomanual::unsupportedFormat(Err::Unsupported));
    EXPECT_FALSE(nanomanual::unsupportedFormat(Err::None));
    EXPECT_FALSE(nanomanual::unsupportedFormat(Err::Corrupt));
    EXPECT_FALSE(nanomanual::unsupportedFormat(Err::Open));
}

TEST(Manual, PageNames) {
    EXPECT_TRUE(nanomanual::isPageName("p1.jpg"));
    EXPECT_TRUE(nanomanual::isPageName("Scans/P1.JPEG"));
    EXPECT_TRUE(nanomanual::isPageName("p1.png"));
    EXPECT_TRUE(nanomanual::isPageName("p1.webp"));
    EXPECT_TRUE(nanomanual::isPageName("p1.gif"));
    EXPECT_FALSE(nanomanual::isPageName("ComicInfo.xml"));
    EXPECT_FALSE(nanomanual::isPageName("notes.txt"));
    EXPECT_FALSE(nanomanual::isPageName("p1.heic"));
    EXPECT_FALSE(nanomanual::isPageName("._p1.jpg"));           // macOS's resource fork
    EXPECT_FALSE(nanomanual::isPageName("scans/.thumb.jpg"));   // hidden
    EXPECT_FALSE(nanomanual::isPageName("__MACOSX/p1.jpg"));
    EXPECT_FALSE(nanomanual::isPageName("scans/__MACOSX/p1.jpg"));
    EXPECT_FALSE(nanomanual::isPageName(".jpg"));
    EXPECT_FALSE(nanomanual::isPageName("scans/"));
}

TEST(Manual, NaturalOrderNumbersAsNumbers) {
    EXPECT_TRUE(nanomanual::naturalLess("p2.jpg", "p10.jpg"));
    EXPECT_FALSE(nanomanual::naturalLess("p10.jpg", "p2.jpg"));
    EXPECT_TRUE(nanomanual::naturalLess("p9", "p10"));
    EXPECT_TRUE(nanomanual::naturalLess("page 1", "page 1a"));
    // Leading zeros do not change the number; equal names fall back to plain order, so the order
    // is still strict.
    EXPECT_TRUE(nanomanual::naturalLess("p007", "p8"));
    EXPECT_TRUE(nanomanual::naturalLess("p01", "p1"));
    EXPECT_FALSE(nanomanual::naturalLess("p1", "p01"));
    EXPECT_FALSE(nanomanual::naturalLess("p1", "p1"));
    // Numbers longer than any integer type still compare by value.
    EXPECT_TRUE(nanomanual::naturalLess("p99999999999999999999", "p100000000000000000000"));
}

TEST(Manual, NaturalOrderIgnoresCaseAndKeepsFoldersTogether) {
    EXPECT_TRUE(nanomanual::naturalLess("a.jpg", "B.jpg"));
    EXPECT_TRUE(nanomanual::naturalLess("B.jpg", "c.jpg"));
    // A folder's pages before a longer name that shares its start.
    EXPECT_TRUE(nanomanual::naturalLess("ch1/p9.jpg", "ch1 extra.jpg"));
    EXPECT_TRUE(nanomanual::naturalLess("ch2/p10.jpg", "ch10/p1.jpg"));
}

TEST(Manual, ComicPagesInNaturalOrderImagesOnly) {
    const std::vector<Entry> entries = {
        file("p10.jpg"), file("p2.jpg"), file("P1.png"), file("ComicInfo.xml"), folder("extra"),
        file("._p3.jpg"), file("__MACOSX/p3.jpg"), file("p3.webp"), file("empty.jpg", 0),
        file("p2.jpg"),   // a second entry of the same name is one page
        linkEntry("p4.jpg"),
    };
    EXPECT_EQ(nanomanual::comicPages(entries),
              (std::vector<std::string>{"P1.png", "p2.jpg", "p3.webp", "p10.jpg"}));
}

TEST(Manual, ComicPagesAcrossFolders) {
    const std::vector<Entry> entries = {
        file("Manual/ch10/001.jpg"), file("Manual/ch2/010.jpg"), file("Manual/ch2/002.jpg"),
        file("Manual/cover.jpg"), file("Manual/ch1/1.jpg"),
    };
    EXPECT_EQ(nanomanual::comicPages(entries),
              (std::vector<std::string>{"Manual/ch1/1.jpg", "Manual/ch2/002.jpg", "Manual/ch2/010.jpg",
                                        "Manual/ch10/001.jpg", "Manual/cover.jpg"}));
}

TEST(Manual, NoPages) {
    EXPECT_TRUE(nanomanual::comicPages({}).empty());
    EXPECT_TRUE(nanomanual::comicPages({file("readme.txt"), folder("scans")}).empty());
}

TEST(Manual, PageDecodable) {
    EXPECT_TRUE(nanomanual::pageDecodable(2000, 3000));
    EXPECT_TRUE(nanomanual::pageDecodable(4000, 6000));    // 24 Mpx, the cap
    EXPECT_FALSE(nanomanual::pageDecodable(4001, 6000));
    EXPECT_FALSE(nanomanual::pageDecodable(0, 100));
    EXPECT_FALSE(nanomanual::pageDecodable(100, -1));
    EXPECT_FALSE(nanomanual::pageDecodable(100000, 100000));   // no int overflow
}

TEST(Manual, PageSizeFitsTheBox) {
    int w = 0, h = 0;
    nanomanual::pageSize(2000, 3000, 0, 640, 450, &w, &h);   // tall: fits the height
    EXPECT_EQ(w, 300);
    EXPECT_EQ(h, 450);
    nanomanual::pageSize(3000, 1000, 0, 640, 450, &w, &h);   // wide: fits the width
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 213);
    nanomanual::pageSize(320, 480, 0, 640, 450, &w, &h);     // small: scaled up as a PDF page is
    EXPECT_EQ(w, 300);
    EXPECT_EQ(h, 450);
}

TEST(Manual, PageSizeZooms) {
    int w = 0, h = 0;
    nanomanual::pageSize(2000, 3000, 1, 640, 450, &w, &h);   // fit width
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 960);
    nanomanual::pageSize(2000, 3000, 2, 640, 450, &w, &h);   // twice the width
    EXPECT_EQ(w, 1280);
    EXPECT_EQ(h, 1920);
}

TEST(Manual, PageSizeNeverOverTheCap) {
    int w = 0, h = 0;
    nanomanual::pageSize(1000, 10000, 2, 1920, 1080, &w, &h);   // 3840 x 38400 wanted
    EXPECT_LE(w, nanomanual::kMaxPageDim);
    EXPECT_LE(h, nanomanual::kMaxPageDim);
    EXPECT_EQ(h, nanomanual::kMaxPageDim);
    EXPECT_GE(w, 1);
    nanomanual::pageSize(1, 1, 0, 0, 0, &w, &h);   // no box yet: still a pixel
    EXPECT_EQ(w, 1);
    EXPECT_EQ(h, 1);
}
