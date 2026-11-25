// GammaOS Nano: the USB-C port's power role, put right for an external monitor.
//
// The RG Cube's port is dual role, and so is a monitor's. Which of the two supplies power decides
// which is the USB host, and only the host can start DisplayPort over the cable. When the Cube
// supplies power it is the host, negotiates (USB PD) and the monitor's picture comes up. When the
// monitor supplies power the Cube waits for its offer, which the monitors seen never make: after
// two hard resets the Cube stays a powered device with no picture, plug-in after plug-in, until a
// reboot happens to change the order.
//
// So when the Cube takes power from something that did not negotiate, the PD port is made
// source-only for a moment. A dual-role partner then takes power from the Cube instead. A monitor
// (it shows a DisplayPort alternate mode, or the picture comes up) keeps it, and the port goes back
// to dual role; anything else that took power (a phone, a cheap power bank) is given the source role
// back (sink-only, then dual role). A charger or a computer port cannot take power: after
// kFlipWaitMs the port goes back to dual role and charging resumes. Once per plug-in.
//
// A dock is left alone: what powers the Cube and negotiated, or made the Cube the USB host, has
// settled the roles its hub and DisplayPort need. Only the port's port_type is ever written, never
// the USB controller's own mode in debugfs: reading or writing that file while the controller is
// suspended panics the kernel.
//
// Android's USB HAL also writes dual role to a port with no partner, on every Type-C uevent; it runs
// as system and port_type is root's, so today it cannot (its "Cannot open file to switch back to
// drp" errors). Were it given the file, it would undo these writes at once.

#define LOG_TAG "GammaOSNano"

#include "NanoTypec.h"

#include <cstdlib>

namespace android {
namespace typec {

std::string pdPort(const std::vector<PortInfo>& ports) {
    for (const auto& p : ports)
        if (p.typeWritable && atof(p.pdRevision.c_str()) > 0.0) return p.name;
    return std::string();
}

std::string current(const std::string& roleFile) {
    const size_t a = roleFile.find('[');
    const size_t b = a == std::string::npos ? std::string::npos : roleFile.find(']', a);
    return b == std::string::npos ? std::string() : roleFile.substr(a + 1, b - a - 1);
}

// The time in a state ("flip:<ms>"), -1 when it is garbled.
static int64_t since(const std::string& state, size_t prefix) {
    const char* t = state.c_str() + prefix;
    char* end = nullptr;
    const long long v = strtoll(t, &end, 10);
    return end == t || *end != 0 || v < 0 ? -1 : v;
}

Step step(const View& v, const std::string& state, int64_t nowMs, int64_t unpluggedSinceMs) {
    Step s;
    s.state = state;
    const std::string tried = "tried:" + std::to_string(nowMs);
    if (state.compare(0, 5, "flip:") == 0) {
        const int64_t t = since(state, 5);
        const bool taken = v.partner && v.powerRole == "source";
        const bool late = t < 0 || nowMs < t || nowMs - t >= (v.plugged ? kFlipSinkMs : kFlipWaitMs);
        if (v.partner && v.powerRole == "sink" && v.dataRole == "host") {
            // Back as a powered USB host despite the source-only port: a dock that supplies power and
            // takes the device role. It is left as it is, dual role, for the rest of the plug-in.
            s.state = tried;
            s.write = "dual";
            s.log = "a dock powers the Cube; the port is dual role again";
        } else if (taken && v.displayPort) {
            s.state = "kept:" + std::to_string(nowMs);
            s.write = "dual";
            s.log = "a monitor takes power from the Cube; the port is dual role again";
        } else if (taken && (v.pd == "no" || late)) {
            s.state = "sink:" + std::to_string(nowMs);
            s.write = "sink";
            s.log = "what took power from the Cube is no monitor; giving it the source role back";
        } else if (late) {
            s.state = tried;
            s.write = "dual";
            s.log = "nothing took power from the Cube; the port is dual role again";
        }
        return s;
    }
    if (state.compare(0, 5, "sink:") == 0) {
        const int64_t t = since(state, 5);
        if ((v.partner && v.powerRole == "sink") || t < 0 || nowMs < t || nowMs - t >= kSinkWaitMs) {
            s.state = tried;
            s.write = "dual";
            s.log = "the port is dual role again";
        }
        return s;
    }
    if (state.compare(0, 5, "kept:") == 0) {
        // The monitor gone from the PD port once settled (unplugged, re-seated, switched off): the
        // next plug-in is tried, however soon.
        const int64_t t = since(state, 5);
        if ((!v.partner && nowMs - t >= kSettleMs) || t < 0 || nowMs < t) s.state.clear();
        return s;
    }
    if (state.compare(0, 6, "tried:") == 0) {
        // Unplugged after the try has settled (not the try's own detach and re-attach): the next
        // plug-in is tried.
        const int64_t t = since(state, 6);
        const int64_t settled = t + kSettleMs;
        const int64_t from = unpluggedSinceMs > settled ? unpluggedSinceMs : settled;
        if (!v.plugged && unpluggedSinceMs > 0 && nowMs - from >= kUnplugMs) s.state.clear();
        if (t < 0 || nowMs < t) s.state.clear();
        return s;
    }
    if (!state.empty()) s.state.clear();   // garbled: start over
    if (v.host) {   // a computer: left alone for the whole plug-in, even while its link is down later
        s.state = tried;
        return s;
    }
    if (v.partner && v.powerRole == "sink" && (v.pd == "yes" || v.dataRole == "host")) {
        // What powers the Cube negotiated, or made it the USB host: a charger or a dock, whose USB and
        // DisplayPort need the roles they settled on. Left alone for the whole plug-in.
        s.state = tried;
        return s;
    }
    if (v.dpCapable && !v.hold && !v.batteryLow && v.partner && v.powerRole == "sink" && v.pd == "no" &&
        v.portType == "dual") {
        s.state = "flip:" + std::to_string(nowMs);
        s.write = "source";
        s.log = "taking power from a partner that did not negotiate; offering power instead";
    }
    return s;
}

}  // namespace typec
}  // namespace android
