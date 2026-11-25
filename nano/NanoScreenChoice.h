// Dual screen mode: which screen an app opens on, and when the controller follows it to the
// built-in screen (persist.gammaos.nano.ctrl_screen).
#pragma once

#include <cstdint>
#include <string>

namespace nanoscreen {

// Controller Screen: the controller stays on the monitor, goes to an app opening on the built-in
// screen, or goes to the built-in screen when it is touched.
enum class CtrlMode { Monitor, Panel, Touched };

inline CtrlMode ctrlMode(const std::string& v) {
    if (v == "panel") return CtrlMode::Panel;
    if (v == "touched") return CtrlMode::Touched;
    return CtrlMode::Monitor;
}

inline bool focusOnLaunch(CtrlMode m) { return m == CtrlMode::Panel; }
inline bool focusOnTouch(CtrlMode m) { return m != CtrlMode::Monitor; }

// An app opens on the screen chosen for it; otherwise on the monitor, unless a game runs there.
inline bool openOnPanel(bool chosePanel, bool choseMonitor, bool gameOnMonitor) {
    if (chosePanel) return true;
    if (choseMonitor) return false;
    return gameOnMonitor;
}

// sys.gammaos.nano.focus.notice: a new value each time, naming the screen that took the controller.
inline std::string noticeValue(uint32_t count, int displayId) {
    return std::to_string(count) + ":" + std::to_string(displayId);
}

}  // namespace nanoscreen
