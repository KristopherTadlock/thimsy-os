#include <gtest/gtest.h>

#include "NanoTouch.h"

using namespace android;

TEST(TouchZones, TapWithinSlopFires) {
  TouchZones z; int taps = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; }); z.endFrame();
  z.down(10, 10, 1000); z.move(15, 15, 1010); z.up(1020);
  EXPECT_EQ(taps, 1);
}
TEST(TouchZones, DragPastSlopIsNotATap) {
  TouchZones z; int taps = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; }); z.endFrame();
  z.down(10, 10, 1000); z.move(40, 10, 1010); z.up(1020);
  EXPECT_EQ(taps, 0);
}
TEST(TouchZones, HoldRunsAfterHalfSecondAndReleaseDoesNothing) {
  TouchZones z; int taps = 0, held = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; }, [&] { held++; }); z.endFrame();
  z.down(10, 10, 3000);
  EXPECT_TRUE(z.tick(16, 3499)); EXPECT_EQ(held, 0);
  z.tick(16, 3500); EXPECT_EQ(held, 1);
  z.up(3600); EXPECT_EQ(taps, 0);
}
TEST(TouchZones, SlowDragStepsOncePerPitchNoFling) {
  TouchZones z; int steps = 0;
  z.beginFrame(); z.scroll({0, 0, 100, 400}, TouchZones::kUpDown, 50, [&](int d) { steps += d; return true; }); z.endFrame();
  z.down(50, 300, 4000);
  for (int i = 1; i <= 15; i++) z.move(50, 300 - i * 10, 4000 + i * 100);
  z.up(5600);
  EXPECT_EQ(steps, 3); EXPECT_FALSE(z.flinging());
}
TEST(TouchZones, FlickFlingsAndStops) {
  TouchZones z; int steps = 0;
  z.beginFrame(); z.scroll({0, 0, 100, 400}, TouchZones::kUpDown, 50, [&](int d) { steps += d; return true; }); z.endFrame();
  z.down(50, 300, 6000); z.move(50, 280, 6010); z.move(50, 240, 6020); z.move(50, 200, 6030); z.up(6040);
  ASSERT_TRUE(z.flinging());
  int64_t t = 6040; while (z.tick(16, t += 16) && t < 20000) {}
  EXPECT_FALSE(z.flinging()); EXPECT_GT(steps, 2);
}
TEST(TouchZones, FingerOnFlingStopsItAndTapsNothing) {
  TouchZones z; int steps = 0, taps = 0;
  z.beginFrame();
  z.scroll({0, 0, 100, 400}, TouchZones::kUpDown, 50, [&](int d) { steps += d; return true; });
  z.tap({0, 0, 100, 400}, [&] { taps++; });
  z.endFrame();
  z.down(50, 300, 0); z.move(50, 260, 10); z.move(50, 200, 20); z.up(30);
  ASSERT_TRUE(z.flinging());
  z.down(50, 100, 60); z.up(70);
  EXPECT_FALSE(z.flinging()); EXPECT_EQ(taps, 0);
}
TEST(TouchZones, PressFiresOnDownViaTakeFired) {
  TouchZones z; int n = 0;
  z.beginFrame(); z.press({0, 0, 50, 50}, [&] { n++; }); z.endFrame();
  z.down(5, 5, 7000);
  TouchZones::Action a; ASSERT_TRUE(z.takeFired(a)); a();
  EXPECT_EQ(n, 1); EXPECT_FALSE(z.tracking());
  TouchRect r; float alpha; EXPECT_TRUE(z.pressShown(7050, r, alpha)); EXPECT_FALSE(z.pressShown(7200, r, alpha));
}
TEST(TouchZones, BlockHidesZonesBelow) {
  TouchZones z; int taps = 0, blockTaps = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; }); z.block({0, 0, 100, 100}, [&] { blockTaps++; }); z.endFrame();
  z.down(10, 10, 8000); z.up(8010);
  EXPECT_EQ(taps, 0); EXPECT_EQ(blockTaps, 1);
}
TEST(TouchZones, TapMarginGivesWayToRealTap) {
  TouchZones z; int a = 0, m = 0;
  z.beginFrame(); z.tap({0, 0, 20, 20}, [&] { a++; }); z.tapMargin({0, 0, 60, 60}, [&] { m++; }); z.endFrame();
  z.down(5, 5, 0); z.up(10); z.down(40, 40, 100); z.up(110);
  EXPECT_EQ(a, 1); EXPECT_EQ(m, 1);
}
TEST(TouchZones, ShortDragOnListTakesOneStepElseSwipes) {
  TouchZones z; int steps = 0, swipes = 0; bool canMove = true;
  z.beginFrame();
  z.scroll({0, 0, 400, 400}, TouchZones::kAcross, 200, [&](int d) { if (!canMove) return false; steps += d; return true; });
  z.swipe({0, 0, 400, 400}, [&](int, int) { swipes++; });
  z.endFrame();
  z.down(300, 50, 0); z.move(240, 50, 500); z.up(1000);       // 60 px < pitch, slow: one step
  EXPECT_EQ(steps, 1); EXPECT_EQ(swipes, 0);
  canMove = false;
  z.down(300, 50, 2000); z.move(240, 50, 2500); z.up(3000);
  EXPECT_EQ(swipes, 1);
}
TEST(TouchRect, ClipAndGrow) {
  TouchRect r{10, 10, 10, 10};
  TouchRect c = r.clippedTo({15, 0, 100, 100}); EXPECT_FLOAT_EQ(c.x, 15); EXPECT_FLOAT_EQ(c.w, 5);
  TouchRect e = r.clippedTo({50, 50, 1, 1}); EXPECT_EQ(e.w, 0); EXPECT_FALSE(e.contains(50, 50));
  TouchRect g = r.grownTo(40, 40); EXPECT_FLOAT_EQ(g.x, -5); EXPECT_FLOAT_EQ(g.w, 40);
}

TEST(TouchZones, PressHighlightFadesOverItsLifetime) {
  TouchZones z;
  z.beginFrame(); z.press({10, 20, 30, 40}, [] {}); z.endFrame();
  TouchRect r; float alpha = 0;
  EXPECT_FALSE(z.pressShown(1000, r, alpha));   // nothing pressed yet
  z.down(15, 25, 1000);
  ASSERT_TRUE(z.pressShown(1000, r, alpha));
  EXPECT_FLOAT_EQ(alpha, 1.0f); EXPECT_FLOAT_EQ(r.x, 10); EXPECT_FLOAT_EQ(r.w, 30);
  ASSERT_TRUE(z.pressShown(1080, r, alpha)); EXPECT_NEAR(alpha, 0.5f, 1e-6);
  EXPECT_FALSE(z.pressShown(1160, r, alpha));
  EXPECT_FALSE(z.pressShown(999, r, alpha));    // a clock before the press shows nothing
  EXPECT_TRUE(z.tick(16, 1100));                // frames keep coming while it is lit
  EXPECT_FALSE(z.tick(16, 1200));
}
TEST(TouchZones, PressMarginLightsItsVisibleRect) {
  TouchZones z; int n = 0;
  z.beginFrame(); z.pressMargin({0, 0, 80, 80}, [&] { n++; }, {30, 30, 20, 20}); z.endFrame();
  z.down(5, 5, 500);
  TouchZones::Action a; ASSERT_TRUE(z.takeFired(a)); a();
  EXPECT_EQ(n, 1);
  TouchRect r; float alpha;
  ASSERT_TRUE(z.pressShown(510, r, alpha));
  EXPECT_FLOAT_EQ(r.x, 30); EXPECT_FLOAT_EQ(r.w, 20);
}
TEST(TouchZones, TakeFiredOnlyOncePerDown) {
  TouchZones z;
  z.beginFrame(); z.press({0, 0, 50, 50}, [] {}); z.endFrame();
  TouchZones::Action a;
  EXPECT_FALSE(z.takeFired(a));
  z.down(5, 5, 100);
  EXPECT_TRUE(z.takeFired(a));
  EXPECT_FALSE(z.takeFired(a));
  z.down(500, 500, 200);                       // a down off the button clears nothing new
  EXPECT_FALSE(z.takeFired(a));
}
TEST(TouchZones, PressGestureIgnoresTheRestOfTheTouch) {
  TouchZones z; int fired = 0, swipes = 0;
  z.beginFrame();
  z.swipe({0, 0, 400, 400}, [&](int, int) { swipes++; });
  z.press({0, 0, 50, 50}, [&] { fired++; });
  z.endFrame();
  z.down(5, 5, 0); z.move(200, 5, 50); z.up(60);
  EXPECT_EQ(swipes, 0);
  EXPECT_EQ(fired, 0);                         // the caller runs it via takeFired, not the zones
  TouchZones::Action a; ASSERT_TRUE(z.takeFired(a)); a();
  EXPECT_EQ(fired, 1);
}
TEST(TouchZones, PressOverScrollActsOnReleaseLikeATap) {
  TouchZones z; int fired = 0, steps = 0;
  z.beginFrame();
  z.scroll({0, 0, 100, 400}, TouchZones::kUpDown, 50, [&](int d) { steps += d; return true; });
  z.press({0, 0, 100, 50}, [&] { fired++; });
  z.endFrame();
  z.down(10, 10, 0);
  TouchZones::Action a; EXPECT_FALSE(z.takeFired(a));
  z.up(20);
  EXPECT_EQ(fired, 1);
  z.down(10, 40, 100); z.move(10, 20, 150); z.move(10, -80, 300); z.up(800);   // a drag from it scrolls
  EXPECT_EQ(fired, 1); EXPECT_EQ(steps, 2);
}
TEST(TouchZones, DownOnFlingDoesNotFirePressInsideIt) {
  TouchZones z; int steps = 0;
  z.beginFrame();
  z.scroll({0, 0, 100, 400}, TouchZones::kUpDown, 50, [&](int d) { steps += d; return true; });
  z.press({0, 0, 100, 50}, [] {});
  z.press({200, 0, 50, 50}, [] {});
  z.endFrame();
  z.down(50, 300, 0); z.move(50, 260, 10); z.move(50, 200, 20); z.up(30);
  ASSERT_TRUE(z.flinging());
  z.down(10, 10, 60); z.up(70);                // on the flinging list: it only stops
  EXPECT_FALSE(z.flinging());
  TouchZones::Action a; EXPECT_FALSE(z.takeFired(a));
  z.down(50, 300, 100); z.move(50, 260, 110); z.move(50, 200, 120); z.up(130);
  ASSERT_TRUE(z.flinging());
  z.down(210, 10, 160);                        // a button outside the fling still presses
  EXPECT_FALSE(z.flinging());
  EXPECT_TRUE(z.takeFired(a));
}
// After a pause the finger's earlier speed is stale: lifting it then must not fling.
TEST(TouchZones, FlickAfterPauseDoesNotFling) {
  auto run = [](bool pause) {
    TouchZones z;
    z.beginFrame(); z.scroll({0, 0, 100, 1000}, TouchZones::kUpDown, 50, [](int) { return true; }); z.endFrame();
    z.down(50, 900, 0); z.move(50, 860, 10); z.move(50, 800, 20); z.move(50, 740, 30);
    int64_t t = 30;
    if (pause) { t += 200; z.move(50, 739, t); }
    z.up(t + 10);
    return z.flinging();
  };
  EXPECT_TRUE(run(false));
  EXPECT_FALSE(run(true));
}
TEST(TouchZones, CancelEndsGestureWithoutActing) {
  TouchZones z; int taps = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; }); z.endFrame();
  z.down(10, 10, 0); z.cancel(); z.up(10);
  EXPECT_EQ(taps, 0); EXPECT_FALSE(z.tracking());
}
TEST(TouchZones, ZonesAnswerOnlyAfterEndFrame) {
  TouchZones z; int taps = 0;
  z.beginFrame(); z.tap({0, 0, 100, 100}, [&] { taps++; });
  EXPECT_FALSE(z.any());
  z.down(10, 10, 0); z.up(10);
  EXPECT_EQ(taps, 0);
  z.endFrame();
  EXPECT_TRUE(z.any());
  z.down(10, 10, 20); z.up(30);
  EXPECT_EQ(taps, 1);
  z.clear();
  EXPECT_FALSE(z.any());
}
