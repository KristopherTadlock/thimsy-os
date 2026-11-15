package android.content;

public class Intent {
    public static final int FLAG_ACTIVITY_NEW_TASK = 0x10000000;

    public Intent setAction(String a) { return this; }
    public Intent setComponent(ComponentName c) { return this; }
    public Intent setPackage(String p) { return this; }
    public Intent putExtra(String k, boolean v) { return this; }
    public Intent putExtra(String k, int v) { return this; }
    public Intent putExtra(String k, String v) { return this; }
    public Intent addFlags(int f) { return this; }
}
