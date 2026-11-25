/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.server.display;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The screens with a monitor (GammaosScreenArrangement.java): which display is display 0, which are
 * switched off or on beside it, or show it (docked, dual screen or mirror), a monitor's mode (Auto,
 * stored, trial, what "try" refuses, the store), and the one-time cleanup of the old external
 * display modes (test/README.md has the command). Exits 1 on a failure.
 */
public final class GammaosScreenArrangementCheck {
    private static int sFailures;

    private static void check(String what, Object expected, Object actual) {
        if (!String.valueOf(expected).equals(String.valueOf(actual))) {
            System.out.println("FAIL " + what + ": expected " + expected + ", got " + actual);
            sFailures++;
        }
    }

    private static List<String> list(String... items) {
        return Arrays.asList(items);
    }

    private static final GammaosScreenArrangement.Mode DOCKED = GammaosScreenArrangement.Mode.DOCKED;
    private static final GammaosScreenArrangement.Mode DUAL = GammaosScreenArrangement.Mode.DUAL;
    private static final GammaosScreenArrangement.Mode MIRROR = GammaosScreenArrangement.Mode.MIRROR;

    private static String plan(String current, List<String> panels, List<String> monitors,
            String panel) {
        return plan(current, panels, monitors, panel, DOCKED);
    }

    private static String plan(String current, List<String> panels, List<String> monitors,
            String panel, GammaosScreenArrangement.Mode mode) {
        final GammaosScreenArrangement.Plan<String> p =
                GammaosScreenArrangement.plan(current, panels, monitors, panel, mode);
        if (p == null) return "leave";
        return p.display0 + " off=" + p.off + (p.on.isEmpty() ? "" : " on=" + p.on) + " panel=" + p.panel
                + (p.mirrored.isEmpty() ? "" : " mirrored=" + p.mirrored);
    }

    public static void main(String[] args) {
        final List<String> none = Collections.emptyList();

        // No monitor: the layout is the stock one, unless a monitor had display 0.
        check("panel alone", "leave", plan("panel", list("panel"), none, null));
        check("no displays yet", "leave", plan(null, none, none, null));
        check("monitor unplugged", "panel off=[] panel=null",
                plan("tv", list("panel"), none, "panel"));

        // A monitor plugged in takes display 0 and the panel is switched off.
        check("monitor plugged in", "tv off=[panel] panel=panel",
                plan("panel", list("panel"), list("tv"), null));
        // At boot the monitor's device can come before the panel's.
        check("monitor first at boot", "tv off=[] panel=null", plan("tv", none, list("tv"), null));
        check("then the panel", "tv off=[panel] panel=panel",
                plan("tv", list("panel"), list("tv"), null));
        // The panel remembered is the one that was display 0, not the first panel found.
        check("two panels", "tv off=[top, bottom] panel=bottom",
                plan("bottom", list("top", "bottom"), list("tv"), null));
        check("remembered panel kept", "tv off=[panel] panel=panel",
                plan("tv", list("panel"), list("tv"), "panel"));

        // Two monitors: the one with display 0 keeps it, the other is switched off too.
        check("second monitor", "tv off=[panel, tv2] panel=panel",
                plan("tv", list("panel"), list("tv2", "tv"), "panel"));
        check("display 0's monitor unplugged", "tv2 off=[panel] panel=panel",
                plan("tv", list("panel"), list("tv2"), "panel"));
        check("two monitors at once", "tv off=[panel, tv2] panel=panel",
                plan("panel", list("panel"), list("tv", "tv2"), null));

        // Already arranged: nothing to rebuild.
        final GammaosScreenArrangement.Plan<String> p =
                GammaosScreenArrangement.plan("tv", list("panel"), list("tv"), "panel", DOCKED);
        check("arranged", true, GammaosScreenArrangement.arranged(p, "tv", 2, a -> a.equals("panel") ? Boolean.FALSE : null));
        check("panel still on", false, GammaosScreenArrangement.arranged(p, "tv", 2, a -> Boolean.TRUE));
        check("panel display 0", false,
                GammaosScreenArrangement.arranged(p, "panel", 2, a -> a.equals("panel") ? Boolean.FALSE : null));
        check("extra entry", false,
                GammaosScreenArrangement.arranged(p, "tv", 3, a -> a.equals("panel") ? Boolean.FALSE : null));
        final GammaosScreenArrangement.Plan<String> back =
                GammaosScreenArrangement.plan("tv", list("panel"), none, "panel", DOCKED);
        check("restore not arranged", false, GammaosScreenArrangement.arranged(back, "tv", 2, a -> Boolean.TRUE));
        check("restore arranged", true, GammaosScreenArrangement.arranged(back, "panel", 1, a -> Boolean.TRUE));

        // Two panels: once the monitor is gone, the other panel is switched back on.
        check("two panels, monitor unplugged", "bottom off=[] on=[top] panel=null",
                plan("tv", list("top", "bottom"), none, "bottom"));
        final GammaosScreenArrangement.Plan<String> both =
                GammaosScreenArrangement.plan("tv", list("top", "bottom"), none, "bottom", DOCKED);
        check("other panel still off", false,
                GammaosScreenArrangement.arranged(both, "bottom", 2, a -> Boolean.FALSE));
        check("other panel missing", false,
                GammaosScreenArrangement.arranged(both, "bottom", 1, a -> null));
        check("other panel back on", true,
                GammaosScreenArrangement.arranged(both, "bottom", 2, a -> Boolean.TRUE));
        // Plugged in again: both off, the panel at display 0 remembered.
        check("two panels, plugged in again", "tv off=[top, bottom] panel=bottom",
                plan("bottom", list("top", "bottom"), list("tv"), null));

        // Dual screen: the panels stay on beside the monitor, which is display 0.
        check("dual: monitor plugged in", "tv off=[] on=[panel] panel=panel",
                plan("panel", list("panel"), list("tv"), null, DUAL));
        check("dual: second monitor off", "tv off=[tv2] on=[panel] panel=panel",
                plan("tv", list("panel"), list("tv", "tv2"), "panel", DUAL));
        check("dual: unplugged", "panel off=[] panel=null",
                plan("tv", list("panel"), none, "panel", DUAL));
        check("dual: no monitor, nothing to do", "leave", plan("panel", list("panel"), none, null, DUAL));
        // Switching while the monitor is in: the layout no longer matches, so it is rebuilt.
        final GammaosScreenArrangement.Plan<String> docked =
                GammaosScreenArrangement.plan("tv", list("panel"), list("tv"), "panel", DOCKED);
        final GammaosScreenArrangement.Plan<String> dual =
                GammaosScreenArrangement.plan("tv", list("panel"), list("tv"), "panel", DUAL);
        check("docked layout is not dual", false,
                GammaosScreenArrangement.arranged(dual, "tv", 2, a -> Boolean.FALSE));
        check("dual layout is not docked", false,
                GammaosScreenArrangement.arranged(docked, "tv", 2, a -> Boolean.TRUE));
        check("dual arranged", true,
                GammaosScreenArrangement.arranged(dual, "tv", 2, a -> Boolean.TRUE));

        // Mirror: the panel stays display 0 and the monitors, disabled for apps, show it.
        check("mirror: monitor plugged in", "panel off=[tv] panel=panel mirrored=[tv]",
                plan("panel", list("panel"), list("tv"), null, MIRROR));
        check("mirror: from docked", "panel off=[tv] panel=panel mirrored=[tv]",
                plan("tv", list("panel"), list("tv"), "panel", MIRROR));
        check("mirror: two monitors", "panel off=[tv, tv2] panel=panel mirrored=[tv, tv2]",
                plan("panel", list("panel"), list("tv", "tv2"), null, MIRROR));
        check("mirror: two panels", "bottom off=[tv] on=[top] panel=bottom mirrored=[tv]",
                plan("bottom", list("top", "bottom"), list("tv"), null, MIRROR));
        check("mirror: unplugged", "panel off=[] panel=null",
                plan("panel", list("panel"), none, "panel", MIRROR));
        check("mirror: no panel, docked", "tv off=[] panel=null",
                plan("tv", none, list("tv"), null, MIRROR));
        final GammaosScreenArrangement.Plan<String> mirror =
                GammaosScreenArrangement.plan("panel", list("panel"), list("tv"), null, MIRROR);
        check("mirror arranged", true,
                GammaosScreenArrangement.arranged(mirror, "panel", 2, a -> Boolean.FALSE));
        check("docked layout is not mirror", false,
                GammaosScreenArrangement.arranged(mirror, "tv", 2, a -> Boolean.FALSE));
        check("dual layout is not mirror", false,
                GammaosScreenArrangement.arranged(mirror, "tv", 2, a -> Boolean.TRUE));
        check("mode", DUAL, GammaosScreenArrangement.mode("dual"));
        check("mode mirror", MIRROR, GammaosScreenArrangement.mode("mirror"));
        check("mode default", DOCKED, GammaosScreenArrangement.mode("docked"));
        check("mode unset", DOCKED, GammaosScreenArrangement.mode(""));
        check("mode unknown", DOCKED, GammaosScreenArrangement.mode("other"));

        // What counts as a monitor.
        check("EDID", true, GammaosScreenArrangement.externalIsMonitor(true, true));
        check("one panel, no EDID", true, GammaosScreenArrangement.externalIsMonitor(false, false));
        check("second panel called external", false,
                GammaosScreenArrangement.externalIsMonitor(false, true));

        // Auto: 1080p60 when the monitor has it.
        check("1080p60 among others", 2, autoMode(
                new int[] {2560, 1920, 1920, 1280}, new int[] {1440, 1080, 1080, 720},
                new float[] {60f, 120f, 60f, 60f}, -1));
        check("59.94 Hz", 0, autoMode(
                new int[] {1920, 1920}, new int[] {1080, 1080}, new float[] {59.94f, 50f}, -1));
        check("closest to 60", 1, autoMode(
                new int[] {1920, 1920}, new int[] {1080, 1080}, new float[] {59.5f, 60.0f}, -1));
        // Without it, the largest mode at 50-61 Hz, never a filtered one.
        check("no 1080p60: largest at 50-61 Hz", 1, autoMode(
                new int[] {1920, 2560, 1280, 3840}, new int[] {1080, 1440, 720, 2160},
                new float[] {50f, 60f, 60f, 30f}, 3));
        check("no 1080p60: nearer 60 on a tie", 1, autoMode(
                new int[] {1920, 1920}, new int[] {1200, 1200}, new float[] {50f, 60f}, -1));
        check("no 1080p60: DCI 4096 never", 1, autoMode(
                new int[] {4096, 1280}, new int[] {2160, 720}, new float[] {60f, 60f}, 0));
        check("no 1080p60: 1080p50", 1, autoMode(
                new int[] {1280, 1920}, new int[] {720, 1080}, new float[] {60f, 50f}, -1));
        // Nothing at 50-61 Hz: the mode at connect if offered, else the first offered mode.
        check("only 30 Hz: the one at connect", 1, autoMode(
                new int[] {1920, 2560}, new int[] {1080, 1440}, new float[] {30f, 30f}, 1));
        check("only 30 Hz, at connect filtered", 1, GammaosScreenArrangement.autoMode(
                new int[] {3840, 2560}, new int[] {2160, 1440}, new float[] {30f, 30f}, 0,
                false, false));
        check("at connect, 3840 with UHD", 0, GammaosScreenArrangement.autoMode(
                new int[] {3840, 2560}, new int[] {2160, 1440}, new float[] {30f, 30f}, 0,
                false, true));
        check("nothing offered: the one at connect", 0, autoMode(
                new int[] {4096}, new int[] {2160}, new float[] {30f}, 0));
        check("nothing offered, at connect unknown", 0, autoMode(
                new int[] {4096}, new int[] {2160}, new float[] {30f}, -1));
        check("no modes", -1, autoMode(new int[0], new int[0], new float[0], -1));

        // The held mode: a trial, else the stored mode if still listed, else Auto.
        final int[] w = {1920, 2560, 1920, 3840, 1920, 1280};
        final int[] h = {1080, 1440, 1080, 2160, 1080, 720};
        final float[] r = {60f, 60f, 120f, 30f, 30f, 60f};
        final GammaosScreenArrangement.MonitorMode qhd = mode("2560x1440@60");
        final GammaosScreenArrangement.MonitorMode fhd120 = mode("1920x1080@120");
        final GammaosScreenArrangement.MonitorMode fhd30 = mode("1920x1080@30");
        check("auto", "0 auto", held(w, h, r, null, null, false));
        check("stored", "1 user", held(w, h, r, null, qhd, false));
        check("trial over stored", "2 trial", held(w, h, r, fhd120, qhd, false));
        check("stored no longer listed", "0 auto",
                held(w, h, r, null, mode("3440x1440@60"), false));
        check("trial no longer listed", "1 user",
                held(w, h, r, mode("1600x900@60"), qhd, false));
        check("stored at 59.94 Hz", "1 user", held(w, h, r, null, mode("2560x1440@59.94"), false));
        check("stored 30 Hz, docked", "4 user", held(w, h, r, null, fhd30, false));
        check("stored 30 Hz, dual screen: auto", "0 auto", held(w, h, r, null, fhd30, true));
        check("stored 4K30 without UHD", "0 auto",
                held(w, h, r, null, mode("3840x2160@30"), false));
        final GammaosScreenArrangement.Choice uhd = GammaosScreenArrangement.monitorMode(w, h, r,
                -1, null, mode("3840x2160@30"), false, true);
        check("stored 4K30 with UHD", "3 user", uhd.index + " " + uhd.why);
        check("dual screen, 120 Hz", "2 user", held(w, h, r, null, fhd120, true));

        // What "try" refuses.
        check("try 1440p", null, refusal(w, h, r, "2560x1440@60", false, false));
        check("try DCI", "wider than 3840", refusal(w, h, r, "4096x2160@30", false, true));
        check("try 720x400", "smaller than 640x480", refusal(w, h, r, "720x400@70", false, false));
        check("try 4K without UHD",
                "3840 wide needs a build that composes it into one layer (ro.gammaos.display.uhd)",
                refusal(w, h, r, "3840x2160@30", false, false));
        check("try 4K with UHD", null, refusal(w, h, r, "3840x2160@30", false, true));
        check("try 30 Hz in dual screen", "below 50 Hz in Dual Screen",
                refusal(w, h, r, "1920x1080@30", true, false));
        check("try 30 Hz docked", null, refusal(w, h, r, "1920x1080@30", false, false));
        check("try an unlisted mode", "not one of the monitor's modes",
                refusal(w, h, r, "1920x1200@60", false, false));
        check("try an unlisted rate", "not one of the monitor's modes",
                refusal(w, h, r, "2560x1440@75", false, false));

        // What "list" shows: offered modes, largest first, then the higher rate.
        check("list", "[3, 1, 2, 0, 4, 5]", Arrays.toString(
                GammaosScreenArrangement.listOrder(w, h, r, false, true)));
        check("list without UHD", "[1, 2, 0, 4, 5]", Arrays.toString(
                GammaosScreenArrangement.listOrder(w, h, r, false, false)));
        check("list in dual screen", "[1, 2, 0, 5]", Arrays.toString(
                GammaosScreenArrangement.listOrder(w, h, r, true, false)));

        // Which modes a monitor lists at all.
        check("listed 1080p", true, GammaosScreenArrangement.listed(1920, 1080, false));
        check("listed 640x480", true, GammaosScreenArrangement.listed(640, 480, false));
        check("listed 720x400", false, GammaosScreenArrangement.listed(720, 400, false));
        check("listed DCI", false, GammaosScreenArrangement.listed(4096, 2160, true));
        check("listed 4K", false, GammaosScreenArrangement.listed(3840, 2160, false));
        check("listed 4K with UHD", true, GammaosScreenArrangement.listed(3840, 2160, true));
        check("offered 1080p50 in dual", true,
                GammaosScreenArrangement.offered(1920, 1080, 50f, true, false));
        check("offered 1080p49.95 in dual", true,
                GammaosScreenArrangement.offered(1920, 1080, 49.95f, true, false));
        check("offered 1080p25 in dual", false,
                GammaosScreenArrangement.offered(1920, 1080, 25f, true, false));
        check("offered 1080p25 in mirror", true,
                GammaosScreenArrangement.offered(1920, 1080, 25f, false, false));

        // Monitor modes as text.
        check("mode text", "2560x1440@60", mode("2560x1440@60"));
        check("mode text 59.94", "1920x1080@59.94", mode("1920x1080@59.94"));
        check("mode text 23.976", "1920x1080@23.98", mode("1920x1080@23.976"));
        check("mode text 29.9", "1920x1080@29.9", mode("1920x1080@29.90"));
        check("mode text 60.001", "1920x1080@60", mode("1920x1080@60.001"));
        check("mode try args", "1920x1080@120",
                GammaosScreenArrangement.MonitorMode.of("1920", "1080", "120"));
        check("mode bad width", null,
                GammaosScreenArrangement.MonitorMode.of("wide", "1080", "60"));
        check("mode bad rate", null,
                GammaosScreenArrangement.MonitorMode.of("1920", "1080", "NaN"));
        check("mode missing rate", null,
                GammaosScreenArrangement.MonitorMode.of("1920", "1080", null));
        check("mode zero rate", null, GammaosScreenArrangement.MonitorMode.of("1920", "1080", "0"));
        check("mode zero width", null, mode("0x1080@60"));
        check("mode no rate", null, mode("1920x1080@"));
        check("mode no size", null, mode("@60"));
        check("mode garbage", null, mode("auto"));
        check("mode null", null, mode(null));
        check("mode equals", true, mode("1920x1080@60").equals(mode("1920x1080@60.0")));
        check("mode matches 59.94", true, mode("1920x1080@60").matches(1920, 1080, 59.94f));
        check("mode matches 50", false, mode("1920x1080@60").matches(1920, 1080, 50f));
        check("prop", "2560x1440@60;user", GammaosScreenArrangement.monitorModeProp(qhd, "user"));
        check("prop no monitor", "", GammaosScreenArrangement.monitorModeProp(null, "auto"));

        // A monitor's key: its EDID, never the port; none without a PnP or product id.
        final String key = GammaosScreenArrangement.monitorKey("DEL", "41200", 2021, "DELL U2720Q");
        check("key", 16, key.length());
        check("key stable", key,
                GammaosScreenArrangement.monitorKey("DEL", "41200", 2021, "DELL U2720Q"));
        check("key other model", false, key.equals(
                GammaosScreenArrangement.monitorKey("DEL", "41201", 2021, "DELL U2720Q")));
        check("key other year", false, key.equals(
                GammaosScreenArrangement.monitorKey("DEL", "41200", 2022, "DELL U2720Q")));
        check("key without name", true,
                GammaosScreenArrangement.monitorKey("DEL", "41200", -1, null) != null);
        check("no key", null, GammaosScreenArrangement.monitorKey(null, "", 2021, "TV"));

        // The store, and its applying markers.
        final GammaosScreenArrangement.MonitorModes store =
                GammaosScreenArrangement.MonitorModes.parse("mode 00ff 2560x1440@60\n"
                        + "mode 0a0b 1920x1080@120\n# note\nmode BAD 1920x1080@60\n"
                        + "mode 0c0d auto\napplying 0a0b\napplying\nother 1 2\n");
        check("store", "mode 00ff 2560x1440@60\nmode 0a0b 1920x1080@120\napplying 0a0b\n",
                store.format());
        check("store round trip", store.format(),
                GammaosScreenArrangement.MonitorModes.parse(store.format()).format());
        check("store empty", "", GammaosScreenArrangement.MonitorModes.parse(null).format());
        check("connect, no marker", false, store.dropIfApplying("00ff"));
        check("connect, no key", false, store.dropIfApplying(null));
        check("connect, marker left", true, store.dropIfApplying("0a0b"));
        check("marker and mode dropped", "mode 00ff 2560x1440@60\n", store.format());

        // Connect at the stored mode, unplug within APPLYING_MS, connect again: the mode stays.
        final GammaosScreenArrangement.MonitorModes replug =
                GammaosScreenArrangement.MonitorModes.parse("mode 00ff 2560x1440@60\n");
        check("replug, connect", false, replug.dropIfApplying("00ff"));
        replug.applying.add("00ff");
        check("replug, unplugged", true, replug.unplugged("00ff"));
        check("replug, unplugged again", false, replug.unplugged("00ff"));
        check("replug, no key", false, replug.unplugged(null));
        final GammaosScreenArrangement.MonitorModes reread =
                GammaosScreenArrangement.MonitorModes.parse(replug.format());
        check("replug, connect again", false, reread.dropIfApplying("00ff"));
        check("replug, mode kept", "mode 00ff 2560x1440@60\n", reread.format());

        // The marker goes APPLYING_MS after the stored mode is first active with no trial.
        final long ms = GammaosScreenArrangement.APPLYING_MS;
        final GammaosScreenArrangement.Applying applying =
                new GammaosScreenArrangement.Applying("00ff");
        check("applying, not at it yet", -1L, applying.update(false, 1000));
        check("applying, at it", ms, applying.update(true, 30000));
        check("applying, still timing", ms - 5000, applying.update(true, 35000));
        check("applying, trial", -1L, applying.update(false, 36000));
        check("applying, back from the trial", ms, applying.update(true, 40000));
        check("applying, not yet", 1L, applying.update(true, 40000 + ms - 1));
        check("applying, done", 0L, applying.update(true, 40000 + ms));

        // A monitor's density follows its resolution.
        check("1080p", 213, GammaosScreenArrangement.monitorDensity(213.33f, 1920, 1080));
        check("720p", 142, GammaosScreenArrangement.monitorDensity(213.33f, 1280, 720));
        check("1440p ultrawide", 284, GammaosScreenArrangement.monitorDensity(213.33f, 3440, 1440));
        check("4K", 427, GammaosScreenArrangement.monitorDensity(213.33f, 3840, 2160));
        check("portrait", 213, GammaosScreenArrangement.monitorDensity(213.33f, 1080, 1920));
        check("no mode", 213, GammaosScreenArrangement.monitorDensity(213.33f, 0, 0));

        // The old external display modes, cleared with the panel as display 0. External as Primary
        // was on by default from build.prop, with no property set: any forced size goes, once.
        final Map<String, String> props = new HashMap<>();
        check("nothing set, first time", "[] true", oldModes(props, true));
        check("monitor is display 0", "[] false", oldModes(props, false));
        props.put("persist.gammaos.ext.cleared", "1");
        check("nothing set, done before", "[] false", oldModes(props, true));
        props.put("persist.gammaos.ext.mirror_resize", "true");
        props.put("persist.gammaos.ext.force_mirror", "false");
        props.put("persist.gammaos.ext.half_4k", "");
        check("mirror_resize on",
                "[persist.gammaos.ext.mirror_resize, persist.gammaos.ext.force_mirror] true",
                oldModes(props, true));
        check("monitor is display 0, mode on", "[] false", oldModes(props, false));
        props.clear();
        props.put("persist.gammaos.ext.cleared", "1");
        props.put("persist.gammaos.ext.primary", "1");
        check("primary on", "[persist.gammaos.ext.primary] true", oldModes(props, true));
        props.put("persist.gammaos.ext.primary", "0");
        props.put("persist.gammaos.ext.forced_default", "1");
        props.put("persist.gammaos.nano.ext_ask", "1");
        check("only set, not on",
                "[persist.gammaos.ext.primary, persist.gammaos.ext.forced_default, "
                        + "persist.gammaos.nano.ext_ask] false",
                oldModes(props, true));

        if (sFailures > 0) {
            System.out.println(sFailures + " failed");
            System.exit(1);
        }
        System.out.println("GammaosScreenArrangementCheck: all passed");
    }

    private static int autoMode(int[] widths, int[] heights, float[] rates, int atConnect) {
        return GammaosScreenArrangement.autoMode(widths, heights, rates, atConnect, false, false);
    }

    private static GammaosScreenArrangement.MonitorMode mode(String value) {
        return GammaosScreenArrangement.MonitorMode.parse(value);
    }

    private static String held(int[] widths, int[] heights, float[] rates,
            GammaosScreenArrangement.MonitorMode trial, GammaosScreenArrangement.MonitorMode stored,
            boolean dual) {
        final GammaosScreenArrangement.Choice c = GammaosScreenArrangement.monitorMode(widths,
                heights, rates, -1, trial, stored, dual, false);
        return c.index + " " + c.why;
    }

    private static String refusal(int[] widths, int[] heights, float[] rates, String m,
            boolean dual, boolean uhd) {
        return GammaosScreenArrangement.tryRefusal(widths, heights, rates, mode(m), dual, uhd);
    }

    private static String oldModes(Map<String, String> props, boolean panelIsDisplay0) {
        final GammaosScreenArrangement.OldModes m =
                GammaosScreenArrangement.oldModes(props, panelIsDisplay0);
        return m.clear + " " + m.clearForcedSize;
    }
}
