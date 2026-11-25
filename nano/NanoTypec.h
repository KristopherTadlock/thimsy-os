// The USB-C port's power role, put right for an external monitor (NanoTypec.cpp has why).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace android {
namespace typec {

constexpr const char* kClass = "/sys/class/typec";
constexpr int64_t kFlipWaitMs = 5000;    // source-only with nothing attached this long: back to dual
constexpr int64_t kFlipSinkMs = 12000;   // source-only with something attached (it may take ~7 s to settle and
                                         // show DisplayPort): this long at most
constexpr int64_t kSinkWaitMs = 10000;   // sink-only this long at most, giving the source role back
constexpr int64_t kSettleMs = 2000;      // after a try, the plug re-attaching is not an unplug for this long
constexpr int64_t kUnplugMs = 1500;      // then nothing on the plug this long is an unplug: the next plug-in is tried

// A port under kClass: its name (port0), whether its port_type can be written, and its
// usb_power_delivery_revision ("0.0", or "0" on older kernels, for a port that cannot negotiate).
struct PortInfo {
    std::string name;
    bool typeWritable = false;
    std::string pdRevision;
};
// The port that negotiates and whose role can be set (the RG Cube has two: the PMIC's, which sees the
// plug, and the PD one, which this is; with the dock device tree's c2c mode the PMIC registers no port,
// so the PD one's number changes); "" when there is none. Found by what a port can do, never by its
// number.
std::string pdPort(const std::vector<PortInfo>& ports);

// What the ports' sysfs files say. A role file lists the choices with the current one in brackets:
// "source [sink]", "[dual] source sink".
struct View {
    bool plugged = false;          // any port has a partner (the PMIC's port sees the plug at once)
    bool partner = false;          // the PD port has a partner: it has settled with what is plugged in
    bool displayPort = false;      // the partner has a DisplayPort alternate mode, or a DP connector is up
    bool dpCapable = false;        // the device can show a picture over USB-C at all (else nothing is tried)
    bool host = false;             // a computer is connected over USB: never interrupted, all plug-in long
    bool hold = false;             // the USB mode is being applied (the gadget is down meanwhile): wait
    bool batteryLow = false;       // a charger's power is not cut for a try
    std::string powerRole;         // the PD port's: "source", "sink", "" when unreadable
    std::string dataRole;          // the PD port's: "host", "device", "" when unreadable
    std::string pd;                // its partner's supports_usb_power_delivery: "yes", "no", "" unknown
    std::string portType;          // "dual", "source", "sink", ""
};

// The state kept between steps, in a system property so a Nano process taking over from another
// carries on: "" nothing done for this plug-in; "flip:<ms>" source-only since then; "sink:<ms>"
// sink-only since then (a partner that took power but is no monitor gives it back); "kept:<ms>"
// a monitor took power then (it losing the PD port's partner is an unplug); "tried:<ms>" done for
// this plug-in, since then.
struct Step {
    std::string state;
    const char* write = nullptr;   // to write to the PD port's port_type: "source", "sink" or "dual"
    const char* log = nullptr;     // what happened, for the log
};

std::string current(const std::string& roleFile);   // the bracketed choice, "" when none
Step step(const View& v, const std::string& state, int64_t nowMs, int64_t unpluggedSinceMs);

}  // namespace typec
}  // namespace android
