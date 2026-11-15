package android.os;

public final class SystemClock {
    static long sNow;

    public static long uptimeMillis() {
        return sNow;
    }
}
