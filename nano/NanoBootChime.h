// Direct-to-ALSA early audio (the menu sound effects and the DSi ambiance before boot completes).
//
// The normal nano audio path (NanoAudioPlayer -> AAudio) cannot make a sound early in boot: on
// some vendors the AudioPolicyManager constructor blocks audioserver bring-up for ~20s (it waits on
// ActivityManagerService, which is gated behind the cold-boot dex2oat compile), so
// AAudioStreamBuilder_openStream() does not return until ~30s+. The ALSA card, however, is
// registered ~6s before nano even starts. So until boot completes these sounds are written straight
// to the card's playback PCM via tinyalsa, bypassing AudioFlinger entirely.

#pragma once

#include <string>

// True if the direct-PCM path should be attempted on THIS device: a card 0 playback node exists and
// the direct path has not already failed this session. Works on any ALSA device, not just the rk817
// (the speaker-route mixer step is best-effort). The engine marks itself unusable on the first
// pcm_open/pcm_write failure so callers cleanly fall back to the normal AAudio path.
bool nanoDirectAudioUsable();

// ---- Sequential direct-to-ALSA engine (RG DS pre-boot-complete window) --------------------------
// A single persistent detached worker owns the one rk817 PCM substream ONLY while a voice is
// active: a small one-shot queue plus one optional looping voice, mixed. `gain` is the FINAL linear
// scalar (per-voice master * dB-mapped system volume), already computed by the caller. All calls are non-blocking and render-thread safe. The
// worker self-paces on pcm_write, yields on EBUSY / write error with no retry, and enforces a hard
// wall-time cap on the loop so it can never hold the card indefinitely.
void nanoDirectPlayOneShot(const std::string& wavPath, float gain);  // queue a one-shot
void nanoDirectStartLoop  (const std::string& wavPath, float gain);  // set/replace the looping voice
void nanoDirectSetGain    (float gain);   // live-update the active loop gain (volume changed before boot completes)
void nanoDirectStopLoop   ();             // clear the loop; worker idles + closes once the queue drains
void nanoDirectHoldOpen   (bool on);      // hold card0 open (silent) while true so one-shots mix without a re-open
void nanoDirectShutdown   ();             // stop everything, close the PCM, let the worker exit
bool nanoDirectActive     ();             // true while the worker owns the PCM (handoff gate)
