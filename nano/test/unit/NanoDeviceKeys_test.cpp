#include <gtest/gtest.h>

#include <map>

#include <android-base/file.h>

#include "NanoDeviceKeys.h"
#include "NanoJson.h"
#include "nano_test_util.h"

using namespace android;

namespace {

std::function<std::string(const std::string&)> props(std::map<std::string, std::string> m) {
    return [m](const std::string& k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
}

std::string slurp(const std::string& path) {
    std::string s;
    EXPECT_TRUE(base::ReadFileToString(path, &s)) << path;
    return s;
}

}  // namespace

TEST(DeviceKeys, BuiltinIsTheRgCubes) {
    const devkeys::Map m = devkeys::builtin();
    EXPECT_EQ(m.id, "builtin");
    EXPECT_TRUE(m.isFunction(68));    // KEY_F10, the Cube's Anbernic button
    EXPECT_TRUE(m.isFunction(316));   // BTN_MODE
    EXPECT_FALSE(m.isFunction(158));
    EXPECT_TRUE(m.isBack(158));
    EXPECT_FALSE(m.sharedBack);
    EXPECT_FALSE(m.isModifier(158));
    ASSERT_EQ(m.function.size(), 2u);
    EXPECT_TRUE(m.function[0].gamepadOnly);
    EXPECT_EQ(m.function[0].android, "F10");
    EXPECT_EQ(m.functionPrompt(), "Anbernic");
}

TEST(DeviceKeys, MalformedTableIsTheBuiltin) {
    for (const char* bad : {"", "{", "[]", "null", "{\"devices\": 7"}) {
        const devkeys::Map m = devkeys::resolve(bad, props({{"ro.product.vendor.model", "RGCUBE"}}));
        EXPECT_EQ(m.id, "builtin") << bad;
        EXPECT_EQ(m.modifierCodes(), (std::vector<int>{68, 316})) << bad;
    }
}

TEST(DeviceKeys, EmptyTableIsTheDefault) {
    const devkeys::Map m = devkeys::resolve("{}", props({}));
    EXPECT_EQ(m.id, "default");
    EXPECT_EQ(m.modifierCodes(), (std::vector<int>{68, 316}));
    EXPECT_TRUE(m.isBack(158));
}

TEST(DeviceKeys, SharedBackMakesBackAModifier) {
    const char* json = R"({"devices": [{"id": "t618", "match": {"ro.board.platform": "ums512"},
                           "function": [], "fallback": "back"}]})";
    const devkeys::Map m = devkeys::resolve(json, props({{"ro.board.platform", "ums512"}}));
    EXPECT_EQ(m.id, "t618");
    EXPECT_TRUE(m.sharedBack);
    EXPECT_FALSE(m.isFunction(158));
    EXPECT_TRUE(m.isModifier(158));
    EXPECT_FALSE(m.isModifier(68));
}

// The cases PhoneWindowManager's resolver (ThimsyDeviceKeys) is held to as well.
TEST(DeviceKeys, SharedCases) {
    njson::Value doc;
    ASSERT_TRUE(njson::parse(slurp(nanotest::fixture("devkeys/cases.json")), &doc));
    const std::string table = slurp(nanotest::fixture("devkeys/" + doc.getString("table")));
    const njson::Value* cases = doc.find("cases");
    ASSERT_TRUE(cases && cases->isArray() && !cases->arr.empty());
    auto ints = [](const njson::Value& c, const char* key) {
        std::vector<int> v;
        if (const njson::Value* a = c.find(key)) for (const auto& e : a->arr) v.push_back(e.asInt());
        return v;
    };
    for (const auto& c : cases->arr) {
        const std::string name = c.getString("name");
        std::map<std::string, std::string> p;
        if (const njson::Value* pv = c.find("props")) for (const auto& [k, v] : pv->obj) p[k] = v.str;
        const devkeys::Map m = devkeys::resolve(table, props(p));
        EXPECT_EQ(m.id, c.getString("id")) << name;
        std::vector<int> fnLinux, backLinux;
        std::vector<std::string> fnAndroid, wantAndroid;
        std::vector<bool> gp, wantGp;
        for (const auto& k : m.function) { fnLinux.push_back(k.linuxCode); fnAndroid.push_back(k.android); gp.push_back(k.gamepadOnly); }
        for (const auto& k : m.back) backLinux.push_back(k.linuxCode);
        for (const auto& e : c.find("function_android")->arr) wantAndroid.push_back(e.str);
        for (const auto& e : c.find("gamepad_only")->arr) wantGp.push_back(e.b);
        EXPECT_EQ(fnLinux, ints(c, "function_linux")) << name;
        EXPECT_EQ(fnAndroid, wantAndroid) << name;
        EXPECT_EQ(gp, wantGp) << name;
        EXPECT_EQ(backLinux, ints(c, "back_linux")) << name;
        EXPECT_EQ(m.sharedBack, c.getBool("shared_back")) << name;
        EXPECT_EQ(m.modifierCodes(), ints(c, "modifiers")) << name;
        EXPECT_EQ(m.hasHomeFunction(), c.getBool("home_function")) << name;
        std::vector<int> homeLinux;
        for (int code : fnLinux) if (m.isHomeFunction(code)) homeLinux.push_back(code);
        EXPECT_EQ(homeLinux, ints(c, "home_linux")) << name;
        ASSERT_TRUE(c.find("function_prompt")) << name;
        EXPECT_EQ(m.functionPrompt(), c.getString("function_prompt")) << name;
    }
}

// Where Back doubles as the function button the chords are made with Back, so prompts name Back's
// button even when the table lists a function key that no button sends (the RG405M's F10).
TEST(DeviceKeys, PromptNamesTheChordButton) {
    const char* json = R"js({"devices": [
        {"id": "t618", "match": {"ro.board.platform": "ums512"},
         "function": [{"label": "F10 (no button)", "linux": 68, "android": "F10"}],
         "back": [{"label": "Home / Back", "prompt": "Back", "linux": 158, "android": "BACK"}],
         "fallback": "back"},
        {"id": "brick", "match": {"ro.board.platform": "ceres"}, "function": [],
         "back": [{"label": "Menu", "linux": 158, "android": "BACK"}], "fallback": "back"},
        {"id": "none", "match": {"ro.board.platform": "none"}, "function": []}]})js";
    EXPECT_EQ(devkeys::resolve(json, props({{"ro.board.platform", "ums512"}})).functionPrompt(), "Back");
    EXPECT_EQ(devkeys::resolve(json, props({{"ro.board.platform", "ceres"}})).functionPrompt(), "Menu");
    EXPECT_EQ(devkeys::resolve(json, props({{"ro.board.platform", "none"}})).functionPrompt(), "Anbernic");
    EXPECT_EQ(devkeys::resolve(json, props({})).functionPrompt(), "Anbernic");
}

TEST(DeviceKeys, HomeFunctionIsTheAndroidHomeKeyOnly) {
    const devkeys::Map cube = devkeys::builtin();
    EXPECT_FALSE(cube.hasHomeFunction());
    for (int code : {68, 316, 172, 158, -1}) EXPECT_FALSE(cube.isHomeFunction(code)) << code;
    const char* json = R"({"default": {"function": [{"linux": 316, "android": "BUTTON_MODE"},
                                                  {"linux": 172, "android": "HOME"},
                                                  {"label": "no code", "android": "HOME"}],
                                     "back": [{"linux": 172, "android": "HOME"}]}})";
    const devkeys::Map m = devkeys::resolve(json, props({}));
    EXPECT_TRUE(m.hasHomeFunction());
    EXPECT_TRUE(m.isHomeFunction(172));
    EXPECT_FALSE(m.isHomeFunction(316));
    EXPECT_FALSE(m.isHomeFunction(-1));   // the HOME entry with no evdev code is PhoneWindowManager's only
    // A Back key that is HOME is not a function key.
    const devkeys::Map back = devkeys::resolve(R"({"default": {"function": [],
        "back": [{"linux": 172, "android": "HOME"}]}})", props({}));
    EXPECT_FALSE(back.hasHomeFunction());
    EXPECT_FALSE(back.isHomeFunction(172));
}

// The function button let go, and held 3 s; the same cases hold ThimsyDeviceKeys.
TEST(DeviceKeys, FunctionReleaseAndHold) {
    njson::Value doc;
    ASSERT_TRUE(njson::parse(slurp(nanotest::fixture("devkeys/cases.json")), &doc));
    const njson::Value* release = doc.find("function_release");
    ASSERT_TRUE(release && release->isArray() && !release->arr.empty());
    for (const auto& c : release->arr) {
        const devkeys::FunctionRelease r = devkeys::functionRelease(
                c.getBool("home"), c.getBool("hotkeys"), c.getBool("used"), c.getInt("held"));
        EXPECT_EQ(r == devkeys::FunctionRelease::QuickMenu ? "quick_menu" : "nothing", c.getString("want"))
                << "home " << c.getBool("home") << " hotkeys " << c.getBool("hotkeys") << " used "
                << c.getBool("used") << " held " << c.getInt("held");
    }
    const njson::Value* hold = doc.find("function_hold");
    ASSERT_TRUE(hold && hold->isArray() && !hold->arr.empty());
    for (const auto& c : hold->arr) {
        const devkeys::FunctionHold h = devkeys::functionHold(c.getBool("home"), c.getBool("hotkeys"));
        const char* got = h == devkeys::FunctionHold::Home   ? "home"
                          : h == devkeys::FunctionHold::Quit ? "quit"
                                                             : "nothing";
        EXPECT_EQ(got, c.getString("want")) << "home " << c.getBool("home") << " hotkeys " << c.getBool("hotkeys");
    }
}

// Toolbox can't give a HOME key back, and Kiosk Mode never holds without the Quick Menu's button.
TEST(DeviceKeys, QuickMenuStaysReachable) {
    EXPECT_TRUE(devkeys::functionToSystem(false, true));
    EXPECT_FALSE(devkeys::functionToSystem(false, false));
    EXPECT_TRUE(devkeys::functionToSystem(true, false));
    EXPECT_TRUE(devkeys::functionToSystem(true, true));

    EXPECT_TRUE(devkeys::kioskInEffect(true, true));
    EXPECT_FALSE(devkeys::kioskInEffect(true, false));   // a setprop or an old state can't lock the menu away
    EXPECT_FALSE(devkeys::kioskInEffect(false, true));
    EXPECT_FALSE(devkeys::kioskInEffect(false, false));

    EXPECT_FALSE(devkeys::kioskAfterSwitch(false, true));   // the switch off turns Kiosk Mode off
    EXPECT_FALSE(devkeys::kioskAfterSwitch(false, false));
    EXPECT_TRUE(devkeys::kioskAfterSwitch(true, true));
    EXPECT_FALSE(devkeys::kioskAfterSwitch(true, false));

    EXPECT_TRUE(devkeys::switchAfterKiosk(true, false));    // Kiosk Mode on turns the switch on
    EXPECT_TRUE(devkeys::switchAfterKiosk(true, true));
    EXPECT_FALSE(devkeys::switchAfterKiosk(false, false));  // Kiosk Mode off leaves it as it was
    EXPECT_TRUE(devkeys::switchAfterKiosk(false, true));

    // Whatever order the two are set in, the result never has Kiosk Mode on without the button.
    for (int kiosk = 0; kiosk < 2; kiosk++) {
        for (int sw = 0; sw < 2; sw++) {
            const bool k1 = devkeys::kioskAfterSwitch(sw, kiosk);
            EXPECT_FALSE(k1 && !sw);
            const bool s2 = devkeys::switchAfterKiosk(kiosk, sw);
            EXPECT_FALSE(kiosk && !s2);
        }
    }
}

// Back acts on release; the same cases hold ThimsyDeviceKeys.backRelease.
TEST(DeviceKeys, BackRelease) {
    njson::Value doc;
    ASSERT_TRUE(njson::parse(slurp(nanotest::fixture("devkeys/cases.json")), &doc));
    const njson::Value* cases = doc.find("back_release");
    ASSERT_TRUE(cases && cases->isArray() && !cases->arr.empty());
    for (const auto& c : cases->arr) {
        const devkeys::BackRelease r = devkeys::backRelease(c.getBool("shared"), c.getBool("used"),
                                                            c.getInt("held"));
        const char* got = r == devkeys::BackRelease::Back        ? "back"
                          : r == devkeys::BackRelease::QuickMenu ? "quick_menu"
                                                                 : "nothing";
        EXPECT_EQ(got, c.getString("want"))
                << "shared " << c.getBool("shared") << " used " << c.getBool("used") << " held " << c.getInt("held");
    }
}
