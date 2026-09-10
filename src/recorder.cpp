/**
 * recorder.cpp  —  Native in-game hardware-accelerated H.264 MP4 recorder
 *
 * Implements Media Foundation IMFSinkWriter video recording with an
 * asynchronous thread worker and bounded ring queue to guarantee 0 FPS loss
 * on the game's render thread.
 *
 * Threading model
 * ───────────────
 * Exactly one worker thread lives for as long as the recorder is initialised.
 * Start and Stop do not create or join threads; they push BeginSession /
 * EndSession markers into the same queue the frames travel through.  Because a
 * single thread drains that queue in order, a stop followed immediately by a
 * start is handled strictly sequentially: the old session is always finalised
 * before the new one opens, so two encoding sessions can never share the sink
 * writer.  The render thread never blocks on encoding or on finalisation.
 *
 * All sink-writer state lives in the worker-local EncodeSession; nothing that
 * the encoder touches is a shared global.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <cstdio>
#include <cstdarg>
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

// Frames and session control share one queue so their relative order is
// preserved: frames queued before a stop still reach the file, and frames
// queued after the next start cannot leak into the previous recording.
enum class ItemKind { Frame, BeginSession, EndSession };

struct QueuedItem
{
    ItemKind    kind = ItemKind::Frame;
    QueuedFrame frame;                  // ItemKind::Frame only
    char        path[MAX_PATH] = "";    // ItemKind::BeginSession only
    uint32_t    fps = 30;               // ItemKind::BeginSession only
    uint32_t    bitrateKbps = 8000;     // ItemKind::BeginSession only
};

// ── State ────────────────────────────────────────────────────────────────────
static std::atomic<bool>     g_MfInitialized{ false };
static std::atomic<bool>     g_IsRecording{ false };
static std::atomic<bool>     g_IsPaused{ false };
static std::atomic<uint64_t> g_RecordedFrames{ 0 };

// Published for the overlay's stats panel; written by whichever thread owns
// the value, so they are atomic rather than plain scalars.
static std::atomic<uint32_t> g_TargetFps{ 30 };
static std::atomic<uint32_t> g_BitrateKbps{ 8000 };
static std::atomic<uint32_t> g_EncoderWidth{ 0 };
static std::atomic<uint32_t> g_EncoderHeight{ 0 };

static char                 g_CurrentPath[MAX_PATH] = ""; // render thread only

static std::mutex           g_ErrMtx;                     // guards g_LastError
static char                 g_LastError[128] = "";

static LARGE_INTEGER        g_QpcFreq = {};               // set once in Init
static LARGE_INTEGER        g_LastFrameQpc = {};          // render thread only
static std::atomic<uint64_t> g_TotalPausedQpc{ 0 };       // producer writes, worker reads
static uint64_t             g_PauseStartQpc = 0;
static DWORD                g_RecordStartTick = 0;
static DWORD                g_TotalPausedMs = 0;
static DWORD                g_PauseStartTick = 0;

static std::mutex                        g_QueueMtx;
static std::condition_variable           g_QueueCv;
static std::deque<QueuedItem>            g_Queue;
static std::vector<std::vector<uint8_t>> g_BufferPool; // Reusable allocations
static std::atomic<bool>                 g_WorkerRunning{ false };

// The worker is detached rather than held in a std::thread: this DLL is never
// unloaded in the normal flow, so a std::thread member that is still joinable
// when static destructors run would call std::terminate() and take the game
// down at exit. Shutdown waits on this flag instead of joining.
static std::atomic<bool>                 g_WorkerFinished{ true };

// Everything the encoder touches, owned solely by the worker thread.
struct EncodeSession
{
    IMFSinkWriter* writer      = nullptr;
    DWORD          streamIndex = 0;
    uint32_t       width       = 0;
    uint32_t       height      = 0;
    uint32_t       fps         = 30;
    uint32_t       bitrateKbps = 8000;
    uint64_t       baseQpc     = 0;
    uint64_t       framesWritten = 0;   // this session only; the published
                                        // g_RecordedFrames belongs to whichever
                                        // session is currently active
    bool           active      = false;
    char           path[MAX_PATH] = "";
};

// ── Helpers ──────────────────────────────────────────────────────────────────
static void SetRecorderError(const char* fmt, ...)
{
    char msg[sizeof(g_LastError)] = "";
    if (fmt && *fmt)
    {
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
        va_end(args);
        Log("[rec] %s", msg);
    }
    std::lock_guard<std::mutex> lock(g_ErrMtx);
    strncpy_s(g_LastError, sizeof(g_LastError), msg, _TRUNCATE);
}

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

// ── Session lifecycle (worker thread only) ───────────────────────────────────
static bool OpenSession(EncodeSession& s, uint32_t width, uint32_t height)
{
    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, s.path, -1, wpath, MAX_PATH);

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2))) return false;
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    HRESULT hr = MFCreateSinkWriterFromURL(wpath, nullptr, attrs, &s.writer);
    attrs->Release();
    if (FAILED(hr))
    {
        SetRecorderError("MFCreateSinkWriterFromURL failed: 0x%08lX", hr);
        s.writer = nullptr;
        return false;
    }

    IMFMediaType* outType = nullptr;
    if (FAILED(MFCreateMediaType(&outType))) { s.writer->Release(); s.writer = nullptr; return false; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, s.bitrateKbps * 1000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, s.fps, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = s.writer->AddStream(outType, &s.streamIndex);
    outType->Release();
    if (FAILED(hr))
    {
        SetRecorderError("AddStream H264 failed: 0x%08lX", hr);
        s.writer->Release(); s.writer = nullptr;
        return false;
    }

    IMFMediaType* inType = nullptr;
    if (FAILED(MFCreateMediaType(&inType))) { s.writer->Release(); s.writer = nullptr; return false; }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, s.fps, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = s.writer->SetInputMediaType(s.streamIndex, inType, nullptr);
    inType->Release();
    if (FAILED(hr))
    {
        SetRecorderError("SetInputMediaType failed: 0x%08lX", hr);
        s.writer->Release(); s.writer = nullptr;
        return false;
    }

    hr = s.writer->BeginWriting();
    if (FAILED(hr))
    {
        SetRecorderError("BeginWriting failed: 0x%08lX", hr);
        s.writer->Release(); s.writer = nullptr;
        return false;
    }

    s.width  = width;
    s.height = height;
    g_EncoderWidth.store(width);
    g_EncoderHeight.store(height);
    Log("[rec] IMFSinkWriter initialized: %ux%u @ %u fps, %u kbps -> %s",
        width, height, s.fps, s.bitrateKbps, s.path);
    return true;
}

static void CloseSession(EncodeSession& s)
{
    if (s.writer)
    {
        Log("[rec] Finalizing MP4 sink writer ...");
        HRESULT hr = s.writer->Finalize();
        if (FAILED(hr))
            SetRecorderError("Finalize failed: 0x%08lX", hr);
        s.writer->Release();
        s.writer = nullptr;
        Log("[rec] Finalized. Total recorded frames: %llu -> %s",
            static_cast<unsigned long long>(s.framesWritten), s.path);
    }
    s.active = false;
}

static bool EncodeFrame(EncodeSession& s, const QueuedFrame& frame)
{
    if (!s.writer) return false;

    const DWORD frameBytes = s.width * s.height * 4;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(frameBytes, &buffer);
    if (FAILED(hr) || !buffer) return false;

    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr)))
    {
        buffer->Release();
        return false;
    }

    MFCopyImage(dst, s.width * 4, frame.pixels.data(), frame.stride, s.width * 4, s.height);
    buffer->Unlock();
    buffer->SetCurrentLength(frameBytes);

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample)))
    {
        buffer->Release();
        return false;
    }
    sample->AddBuffer(buffer);

    const uint64_t pausedQpc = g_TotalPausedQpc.load();
    uint64_t frameQpc = (frame.qpc >= pausedQpc) ? (frame.qpc - pausedQpc) : 0;
    if (s.baseQpc == 0) s.baseQpc = frameQpc;

    LONGLONG sampleTime = 0;
    if (g_QpcFreq.QuadPart > 0 && frameQpc >= s.baseQpc)
    {
        sampleTime = static_cast<LONGLONG>(
            ((frameQpc - s.baseQpc) * 10000000ULL) / static_cast<uint64_t>(g_QpcFreq.QuadPart));
    }
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(10000000LL / (s.fps ? s.fps : 30));

    hr = s.writer->WriteSample(s.streamIndex, sample);
    sample->Release();
    buffer->Release();

    if (FAILED(hr))
    {
        Log("[rec] WriteSample failed: 0x%08lX", hr);
        return false;
    }

    ++s.framesWritten;
    g_RecordedFrames.fetch_add(1);
    return true;
}

// ── Queue helpers ────────────────────────────────────────────────────────────
static void RecycleBufferLocked(std::vector<uint8_t>&& pixels)
{
    if (g_BufferPool.size() < MAX_QUEUE_FRAMES)
    {
        pixels.clear();
        g_BufferPool.push_back(std::move(pixels));
    }
}

static size_t QueuedFrameCountLocked()
{
    size_t n = 0;
    for (const QueuedItem& i : g_Queue)
        if (i.kind == ItemKind::Frame) ++n;
    return n;
}

// Drop the oldest *frame* when the worker falls behind. Session markers are
// never dropped: losing an EndSession would leave a recording unfinalised.
static void DropOldestFrameLocked()
{
    for (auto it = g_Queue.begin(); it != g_Queue.end(); ++it)
    {
        if (it->kind == ItemKind::Frame)
        {
            RecycleBufferLocked(std::move(it->frame.pixels));
            g_Queue.erase(it);
            return;
        }
    }
}

// ── Background Worker Thread ─────────────────────────────────────────────────
static void RecorderWorkerLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log("[rec] Background encoding worker thread started");

    EncodeSession session;

    for (;;)
    {
        QueuedItem item;
        {
            std::unique_lock<std::mutex> lock(g_QueueMtx);
            g_QueueCv.wait(lock, [] {
                return !g_Queue.empty() || !g_WorkerRunning.load();
            });

            if (g_Queue.empty())
            {
                if (!g_WorkerRunning.load()) break;  // stop only once drained
                continue;
            }

            item = std::move(g_Queue.front());
            g_Queue.pop_front();
        }

        switch (item.kind)
        {
        case ItemKind::BeginSession:
            // Stop always queues an EndSession first, so this should be a no-op;
            // close defensively so a sink writer can never be leaked.
            CloseSession(session);
            session = EncodeSession{};
            strncpy_s(session.path, sizeof(session.path), item.path, _TRUNCATE);
            session.fps         = item.fps;
            session.bitrateKbps = item.bitrateKbps;
            session.active      = true;
            g_RecordedFrames.store(0);
            break;

        case ItemKind::Frame:
            if (!session.active)
                break;  // straggler from a session that has already ended

            if (!session.writer &&
                !OpenSession(session, item.frame.width, item.frame.height))
            {
                Log("[rec] Failed to initialize SinkWriter in worker; stopping recorder");
                session.active = false;
                g_IsRecording.store(false);
                break;
            }
            EncodeFrame(session, item.frame);
            break;

        case ItemKind::EndSession:
            CloseSession(session);
            break;
        }

        if (item.kind == ItemKind::Frame)
        {
            std::lock_guard<std::mutex> lock(g_QueueMtx);
            RecycleBufferLocked(std::move(item.frame.pixels));
        }
    }

    CloseSession(session); // shutting down with a recording still open

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.clear();
        g_BufferPool.clear();
        g_BufferPool.shrink_to_fit();
    }

    Log("[rec] Background encoding worker thread finished");
    CoUninitialize();
    g_WorkerFinished.store(true);
}

// ── Public API ───────────────────────────────────────────────────────────────

void Recorder_Init()
{
    QueryPerformanceFrequency(&g_QpcFreq);
    InitMediaFoundation();
    CreateDirectoryA("C:\\d3d9capture", nullptr);
    CreateDirectoryA(RECORDINGS_DIR, nullptr);

    // One worker for the life of the recorder; Start/Stop only queue markers.
    if (!g_WorkerRunning.exchange(true))
    {
        g_WorkerFinished.store(false);
        std::thread(RecorderWorkerLoop).detach();
    }
}

void Recorder_Shutdown()
{
    Recorder_Stop();               // queues EndSession if a recording is open

    g_WorkerRunning.store(false);
    g_QueueCv.notify_all();

    // Bounded wait for the worker to finalise; never a join, so this is safe to
    // call from a DLL teardown path where joining would risk the loader lock.
    for (int i = 0; i < 500 && !g_WorkerFinished.load(); ++i)
        Sleep(10);

    if (g_MfInitialized.exchange(false))
        MFShutdown();
}

bool Recorder_Start(const char* customPath, uint32_t fps, uint32_t bitrateKbps)
{
    if (g_IsRecording.load())
    {
        Log("[rec] Already recording; ignoring Start");
        return false;
    }
    if (!g_WorkerRunning.load())
    {
        Log("[rec] Recorder not initialised; ignoring Start");
        return false;
    }

    InitMediaFoundation();

    g_TargetFps.store(fps > 0 ? fps : 30);
    g_BitrateKbps.store(bitrateKbps > 0 ? bitrateKbps : 8000);
    g_RecordedFrames.store(0);
    g_EncoderWidth.store(0);
    g_EncoderHeight.store(0);
    g_TotalPausedQpc.store(0);
    g_PauseStartQpc = 0;
    g_TotalPausedMs = 0;
    g_PauseStartTick = 0;
    SetRecorderError("");

    if (customPath && customPath[0] != '\0')
        strncpy_s(g_CurrentPath, sizeof(g_CurrentPath), customPath, _TRUNCATE);
    else
        GenerateDefaultFileName(g_CurrentPath, sizeof(g_CurrentPath));

    QueryPerformanceCounter(&g_LastFrameQpc);
    g_RecordStartTick = GetTickCount();

    QueuedItem begin;
    begin.kind        = ItemKind::BeginSession;
    begin.fps         = g_TargetFps.load();
    begin.bitrateKbps = g_BitrateKbps.load();
    strncpy_s(begin.path, sizeof(begin.path), g_CurrentPath, _TRUNCATE);
    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(begin));
    }
    g_QueueCv.notify_one();

    g_IsPaused.store(false);
    g_IsRecording.store(true);

    Log("[rec] Recording started -> %s (%u FPS, %u kbps)",
        g_CurrentPath, g_TargetFps.load(), g_BitrateKbps.load());
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
        g_TotalPausedQpc.fetch_add(curQpc - g_PauseStartQpc);
    }
    g_TotalPausedMs += (GetTickCount() - g_PauseStartTick);
    g_IsPaused.store(false);
    Log("[rec] Recording resumed (paused for %llu QPC ticks)",
        static_cast<unsigned long long>(curQpc - g_PauseStartQpc));
}

void Recorder_Stop()
{
    if (!g_IsRecording.load()) return;

    Log("[rec] Stopping recording (async)...");
    g_IsRecording.store(false);
    g_IsPaused.store(false);

    QueuedItem end;
    end.kind = ItemKind::EndSession;
    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(end));
    }
    g_QueueCv.notify_one();

    Log("[rec] Recording stop queued; render thread not blocked.");
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
    outStats->width          = g_EncoderWidth.load();
    outStats->height         = g_EncoderHeight.load();
    outStats->fps            = g_TargetFps.load();
    outStats->bitrateKbps    = g_BitrateKbps.load();
    strncpy_s(outStats->outputPath, sizeof(outStats->outputPath), g_CurrentPath, _TRUNCATE);
    {
        std::lock_guard<std::mutex> lock(g_ErrMtx);
        strncpy_s(outStats->lastError, sizeof(outStats->lastError), g_LastError, _TRUNCATE);
    }

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

    const uint32_t targetFps = g_TargetFps.load();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_QpcFreq.QuadPart > 0 && targetFps > 0)
    {
        double elapsedSec = static_cast<double>(now.QuadPart - g_LastFrameQpc.QuadPart) / static_cast<double>(g_QpcFreq.QuadPart);
        double minIntervalSec = 1.0 / static_cast<double>(targetFps);
        if (elapsedSec < (minIntervalSec * 0.88))
        {
            return false; // Framerate limiter pacing
        }
    }
    return true;
}

void Recorder_OnFrameReady(const void* pixels, uint32_t width, uint32_t height,
                           uint32_t stride, uint64_t frameIdx)
{
    (void)frameIdx;
    if (!g_IsRecording.load() || g_IsPaused.load() || !pixels) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_LastFrameQpc = now;

    const size_t byteSize = static_cast<size_t>(width) * height * 4;

    QueuedItem item;
    item.kind         = ItemKind::Frame;
    item.frame.width  = width;
    item.frame.height = height;
    item.frame.stride = stride;
    item.frame.qpc    = static_cast<uint64_t>(now.QuadPart);

    {
        std::unique_lock<std::mutex> lock(g_QueueMtx);

        // Discard oldest frame if worker is overloaded to protect render thread
        if (QueuedFrameCountLocked() >= MAX_QUEUE_FRAMES)
        {
            DropOldestFrameLocked();
        }

        // Reuse memory from buffer pool
        if (!g_BufferPool.empty())
        {
            item.frame.pixels = std::move(g_BufferPool.back());
            g_BufferPool.pop_back();
        }
    }

    if (item.frame.pixels.size() < byteSize)
        item.frame.pixels.resize(byteSize);

    // Fast copy from render thread
    const BYTE* src = static_cast<const BYTE*>(pixels);
    BYTE* dst = item.frame.pixels.data();
    const DWORD rowBytes = width * 4;
    for (uint32_t y = 0; y < height; ++y, src += stride, dst += rowBytes)
    {
        memcpy(dst, src, rowBytes);
    }

    {
        std::lock_guard<std::mutex> lock(g_QueueMtx);
        g_Queue.push_back(std::move(item));
    }
    g_QueueCv.notify_one();
}
