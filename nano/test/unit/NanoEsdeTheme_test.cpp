// NanoEsdeTheme: resolution of a small theme, the H6 limits (include and variable bombs), and path
// confinement. Every bomb has to be refused or defused in well under a second.
#include "NanoEsdeTheme.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>
#include <ziparchive/zip_archive.h>

#include "nano_test_util.h"

using nanoesde::Element;
using nanoesde::LoadStats;
using nanoesde::Theme;

namespace {

const std::map<std::string, std::string> kSnes = {
    {"system.theme", "snes"}, {"system.name", "snes"}, {"system.fullName", "Super Nintendo"}};

bool loadTheme(Theme& t, const std::string& dir, const std::string& variant = "default",
               float aspect = 1.7778f, double* seconds = nullptr) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = t.load(dir, "", variant, "", "automatic", "", aspect, kSnes);
    if (seconds) *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

void writeFile(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "w");
    ASSERT_NE(f, nullptr) << path;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

const char kCaps[] =
    "<themeCapabilities><variant name=\"default\"><selectable>true</selectable></variant>"
    "</themeCapabilities>";

std::string tempTheme(const char* tag) {
    const std::string dir = nanotest::makeTempDir(tag);
    writeFile(dir + "/capabilities.xml", kCaps);
    return dir;
}

void removeTree(const std::string& dir) {
    if (!dir.empty()) system(("rm -rf '" + dir + "'").c_str());
}

}  // namespace

TEST(EsdeTheme, ResolvesIncludesVariablesVariantsAndAspect) {
    const std::string dir = nanotest::fixture("esde/basic");
    Theme t;
    ASSERT_TRUE(loadTheme(t, dir)) << t.error();
    EXPECT_EQ(t.capabilities().themeName, "Basic");
    const Element* title = t.element("system", "text", "title");
    ASSERT_NE(title, nullptr);
    EXPECT_EQ(title->getS("text", ""), "Hello");   // defined in the included file
    EXPECT_EQ(title->get("color")->rgba, 0xff8800ffu);
    const Element* logo = t.element("system", "image", "logo");
    ASSERT_NE(logo, nullptr);
    // ${system.*} stays raw in paths: the renderer fills it in per system.
    EXPECT_EQ(logo->getPath("path"), dir + "/logos/${system.theme}.svg");
    EXPECT_FLOAT_EQ(logo->getPair("pos", 1, 0), 0.25f);
    // A ./../ variable resolves against the file that defines it.
    const Element* bg = t.element("gamelist", "image", "bg");
    ASSERT_NE(bg, nullptr);
    EXPECT_EQ(bg->getPath("path"), dir + "/art/bg.png");
    EXPECT_EQ(t.stats().files, 2u);

    Theme alt;
    ASSERT_TRUE(loadTheme(alt, dir, "alt", 1.3333f));
    EXPECT_EQ(alt.element("system", "text", "title")->getS("text", ""), "Alt Hello");
    EXPECT_FLOAT_EQ(alt.element("system", "image", "logo")->getPair("pos", 1, 0), 0.3f);
}

TEST(EsdeTheme, SelfIncludeIsRefusedAndFast) {
    Theme t;
    double s = 0;
    ASSERT_TRUE(loadTheme(t, nanotest::fixture("esde/incbomb"), "default", 1.7778f, &s)) << t.error();
    EXPECT_LT(s, 1.0);
    EXPECT_EQ(t.stats().files, 3u);   // the theme, then each self-include read once and refused
    EXPECT_NE(t.element("system", "image", "bg"), nullptr);
}

TEST(EsdeTheme, IncludeFanOutStopsAtTheFileLimit) {
    // Each file includes the next twice: 2^20 loads without a cycle.
    const std::string dir = tempTheme("fanbomb");
    writeFile(dir + "/theme.xml", "<theme><include>./f1.xml</include><include>./f1.xml</include></theme>");
    for (int i = 1; i < 20; i++) {
        const std::string next = "./f" + std::to_string(i + 1) + ".xml";
        writeFile(dir + "/f" + std::to_string(i) + ".xml",
                  "<theme><include>" + next + "</include><include>" + next + "</include></theme>");
    }
    writeFile(dir + "/f20.xml", "<theme/>");
    Theme t;
    double s = 0;
    EXPECT_FALSE(loadTheme(t, dir, "default", 1.7778f, &s));
    EXPECT_LT(s, 1.0);
    EXPECT_NE(t.error().find("512"), std::string::npos) << t.error();
    EXPECT_LE(t.stats().files, nanoesde::kMaxThemeFiles);
    removeTree(dir);
}

TEST(EsdeTheme, IncludesStopAtTheByteLimit) {
    const std::string dir = tempTheme("bytebomb");
    std::string big = "<theme><!--" + std::string(1 << 20, 'x') + "--></theme>";
    writeFile(dir + "/big.xml", big);
    std::string theme = "<theme>";
    for (int i = 0; i < 20; i++) theme += "<include>./big.xml</include>";
    writeFile(dir + "/theme.xml", theme + "</theme>");
    Theme t;
    EXPECT_FALSE(loadTheme(t, dir));
    EXPECT_NE(t.error().find("16 MB"), std::string::npos) << t.error();
    EXPECT_LE(t.stats().bytes, nanoesde::kMaxThemeBytes);
    removeTree(dir);
}

TEST(EsdeTheme, DoublingVariablesAreRefused) {
    Theme t;
    double s = 0;
    EXPECT_FALSE(loadTheme(t, nanotest::fixture("esde/varbomb"), "default", 1.7778f, &s));
    EXPECT_LT(s, 1.0);
    EXPECT_NE(t.error().find("64 KB"), std::string::npos) << t.error();
}

TEST(EsdeTheme, TotalExpansionIsCapped) {
    // Each value stays under 64 KB, but 400 uses of a 60 KB variable is 24 MB.
    const std::string dir = tempTheme("expand");
    std::string theme = "<theme><variables><v>" + std::string(60 * 1024, 'A') + "</v></variables>"
                        "<view name=\"system\">";
    for (int i = 0; i < 400; i++) theme += "<text name=\"t" + std::to_string(i) + "\"><text>${v}</text></text>";
    writeFile(dir + "/theme.xml", theme + "</view></theme>");
    Theme t;
    double s = 0;
    EXPECT_FALSE(loadTheme(t, dir, "default", 1.7778f, &s));
    EXPECT_LT(s, 2.0);
    EXPECT_NE(t.error().find("substitution"), std::string::npos) << t.error();
    removeTree(dir);
}

TEST(EsdeTheme, PathsOutsideTheThemeFolderAreDropped) {
    const std::string dir = nanotest::fixture("esde/escape");
    Theme t;
    ASSERT_TRUE(loadTheme(t, dir)) << t.error();
    EXPECT_EQ(t.stats().files, 1u);   // neither /etc include was read
    const Element* bg = t.element("system", "image", "bg");
    ASSERT_NE(bg, nullptr);
    EXPECT_FALSE(bg->has("path"));
    const Element* text = t.element("system", "text", "t");
    ASSERT_NE(text, nullptr);
    EXPECT_FALSE(text->has("fontPath"));
}

TEST(EsdeTheme, SymlinkedIncludeLeavingTheFolderIsIgnored) {
    const std::string outside = nanotest::makeTempDir("outside");
    writeFile(outside + "/evil.xml", "<theme><view name=\"system\"><text name=\"x\"><text>evil</text></text></view></theme>");
    const std::string dir = tempTheme("symlink");
    ASSERT_EQ(symlink((outside + "/evil.xml").c_str(), (dir + "/inc.xml").c_str()), 0);
    writeFile(dir + "/theme.xml", "<theme><include>./inc.xml</include></theme>");
    Theme t;
    ASSERT_TRUE(loadTheme(t, dir)) << t.error();
    EXPECT_EQ(t.element("system", "text", "x"), nullptr);
    removeTree(dir);
    removeTree(outside);
}

TEST(EsdeTheme, CancelStopsTheLoad) {
    std::atomic<bool> cancel{true};
    Theme t;
    t.setCancel(&cancel);
    EXPECT_FALSE(loadTheme(t, nanotest::fixture("esde/basic")));
    EXPECT_EQ(t.error(), "cancelled");
}

// ---- real themes: the limits and confinement must not change how they parse --------------------

namespace {

struct ParseHash {
    size_t loads = 0, fails = 0, elements = 0;
    uint64_t hash = 1469598103934665603ull;
    void mix(const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) { hash ^= static_cast<const unsigned char*>(p)[i]; hash *= 1099511628211ull; }
    }
    // Paths are hashed relative to the theme folder, so the hash does not depend on where it is.
    void mixs(std::string s, const std::string& root) {
        if (s.compare(0, root.size(), root) == 0) s = "<root>" + s.substr(root.size());
        mix(s.data(), s.size() + 1);
    }
};

// Every variant x colour scheme x aspect x system, as test/README.md describes; the expected
// values were recorded with the parser before the H6 limits were added.
ParseHash hashThemeSet(const std::string& dir) {
    ParseHash h;
    const nanoesde::Capabilities caps = Theme::parseCapabilities(dir);
    std::vector<std::string> vars, schemes;
    for (const auto& v : caps.variants) vars.push_back(v.name);
    for (const auto& c : caps.colorSchemes) schemes.push_back(c.name);
    if (vars.empty()) vars.push_back("");
    if (schemes.empty()) schemes.push_back("");
    for (const auto& v : vars)
        for (const auto& c : schemes)
            for (float a : {1.7778f, 1.3333f})
                for (const char* sys : {"snes", "n64", "psx", "nds", "gba", "default"}) {
                    Theme t;
                    const std::map<std::string, std::string> sv{
                        {"system.theme", sys}, {"system.name", sys}, {"system.fullName", sys}};
                    const bool ok = t.load(dir, "", v, c, "automatic", "", a, sv);
                    h.loads++;
                    h.mix(&ok, 1);
                    if (!ok) { h.fails++; h.mixs(t.error(), dir); continue; }
                    for (const char* vn : {"system", "gamelist"}) {
                        const nanoesde::View* w = t.view(vn);
                        if (!w) continue;
                        for (const Element* e : w->drawOrder) {
                            h.elements++;
                            h.mixs(e->type, dir);
                            h.mixs(e->name, dir);
                            for (const auto& p : e->props) {
                                h.mixs(p.first, dir);
                                const int type = p.second.type;
                                h.mix(&type, sizeof(type));
                                h.mix(p.second.v, sizeof(p.second.v));
                                h.mixs(p.second.s, dir);
                                h.mix(&p.second.u, sizeof(p.second.u));
                                h.mix(&p.second.f, sizeof(p.second.f));
                                h.mix(&p.second.b, sizeof(p.second.b));
                                h.mix(&p.second.rgba, sizeof(p.second.rgba));
                            }
                        }
                    }
                }
    return h;
}

// Unzip into dest; returns the folder holding capabilities.xml ("" on failure).
std::string unzipTheme(const std::string& zip, const std::string& dest) {
    ZipArchiveHandle za = nullptr;
    if (OpenArchive(zip.c_str(), &za) != 0) { CloseArchive(za); return ""; }
    void* cookie = nullptr;
    std::string root;
    if (StartIteration(za, &cookie) == 0) {
        ZipEntry64 e;
        std::string name;
        while (Next(cookie, &e, &name) == 0) {
            if (name.find("..") != std::string::npos) continue;
            const std::string out = dest + "/" + name;
            for (size_t sl = out.find('/', dest.size() + 1); sl != std::string::npos; sl = out.find('/', sl + 1))
                mkdir(out.substr(0, sl).c_str(), 0755);
            if (!name.empty() && name.back() == '/') continue;
            const int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) continue;
            ExtractEntryToFile(za, &e, fd);
            close(fd);
            const size_t cap = name.rfind("capabilities.xml");
            if (cap != std::string::npos && cap + 16 == name.size() &&
                (root.empty() || name.size() < root.size() + 17))
                root = dest + (cap ? "/" + name.substr(0, cap - 1) : "");
        }
        EndIteration(cookie);
    }
    CloseArchive(za);
    return root;
}

struct RealTheme {
    const char* label;
    std::string zip;
    size_t loads, elements;
    uint64_t hash;
};

void checkRealTheme(const RealTheme& rt) {
    if (access(rt.zip.c_str(), R_OK) != 0) GTEST_SKIP() << rt.zip << " is not on this host";
    const std::string tmp = nanotest::makeTempDir("realtheme");
    const std::string dir = unzipTheme(rt.zip, tmp);
    ASSERT_FALSE(dir.empty()) << rt.zip;
    const ParseHash h = hashThemeSet(dir);
    EXPECT_EQ(h.fails, 0u);
    EXPECT_EQ(h.loads, rt.loads);
    EXPECT_EQ(h.elements, rt.elements);
    EXPECT_EQ(h.hash, rt.hash) << rt.label << ": the parse result changed";
    removeTree(tmp);
}

}  // namespace

TEST(EsdeRealThemes, BundledSlateParsesUnchanged) {
    checkRealTheme({"slate", nanotest::fixtureDir() + "/../../nano_esde_themes/slate-es-de.zip", 72, 5184,
                    0xc585d7820e29e9d7ull});
}

// Colorful ships in the thimsy build repo, not this tree: checked when the build cache is present
// (or NANO_TEST_COLORFUL_ZIP names the zip).
TEST(EsdeRealThemes, ColorfulParsesUnchanged) {
    std::string zip;
    if (const char* env = getenv("NANO_TEST_COLORFUL_ZIP")) zip = env;
    else if (const char* home = getenv("HOME"))
        zip = std::string(home) +
              "/thimsy/work/cache/nano/colorful-simplified-es-de-669e7f6a2de57c5ab69fd865e70d1cd357dbabbb.zip";
    checkRealTheme({"colorful", zip, 864, 43968, 0x7dc094d5f6c78b3full});
}
