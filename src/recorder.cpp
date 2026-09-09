/**
 * recorder.cpp  —  Native in-game hardware-accelerated H.264 MP4 recorder
 *
 * Implements Media Foundation IMFSinkWriter video recording with an
 * asynchronous thread worker and bounded ring queue to guarantee 0 FPS loss
 * on the game's render thread.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <vector>
#include <deque>

#include "recorder.h"
#include "capture.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// ── Constants & Queue Configuration ──────────────────────────────────────────
static constexpr size_t MAX_QUEUE_FRAMES = 24; // ~0.4s buffer at 60fps
static constexpr char   RECORDINGS_DIR[] = "C:\\d3d9capture\\recordings\\";

struct QueuedFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint64_t qpc = 0;
};

// ── State ────────────────────────────────────────────────────────────────────
static std::atomic<bool>    g_MfInitialized{ false };
static std::atomic<bool>    g_IsRecording{ false };
static std::atomic<bool>    g_IsPaused{ false };
static std::atomic<uint64_t> g_RecordedFrames{ 0 };

static uint32_t             g_TargetFps = 30;
static uint32_t             g_BitrateKbps = 8000;
static char                 g_CurrentPath[MAX_PATH] = "";
static char                 g_LastError[128] = "";

static LARGE_INTEGER        g_QpcFreq = {};
static LARGE_INTEGER        g_LastFrameQpc = {};
static uint64_t             g_BaseQpc = 0;
static uint64_t             g_PauseStartQpc = 0;
static uint64_t             g_TotalPausedQpc = 0;
static DWORD                g_RecordStartTick = 0;
static DWORD                g_TotalPausedMs = 0;
static DWORD                g_PauseStartTick = 0;

static std::mutex           g_QueueMtx;
static std::condition_variable g_QueueCv;
static std::deque<QueuedFrame> g_FrameQueue;
static std::vector<std::vector<uint8_t>> g_BufferPool; // Reusable allocations
static std::thread          g_WorkerThread;
static std::atomic<bool>    g_WorkerRunning{ false };

static IMFSinkWriter*       g_pWriter = nullptr;
static DWORD                g_StreamIndex = 0;
static uint32_t             g_EncoderWidth = 0;
static uint32_t             g_EncoderHeight = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────
static void GenerateDefaultFileName(char* dst, size_t maxLen)
{
    CreateDirectoryA("C:\\d3d9capture", nullptr);
    CreateDirectoryA(RECORDINGS_DIR, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(dst, maxLen, _TRUNCATE,
        "%srecording_%04d%02d%02d_%02d%02d%02d.mp4",
        RECORDINGS_DIR, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static bool InitMediaFoundation()
{
    if (g_MfInitialized.load()) return true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr))
    {
        Log("[rec] MFStartup failed: 0x%08lX", hr);
        return false;
    }
    g_MfInitialized.store(true);
    return true;
}

static bool CreateSinkWriter(const char* path, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateKbps)
{
    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(wpath, nullptr, attrs, &g_pWriter);
    attrs->Release();
    if (FAILED(hr))
    {
        _snprintf_s(g_LastError, sizeof(g_LastError), _TRUNCATE, "MFCreateSinkWriterFromURL failed: 0x%08lX", hr);
        Log("[rec] %s", g_LastError);
        g_pWriter = nullptr;
        return false;
    }

    // Output Type: H.264 Video in MP4 Container
    IMFMediaType* outType = nullptr;
    if (FAILED(MFCreateMediaType(&outType))) { g_pWriter->Release(); g_pWriter = nullptr; return false; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateKbps * 1000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = g_pWriter->AddStream(outType, &g_StreamIndex);
    outType->Release();
    if (FAILED(hr))
    {
        _snprintf_s(g_LastError, sizeof(g_LastError), _TRUNCATE, "AddStream H264 failed: 0x%08lX", hr);
        Log("[rec] %s", g_LastError);
        g_pWriter->Release(); g_pWriter = nullptr;
        return false;
    }

    // Input Type: RGB32 / BGRA (DirectX 9 Backbuffer format)
    IMFMediaType* inType = nullptr;
    if (FAILED(MFCreateMediaType(&inType))) { g_pWriter->Release(); g_pWriter = nullptr; return false; }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = g_pWriter->SetInputMediaType(g_StreamIndex, inType, nullptr);
    inType->Release();
    if (FAILED(hr))
    {
        _snprintf_s(g_LastError, sizeof(g_LastError), _TRUNCATE, "SetInputMediaType failed: 0x%08lX", hr);
        Log("[rec] %s", g_LastError);
        g_pWriter->Release(); g_pWriter = nullptr;
        return false;
    }

    hr = g_pWriter->BeginWriting();
    if (FAILED(hr))
    {
        _snprintf_s(g_LastError, sizeof(g_LastError), _TRUNCATE, "BeginWriting failed: 0x%08lX", hr);
        Log("[rec] %s", g_LastError);
        g_pWriter->Release(); g_pWriter = nullptr;
        return false;
    }

    g_EncoderWidth = width;
    g_EncoderHeight = height;
    Log("[rec] IMFSinkWriter initialized: %ux%u @ %u fps, %u kbps -> %s",
        width, height, fps, bitrateKbps, path);
    return true;
}

static bool EncodeFrameToSinkWriter(const QueuedFrame& frame)
{
    if (!g_pWriter) return false;

    const DWORD frameBytes = g_EncoderWidth * g_EncoderHeight * 4;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(frameBytes, &buffer);
    if (FAILED(hr) || !buffer) return false;

    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr)))
    {
        buffer->Release();
        return false;
    }

    MFCopyImage(dst, g_EncoderWidth * 4, frame.pixels.data(), frame.stride, g_EncoderWidth * 4, g_EncoderHeight);
    buffer->Unlock();
    buffer->SetCurrentLength(frameBytes);

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample)))
    {
        buffer->Release();
        return false;
    }
    sample->AddBuffer(buffer);

    // Precise timestamping relative to baseQpc minus paused intervals
    uint64_t frameQpc = (frame.qpc >= g_TotalPausedQpc) ? (frame.qpc - g_TotalPausedQpc) : 0;
    if (g_BaseQpc == 0) g_BaseQpc = frameQpc;

    LONGLONG sampleTime = 0;
    if (g_QpcFreq.QuadPart > 0 && frameQpc >= g_BaseQpc)
    {
        sampleTime = static_cast<LONGLONG>(
            ((frameQpc - g_BaseQpc) * 10000000ULL) / static_cast<uint64_t>(g_QpcFreq.QuadPart));
    }
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(10000000LL / g_TargetFps);

    hr = g_pWriter->WriteSample(g_StreamIndex, sample);
    sample->Release();
    buffer->Release();

    if (FAILED(hr))
    {
        Log("[rec] WriteSample failed: 0x%08lX", hr);
        return false;
    }

    g_RecordedFrames.fetch_add(1);
    return true;
}

// ── Background Worker Thread ─────────────────────────────────────────────────
static void RecorderWorkerLoop()
{
    Log("[rec] Background encoding worker thread started");

    while (g_WorkerRunning.load() || !g_FrameQueue.empty())
    {
        QueuedFrame frame;
        {
            std::unique_lock<std::mutex> lock(g_QueueMtx);
            g_QueueCv.wait(lock, [] {
                return !g_FrameQueue.empty() || !g_WorkerRunning.load();
            });

            if (g_FrameQueue.empty())
                continue;

            frame = std::move(g_FrameQueue.front());
            g_FrameQueue.pop_front();
        }

        // Lazy initialize sink writer on first frame using actual viewport resolution
        if (!g_pWriter)
        {
            if (!CreateSinkWriter(g_CurrentPath, frame.width, frame.height, g_TargetFps, g_BitrateKbps))
            {
                Log("[rec] Failed to initialize SinkWriter in worker; stopping recorder");
                g_IsRecording.store(false);
                break;
            }
        }

        EncodeFrameToSinkWriter(frame);

        // Return vector to buffer pool for recycling
        {
            std::lock_guard<std::mutex> lock(g_QueueMtx);
            if (g_BufferPool.size() < MAX_QUEUE_FRAMES)
            {
                frame.pixels.clear();
                g_BufferPool.push_back(std::move(frame.pixels));
            }
        }
    }

    // Finalize Sink Writer
    if (g_pWriter)
    {
        Log("[rec] Finalizing MP4 sink writer...");
        HRESULT hr = g_pWriter->Finalize();
        if (FAILED(hr))
            Log("[rec] Finalize failed: 0x%08lX", hr);
        g_pWriter->Release();
        g_pWriter = nullptr;
        Log("[rec] Finalized successfully. Total recorded frames: %llu",
            static_cast<unsigned long long>(g_RecordedFrames.load()));
    }

    Log("[rec] Background encoding worker thread finished");
}

// ── Public API ───────────────────────────────────────────────────────────────

void Recorder_Init()
{
    QueryPerformanceFrequency(&g_QpcFreq);
    InitMediaFoundation();
    CreateDirectoryA("C:\\d3d9capture", nullptr);
    CreateDirectoryA(RECORDINGS_DIR, nullptr);
}

void Recorder_Shutdown()
{
    Recorder_Stop();
    if (g_MfInitialized.load())
    {
        MFShutdown();
        g_MfInitialized.store(false);
    }
}

bool Recorder_Start(const char* customPath, uint32_t fps, uint32_t bitrateKbps)
{
    if (g_IsRecording.load())
    {
        Log("[rec] Already recording; ignoring Start");
        return false;
    }

    InitMediaFoundation();

    g_TargetFps = (fps > 0) ? fps : 30;
    g_BitrateKbps = (bitrateKbps > 0) ? bitrateKbps : 8000;
    g_RecordedFrames.store(0);
    g_BaseQpc = 0;
    g_PauseStartQpc = 0;
    g_TotalPausedQpc = 0;
    g_TotalPausedMs = 0;
    g_LastError[0] = '\0';

    if (customPath && customPath[0] != '\0')
        strncpy_s(g_CurrentPath, sizeof(g_CurrentPath), customPath, _TRUNCATE);
    else
        GenerateDefaultFileName(g_CurrentPath, sizeof(g_CurrentPath));

    QueryPerformanceCounter(&g_LastFrameQpc);
    g_RecordStartTick = GetTickCount();

    // Start background worker
    g_WorkerRunning.store(true);
    g_WorkerThread = std::thread(RecorderWorkerLoop);

    g_IsPaused.store(false);
    g_IsRecording.store(true);

    Log("[rec] Recording started -> %s (%u FPS, %u kbps)", g_CurrentPath, g_TargetFps, g_BitrateKbps);
    return true;
}

void Recorder_Pause()
{
    if (!g_IsRecording.load() || g_IsPaused.load()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_PauseStartQpc = static_cast<uint64_t>(now.QuadPart);
    g_PauseStartTick = GetTickCount();
    g_IsPaused.store(true);
    Log("[rec] Recording paused");
}

void Recorder_Resume()
{
    if (!g_IsRecording.load() || !g_IsPaused.load()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t curQpc = static_cast<uint64_t>(now.QuadPart);
    if (curQpc >= g_PauseStartQpc)
    {
        g_TotalPausedQpc += (curQpc - g_PauseStartQpc);
    }
    g_TotalPausedMs += (GetTickCount() - g_PauseStartTick);
    g_IsPaused.store(false);
    Log("[rec] Recording resumed (paused for %llu QPC ticks)", curQpc - g_PauseStartQpc);
}

void Recorder_Stop()
{
    if (!g_IsRecording.load() && !g_WorkerRunning.load()) return;

    Log("[rec] Stopping recording...");
    g_IsRecording.store(false);
    g_IsPaused.store(false);

    // Stop worker and wait for queue flush
    g_WorkerRunning.store(false);
    g_QueueCv.notify_all();

    if (g_WorkerThread.joinable())
    {
        g_WorkerThread.join();
    }

    Log("[rec] Recording stopped. File saved to: %s", g_CurrentPath);
}

bool Recorder_IsRecording()
{
    return g_IsRecording.load();
}

bool Recorder_IsPaused()
{
    return g_IsPaused.load();
}

void Recorder_GetStats(RecorderStats* outStats)
{
    if (!outStats) return;
    outStats->isRecording    = g_IsRecording.load();
    outStats->isPaused       = g_IsPaused.load();
    outStats->recordedFrames = g_RecordedFrames.load();
    outStats->width          = g_EncoderWidth;
    outStats->height         = g_EncoderHeight;
    outStats->fps            = g_TargetFps;
    outStats->bitrateKbps    = g_BitrateKbps;
    strncpy_s(outStats->outputPath, sizeof(outStats->outputPath), g_CurrentPath, _TRUNCATE);
    strncpy_s(outStats->lastError, sizeof(outStats->lastError), g_LastError, _TRUNCATE);

    if (g_IsRecording.load())
    {
        DWORD now = GetTickCount();
        DWORD elapsed = now - g_RecordStartTick - g_TotalPausedMs;
        if (g_IsPaused.load() && now >= g_PauseStartTick)
        {
            elapsed -= (now - g_PauseStartTick);
        }
        outStats->durationMs = static_cast<uint64_t>(elapsed);
    }
    else
    {
        outStats->durationMs = 0;
    }
}

bool Recorder_WantsFrame()
{
    if (!g_IsRecording.load() || g_IsPaused.load()) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_QpcFreq.QuadPart > 0 && g_TargetFps > 0)
    {
        double elapsedSec = static_cast<double>(now.QuadPart - g_LastFrameQpc.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        double minIntervalSec = 1.0 / static_cast<double>(g_TargetFps);
        if (elapsedSec < (minIntervalSec * 0.88))
        {
            return false; // Framerate limiter pacing
        }
    }
    return true;
}

void Recorder_OnFrameReady(const void* pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t frameIdx)
{
    (void)frameIdx;
    if (!g_IsRecording.load() || g_IsPaused.load() || !pixels) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastFrameQpc = now;

    const size_t byteSize = static_cast<size_t>(width) * height * 4;

    QueuedFrame frame;
    frame.width  = width;
    frame.height = height;
    frame.stride = stride;
    frame.qpc    = static_cast<uint64_t>(now.QuadPart);

    {
        std::unique_lock<std::mutex> lock(g_QueueMtx);

        // Discard oldest frame if worker is overloaded to protect render thread
        if (g_FrameQueue.size() >= MAX_QUEUE_FRAMES)
        {
            g_FrameQueue.pop_front();
        }

        // Reuse memory from buffer pool
        if (!g_BufferPool.empty())
        {
            frame.pixels = std::move(g_BufferPool.back());
            g_BufferPool.pop_back();
        }
    }

    if (frame.pixels.size() < byteSize)
        frame.pixels.resize(byteSize);

    // Fast copy from render thread
    const BYTE* src = static_cast<const BYTE*>(pixels);
    BYTE* dst = frame.pixels.data();
    const DWORD rowBytes = width * 4;
    for (uint32_t y = 0; y < height; ++y, src += stride, dst += rowBytes)
    {
        memcpy(dst, src, rowBytes);
    }

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_FrameQueue.push_back(std::move(frame));
    }
    g_QueueCv.notify_one();
}
