// A chosen ES-DE theme set that is not installed (OA-37): the default stands in, the choice is
// remembered, and the set is looked for again at most every few seconds unless something prompts it.
// The stand-in follows each system's variant trigger, and the first load at boot waits for storage
// a little before the default stands in.

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

TEST(EsdeThemeFallback, TheChosenSetGoneFromUnderTheHomeIsLoadedAgain) {
    // The card with the set on it was pulled while the set was up: reload, and the default stands in.
    EXPECT_TRUE(loadedSetGone(true, "slate-es-de", "slate-es-de", kDef, false));
    EXPECT_EQ(setToLoad("slate-es-de", false, kDef, true), kDef);
    EXPECT_EQ(missingSet("slate-es-de", false, kDef), "slate-es-de");
}

TEST(EsdeThemeFallback, NothingIsGoneWhileTheChosenSetIsThereOrNotTheOneShown) {
    EXPECT_FALSE(loadedSetGone(true, "slate-es-de", "slate-es-de", kDef, true));    // still there
    EXPECT_FALSE(loadedSetGone(false, "slate-es-de", "slate-es-de", kDef, false));  // nothing loaded
    EXPECT_FALSE(loadedSetGone(true, kDef, "slate-es-de", kDef, false));            // already a stand-in
    EXPECT_FALSE(loadedSetGone(true, kDef, kDef, kDef, false));                     // the default itself
    EXPECT_FALSE(loadedSetGone(true, "", "", kDef, false));
}

TEST(EsdeThemeFallback, TheStandInsTriggerIsItsFirstVariantsWhenNoneIsChosen) {
    EXPECT_EQ(triggerBase("", false, "withVideos"), "withVideos");         // loaded with none chosen
    EXPECT_EQ(triggerBase("noVideos", true, "withVideos"), "noVideos");    // a variant it declares
    EXPECT_EQ(triggerBase("gone", false, "withVideos"), "withVideos");     // one it does not declare
    EXPECT_EQ(triggerBase("", false, ""), "");                             // it declares no variant
}

TEST(EsdeThemeFallback, TheStandInIsParsedWithTheTriggeredVariantOrItsOwnSelection) {
    EXPECT_EQ(standInVariant("", "withVideos", "noMedia"), "noMedia");     // a system without media
    EXPECT_EQ(standInVariant("", "withVideos", "withVideos"), "");         // no trigger: the load's key
    EXPECT_EQ(standInVariant("", "", ""), "");
}

TEST(EsdeThemeFallback, TheFirstLoadWaitsForStorageOnlyUnderTheBootScreenAndBriefly) {
    EXPECT_TRUE(waitForStorage(true, false, 20000, 0));                    // the boot sound is not ready
    EXPECT_TRUE(waitForStorage(true, false, 20000 + kStorageWaitMs - 1, 20000));
    EXPECT_FALSE(waitForStorage(true, false, 20000 + kStorageWaitMs, 20000));   // waited long enough
    EXPECT_FALSE(waitForStorage(true, true, 20000, 0));                    // storage is readable
    EXPECT_FALSE(waitForStorage(false, false, 20000, 0));                  // no boot screen over the home
}
