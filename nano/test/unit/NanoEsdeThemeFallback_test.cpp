// A chosen ES-DE theme set that is not installed (OA-37): the default stands in, the choice is
// remembered, and the set is looked for again at most every few seconds unless something prompts it.

#include <gtest/gtest.h>

#include "NanoEsdeThemeFallback.h"

using namespace android::esdethemefb;

static const std::string kDef = "colorful-simplified-es-de";

TEST(EsdeThemeFallback, LoadsTheChosenSetWhenItIsInstalled) {
    EXPECT_EQ(setToLoad("slate-es-de", true, kDef, true), "slate-es-de");
    EXPECT_EQ(setToLoad("slate-es-de", true, kDef, false), "slate-es-de");
    EXPECT_EQ(missingSet("slate-es-de", true, kDef), "");
}

TEST(EsdeThemeFallback, TheDefaultStandsInForAMissingSetAndTheSetIsRemembered) {
    EXPECT_EQ(setToLoad("slate-es-de", false, kDef, true), kDef);
    EXPECT_EQ(missingSet("slate-es-de", false, kDef), "slate-es-de");
}

TEST(EsdeThemeFallback, NothingLoadsWhenTheDefaultIsMissingTooButTheSetIsStillLookedFor) {
    EXPECT_EQ(setToLoad("slate-es-de", false, kDef, false), "");
    EXPECT_EQ(missingSet("slate-es-de", false, kDef), "slate-es-de");
}

TEST(EsdeThemeFallback, AMissingDefaultHasNothingToStandInForIt) {
    EXPECT_EQ(setToLoad(kDef, false, kDef, false), "");
    EXPECT_EQ(missingSet(kDef, false, kDef), "");
    EXPECT_EQ(setToLoad(kDef, true, kDef, true), kDef);
    EXPECT_EQ(missingSet(kDef, true, kDef), "");
}

TEST(EsdeThemeFallback, TheLookIsRateLimited) {
    EXPECT_FALSE(recheckDue("", 100000, 0));                       // nothing missing: never
    EXPECT_FALSE(recheckDue("slate-es-de", 1000, 0));
    EXPECT_FALSE(recheckDue("slate-es-de", kRecheckMs - 1, 0));
    EXPECT_TRUE(recheckDue("slate-es-de", kRecheckMs, 0));
    EXPECT_TRUE(recheckDue("slate-es-de", 70000, 60000));
    EXPECT_FALSE(recheckDue("slate-es-de", 60500, 60000));
}
