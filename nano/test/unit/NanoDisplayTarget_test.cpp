// The display Nano and drastic-nano draw on (NanoDisplayTarget): a monitor as display 0 takes over,
// and a second screen is another panel, beside the monitor only in dual screen mode.

#include <gtest/gtest.h>

#include <vector>

#include "NanoDisplayTarget.h"

using namespace android::nanodisplay;

namespace {

Display panel(uint64_t id, int port, int stack) {
    Display d;
    d.id = id;
    d.port = port;
    d.hasLayerStack = stack >= 0;
    d.layerStack = stack >= 0 ? (uint32_t)stack : 0;
    d.internal = true;
    return d;
}

Display monitor(uint64_t id, int port, int stack) {
    Display d = panel(id, port, stack);
    d.internal = false;
    return d;
}

}  // namespace

TEST(DisplayTarget, NoDisplays) {
    const Choice c = choose({}, 0);
    EXPECT_EQ(-1, c.target);
    EXPECT_EQ(-1, c.second);
    EXPECT_FALSE(c.monitor);
}

TEST(DisplayTarget, PanelAlone) {
    const Choice c = choose({panel(1, 0, 0)}, 0);
    EXPECT_EQ(0, c.target);
    EXPECT_EQ(-1, c.second);
    EXPECT_FALSE(c.monitor);
}

TEST(DisplayTarget, MonitorPluggedInTakesOver) {
    // The panel switched off on its new layer stack, the monitor on layer stack 0.
    const Choice c = choose({panel(1, 0, 2), monitor(2, 1, 0)}, 0);
    EXPECT_EQ(1, c.target);
    EXPECT_TRUE(c.monitor);
    EXPECT_EQ(-1, c.second);   // never the panel while a monitor is in use
}

TEST(DisplayTarget, MonitorNotYetDisplay0) {
    // Just connected, before the swap: still the panel.
    const Choice c = choose({panel(1, 0, 0), monitor(2, 1, 1)}, 0);
    EXPECT_EQ(0, c.target);
    EXPECT_FALSE(c.monitor);
    EXPECT_EQ(-1, c.second);   // a monitor is never a second screen
}

TEST(DisplayTarget, NoStatesYetIsTheFirstDisplay) {
    const Choice c = choose({panel(1, 0, -1), monitor(2, 1, -1)}, 0);
    EXPECT_EQ(0, c.target);
    EXPECT_FALSE(c.monitor);
}

TEST(DisplayTarget, TwoPanelsPortRuleAndSecondScreen) {
    // A dual-screen device drawing on port 1, which DisplayManager made display 2.
    const Choice c = choose({panel(10, 0, 0), panel(11, 1, 2)}, 1);
    EXPECT_EQ(1, c.target);
    EXPECT_EQ(0, c.second);
    EXPECT_FALSE(c.monitor);
    // Port 0: the panel showing display 0, the other as the second screen.
    const Choice d = choose({panel(10, 0, 0), panel(11, 1, 2)}, 0);
    EXPECT_EQ(0, d.target);
    EXPECT_EQ(1, d.second);
}

TEST(DisplayTarget, TwoPanelsAndAMonitor) {
    const Choice c = choose({panel(10, 0, 3), panel(11, 1, 4), monitor(12, 2, 0)}, 1);
    EXPECT_EQ(2, c.target);
    EXPECT_TRUE(c.monitor);
    EXPECT_EQ(-1, c.second);
}

TEST(DisplayTarget, PortNamingAMonitorIsIgnored) {
    const Choice c = choose({panel(10, 0, 0), monitor(12, 2, 1)}, 2);
    EXPECT_EQ(0, c.target);
    EXPECT_FALSE(c.monitor);
}

TEST(DisplayTarget, DualScreenPanelBesideTheMonitor) {
    const Choice c = choose({panel(1, 0, 2), monitor(2, 1, 0)}, 0, true);
    EXPECT_EQ(1, c.target);
    EXPECT_TRUE(c.monitor);
    EXPECT_EQ(0, c.second);
    // Before the swap the panel is still the target, and the monitor is never a second screen.
    const Choice d = choose({panel(1, 0, 0), monitor(2, 1, 1)}, 0, true);
    EXPECT_EQ(0, d.target);
    EXPECT_FALSE(d.monitor);
    EXPECT_EQ(-1, d.second);
    // Unplugged: the panel alone.
    const Choice e = choose({panel(1, 0, 0)}, 0, true);
    EXPECT_EQ(0, e.target);
    EXPECT_EQ(-1, e.second);
}

TEST(DisplayTarget, DualScreenWaitsForThePanel) {
    // Docked to dual: the monitor has layer stack 0 and the panel is still blanked.
    Display blanked = panel(1, 0, 0);
    blanked.layerStack = kBlankLayerStack;
    const Choice c = choose({blanked, monitor(2, 1, 0)}, 0, true);
    EXPECT_EQ(1, c.target);
    EXPECT_TRUE(c.monitor);
    EXPECT_EQ(-1, c.second);
    EXPECT_EQ(0, c.blankSecond);
    EXPECT_EQ(-1, choose({panel(1, 0, -1), monitor(2, 1, 0)}, 0, true).second);   // no state yet
    const Choice on = choose({panel(1, 0, 2), monitor(2, 1, 0)}, 0, true);
    EXPECT_EQ(0, on.second);
    EXPECT_EQ(-1, on.blankSecond);
    EXPECT_EQ(-1, choose({blanked, monitor(2, 1, 0)}, 0).blankSecond);   // docked: no second screen
}

TEST(DisplayTarget, DualScreenTwoPanelsUsesThePrimaryPanel) {
    const Choice c = choose({panel(10, 0, 3), panel(11, 1, 4), monitor(12, 2, 0)}, 1, true);
    EXPECT_EQ(2, c.target);
    EXPECT_EQ(1, c.second);
    const Choice d = choose({panel(10, 0, 3), panel(11, 1, 4), monitor(12, 2, 0)}, 0, true);
    EXPECT_EQ(0, d.second);
}

TEST(DisplayTarget, MirrorKeepsThePanel) {
    // Both show layer stack 0; the panel is the target whichever SurfaceFlinger lists first.
    const Choice c = choose({monitor(2, 1, 0), panel(1, 0, 0)}, 0);
    EXPECT_EQ(1, c.target);
    EXPECT_FALSE(c.monitor);
    EXPECT_EQ(-1, c.second);   // the monitor is never a second screen
    EXPECT_TRUE(c.shown);
    const Choice d = choose({panel(1, 0, 0), monitor(2, 1, 0)}, 0);
    EXPECT_EQ(0, d.target);
    EXPECT_EQ(-1, d.second);
    // Two panels: the other panel is still the second screen.
    const Choice e = choose({monitor(12, 2, 0), panel(10, 0, 0), panel(11, 1, 2)}, 0);
    EXPECT_EQ(1, e.target);
    EXPECT_EQ(2, e.second);
}

TEST(DisplayTarget, UiOnThePanel) {
    UiState s;
    EXPECT_FALSE(uiOnPanel(s));   // the home: the monitor
    s.menu = true;
    EXPECT_TRUE(uiOnPanel(s));
    s.menu = false;
    s.settingsOpen = true;
    EXPECT_TRUE(uiOnPanel(s));
    s.media = true;               // media from a menu: the monitor
    EXPECT_FALSE(uiOnPanel(s));
    s.overGame = true;            // over a game: the panel, the game keeps the monitor
    EXPECT_TRUE(uiOnPanel(s));
}

TEST(DisplayTarget, ScreensOff) {
    // Asleep, DisplayManager blanks every display's layer stack: nothing to follow.
    const Choice c = choose({panel(1, 0, -1), monitor(2, 1, -1)}, 0, true);
    EXPECT_FALSE(c.shown);
    EXPECT_TRUE(choose({panel(1, 0, 2), monitor(2, 1, 0)}, 0, true).shown);
    // As SurfaceFlinger reports it: every layer stack blank.
    Display p = panel(1, 0, 0), m = monitor(2, 1, 0);
    p.layerStack = m.layerStack = kBlankLayerStack;
    EXPECT_FALSE(choose({p, m}, 0, true).shown);
}

TEST(DisplayTarget, WhatIsAMonitor) {
    // One panel: an external display while a connector has a monitor, EDID or not.
    EXPECT_TRUE(isMonitor(true, true, true, false));
    EXPECT_TRUE(isMonitor(true, true, false, false));
    EXPECT_FALSE(isMonitor(true, false, true, false));   // nothing on the connector
    EXPECT_FALSE(isMonitor(false, true, true, false));   // the panel
    // Two panels: the second panel the composer calls external has no EDID.
    EXPECT_FALSE(isMonitor(true, true, false, true));
    EXPECT_TRUE(isMonitor(true, true, true, true));
}
