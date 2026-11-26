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

// The Linux display (display/Display.h) over a recording Kms, on a scratch property store, and
// readPanelOrientation (display/PanelOrientation.cpp) over a fake ioctl.

#include "display/Display.h"

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include <drm.h>
#include <drm_mode.h>

#include <cutils/properties.h>

#include "compat_test_util.h"

using namespace nanolinux::display;

namespace {

// Records every call as one word; "rot=<deg>" for setRotationDeg.
struct Recorder {
    std::vector<std::string> calls;
    bool active = true;
    bool splashFindsDisplay = true;
    bool takeBackOk = true;
    int takeBackTimeout = -1;
    std::string orientation;
    int overlayPlanes = 0;

    std::string joined() const {
        std::string s;
        for (const std::string& c : calls) s += (s.empty() ? "" : " ") + c;
        return s;
    }
};

class FakeKms : public Kms {
 public:
    explicit FakeKms(Recorder* r) : r_(r) {}
    void earlySplash(int fd) override {
        r_->calls.push_back("splash(" + std::to_string(fd) + ")");
        r_->active = r_->splashFindsDisplay;
    }
    void finishGpu() override { r_->calls.push_back("finish"); }
    bool active() const override { return r_->active; }
    void setRotationDeg(int deg) override { r_->calls.push_back("rot=" + std::to_string(deg)); }
    std::string panelOrientation() override {
        r_->calls.push_back("orientation");
        return r_->orientation;
    }
    void waitForFlips() override { r_->calls.push_back("flips"); }
    int disableOverlayPlanes() override {
        r_->calls.push_back("planes");
        return r_->overlayPlanes;
    }
    void rememberInstallMatrix() override { r_->calls.push_back("matrix"); }
    void stop() override {
        r_->calls.push_back("stop");
        r_->active = false;
    }
    bool takeBack(int timeoutMs) override {
        r_->calls.push_back("takeBack");
        r_->takeBackTimeout = timeoutMs;
        if (r_->takeBackOk) r_->active = true;
        return r_->takeBackOk;
    }

 private:
    Recorder* r_;
};

bool alwaysSf() { return true; }

class DisplayTest : public compat_test::StoreTest {
 protected:
    std::unique_ptr<nanoplat::Display> make() {
        Options o;
        o.fbconRotatePath = fbcon;
        return create(std::unique_ptr<Kms>(new FakeKms(&rec)), o);
    }
    void writeFbcon(const char* text) {
        FILE* f = fopen(fbcon.c_str(), "w");
        ASSERT_TRUE(f != nullptr);
        fputs(text, f);
        fclose(f);
    }
    Recorder rec;
    std::string fbcon = dir_ / "fbcon-rotate";  // absent unless a test writes it
};

TEST(LinuxDisplayRotation, Sources) {
    EXPECT_EQ(rotationFromPanelOrientation("Normal"), 0);
    EXPECT_EQ(rotationFromPanelOrientation("Right Side Up"), 90);
    EXPECT_EQ(rotationFromPanelOrientation("Upside Down"), 180);
    EXPECT_EQ(rotationFromPanelOrientation("Left Side Up"), 270);
    EXPECT_EQ(rotationFromPanelOrientation(""), -1);
    EXPECT_EQ(rotationFromPanelOrientation("right side up"), -1);
    EXPECT_EQ(rotationFromFbcon("0\n"), 0);
    EXPECT_EQ(rotationFromFbcon("1\n"), 90);
    EXPECT_EQ(rotationFromFbcon(" 2"), 180);
    EXPECT_EQ(rotationFromFbcon("3"), 270);
    EXPECT_EQ(rotationFromFbcon("4"), -1);
    EXPECT_EQ(rotationFromFbcon(""), -1);
    EXPECT_EQ(rotationFromDegrees("90"), 90);
    EXPECT_EQ(rotationFromDegrees("270\n"), 270);
    EXPECT_EQ(rotationFromDegrees("0"), 0);
    EXPECT_EQ(rotationFromDegrees("45"), -1);
    EXPECT_EQ(rotationFromDegrees(""), -1);
}

TEST(LinuxDisplayCreate, NeedsKms) { EXPECT_TRUE(create(nullptr) == nullptr); }

TEST_F(DisplayTest, StartScanoutAlwaysTakesThePanel) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.orientation = "Normal";
    // The firmware's reasons to leave the panel to SurfaceFlinger don't exist on Linux.
    EXPECT_EQ(d->startScanout(-1, false, alwaysSf), -1);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=0");
}

TEST_F(DisplayTest, StartScanoutPassesTheEarlyFd) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.orientation = "Normal";
    EXPECT_EQ(d->startScanout(7, true, nullptr), -1);  // drmEarlySplash owns the fd
    EXPECT_EQ(rec.calls.at(0), "splash(7)");
}

TEST_F(DisplayTest, NoDisplayNoRotation) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.splashFindsDisplay = false;
    EXPECT_EQ(d->startScanout(-1, false, nullptr), -1);
    EXPECT_EQ(rec.joined(), "splash(-1)");
    EXPECT_FALSE(d->releaseForChild());
}

TEST_F(DisplayTest, RotationFromThePanelOrientationFirst) {
    writeFbcon("3\n");
    property_set("ro.thimsy.device.panel.rotation", "180");
    rec.orientation = "Right Side Up";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=90");
}

TEST_F(DisplayTest, RotationFromDeviceJsonWithoutTheProperty) {
    writeFbcon("3\n");
    property_set("ro.thimsy.device.panel.rotation", "90");
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=90");
}

TEST_F(DisplayTest, FbconZeroDoesNotBeatDeviceJson) {
    // A DSI panel with no orientation property: the kernel's fbcon hint reads 0 too.
    writeFbcon("0\n");
    property_set("ro.thimsy.device.panel.rotation", "270");
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=270");
}

TEST_F(DisplayTest, RotationFromFbconLast) {
    writeFbcon("3\n");
    property_set("ro.thimsy.device.panel.rotation", "garbage");
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=270");
}

TEST_F(DisplayTest, DrmSourceNamedExplicitly) {
    property_set("ro.thimsy.device.panel.rotation_source", "drm-panel-orientation");
    property_set("ro.thimsy.device.panel.rotation", "90");
    rec.orientation = "Normal";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=0");
}

TEST_F(DisplayTest, DeviceJsonSourceOverridesDrmNormal) {
    property_set("ro.thimsy.device.panel.rotation_source", "device-json");
    property_set("ro.thimsy.device.panel.rotation", "270");
    writeFbcon("1\n");
    rec.orientation = "Normal";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) rot=270");  // the property is not even read
}

TEST_F(DisplayTest, DeviceTreeSourceFallsBackToDrm) {
    property_set("ro.thimsy.device.panel.rotation_source", "device-tree");
    rec.orientation = "Upside Down";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=180");
}

TEST_F(DisplayTest, FbconSourceFirst) {
    property_set("ro.thimsy.device.panel.rotation_source", "fbcon");
    property_set("ro.thimsy.device.panel.rotation", "270");
    writeFbcon("1\n");
    rec.orientation = "Left Side Up";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) rot=90");
}

TEST_F(DisplayTest, FbconSourceThenDeviceJson) {
    property_set("ro.thimsy.device.panel.rotation_source", "fbcon");
    property_set("ro.thimsy.device.panel.rotation", "270");
    rec.orientation = "Right Side Up";
    make()->startScanout(-1, false, nullptr);  // no fbcon file
    EXPECT_EQ(rec.joined(), "splash(-1) rot=270");
}

TEST_F(DisplayTest, UnknownSourceUsesTheDefaultOrder) {
    property_set("ro.thimsy.device.panel.rotation_source", "drm");
    property_set("ro.thimsy.device.panel.rotation", "90");
    rec.orientation = "Upside Down";
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=180");
}

TEST_F(DisplayTest, RotationZeroWithNoSource) {
    make()->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1) orientation rot=0");
}

TEST_F(DisplayTest, SurfaceFlingerPropertyWins) {
    property_set("ro.surface_flinger.primary_display_orientation", "ORIENTATION_270");
    rec.orientation = "Right Side Up";
    std::unique_ptr<nanoplat::Display> d = make();
    d->startScanout(-1, false, nullptr);
    EXPECT_EQ(rec.joined(), "splash(-1)");  // drmEarlySplash applied it
    ASSERT_TRUE(d->releaseForChild());
    ASSERT_TRUE(d->reacquire());
    EXPECT_EQ(rec.joined(), "splash(-1) finish flips planes matrix stop takeBack");
}

TEST_F(DisplayTest, HandOffOrder) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.orientation = "Left Side Up";
    rec.overlayPlanes = 1;
    d->startScanout(-1, false, nullptr);
    rec.calls.clear();
    ASSERT_TRUE(d->releaseForChild());
    EXPECT_EQ(rec.joined(), "finish flips planes matrix stop");
    EXPECT_FALSE(d->releaseForChild());  // already released: nothing more is called
    EXPECT_EQ(rec.joined(), "finish flips planes matrix stop");
    rec.calls.clear();
    ASSERT_TRUE(d->reacquire());
    // The re-splash re-read the orientation property; the chosen rotation is put back, not chosen again.
    EXPECT_EQ(rec.joined(), "takeBack rot=270");
    EXPECT_EQ(rec.takeBackTimeout, 8000);
    rec.calls.clear();
    EXPECT_TRUE(d->reacquire());  // not released: only reports whether the panel is Nano's
    EXPECT_EQ(rec.joined(), "");
}

TEST_F(DisplayTest, ReacquireFailureCanBeRetried) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.orientation = "Normal";
    d->startScanout(-1, false, nullptr);
    ASSERT_TRUE(d->releaseForChild());
    rec.calls.clear();
    rec.takeBackOk = false;
    EXPECT_FALSE(d->reacquire());
    rec.takeBackOk = true;
    EXPECT_TRUE(d->reacquire());
    EXPECT_EQ(rec.joined(), "takeBack takeBack rot=0");
}

TEST_F(DisplayTest, ReleaseNeedsThePanel) {
    std::unique_ptr<nanoplat::Display> d = make();
    rec.active = false;
    EXPECT_FALSE(d->releaseForChild());
    EXPECT_EQ(rec.joined(), "");
    EXPECT_FALSE(d->reacquire());
}

// --- readPanelOrientation over a fake connector ---------------------------------------------------

struct FakeProp {
    uint32_t id;
    const char* name;
    uint32_t flags;
    uint64_t value;
    std::vector<std::pair<uint64_t, const char*>> enums;
};

struct FakeDrm {
    uint32_t connector = 42;
    std::vector<FakeProp> props;
    bool failGetProperties = false;
    int calls = 0;
};
FakeDrm* gDrm = nullptr;

int fakeIoctl(int fd, unsigned long request, void* arg) {
    gDrm->calls++;
    if (fd != 9) return -1;
    if (request == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
        auto* op = static_cast<drm_mode_obj_get_properties*>(arg);
        if (gDrm->failGetProperties || op->obj_id != gDrm->connector || op->obj_type != DRM_MODE_OBJECT_CONNECTOR)
            return -1;
        const uint32_t n = (uint32_t)gDrm->props.size();
        if (op->count_props >= n && op->props_ptr && op->prop_values_ptr) {
            auto* ids = reinterpret_cast<uint32_t*>((uintptr_t)op->props_ptr);
            auto* values = reinterpret_cast<uint64_t*>((uintptr_t)op->prop_values_ptr);
            for (uint32_t i = 0; i < n; i++) {
                ids[i] = gDrm->props[i].id;
                values[i] = gDrm->props[i].value;
            }
        }
        op->count_props = n;
        return 0;
    }
    if (request == DRM_IOCTL_MODE_GETPROPERTY) {
        auto* gp = static_cast<drm_mode_get_property*>(arg);
        for (const FakeProp& p : gDrm->props) {
            if (p.id != gp->prop_id) continue;
            snprintf(gp->name, sizeof(gp->name), "%s", p.name);
            gp->flags = p.flags;
            const uint32_t n = (uint32_t)p.enums.size();
            if (gp->count_enum_blobs >= n && gp->enum_blob_ptr) {
                auto* out = reinterpret_cast<drm_mode_property_enum*>((uintptr_t)gp->enum_blob_ptr);
                for (uint32_t e = 0; e < n; e++) {
                    out[e].value = p.enums[e].first;
                    snprintf(out[e].name, sizeof(out[e].name), "%s", p.enums[e].second);
                }
            }
            gp->count_enum_blobs = n;
            gp->count_values = n;
            return 0;
        }
        return -1;
    }
    return -1;
}

class PanelOrientationTest : public ::testing::Test {
 protected:
    void SetUp() override {
        gDrm = &drm;
        drm.props.push_back({1, "EDID", DRM_MODE_PROP_BLOB, 0, {}});
        drm.props.push_back({2, "DPMS", DRM_MODE_PROP_ENUM, 0, {{0, "On"}, {3, "Off"}}});
    }
    void TearDown() override { gDrm = nullptr; }
    void addOrientation(uint64_t value) {
        drm.props.push_back({7, "panel orientation", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, value,
                             {{0, "Normal"}, {1, "Upside Down"}, {2, "Left Side Up"}, {3, "Right Side Up"}}});
    }
    FakeDrm drm;
};

TEST_F(PanelOrientationTest, ReadsTheEnumName) {
    addOrientation(3);
    EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string("Right Side Up"));
    drm.props.back().value = 0;
    EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string("Normal"));
}

TEST_F(PanelOrientationTest, NoProperty) { EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string()); }

TEST_F(PanelOrientationTest, Failures) {
    addOrientation(2);
    EXPECT_EQ(readPanelOrientation(-1, 42, fakeIoctl), std::string());
    EXPECT_EQ(readPanelOrientation(9, 0, fakeIoctl), std::string());
    EXPECT_EQ(readPanelOrientation(9, 42, nullptr), std::string());
    EXPECT_EQ(readPanelOrientation(9, 41, fakeIoctl), std::string());  // another connector
    drm.failGetProperties = true;
    EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string());
}

TEST_F(PanelOrientationTest, UnknownValue) {
    addOrientation(9);
    EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string());
}

TEST_F(PanelOrientationTest, NotAnEnum) {
    drm.props.push_back({7, "panel orientation", DRM_MODE_PROP_RANGE, 3, {}});
    EXPECT_EQ(readPanelOrientation(9, 42, fakeIoctl), std::string());
}

}  // namespace
