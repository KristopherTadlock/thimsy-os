#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "NanoEmuSync.h"

using namespace android;
using emusync::Act;

TEST(EmuSync, NeverSyncedTheCardWins) {
    EXPECT_EQ(emusync::decide(false, -1, -1, 100, 200), Act::ToApp);    // a reinstall: the card's settings
    EXPECT_EQ(emusync::decide(false, -1, -1, -1, 200), Act::ToApp);
    EXPECT_EQ(emusync::decide(false, -1, -1, 100, -1), Act::ToCard);    // a card new to them: the app's go there
    EXPECT_EQ(emusync::decide(false, -1, -1, -1, -1), Act::None);
}

TEST(EmuSync, ACardNewToThisDeviceCanTakeItsSettings) {
    EXPECT_EQ(emusync::decide(false, -1, -1, 100, 200, true), Act::ToCard);
    EXPECT_EQ(emusync::decide(false, -1, -1, -1, 200, true), Act::ToApp);   // what this device lacks still comes
    EXPECT_EQ(emusync::decide(false, -1, -1, 100, 100, true), Act::None);
    EXPECT_EQ(emusync::decide(true, 100, 200, 100, 300, true), Act::ToApp);  // once synced, as always
}

TEST(EmuSync, WhatChangedGoesAcross) {
    EXPECT_EQ(emusync::decide(true, 100, 200, 100, 200), Act::None);
    EXPECT_EQ(emusync::decide(true, 100, 200, 100, 300), Act::ToApp);   // edited on the card
    EXPECT_EQ(emusync::decide(true, 100, 200, 150, 200), Act::ToCard);  // changed in the emulator
    EXPECT_EQ(emusync::decide(true, 100, 200, 150, 300), Act::ToApp);   // both: the card wins
}

TEST(EmuSync, DeletionsGoAcross) {
    EXPECT_EQ(emusync::decide(true, 100, 200, 100, -1), Act::DeleteApp);    // deleted on the card
    EXPECT_EQ(emusync::decide(true, 100, 200, -1, 200), Act::DeleteCard);   // deleted by the emulator
    EXPECT_EQ(emusync::decide(true, 100, 200, 150, -1), Act::DeleteApp);    // deleted on the card, changed here: the card wins
    EXPECT_EQ(emusync::decide(true, 100, 200, -1, 300), Act::ToApp);        // deleted here, edited on the card
    EXPECT_EQ(emusync::decide(true, 100, 200, -1, -1), Act::None);          // gone from both
}

TEST(EmuSync, BaseRoundTrips) {
    emusync::Base b;
    b[emusync::baseKey("1234-ABCD", "scummvm", "files/scummvm.ini")] = {123456789012345LL, -1, 0};
    b[emusync::baseKey("1234-ABCD", "retroarch", "autoconfig/android/Xbox One.cfg")] = {1, 2, 18446744073709551615ULL};
    EXPECT_EQ(emusync::baseLoad(emusync::baseSave(b)), b);
    EXPECT_TRUE(emusync::baseLoad("garbage\nno\ttabs-enough\n").empty());
    EXPECT_TRUE(emusync::baseLoad("a\t1\t2\n").empty());   // the card copy's hash is part of every line
}

TEST(EmuSync, Paths) {
    for (const auto& it : emusync::items()) {
        if (std::string(it.rel) == "config.yml") {
            EXPECT_EQ(emusync::appPath(it), "/data/media/0/Android/data/org.vita3k.emulator/files/config.yml");
            EXPECT_EQ(emusync::cardPath("/storage/1234-ABCD", it),
                      "/storage/1234-ABCD/emulator_config/vita3k/app/external/config.yml");
        }
        if (std::string(it.rel) == "files/scummvm.ini") {
            EXPECT_EQ(emusync::appPath(it), "/data/data/org.scummvm.scummvm/files/scummvm.ini");
            EXPECT_EQ(emusync::cardPath("/storage/1234-ABCD", it),
                      "/storage/1234-ABCD/emulator_config/scummvm/app/files/scummvm.ini");
        }
    }
}

TEST(EmuSync, EveryShippedEmulatorKeepsItsAppSettings) {
    std::set<std::string> emus;
    for (const auto& it : emusync::items()) emus.insert(it.emu);
    for (const char* e : {"retroarch", "ppsspp", "m64plus", "dolphin", "armsx2", "scummvm", "vita3k", "azahar",
                          "watermelonds", "seedlessds", "hakux", "drastic"})
        EXPECT_TRUE(emus.count(e)) << e;
}

TEST(EmuSync, LeftoversAreNotSynced) {
    EXPECT_TRUE(emusync::skipFile("prefs.xml.bak"));
    EXPECT_TRUE(emusync::skipFile("prefs.xml.nanotmp"));
    EXPECT_TRUE(emusync::skipFile(".hidden"));
    EXPECT_TRUE(emusync::skipFile("x.tmp"));
    EXPECT_TRUE(emusync::skipFile("com.google.android.gms.appid.xml"));
    EXPECT_TRUE(emusync::skipFile("WebViewChromiumPrefs.xml"));
    EXPECT_TRUE(emusync::skipFile("FirebaseHeartBeatW0RFRkFVTFRd+MTo1.xml"));
    EXPECT_FALSE(emusync::skipFile("org.ppsspp.ppsspp_preferences.xml"));
    EXPECT_FALSE(emusync::skipFile("keymap.xml"));
    EXPECT_FALSE(emusync::skipFile("Xbox One Controller.cfg"));
}

TEST(EmuSync, VolumeIds) {
    EXPECT_TRUE(emusync::isVolumeId("1A2B-3C4D"));
    EXPECT_TRUE(emusync::isVolumeId("0123456789abcdef0123456789ABCDEF"));
    EXPECT_FALSE(emusync::isVolumeId("emulated"));
    EXPECT_FALSE(emusync::isVolumeId("primary"));
    EXPECT_FALSE(emusync::isVolumeId("1A2B3C4D"));
    EXPECT_FALSE(emusync::isVolumeId("1A2B-3C4G"));
}

TEST(EmuSync, SettingsNamingACardThatIsOutNameThisOne) {
    auto out = [](const std::string& v) { return v != "9999-9999"; };
    EXPECT_EQ(emusync::repoint("<string name=\"dir\">/storage/1111-2222/emulator_config/m64plus</string>", "AAAA-BBBB", out),
              "<string name=\"dir\">/storage/AAAA-BBBB/emulator_config/m64plus</string>");
    EXPECT_EQ(emusync::repoint("pref-path: /mnt/media_rw/1111-2222/emulator_config/vita3k/", "AAAA-BBBB", out),
              "pref-path: /mnt/media_rw/AAAA-BBBB/emulator_config/vita3k/");
    EXPECT_EQ(emusync::repoint("content://com.android.externalstorage.documents/tree/1111-2222%3Aemulator_config%2Fazahar",
                               "AAAA-BBBB", out),
              "content://com.android.externalstorage.documents/tree/AAAA-BBBB%3Aemulator_config%2Fazahar");
    EXPECT_EQ(emusync::repoint("tree%2F1111-2222%253Aemulator_config and /storage/1111-2222/emulator_config", "AAAA-BBBB", out),
              "tree%2FAAAA-BBBB%253Aemulator_config and /storage/AAAA-BBBB/emulator_config");
    // Left as they are: a card that is in, this card, other folders, internal storage.
    EXPECT_EQ(emusync::repoint("/storage/9999-9999/emulator_config/x", "AAAA-BBBB", out), "/storage/9999-9999/emulator_config/x");
    EXPECT_EQ(emusync::repoint("/storage/AAAA-BBBB/emulator_config/x", "AAAA-BBBB", out), "/storage/AAAA-BBBB/emulator_config/x");
    EXPECT_EQ(emusync::repoint("/storage/1111-2222/ROMs/psx", "AAAA-BBBB", out), "/storage/1111-2222/ROMs/psx");
    EXPECT_EQ(emusync::repoint("/storage/emulated/0/emulator_config/x", "AAAA-BBBB", out), "/storage/emulated/0/emulator_config/x");
    EXPECT_EQ(emusync::repoint("", "AAAA-BBBB", out), "");
}

TEST(EmuSync, FolderGrantsInSettings) {
    const std::string prefs =
        "<string name=\"roms\">content://com.android.externalstorage.documents/tree/1A2B-3C4D%3AROMs%2Fpsx</string>"
        "<string name=\"r2\">content://com.android.externalstorage.documents/tree/primary%3AROMs%2Fsnes/document/primary%3AROMs%2Fsnes</string>"
        "<string name=\"dup\">content://com.android.externalstorage.documents/tree/1A2B-3C4D%3AROMs%2Fpsx%2F</string>"
        "<string name=\"bad\">content://com.android.externalstorage.documents/tree/nocolon</string>";
    const auto u = emusync::treeUris(prefs);
    ASSERT_EQ(u.size(), 2u);
    EXPECT_EQ(u[0], std::make_pair(std::string("1A2B-3C4D"), std::string("ROMs/psx")));
    EXPECT_EQ(u[1], std::make_pair(std::string("primary"), std::string("ROMs/snes")));
    EXPECT_TRUE(emusync::treeUris("no grants").empty());
    // ScummVM's paths name the tree as Uri.encode writes it.
    const auto s = emusync::treeUris("[monkey1]\npath=/saf/primary%3AROMs%2Fscummvm/Monkey Island\nsavepath=/saf/1A2B-3C4D%3Asaves\n");
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0], std::make_pair(std::string("primary"), std::string("ROMs/scummvm")));
    EXPECT_EQ(s[1], std::make_pair(std::string("1A2B-3C4D"), std::string("saves")));
}

TEST(EmuSync, ProfilesAnAppUnpackedAreLeftOut) {
    std::vector<int64_t> t;
    for (int i = 0; i < 300; i++) t.push_back(1000070 + i / 10);   // unpacked over 30 s, into the next minute
    t.push_back(5000000);   // the user's own, later
    t.push_back(10);        // and one from before
    const auto u = emusync::unpackedTogether(t);
    EXPECT_EQ(u.size(), 300u);
    EXPECT_TRUE(std::find(u.begin(), u.end(), 300u) == u.end());
    EXPECT_TRUE(std::find(u.begin(), u.end(), 301u) == u.end());
    EXPECT_TRUE(emusync::unpackedTogether({1, 2, 3}).empty());   // a few files: all the user's
}

TEST(EmuSync, HashIsNeverUnknown) {
    EXPECT_NE(emusync::hash(""), 0u);
    EXPECT_NE(emusync::hash("a"), emusync::hash("b"));
}
