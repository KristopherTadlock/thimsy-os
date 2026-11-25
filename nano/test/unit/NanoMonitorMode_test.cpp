#include <gtest/gtest.h>

#include "NanoMonitorMode.h"

using namespace android;

namespace {

monmode::Mode mode(const char* text) {
    monmode::Mode m;
    EXPECT_TRUE(monmode::parseMode(text, &m)) << text;
    return m;
}

std::vector<std::string> texts(const std::vector<monmode::Mode>& modes) {
    std::vector<std::string> out;
    for (const auto& m : modes) out.push_back(m.text());
    return out;
}

// What `cmd display gammaos-monitor-mode list` prints (DisplayManagerService.gammaosListMonitorModes).
const char* kList =
    "monitor local:4619827259835644672\n"
    "key 0123456789abcdef\n"
    "held 2560x1440@60 user\n"
    "active 2560x1440@60\n"
    "auto 1920x1080@60\n"
    "stored 2560x1440@60\n"
    "revert 2560x1440@60\n"
    "trial none\n"
    "mode 3840x2160@30\n"
    "mode 2560x1440@60\n"
    "mode 1920x1080@120\n"
    "mode 1920x1080@60\n"
    "mode 1920x1080@59.94\n"
    "mode 1920x1080@50\n"
    "mode 1920x1080@24\n"
    "mode 1280x720@60\n";

}  // namespace

TEST(MonitorMode, ParsesModes) {
    monmode::Mode m;
    ASSERT_TRUE(monmode::parseMode("2560x1440@60", &m));
    EXPECT_EQ(m.width, 2560);
    EXPECT_EQ(m.height, 1440);
    EXPECT_EQ(m.rate, "60");
    EXPECT_EQ(m.text(), "2560x1440@60");
    ASSERT_TRUE(monmode::parseMode("1920x1080@59.94", &m));
    EXPECT_NEAR(m.hz(), 59.94f, 0.001f);
    EXPECT_EQ(monmode::sizeText(m), "1920 x 1080");
    // Anything that could not go on a command line as it is, or is no mode.
    for (const char* bad : {"", "none", "1920x1080", "1920x1080@", "x1080@60", "1920x@60", "1920x1080@60;rm",
                            "1920x1080@6 0", "1920x1080@-60", "1920x1080@.5", "1920x1080@60.", "1920x1080@1.2.3",
                            "123456x1080@60", "0x1080@60", "1920x1080@0", "@60x1080"}) {
        monmode::Mode keep = m;
        EXPECT_FALSE(monmode::parseMode(bad, &keep)) << bad;
        EXPECT_EQ(keep.text(), m.text()) << bad;
    }
    EXPECT_EQ(monmode::Mode().text(), "");
}

TEST(MonitorMode, MatchesRatesWithinHalfAHertz) {
    EXPECT_TRUE(monmode::sameMode(mode("1920x1080@60"), mode("1920x1080@59.94")));
    EXPECT_FALSE(monmode::sameMode(mode("1920x1080@60"), mode("1920x1080@50")));
    EXPECT_FALSE(monmode::sameMode(mode("1920x1080@60"), mode("1920x1200@60")));
    EXPECT_TRUE(monmode::sameSize(mode("1920x1080@60"), mode("1920x1080@24")));
    EXPECT_FALSE(monmode::sameSize(monmode::Mode(), monmode::Mode()));
}

TEST(MonitorMode, ReadsTheProperty) {
    monmode::Mode held;
    std::string why;
    ASSERT_TRUE(monmode::parseProp("2560x1440@60;trial", &held, &why));
    EXPECT_EQ(held.text(), "2560x1440@60");
    EXPECT_EQ(why, "trial");
    ASSERT_TRUE(monmode::parseProp("1920x1080@60;auto", &held, &why));
    EXPECT_EQ(why, "auto");
    EXPECT_FALSE(monmode::parseProp("", &held, &why));             // no monitor
    EXPECT_FALSE(monmode::parseProp("1920x1080@60", &held, &why));
    EXPECT_FALSE(monmode::parseProp("none;auto", &held, &why));
    EXPECT_EQ(held.text(), "1920x1080@60");                          // untouched by a failed parse
}

TEST(MonitorMode, ReadsTheList) {
    monmode::List l;
    ASSERT_TRUE(monmode::parseList(kList, &l));
    EXPECT_EQ(l.held.text(), "2560x1440@60");
    EXPECT_EQ(l.why, "user");
    EXPECT_EQ(l.active.text(), "2560x1440@60");
    EXPECT_EQ(l.autoMode.text(), "1920x1080@60");
    EXPECT_EQ(l.stored.text(), "2560x1440@60");
    ASSERT_EQ(l.modes.size(), 8u);
    EXPECT_EQ(l.modes.front().text(), "3840x2160@30");
    EXPECT_EQ(l.modes.back().text(), "1280x720@60");

    ASSERT_TRUE(monmode::parseList("held 1920x1080@60 auto\nactive none\nauto 1920x1080@60\nstored none\n"
                                   "trial none\nmode 1920x1080@60\n", &l));
    EXPECT_FALSE(l.active.valid());
    EXPECT_FALSE(l.stored.valid());
    EXPECT_EQ(l.modes.size(), 1u);

    monmode::List keep = l;
    EXPECT_FALSE(monmode::parseList("", &l));
    EXPECT_FALSE(monmode::parseList("Error: no monitor\n", &l));
    EXPECT_EQ(l.modes.size(), keep.modes.size());
}

TEST(MonitorMode, ReadsATrial) {
    monmode::Trial t;
    ASSERT_TRUE(monmode::parseTrial("trial 2560x1440@60 15 1920x1080@60\n", &t));
    EXPECT_EQ(t.mode.text(), "2560x1440@60");
    EXPECT_EQ(t.seconds, 15);
    EXPECT_EQ(t.revert.text(), "1920x1080@60");
    EXPECT_FALSE(monmode::parseTrial("Error: the monitor didn't switch to 2560x1440@60\n", &t));
    EXPECT_FALSE(monmode::parseTrial("trial 2560x1440@60 x 1920x1080@60\n", &t));
    EXPECT_FALSE(monmode::parseTrial("trial none\n", &t));
    EXPECT_FALSE(monmode::parseTrial("", &t));
}

TEST(MonitorMode, HidesSlowModesInDualScreen) {
    monmode::List l;
    ASSERT_TRUE(monmode::parseList(kList, &l));
    EXPECT_EQ(monmode::offered(l.modes, false).size(), 8u);
    const auto dual = texts(monmode::offered(l.modes, true));
    EXPECT_EQ(dual, (std::vector<std::string>{"2560x1440@60", "1920x1080@120", "1920x1080@60", "1920x1080@59.94",
                                              "1920x1080@50", "1280x720@60"}));
}

TEST(MonitorMode, OffersEachSizeOnce) {
    monmode::List l;
    ASSERT_TRUE(monmode::parseList(kList, &l));
    // At 60 Hz: every size at 60 Hz, 4K at the only rate it has.
    EXPECT_EQ(texts(monmode::sizeChoices(l.modes, mode("1920x1080@60"))),
              (std::vector<std::string>{"3840x2160@30", "2560x1440@60", "1920x1080@60", "1280x720@60"}));
    // At 120 Hz: 1080p keeps 120; the other sizes don't have it and take the rate nearest 60.
    EXPECT_EQ(texts(monmode::sizeChoices(l.modes, mode("1920x1080@120"))),
              (std::vector<std::string>{"3840x2160@30", "2560x1440@60", "1920x1080@120", "1280x720@60"}));
    // No current mode: the rate nearest 60 Hz (60 rather than 59.94).
    EXPECT_EQ(texts(monmode::sizeChoices(l.modes, monmode::Mode())),
              (std::vector<std::string>{"3840x2160@30", "2560x1440@60", "1920x1080@60", "1280x720@60"}));
    // Two rates as near to 60: the higher.
    const std::vector<monmode::Mode> tie = {mode("1920x1080@50"), mode("1920x1080@70")};
    EXPECT_EQ(texts(monmode::sizeChoices(tie, monmode::Mode())), (std::vector<std::string>{"1920x1080@70"}));
    EXPECT_TRUE(monmode::sizeChoices({}, mode("1920x1080@60")).empty());
}

TEST(MonitorMode, OffersTheRatesOfTheCurrentSize) {
    monmode::List l;
    ASSERT_TRUE(monmode::parseList(kList, &l));
    const auto rates = monmode::rateChoices(l.modes, mode("1920x1080@60"));
    EXPECT_EQ(texts(rates), (std::vector<std::string>{"1920x1080@120", "1920x1080@60", "1920x1080@59.94",
                                                      "1920x1080@50", "1920x1080@24"}));
    EXPECT_EQ(monmode::indexOf(rates, mode("1920x1080@60"), false), 1);
    EXPECT_EQ(monmode::indexOf(rates, mode("1920x1080@24"), false), 4);
    EXPECT_EQ(monmode::indexOf(rates, mode("1920x1080@100"), false), -1);
    EXPECT_EQ(monmode::indexOf(monmode::sizeChoices(l.modes, mode("1920x1080@60")), mode("1920x1080@24"), true), 2);
    EXPECT_TRUE(monmode::rateChoices(l.modes, monmode::Mode()).empty());
}
