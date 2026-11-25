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

package com.android.server.wm;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

/**
 * RootWindowContainer's RetroArch config rule (NanoRetroArchConfig.java) against the cases
 * NanoRaConfig_test.cpp runs on Nano's (test/README.md has the command). Exits 1 on a failure.
 */
public final class NanoRetroArchConfigCheck {
    private static int sFailures;

    private static void check(String what, String expected, String actual) {
        if (!expected.equals(actual)) {
            System.out.println("FAIL " + what + ": expected " + expected + ", got " + actual);
            sFailures++;
        }
    }

    public static void main(String[] args) throws Exception {
        final String def = NanoRetroArchConfig.DEFAULT_CONFIG;
        final List<String> lines = Files.readAllLines(Paths.get(args[0]), StandardCharsets.UTF_8);
        int cases = 0;
        for (String line : lines) {
            if (line.isEmpty() || line.startsWith("#")) continue;
            final String[] f = line.split("\t", -1);
            if (f.length != 4) throw new IllegalArgumentException("bad case: " + line);
            final String dir = f[0].equals("-") ? "" : f[0];
            final StringBuilder mounts = new StringBuilder();
            if (!f[1].equals("-")) {
                for (String mp : f[1].split(" ")) {
                    mounts.append("/dev/fuse ").append(mp).append(" fuse rw,nosuid 0 0\n");
                }
            }
            final String root = NanoRetroArchConfig.volumeRoot(dir);
            final boolean mounted = root.isEmpty()
                    || NanoRetroArchConfig.mountListed(mounts.toString(), root);
            final String expected = f[3].equals("default") ? def : f[3];
            check(line, expected,
                    NanoRetroArchConfig.launchConfig(dir, mounted, f[2].equals("1")));
            // choose() takes "config there" from Nano's verified folder.
            check("choose " + line, expected, NanoRetroArchConfig.choose(
                    dir, f[2].equals("1") ? dir : "", mounts.toString()));
            cases++;
        }
        if (cases < 10) check("case count", ">= 10", Integer.toString(cases));

        final String card = "/storage/1234-ABCD/emulator_config/retroarch";
        final String mounts = "/dev/fuse /storage/emulated fuse rw 0 0\n"
                + "/dev/fuse /storage/1234-ABCD fuse rw 0 0";
        check("verified folder is another", def,
                NanoRetroArchConfig.choose(card, "/storage/9999-0000/emulator_config/retroarch",
                        mounts));
        check("nothing verified yet", def, NanoRetroArchConfig.choose(card, "", mounts));
        check("no setting, stale verified", def, NanoRetroArchConfig.choose("", card, mounts));
        check("card", card + "/retroarch.cfg", NanoRetroArchConfig.choose(card, card, mounts));

        System.out.println(sFailures == 0 ? "PASS " + cases + " cases" : sFailures + " failures");
        System.exit(sFailures == 0 ? 0 : 1);
    }
}
