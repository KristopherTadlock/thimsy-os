#include <gtest/gtest.h>

#include "NanoTypec.h"

using namespace android;

namespace {

typec::View view(bool partner, const char* role, const char* pd, const char* type = "dual", bool plugged = true,
                 bool dp = false) {
    typec::View v;
    v.plugged = plugged || partner;
    v.partner = partner;
    v.powerRole = role;
    v.pd = pd;
    v.portType = type;
    v.displayPort = dp;
    v.dpCapable = true;
    return v;
}

}  // namespace

TEST(Typec, CurrentChoiceIsTheBracketedOne) {
    EXPECT_EQ(typec::current("source [sink]"), "sink");
    EXPECT_EQ(typec::current("[dual] source sink"), "dual");
    EXPECT_EQ(typec::current("[source] sink"), "source");
    EXPECT_EQ(typec::current("source sink"), "");
    EXPECT_EQ(typec::current(""), "");
}

TEST(Typec, PicksThePortThatNegotiates) {
    // The RG Cube: the PMIC's port (sees the plug, cannot negotiate or be set) and the PD port.
    EXPECT_EQ(typec::pdPort({{"port0", false, "0.0"}, {"port1", true, "3.0"}}), "port1");
    EXPECT_EQ(typec::pdPort({{"port1", true, "3.0"}, {"port0", false, "0.0"}}), "port1");
    EXPECT_EQ(typec::pdPort({{"port0", true, "0.0"}}), "");   // settable but no PD: nothing to negotiate with
    EXPECT_EQ(typec::pdPort({{"port0", false, "2.0"}}), "");  // read-only
    EXPECT_EQ(typec::pdPort({}), "");
    EXPECT_EQ(typec::pdPort({{"port0", true, "0"}, {"port1", true, "3"}}), "port1");   // older kernels: "%d"
}

TEST(Typec, OffersPowerToAPartnerThatDidNotNegotiate) {
    const typec::Step s = typec::step(view(true, "sink", "no"), "", 1000, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "source");
    EXPECT_EQ(s.state, "flip:1000");
}

TEST(Typec, LeavesEverythingElseAlone) {
    EXPECT_EQ(typec::step(view(true, "sink", "yes"), "", 1000, 0).write, nullptr);    // a PD charger
    EXPECT_EQ(typec::step(view(true, "sink", ""), "", 1000, 0).write, nullptr);       // PD unknown
    EXPECT_EQ(typec::step(view(true, "source", "no"), "", 1000, 0).write, nullptr);   // the Cube supplies already
    EXPECT_EQ(typec::step(view(false, "sink", ""), "", 1000, 0).write, nullptr);      // still negotiating
    EXPECT_EQ(typec::step(view(true, "sink", "no", "sink"), "", 1000, 0).write, nullptr);   // not dual role
    typec::View pc = view(true, "sink", "no");
    pc.host = true;   // a computer is connected: file transfer is not interrupted
    EXPECT_EQ(typec::step(pc, "", 1000, 0).write, nullptr);
    typec::View applying = view(true, "sink", "no");
    applying.hold = true;   // the USB mode is being applied: wait
    EXPECT_EQ(typec::step(applying, "", 1000, 0).write, nullptr);
    EXPECT_EQ(typec::step(applying, "", 1000, 0).state, "");
    typec::View low = view(true, "sink", "no");
    low.batteryLow = true;
    EXPECT_EQ(typec::step(low, "", 1000, 0).write, nullptr);
    typec::View other = view(true, "sink", "no");
    other.dpCapable = false;   // a device with no picture over USB-C
    EXPECT_EQ(typec::step(other, "", 1000, 0).write, nullptr);
}

TEST(Typec, KeepsTheSourceRoleWithAMonitor) {
    EXPECT_EQ(typec::step(view(false, "source", ""), "flip:1000", 2000, 0).write, nullptr);   // re-attaching
    EXPECT_EQ(typec::step(view(true, "sink", "no", "source"), "flip:1000", 1200, 0).write, nullptr);
    // Negotiated, the DisplayPort mode not shown yet: waits.
    EXPECT_EQ(typec::step(view(true, "source", "yes", "source"), "flip:1000", 8000, 0).write, nullptr);
    EXPECT_EQ(typec::step(view(true, "source", "", "source"), "flip:1000", 8000, 0).write, nullptr);
    const typec::Step s = typec::step(view(true, "source", "yes", "source", true, true), "flip:1000", 8500, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "dual");
    EXPECT_EQ(s.state, "kept:8500");
}

TEST(Typec, GivesTheSourceRoleBackToWhatIsNoMonitor) {
    // Took power without negotiating (a cheap power bank): at once.
    typec::Step s = typec::step(view(true, "source", "no", "source"), "flip:1000", 2500, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "sink");
    EXPECT_EQ(s.state, "sink:2500");
    // Negotiated but never showed DisplayPort (a phone): at the deadline.
    s = typec::step(view(true, "source", "yes", "source"), "flip:1000", 1000 + typec::kFlipSinkMs, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "sink");
    EXPECT_EQ(typec::step(view(false, "", "", "sink"), "sink:2500", 3000, 0).write, nullptr);
    s = typec::step(view(true, "sink", "no", "sink"), "sink:2500", 4000, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "dual");
    EXPECT_EQ(s.state, "tried:4000");
    s = typec::step(view(false, "", "", "sink"), "sink:2500", 2500 + typec::kSinkWaitMs, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "dual");
}

TEST(Typec, WaitsLongerWhileSomethingIsAttached) {
    // Something attached to the source-only port takes up to ~7 s to settle as a sink.
    EXPECT_EQ(typec::step(view(false, "source", "", "source", true), "flip:1000", 1000 + typec::kFlipWaitMs, 0).write,
              nullptr);
    EXPECT_STREQ(typec::step(view(false, "source", "", "source", true), "flip:1000", 1000 + typec::kFlipSinkMs, 0).write,
                 "dual");
}

TEST(Typec, BackToDualWhenNothingTakesPower) {
    EXPECT_EQ(typec::step(view(false, "source", "", "dual", false), "flip:1000", 1000 + typec::kFlipWaitMs - 1, 1100)
                  .write,
              nullptr);
    const typec::Step s =
        typec::step(view(false, "source", "", "dual", false), "flip:1000", 1000 + typec::kFlipWaitMs, 1100);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "dual");
    EXPECT_EQ(s.state, "tried:" + std::to_string(1000 + typec::kFlipWaitMs));
    // A time from later (another clock) or a garbled property does not keep the port one-role.
    EXPECT_STREQ(typec::step(view(false, "source", ""), "flip:9000", 1000, 0).write, "dual");
    EXPECT_STREQ(typec::step(view(false, "source", ""), "flip:", 1000, 0).write, "dual");
    EXPECT_STREQ(typec::step(view(false, "", ""), "sink:x", 1000, 0).write, "dual");
}

TEST(Typec, OnceUntilAnUnplug) {
    // The charger is back as the source after the try: not tried again.
    EXPECT_EQ(typec::step(view(true, "sink", "no"), "tried:6000", 9000, 0).write, nullptr);
    EXPECT_EQ(typec::step(view(true, "sink", "no"), "tried:6000", 9000, 0).state, "tried:6000");
    // Out briefly is not an unplug; out kUnplugMs (after the try has settled) is.
    const int64_t out = 6000 + typec::kSettleMs + 1000;
    EXPECT_EQ(typec::step(view(false, "", "", "dual", false), "tried:6000", out + typec::kUnplugMs - 1, out).state,
              "tried:6000");
    EXPECT_EQ(typec::step(view(false, "", "", "dual", false), "tried:6000", out + typec::kUnplugMs, out).state, "");
    // Out since before the try ended counts only from when it settled.
    EXPECT_EQ(typec::step(view(false, "", "", "dual", false), "tried:6000", 6000 + typec::kSettleMs, 5000).state,
              "tried:6000");
}

TEST(Typec, AChargerIsTriedOnceNotOverAndOver) {
    // A non-PD charger: the PD port's partner appears only once its hard resets end (~6 s), the flip
    // finds nothing, and the plug is out during the flip itself. The time out during the flip must not
    // count as an unplug after it.
    int64_t now = 10000;
    std::string state;
    typec::Step s = typec::step(view(true, "sink", "no"), state, now, 0);
    ASSERT_STREQ(s.write, "source");
    state = s.state;
    const int64_t outSince = now + 100;   // the charger cannot attach to a source-only port
    now += typec::kFlipWaitMs;
    s = typec::step(view(false, "", "", "source", false), state, now, outSince);
    ASSERT_STREQ(s.write, "dual");
    state = s.state;
    // The plug is seen again within a second of dual role; the PD partner some 6 s later.
    s = typec::step(view(false, "", "", "dual", false), state, now + 900, outSince);
    EXPECT_EQ(s.state, state);
    for (int64_t t = now + 1000; t < now + 7000; t += 500) {
        s = typec::step(view(t >= now + 6000, "sink", "no", "dual", true), state, t, 0);
        EXPECT_EQ(s.write, nullptr) << t;
        EXPECT_EQ(s.state, state) << t;
    }
}

TEST(Typec, AComputerSeenOnceIsLeftAloneAllPlugInLong) {
    typec::View pc = view(false, "", "", "dual", true);
    pc.host = true;   // seen before the PD port's partner settles
    typec::Step s = typec::step(pc, "", 1000, 0);
    EXPECT_EQ(s.write, nullptr);
    EXPECT_EQ(s.state, "tried:1000");
    // Later its link is down (MTP set up again, the computer asleep): still left alone.
    s = typec::step(view(true, "sink", "no"), s.state, 7000, 0);
    EXPECT_EQ(s.write, nullptr);
    EXPECT_EQ(s.state, "tried:1000");
}

TEST(Typec, AMonitorReseatedAtOnceIsTriedAgain) {
    std::string state = "kept:5000";
    EXPECT_EQ(typec::step(view(true, "source", "yes", "dual", true, true), state, 6000, 0).state, state);
    // Its PD partner gone, however briefly, once settled: the next plug-in is tried.
    EXPECT_EQ(typec::step(view(false, "", "", "dual", true), state, 5000 + typec::kSettleMs - 1, 0).state, state);
    EXPECT_EQ(typec::step(view(false, "", "", "dual", true), state, 5000 + typec::kSettleMs, 0).state, "");
}

TEST(Typec, AMonitorPluggedInAgainIsTriedAgain) {
    std::string state = "tried:5000";
    typec::Step s = typec::step(view(true, "source", "yes", "dual", true, true), state, 20000, 0);   // working
    EXPECT_EQ(s.state, state);
    s = typec::step(view(false, "", "", "dual", false), state, 30000 + typec::kUnplugMs, 30000);   // unplugged
    EXPECT_EQ(s.state, "");
    s = typec::step(view(true, "sink", "no"), "", 40000, 0);   // plugged in again, the monitor supplying
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "source");
}

TEST(Typec, FindsThePdPortByWhatItCanDo) {
    // The dock device tree's c2c mode: the PMIC registers no port, so the PD port alone, as port0.
    EXPECT_EQ(typec::pdPort({{"port0", true, "3.0"}}), "port0");
    EXPECT_EQ(typec::pdPort({{"port2", true, "2.0"}}), "port2");
    // The data role file reads like the power role's.
    EXPECT_EQ(typec::current("host [device]"), "device");
    EXPECT_EQ(typec::current("[host] device"), "host");
}

TEST(Typec, LeavesDocksAlone) {
    typec::View dock = view(true, "sink", "yes");
    dock.dataRole = "host";   // a PD dock that powers the Cube and made it the USB host
    typec::Step s = typec::step(dock, "", 1000, 0);
    EXPECT_EQ(s.write, nullptr);
    EXPECT_EQ(s.state, "tried:1000");
    // A dock that powers the Cube as a host without PD: never made source-only.
    typec::View plain = view(true, "sink", "no");
    plain.dataRole = "host";
    s = typec::step(plain, "", 1000, 0);
    EXPECT_EQ(s.write, nullptr);
    EXPECT_EQ(s.state, "tried:1000");
    // A PD charger (the Cube a USB device): left alone too, once per plug-in.
    typec::View charger = view(true, "sink", "yes");
    charger.dataRole = "device";
    s = typec::step(charger, "", 1000, 0);
    EXPECT_EQ(s.write, nullptr);
    EXPECT_EQ(s.state, "tried:1000");
    EXPECT_EQ(typec::step(charger, s.state, 9000, 0).state, "tried:1000");
    // Something that powers the Cube as a device without PD is still offered power.
    typec::View monitor = view(true, "sink", "no");
    monitor.dataRole = "device";
    s = typec::step(monitor, "", 1000, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "source");
    // Not settled yet (no partner on the PD port): nothing decided.
    typec::View early = view(false, "sink", "");
    early.dataRole = "host";
    EXPECT_EQ(typec::step(early, "", 1000, 0).state, "");
}

TEST(Typec, ADockBackDuringTheTryEndsIt) {
    // The port is source-only, yet the partner comes back powering the Cube as its USB host.
    typec::View dock = view(true, "sink", "yes", "source");
    dock.dataRole = "host";
    typec::Step s = typec::step(dock, "flip:1000", 3000, 0);
    ASSERT_NE(s.write, nullptr);
    EXPECT_STREQ(s.write, "dual");
    EXPECT_EQ(s.state, "tried:3000");
    ASSERT_NE(s.log, nullptr);
    EXPECT_NE(std::string(s.log).find("a dock powers the Cube"), std::string::npos);
    // Then left alone until an unplug.
    dock.portType = "dual";
    EXPECT_EQ(typec::step(dock, s.state, 8000, 0).write, nullptr);
    EXPECT_EQ(typec::step(dock, s.state, 8000, 0).state, "tried:3000");
    // A sink as a USB device during the try is the try's own re-attach: it waits as before.
    typec::View device = view(true, "sink", "no", "source");
    device.dataRole = "device";
    EXPECT_EQ(typec::step(device, "flip:1000", 1200, 0).write, nullptr);
    EXPECT_EQ(typec::step(device, "flip:1000", 1200, 0).state, "flip:1000");
}
