/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The firmware's nanoplat::Power (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6, G7-power): the
// battery, brightness and power-action code of NanoMenuSystem.cpp, moved verbatim, with that file's
// LOG_TAG. Each method's parameters carry the names the moved lines used in NanoMenu's member
// functions (pollBattery's mBatteryPercent and mBatteryCharging, by reference; setBrightnessViaHal's
// brightness; syncBrightnessToAndroid's mBrightness; prepareShutdown's action), so the lines are the
// same. The three health readers stay file-static, as they were; the brightness mirror's three
// counters stay function-local statics of the method that uses them.

#define LOG_TAG "GammaOSNano"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include <aidl/android/hardware/health/BatteryStatus.h>
#include <aidl/android/hardware/health/IHealth.h>
#include <aidl/android/hardware/light/ILights.h>
#include <aidl/android/hardware/light/HwLight.h>
#include <aidl/android/hardware/light/HwLightState.h>
#include <aidl/android/hardware/light/LightType.h>
#include <android/binder_manager.h>
#include <android/hardware/health/2.0/IHealth.h>   // HIDL fallback (Brick ships @2.0)

#include <cutils/properties.h>
#include <utils/Log.h>

#include "NanoMenu.h"   // nanoThreadNormalPriority
#include "NanoPlatform.h"
#include "Power.h"

namespace android {

// Try the framework's IHealth AIDL HAL for battery state. This is the same
// source of truth BatteryService reads, so values stay in sync with the
// rest of Android (settings, system UI, apps). Returns true on success.
// The cached std::shared_ptr<IHealth> avoids re-resolving the binder every
// second; if the service restarts, isOk() will fail and we drop the handle.
static bool queryHealthHal(int* outPercent, bool* outCharging) {
    using aidl::android::hardware::health::BatteryStatus;
    using aidl::android::hardware::health::IHealth;

    static std::shared_ptr<IHealth> sHal;
    if (!sHal) {
        ndk::SpAIBinder binder(AServiceManager_checkService(
                "android.hardware.health.IHealth/default"));
        if (!binder.get()) return false;
        sHal = IHealth::fromBinder(binder);
        if (!sHal) return false;
    }

    int32_t cap = -1;
    auto s1 = sHal->getCapacity(&cap);
    if (!s1.isOk()) {
        sHal.reset();
        return false;
    }
    if (cap < 0) cap = 0;
    if (cap > 100) cap = 100;
    *outPercent = cap;

    BatteryStatus status = BatteryStatus::UNKNOWN;
    auto s2 = sHal->getChargeStatus(&status);
    // If status query fails we still return a valid percent; charging stays
    // whatever it was. Better than dropping the whole reading.
    if (s2.isOk()) {
        *outCharging = (status == BatteryStatus::CHARGING
                        || status == BatteryStatus::FULL);
    }
    return true;
}

// HIDL @2.0 fallback: the TrimUI Brick (and other A14 vendor stacks) register
// android.hardware.health@2.0::IHealth/default, not the newer AIDL HAL. This is
// still the framework health HAL (BatteryService's source), not raw sysfs.
static bool queryHealthHidl(int* outPercent, bool* outCharging) {
    namespace H2 = android::hardware::health::V2_0;
    namespace H1 = android::hardware::health::V1_0;   // BatteryStatus lives here
    static android::sp<H2::IHealth> sHidl;
    if (sHidl == nullptr) {
        sHidl = H2::IHealth::getService("default");
        if (sHidl == nullptr) return false;
    }
    int cap = -1;
    auto r1 = sHidl->getCapacity([&](H2::Result res, int32_t v) {
        if (res == H2::Result::SUCCESS) cap = v;
    });
    if (!r1.isOk()) { sHidl = nullptr; return false; }
    if (cap < 0) return false;
    if (cap > 100) cap = 100;
    *outPercent = cap;
    sHidl->getChargeStatus([&](H2::Result res, H1::BatteryStatus st) {
        if (res == H2::Result::SUCCESS)
            *outCharging = (st == H1::BatteryStatus::CHARGING
                            || st == H1::BatteryStatus::FULL);
    });
    return true;
}

// Raw power_supply sysfs fallback. The IHealth HAL is the framework source of
// truth, but it is not always reachable from every process context (e.g. the
// resident nano overlay in the bootanim domain, or a minimal boot before the
// HAL registers). The kernel power_supply nodes are always present once the
// battery driver is up, so this keeps the indicator live when the binder path
// returns nothing. Scans /sys/class/power_supply/* for the first node that
// carries a capacity file and is of type "Battery".
static bool queryHealthSysfs(int* outPercent, bool* outCharging) {
    static const char* kBases[] = {
        "/sys/class/power_supply/battery",
        "/sys/class/power_supply/cw2015-battery",
        "/sys/class/power_supply/bat",
    };
    auto readInt = [](const char* path, int* out) -> bool {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        int v = -1;
        int n = fscanf(f, "%d", &v);
        fclose(f);
        if (n != 1) return false;
        *out = v;
        return true;
    };
    char path[256];
    for (const char* base : kBases) {
        snprintf(path, sizeof(path), "%s/capacity", base);
        int cap = -1;
        if (!readInt(path, &cap)) continue;
        if (cap < 0) cap = 0;
        if (cap > 100) cap = 100;
        *outPercent = cap;
        // status: "Charging"/"Full"/"Discharging"/"Not charging"/"Unknown"
        snprintf(path, sizeof(path), "%s/status", base);
        FILE* f = fopen(path, "r");
        if (f) {
            char st[32] = {};
            if (fgets(st, sizeof(st), f))
                *outCharging = (strncmp(st, "Charging", 8) == 0
                                || strncmp(st, "Full", 4) == 0);
            fclose(f);
        }
        return true;
    }
    return false;
}

namespace {

class FirmwarePower : public nanoplat::Power {
 public:
    void readBattery(int& mBatteryPercent, bool& mBatteryCharging) override {
        // Battery comes ONLY from the framework health HAL (BatteryService's source).
        // We deliberately do NOT read power_supply sysfs directly: in minimal boot the
        // HAL may not be registered yet, in which case the percentage stays unknown
        // (-1, indicator hidden) until the framework comes online and reports it.
        int pct = -1;
        bool charging = false;
        if (queryHealthHal(&pct, &charging) || queryHealthHidl(&pct, &charging)
            || queryHealthSysfs(&pct, &charging)) {
            mBatteryPercent = pct;
            mBatteryCharging = charging;
        }
        // Charging status: the IHealth getChargeStatus query is unreliable on this device (it
        // can leave the flag false while the charger is connected, so the DSi battery never
        // turns green). The power_supply sysfs "status" node is the kernel truth, so always
        // override the charging flag from it when it is readable (percentage still prefers the
        // HAL above). This makes plug/unplug reflect within the 1s poll.
        { int p2 = -1; bool c2 = false;
          if (queryHealthSysfs(&p2, &c2)) mBatteryCharging = c2; }
        // else: leave the last known value (or -1) until a source is reachable.
    }

    bool setBrightnessViaHal(int brightness) override {
        using aidl::android::hardware::light::ILights;
        using aidl::android::hardware::light::HwLight;
        using aidl::android::hardware::light::HwLightState;
        using aidl::android::hardware::light::LightType;

        ndk::SpAIBinder binder(
                AServiceManager_checkService("android.hardware.light.ILights/default"));
        if (!binder.get()) {
            return false;
        }
        std::shared_ptr<ILights> hal = ILights::fromBinder(binder);
        if (!hal) {
            return false;
        }

        std::vector<HwLight> lights;
        hal->getLights(&lights);
        // Drive EVERY backlight light, not just the first: dual-panel devices
        // (RG DS) can expose one HwLight per panel, and stopping at the first
        // left the second panel unblanked across sleep and untouched by the
        // brightness HUD.
        bool any = false;
        for (const auto& light : lights) {
            if (light.type == LightType::BACKLIGHT) {
                HwLightState state{};
                // Standard Android convention: brightness in alpha channel of ARGB
                state.color = 0xFF000000 | (brightness << 16) | (brightness << 8) | brightness;
                hal->setLightState(light.id, state);
                any = true;
            }
        }
        return any;
    }

    void mirrorBrightness(int mBrightness) override {
        static std::atomic<int> sPending{-1};      // latest level to push (-1 none)
        static std::atomic<int> sPushed{-1};       // last level handed to `settings put`
        static std::atomic<bool> sRunning{false};
        if (sPushed.load() == mBrightness && sPending.load() < 0) return;   // already mirrored
        sPending.store(mBrightness);
        if (sRunning.exchange(true)) return;       // the worker drains sPending before it exits
        std::thread([]() {
            nanoThreadNormalPriority();
            for (;;) {
                const int lvl = sPending.exchange(-1);
                if (lvl < 0) break;
                sPushed.store(lvl);
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "settings put system screen_brightness %d 2>/dev/null", lvl);
                (void)system(cmd);
            }
            sRunning.store(false);
            // A level queued between the last exchange and the flag clear is picked up by the next
            // call (it sees sPending >= 0 and starts a new worker).
        }).detach();
    }

    int savedBrightness() override {
        FILE* fp = popen("settings get system screen_brightness 2>/dev/null", "r");
        if (!fp) return -1;
        char buf[32] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            int val = atoi(buf);
            if (val >= 1 && val <= 255) return val;
        } else {
            pclose(fp);
        }
        return -1;
    }

    void powerAction(const char* action) override {
        // Proceed with the requested action
        property_set("service.bootanim.nano_action", action);
    }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::Power> nanofirmware::createPower() {
    return std::unique_ptr<nanoplat::Power>(new android::FirmwarePower);
}
