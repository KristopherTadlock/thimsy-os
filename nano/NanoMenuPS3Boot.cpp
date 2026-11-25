/*
 * GammaOS Nano - home sound effects and early-boot audio.
 *
 * The per-theme navigation sound effects (XMB / DSi / Minima / ES-DE), the DSi
 * home ambiance, and the direct-ALSA early-audio hold + hand-off that keeps
 * those sounds audible before the audio server is up.
 */

#include "NanoMenu.h"
#include "NanoBootChime.h"     // direct-ALSA early-audio engine (bypasses AudioFlinger)

#include <math.h>
#include <unistd.h>       // access() for the audio path fallback
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include <GLES2/gl2.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

namespace android {

// Resolve a menu-audio wav (dev override first, then the bundled install path).
static std::string dsiAudioPath(const char* file) {
    char p[256];
    snprintf(p, sizeof(p), "/data/system/nano_xmb/audio/%s", file);
    if (access(p, R_OK) == 0) return std::string(p);
    snprintf(p, sizeof(p), "/system/etc/nano_xmb/audio/%s", file);
    return std::string(p);
}

// True once the framework has finished booting; after this the audio server is up and the normal
// AAudio path is used, so the direct-PCM path releases the card.
static bool dsiBootCompleted() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.boot_completed", v, "");
    return v[0] == '1';
}

// Final early-audio gain = per-voice web master * the user's saved system music volume, mapped through
// the SAME curve audioserver applies to STREAM_MUSIC, so the direct-PCM loudness equals what the user
// hears from media at that setting (the old linear map made the early audio disproportionately loud).
// Curve = AOSP default speaker MUSIC table (1,-58)(20,-40)(60,-17)(100,0) dB; DbToAmpl = 10^(dB/20).
// persist.gammaos.nano.volume is the route-resolved index (PhoneWindowManager writes it, persist props
// load in first-stage init). index 0 -> mute (skip); no saved volume (first boot) -> full master.
static float dsiEarlyAudioGain(float master) {
    char vmax[PROPERTY_VALUE_MAX] = {}, vcur[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.volmax", vmax, "");
    property_get("persist.gammaos.nano.volume", vcur, "");
    int mx = vmax[0] ? atoi(vmax) : 0;
    int cv = vcur[0] ? atoi(vcur) : -1;
    if (cv == 0) return 0.0f;                          // muted -> silent
    if (mx <= 0 || cv < 0) return master;              // first boot, no saved volume -> full
    if (cv > mx) cv = mx;
    int volIdx = (100 * cv) / mx;                      // rescale to the curve's 0..100 domain
    float db;
    if      (volIdx <= 1)  db = -58.0f;
    else if (volIdx <= 20) db = -58.0f + (volIdx - 1)  * (18.0f / 19.0f);
    else if (volIdx <= 60) db = -40.0f + (volIdx - 20) * (23.0f / 40.0f);
    else                   db = -17.0f + (volIdx - 60) * (17.0f / 40.0f);
    return master * expf(db * 0.115129f);              // 10^(dB/20)
}

// ---- DSi interactive SFX (nav_blip / app_launch / settings_nav / settings_back / settings_enter) ----
// The web app (audio.js) plays a distinct sound for each menu event; nano was silent in DS mode (the
// PS3 cursor sound ps3NavSound is gated to the PS3 theme). Five FILE-STATIC low-latency retriggerable
// players (one per clip) load their wav once on a bg thread and then trigger lock-free from the render
// thread. File-static (not NanoMenu members) so the class layout is unchanged (no full recompile).
// NdsSfxId now lives at namespace scope in NanoMenu.h so other TUs can name the ids too.
static NanoSfxPlayer  gNdsSfx[NDS_SFX_COUNT];
static std::atomic<bool> gNdsSfxOpening[NDS_SFX_COUNT] = {};   // per-clip open-in-flight guard

// Trigger one DSi interactive SFX (NDS_SFX_* id). Lazy-loads the clip once on a bg thread (AAudio /
// decode never on the render thread), then triggers are lock-free. Only in the DSi theme (the PS3
// theme uses ps3NavSound). Post-boot only in practice (nav happens after the audio server is up).
// Shared early-audio hook for the interactive menu effects (DSi ndsSfxPlay + PS3 ps3NavSound). Before
// boot-complete the RK3568 audio server (AudioPolicyManager) is still ~23s into its init, so AAudio is
// blocked; play the effect on the direct-ALSA one-shot path instead, where the worker mixes it over any
// early BGM bed. Lives here because dsiBootCompleted / dsiEarlyAudioGain / dsiAudioPath are file-local.
// Returns true if it was queued to the direct mixer (caller done); false -> the caller uses AAudio.
bool NanoMenu::earlySfxOneShot(const char* wavName, float master) {
    if (!(nanoDirectAudioUsable() && !dsiBootCompleted())) return false;
    nanoDirectPlayOneShot(dsiAudioPath(wavName), dsiEarlyAudioGain(master));
    return true;
}

// User setting (persist.gammaos.nano.nav_sounds, default on): when off, silence the interactive
// navigation / UI sound effects (cursor, ok, back, drill, launch, error) across ALL themes. Read
// live so the toggle applies immediately. Gating the three theme SFX dispatchers covers every nav/UI call site.
static bool navSoundsOn() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.nav_sounds", v, "1");
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F');
}

// Decode a DSi clip ahead of time (the render loop's idle tick calls this for the launch
// clip) so the first launch does not stall a frame on the decode + player open.
void NanoMenu::ndsSfxPreload(int which) {
    if (!mNdsTheme || which < 0 || which >= NDS_SFX_COUNT) return;
    if (gNdsSfx[which].loaded()) return;
    if (gNdsSfxOpening[which].exchange(true)) return;
    static const char* kFiles[NDS_SFX_COUNT] = {
        "nav_blip.wav", "app_launch.wav", "settings_nav.wav", "settings_back.wav", "settings_enter.wav" };
    std::string path = dsiAudioPath(kFiles[which]);
    std::thread([which, path]() {
        nanoThreadNormalPriority();
        (void)gNdsSfx[which].load(path, 0.4f);
        gNdsSfxOpening[which].store(false);
    }).detach();
}

void NanoMenu::ndsSfxPlay(int which) {
    if (!mNdsTheme) return;
    if (!navSoundsOn()) return;
    if (which < 0 || which >= NDS_SFX_COUNT) return;
    static const char* kFiles[NDS_SFX_COUNT] = {
        "nav_blip.wav", "app_launch.wav", "settings_nav.wav", "settings_back.wav", "settings_enter.wav" };
    // Pre-boot-complete (RK3568 audio server still initialising): play on the direct-ALSA mixer so the
    // nav / enter / back / launch effects are audible in the early menu instead of silent. Post-boot
    // uses the pre-loaded low-latency AAudio player.
    if (earlySfxOneShot(kFiles[which], 0.4f)) return;
    if (gNdsSfx[which].loaded()) { gNdsSfx[which].trigger(); return; }
    if (gNdsSfxOpening[which].exchange(true)) return;             // one decode in flight per clip
    std::string path = dsiAudioPath(kFiles[which]);
    std::thread([which, path]() {
        // master 0.4 = half the web level (user: the DSi SFX were too loud, drop to 50%). The BGM
        // ambiance is unchanged.
        if (gNdsSfx[which].load(path, 0.4f)) gNdsSfx[which].trigger();
        gNdsSfxOpening[which].store(false);
    }).detach();
}

// ---- Authentic PS3 XMB nav effects (firmware system_plugin, /work/ps3/firmware_audio/system) --------
// SE02 cursor move / SE03 normal OK / SE04 back / SE05 category OK / SE08 option / SE09 error. Mirrors
// the DSi gNdsSfx model: FILE-STATIC low-latency players, one per clip, decoded once on a bg thread then
// triggered lock-free. Pre-boot-complete they route to the direct-ALSA mixer (earlySfxOneShot) so PS3
// XMB nav is audible in the early menu too; post-boot they use the pre-loaded AAudio players.
// Ps3SfxId (PS3_SFX_*) lives at namespace scope in NanoMenu.h so both this file-static array and the
// nav call sites in NanoMenuPS3Menu.cpp can name it.
static NanoSfxPlayer     gPs3Sfx[PS3_SFX_COUNT];
static std::atomic<bool> gPs3SfxOpening[PS3_SFX_COUNT] = {};

void NanoMenu::ps3Sfx(int which) {
    if (mNdsTheme || mMinimaTheme) return;              // PS3 XMB theme only (DSi=ndsSfxPlay, Minima=minimaSfx)
    if (!navSoundsOn()) return;
    if (which < 0 || which >= PS3_SFX_COUNT) return;
    static const char* kFiles[PS3_SFX_COUNT] = {
        "SE02_Cursor.wav", "SE03_Normal_OK.wav", "SE04_Back.wav",
        "SE05_Category_OK.wav", "SE08_Option.wav", "SE09_Error.wav" };
    if (earlySfxOneShot(kFiles[which], 0.8f)) return;   // pre-boot-complete: direct mixer
    if (gPs3Sfx[which].loaded()) { gPs3Sfx[which].trigger(); return; }
    if (gPs3SfxOpening[which].exchange(true)) return;    // one decode in flight per clip
    std::string path = dsiAudioPath(kFiles[which]);
    std::thread([which, path]() {
        if (gPs3Sfx[which].load(path, 0.8f)) gPs3Sfx[which].trigger();
        gPs3SfxOpening[which].store(false);
    }).detach();
}

// ---- Minima interactive SFX (soft/premium nav/select/back/drill/error blips) --------------------
// Same file-static low-latency model as gNdsSfx / gPs3Sfx. Only in the Minima theme. The clips are
// composed for Minima (NextUI itself is silent) and installed alongside the other sound sets in the
// nano_xmb audio dir; they are triggered per-frame by state-diff from minimaSfxTick (NanoMenuMinima.cpp).
static NanoSfxPlayer     gMinSfx[MIN_SFX_COUNT];
static std::atomic<bool> gMinSfxOpening[MIN_SFX_COUNT] = {};

void NanoMenu::minimaSfx(int which) {
    if (!mMinimaTheme) return;                          // Minima theme only
    if (!navSoundsOn()) return;
    if (which < 0 || which >= MIN_SFX_COUNT) return;
    static const char* kFiles[MIN_SFX_COUNT] = {
        "minima_cursor.wav", "minima_ok.wav", "minima_back.wav",
        "minima_drill.wav", "minima_error.wav" };
    if (earlySfxOneShot(kFiles[which], 0.6f)) return;   // pre-boot-complete: direct mixer
    if (gMinSfx[which].loaded()) { gMinSfx[which].trigger(); return; }
    if (gMinSfxOpening[which].exchange(true)) return;   // one decode in flight per clip
    std::string path = dsiAudioPath(kFiles[which]);
    std::thread([which, path]() {
        if (gMinSfx[which].load(path, 0.6f)) gMinSfx[which].trigger();
        gMinSfxOpening[which].store(false);
    }).detach();
}

// ---- ES-DE theme navigation SFX -----------------------------------------------------------------
// The active ES-DE theme's own <sound name=...> wavs (systembrowse/quicksysselect/select/back/
// scroll/favorite/launch), parsed into the system view as type "sound" with an absolute path.
// Same low-latency file-static player model as gPs3Sfx: one player per slot, reloaded when the
// theme changes the path. No-op when the theme omits a given sound (path empty).
static NanoSfxPlayer     gEsdeSfx[7];

// The ES-DE home's startup sound (mods/57-boot-sound.sh installs it): its first two seconds as the
// system view appears after a cold boot. The audio service answers only some seconds after the home
// can draw, so the boot screen stays up until the stream is open, then the view and the sound come
// together (NanoBootScreen.cpp). Played after the boot screen went without it, it is dropped.
static NanoSfxPlayer     gBootSfx;
static std::atomic<int>  gBootSfxState{2};   // 0 getting ready, 1 ready to play, 2 none
static constexpr int     kBootSoundWaitMs = 60000;

void NanoMenu::bootSoundStart() {
    // Its own setting (persist.gammaos.nano.boot_sound, default on), apart from the navigation sounds.
    if (!mEsdeTheme || !property_get_bool("persist.gammaos.nano.boot_sound", true)) return;
    std::string path = "/data/system/nano_xmb/audio/esde_boot.mp3";   // a development override first
    if (access(path.c_str(), R_OK) != 0) path = "/system/etc/nano_xmb/audio/esde_boot.mp3";
    if (access(path.c_str(), R_OK) != 0) return;
    gBootSfxState.store(0);
    std::thread([path]() {
        // Decoded as soon as the media services can (they start well before Android's
        // AudioService), then waits for AudioService ("audio"): it sets the stream volumes, which
        // are muted until then. A minute without it means it is broken; a decode that still fails
        // once AudioService is up never will.
        const int64_t deadline = uptimeMillis() + kBootSoundWaitMs;
        auto waiting = [&]() { return uptimeMillis() < deadline && gBootSfxState.load() == 0; };
        auto audioUp = []() { return defaultServiceManager()->checkService(String16("audio")) != nullptr; };
        bool loaded = false;
        while (!loaded && waiting()) {
            const bool lastTry = audioUp();
            loaded = gBootSfx.loadMedia(path, 0.8f, 2000);
            if (!loaded && lastTry) break;
            if (!loaded) usleep(250 * 1000);
        }
        if (!loaded || !waiting()) {
            if (loaded) gBootSfx.release();
            gBootSfxState.store(2);
            return;
        }
        while (waiting() && !audioUp()) usleep(50 * 1000);
        // The audio service is up, so the stream opens within moments or not at all: 3 s more at most.
        const int64_t openBy = std::min(deadline, uptimeMillis() + 3000);
        while (uptimeMillis() < openBy && gBootSfxState.load() == 0) {
            if (gBootSfx.prepare()) {
                int expect = 0;
                if (!gBootSfxState.compare_exchange_strong(expect, 1)) gBootSfx.release();   // too late
                return;
            }
            usleep(50 * 1000);
        }
        gBootSfx.release();
        gBootSfxState.store(2);
    }).detach();
}

int NanoMenu::bootSoundState() const { return gBootSfxState.load(); }

void NanoMenu::bootSoundPlay() {
    if (gBootSfxState.exchange(2) != 1) return;
    gBootSfx.trigger();
    // No stream of Nano's left open when a game starts (it wants the mixer to itself).
    std::thread([]() { usleep(3000 * 1000); gBootSfx.release(); }).detach();
}

void NanoMenu::bootSoundDrop() {
    if (gBootSfxState.exchange(2) == 1) gBootSfx.release();
}
static std::string       gEsdeSfxPath[7];
static std::atomic<bool> gEsdeSfxOpening[7];

void NanoMenu::esdeSfx(int which) {
    if (!mEsdeTheme || !navSoundsOn()) return;
    if (which < 0 || which >= 7) return;
    const std::string path = mEsdeSoundPath[which];   // cached at load; no live-doc race
    if (path.empty()) return;
    if (gEsdeSfx[which].loaded() && gEsdeSfxPath[which] == path) { gEsdeSfx[which].trigger(); return; }
    if (gEsdeSfxOpening[which].exchange(true)) return;    // one decode in flight per clip
    std::thread([which, path]() {
        if (gEsdeSfx[which].load(path, 0.8f)) { gEsdeSfxPath[which] = path; gEsdeSfx[which].trigger(); }
        gEsdeSfxOpening[which].store(false);
    }).detach();
}

// Snapshot the 7 ES-DE navigation-sound wav paths from the just-loaded theme (called by
// ensureEsdeTheme). The <sound> elements are theme-global (view name="all"), so they are the same
// for every system; caching them here keeps esdeSfx off the live doc, which the render thread
// reloads per focused system.
void NanoMenu::esdeCacheSounds() {
    static const char* kNames[7] = {
        "systembrowse", "quicksysselect", "select", "back", "scroll", "favorite", "launch" };
    for (int i = 0; i < 7; i++) mEsdeSoundPath[i].clear();
    if (!mEsdeDoc.valid()) return;
    const nanoesde::View* v = mEsdeDoc.view("system");
    if (!v) return;
    for (int i = 0; i < 7; i++) {
        auto it = v->elements.find(std::string("sound\x1f") + kNames[i]);
        if (it != v->elements.end()) mEsdeSoundPath[i] = it->second.getPath("path");
    }
}

// Per-frame DSi SFX driver: detect nav / drill / back / launch by DIFFING this frame's menu state
// against the previous frame, so exactly one sound fires per event regardless of whether the change
// came from the D-pad or from touch (no per-call-site wiring, no double-fire). Called once per frame
// from renderNdsCarousel. Suppressed during a launch fade.
void NanoMenu::ndsSfxTick() {
    if (!mNdsTheme) return;
    static bool    sInit = false, sRoot = true, sModal = false;
    static int     sDepth = 0, sSel = 0, sModalSel = 0;
    static int64_t sFade = 0;
    // launch: fire once when the launch fade-out begins (a card/app is being launched).
    if (mLaunchFadeStart != 0 && sFade == 0) ndsSfxPlay(NDS_SFX_LAUNCH);
    sFade = mLaunchFadeStart;

    const bool root  = mNdsAtRoot;
    const int  depth = (int)mPs3Stack.size();
    const int  sel   = mNdsAtRoot ? mPs3CatIdx
                     : (mPs3Stack.empty() ? mPs3ItemIdx : mPs3Stack.back().sel);
    // A modal (chooser / picker / option menu / dialog) owns input; track its own cursor so moving
    // through its options still chimes (settings_nav) and opening/closing it fires enter/back, 1:1
    // with the DSi. Pick the active modal's selection index.
    const bool modal = ndsInModal();
    const int  modalSel = mPs3TzActive   ? mTzSelected
                        : mPs3LangActive  ? mLangSelected
                        : (mPs3DlgActive || mPs3DlgClosing) ? mPs3DlgSel
                        : mPs3OptActive   ? (mPs3OptSubOpen ? mPs3OptSubSel : mPs3OptSel)
                        : 0;

    if (!sInit) { sInit = true; sRoot = root; sDepth = depth; sSel = sel; sModal = modal; sModalSel = modalSel; return; }
    if (mLaunchFadeStart != 0) {   // no chime during a launch fade
        sRoot = root; sDepth = depth; sSel = sel; sModal = modal; sModalSel = modalSel; return;
    }

    if (modal || sModal) {
        if      (modal && !sModal)                ndsSfxPlay(NDS_SFX_SET_ENTER);   // opened a chooser/picker/dialog
        else if (!modal && sModal)                ndsSfxPlay(NDS_SFX_SET_BACK);    // closed it
        else if (modal && modalSel != sModalSel)  ndsSfxPlay(NDS_SFX_SET_NAV);     // moved within it
    } else {
        const int curLevel  = root  ? 0 : depth + 1;    // 0 = categories root, 1 = a category, 2+ = a submenu
        const int prevLevel = sRoot ? 0 : sDepth + 1;
        if      (curLevel > prevLevel) ndsSfxPlay(NDS_SFX_SET_ENTER);                    // drilled in
        else if (curLevel < prevLevel) ndsSfxPlay(NDS_SFX_SET_BACK);                     // popped up
        else if (sel != sSel)          ndsSfxPlay(ndsCurLevelIsList() ? NDS_SFX_SET_NAV  // settings-list move
                                                                      : NDS_SFX_NAV);    // carousel move
    }
    sRoot = root; sDepth = depth; sSel = sel; sModal = modal; sModalSel = modalSel;
}

// Per-frame carousel background ambiance: loop menu_ambiance.wav while the DSi home is up,
// stopped otherwise. Looped by seeking back to 0 when the track ends. Independent player instance.
void NanoMenu::ndsAmbianceTick(bool wantOnHome) {
    const bool direct = nanoDirectAudioUsable() && !dsiBootCompleted();

    // ---- Phase A: pre-boot-complete -> DIRECT PCM loop (audio server not up yet) ----
    if (direct) {
        // Open the card and stream silence from the very first frame, before the first sound is
        // queued. A smart PA (the RG DS Plus loudspeaker, aw882xx) only powers up once its PCM is
        // running and takes the better part of a second to come up, so a sound that opened the
        // card itself lost its first second; the PS3 theme already holds this way.
        if (!mDirectWarmHold) { nanoDirectHoldOpen(true); mDirectWarmHold = true; }
        if (!wantOnHome) { if (mDirectAmbiancePlaying) { nanoDirectStopLoop(); mDirectAmbiancePlaying = false; } return; }
        if (!mDirectAmbiancePlaying) {
            nanoDirectStartLoop(dsiAudioPath("menu_ambiance.wav"), dsiEarlyAudioGain(0.56f));   // web 0.8*0.7
            mDirectAmbiancePlaying = true;
        } else {
            nanoDirectSetGain(dsiEarlyAudioGain(0.56f));   // track a live volume change during boot
        }
        return;
    }

    // ---- Handoff: booted (or direct unavailable) while the direct bed or the warm hold was up -> release for AAudio ----
    if (mDirectAmbiancePlaying || mDirectWarmHold) {
        if (mDirectAmbiancePlaying) nanoDirectStopLoop();
        if (mDirectWarmHold) { nanoDirectHoldOpen(false); mDirectWarmHold = false; }
        nanoDirectShutdown();                 // bounded wait: the PCM is closed before AAudio opens (no EBUSY)
        mDirectAmbiancePlaying = false;
        mDirectHandedOff = true;              // card0 handed to the HAL; the safety-net tick need not repeat it
        // fall through: mAmbiancePlaying is still false, so the AAudio block opens fresh.
    }

    // ---- Phase B: AAudio path (all devices post-boot, or where direct is unavailable) ----
    if (mAmbianceOpening) return;   // a bg open is in flight: never touch the player concurrently
    if (!wantOnHome) {
        if (mAmbiancePlaying) { mAmbiancePlayer.stop(); mAmbiancePlaying = false; }
        return;
    }
    if (!mAmbiancePlaying) {
        mAmbianceOpening.store(true);
        std::string path = dsiAudioPath("menu_ambiance.wav");
        std::thread([this, path]() {
            mAmbiancePlayer.init();
            if (mAmbiancePlayer.open(path)) {
                mAmbiancePlayer.setVolume(0.56f);   // web master 0.8 * voice 0.7 (was 0.7, a 1:1 miss)
                mAmbiancePlayer.play();
                mAmbiancePlaying = true;
            }
            mAmbianceOpening.store(false);
        }).detach();
        return;
    }
    if (mAmbiancePlayer.ended()) { mAmbiancePlayer.seek(0.0); mAmbiancePlayer.play(); }  // loop
}

// PS3 XMB early-boot audio hold. The DSi keeps card0 (the direct-ALSA PCM) held open through the
// pre-boot-complete window via its menu_ambiance loop, so its nav SFX mix into an already-open
// substream. The PS3 theme has no early BGM, so it never held card0: the worker parked after each
// sound, and the next nav SFX re-opened card0 exactly as the RK3568
// audio HAL was grabbing it (EBUSY), permanently latching gEng.failed and dropping all PS3 early
// SFX onto the still-blocked AAudio path -> silence until boot_completed. Fix: hold card0 open with
// SILENCE through the pre-boot-complete window (nav one-shots mix in), then hand it to the HAL at
// boot_completed so post-boot audio is unaffected. DSi is handled by ndsAmbianceTick; this is the
// PS3 (!mNdsTheme) equivalent. Called once per frame from render().
void NanoMenu::ps3EarlyAudioTick() {
    if (mNdsTheme) {
        // A LIVE PS3->DSi theme switch (the Home Theme row flips mNdsTheme in-memory without a
        // restart) during the hold window would leak our hold; release it so ndsAmbianceTick
        // cleanly owns the direct engine from here.
        if (mPs3DirectHolding) { nanoDirectHoldOpen(false); mPs3DirectHolding = false; }
        return;                                              // DSi uses ndsAmbianceTick's own hold
    }
    const bool direct = nanoDirectAudioUsable() && !dsiBootCompleted();
    if (direct) {
        if (!mPs3DirectHolding) { nanoDirectHoldOpen(true); mPs3DirectHolding = true; }
        return;
    }
    // Booted (or direct unavailable) while we were holding -> release the hold + the card for the
    // audio HAL, exactly once. nanoDirectShutdown does a bounded wait so card0 is closed before
    // AAudio opens (no EBUSY), mirroring the DSi handoff in ndsAmbianceTick.
    if (mPs3DirectHolding) {
        nanoDirectHoldOpen(false);
        nanoDirectShutdown();
        mPs3DirectHolding = false;
        mDirectHandedOff = true;              // card0 handed to the HAL; the safety-net tick need not repeat it
    }
}

// Guaranteed direct-PCM -> audio-HAL handoff, called every frame from the main run loop
// (NOT from render(), which skips the audio ticks while the first-boot setup wizard is up).
// The home ambiance tick (ndsAmbianceTick) and the PS3 hold tick (ps3EarlyAudioTick) each
// release card0 with a bounded wait (nanoDirectShutdown) at boot-complete, but only when
// their own bed was up, and neither runs during the setup wizard. So on a first boot the
// direct engine could keep owning card0 past boot_completed, and every later open by the
// audio HAL / AudioFlinger would EBUSY -> all system audio dies after setup. Close the
// engine here exactly once, after the home ticks have had their chance to hand off cleanly.
void NanoMenu::nanoDirectHandoffTick() {
    if (mDirectHandedOff || !dsiBootCompleted()) return;
    // If a direct bed is still up, let its own tick do the smooth handoff (and set the flag);
    // only step in for the case where nothing on the home ever ran (the setup wizard).
    if (mDirectAmbiancePlaying || mPs3DirectHolding) return;
    nanoDirectShutdown();                     // bounded wait: card0 is free before the HAL opens it
    mDirectHandedOff = true;
}

} // namespace android
