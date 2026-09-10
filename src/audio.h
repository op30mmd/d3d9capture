#pragma once
/**
 * audio.h  —  Low-latency in-process game audio capture
 *
 * The video side of this project pulls pixels out of the back buffer rather
 * than scraping the screen; this is the audio equivalent.  PCM is taken from
 * the game's own WASAPI render buffer, in the game's process, at the last
 * moment before the audio engine consumes it — no desktop loopback, no second
 * copy of every other application's sound.
 *
 * Why WASAPI and not XAudio2 / DirectSound
 * ────────────────────────────────────────
 * Since Vista every audio path on Windows ends at an IAudioRenderClient:
 * XAudio2's mastering voice, DirectSound's emulated buffers, FMOD, Wwise and a
 * game's own mixer all render into one.  Hooking that single pair of methods
 * therefore captures every engine with one hook, instead of one hook per API.
 *
 * Lifecycle
 *   Audio_Init()       – installs the hooks (called once, from Capture_Init)
 *   Audio_SetWanted()  – a consumer announces interest; nothing is copied off
 *                        the audio thread unless somebody is listening
 *   Audio_ReadStereo16()– consumer drains one capture buffer at a time
 *   Audio_Shutdown()   – restores the patched vtable slots
 *
 * Threading
 *   The producer is the game's audio thread (or the loopback fallback thread).
 *   It has a hard deadline — missing it is an audible glitch in the game — so
 *   the hook takes no lock, allocates nothing, and does one memcpy.  Delivery
 *   is a single-producer / single-consumer ring; the consumer is expected to be
 *   the recorder's encode worker, and there must be only one.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Where the PCM being captured comes from.  Chosen per capture session and
// reported in AudioStats, because "the recording has no sound" and "the
// recording has the wrong sound" are both explained by this one value.
enum AudioSourceKind
{
    AudioSource_None     = 0,  // nothing has been captured yet
    AudioSource_Hook     = 1,  // the game's own render buffer (low latency)
    AudioSource_Loopback = 2   // desktop loopback fallback (see audio.cpp)
};

// User configurable audio capture mode
enum AudioCaptureMode
{
    AudioCaptureMode_Auto       = 0, // Direct hook prioritized; auto-fallback to loopback if silent/unavailable
    AudioCaptureMode_DirectHook = 1, // Only in-process WASAPI render hook
    AudioCaptureMode_Loopback   = 2  // Desktop / system loopback capture
};

// Largest run of PCM frames one Audio_ReadStereo16 call can return.  A capture
// buffer bigger than this is split across several reads, so a caller's
// destination buffer never has to be sized from a device period it cannot know
// in advance.  dst must hold AUDIO_MAX_CHUNK_FRAMES * 2 int16 samples.
static constexpr uint32_t AUDIO_MAX_CHUNK_FRAMES = 4096;

struct AudioStats
{
    bool     hooksInstalled;   // the WASAPI vtable patches are live
    bool     enabled;          // master switch (overlay)
    bool     wanted;           // a consumer is currently listening
    int      source;           // AudioSourceKind (active source)
    int      captureMode;      // AudioCaptureMode (configured mode)
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t bitsPerSample;
    bool     isFloat;
    float    devicePeriodMs;   // the buffer period being tapped; the latency
                               // floor of the hook path
    uint64_t capturedFrames;
    uint64_t droppedFrames;    // PCM frames lost to ring overruns
    uint64_t overruns;         // times the consumer fell behind
    uint32_t ringFillPercent;
    float    peakDb;           // most recent peak level, dBFS
    uint32_t activeStreams;    // count of detected game audio clients
    char     lastError[128];
};

void Audio_Init();
void Audio_Shutdown();

// Audio Capture Mode configuration
void Audio_SetCaptureMode(int mode);
int  Audio_GetCaptureMode();

// Master switch.  When disabled the hooks stay installed but pass straight
// through, so toggling it can never leave the game's audio broken.
void Audio_SetEnabled(bool enabled);
bool Audio_IsEnabled();

// Consumers announce interest.  While nothing is wanted the hook does nothing
// but call the original, which is what keeps an idle injection free.
void Audio_SetWanted(bool wanted);
bool Audio_IsWanted();

// Discard everything buffered.  Call at the start of a capture session so it
// does not open with however many milliseconds of stale audio were in the ring.
void Audio_Flush();

uint32_t Audio_GetSampleRate();

/**
 * Drain one capture buffer, converted to interleaved 16-bit stereo — the one
 * format the Media Foundation AAC encoder accepts, and the reason the
 * conversion lives here rather than in the recorder.
 *
 * Returns the number of PCM frames written to dst (0 if nothing is buffered).
 * *outQpc receives the QueryPerformanceCounter value taken when the game wrote
 * that buffer, which is what lets the recorder line audio up with video that
 * carries the same kind of stamp.
 */
uint32_t Audio_ReadStereo16(int16_t* dst, uint32_t maxFrames, uint64_t* outQpc);

void Audio_GetStats(AudioStats* out);

// Software volume / gain multiplier (1.0 = 100% normal, 0.0 = mute, 2.0 = +6 dB boost)
void  Audio_SetVolume(float volume);
float Audio_GetVolume();

// Generates a sine test tone into the ring buffer to test audio pipeline
void Audio_InjectTestTone(uint32_t freqHz = 440, uint32_t durationMs = 500);
