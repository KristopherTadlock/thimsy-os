package android.view;

import java.util.HashMap;
import java.util.Map;

/** The constants ThimsyHotkeyController uses, with Android's values. */
public class KeyEvent {
    public static final int ACTION_DOWN = 0;
    public static final int ACTION_UP = 1;
    public static final int FLAG_FROM_SYSTEM = 0x8;
    public static final int KEYCODE_UNKNOWN = 0;
    public static final int KEYCODE_R = 46;
    public static final int KEYCODE_F = 34;
    public static final int KEYCODE_Q = 45;
    public static final int KEYCODE_ALT_LEFT = 57;
    public static final int KEYCODE_SHIFT_LEFT = 59;
    public static final int KEYCODE_SPACE = 62;
    public static final int KEYCODE_CTRL_LEFT = 113;
    public static final int KEYCODE_F2 = 132;
    public static final int KEYCODE_F4 = 134;
    public static final int META_SHIFT_ON = 0x1;
    public static final int META_ALT_ON = 0x2;
    public static final int META_ALT_LEFT_ON = 0x10;
    public static final int META_SHIFT_LEFT_ON = 0x40;
    public static final int META_CTRL_ON = 0x1000;
    public static final int META_CTRL_LEFT_ON = 0x2000;

    private static final Map<String, Integer> CODES = new HashMap<>();
    static {
        CODES.put("KEYCODE_R", KEYCODE_R);
        CODES.put("KEYCODE_F", KEYCODE_F);
        CODES.put("KEYCODE_Q", KEYCODE_Q);
        CODES.put("KEYCODE_SPACE", KEYCODE_SPACE);
        CODES.put("KEYCODE_F2", KEYCODE_F2);
        CODES.put("KEYCODE_F4", KEYCODE_F4);
    }

    private final int mAction;
    private final int mCode;
    private final int mMeta;

    public KeyEvent(long downTime, long eventTime, int action, int code, int repeat, int metaState,
            int deviceId, int scancode, int flags, int source) {
        mAction = action;
        mCode = code;
        mMeta = metaState;
    }

    public static int keyCodeFromString(String name) {
        return CODES.getOrDefault(name, KEYCODE_UNKNOWN);
    }

    public final int getAction() { return mAction; }
    public final int getKeyCode() { return mCode; }
    public final int getMetaState() { return mMeta; }
}
