package com.android.server.gammaos;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Runs NanoUriGrant.parse on each line of stdin and prints one line for each: "package\tdocument
 * id", or "-" when the argument is refused.
 */
public final class UriGrantCheck {
    public static void main(String[] args) throws Exception {
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        StringBuilder out = new StringBuilder();
        for (String line; (line = in.readLine()) != null; ) {
            String[] g = NanoUriGrant.parse(line);
            out.append(g == null ? "-" : g[0] + "\t" + g[1]).append('\n');
        }
        System.out.print(out);
    }
}
