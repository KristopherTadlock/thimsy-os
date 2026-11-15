package android.hardware.input;

import android.os.SystemClock;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.List;

/** Records each injected key as {uptime ms, keycode, action, meta}. */
public final class InputManager {
    public static final int INJECT_INPUT_EVENT_MODE_ASYNC = 0;
    private static final InputManager sInstance = new InputManager();
    public final List<long[]> injected = new ArrayList<>();

    public static InputManager getInstance() {
        return sInstance;
    }

    public boolean injectInputEvent(KeyEvent ev, int mode) {
        injected.add(new long[] {SystemClock.uptimeMillis(), ev.getKeyCode(), ev.getAction(), ev.getMetaState()});
        return true;
    }
}
