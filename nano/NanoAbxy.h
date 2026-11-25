// gammapad gives apps the face buttons by position, as on an Xbox pad: a Nintendo-layout pad's (the
// built-in pad, which reports them by print) are swapped, and ABXY Swap
// (persist.gammaos.gamepad.abxy_swap) then swaps every pad's. Nano's menus and drastic-nano's overlay read
// each pad's by its print (A confirms), so they undo that on gammapad's virtual pad (unswap); their game
// players take what apps get, by position (gameCode). gammapad says which
// layout each face button comes from with an EV_MSC MSC_SERIAL event before it
// (VirtualGamepad::writeEvent): 1 Nintendo, 2 Xbox.
#pragma once

#include <atomic>
#include <cutils/properties.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>

namespace nanoabxy {

inline int swapped(int code) {
    switch (code) {
        case BTN_A: return BTN_B;
        case BTN_B: return BTN_A;
        case BTN_X: return BTN_Y;
        case BTN_Y: return BTN_X;
        default: return code;
    }
}

inline bool swapOn() {
    return property_get_int32("persist.gammaos.gamepad.abxy_swap", 0) != 0;
}

// The virtual pad (gammapad's VirtualGamepad.cpp kPhysId).
inline bool isGammapadPad(int fd) {
    char phys[64] = {};
    return fd >= 0 && ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys) >= 0 && strcmp(phys, "gammapad-virtual") == 0;
}

// held: per face button held (BTN_A..BTN_Y order), the code it was given, plus kFlip when it came from
// an Xbox-layout pad (for modelCode) and kGameSwap when ABXY Swap applied to it (for gameCode).
constexpr unsigned short kFlip = 0x8000, kGameSwap = 0x4000, kCodeMask = 0x3fff;

struct FdState {
    std::atomic<unsigned char> tag{0};   // last tag: 0 none yet (taken as Nintendo), 1 Nintendo, 2 Xbox
    std::atomic<bool> flip{false};       // the face button last decoded came from an Xbox-layout pad
    std::atomic<bool> gameSwap{false};   // ABXY Swap applies to the face button last decoded
    std::atomic<unsigned short> held[4] = {};
};

inline FdState& stateOf(int fd) {
    static FdState sStates[1024];
    return sStates[(unsigned)fd % 1024u];
}

inline int faceIndex(int code) {
    switch (code) {
        case BTN_A: return 0;
        case BTN_B: return 1;
        case BTN_X: return 2;
        case BTN_Y: return 3;
        default: return -1;
    }
}

// Drops what fd's held face buttons were given: its events were discarded unread, or it is a newly
// opened device that may reuse a closed one's number.
inline void forget(int fd) {
    if (fd < 0) return;
    for (auto& h : stateOf(fd).held) h.store(0, std::memory_order_relaxed);
}

// A face button read from fd, as its print names it. Every event read from the fd goes through here,
// so the source tags are seen in order. A release gets the code its press got, whatever its tag: the
// pads share the virtual pad's key state, and gammapad's own releases carry the last pad's tag.
inline void unswap(int fd, input_event& ev) {
    if (fd < 0) return;
    if (ev.type == EV_MSC && ev.code == MSC_SERIAL && (ev.value == 1 || ev.value == 2)) {
        if (isGammapadPad(fd)) stateOf(fd).tag.store((unsigned char)ev.value, std::memory_order_relaxed);
        return;
    }
    const int i = ev.type == EV_KEY ? faceIndex(ev.code) : -1;
    if (i < 0) return;
    FdState& st = stateOf(fd);
    const unsigned short given = ev.value == 1 ? 0 : st.held[i].load(std::memory_order_relaxed);
    if (given) {
        ev.code = given & kCodeMask;
        st.flip.store((given & kFlip) != 0, std::memory_order_relaxed);
        st.gameSwap.store((given & kGameSwap) != 0, std::memory_order_relaxed);
        if (ev.value == 0) st.held[i].store(0, std::memory_order_relaxed);
        return;
    }
    bool flip = false, gameSwap = false;
    if (isGammapadPad(fd)) {
        flip = st.tag.load(std::memory_order_relaxed) == 2;
        gameSwap = swapOn();
        if (!flip != gameSwap) ev.code = swapped(ev.code);
    }
    st.flip.store(flip, std::memory_order_relaxed);
    st.gameSwap.store(gameSwap, std::memory_order_relaxed);
    if (ev.value != 0)
        st.held[i].store((unsigned short)(ev.code | (flip ? kFlip : 0) | (gameSwap ? kGameSwap : 0)),
                         std::memory_order_relaxed);
}

// The face button code just decoded from fd by unswap, as the built-in pad's model names that
// position (bmap::padModel for the virtual pad): an Xbox-layout pad's bottom button arrives as BTN_A
// but sits where the built-in pad's BTN_B does.
inline int modelCode(int fd, int code) {
    return fd >= 0 && faceIndex(code) >= 0 && stateOf(fd).flip.load(std::memory_order_relaxed) ? swapped(code) : code;
}

// The face button a game gets for the code just decoded from fd: the position as the built-in pad's
// print names it (modelCode), swapped by ABXY Swap as gammapad swaps it for apps.
inline int gameCode(int fd, int code) {
    const int m = modelCode(fd, code);
    return faceIndex(m) >= 0 && stateOf(fd).gameSwap.load(std::memory_order_relaxed) ? swapped(m) : m;
}

}  // namespace nanoabxy
