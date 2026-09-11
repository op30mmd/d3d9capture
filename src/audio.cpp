/**
 * audio.cpp  —  Low-latency in-process game audio capture & WASAPI hooks
 *
 * Implements direct hooking of IAudioRenderClient::GetBuffer and
 * IAudioRenderClient::ReleaseBuffer, tapping the game's audio at the render
 * buffer level with near-zero latency and no extra mix/copy stages.
 *
 * Multi-Client & Multi-Stream Architecture:
 *   - Tracks format (sample rate, channels, bit depth, float vs int) per client
 *     from IAudioClient::Initialize.
 *   - Identifies and isolates the Master Game Audio Render Client (highest
 *     channel count & sample rate), ignoring secondary voice chat or mic streams
 *     to prevent interleaving, tempo-doubling, or garbled audio.
 *   - Lock-free / mutex-serialized thread-safe ring buffer.
 *   - Universal ITU-R BS.775 downmix from 7.1/5.1/stereo/mono in 32-bit float,
 *     32-bit int, 16-bit int, or 24-bit to interleaved 16-bit stereo.
 *   - Intelligent loopback fallback that only runs if direct hooks are inactive.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "audio.h"
#include "capture.h"

// ── Configuration & Ring Buffer ──────────────────────────────────────────────
static constexpr size_t RING_SLOTS = 256; // ~2.5 to 5 seconds of audio buffer

#pragma pack(push, 1)
struct AudioRingSlot
{
    uint64_t qpc;
    uint32_t numFrames;
    uint32_t sampleRate;
    int16_t  data[AUDIO_MAX_CHUNK_FRAMES * 2]; // Interleaved stereo 16-bit
};
#pragma pack(pop)

// ── Client Format & Stream Tracking ──────────────────────────────────────────
struct ClientAudioFormat
{
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bitsPerSample = 32;
    bool     isFloat = true;
    float    devicePeriodMs = 10.0f;
    int      score = 0; // channels * 100000 + sampleRate
};

struct TrackedAudioClient
{
    IAudioClient*     client = nullptr;
    ClientAudioFormat format;
};

// ── Level measurement ────────────────────────────────────────────────────────
// The peak meter and the "is this stream audible" gate must ignore content no
// speaker can reproduce.  GTA San Andreas is the case that forced this: with
// nothing playing, its DirectSound->WASAPI 3D mixer idles with a ~5 Hz,
// -38 dBFS wander on the centre and rear channels (front L/R stay at exactly
// zero), slowly modulated over ~8 s.  A plain max(|x|) reads that as a level
// bouncing between -60 and -38 dBFS, so the overlay's waveform bounced with it.
//
// Every level measurement therefore runs through two cascaded one-pole
// high-pass stages (12 dB/oct, -3 dB at 30 Hz) with independent state per
// channel.  Measured against the captured wander that takes it from -38 to
// -85 dBFS, well under the -70 dBFS audibility gate, while a 60 Hz tone reads
// ~2 dB low and 100 Hz under 1 dB.  DC is removed entirely.
static constexpr uint32_t LEVEL_FILTER_CHANNELS = 8;
static constexpr float    LEVEL_FILTER_CUTOFF_HZ = 30.0f;

struct LevelFilter
{
    float x1[2][LEVEL_FILTER_CHANNELS] = {};
    float y1[2][LEVEL_FILTER_CHANNELS] = {};
};

static inline float LevelFilterStep(LevelFilter* f, uint32_t ch, float x, float r)
{
    // Channels past what the state covers (unusual layouts) are measured raw
    // rather than sharing state with another channel.
    if (!f || ch >= LEVEL_FILTER_CHANNELS) return x;

    float y = x - f->x1[0][ch] + r * f->y1[0][ch];
    f->x1[0][ch] = x;
    f->y1[0][ch] = y;

    float y2 = y - f->x1[1][ch] + r * f->y1[1][ch];
    f->x1[1][ch] = y;
    f->y1[1][ch] = y2;

    // The recursive terms decay towards zero during silence; flush them before
    // they reach the denormal range, where x87/SSE arithmetic gets very slow
    // on the game's audio thread.
    if (fabsf(f->y1[0][ch]) < 1e-20f) f->y1[0][ch] = 0.0f;
    if (fabsf(f->y1[1][ch]) < 1e-20f) f->y1[1][ch] = 0.0f;
    return y2;
}

static inline float LevelFilterCoefficient(uint32_t sampleRate)
{
    const float fs = sampleRate ? static_cast<float>(sampleRate) : 48000.0f;
    return 1.0f - 2.0f * 3.14159265f * LEVEL_FILTER_CUTOFF_HZ / fs;
}

struct TrackedRenderClient
{
    IAudioRenderClient* renderClient = nullptr;
    IAudioClient*       audioClient = nullptr;
    ClientAudioFormat   format;
    BYTE*               lastBuffer = nullptr;
    UINT32              lastFrames = 0;
    DWORD               lastReleaseTick = 0;
    DWORD               lastAudibleTick = 0;
    float               lastPeak = 0.0f;
    bool                isAudible = false;
    LevelFilter         levelFilter;   // touched only by this client's audio thread
};

static constexpr size_t MAX_TRACKED = 16;
static TrackedAudioClient  s_AudioClients[MAX_TRACKED] = {};
static TrackedRenderClient s_RenderClients[MAX_TRACKED] = {};
static std::mutex          s_ClientTrackingMtx;

// The active master render client (e.g. GTA V 7.1/stereo 48kHz game world mixer)
static std::atomic<IAudioRenderClient*> s_MasterRenderClient{ nullptr };
static std::atomic<DWORD>               s_LastHookReleaseTick{ 0 };
static std::atomic<DWORD>               s_LastAudibleHookTick{ 0 };
static std::atomic<DWORD>               s_RecordStartTick{ 0 };
static std::atomic<bool>                s_InInit{ false };

// ── Global State ─────────────────────────────────────────────────────────────
static std::atomic<bool>     s_HooksInstalled{ false };
static std::atomic<bool>     g_AudioEnabled{ true };
static std::atomic<bool>     g_AudioWanted{ false };
static std::atomic<int>      s_CaptureMode{ AudioCaptureMode_Auto };
static std::atomic<int>      s_Source{ AudioSource_None };

static std::atomic<uint32_t> s_SampleRate{ 48000 };
static std::atomic<uint32_t> s_Channels{ 2 };
static std::atomic<uint32_t> s_BitsPerSample{ 32 };
static std::atomic<bool>     s_IsFloat{ true };
static std::atomic<float>    s_DevicePeriodMs{ 10.0f };

static std::atomic<uint64_t> s_CapturedFrames{ 0 };
static std::atomic<uint64_t> s_DroppedFrames{ 0 };
static std::atomic<uint64_t> s_Overruns{ 0 };
static std::atomic<float>    s_PeakDb{ -96.0f };
static std::atomic<float>    s_SoftwareGain{ 1.0f };

static std::mutex            s_ErrMtx;
static char                  s_LastError[128] = "";

static LARGE_INTEGER         s_QpcFreq = {};

// SPSC / Multi-Producer Protected Ring Buffer
static AudioRingSlot*        s_RingSlots = nullptr;
static std::mutex            s_RingMtx;
static std::atomic<uint32_t> s_RingHead{ 0 };
static std::atomic<uint32_t> s_RingTail{ 0 };
static uint32_t              s_CurrentSlotOffset = 0; // Consumer read offset inside current slot

// Loopback fallback thread state
static std::thread           s_LoopbackThread;
static std::atomic<bool>     s_LoopbackRunning{ false };
static std::mutex            s_LoopbackMtx;
static std::condition_variable s_LoopbackCv;

// ── Saved Function Pointers & VTable Patching ────────────────────────────────
typedef HRESULT (WINAPI *PFN_GetBuffer)(IAudioRenderClient*, UINT32, BYTE**);
typedef HRESULT (WINAPI *PFN_ReleaseBuffer)(IAudioRenderClient*, UINT32, DWORD);

typedef HRESULT (WINAPI *PFN_AudioClient_Initialize)(
    IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
typedef HRESULT (WINAPI *PFN_AudioClient_GetService)(IAudioClient*, REFIID, void**);

static PFN_GetBuffer                 s_OrigGetBuffer = nullptr;
static PFN_ReleaseBuffer             s_OrigReleaseBuffer = nullptr;
static PFN_AudioClient_Initialize    s_OrigClientInit = nullptr;
static PFN_AudioClient_GetService    s_OrigClientGetService = nullptr;

static void** s_RenderVTable = nullptr;
static void** s_ClientVTable = nullptr;
static std::mutex s_HookMtx;

// Per-thread tracking for active GetBuffer pointers (audio thread fast path)
static thread_local BYTE*               t_LastBuffer = nullptr;
static thread_local UINT32              t_LastFramesRequested = 0;
static thread_local IAudioRenderClient* t_LastClient = nullptr;

static void SetAudioError(const char* fmt, ...)
{
    char msg[sizeof(s_LastError)] = "";
    if (fmt && *fmt)
    {
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
        va_end(args);
        Log("[audio] %s", msg);
    }
    std::lock_guard<std::mutex> lock(s_ErrMtx);
    strncpy_s(s_LastError, sizeof(s_LastError), msg, _TRUNCATE);
}

static bool PatchVTableSlot(void** ppSlot, void* pNew, void** ppOld)
{
    if (!ppSlot || !pNew || !ppOld) return false;
    if (*ppSlot == pNew) return true;

    DWORD oldProt = 0;
    if (!VirtualProtect(ppSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt))
    {
        Log("[audio] VirtualProtect(%p) failed: %lu", ppSlot, GetLastError());
        return false;
    }

    void* old = InterlockedExchangePointer(ppSlot, pNew);
    *ppOld = old;

    DWORD ignored = 0;
    VirtualProtect(ppSlot, sizeof(void*), oldProt, &ignored);
    FlushInstructionCache(GetCurrentProcess(), ppSlot, sizeof(void*));
    return true;
}

// ── Format Parsing & Tracking ────────────────────────────────────────────────
static ClientAudioFormat ParseWaveFormat(const WAVEFORMATEX* pwfx, REFERENCE_TIME hnsBufferDuration)
{
    ClientAudioFormat fmt;
    if (!pwfx) return fmt;

    fmt.sampleRate = pwfx->nSamplesPerSec ? pwfx->nSamplesPerSec : 48000;
    fmt.channels = pwfx->nChannels ? pwfx->nChannels : 2;
    fmt.bitsPerSample = pwfx->wBitsPerSample ? pwfx->wBitsPerSample : 16;
    fmt.isFloat = false;

    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        fmt.isFloat = true;
    }
    else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            fmt.isFloat = true;
        else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            fmt.isFloat = false;
    }

    if (hnsBufferDuration > 0)
    {
        fmt.devicePeriodMs = static_cast<float>(hnsBufferDuration) / 10000.0f;
    }

    fmt.score = static_cast<int>(fmt.channels) * 100000 + static_cast<int>(fmt.sampleRate);
    return fmt;
}

static void RegisterAudioClientFormat(IAudioClient* client, const ClientAudioFormat& fmt)
{
    std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
    for (size_t i = 0; i < MAX_TRACKED; ++i)
    {
        if (s_AudioClients[i].client == client || s_AudioClients[i].client == nullptr)
        {
            s_AudioClients[i].client = client;
            s_AudioClients[i].format = fmt;
            return;
        }
    }
    s_AudioClients[0].client = client;
    s_AudioClients[0].format = fmt;
}

static ClientAudioFormat LookupAudioClientFormat(IAudioClient* client)
{
    std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
    if (client != nullptr)
    {
        for (size_t i = 0; i < MAX_TRACKED; ++i)
        {
            if (s_AudioClients[i].client == client)
            {
                return s_AudioClients[i].format;
            }
        }
    }
    ClientAudioFormat def;
    def.sampleRate     = s_SampleRate.load(std::memory_order_relaxed);
    def.channels       = s_Channels.load(std::memory_order_relaxed);
    def.bitsPerSample  = s_BitsPerSample.load(std::memory_order_relaxed);
    def.isFloat        = s_IsFloat.load(std::memory_order_relaxed);
    def.devicePeriodMs = s_DevicePeriodMs.load(std::memory_order_relaxed);
    def.score          = static_cast<int>(def.channels) * 100000 + static_cast<int>(def.sampleRate);
    return def;
}

static void UpdateGlobalFormat(const ClientAudioFormat& fmt)
{
    s_SampleRate.store(fmt.sampleRate ? fmt.sampleRate : 48000);
    s_Channels.store(fmt.channels ? fmt.channels : 2);
    s_BitsPerSample.store(fmt.bitsPerSample ? fmt.bitsPerSample : 32);
    s_IsFloat.store(fmt.isFloat);
    s_DevicePeriodMs.store(fmt.devicePeriodMs > 0.0f ? fmt.devicePeriodMs : 10.0f);
}

static void RegisterRenderClient(IAudioRenderClient* renderClient, IAudioClient* audioClient, const ClientAudioFormat& fmt)
{
    std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);

    TrackedRenderClient* entry = nullptr;
    for (size_t i = 0; i < MAX_TRACKED; ++i)
    {
        if (s_RenderClients[i].renderClient == renderClient || s_RenderClients[i].renderClient == nullptr)
        {
            entry = &s_RenderClients[i];
            break;
        }
    }
    if (!entry) entry = &s_RenderClients[0];

    entry->renderClient    = renderClient;
    entry->audioClient     = audioClient;
    entry->format          = fmt;
    entry->lastBuffer      = nullptr;
    entry->lastFrames      = 0;
    entry->lastReleaseTick = GetTickCount();
    entry->lastAudibleTick = 0;
    entry->lastPeak        = 0.0f;
    entry->isAudible       = false;
    entry->levelFilter     = LevelFilter{};

    // Evaluate master client candidacy:
    // Game master audio has >= 2 channels and >= 32000 Hz
    if (fmt.sampleRate >= 32000 && fmt.channels >= 2)
    {
        IAudioRenderClient* curMaster = s_MasterRenderClient.load();
        if (!curMaster)
        {
            s_MasterRenderClient.store(renderClient);
            UpdateGlobalFormat(fmt);
            Log("[audio] Assigned primary master render client: %p (%u Hz, %u ch, %u-bit %s, score=%d)",
                renderClient, fmt.sampleRate, fmt.channels, fmt.bitsPerSample,
                fmt.isFloat ? "float" : "pcm", fmt.score);
        }
        else
        {
            // Check if new client has equal or higher score (e.g. 8ch replacing 8ch on level change, or 8ch vs 2ch)
            TrackedRenderClient* curEntry = nullptr;
            for (size_t i = 0; i < MAX_TRACKED; ++i)
            {
                if (s_RenderClients[i].renderClient == curMaster) { curEntry = &s_RenderClients[i]; break; }
            }
            const DWORD now = GetTickCount();
            const bool curMasterAudible = curEntry && (curEntry->lastAudibleTick > 0) && ((now - curEntry->lastAudibleTick) < 1000);

            // Only update master during registration if current master is NOT actively audible
            if (!curEntry || (!curMasterAudible && fmt.score > curEntry->format.score))
            {
                s_MasterRenderClient.store(renderClient);
                UpdateGlobalFormat(fmt);
                Log("[audio] Upgraded master render client: %p (%u Hz, %u ch, %u-bit %s, score=%d)",
                    renderClient, fmt.sampleRate, fmt.channels, fmt.bitsPerSample,
                    fmt.isFloat ? "float" : "pcm", fmt.score);
            }
        }
    }
}

static TrackedRenderClient* FindRenderClientLocked(IAudioRenderClient* renderClient)
{
    for (size_t i = 0; i < MAX_TRACKED; ++i)
    {
        if (s_RenderClients[i].renderClient == renderClient)
            return &s_RenderClients[i];
    }
    return nullptr;
}

// ── Audio Format Conversion & Audibility ──────────────────────────────────────
static void ConvertToStereo16(
    const uint8_t* src,
    uint32_t frameOffset,
    uint32_t numFrames,
    uint32_t channels,
    uint32_t bitsPerSample,
    bool isFloat,
    bool isSilent,
    int16_t* dst)
{
    if (isSilent || !src || numFrames == 0)
    {
        memset(dst, 0, numFrames * 2 * sizeof(int16_t));
        return;
    }

    if (isFloat && bitsPerSample == 32)
    {
        const float* fsrc = reinterpret_cast<const float*>(src) + (frameOffset * channels);
        if (channels == 2)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                float l = fsrc[i * 2 + 0];
                float r = fsrc[i * 2 + 1];
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                dst[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                dst[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
        }
        else if (channels >= 6)
        {
            // 5.1 / 7.1 surround downmixing (ITU-R BS.775)
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                const float* f = &fsrc[i * channels];
                float fl  = f[0];
                float fr  = f[1];
                float fc  = f[2];
                float lfe = f[3];
                float bl  = f[4];
                float br  = f[5];
                float sl  = (channels >= 8) ? f[6] : 0.0f;
                float sr  = (channels >= 8) ? f[7] : 0.0f;

                float l = (fl + 0.7071f * fc + 0.7071f * (bl + sl) + 0.5f * lfe) * 0.7f;
                float r = (fr + 0.7071f * fc + 0.7071f * (br + sr) + 0.5f * lfe) * 0.7f;
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                dst[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                dst[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
        }
        else if (channels == 1)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                float m = fsrc[i];
                if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;
                int16_t s = static_cast<int16_t>(m * 32767.0f);
                dst[i * 2 + 0] = s;
                dst[i * 2 + 1] = s;
            }
        }
        else
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                float l = fsrc[i * channels + 0];
                float r = fsrc[i * channels + 1];
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                dst[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                dst[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
        }
    }
    else if (!isFloat && bitsPerSample == 32)
    {
        // 32-bit Integer PCM
        const int32_t* isrc = reinterpret_cast<const int32_t*>(src) + (frameOffset * channels);
        if (channels == 2)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                dst[i * 2 + 0] = static_cast<int16_t>(isrc[i * 2 + 0] >> 16);
                dst[i * 2 + 1] = static_cast<int16_t>(isrc[i * 2 + 1] >> 16);
            }
        }
        else if (channels >= 6)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                const int32_t* f = &isrc[i * channels];
                float fl  = (f[0] >> 16) / 32768.0f;
                float fr  = (f[1] >> 16) / 32768.0f;
                float fc  = (f[2] >> 16) / 32768.0f;
                float lfe = (f[3] >> 16) / 32768.0f;
                float bl  = (f[4] >> 16) / 32768.0f;
                float br  = (f[5] >> 16) / 32768.0f;
                float sl  = (channels >= 8) ? ((f[6] >> 16) / 32768.0f) : 0.0f;
                float sr  = (channels >= 8) ? ((f[7] >> 16) / 32768.0f) : 0.0f;

                float l = (fl + 0.7071f * fc + 0.7071f * (bl + sl) + 0.5f * lfe) * 0.7f;
                float r = (fr + 0.7071f * fc + 0.7071f * (br + sr) + 0.5f * lfe) * 0.7f;
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                dst[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                dst[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
        }
        else if (channels == 1)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                int16_t s = static_cast<int16_t>(isrc[i] >> 16);
                dst[i * 2 + 0] = s;
                dst[i * 2 + 1] = s;
            }
        }
        else
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                dst[i * 2 + 0] = static_cast<int16_t>(isrc[i * channels + 0] >> 16);
                dst[i * 2 + 1] = static_cast<int16_t>(isrc[i * channels + 1] >> 16);
            }
        }
    }
    else if (!isFloat && bitsPerSample == 16)
    {
        const int16_t* isrc = reinterpret_cast<const int16_t*>(src) + (frameOffset * channels);
        if (channels == 2)
        {
            memcpy(dst, isrc, numFrames * 2 * sizeof(int16_t));
        }
        else if (channels >= 6)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                const int16_t* f = &isrc[i * channels];
                float fl  = f[0] / 32768.0f;
                float fr  = f[1] / 32768.0f;
                float fc  = f[2] / 32768.0f;
                float lfe = f[3] / 32768.0f;
                float bl  = f[4] / 32768.0f;
                float br  = f[5] / 32768.0f;
                float sl  = (channels >= 8) ? (f[6] / 32768.0f) : 0.0f;
                float sr  = (channels >= 8) ? (f[7] / 32768.0f) : 0.0f;

                float l = (fl + 0.7071f * fc + 0.7071f * (bl + sl) + 0.5f * lfe) * 0.7f;
                float r = (fr + 0.7071f * fc + 0.7071f * (br + sr) + 0.5f * lfe) * 0.7f;
                if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
                dst[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                dst[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
            }
        }
        else if (channels == 1)
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                dst[i * 2 + 0] = isrc[i];
                dst[i * 2 + 1] = isrc[i];
            }
        }
        else
        {
            for (uint32_t i = 0; i < numFrames; ++i)
            {
                dst[i * 2 + 0] = isrc[i * channels + 0];
                dst[i * 2 + 1] = isrc[i * channels + 1];
            }
        }
    }
    else
    {
        // Generic fallback (e.g. 24-bit PCM)
        const uint32_t bytesPerSample = bitsPerSample / 8;
        const uint8_t* bsrc = src + (frameOffset * channels * bytesPerSample);
        for (uint32_t i = 0; i < numFrames; ++i)
        {
            int32_t l = 0, r = 0;
            if (bytesPerSample == 3)
            {
                const uint8_t* pL = bsrc + (i * channels + 0) * 3;
                l = static_cast<int32_t>((pL[0] << 8) | (pL[1] << 16) | (pL[2] << 24)) >> 16;
                const uint8_t* pR = bsrc + ((channels > 1) ? (i * channels + 1) : (i * channels + 0)) * 3;
                r = static_cast<int32_t>((pR[0] << 8) | (pR[1] << 16) | (pR[2] << 24)) >> 16;
            }
            dst[i * 2 + 0] = static_cast<int16_t>(l);
            dst[i * 2 + 1] = static_cast<int16_t>(r);
        }
    }
}

// Peak of one buffer as heard: every sample goes through the high-pass in
// `filter` (see LevelFilter) before the max is taken, so DC and sub-audible
// wander never register.  `filter` may be null for a raw measurement.
static bool CheckBufferAudible(
    const BYTE* pData,
    UINT32 numFrames,
    uint32_t channels,
    uint32_t bitsPerSample,
    bool isFloat,
    uint32_t sampleRate,
    LevelFilter* filter,
    float* outPeak)
{
    if (!pData || numFrames == 0 || channels == 0)
    {
        if (outPeak) *outPeak = 0.0f;
        return false;
    }

    const float r = LevelFilterCoefficient(sampleRate);
    float maxVal = 0.0f;

    if (isFloat && bitsPerSample == 32)
    {
        const float* f = reinterpret_cast<const float*>(pData);
        for (uint32_t i = 0; i < numFrames; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                float v = fabsf(LevelFilterStep(filter, ch, f[i * channels + ch], r));
                if (v > maxVal) maxVal = v;
            }
    }
    else if (!isFloat && bitsPerSample == 32)
    {
        const int32_t* p = reinterpret_cast<const int32_t*>(pData);
        for (uint32_t i = 0; i < numFrames; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                float norm = static_cast<float>(p[i * channels + ch]) / 2147483648.0f;
                float v = fabsf(LevelFilterStep(filter, ch, norm, r));
                if (v > maxVal) maxVal = v;
            }
    }
    else if (!isFloat && bitsPerSample == 16)
    {
        const int16_t* p = reinterpret_cast<const int16_t*>(pData);
        for (uint32_t i = 0; i < numFrames; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
            {
                float norm = static_cast<float>(p[i * channels + ch]) / 32768.0f;
                float v = fabsf(LevelFilterStep(filter, ch, norm, r));
                if (v > maxVal) maxVal = v;
            }
    }

    // The high-pass can overshoot full scale by a fraction of a dB near Nyquist.
    if (maxVal > 1.0f) maxVal = 1.0f;
    if (outPeak) *outPeak = maxVal;
    // Considered audible if peak exceeds ~ -70 dBFS (0.0003f)
    return (maxVal > 0.0003f);
}

// ── Ring Buffer Push (Thread-Safe, Downmixed & Stream-Mixing) ─────────────────
static void PushAudioChunk(
    uint64_t qpc,
    uint32_t numFrames,
    uint32_t sampleRate,
    uint32_t channels,
    uint32_t bitsPerSample,
    bool isFloat,
    bool isSilent,
    const BYTE* pData,
    int sourceKind)
{
    if (!s_RingSlots || numFrames == 0) return;

    if (numFrames > AUDIO_MAX_CHUNK_FRAMES)
        numFrames = AUDIO_MAX_CHUNK_FRAMES;

    // Convert incoming format to interleaved stereo 16-bit
    static thread_local int16_t s_TempStereo[AUDIO_MAX_CHUNK_FRAMES * 2];
    ConvertToStereo16(pData, 0, numFrames, channels, bitsPerSample, isFloat, isSilent, s_TempStereo);

    std::lock_guard<std::mutex> lk(s_RingMtx);

    const uint32_t head = s_RingHead.load(std::memory_order_relaxed);
    const uint32_t tail = s_RingTail.load(std::memory_order_acquire);


    const uint32_t nextHead = (head + 1) % RING_SLOTS;
    if (nextHead == tail)
    {
        s_DroppedFrames.fetch_add(numFrames, std::memory_order_relaxed);
        s_Overruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    AudioRingSlot& slot = s_RingSlots[head];
    slot.qpc        = qpc;
    slot.numFrames  = numFrames;
    slot.sampleRate = sampleRate ? sampleRate : 48000;
    memcpy(slot.data, s_TempStereo, numFrames * 2 * sizeof(int16_t));

    s_RingHead.store(nextHead, std::memory_order_release);
    s_Source.store(sourceKind, std::memory_order_relaxed);
    s_CapturedFrames.fetch_add(numFrames, std::memory_order_relaxed);
}

// ── Hook Callbacks ───────────────────────────────────────────────────────────
static HRESULT WINAPI Hooked_GetBuffer(
    IAudioRenderClient* This,
    UINT32 NumFramesRequested,
    BYTE** ppData)
{
    HRESULT hr = s_OrigGetBuffer(This, NumFramesRequested, ppData);
    if (SUCCEEDED(hr) && ppData && *ppData)
    {
        t_LastBuffer = *ppData;
        t_LastFramesRequested = NumFramesRequested;
        t_LastClient = This;

        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        TrackedRenderClient* trc = FindRenderClientLocked(This);
        if (trc)
        {
            trc->lastBuffer = *ppData;
            trc->lastFrames = NumFramesRequested;
        }
    }
    return hr;
}

static HRESULT WINAPI Hooked_ReleaseBuffer(
    IAudioRenderClient* This,
    UINT32 NumFramesWritten,
    DWORD dwFlags)
{
    const DWORD now = GetTickCount();
    ClientAudioFormat fmt;
    BYTE* pData = nullptr;
    LevelFilter* levelFilter = nullptr;   // entries are static, so the pointer outlives the lock

    {
        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        TrackedRenderClient* trc = FindRenderClientLocked(This);
        if (!trc)
        {
            for (size_t i = 0; i < MAX_TRACKED; ++i)
            {
                if (s_RenderClients[i].renderClient == nullptr)
                {
                    s_RenderClients[i].renderClient    = This;
                    s_RenderClients[i].audioClient     = nullptr;
                    s_RenderClients[i].format          = LookupAudioClientFormat(nullptr);
                    s_RenderClients[i].lastBuffer      = nullptr;
                    s_RenderClients[i].lastFrames      = 0;
                    s_RenderClients[i].lastReleaseTick = now;
                    s_RenderClients[i].lastAudibleTick = 0;
                    s_RenderClients[i].lastPeak        = 0.0f;
                    s_RenderClients[i].isAudible       = false;
                    s_RenderClients[i].levelFilter     = LevelFilter{};
                    trc = &s_RenderClients[i];
                    break;
                }
            }
        }

        if (trc)
        {
            trc->lastReleaseTick = now;
            fmt = trc->format;
            pData = trc->lastBuffer ? trc->lastBuffer : ((t_LastClient == This) ? t_LastBuffer : nullptr);
            trc->lastBuffer = nullptr;
            levelFilter = &trc->levelFilter;
        }
        else
        {
            pData = (t_LastClient == This) ? t_LastBuffer : nullptr;
            fmt = LookupAudioClientFormat(nullptr);
        }
        t_LastBuffer = nullptr;
    }

    if (!g_AudioEnabled.load(std::memory_order_relaxed) ||
        NumFramesWritten == 0)
    {
        return s_OrigReleaseBuffer(This, NumFramesWritten, dwFlags);
    }

    // Loopback-only mode bypasses direct hook capture
    if (s_CaptureMode.load(std::memory_order_relaxed) == AudioCaptureMode_Loopback)
    {
        return s_OrigReleaseBuffer(This, NumFramesWritten, dwFlags);
    }

    // Discard low-rate speech / voice-chat clients (e.g. 16kHz mono)
    if (fmt.sampleRate < 32000 || fmt.channels < 2)
    {
        return s_OrigReleaseBuffer(This, NumFramesWritten, dwFlags);
    }

    const bool isSilentFlag = (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    float bufferPeak = 0.0f;
    const bool isAudible = (!isSilentFlag && pData)
        ? CheckBufferAudible(pData, NumFramesWritten, fmt.channels, fmt.bitsPerSample, fmt.isFloat,
                             fmt.sampleRate, levelFilter, &bufferPeak)
        : false;

    const float gain = s_SoftwareGain.load(std::memory_order_relaxed);

    if (isAudible)
    {
        s_LastAudibleHookTick.store(now, std::memory_order_relaxed);
        if (gain > 0.0001f)
        {
            float gainedPeak = bufferPeak * gain;
            float db = (gainedPeak > 0.00001f) ? (20.0f * log10f(gainedPeak)) : -96.0f;
            if (db > 0.0f) db = 0.0f;
            s_PeakDb.store(db, std::memory_order_relaxed);
        }
        else
        {
            s_PeakDb.store(-96.0f, std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        TrackedRenderClient* trc = FindRenderClientLocked(This);
        if (trc)
        {
            trc->lastAudibleTick = now;
            trc->lastPeak = bufferPeak;
            trc->isAudible = true;
        }
    }
    else
    {
        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        TrackedRenderClient* trc = FindRenderClientLocked(This);
        if (trc)
        {
            trc->isAudible = false;
        }
    }

    // Stream arbitration and single-master enforcement
    IAudioRenderClient* curMaster = s_MasterRenderClient.load();
    TrackedRenderClient* masterInfo = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        if (curMaster) masterInfo = FindRenderClientLocked(curMaster);
    }

    const bool masterAlive   = masterInfo && ((now - masterInfo->lastReleaseTick) < 2000);
    const bool masterAudible = masterInfo && (masterInfo->lastAudibleTick > 0) && ((now - masterInfo->lastAudibleTick) < 1000);

    if (!curMaster || !masterAlive)
    {
        s_MasterRenderClient.store(This);
        curMaster = This;
        UpdateGlobalFormat(fmt);
        Log("[audio] Adopted master render client: %p (%u Hz, %u ch, %s)",
            This, fmt.sampleRate, fmt.channels, fmt.isFloat ? "float" : "pcm");
    }
    else if (This != curMaster)
    {
        bool promote = false;
        const char* reason = "";

        if (!masterAudible && isAudible)
        {
            // Failover: Master has never made sound or has been silent for > 1s (e.g. 2ch intro while 8ch is silent)
            promote = true;
            reason = "cur_master_silent";
        }
        else if (isAudible && fmt.score > masterInfo->format.score)
        {
            // Upgrade: Higher channel count active sound (e.g. 8ch world sound starting after intro)
            promote = true;
            reason = "higher_channel_score";
        }

        if (promote)
        {
            s_MasterRenderClient.store(This);
            curMaster = This;
            UpdateGlobalFormat(fmt);
            Log("[audio] Promoted master render client to %p (%u Hz, %u ch, %s, reason=%s)",
                This, fmt.sampleRate, fmt.channels, fmt.isFloat ? "float" : "pcm", reason);
        }
        else
        {
            // Non-master client: pass through to game audio engine, never push to recording queue
            return s_OrigReleaseBuffer(This, NumFramesWritten, dwFlags);
        }
    }

    // Only push to recording queue if a consumer has requested audio
    if (pData && g_AudioWanted.load(std::memory_order_relaxed))
    {
        s_LastHookReleaseTick.store(now, std::memory_order_relaxed);

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        PushAudioChunk(
            static_cast<uint64_t>(qpc.QuadPart),
            NumFramesWritten,
            fmt.sampleRate,
            fmt.channels,
            fmt.bitsPerSample,
            fmt.isFloat,
            isSilentFlag,
            pData,
            AudioSource_Hook);
    }

    return s_OrigReleaseBuffer(This, NumFramesWritten, dwFlags);
}

static HRESULT WINAPI Hooked_AudioClient_Initialize(
    IAudioClient* This,
    AUDCLNT_SHAREMODE ShareMode,
    DWORD StreamFlags,
    REFERENCE_TIME hnsBufferDuration,
    REFERENCE_TIME hnsPeriodicity,
    const WAVEFORMATEX* pFormat,
    LPCGUID AudioSessionGuid)
{
    if (pFormat && !s_InInit.load())
    {
        ClientAudioFormat fmt = ParseWaveFormat(pFormat, hnsBufferDuration);
        RegisterAudioClientFormat(This, fmt);
        Log("[audio] AudioClient %p Initialize: %u Hz, %u ch, %u bits (%s), period=%.2f ms",
            This, fmt.sampleRate, fmt.channels, fmt.bitsPerSample,
            fmt.isFloat ? "float" : "pcm", fmt.devicePeriodMs);
    }
    return s_OrigClientInit(This, ShareMode, StreamFlags, hnsBufferDuration, hnsPeriodicity, pFormat, AudioSessionGuid);
}

static void HookAudioRenderClientVTable(void** renderVtbl);

static HRESULT WINAPI Hooked_AudioClient_GetService(
    IAudioClient* This,
    REFIID riid,
    void** ppv)
{
    HRESULT hr = s_OrigClientGetService(This, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv)
    {
        if (IsEqualIID(riid, __uuidof(IAudioRenderClient)))
        {
            IAudioRenderClient* pRender = reinterpret_cast<IAudioRenderClient*>(*ppv);
            void** vtbl = *reinterpret_cast<void***>(pRender);
            HookAudioRenderClientVTable(vtbl);

            if (!s_InInit.load())
            {
                ClientAudioFormat fmt = LookupAudioClientFormat(This);
                RegisterRenderClient(pRender, This, fmt);
            }
        }
    }
    return hr;
}

static void HookAudioRenderClientVTable(void** renderVtbl)
{
    std::lock_guard<std::mutex> lk(s_HookMtx);
    if (!renderVtbl) return;

    if (renderVtbl[3] == reinterpret_cast<void*>(Hooked_GetBuffer))
    {
        s_RenderVTable = renderVtbl;
        return; // already hooked
    }

    s_RenderVTable = renderVtbl;
    PatchVTableSlot(&renderVtbl[3], reinterpret_cast<void*>(Hooked_GetBuffer), reinterpret_cast<void**>(&s_OrigGetBuffer));
    PatchVTableSlot(&renderVtbl[4], reinterpret_cast<void*>(Hooked_ReleaseBuffer), reinterpret_cast<void**>(&s_OrigReleaseBuffer));

    Log("[audio] IAudioRenderClient hooks installed (GetBuffer: %p -> %p, ReleaseBuffer: %p -> %p)",
        reinterpret_cast<void*>(s_OrigGetBuffer), reinterpret_cast<void*>(Hooked_GetBuffer),
        reinterpret_cast<void*>(s_OrigReleaseBuffer), reinterpret_cast<void*>(Hooked_ReleaseBuffer));
    s_HooksInstalled.store(true);
}

// ── Loopback Fallback Worker ─────────────────────────────────────────────────
static void LoopbackCaptureThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("[audio] Loopback fallback watchdog started");

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pClient = nullptr;
    IAudioCaptureClient* pCapture = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    bool loopbackStarted = false;

    auto CleanupLoopback = [&]() {
        if (pClient) { pClient->Stop(); pClient->Release(); pClient = nullptr; }
        if (pCapture) { pCapture->Release(); pCapture = nullptr; }
        if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
        if (pDevice) { pDevice->Release(); pDevice = nullptr; }
        if (pEnum) { pEnum->Release(); pEnum = nullptr; }
        loopbackStarted = false;
    };

    while (s_LoopbackRunning.load())
    {
        {
            std::unique_lock<std::mutex> lk(s_LoopbackMtx);
            s_LoopbackCv.wait_for(lk, std::chrono::milliseconds(25), [&] {
                return !s_LoopbackRunning.load() || g_AudioWanted.load();
            });
        }

        if (!s_LoopbackRunning.load()) break;
        if (!g_AudioWanted.load())
        {
            CleanupLoopback();
            continue;
        }

        const DWORD now = GetTickCount();
        const int mode = s_CaptureMode.load(std::memory_order_relaxed);
        const bool hasDirectMaster = (s_MasterRenderClient.load() != nullptr);
        const DWORD lastHookRelease = s_LastHookReleaseTick.load(std::memory_order_relaxed);
        const bool directHookActive = hasDirectMaster || (lastHookRelease > 0 && (now - lastHookRelease) < 1500);

        if (mode == AudioCaptureMode_DirectHook)
        {
            if (pClient && loopbackStarted) { pClient->Stop(); loopbackStarted = false; }
            Sleep(50);
            continue;
        }

        if (mode == AudioCaptureMode_Auto && directHookActive)
        {
            // Direct hook is actively capturing: pause loopback without destroying client
            if (pClient && loopbackStarted) { pClient->Stop(); loopbackStarted = false; }
            Sleep(20);
            continue;
        }

        // Loopback is required (either mode == Loopback, or mode == Auto with silent direct hook)
        if (!pClient)
        {
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
            if (FAILED(hr)) { Sleep(100); continue; }

            hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }

            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pClient));
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }

            hr = pClient->GetMixFormat(&pwfx);
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }

            hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     10000000, 0, pwfx, nullptr);
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }

            hr = pClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCapture));
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }

            hr = pClient->Start();
            if (FAILED(hr)) { CleanupLoopback(); Sleep(100); continue; }
            loopbackStarted = true;

            Log("[audio] Activated desktop loopback capture (mode=%d, directHookActive=%d)",
                mode, directHookActive ? 1 : 0);
        }
        else if (!loopbackStarted)
        {
            pClient->Start();
            loopbackStarted = true;
        }

        // Drain capture buffers
        UINT32 packetLength = 0;
        HRESULT hr = pCapture->GetNextPacketSize(&packetLength);
        while (SUCCEEDED(hr) && packetLength > 0 && s_LoopbackRunning.load())
        {
            // If in auto mode and direct hook is active, pause loopback immediately
            if (s_CaptureMode.load(std::memory_order_relaxed) == AudioCaptureMode_Auto)
            {
                const bool hasMaster = (s_MasterRenderClient.load() != nullptr);
                const DWORD recentRelease = s_LastHookReleaseTick.load(std::memory_order_relaxed);
                if (hasMaster || (recentRelease > 0 && (GetTickCount() - recentRelease) < 1500))
                {
                    pClient->Stop();
                    loopbackStarted = false;
                    break;
                }
            }

            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpc = 0;

            hr = pCapture->GetBuffer(&pData, &numFrames, &flags, &devPos, &qpc);
            if (SUCCEEDED(hr))
            {
                if (qpc == 0)
                {
                    LARGE_INTEGER qpcNow;
                    QueryPerformanceCounter(&qpcNow);
                    qpc = static_cast<uint64_t>(qpcNow.QuadPart);
                }

                const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                     (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                      IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)));

                PushAudioChunk(
                    qpc,
                    numFrames,
                    pwfx->nSamplesPerSec,
                    pwfx->nChannels,
                    pwfx->wBitsPerSample,
                    isFloat,
                    isSilent,
                    pData,
                    AudioSource_Loopback);

                pCapture->ReleaseBuffer(numFrames);
            }

            hr = pCapture->GetNextPacketSize(&packetLength);
        }

        Sleep(5);
    }

    CleanupLoopback();
    CoUninitialize();
    Log("[audio] Loopback fallback watchdog finished");
}

// ── Public API ───────────────────────────────────────────────────────────────

void Audio_Init()
{
    QueryPerformanceFrequency(&s_QpcFreq);

    if (!s_RingSlots)
    {
        s_RingSlots = reinterpret_cast<AudioRingSlot*>(
            VirtualAlloc(nullptr, sizeof(AudioRingSlot) * RING_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!s_RingSlots)
        {
            SetAudioError("VirtualAlloc for audio ring buffer failed");
            return;
        }
    }

    s_RingHead.store(0);
    s_RingTail.store(0);
    s_CurrentSlotOffset = 0;

    // Direct WASAPI hook installation
    s_InInit.store(true);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    if (SUCCEEDED(hr) && pEnum)
    {
        IMMDevice* pDevice = nullptr;
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
        if (SUCCEEDED(hr) && pDevice)
        {
            IAudioClient* pClient = nullptr;
            hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pClient));
            if (SUCCEEDED(hr) && pClient)
            {
                void** clientVtbl = *reinterpret_cast<void***>(pClient);
                s_ClientVTable = clientVtbl;

                // Hook IAudioClient::Initialize (slot 3) and GetService (slot 14)
                PatchVTableSlot(&clientVtbl[3], reinterpret_cast<void*>(Hooked_AudioClient_Initialize), reinterpret_cast<void**>(&s_OrigClientInit));
                PatchVTableSlot(&clientVtbl[14], reinterpret_cast<void*>(Hooked_AudioClient_GetService), reinterpret_cast<void**>(&s_OrigClientGetService));

                WAVEFORMATEX* pwfx = nullptr;
                if (SUCCEEDED(pClient->GetMixFormat(&pwfx)) && pwfx)
                {
                    ClientAudioFormat fmt = ParseWaveFormat(pwfx, 100000);
                    UpdateGlobalFormat(fmt);

                    hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pwfx, nullptr);
                    if (SUCCEEDED(hr))
                    {
                        IAudioRenderClient* pRender = nullptr;
                        hr = pClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRender));
                        if (SUCCEEDED(hr) && pRender)
                        {
                            void** renderVtbl = *reinterpret_cast<void***>(pRender);
                            HookAudioRenderClientVTable(renderVtbl);
                            pRender->Release();
                        }
                    }
                    CoTaskMemFree(pwfx);
                }
                pClient->Release();
            }
            pDevice->Release();
        }
        pEnum->Release();
    }
    s_InInit.store(false);

    // Start background loopback fallback watchdog
    if (!s_LoopbackRunning.exchange(true))
    {
        s_LoopbackThread = std::thread(LoopbackCaptureThread);
    }

    Log("[audio] Audio_Init complete (hooks installed=%d, sampleRate=%u)",
        s_HooksInstalled.load() ? 1 : 0, s_SampleRate.load());
}

void Audio_Shutdown()
{
    // Stop loopback fallback watchdog
    if (s_LoopbackRunning.exchange(false))
    {
        s_LoopbackCv.notify_all();
        if (s_LoopbackThread.joinable())
            s_LoopbackThread.join();
    }

    // Restore vtable hooks
    std::lock_guard<std::mutex> lk(s_HookMtx);
    if (s_RenderVTable)
    {
        void* dummy = nullptr;
        if (s_OrigGetBuffer)
            PatchVTableSlot(&s_RenderVTable[3], reinterpret_cast<void*>(s_OrigGetBuffer), &dummy);
        if (s_OrigReleaseBuffer)
            PatchVTableSlot(&s_RenderVTable[4], reinterpret_cast<void*>(s_OrigReleaseBuffer), &dummy);
        s_RenderVTable = nullptr;
    }
    if (s_ClientVTable)
    {
        void* dummy = nullptr;
        if (s_OrigClientInit)
            PatchVTableSlot(&s_ClientVTable[3], reinterpret_cast<void*>(s_OrigClientInit), &dummy);
        if (s_OrigClientGetService)
            PatchVTableSlot(&s_ClientVTable[14], reinterpret_cast<void*>(s_OrigClientGetService), &dummy);
        s_ClientVTable = nullptr;
    }
    s_HooksInstalled.store(false);
    s_MasterRenderClient.store(nullptr);

    if (s_RingSlots)
    {
        VirtualFree(s_RingSlots, 0, MEM_RELEASE);
        s_RingSlots = nullptr;
    }

    Log("[audio] Audio_Shutdown complete (hooks restored)");
}

void Audio_SetEnabled(bool enabled)
{
    g_AudioEnabled.store(enabled);
}

bool Audio_IsEnabled()
{
    return g_AudioEnabled.load();
}

void Audio_SetWanted(bool wanted)
{
    if (wanted)
    {
        s_RecordStartTick.store(GetTickCount(), std::memory_order_relaxed);
    }
    g_AudioWanted.store(wanted);
    s_LoopbackCv.notify_all();
}

bool Audio_IsWanted()
{
    return g_AudioWanted.load();
}

void Audio_Flush()
{
    std::lock_guard<std::mutex> lk(s_RingMtx);
    s_RingTail.store(s_RingHead.load(std::memory_order_relaxed), std::memory_order_release);
    s_CurrentSlotOffset = 0;
    s_Source.store(AudioSource_None);
    s_PeakDb.store(-96.0f);
}

uint32_t Audio_GetSampleRate()
{
    uint32_t rate = s_SampleRate.load();
    return rate ? rate : 48000;
}

uint32_t Audio_ReadStereo16(int16_t* dst, uint32_t maxFrames, uint64_t* outQpc)
{
    if (!dst || maxFrames == 0 || !s_RingSlots) return 0;

    std::lock_guard<std::mutex> lk(s_RingMtx);

    const uint32_t tail = s_RingTail.load(std::memory_order_relaxed);
    const uint32_t head = s_RingHead.load(std::memory_order_acquire);

    if (tail == head) return 0; // Empty

    AudioRingSlot& slot = s_RingSlots[tail];
    const uint32_t remainingInSlot = slot.numFrames - s_CurrentSlotOffset;
    const uint32_t framesToRead = (remainingInSlot < maxFrames) ? remainingInSlot : maxFrames;

    if (outQpc)
    {
        uint64_t baseQpc = slot.qpc;
        if (s_CurrentSlotOffset > 0 && s_QpcFreq.QuadPart > 0 && slot.sampleRate > 0)
        {
            baseQpc += (static_cast<uint64_t>(s_CurrentSlotOffset) * static_cast<uint64_t>(s_QpcFreq.QuadPart)) / slot.sampleRate;
        }
        *outQpc = baseQpc;
    }

    // Direct copy from pre-converted interleaved stereo 16-bit slot
    memcpy(dst, &slot.data[s_CurrentSlotOffset * 2], framesToRead * 2 * sizeof(int16_t));

    // Apply software volume gain if not 100%
    float gain = s_SoftwareGain.load(std::memory_order_relaxed);
    if (gain != 1.0f)
    {
        for (uint32_t i = 0; i < framesToRead * 2; ++i)
        {
            float s = static_cast<float>(dst[i]) * gain;
            if (s > 32767.0f) s = 32767.0f;
            else if (s < -32768.0f) s = -32768.0f;
            dst[i] = static_cast<int16_t>(s);
        }
    }

    // Peak dBFS of what the encoder receives, through the same high-pass as
    // the hook path (the downmix folds the centre/rear wander into L/R, so an
    // unfiltered reading here would bounce exactly like the hook's did).
    // Consumer thread only, so one static filter is enough.
    static LevelFilter s_ReadLevelFilter;
    float peak = 0.0f;
    const bool audible = gain > 0.0001f &&
        CheckBufferAudible(reinterpret_cast<const BYTE*>(dst), framesToRead, 2, 16, false,
                           slot.sampleRate, &s_ReadLevelFilter, &peak);
    float peakDb = -96.0f;
    if (audible)
    {
        peakDb = 20.0f * log10f(peak);
        if (peakDb > 0.0f) peakDb = 0.0f;
        s_LastAudibleHookTick.store(GetTickCount(), std::memory_order_relaxed);
    }
    s_PeakDb.store(peakDb, std::memory_order_relaxed);

    s_CurrentSlotOffset += framesToRead;
    if (s_CurrentSlotOffset >= slot.numFrames)
    {
        s_CurrentSlotOffset = 0;
        s_RingTail.store((tail + 1) % RING_SLOTS, std::memory_order_release);
    }

    return framesToRead;
}

void Audio_GetStats(AudioStats* out)
{
    if (!out) return;
    out->hooksInstalled   = s_HooksInstalled.load();
    out->enabled          = g_AudioEnabled.load();
    out->wanted           = g_AudioWanted.load();
    out->source           = s_Source.load();
    out->captureMode      = s_CaptureMode.load();
    out->sampleRate       = s_SampleRate.load();
    out->channels         = s_Channels.load();
    out->bitsPerSample    = s_BitsPerSample.load();
    out->isFloat          = s_IsFloat.load();
    out->devicePeriodMs   = s_DevicePeriodMs.load();
    out->capturedFrames   = s_CapturedFrames.load();
    out->droppedFrames    = s_DroppedFrames.load();
    out->overruns         = s_Overruns.load();

    DWORD now = GetTickCount();
    DWORD lastAud = s_LastAudibleHookTick.load(std::memory_order_relaxed);
    float curPeak = s_PeakDb.load(std::memory_order_relaxed);
    float gain = s_SoftwareGain.load(std::memory_order_relaxed);

    if (gain <= 0.0001f)
    {
        curPeak = -96.0f;
    }
    else if (lastAud > 0 && (now - lastAud) > 150)
    {
        float elapsedMs = static_cast<float>(now - lastAud);
        curPeak -= (elapsedMs - 150.0f) * 0.15f;
        if (curPeak < -96.0f) curPeak = -96.0f;
    }
    out->peakDb = curPeak;

    uint32_t active = 0;
    {
        std::lock_guard<std::mutex> lk(s_ClientTrackingMtx);
        for (size_t i = 0; i < MAX_TRACKED; ++i)
        {
            if (s_RenderClients[i].renderClient && (now - s_RenderClients[i].lastReleaseTick) < 1000)
                active++;
        }
    }
    out->activeStreams = active;

    const uint32_t head = s_RingHead.load(std::memory_order_relaxed);
    const uint32_t tail = s_RingTail.load(std::memory_order_relaxed);
    const uint32_t used = (head >= tail) ? (head - tail) : (RING_SLOTS - (tail - head));
    out->ringFillPercent  = (used * 100) / RING_SLOTS;

    std::lock_guard<std::mutex> lk(s_ErrMtx);
    strncpy_s(out->lastError, sizeof(out->lastError), s_LastError, _TRUNCATE);
}

void Audio_SetCaptureMode(int mode)
{
    s_CaptureMode.store(mode, std::memory_order_relaxed);
    s_LoopbackCv.notify_all();
    Log("[audio] Audio capture mode set to %d (%s)",
        mode,
        (mode == AudioCaptureMode_Auto) ? "Auto" :
        ((mode == AudioCaptureMode_DirectHook) ? "DirectHook" : "Loopback"));
}

int Audio_GetCaptureMode()
{
    return s_CaptureMode.load(std::memory_order_relaxed);
}

void Audio_SetVolume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 3.0f) volume = 3.0f;
    s_SoftwareGain.store(volume, std::memory_order_relaxed);
}

float Audio_GetVolume()
{
    return s_SoftwareGain.load(std::memory_order_relaxed);
}

void Audio_InjectTestTone(uint32_t freqHz, uint32_t durationMs)
{
    if (freqHz == 0) freqHz = 440;
    if (durationMs == 0) durationMs = 500;
    if (durationMs > 5000) durationMs = 5000;

    const uint32_t sampleRate = Audio_GetSampleRate();
    const uint32_t totalFrames = (sampleRate * durationMs) / 1000;
    const uint32_t chunkFrames = 480;

    float tonePhase = 0.0f;
    const float phaseInc = 2.0f * 3.14159265f * static_cast<float>(freqHz) / static_cast<float>(sampleRate);

    float chunkData[480 * 2]; // stereo float

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t curQpc = static_cast<uint64_t>(qpc.QuadPart);

    uint32_t framesSent = 0;
    while (framesSent < totalFrames)
    {
        uint32_t curChunk = (totalFrames - framesSent < chunkFrames) ? (totalFrames - framesSent) : chunkFrames;
        for (uint32_t i = 0; i < curChunk; ++i)
        {
            float v = sinf(tonePhase) * 0.5f; // -6 dBFS amplitude
            chunkData[i * 2 + 0] = v;
            chunkData[i * 2 + 1] = v;
            tonePhase += phaseInc;
            if (tonePhase > 2.0f * 3.14159265f) tonePhase -= 2.0f * 3.14159265f;
        }

        PushAudioChunk(
            curQpc,
            curChunk,
            sampleRate,
            2,
            32,
            true,
            false,
            reinterpret_cast<const BYTE*>(chunkData),
            AudioSource_Hook);

        framesSent += curChunk;
        if (s_QpcFreq.QuadPart > 0)
        {
            curQpc += (static_cast<uint64_t>(curChunk) * static_cast<uint64_t>(s_QpcFreq.QuadPart)) / sampleRate;
        }
    }
    s_LastHookReleaseTick.store(GetTickCount(), std::memory_order_relaxed);
    s_LastAudibleHookTick.store(GetTickCount(), std::memory_order_relaxed);

    float gain = s_SoftwareGain.load(std::memory_order_relaxed);
    float toneDb = (gain > 0.0001f) ? (20.0f * log10f(0.5f * gain)) : -96.0f;
    if (toneDb > 0.0f) toneDb = 0.0f;
    s_PeakDb.store(toneDb, std::memory_order_relaxed);

    Log("[audio] Injected %u ms test tone (%u Hz, %u frames)", durationMs, freqHz, framesSent);
}

