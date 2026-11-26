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

// The layer-2 stubs (nanostubs/stub.h): every header compiles in one translation unit with the
// compat shims, Nano's call patterns compile against them (each block below copies the shape of a
// call site in a [core] file, named in its comment), and at run time everything is inert: creates
// and lookups fail, nothing crashes, and each stubbed function logs once.

#include <aaudio/AAudio.h>
#include <aidl/android/hardware/health/BatteryStatus.h>
#include <aidl/android/hardware/health/IHealth.h>
#include <aidl/android/hardware/light/HwLight.h>
#include <aidl/android/hardware/light/HwLightState.h>
#include <aidl/android/hardware/light/ILights.h>
#include <aidl/android/hardware/light/LightType.h>
#include <android/binder_manager.h>
#include <android/gui/BnScreenCaptureListener.h>
#include <android/hardware/health/2.0/IHealth.h>
#include <android/hardware/light/2.0/ILight.h>
#include <android/hardware_buffer.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/performance_hint.h>
#include <android/sensor.h>
#include <binder/IBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <cutils/native_handle.h>
#include <gui/BufferItem.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/DisplayCaptureArgs.h>
#include <gui/GLConsumer.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/ISurfaceComposer.h>
#include <gui/LayerState.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceControl.h>
#include <gui/SyncScreenCaptureListener.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <nanocompat/nanocompat.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicTypes.h>
#include <ui/LayerStack.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/StaticDisplayInfo.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/Thread.h>
#include <vndk/hardware_buffer.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

std::mutex gLogMutex;
std::vector<std::string> gLog;

void captureLog() {
    nanocompat::setLogSink([](int, const char* tag, const char* msg) {
        std::lock_guard<std::mutex> lk(gLogMutex);
        gLog.push_back(std::string(tag ? tag : "") + ": " + (msg ? msg : ""));
    });
}

// Installed before any test runs, so every stub's first call is seen.
const bool gCapturing = (captureLog(), true);

size_t logLinesWith(const std::string& needle) {
    std::lock_guard<std::mutex> lk(gLogMutex);
    size_t n = 0;
    for (const std::string& l : gLog)
        if (l.find(needle) != std::string::npos) n++;
    return n;
}

}  // namespace

namespace android {

// NanoMenu.h: class NanoMenu : public Thread, public IBinder::DeathRecipient.
class FakeMenu : public Thread, public IBinder::DeathRecipient {
 public:
    void binderDied(const wp<IBinder>&) override { died = true; }
    bool died = false;

 private:
    bool threadLoop() override { return false; }
};

// NanoMenuPS3Clock.cpp: class NanoPspCaptureListener : public gui::BnScreenCaptureListener.
class FakeCaptureListener : public gui::BnScreenCaptureListener {
 public:
    binder::Status onScreenCaptureCompleted(const gui::ScreenCaptureResults& results) override {
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mResults = results;
            mDone = true;
        }
        mCv.notify_one();
        return binder::Status::ok();
    }
    bool wait(int timeoutMs, gui::ScreenCaptureResults& out) {
        std::unique_lock<std::mutex> lk(mMutex);
        if (!mCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return mDone; })) return false;
        out = mResults;
        return true;
    }

 private:
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mDone = false;
    gui::ScreenCaptureResults mResults;
};

TEST(SurfaceFlinger, NoDisplaysAndNullFactories) {
    sp<FakeMenu> menu = new FakeMenu();
    sp<SurfaceComposerClient> session = new SurfaceComposerClient();
    EXPECT_NE(session->linkToComposerDeath(menu.get()), NO_ERROR);   // NanoMenu.cpp: linkToComposerDeath(this)

    const std::vector<PhysicalDisplayId> ids = SurfaceComposerClient::getPhysicalDisplayIds();
    EXPECT_TRUE(ids.empty());
    PhysicalDisplayId pid;
    EXPECT_EQ((int)pid.getPort(), 0);
    sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(pid);
    EXPECT_TRUE(token == nullptr);

    ui::DisplayState state;
    EXPECT_NE(SurfaceComposerClient::getDisplayState(token, &state), NO_ERROR);
    EXPECT_TRUE(state.layerStack == ui::INVALID_LAYER_STACK);
    EXPECT_TRUE(state.orientation == ui::ROTATION_0);
    ui::LayerStack stack = ui::DEFAULT_LAYER_STACK;
    EXPECT_TRUE(stack != ui::INVALID_LAYER_STACK);
    EXPECT_EQ(ui::LayerStack::fromValue(0x6E414D6F).id, 0x6E414D6Fu);
    ui::DisplayMode mode;
    EXPECT_NE(SurfaceComposerClient::getActiveDisplayMode(token, &mode), NO_ERROR);
    ui::StaticDisplayInfo info;
    EXPECT_NE(SurfaceComposerClient::getStaticDisplayInfo(pid.value, &info), NO_ERROR);
    EXPECT_FALSE(info.connectionType != ui::DisplayConnectionType::Internal);
    EXPECT_FALSE(info.deviceProductInfo.has_value());
    SurfaceComposerClient::setDisplayPowerMode(token, 2);

    // NanoMenu.cpp, readyToRun: the surface, its transaction and its window.
    ui::Size resolution = mode.resolution;
    sp<SurfaceControl> control = session->createSurface(String8("GammaOSNano"), resolution.getWidth(),
                                                        resolution.getHeight(), PIXEL_FORMAT_RGBX_8888,
                                                        true ? 0u : ISurfaceComposerClient::eOpaque);
    EXPECT_TRUE(control == nullptr);
    SurfaceComposerClient::Transaction t;
    Rect forcedRes(0, 0, resolution.width, resolution.height);
    EXPECT_EQ(forcedRes.getWidth(), -1);
    t.setDisplayProjection(token, ui::ROTATION_0, forcedRes, Rect(4, 3));
    t.setLayerStack(control, stack);
    t.setFrameRate(control, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                   ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
    t.show(control).hide(control).reparent(control, nullptr).setLayer(control, 0x7fffffff);
    t.setMatrix(control, 1.0f, 0.0f, 0.0f, 1.0f);
    t.setDisplayLayerStack(token, stack);
    EXPECT_NE(t.apply(), NO_ERROR);
    // NanoMenuOverlay.cpp: a chained temporary.
    SurfaceComposerClient::Transaction()
        .setFlags(control, false ? layer_state_t::eLayerOpaque : 0u, layer_state_t::eLayerOpaque)
        .setBackgroundBlurRadius(control, 0)
        .apply();
}

TEST(SurfaceFlinger, MirrorCaptureAndConsumers) {
    // NanoMenuPS3Clock.cpp: the mirror's buffer queue and consumer.
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    EXPECT_TRUE(producer == nullptr);
    sp<BufferItemConsumer> bic = new BufferItemConsumer(consumer, GraphicBuffer::USAGE_HW_TEXTURE, 4, false);
    bic->setName(String8("nano-clock-mirror"));
    bic->setDefaultBufferSize(16u, 16u);
    bic->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888);
    BufferItem item;
    EXPECT_NE(bic->acquireBuffer(&item, 0, false), NO_ERROR);
    bic->releaseBuffer(item, Fence::NO_FENCE);
    EXPECT_TRUE(item.mGraphicBuffer == nullptr);
    if (item.mFence != nullptr && item.mFence != Fence::NO_FENCE) item.mFence->wait(10);

    sp<SurfaceComposerClient> sess = new SurfaceComposerClient();
    std::optional<PhysicalDisplayId> physId = PhysicalDisplayId();
    sp<SurfaceControl> mirror = sess->mirrorDisplay(*physId);
    EXPECT_TRUE(mirror == nullptr);
    sp<IBinder> disp = SurfaceComposerClient::createDisplay(String8("nano-clock-mirror"), false);
    EXPECT_TRUE(disp == nullptr);
    SurfaceComposerClient::Transaction t;
    EXPECT_NE(t.setDisplaySurface(disp, producer), NO_ERROR);
    SurfaceComposerClient::destroyDisplay(disp);

    // The capture worker's arguments and listener.
    gui::DisplayCaptureArgs args;
    args.displayToken = disp;
    args.width = 0;
    args.pixelFormat = ui::PixelFormat::RGBA_8888;
    args.captureSecureLayers = false;
    args.allowProtected = false;
    std::vector<sp<IBinder>> excludes(2);
    for (const sp<IBinder>& h : excludes)
        if (h != nullptr) args.excludeHandles.insert(h);
    args.excludeHandles.insert(nullptr);
    EXPECT_EQ(args.excludeHandles.size(), 1u);
    sp<FakeCaptureListener> listener = new FakeCaptureListener();
    EXPECT_NE(ScreenshotClient::captureDisplay(args, listener), NO_ERROR);
    gui::ScreenCaptureResults res;
    EXPECT_FALSE(listener->wait(1, res));
    EXPECT_TRUE(res.fenceResult.ok());
    sp<Fence> f = res.fenceResult.value_or(Fence::NO_FENCE);
    EXPECT_TRUE(f == Fence::NO_FENCE);
    res = gui::ScreenCaptureResults();

    // NanoMenuRender.cpp: the in-process overlay capture.
    DisplayId did = PhysicalDisplayId();
    gui::CaptureArgs cargs;
    sp<SyncScreenCaptureListener> sync = new SyncScreenCaptureListener();
    EXPECT_NE(ScreenshotClient::captureDisplay(did, cargs, sync), NO_ERROR);
    ScreenCaptureResults r2 = sync->waitForResults();
    EXPECT_TRUE(r2.buffer == nullptr);
    sp<GraphicBuffer> buf = new GraphicBuffer();
    void* base = &buf;
    EXPECT_NE(buf->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &base), NO_ERROR);
    EXPECT_TRUE(base == nullptr);
    EXPECT_EQ(buf->getWidth() + buf->getHeight() + buf->getStride(), 0u);
    EXPECT_TRUE((void*)buf->getNativeBuffer() == nullptr);
    EXPECT_EQ(buf->getId(), 0u);
    buf->unlock();

    // NanoVideo.cpp: the OES path's consumer and window.
    sp<GLConsumer> glc = new GLConsumer(consumer, 7u, 0x8D65u, true, false);
    glc->setName(String8("NanoVideo"));
    glc->setDefaultBufferSize(16u, 16u);
    EXPECT_NE(glc->updateTexImage(), NO_ERROR);
    float m[16];
    glc->getTransformMatrix(m);
    EXPECT_EQ(m[0], 1.0f);
    EXPECT_EQ(m[1], 0.0f);
    glc->abandon();
    sp<Surface> s = new Surface(producer);
    ANativeWindow* win = s.get();
    EXPECT_TRUE(win != nullptr);
    EXPECT_NE(s->setMaxDequeuedBufferCount(3), 0);
    EXPECT_NE(native_window_set_buffers_user_dimensions(s.get(), 4, 4), 0);
    void* asEglWindow = s.get();   // EGL_NO_PLATFORM_SPECIFIC_TYPES: EGLNativeWindowType is void*
    EXPECT_TRUE(asEglWindow != nullptr);
}

TEST(Binder, ServiceManagerKnowsNothing) {
    sp<IServiceManager> sm = defaultServiceManager();
    ASSERT_TRUE(sm != nullptr);
    EXPECT_TRUE(sm == defaultServiceManager());
    EXPECT_TRUE(sm->checkService(String16("SurfaceFlinger")) == nullptr);
    EXPECT_TRUE(defaultServiceManager()->checkService(String16("usb")) == nullptr);
    IPCThreadState::self()->stopProcess();
}

TEST(Hals, NoLightsNoHealth) {
    {
        using aidl::android::hardware::light::HwLight;
        using aidl::android::hardware::light::HwLightState;
        using aidl::android::hardware::light::ILights;
        using aidl::android::hardware::light::LightType;
        ndk::SpAIBinder binder(AServiceManager_checkService("android.hardware.light.ILights/default"));
        EXPECT_TRUE(binder.get() == nullptr);
        std::shared_ptr<ILights> hal = ILights::fromBinder(binder);
        EXPECT_FALSE(hal);
        if (hal) {
            std::vector<HwLight> lights;
            hal->getLights(&lights);
            for (const auto& light : lights) {
                if (light.type == LightType::BACKLIGHT) {
                    HwLightState state{};
                    state.color = 0xFF000000 | (128 << 16);
                    hal->setLightState(light.id, state);
                }
            }
        }
    }
    {
        using HidlLight = ::android::hardware::light::V2_0::ILight;
        using HidlType = ::android::hardware::light::V2_0::Type;
        using HidlLightState = ::android::hardware::light::V2_0::LightState;
        using HidlBrightness = ::android::hardware::light::V2_0::Brightness;
        using HidlFlash = ::android::hardware::light::V2_0::Flash;
        android::sp<HidlLight> hal = HidlLight::getService();
        EXPECT_TRUE(hal == nullptr);
        if (hal != nullptr) {
            HidlLightState state{};
            state.color = 0xFF000000;
            state.flashMode = HidlFlash::NONE;
            state.brightnessMode = HidlBrightness::USER;
            hal->setLight(HidlType::BACKLIGHT, state);
        }
    }
    {
        using aidl::android::hardware::health::BatteryStatus;
        using aidl::android::hardware::health::IHealth;
        std::shared_ptr<IHealth> h = IHealth::fromBinder(ndk::SpAIBinder(AServiceManager_checkService("x")));
        EXPECT_FALSE(h);
        BatteryStatus st = BatteryStatus::UNKNOWN;
        if (h) {
            int32_t cap = -1;
            auto s1 = h->getCapacity(&cap);
            if (s1.isOk()) h->getChargeStatus(&st);
        }
        EXPECT_TRUE(st != BatteryStatus::FULL);
    }
    {
        namespace H2 = android::hardware::health::V2_0;
        namespace H1 = android::hardware::health::V1_0;
        android::sp<H2::IHealth> hidl = H2::IHealth::getService("default");
        EXPECT_TRUE(hidl == nullptr);
        bool charging = false;
        if (hidl != nullptr) {
            auto r1 = hidl->getCapacity([&](H2::Result, int32_t) {});
            if (r1.isOk())
                hidl->getChargeStatus([&](H2::Result res, H1::BatteryStatus st) {
                    if (res == H2::Result::SUCCESS) charging = (st == H1::BatteryStatus::CHARGING);
                });
        }
        hidl = nullptr;
        EXPECT_FALSE(charging);
    }
}

}  // namespace android

TEST(Ndk, MediaCreatesNothing) {
    AMediaFormat* fmt = AMediaFormat_new();
    EXPECT_TRUE(fmt == nullptr);
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, 44100);
    AMediaFormat_setBuffer(fmt, AMEDIAFORMAT_KEY_CSD_0, "x", 1);
    int32_t i32 = 7;
    int64_t i64 = 7;
    const char* str = "x";
    EXPECT_FALSE(AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &i32));
    EXPECT_FALSE(AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &i64));
    EXPECT_FALSE(AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_TITLE, &str));
    EXPECT_TRUE(str == nullptr);
    EXPECT_STREQ(AMEDIAFORMAT_KEY_MIME, "mime");
    EXPECT_STREQ(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count");
    EXPECT_EQ(AMediaFormat_delete(fmt), AMEDIA_OK);

    AMediaCodec* c = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    EXPECT_TRUE(c == nullptr);
    EXPECT_TRUE(AMediaCodec_createCodecByName("c2.android.aac.decoder") == nullptr);
    EXPECT_NE(AMediaCodec_configure(c, fmt, nullptr, nullptr, 0), AMEDIA_OK);
    EXPECT_NE(AMediaCodec_start(c), AMEDIA_OK);
    EXPECT_LT(AMediaCodec_dequeueInputBuffer(c, 1000), 0);
    size_t sz = 9;
    EXPECT_TRUE(AMediaCodec_getInputBuffer(c, 0, &sz) == nullptr);
    EXPECT_EQ(sz, 0u);
    AMediaCodecBufferInfo info;
    ssize_t oi = AMediaCodec_dequeueOutputBuffer(c, &info, 1000);
    EXPECT_LT(oi, 0);
    EXPECT_NE(oi, (ssize_t)AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED);
    EXPECT_NE(oi, (ssize_t)AMEDIACODEC_INFO_TRY_AGAIN_LATER);
    EXPECT_TRUE(AMediaCodec_getOutputFormat(c) == nullptr);
    AMediaCodec_stop(c);
    EXPECT_EQ(AMediaCodec_delete(c), AMEDIA_OK);

    AMediaDataSource* ds = AMediaDataSource_new();
    EXPECT_TRUE(ds == nullptr);
    AMediaDataSource_setUserdata(ds, nullptr);
    AMediaDataSource_setReadAt(ds, [](void*, off64_t, void*, size_t) -> ssize_t { return -1; });
    AMediaDataSource_setGetSize(ds, [](void*) -> ssize_t { return -1; });
    AMediaDataSource_setClose(ds, [](void*) {});
    AMediaDataSource_delete(ds);

    AMediaExtractor* ex = AMediaExtractor_new();
    EXPECT_TRUE(ex == nullptr);
    EXPECT_NE(AMediaExtractor_setDataSourceCustom(ex, ds), AMEDIA_OK);
    EXPECT_NE(AMediaExtractor_setDataSourceFd(ex, 0, 0, 1), AMEDIA_OK);
    EXPECT_NE(AMediaExtractor_setDataSource(ex, "http://x"), AMEDIA_OK);
    EXPECT_EQ(AMediaExtractor_getTrackCount(ex), 0u);
    EXPECT_TRUE(AMediaExtractor_getTrackFormat(ex, 0) == nullptr);
    EXPECT_TRUE(AMediaExtractor_getFileFormat(ex) == nullptr);
    EXPECT_FALSE(AMediaExtractor_advance(ex));
    EXPECT_NE(AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC), AMEDIA_OK);
    EXPECT_EQ(AMediaExtractor_delete(ex), AMEDIA_OK);

    AImageReader* reader = reinterpret_cast<AImageReader*>(&sz);
    EXPECT_NE(AImageReader_new(64, 64, AIMAGE_FORMAT_YUV_420_888, 8, &reader), AMEDIA_OK);
    EXPECT_TRUE(reader == nullptr);
    ANativeWindow* window = reinterpret_cast<ANativeWindow*>(&sz);
    EXPECT_NE(AImageReader_getWindow(reader, &window), AMEDIA_OK);
    EXPECT_TRUE(window == nullptr);
    AImage* img = reinterpret_cast<AImage*>(&sz);
    EXPECT_NE(AImageReader_acquireLatestImage(reader, &img), AMEDIA_OK);
    EXPECT_TRUE(img == nullptr);
    uint8_t* data = &reinterpret_cast<uint8_t*>(&sz)[0];
    int len = 5;
    EXPECT_NE(AImage_getPlaneData(img, 0, &data, &len), AMEDIA_OK);
    EXPECT_TRUE(data == nullptr);
    EXPECT_EQ(len, 0);
    AImageCropRect crop = {0, 0, 0, 0};
    EXPECT_NE(AImage_getCropRect(img, &crop), AMEDIA_OK);
    AImage_delete(img);
    AImageReader_delete(reader);
}

TEST(Ndk, AudioSensorsHintsAndLooper) {
    // NanoAudio.cpp: a callback stream.
    AAudioStreamBuilder* b = reinterpret_cast<AAudioStreamBuilder*>(&b);
    EXPECT_NE(AAudio_createStreamBuilder(&b), AAUDIO_OK);
    EXPECT_TRUE(b == nullptr);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, [](AAudioStream*, void*, void*, int32_t) {
        return (aaudio_data_callback_result_t)AAUDIO_CALLBACK_RESULT_CONTINUE;
    }, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, [](AAudioStream*, void*, aaudio_result_t err) {
        (void)(err == AAUDIO_ERROR_DISCONNECTED);
    }, nullptr);
    AAudioStream* s = reinterpret_cast<AAudioStream*>(&s);
    EXPECT_NE(AAudioStreamBuilder_openStream(b, &s), AAUDIO_OK);
    EXPECT_TRUE(s == nullptr);
    EXPECT_NE(AAudioStream_requestStart(s), AAUDIO_OK);
    EXPECT_EQ(AAudioStream_getSampleRate(s), 0);
    AAudioStream_close(s);
    AAudioStreamBuilder_delete(b);

    // NanoMenuPS3Clock.cpp: the accelerometer.
    ASensorManager* mgr = ASensorManager_getInstanceForPackage("com.gammaos.nano");
    if (mgr == nullptr) mgr = ASensorManager_getInstance();
    EXPECT_TRUE(mgr == nullptr);
    const ASensor* accel = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ACCELEROMETER);
    ALooper* looper = ALooper_forThread();
    if (looper == nullptr) looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    EXPECT_TRUE(looper == nullptr);
    ASensorEventQueue* q = ASensorManager_createEventQueue(mgr, looper, 3, nullptr, nullptr);
    EXPECT_TRUE(q == nullptr);
    ASensorEventQueue_enableSensor(q, accel);
    ASensorEventQueue_setEventRate(q, accel, 16666);
    ASensorEvent ev;
    ev.acceleration.x = 0;
    EXPECT_LE(ASensorEventQueue_getEvents(q, &ev, 1), 0);
    ASensorEventQueue_disableSensor(q, accel);

    // NanoMenu.cpp: the performance hint session.
    APerformanceHintManager* hm = APerformanceHint_getManager();
    int32_t tids[1] = {1};
    APerformanceHintSession* hs = APerformanceHint_createSession(hm, tids, 1, 16666666LL);
    EXPECT_TRUE(hs == nullptr);
    EXPECT_NE(APerformanceHint_reportActualWorkDuration(hs, 1000), 0);
}

TEST(Ndk, HardwareBufferIsDeclaredOnly) {
    AHardwareBuffer_Desc d = {};
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    EXPECT_EQ(d.format, 1u);
    // AHardwareBuffer_release/_describe/_lock/_unlock: declared, defined by the DRM buffer back end
    // (not linked here). Taking their types only checks the declarations.
    static_assert(std::is_same<decltype(&AHardwareBuffer_release), void (*)(AHardwareBuffer*)>::value, "");
    static_assert(std::is_same<decltype(&AHardwareBuffer_lock),
                               int (*)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**)>::value, "");
    static_assert(std::is_same<decltype(&AHardwareBuffer_unlock), int (*)(AHardwareBuffer*, int32_t*)>::value, "");
}

TEST(Log, EachStubLogsOnce) {
    EXPECT_TRUE(gCapturing);
    for (int i = 0; i < 3; i++) {
        (void)AMediaCodec_createDecoderByType("video/avc");
        (void)android::SurfaceComposerClient::getPhysicalDisplayIds();
    }
    EXPECT_EQ(logLinesWith("AMediaCodec_createDecoderByType"), 1u);
    EXPECT_EQ(logLinesWith("getPhysicalDisplayIds"), 1u);
    EXPECT_EQ(logLinesWith("IServiceManager::checkService"), 1u);
    EXPECT_EQ(logLinesWith("AServiceManager_checkService"), 1u);
    EXPECT_EQ(logLinesWith("nanostub: "), logLinesWith("inert stub"));
}
