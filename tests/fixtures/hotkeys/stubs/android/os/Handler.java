package android.os;

import java.util.ArrayList;
import java.util.List;

/** A handler on SystemClock's virtual time: advanceTo runs what falls due, in order. */
public class Handler {
    private static final class Msg {
        final long when;
        final long seq;
        final Runnable r;

        Msg(long when, long seq, Runnable r) {
            this.when = when;
            this.seq = seq;
            this.r = r;
        }
    }

    private final List<Msg> mQueue = new ArrayList<>();
    private long mSeq;

    public final boolean post(Runnable r) {
        return postDelayed(r, 0);
    }

    public final boolean postDelayed(Runnable r, long delayMillis) {
        mQueue.add(new Msg(SystemClock.uptimeMillis() + Math.max(0, delayMillis), mSeq++, r));
        return true;
    }

    public final void removeCallbacks(Runnable r) {
        mQueue.removeIf(m -> m.r == r);
    }

    public void advanceTo(long ms) {
        for (;;) {
            Msg next = null;
            for (Msg m : mQueue) {
                if (m.when <= ms && (next == null || m.when < next.when
                        || (m.when == next.when && m.seq < next.seq))) {
                    next = m;
                }
            }
            if (next == null) break;
            mQueue.remove(next);
            SystemClock.sNow = Math.max(SystemClock.sNow, next.when);
            next.r.run();
        }
        SystemClock.sNow = Math.max(SystemClock.sNow, ms);
    }
}
