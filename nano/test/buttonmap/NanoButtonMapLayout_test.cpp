// The button map panel's layout at every screen size Thimsy meets (button_map_overlay_plan.md §9.2
// T5), and the navigation over it: the D-pad, the stick, L1/R1 pages and touch (§6.5). The scenes
// are the golden test's (test/buttonmap/scenes), laid out with the real font and art.

#include <gtest/gtest.h>

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "NanoButtonMap.h"
#include "NanoButtonMapArt.h"
#include "NanoButtonMapNav.h"

using namespace android;
using namespace android::bmap;
using namespace android::bmart;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string exeDir() {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return ".";
    exe[n] = '\0';
    std::string d(exe);
    return d.substr(0, d.find_last_of('/'));
}

// The Nano folder: the installed data beside the binary, NANO_SRC, or the source tree.
std::string nanoDir() {
    if (const char* e = getenv("NANO_SRC")) return e;
    if (access((exeDir() + "/ps3xmb/controllers/controllers.json").c_str(), R_OK) == 0) return exeDir();
    if (const char* top = getenv("ANDROID_BUILD_TOP")) return std::string(top) + "/frameworks/base/cmds/gammaos-nano";
    return ".";
}

std::string firstFile(const std::vector<std::string>& c) {
    for (const auto& p : c) if (!p.empty() && access(p.c_str(), R_OK) == 0) return p;
    return std::string();
}

std::string top() { return getenv("ANDROID_BUILD_TOP") ? getenv("ANDROID_BUILD_TOP") : nanoDir() + "/../../../.."; }

const Assets& assets() {
    static Catalog cat;
    static Assets a = [] {
        Assets x;
        EXPECT_TRUE(loadCatalogFrom(nanoDir() + "/ps3xmb/controllers", nanoDir() + "/ps3xmb/buttonmap", &cat));
        x.catalog = &cat;
        x.artDir = nanoDir() + "/ps3xmb/controllers";
        x.fontPath = firstFile({exeDir() + "/Roboto-Regular.ttf", top() + "/external/roboto-fonts/Roboto-Regular.ttf"});
        const std::string he = firstFile({exeDir() + "/notosanshebrew/NotoSansHebrew-Regular.ttf", exeDir() + "/NotoSansHebrew-Regular.ttf",
                                          top() + "/external/noto-fonts/notosanshebrew/NotoSansHebrew-Regular.ttf"});
        if (!he.empty()) x.fallbackFonts.push_back(he);
        return x;
    }();
    return a;
}

std::vector<std::pair<std::string, Scene>> scenes() {
    std::vector<std::pair<std::string, Scene>> out;
    const std::string dir = nanoDir() + "/test/buttonmap/scenes";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        Scene s;
        if (sceneFromJson(slurp(dir + "/" + n), &s)) out.emplace_back(n.substr(0, n.size() - 5), s);
    }
    return out;
}

struct Size { int w, h; };
const Size kSizes[] = {{640, 480}, {720, 720}, {800, 480}, {854, 480}, {960, 544}, {1024, 768},
                       {1280, 720}, {1280, 800}, {1920, 1080}, {1080, 1920}, {720, 1280}};

std::string dump(const Layout& L) {
    std::ostringstream o;
    o << "panel " << L.panel.x << "," << L.panel.y << " " << L.panel.w << "x" << L.panel.h << " body " << L.body.x << ","
      << L.body.y << " " << L.body.w << "x" << L.body.h << " px " << L.bodyPx << " content " << L.contentH << "\n";
    for (const auto& t : L.fixed) o << "  fixed " << t.r.x << "," << t.r.y << " " << t.r.w << "x" << t.r.h << " '" << t.text << "'\n";
    for (size_t i = 0; i < L.items.size(); i++) {
        const Item& it = L.items[i];
        o << "  item " << i << " y=" << it.y << " h=" << it.h << " s=" << it.section << (it.drawing ? " drawing" : "") << "\n";
        for (const auto& t : it.texts) o << "    text " << t.r.x << "," << t.r.y << " " << t.r.w << "x" << t.r.h << " w=" << t.textW << " '" << t.text << "'\n";
        for (const auto& c : it.chips) o << "    chip " << c.x << "," << c.y << " " << c.w << "x" << c.h << "\n";
    }
    return o.str();
}

bool segHits(const Leader& l, const Rect& r) {
    // Sampled: a leader that runs through a chip, not one that grazes its edge.
    for (int i = 1; i < 32; i++) {
        const float t = i / 32.0f, x = l.x0 + (l.x1 - l.x0) * t, y = l.y0 + (l.y1 - l.y0) * t;
        if (x > r.x + 1 && x < r.r() - 1 && y > r.y + 1 && y < r.b() - 1) return true;
    }
    return false;
}

}  // namespace

TEST(ButtonMapLayout, AssetsPresent) {
    ASSERT_FALSE(assets().fontPath.empty()) << "Roboto-Regular.ttf not found";
    ASSERT_GE(scenes().size(), 10u);
}

TEST(ButtonMapLayout, FontSizeFormula) {
    EXPECT_EQ(16, bodyPxFor(640, 480));
    EXPECT_EQ(18, bodyPxFor(960, 544));
    EXPECT_EQ(24, bodyPxFor(720, 720));
    EXPECT_EQ(24, bodyPxFor(1280, 720));
    EXPECT_EQ(26, bodyPxFor(1024, 768));
    EXPECT_EQ(36, bodyPxFor(1920, 1080));
    EXPECT_EQ(36, bodyPxFor(1080, 1920));
    EXPECT_EQ(14, bodyPxFor(320, 240));
    EXPECT_EQ(40, bodyPxFor(3840, 2160));
    EXPECT_EQ(14, bodyPxFor(640, 480, 0.5f));
}

TEST(ButtonMapLayout, RulesAtEverySize) {
    const auto all = scenes();
    for (const auto& sc : all) {
        int lastPx = 0, lastUnit = 0, lastW = 0;
        std::vector<Size> sizes(std::begin(kSizes), std::end(kSizes));
        std::sort(sizes.begin(), sizes.end(), [](Size a, Size b) { return std::min(a.w, a.h) < std::min(b.w, b.h); });
        for (const Size sz : sizes) {
            SCOPED_TRACE(sc.first + " " + std::to_string(sz.w) + "x" + std::to_string(sz.h));
            Layout L;
            ASSERT_TRUE(layout(sc.second, assets(), sz.w, sz.h, &L));
            const std::string d = dump(L);
            const int unit = std::min(sz.w, sz.h);
            const float aspect = (float)sz.w / sz.h;
            // 1. Centred on 16:9 and wider, full width otherwise.
            if (aspect >= 1.70f) {
                EXPECT_TRUE(L.centred) << d;
                EXPECT_LE(std::abs(L.panel.x + L.panel.w / 2 - sz.w / 2), 1) << d;
                EXPECT_LE(std::abs(L.panel.y + L.panel.h / 2 - sz.h / 2), 1) << d;
                EXPECT_LE(L.panel.w, (int)lroundf(sz.w * 0.80f)) << d;
            } else {
                const int m = std::max(4, (int)lroundf(unit * 0.02f));
                EXPECT_GE(L.panel.w, sz.w - 2 * (int)ceilf(0.02f * unit)) << d;
                EXPECT_LE(L.panel.h, sz.h - 2 * m) << d;
            }
            // 2. Inside the screen, everything inside the panel; items inside the body's columns.
            const Rect screen{0, 0, sz.w, sz.h};
            EXPECT_TRUE(L.panel.inside(screen)) << d;
            for (const auto& t : L.fixed) EXPECT_TRUE(t.r.inside(L.panel)) << t.text << "\n" << d;
            EXPECT_TRUE(L.body.inside(L.panel)) << d;
            for (const auto& it : L.items) {
                const Rect band{L.body.x, it.y, L.body.w, it.h};
                EXPECT_LE(it.h, L.body.h) << "an item taller than the view\n" << d;
                for (const auto& t : it.texts) EXPECT_TRUE(t.r.inside(band)) << t.text << "\n" << d;
                for (const auto& c : it.chips) EXPECT_TRUE(c.inside(band)) << d;
                for (const auto& l : it.lines) {
                    EXPECT_TRUE(l.x0 >= band.x - 1 && l.x0 <= band.r() + 1 && l.y0 >= band.y - 1 && l.y0 <= band.b() + 1) << d;
                    EXPECT_TRUE(l.x1 >= band.x - 1 && l.x1 <= band.r() + 1 && l.y1 >= band.y - 1 && l.y1 <= band.b() + 1) << d;
                }
                if (it.drawing) {
                    EXPECT_GE(it.drawBox.x, band.x - 1) << d;
                    EXPECT_LE(it.drawBox.r(), band.r() + 1) << d;
                }
            }
            // 3. No text narrower than its words.
            for (const auto& t : L.fixed) EXPECT_GE(t.r.w, t.textW) << t.text;
            for (const auto& it : L.items)
                for (const auto& t : it.texts) EXPECT_GE(t.r.w, t.textW) << t.text;
            // 4. Nothing overlaps; no leader runs through another chip.
            for (const auto& it : L.items) {
                std::vector<Rect> boxes;
                for (const auto& t : it.texts) if (t.role != Role::kChip) boxes.push_back(t.r);
                for (const auto& c : it.chips) boxes.push_back(c);
                for (size_t i = 0; i < boxes.size(); i++)
                    for (size_t j = i + 1; j < boxes.size(); j++) EXPECT_FALSE(boxes[i].intersects(boxes[j])) << i << " " << j << "\n" << d;
                for (size_t i = 0; i < it.lines.size(); i++)
                    for (size_t j = 0; j < it.chips.size(); j++)
                        if (i != j) EXPECT_FALSE(segHits(it.lines[i], it.chips[j])) << i << " " << j << "\n" << d;
            }
            for (size_t i = 0; i < L.fixed.size(); i++)
                for (size_t j = i + 1; j < L.fixed.size(); j++) EXPECT_FALSE(L.fixed[i].r.intersects(L.fixed[j].r)) << d;
            for (size_t i = 1; i < L.items.size(); i++) EXPECT_GE(L.items[i].y, L.items[i - 1].y + L.items[i - 1].h) << d;
            // 5. Legible at every size, never smaller on a bigger screen.
            EXPECT_GE(L.bodyPx, 14);
            EXPECT_GE(L.smallPx, 13);
            // (a square screen's panel can be narrower than a smaller wide screen's)
            if (unit > lastUnit && L.panel.w >= lastW) EXPECT_GE(L.bodyPx, lastPx) << d;
            lastPx = L.bodyPx;
            lastW = L.panel.w;
            lastUnit = unit;
            // 6. Every row and hotkey shown exactly once.
            if (!sc.second.rows.empty()) EXPECT_EQ((int)sc.second.rows.size(), L.rowsPlaced) << d;
            EXPECT_EQ((int)sc.second.hotkeys.size(), L.hotkeysPlaced) << d;
            // 7. At most two pages at 640x480.
            Nav nav;
            nav.setModel(L.nav());
            if (sz.w == 640 && sz.h == 480) EXPECT_LE(nav.pageCount(), 2) << d;
            // 8. Deterministic.
            Layout again;
            ASSERT_TRUE(layout(sc.second, assets(), sz.w, sz.h, &again));
            EXPECT_EQ(d, dump(again));
        }
    }
}

// The design targets of §7.2: panel sizes and body text.
TEST(ButtonMapLayout, PanelTargets) {
    Scene s;
    s.title = "SNES";
    s.rows.push_back({"btn-a", "A", "A", false});
    struct T { int w, h, pw, ph, px; bool centred; };
    const T kT[] = {{1280, 720, 972, 648, 24, true},  {1920, 1080, 1458, 972, 36, true}, {960, 544, 735, 490, 18, true},
                    {854, 480, 648, 432, 16, true},   {640, 480, 620, 460, 16, false},   {720, 720, 692, 692, 24, false},
                    {1024, 768, 994, 738, 26, false}, {800, 480, 780, 460, 16, false}};
    for (const auto& t : kT) {
        Layout L;
        ASSERT_TRUE(layout(s, assets(), t.w, t.h, &L));
        EXPECT_EQ(t.pw, L.panel.w) << t.w << "x" << t.h;
        EXPECT_EQ(t.ph, L.panel.h) << t.w << "x" << t.h;
        EXPECT_EQ(t.px, L.bodyPx) << t.w << "x" << t.h;
        EXPECT_EQ(t.centred, L.centred) << t.w << "x" << t.h;
    }
    Layout P;
    ASSERT_TRUE(layout(s, assets(), 1080, 1920, &P));
    EXPECT_LT(P.panel.h, 1920 - 44);   // portrait: as tall as the content
    EXPECT_EQ(1036, P.panel.w);
}

// ---- navigation over real layouts ----

TEST(ButtonMapNav, PagingAtEverySize) {
    for (const auto& sc : scenes()) {
        for (const Size sz : kSizes) {
            SCOPED_TRACE(sc.first + " " + std::to_string(sz.w) + "x" + std::to_string(sz.h));
            Layout L;
            ASSERT_TRUE(layout(sc.second, assets(), sz.w, sz.h, &L));
            Nav nav;
            nav.setModel(L.nav());
            const int pages = nav.pageCount();
            EXPECT_EQ(1, nav.page());
            EXPECT_FALSE(nav.pageUp());
            EXPECT_FALSE(nav.lineUp());
            int steps = 0;
            std::vector<bool> seen(L.items.size(), false);
            auto mark = [&] {
                for (size_t i = 0; i < L.items.size(); i++)
                    if (L.items[i].y >= nav.scroll() - 0.5f && L.items[i].y + L.items[i].h <= nav.scroll() + L.body.h + 0.5f) seen[i] = true;
            };
            mark();
            while (nav.pageDown()) {
                steps++;
                EXPECT_EQ(steps + 1, nav.page());
                mark();
                ASSERT_LE(steps, 50);
            }
            EXPECT_EQ(pages - 1, steps);
            EXPECT_EQ(pages, nav.page());
            EXPECT_FALSE(nav.canDown());
            EXPECT_FALSE(nav.pageDown());
            EXPECT_FALSE(nav.lineDown());   // no movement past the end
            for (size_t i = 0; i < seen.size(); i++) EXPECT_TRUE(seen[i]) << "item " << i << " never wholly on screen";
            int back = 0;
            while (nav.pageUp()) ASSERT_LE(++back, 50);
            EXPECT_EQ(pages - 1, back);
            EXPECT_EQ(0.0f, nav.scroll());
            // Line by line reaches the same end, one item at a time.
            int lines = 0;
            while (nav.lineDown()) ASSERT_LE(++lines, 500);
            EXPECT_EQ(nav.maxScroll(), nav.scroll());
            EXPECT_LE(lines, (int)L.items.size());
        }
    }
}

TEST(ButtonMapNav, SectionsWithTheDpad) {
    for (const auto& sc : scenes()) {
        if (sc.second.hotkeys.empty()) continue;
        Layout L;
        ASSERT_TRUE(layout(sc.second, assets(), 640, 480, &L));
        Nav nav;
        nav.setModel(L.nav());
        ASSERT_EQ(2u, L.sections.size()) << sc.first;
        EXPECT_EQ(0, nav.section());
        const bool moved = nav.nextSection();
        if (moved) EXPECT_EQ(std::min((float)L.items[L.sections[1]].y, nav.maxScroll()), nav.scroll()) << sc.first;
        EXPECT_FALSE(nav.nextSection() && nav.nextSection()) << sc.first;
        nav.prevSection();
        nav.prevSection();
        EXPECT_EQ(0.0f, nav.scroll()) << sc.first;
        EXPECT_FALSE(nav.prevSection());
    }
}

// ---- touch, on real layouts ----

TEST(ButtonMapNav, TapOutsideClosesTapInsideDoesNothing) {
    Scene s = scenes().front().second;
    for (const Size sz : {Size{640, 480}, Size{1920, 1080}}) {
        Layout L;
        ASSERT_TRUE(layout(s, assets(), sz.w, sz.h, &L));
        Nav nav;
        nav.setModel(L.nav());
        nav.touchDown(2, 2, 0);
        EXPECT_EQ(NavAction::kClose, nav.touchUp(50)) << sz.w;
        nav.touchDown(L.panel.x + L.panel.w / 2.0f, L.panel.y + 10.0f, 100);
        EXPECT_EQ(NavAction::kNone, nav.touchUp(150)) << sz.w;
        // The same taps from a touch panel at another resolution, mapped onto the layout.
        nav.setTouchSpace(sz.w / 2.0f, sz.h / 2.0f, sz.w, sz.h);
        nav.touchDown((L.panel.x + L.panel.w / 2.0f) / 2, (L.panel.y + 10.0f) / 2, 200);
        EXPECT_EQ(NavAction::kNone, nav.touchUp(250));
        nav.touchDown(0.5f, 0.5f, 300);
        EXPECT_EQ(NavAction::kClose, nav.touchUp(350));
    }
}

// ---- touch and keys on a synthetic body ----

namespace {
NavModel model(int items = 20, int h = 30, int view = 200) {
    NavModel m;
    for (int i = 0; i < items; i++) { m.itemY.push_back(i * h); m.itemH.push_back(h); }
    m.sections = {0, items / 2};
    m.viewH = view;
    m.panel = {20, 20, 600, 440};
    m.body = {40, 80, 560, (float)view};
    m.slop = 10;
    m.swipeMin = 38;
    return m;
}
}  // namespace

TEST(ButtonMapNav, ScrollBoundsAndPages) {
    Nav nav;
    nav.setModel(model());
    // 20 items of 30 in a 200 view: 6 whole items a page, the last page from item 18.
    EXPECT_EQ(4, nav.pageCount());
    EXPECT_EQ(540.0f, nav.maxScroll());
    EXPECT_TRUE(nav.lineDown());
    EXPECT_EQ(30.0f, nav.scroll());
    EXPECT_TRUE(nav.pageDown());
    EXPECT_EQ(210.0f, nav.scroll());   // item 7, the first not wholly shown
    nav.scrollTo(10000);
    EXPECT_EQ(540.0f, nav.scroll());
    EXPECT_EQ(4, nav.page());
    nav.scrollTo(-50);
    EXPECT_EQ(0.0f, nav.scroll());
    EXPECT_TRUE(nav.nextSection());
    EXPECT_EQ(300.0f, nav.scroll());
    EXPECT_TRUE(nav.prevSection());
    EXPECT_EQ(0.0f, nav.scroll());
    // Content that fits: one page, nothing moves.
    Nav small;
    small.setModel(model(3));
    EXPECT_EQ(1, small.pageCount());
    EXPECT_FALSE(small.lineDown());
    EXPECT_FALSE(small.pageDown());
    EXPECT_FALSE(small.nextSection() && small.scroll() > 0);
}

TEST(ButtonMapNav, DragFollowsTheFinger) {
    Nav nav;
    nav.setModel(model());
    nav.touchDown(300, 300, 0);
    nav.touchMove(300, 295, 10);    // inside the slop: nothing
    EXPECT_EQ(0.0f, nav.scroll());
    nav.touchMove(300, 285, 20);    // past it: the drag starts here
    nav.touchMove(300, 205, 1000);  // 80 px up, slowly
    EXPECT_EQ(80.0f, nav.scroll());
    EXPECT_TRUE(nav.dragging());
    nav.touchMove(300, 405, 2000);  // past the top: clamped
    EXPECT_EQ(0.0f, nav.scroll());
    EXPECT_EQ(NavAction::kNone, nav.touchUp(3000));
    EXPECT_FALSE(nav.flinging());   // held still: no flick
}

TEST(ButtonMapNav, FlingDecaysAndStopsAtTheEnds) {
    Nav nav;
    nav.setModel(model());
    nav.touchDown(300, 400, 0);
    nav.touchMove(300, 380, 16);
    nav.touchMove(300, 340, 32);
    nav.touchMove(300, 300, 48);    // 2.5 px/ms upward
    nav.touchUp(56);
    ASSERT_TRUE(nav.flinging());
    float last = nav.scroll(), lastStep = 1e9f;
    int64_t t = 56;
    while (nav.tick(t += 16)) {
        const float step = nav.scroll() - last;
        EXPECT_GE(step, -0.001f);             // one way
        EXPECT_LE(step, lastStep + 0.001f);   // slowing down
        lastStep = step;
        last = nav.scroll();
        ASSERT_LT(t, 20000);
    }
    EXPECT_FALSE(nav.flinging());
    EXPECT_LE(nav.scroll(), nav.maxScroll());
    // A hard flick stops at the end, exactly.
    nav.touchDown(300, 400, 30000);
    nav.touchMove(300, 300, 30010);
    nav.touchMove(300, 100, 30020);   // 20 px/ms
    nav.touchUp(30025);
    while (nav.tick(t += 16)) ASSERT_LT(t, 60000);
    EXPECT_EQ(nav.maxScroll(), nav.scroll());
    // A finger landing on a fling stops it and taps nothing (not even outside the panel).
    nav.scrollTo(0);
    nav.touchDown(300, 100, 70000);
    nav.touchMove(300, 300, 70010);
    nav.touchMove(300, 100, 70020);
    nav.touchUp(70025);
    ASSERT_TRUE(nav.flinging() || nav.scroll() == nav.maxScroll());
    nav.touchDown(5, 5, 70030);
    EXPECT_FALSE(nav.flinging());
    EXPECT_EQ(NavAction::kNone, nav.touchUp(70040));
}

TEST(ButtonMapNav, SwipeSwitchesSections) {
    Nav nav;
    nav.setModel(model());
    nav.touchDown(400, 300, 0);
    nav.touchMove(380, 302, 10);
    nav.touchMove(340, 303, 20);   // 60 px left
    EXPECT_EQ(NavAction::kNextSection, nav.touchUp(30));
    EXPECT_EQ(300.0f, nav.scroll());
    nav.touchDown(300, 300, 100);
    nav.touchMove(320, 300, 110);
    nav.touchMove(360, 301, 120);
    EXPECT_EQ(NavAction::kPrevSection, nav.touchUp(130));
    EXPECT_EQ(0.0f, nav.scroll());
    nav.touchDown(300, 300, 200);
    nav.touchMove(325, 300, 210);   // past the slop, short of a swipe
    EXPECT_EQ(NavAction::kNone, nav.touchUp(220));
}

TEST(ButtonMapNav, MultiFingerGesturesAreDropped) {
    Nav nav;
    nav.setModel(model());
    nav.pointers(1);
    nav.touchDown(5, 5, 0);
    nav.pointers(2);   // a second finger: Nano's two-finger tap, not the overlay's
    EXPECT_EQ(NavAction::kNone, nav.touchUp(20));
    nav.touchDown(5, 5, 30);
    EXPECT_EQ(NavAction::kNone, nav.touchUp(40));   // still multi until every finger is up
    nav.pointers(0);
    nav.touchDown(5, 5, 50);
    EXPECT_EQ(NavAction::kClose, nav.touchUp(60));
}

// ---- buttons and the stick ----

TEST(ButtonMapNavInput, ButtonsByPositionThroughThePadModel) {
    PadSpec s;
    std::string json = slurp(nanoDir() + "/test/fixtures/buttonmap/pads/rg_cube.json");
    ASSERT_TRUE(padSpecFromJson(json, &s));
    GamepadState off;
    const NavConfig c = navConfigFor(padModel(s, off));
    EXPECT_EQ(Phys::South, c.back);    // Nano's Back is BTN_EAST, the bottom button on the Cube
    EXPECT_EQ(Phys::West, c.toggle);
    GamepadState swap;
    swap.running = swap.swap = true;
    EXPECT_EQ(Phys::East, navConfigFor(padModel(s, swap)).back);
    NavInput in(c);
    EXPECT_EQ(NavAction::kClose, in.button(Phys::South, true, 0));
    EXPECT_EQ(NavAction::kClose, in.button(Phys::Start, true, 0));
    EXPECT_EQ(NavAction::kToggleView, in.button(Phys::West, true, 0));
    EXPECT_EQ(NavAction::kPageDown, in.button(Phys::R1, true, 0));
    in.button(Phys::R1, false, 10);
    EXPECT_EQ(NavAction::kPageUp, in.button(Phys::L1, true, 20));
    in.button(Phys::L1, false, 30);
    EXPECT_EQ(NavAction::kNextSection, in.button(Phys::DRight, true, 40));
    in.button(Phys::DRight, false, 50);
    EXPECT_EQ(NavAction::kNone, in.button(Phys::North, true, 60));
}

TEST(ButtonMapNavInput, RepeatOnHold) {
    NavInput in;
    EXPECT_EQ(NavAction::kLineDown, in.button(Phys::DDown, true, 0));
    EXPECT_EQ(NavAction::kNone, in.tick(349));
    EXPECT_EQ(NavAction::kLineDown, in.tick(350));
    EXPECT_EQ(NavAction::kNone, in.tick(400));
    EXPECT_EQ(NavAction::kLineDown, in.tick(440));
    EXPECT_EQ(NavAction::kLineDown, in.tick(2000));   // a late tick repeats once, not a burst
    EXPECT_EQ(NavAction::kNone, in.tick(2001));
    in.button(Phys::DDown, false, 2010);
    EXPECT_EQ(NavAction::kNone, in.tick(5000));
}

TEST(ButtonMapNavInput, StickDeadzoneEdgeAndRepeat) {
    NavInput in;
    EXPECT_EQ(NavAction::kNone, in.stick(0.0f, 0.3f, 0));    // inside the dead zone
    EXPECT_EQ(NavAction::kNone, in.stick(0.2f, 0.49f, 10));
    EXPECT_EQ(NavAction::kLineDown, in.stick(0.1f, 0.6f, 20));   // the edge acts once
    EXPECT_EQ(NavAction::kNone, in.stick(0.1f, 0.9f, 30));
    EXPECT_EQ(NavAction::kNone, in.stick(0.0f, 0.4f, 40));   // above the release: still held
    EXPECT_EQ(NavAction::kLineDown, in.tick(20 + 350));       // held: repeats
    EXPECT_EQ(NavAction::kNone, in.stick(0.0f, 0.3f, 400));  // below the release
    EXPECT_EQ(NavAction::kNone, in.tick(2000));
    EXPECT_EQ(NavAction::kLineUp, in.stick(0.0f, -0.7f, 2100));
    EXPECT_EQ(NavAction::kPrevSection, in.stick(-0.9f, -0.2f, 2200));   // a new direction past the dead zone
    EXPECT_EQ(NavAction::kNone, in.stick(0.0f, 0.0f, 2300));
    EXPECT_EQ(NavAction::kNextSection, in.stick(0.8f, 0.1f, 2400));
    // Turned to another direction short of the dead zone: the old one stops repeating.
    EXPECT_EQ(NavAction::kNone, in.stick(0.1f, 0.4f, 2500));
    EXPECT_EQ(NavAction::kNone, in.tick(4000));
}

TEST(ButtonMapScene, BadgeHeaderAndHintFromTheResult) {
    DeviceId xbox;
    xbox.vendor = 0x045e;
    PadModel pad = padModel(standardPadSpec(xbox), GamepadState());
    MapResult r;
    r.ctx.kind = Context::kBrowser;
    r.consoleName = "Browser Controls";
    r.emulatorName = "Brave";
    r.source = Source::kConfig;
    r.browserGreyed = true;
    r.browserHint = browserHint(BrowserState::kNeedsControllerEnable);
    Scene s = sceneFor(r, pad);
    EXPECT_EQ("Action", s.consoleHeader);
    EXPECT_TRUE(s.greyed);
    EXPECT_EQ(r.browserHint, s.hint);
    EXPECT_EQ("", s.badge);

    MapResult d;
    d.ctx.kind = Context::kGame;
    d.consoleName = "GameCube";
    d.emulatorName = "Dolphin";
    d.source = Source::kNotMapped;
    d.notMappedWhere = "Settings > GameCube Input";
    d.notes.push_back("ignored");
    s = sceneFor(d, pad);
    EXPECT_EQ("Console button", s.consoleHeader);
    EXPECT_EQ("Not mapped in Dolphin", s.badge);
    EXPECT_EQ("Settings > GameCube Input", s.hint);
    EXPECT_TRUE(s.rows.empty());

    d.source = Source::kConfig;
    d.emulatorName = "RetroArch Snes9x";
    d.notes = {"Game remap: Super Metroid (USA).rmp"};
    s = sceneFor(d, pad);
    EXPECT_EQ("From your RetroArch settings \xC2\xB7 Game remap: Super Metroid (USA).rmp", s.badge);
}
