// The overlay raise for a chord: the button map alone over a game skips the XMB, and anything else
// shown later catches it up.

#include <gtest/gtest.h>

#include "NanoOverlayRaise.h"

using namespace android::overlayraise;

TEST(OverlayRaise, OnlyTheButtonMapOverAGameSkipsTheXmb) {
    EXPECT_TRUE(mapOnly(kSummonButtonMap, false));
    EXPECT_FALSE(mapOnly(kSummonButtonMap, true));   // raised as the launcher: the home is the menu
    for (int s : {kSummonNone, kSummonQuickMenu, kSummonApplications}) {
        EXPECT_FALSE(mapOnly(s, false)) << s;
        EXPECT_FALSE(mapOnly(s, true)) << s;
    }
}

TEST(OverlayRaise, CatchesUpOnceSomethingButTheMapShows) {
    EXPECT_FALSE(catchUp(true, true, true));     // the map is still all there is
    EXPECT_TRUE(catchUp(true, true, false));     // the map closed into the menu, or another summon
    EXPECT_FALSE(catchUp(true, false, false));   // hidden: the next raise prepares for itself
    EXPECT_FALSE(catchUp(false, true, false));   // already prepared
}

TEST(OverlayRaise, TheMapAloneDrawsNoScrimWave) {
    EXPECT_TRUE(scrimWave(true, false, false));
    EXPECT_TRUE(scrimWave(false, true, false));
    EXPECT_FALSE(scrimWave(false, false, false));
    EXPECT_FALSE(scrimWave(true, false, true));    // ps3bg::render would init it, unfrozen, for nothing
    EXPECT_FALSE(scrimWave(true, true, true));
}
