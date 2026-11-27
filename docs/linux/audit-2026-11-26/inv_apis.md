APIS INVENTORY: Android APIs, headers and libraries in Nano's shared code, and how the Linux build copes with them
Repo: C:/Users/krist/code/thimsy-os-linux (branch linux). Read-only. I compiled or built nothing.

## 0. How I counted, and what counts as "shared"
- Partition source: C:/Users/krist/code/thimsy-os-linux/nano/build/sources.txt. Section sizes: [core] 105, [drm] 1, [firmware] 21, [dropped-linux] 10 (a subset of [firmware]), [linux] 19, [host] 16, [scrape] 6, [test-unit] 54.
- "Shared" here means the top-level nano/*.cpp, *.h and *.inc files plus bootscreen/NanoBootArt.*, without the 15 firmware-only top-level files. Those 15 are main.cpp, NanoMenuDrmBuffers.cpp, NanoMenuRgbFollow.cpp, NanoMenuShares.cpp, NanoBootChime.cpp, NanoMenuDrasticPark.cpp, NanoBtStable.cpp, LibretroRunner.cpp, FakeJNI.cpp/.h, DrasticRunner.cpp/.h, DrasticGpu3d.cpp and DrasticSettings.cpp/.h. Third-party files (stb_image.h, libretro.h) are also left out.
- That leaves about 201 files. They cover [core], [drm] (NanoMenuDrm.cpp, which Linux compiles into nano_platform_linux) and [scrape].
- Include counts come from grep for `#include <android/|android-base/|cutils/|utils/|log/|binder/|gui/|ui/|aidl/|android/hardware/|aaudio/|media/|jni.h|sys/system_properties|private/|system/|vndk/|ziparchive/|EGL/eglext|tinyalsa>`, split into firmware-only and shared.
- Call-site counts are `grep -oE` over the same shared file set. They include comment hits, so treat them as approximate.

## 1. Platform preprocessor switches in shared code: ZERO
- I checked every `#if/#ifdef/#ifndef/#elif` in shared files. None tests `__ANDROID__`, `__BIONIC__`, `THIMSY_*`, `NANO_LINUX`, `NANO_HOST`, `NANO_WITH_*`, `NANOCOMPAT*` or `__linux__`.
- The conditionals that do exist are architecture or feature checks only:
  - `__aarch64__`: 4 (NanoMenuPS3Bg.cpp:39, DrasticGpu3d.cpp:67/2282/2306)
  - `GAMMAOS_NANO_HAVE_NEON`: NanoMenuDrm.cpp:56, 1405
  - `__ARM_NEON`: NanoMenuDrm.h:35
  - `#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID` / `EGL_NO_NATIVE_FENCE_FD_ANDROID`: NanoMenuDrm.h:114, 117
  - `__has_include("NanoScraperDevCreds.gen.h")`: NanoScraperDevCreds.h:18-19
  - `NANO_XMB_HIDE_LEGACY` ×23, `__NR_renameat2`/`__NR_close_range`, `GL_FRAGMENT_PRECISION_HIGH`
- This is deliberate and enforced. IMPLEMENTATION.md:1014 says the `__ANDROID__`/`NANO_PLATFORM_*` conditional ratchet has "budget 0", and DECISIONS.md:364 (G0-14) says the same.
- The only `__BIONIC__` test anywhere is in the force-included compat header nano/compat/include/nanocompat/bionic.h, which is not shared code.
- Conclusion: the shared code has no `#ifdef` platform specifics. It does keep Android API calls, unconditionally. Linux makes them compile and behave by emulating or stubbing Android underneath, not by keeping them out.

## 2. Android `#include` lines in shared code, by family
In total, shared files have 292 Android-header include lines in 88 files; the firmware-only files have 48 more.

| Family | Shared include lines / files | Firmware-only lines / files | What Linux provides |
|---|---|---|---|
| cutils/properties.h | 53 / 53 | 9 / 9 | SHIM, config store |
| utils/Log.h | 34 / 34 | 9 / 9 | SHIM, log |
| log/log.h | 32 / 32 | 2 / 2 | SHIM, log |
| utils/SystemClock.h, Timers.h | 31 / 29 | 5 / 4 | SHIM, clock_gettime |
| gui/* and android/gui/* (SurfaceFlinger client) | 24 / 7 | 0 | STUB, inert |
| ui/* (SF value types, GraphicBuffer, Fence, GraphicBufferMapper) | 23 / 6 | 2 / 1 | STUB |
| media/Ndk* (MediaCodec, Extractor, Format, DataSource, Image, ImageReader) | 19 / 8 | 0 | STUB, every create returns null |
| android-base/properties.h | 12 / 12 | 0 | SHIM |
| aidl/android/hardware/* and android/hardware/*/2.0 (ILights, IHealth) | 12 / 2 (NanoMenu.cpp, NanoMenuSystem.cpp) | 0 | STUB, "no service" |
| sys/system_properties.h | 9 / 9 | 1 / 1 | SHIM, store serials and wait |
| android/log.h | 9 / 9 | 1 / 1 | SHIM |
| binder/* and android/binder_* | 7 / 5 | 3 / 1 (ProcessState, main.cpp) | STUB; ProcessState NOT_STUBBED |
| android/imagedecoder.h, bitmap.h, rect.h | 5 / 2 (NanoThemeEngine.cpp, NanoMenuPhotos.cpp) | 0 | SHIM, a REAL decoder (I30) |
| AHardwareBuffer (android/hardware_buffer.h, vndk/) | 3 / 2 (NanoMenuDrm.h, NanoMenuDrm.cpp) | 4 / 3 | STUB, allocation fails |
| utils/RefBase.h, StrongPointer.h, Thread.h | 3 / 3 | 0 | SHIM |
| ziparchive/ | 3 / 2 (NanoArchive.cpp, NanoOtaCheck.cpp) | 0 | none: both files compiled out (ARCHIVE OFF) |
| android-base/file.h, unique_fd.h | 2 / 2 | 0 | SHIM, POSIX |
| android/sensor.h, looper.h | 2 / 1 (NanoMenu.h) | 0 | STUB |
| android/performance_hint.h, native_window.h | 2 / 1 (NanoMenu.cpp) | 0 | STUB |
| utils/String8.h, String16.h | 2 / 2 | 0 | STUB |
| private/android_filesystem_config.h | 2 / 2 (NanoBiosUtil.cpp, NanoMenuSyncthing.cpp) | 0 | SHIM, AID constants |
| aaudio/AAudio.h | 1 / 1 (NanoMenuPS3Menu.cpp) | 1 / 1 | STUB, open fails |
| cutils/native_handle.h | 1 / 1 (NanoMenuDrm.cpp) | 1 / 1 | STUB |
| system/thread_defs.h | 1 / 1 (NanoMenuRender.cpp) | 0 | SHIM |
| jni.h, FakeJNI.h | 0 | 6 / 4 | NOT_STUBBED; firmware only |
| tinyalsa | 0 | 1 / 1 (NanoBootChime.cpp) | firmware only |

- On top of the table, EGL/eglext.h appears in 5 shared files. It is Khronos, not Android, but it carries Android extensions (see section 4).
- The table counts direct includes only. NanoMenu.h includes binder/IBinder.h, utils/Thread.h, android/sensor.h, android/looper.h and sys/system_properties.h, and 53 files include NanoMenu.h.
- The census (C:/Users/krist/code/thimsy-os-linux/tools/nano-tree/include-census.json, "sources" section, counting "stub" entries) shows how far stubs reach transitively. 66 Android.bp sources reach at least one layer-2 stub header. 57 of them are compiled on Linux (56 [core] plus NanoMenuDrm.cpp); the other 9 are firmware.
- The top stub-reachers among Linux-compiled files: NanoMenu.cpp 23, NanoMenuPS3Clock.cpp 17, NanoMenuRender.cpp 15, NanoMenuPS3Menu.cpp 13, NanoMenuVideo.cpp 12, NanoVideo.cpp 11, NanoMenuSystem.cpp 11, NanoMenuMedia.cpp 11, NanoMenuEsdeMenu.cpp 11, NanoMenuOverlay.cpp 10, NanoTsDemux.cpp 8.
- Most of the files at 3 reach stubs only through NanoMenu.h.

Shared files that include a non-logging, non-property, non-clock Android header (29):
- NanoMenu.cpp, NanoMenu.h, NanoMenuRender.cpp, NanoMenuPS3Clock.cpp, NanoMenuOverlay.cpp, NanoMenuSystem.cpp
- NanoMenuDrm.cpp, NanoMenuDrm.h, NanoDisplayTargetSf.h
- NanoVideo.cpp, NanoVideo.h, NanoMenuVideo.cpp, NanoAudio.cpp, NanoAacDec.cpp, NanoIcyDemux.cpp, NanoAviDemux.cpp, NanoHls.h, NanoTsDescramble.h
- NanoMenuPS3Menu.cpp, NanoMenuPS3Boot.cpp, NanoMenuMtp.cpp
- NanoThemeEngine.cpp, NanoMenuPhotos.cpp
- NanoArchive.cpp, NanoOtaCheck.cpp
- NanoMenuSyncthing.cpp, NanoBiosUtil.cpp, NanoDeviceKeys.cpp, NanoBootSequence.h

## 3. Android API call sites in shared code
Counted with `grep -oE` over the ~201 shared files.

Properties:
- `property_get*(`: 649 in 50 files
- `property_set(`: 491 in 34 files
- `base::Get*Property`: 83 in 10 files
- `SetProperty`: 29 in 4 (NanoMenuXmb.cpp 14, NanoMenuUtils.h 9, NanoMenuInput.cpp 5, NanoMenuOverlay.cpp 1)
- `__system_property_*`: 58 in 7 (NanoMenu.cpp 22, NanoMenuInput.cpp 8, NanoMenuPS3Menu.cpp 7, NanoMenuPS3Clock.cpp 6, NanoMenuRender.cpp 6, NanoThemeEngine.cpp 6, NanoMenuOverlay.cpp 3)
- Literal property keys: tools/nano-tree/props.txt lists 994 (my grep count; the file header says 998).
- tools/nano-tree/control-keys.json gives triggers these dispositions: routed 12, store-only 18, refused 15, not-reached 21.
- Routed rows by route kind: session 4, session-key 2, hook 2, launcher 2, first-frame 1, rc-reset 1.

Other APIs:
- Logging (ALOG*/SLOG*/LOG_ALWAYS_FATAL/__android_log_*): 811 in 69 files
- Clocks (uptimeMillis, elapsedRealtime*, systemTime): 352 in 29 files
- NDK media (`AMedia(Codec|Extractor|Format|DataSource)_*`): 366 in 11 files. NanoVideo.cpp 124, NanoAudio.cpp 101, NanoMenuVideo.cpp 38, NanoIcyDemux.cpp 32, NanoAacDec.cpp 25, NanoAviDemux.cpp 20, NanoHls.cpp 6, NanoTsDescramble.cpp 6, NanoVideo.h 4, NanoHls.h 1, NanoMenu.h 1.
- AAudio: 17 in 3 (NanoMenuPS3Menu.cpp 14, e.g. :10305 `AAudio_createStreamBuilder`; NanoMenu.h; NanoBootChime.h)
- AImageDecoder/AndroidBitmap: 94 in 2 (NanoThemeEngine.cpp 43, NanoMenuPhotos.cpp 43)
- AHardwareBuffer: 24 in 5 shared files (NanoMenuDrm.cpp 15, NanoMenuDrmBuffers.h 4, NanoMenuDrm.h 2, NanoMenu.cpp 1, NanoMenuRender.cpp 1)
- SurfaceComposerClient/SurfaceControl/Transaction: 188 in 9 (NanoMenu.cpp 38, NanoMenuOverlay.cpp 42, NanoMenuRender.cpp 33, NanoMenuPS3Clock.cpp 23, NanoMenu.h 9, NanoDisplayTargetSf.h 5, NanoBootSequence.cpp, NanoControlCenter.cpp, NanoMenuPS3Menu.cpp 1 each)
- Binder service lookups: 15 in 3 (NanoMenu.cpp 11, NanoMenuMtp.cpp 1, NanoMenuPS3Boot.cpp 1)
- ASensor/ALooper: 20 in 3 (NanoMenuPS3Clock.cpp:350-351 `ASensorManager_getInstanceForPackage`, NanoMenu.h, NanoVideo.h)
- APerformanceHint: 6 in 2 (NanoMenu.cpp:1212-1226)
- ILights/IHealth: 19 in 2 (NanoMenu.cpp 12, NanoMenuSystem.cpp 5)
- `AID_*`: 5 in 2
- sp<>/wp<>/RefBase: 63 in 10
- android::Thread/threadLoop/readyToRun: 65 in 14
- String8/String16: 12 in 6
- ziparchive: 16 in 3
- Platform-layer use, for comparison: `nanoplat::get()` appears 63 times in 15 shared files.

## 4. Places where shared code still attempts an Android mechanism on Linux
On Linux each of these lands on a stub, an emulation or a link-time wrap instead of being absent.
- **SurfaceFlinger wait loops (would hang if reached).**
  - NanoMenu.cpp:231-240 (`initSurfaceFlingerPath`) runs `while (sm->checkService("SurfaceFlinger")==nullptr) usleep(10000);`. NanoMenu.cpp:1060-1066 polls the same way in overlay or `nanoForceSfPath()` mode.
  - The stub's own header says this "never ends on Linux; neither path is taken there" (nano/compat/stubs/include/binder/IServiceManager.h:17-23).
  - So safety depends on runtime flags (`mDrmBootPath`, `sDrmActive`, `mOverlayMode`), not on the platform layer.
- **Lights HAL in shared code.** NanoMenu.cpp:940-1000 starts a thread that calls `AServiceManager_checkService("android.hardware.light.ILights/default")` and the AIDL ILights directly. That duplicates `Power::setBrightnessViaHal`, which NanoMenuSystem.cpp:115 already routes through `nanoplat`. On Linux the stub returns null, so it is a silent no-op.
- **Vestigial HAL includes.** NanoMenuSystem.cpp:35-41 still includes the IHealth/ILights AIDL and HIDL headers, although its calls now go through `nanoplat::get().power` (:115, :148, :152, :496, :825).
- **Boot sound on Linux.** NanoMenuPS3Boot.cpp:201 (`kBootSoundWaitMs` = 60000) and :215-223. The thread polls `defaultServiceManager()->checkService("audio")` (stub: always null) and retries `gBootSfx.loadMedia`, an AMediaExtractor stub, every 250 ms until the 60 s deadline. About 240 attempts against stubs.
- **MTP.** NanoMenuMtp.cpp:379 gates on `checkService("usb")`, a binder stub.
- **Performance hint.** NanoMenu.cpp:1212 calls `APerformanceHint_getManager()` (stub null), gated only by `persist.gammaos.nano.perf.adpf`.
- **Process shutdown.** NanoMenu.cpp:6291 calls `IPCThreadState::self()->stopProcess()` (stub no-op).
- **Sensors and audio.** NanoMenuPS3Clock.cpp:350 (sensor stub null) and NanoMenuPS3Menu.cpp:10305 (AAudio open fails; the EQ preview stays silent).
- **Media.** All NDK media users decode through stubs that return null (nano/compat/stubs/src/ndk_media.cpp). Music, video, IPTV, radio, AAC, ICY, AVI, HLS and TS therefore fail at run time on Linux. Audio output already has a platform interface (`nanoplat` Audio, with ALSA on Linux), but decoding does not.
- **Android uid/gid ownership on Linux.**
  - NanoBiosUtil.cpp:179 runs `fchown(fd, ..., AID_EXT_DATA_RW)`; NanoMenuSyncthing.cpp:466 runs `chown(cur, AID_MEDIA_RW, AID_MEDIA_RW)`.
  - The shim supplies Android's numbers, per private/android_filesystem_config.h's header comment: "On Linux everything runs as root, so a chown to one of these ids is harmless; nothing maps them to Linux users."
  - So Linux files get chowned to the Android uids (1023, and AID_EXT_DATA_RW's value).
- **Android EGL extensions, faked on Linux.**
  - NanoMenuDrm.cpp:1118 calls `eglGetProcAddress("eglGetNativeClientBufferANDROID")`. NanoMenuDrm.cpp:1142-1143 requires `EGL_ANDROID_image_native_buffer` and `EGL_ANDROID_get_native_client_buffer` before it enables the zero-copy ring.
  - Other shared Android EGL names: NanoMenu.cpp (EGL_ANDROID_image_native_buffer, EGL_SYNC_NATIVE_FENCE_ANDROID), NanoMenuRender.cpp and NanoMenuDrm.h (EGL_SYNC_NATIVE_FENCE_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID), NanoMenuDrm.cpp (`eglDupNativeFenceFDANDROID`), NanoMenuPS3Clock.cpp (EGL_NATIVE_BUFFER_ANDROID).
  - On Linux, nano/platform/linux/display/EglWraps.cpp wraps `eglGetProcAddress` and `eglQueryString` at link time (linker-wraps.txt lines `eglGetProcAddress`, `eglQueryString`).
  - The wrap returns a stand-in for `eglGetNativeClientBufferANDROID` and appends the two Android extension names to Mesa's extension string when the display has `EGL_EXT_image_dma_buf_import` and `EGL_KHR_image_base`.
  - This is the clearest case of Linux impersonating Android so that unchanged shared code takes an Android path. The real Linux behaviour lives in platform/linux/display/DrmBuffersGbm.cpp, which replaces NanoMenuDrmBuffers.cpp.
- **Launch protocol.**
  - The `nanoplat::Launcher::commit(const android::LaunchTuple&)` interface (NanoPlatform.h:119) carries Android property-handshake keys (NanoLaunchCommit.h: `sys.gammaos.nano.launch_app` … `do_launch`, `service.bootanim.nano_retroarch`).
  - NanoPlatform.h:84-89 says Linux's commit "writes the same keys, and the handshake's trigger starts the game … through the control keys".
  - Other Android-shaped interface members: `Display::startScanout(..., bool (*forceSfPath)())` at :66, and `Power::setBrightnessViaHal` / `savedBrightness` ("the firmware runs `settings get`") at :215 and :224.

## 5. What Linux emulates or stubs so these files compile

### Layer 1: nano/compat/ (target nano_compat, compat/src/*.cpp)
It is first on the include path, never on Android's, and force-includes nanocompat/bionic.h into every translation unit (CMakeLists.txt:243-249).

Harmless portability shims (a pure library replacement with no OS-behaviour meaning):
- log/log.h, utils/Log.h, android/log.h → src/log.cpp (292 lines). One line per message to stderr, plus a ring file /run/thimsy/log/nano.log (256 KiB). `setLogFile` is called at main_linux.cpp:81.
- utils/SystemClock.h, utils/Timers.h, utils/Errors.h → src/utils.cpp (111 lines), on clock_gettime.
- utils/RefBase.h, utils/StrongPointer.h → src/RefBase.cpp (234 lines).
- utils/Thread.h, utils/ThreadDefs.h, system/thread_defs.h → src/Thread.cpp (205 lines), on pthread and setpriority. `PRIORITY_DISPLAY` becomes nice -4; IMPLEMENTATION.md:184 marks that value as a guess.
- android-base/file.h, unique_fd.h, parsebool.h, parseint.h, strings.h → src/file.cpp (227 lines), plain POSIX.
- nanocompat/bionic.h (127 lines): Bionic `mallopt` M_* constants as values glibc ignores, plus `strlcpy`/`strlcat` (glibc older than 2.38), `gettid` (older than 2.30) and `getprogname`. tools/nano-tree/bionic-identifiers.txt is its ratchet; the shared files have 9 `mallopt`/`strlcpy`/`getprogname` hits.

Behaviour emulation (Android semantics reproduced on Linux):
- **Properties.** cutils/properties.h, android-base/properties.h, sys/system_properties.h → src/properties.cpp (536 lines), on libthimsy's `thimsy::ConfigStore`.
  - Android quirks are kept: the 92-byte value limit, and serials that bump on a same-value set "as on Android".
  - Property observers (nanocompat/nanocompat.h `addPropertyObserver`) turn writes into actions. They are registered by platform/linux/ControlKeys.cpp:301 (288 lines; table `kControlRows`) and on the host by HostPlatform.cpp:136.
  - This makes the property API both a settings store and the transport for Android framework messages on Linux.
- **Image decoding.** android/imagedecoder.h, android/bitmap.h, android/rect.h → src/imagedecoder.cpp (1291 lines), a real AImageDecoder on libpng, libjpeg, libwebp and giflib (decision I30). HEIF and AVIF are unsupported. Per-format `NANOCOMPAT_NO_*` switches are set by CMakeLists.txt:254-304.
- **Android uids.** private/android_filesystem_config.h provides Android's numeric uids and gids, and they are applied to Linux files (see section 4).

### Layer 2: nano/compat/stubs/ (target nano_stubs; defines EGL_NO_PLATFORM_SPECIFIC_TYPES, CMakeLists.txt:332-339)
- These are inert stubs. Every create, open or lookup fails, and other calls are no-ops that return failure or zero. Each function logs once through `NANOSTUB_ONCE` (stubs/include/nanostubs/stub.h).
- The budget is 446 declared names (tools/nano-tree/stubs-budget.txt). It may only shrink.
- Headers (68):
  - aaudio/AAudio.h
  - AIDL health (BatteryStatus, IHealth) and AIDL light (HwLight, HwLightState, ILights, LightType)
  - android/binder_auto_utils.h, binder_ibinder.h, binder_manager.h
  - android/gui/BnScreenCaptureListener.h
  - HIDL health 1.0/2.0 and HIDL light 2.0
  - android/hardware_buffer.h, looper.h, native_window.h, performance_hint.h, sensor.h
  - binder/IBinder.h, IPCThreadState.h, IServiceManager.h, Status.h
  - cutils/native_handle.h
  - 17 gui/* headers (SurfaceComposerClient, SurfaceControl, Surface, BufferQueue, BufferItemConsumer, GLConsumer, DisplayCaptureArgs, SyncScreenCaptureListener, LayerState, ScreenCaptureResults, …)
  - hidl/Status.h
  - 7 media/Ndk* headers
  - system/window.h
  - 14 ui/* headers
  - utils/String8.h, String16.h
  - vndk/hardware_buffer.h
- Definitions:
  - src/ndk_media.cpp (167 lines): `AMediaFormat_new` returns null, …
  - src/ndk_system.cpp (82 lines): AAudio returns AAUDIO_ERROR_UNAVAILABLE; sensors, looper and perf hint return null; `AServiceManager_checkService` returns null.
  - src/stub_log.cpp (30 lines).
  - Some stubs are inline in headers, e.g. `IServiceManager::checkService` returns nullptr.
- Not stubbed (tools/nano-tree/include_census.py:120-124 `NOT_STUBBED`): `jni.h` and `binder/ProcessState.h`. Their users are firmware-only.

### Layer 3: link-time substitution
- **Shell gate.** `--wrap=system,popen,pclose,execv,execvp,execve` for thimsy-nano, thimsy-nano-scrape and nano-headless (CMakeLists.txt:198-219, 452, 489, 510; definitions in platform/linux/ShellGate.cpp, 715 lines). It translates, refuses (exit 127) or passes through Android commands, per IMPLEMENTATION.md:255 and decision I15. The headless build also wraps `ioctl`.
- **EGL gate.** `--wrap=eglGetProcAddress,eglQueryString` via platform/linux/display/EglWraps.cpp (see section 4).
- **Generated stubs for dropped files.** They come from `nm` of the final link (cmake/stubgen.cmake, test/host/stubgen/gen.py) and cover undefined symbols defined by [dropped-linux] files and feature-off files. Each stub returns T(), or nothing, and logs once (decision I29).
  - Examples: DrasticRunner (117+ methods named from [core]), LibretroRunner (25+), NanoMenu::Shares methods (13+), NanoBootChime `nanoDirect*` (8+), `drasticPark*` (3), `nanoStartBtStability`, `nanoRgbFollowSample`, `drmAllocAhbTarget`, `gEarlyDrmFd`.
  - So [core] code calls into DS-player and libretro functions on Linux and gets generated no-ops.
- **Path mapping.** On the image, Android paths (/data, /sdcard, /system/etc, /system/fonts, /system/bin) are symlinks into Linux locations (IMPLEMENTATION.md:256).

## 6. Files excluded from the Linux build, and features compiled out
- **Excluded entirely ([firmware], 21 files).**
  - main.cpp
  - platform/firmware/{Platform, Display, Launcher, Audio, Volumes, Power, SystemActions, InputPolicy, Network}.cpp
  - NanoMenuDrmBuffers.cpp: the AHB back end; Linux uses DrmBuffersGbm.cpp.
  - NanoMenuRgbFollow.cpp, NanoMenuShares.cpp: they need sibling headers.
  - NanoBootChime.cpp: tinyalsa.
  - NanoMenuDrasticPark.cpp, NanoBtStable.cpp, LibretroRunner.cpp ("until Phase 5")
  - FakeJNI.cpp, DrasticRunner.cpp, DrasticGpu3d.cpp, DrasticSettings.cpp
- 10 of those are [dropped-linux] and get generated stubs: main, DrmBuffers, RgbFollow, Shares, BootChime, DrasticPark, BtStable, LibretroRunner, DrasticRunner, DrasticGpu3d.
- **Feature switches (nano/cmake/features.cmake, CMakeLists.txt:85-88 and 106-109; all OFF by default).** Files left out are still in [core] for Android; Linux links generated stubs in their place.
  - `NANO_WITH_AC3` → NanoAc3.cpp (liba52, GPL)
  - `NANO_WITH_PDF` → NanoMenuPdf.cpp (pdfium)
  - `NANO_WITH_ARCHIVE` → NanoArchive.cpp and NanoOtaCheck.cpp (libziparchive and the LZMA SDK; OTA is Android's update flow)
  - `NANO_WITH_MEDIA` → drops no file, and ON is a FATAL_ERROR. The media, music, video and radio files compile and link against the layer-2 media stubs and fail at run time.
- **Display paths.** [drm] NanoMenuDrm.cpp is compiled on Linux. The host build replaces it with platform/host/DrmHost.cpp.
- **Interface purity, documented exceptions.** Decision I24 (IMPLEMENTATION.md:1051) says files in drastic-nano, gammaos-nano-scrape and the host tests may not use `nanoplat::`. They reach platform behaviour only through layers 1-3: shims, stubs and the shell gate. The scraper binary (the [scrape] files) links nano_compat and nano_shellgate but not nano_core (CMakeLists.txt:495-511).

## 7. Measured against the owner's principle
- **What already matches.** Shared code has zero `#ifdef` platform switches. Clocks, logging, RefBase/Thread, libbase file helpers and Bionic libc extras are pure portability shims.
- **What does not match.** Most of the Android surface is kept alive in shared code and made inert or reinterpreted below it, not moved behind `nanoplat`:
  - SurfaceFlinger, binder, HALs, sensors, perf hint and AAudio: 188 SF references in 9 files; 57 Linux-compiled translation units reach stubs.
  - NDK media: 366 references in 11 files, all failing at run time.
  - Android EGL extensions, faked by EglWraps.cpp.
  - Android properties as a message bus: ControlKeys observers, and the 12 routed rows.
  - Android shell commands, translated by ShellGate.
  - Android uids, applied by chown.
  - Generated no-op stubs for firmware-only functions that [core] calls.
- **What the design says about it.** IMPLEMENTATION.md Principle 2 (:54) and B.1 (:164-171) choose this on purpose to keep `nano-144` merges one-sided. Layer 4 (`NanoPlatform.h`) is meant to absorb these "area by area", and the stub budget is meant to shrink as it does.