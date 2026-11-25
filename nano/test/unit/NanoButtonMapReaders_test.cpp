// The button map's per-emulator readers on fixture trees laid out like the device (T1, T2), and
// build()'s rows through the pad model. Each fixture is an excerpt of a shipped config (RetroArch's
// payload) or written from the emulator's format at the shipped version (button_map_overlay_plan.md §3).

#include <gtest/gtest.h>

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <openssl/md5.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "NanoButtonMap.h"
#include "NanoButtonMapReaders.h"
#include "nano_test_fs.h"

using namespace android;
using namespace android::bmap;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string nanoDir() { return nanotest::fixtureDir() + "/../.."; }

const Catalog& cat() {
    static Catalog c = [] {
        Catalog x;
        EXPECT_TRUE(loadCatalogFrom(nanoDir() + "/ps3xmb/controllers", nanoDir() + "/ps3xmb/buttonmap", &x));
        return x;
    }();
    return c;
}

// Readers walk the root without following links, so it is the fixture's real path.
// Make cannot install data files whose names have spaces or parentheses, so the fixtures spell
// them "~20", "~28", "~29"; root() copies a fixture into a temp dir under the real names.
std::string decodeName(const std::string& n) {
    std::string out;
    for (size_t i = 0; i < n.size(); i++) {
        if (n[i] == '~' && i + 2 < n.size() && isxdigit((unsigned char)n[i + 1]) && isxdigit((unsigned char)n[i + 2])) {
            out += (char)strtol(n.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += n[i];
        }
    }
    return out;
}

void copyTree(const std::string& from, const std::string& to) {
    mkdir(to.c_str(), 0755);
    DIR* d = opendir(from.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string src = from + "/" + n, dst = to + "/" + decodeName(n);
        struct stat st;
        if (lstat(src.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            copyTree(src, dst);
        } else if (S_ISREG(st.st_mode)) {
            std::ifstream in(src, std::ios::binary);
            std::ofstream(dst, std::ios::binary) << in.rdbuf();
        }
    }
    closedir(d);
}

std::string root(const std::string& rel) {
    static nanotest::TempDir cache("bmapfix");
    static std::map<std::string, std::string> copied;
    auto it = copied.find(rel);
    if (it != copied.end()) return it->second;
    std::string flat = rel;
    for (char& c : flat)
        if (c == '/') c = '_';
    const std::string dst = cache / flat;
    copyTree(nanotest::fixture("buttonmap/" + rel), dst);
    char buf[PATH_MAX];
    return copied[rel] = realpath(dst.c_str(), buf) ? std::string(buf) : dst;
}

std::string realDir(const nanotest::TempDir& t) {
    char buf[PATH_MAX];
    return realpath(t.path().c_str(), buf) ? std::string(buf) : t.path();
}

void writeFile(const std::string& path, const std::string& text) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    std::ofstream(path, std::ios::binary) << text;
}

PadModel cube(bool gammapad = false, bool swap = false) {
    PadSpec s;
    EXPECT_TRUE(padSpecFromJson(slurp(nanotest::fixture("buttonmap/pads/rg_cube.json")), &s));
    GamepadState gp;
    gp.running = gammapad;
    gp.swap = swap;
    return padModel(s, gp);
}

DeviceId cubeDev() {
    DeviceId d;
    d.vendor = 0x484b;
    d.product = 0x1101;
    d.name = "retrogame_joypad";
    d.descriptor = "cubedesc";
    return d;
}

DeviceId gammapadDev() {
    DeviceId d;
    d.vendor = 0x045e;
    d.product = 0x0b13;
    d.name = "Xbox Wireless Controller";
    return d;
}

ReaderEnv env(const std::string& rel, DeviceId d = cubeDev()) {
    ReaderEnv e;
    e.root = root(rel);
    e.dev = d;
    return e;
}

Context game(const std::string& emu, const std::string& sys, const std::string& core = std::string(),
             const std::string& rom = std::string()) {
    Context c;
    c.kind = Context::kGame;
    c.emulator = emu;
    c.systemId = sys;
    c.core = core;
    c.romPath = rom;
    return c;
}

// consoleId -> "Your controller", as the overlay shows it.
std::map<std::string, std::string> rows(const MapResult& r, const PadModel& pad) {
    std::map<std::string, std::string> m;
    for (const auto& row : r.rows) m[row.consoleId.empty() ? row.consoleName : row.consoleId] = rowKeys(row, pad);
    return m;
}

std::map<std::string, std::string> hotkeys(const MapResult& r) {
    std::map<std::string, std::string> m;
    for (const auto& h : r.hotkeys) m[h.action] = h.keys;
    return m;
}

const EmuBind* bind(const ReadResult& r, const std::string& id) {
    for (const auto& b : r.binds) if (b.id == id || (b.id.empty() && b.name == id)) return &b;
    return nullptr;
}

}  // namespace

// ---- the shared readers ----

TEST(ButtonMapRead, BoundedAndNoLinks) {
    nanotest::TempDir t("bmapread");
    const std::string r = realDir(t);
    writeFile(r + "/data/data/a.pkg/files/x.cfg", "a = \"1\"\n");
    writeFile(r + "/data/data/a.pkg/files/big.cfg", std::string(4096, 'x'));
    ReaderEnv e;
    e.root = r;
    std::string out;
    EXPECT_TRUE(readAppFile(e, "/data/user/0/a.pkg/files/x.cfg", &out));
    EXPECT_EQ("1", cfgValue(out, "a"));
    EXPECT_FALSE(readAppFile(e, "/data/data/a.pkg/files/big.cfg", &out, 1024));
    EXPECT_FALSE(readAppFile(e, "/data/data/a.pkg/files/../files/x.cfg", &out));
    ASSERT_EQ(0, symlink((r + "/data/data/a.pkg/files/x.cfg").c_str(), (r + "/data/data/a.pkg/files/link.cfg").c_str()));
    EXPECT_FALSE(readAppFile(e, "/data/data/a.pkg/files/link.cfg", &out));
    // An app folder that is a link is not read at all.
    mkdir((r + "/data/data/real").c_str(), 0755);
    writeFile(r + "/data/data/real/y.cfg", "k = v\n");
    ASSERT_EQ(0, symlink((r + "/data/data/real").c_str(), (r + "/data/data/b.pkg").c_str()));
    EXPECT_FALSE(readAppFile(e, "/data/data/b.pkg/y.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/data/data/none.pkg/y.cfg", &out));
}

TEST(ButtonMapRead, IniAndCfg) {
    const std::string ini = "top = 1\n[A]\nk = v\n; c = d\n[B]\nk = w\nk = x\n";
    EXPECT_EQ("v", iniValue(ini, "A", "k"));
    EXPECT_EQ("x", iniValue(ini, "B", "k"));
    EXPECT_EQ("1", iniValue(ini, "", "top"));
    bool found = true;
    EXPECT_EQ("", iniValue(ini, "A", "c", &found));
    EXPECT_FALSE(found);
    EXPECT_TRUE(iniHasSection(ini, "B"));
    EXPECT_FALSE(iniHasSection(ini, "C"));
    EXPECT_EQ("a b", cfgValue("k = \"a b\"\n# k = no\n", "k"));
    EXPECT_EQ("bare", cfgValue("k=bare", "k"));
}

// A file past the 4096th line is still read, up to the per-file line bound.
TEST(ButtonMapRead, LongFilesReadToTheLineBound) {
    std::string cfg, ini = "[S]\n";
    for (int i = 0; i < 6000; i++) {
        cfg += "pad_" + std::to_string(i) + " = \"x\"\n";
        ini += "k" + std::to_string(i) + " = x\n";
    }
    EXPECT_EQ("late", cfgValue(cfg + "k = \"late\"\n", "k"));
    EXPECT_EQ("late", iniValue(ini + "last = late\n", "S", "last"));
}

// SharedPreferences entities decode once, in one pass: a large escaped string is not a stall.
TEST(ButtonMapRead, PrefsEntitiesDecodeOnceAndQuickly) {
    nanotest::TempDir t("bmapprefs");
    const std::string r = realDir(t);
    std::string big;
    for (int i = 0; i < 150000; i++) big += "&quot;";
    writeFile(r + "/data/data/com.armsx2/shared_prefs/ARMSX2.xml",
              "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n"
              "    <string name=\"blob\">" + big + "</string>\n"
              "    <string name=\"pad.lstick.mode\">&amp;lt;a&quot;&apos;&gt;</string>\n</map>\n");
    ReaderEnv e;
    e.root = r;
    const auto t0 = std::chrono::steady_clock::now();
    const ReadResult rr = readArmsx2(e, game("armsx2", "ps2"));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 1000);
    bool seen = false;
    for (const auto& n : rr.notes) seen = seen || n == "Left stick: &lt;a\"'> mode";
    EXPECT_TRUE(seen);
}

// ---- RetroArch (T1, T2) ----

TEST(ButtonMapRetroArch, ShippedBindsOnTheCube) {
    const ReaderEnv e = env("retroarch/shipped");
    const Context c = game("retroarch", "snes", "snes9x");
    const ReadResult rr = readRetroArch(e, c, cat());
    EXPECT_EQ(Source::kConfig, rr.source);
    EXPECT_EQ("RetroArch Snes9x", rr.emulatorName);
    const MapResult r = build(e, c, cube(), cat());
    EXPECT_EQ("snes", r.drawing);
    auto m = rows(r, cube());
    EXPECT_EQ("B", m["btn-b"]);   // RetroPad B is 96: the bottom button with gammapad off
    EXPECT_EQ("A", m["btn-a"]);
    EXPECT_EQ("Y", m["btn-y"]);
    EXPECT_EQ("X", m["btn-x"]);
    EXPECT_EQ("L1", m["btn-l"]);
    EXPECT_EQ("R1", m["btn-r"]);
    EXPECT_EQ("Start", m["btn-start"]);
    EXPECT_EQ("Select", m["btn-select"]);
    EXPECT_EQ("D-pad / Left stick", m["dpad"]);   // analog_dpad_mode 1, a core that never reads a stick
    auto h = hotkeys(r);
    EXPECT_EQ("Select + R2", h["Save state"]);
    EXPECT_EQ("Select + L2", h["Load state"]);
    EXPECT_EQ("Select + R1", h["Fast forward"]);
    EXPECT_EQ("Select + A", h["Rewind"]);
    EXPECT_EQ("L3 + R3", h["RetroArch menu"]);   // input_menu_toggle_gamepad_combo 2; 189 is no pad key
}

// §5's example: SNES on Snes9x in each gammapad state.
TEST(ButtonMapRetroArch, AbxySwapExample) {
    const Context c = game("retroarch", "snes", "snes9x");
    struct State { bool on, swap; const char* bottom; const char* right; const char* top; const char* left; };
    const State kStates[] = {
        {false, false, "btn-b", "btn-a", "btn-x", "btn-y"},
        {true, false, "btn-b", "btn-a", "btn-x", "btn-y"},
        {true, true, "btn-a", "btn-b", "btn-y", "btn-x"},
    };
    for (const auto& s : kStates) {
        SCOPED_TRACE(std::to_string(s.on) + std::to_string(s.swap));
        const PadModel pad = cube(s.on, s.swap);
        const MapResult r = build(env("retroarch/shipped", s.on ? gammapadDev() : cubeDev()), c, pad, cat());
        auto m = rows(r, pad);
        EXPECT_EQ("B", m[s.bottom]);
        EXPECT_EQ("A", m[s.right]);
        EXPECT_EQ("X", m[s.top]);
        EXPECT_EQ("Y", m[s.left]);
    }
}

TEST(ButtonMapRetroArch, AutoconfigFillsOnlyNulBinds) {
    // The gammapad virtual pad matches the Xbox profile by VID:PID; the pinned player binds still win,
    // and its trigger axes fill the nul l2/r2 axes.
    const ReadResult rr = readRetroArch(env("retroarch/shipped", gammapadDev()), game("retroarch", "psx", "pcsx_rearmed"), cat());
    bool pad = false;
    for (const auto& n : rr.notes) pad = pad || n == "Pad profile: Xbox Wireless Controller 2020.cfg";
    EXPECT_TRUE(pad);
    const EmuBind* l2 = bind(rr, "btn-l2");
    ASSERT_NE(nullptr, l2);
    bool key = false, axis = false;
    for (const auto& in : l2->in) {
        key = key || in == Input::key(104);
        axis = axis || in == Input::axis(aaxis::kLTrigger, 1);
    }
    EXPECT_TRUE(key);
    EXPECT_TRUE(axis);
    // The built-in pad matches retrogame_joypad.cfg, which has no trigger axes.
    const ReadResult cubeRr = readRetroArch(env("retroarch/shipped"), game("retroarch", "psx", "pcsx_rearmed"), cat());
    EXPECT_EQ(1u, bind(cubeRr, "btn-l2")->in.size());
}

// An autoconfig folder that is a link is neither listed nor read: no pad profile comes from it.
TEST(ButtonMapRetroArch, LinkedAutoconfigFolderIsNotUsed) {
    nanotest::TempDir t("bmaplink");
    const std::string r = realDir(t);
    copyTree(nanotest::fixture("buttonmap/retroarch/shipped"), r);
    const std::string ac = r + "/data/data/com.retroarch.aarch64/autoconfig";
    ASSERT_EQ(0, rename(ac.c_str(), (r + "/data/data/elsewhere").c_str()));
    ASSERT_EQ(0, symlink((r + "/data/data/elsewhere").c_str(), ac.c_str()));
    ReaderEnv e;
    e.root = r;
    e.dev = gammapadDev();
    const ReadResult rr = readRetroArch(e, game("retroarch", "psx", "pcsx_rearmed"), cat());
    for (const auto& n : rr.notes) EXPECT_EQ(std::string::npos, n.find("Pad profile")) << n;
}

// RetroArch's folder moved to the card: launches pass the card's retroarch.cfg, so the map reads it
// (raConfig), with its override and remap folders on the card too, not the default config.
TEST(ButtonMapRetroArch, ReadsTheConfigTheLaunchesPass) {
    nanotest::TempDir t("bmapcard");
    const std::string r = realDir(t);
    copyTree(nanotest::fixture("buttonmap/retroarch/shipped"), r);
    const std::string card = "/storage/ABCD-1234/RetroArch";
    const std::string raw = r + "/mnt/media_rw/ABCD-1234/RetroArch";
    std::string cfg = slurp(r + "/data/media/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg");
    cfg += "input_player1_a_btn = \"100\"\ninput_player1_x_btn = \"97\"\n";
    cfg += "rgui_config_directory = \"" + card + "/config\"\n";
    writeFile(raw + "/retroarch.cfg", cfg);
    writeFile(raw + "/config/Snes9x/Snes9x.cfg", "input_player1_l_btn = \"103\"\ninput_player1_r_btn = \"102\"\n");
    ReaderEnv e;
    e.root = r;
    e.dev = cubeDev();
    const Context c = game("retroarch", "snes", "snes9x");
    auto def = rows(build(e, c, cube(), cat()), cube());
    EXPECT_EQ("A", def["btn-a"]);
    EXPECT_EQ("L1", def["btn-l"]);
    e.raConfig = card + "/retroarch.cfg";
    const MapResult m = build(e, c, cube(), cat());
    auto onCard = rows(m, cube());
    EXPECT_EQ("X", onCard["btn-a"]);   // 100: the top button
    EXPECT_EQ("A", onCard["btn-x"]);
    EXPECT_EQ("R1", onCard["btn-l"]);   // the card's core override
    EXPECT_EQ("L1", onCard["btn-r"]);
    EXPECT_EQ(Source::kConfig, readRetroArch(e, c, cat()).source);
    // A card path is read at its raw folder, still without following a link.
    std::string out;
    EXPECT_TRUE(readAppFile(e, card + "/retroarch.cfg", &out));
    ASSERT_EQ(0, symlink((raw + "/retroarch.cfg").c_str(), (raw + "/link.cfg").c_str()));
    EXPECT_FALSE(readAppFile(e, card + "/link.cfg", &out));
}

// A /storage path is read at a card's raw folder only for a card's volume id: no "..", no other
// name under /storage.
TEST(ButtonMapReaders, StoragePathsReadOnlyACardsFolder) {
    nanotest::TempDir t("bmapvol");
    const std::string r = realDir(t);
    writeFile(r + "/mnt/media_rw/ABCD-1234/a.cfg", "x = \"1\"\n");
    writeFile(r + "/mnt/media_rw/emulated/a.cfg", "x = \"1\"\n");
    writeFile(r + "/mnt/media_rw/odd_vol/a.cfg", "x = \"1\"\n");
    writeFile(r + "/data/secret.cfg", "x = \"1\"\n");
    ReaderEnv e;
    e.root = r;
    std::string out;
    EXPECT_TRUE(readAppFile(e, "/storage/ABCD-1234/a.cfg", &out));
    writeFile(r + "/mnt/media_rw/public:179,1/a.cfg", "x = \"1\"\n");
    EXPECT_TRUE(readAppFile(e, "/storage/public:179,1/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/ABCD-1234/../../../data/secret.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/../data/secret.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/emulatedX/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/emulated/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/odd_vol/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/self/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage//ABCD-1234/a.cfg", &out));
    EXPECT_FALSE(readAppFile(e, "/storage/", &out));
}

TEST(ButtonMapRetroArch, NoConfigUsesTheProfile) {
    const ReadResult rr = readRetroArch(env("retroarch/nocfg"), game("retroarch", "snes", "snes9x"), cat());
    EXPECT_EQ(Source::kDefaults, rr.source);
    const EmuBind* b = bind(rr, "btn-b");
    ASSERT_NE(nullptr, b);
    ASSERT_FALSE(b->in.empty());
    EXPECT_EQ(Input::key(96), b->in[0]);
}

TEST(ButtonMapRetroArch, CoreOverride) {
    const Context c = game("retroarch", "snes", "snes9x");
    const MapResult r = build(env("retroarch/override"), c, cube(), cat());
    auto m = rows(r, cube());
    EXPECT_EQ("X", m["btn-a"]);   // input_player1_a_btn = 100: the top button
    EXPECT_EQ("A", m["btn-x"]);
    EXPECT_EQ("Core override: Snes9x.cfg", r.notes.at(0));
}

TEST(ButtonMapRetroArch, RemapPrecedenceGameThenFolderThenCore) {
    const PadModel pad = cube();
    // Game remap: L and R swapped, Select unmapped.
    MapResult r = build(env("retroarch/remaps"), game("retroarch", "snes", "snes9x", "/storage/emulated/0/ROMs/SNES/Super Metroid (USA).sfc"),
                        pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("R1", m["btn-l"]);
    EXPECT_EQ("L1", m["btn-r"]);
    EXPECT_EQ("\xE2\x80\x94", m["btn-select"]);
    EXPECT_EQ("B", m["btn-b"]);   // remaps don't stack: the core remap is not applied
    bool note = false;
    for (const auto& n : r.notes) note = note || n == "Game remap: Super Metroid (USA).rmp";
    EXPECT_TRUE(note);
    // Folder remap (the ROM's folder is SNES): Y and X swapped.
    r = build(env("retroarch/remaps"), game("retroarch", "snes", "snes9x", "/storage/emulated/0/ROMs/SNES/Other.sfc"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("Y", m["btn-x"]);
    EXPECT_EQ("X", m["btn-y"]);
    // Core remap: B and A swapped.
    r = build(env("retroarch/remaps"), game("retroarch", "snes", "snes9x", "/storage/emulated/0/ROMs/x/Other.sfc"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);
    EXPECT_EQ("A", m["btn-b"]);
}

TEST(ButtonMapRetroArch, DeviceTypeFromTheRemap) {
    const PadModel pad = cube();
    MapResult r = build(env("retroarch/shipped"), game("retroarch", "psx", "mednafen_psx_hw"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ(0u, m.count("stick-l"));   // a digital pad: no sticks
    EXPECT_EQ("D-pad / Left stick", m["dpad"]);
    r = build(env("retroarch/psx_dualshock"), game("retroarch", "psx", "mednafen_psx_hw"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("Left stick", m["stick-l"]);
    EXPECT_EQ("Right stick", m["stick-r"]);
    EXPECT_EQ("L3", m["btn-l3"]);
    EXPECT_EQ("D-pad", m["dpad"]);   // the core reads the stick: no left-stick D-pad
    bool ds = false;
    for (const auto& n : r.notes) ds = ds || n == "DualShock";
    EXPECT_TRUE(ds);
}

TEST(ButtonMapRetroArch, CoreOptionsAndSystems) {
    const PadModel pad = cube();
    auto m = rows(build(env("retroarch/mesen_shift"), game("retroarch", "nes", "mesen"), pad, cat()), pad);
    EXPECT_EQ("B", m["btn-a"]);   // shifted clockwise: NES A on RetroPad B
    EXPECT_EQ("Y", m["btn-b"]);
    m = rows(build(env("retroarch/shipped"), game("retroarch", "nes", "mesen"), pad, cat()), pad);
    EXPECT_EQ("A", m["btn-a"]);
    // Genesis Plus GX on the Master System: 1 and 2.
    const MapResult sms = build(env("retroarch/shipped"), game("retroarch", "mastersystem", "genesis_plus_gx"), pad, cat());
    m = rows(sms, pad);
    EXPECT_EQ("mastersystem", sms.drawing);
    EXPECT_EQ("B", m["btn-1"]);
    EXPECT_EQ("A", m["btn-2"]);
    EXPECT_EQ(0u, m.count("btn-mode"));
    // N64 on mupen64plus: the right stick is the C buttons.
    m = rows(build(env("retroarch/shipped"), game("retroarch", "n64", "mupen64plus_next_gles3"), pad, cat()), pad);
    EXPECT_EQ("Right stick Up", m["c-up"]);
    EXPECT_EQ("L2", m["btn-z"]);
    EXPECT_EQ("A", m["C-Right (hold R2)"]);
}

// Beetle WonderSwan (libretro.c update_input): landscape X1-X4 on the D-pad, Y1-Y4 on R2/R1/L2/L1;
// the portrait layout when its options turn the buttons with the screen.
TEST(ButtonMapRetroArch, WonderSwanLandscapeAndPortrait) {
    const PadModel pad = cube();
    for (const char* sys : {"wonderswan", "wonderswancolor"}) {
        SCOPED_TRACE(sys);
        const MapResult r = build(env("retroarch/shipped"), game("retroarch", sys, "mednafen_wswan"), pad, cat());
        EXPECT_EQ(Source::kConfig, r.source);
        EXPECT_EQ(sys, r.console);
        EXPECT_EQ("wonderswan", r.drawing);
        EXPECT_EQ("RetroArch Beetle WonderSwan", r.emulatorName);
        auto m = rows(r, pad);
        EXPECT_EQ("D-pad Up / Left stick Up", m["btn-x1"]);   // analog_dpad_mode 1
        EXPECT_EQ("D-pad Right / Left stick Right", m["btn-x2"]);
        EXPECT_EQ("D-pad Down / Left stick Down", m["btn-x3"]);
        EXPECT_EQ("D-pad Left / Left stick Left", m["btn-x4"]);
        EXPECT_EQ("R2", m["btn-y1"]);
        EXPECT_EQ("R1", m["btn-y2"]);
        EXPECT_EQ("L2", m["btn-y3"]);
        EXPECT_EQ("L1", m["btn-y4"]);
        EXPECT_EQ("A", m["btn-a"]);
        EXPECT_EQ("B", m["btn-b"]);
        EXPECT_EQ("Start", m["btn-start"]);
        EXPECT_EQ("Select", m["Rotate"]);
        for (const auto& row : r.rows) EXPECT_FALSE(row.unbound()) << row.consoleId;
    }
    EXPECT_EQ("WonderSwan Color",
              build(env("retroarch/shipped"), game("retroarch", "wonderswancolor", "mednafen_wswan"), pad, cat()).consoleName);

    nanotest::TempDir t("bmapwswan");
    const std::string r = realDir(t);
    copyTree(nanotest::fixture("buttonmap/retroarch/shipped"), r);
    const std::string opt = r + "/data/media/0/RetroArch/config/Beetle WonderSwan/Beetle WonderSwan.opt";
    ReaderEnv e;
    e.root = r;
    e.dev = cubeDev();
    const Context c = game("retroarch", "wonderswan", "mednafen_wswan");
    struct Case { const char* opts; bool portrait; };
    const Case kCases[] = {
        {"wswan_rotate_display = \"manual\"\nwswan_rotate_keymap = \"auto\"\n", false},
        {"wswan_rotate_display = \"portrait\"\nwswan_rotate_keymap = \"auto\"\n", true},
        {"wswan_rotate_display = \"portrait\"\nwswan_rotate_keymap = \"disabled\"\n", false},
        {"wswan_rotate_display = \"landscape\"\nwswan_rotate_keymap = \"enabled\"\n", true},
    };
    for (const auto& k : kCases) {
        SCOPED_TRACE(k.opts);
        writeFile(opt, k.opts);
        auto m = rows(build(e, c, pad, cat()), pad);
        if (k.portrait) {
            EXPECT_EQ("Y", m["btn-x1"]);   // RetroPad Y, X, A, B
            EXPECT_EQ("X", m["btn-x2"]);
            EXPECT_EQ("A", m["btn-x3"]);
            EXPECT_EQ("B", m["btn-x4"]);
            EXPECT_EQ("D-pad Left / Left stick Left", m["btn-y1"]);
            EXPECT_EQ("D-pad Up / Left stick Up", m["btn-y2"]);
            EXPECT_EQ("D-pad Right / Left stick Right", m["btn-y3"]);
            EXPECT_EQ("D-pad Down / Left stick Down", m["btn-y4"]);
            EXPECT_EQ("L1", m["btn-a"]);
            EXPECT_EQ("R1", m["btn-b"]);
        } else {
            EXPECT_EQ("D-pad Up / Left stick Up", m["btn-x1"]);
            EXPECT_EQ("R2", m["btn-y1"]);
            EXPECT_EQ("A", m["btn-a"]);
            EXPECT_EQ("B", m["btn-b"]);
        }
        EXPECT_EQ("Start", m["btn-start"]);
    }
}

TEST(ButtonMapRetroArch, UnknownCoreShowsTheRetroPad) {
    const MapResult r = build(env("retroarch/shipped"), game("retroarch", "", "someday_core"), cube(), cat());
    EXPECT_EQ("modern", r.drawing);
    auto m = rows(r, cube());
    EXPECT_EQ("B", m["btn-a"]);   // RetroPad B on the generic pad's bottom button
}

TEST(ButtonMapRetroArch, MalformedConfigIsSafe) {
    const ReadResult rr = readRetroArch(env("retroarch/malformed"), game("retroarch", "snes", "snes9x"), cat());
    EXPECT_EQ(Source::kConfig, rr.source);
    for (const auto& b : rr.binds)
        for (const auto& in : b.in) EXPECT_NE(Input::kOther, in.kind);
}

// ---- PPSSPP ----

TEST(ButtonMapPpsspp, ShippedControlsByPosition) {
    const PadModel pad = cube();
    const MapResult r = build(env("ppsspp/shipped"), game("ppsspp", "psp"), pad, cat());
    EXPECT_EQ(Source::kConfig, r.source);
    auto m = rows(r, pad);
    EXPECT_EQ("B", m["btn-cross"]);   // the bottom button
    EXPECT_EQ("A", m["btn-circle"]);
    EXPECT_EQ("Y", m["btn-square"]);
    EXPECT_EQ("X", m["btn-triangle"]);
    EXPECT_EQ("D-pad", m["dpad"]);
    EXPECT_EQ("Left stick", m["stick-l"]);
    auto h = hotkeys(r);
    EXPECT_EQ("R2", h["Fast forward (hold)"]);   // the keyboard's 1-62 and the provisioned F2/F4 are not shown
    EXPECT_EQ("L2", h["Pause menu"]);
    EXPECT_EQ(0u, h.count("Save state"));
}

TEST(ButtonMapPpsspp, MissingKeysAreUnboundAndOtherDevicesIgnored) {
    const PadModel pad = cube();
    auto m = rows(build(env("ppsspp/mixed"), game("ppsspp", "psp"), pad, cat()), pad);
    EXPECT_EQ("A", m["btn-cross"]);            // 1-62 and 0-96 dropped, 10-97 kept
    EXPECT_EQ("L1 + B", m["btn-circle"]);      // a chord
    EXPECT_EQ("\xE2\x80\x94", m["btn-square"]);  // the section exists: a missing key is unbound
    EXPECT_EQ("D-pad Up", m["dpad-up"]);
}

TEST(ButtonMapPpsspp, NoSectionMeansDefaults) {
    const PadModel pad = cube();
    const MapResult r = build(env("ppsspp/nosection"), game("ppsspp", "psp"), pad, cat());
    EXPECT_EQ(Source::kDefaults, r.source);
    EXPECT_EQ("B", rows(r, pad)["btn-cross"]);
    nanotest::TempDir t("bmapnone");
    ReaderEnv e;
    e.root = realDir(t);
    EXPECT_EQ(Source::kDefaults, readPpsspp(e, game("ppsspp", "psp")).source);
}

namespace {
// A 40 KB ISO 9660 image whose root folder holds UMD_DATA.BIN.
std::string tinyPspIso(const std::string& id) {
    std::string img(20 * 2048, '\0');
    char* pvd = &img[16 * 2048];
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    auto le32 = [](char* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (char)(v >> (8 * i)); };
    le32(pvd + 156 + 2, 18);      // the root folder at sector 18
    le32(pvd + 156 + 10, 2048);
    char* dir = &img[18 * 2048];
    dir[0] = 33 + 14;
    le32(dir + 2, 19);            // UMD_DATA.BIN at sector 19
    dir[32] = 14;
    memcpy(dir + 33, "UMD_DATA.BIN;1", 14);
    memcpy(&img[19 * 2048], id.data(), id.size());
    return img;
}
}  // namespace

TEST(ButtonMapPpsspp, PerGameIniReplacesControls) {
    nanotest::TempDir t("bmapppsspp");
    const std::string r = realDir(t);
    writeFile(r + "/data/media/0/PSP/SYSTEM/ppsspp.ini", "[General]\n");
    writeFile(r + "/data/media/0/PSP/SYSTEM/controls.ini", "[ControlMapping]\nCross = 10-96\n");
    writeFile(r + "/data/media/0/ROMs/psp/Game.iso", tinyPspIso("ULUS-10041|0001|G"));
    ReaderEnv e;
    e.root = r;
    EXPECT_EQ("ULUS10041", pspDiscId(e, "/storage/emulated/0/ROMs/psp/Game.iso"));
    const Context c = game("ppsspp", "psp", "", "/storage/emulated/0/ROMs/psp/Game.iso");
    // A game file with no [ControlMapping]: PPSSPP's built-in defaults, not controls.ini.
    writeFile(r + "/data/media/0/PSP/SYSTEM/ULUS10041_ppsspp.ini", "[Graphics]\nx = 1\n");
    ReadResult rr = readPpsspp(e, c);
    EXPECT_EQ(Source::kDefaults, rr.source);
    writeFile(r + "/data/media/0/PSP/SYSTEM/ULUS10041_ppsspp.ini", "[ControlMapping]\nCross = 10-100\n");
    rr = readPpsspp(e, c);
    EXPECT_EQ(Source::kConfig, rr.source);
    EXPECT_EQ(Input::key(100), bind(rr, "btn-cross")->in.at(0));
    // Not a disc image.
    writeFile(r + "/data/media/0/ROMs/psp/junk.iso", std::string(100, 'x'));
    EXPECT_EQ("", pspDiscId(e, "/storage/emulated/0/ROMs/psp/junk.iso"));
}

// ---- Dolphin ----

TEST(ButtonMapDolphin, ExpressionsAndDevice) {
    const PadModel pad = cube();
    const MapResult r = build(env("dolphin/bound"), game("dolphin", "gc"), pad, cat());
    EXPECT_EQ(Source::kConfig, r.source);
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);            // `Button B` is 97: the right button
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("X / Y", m["btn-x"]);        // an alternative
    EXPECT_EQ("Y", m["btn-y"]);            // a qualified input
    EXPECT_EQ("Left stick", m["stick-l"]);
    EXPECT_EQ("D-pad", m["dpad"]);
    EXPECT_EQ("`Button L1` & `Button A`", m["stick-r:up"]);   // advanced: shown as written
}

TEST(ButtonMapDolphin, AnotherPadsBindsAndNoBinds) {
    const PadModel pad = cube();
    const MapResult other = build(env("dolphin/otherpad"), game("dolphin", "gc"), pad, cat());
    EXPECT_EQ("Xbox Wireless Controller", rows(other, pad)["btn-a"]);
    EXPECT_EQ("Bound to Xbox Wireless Controller", other.notes.at(0));
    const MapResult empty = build(env("dolphin/empty"), game("dolphin", "gc"), pad, cat());
    EXPECT_EQ(Source::kNotMapped, empty.source);
    EXPECT_EQ("Dolphin > Settings > GameCube Input", empty.notMappedWhere);
    nanotest::TempDir t("bmapdolphin");
    ReaderEnv e;
    e.root = realDir(t);
    EXPECT_EQ(Source::kNotMapped, readDolphin(e, game("dolphin", "wii")).source);
    EXPECT_EQ("Dolphin > Settings > Wii Input", readDolphin(e, game("dolphin", "wii")).notMappedWhere);
}

TEST(ButtonMapDolphin, GameProfileFromTheDiscHeader) {
    nanotest::TempDir t("bmapdolphingame");
    const std::string r = realDir(t);
    const std::string cfg = r + "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/Config";
    writeFile(cfg + "/GCPadNew.ini", "[GCPad1]\nButtons/A = `Button A`\n");
    writeFile(cfg + "/Profiles/GCPad/Mine.ini", "[Profile]\nButtons/A = `Button Y`\n");
    writeFile(r + "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/GameSettings/GALE01.ini",
              "[Controls]\nPadProfile1 = Mine\n");
    writeFile(r + "/data/media/0/ROMs/gc/Melee.iso", "GALE01" + std::string(0x60, '\0'));
    ReaderEnv e;
    e.root = r;
    const ReadResult rr = readDolphin(e, game("dolphin", "gc", "", "/storage/emulated/0/ROMs/gc/Melee.iso"));
    EXPECT_EQ(Input::key(100), bind(rr, "btn-a")->in.at(0));
    EXPECT_EQ("Game profile: Mine", rr.notes.at(0));
}

// An app's file can hold any amount of text: what reaches the panel is clipped, on a character.
TEST(ButtonMapDolphin, LongTextIsClipped) {
    nanotest::TempDir t("bmapdolphinlong");
    const std::string r = realDir(t);
    std::string expr, name;
    for (int i = 0; i < 20000; i++) expr += "`Button A` & ";
    for (int i = 0; i < 20000; i++) name += "\xC3\xA9";
    writeFile(r + "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/Config/GCPadNew.ini",
              "[GCPad1]\nDevice = Android/0/" + name + "\nButtons/A = `Button A`\nButtons/B = " + expr + "`Button B`\n");
    ReaderEnv e;
    e.root = r;
    e.dev = cubeDev();
    MapResult m = build(e, game("dolphin", "gc"), cube(), cat());
    ASSERT_FALSE(m.notes.empty());
    EXPECT_LE(m.notes[0].size(), 200u);
    EXPECT_EQ(0, m.notes[0].compare(0, 9, "Bound to "));
    const std::string tail = m.notes[0].substr(9);
    EXPECT_EQ(std::count(tail.begin(), tail.end(), '\xC3'), std::count(tail.begin(), tail.end(), '\xA9'));
    for (const auto& row : m.rows) EXPECT_LE(rowKeys(row, cube()).size(), 200u) << row.consoleId;
    writeFile(r + "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/Config/GCPadNew.ini",
              "[GCPad1]\nButtons/B = " + expr + "`Button B`\n");
    m = build(e, game("dolphin", "gc"), cube(), cat());
    bool found = false;
    for (const auto& row : m.rows) {
        if (row.consoleId != "btn-b") continue;
        found = true;
        EXPECT_LE(row.offPad.size(), 200u);
        EXPECT_EQ(0, row.offPad.compare(0, 13, "`Button A` & "));
    }
    EXPECT_TRUE(found);
}

// Move Emulator Files to microSD leaves Dolphin's Config and GameSettings as links to
// <card>/emulator_config/dolphin: its binds are read there, not taken for "not mapped".
TEST(ButtonMapDolphin, ConfigMovedToTheCard) {
    nanotest::TempDir t("bmapdolphinmoved");
    const std::string r = realDir(t);
    copyTree(nanotest::fixture("buttonmap/dolphin/moved"), r);
    const std::string files = r + "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files";
    const std::string card = "/storage/1234-ABCD/emulator_config/dolphin";
    writeFile(files + "/StateSaves/.keep", "");
    ASSERT_EQ(0, symlink((card + "/Config").c_str(), (files + "/Config").c_str()));
    ReaderEnv e;
    e.root = r;
    e.dev = cubeDev();
    const PadModel pad = cube();
    MapResult m = build(e, game("dolphin", "gc"), pad, cat());
    EXPECT_EQ(Source::kConfig, m.source);
    EXPECT_EQ("gamecube", m.drawing);
    auto rw = rows(m, pad);
    EXPECT_EQ("A", rw["btn-a"]);   // `Button B` is 97: the right button
    EXPECT_EQ("B", rw["btn-b"]);
    EXPECT_EQ("X", rw["btn-x"]);
    EXPECT_EQ("Y", rw["btn-y"]);
    EXPECT_EQ("R1", rw["btn-z"]);
    EXPECT_EQ("L2", rw["btn-l"]);
    EXPECT_EQ("Left stick", rw["stick-l"]);
    EXPECT_EQ("Right stick", rw["stick-r"]);
    EXPECT_EQ("D-pad", rw["dpad"]);
    m = build(e, game("dolphin", "wii"), pad, cat());
    EXPECT_EQ(Source::kConfig, m.source);
    EXPECT_EQ("Start", rows(m, pad)["btn-plus"]);

    // A game's profile, with GameSettings moved as well.
    writeFile(r + "/mnt/media_rw/1234-ABCD/emulator_config/dolphin/GameSettings/GALE01.ini", "[Controls]\nPadProfile1 = Mine\n");
    writeFile(r + "/mnt/media_rw/1234-ABCD/emulator_config/dolphin/Config/Profiles/GCPad/Mine.ini",
              "[Profile]\nButtons/A = `Button Y`\n");
    ASSERT_EQ(0, symlink((card + "/GameSettings").c_str(), (files + "/GameSettings").c_str()));
    writeFile(r + "/data/media/0/ROMs/gc/Melee.iso", "GALE01" + std::string(0x60, '\0'));
    const ReadResult rr = readDolphin(e, game("dolphin", "gc", "", "/storage/emulated/0/ROMs/gc/Melee.iso"));
    ASSERT_NE(nullptr, bind(rr, "btn-a"));
    ASSERT_FALSE(bind(rr, "btn-a")->in.empty());
    EXPECT_EQ(Input::key(100), bind(rr, "btn-a")->in[0]);
    EXPECT_EQ("Game profile: Mine", rr.notes.at(0));
}

// Only a link into a card's emulator_config is followed; any other leaves the file unread.
TEST(ButtonMapRead, MoveLinksLeadOnlyToTheCardsEmulatorFolders) {
    nanotest::TempDir t("bmapmovelinks");
    const std::string r = realDir(t);
    const std::string app = r + "/data/media/0/Android/data/a.pkg/files";
    writeFile(app + "/keep", "");
    writeFile(r + "/mnt/media_rw/1234-ABCD/emulator_config/a/Config/x.ini", "k = \"card\"\n");
    writeFile(r + "/mnt/media_rw/1234-ABCD/other/Config/x.ini", "k = \"other\"\n");
    writeFile(r + "/data/media/0/Elsewhere/x.ini", "k = \"internal\"\n");
    writeFile(r + "/data/secret/x.ini", "k = \"secret\"\n");
    ReaderEnv e;
    e.root = r;
    const std::string dev = "/storage/emulated/0/Android/data/a.pkg/files/Config/x.ini";
    std::string out;
    auto linkTo = [&](const std::string& target) {
        unlink((app + "/Config").c_str());
        return symlink(target.c_str(), (app + "/Config").c_str()) == 0;
    };
    ASSERT_TRUE(linkTo("/storage/1234-ABCD/emulator_config/a/Config/"));
    EXPECT_EQ("/storage/1234-ABCD/emulator_config/a/Config/x.ini", throughMoveLinks(e, dev));
    ASSERT_TRUE(readMovedAppFile(e, dev, &out));
    EXPECT_EQ("card", cfgValue(out, "k"));
    EXPECT_FALSE(readAppFile(e, dev, &out));   // the plain read still follows no link
    for (const char* bad : {"/storage/1234-ABCD/other/Config", "/storage/emulated/0/Elsewhere", "/data/secret",
                            "/storage/1234-ABCD/emulator_config/../other/Config", "../../../../secret",
                            "/storage/self/primary/emulator_config/a/Config", "/storage/odd_vol/emulator_config/a/Config",
                            "/storage/1234-ABCD/emulator_config"}) {
        SCOPED_TRACE(bad);
        ASSERT_TRUE(linkTo(bad));
        EXPECT_EQ("", throughMoveLinks(e, dev));
        EXPECT_FALSE(readMovedAppFile(e, dev, &out));
    }
    // A link on the card itself is held to the same rule.
    ASSERT_TRUE(linkTo("/storage/1234-ABCD/emulator_config/a/Config"));
    const std::string cardCfg = r + "/mnt/media_rw/1234-ABCD/emulator_config/a/Config";
    ASSERT_EQ(0, rename(cardCfg.c_str(), (cardCfg + ".real").c_str()));
    ASSERT_EQ(0, symlink((r + "/data/secret").c_str(), cardCfg.c_str()));
    EXPECT_FALSE(readMovedAppFile(e, dev, &out));
    // A link the leaf is never followed; a path with no link reads as before.
    unlink(cardCfg.c_str());
    ASSERT_EQ(0, rename((cardCfg + ".real").c_str(), cardCfg.c_str()));
    ASSERT_EQ(0, symlink((cardCfg + "/x.ini").c_str(), (cardCfg + "/leaf.ini").c_str()));
    EXPECT_FALSE(readMovedAppFile(e, "/storage/emulated/0/Android/data/a.pkg/files/Config/leaf.ini", &out));
    writeFile(app + "/plain.ini", "k = \"plain\"\n");
    ASSERT_TRUE(readMovedAppFile(e, "/storage/emulated/0/Android/data/a.pkg/files/plain.ini", &out));
    EXPECT_EQ("plain", cfgValue(out, "k"));

    // A card folder swapped for a link after the links were checked: the read follows none.
    ASSERT_TRUE(linkTo("/storage/1234-ABCD/emulator_config/a/Config"));
    const std::string moved = throughMoveLinks(e, dev);
    ASSERT_EQ("/storage/1234-ABCD/emulator_config/a/Config/x.ini", moved);
    ASSERT_EQ(0, rename(cardCfg.c_str(), (cardCfg + ".real").c_str()));
    ASSERT_EQ(0, symlink((r + "/data/secret").c_str(), cardCfg.c_str()));
    EXPECT_FALSE(readAppFile(e, moved, &out));
    unlink(cardCfg.c_str());
    ASSERT_EQ(0, rename((cardCfg + ".real").c_str(), cardCfg.c_str()));
    // A card that is not in: nothing to read.
    ASSERT_TRUE(linkTo("/storage/9999-0000/emulator_config/a/Config"));
    EXPECT_FALSE(readMovedAppFile(e, dev, &out));
    // Links from card folder to card folder, as far as the hop limit.
    const std::string cardTop = r + "/mnt/media_rw/1234-ABCD/emulator_config";
    for (int i = 1; i <= 4; i++)
        ASSERT_EQ(0, symlink(("/storage/1234-ABCD/emulator_config/hop" + std::to_string(i + 1)).c_str(),
                             (cardTop + "/hop" + std::to_string(i)).c_str()));
    ASSERT_EQ(0, symlink("/storage/1234-ABCD/emulator_config/a", (cardTop + "/hop5").c_str()));
    ASSERT_TRUE(linkTo("/storage/1234-ABCD/emulator_config/hop4/Config"));
    EXPECT_EQ("/storage/1234-ABCD/emulator_config/a/Config/x.ini", throughMoveLinks(e, dev));
    ASSERT_TRUE(linkTo("/storage/1234-ABCD/emulator_config/hop1/Config"));
    EXPECT_EQ("", throughMoveLinks(e, dev));
}

// ---- Azahar ----

TEST(ButtonMapAzahar, BindsAndNotMapped) {
    const PadModel pad = cube();
    const MapResult r = build(env("azahar/bound"), game("azahar", "n3ds"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("X", m["btn-x"]);
    EXPECT_EQ("Left stick", m["stick-l"]);
    EXPECT_EQ("L2", m["btn-zl"]);
    const MapResult none = build(env("azahar/unbound"), game("azahar", "n3ds"), pad, cat());
    EXPECT_EQ(Source::kNotMapped, none.source);
    EXPECT_EQ("Azahar > Settings > Controls > Auto-map", none.notMappedWhere);
}

// ---- Flycast ----

TEST(ButtonMapFlycast, PerDeviceFileAndDefaults) {
    const PadModel pad = cube();
    MapResult r = build(env("flycast/cube"), game("flycast", "dreamcast"), pad, cat());
    EXPECT_EQ(Source::kConfig, r.source);
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("L2", m["btn-l"]);
    EXPECT_EQ("Left stick", m["stick-l"]);
    EXPECT_EQ("D-pad", m["dpad"]);
    EXPECT_EQ("Select", hotkeys(r)["Flycast menu"]);
    // The gammapad virtual pad has its own file; another pad's file is not this one's.
    r = build(env("flycast/cube", gammapadDev()), game("flycast", "dreamcast"), cube(true), cat());
    EXPECT_EQ(Source::kDefaults, r.source);
    r = build(env("flycast/xbox", gammapadDev()), game("flycast", "dreamcast"), cube(true), cat());
    EXPECT_EQ(Source::kConfig, r.source);
    EXPECT_EQ("B", rows(r, cube(true))["btn-a"]);   // 96 through gammapad: the bottom button
    r = build(env("flycast/xbox"), game("flycast", "dreamcast"), pad, cat());
    EXPECT_EQ(Source::kDefaults, r.source);
    EXPECT_EQ("B", rows(r, pad)["btn-a"]);
    EXPECT_EQ("Select", hotkeys(r)["Flycast menu"]);
}

// ---- ARMSX2 ----

TEST(ButtonMapArmsx2, PartialPrefsFirstActionWinsAndPerGame) {
    const PadModel pad = cube();
    MapResult r = build(env("armsx2/partial"), game("armsx2", "ps2"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-cross"]);
    EXPECT_EQ("B", m["btn-circle"]);
    EXPECT_EQ("\xE2\x80\x94", m["btn-square"]);   // 97 is Cross's already
    EXPECT_EQ("\xE2\x80\x94", m["btn-l3"]);       // 0: unbound
    EXPECT_EQ("X", m["btn-triangle"]);
    EXPECT_EQ("L2", m["btn-l2"]);
    EXPECT_EQ("D-pad / Left stick", m["dpad"]);   // the left stick in D-pad mode
    EXPECT_EQ("R3", hotkeys(r)["Save state"]);
    bool macros = false;
    for (const auto& n : r.notes) macros = macros || n == "Macros not shown";
    EXPECT_TRUE(macros);
    r = build(env("armsx2/partial"), game("armsx2", "ps2", "", "/storage/emulated/0/ROMs/ps2/Ico (USA).iso"), pad, cat());
    EXPECT_EQ("L1", rows(r, pad)["btn-triangle"]);
    nanotest::TempDir t("bmaparmsx2");
    ReaderEnv e;
    e.root = realDir(t);
    EXPECT_EQ(Source::kDefaults, readArmsx2(e, game("armsx2", "ps2")).source);
}

// ---- Vita3K ----

TEST(ButtonMapVita3k, ThroughSdl) {
    const PadModel pad = cube();
    auto m = rows(build(env("vita3k/swapped"), game("vita3k", "vita"), pad, cat()), pad);
    EXPECT_EQ("A", m["btn-cross"]);   // binds[0] = 1: SDL EAST, keycode 97
    EXPECT_EQ("B", m["btn-circle"]);
    EXPECT_EQ("Left stick", m["stick-l"]);
    nanotest::TempDir t("bmapvita");
    ReaderEnv e;
    e.root = realDir(t);
    const MapResult d = build(e, game("vita3k", "vita"), pad, cat());
    EXPECT_EQ(Source::kDefaults, d.source);
    EXPECT_EQ("B", rows(d, pad)["btn-cross"]);
}

// ---- WatermelonDS ----

TEST(ButtonMapWatermelonDs, Assignments) {
    const PadModel pad = cube();
    const MapResult r = build(env("watermelonds/custom"), game("watermelonds", "nds"), pad, cat());
    EXPECT_EQ(Source::kConfig, r.source);
    auto m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);
    EXPECT_EQ("A", m["btn-b"]);
    EXPECT_EQ("\xE2\x80\x94", m["btn-x"]);              // 96 is A's: the first input in list order takes it
    EXPECT_EQ("D-pad Up", m["dpad-up"]);                // the other controller's axis is not this pad's
    EXPECT_EQ("Select + L1", m["btn-l"]);               // a chord
    EXPECT_EQ("\xE2\x80\x94", m["btn-start"]);          // missing from the file: unbound
    EXPECT_EQ("R1", hotkeys(r)["Save state"]);
}

TEST(ButtonMapWatermelonDs, UnknownInputOrBrokenFileMeansDefaults) {
    const PadModel pad = cube();
    for (const char* c : {"watermelonds/unknown", "watermelonds/broken"}) {
        const MapResult r = build(env(c), game("watermelonds", "nds"), pad, cat());
        EXPECT_EQ(Source::kDefaults, r.source) << c;
        auto m = rows(r, pad);
        EXPECT_EQ("A", m["btn-a"]) << c;   // DS A on the right button, as the app's defaults put it
        EXPECT_EQ("B", m["btn-b"]) << c;
        EXPECT_EQ("Anbernic", hotkeys(r)["Pause menu"]);
    }
}

// ---- drastic-nano and DraStic ----

TEST(ButtonMapDrastic, NanoSeedPropsAndOverride) {
    const PadModel pad = cube();
    std::map<std::string, std::string> props;
    ReaderEnv e = env("drastic-nano/override");
    e.prop = [&](const std::string& k) { auto it = props.find(k); return it == props.end() ? std::string() : it->second; };
    MapResult r = build(e, game("drastic-nano", "nds"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);   // 96 here is evdev 304: the right button, printed A
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("X", m["btn-x"]);
    EXPECT_EQ("Y", m["btn-y"]);
    EXPECT_EQ("D-pad / Left stick", m["dpad"]);   // the hat and the stick always drive it
    auto h = hotkeys(r);
    EXPECT_EQ("L2", h["Swap screens"]);
    EXPECT_EQ("R3", h["Touch cursor"]);
    // A code bound twice: the higher slot wins (Menu, 20, over A, 3).
    props["persist.gammaos.drastic_nano.key.20"] = "96";
    r = build(e, game("drastic-nano", "nds"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("\xE2\x80\x94", m["btn-a"]);   // and BTN_SOUTH, taken, gets no fallback
    EXPECT_EQ("A", hotkeys(r)["DraStic menu"]);
    props.clear();
    // The game's override swaps A and B and puts fast forward on evdev 312.
    r = build(e, game("drastic-nano", "nds", "", "/storage/emulated/0/ROMs/nds/Mario Kart DS.nds"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);
    EXPECT_EQ("A", m["btn-b"]);
    EXPECT_EQ("L2", hotkeys(r)["Fast forward"]);
    EXPECT_EQ("Game override: Mario Kart DS.cfg", r.notes.at(0));
}

// drastic-nano plays the face buttons by position through ABXY Swap; the pad model names gammapad's
// buttons by print, so with the swap on DS A is the bottom button.
TEST(ButtonMapDrastic, NanoFollowsAbxySwap) {
    std::map<std::string, std::string> props = {{"init.svc.gammapad", "running"}};
    ReaderEnv e = env("drastic-nano/override", gammapadDev());
    e.prop = [&](const std::string& k) { auto it = props.find(k); return it == props.end() ? std::string() : it->second; };
    MapResult r = build(e, game("drastic-nano", "nds"), cube(true), cat());
    auto m = rows(r, cube(true));
    EXPECT_EQ("A", m["btn-a"]);
    EXPECT_EQ("B", m["btn-b"]);
    props["persist.gammaos.gamepad.abxy_swap"] = "1";
    r = build(e, game("drastic-nano", "nds"), cube(true, true), cat());
    m = rows(r, cube(true, true));
    EXPECT_EQ("B", m["btn-a"]);
    EXPECT_EQ("A", m["btn-b"]);
    EXPECT_EQ("Y", m["btn-x"]);
    EXPECT_EQ("X", m["btn-y"]);
}

TEST(ButtonMapDrastic, AppProfileZero) {
    const PadModel pad = cube();
    // Built here: Soong refuses a data file with '$' in its name.
    nanotest::TempDir prefs("bmapdrasticp");
    ReaderEnv pe;
    pe.root = realDir(prefs);
    pe.dev = cubeDev();
    writeFile(pe.root + "/data/data/com.dsemu.drastic/shared_prefs/_Dra$t1c_Pref$_.xml",
              "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n"
              "    <int name=\"_KeyMapConfigs_0_3\" value=\"97\" />\n"
              "    <int name=\"_KeyMapConfigs_0_2\" value=\"96\" />\n"
              "    <int name=\"_KeyMapConfigs_0_17\" value=\"105\" />\n"
              "    <int name=\"_KeyMapConfigs_1_3\" value=\"100\" />\n</map>\n");
    const MapResult r = build(pe, game("drastic", "nds"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);   // DraStic's _KeyMapConfigs_0_3 = 97, an Android keycode
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("Mapping read from DraStic profile 1 (assumed)", r.notes.at(0));
    nanotest::TempDir t("bmapdrastic");
    ReaderEnv e;
    e.root = realDir(t);
    const MapResult d = build(e, game("drastic", "nds"), pad, cat());
    EXPECT_EQ(Source::kDefaults, d.source);
    EXPECT_EQ("B", rows(d, pad)["btn-a"]);   // DraStic's default puts DS A on BUTTON_A, the bottom
}

// ---- M64Plus FZ ----

TEST(ButtonMapM64, UserProfileAndBuiltIn) {
    const PadModel pad = cube();
    MapResult r = build(env("m64/user"), game("mupen64plus-fz", "n64"), pad, cat());
    EXPECT_EQ(Source::kConfig, r.source);
    auto m = rows(r, pad);
    EXPECT_EQ("A / Y", m["btn-a"]);   // two codes for one command
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("L2", m["btn-z"]);
    EXPECT_EQ("Left stick", m["stick-l"]);
    EXPECT_EQ("Right stick Right", m["c-right"]);
    EXPECT_EQ("Anbernic", hotkeys(r)["M64Plus menu"]);
    r = build(env("m64/builtin"), game("mupen64plus-fz", "n64"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);   // "Android Gamepad": A on 96, the bottom button
    EXPECT_EQ("Y", m["btn-b"]);   // B on 99
    EXPECT_EQ("Profile: Android Gamepad", r.notes.at(0));
    EXPECT_EQ("A / X", hotkeys(r)["M64Plus menu"]);   // 97 and 100 (and Escape, not on the pad)
}

TEST(ButtonMapM64, PerGameProfileByRomMd5) {
    nanotest::TempDir t("bmapm64");
    const std::string r = realDir(t);
    const std::string rom = "N64 ROM BYTES\n";
    writeFile(r + "/data/media/0/ROMs/n64/Game.z64", rom);
    unsigned char d[MD5_DIGEST_LENGTH];
    MD5((const unsigned char*)rom.data(), rom.size(), d);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    const std::string prefs = r + "/data/data/org.mupen64plusae.v3.fzurita/shared_prefs/";
    writeFile(prefs + std::string(hex) + "_preferences.xml",
              "<map>\n    <string name=\"controllerProfile1Game\">Xbox</string>\n</map>\n");
    ReaderEnv e;
    e.root = r;
    const Context c = game("mupen64plus-fz", "n64", "", "/storage/emulated/0/ROMs/n64/Game.z64");
    ReadResult rr = readM64PlusFz(e, c);
    EXPECT_EQ("Game profile: Xbox", rr.notes.at(0));
    EXPECT_EQ(Input::key(96), bind(rr, "btn-a")->in.at(0));
    writeFile(prefs + std::string(hex) + "_preferences.xml", "<map>\n    <string name=\"controllerProfile1Game\"></string>\n</map>\n");
    rr = readM64PlusFz(e, c);
    EXPECT_EQ(Source::kNotMapped, rr.source);
}

// ---- SeedlessDS ----

TEST(ButtonMapSeedless, ProfilesAndOldVersion) {
    const PadModel pad = cube();
    MapResult r = build(env("seedless/p2"), game("seedlessds", "nds"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);                    // profile 2's p2_k_4 = 97
    EXPECT_EQ("B", m["btn-b"]);
    EXPECT_EQ("\xE2\x80\x94", m["btn-x"]);          // 96 is B's: the first in list order wins
    EXPECT_EQ("\xE2\x80\x94", m["btn-l"]);          // cleared, and _AnalogTriggers is off
    EXPECT_EQ("D-pad / Left stick", m["dpad"]);
    EXPECT_EQ("Key map profile 3", r.notes.at(0));
    r = build(env("seedless/oldver"), game("seedlessds", "nds"), pad, cat());
    m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);                    // _KeyMapVer 1: the app erases k_ keys
    EXPECT_EQ("L1 / L2", m["btn-l"]);              // the trigger presses L too
}

// ---- hakuX ----

TEST(ButtonMapHakuX, UnitThenModelThenDefaults) {
    const PadModel pad = cube();
    MapResult r = build(env("hakux/profiles"), game("hakux", "xbox"), pad, cat());
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["btn-a"]);   // the unit's profile: k97
    EXPECT_EQ("Left stick Up", m["stick-l:up"]);   // a whole axis: both halves
    EXPECT_EQ("Left stick Down", m["stick-l:down"]);
    EXPECT_EQ("R2", m["btn-rt"]);
    EXPECT_EQ("Profile for this controller", r.notes.at(0));
    r = build(env("hakux/profiles", gammapadDev()), game("hakux", "xbox"), cube(true), cat());
    EXPECT_EQ("Profile for this model", r.notes.at(0));
    EXPECT_EQ("B", rows(r, cube(true))["btn-a"]);   // k96 through gammapad: the bottom button
    DeviceId other;
    other.vendor = 0x1234;
    other.name = "Other";
    r = build(env("hakux/profiles", other), game("hakux", "xbox"), pad, cat());
    EXPECT_EQ(Source::kDefaults, r.source);
    m = rows(r, pad);
    EXPECT_EQ("B", m["btn-a"]);
    EXPECT_EQ("Right stick", m["stick-r"]);
}

// ---- ScummVM ----

TEST(ButtonMapScummvm, EngineDefaultsAndOverrides) {
    const PadModel pad = cube();
    MapResult r = build(env("scummvm/keymap"), game("scummvm", "scummvm"), pad, cat());
    EXPECT_EQ("", r.drawing);   // a pointer-and-click table
    auto m = rows(r, pad);
    EXPECT_EQ("A", m["Left click"]);       // JOY_B = 97: the right button
    EXPECT_EQ("B", m["Right click"]);      // JOY_A plus the keyboard's F2
    EXPECT_EQ("\xE2\x80\x94", m["Skip line"]);  // set empty: unbound
    EXPECT_EQ("X", m["Skip"]);
    EXPECT_EQ("Left stick", m["Move the pointer"]);
    nanotest::TempDir t("bmapscumm");
    ReaderEnv e;
    e.root = realDir(t);
    r = build(e, game("scummvm", "scummvm"), pad, cat());
    EXPECT_EQ(Source::kDefaults, r.source);
    EXPECT_EQ("B", rows(r, pad)["Left click"]);
}

TEST(ButtonMapReaders, UnknownEmulator) {
    const MapResult r = build(env("retroarch/shipped"), game("whatever", "snes"), cube(), cat());
    EXPECT_EQ(Source::kUnknown, r.source);
    EXPECT_TRUE(r.rows.empty());
}

// Every reader on an empty root, and on a root of junk files: no crash, and a result.
TEST(ButtonMapReaders, EmptyAndJunkRoots) {
    nanotest::TempDir t("bmapjunk");
    const std::string r = realDir(t);
    ReaderEnv e;
    e.root = r;
    e.dev = cubeDev();
    const char* const kEmus[] = {"retroarch", "ppsspp", "dolphin", "azahar", "flycast", "armsx2", "vita3k", "watermelonds",
                                 "drastic-nano", "drastic", "mupen64plus-fz", "scummvm", "seedlessds", "hakux"};
    for (const char* emu : kEmus) {
        const MapResult m = build(e, game(emu, defaultSystemFor(emu).empty() ? "snes" : defaultSystemFor(emu), "snes9x"), cube(), cat());
        EXPECT_NE(Source::kUnknown, m.source) << emu;
    }
    const std::string junk = std::string("\x00\xff[<map>\"\"{{{{[[[", 16) + std::string(5000, '<');
    for (const char* f : {"/data/media/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg", "/data/media/0/PSP/SYSTEM/controls.ini",
                          "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/Config/GCPadNew.ini",
                          "/data/data/org.azahar_emu.azahar/shared_prefs/org.azahar_emu.azahar_preferences.xml",
                          "/data/data/com.armsx2/shared_prefs/ARMSX2.xml", "/data/data/me.magnum.melondualds/files/controller_config.json",
                          "/data/data/com.dsemu.drastic/shared_prefs/_Dra$t1c_Pref$_.xml",
                          "/data/data/org.mupen64plusae.v3.fzurita/files/Profiles/controller.cfg",
                          "/data/data/org.scummvm.scummvm/files/scummvm.ini", "/data/data/com.seedlessds.app/shared_prefs/keymap.xml",
                          "/data/data/com.rfandango.haku_x/files/controllers.json",
                          "/data/media/0/Android/data/org.vita3k.emulator/files/config.yml"})
        writeFile(r + f, junk);
    for (const char* emu : kEmus)
        build(e, game(emu, defaultSystemFor(emu).empty() ? "snes" : defaultSystemFor(emu), "snes9x"), cube(), cat());
}

// Every note, hotkey and action row a reader writes on the fixtures is a string the panel translates
// (bmap::uiStrings, which the i18n check holds every language to).
TEST(ButtonMapRead, EveryReaderTextIsTranslatable) {
    auto marker = [](const char* s) -> const char* {
        static std::map<std::string, std::string> seen;
        std::string& v = seen[s];
        if (v.empty()) v = std::string("<") + s + ">";
        return v.c_str();
    };
    const std::vector<std::pair<std::string, std::vector<std::string>>> emus = {
        {"retroarch", {"retroarch"}}, {"ppsspp", {"ppsspp"}}, {"dolphin", {"dolphin"}}, {"azahar", {"azahar"}},
        {"flycast", {"flycast"}}, {"armsx2", {"armsx2"}}, {"vita3k", {"vita3k"}}, {"watermelonds", {"watermelonds"}},
        {"drastic-nano", {"drastic-nano", "drastic"}}, {"m64", {"mupen64plus-fz"}}, {"scummvm", {"scummvm"}},
        {"seedless", {"seedlessds"}}, {"hakux", {"hakux"}}};
    const std::vector<std::pair<std::string, std::string>> raCores = {
        {"snes9x", "snes"}, {"mgba", "gba"}, {"mesen", "nes"}, {"genesis_plus_gx", "genesis"},
        {"mednafen_psx_hw", "psx"}, {"mupen64plus_next_gles3", "n64"}, {"fbneo", "arcade"}};
    int checked = 0;
    auto check = [&](const std::string& what, const std::string& text) {
        if (text.empty()) return;
        checked++;
        EXPECT_NE(text, translate(text, marker)) << what << ": \"" << text << "\" is not in bmap::uiStrings()";
    };
    for (const auto& e : emus) {
        DIR* d = opendir(nanotest::fixture("buttonmap/" + e.first).c_str());
        ASSERT_NE(nullptr, d) << e.first;
        std::vector<std::string> cases;
        while (dirent* de = readdir(d))
            if (de->d_name[0] != '.') cases.push_back(de->d_name);
        closedir(d);
        for (const auto& c : cases) {
            for (const auto& emu : e.second) {
                std::vector<Context> ctxs;
                if (emu == "retroarch") {
                    for (const auto& rc : raCores) ctxs.push_back(game(emu, rc.second, rc.first, "/sdcard/ROMs/x/game.bin"));
                } else {
                    ctxs.push_back(game(emu, defaultSystemFor(emu)));
                }
                for (const auto& ctx : ctxs) {
                    const ReadResult rr = readEmulator(env(e.first + "/" + c), ctx, cat());
                    const std::string where = e.first + "/" + c + " " + emu + " " + ctx.core;
                    for (const auto& n : rr.notes) check(where + " note", n);
                    for (const auto& h : rr.hotkeys) check(where + " hotkey", h.action);
                    // A console's printed name ("Z", the PS2's "Analog") and a binding's own words (a
                    // Dolphin expression, another pad's name) are shown as they are.
                    for (const auto& b : rr.binds) {
                        if (b.id.empty() && emu != "retroarch" && b.name.size() > 2 && b.name != "Analog")
                            check(where + " row", b.name);
                        for (const auto& in : b.in)
                            if (in.kind == Input::kOther && (in.text.rfind("Keyboard ", 0) == 0 || in.text == "Another controller"))
                                check(where + " bound to", in.text);
                    }
                }
            }
        }
    }
    EXPECT_GT(checked, 40);
}
