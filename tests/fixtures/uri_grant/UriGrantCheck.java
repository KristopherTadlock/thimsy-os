package com.android.server.gammaos;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Runs NanoUriGrant.parse on each line of stdin and prints one line for each: "package\tdocument
 * id", or "-" when the argument is refused. With the argument "resolve", each line is "command
 * argument\tfile text" ("-": no file) and each answer "argFile\tresolve", "-" for null.
 */
public final class UriGrantCheck {
    public static void main(String[] args) throws Exception {
        final boolean resolve = args.length > 0 && args[0].equals("resolve");
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
        StringBuilder out = new StringBuilder();
        for (String line; (line = in.readLine()) != null; ) {
            if (resolve) {
                final String[] f = line.split("\t", -1);
                final String file = NanoUriGrant.argFile(f[0]);
                final String r = NanoUriGrant.resolve(f[0], f[1].equals("-") ? null : f[1]);
                out.append(file == null ? "-" : file).append('\t').append(r == null ? "-" : r).append('\n');
                continue;
            }
            String[] g = NanoUriGrant.parse(line);
            out.append(g == null ? "-" : g[0] + "\t" + g[1]).append('\n');
        }
        System.out.print(out);
    }
}
