package com.android.server.policy;

import android.os.Handler;
import android.os.SystemClock;
import android.hardware.input.InputManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.lang.reflect.Method;

/**
 * Drives ThimsyHotkeyController's drivers on a virtual clock (the stub Handler and SystemClock) and
 * prints what it injected. Reads a script on stdin, {"steps": [{"at": ms, "run": {driver spec},
 * "action": name} | {"at": ms, "reset": true}, ...], "until": ms}, and prints {"events": [{"at",
 * "key", "down", "meta"}, ...]}.
 */
public final class HotkeyControllerCheck {
    public static void main(String[] args) throws Exception {
        java.io.ByteArrayOutputStream in = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        for (int n; (n = System.in.read(buf)) > 0; ) in.write(buf, 0, n);
        JSONObject script = new JSONObject(new String(in.toByteArray(), "UTF-8"));
        Handler handler = new Handler();
        ThimsyHotkeyController c = new ThimsyHotkeyController(null, handler,
                new ThimsyHotkeyController.Host() {
                    @Override
                    public String foregroundPackage() {
                        return "com.example.emu";
                    }

                    @Override
                    public void exitApp() {
                    }

                    @Override
                    public void forceStopApp(String pkg) {
                    }
                });
        Method run = ThimsyHotkeyController.class.getDeclaredMethod("run", JSONObject.class,
                String.class, String.class);
        run.setAccessible(true);
        JSONArray steps = script.getJSONArray("steps");
        for (int i = 0; i < steps.length(); i++) {
            JSONObject step = steps.getJSONObject(i);
            handler.advanceTo(step.getLong("at"));
            if (step.optBoolean("reset")) {
                c.reset();
            } else {
                run.invoke(c, step.getJSONObject("run"), step.optString("action", "save_state"),
                        "com.example.emu");
            }
        }
        handler.advanceTo(script.getLong("until"));
        JSONArray events = new JSONArray();
        for (long[] e : InputManager.getInstance().injected) {
            JSONObject o = new JSONObject();
            o.put("at", e[0]);
            o.put("key", e[1]);
            o.put("down", e[2] == 0);
            o.put("meta", e[3]);
            events.put(o);
        }
        JSONObject out = new JSONObject();
        out.put("events", events);
        out.put("holding", c.isHolding());
        out.put("now", SystemClock.uptimeMillis());
        System.out.println(out.toString());
    }
}
