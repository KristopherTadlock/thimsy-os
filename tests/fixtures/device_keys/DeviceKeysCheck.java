package com.android.server.policy;

import org.json.JSONArray;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Runs ThimsyDeviceKeys.resolve over a cases file ({"table": path relative to it, "cases": [{"props":
 * {...}}, ...], "back_release": [{"shared", "used", "held"}, ...], "function_release": [{"home",
 * "hotkeys", "used", "held"}, ...], "function_hold": [{"home", "hotkeys"}, ...], "button_map":
 * [{"setting", "vendor", "product", "gammapad", "swap", "gammapad_vendor", "gammapad_product",
 * "remap"}, ...]}) and prints {"resolved": [...], "back_release": [...], "function_release": [...],
 * "function_hold": [...], "button_map": [keycode, ...]}, in the cases' order. "table": null resolves
 * with no table.
 */
public final class DeviceKeysCheck {
    public static void main(String[] args) throws Exception {
        Path casesPath = Paths.get(args[0]);
        JSONObject doc = new JSONObject(read(casesPath));
        String table = doc.isNull("table") ? null
                : read(casesPath.resolveSibling(doc.getString("table")));
        JSONArray cases = doc.getJSONArray("cases");
        JSONArray out = new JSONArray();
        for (int i = 0; i < cases.length(); i++) {
            JSONObject props = cases.getJSONObject(i).optJSONObject("props");
            final JSONObject p = props == null ? new JSONObject() : props;
            ThimsyDeviceKeys k = table == null ? ThimsyDeviceKeys.builtin()
                    : ThimsyDeviceKeys.resolve(table, name -> p.optString(name));
            JSONObject r = new JSONObject();
            r.put("id", k.id);
            JSONArray fl = new JSONArray(), fa = new JSONArray(), gp = new JSONArray(), bl = new JSONArray(),
                    ba = new JSONArray();
            for (ThimsyDeviceKeys.Key key : k.function) {
                fl.put(key.linux);
                fa.put(key.android);
                gp.put(key.gamepadOnly);
            }
            for (ThimsyDeviceKeys.Key key : k.back) {
                bl.put(key.linux);
                ba.put(key.android);
            }
            r.put("function_linux", fl);
            r.put("function_android", fa);
            r.put("gamepad_only", gp);
            r.put("back_linux", bl);
            r.put("back_android", ba);
            r.put("shared_back", k.sharedBack);
            r.put("home_function", k.hasHomeFunction());
            r.put("function_prompt", k.functionPrompt());
            out.put(r);
        }
        JSONArray release = new JSONArray();
        JSONArray rc = doc.optJSONArray("back_release");
        for (int i = 0; rc != null && i < rc.length(); i++) {
            JSONObject c = rc.getJSONObject(i);
            int r = ThimsyDeviceKeys.backRelease(c.getBoolean("shared"), c.getBoolean("used"),
                    c.getLong("held"));
            release.put(r == ThimsyDeviceKeys.BACK_RELEASE_BACK ? "back"
                    : r == ThimsyDeviceKeys.BACK_RELEASE_QUICK_MENU ? "quick_menu" : "nothing");
        }
        JSONArray fnRelease = new JSONArray();
        JSONArray fr = doc.optJSONArray("function_release");
        for (int i = 0; fr != null && i < fr.length(); i++) {
            JSONObject c = fr.getJSONObject(i);
            int r = ThimsyDeviceKeys.functionRelease(c.getBoolean("home"), c.getBoolean("hotkeys"),
                    c.getBoolean("used"), c.getLong("held"));
            fnRelease.put(r == ThimsyDeviceKeys.FUNCTION_RELEASE_QUICK_MENU ? "quick_menu" : "nothing");
        }
        JSONArray fnHold = new JSONArray();
        JSONArray fh = doc.optJSONArray("function_hold");
        for (int i = 0; fh != null && i < fh.length(); i++) {
            JSONObject c = fh.getJSONObject(i);
            int h = ThimsyDeviceKeys.functionHold(c.getBoolean("home"), c.getBoolean("hotkeys"));
            fnHold.put(h == ThimsyDeviceKeys.FUNCTION_HOLD_HOME ? "home"
                    : h == ThimsyDeviceKeys.FUNCTION_HOLD_QUIT ? "quit" : "nothing");
        }
        JSONArray buttonMap = new JSONArray();
        JSONArray bm = doc.optJSONArray("button_map");
        for (int i = 0; bm != null && i < bm.length(); i++) {
            JSONObject c = bm.getJSONObject(i);
            buttonMap.put(ThimsyDeviceKeys.buttonMapKeycode(c.getString("setting"),
                    Integer.parseInt(c.getString("vendor"), 16), Integer.parseInt(c.getString("product"), 16),
                    c.getBoolean("gammapad"), c.getBoolean("swap"),
                    Integer.parseInt(c.getString("gammapad_vendor"), 16),
                    Integer.parseInt(c.getString("gammapad_product"), 16), c.getString("remap")));
        }
        JSONObject result = new JSONObject();
        result.put("button_map", buttonMap);
        result.put("resolved", out);
        result.put("back_release", release);
        result.put("function_release", fnRelease);
        result.put("function_hold", fnHold);
        System.out.println(result.toString());
    }

    private static String read(Path p) throws Exception {
        return new String(Files.readAllBytes(p), StandardCharsets.UTF_8);
    }
}
